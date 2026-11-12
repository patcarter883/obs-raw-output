/*
 * open-broadcast-raw-output
 *
 * A thin OBS output that hands over UNCOMPRESSED frames.
 *
 * This is the Option A design (open-broadcast-product/OBS_INGEST_OPTIONS.md,
 * DECISIONS.md DT-7): rather than letting OBS encode and then decoding on the
 * far side (the MPEG-TS stand-in, which double-encodes), OBS delivers raw
 * pixels and the encoder's own pipeline is the ONLY encoder in the chain.
 *
 * How it works: an obs_output registered with no video encoder set receives
 * raw frames through obs_output_info.raw_video(). OBS composites and hands us
 * the frame planes; we copy them, row by row if the plane is padded, into a
 * tight, fixed layout and write them to a TCP socket. Nothing is encoded.
 *
 * Wire format (all integers little-endian; structs are plain uint32/uint64 so
 * there is no padding to disagree about). Both streams are multiplexed onto the
 * ONE socket: every frame header carries a kind, so video and audio arrive
 * interleaved and in order, paced against each other rather than drifting apart
 * the way two independent sockets would.
 *
 *   stream header (once, on connect)
 *     u32 magic 'OBCR' | u32 version | u32 width | u32 height
 *     u32 fps_num | u32 fps_den | u32 obs_format(enum video_format)
 *     u32 planes | u32 linesize[3] | u32 plane_bytes[3](tight)
 *     u32 reserved
 *     u32 audio_rate | u32 audio_channels | u32 audio_format | u32 audio_reserved[5]
 *
 * `linesize` is sent as zero and the receiver must ignore it: it is not known
 * until the first frame arrives, while this header goes out at connect. It is
 * kept only so the field layout can grow into it. The receiver relies solely on
 * `plane_bytes`, which describe the tight layout actually written.
 *
 * `audio_rate == 0` means this source has no audio; the audio section is then
 * all zeroes and no audio frames appear.
 *
 *   frame header (per frame or audio chunk)
 *     u64 timestamp | u32 kind | u32 payload_bytes | u32 units | u32 reserved
 *   frame payload
 *     video (kind 0): plane 0, then plane 1, ... each tightly packed (no stride pad)
 *     audio (kind 1): channel 0, then channel 1, ... each `units * 4` bytes
 *
 * SYNCHRONISATION. `timestamp` is OBS's own timestamp in nanoseconds, and OBS
 * stamps video and audio from the SAME clock -- that is what makes the two
 * streams alignable at all. The receiver must keep them, not re-stamp.
 *
 * Video planes are sent tight so the receiver's layout depends only on
 * width/height/format, never on OBS's internal line stride. Audio is sent in
 * OBS's native planar float32 so no interleaving copy is needed on either side.
 */

#include <obs-module.h>
#include <obs-frontend-api.h>
#include <util/platform.h>
#include <media-io/audio-io.h>

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Sockets and timers: POSIX names on Unix, Winsock on Windows. The rest of the
 * plugin (frame layout, OBS plumbing) is platform-neutral C. */
#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <windows.h>
typedef SOCKET obc_socket_t;
#  define OBC_NO_SOCKET INVALID_SOCKET
#  define obc_close_socket closesocket
/* Winsock never raises SIGPIPE, so a send to a dead peer returns an error. */
#  define OBC_MSG_NOSIGNAL 0
#  define obc_interrupted() false
#else
#  include <arpa/inet.h>
#  include <fcntl.h>
#  include <netdb.h>
#  include <netinet/in.h>
#  include <netinet/tcp.h>
#  include <pthread.h>
#  include <sys/select.h>
#  include <sys/socket.h>
#  include <sys/time.h>
#  include <unistd.h>
typedef int obc_socket_t;
#  define OBC_NO_SOCKET (-1)
#  define obc_close_socket close
#  define OBC_MSG_NOSIGNAL MSG_NOSIGNAL
#  define obc_interrupted() (errno == EINTR)
#endif

/* One mutex guards the socket and the audio cursor; OBS drives the video and
 * audio paths from separate threads. Windows has no pthreads, so the names go
 * through this shim and a CRITICAL_SECTION backs them there. */
#ifdef _WIN32
typedef CRITICAL_SECTION obc_mutex_t;
static void obc_mutex_init(obc_mutex_t *m) { InitializeCriticalSection(m); }
static void obc_mutex_destroy(obc_mutex_t *m) { DeleteCriticalSection(m); }
static void obc_mutex_lock(obc_mutex_t *m) { EnterCriticalSection(m); }
static void obc_mutex_unlock(obc_mutex_t *m) { LeaveCriticalSection(m); }
#else
typedef pthread_mutex_t obc_mutex_t;
static void obc_mutex_init(obc_mutex_t *m) { pthread_mutex_init(m, NULL); }
static void obc_mutex_destroy(obc_mutex_t *m) { pthread_mutex_destroy(m); }
static void obc_mutex_lock(obc_mutex_t *m) { pthread_mutex_lock(m); }
static void obc_mutex_unlock(obc_mutex_t *m) { pthread_mutex_unlock(m); }
#endif

/* One thread watches the encoder's control port, so the thread handle needs the
 * same treatment: CreateThread on Windows, pthreads elsewhere. */
#ifdef _WIN32
typedef HANDLE obc_thread_t;
typedef DWORD(WINAPI *obc_thread_fn_t)(LPVOID);
static bool obc_thread_start(obc_thread_t *t, obc_thread_fn_t fn, void *arg)
{
	*t = CreateThread(NULL, 0, fn, arg, 0, NULL);
	return *t != NULL;
}
static void obc_thread_join(obc_thread_t t)
{
	WaitForSingleObject(t, INFINITE);
	CloseHandle(t);
}
#else
typedef pthread_t obc_thread_t;
typedef void *(*obc_thread_fn_t)(void *);
static bool obc_thread_start(obc_thread_t *t, obc_thread_fn_t fn, void *arg)
{
	return pthread_create(t, NULL, fn, arg) == 0;
}
static void obc_thread_join(obc_thread_t t) { pthread_join(t, NULL); }
#endif

#define OBC_MAGIC 0x4F424352u /* 'OBCR' */
/* v2 added the audio section and the frame kind. v1 carried video only, and
 * nothing consumed it, so there is no shim: receivers must accept v2 only. */
#define OBC_VERSION 2u
#define OBC_MAX_PLANES 3
#define OBC_DEFAULT_TARGET "127.0.0.1:9300"
/* Payload kinds on the shared socket. */
#define OBC_KIND_VIDEO 0u
#define OBC_KIND_AUDIO 1u
/* Only float32 planar exists here, because that is what OBS hands over. */
#define OBC_AUDIO_F32_PLANAR 1u
#define OBC_SEND_TIMEOUT_MS 200
#define OBC_SNDBUF_BYTES (8 * 1024 * 1024)
/* ~1.5 s at 60 fps. After the peer goes away (encoder stopped or restarted) the
 * output stops itself instead of spinning forever dropping every frame. */
