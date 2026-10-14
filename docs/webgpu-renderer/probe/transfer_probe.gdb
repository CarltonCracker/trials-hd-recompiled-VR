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
import gdb, time, os, struct
from collections import defaultdict

OUT_TXT = os.environ.get('TRANSFER_PROBE_OUT', 'out/transfer_probe/transfers_summary.txt')
# WINDOWS format: start_sec:num_frames
WINDOWS_STR = os.environ.get('PROBE_WINDOWS', '27:5,60:10')
WINDOWS = [(float(a), int(b)) for a, b in (w.split(':') for w in WINDOWS_STR.split(','))]

start_time = time.monotonic()

state = {
    'win': 0,
    'frames_left': -1,
    'cur_frame': 0,
    'in_window': False,
    'total_frames_probed': 0,
    'total_transfers': 0,
    'total_clear_transfers': 0,
    'total_copy_transfers': 0,
    'frame_stats': [],
    'cur_frame_transfers': 0,
    'cur_frame_clears': 0,
    'cur_frame_copies': 0,
    'key_pairs': defaultdict(lambda: {'count': 0, 'clears': 0, 'copies': 0, 'tile_ranges': set()}),
}

COLOR_FORMAT_NAMES = {
    0: '8_8_8_8',
    1: '8_8_8_8_GAMMA',
    2: '2_10_10_10',
    3: '2_10_10_10_FLOAT',
    4: '16_16',
    5: '16_16_16_16',
    6: '16_16_FLOAT',
    7: '16_16_16_16_FLOAT',
    8: '2_10_10_10_AS_10_10_10_2',
    9: '2_10_10_10_FLOAT_AS_10_10_10_2',
    10: '32_FLOAT',
    11: '32_32_FLOAT',
}

DEPTH_FORMAT_NAMES = {
    0: 'D24S8',
    1: 'D24FS8',
}

MSAA_NAMES = {
    0: '1X',
    1: '2X',
    2: '4X',
}

def decode_rt_key(key_val):
    base_tiles = key_val & 0x7FF
    pitch_tiles = (key_val >> 11) & 0xFF
    msaa = (key_val >> 19) & 0x3
    is_depth = bool((key_val >> 21) & 0x1)
    fmt = (key_val >> 22) & 0xF
    fmt_str = DEPTH_FORMAT_NAMES.get(fmt, str(fmt)) if is_depth else COLOR_FORMAT_NAMES.get(fmt, str(fmt))
    msaa_str = MSAA_NAMES.get(msaa, str(msaa))
    kind = "DEPTH" if is_depth else "COLOR"
    desc = "0x%08X[%s base=%d pitch=%d %s %s]" % (key_val, kind, base_tiles, pitch_tiles, fmt_str, msaa_str)
    return {
        'key': key_val,
        'is_depth': is_depth,
        'base_tiles': base_tiles,
        'pitch_tiles': pitch_tiles,
        'msaa': msaa,
        'format': fmt,
        'desc': desc
    }

class TransfersBreakpoint(gdb.Breakpoint):
    def stop(self):
        if not state['in_window']:
            return False
        
        try:
            mem = gdb.selected_inferior()
            # x86-64 System V ABI at function entry:
            # %rdi = this
            # %esi = render_target_count
            # %rdx = render_targets (RenderTarget* const*)
            # %rcx = render_target_transfers (const std::vector<Transfer>*)
            rt_count = int(gdb.parse_and_eval('$rsi')) & 0xFFFFFFFF
            rts_ptr = int(gdb.parse_and_eval('$rdx'))
            transfers_vec_ptr = int(gdb.parse_and_eval('$rcx'))
            
            if rt_count > 16:  # sanity check (usually 2 or 5)
                return False
                
            for i in range(rt_count):
                dest_ptr_bytes = mem.read_memory(rts_ptr + i * 8, 8)
                dest_ptr = struct.unpack('<Q', dest_ptr_bytes)[0]
                if dest_ptr == 0:
                    continue
                
                # RenderTarget::key_ is at offset 8 (uint32)
                dest_key_val = struct.unpack('<I', mem.read_memory(dest_ptr + 8, 4))[0]
                dest_info = decode_rt_key(dest_key_val)
                
                # std::vector<Transfer> has _M_start at +0, _M_finish at +8; sizeof(vector) == 24
                vec_addr = transfers_vec_ptr + i * 24
                start_ptr = struct.unpack('<Q', mem.read_memory(vec_addr, 8))[0]
                finish_ptr = struct.unpack('<Q', mem.read_memory(vec_addr + 8, 8))[0]
                
                if finish_ptr <= start_ptr:
                    continue
                num_transfers = (finish_ptr - start_ptr) // 32
                if num_transfers > 1000:
                    continue
                    
                for j in range(num_transfers):
                    t_addr = start_ptr + j * 32
                    t_bytes = mem.read_memory(t_addr, 32)
                    start_tiles, end_tiles, src_ptr, host_depth_src_ptr = struct.unpack('<IIQQ', t_bytes[:24])
                    is_clear = bool(struct.unpack('<?', t_bytes[24:25])[0])
                    clear_val = struct.unpack('<I', t_bytes[28:32])[0]
                    
                    if src_ptr != 0:
                        src_key_val = struct.unpack('<I', mem.read_memory(src_ptr + 8, 4))[0]
                        src_info = decode_rt_key(src_key_val)
                        src_desc = src_info['desc']
                    else:
                        src_desc = "NULL"
                    
                    dest_desc = dest_info['desc']
                    
                    state['total_transfers'] += 1
                    state['cur_frame_transfers'] += 1
                    if is_clear:
                        state['total_clear_transfers'] += 1
                        state['cur_frame_clears'] += 1
                    else:
                        state['total_copy_transfers'] += 1
                        state['cur_frame_copies'] += 1
                        
                    pair_key = (src_desc, dest_desc)
                    state['key_pairs'][pair_key]['count'] += 1
                    if is_clear:
                        state['key_pairs'][pair_key]['clears'] += 1
                    else:
                        state['key_pairs'][pair_key]['copies'] += 1
                    state['key_pairs'][pair_key]['tile_ranges'].add((start_tiles, end_tiles))
                    
                    print("TRANSFER [win %d frame %d] %s -> %s (tiles %d..%d, is_clear=%d)" %
                          (state['win'], state['cur_frame'], src_desc, dest_desc, start_tiles, end_tiles, is_clear),
                          flush=True)
        except Exception as e:
            print("ERROR in TransfersBreakpoint:", e, flush=True)
        return False

