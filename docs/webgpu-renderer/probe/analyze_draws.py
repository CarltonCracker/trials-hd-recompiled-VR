#!/usr/bin/env python3
"""Summarize draws.bin (gdb draw probe) joined with shader disassembly."""
import collections, glob, os, re, struct, sys

BIN, SHDIR = sys.argv[1], sys.argv[2]
HDR = struct.Struct('<4sIIIIQQ')
CTX_LO, CTX_N, FET_LO, FET_N = 0x2000, 0x400, 0x4800, 0x200
REC = HDR.size + 24 + CTX_N * 4 + FET_N * 4

PRIM = {1: 'point_list', 2: 'line_list', 3: 'line_strip', 4: 'tri_list', 5: 'tri_fan', 6: 'tri_strip',
        8: 'rect_list', 0xC: 'line_loop', 0xD: 'quad_list', 0xE: 'quad_strip', 0xF: 'polygon'}
CFMT = {0: 'k_8_8_8_8', 1: 'k_8_8_8_8_GAMMA', 2: 'k_2_10_10_10', 3: 'k_2_10_10_10_FLOAT', 4: 'k_16_16',
        5: 'k_16_16_16_16', 6: 'k_16_16_FLOAT', 7: 'k_16_16_16_16_FLOAT', 10: 'k_2_10_10_10_AS_10_10_10_10',
        12: 'k_2_10_10_10_FLOAT_AS_16_16_16_16', 14: 'k_32_FLOAT', 15: 'k_32_32_FLOAT'}
TFMT = {2: 'k_8', 4: 'k_5_6_5', 6: 'k_8_8_8_8', 7: 'k_2_10_10_10', 10: 'k_8_8', 15: 'k_4_4_4_4', 16: 'k_10_11_11',
        17: 'k_11_11_10', 18: 'DXT1', 19: 'DXT2_3', 20: 'DXT4_5', 22: 'k_24_8', 23: 'k_24_8_FLOAT', 24: 'k_16',
        25: 'k_16_16', 26: 'k_16_16_16_16', 30: 'k_16_FLOAT', 31: 'k_16_16_FLOAT', 32: 'k_16_16_16_16_FLOAT',
        36: 'k_32_FLOAT', 37: 'k_32_32_FLOAT', 38: 'k_32_32_32_32_FLOAT', 49: 'DXN', 51: 'DXT3A', 52: 'DXT5A'}
BLEND = {0: '0', 1: '1', 4: 'Sc', 5: '1-Sc', 6: 'Sa', 7: '1-Sa', 8: 'Dc', 9: '1-Dc', 10: 'Da', 11: '1-Da',
         12: 'K', 13: '1-K', 14: 'Ka', 15: '1-Ka', 16: 'SaSat'}
BOP = {0: '+', 1: '-', 2: 'min', 3: 'max', 4: 'rsub'}
CMP = ['never', 'less', 'eq', 'le', 'gt', 'ne', 'ge', 'always']
CLAMP = ['repeat', 'mirror', 'clamp_edge', 'mirror_clamp_edge', 'clamp_half', 'mirror_clamp_half', 'border', 'mirror_border']
DIM = ['1D', '2D', '3D', 'cube']


def bits(v, lo, n):
    return (v >> lo) & ((1 << n) - 1)


def sbits(v, lo, n):
    x = bits(v, lo, n)
    return x - (1 << n) if x >> (n - 1) else x


shader_cache = {}


def shader_slots(h, kind):
    if not h:
        return set(), set(), set()
    if (h, kind) not in shader_cache:
        path = os.path.join(SHDIR, 'shader_%016X.ucode.%s' % (h, 'vert' if kind == 'vs' else 'frag'))
        tf, vf, flags = set(), set(), set()
        if os.path.exists(path):
            s = open(path).read()
            tf = {int(x) for x in re.findall(r'\btf(\d+)', s)}
            vf = {int(x) for x in re.findall(r'\bvf(\d+)', s)}
            if 'eA' in s: flags.add('memexport')
            if 'kill' in s: flags.add('kill')
            if re.search(r'\boC[123]\b', s): flags.add('mrt')
            if 'oDepth' in s: flags.add('odepth')
        else:
            flags.add('missing_disasm')
        shader_cache[(h, kind)] = (tf, vf, flags)
    return shader_cache[(h, kind)]


