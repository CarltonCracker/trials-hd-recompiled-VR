#!/usr/bin/env bash
# Local build only: the output embeds the user's game code. Never publish it.
set -euo pipefail
cd "$(dirname "$0")/../.."
if ! command -v emcmake >/dev/null; then
  sdk="${EMSDK:-$HOME/emsdk}"
  if [[ ! -f "$sdk/emsdk_env.sh" ]]; then
    echo 'Install/activate Emscripten first (expected ~/emsdk/emsdk_env.sh).' >&2
    exit 1
  fi
  source "$sdk/emsdk_env.sh"
fi
if ! command -v ninja >/dev/null && [[ -x .venv/bin/ninja ]]; then
  export PATH="$PWD/.venv/bin:$PATH"
fi
emcmake cmake --preset web-wasm64-release
cmake --build out/build/web-wasm64-release -j"${CMAKE_BUILD_PARALLEL_LEVEL:-8}"
printf '\nServe locally: python3 tools/web/serve.py out/build/web-wasm64-release/web\n'
