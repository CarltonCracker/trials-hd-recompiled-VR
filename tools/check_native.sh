#!/usr/bin/env bash
# Guard for the native Linux build while the web port (docs/web-port/README.md) is
# developed: proves a change did not alter the native game.
#
# Usage:
#   tools/check_native.sh baseline [dir]   # before a change: record the reference
#   tools/check_native.sh check [dir]      # after it: build, compare, boot both renderers
# dir defaults to out/native_baseline (gitignored).
#
# check fails (exit 1) when:
# - the native build fails, or its cache has TRIALSHD_WEB enabled;
# - a native binary contains Emscripten/web-port symbols;
# - generated/default differs from the baseline (the changed files are listed; a
#   reviewed codegen change is accepted by re-running baseline);
# - either renderer (WebGPU, Vulkan) fails to boot through tests/headless/main_menu_nav.txt,
#   or its menu captures fall below 33 dB PSNR against the baseline.
# macOS cannot be checked here: see docs/web-port/README.md section 16.1.
# CHECK_GPU_INDEX picks the Vulkan device / WebGPU adapter (default 1: the RX 6600 on
# the original build server; use 0 where the only GPU is device 0).
set -uo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
mode="${1:-}"
dir="${2:-$root/out/native_baseline}"
build="$root/out/build/linux-amd64-relwithdebinfo"
export PATH="$root/.venv/bin:$PATH"
fail=0

if [[ "$mode" != baseline && "$mode" != check ]]; then
  sed -n '2,16p' "$0"
  exit 2
fi
mkdir -p "$dir"

echo "== native build"
if ! cmake --build "$build" -j"$(nproc)" > "$dir/build_$mode.log" 2>&1; then
  tail -20 "$dir/build_$mode.log"
  echo "FAIL: native build"
  exit 1
fi
if grep -q '^TRIALSHD_WEB:BOOL=ON' "$build/CMakeCache.txt" 2>/dev/null; then
  echo "FAIL: TRIALSHD_WEB is ON in the native build cache"
  fail=1
fi

echo "== web symbols in native binaries"
leaks=$(for f in "$build"/trials_hd "$build"/*.so; do
          nm -C --defined-only "$f" 2>/dev/null | grep -iE 'emscripten|emdawnwebgpu|wasm_|_web_audio|web_input' \
            | sed "s|^|$(basename "$f"): |"
        done | head -20)
if [[ -n "$leaks" ]]; then
  echo "$leaks"
  echo "FAIL: web-port symbols in native binaries"
  fail=1
fi

echo "== generated code fingerprint"
(cd "$root/generated/default" && sha256sum -- *.cpp *.h | sort -k2) > "$dir/generated_$mode.sha256"
if [[ $mode == check ]]; then
  if ! diff -q "$dir/generated_baseline.sha256" "$dir/generated_check.sha256" > /dev/null; then
    diff "$dir/generated_baseline.sha256" "$dir/generated_check.sha256" | grep '^[<>]' | awk '{print $1, $3}' | sort -u -k2 | head -20
    echo "FAIL: generated/default changed (re-run 'baseline' only after reviewing the codegen change)"
    fail=1
  fi
fi

echo "== boot both renderers (main menu navigation)"
for renderer in webgpu vulkan; do
  if [[ $renderer == webgpu ]]; then
    args=(--gpu_plugin=webgpu --webgpu_headless=true --webgpu_adapter="${CHECK_GPU_INDEX:-1}")
  else
    args=(--gpu_plugin=xenos --vulkan_device="${CHECK_GPU_INDEX:-1}" --vulkan_headless_surface=true)
  fi
  out="$dir/${mode}_$renderer"
  rm -rf "$out" && mkdir -p "$out"
  LOG_LEVEL=info INPUT="$root/tests/headless/main_menu_nav.txt" \
    timeout 90 "$root/tools/headless_run.sh" 45 "${args[@]}" > "$out/run.txt" 2>&1
  cp "$root"/out/run/nav_*.png "$out/" 2>/dev/null
  cp "$root/out/run/trials_hd.log" "$out/" 2>/dev/null
  count=$(ls "$out"/nav_*.png 2>/dev/null | wc -l)
  if (( count < 9 )); then
    echo "FAIL: $renderer produced $count of 9 menu captures (see $out/run.txt)"
    fail=1
    continue
  fi
  echo "$renderer: $count captures"
  if [[ $mode == check ]]; then
    python3 - "$dir/baseline_$renderer" "$out" <<'EOF' || fail=1
import sys, numpy as np
from pathlib import Path
from PIL import Image
ref, new = map(Path, sys.argv[1:])
worst = None
for a in sorted(ref.glob('nav_*.png')):
    b = new / a.name
    x = np.asarray(Image.open(a).convert('RGB'), dtype=float)
    y = np.asarray(Image.open(b).convert('RGB'), dtype=float)
    mse = ((x - y) ** 2).mean()
    psnr = 99.0 if mse == 0 else 10 * np.log10(255 ** 2 / mse)
    worst = psnr if worst is None else min(worst, psnr)
    if psnr < 33:
        print(f'  {a.name}: {psnr:.1f} dB')
print(f'  worst {worst:.1f} dB vs baseline')
sys.exit(0 if worst is not None and worst >= 33 else 1)
EOF
  fi
done

if [[ $mode == baseline ]]; then
  echo "baseline recorded in $dir"
elif (( fail )); then
  echo "NATIVE CHECK FAILED"
  exit 1
else
  echo "NATIVE CHECK PASSED"
fi
