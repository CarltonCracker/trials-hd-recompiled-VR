#!/usr/bin/env python3
"""Compare two draw probe binaries (e.g. Vulkan probe vs WebGPU debug dump).

Reads records with format matching probe/draw_probe.gdb (36 B header, 24 B index buffer,
4096 B context registers, 2048 B fetch registers) and compares draw streams draw-by-draw.
"""

import argparse
import struct
import sys

HDR = struct.Struct('<4sIIIIQQ')
IB = struct.Struct('<IIIIQ')
CTX_LO, CTX_N = 0x2000, 0x400
FET_LO, FET_N = 0x4800, 0x200
REC_SIZE = HDR.size + 24 + CTX_N * 4 + FET_N * 4

PRIM = {
    1: 'point_list', 2: 'line_list', 3: 'line_strip', 4: 'tri_list',
    5: 'tri_fan', 6: 'tri_strip', 8: 'rect_list', 0xC: 'line_loop',
    0xD: 'quad_list', 0xE: 'quad_strip', 0xF: 'polygon'
}

KEY_REGS = [
    (0x2000, 'RB_SURFACE_INFO'),
    (0x2001, 'RB_COLOR_INFO'),
    (0x2002, 'RB_DEPTH_INFO'),
    (0x2003, 'RB_COLOR1_INFO'),
    (0x2004, 'RB_COLOR2_INFO'),
    (0x2005, 'RB_COLOR3_INFO'),
    (0x2100, 'RB_BLENDCONTROL'),
    (0x2104, 'RB_COLOR_MASK'),
    (0x2200, 'RB_DEPTHCONTROL'),
    (0x2205, 'PA_SU_SC_MODE_CNTL'),
    (0x2208, 'RB_MODECONTROL'),
    (0x2210, 'PA_SC_SCREEN_SCISSOR_TL'),
    (0x2211, 'PA_SC_SCREEN_SCISSOR_BR'),
    (0x2220, 'PA_CL_VPORT_XSCALE'),
    (0x2221, 'PA_CL_VPORT_XOFFSET'),
    (0x2222, 'PA_CL_VPORT_YSCALE'),
    (0x2223, 'PA_CL_VPORT_YOFFSET'),
    (0x2224, 'PA_CL_VPORT_ZSCALE'),
    (0x2225, 'PA_CL_VPORT_ZOFFSET'),
    (0x2318, 'RB_COPY_CONTROL'),
    (0x231B, 'RB_COPY_DEST_INFO'),
    (0x231C, 'RB_COPY_DEST_BASE'),
]


def read_records(path):
    with open(path, 'rb') as f:
        data = f.read()
    if len(data) % REC_SIZE != 0:
        print(f"Warning: {path} size {len(data)} is not a multiple of record size {REC_SIZE}", file=sys.stderr)
    n = len(data) // REC_SIZE
    records = []
    for i in range(n):
        chunk = data[i * REC_SIZE:(i + 1) * REC_SIZE]
        kind, win, prim, count, major, vh, ph = HDR.unpack_from(chunk, 0)
        ib_tuple = IB.unpack_from(chunk, HDR.size)
        ctx_offset = HDR.size + 24
        ctx = struct.unpack_from(f'<{CTX_N}I', chunk, ctx_offset)
        fet_offset = ctx_offset + CTX_N * 4
        fet = struct.unpack_from(f'<{FET_N}I', chunk, fet_offset)
        records.append({
            'index': i,
            'kind': kind.decode('ascii', errors='replace').strip('\x00'),
            'win': win,
            'prim': prim,
            'count': count,
            'major': major,
            'vh': vh,
            'ph': ph,
            'ib': ib_tuple,
            'ctx': ctx,
            'fet': fet,
        })
    return records


def summarize_records(recs):
    counts = {'DRAW': 0, 'COPY': 0, 'SWAP': 0, 'other': 0}
    for r in recs:
        k = r['kind']
        counts[k if k in counts else 'other'] += 1
    return counts


def run_self_test():
    import tempfile
    import os
    print("Running diff_draws self-test...")
    with tempfile.NamedTemporaryFile(suffix='.bin', delete=False) as f1, \
         tempfile.NamedTemporaryFile(suffix='.bin', delete=False) as f2:
        path1 = f1.name
        path2 = f2.name

    try:
        # Construct synthetic records: SWAP, DRAW, SWAP
        swap0 = HDR.pack(b'SWAP', 0, 0, 0, 0, 0, 0) + bytes(24) + bytes(CTX_N * 4) + bytes(FET_N * 4)
        ib = IB.pack(0, 1, 12, 0x12340000, 24)
        ctx = bytearray(CTX_N * 4)
        fet = bytearray(FET_N * 4)
        # Set RB_COLOR_INFO (0x2001) to 0x0A020280
        struct.pack_into('<I', ctx, (0x2001 - CTX_LO) * 4, 0x0A020280)
        draw0 = HDR.pack(b'DRAW', 0, 4, 12, 0, 0x1122334455667788, 0x99AABBCCDDEEFF00) + ib + bytes(ctx) + bytes(fet)
        swap1 = HDR.pack(b'SWAP', 0, 1, 0, 0, 0, 0) + bytes(24) + bytes(CTX_N * 4) + bytes(FET_N * 4)

        assert len(swap0) == REC_SIZE, f"SWAP size {len(swap0)} != {REC_SIZE}"
        assert len(draw0) == REC_SIZE, f"DRAW size {len(draw0)} != {REC_SIZE}"

        with open(path1, 'wb') as f:
            f.write(swap0 + draw0 + swap1)
        with open(path2, 'wb') as f:
            f.write(swap0 + draw0 + swap1)

        recs1 = read_records(path1)
        recs2 = read_records(path2)
        assert len(recs1) == 3
        assert recs1[1]['kind'] == 'DRAW'
        assert recs1[1]['prim'] == 4
        assert recs1[1]['count'] == 12
        assert recs1[1]['vh'] == 0x1122334455667788
        assert recs1[1]['ph'] == 0x99AABBCCDDEEFF00
        assert recs1[1]['ctx'][0x2001 - CTX_LO] == 0x0A020280

        # Identical comparison
        assert diff_records(recs1, recs2) == 0

        # Mutate path2 (change primitive from 4 to 6)
        draw0_mut = HDR.pack(b'DRAW', 0, 6, 12, 0, 0x1122334455667788, 0x99AABBCCDDEEFF00) + ib + bytes(ctx) + bytes(fet)
        with open(path2, 'wb') as f:
            f.write(swap0 + draw0_mut + swap1)
        recs2_mut = read_records(path2)
        assert diff_records(recs1, recs2_mut) == 1

        print("Self-test passed! All record encodings, comparisons, and diffs verified.")
        return 0
    finally:
        if os.path.exists(path1): os.remove(path1)
        if os.path.exists(path2): os.remove(path2)


