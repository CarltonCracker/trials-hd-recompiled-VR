#!/usr/bin/env python3
"""Extract files from an Xbox 360 STFS (LIVE/PIRS/CON) package.

Block/hash math mirrors rexglue-sdk src/filesystem/devices/stfs_container_device.cpp.
Usage: stfs_extract.py <package> <out_dir> [--list]

For the base game package this also writes <out_dir>/package_license.toml with
the license mask from the package header, which the game reports at runtime.
"""
import os
import struct
import sys

BLOCK = 0x1000
PER_LEVEL = (0xAA, 0x70E4, 0x4AF768)
END_OF_CHAIN = 0xFFFFFF


def u24le(b):
    return b[0] | (b[1] << 8) | (b[2] << 16)


class Stfs:
    def __init__(self, path):
        self.f = open(path, "rb")
        hdr = self.f.read(0x971A)
        magic = hdr[0:4]
        if magic not in (b"LIVE", b"PIRS", b"CON "):
            raise SystemExit(f"not an STFS package (magic {magic!r})")
        self.header_size = struct.unpack(">I", hdr[0x340:0x344])[0]
        self.content_type = struct.unpack(">I", hdr[0x344:0x348])[0]
        self.volume_type = struct.unpack(">I", hdr[0x3A9:0x3AD])[0]
        if self.volume_type != 0:
            raise SystemExit("SVOD packages are not supported by this script")
        vd = hdr[0x379:0x379 + 0x24]
        self.flags = vd[2]
        self.read_only = bool(self.flags & 1)
        self.root_active = bool(self.flags & 2)
        self.ft_block_count = struct.unpack("<H", vd[3:5])[0]
        self.ft_block = u24le(vd[5:8])
        self.total_blocks = struct.unpack(">I", vd[0x1C:0x20])[0]
        self.title_id = struct.unpack(">I", hdr[0x360:0x364])[0]
        self.display_name = hdr[0x411:0x411 + 0x80].decode("utf-16-be", "ignore").split("\0")[0]
        # 16 license entries at 0x22C: u64 license id, u32 license bits, u32 flags.
        self.licenses = []
        for i in range(16):
            lid, bits, flags = struct.unpack(">QII", hdr[0x22C + 16 * i:0x22C + 16 * (i + 1)])
            if lid:
                self.licenses.append((lid, bits, flags))
        self.bpht = 1 if self.read_only else 2
        self.step0 = PER_LEVEL[0] + self.bpht
        self.step1 = PER_LEVEL[1] + (PER_LEVEL[0] + 1) * self.bpht
        self.base = (self.header_size + BLOCK - 1) & ~(BLOCK - 1)
        self.cache = {}

    def block_to_offset(self, idx):
        base, block = PER_LEVEL[0], idx
        for _ in range(3):
            block += ((idx + base) // base) * self.bpht
            if idx < base:
                break
            base *= PER_LEVEL[0]
        return self.base + (block << 12)

    def hash_block_number(self, idx, level):
        if level == 0:
            if idx < PER_LEVEL[0]:
                return 0
            block = (idx // PER_LEVEL[0]) * self.step0
            block += ((idx // PER_LEVEL[1]) + 1) * self.bpht
            return block if idx < PER_LEVEL[1] else block + self.bpht
        if level == 1:
            if idx < PER_LEVEL[1]:
                return self.step0
            return (idx // PER_LEVEL[1]) * self.step1 + self.bpht
        return self.step1

    def hash_offset(self, idx, level):
        return self.base + (self.hash_block_number(idx, level) << 12)

    def table(self, off):
        if off not in self.cache:
            self.f.seek(off)
            self.cache[off] = self.f.read(BLOCK)
        return self.cache[off]

    @staticmethod
    def entry(table, record):
        e = table[record * 0x18:(record + 1) * 0x18]
        return e[0x14], (e[0x15] << 16) | (e[0x16] << 8) | e[0x17]

    def next_block(self, idx):
        sec = BLOCK if self.root_active else 0
        if self.read_only:
            sec = 0
        else:
            if self.total_blocks > PER_LEVEL[0]:
                if self.total_blocks > PER_LEVEL[1]:
                    t2 = self.table(self.hash_offset(idx, 2) + sec)
                    st, _ = self.entry(t2, (idx // PER_LEVEL[1]) % PER_LEVEL[0])
                    sec = BLOCK if st & 0x40 else 0
                t1 = self.table(self.hash_offset(idx, 1) + sec)
                st, _ = self.entry(t1, (idx // PER_LEVEL[0]) % PER_LEVEL[0])
                sec = BLOCK if st & 0x40 else 0
        t0 = self.table(self.hash_offset(idx, 0) + sec)
        _, nxt = self.entry(t0, idx % PER_LEVEL[0])
        return nxt

    def entries(self):
        out, blk = [], self.ft_block
        for _ in range(self.ft_block_count):
            self.f.seek(self.block_to_offset(blk))
            data = self.f.read(BLOCK)
            for m in range(0x40):
                e = data[m * 0x40:(m + 1) * 0x40]
                if e[0] == 0:
                    break
                nl = e[0x28] & 0x3F
                out.append({
                    "name": e[:nl].decode("ascii", "replace"),
                    "dir": bool(e[0x28] & 0x80),
                    "start": u24le(e[0x2F:0x32]),
                    "parent": struct.unpack(">H", e[0x32:0x34])[0],
                    "size": struct.unpack(">I", e[0x34:0x38])[0],
                })
            blk = self.next_block(blk)
            if blk == END_OF_CHAIN:
                break
        for e in out:
            parts, p = [e["name"]], e["parent"]
            while p != 0xFFFF:
                parts.insert(0, out[p]["name"])
                p = out[p]["parent"]
            e["path"] = "/".join(parts)
        return out

    def read_file(self, e):
        chunks, blk, remaining = [], e["start"], e["size"]
        while remaining and blk != END_OF_CHAIN:
            n = min(BLOCK, remaining)
            self.f.seek(self.block_to_offset(blk))
            chunks.append(self.f.read(n))
            remaining -= n
            blk = self.next_block(blk)
        if remaining:
            raise RuntimeError(f"{e['path']}: chain ended with {remaining} bytes missing")
        return b"".join(chunks)


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    pkg, out_dir = sys.argv[1], sys.argv[2]
    s = Stfs(pkg)
    print(f"title {s.title_id:08X} content {s.content_type:08X} '{s.display_name}' "
          f"read_only={s.read_only} blocks={s.total_blocks}")
    for e in s.entries():
        if e["dir"]:
            continue
        print(f"{e['size']:>12}  {e['path']}")
        if "--list" in sys.argv:
            continue
        dst = os.path.join(out_dir, e["path"])
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        with open(dst, "wb") as f:
            f.write(s.read_file(e))
    for e in [] if "--list" in sys.argv else s.entries():
        if e["dir"]:
            os.makedirs(os.path.join(out_dir, e["path"]), exist_ok=True)

    # Record the package's own license so the game sees the same license mask
    # it would on the console that owns this package. Only the base game
    # package (content type 0x000D0000, Arcade title) carries the title license.
    mask = 0
    for _, bits, _ in s.licenses:
        mask |= bits
    print(f"license entries: {len(s.licenses)}  mask: 0x{mask:X}")
    if "--list" not in sys.argv and s.content_type == 0x000D0000:
        with open(os.path.join(out_dir, "package_license.toml"), "w") as f:
            f.write("# Written by tools/stfs_extract.py from the package's license block.\n")
            f.write(f"license_mask = 0x{mask:X}\n")


if __name__ == "__main__":
    main()
