# open-broadcast raw output (OBS plugin)

A thin OBS output that hands over **uncompressed frames** — video **and synchronised audio** — so the
encoder on the far end is the only encoder in the chain.

This is Option A of `open-broadcast-product/OBS_INGEST_OPTIONS.md` / `DECISIONS.md` DT-7, the actual target.
The MPEG-TS leg in `../obs-phase0/` is only a transport stand-in: it lets OBS encode and then decodes on the
far side, which **double-encodes**. Here OBS supplies raw pixels and the encoder's own pipeline does the only
encode.

## What it is and why it exists

The plugin registers an `obs_output` (`obc_raw_output`) with **no video encoder attached**. That is the whole
mechanism: with no encoder, libobs delivers frames to `obs_output_info.raw_video()` unencoded. The plugin
copies each plane, tightens any line-stride padding, and writes it to a TCP socket.

It does the same for audio. Setting `.flags = OBS_OUTPUT_AV` and requesting `OBS_OUTPUT_AUDIO` makes libobs
hand the mix to `obs_output_info.raw_audio()`, so the synchronised audio goes out on the **same connection**,
carrying OBS's own timestamps. With no service set, nothing else has to be configured.

No encoding, no muxing, no container. At 1080p60 NV12 a frame is **3,110,400 bytes** (1920×1080×1.5), i.e.
~**1493 Mbit/s** on the wire — this is a LAN/loopback transport, not a WAN one.

## Download

A prebuilt Windows package is published with every release — no compiler, no OBS SDK:

| Platform | File |
|---|---|
| Windows 10/11 64-bit | `open-broadcast-raw-output-windows-x64.zip` |

- Latest release: <https://github.com/patcarter883/obs-raw-output/releases/latest>
- Download page: <https://stream.huntervalleyit.net.au/download>

