#!/usr/bin/env python3
"""Survey the game's statically linked Direct3D runtime by the GPU commands it emits.

Every Xbox 360 Direct3D function that talks to the GPU writes PM4 type-3 packets
(header = 0xC0000000 | (count - 1) << 16 | opcode << 8) into the command buffer, and
the header is almost always built as an immediate: `lis rX,0xC0nn` followed by
`ori rX,rX,0xOP00` (or addi). This finds those constants, attributes them to
functions, and lists each emitter's direct callers -- the candidate D3D API layer the
native renderer hooks (draws, shader/constant loads, resolves, swaps, waits).

Usage: d3d_survey.py <dump_dir> <generated/default dir> [--callers N]
Output is addresses and opcode names only (no game code).
"""
import bisect
import collections
import os
import re
import struct
import sys

PM4 = {
    0x48: "ME_INIT", 0x10: "NOP", 0x3F: "INDIRECT_BUFFER", 0x37: "INDIRECT_BUFFER_PFD",
    0x26: "WAIT_FOR_IDLE", 0x3C: "WAIT_REG_MEM", 0x52: "WAIT_REG_EQ", 0x53: "WAIT_REG_GTE",
    0x5C: "WAIT_UNTIL_READ", 0x5D: "WAIT_IB_PFD_COMPLETE", 0x21: "REG_RMW", 0x3E: "REG_TO_MEM",
    0x3D: "MEM_WRITE", 0x4F: "MEM_WRITE_CNTR", 0x44: "COND_EXEC", 0x45: "COND_WRITE",
    0x46: "EVENT_WRITE", 0x58: "EVENT_WRITE_SHD", 0x59: "EVENT_WRITE_CFL", 0x5A: "EVENT_WRITE_EXT",
    0x5B: "EVENT_WRITE_ZPD", 0x22: "DRAW_INDX", 0x36: "DRAW_INDX_2", 0x34: "DRAW_INDX_BIN",
    0x35: "DRAW_INDX_2_BIN", 0x23: "VIZ_QUERY", 0x25: "SET_STATE", 0x2D: "SET_CONSTANT",
    0x55: "SET_CONSTANT2", 0x56: "SET_SHADER_CONSTANTS", 0x2F: "LOAD_ALU_CONSTANT",
    0x27: "IM_LOAD", 0x2B: "IM_LOAD_IMMEDIATE", 0x2E: "LOAD_CONSTANT_CONTEXT",
    0x3B: "INVALIDATE_STATE", 0x4A: "SET_SHADER_BASES", 0x4B: "SET_BIN_BASE_OFFSET",
    0x50: "SET_BIN_MASK", 0x51: "SET_BIN_SELECT", 0x5E: "CONTEXT_UPDATE", 0x54: "INTERRUPT",
    0x64: "XE_SWAP", 0x2C: "IM_STORE",
}


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    dump_dir, gen_dir = sys.argv[1], sys.argv[2]
    max_callers = int(sys.argv[sys.argv.index("--callers") + 1]) if "--callers" in sys.argv else 6

    with open(os.path.join(dump_dir, "sections.txt")) as f:
        base = int(f.readline().split()[1], 16)
        code = [(int(p[1], 16), int(p[1], 16) + int(p[2], 16))
                for p in (l.split() for l in f) if "x" in p[3:]]
    with open(os.path.join(dump_dir, "image.bin"), "rb") as f:
        image = f.read()
    word = lambda a: struct.unpack(">I", image[a - base:a - base + 4])[0]

    funcs = []
    for name in os.listdir(gen_dir):
        if name.endswith("_register.cpp"):
            with open(os.path.join(gen_dir, name)) as f:
                funcs += [int(m, 16) for m in re.findall(r"SetFunction\(0x([0-9A-Fa-f]+)", f.read())]
    funcs.sort()
    owner = lambda a: funcs[bisect.bisect_right(funcs, a) - 1]

    emits = collections.defaultdict(collections.Counter)   # func -> Counter(opcode name)
    callers = collections.defaultdict(set)                 # callee -> {caller funcs}
    for start, end in code:
        for a in range(start, end - 4, 4):
            w = word(a)
            op = w >> 26
            if op == 18 and w & 3 == 1:  # bl
                tgt = (a + ((w & 0x03FFFFFC) ^ 0x02000000) - 0x02000000) & 0xFFFFFFFF
                callers[tgt].add(owner(a))
                continue
            # Fixed-size packets: lis rD,0xC0nn ; ori/addi rX,rD,OP<<8.
            # Variable-size packets: the count is computed and merged with
            # oris rX,rY,0xC000 (type 3), and the opcode is or'ed in separately
            # (ori rX,rX,OP<<8 within a few instructions either side).
            imm = w & 0xFFFF
            if op == 15 and not (w >> 16) & 31 and 0xC000 <= imm <= 0xC3FF:
                rd = (w >> 21) & 31
            elif op == 25 and 0xC000 <= imm <= 0xC3FF:  # oris rA,rS,imm: dest is bits 16-20
                rd = (w >> 16) & 31
            else:
                continue
            for k in list(range(1, 6)) + [-1, -2, -3]:
                w2 = word(a + 4 * k)
                if w2 >> 26 in (24, 14) and ((w2 >> 16) & 31 == rd or (w2 >> 21) & 31 == rd):
                    lo = w2 & 0xFFFF
                    opcode = (lo >> 8) & 0x7F
                    if lo & 0xFF == 0 and opcode in PM4:
                        emits[owner(a)][PM4[opcode]] += 1
                        break

    by_opcode = collections.defaultdict(list)
    for f, ops in emits.items():
        for name in ops:
            by_opcode[name].append(f)

    print(f"# {len(emits)} functions emit PM4 type-3 packets "
          f"(range sub_{min(emits):08X} .. sub_{max(emits):08X})\n")
    for name in sorted(by_opcode, key=lambda n: (-len(by_opcode[n]), n)):
        print(f"{name}: {len(by_opcode[name])} function(s)")
        for f in sorted(by_opcode[name]):
            others = ",".join(sorted(n for n in emits[f] if n != name))
            cs = sorted(callers.get(f, ()))
            shown = " ".join(f"sub_{c:08X}" for c in cs[:max_callers])
            more = f" (+{len(cs) - max_callers})" if len(cs) > max_callers else ""
            print(f"  sub_{f:08X}  also[{others}]  callers({len(cs)}): {shown}{more}")


if __name__ == "__main__":
    main()
