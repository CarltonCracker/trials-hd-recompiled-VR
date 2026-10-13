# gdb script: sampling profiler for a running process (no perf needed).
#
#   gdb -p <pid> -batch -x tools/profile/gdb_sampler.py
#
# Environment: SAMPLER_COUNT (default 200), SAMPLER_INTERVAL seconds (0.05),
# SAMPLER_DEPTH frames (24), SAMPLER_OUT output file (sampler.txt),
# SAMPLER_THREADS comma-separated thread name substrings to record (all if unset).
# Each sample stops the process, records every thread's stack, and resumes it.
# Output: one line per thread sample: "<thread name>\t<frame0>;<frame1>;...".
import os
import signal
import threading
import time

import gdb

count = int(os.environ.get("SAMPLER_COUNT", "200"))
interval = float(os.environ.get("SAMPLER_INTERVAL", "0.05"))
depth = int(os.environ.get("SAMPLER_DEPTH", "24"))
out_path = os.environ.get("SAMPLER_OUT", "sampler.txt")
thread_filter = [t for t in os.environ.get("SAMPLER_THREADS", "").split(",") if t]

gdb.execute("set pagination off")
gdb.execute("set print thread-events off")
gdb.execute("set confirm off")
# The runtime uses signals itself (guest memory faults, thread suspension).
for sig in ("SIGSEGV", "SIGBUS", "SIGUSR1", "SIGUSR2", "SIGPIPE", "SIGALRM", "SIGCHLD"):
    gdb.execute(f"handle {sig} nostop noprint pass")
gdb.execute("handle SIGINT stop noprint nopass")

pid = gdb.selected_inferior().pid


def frame_name(frame):
    name = frame.name()
    if name:
        return name
    pc = frame.pc()
    solib = gdb.solib_name(pc)
    return f"0x{pc:x}" + (f"@{os.path.basename(solib)}" if solib else "")


def interrupt_later():
    time.sleep(interval)
    try:
        os.kill(pid, signal.SIGINT)
    except ProcessLookupError:
        pass


with open(out_path, "w") as out:
    for _ in range(count):
        for thread in gdb.selected_inferior().threads():
            if not thread.is_valid():
                continue
            if thread_filter and not any(t in (thread.name or "") for t in thread_filter):
                continue
            try:
                thread.switch()
                frame = gdb.newest_frame()
            except gdb.error:
                continue
            frames = []
            while frame is not None and len(frames) < depth:
                try:
                    frames.append(frame_name(frame))
                    frame = frame.older()
                except gdb.error:
                    break
            out.write(f"{thread.name or thread.num}\t{';'.join(frames)}\n")
            out.flush()
        timer = threading.Thread(target=interrupt_later)
        timer.start()
        try:
            gdb.execute("continue", to_string=True)
        except gdb.error:
            break
        timer.join()

gdb.execute("detach")
