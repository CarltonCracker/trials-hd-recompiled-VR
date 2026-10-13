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

- input/mnk: synthesize XamInputGetKeystroke events from bound keys
- vulkan command processor: host presentation FPS cap (present_fps_limit),
  bypassed when the game loop already paces real frames (present_fps_limit_guest_paced),
  guest FPS / draws / resolves per frame logging (log_guest_fps),
  render target / resolve format logging (log_render_formats)
- core: unlink the guest memory shm right after creating it (no leaks on crash)
- vulkan presenter: VK_EXT_headless_surface (vulkan_headless_surface)
- graphics: per-occlusion-query and per-frame present logs moved to trace
- resolve shaders: pack 16_16 / 16_16_16_16 destinations in the copy's number
  format (signed integer depth for shadow maps) - sources in
  src/graphics/shaders/xesl, rebuilt with tools/build_resolve_shaders.sh
- SPIR-V texture fetch: result exponent bias from fetch constant word 3 (was
  word 4, the LOD bias), integer number format for 16-bit textures
- resolve readback: readback_resolve_max_bytes (read back only small resolves)
- diagnostics: dump_resolves_frame / dump_resolves_dir (resolve dumps for
  tools/decode_resolve.py), gpu_profile (GPU time per frame by kind of work,
  Vulkan timestamps, per render pass and per load / resolve / transfer),
  log_rt_transfers, log_cp_waits, log_texture_loads
- render targets: elide_rt_transfers (ownership transfers of resolve-cleared
  ranges become clears; transfers fully overwritten by Direct3D 9 clear
  rectangles are dropped), execute_unclipped_draw_vs_on_cpu on by default
- input: serialize InputSystem (device list rebuilt concurrently by several
  guest threads - heap corruption)
- graphics: lock the command processor's CallInThread queue (pushed from
  other threads); logging: reserve the category registry (read lock-free)
- build: REXGLUE_OUTPUT_DIR; guest memory mapped above AddressSanitizer's
  shadow in ASan builds

EOF
  git -C "$sdk" diff
} > "$out"
echo "wrote $out ($(grep -c '^diff --git' "$out") files)"
