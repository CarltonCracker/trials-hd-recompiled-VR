#!/usr/bin/env bash
# Per-thread CPU use and stack samples of a running trials_hd, for machines
# where perf isn't available (containers): /proc task times plus eu-stack.
#
# Usage: tools/profile/thread_sample.sh <pid> <out_dir> [seconds=10] [stack_samples=60]
# Output: <out_dir>/threads.txt (busy % per thread over the window) and
#         <out_dir>/stacks.txt (raw eu-stack samples, for tools/profile/stack_top.py).
set -uo pipefail
pid="$1"; out="$2"; secs="${3:-10}"; samples="${4:-60}"
mkdir -p "$out"
hz=$(getconf CLK_TCK)
snap() {
  for t in /proc/"$pid"/task/*; do
    # Fields 14/15 (utime/stime) follow the parenthesized comm, which may contain spaces.
    read -r stat <"$t/stat" 2>/dev/null || continue
    rest="${stat##*) }"
    set -- $rest
    echo "${t##*/} $(( ${12} + ${13} )) $(tr ' ' '_' <"$t/comm")"
  done
}
snap >"$out/.t0"
start=$(date +%s.%N)
: >"$out/stacks.txt"
interval=$(awk -v s="$secs" -v n="$samples" 'BEGIN { printf "%.3f", s / n }')
for _ in $(seq 1 "$samples"); do
  eu-stack -p "$pid" >>"$out/stacks.txt" 2>/dev/null
  sleep "$interval"
done
snap >"$out/.t1"
end=$(date +%s.%N)
awk -v hz="$hz" -v dt="$(awk -v a="$start" -v b="$end" 'BEGIN { print b - a }')" '
  NR == FNR { t0[$1] = $2; next }
  ($1 in t0) { busy = ($2 - t0[$1]) / hz / dt * 100; if (busy >= 1) printf "%6.1f%%  %s %s\n", busy, $1, $3 }
' "$out/.t0" "$out/.t1" | sort -rn >"$out/threads.txt"
rm -f "$out/.t0" "$out/.t1"
cat "$out/threads.txt"
