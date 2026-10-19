#!/usr/bin/env python3
"""Check 1440p achievements_close headless captures (requires Pillow).

Usage: python3 tests/check_achievements_close.py out/achievements-close/after
The static menu-label region outside the window must return to full brightness
on each close, and dim again after reopening. Frozen/stale frames fail this check.
"""
import sys
from pathlib import Path

from PIL import Image, ImageStat


def brightness(folder, name):
    image = Image.open(folder / f"achievements_{name}.png").convert("RGB")
    if image.size != (2560, 1440):
        raise SystemExit(f"{name}: expected 2560x1440, got {image.size}")
    return sum(ImageStat.Stat(image.crop((280, 650, 425, 1000))).mean)


def main():
    folder = Path(sys.argv[1])
    dimmed = brightness(folder, "before_close")
    for name in ("closed_button", "reopened", "closed_x", "reopened_again", "closed_escape"):
        ratio = brightness(folder, name) / dimmed
        lower, upper = (2.4, 2.6) if name.startswith("closed") else (0.95, 1.05)
        if not lower <= ratio <= upper:
            raise SystemExit(f"{name}: brightness ratio {ratio:.3f}; menu failed to close/reopen")
        print(f"{name}: {ratio:.3f} PASS")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    main()
