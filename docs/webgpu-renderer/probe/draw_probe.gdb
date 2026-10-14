set pagination off
set confirm off
set breakpoint pending on
handle SIGSEGV nostop noprint pass
handle SIGBUS nostop noprint pass
handle SIGUSR1 nostop noprint pass
handle SIGUSR2 nostop noprint pass
handle SIG32 nostop noprint pass
handle SIG33 nostop noprint pass
handle SIG34 nostop noprint pass
handle SIG35 nostop noprint pass
handle SIGPIPE nostop noprint pass
python
import gdb, struct, time, os
OUT = os.environ.get('PROBE_OUT', 'draws.bin')
# Capture windows: (start seconds after launch, swaps to record)
WINDOWS = [(float(a), int(b)) for a, b in (w.split(':') for w in os.environ.get('PROBE_WINDOWS', '27:2,58:3').split(','))]
start = time.monotonic()
out = open(OUT, 'wb')
state = {'win': 0, 'swaps': -1, 'off': None, 'draws': 0, 'copies': 0,
         'pmb': None, 'strip_count': 0, 'auto_strip_count': 0, 'strip_indices': 0, 'strip_ffff': 0,
         'other_indexed_count': 0, 'other_indices': 0, 'other_ffff': 0}
CTX_LO, CTX_N = 0x2000, 0x400
FET_LO, FET_N = 0x4800, 0x200

def offsets(this):
    if state['off'] is None:
        cp = '((rex::graphics::CommandProcessor*)%d)' % this
        rf = int(gdb.parse_and_eval(cp + '->register_file_'))
        vso = int(gdb.parse_and_eval('(long)&((rex::graphics::CommandProcessor*)0)->active_vertex_shader_'))
        pso = int(gdb.parse_and_eval('(long)&((rex::graphics::CommandProcessor*)0)->active_pixel_shader_'))
        ho = int(gdb.parse_and_eval('(long)&((rex::graphics::Shader*)0)->ucode_data_hash_'))
        state['off'] = (rf, vso, pso, ho)
    return state['off']

def get_pmb(this):
    if state['pmb'] is None:
        try:
            cp = '((rex::graphics::CommandProcessor*)0x%x)' % this
            state['pmb'] = int(gdb.parse_and_eval(cp + '->memory_->physical_membase_'))
        except Exception as e:
            try:
                cp = '((rex::graphics::CommandProcessor*)0x%x)' % this
                state['pmb'] = int(gdb.parse_and_eval(cp + '->memory_->mapping_base_'))
            except Exception as e2:
                print('PMB lookup error:', e, e2, flush=True)
    return state['pmb']

def active():
    if state['win'] >= len(WINDOWS):
        return False
    t, _ = WINDOWS[state['win']]
    return time.monotonic() - start >= t

def rd(mem, a, n):
    return bytes(mem.read_memory(a, n))

def record(kind, this, prim=0, count=0, ib=0, major=0):
    mem = gdb.selected_inferior()
    rf, vso, pso, ho = offsets(this)
    vs = struct.unpack('<Q', rd(mem, this + vso, 8))[0]
    ps = struct.unpack('<Q', rd(mem, this + pso, 8))[0]
    vh = struct.unpack('<Q', rd(mem, vs + ho, 8))[0] if vs else 0
    ph = struct.unpack('<Q', rd(mem, ps + ho, 8))[0] if ps else 0
    ibf = rd(mem, ib, 24) if ib else bytes(24)
    ctx = rd(mem, rf + CTX_LO * 4, CTX_N * 4)
    fet = rd(mem, rf + FET_LO * 4, FET_N * 4)
    hdr = struct.pack('<4sIIIIQQ', kind, state['win'], prim, count, major, vh, ph)
    out.write(hdr + ibf + ctx + fet)

    # S-06: Scan index buffers from guest memory
    if kind == b'DRAW':
        if prim == 6 and not ib:
            state['auto_strip_count'] += 1
        elif ib:
            pmb = get_pmb(this)
            if pmb:
                fmt, endian, cnt, gbase = struct.unpack_from('<IIII', ibf)
                if cnt > 0 and gbase > 0:
                    host_addr = pmb + (gbase & 0x1FFFFFFF)
                    is_strip = (prim == 6)
                    if is_strip:
                        state['strip_count'] += 1
                    else:
                        state['other_indexed_count'] += 1
                    try:
                        if fmt == 0:  # 16-bit
                            raw = rd(mem, host_addr, cnt * 2)
                            endian_char = '>' if endian == 1 else '<'
                            indices = struct.unpack(f'{endian_char}{cnt}H', raw)
                            ffff = sum(1 for idx in indices if idx == 0xFFFF)
                            if is_strip:
                                state['strip_indices'] += cnt
                                state['strip_ffff'] += ffff
                            else:
                                state['other_indices'] += cnt
                                state['other_ffff'] += ffff
                        else:  # 32-bit
                            raw = rd(mem, host_addr, cnt * 4)
                            endian_char = '>' if endian == 1 else '<'
                            indices = struct.unpack(f'{endian_char}{cnt}I', raw)
                            ffff = sum(1 for idx in indices if idx == 0xFFFFFFFF)
                            if is_strip:
                                state['strip_indices'] += cnt
                                state['strip_ffff'] += ffff
                            else:
                                state['other_indices'] += cnt
                                state['other_ffff'] += ffff
                    except Exception as e:
                        print('Index read error at', hex(host_addr), ':', e, flush=True)

