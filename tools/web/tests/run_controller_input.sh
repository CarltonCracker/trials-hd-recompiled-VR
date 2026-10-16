#!/usr/bin/env bash
# Real web driver + shared-memory assertions, independent of generated game code.
set -euo pipefail
cd "$(dirname "$0")/../../.."
sdk=thirdparty/rexglue-sdk
mkdir -p out/web-spikes
em++ tools/web/tests/controller_input.cpp src/web/web_input.cpp \
  "$sdk/src/input/web/web_input_driver.cpp" "$sdk/src/ui/web/web_ui.cpp" --target=wasm64 -std=c++23 -O2 -msimd128 \
  -fno-strict-aliasing -pthread -sPROXY_TO_PTHREAD -sPTHREAD_POOL_SIZE=2 \
  -sENVIRONMENT=node -sEXIT_RUNTIME=1 -DSPDLOG_COMPILED_LIB -DSPDLOG_FMT_EXTERNAL \
  -I. -I"$sdk/include" -I"$sdk/thirdparty" -I"$sdk/thirdparty/simde" \
  -I"$sdk/thirdparty/fmt/include" -I"$sdk/thirdparty/spdlog/include" \
  -I"$sdk/thirdparty/tomlplusplus/include" -I"$sdk/thirdparty/sdl3/include" \
  -o out/web-spikes/controller_input.js
"${EMSDK_NODE:?Activate emsdk first}" out/web-spikes/controller_input.js
