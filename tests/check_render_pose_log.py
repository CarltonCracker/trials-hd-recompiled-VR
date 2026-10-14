#!/usr/bin/env python3
"""Check distinct render inputs within fixed ticks, not monitor presentation."""
import collections
import re
import sys

PATTERN = re.compile(
    r"render pose: frame=(\d+) tick=(\d+) alpha=([\d.]+) node=([0-9A-F]+) "
    r"matrices=(\d+) component=(\d+) raw=([-\d.]+) rendered=([-\d.]+) "
    r"camera=([0-9A-F]+)(?: camera_view=([0-9A-F]+))? "
    r"camera_raw_x=([-\d.]+) camera_rendered_x=([-\d.]+)")


def check(text):
    assert "Unhandled guest access violation" not in text, "guest memory fault"
    models, cameras = collections.defaultdict(list), collections.defaultdict(list)
    for frame, tick, alpha, node, count, component, raw, rendered, camera, view, cx, ix in PATTERN.findall(text):
        assert 0 <= float(alpha) < 1, "invalid interpolation fraction"
        assert 1 <= int(count) <= 30 and 0 <= int(component) < 16, "invalid palette"
        models[(tick, node, count, component)].append((frame, float(raw), float(rendered)))
        cameras[(tick, view or camera)].append((frame, float(cx), float(ix)))

    def distinct(samples):
        # Require different render frames with identical simulation inputs but different poses.
        return (len({f for f, _, _ in samples}) > 1 and
                len({r for _, r, _ in samples}) == 1 and
                len({p for _, _, p in samples}) > 1 and
                any(abs(r-p) > 1e-6 for _, r, p in samples))

    rigid = sum(distinct(v) for k, v in models.items() if int(k[2]) == 1)
    animated = sum(distinct(v) for k, v in models.items() if int(k[2]) > 1)
    camera = sum(distinct(v) for v in cameras.values())
    assert rigid, "no distinct rigid-model poses within a fixed tick"
    assert animated, "no distinct animated-palette poses within a fixed tick"
    assert camera, "no distinct camera poses within a fixed tick"
    return rigid, animated, camera


if __name__ == "__main__":
    with open(sys.argv[1], errors="replace") as log:
        counts = check(log.read())
    print("distinct sub-tick samples: rigid %d; animated %d; camera %d" % counts)
