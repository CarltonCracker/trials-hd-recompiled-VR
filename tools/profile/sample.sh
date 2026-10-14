#!/usr/bin/env bash
# Sampling profiler: tools/profile/sample.sh <pid> [samples=300] [interval_s=0.05] [threads] [out]
# Attaches gdb (gdb_sampler.py) and records the stacks of the threads whose
# names contain any of the comma-separated substrings in [threads] (all if
# empty). Every sample stops the whole process briefly, so it runs slower while
# sampling; shares within a thread stay meaningful. Summarize with summarize.py.
set -euo pipefail
pid=$1 count=${2:-300} interval=${3:-0.05} threads=${4:-} out=${5:-samples.txt}
here="$(cd "$(dirname "$0")" && pwd)"
# gdb must stay in its event loop after the script runs: keep stdin open
# (process substitution, so nothing waits for the sleep once gdb quits).
SAMPLER_COUNT=$count SAMPLER_INTERVAL=$interval SAMPLER_THREADS=$threads SAMPLER_OUT=$out \
  gdb -q -nx -p "$pid" -x "$here/gdb_sampler.py" < <(sleep 3600) > "$out.gdb.log" 2>&1 || true
echo "$(wc -l < "$out") thread samples in $out"
