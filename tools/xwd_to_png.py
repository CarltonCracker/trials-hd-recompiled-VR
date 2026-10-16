#!/usr/bin/env python3
"""Convert an Xvfb framebuffer (-fbdir, XWD format, 24/32 bpp) to PNG.

Usage: xwd_to_png.py <Xvfb_screen0> <out.png>
Needs Pillow.
"""
import struct
import sys

from PIL import Image


def main():
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    with open(sys.argv[1], "rb") as f:
        data = f.read()
    # The header is 25 CARD32s; its byte order is the server's, so try both.
    for endian in (">", "<"):
        hdr = struct.unpack(endian + "25I", data[:100])
        if 100 <= hdr[0] < 4096:
            break
    else:
        raise SystemExit("unrecognized XWD header")
    header_size, width, height = hdr[0], hdr[4], hdr[5]
    byte_order, bpp, stride, ncolors = hdr[7], hdr[11], hdr[12], hdr[19]
    if bpp != 32:
        raise SystemExit(f"unsupported bits_per_pixel {bpp}")
    offset = header_size + ncolors * 12
    raw = "BGRX" if byte_order == 0 else "XRGB"
    img = Image.frombuffer("RGB", (width, height), data[offset:offset + stride * height],
                           "raw", raw, stride, 1)
    img.save(sys.argv[2])


if __name__ == "__main__":
    main()
