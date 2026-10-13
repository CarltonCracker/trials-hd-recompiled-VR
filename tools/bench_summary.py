#!/usr/bin/env python3
"""Summarize a benchmark run (tests/headless/bench_fps.txt).

Prints the per-second guest FPS samples (--log_guest_fps) and the median of the
samples above --threshold FPS (default 0: all but the first and last).

Usage: bench_summary.py <trials_hd.log> [--threshold FPS]
"""
import re
import statistics
import sys


def main():
    args = sys.argv[1:]
    if not args:
        raise SystemExit(__doc__)
    threshold = 0.0
    if "--threshold" in args:
        i = args.index("--threshold")
        threshold = float(args[i + 1])
        del args[i:i + 2]
    pattern = re.compile(r"guest fps: ([\d.]+) draws/frame: ([\d.]+) resolves/frame: ([\d.]+)")
    samples = []
    with open(args[0], errors="replace") as f:
        for line in f:
            m = pattern.search(line)
            if m:
                samples.append(tuple(float(v) for v in m.groups()))
    if not samples:
        raise SystemExit("no 'guest fps' lines (run with --log_guest_fps=true)")
    print("fps:", " ".join(f"{s[0]:.0f}" for s in samples))
    uncapped = [s for s in samples if s[0] > threshold]
    if not uncapped:
        print(f"no samples above {threshold} FPS")
        return
    # The first and last uncapped seconds are partial; drop them when possible.
    if len(uncapped) > 4:
        uncapped = uncapped[1:-1]
    fps = [s[0] for s in uncapped]
    print(f"guest fps: median {statistics.median(fps):.1f} FPS "
          f"({1000 / statistics.median(fps):.2f} ms/frame), min {min(fps):.1f}, "
          f"max {max(fps):.1f}, n={len(fps)}, "
          f"draws/frame {statistics.median(s[1] for s in uncapped):.0f}, "
          f"resolves/frame {statistics.median(s[2] for s in uncapped):.1f}")


if __name__ == "__main__":
    main()
