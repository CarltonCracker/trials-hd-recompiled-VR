#!/usr/bin/env bash
# Sample per-draw Xenos GPU state from the Vulkan backend during a scripted ride.
# Breakpoints on VulkanCommandProcessor::IssueDraw/IssueCopy/IssueSwap copy the
# context registers (0x2000-0x23FF) and fetch constants (0x4800-0x49FF) per draw.
# No code changes; needs gdb, Xvfb, a relwithdebinfo build, and the repo .venv
# (Pillow + python-xlib for tools/xsend.py).
#
# Usage: docs/webgpu-renderer/probe/run_draw_probe.sh [windows] [out dir]
#   windows: comma list of <seconds after launch>:<frames>, default 30:2,62:3,78:2
#   (track-select menu, then riding 'Middle Name: Danger').
# Then: python3 docs/webgpu-renderer/probe/analyze_draws.py <out>/draws.bin out/shader_dump
# (shader disassembly comes from --dump_shaders=out/shader_dump).
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd)"
here="$root/docs/webgpu-renderer/probe"
out="${2:-$root/out/draw_probe}"
mkdir -p "$out"
wrap="$out/gdbwrap.sh"
cat > "$wrap" <<WRAP
#!/bin/bash
export PROBE_OUT="$out/draws.bin" PROBE_WINDOWS="${1:-30:2,62:3,78:2}"
exec gdb -batch -x "$here/draw_probe.gdb" --args "$root/out/build/linux-amd64-relwithdebinfo/trials_hd" "\$@" > "$out/gdb.log" 2>&1
WRAP
chmod +x "$wrap"
cd "$root"
PATH="$root/.venv/bin:$PATH" LOG_LEVEL=info INPUT=tests/headless/ride_middle_name_danger.txt \
  TRIALS_HD_EXE="$wrap" tools/headless_run.sh 95 --vulkan_device=1 --vulkan_headless_surface=true \
  --host_vsync=false --dump_shaders=out/shader_dump
grep PROBE "$out/gdb.log" || true
