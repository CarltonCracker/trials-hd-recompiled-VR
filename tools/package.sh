#!/usr/bin/env bash
# Assemble a runnable build folder from a finished CMake build.
#
# Usage: tools/package.sh [preset=linux-amd64-release] [out_dir] [--link-assets]
#
# The folder holds the executable, the SDK runtime libraries it loads from its
# own directory ($ORIGIN) and a short README. It never contains game data: on
# first start the game asks for the player's own package and installs it to
# <out_dir>/assets.
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
mkdir -p "$out"
cp "$build/trials_hd" "$out/"
cp "$build"/*.so "$out/"
if ls "$build/src/gpu_webgpu"/librexgpu-webgpu*.so >/dev/null 2>&1; then
  cp -u "$build/src/gpu_webgpu"/librexgpu-webgpu*.so "$out/"
fi
cat > "$out/README.txt" <<'EOF'
Trials HD Recompiled - Linux x86-64

This folder contains no game data. You need your own copy of Trials HD (XBLA).

1. Run ./trials_hd   (a Vulkan-capable GPU is needed; a controller is recommended)

2. On first start it asks for your Trials HD package: the file in
   <your content folder>/0000000000000000/5841095A/000D0000/, or a folder that
   contains it. The Big Pack and Big Thrills DLC and custom tracks (saved games in
   5841095A/00000001/, levels only) in the same folder are found too.
   It installs them into ./assets next to this file, then the game starts. Later
   starts go straight to the game.

   Add DLC or tracks later: settings (F1, Guide, or Back + Start) > Game > Add DLC or tracks...

   Without the setup screen:  ./trials_hd --install_game=/path/to/package-or-folder
                              ./trials_hd --install_dlc=/path/to/package-or-folder

The WebGPU renderer (development): ./trials_hd --gpu_plugin=webgpu

Graphics settings: press the controller's Guide button (or Back + Start, or F1).
All settings (trials_hd.toml), save data (5841095A/), achievements, and cache
are saved locally in this folder next to the executable.
EOF

if [[ " $* " == *" --link-assets "* ]]; then
  ln -s "$root/assets" "$out/assets"
fi

echo "packaged $preset -> $out"
ls -la "$out"
