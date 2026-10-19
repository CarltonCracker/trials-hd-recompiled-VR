#!/usr/bin/env python3
"""Check 1440p achievements_controller headless captures (requires Pillow).

Usage: python3 tests/check_achievements_controller.py out/run
"""
import sys
from pathlib import Path

from PIL import Image, ImageChops, ImageStat


def main():
    folder = Path(sys.argv[1])

    def load(name):
        image = Image.open(folder / f"controller_{name}.png").convert("RGB")
        if image.size != (2560, 1440):
            raise SystemExit(f"{name}: expected 2560x1440, got {image.size}")
        return image

    top = load("top")
    region = (650, 400, 1800, 1000)
    for name in ("page_down", "page_up", "dpad", "stick", "reset"):
        difference = ImageChops.difference(top.crop(region), load(name).crop(region))
        red, green, blue = difference.split()
        histogram = ImageChops.lighter(ImageChops.lighter(red, green), blue).histogram()
        changed = sum(histogram[17:]) / (difference.width * difference.height)
        expected_change = name not in ("page_up", "reset")
        if (expected_change and changed < 0.02) or (not expected_change and changed > 0.01):
            raise SystemExit(f"{name}: unexpected content change {changed:.3f}")
        print(f"{name}: changed fraction {changed:.3f} PASS")

    def background(image):
        return sum(ImageStat.Stat(image.crop((280, 650, 425, 1000))).mean)

    for name in ("closed_a", "closed_b"):
        ratio = background(load(name)) / background(top)
        if not 2.4 <= ratio <= 2.6:
            raise SystemExit(f"{name}: menu did not close (brightness ratio {ratio:.3f})")
        print(f"{name}: {ratio:.3f} PASS")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    main()
