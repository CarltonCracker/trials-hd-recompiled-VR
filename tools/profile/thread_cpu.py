#!/usr/bin/env python3
"""Per-thread CPU usage of a process over a time window (from /proc).

Usage: thread_cpu.py <pid> [seconds=10]
Prints each thread's name and CPU use in % of one core, busiest first.
"""
import os
import sys
import time

TICKS = os.sysconf("SC_CLK_TCK")


def snapshot(pid):
    result = {}
    for tid in os.listdir(f"/proc/{pid}/task"):
        try:
            with open(f"/proc/{pid}/task/{tid}/stat") as f:
                stat = f.read()
            with open(f"/proc/{pid}/task/{tid}/comm") as f:
                comm = f.read().strip()
        except OSError:
            continue
        fields = stat[stat.rindex(")") + 2:].split()
        result[tid] = (comm, int(fields[11]) + int(fields[12]))  # utime + stime
    return result


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    pid = int(sys.argv[1])
    seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 10.0
    before = snapshot(pid)
    t0 = time.monotonic()
    time.sleep(seconds)
    after = snapshot(pid)
    elapsed = time.monotonic() - t0
    rows = []
    for tid, (comm, ticks) in after.items():
        used = ticks - before.get(tid, (comm, ticks))[1]
        rows.append((used / TICKS / elapsed * 100, tid, comm))
    rows.sort(reverse=True)
    total = sum(r[0] for r in rows)
    print(f"total {total:.0f}% of one core over {elapsed:.1f} s")
    for pct, tid, comm in rows:
        if pct >= 0.5:
            print(f"  {pct:6.1f}%  {tid:>8}  {comm}")


if __name__ == "__main__":
    main()