#define OBC_MAX_CONSECUTIVE_FAILURES 90

struct obc_stream_header {
	uint32_t magic;
	uint32_t version;
	uint32_t width;
	uint32_t height;
	uint32_t fps_num;
	uint32_t fps_den;
	uint32_t format;
	uint32_t planes;
	uint32_t linesize[OBC_MAX_PLANES];
	uint32_t plane_bytes[OBC_MAX_PLANES];
	uint32_t reserved;
	/* v2 audio section; audio_rate == 0 means no audio on this connection. */
	uint32_t audio_rate;
	uint32_t audio_channels;
	uint32_t audio_format;
	uint32_t audio_reserved[5];
};

struct obc_frame_header {
	uint64_t timestamp;
	uint32_t kind;		/* OBC_KIND_VIDEO or OBC_KIND_AUDIO */
	uint32_t payload_bytes;
	uint32_t units;		/* video: plane count; audio: sample frames */
	uint32_t reserved;
};

struct obc_raw {
	obs_output_t *output;

	obc_socket_t fd;
	bool connected;
	/* Kept so a lost peer can be re-dialled without the operator restarting
	 * the output: an encoder restart must cost a second of frames, not the
	 * whole stream. */
	char host[256];
	char port[32];
	uint64_t next_reconnect_ms;
	/* The stream header, kept so a reconnect re-announces the geometry. The
	 * encoder reads exactly one per connection; dialling again and sending
	 * only frame headers leaves it reading a frame as a stream header. */
	struct obc_stream_header shdr;
	bool shdr_valid;

	uint32_t width;
	uint32_t height;
	uint32_t fps_num;
	uint32_t fps_den;
	uint32_t format;
	uint32_t planes;
	uint32_t linesize[OBC_MAX_PLANES]; /* 0 in the header; see above */
	uint32_t plane_bytes[OBC_MAX_PLANES]; /* tight sizes we put on the wire */
	uint32_t row_bytes[OBC_MAX_PLANES];
	uint32_t rows[OBC_MAX_PLANES];
	uint32_t payload_bytes;

	uint64_t frames_sent;
	uint64_t frames_dropped;
	uint64_t audio_chunks_sent;
	uint64_t audio_chunks_dropped;
	uint64_t bytes_sent; /* video frames + audio chunks, headers included */
	uint32_t consecutive_failures;
	bool logged_stride;
	/* Set once the stop path has run, so a shutdown delivered by two libobs
	 * paths reports and tears down only once per start. */
	bool stopped;

	uint32_t audio_rate;
	uint32_t audio_channels;
	bool audio_present;

	obc_mutex_t lock;
};

/* ------------------------------------------------------------------ planing */

/* Tight row width and row count per plane for each OBS video format. OBS's
 * 4:2:0 formats are both half-height chroma; I420 has three planes, NV12 two.
 * P010 is the same two-plane 4:2:0 layout at 16 bits per sample: the 10 bits sit
 * left-justified in a full 16-bit word, so every row is twice as wide as NV12's. */
static bool obc_geometry(uint32_t format,
			 uint32_t width,
			 uint32_t height,
			 uint32_t planes[OBC_MAX_PLANES],
			 uint32_t rows[OBC_MAX_PLANES],
			 uint32_t row_bytes[OBC_MAX_PLANES],
			 uint32_t *n_planes)
{
	const uint32_t cw = (width + 1) / 2;
	const uint32_t ch = (height + 1) / 2;

	switch (format) {
	case VIDEO_FORMAT_I420:
		*n_planes = 3;
		planes[0] = 0;
		planes[1] = 1;
		planes[2] = 2;
		rows[0] = height;
		rows[1] = ch;
		rows[2] = ch;
		row_bytes[0] = width;
		row_bytes[1] = cw;
		row_bytes[2] = cw;
		return true;
	case VIDEO_FORMAT_NV12:
		*n_planes = 2;
		planes[0] = 0;
		planes[1] = 1;
		rows[0] = height;
		rows[1] = ch;
		row_bytes[0] = width;
		row_bytes[1] = width; /* interleaved UV */
		return true;
	case VIDEO_FORMAT_P010:
		/* Same geometry as NV12 with 16-bit samples: the interleaved UV row is
		 * (width / 2) U/V pairs of 2 bytes each, i.e. width * 2. */
		*n_planes = 2;
		planes[0] = 0;
		planes[1] = 1;
		rows[0] = height;
		rows[1] = ch;
		row_bytes[0] = width * 2;
		row_bytes[1] = width * 2;
		return true;
	case VIDEO_FORMAT_YUY2:
		*n_planes = 1;
		planes[0] = 0;
		rows[0] = height;
		row_bytes[0] = width * 2;
		return true;
	case VIDEO_FORMAT_UYVY:
		*n_planes = 1;
		planes[0] = 0;
		rows[0] = height;
		row_bytes[0] = width * 2;
		return true;
	case VIDEO_FORMAT_BGRA:
	case VIDEO_FORMAT_BGRX:
	case VIDEO_FORMAT_RGBA:
	case VIDEO_FORMAT_BGR3:
		*n_planes = 1;
		planes[0] = 0;
		rows[0] = height;
		row_bytes[0] = width * 4;
		return true;
	default:
		return false;
	}
}

/* ------------------------------------------------------------------ network */

static const char *obc_socket_error_text(void)
{
#ifdef _WIN32
	return "see Winsock error (WSA) code";
#else
	return strerror(errno);
#endif
}

static void obc_set_send_timeout(obc_socket_t fd)
{
#ifdef _WIN32
	DWORD ms = OBC_SEND_TIMEOUT_MS;
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, (const char *)&ms, sizeof(ms));
#else
	struct timeval tv;
	tv.tv_sec = 0;
	tv.tv_usec = OBC_SEND_TIMEOUT_MS * 1000;
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif

	int sndbuf = OBC_SNDBUF_BYTES;
	setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (const char *)&sndbuf,
		   sizeof(sndbuf));

	int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *)&one,
		   sizeof(one));
}

/* A receive timeout turns an idle control channel into a loop iteration
 * instead of a parked thread, which is what lets shutdown be prompt. */
static void obc_set_recv_timeout(obc_socket_t fd, int ms)
{
#ifdef _WIN32
	DWORD window = (DWORD)ms;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, (const char *)&window,
		   sizeof(window));
