# gdb script: sampling profiler for a running process (no perf needed).
# Use it through tools/profile/sample.sh, which interrupts the process
# periodically (a Python timer inside gdb doesn't run while the inferior does).
#
# Environment: SAMPLER_COUNT (default 200),
# SAMPLER_DEPTH frames (24), SAMPLER_OUT output file (sampler.txt),
# SAMPLER_THREADS comma-separated thread name substrings to record (all if unset).
# Each sample stops the process, records every thread's stack, and resumes it.
# Output: one line per thread sample: "<thread name>\t<frame0>;<frame1>;...".
import os

import gdb

count = int(os.environ.get("SAMPLER_COUNT", "200"))
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
# Count reached: stop recording but keep swallowing interrupts until detached.



def frame_name(frame):
    name = frame.name()
    if name:
        return name
    pc = frame.pc()
    solib = gdb.solib_name(pc)
    return f"0x{pc:x}" + (f"@{os.path.basename(solib)}" if solib else "")


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
        try:
            gdb.execute("continue", to_string=True)
        except gdb.error:
            break

gdb.execute("detach")
