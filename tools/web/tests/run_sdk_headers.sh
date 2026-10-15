#!/usr/bin/env bash
# Standalone wasm64 assertions; activate emsdk first. No generated game code used.
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$root"
sdk=thirdparty/rexglue-sdk
mkdir -p out/web-spikes
em++ tools/web/tests/sdk_headers.cpp --target=wasm64 -std=c++23 -O2 -msimd128 \
  -pthread -sPROXY_TO_PTHREAD -sPTHREAD_POOL_SIZE=2 -sENVIRONMENT=node -sEXIT_RUNTIME=1 \
  -DSPDLOG_COMPILED_LIB -DSPDLOG_FMT_EXTERNAL -I"$sdk/include" \
  -I"$sdk/thirdparty/simde" -I"$sdk/thirdparty/fmt/include" \
  -I"$sdk/thirdparty/spdlog/include" -I"$sdk/thirdparty/tomlplusplus/include" \
  -o out/web-spikes/sdk_headers.js
"${EMSDK_NODE:?Activate emsdk first}" out/web-spikes/sdk_headers.js