#else
	struct timeval tv;
	tv.tv_sec = ms / 1000;
	tv.tv_usec = (ms % 1000) * 1000;
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
#endif
}

static void obc_set_nonblocking(obc_socket_t fd, bool on)
{
#ifdef _WIN32
	u_long mode = on ? 1 : 0;
	ioctlsocket(fd, FIONBIO, &mode);
#else
	int flags = fcntl(fd, F_GETFL, 0);
	if (flags < 0)
		return;
	fcntl(fd, F_SETFL, on ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK));
#endif
}

static int obc_socket_error(void)
{
#ifdef _WIN32
	return WSAGetLastError();
#else
	return errno;
#endif
}

/* errno/EAGAIN equivalents: a non-blocking operation that needs to wait, and a
 * timeout that is a poll interval rather than a failure. */
static bool obc_in_progress(void)
{
#ifdef _WIN32
	const int err = WSAGetLastError();
	return err == WSAEWOULDBLOCK || err == WSAEINPROGRESS;
#else
	return errno == EINPROGRESS;
#endif
}

static bool obc_would_block(void)
{
#ifdef _WIN32
	const int err = WSAGetLastError();
	return err == WSAEWOULDBLOCK || err == WSAETIMEDOUT;
#else
	return errno == EAGAIN || errno == EWOULDBLOCK;
#endif
}

static bool obc_send_all(obc_socket_t fd, const void *buf, size_t len)
{
	const uint8_t *p = buf;
	while (len > 0) {
		const int n = (int)send(fd, (const char *)p, (int)len,
					OBC_MSG_NOSIGNAL);
		if (n > 0) {
			p += n;
			len -= (size_t)n;
			continue;
		}
		if (n < 0 && obc_interrupted())
			continue;
		return false;
	}
	return true;
}

/* Send one plane. When OBS's stride is already tight this is a single write;
 * otherwise copy row by row so the receiver's layout never depends on stride. */
static bool obc_send_plane(struct obc_raw *ctx,
			   const uint8_t *data,
			   uint32_t linesize,
			   uint32_t row_bytes,
			   uint32_t rows)
{
	if (linesize == row_bytes || linesize == 0)
		return obc_send_all(ctx->fd, data, (size_t)row_bytes * rows);

	for (uint32_t y = 0; y < rows; y++) {
		if (!obc_send_all(ctx->fd, data + (size_t)y * linesize, row_bytes))
			return false;
	}
	return true;
}

/* ------------------------------------------------------------------- output */

static const char *obc_get_name(void *unused)
{
	UNUSED_PARAMETER(unused);
	return "Open Broadcast raw (uncompressed) frames";
}

static void obc_get_defaults(obs_data_t *settings)
{
	obs_data_set_default_string(settings, "target", OBC_DEFAULT_TARGET);
	/* Audio is on by default: the product wants synchronised audio. */
	obs_data_set_default_bool(settings, "audio", true);
}

static obs_properties_t *obc_get_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *target = obs_properties_add_text(
		props, "target", "Target host:port (the encoder listens here)",
		OBS_TEXT_DEFAULT);
	obs_property_set_long_description(
		target,
		"TCP endpoint that receives uncompressed frames. Env var "
		"OBC_RAW_TARGET overrides this when the output is started from "
		"the Tools menu.");
	obs_property_t *audio = obs_properties_add_bool(
		props, "audio", "Send audio (synchronised with the video)");
	obs_property_set_long_description(
		audio,
		"Sends the OBS audio mix on the same connection, timestamped from "
		"the same clock as the video so the receiver can align them. "
		"OBC_RAW_AUDIO=0 overrides this.");
	UNUSED_PARAMETER(data);
	return props;
}

static void *obc_create(obs_data_t *settings, obs_output_t *output)
{
	struct obc_raw *ctx = bzalloc(sizeof(struct obc_raw));
	ctx->output = output;
	ctx->fd = OBC_NO_SOCKET;
	obc_mutex_init(&ctx->lock);
	UNUSED_PARAMETER(settings);
	return ctx;
}

static void obc_destroy(void *data)
{
	struct obc_raw *ctx = data;
	if (ctx->fd != OBC_NO_SOCKET) {
		obc_close_socket(ctx->fd);
		ctx->fd = OBC_NO_SOCKET;
	}
	obc_mutex_destroy(&ctx->lock);
	bfree(ctx);
}

/* Tear the output down. end_data_capture() WAITS for the streaming thread to
 * return, and that thread takes ctx->lock in raw_video -- so this must never be
 * called with ctx->lock held, or OBS deadlocks on shutdown. */
static void obc_teardown(struct obc_raw *ctx)
{
	obc_mutex_lock(&ctx->lock);
	if (ctx->fd != OBC_NO_SOCKET) {
		obc_close_socket(ctx->fd);
		ctx->fd = OBC_NO_SOCKET;
	}
	ctx->connected = false;
	obc_mutex_unlock(&ctx->lock);
}

static uint64_t obc_now_ms(void)
{
#ifdef _WIN32
	return (uint64_t)GetTickCount64();
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000u + (uint64_t)ts.tv_nsec / 1000000u;
#endif
}

/* Re-dial the encoder after the link drops, at most once a second. The output
 * stays STARTED: an encoder restart must cost a second of frames, not the
 * operator's stream. */
static bool obc_reconnect(struct obc_raw *ctx)
{
	const uint64_t now = obc_now_ms();
	if (now < ctx->next_reconnect_ms)
		return false;
	ctx->next_reconnect_ms = now + 1000;

	struct addrinfo hints;
	struct addrinfo *res = NULL;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(ctx->host, ctx->port, &hints, &res) != 0)
		return false;

	obc_socket_t fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	bool ok = false;
	if (fd != OBC_NO_SOCKET) {
		obc_set_send_timeout(fd);
		if (connect(fd, res->ai_addr, res->ai_addrlen) == 0
		    && (!ctx->shdr_valid
			|| obc_send_all(fd, &ctx->shdr, sizeof(ctx->shdr)))) {
			ctx->fd = fd;
			ctx->connected = true;
			ctx->consecutive_failures = 0;
			ctx->logged_stride = false;
			ok = true;
			blog(LOG_INFO, "[obc-raw] reconnected to %s:%s", ctx->host,
			     ctx->port);
		} else {
			obc_close_socket(fd);
		}
	}
	freeaddrinfo(res);
	return ok;
}

