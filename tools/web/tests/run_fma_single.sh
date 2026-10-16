#!/usr/bin/env bash
# W-L16: bit-exact native and wasm64 checks; activate emsdk first.
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$root"
mkdir -p out/web-spikes
"${CXX:-c++}" tools/web/tests/fma_single.cpp -std=c++23 -O2 -ffp-contract=off \
  -Ithirdparty/rexglue-sdk/include -o out/web-spikes/fma_single_native
out/web-spikes/fma_single_native
em++ tools/web/tests/fma_single.cpp --target=wasm64 -std=c++23 -O2 -ffp-contract=off \
  -sENVIRONMENT=node -sEXIT_RUNTIME=1 -Ithirdparty/rexglue-sdk/include \
  -o out/web-spikes/fma_single.js
"${EMSDK_NODE:?Activate emsdk first}" out/web-spikes/fma_single.js
