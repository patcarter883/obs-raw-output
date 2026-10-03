#!/usr/bin/env python3
"""Receive uncompressed frames from OBS via the open-broadcast raw output.

The OBS plugin is the TCP *client*, so this listens, accepts one connection,
reads the stream header, then writes every frame's tightly packed planes to a
flat .raw file and reports what actually arrived.

Numeric `format` values are OBS's `enum video_format` in declaration order:
I420=1, NV12=2, YVYU=3, YUY2=4, UYVY=5, RGBA=6, BGRA=7, BGRX=8.

Turn a captured frame into something viewable (frame 0):

  ffmpeg -f rawvideo -pix_fmt nv12 -s 1280x720 -i frames.raw -frames:v 1 frame0.png

or a later frame N (0-based):

  ffmpeg -f rawvideo -pix_fmt nv12 -s 1280x720 -i frames.raw \
         -vf "select=eq(n\\,N)" -frames:v 1 frameN.png
"""

import argparse
import os
import socket
import struct
import sys
import time

STREAM_HDR = struct.Struct("<15I")  # magic,version,w,h,fps_n,fps_d,fmt,planes,
#                                     linesize[3], plane_bytes[3], reserved
FRAME_HDR = struct.Struct("<QII")   # timestamp, payload_bytes, planes

MAGIC = 0x4F424352  # 'OBCR'
FORMAT_NAMES = {
    1: "I420", 2: "NV12", 3: "YVYU", 4: "YUY2", 5: "UYVY",
    6: "RGBA", 7: "BGRA", 8: "BGRX", 9: "Y800",
}
PIX_FMT = {
    "I420": "yuv420p", "NV12": "nv12", "YUY2": "yuyv422", "UYVY": "uyvy422",
    "YUYV": "yuyv422", "BGRA": "bgra", "RGBA": "rgba", "BGRX": "bgr0",
    "BGR3": "bgr24", "Y800": "gray",
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


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=9300)
    ap.add_argument("--bind", default="127.0.0.1")
    ap.add_argument("--out", default="out/frames.raw")
    ap.add_argument("--frames", type=int, default=0, help="0 = until the peer stops")
    ap.add_argument("--seconds", type=float, default=0, help="0 = no limit")
    ap.add_argument("--accept-timeout", type=float, default=120.0)
    ap.add_argument("--recv-timeout", type=float, default=5.0,
                    help="seconds with no data before giving up on a stalled peer")
    args = ap.parse_args()

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
     ls0, ls1, ls2, pb0, pb1, pb2, reserved) = STREAM_HDR.unpack(raw)

    if magic != MAGIC:
        print(f"bad magic 0x{magic:08X} (expected 0x{MAGIC:08X})", file=sys.stderr)
        return 3

    fmt_name = FORMAT_NAMES.get(fmt, f"unknown({fmt})")
    plane_bytes = [pb0, pb1, pb2][:planes]
    linesize = [ls0, ls1, ls2][:planes]
    payload = sum(plane_bytes)
    fps = fps_num / fps_den if fps_den else 0.0

    print(f"stream: version={version} {width}x{height} @ {fps_num}/{fps_den} "
          f"({fps:.2f} fps) format={fmt_name} planes={planes}")
    print(f"        plane_bytes={plane_bytes} (tight) payload={payload} bytes/frame "
          f"~{payload * fps * 8 / 1e6:.1f} Mbit/s")
    print(f"        OBS delivered linesize={linesize}"
          + ("  (tight, no repack needed)" if linesize == plane_bytes
             and planes == 1 else ""))

    os.makedirs(os.path.dirname(args.out) or ".", exist_ok=True)
    n_frames = 0
    n_bytes = 0
    first_ts = None
    last_ts = None
    t_start = time.time()
    bad = 0

    with open(args.out, "wb") as fh:
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
            ts, nbytes, fplanes = FRAME_HDR.unpack(fh_raw)

            if nbytes != payload:
                print(f"frame {n_frames}: payload {nbytes} != header {payload}; "
                      f"aborting to avoid writing garbage", file=sys.stderr)
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

            fh.write(data)
            if first_ts is None:
                first_ts = ts
            last_ts = ts
            n_frames += 1
            n_bytes += nbytes

    elapsed = time.time() - t_start
    span = (last_ts - first_ts) / 1e9 if (first_ts is not None and last_ts is not None) else 0.0
    print()
    print(f"received {n_frames} uncompressed frames, {n_bytes} bytes "
          f"({n_bytes / 1e6:.1f} MB) in {elapsed:.1f}s wall")
    if n_frames:
        print(f"frame span {span:.2f}s => {n_frames / span:.2f} fps"
              if span > 0 else "")
        print(f"wrote {args.out}")
        print()
        print("view frame 0:")
        print(f"  ffmpeg -f rawvideo -pix_fmt {PIX_FMT.get(fmt_name, 'nv12')} "
              f"-s {width}x{height} -i {args.out} -frames:v 1 frame0.png")
    return 0 if (n_frames and not bad) else 1


if __name__ == "__main__":
    sys.exit(main())
