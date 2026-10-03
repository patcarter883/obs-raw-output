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

No encoding, no muxing, no container: `1280x720` NV12/I420 ≈ **1.3 MB per frame**, ~**332 Mbit/s** at 30 fps.

## Wire format

All integers little-endian. Structures are plain `uint32`/`uint64`, so there is no padding to disagree about.

```
stream header (once, on connect)   15 × u32 = 60 bytes
  magic 'OBCR' | version | width | height | fps_num | fps_den
  obs_format (enum video_format) | planes
  linesize[3] | plane_bytes[3] (tight) | reserved

frame header (per frame)           16 bytes
  timestamp (u64, ns) | payload_bytes (u32) | planes (u32)

frame payload
  plane 0, plane 1, plane 2 — each tightly packed, no stride padding
```

Planes go out tight so the receiver's layout depends only on `width`/`height`/`format`, never on OBS's
internal line stride — the plugin repacks row by row whenever OBS's stride is wider than the row, and logs
the first frame's stride so that copy is visible.

`linesize` is sent as **zero**: it is not known until the first frame arrives, while this header goes out at
connect time. It is kept only so the layout can grow into it. **Receivers must ignore it and use
`plane_bytes` alone.**

OBS `video_format` values are the enum's declaration order: `I420=1`, `NV12=2`, `YUY2=4`, `UYVY=5`, `BGRA=7`.

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

## Status

- **Phase 0: done.** Uncompressed frames leave OBS and arrive intact, proven on this host.
- **Phase 1:** the encoder grows `input_mode::raw_local`, which listens on the same port and pushes the
  frames into its GStreamer pipeline via `appsrc` — so the encode stage runs exactly once, in the encoder.

The output is started from the Tools menu (the NDI-output pattern) because an `obs_output` registered
without a service has nothing in Settings to start it. A proper properties dialog lands with the product
integration; `OBC_RAW_TARGET` keeps it scriptable until then.
