#!/usr/bin/env python3
"""Receive uncompressed video and synchronised audio from OBS via the raw output.

The OBS plugin is the TCP *client*, so this listens, accepts one connection, then
demuxes everything that arrives. Video and audio share that one connection: every
frame header carries a kind, so the two streams stay ordered and paced against
each other.

Wire format v2 (authoritative definition: src/raw-output.c)

  stream header   23 x u32 = 92 bytes
      magic 'OBCR' | version | width | height | fps_num | fps_den
      obs_format | planes | linesize[3] | plane_bytes[3](tight) | reserved
      audio_rate | audio_channels | audio_format | audio_reserved[5]
  frame header    24 bytes: <QIIII = timestamp, kind, payload_bytes, units, reserved
  payload         kind 0 video: plane 0..n, tightly packed
                  kind 1 audio: channel 0..n, each units*4 bytes

`audio_rate == 0` means the source has no audio. `linesize` is always 0 in the
header -- it is not known until the first frame arrives -- so keep using
`plane_bytes`; the tool logs the first frame's real stride instead.

Audio arrives PLANAR float32, OBS's native layout. This tool interleaves it on
the way out (--planar keeps it as received), so the .f32 file plays directly:

  ffmpeg -f f32le -ar 48000 -ac 2 -i frames_audio.f32 -c:a pcm_s16le out.wav

Numeric `format` values are OBS's `enum video_format` in declaration order:
I420=1, NV12=2, YVYU=3, YUY2=4, UYVY=5, RGBA=6, BGRA=7, BGRX=8.

Turn a captured video frame into something viewable:

  ffmpeg -f rawvideo -pix_fmt nv12 -s 1920x1080 -i frames.raw -frames:v 1 frame0.png

or a later frame N (0-based):

  ffmpeg -f rawvideo -pix_fmt nv12 -s 1920x1080 -i frames.raw \\
         -vf "select=eq(n\\,N)" -frames:v 1 frameN.png
"""

import argparse
import array
import bisect
import os
import socket
import struct
import sys
import time

STREAM_HDR = struct.Struct("<23I")
FRAME_HDR = struct.Struct("<QIIII")  # timestamp, kind, payload_bytes, units, reserved

MAGIC = 0x4F424352  # 'OBCR'
VERSION = 2
KIND_VIDEO = 0
KIND_AUDIO = 1
AUDIO_F32_PLANAR = 1
AUDIO_FMT_NAMES = {0: "none", 1: "float32-planar"}

FORMAT_NAMES = {
    1: "I420", 2: "NV12", 3: "YVYU", 4: "YUY2", 5: "UYVY",
    6: "RGBA", 7: "BGRA", 8: "BGRX", 9: "Y800",
}
PIX_FMT = {
    "I420": "yuv420p", "NV12": "nv12", "YUY2": "yuyv422", "UYVY": "uyvy422",
    "BGRA": "bgra", "RGBA": "rgba", "BGRX": "bgr0", "BGR3": "bgr24",
    "Y800": "gray",
}


def recv_exact(sock, n):
    chunks = []
    got = 0
    while got < n:
        b = sock.recv(min(1 << 20, n - got))
        if not b:
            return None
        chunks.append(b)
        got += len(b)
    return b"".join(chunks)


def interleave(chunk, channels, frames):
    """Channel-major float32 planes -> interleaved, for playable output."""
    if channels <= 1:
        return chunk
    src = array.array("f")
    src.frombytes(chunk)
    dst = array.array("f", b"\x00" * (channels * frames * 4))
    # src[c * frames + f] -> dst[f * channels + c]
    for c in range(channels):
        base = c * frames
        dst[c::channels] = src[base:base + frames]
    return dst.tobytes()


def median(xs):
    xs = sorted(xs)
    n = len(xs)
    if n == 0:
        return 0.0
    mid = n // 2
    return xs[mid] if n % 2 else (xs[mid - 1] + xs[mid]) / 2.0


def nearest_audio_offset(audio_ts, t):
    """Audio timestamp nearest to video timestamp `t`, signed (audio - video)."""
    i = bisect.bisect_left(audio_ts, t)
    best = None
    for j in (i - 1, i, i + 1):
        if 0 <= j < len(audio_ts):
            d = audio_ts[j] - t
            if best is None or abs(d) < abs(best):
                best = d
    return best