class SwapBreakpoint(gdb.Breakpoint):
    def stop(self):
        now = time.monotonic() - start_time
        if state['win'] < len(WINDOWS):
            w_start, w_frames = WINDOWS[state['win']]
            if not state['in_window']:
                if now >= w_start:
                    state['in_window'] = True
                    state['frames_left'] = w_frames
                    state['cur_frame'] = 0
                    print("PROBE_TRANSFERS: starting window %d at t=%.2fs (%d frames)" %
                          (state['win'], now, w_frames), flush=True)
            else:
                # End of a probed frame
                state['frame_stats'].append({
                    'window': state['win'],
                    'frame': state['cur_frame'],
                    'transfers': state['cur_frame_transfers'],
                    'clears': state['cur_frame_clears'],
                    'copies': state['cur_frame_copies'],
                })
                print("FRAME %d done: %d transfers (%d clears, %d copies)" %
                      (state['cur_frame'], state['cur_frame_transfers'],
                       state['cur_frame_clears'], state['cur_frame_copies']), flush=True)
                state['cur_frame_transfers'] = 0
                state['cur_frame_clears'] = 0
                state['cur_frame_copies'] = 0
                state['cur_frame'] += 1
                state['total_frames_probed'] += 1
                state['frames_left'] -= 1
                
                if state['frames_left'] <= 0:
                    print("PROBE_TRANSFERS: finished window %d at t=%.2fs" % (state['win'], now), flush=True)
                    state['in_window'] = False
                    state['win'] += 1
                    if state['win'] >= len(WINDOWS):
                        write_summary()
                        print("PROBE_TRANSFERS: all windows complete, exiting", flush=True)
                        gdb.execute("quit")
        return False

def write_summary():
    os.makedirs(os.path.dirname(os.path.abspath(OUT_TXT)), exist_ok=True)
    with open(OUT_TXT, 'w') as f:
        f.write("=== RENDER TARGET TRANSFERS PROBE SUMMARY ===\n")
        f.write("Total frames probed: %d\n" % state['total_frames_probed'])
        f.write("Total transfers: %d\n" % state['total_transfers'])
        f.write("Total clear transfers (elided to clear): %d\n" % state['total_clear_transfers'])
        f.write("Total copy transfers (actual EDRAM blits): %d\n" % state['total_copy_transfers'])
        if state['total_frames_probed'] > 0:
            f.write("Transfers per frame: %.2f (%.2f clears, %.2f copies)\n\n" %
                    (state['total_transfers'] / state['total_frames_probed'],
                     state['total_clear_transfers'] / state['total_frames_probed'],
                     state['total_copy_transfers'] / state['total_frames_probed']))
        f.write("--- Unique Key Pairs ---\n")
        for (src_desc, dest_desc), data in sorted(state['key_pairs'].items(), key=lambda x: -x[1]['count']):
            f.write("Pair: %s  ==>  %s\n" % (src_desc, dest_desc))
            f.write("  Count: %d (clears: %d, copies: %d)\n" % (data['count'], data['clears'], data['copies']))
            ranges_str = ", ".join("%d..%d" % r for r in sorted(data['tile_ranges']))
            f.write("  Tile ranges: %s\n\n" % ranges_str)
        f.write("--- Per-Frame Breakdown ---\n")
        for fs in state['frame_stats']:
            f.write("Win %d Frame %d: %d transfers (%d clears, %d copies)\n" %
                    (fs['window'], fs['frame'], fs['transfers'], fs['clears'], fs['copies']))

def on_objfile(ev):
    if 'librexgpu' not in (ev.new_objfile.filename or ''):
        return
    transfer_sym = "rex::graphics::vulkan::VulkanRenderTargetCache::PerformTransfersAndResolveClears(unsigned int, rex::graphics::RenderTargetCache::RenderTarget* const*, std::vector<rex::graphics::RenderTargetCache::Transfer, std::allocator<rex::graphics::RenderTargetCache::Transfer> > const*, unsigned long const*, rex::graphics::RenderTargetCache::Transfer::Rectangle const*)"
    swap_sym = "rex::graphics::vulkan::VulkanCommandProcessor::IssueSwap(unsigned int, unsigned int, unsigned int)"
    
    try:
        t_addr = int(gdb.parse_and_eval("(long)&'%s'" % transfer_sym))
        TransfersBreakpoint('*0x%x' % t_addr)
        print("BP PerformTransfersAndResolveClears: 0x%x" % t_addr, flush=True)
    except Exception as e:
        print("Failed to hook PerformTransfersAndResolveClears:", e, flush=True)
        
    try:
        s_addr = int(gdb.parse_and_eval("(long)&'%s'" % swap_sym))
        SwapBreakpoint('*0x%x' % s_addr)
        print("BP IssueSwap: 0x%x" % s_addr, flush=True)
    except Exception as e:
        print("Failed to hook IssueSwap:", e, flush=True)

gdb.events.new_objfile.connect(on_objfile)
end

run
quit