static bool obc_start(void *data)
{
	struct obc_raw *ctx = data;
	obs_data_t *settings = obs_output_get_settings(ctx->output);

	const char *target = getenv("OBC_RAW_TARGET");
	if (!target || !*target)
		target = obs_data_get_string(settings, "target");
	if (!target || !*target)
		target = OBC_DEFAULT_TARGET;

	char host[256];
	char port[32];
	const char *colon = strrchr(target, ':');
	if (!colon || (size_t)(colon - target) >= sizeof(host)) {
		blog(LOG_ERROR, "[obc-raw] target '%s' must be host:port", target);
		obs_data_release(settings);
		return false;
	}
	memcpy(host, target, (size_t)(colon - target));
	host[colon - target] = '\0';
	snprintf(port, sizeof(port), "%s", colon + 1);

	snprintf(ctx->host, sizeof(ctx->host), "%s", host);
	snprintf(ctx->port, sizeof(ctx->port), "%s", port);

	struct obs_video_info ovi;
	if (!obs_get_video_info(&ovi)) {
		blog(LOG_ERROR, "[obc-raw] no video info; cannot report geometry");
		obs_data_release(settings);
		return false;
	}

	uint32_t rows[OBC_MAX_PLANES] = {0};
	uint32_t row_bytes[OBC_MAX_PLANES] = {0};
	uint32_t n_planes = 0;
	if (!obc_geometry(ovi.output_format, ovi.output_width, ovi.output_height,
			  (uint32_t[OBC_MAX_PLANES]){0}, rows, row_bytes, &n_planes)) {
		/* libobs already names every video_format for us; never hand-roll a
		 * table here. */
		blog(LOG_ERROR, "[obc-raw] unsupported output format %u (%s)",
		     (unsigned)ovi.output_format,
		     get_video_format_name(ovi.output_format));
		obs_data_release(settings);
		return false;
	}

	/* Audio is opt-out, and the announced rate/channels are part of the
	 * stream header, so resolve it before connecting. */
	bool want_audio = obs_data_get_bool(settings, "audio");
	const char *audio_env = getenv("OBC_RAW_AUDIO");
	if (audio_env && *audio_env)
		want_audio = strcmp(audio_env, "0") != 0;

	struct obs_audio_info oai;
	bool have_audio = want_audio && obs_get_audio_info(&oai) &&
			  oai.samples_per_sec > 0;
	ctx->audio_present = have_audio;
	ctx->audio_rate = have_audio ? oai.samples_per_sec : 0;
	ctx->audio_channels = have_audio ? get_audio_channels(oai.speakers) : 0;
	/* SPEAKERS_UNKNOWN means "fall back to stereo", per the enum. */
	if (have_audio && ctx->audio_channels == 0)
		ctx->audio_channels = 2;

	struct addrinfo hints;
	struct addrinfo *res = NULL;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	int gai = getaddrinfo(host, port, &hints, &res);
	if (gai != 0) {
		blog(LOG_ERROR, "[obc-raw] cannot resolve %s:%s (%s)", host, port,
		     gai_strerror(gai));
		obs_data_release(settings);
		return false;
	}

	obc_socket_t fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
	if (fd < 0) {
		blog(LOG_ERROR, "[obc-raw] socket() failed: %s", obc_socket_error_text());
		freeaddrinfo(res);
		obs_data_release(settings);
		return false;
	}
	obc_set_send_timeout(fd);

	if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
		blog(LOG_ERROR,
		     "[obc-raw] cannot connect to %s:%s: %s (is the encoder listening?)",
		     host, port, obc_socket_error_text());
		obc_close_socket(fd);
		freeaddrinfo(res);
		obs_data_release(settings);
		return false;
	}
	freeaddrinfo(res);

	ctx->fd = fd;
	ctx->connected = true;
	ctx->width = ovi.output_width;
	ctx->height = ovi.output_height;
	ctx->fps_num = ovi.fps_num;
	ctx->fps_den = ovi.fps_den;
	ctx->format = (uint32_t)ovi.output_format;
	ctx->planes = n_planes;

	uint32_t payload = 0;
	for (uint32_t i = 0; i < n_planes; i++) {
		ctx->rows[i] = rows[i];
		ctx->row_bytes[i] = row_bytes[i];
		ctx->plane_bytes[i] = row_bytes[i] * rows[i];
		payload += ctx->plane_bytes[i];
	}
	ctx->payload_bytes = payload;
	ctx->frames_sent = 0;
	ctx->frames_dropped = 0;
	ctx->audio_chunks_sent = 0;
	ctx->audio_chunks_dropped = 0;
	ctx->bytes_sent = 0;
	ctx->consecutive_failures = 0;
	ctx->logged_stride = false;
	ctx->stopped = false;

	struct obc_stream_header hdr;
	memset(&hdr, 0, sizeof(hdr));
	hdr.magic = OBC_MAGIC;
	hdr.version = OBC_VERSION;
	hdr.width = ctx->width;
	hdr.height = ctx->height;
	hdr.fps_num = ctx->fps_num;
	hdr.fps_den = ctx->fps_den;
	hdr.format = ctx->format;
	hdr.planes = ctx->planes;
	for (uint32_t i = 0; i < n_planes; i++)
		hdr.plane_bytes[i] = ctx->plane_bytes[i];
	hdr.audio_rate = ctx->audio_rate;
	hdr.audio_channels = ctx->audio_channels;
	hdr.audio_format = ctx->audio_present ? OBC_AUDIO_F32_PLANAR : 0;

	ctx->shdr = hdr;
	ctx->shdr_valid = true;
	if (!obc_send_all(fd, &hdr, sizeof(hdr))) {
		blog(LOG_ERROR, "[obc-raw] failed to send stream header");
		obc_teardown(ctx);
		obs_data_release(settings);
		return false;
	}

	/* No encoders are attached: this is what makes OBS deliver raw frames.
	 * Audio is requested the same way -- with no service set, nothing else
	 * has to be configured for OBS to hand the mix over. */
	const uint32_t capture_flags =
		OBS_OUTPUT_VIDEO | (ctx->audio_present ? OBS_OUTPUT_AUDIO : 0u);

	obs_output_set_media(ctx->output, obs_get_video(), obs_get_audio());
	if (!obs_output_can_begin_data_capture(ctx->output, capture_flags)) {
		blog(LOG_ERROR,
		     "[obc-raw] cannot begin raw capture (flags 0x%x)",
		     (unsigned)capture_flags);
		obc_teardown(ctx);
		obs_data_release(settings);
		return false;
	}
	if (!obs_output_begin_data_capture(ctx->output, capture_flags)) {
		blog(LOG_ERROR, "[obc-raw] begin_data_capture failed");
		obc_teardown(ctx);
		obs_data_release(settings);
		return false;
	}

	blog(LOG_INFO,
	     "[obc-raw] streaming uncompressed %ux%u @ %u/%u format=%u planes=%u "
	     "payload=%u bytes/frame (~%.1f Mbit/s) to %s:%s",
	     ctx->width, ctx->height, ctx->fps_num, ctx->fps_den, ctx->format,
	     ctx->planes, ctx->payload_bytes,
	     (double)ctx->payload_bytes * ctx->fps_num / ctx->fps_den * 8.0 / 1e6,
	     host, port);
	if (ctx->audio_present)
		blog(LOG_INFO,
		     "[obc-raw] audio: %u Hz, %u channel(s), float32 planar "
		     "(~%.1f Mbit/s), timestamps shared with the video clock",
		     ctx->audio_rate, ctx->audio_channels,
		     (double)ctx->audio_rate * ctx->audio_channels * 32.0 / 1e6);
	else
		blog(LOG_WARNING,
		     "[obc-raw] audio disabled; sending video only");

	obs_data_release(settings);
	return true;
}