class Draw(gdb.Breakpoint):
    def stop(self):
        if not active() or state['swaps'] < 0:
            return False
        f = gdb.selected_frame()
        this = int(f.read_register('rdi'))
        record(b'DRAW', this, int(f.read_register('rsi')) & 0xFFFFFFFF, int(f.read_register('rdx')) & 0xFFFFFFFF,
               int(f.read_register('rcx')), int(f.read_register('r8')) & 1)
        state['draws'] += 1
        return False

class Copy(gdb.Breakpoint):
    def stop(self):
        if not active() or state['swaps'] < 0:
            return False
        this = int(gdb.selected_frame().read_register('rdi'))
        record(b'COPY', this)
        state['copies'] += 1
        return False

class Swap(gdb.Breakpoint):
    def stop(self):
        if not active():
            return False
        this = int(gdb.selected_frame().read_register('rdi'))
        state['swaps'] += 1
        out.write(struct.pack('<4sIIIIQQ', b'SWAP', state['win'], state['swaps'], 0, 0, 0, 0) + bytes(24) + bytes(CTX_N * 4) + bytes(FET_N * 4))
        out.flush()
        _, n = WINDOWS[state['win']]
        if state['swaps'] >= n:
            print('PROBE window %d done: %d draws, %d copies, t=%.1f' % (state['win'], state['draws'], state['copies'], time.monotonic() - start), flush=True)
            print('PROBE strips: %d indexed strip draws (%d indices, %d equal to 0xFFFF); %d auto-indexed strip draws' %
                  (state['strip_count'], state['strip_indices'], state['strip_ffff'], state['auto_strip_count']), flush=True)
            print('PROBE other indexed: %d draws, %d total indices, %d equal to 0xFFFF' %
                  (state['other_indexed_count'], state['other_indices'], state['other_ffff']), flush=True)
            state['win'] += 1
            state['swaps'] = -1
            state['draws'] = state['copies'] = 0
        return False

SYMS = {
 'IssueDraw': ("rex::graphics::vulkan::VulkanCommandProcessor::IssueDraw(rex::graphics::xenos::PrimitiveType, unsigned int, rex::graphics::CommandProcessor::IndexBufferInfo*, bool)", Draw),
 'IssueCopy': ("rex::graphics::vulkan::VulkanCommandProcessor::IssueCopy()", Copy),
 'IssueSwap': ("rex::graphics::vulkan::VulkanCommandProcessor::IssueSwap(unsigned int, unsigned int, unsigned int)", Swap),
}
def on_objfile(ev):
    if 'librexgpu' not in (ev.new_objfile.filename or ''):
        return
    for k, (sym, cls) in SYMS.items():
        addr = int(gdb.parse_and_eval("(long)&'%s'" % sym))
        cls('*0x%x' % addr)
        print('BP', k, hex(addr), flush=True)
gdb.events.new_objfile.connect(on_objfile)
end
run

quit