def diff_records(recs_a, recs_b, verbose=False, max_diffs=25):
    min_len = min(len(recs_a), len(recs_b))
    max_len = max(len(recs_a), len(recs_b))
    mismatches = 0
    diff_count = 0

    for i in range(min_len):
        ra = recs_a[i]
        rb = recs_b[i]
        diffs = []

        if ra['kind'] != rb['kind']:
            diffs.append(f"kind: {ra['kind']} vs {rb['kind']}")
        elif ra['kind'] == 'DRAW':
            if ra['prim'] != rb['prim']:
                pa = PRIM.get(ra['prim'], hex(ra['prim']))
                pb = PRIM.get(rb['prim'], hex(rb['prim']))
                diffs.append(f"prim: {pa} vs {pb}")
            if ra['count'] != rb['count']:
                diffs.append(f"index_count: {ra['count']} vs {rb['count']}")
            if ra['major'] != rb['major']:
                diffs.append(f"major_mode: {ra['major']} vs {rb['major']}")
            if ra['vh'] != rb['vh']:
                diffs.append(f"vs_hash: 0x{ra['vh']:016X} vs 0x{rb['vh']:016X}")
            if ra['ph'] != rb['ph']:
                diffs.append(f"ps_hash: 0x{ra['ph']:016X} vs 0x{rb['ph']:016X}")
            if ra['ib'] != rb['ib']:
                diffs.append(f"index_buffer: {ra['ib']} vs {rb['ib']}")

            for reg_idx, reg_name in KEY_REGS:
                va = ra['ctx'][reg_idx - CTX_LO]
                vb = rb['ctx'][reg_idx - CTX_LO]
                if va != vb:
                    diffs.append(f"{reg_name}: 0x{va:08X} vs 0x{vb:08X}")

        elif ra['kind'] == 'COPY':
            for reg_idx, reg_name in [(0x2318, 'RB_COPY_CONTROL'), (0x231B, 'RB_COPY_DEST_INFO'), (0x231C, 'RB_COPY_DEST_BASE')]:
                va = ra['ctx'][reg_idx - CTX_LO]
                vb = rb['ctx'][reg_idx - CTX_LO]
                if va != vb:
                    diffs.append(f"{reg_name}: 0x{va:08X} vs 0x{vb:08X}")

        if diffs:
            mismatches += 1
            if verbose and diff_count < max_diffs:
                diff_count += 1
                print(f"Record {i} mismatch ({ra['kind']}):")
                for d in diffs:
                    print(f"   - {d}")

    if min_len < max_len:
        if verbose:
            print(f"\nLength mismatch: {min_len} common records, {max_len - min_len} trailing in {'A' if len(recs_a) > len(recs_b) else 'B'}")
        mismatches += (max_len - min_len)

    return mismatches


def main():
    parser = argparse.ArgumentParser(description="Diff two draw probe binaries.")
    parser.add_argument("file_a", nargs="?", help="First binary file (e.g. Vulkan probe draws.bin)")
    parser.add_argument("file_b", nargs="?", help="Second binary file (e.g. WebGPU debug dump draws_webgpu.bin)")
    parser.add_argument("-v", "--verbose", action="store_true", help="Print detailed differences for each mismatch")
    parser.add_argument("--max-diffs", type=int, default=25, help="Maximum number of diff details to print")
    parser.add_argument("--self-test", action="store_true", help="Run self-tests and exit")
    args = parser.parse_args()

    if args.self_test:
        return run_self_test()

    if not args.file_a or not args.file_b:
        parser.error("file_a and file_b are required unless --self-test is given")

    recs_a = read_records(args.file_a)
    recs_b = read_records(args.file_b)

    counts_a = summarize_records(recs_a)
    counts_b = summarize_records(recs_b)

    print(f"File A: {args.file_a} ({len(recs_a)} records: {counts_a['DRAW']} draws, {counts_a['COPY']} copies, {counts_a['SWAP']} swaps)")
    print(f"File B: {args.file_b} ({len(recs_b)} records: {counts_b['DRAW']} draws, {counts_b['COPY']} copies, {counts_b['SWAP']} swaps)")

    mismatches = diff_records(recs_a, recs_b, verbose=args.verbose or True, max_diffs=args.max_diffs)
    if mismatches == 0:
        print(f"\nAll {len(recs_a)} records match perfectly!")
        return 0
    else:
        print(f"\nTotal record mismatches: {mismatches} / {max(len(recs_a), len(recs_b))}")
        return 1


if __name__ == '__main__':
    sys.exit(main())
