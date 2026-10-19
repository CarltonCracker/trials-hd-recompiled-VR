#!/usr/bin/env python3
"""Check 1440p pc_settings_menu captures/log (requires Pillow).

Usage: python3 tests/check_pc_settings_menu.py out/pc-settings
Captions are inspected visually; this check verifies both entry paths dispatch
through the host hook and that opening/closing restores the correct background.
"""
import sys
from pathlib import Path

from PIL import Image, ImageStat


def main():
    folder = Path(sys.argv[1])
    # Debug runs can rotate between the two activations (SDK default: 5 MB).
    logs = sorted(folder.glob("trials_hd*.log"))
    if not logs:
        raise SystemExit("Missing trials_hd log files")
    log = "\n".join(path.read_text(errors="replace") for path in logs)
    if log.count("PC SETTINGS button pressed; opening host settings") < 2:
        raise SystemExit("Controller and keyboard must both reach the PC SETTINGS hook")
    if "Could not set PC SETTINGS caption" in log:
        raise SystemExit("The guest UI text setter failed")

    def brightness(name):
        image = Image.open(folder / f"pc_{name}.png").convert("RGB")
        if image.size != (2560, 1440):
            raise SystemExit(f"{name}: expected 2560x1440, got {image.size}")
        return sum(ImageStat.Stat(image.crop((280, 650, 425, 1000))).mean)

    reference = brightness("main_menu")
    for name in ("selected", "settings_controller", "settings_graphics", "settings_closed",
                 "settings_keyboard", "settings_closed_again"):
        ratio = brightness(name) / reference
        dimmed = name in ("settings_controller", "settings_graphics", "settings_keyboard")
        lower, upper = (0.36, 0.44) if dimmed else (0.97, 1.03)
        if not lower <= ratio <= upper:
            raise SystemExit(f"{name}: unexpected background brightness {ratio:.3f}")
        print(f"{name}: {ratio:.3f} PASS")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    main()