data = open(BIN, 'rb').read()
n = len(data) // REC
wins = collections.defaultdict(lambda: collections.defaultdict(collections.Counter))
frames = collections.Counter()
for i in range(n):
    rec = data[i * REC:(i + 1) * REC]
    kind, win, prim, count, major, vh, ph = HDR.unpack_from(rec)
    o = HDR.size
    ib = rec[o:o + 24]; o += 24
    ctx = struct.unpack_from('<%dI' % CTX_N, rec, o); o += CTX_N * 4
    fet = struct.unpack_from('<%dI' % FET_N, rec, o)
    R = lambda r: ctx[r - CTX_LO]
    W = wins[win]
    if kind == b'SWAP':
        frames[win] += 1
        continue
    surf, cinfo, dinfo = R(0x2000), R(0x2001), R(0x2002)
    msaa = 1 << bits(surf, 16, 2)
    pitch = bits(surf, 0, 14)
    if kind == b'COPY':
        cc = R(0x2318); di = R(0x231B)  # RB_COPY_CONTROL, RB_COPY_DEST_INFO
        W['copy_cmd'][('src_sel=%d' % bits(cc, 0, 3), 'clear_c=%d' % bits(cc, 8, 1), 'clear_z=%d' % bits(cc, 9, 1),
                       'cmd=%d' % bits(cc, 20, 2), 'msaa=%d' % msaa, 'pitch=%d' % pitch)] += 1
        continue
    W['draws']['total'] += 1
    W['prim'][PRIM.get(prim, hex(prim))] += 1
    di_ = R(0x21FC)
    if ib != bytes(24):
        fmt, endian, cnt, gbase = struct.unpack_from('<IIII', ib)
        W['index'][('32bit' if fmt else '16bit', 'endian=%d' % endian)] += 1
    else:
        W['index'][('auto',)] += 1
    mode = bits(R(0x2208), 0, 3)
    W['edram_mode'][mode] += 1
    vs_tf, vs_vf, vs_fl = shader_slots(vh, 'vs')
    ps_tf, _, ps_fl = shader_slots(ph, 'ps')
    for f in vs_fl: W['vs_flags'][f] += 1
    for f in ps_fl: W['ps_flags'][f] += 1
    W['shader_pairs'][(vh, ph)] += 1
    if mode == 4:
        cmask = R(0x2104)
        rts = []
        rtregs = [0x2001, 0x2003, 0x2004, 0x2005]
        for k in range(4):
            if (cmask >> (4 * k)) & 0xF and (k == 0 or 'mrt' in ps_fl or True):
                if ph == 0 and k > 0: continue
                ci = R(rtregs[k])
                rts.append('%s%s' % (CFMT.get(bits(ci, 16, 4), bits(ci, 16, 4)),
                                     ('/bias%d' % sbits(ci, 20, 6)) if sbits(ci, 20, 6) else ''))
        dc = R(0x2200)
        z = 'z' if bits(dc, 1, 1) else '-'
        depth = ('D24FS8' if bits(dinfo, 16, 1) else 'D24S8') if (bits(dc, 1, 1) or bits(dc, 0, 1)) else 'nodepth'
        W['rt_config'][(tuple(rts), depth, 'msaa%d' % msaa, 'pitch%d' % pitch)] += 1
        W['depth'][('z' if bits(dc, 1, 1) else '-', 'w' if bits(dc, 2, 1) else '-',
                    CMP[bits(dc, 4, 3)], 'stencil' if bits(dc, 0, 1) else '')] += 1
        if bits(dc, 0, 1):
            W['stencil'][(CMP[bits(dc, 8, 3)], bits(dc, 11, 3), bits(dc, 14, 3), bits(dc, 17, 3), 'bf' if bits(dc, 7, 1) else '')] += 1
        for k, reg in enumerate([0x2201, 0x2209, 0x220A, 0x220B][:max(1, len(rts))]):
            b = R(reg)
            key = ('rt%d' % k, '%s*S %s %s*D' % (BLEND.get(bits(b, 0, 5)), BOP.get(bits(b, 5, 3)), BLEND.get(bits(b, 8, 5))),
                   'a: %s*S %s %s*D' % (BLEND.get(bits(b, 16, 5)), BOP.get(bits(b, 21, 3)), BLEND.get(bits(b, 24, 5))))
            W['blend'][key] += 1
        cc = R(0x2202)
        if bits(cc, 3, 1): W['alpha_test'][CMP[bits(cc, 0, 3)]] += 1
        if bits(cc, 4, 1): W['alpha_to_mask']['on'] += 1
    sc = R(0x2205)
    W['raster'][('cull_front' if bits(sc, 0, 1) else '', 'cull_back' if bits(sc, 1, 1) else '',
                 'poly_mode=%d' % bits(sc, 3, 2), 'polyoff' if bits(sc, 11, 3) else '',
                 'msaa_en' if bits(sc, 15, 1) else '', 'prov_last' if bits(sc, 19, 1) else '',
                 'multiprim_ib' if bits(sc, 21, 1) else '')] += 1
    clip = R(0x2204)
    W['clip'][('ucp=%x' % bits(clip, 0, 6), 'clip_dis' if bits(clip, 16, 1) else '', 'dx_clip' if bits(clip, 19, 1) else '',
               'vtx_kill_or' if bits(clip, 21, 1) else '')] += 1
    for slot in sorted(vs_tf | ps_tf):
        d = fet[slot * 6:slot * 6 + 6]
        if bits(d[0], 0, 2) != 2:
            W['tex'][('slot%d_not_texture' % slot,)] += 1
            continue
        fmt = bits(d[1], 0, 6)
        dim = DIM[bits(d[5], 9, 2)]
        if dim == '2D' or dim == 'cube':
            w, h = bits(d[2], 0, 13) + 1, bits(d[2], 13, 13) + 1
        else:
            w, h = bits(d[2], 0, 11) + 1, bits(d[2], 11, 11) + 1
        mips = '%d-%d' % (bits(d[4], 2, 4), bits(d[4], 6, 4))
        stage = 'vs' if slot in vs_tf else 'ps'
        sign = ''.join('USBG'[bits(d[0], 2 + 2 * c, 2)] for c in range(4))
        W['tex'][(stage, TFMT.get(fmt, fmt), dim, 'tiled' if bits(d[0], 31, 1) else 'linear',
                  'endian%d' % bits(d[1], 6, 2), 'sign=' + sign, 'num=%s' % ('int' if bits(d[3], 0, 1) else 'frac'),
                  'exp%d' % sbits(d[3], 13, 6) if sbits(d[3], 13, 6) else '')] += 1
        W['tex_size'][(TFMT.get(fmt, fmt), '%dx%d' % (w, h), 'mips' + mips, 'packed' if bits(d[5], 11, 1) else '')] += 1
        W['sampler'][(CLAMP[bits(d[0], 10, 3)], CLAMP[bits(d[0], 13, 3)], 'mag%d' % bits(d[3], 19, 2), 'min%d' % bits(d[3], 21, 2),
                      'mip%d' % bits(d[3], 23, 2), 'aniso%d' % bits(d[3], 25, 3), 'border%d' % bits(d[5], 0, 2) if bits(d[0], 10, 3) >= 6 else '')] += 1
        W['swizzle']['%03x' % bits(d[3], 1, 12)] += 1
    for vslot in sorted(vs_vf):
        d0, d1 = fet[vslot * 2], fet[vslot * 2 + 1]
        W['vfetch'][('type%d' % bits(d0, 0, 2), 'endian%d' % bits(d1, 0, 2))] += 1

for win in sorted(wins):
    W = wins[win]
    print('=' * 20, 'window', win, 'frames', frames[win], 'draws/frame %.0f' % (W['draws']['total'] / max(1, frames[win] - 1)))
    for key in ['prim', 'index', 'edram_mode', 'vs_flags', 'ps_flags', 'rt_config', 'depth', 'stencil', 'blend', 'alpha_test',
                'alpha_to_mask', 'raster', 'clip', 'tex', 'tex_size', 'sampler', 'swizzle', 'vfetch', 'copy_cmd']:
        c = W[key]
        if not c: continue
        print('--', key)
        for k, v in c.most_common(25):
            print('   %6d  %s' % (v, k))
    print('-- unique shader pairs', len(W['shader_pairs']))
