#!/usr/bin/env bash
# Sampling profiler: tools/profile/sample.sh <pid> [samples=300] [interval_s=0.05] [threads] [out]
# Attaches gdb (gdb_sampler.py) and records the stacks of the threads whose
# names contain any of the comma-separated substrings in [threads] (all if
# empty). Every sample stops the whole process briefly, so it runs slower while
# sampling; shares within a thread stay meaningful. Summarize with summarize.py.
set -euo pipefail
pid=$1 count=${2:-300} interval=${3:-0.05} threads=${4:-} out=${5:-samples.txt}
here="$(cd "$(dirname "$0")" && pwd)"
# Keep stdin open for gdb's event loop without an orphan holding output pipes.
input_dir=$(mktemp -d)
trap 'rm -rf "$input_dir"' EXIT
mkfifo "$input_dir/stdin"
exec {input_fd}<>"$input_dir/stdin"
SAMPLER_COUNT=$count SAMPLER_INTERVAL=$interval SAMPLER_THREADS=$threads SAMPLER_OUT=$out \
  gdb -q -nx -p "$pid" -x "$here/gdb_sampler.py" <&$input_fd > "$out.gdb.log" 2>&1 || true
exec {input_fd}>&-
echo "$(wc -l < "$out") thread samples in $out"
