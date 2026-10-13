#!/usr/bin/env python3
"""Disassemble a range of a dumped XEX image (see tools/xex_dump).

Usage: disasm.py <dump_dir> <address> [before_insns] [after_insns]
Needs capstone (pip install capstone). VMX128 opcodes print as '.long'.
"""
import os
import sys

import capstone


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    dump_dir, addr = sys.argv[1], int(sys.argv[2], 16)
    before = int(sys.argv[3]) if len(sys.argv) > 3 else 16
    after = int(sys.argv[4]) if len(sys.argv) > 4 else 16
    with open(os.path.join(dump_dir, "sections.txt")) as f:
        base = int(f.readline().split()[1], 16)
    with open(os.path.join(dump_dir, "image.bin"), "rb") as f:
        image = f.read()

    md = capstone.Cs(capstone.CS_ARCH_PPC, capstone.CS_MODE_32 | capstone.CS_MODE_BIG_ENDIAN)
    start = addr - before * 4
    for i in range(before + after + 1):
        a = start + i * 4
        word = image[a - base:a - base + 4]
        insn = next(md.disasm(word, a), None)
        text = f"{insn.mnemonic} {insn.op_str}" if insn else f".long 0x{word.hex()}"
        mark = "=>" if a == addr else "  "
        print(f"{mark} {a:08X}  {word.hex()}  {text}")


if __name__ == "__main__":
    main()
