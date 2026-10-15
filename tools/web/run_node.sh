#!/usr/bin/env bash
# Runs the web build under Node.js with the host file system (preset
# web-wasm64-node): the runtime boot without a browser, for WM1 checks
# (docs/web-port/README.md 11). Activate emsdk first.
#
#   tools/web/run_node.sh [seconds] [extra runtime flags...]
#
# Uses the user's own assets/ (never bundled). Output: out/web-run/node.log.
set -uo pipefail
root="$(cd "$(dirname "$0")/../.." && pwd)"
seconds="${1:-60}"
shift || true
build="$root/out/build/web-wasm64-node"
run="$root/out/web-run"
mkdir -p "$run"
cd "$run"
# Node keeps running on SIGINT while workers are busy; this preload makes it exit.
echo "process.on('SIGINT', () => process.exit(0));" > "$run/exit_on_sigint.js"
timeout --signal=INT --kill-after=10 "$seconds" "${EMSDK_NODE:?Activate emsdk first}" \
  -r "$run/exit_on_sigint.js" "$build/trials_hd.js" --game_data_root="$root/assets" --log_file="$run/trials_hd.log" "$@" \
  > "$run/node.log" 2>&1
status=$?
echo "exit status $status (124 = stopped after ${seconds}s)"
grep -E "first guest swap|guest swaps|error|Error|abort|Aborted" "$run/node.log" "$run/trials_hd.log" 2>/dev/null | head -20
exit 0
