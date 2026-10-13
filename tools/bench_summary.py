#!/usr/bin/env python3
"""Summarize a benchmark run (tests/headless/bench_gpu.txt).

Prints the per-second guest FPS samples (--log_guest_fps), the median of the
samples above --threshold FPS (default 0: all but the first and last), and the
median GPU time per frame by kind of work (--gpu_profile) over the last
--gpu-window seconds (default 15, the steady part of the ride).

Usage: bench_summary.py <trials_hd.log> [--threshold FPS] [--gpu-window SECONDS]
"""
import re
import statistics
import sys


def main():
    args = sys.argv[1:]
    if not args:
        raise SystemExit(__doc__)
    threshold = 0.0
    gpu_window = 15
    if "--gpu-window" in args:
        i = args.index("--gpu-window")
        gpu_window = int(args[i + 1])
        del args[i:i + 2]
    if "--threshold" in args:
        i = args.index("--threshold")
        threshold = float(args[i + 1])
        del args[i:i + 2]
    pattern = re.compile(r"guest fps: ([\d.]+) draws/frame: ([\d.]+) resolves/frame: ([\d.]+)")
    gpu_pattern = re.compile(
        r"gpu time/frame: ([\d.]+) ms \(draws/other ([\d.]+), resolves ([\d.]+), "
        r"rt transfers ([\d.]+), texture loads ([\d.]+), swap ([\d.]+)\)")
    samples = []
    gpu_samples = []
    with open(args[0], errors="replace") as f:
        for line in f:
            m = pattern.search(line)
            if m:
                samples.append(tuple(float(v) for v in m.groups()))
            m = gpu_pattern.search(line)
            if m:
                gpu_samples.append(tuple(float(v) for v in m.groups()))
    if gpu_samples:
        window = gpu_samples[-gpu_window:]
        names = ("total", "draws/other", "resolves", "rt transfers", "texture loads", "swap")
        print("gpu ms/frame (median of last {}s): ".format(len(window)) + ", ".join(
            f"{name} {statistics.median(s[i] for s in window):.2f}"
            for i, name in enumerate(names)))
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