static void obc_stop(void *data, uint64_t ts)
{
	struct obc_raw *ctx = data;
	UNUSED_PARAMETER(ts);

	/* libobs can deliver a single shutdown through more than one path (the
	 * frontend teardown and module unload both call stop), so make this
	 * idempotent: report and tear down exactly once per start. */
	obc_mutex_lock(&ctx->lock);
	if (ctx->stopped) {
		obc_mutex_unlock(&ctx->lock);
		return;
	}
	ctx->stopped = true;
	obc_mutex_unlock(&ctx->lock);

	/* Unlocked on purpose: see obc_teardown(). */
	obc_teardown(ctx);

	/* Unconditional, and exactly once per start: libobs calls `stop` during
	 * shutdown AFTER the output stops being active, so gating this on
	 * obs_output_active() left the end unsignalled and obs_output_release()
	 * waiting on an event that never came -- OBS hung on exit. */
	obs_output_end_data_capture(ctx->output);

	obc_mutex_lock(&ctx->lock);
	uint64_t sent = ctx->frames_sent;
	uint64_t dropped = ctx->frames_dropped;
	uint64_t chunks = ctx->audio_chunks_sent;
	uint64_t chunks_dropped = ctx->audio_chunks_dropped;
	obc_mutex_unlock(&ctx->lock);

	blog(LOG_INFO,
	     "[obc-raw] stopped after %llu frames (%llu dropped) and %llu audio "
	     "chunks (%llu dropped)",
	     (unsigned long long)sent, (unsigned long long)dropped,
	     (unsigned long long)chunks, (unsigned long long)chunks_dropped);
}

static void obc_raw_video(void *data, struct video_data *frame)
{
	struct obc_raw *ctx = data;

	obc_mutex_lock(&ctx->lock);
	if (!ctx->connected || ctx->fd < 0) {
		if (!obc_reconnect(ctx)) {
			obc_mutex_unlock(&ctx->lock);
			return;
		}
	}

	/* Planes are repacked to the tight layout announced at connect, so OBS's
	 * delivery stride never reaches the wire. Report it once per session
	 * anyway: a stride wider than the row is what triggers the per-row copy,
	 * and this is the only place that becomes visible. */
	bool ok = true;
	if (!ctx->logged_stride) {
		ctx->logged_stride = true;
		blog(LOG_INFO,
		     "[obc-raw] first frame: OBS linesize=[%u,%u,%u], tight row bytes=[%u,%u,%u]",
		     frame->linesize[0], frame->linesize[1], frame->linesize[2],
		     ctx->row_bytes[0], ctx->row_bytes[1], ctx->row_bytes[2]);
	}

	struct obc_frame_header fh;
	fh.timestamp = frame->timestamp;
	fh.kind = OBC_KIND_VIDEO;
	fh.payload_bytes = ctx->payload_bytes;
	fh.units = ctx->planes;
	fh.reserved = 0;

	if (!obc_send_all(ctx->fd, &fh, sizeof(fh)))
		ok = false;

	for (uint32_t i = 0; ok && i < ctx->planes; i++) {
		uint32_t ls = frame->linesize[i];
		if (ls == 0)
			ls = ctx->row_bytes[i];
		ok = obc_send_plane(ctx, frame->data[i], ls, ctx->row_bytes[i],
				    ctx->rows[i]);
	}

	bool signal_stop = false;
	if (ok) {
		ctx->frames_sent++;
		ctx->bytes_sent += (uint64_t)sizeof(fh) + ctx->payload_bytes;
		ctx->consecutive_failures = 0;
	} else {
		ctx->frames_dropped++;
		ctx->consecutive_failures++;
		if (ctx->consecutive_failures >= OBC_MAX_CONSECUTIVE_FAILURES) {
			ctx->connected = false;
			if (ctx->fd != OBC_NO_SOCKET) {
				obc_close_socket(ctx->fd);
				ctx->fd = OBC_NO_SOCKET;
			}
			ctx->consecutive_failures = 0;
			signal_stop = true;
		}
	}

	obc_mutex_unlock(&ctx->lock);

	/* Signalled outside the lock: the stop path takes this same lock, so
	 * calling it while held would deadlock the video thread. */
	if (signal_stop) {
		blog(LOG_WARNING,
		     "[obc-raw] peer lost; dropping frames and reconnecting");
	}
}

/* Audio arrives as PLANAR float32 -- one plane per channel, `frames` samples
 * each, 4 bytes per sample -- and is sent in exactly that shape, plane by
 * plane, so neither side pays for an interleaving copy.
 *
 * The synchronisation lives in the timestamp: OBS stamps this from the same
 * clock it stamps video with, and we forward it untouched. Re-stamping here (or
 * on the far side) is what would actually break A/V sync. */
static void obc_raw_audio(void *data, struct audio_data *frame)
{
	struct obc_raw *ctx = data;

	obc_mutex_lock(&ctx->lock);
	if (!ctx->connected || ctx->fd < 0 || !ctx->audio_present ||
	    frame->frames == 0 || ctx->audio_channels == 0) {
		obc_mutex_unlock(&ctx->lock);
		return;
	}

	const uint32_t channels = ctx->audio_channels;
	const uint32_t plane_bytes = frame->frames * 4u; /* float32 */

	struct obc_frame_header fh;
	fh.timestamp = frame->timestamp;
	fh.kind = OBC_KIND_AUDIO;
	fh.payload_bytes = plane_bytes * channels;
	fh.units = frame->frames;
	fh.reserved = 0;

	bool ok = obc_send_all(ctx->fd, &fh, sizeof(fh));
	for (uint32_t c = 0; ok && c < channels; c++)
		ok = obc_send_all(ctx->fd, frame->data[c], plane_bytes);

	bool signal_stop = false;
	if (ok) {
		ctx->audio_chunks_sent++;
		ctx->bytes_sent += (uint64_t)sizeof(fh) + fh.payload_bytes;
	} else {
		/* Deliberately does NOT reset the failure run: video owns the
		 * reset, so a healthy audio stream cannot mask a dead video
		 * one. Both fail together anyway -- they share the socket. */
		ctx->audio_chunks_dropped++;
		ctx->consecutive_failures++;
		if (ctx->consecutive_failures >= OBC_MAX_CONSECUTIVE_FAILURES) {
			ctx->connected = false;
			if (ctx->fd != OBC_NO_SOCKET) {
				obc_close_socket(ctx->fd);
				ctx->fd = OBC_NO_SOCKET;
			}
			ctx->consecutive_failures = 0;
			signal_stop = true;
		}
	}

	obc_mutex_unlock(&ctx->lock);

	if (signal_stop) {
		blog(LOG_WARNING,
		     "[obc-raw] peer lost while sending audio; dropping and reconnecting");
	}
}

