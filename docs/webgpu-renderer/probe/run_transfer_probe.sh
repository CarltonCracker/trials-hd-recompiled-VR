#!/usr/bin/env bash
# Count and analyze render target transfers per frame from the Vulkan backend.
# Hooks PerformTransfersAndResolveClears and IssueSwap via gdb without code changes.
#
# Usage: docs/webgpu-renderer/probe/run_transfer_probe.sh [windows] [out dir]
#   windows: comma list of <seconds after launch>:<frames>, default 27:5,60:10
#   (track-select menu, then riding 'Middle Name: Danger').
set -euo pipefail

root="$(cd "$(dirname "$0")/../../.." && pwd)"
here="$root/docs/webgpu-renderer/probe"
out="${2:-$root/out/transfer_probe}"
mkdir -p "$out"
wrap="$out/gdbwrap.sh"

cat > "$wrap" <<WRAP
#!/bin/bash
export TRANSFER_PROBE_OUT="$out/transfers_summary.txt" PROBE_WINDOWS="${1:-27:5,60:10}"
exec gdb -batch -x "$here/transfer_probe.gdb" --args "$root/out/build/linux-amd64-relwithdebinfo/trials_hd" "\$@" > "$out/gdb.log" 2>&1
WRAP

chmod +x "$wrap"
cd "$root"

PATH="$root/.venv/bin:$PATH" LOG_LEVEL=info INPUT=tests/headless/ride_middle_name_danger.txt \
  TRIALS_HD_EXE="$wrap" tools/headless_run.sh "${PROBE_SECS:-80}" --vulkan_device=1 --vulkan_headless_surface=true \
  --host_vsync=false

if [[ -f "$out/transfers_summary.txt" ]]; then
  cat "$out/transfers_summary.txt"
else
  echo "Probe failed; check $out/gdb.log"
  tail -n 30 "$out/gdb.log" || true
fi
