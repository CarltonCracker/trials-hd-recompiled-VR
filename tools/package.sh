#!/usr/bin/env bash
# Assemble a runnable build folder from a finished CMake build.
#
# Usage: tools/package.sh [preset=linux-amd64-release] [out_dir] [--link-assets]
#
# The folder holds the executable, the SDK runtime libraries it loads from its
# own directory ($ORIGIN), the package extractor and a short README. It never
# contains game data: players put their own extracted copy in <out_dir>/assets.
# --link-assets symlinks <repo>/assets there for local testing (do not share a
# folder containing a real assets directory).
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
preset="${1:-linux-amd64-release}"
out="${2:-$root/dist/trials_hd-linux-x86_64}"
build="$root/out/build/$preset"

if [[ ! -x "$build/trials_hd" ]]; then
  echo "no trials_hd in $build; build the '$preset' preset first" >&2
  exit 1
fi

rm -rf "$out"
mkdir -p "$out/tools"
cp "$build/trials_hd" "$out/"
cp "$build"/*.so "$out/"
if ls "$build/src/gpu_webgpu"/librexgpu-webgpu*.so >/dev/null 2>&1; then
  cp -u "$build/src/gpu_webgpu"/librexgpu-webgpu*.so "$out/"
fi
cp "$root/tools/stfs_extract.py" "$out/tools/"
cat > "$out/README.txt" <<'EOF'
Trials HD Recompiled - Linux x86-64

This folder contains no game data. You need your own copy of Trials HD (XBLA).

1. Extract your game package into ./assets next to this file:
     python3 tools/stfs_extract.py "/path/to/5841095A/000D0000/<package file>" assets
   You should get assets/default.xex and assets/data/*.pak.

2. Run ./trials_hd   (a Vulkan-capable GPU and an Xbox-style controller are needed)
   The WebGPU renderer is the default; for the Vulkan one: ./trials_hd --gpu_plugin=xenos

Graphics settings: press the controller's Guide button (or Back + Start, or F1).
All settings (trials_hd.toml), save data (5841095A/), achievements, and cache
are saved locally in this folder next to the executable.
EOF

if [[ " $* " == *" --link-assets "* ]]; then
  ln -s "$root/assets" "$out/assets"
fi

echo "packaged $preset -> $out"
ls -la "$out"
