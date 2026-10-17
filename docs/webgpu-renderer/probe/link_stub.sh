#!/usr/bin/env bash
# L-01 link experiment (section 7.6.2): link a stub GPU plugin from the SDK's
# backend-agnostic graphics objects only (no Vulkan, no SPIR-V) with
# --no-undefined, to confirm which SDK sources rexgpu-webgpu must compile.
# Reuses the object files of an existing relwithdebinfo rexgpu-xenos build.
# Usage: docs/webgpu-renderer/probe/link_stub.sh [out_dir]
set -euo pipefail

repo=$(cd "$(dirname "$0")/../../.." && pwd)
build=$repo/out/build/linux-amd64-relwithdebinfo
sdk_out=$repo/thirdparty/rexglue-sdk/out/linux-amd64
objs=$build/rexglue-sdk/src/graphics/CMakeFiles/rexgpu-xenos.dir
ninja=$(grep -m1 '^CMAKE_MAKE_PROGRAM' "$build/CMakeCache.txt" | cut -d= -f2)
out=${1:-$(mktemp -d)}
mkdir -p "$out"

# Compile flags of an SDK graphics source, without its precompiled header.
commands=$("$ninja" -C "$build" -t commands rexgpu-xenos)
compile=$(grep -m1 'src/graphics/command_processor.cpp' <<<"$commands" |
  sed -E 's/ -Xclang -include-pch -Xclang [^ ]+//; s/ -Xclang -include -Xclang [^ ]+//;
          s/ -MD -MT .*//; s/-Drexgpu_xenos_EXPORTS//; s/\\"/"/g')
(cd "$build" && eval "$compile -w -o '$out/link_stub_plugin.o' -c '$repo/docs/webgpu-renderer/probe/link_stub_plugin.cpp'")

# REXGPU_XENOS_SOURCES minus plugin_main, SPIR-V and Vulkan (section 10.3).
shared="flags register_file registers pipeline/shader/shader format/ucode sampler_info
  graphics_system command_processor util/draw_extent_estimator util/draw
  packet_disassembler primitive_processor pipeline/render_target/cache shared_memory
  pipeline/texture/cache pipeline/shader/interpreter pipeline/shader/translator
  pipeline/shader/translator_disasm"
files=()
for f in $shared; do files+=("$objs/$f.cpp.o"); done

tracy=()
if [[ "$compile" == *"-DTRACY_ENABLE"* ]]; then
  tracy+=("$sdk_out/libTracyClientrd.so")
fi
clang++ -fPIC -pthread -shared -Wl,--no-undefined -o "$out/librexgpu-stubrd.so" \
  "$out/link_stub_plugin.o" "${files[@]}" \
  "$sdk_out/librexruntimerd.so" "$sdk_out/libxxhashrd.a" "$sdk_out/libspdlogrd.a" \
  "$sdk_out/libfmtrd.a" "$sdk_out/libSDL3rd.a" -lm "${tracy[@]}" -ldl
echo "linked with no undefined symbols: $out/librexgpu-stubrd.so"