static uint64_t obc_get_total_bytes(void *data)
{
	struct obc_raw *ctx = data;
	uint64_t bytes;
	obc_mutex_lock(&ctx->lock);
	bytes = ctx->bytes_sent;
	obc_mutex_unlock(&ctx->lock);
	return bytes;
}

static int obc_get_dropped_frames(void *data)
{
	struct obc_raw *ctx = data;
	int dropped;
	obc_mutex_lock(&ctx->lock);
	/* OBS shows this number as "dropped frames"; audio drops are invisible
	 * otherwise, so count them too. */
	dropped = (int)(ctx->frames_dropped + ctx->audio_chunks_dropped);
	obc_mutex_unlock(&ctx->lock);
	return dropped;
}

static struct obs_output_info obc_output_info = {
	.id = "obc_raw_output",
	/* AV, not VIDEO: this output is able to take audio, and OBS only offers
	 * the mix to raw_audio() when the flag says so. */
	.flags = OBS_OUTPUT_AV,
	.get_name = obc_get_name,
	.create = obc_create,
	.destroy = obc_destroy,
	.start = obc_start,
	.stop = obc_stop,
	.raw_video = obc_raw_video,
	.raw_audio = obc_raw_audio,
	.get_defaults = obc_get_defaults,
	.get_properties = obc_get_properties,
	.get_total_bytes = obc_get_total_bytes,
	.get_dropped_frames = obc_get_dropped_frames,
};

/* -------------------------------------------------------------------- menu */

/* Implemented in obc-settings-dialog.cpp (Qt). Fills target/audio and returns
 * true only when the operator accepted the dialog. */
extern bool obc_settings_dialog_run(char *target, size_t target_len,
				    bool *audio);

/* Settings live in the plugin's own config, never in the OBS profile: the
 * operator's target has to survive a restart, and the profile belongs to the
 * user. Missing keys mean "never set", not "send nowhere". */
static obs_data_t *obc_load_config(void)
{
	char *path = obs_module_config_path("settings.json");
	obs_data_t *data = NULL;

	if (path) {
		data = obs_data_create_from_json_file(path);
		bfree(path);
	}
	if (!data)
		data = obs_data_create();
	return data;
}

static void obc_save_config(const char *target, bool audio)
{
	char *dir = obs_module_config_path("");
	if (dir) {
		os_mkdirs(dir);
		bfree(dir);
	}

	char *path = obs_module_config_path("settings.json");
	if (!path)
		return;

	obs_data_t *data = obs_data_create();
	obs_data_set_string(data, "target", target);
	obs_data_set_bool(data, "audio", audio);
	obs_data_save_json(data, path);
	obs_data_release(data);
	bfree(path);
}

/* Registered with no service, so nothing in Settings starts it. Like the NDI
 * output, it is driven from Tools: one item starts/stops it, one opens the
 * settings dialog. OBC_RAW_TARGET still overrides the saved target, which
 * keeps automated runs scriptable. */
static obs_output_t *obc_menu_output = NULL;

/* Implemented in menu-action.cpp (Qt): a checkable Tools item, because the
 * plain menu-item API gives the operator no sign of whether the output runs. */
extern void *obc_menu_action_create(const char *name, void (*on_toggled)(bool));
extern void obc_menu_action_set_checked(void *action, bool checked);

static void *obc_menu_action = NULL;

static void obc_menu_on_started(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(cd);
	blog(LOG_INFO, "[obc-raw] output running");
	obc_menu_action_set_checked(obc_menu_action, true);
}

static void obc_menu_on_stopped(void *data, calldata_t *cd)
{
	UNUSED_PARAMETER(data);
	UNUSED_PARAMETER(cd);
	blog(LOG_INFO, "[obc-raw] output stopped");
	obc_menu_action_set_checked(obc_menu_action, false);
}

static void obc_start_output(void)
{
	if (obc_menu_output && obs_output_active(obc_menu_output))
		return;

	/* The operator's saved target, falling back to the built-in default.
	 * Only the Tools-menu path reads the saved value; OBC_RAW_TARGET still
	 * overrides it inside obc_start(). */
	obs_data_t *saved = obc_load_config();
	obs_data_t *settings = obs_data_create();
	obs_data_set_default_string(settings, "target", OBC_DEFAULT_TARGET);
	obs_data_set_default_bool(settings, "audio", true);

	const char *saved_target = obs_data_get_string(saved, "target");
	if (saved_target && *saved_target)
		obs_data_set_string(settings, "target", saved_target);
	if (obs_data_has_user_value(saved, "audio"))
		obs_data_set_bool(settings, "audio",
				  obs_data_get_bool(saved, "audio"));
	obs_data_release(saved);

	obc_menu_output = obs_output_create("obc_raw_output", "obc_raw_output",
					    settings, NULL);
	obs_data_release(settings);

	if (!obc_menu_output) {
		blog(LOG_ERROR, "[obc-raw] could not create output");
		obc_menu_action_set_checked(obc_menu_action, false);
		return;
	}

	/* The menu state follows the output, not the click: OBS can stop it on
	 * its own (reconnect failure, exit), and a checkbox that lies is worse
	 * than none. */
	signal_handler_t *sh = obs_output_get_signal_handler(obc_menu_output);
	signal_handler_connect(sh, "start", obc_menu_on_started, NULL);
	signal_handler_connect(sh, "stop", obc_menu_on_stopped, NULL);

	if (!obs_output_start(obc_menu_output)) {
		blog(LOG_ERROR,
		     "[obc-raw] start failed (see the error above; is the receiver listening?)");
		obc_menu_action_set_checked(obc_menu_action, false);
	}
}

static void obc_stop_output(void)
{
	if (obc_menu_output && obs_output_active(obc_menu_output))
		obs_output_stop(obc_menu_output);
}

static void obc_menu_action_toggled(bool checked)
{
	if (checked) {
		obc_start_output();
		return;
	}
	obc_stop_output();
}

