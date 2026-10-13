#!/usr/bin/env python3
"""Find code addresses referenced from data or code that codegen did not register.

Vtables and callback tables hold pointers to functions that are never called
directly, so the analyzer can miss them; calling one at runtime hits
InvalidFunctionTrap. This scans the non-executable sections of a dumped image
(tools/xex_dump) for pointers into executable sections, plus lis/addi address
constants in code, drops the ones that are
already registered, and keeps those that look like a function start (the
preceding word ends a function or is padding).

Usage: find_missing_funcs.py <dump_dir> <generated/default dir> [--toml]
"""
import os
import re
import struct
import sys

BLR, BCTR = 0x4E800020, 0x4E800420


def ends_function(word):
    if word in (BLR, BCTR, 0):
        return True
    # Unconditional relative branch without link: b target
    return (word >> 26) == 18 and (word & 3) == 0


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    dump_dir, gen_dir = sys.argv[1], sys.argv[2]
    with open(os.path.join(dump_dir, "sections.txt")) as f:
        base = int(f.readline().split()[1], 16)
        sections = []
        for line in f:
            parts = line.split()
            sections.append((parts[0], int(parts[1], 16), int(parts[2], 16), "x" in parts[3:]))
    with open(os.path.join(dump_dir, "image.bin"), "rb") as f:
        image = f.read()

    registered = set()
    for name in os.listdir(gen_dir):
        if name.endswith("_register.cpp"):
            with open(os.path.join(gen_dir, name)) as f:
                registered |= {int(m, 16) for m in re.findall(r"SetFunction\(0x([0-9A-Fa-f]+)", f.read())}

    code = [(start, start + size) for _, start, size, x in sections if x]

    # .pdata: (begin, packed) pairs; function length in words is bits 8..29.
    # A candidate strictly inside one of these ranges is a label, not a function.
    bodies = []
    for name, start, size, x in sections:
        if name == ".pdata":
            for off in range(0, size - 7, 8):
                begin, packed = struct.unpack(">II", image[start - base + off:start - base + off + 8])
                if begin:
                    bodies.append((begin, begin + ((packed >> 8) & 0x3FFFFF) * 4))
    bodies.sort()
    starts = [b for b, _ in bodies]

    def inside_body(a):
        import bisect
        i = bisect.bisect_right(starts, a) - 1
        return i >= 0 and bodies[i][0] < a < bodies[i][1]

    def jump_table_label(a):
        # MSVC switch dispatch: add rX,rX,rY ; mtctr rX ; bctr ; <case label>
        w = word(a - 12)
        return word(a - 4) == BCTR and w >> 26 == 31 and (w >> 1) & 0x3FF == 266

    def in_code(a):
        return any(s <= a < e for s, e in code)

    def word(a):
        return struct.unpack(">I", image[a - base:a - base + 4])[0]

    refs = {}
    for name, start, size, x in sections:
        if x or name in (".pdata", ".reloc"):
            continue
        for off in range(0, size - 3, 4):
            v = struct.unpack(">I", image[start - base + off:start - base + off + 4])[0]
            if v & 3 or v in registered or not in_code(v) or not in_code(v - 4):
                continue
            if (ends_function(word(v - 4)) and word(v) != 0 and not inside_body(v)
                    and not jump_table_label(v)):
                refs.setdefault(v, []).append(start + off)

    # Addresses built in code: lis rD,hi followed (within a few insns) by
    # addi rX,rD,lo / ori rX,rD,lo, e.g. callbacks stored into structs.
    for name, start, size, x in sections:
        if not x:
            continue
        for off in range(0, size - 3, 4):
            w = word(start + off)
            if w >> 26 != 15 or (w >> 16) & 0x1F != 0:  # lis = addis rD,0,imm
                continue
            rd, hi = (w >> 21) & 0x1F, w & 0xFFFF
            for k in range(1, 9):
                if off + 4 * k >= size:
                    break
                w2 = word(start + off + 4 * k)
                op, ra = w2 >> 26, (w2 >> 16) & 0x1F
                if ra != rd or op not in (14, 24):  # addi / ori
                    continue
                lo = w2 & 0xFFFF
                if op == 14 and lo & 0x8000:
                    lo -= 0x10000
                v = ((hi << 16) + lo) & 0xFFFFFFFF
                if (not v & 3 and v not in registered and in_code(v) and in_code(v - 4)
                        and ends_function(word(v - 4)) and word(v) != 0 and not inside_body(v)
                        and not jump_table_label(v)):
                    refs.setdefault(v, []).append(start + off)
                break

    toml = "--toml" in sys.argv
    for addr in sorted(refs):
        where = ", ".join(f"0x{r:08X}" for r in refs[addr][:3])
        if toml:
            print(f"0x{addr:08X} = {{}}  # referenced from data at {where}")
        else:
            print(f"0x{addr:08X}  refs={len(refs[addr])}  from {where}")
    print(f"# {len(refs)} candidate(s), {len(registered)} registered functions", file=sys.stderr)


if __name__ == "__main__":
    main()
