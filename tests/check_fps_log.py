#!/usr/bin/env python3
"""Check runtime rates (not gameplay): check_fps_log.py LOG [render_limit]."""
import re
import statistics
import sys

with open(sys.argv[1], errors="replace") as log:
    text = log.read()
assert "Unhandled guest access violation" not in text, "guest memory fault during run"
updates = [(float(a), float(b)) for a, b in re.findall(
    r"fixed updates/sec: ([\d.]+) real renders/sec: ([\d.]+)", text)]
frames = [(float(a), float(b), float(c)) for a, b, c in re.findall(
    r"guest fps: ([\d.]+) draws/frame: ([\d.]+) resolves/frame: ([\d.]+)", text)]
# Ignore startup/loading; screenshots must separately confirm the run reached gameplay.
assert len(updates) >= 20 and len(frames) >= 20, "insufficient runtime samples"
updates, frames = updates[-15:], frames[-15:]
fixed = statistics.mean(a for a, _ in updates)
render = statistics.median(b for _, b in updates)
guest = statistics.median(a for a, _, _ in frames)
assert 59 <= fixed <= 61, f"fixed updates ran at {fixed:.2f} Hz"
assert abs(render - guest) <= max(3, guest * .05), "render dispatch did not produce real swaps"
assert min(b for _, b, _ in frames) > 0, "frames have no game draws"
assert min(c for _, _, c in frames) > 0, "frames have no game resolves"
if len(sys.argv) > 2:
    limit = float(sys.argv[2])
    assert abs(render - limit) <= max(3, limit * .05), f"render cap: {render} != {limit}"
else:
    assert render > 65, f"rendering still capped at {render} FPS"
print(f"fixed {fixed:.2f} Hz; real rendering {render:.1f} FPS; guest swaps {guest:.1f} FPS")
