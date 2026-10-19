#!/usr/bin/env python3
"""Pack a folder into a read-only STFS (LIVE) package, for installer tests.

Usage: tests/installer/make_stfs.py <folder> <package> [--content-type 0xD0000|0x2]
                          [--title 0x5841095A] [--name "Display name"] [--license 0x1]
                          [--name-slot N] [--exclude NAME ...]

Writes the block and hash-table layout src/installer/stfs_package.cpp reads
(read-only packages: one hash block per level, next-block pointers in level 0).
Repack your own extracted game to test the setup screen without the original
package; never commit or share the result.
"""
import argparse
import os
import struct

BLOCK = 0x1000
END = 0xFFFFFF
HEADER_SIZE = 0xA000  # >= 0x971A, the header the reader requires


def block_to_offset(idx):
    base_lvl, block = 0xAA, idx
    for _ in range(3):
        block += (idx + base_lvl) // base_lvl
        if idx < base_lvl:
            break
        base_lvl *= 0xAA
    return HEADER_SIZE + (block << 12)


def hash_block_offset(idx):
    """Level-0 hash block holding idx's record (read-only: one block per table)."""
    if idx < 0xAA:
        number = 0
    else:
        number = (idx // 0xAA) * 0xAB + ((idx // 0x70E4) + 1)
        if idx >= 0x70E4:
            number += 1
    return HEADER_SIZE + (number << 12)


def collect(folder, exclude):
    """(name, is_dir, parent index, host path) in file-table order."""
    entries = []
    def walk(directory, parent):
        for name in sorted(os.listdir(directory)):
            if name in exclude:
                continue
            path = os.path.join(directory, name)
            if os.path.islink(path):
                continue
            if os.path.isdir(path):
                entries.append((name, True, parent, path))
                walk(path, len(entries) - 1)
            elif os.path.isfile(path):
                entries.append((name, False, parent, path))
    walk(folder, 0xFFFF)
    return entries


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('folder')
    parser.add_argument('package')
    parser.add_argument('--content-type', type=lambda v: int(v, 0), default=0x000D0000)
    parser.add_argument('--title', type=lambda v: int(v, 0), default=0x5841095A)
    parser.add_argument('--name', default='Trials HD')
    parser.add_argument('--license', type=lambda v: int(v, 0), default=1)
    # Language slot of the display name (0 English; a German console uses 4).
    parser.add_argument('--name-slot', type=int, default=0)
    parser.add_argument('--exclude', nargs='*', default=[])
    args = parser.parse_args()

    entries = collect(args.folder, set(args.exclude))
    if len(entries) >= 0xFFFF:
        raise SystemExit('too many files')
    table_blocks = max(1, (len(entries) + 63) // 64)
    next_block = table_blocks
    starts = []
    for name, is_dir, _, path in entries:
        if len(name.encode()) > 0x28:
            raise SystemExit(f'name too long for STFS: {name}')
        size = 0 if is_dir else os.path.getsize(path)
        starts.append((next_block, size))
        next_block += (size + BLOCK - 1) // BLOCK
    total = next_block

    end = max(block_to_offset(total - 1), hash_block_offset(total - 1)) + BLOCK
    with open(args.package, 'w+b') as out:
        out.truncate(end)
        header = bytearray(HEADER_SIZE)
        header[:4] = b'LIVE'
        struct.pack_into('>QII', header, 0x22C, 1, args.license, 0)
        struct.pack_into('>II', header, 0x340, HEADER_SIZE, args.content_type)
        struct.pack_into('>I', header, 0x360, args.title)
        vd = 0x379
        header[vd + 2] = 1  # read only
        struct.pack_into('<H', header, vd + 3, table_blocks)
        header[vd + 5:vd + 8] = (0).to_bytes(3, 'little')
        struct.pack_into('>I', header, vd + 0x1C, total)
        struct.pack_into('>I', header, 0x3A9, 0)  # STFS, not SVOD
        name = args.name.encode('utf-16-be')[:0x7E]
        slot = 0x411 + args.name_slot * 0x80
        header[slot:slot + len(name)] = name
        out.seek(0)
        out.write(header)

        def link(idx, nxt):
            out.seek(hash_block_offset(idx) + (idx % 0xAA) * 0x18 + 0x15)
            out.write(nxt.to_bytes(3, 'big'))

        table = bytearray(table_blocks * BLOCK)
        for i, ((name, is_dir, parent, _), (start, size)) in enumerate(zip(entries, starts)):
            e = memoryview(table)[i * 64:(i + 1) * 64]
            encoded = name.encode()
            e[:len(encoded)] = encoded
            e[0x28] = len(encoded) | (0x80 if is_dir else 0)
            blocks = (size + BLOCK - 1) // BLOCK
            e[0x29:0x2C] = blocks.to_bytes(3, 'little')
            e[0x2C:0x2F] = blocks.to_bytes(3, 'little')
            e[0x2F:0x32] = start.to_bytes(3, 'little')
            struct.pack_into('>HI', e, 0x32, parent, size)
        for b in range(table_blocks):
            out.seek(block_to_offset(b))
            out.write(table[b * BLOCK:(b + 1) * BLOCK])
            link(b, b + 1 if b + 1 < table_blocks else END)

        for (name, is_dir, _, path), (start, size) in zip(entries, starts):
            if is_dir or size == 0:
                continue
            count = (size + BLOCK - 1) // BLOCK
            with open(path, 'rb') as src:
                for k in range(count):
                    out.seek(block_to_offset(start + k))
                    out.write(src.read(BLOCK))
                    link(start + k, start + k + 1 if k + 1 < count else END)
    print(f'{args.package}: {len(entries)} entries, {total} blocks')


if __name__ == '__main__':
    main()
