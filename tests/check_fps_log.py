#!/usr/bin/env python3
"""Check runtime rates (not gameplay): check_fps_log.py LOG [render_limit].

With --log_frame_timing=true in the run, also summarizes the measured frame rate
over the whole log (the performance overlay's numbers)."""
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
# Logs from builds that report presents: every rendered frame must reach the
# surface (the Vulkan presenter once showed only every other one when GPU-bound).
presented = [float(p) for p in re.findall(r"guest fps: [\d.]+ .*presented fps: ([\d.]+)", text)]
if presented:
    shown = statistics.median(presented[-15:])
    assert shown >= guest * .95, f"presented {shown:.1f} of {guest:.1f} guest FPS"
if len(sys.argv) > 2:
    limit = float(sys.argv[2])
    assert abs(render - limit) <= max(3, limit * .05), f"render cap: {render} != {limit}"
else:
    assert render > 65, f"rendering still capped at {render} FPS"
print(f"fixed {fixed:.2f} Hz; real rendering {render:.1f} FPS; guest swaps {guest:.1f} FPS")
# Measured frame timing over the whole run (--log_frame_timing=true), not only its
# end: what the performance overlay shows, once per second.
timing = [(float(f), float(w)) for f, w in re.findall(
    r"frame timing: ([\d.]+) FPS, avg [\d.]+ ms, worst ([\d.]+) ms", text)]
if timing:
    fps = sorted(f for f, _ in timing)
    p5 = fps[int(len(fps) * 0.05)]
    slow = sum(f < 55 for f in fps)
    print(f"measured over {len(fps)} s: min {fps[0]:.1f} FPS, 5th percentile {p5:.1f}, "
          f"median {statistics.median(fps):.1f}; {slow} s below 55 FPS; "
          f"worst frame {max(w for _, w in timing):.1f} ms")
