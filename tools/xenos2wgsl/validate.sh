#!/usr/bin/env bash
# Validate every .wgsl file in a folder with Tint (WGSL validation plus a
# SPIR-V compile with spirv-val, as Dawn's Vulkan backend would do).
# Usage: tools/xenos2wgsl/validate.sh <dir> [tint]
set -uo pipefail
dir=$1
tint=${2:-$(cd "$(dirname "$0")/../.." && pwd)/out/dawn-build/tint}
pass=0
fail=0
for f in "$dir"/*.wgsl; do
  if out=$("$tint" --format spirv --validate true -o /dev/null "$f" 2>&1); then
    pass=$((pass + 1))
  else
    fail=$((fail + 1))
    echo "== $(basename "$f")"
    echo "$out" | head -20
  fi
done
echo "$pass valid, $fail invalid"
[ "$fail" -eq 0 ]
