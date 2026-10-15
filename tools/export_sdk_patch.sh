#!/usr/bin/env bash
# Regenerate patches/rexglue-sdk/trials-hd.patch from the working tree of the
# thirdparty/rexglue-sdk submodule (all local changes vs. the pinned commit).
# CMake applies this patch to a clean SDK checkout at configure time.
set -euo pipefail

root="$(cd "$(dirname "$0")/.." && pwd)"
sdk="$root/thirdparty/rexglue-sdk"
out="$root/patches/rexglue-sdk/trials-hd.patch"

# Include new files in the diff without staging their contents.
git -C "$sdk" ls-files --others --exclude-standard -z | xargs -0 -r git -C "$sdk" add -N --

{
  cat <<'EOF'
Trials HD port: changes to the ReXGlue SDK (applied to the pinned submodule
commit by CMake at configure time; regenerate with tools/export_sdk_patch.sh).

- input/mnk: synthesize XamInputGetKeystroke events from bound keys (the
  game's menus read keystrokes)
- vulkan command processor: host presentation FPS cap (present_fps_limit),
  bypassed when the game loop already paces real frames (present_fps_limit_guest_paced),
  guest FPS / draws / resolves per frame logging (log_guest_fps)
- core: unlink the guest memory shm right after creating it (no leaks on crash)
- vulkan presenter: VK_EXT_headless_surface (vulkan_headless_surface)
- graphics: per-occlusion-query and per-frame present logs moved to trace
- resolve shaders: pack 16_16 / 16_16_16_16 destinations in the copy's number
  format (signed integer depth for shadow maps) - sources in
  src/graphics/shaders/xesl, rebuilt with tools/build_resolve_shaders.sh
- SPIR-V texture fetch: result exponent bias from fetch constant word 3 (was
  word 4, the LOD bias), integer number format for 16-bit textures
- resolve readback: readback_resolve_max_bytes (read back only small resolves)
- render targets: elide_rt_transfers (ownership transfers of resolve-cleared
  ranges become clears; transfers fully overwritten by Direct3D 9 clear
  rectangles are dropped), execute_unclipped_draw_vs_on_cpu on by default
- input: serialize InputSystem (device list rebuilt concurrently by several
  guest threads - heap corruption)
- graphics: lock the command processor's CallInThread queue (pushed from
  other threads); logging: reserve the category registry (read lock-free)
- macOS: user data under Application Support and Apple Silicon presentation defaults
- ui/overlay: achievements close callback and input gating
- command processor CPU: sequential register writes (type-0 packets) go
  through the range path, context registers (0x2000-0x3FFF) are bulk-copied,
  the unknown-register debug check is a bit test instead of a big switch, the
  memexport readback cvar lookup only runs for memexport draws, and
  SharedMemory::RequestRanges reuses its scratch vector (720p uncapped on an
  RX 6600: ~145 -> ~183 FPS, now GPU-bound)
- vulkan: skip draws with no observable effect (no pixel shader output,
  depth, stencil, memexport or occlusion query: ~170 per frame in Trials HD),
  counted in the log_guest_fps line
- command processor idle: shorter spin before waiting for new commands, and
  WAIT_REG_MEM naps after a short spin instead of yielding for the whole wait
- threading: event-driven WaitMultiple (sleeps until a condition changes
  instead of polling every millisecond and spinning near each timeout)
- ui: dialogs say whether they need continuous repaint; the always-present
  achievement toast only does while showing one (the window was repainted
  ~1400 times per second with VSync off)
- web (Emscripten) memory model (docs/web-port/README.md 7.9.2-7.9.3, W-L04):
  one guest window with the aliases folded per heap, page protection as
  bookkeeping, dirty-bitmap write tracking delivered to the invalidation
  callbacks (memory_emscripten.cpp, exception_handler_emscripten.cpp and
  REX_PLATFORM_EMSCRIPTEN / REX_ARCH_WASM branches; native code unchanged)

EOF
  git -C "$sdk" diff
} > "$out"
echo "wrote $out ($(grep -c '^diff --git' "$out") files)"
