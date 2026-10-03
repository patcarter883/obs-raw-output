# open-broadcast raw output (OBS plugin)

A thin OBS output that hands over **uncompressed frames**.

This is Option A of `open-broadcast-product/OBS_INGEST_OPTIONS.md` / `DECISIONS.md` DT-7 — the actual
target. The MPEG-TS leg in `../obs-phase0/` is only a transport stand-in: it lets OBS encode and then
decodes on the far side, which **double-encodes**. Here OBS supplies raw pixels and the encoder's own
pipeline is the only encoder in the chain.

## What it does

Registers an `obs_output` (`obc_raw_output`) with **no video encoder attached**. That is the trick: with
no encoder, libobs delivers frames to `obs_output_info.raw_video()` unencoded. The plugin copies the
planes, tightens any line-stride padding, and writes them to a TCP socket.

It does the same for audio: requesting `OBS_OUTPUT_AUDIO` makes libobs hand the mix to
`obs_output_info.raw_audio()`, so the **synchronised** audio goes out on the same connection, carrying OBS's
own timestamps.

No encoding, no muxing, no container: `1280x720` NV12/I420 ≈ **1.3 MB per frame**, ~**332 Mbit/s** at 30 fps.

## Wire format

All integers little-endian. Structures are plain `uint32`/`uint64`, so there is no padding to disagree about.

```
stream header (once, on connect)   23 × u32 = 92 bytes
  magic 'OBCR' | version | width | height | fps_num | fps_den
  obs_format (enum video_format) | planes
  linesize[3] | plane_bytes[3] (tight) | reserved
  audio_rate | audio_channels | audio_format | audio_reserved[5]

frame header (per frame / audio chunk)   24 bytes
  timestamp (u64, ns) | kind | payload_bytes | units | reserved

frame payload
  kind 0 video:  plane 0, plane 1, ... — each tightly packed, no stride padding
  kind 1 audio:  channel 0, channel 1, ... — each `units × 4` bytes (float32)
```

`version` is **2**. v1 was video-only with a 16-byte frame header, and nothing consumed it, so there is no
compatibility shim — **receivers must accept v2 only**.

Planes go out tight so the receiver's layout depends only on `width`/`height`/`format`, never on OBS's
internal line stride — the plugin repacks row by row whenever OBS's stride is wider than the row, and logs
the first frame's stride so that copy is visible.

`linesize` is sent as **zero**: it is not known until the first frame arrives, while this header goes out at
connect time. It is kept only so the layout can grow into it. **Receivers must ignore it and use
`plane_bytes` alone.**

OBS `video_format` values are the enum's declaration order: `I420=1`, `NV12=2`, `YUY2=4`, `UYVY=5`, `BGRA=7`.

## Audio and synchronisation

Audio travels on the **same connection** as the video, multiplexed by the frame header's `kind`. One socket
was chosen over a second port deliberately: the streams stay ordered and paced against each other, where two
independent sockets can diverge under load and leave the receiver reconciling buffers.

Audio is **planar float32** — OBS's native layout, one plane per channel — so neither side pays for an
interleaving copy. `audio_rate == 0` in the header means the source has no audio and no audio kinds appear;
otherwise `audio_channels` and `audio_rate` describe every chunk.

**Sync is carried entirely by the timestamps.** OBS stamps video and audio from the same clock and the plugin
forwards those timestamps untouched; re-stamping on either side is what would actually break A/V sync.
`tools/raw_capture.py` measures the result by taking the median audio-to-video offset over each end of the
run, so the ±half-frame interleave jitter averages out. A 12 s run on this host:

```
received 720 uncompressed video frames in 12.0s (video span 11.98s => 60.08 fps)
received 560 audio chunks = 573440 frames (11.95s of audio at 48000 Hz)
sync: audio-video timestamp offset -0.14 ms at the start, -0.48 ms at the end
      drift -0.33 ms over 11.98s -> no drift (streams share OBS's clock)
```

Sub-millisecond alignment, and a drift of 0.33 ms over 12 s — far inside one video frame interval.

Audio is **opt-out**: it is sent by default, and `OBC_RAW_AUDIO=0` (or the "Send audio" setting) disables it.

## Build and install

