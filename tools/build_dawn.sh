#!/usr/bin/env bash
# Build Dawn (WebGPU) for the rexgpu-webgpu plugin and install it to
# out/dawn-install. The pin and options are explained in
# docs/webgpu-renderer/README.md, sections 7.6.1 and 10.2.
#
# Usage: tools/build_dawn.sh [commit]   (default: the pinned commit below)
# Env:   DAWN_BUILD_TYPE (default Release), DAWN_JOBS (default: all cores)
#
# Then configure the game with:
#   -DTRIALSHD_WEBGPU=ON -DCMAKE_PREFIX_PATH=$PWD/out/dawn-install
set -euo pipefail

DAWN_PIN=088a8d00efd1c70b58c549d355401d115904b648  # chromium/8104, 2026-10-09
commit=${1:-$DAWN_PIN}

repo=$(cd "$(dirname "$0")/.." && pwd)
src=$repo/thirdparty/dawn
build=$repo/out/dawn-build
install=$repo/out/dawn-install
build_type=${DAWN_BUILD_TYPE:-Release}
if [ "$(uname)" = Darwin ]; then
  jobs=${DAWN_JOBS:-$(sysctl -n hw.ncpu)}
  platform_opts=(-DDAWN_ENABLE_METAL=ON -DDAWN_ENABLE_VULKAN=OFF)
else
  jobs=${DAWN_JOBS:-$(nproc)}
  platform_opts=(-DDAWN_ENABLE_VULKAN=ON -DDAWN_USE_X11=ON -DDAWN_USE_WAYLAND=ON)
fi

# Use the repo's ninja when the system has none (the game build does the same).
if ! command -v ninja >/dev/null && [ -x "$repo/.venv/bin/ninja" ]; then
  export PATH=$repo/.venv/bin:$PATH
fi

if [ ! -d "$src/.git" ]; then
  git init -q "$src"
  git -C "$src" remote add origin https://dawn.googlesource.com/dawn
fi
if [ "$(git -C "$src" rev-parse -q --verify HEAD 2>/dev/null)" != "$commit" ]; then
  echo "Fetching Dawn $commit"
  git -C "$src" fetch -q --depth 1 origin "$commit"
  git -C "$src" checkout -q --detach FETCH_HEAD
fi
echo "Dawn at $(git -C "$src" log -1 --format='%h %cs %s')"

# DAWN_FETCH_DEPENDENCIES clones third_party/ at configure time (no depot_tools).
# PIC: Dawn is linked statically into a shared library (the GPU plugin).
# Same compiler and standard library as the game build, to keep the C++ ABI.
cmake -S "$src" -B "$build" -G Ninja \
  -DCMAKE_BUILD_TYPE="$build_type" \
  -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
  -DDAWN_FETCH_DEPENDENCIES=ON -DDAWN_ENABLE_INSTALL=ON \
  -DDAWN_BUILD_SAMPLES=OFF -DDAWN_BUILD_TESTS=OFF -DTINT_BUILD_TESTS=OFF \
  -DDAWN_BUILD_PROTOBUF=OFF -DDAWN_USE_GLFW=OFF \
  -DDAWN_ENABLE_D3D11=OFF -DDAWN_ENABLE_D3D12=OFF -DDAWN_ENABLE_NULL=OFF \
  -DDAWN_ENABLE_DESKTOP_GL=OFF -DDAWN_ENABLE_OPENGLES=OFF \
  -DTINT_BUILD_SPV_READER=ON -DTINT_BUILD_CMD_TOOLS=ON \
  "${platform_opts[@]}"

cmake --build "$build" -j "$jobs"
cmake --install "$build" --prefix "$install"
echo "Dawn installed to $install"
