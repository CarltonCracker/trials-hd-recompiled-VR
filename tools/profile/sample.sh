#!/usr/bin/env bash
# Sampling profiler: tools/profile/sample.sh <pid> <samples> [interval_s=0.05] [threads] [out]
# Attaches gdb (tools/profile/gdb_sampler.py) and interrupts the process every
# interval to record stacks of the threads whose names contain any of the
# comma-separated substrings in [threads] (all threads if empty). Summarize
# the output with tools/profile/summarize.py.
set -u
pid=$1 count=$2 interval=${3:-0.05} threads=${4:-} out=${5:-sampler.txt}
here="$(cd "$(dirname "$0")" && pwd)"
SAMPLER_COUNT=$count SAMPLER_THREADS=$threads SAMPLER_OUT=$out \
  gdb -p "$pid" -batch -x "$here/gdb_sampler.py" > "$out.gdb.log" 2>&1 &
gdb_pid=$!
# Wait for gdb to attach (and load symbols) before interrupting.
until grep -q "^TracerPid:[[:space:]]*$gdb_pid$" "/proc/$pid/status" 2>/dev/null ||
    ! kill -0 $gdb_pid 2>/dev/null; do
  sleep 0.5
done
sleep 2
# Only signal while gdb is attached - once it detaches, SIGINT would reach
# the process itself (and quit the game).
while sleep "$interval" && kill -0 $gdb_pid 2>/dev/null && \
    grep -q "^TracerPid:[[:space:]]*$gdb_pid$" "/proc/$pid/status" 2>/dev/null; do
  kill -INT "$pid" 2>/dev/null || break
done
wait $gdb_pid