```sh
cmake -S . -B build -D CMAKE_BUILD_TYPE=Release
cmake --build build -j"$(nproc)"
cmake --install build          # -> ~/.config/obs-studio/plugins/open-broadcast-raw-output/bin/64bit/
```

OBS loads a plugin from `<plugins-dir>/<name>/bin/64bit/<name>.so`, so the shared object must be named
exactly after the directory — hence `PREFIX ""` in `CMakeLists.txt`.

Requires the libobs dev headers (`libobs` + `obs-frontend-api` pkg-config). Present on this host with
`obs-studio` 32.2.2.

## Use

1. Start the receiver side first (it **listens**; the plugin is the client):

   ```sh
   python3 tools/raw_capture.py --port 9300 --out out/frames.raw --seconds 10
   ```

2. Start it. Either:

   - **Interactively:** OBS → **Tools → "Open Broadcast raw output (start/stop)"** (same toggle stops it), or
   - **Automated:** launch OBS with **`OBC_RAW_AUTOSTART=1`** and the output starts itself at
     `OBS_FRONTEND_EVENT_FINISHED_LOADING`. This exists because OBS on this host is a **native Wayland
     client** — `xdotool` cannot even see its window, so no external tool can open its menus.

   The target defaults to `127.0.0.1:9300`; export **`OBC_RAW_TARGET=host:port`** to point it elsewhere.

3. Confirm what arrived:

   ```sh
   ffmpeg -f rawvideo -pix_fmt nv12 -s 1280x720 -i out/frames.raw -frames:v 1 frame0.png
   ```

## Verified (this host, OBS 32.2.2)

```
[obc-raw] streaming uncompressed 1920x1080 @ 60/1 format=2 planes=2
          payload=3110400 bytes/frame (~1493.0 Mbit/s) to 127.0.0.1:9300
```

`3110400` bytes is exactly 1920×1080×1.5 — raw NV12. A 30 s capture yielded **1800 frames / 5.6 GB at a
sustained 60.03 fps** with **zero frame loss** (1800 frames over a 29.98 s span), and decoding a frame
produced a correct image (right colours, geometry, no stride artefacts). Frame 0 was blank because capture
started before the scene rendered — expected, not a bug.

Audio came with it: `48000 Hz, 2 channel(s), float32 planar`, and over a 12 s run **560 chunks / 4.6 MB =
11.95 s of audio** against 11.98 s of video, with the sync measurement shown above.

## Gotchas found by running it for real

- **Do not hold the output's lock across `obs_output_end_data_capture()`.** It waits for the streaming thread
  to return, and that thread takes the same lock in `raw_video` — so OBS deadlocks on shutdown and only
  SIGKILL clears it. Teardown runs unlocked.
- **An output must stop itself when the peer goes away.** Otherwise it spins forever dropping every frame.
  We count consecutive failures and call `obs_output_signal_stop(output, OBS_OUTPUT_DISCONNECTED)` after
  ~1.5 s. libobs then retries every 2 s, so an encoder restart reconnects on its own.
- The libobs `.pc` files carry **no `-I`** for their own headers (which live under `obs/`); CMake has to add
  that path explicitly.
- The receiver must set a **socket timeout** — a stalled-but-open peer otherwise blocks the reader forever.
- **On a shared socket, let only one stream own the failure reset.** A dead peer fails video and audio
  together, so if audio also reset the counter a healthy audio stream could mask a dead video one. Audio
  increments the run, video owns the reset, and either may trip the stop.
- The two callbacks arrive on **different threads** and share one socket, so the send path is serialised by
  the same lock the video path already used — the interleaved writes stay frame-aligned.

## Status

- **Phase 0: done.** Uncompressed frames *and synchronised audio* leave OBS and arrive intact, proven on
  this host (A/V offset under 1 ms, no drift).
- **Phase 1:** the encoder grows `input_mode::raw_local`, which listens on the same port and pushes the
  frames into its GStreamer pipeline via `appsrc` — so the encode stage runs exactly once, in the encoder.

The output is started from the Tools menu (the NDI-output pattern) because an `obs_output` registered
without a service has nothing in Settings to start it. A proper properties dialog lands with the product
integration; `OBC_RAW_TARGET` keeps it scriptable until then.
