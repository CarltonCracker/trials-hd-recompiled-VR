#!/usr/bin/env python3
"""Stable riding-window FPS of tools/bench_vulkan.sh runs.

Takes the last 15 per-second samples with > 1000 draws/frame and >= 18
resolves/frame (the ride, not menus) from each log and prints their median.
With several logs, also prints the median of the per-run medians.

Usage: bench_window.py <trials_hd.log>...
"""
import re
import statistics
import sys

PATTERN = re.compile(r"guest fps: ([\d.]+) draws/frame: ([\d.]+) resolves/frame: ([\d.]+)")


def run_median(path):
    samples = []
    with open(path, errors="replace") as f:
        for line in f:
            m = PATTERN.search(line)
            if m:
                fps, draws, resolves = (float(v) for v in m.groups())
                if draws > 1000 and resolves >= 18:
                    samples.append(fps)
    if len(samples) < 15:
        raise SystemExit(f"{path}: only {len(samples)} riding samples (ride didn't run?)")
    return statistics.median(samples[-15:])


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    medians = [run_median(p) for p in sys.argv[1:]]
    for p, m in zip(sys.argv[1:], medians):
        print(f"{m:7.1f}  {p}")
    if len(medians) > 1:
        print(f"{statistics.median(medians):7.1f}  median of {len(medians)} runs")


if __name__ == "__main__":
    main()