def report_sync(video_ts, audio_ts):
    """Estimate the A/V offset and whether it drifts over the run.

    OBS stamps both streams from one clock. If they were independently clocked,
    the offset would walk steadily; a stable offset means the receiver can align
    them from the timestamps alone. Measured as a median over each end of the
    run, so the +/- half-frame interleave jitter averages out.
    """
    if len(video_ts) < 20 or len(audio_ts) < 20:
        print("sync: not enough samples from both streams to measure")
        return

    audio_ts_sorted = audio_ts  # already in arrival order == timestamp order
    span = video_ts[-1] - video_ts[0]
    if span <= 0:
        print("sync: video timestamps do not advance; cannot measure")
        return

    lead = video_ts[0] + span // 4
    trail = video_ts[0] + (span * 3) // 4

    early = [d for d in (nearest_audio_offset(audio_ts_sorted, t)
                         for t in video_ts if t <= lead) if d is not None]
    late = [d for d in (nearest_audio_offset(audio_ts_sorted, t)
                        for t in video_ts if t >= trail) if d is not None]
    if not early or not late:
        print("sync: could not sample both ends of the run")
        return

    off_early = median(early) / 1e6   # ms
    off_late = median(late) / 1e6
    drift = off_late - off_early
    frame_ms = 1000.0 / 60.0  # nominal; only used to give drift a scale

    print()
    print(f"sync: audio-video timestamp offset {off_early:+.2f} ms at the start, "
          f"{off_late:+.2f} ms at the end")
    verdict = ("no drift (streams share OBS's clock)"
               if abs(drift) < frame_ms
               else f"DRIFT of {drift:+.2f} ms across the run -- investigate")
    print(f"      drift {drift:+.2f} ms over {span / 1e9:.2f}s -> {verdict}")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=9300)
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--out", default="out/frames.raw")
    ap.add_argument("--audio-out", default=None,
                    help="default: <out> with _audio.f32 appended")
    ap.add_argument("--no-audio", action="store_true",
                    help="do not write the audio file (still reports it)")
    ap.add_argument("--planar", action="store_true",
                    help="write audio as received (planar) instead of interleaved")
    ap.add_argument("--frames", type=int, default=0, help="0 = until the peer stops")
    ap.add_argument("--seconds", type=float, default=0, help="0 = no limit")
    ap.add_argument("--accept-timeout", type=float, default=120.0)
    ap.add_argument("--recv-timeout", type=float, default=5.0,
                    help="seconds with no data before giving up on a stalled peer")
    args = ap.parse_args()

    audio_out = args.audio_out
    if audio_out is None:
        root, ext = os.path.splitext(args.out)
        audio_out = f"{root}_audio{ext or '.raw'}"

    srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    srv.bind((args.bind, args.port))
    srv.listen(1)
    srv.settimeout(args.accept_timeout)
    print(f"listening on {args.bind}:{args.port} for the OBS raw output "
          f"(start it from OBS Tools; OBC_RAW_TARGET={args.bind}:{args.port})",
          flush=True)

    conn, peer = srv.accept()
    # Without this the reader blocks forever if the peer stalls but never
    # closes -- which is exactly what happens when OBS's output is left running
    # against a receiver that has gone away.
    conn.settimeout(args.recv_timeout)
    print(f"connected from {peer[0]}:{peer[1]}", flush=True)

    raw = recv_exact(conn, STREAM_HDR.size)
    if raw is None:
        print("peer closed before the stream header", file=sys.stderr)
        return 2
    (magic, version, width, height, fps_num, fps_den, fmt, planes,
     ls0, ls1, ls2, pb0, pb1, pb2, reserved,
     audio_rate, audio_channels, audio_format, *_rest) = STREAM_HDR.unpack(raw)

    if magic != MAGIC:
        print(f"bad magic 0x{magic:08X} (expected 0x{MAGIC:08X})", file=sys.stderr)
        return 3
    if version != VERSION:
        print(f"stream version {version}, expected {VERSION} -- this tool speaks v2 "
              f"(v1 carried video only; re-read src/raw-output.c)", file=sys.stderr)
        return 4

    fmt_name = FORMAT_NAMES.get(fmt, f"unknown({fmt})")
    plane_bytes = [pb0, pb1, pb2][:planes]
    linesize = [ls0, ls1, ls2][:planes]
    payload = sum(plane_bytes)
    fps = fps_num / fps_den if fps_den else 0.0
    has_audio = audio_rate > 0 and audio_channels > 0

    print(f"stream: version={version} {width}x{height} @ {fps_num}/{fps_den} "
          f"({fps:.2f} fps) format={fmt_name} planes={planes}")
    print(f"        plane_bytes={plane_bytes} (tight) payload={payload} bytes/frame "
          f"~{payload * fps * 8 / 1e6:.1f} Mbit/s")
    print(f"        OBS delivered linesize={linesize}"
          + ("  (tight, no repack needed)" if linesize == plane_bytes
             and planes == 1 else ""))
    if has_audio:
        print(f"        audio: {audio_rate} Hz, {audio_channels} ch, "
              f"{AUDIO_FMT_NAMES.get(audio_format, audio_format)} "
              f"~{audio_rate * audio_channels * 32 / 1e6:.1f} Mbit/s")
    else:
        print("        audio: none on this stream")

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    if has_audio and not args.no_audio:
        os.makedirs(os.path.dirname(audio_out) or ".", exist_ok=True)

    n_frames = 0
    n_bytes = 0
    n_chunks = 0
    audio_bytes = 0
    audio_samples = 0
    video_ts = []
    audio_ts = []
    t_start = time.time()
    bad = 0

    vfh = open(args.out, "wb")
    afh = (open(audio_out, "wb") if (has_audio and not args.no_audio) else None)
    try:
        while True:
            if args.frames and n_frames >= args.frames:
                break
            if args.seconds and (time.time() - t_start) >= args.seconds:
                break

            try:
                fh_raw = recv_exact(conn, FRAME_HDR.size)
            except socket.timeout:
                print(f"stalled: no data for {args.recv_timeout:.1f}s; stopping",
                      file=sys.stderr)
                break
            if fh_raw is None:
                print("peer stopped (clean close)", flush=True)
                break
            ts, kind, nbytes, units, _rsv = FRAME_HDR.unpack(fh_raw)

            if kind == KIND_VIDEO:
                if nbytes != payload:
                    print(f"frame {n_frames}: payload {nbytes} != header {payload}; "
                          f"aborting to avoid writing garbage", file=sys.stderr)
                    bad += 1
                    break
            elif kind == KIND_AUDIO:
                expect = units * audio_channels * 4
                if nbytes != expect:
                    print(f"audio chunk {n_chunks}: payload {nbytes} != "
                          f"units*channels*4 {expect}; aborting", file=sys.stderr)
                    bad += 1
                    break
            else:
                print(f"unknown frame kind {kind}; aborting", file=sys.stderr)
                bad += 1
                break

            try:
                data = recv_exact(conn, nbytes)
            except socket.timeout:
                print("stalled mid-frame; stopping", file=sys.stderr)
                break
            if data is None:
                print("peer closed mid-frame", file=sys.stderr)
                break

            if kind == KIND_VIDEO:
                vfh.write(data)
                video_ts.append(ts)
                n_frames += 1
                n_bytes += nbytes
            else:
                if afh is not None:
                    afh.write(data if args.planar
                              else interleave(data, audio_channels, units))
                audio_ts.append(ts)
                n_chunks += 1
                audio_bytes += nbytes
                audio_samples += units
    finally:
        vfh.close()
        if afh is not None:
            afh.close()

    elapsed = time.time() - t_start
    print()
    print(f"received {n_frames} uncompressed video frames, {n_bytes} bytes "
          f"({n_bytes / 1e6:.1f} MB) in {elapsed:.1f}s wall")
    if n_frames > 1:
        span = (video_ts[-1] - video_ts[0]) / 1e9
        if span > 0:
            print(f"video span {span:.2f}s => {len(video_ts) / span:.2f} fps")
    if n_frames:
        print(f"wrote {args.out}")
        print()
        print("view frame 0:")
        print(f"  ffmpeg -f rawvideo -pix_fmt {PIX_FMT.get(fmt_name, 'nv12')} "
              f"-s {width}x{height} -i {args.out} -frames:v 1 frame0.png")

    if n_chunks:
        secs = audio_samples / audio_rate if audio_rate else 0.0
        print()
        print(f"received {n_chunks} audio chunks, {audio_bytes} bytes "
              f"({audio_bytes / 1e6:.1f} MB) = {audio_samples} frames "
              f"({secs:.2f}s of audio at {audio_rate} Hz)")
        if afh is not None:
            layout = "planar (as received)" if args.planar else "interleaved"
            print(f"wrote {audio_out} ({layout})")
            print()
            print("listen to it:")
            print(f"  ffmpeg -f f32le -ar {audio_rate} -ac {audio_channels} "
                  f"-i {audio_out} -c:a pcm_s16le audio.wav")
        report_sync(video_ts, audio_ts)
    elif has_audio:
        print()
        print("audio was announced in the header but no chunks arrived")

    return 0 if (n_frames and not bad) else 1


if __name__ == "__main__":
    sys.exit(main())
