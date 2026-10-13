#!/usr/bin/env python3
"""Decode resolve dumps (dump_resolves_frame / dump_resolves_dir) into images.

Each resolve_NNN.bin holds the tiled destination texture bytes as written to guest
memory, and resolve_NNN.txt its parameters. This untiles the Xbox 360 2D tiled layout
and decodes the destination formats used by resolves, writing resolve_NNN.png
(channels normalized per image for viewing) and printing per-channel value ranges.

Usage: decode_resolve.py <dump_dir> [index ...]
Needs numpy and Pillow.
"""
import os
import sys

import numpy as np
from PIL import Image

# Destination texture formats (xenos::TextureFormat) -> bytes per pixel.
BYTES_PER_PIXEL = {6: 4, 7: 4, 23: 4, 25: 4, 26: 8, 31: 4, 32: 8, 36: 4}


def read_meta(path):
    meta = {}
    with open(path) as f:
        for line in f:
            key, value = line.split()
            meta[key] = int(value, 0)
    return meta


def tiled_offsets(width, height, pitch, bpp_log2):
    """Byte offsets of each (y, x) texel in a 2D tiled surface (pitch in texels)."""
    y = np.arange(height, dtype=np.int64)[:, None]
    x = np.arange(width, dtype=np.int64)[None, :]
    macro = ((y >> 5) * (pitch >> 5)) << (bpp_log2 + 7)
    micro = ((y & 6) << 2) << bpp_log2
    row = macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 8) << (3 + bpp_log2)) + ((y & 1) << 4)
    macro = (x >> 5) << (bpp_log2 + 7)
    micro = (x & 7) << bpp_log2
    offset = row + macro + ((micro & ~0xF) << 1) + (micro & 0xF)
    return (((offset & ~0x1FF) << 3) + ((offset & 0x1C0) << 2) + (offset & 0x3F)
            + ((y & 16) << 7) + (((((y & 8) >> 2) + (x >> 3)) & 3) << 6))


def endian_swap(words, endian):
    """Undo the resolve's Endian128 mode on 32-bit words (as stored in memory)."""
    b = words.view(np.uint8).reshape(-1, 4)
    if endian == 1:    # 8in16
        b = b[:, [1, 0, 3, 2]]
    elif endian == 2:  # 8in32
        b = b[:, [3, 2, 1, 0]]
    elif endian == 3:  # 16in32
        b = b[:, [2, 3, 0, 1]]
    return np.ascontiguousarray(b).view("<u4").reshape(words.shape)


def float20e4_to_float(v):
    exponent = (v >> 20) & 0xF
    mantissa = v & 0xFFFFF
    out = np.where(exponent == 0, mantissa / float(1 << 20) * 2.0 ** -14,
                   (1.0 + mantissa / float(1 << 20)) * 2.0 ** (exponent.astype(np.int64) - 15))
    return np.where(v == 0, 0.0, out)


def decode(dump_dir, index):
    stem = os.path.join(dump_dir, f"resolve_{index:03}")
    meta = read_meta(stem + ".txt")
    fmt = meta["dest_format"]
    bpp = BYTES_PER_PIXEL.get(fmt)
    if bpp != 4:
        return f"resolve_{index:03}: format {fmt} not decoded"
    width, height = meta["width_div_8"] * 8, meta["height_div_8"] * 8
    pitch = meta["pitch"]
    raw = np.fromfile(stem + ".bin", dtype="<u4")
    base_offset = meta["base"] - meta["address"]
    offs = (tiled_offsets(width, height, pitch, 2) + base_offset) // 4
    offs = np.clip(offs, 0, raw.size - 1)
    words = endian_swap(raw, meta["dest_endian"])[offs]

    if fmt == 6:    # k_8_8_8_8
        ch = np.stack([(words >> s) & 0xFF for s in (0, 8, 16, 24)], -1).astype(np.float64) / 255
        if meta["dest_swap"]:
            ch = ch[..., [2, 1, 0, 3]]
        names = "RGBA"
    elif fmt == 7:  # k_2_10_10_10
        ch = np.stack([(words >> s) & 0x3FF for s in (0, 10, 20)] + [(words >> 30) & 3], -1).astype(np.float64)
        ch[..., :3] /= 1023
        ch[..., 3] /= 3
        names = "RGBA"
    elif fmt == 25:  # k_16_16
        lo = (words & 0xFFFF).astype(np.int64)
        hi = (words >> 16).astype(np.int64)
        if meta["dest_number"] in (1, 3):  # signed
            lo = np.where(lo >= 0x8000, lo - 0x10000, lo)
            hi = np.where(hi >= 0x8000, hi - 0x10000, hi)
        ch = np.stack([lo, hi], -1).astype(np.float64)
        names = "RG"
    elif fmt == 23:  # k_24_8_FLOAT (depth in the top 24 bits)
        ch = np.stack([float20e4_to_float(words >> 8), (words & 0xFF) / 255.0], -1)
        names = ["depth", "stencil"]
    else:
        return f"resolve_{index:03}: format {fmt} not decoded"

    stats = ", ".join(f"{n}=[{ch[..., i].min():.4g}, {np.median(ch[..., i]):.4g}, {ch[..., i].max():.4g}]"
                      for i, n in enumerate(names))
    # Viewable image: first three channels (or the single/two channels), normalized.
    view = ch[..., :3] if ch.shape[-1] >= 3 else np.concatenate([ch, np.zeros(ch.shape[:2] + (3 - ch.shape[-1],))], -1)
    lo_v, hi_v = np.percentile(view, 0.5), np.percentile(view, 99.5)
    img = np.clip((view - lo_v) / max(hi_v - lo_v, 1e-9), 0, 1)
    Image.fromarray((img * 255).astype(np.uint8)).save(stem + ".png")
    return f"resolve_{index:03}: fmt {fmt} {width}x{height} {stats}"


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    dump_dir = sys.argv[1]
    indices = [int(a) for a in sys.argv[2:]] or sorted(
        int(n[8:11]) for n in os.listdir(dump_dir) if n.startswith("resolve_") and n.endswith(".txt"))
    for i in indices:
        print(decode(dump_dir, i))


if __name__ == "__main__":
    main()
