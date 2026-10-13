#!/usr/bin/env bash
# Run trials_hd on a headless machine under Xvfb, logging to a file and saving
# periodic screenshots of the virtual display.
#
# Usage: [LOG_LEVEL=trace] [INPUT=script] tools/headless_run.sh [seconds=60] [extra args...]
# Output goes to out/run/: trials_hd.log and shot_NNN.png (gitignored).
# INPUT names a tools/xsend.py script that is played against the display (keys
# act as controller input via --mnk_mode, which the port enables by default).
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
secs="${1:-60}"
shift || true
exe="${TRIALS_HD_EXE:-$root/out/build/linux-amd64-relwithdebinfo/trials_hd}"
out="$root/out/run"
mkdir -p "$out"
rm -f "$out"/shot_*.png "$out"/Xvfb_screen0 "$out"/trials_hd*.log "$out"/stdout.log "$out"/input.log

display=":$((90 + RANDOM % 100))"
# Scriptable virtual gamepad (src/virtual_pad.cpp), driven by xsend pad/axis lines.
pad_fifo="$out/pad.fifo"
rm -f "$pad_fifo" && mkfifo "$pad_fifo"
export TRIALS_HD_VIRTUAL_PAD="$pad_fifo"
Xvfb "$display" -screen 0 1280x720x24 -fbdir "$out" -nolisten tcp &
xvfb_pid=$!
game_pid=""
input_pid=""
trap 'kill "$input_pid" "$game_pid" "$xvfb_pid" 2>/dev/null || true' EXIT
sleep 1

DISPLAY="$display" "$exe" --game_data_root="$root/assets" --log_level="${LOG_LEVEL:-debug}" \
  --log_file="$out/trials_hd.log" "$@" > "$out/stdout.log" 2>&1 &
game_pid=$!

if [[ -n "${INPUT:-}" ]]; then
  python3 "$root/tools/xsend.py" "$display" "$INPUT" "$out/Xvfb_screen0" "$out" "$pad_fifo" \
    > "$out/input.log" 2>&1 &
  input_pid=$!
fi

for ((i = 0; i < secs / 5; i++)); do
  sleep 5
  if ! kill -0 "$game_pid" 2>/dev/null; then
    break
  fi
  python3 "$root/tools/xwd_to_png.py" "$out/Xvfb_screen0" \
    "$(printf '%s/shot_%03d.png' "$out" "$i")" || true
done

if kill -0 "$game_pid" 2>/dev/null; then
  echo "still running after ${secs}s"
else
  wait "$game_pid" && rc=0 || rc=$?
  echo "exited with code $rc"
fi