/* ------------------------------------------------------- encoder control */

/* The encoder says when to run. It opens a control port beside its frame
 * listener (frame port + 1, derived the same way on both sides) and writes one
 * word per line: `start` while it is previewing or encoding, `stop` when that
 * ends. So the operator starts the stream in the encoder and this output comes
 * with it, instead of holding a Tools toggle in step with it -- forgetting that
 * toggle looked exactly like a broken encoder.
 *
 * The channel only ever asks for a transition; it never drives the output
 * itself. Both land on the UI thread through obs_queue_task, which is where
 * every other start/stop here runs, and it is stopped before the frontend goes
 * away so no task is queued into a dead UI.
 *
 * A lost channel is not a stop: the encoder restarts routinely, so this thread
 * re-dials and the ingest's own reconnect covers the frames. The Tools toggle
 * stays as the manual override.
 */
#define OBC_CONTROL_RETRY_MS 1000
#define OBC_CONTROL_RECV_TIMEOUT_MS 500
#define OBC_CONTROL_CONNECT_WAIT_MS 1000

static obc_mutex_t obc_control_lock;
static bool obc_control_stop = true;
static obc_thread_t obc_control_thread;
static bool obc_control_thread_started = false;

static bool obc_control_stopping(void)
{
	bool stopping;
	obc_mutex_lock(&obc_control_lock);
	stopping = obc_control_stop;
	obc_mutex_unlock(&obc_control_lock);
	return stopping;
}

/* Sleeps in slices so a stop request is honoured promptly. */
static void obc_control_pause(int ms)
{
	int waited = 0;
	while (waited < ms && !obc_control_stopping()) {
		os_sleep_ms(50);
		waited += 50;
	}
}

static void obc_control_apply_start(void *param)
{
	UNUSED_PARAMETER(param);
	obc_start_output();
}

static void obc_control_apply_stop(void *param)
{
	UNUSED_PARAMETER(param);
	obc_stop_output();
}

static void obc_control_handle_line(const char *line)
{
	if (strcmp(line, "start") == 0) {
		obs_queue_task(OBS_TASK_UI, obc_control_apply_start, NULL,
			       false);
	} else if (strcmp(line, "stop") == 0) {
		obs_queue_task(OBS_TASK_UI, obc_control_apply_stop, NULL,
			       false);
	} else if (*line) {
		blog(LOG_WARNING, "[obc-raw] control: ignoring '%s'", line);
	}
}

/* The target the output would use, so the channel follows the same host:port
 * the frames go to and its port + 1. */
static void obc_control_target(char *out, size_t len)
{
	const char *env = getenv("OBC_RAW_TARGET");
	if (env && *env) {
		snprintf(out, len, "%s", env);
		return;
	}

	obs_data_t *saved = obc_load_config();
	const char *target = obs_data_get_string(saved, "target");
	if (target && *target)
		snprintf(out, len, "%s", target);
	else
		snprintf(out, len, "%s", OBC_DEFAULT_TARGET);
	obs_data_release(saved);
}

/* A filtered port would otherwise hold this thread for the OS connect timeout,
 * and OBS's exit waits on the join. */
static obc_socket_t obc_control_dial(const char *host, const char *port)
{
	struct addrinfo hints;
	struct addrinfo *res = NULL;
	memset(&hints, 0, sizeof(hints));
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	if (getaddrinfo(host, port, &hints, &res) != 0)
		return OBC_NO_SOCKET;

	obc_socket_t fd = socket(res->ai_family, res->ai_socktype,
				 res->ai_protocol);
	obc_socket_t ok = OBC_NO_SOCKET;

	if (fd != OBC_NO_SOCKET) {
		obc_set_nonblocking(fd, true);
		if (connect(fd, res->ai_addr, res->ai_addrlen) == 0) {
			ok = fd;
		} else if (obc_in_progress()) {
			int waited = 0;
			while (waited < OBC_CONTROL_CONNECT_WAIT_MS &&
			       !obc_control_stopping()) {
				fd_set wfds;
				struct timeval tv;
				FD_ZERO(&wfds);
				FD_SET(fd, &wfds);
				tv.tv_sec = 0;
				tv.tv_usec = 100 * 1000;
				const int ready = select((int)fd + 1, NULL,
							 &wfds, NULL, &tv);
				if (ready > 0) {
					int err = 0;
#ifdef _WIN32
					int elen = (int)sizeof(err);
#else
					socklen_t elen = sizeof(err);
#endif
					getsockopt(fd, SOL_SOCKET, SO_ERROR,
						   (char *)&err, &elen);
					if (err == 0)
						ok = fd;
					break;
				}
				if (ready < 0 && !obc_interrupted())
					break;
				waited += 100;
			}
		}
	}
	freeaddrinfo(res);

	if (ok == OBC_NO_SOCKET) {
		if (fd != OBC_NO_SOCKET)
			obc_close_socket(fd);
		return OBC_NO_SOCKET;
	}

	obc_set_nonblocking(ok, false);
	obc_set_recv_timeout(ok, OBC_CONTROL_RECV_TIMEOUT_MS);
	return ok;
}

static void *obc_control_main(void *arg)
{
	UNUSED_PARAMETER(arg);

	char target[256];
	obc_control_target(target, sizeof(target));

	char host[256];
	char port[32];
	const char *colon = strrchr(target, ':');
	if (!colon || (size_t)(colon - target) >= sizeof(host)) {
		blog(LOG_WARNING,
		     "[obc-raw] control: target '%s' must be host:port; the output has to be started manually",
		     target);
		return 0;
	}
	memcpy(host, target, (size_t)(colon - target));
	host[colon - target] = '\0';
	snprintf(port, sizeof(port), "%s", colon + 1);

	const long frame_port = strtol(port, NULL, 10);
	if (frame_port <= 0 || frame_port >= 65535) {
		blog(LOG_WARNING,
		     "[obc-raw] control: no port above '%s' to watch; the output has to be started manually",
		     port);
		return 0;
	}
	char control_port[16];
	snprintf(control_port, sizeof(control_port), "%ld", frame_port + 1);

	blog(LOG_INFO,
	     "[obc-raw] control: following %s:%s for the encoder's start/stop",
	     host, control_port);

	char line[128];
	size_t line_len = 0;

	while (!obc_control_stopping()) {
		obc_socket_t fd = obc_control_dial(host, control_port);
		if (fd == OBC_NO_SOCKET) {
			obc_control_pause(OBC_CONTROL_RETRY_MS);
			continue;
		}

		blog(LOG_INFO, "[obc-raw] control: channel up");
		line_len = 0;

		for (;;) {
			if (obc_control_stopping())
				break;

			char buf[64];
			const int got = (int)recv(fd, buf, sizeof(buf), 0);
			if (got == 0) {
				blog(LOG_INFO,
				     "[obc-raw] control: encoder closed the channel");
				break;
			}
			if (got < 0) {
				if (obc_interrupted() || obc_would_block())
					continue;
				blog(LOG_INFO,
				     "[obc-raw] control: channel error %d",
				     obc_socket_error());
				break;
			}

			for (int i = 0; i < got; i++) {
				const char c = buf[i];
				if (c == '\n') {
					line[line_len] = '\0';
					obc_control_handle_line(line);
					line_len = 0;
					continue;
				}
				if (c == '\r')
					continue;
				if (line_len + 1 < sizeof(line))
					line[line_len++] = c;
				else
					line_len = 0; /* runaway word: drop it */
			}
		}

		obc_close_socket(fd);
		obc_control_pause(OBC_CONTROL_RETRY_MS);
	}

	return 0;
}

