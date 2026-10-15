#!/usr/bin/env bash
# W-L04: the SDK's guest memory with the web memory model, wasm64 under Node.
# Activate emsdk first. Builds only SDK/test sources; no game content.
set -euo pipefail
root="$(cd "$(dirname "$0")/../../.." && pwd)"
cd "$root"
sdk=thirdparty/rexglue-sdk
out=out/web-spikes/memory_model
mkdir -p "$out/obj"
flags=(--target=wasm64 -std=gnu++23 -O2 -msimd128 -pthread -fwasm-exceptions
  -DSPDLOG_COMPILED_LIB -DSPDLOG_FMT_EXTERNAL -I"$sdk/include"
  -isystem "$sdk/thirdparty/simde" -isystem "$sdk/thirdparty/fmt/include"
  -isystem "$sdk/thirdparty/spdlog/include" -isystem "$sdk/thirdparty/tomlplusplus/include"
  -isystem "$sdk/thirdparty/cli11/include")
sources=(
  "$sdk/src/system/xmemory.cpp" "$sdk/src/system/mmio_handler.cpp"
  "$sdk/src/core/memory_emscripten.cpp" "$sdk/src/core/exception_handler_emscripten.cpp"
  "$sdk/src/core/cvar.cpp" "$sdk/src/core/logging.cpp" "$sdk/src/core/mutex.cpp"
  "$sdk/src/core/math_gcc.cpp"
  "$sdk/thirdparty/fmt/src/format.cc" "$sdk/thirdparty/fmt/src/os.cc"
  "$sdk/thirdparty/spdlog/src/spdlog.cpp" "$sdk/thirdparty/spdlog/src/stdout_sinks.cpp"
  "$sdk/thirdparty/spdlog/src/color_sinks.cpp" "$sdk/thirdparty/spdlog/src/file_sinks.cpp"
  "$sdk/thirdparty/spdlog/src/async.cpp" "$sdk/thirdparty/spdlog/src/cfg.cpp"
  tools/web/tests/memory_model.cpp tools/web/tests/memory_model_stubs.cpp
)
objects=()
pids=()
for source in "${sources[@]}"; do
  object="$out/obj/$(echo "${source#"$sdk/"}" | tr / _).o"
  objects+=("$object")
  em++ "${flags[@]}" -c "$source" -o "$object" & pids+=($!)
done
for pid in "${pids[@]}"; do wait "$pid"; done
# The guest window (2.75 GiB) comes from sbrk past the initial heap.
em++ --target=wasm64 -O2 -pthread -fwasm-exceptions -sPROXY_TO_PTHREAD -sPTHREAD_POOL_SIZE=8 \
  -sINITIAL_MEMORY=3328mb -sMAXIMUM_MEMORY=8gb -sALLOW_MEMORY_GROWTH=1 \
  -sENVIRONMENT=node -sEXIT_RUNTIME=1 "${objects[@]}" -o "$out/memory_model.js"
"${EMSDK_NODE:?Activate emsdk first}" "$out/memory_model.js"
