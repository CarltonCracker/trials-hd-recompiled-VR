# gdb script: sampling profiler for a running trials_hd (no perf needed; this
# host has kernel.perf_event_paranoid=3). Use it through sample.sh.
#
# Driven by gdb's own event loop: a Python timer posts "interrupt" every
# SAMPLER_INTERVAL seconds, each stop records the stacks of the selected
# threads and resumes with "continue &". (Interrupting from outside gdb, as an
# earlier version did, stopped after two samples.)
#
# Environment: SAMPLER_COUNT (300), SAMPLER_INTERVAL (0.05), SAMPLER_DEPTH (30),
# SAMPLER_OUT (samples.txt), SAMPLER_THREADS (comma-separated name substrings,
# all threads if unset). Output: "<thread name>|<tid>\t<frame0>;<frame1>;..."
# per thread per sample.
import os
import threading

import gdb

COUNT = int(os.environ.get('SAMPLER_COUNT', '300'))
INTERVAL = float(os.environ.get('SAMPLER_INTERVAL', '0.05'))
DEPTH = int(os.environ.get('SAMPLER_DEPTH', '30'))
OUT = os.environ.get('SAMPLER_OUT', 'samples.txt')
FILTER = [t for t in os.environ.get('SAMPLER_THREADS', '').split(',') if t]

for command in ('set pagination off', 'set confirm off', 'set print thread-events off',
                'set height 0'):
    gdb.execute(command)
# The runtime uses signals itself (guest memory faults, thread suspension).
for sig in ('SIGSEGV', 'SIGBUS', 'SIGUSR1', 'SIGUSR2', 'SIGPIPE', 'SIGALRM', 'SIGCHLD', 'SIG32',
            'SIG33', 'SIG34', 'SIG35'):
    gdb.execute('handle %s nostop noprint pass' % sig)

out = open(OUT, 'w')
state = {'samples': 0}


def frame_name(frame):
    name = frame.name()
    if name:
        return name
    solib = gdb.solib_name(frame.pc())
    return '0x%x' % frame.pc() + ('@' + os.path.basename(solib) if solib else '')


def record():
    for thread in gdb.selected_inferior().threads():
        if not thread.is_valid():
            continue
        name = thread.name or str(thread.num)
        if FILTER and not any(f in name for f in FILTER):
            continue
        try:
            thread.switch()
            frame = gdb.newest_frame()
        except gdb.error:
            continue
        frames = []
        while frame is not None and len(frames) < DEPTH:
            try:
                frames.append(frame_name(frame))
                frame = frame.older()
            except gdb.error:
                break
        out.write('%s|%d\t%s\n' % (name, thread.ptid[1], ';'.join(frames)))
    out.flush()


def resume():
    gdb.execute('continue &')
    threading.Timer(INTERVAL, lambda: gdb.post_event(lambda: gdb.execute('interrupt'))).start()


def on_stop(event):
    state['samples'] += 1
    record()
    if state['samples'] >= COUNT:
        out.close()
        gdb.post_event(lambda: (gdb.execute('detach'), gdb.execute('quit')))
        return
    gdb.post_event(resume)


gdb.events.stop.connect(on_stop)
on_stop(None)  # Attaching stopped the process: first sample, then start the loop.
