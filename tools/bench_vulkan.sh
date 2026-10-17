#!/usr/bin/env bash
# Headless, muted Vulkan benchmark: a private weston headless compositor plus the
# virtual pad riding 'Middle Name: Danger' (tests/headless/bench_fps.txt). Never
# opens a window on the desktop and never plays audio.
#
# Usage: tools/bench_vulkan.sh <out_dir> <width> <height> [extra game args...]
#   1440p: tools/bench_vulkan.sh out/bench/1440 2560 1440 --draw_resolution_scale_x=2 --draw_resolution_scale_y=2
#   720p:  tools/bench_vulkan.sh out/bench/720 1280 720 --draw_resolution_scale_x=1 --draw_resolution_scale_y=1
# Env:
#   EXE=path        binary (default out/build/linux-amd64-relwithdebinfo/trials_hd)
#   PERF=1          perf record (call graphs) for 10 s of the ride -> <out_dir>/perf.data
#   PERF=stat       perf stat page faults / context switches -> <out_dir>/perfstat.txt
#   CAPTURE=1       save the start-line frame (before the throttle) to <out_dir>/start.ppm
#   TRIALS_GPU_PROF=1  with tools/profile/vk_gpu_prof.patch applied: GPU_PROF lines in the log
# Output: <out_dir>/trials_hd.log, fps.txt, gpu_busy.txt; prints the bench_summary line.
# The game rewrites trials_hd.toml next to the binary; it is backed up and restored.
set -uo pipefail
root="$(cd "$(dirname "$0")/.." && pwd)"
# One benchmark at a time across all worktrees and agents (they share the GPU):
# wait for the lock in the git common dir.
lock="$(git -C "$root" rev-parse --path-format=absolute --git-common-dir)/vk-bench.lock"
exec 9>"$lock"
if ! flock -n 9; then
  echo "waiting for another benchmark to finish ($lock)..." >&2
  flock 9
fi
out="$1"; w="$2"; h="$3"; shift 3
mkdir -p "$out"
exe="${EXE:-$root/out/build/linux-amd64-relwithdebinfo/trials_hd}"
cfg="$(dirname "$exe")/trials_hd.toml"
[[ -f "$cfg" ]] && cp "$cfg" "$out/config_backup.toml"
sock="bench-$$"
weston --backend=headless --renderer=gl --width="$w" --height="$h" --refresh-rate=240000 \
  --socket="$sock" --idle-time=0 >"$out/weston.log" 2>&1 &
wpid=$!
sleep 1.5
fifo="$out/pad.fifo"; rm -f "$fifo"; mkfifo "$fifo"
export TRIALS_HD_VIRTUAL_PAD="$fifo"
# The PS5 controller on the dev machine would take player 1; hide real pads.
WAYLAND_DISPLAY="$sock" SDL_VIDEO_DRIVER=wayland SDL_AUDIO_DRIVER=dummy \
  SDL_GAMECONTROLLER_IGNORE_DEVICES=0x054C/0x0CE6 SDL_JOYSTICK_HIDAPI=0 SDL_JOYSTICK_LINUX=0 \
  "$exe" --game_data_root="$root/assets" --log_level=info --log_file="$out/trials_hd.log" \
  --gpu_plugin=xenos --log_guest_fps=true --host_vsync=false --vsync=false --present_fps_limit=0 \
  --display_width="$w" --display_height="$h" "$@" >"$out/stdout.log" 2>&1 &
gpid=$!
(
  exec 3>"$fifo"
  while read -r cmd a b; do
    case "$cmd" in
      wait) sleep "$a" ;;
      pad) echo "btn $a $b" >&3 ;;
      axis)
        # CAPTURE=1: grab the stationary start-line frame just before the throttle.
        if [[ "${CAPTURE:-0}" == 1 && "$a" == rt && "$b" != 0 ]]; then
          echo "capture $(realpath "$out")/start.ppm" >&3
          sleep 1
        fi
        echo "axis $a $b" >&3 ;;
    esac
  done < <(grep -vE '^\s*(#|$)' "$root/tests/headless/bench_fps.txt")
) &
ipid=$!
# The ride starts around t=50 s.
sleep 55
busy=/sys/class/drm/card1/device/gpu_busy_percent
for _ in $(seq 1 10); do cat "$busy" 2>/dev/null; sleep 1; done >"$out/gpu_busy.txt" &
case "${PERF:-0}" in
  1) perf record -F 999 -g --call-graph=fp -p "$gpid" -o "$out/perf.data" -- sleep 10 >/dev/null 2>&1 ;;
  stat) perf stat -e page-faults,context-switches,cpu-migrations -p "$gpid" \
          -o "$out/perfstat.txt" -- sleep 10 >/dev/null 2>&1 ;;
  *) sleep 10 ;;
esac
sleep 8
kill "$ipid" "$gpid" 2>/dev/null; sleep 1; kill -9 "$gpid" 2>/dev/null
kill "$wpid" 2>/dev/null
[[ -f "$out/config_backup.toml" ]] && cp "$out/config_backup.toml" "$cfg"
grep "guest fps" "$out/trials_hd.log" | sed 's/.*guest fps/guest fps/' | tail -30 >"$out/fps.txt"
echo "gpu busy %: $(tr '\n' ' ' <"$out/gpu_busy.txt")"
python3 "$root/tools/bench_summary.py" "$out/trials_hd.log" --threshold 61