The zip is laid out as OBS expects, so extract it into the OBS install directory
(`C:\Program Files\obs-studio\`), which puts the module at
`obs-plugins\64bit\open-broadcast-raw-output.dll`. OBS scans that tree on start;
the output then appears under **Settings → Output** as an open-broadcast output.
Verify the published `.sha256` before extracting: the build is not code-signed.

**A plugin loads only into the OBS that built it.** The release states the OBS
version it targets, and an OBS upgrade can invalidate it — if OBS stops loading
the module after an update, rebuild from source against the new headers (below).
Only Windows ships a binary: on Linux the module is built against your distro's
OBS, and there is no macOS build.

## Build and install

Requires the libobs and obs-frontend-api dev headers (`pkg-config libobs obs-frontend-api`) plus Qt6 Widgets.
Present on the reference host with OBS Studio 33.0.1 (32.2.2 before that), cmake ≥ 3.28 and a C++17
compiler. The headers and libraries come from the OBS version you build against — that is the OBS the
resulting plugin loads into.

```sh
cmake -S . -B build -D CMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
cmake --install build
```

`cmake --install` copies the module to:

```
~/.config/obs-studio/plugins/open-broadcast-raw-output/bin/64bit/open-broadcast-raw-output.so
```

OBS loads a plugin from `<config>/obs-studio/plugins/<name>/bin/64bit/<name>.so`, so the shared object **must
be named exactly after its directory** — hence `PREFIX ""` in `CMakeLists.txt`. To install into a different
tree (e.g. an isolated test config), copy the built object by hand:

```sh
cp build/open-broadcast-raw-output.so \
   "$XDG_CONFIG_HOME"/obs-studio/plugins/open-broadcast-raw-output/bin/64bit/open-broadcast-raw-output.so
```

A Qt settings dialog is compiled in from `src/obc-settings-dialog.cpp`; it uses no `Q_OBJECT`/moc
machinery, so there is no generated-code step.

## Operator settings

The output is registered with **no service**, so OBS shows its properties nowhere. It is driven from the
**Tools** menu, the NDI-output pattern:

- **Tools → "Open Broadcast raw output (start/stop)"** — toggles the output.
- **Tools → "Open Broadcast raw output (settings...)"** — a Qt dialog with two fields: the **Target
  host:port** and a **"Send audio, synchronised with the video"** checkbox.

Settings persist in the plugin's **own config**, never in the OBS profile (the profile belongs to the user):

```
<config>/obs-studio/plugin_config/open-broadcast-raw-output/settings.json
```

e.g. `{"target":"127.0.0.1:9300","audio":true}`. The keys are **`target`** (string) and **`audio`** (bool);
missing keys mean "never set", not "send nowhere". Applying the dialog while the output is live re-creates it
so the change takes effect immediately.

> The send-audio setting's JSON key is **`audio`**, not `send_audio`; the environment override is
> **`OBC_RAW_AUDIO`**. Audio is **opt-out** — on by default.

## Starting and stopping: the encoder drives it

The output follows the encoder. The encoder opens a control port beside its frame listener — **frame
port + 1**, so a target of `127.0.0.1:9300` pairs with a control port of `9301` — and writes one word per
line:

| Line | Meaning |
|---|---|
| `start` | The encoder is previewing or encoding: frames are wanted. |
| `stop` | That ended: stop sending. |

The channel is **state, not events**. A plugin that connects late — OBS started after the encoder, or
restarted mid-session — is told the current state on accept, so OBS resumes into a running encoder
without a click. Start is idempotent: `start` while already running changes nothing.

Behaviour that matters operationally:

- **A lost channel is not a stop.** The encoder restarts routinely, so the plugin keeps its output and
  re-dials the channel every second; the frame socket has its own reconnect. Only an explicit `stop`
  stops the output.
- **The Tools items stay the manual override.** Starting from the menu still works when the encoder is
  not running the channel, and `OBC_RAW_AUTOSTART` still starts the output at load for automated runs.
  The menu check state follows the output itself, not the click.
- **The channel never touches the output directly.** Both transitions are queued onto the OBS UI thread
  with `obs_queue_task(OBS_TASK_UI, …)`, which is where every other start/stop in this plugin runs, and
  the thread is joined at `OBS_FRONTEND_EVENT_EXIT` so nothing is queued into a dead UI.

## Defaults

| Setting | Default | Notes |
|---|---|---|
| `target` | `127.0.0.1:9300` | Matches the encoder's `raw_local_input::default_port` (`open-broadcast-encoder/source/encode/raw_local.h:46`). Both ends agree out of the box. |
| `audio` | `true` | Audio is on by default; the product wants synchronised audio. |

## Environment overrides (automated runs)

An output with no service cannot be started by clicking menus in a headless/automated run, so:

| Env var | Effect |
|---|---|
| `OBC_RAW_TARGET=host:port` | Overrides the saved/default target inside `obc_start()`. |
| `OBC_RAW_AUTOSTART=1` | Starts the output itself at `OBS_FRONTEND_EVENT_FINISHED_LOADING`. Any value other than `0` enables it; opt-in, never default. |
| `OBC_RAW_AUDIO=0` | Disables audio, overriding the saved `audio` setting. |

## Wire format (version 2)

All integers are **little-endian**. Structures are plain `uint32`/`uint64`, so there is no padding to
disagree about. `v1` was video-only with a 16-byte frame header and nothing consumed it, so there is **no
compatibility shim — receivers must accept v2 only**.

### Stream header — once, on connect — 23 × `u32` = **92 bytes**

| # | Field | Type | Units / meaning |
|---|---|---|---|
| 1 | `magic` | u32 | `0x4F424352` (`'OBCR'`) |
| 2 | `version` | u32 | `2` |
| 3 | `width` | u32 | pixels |
| 4 | `height` | u32 | pixels |
| 5 | `fps_num` | u32 | frames-per-second numerator |
| 6 | `fps_den` | u32 | frames-per-second denominator |
| 7 | `obs_format` | u32 | OBS `enum video_format` declaration order (I420=1, NV12=2, YUY2=4, UYVY=5, BGRA=7, …) |
| 8 | `planes` | u32 | number of video planes |
| 9 | `linesize[3]` | u32×3 | **always sent as 0** — see note below |
| 10 | `plane_bytes[3]` | u32×3 | tight plane sizes in bytes (the truth for layout) |
| 11 | `reserved` | u32 | 0 |
| 12 | `audio_rate` | u32 | sample rate in Hz; **0 means this source has no audio** |
| 13 | `audio_channels` | u32 | channel count (0 when no audio) |
| 14 | `audio_format` | u32 | `1` = float32 planar; `0` = none |
| 15 | `audio_reserved[5]` | u32×5 | 0 |

`linesize` is **zero** because it is not known until the first frame arrives, while this header is written at
connect time. **Receivers must ignore it and derive layout from `plane_bytes` alone.** The plugin logs the
first frame's real OBS stride at `LOG_INFO` so a repacking copy is visible.

When audio is disabled, `audio_rate`/`audio_channels`/`audio_format` are all 0 and no audio chunks appear
(verified — see below).

### Frame header — per video frame and per audio chunk — **24 bytes**

| Field | Type | Units / meaning |
|---|---|---|
| `timestamp` | u64 | OBS timestamp in **nanoseconds** (video and audio share one clock) |
| `kind` | u32 | `0` = video, `1` = audio |
| `payload_bytes` | u32 | bytes of payload following this header |
| `units` | u32 | video: plane count; audio: **sample frames** per channel |
| `reserved` | u32 | 0 |

### Payload

**Video (kind 0)** — plane 0, then plane 1, … each **tightly packed** (no stride padding). Total bytes =
sum of `plane_bytes`, which equals `payload_bytes`. For 1920×1080 NV12 that is
`2073600 + 1036800 = 3110400` bytes (planes split `w*h` and `w*ceil(h/2)`).

**Audio (kind 1)** — channel 0, then channel 1, … **each `units × 4` bytes** (planar float32, OBS's native
layout). Total bytes = `channels × units × 4` = `payload_bytes`. Observed chunks are `units = 1024` sample
frames → `1024 × 2 × 4 = 8192` bytes, arriving every 21.33 ms (~46.88 chunks/s at 48 kHz).

**Synchronisation is carried entirely by the timestamps.** OBS stamps video and audio from the same clock and
the plugin forwards them untouched; re-stamping on either side is what would actually break A/V sync. Nothing
else (no sequence numbers, no re-timing) exists on the wire.

### Consuming the audio layout

Audio is **planar** float32, one plane per channel. A consumer must **not** hand that layout straight to a
GStreamer `appsrc`: a non-interleaved appsrc buffer carries no `GstAudioMeta`, so `audioconvert` fails with
"the stream is in the wrong format". Interleave the channels on the way in (a few kB per chunk). Use
`tools/raw_capture.py` as the reference consumer; `tools/appsrc_audio_probe.c` isolates this question.

## Verified (this host, OBS 32.2.2)

Stream header observed on the wire: `magic=0x4f424352 version=2 1920x1080 fps=60/1 obs_format=2 (NV12)
planes=2 plane_bytes=[2073600,1036800,0] linesize=[0,0,0] audio_rate=48000 audio_channels=2
audio_format=1`.

**Audio ON** (`settings.json` `{"target":"127.0.0.1:9377","audio":true}`, isolated OBS with the scene's
media source playing):

```
received 1380 audio chunks = 1,413,120 sample frames  (payload 8192 B each = 1024 frames x 2 ch x 4 B)
received 1769 video frames   (payload 3,110,400 B each, 60.00 fps, 0 malformed)
audio span 29.44s of 48 kHz stereo against 29.47s of video
inter-chunk gap: constant 21.33 ms, 0 gaps >200 ms
sync: median audio-video offset ~0 ms for the whole run (least-squares drift slope -0.0007 ms/s,
i.e. -0.02 ms over 28.6 s) — no measurable drift
```

A second, **independent** run (a listener written straight from the wire format in `src/raw-output.c`, not
the reference consumer) reproduced the counts and payload exactly: `1716 video frames / 1340 audio chunks`
in **28.58 s**, the same 8192-byte chunks at a constant **21.33 ms** spacing, 0 malformed payloads, and a
median audio-video offset of ~**0.000 ms**. Endpoint-to-endpoint drift is dominated by ±half-a-frame
sampling jitter (audio and video chunks land between each other's timestamps), so a windowed median /
regression is the meaningful figure — the earlier single-endpoint `-0.666 ms` was within that noise.
Sub-millisecond alignment and no measurable drift: the two streams share OBS's clock.

The plugin's own counters agree exactly with the listener: `[obc-raw] stopped after 1769 frames (0 dropped)
and 1380 audio chunks (0 dropped)`.

**Audio OFF** (control run, `audio:false`): header reads `audio_rate=0 audio_channels=0 audio_format=0`,
`video_frames=454 audio_chunks=0`. Confirms the no-audio encoding.

**Live profile untouched:** `/home/pat/.config/obs-studio/basic/profiles/Untitled/basic.ini` was byte-for-byte
unchanged (same sha256, same mtime) across every run; `ColorFormat=NV12` before and after.

Video decode sanity (from the raw file):

```sh
ffmpeg -f rawvideo -pix_fmt nv12 -s 1920x1080 -i out/frames.raw -frames:v 1 frame0.png
```

## Tools

- `tools/raw_capture.py` — the reference consumer: demuxes both kinds from the single socket, writes raw
  video and (optionally interleaved) raw float32 audio, and reports the A/V offset and drift.
- `tools/appsrc_audio_probe.c` — isolates the ingest-layout question: pushes planar (one memory per channel,
  and a single concatenated buffer) versus interleaved float32 into a real
  `appsrc ! queue ! audioresample ! audioconvert ! avenc_aac ! aacparse ! mpegtsmux` chain, with pad probes
  counting what leaves each stage:

  ```sh
  cc -o probe tools/appsrc_audio_probe.c $(pkg-config --cflags --libs gstreamer-app-1.0)
  ./probe 0 pre /tmp/probe_planar2.ts    # planar, one memory per channel
  ./probe 1 pre /tmp/probe_planar1.ts    # planar, one memory
  ./probe 2 pre /tmp/probe_inter.ts      # interleaved
  ```
- `tools/cpu_sampler.py` — per-process CPU over a window, as cpu-seconds and share of one core:
  `python3 tools/cpu_sampler.py obs=<pid> enc=<pid> 20`.

To run an automated capture, start the receiver first (it **listens**; the plugin is the client):

```sh
python3 tools/raw_capture.py --port 9300 --out out/frames.raw --seconds 10
OBC_RAW_AUTOSTART=1 OBC_RAW_TARGET=127.0.0.1:9300 obs --disable-shutdown-check
```

## Known limitations

- **Uncompressed = huge.** 1080p60 NV12 is ~1.49 Gbit/s; 4K is ~4× that. LAN/loopback only.
- **The plugin is the TCP client; the receiver must be listening first.** If `connect()` fails, the output
  does not start (it logs the reason and refuses).
- **One connection, one consumer.** The output writes to a single socket; there is no fan-out.
- **Reconnection is the plugin's own.** When sends fail for `OBC_MAX_CONSECUTIVE_FAILURES` (90) consecutive
  frames the socket is closed and marked disconnected, and the next video callback re-dials at most once a
  second — the output itself stays running, so an encoder restart costs a second of frames rather than the
  operator's stream. A re-dial replays the stream header, because the encoder reads exactly one per
  connection.
- **`linesize` in the header is always 0** by design; receivers must use `plane_bytes`.
- **Audio is planar float32**, not interleaved — GStreamer `appsrc` consumers must interleave.
- **v2 only.** v1 (video-only, 16-byte frame header) is not accepted.
- **Sync is timestamps only.** The plugin never re-times; a consumer that re-stamps breaks A/V sync.
- **Channel count** comes from OBS's speaker layout; `SPEAKERS_UNKNOWN` falls back to stereo.
- **Video formats:** I420, NV12, P010, YUY2, UYVY, BGRA/BGRX/RGBA/BGR3 have geometry; an unsupported OBS
  output format makes `start` fail loudly rather than guess. NVIDIA/P010 has no H.264 path on the encoder
  side, but that is the encoder's concern.
- The output is registered with **no service**, so it never appears in OBS Settings → Output: the encoder
  starts and stops it over the control channel, and the Tools menu is the manual override.

## Gotchas found by running it for real

- **Remove the frontend event callback on `OBS_FRONTEND_EVENT_EXIT`, not in `obs_module_unload`.** libobs
  unloads modules *after* it has torn the frontend down, so an
  `obs_frontend_remove_event_callback()` there finds no callbacks and OBS logs
  `Tried to call obs_frontend_remove_event_callback with no callbacks!` once per exit. The plugin records
  the EXIT event (delivered while the frontend is still alive) and skips the remove on that path; the remove
  still runs if the module is ever unloaded while the frontend is alive (the plugin manager).
- **Never hold the output's lock across `obs_output_end_data_capture()`.** It waits for the streaming thread,
  which takes the same lock in `raw_video` — so OBS deadlocks on shutdown and only SIGKILL clears it.
  Teardown runs unlocked.
- **On a shared socket, let only one stream own the failure reset.** A dead peer fails video and audio
  together, so if audio also reset the run a healthy audio stream could mask a dead video one. Audio
  increments the run; video owns the reset; either may trip the stop.
- The video and audio callbacks arrive on **different threads** and share one socket, so the send path is
  serialised by one lock — the interleaved writes stay frame-aligned.
- The libobs `.pc` files carry `-Werror` and `-std=gnu17`; inheriting them applies a dependency's warning
  policy to our translation units (and fires on OBS's own headers in the C++ TU). `CMakeLists.txt` strips
  both and marks the dep includes `SYSTEM`.
- The libobs `.pc` files carry **no `-I`** for their own headers (which live under `obs/`); CMake adds
  `/usr/include/obs` explicitly.
- A receiver should set a **socket timeout** — a stalled-but-open peer otherwise blocks the reader forever.
- OBS on the reference host is a **native Wayland client**: `xdotool` cannot see its window, so no external
  tool can open its menus — script it with `OBC_RAW_AUTOSTART=1` instead.

## Status

**v1.0.0.** Proven on the reference host against **OBS 33.0.1** (and 32.2.2 before that), cmake ≥ 3.28:

- Uncompressed video **and synchronised audio** leave OBS and arrive intact (A/V offset under 1 ms, drift
  under 1 ms over ~20 s, zero dropped frames/chunks in the reference runs).
- The encoder's `input_mode::raw_local` listens on `9300` and feeds the frames into its GStreamer pipeline,
  so the encode stage runs exactly once, in the encoder.
- The encoder **starts and stops this output** over the control channel; verified in both directions, with
  the stop judged by the frame socket going silent (byte count unchanged afterwards), not by a log line.

**Versioning.** This is a binary artefact **per OBS major**: it links `libobs` and `obs-frontend-api`
directly, so a plugin built for one major does not load into the next. Release notes name the OBS version
each artefact was built against, and each new OBS major means a rebuild.

## Licence

**GPL-2.0-or-later** — see `LICENSE`.

The plugin links libobs, which is GPL-2.0-or-later, so this component is GPL even though the rest of the
open-broadcast stack (encoder, receiver, backplane) is AGPL-3.0-or-later. They are separate programs: the
plugin and the encoder are joined by a socket and a port, never linked into one binary.

The frames leave OBS uncompressed precisely so nothing here practises a codec — OBS hands over pixels, and
the encoder performs the single encode.
