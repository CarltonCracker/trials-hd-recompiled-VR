#!/usr/bin/env python3
"""
xsh_dump.py - Parse Xenos shader cache (.xsh) files.

Format (from docs/webgpu-renderer/README.md section 5.1 and SDK pipeline_cache.h):
  Header (8 bytes):
    magic:   u32 'XESH' (0x48534558)
    version: u32 0x20201219 (swapped 0x19122020)
  Records:
    hash:        u64
    count_type:  u32 (type: bit 31, 0 = vertex, 1 = pixel; dwords: bits 0..30)
    ucode_bytes: dwords * 4
"""

import argparse
import os
import struct
import sys
ROOT_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def get_default_xsh():
    candidates = [
        os.path.join(ROOT_DIR, "out/build/linux-amd64-relwithdebinfo/cache/shaders/shareable/5841095A.xsh"),
        os.path.join(ROOT_DIR, "cache/shaders/shareable/5841095A.xsh"),
        os.path.expanduser("~/.local/share/trials_hd/cache/shaders/shareable/5841095A.xsh"),
    ]
    for c in candidates:
        if os.path.exists(c):
            return c
    return candidates[0]


DEFAULT_XSH = get_default_xsh()
MAGIC = 0x48534558  # 'XESH'
VERSION = 0x20201219


def parse_xsh(file_path):
    with open(file_path, "rb") as f:
        hdr = f.read(8)
        if len(hdr) < 8:
            raise ValueError(f"File too small: {file_path}")
        magic, ver_swapped = struct.unpack("<II", hdr)
        if magic != MAGIC:
            raise ValueError(f"Invalid magic: {hex(magic)} (expected {hex(MAGIC)})")

        ver = ((ver_swapped & 0xFF) << 24) | (((ver_swapped >> 8) & 0xFF) << 16) | \
              (((ver_swapped >> 16) & 0xFF) << 8) | ((ver_swapped >> 24) & 0xFF)
        if ver != VERSION:
            raise ValueError(f"Invalid version: {hex(ver)} (expected {hex(VERSION)})")

        shaders = []
        while True:
            rec_hdr = f.read(12)
            if not rec_hdr:
                break
            if len(rec_hdr) < 12:
                raise ValueError("Unexpected truncated record header")
            shader_hash, count_type = struct.unpack("<QI", rec_hdr)
            is_pixel = (count_type >> 31) & 1
            dword_count = count_type & 0x7FFFFFFF
            ucode = f.read(dword_count * 4)
            if len(ucode) != dword_count * 4:
                raise ValueError(f"Unexpected EOF reading ucode for {shader_hash:016X}")
            shaders.append({
                "hash": shader_hash,
                "type": "PS" if is_pixel else "VS",
                "dwords": dword_count,
                "bytes": len(ucode),
                "ucode": ucode,
            })
    return shaders


def main():
    parser = argparse.ArgumentParser(description="Parse and dump Xenos shader cache (.xsh)")
    parser.add_argument("xsh_file", nargs="?", default=DEFAULT_XSH,
                        help=f"Path to .xsh file (default: {DEFAULT_XSH})")
    parser.add_argument("-o", "--out-dir", help="Directory to dump shader ucode binaries")
    parser.add_argument("-q", "--quiet", action="store_true", help="Print summary only")
    args = parser.parse_args()

    if not os.path.exists(args.xsh_file):
        print(f"Error: file not found: {args.xsh_file}", file=sys.stderr)
        return 1

    shaders = parse_xsh(args.xsh_file)

    if args.out_dir:
        os.makedirs(args.out_dir, exist_ok=True)
        for s in shaders:
            ext = "frag" if s["type"] == "PS" else "vert"
            out_name = f"shader_{s['hash']:016X}.ucode.bin.{ext}"
            out_path = os.path.join(args.out_dir, out_name)
            with open(out_path, "wb") as f:
                f.write(s["ucode"])

    vs_count = sum(1 for s in shaders if s["type"] == "VS")
    ps_count = sum(1 for s in shaders if s["type"] == "PS")
    total_dwords = sum(s["dwords"] for s in shaders)
    avg_dwords = total_dwords / len(shaders) if shaders else 0
    max_dwords = max((s["dwords"] for s in shaders), default=0)

    if not args.quiet:
        print(f"{'Index':<6} {'Hash':<18} {'Type':<6} {'Dwords':<8} {'Bytes':<8}")
        print("-" * 50)
        for idx, s in enumerate(shaders):
            print(f"{idx:<6} {s['hash']:016X}   {s['type']:<6} {s['dwords']:<8} {s['bytes']:<8}")
        print("-" * 50)

    print(f"Total shaders: {len(shaders)} ({vs_count} vertex + {ps_count} pixel)")
    print(f"Average dwords: {avg_dwords:.1f}, Max dwords: {max_dwords}")

    if args.out_dir:
        print(f"Dumped {len(shaders)} ucode binaries to {args.out_dir}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
