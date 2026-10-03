#!/usr/bin/env python3
"""Sample per-process CPU over a window, to price a transport choice honestly.

Usage: cpu_sampler.py name=pid name=pid ... <seconds>
Reports cpu-seconds used, share of all logical CPUs, and share of one core.
"""

import os
import sys
import time


def cpu_ticks(pid):
    try:
        with open(f"/proc/{pid}/stat", encoding="ascii") as handle:
            parts = handle.read().split()
        return int(parts[13]) + int(parts[14])  # utime + stime
    except (OSError, IndexError, ValueError):
        return None


def host_ticks():
    with open("/proc/stat", encoding="ascii") as handle:
        fields = handle.readline().split()[1:]
    return sum(int(x) for x in fields)


def rss_mb(pid):
    try:
        with open(f"/proc/{pid}/status", encoding="ascii") as handle:
            for line in handle:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1]) / 1024.0
    except OSError:
        pass
    return None


def main():
    pids = {}
    for arg in sys.argv[1:-1]:
        name, pid = arg.split("=")
        pids[name] = int(pid)
    seconds = float(sys.argv[-1])
    hz = os.sysconf("SC_CLK_TCK")
    nproc = os.cpu_count() or 1

    before = {name: cpu_ticks(pid) for name, pid in pids.items()}
    host_before = host_ticks()
    time.sleep(seconds)
    host_after = host_ticks()

    # /proc/stat's first line is the sum across every CPU, so dividing by hz
    # gives aggregate cpu-seconds, not wall time. Wall time is that over nproc.
    wall = (host_after - host_before) / hz / nproc
    print(f"window {wall:.1f}s wall, {nproc} logical CPUs")
    for name, pid in pids.items():
        now = cpu_ticks(pid)
        if before[name] is None or now is None:
            print(f"  {name}: process gone")
            continue
        used = (now - before[name]) / hz
        of_total = 100.0 * used / (wall * nproc)
        of_core = 100.0 * used / wall
        mem = rss_mb(pid)
        mem_str = f"{mem:7.1f} MB RSS" if mem is not None else "     ? MB RSS"
        print(f"  {name:9s} pid {pid:7d}: {used:6.2f} cpu-s -> "
              f"{of_total:5.2f}% of the machine, {of_core:6.2f}% of one core, "
              f"{mem_str}")


if __name__ == "__main__":
    main()
