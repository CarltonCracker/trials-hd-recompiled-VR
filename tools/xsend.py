#!/usr/bin/env python3
"""Drive a headless run with synthetic key presses (X11 XTEST).

Usage: xsend.py <display> <script> [fb_file] [shot_dir] [pad_fifo]

Script lines (blank lines and # comments ignored):
  wait <seconds>
  tap <keysym> [ms]      press and release (default 80 ms)
  hold <keysym> <ms>     hold a key down
  down <keysym>          press a key (stays down until `up`)
  up <keysym>            release a key
  shot <name>            save <shot_dir>/<name>.png from the Xvfb framebuffer
  pad <button> <0|1>     virtual gamepad button (needs pad_fifo; see src/virtual_pad.h)
  axis <axis> <value>    virtual gamepad axis, e.g. `axis rt 32767`
  capture <name>         save the game's own frame to <shot_dir>/<name>.png (works
                         with --vulkan_headless_surface; needs pad_fifo)
  set <cvar> <value>     change a cvar at runtime (virtual pad), e.g. set vsync false
  click <x> <y>         left-click at virtual-display coordinates
  move <title> <x> <y>  position a window (no window manager under Xvfb)
  focus <title>          give input focus to the first window whose title contains
                         <title> (needed under Xvfb: there is no window manager)

Keysyms are X names: space, Return, Up, Down, e, q, Shift_L, ...
Keys act as controller input through the keyboard driver (on by default).
Needs python-xlib (and Pillow for `shot`).
"""
import os
import subprocess
import sys
import time

from Xlib import X, XK, display
from Xlib.ext import xtest


def keycode(d, name):
    sym = XK.string_to_keysym(name)
    if not sym:
        raise SystemExit(f"unknown keysym {name!r}")
    return d.keysym_to_keycode(sym)


def find_window(win, title):
    name = win.get_wm_name()
    if isinstance(name, bytes):
        name = name.decode("utf-8", "replace")
    if name and title in name:
        return win
    for child in win.query_tree().children:
        found = find_window(child, title)
        if found:
            return found
    return None


def press(d, code, down):
    xtest.fake_input(d, 2 if down else 3, code)  # KeyPress / KeyRelease
    d.sync()


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    d = display.Display(sys.argv[1])
    fb = sys.argv[3] if len(sys.argv) > 3 else None
    shot_dir = sys.argv[4] if len(sys.argv) > 4 else "."
    here = os.path.dirname(os.path.abspath(__file__))
    pad = open(sys.argv[5], "w") if len(sys.argv) > 5 else None
    with open(sys.argv[2]) as f:
        lines = [l.split("#")[0].split() for l in f]
    for parts in filter(None, lines):
        cmd, args = parts[0], parts[1:]
        if cmd == "wait":
            time.sleep(float(args[0]))
        elif cmd in ("tap", "hold"):
            ms = int(args[1]) if len(args) > 1 else 80
            code = keycode(d, args[0])
            press(d, code, True)
            time.sleep(ms / 1000)
            press(d, code, False)
        elif cmd in ("pad", "axis"):
            if not pad:
                raise SystemExit("pad/axis commands need a pad FIFO")
            pad.write(f"{'btn' if cmd == 'pad' else 'axis'} {args[0]} {args[1]}\n")
            pad.flush()
        elif cmd == "set":
            pad.write(f"set {args[0]} {args[1]}\n")
            pad.flush()
        elif cmd == "capture":
            if not pad:
                raise SystemExit("capture needs a pad FIFO")
            ppm = os.path.join(shot_dir, args[0] + ".ppm")
            pad.write(f"capture {os.path.abspath(ppm)}\n")
            pad.flush()
            for _ in range(50):  # wait for the game to write it
                time.sleep(0.1)
                if os.path.exists(ppm) and os.path.getsize(ppm) > 0:
                    time.sleep(0.2)
                    break
            try:
                from PIL import Image
                Image.open(ppm).save(os.path.join(shot_dir, args[0] + ".png"))
                os.remove(ppm)
            except Exception as e:
                print(f"capture failed: {e}", flush=True)
        elif cmd in ("down", "up"):
            press(d, keycode(d, args[0]), cmd == "down")
        elif cmd == "click":
            xtest.fake_input(d, X.MotionNotify, x=int(args[0]), y=int(args[1]))
            d.sync()
            time.sleep(0.15)
            xtest.fake_input(d, X.ButtonPress, 1)
            d.sync()
            time.sleep(0.15)
            xtest.fake_input(d, X.ButtonRelease, 1)
            d.sync()
        elif cmd == "move":
            win = find_window(d.screen().root, " ".join(args[:-2]))
            if not win:
                raise SystemExit(f"no window titled {' '.join(args[:-2])!r}")
            win.configure(x=int(args[-2]), y=int(args[-1]))
            d.sync()
        elif cmd == "focus":
            win = find_window(d.screen().root, " ".join(args))
            if not win:
                raise SystemExit(f"no window titled {' '.join(args)!r}")
            win.set_input_focus(X.RevertToParent, X.CurrentTime)
            d.sync()
        elif cmd == "shot" and fb:
            subprocess.run([sys.executable, os.path.join(here, "xwd_to_png.py"), fb,
                            os.path.join(shot_dir, args[0] + ".png")], check=False)
        else:
            raise SystemExit(f"bad script line: {' '.join(parts)}")
        print(" ".join(parts), flush=True)


if __name__ == "__main__":
    main()