/* CreateThread's entry point has a different signature, so the control loop is
 * the same function on both platforms and only this shim differs. */
#ifdef _WIN32
static DWORD WINAPI obc_control_entry(LPVOID arg)
{
	obc_control_main(arg);
	return 0;
}
#else
#  define obc_control_entry obc_control_main
#endif

static void obc_control_start_thread(void)
{
	obc_mutex_lock(&obc_control_lock);
	obc_control_stop = false;
	obc_mutex_unlock(&obc_control_lock);

	if (!obc_thread_start(&obc_control_thread, obc_control_entry, NULL)) {
		blog(LOG_WARNING,
		     "[obc-raw] control: no channel thread; the output has to be started manually");
		return;
	}
	obc_control_thread_started = true;
}

static void obc_control_stop_thread(void)
{
	if (!obc_control_thread_started)
		return;

	obc_mutex_lock(&obc_control_lock);
	obc_control_stop = true;
	obc_mutex_unlock(&obc_control_lock);

	obc_thread_join(obc_control_thread);
	obc_control_thread_started = false;
}

/* The settings dialog is the only place these two values can be set: the
 * output has no service, so OBS shows its properties nowhere else. */
static void obc_menu_settings(void *private_data)
{
	UNUSED_PARAMETER(private_data);

	obs_data_t *saved = obc_load_config();
	char target[256];
	bool audio = true;

	const char *saved_target = obs_data_get_string(saved, "target");
	if (saved_target && *saved_target)
		snprintf(target, sizeof(target), "%s", saved_target);
	else
		snprintf(target, sizeof(target), "%s", OBC_DEFAULT_TARGET);
	if (obs_data_has_user_value(saved, "audio"))
		audio = obs_data_get_bool(saved, "audio");
	obs_data_release(saved);

	if (!obc_settings_dialog_run(target, sizeof(target), &audio)) {
		blog(LOG_INFO,
		     "[obc-raw] settings dialog closed without changes");
		return;
	}

	obc_save_config(target, audio);

	/* Re-create the output when it is live, so the operator sees the change
	 * take effect on OK rather than at some later start. */
	const bool was_active =
		obc_menu_output && obs_output_active(obc_menu_output);
	if (was_active)
		obs_output_stop(obc_menu_output);
	if (obc_menu_output) {
		obs_output_release(obc_menu_output);
		obc_menu_output = NULL;
	}

	blog(LOG_INFO, "[obc-raw] target %s, audio %s", target,
	     audio ? "on" : "off");
	if (was_active)
		obc_start_output();
}

/* Automated runs (and CI) have no way to open the Tools menu, so allow the
 * output to start itself once OBS has finished loading. Opt-in, never default:
 * OBC_RAW_AUTOSTART=1 ./obs */
static bool obc_frontend_exit_seen = false;

static void obc_frontend_event(enum obs_frontend_event event, void *private_data)
{
	UNUSED_PARAMETER(private_data);

	if (event == OBS_FRONTEND_EVENT_EXIT) {
		/* libobs unloads modules AFTER it has torn the frontend down. By
		 * then obs_frontend_remove_event_callback() finds no callbacks
		 * and OBS logs
		 *   "Tried to call obs_frontend_remove_event_callback with no callbacks!"
		 * The frontend announces its own teardown here, so remember it
		 * and let obs_module_unload() skip the doomed remove. */
		obc_frontend_exit_seen = true;
		/* The control thread queues work onto the UI thread, which is
		 * going away: stop it before that, not after. */
		obc_control_stop_thread();
		return;
	}

	if (event != OBS_FRONTEND_EVENT_FINISHED_LOADING)
		return;

	const char *auto_start = getenv("OBC_RAW_AUTOSTART");
	if (auto_start && *auto_start && strcmp(auto_start, "0") != 0) {
		blog(LOG_INFO, "[obc-raw] OBC_RAW_AUTOSTART set: starting output");
		obc_start_output();
	}
}

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("open-broadcast-raw-output", "en-US")

bool obs_module_load(void)
{
#ifdef _WIN32
	WSADATA wsa;
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
		blog(LOG_ERROR, "[obc-raw] WSAStartup failed");
#endif
	obs_register_output(&obc_output_info);
	obc_menu_action = obc_menu_action_create(
		"Open Broadcast raw output (start/stop manually)",
		obc_menu_action_toggled);
	obs_frontend_add_tools_menu_item(
		"Open Broadcast raw output (settings...)", obc_menu_settings,
		NULL);
	obs_frontend_add_event_callback(obc_frontend_event, NULL);

	/* The encoder starts and stops this output; the Tools items remain the
	 * manual override for an encoder that is not running the channel. */
	obc_mutex_init(&obc_control_lock);
	obc_control_start_thread();

	blog(LOG_INFO,
	     "[obc-raw] loaded: uncompressed frame output registered as 'obc_raw_output'");
	return true;
}

void obs_module_unload(void)
{
	/* Before the output goes: the thread only queues UI work, but it must
	 * not outlive the frontend it queues into. */
	obc_control_stop_thread();
	obc_mutex_destroy(&obc_control_lock);

	/* libobs unloads modules after the frontend has been shut down, so on a
	 * normal exit OBS_FRONTEND_EVENT_EXIT has already fired and the
	 * frontend callbacks are gone; removing ours then would only log
	 * "Tried to call obs_frontend_remove_event_callback with no callbacks!".
	 * Remove only when the exit was not seen -- i.e. the module is being
	 * unloaded while the frontend is still alive. */
	if (!obc_frontend_exit_seen)
		obs_frontend_remove_event_callback(obc_frontend_event, NULL);

	if (obc_menu_output != NULL) {
		if (obs_output_active(obc_menu_output))
			obs_output_stop(obc_menu_output);
		obs_output_release(obc_menu_output);
		obc_menu_output = NULL;
	}
}
