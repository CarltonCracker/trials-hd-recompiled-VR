# The ReXGlue SDK and our patch

## Layout

- `thirdparty/rexglue-sdk`: git submodule, pinned to upstream v0.10.0 (`c94f5eb`). It has
  its own nested submodules (SDL3, FFmpeg, imgui, Vulkan headers, spdlog, …); clone with
  `--recursive`.
- `patches/rexglue-sdk/trials-hd.patch`: **all** our SDK changes as one diff (~86k lines,
  ~176 files; much of it is generated SPIR-V headers). The text before the first
  `diff --git` is a hand-written changelog of what the patch does; read it to learn
  what's patched.
- The submodule never gets our commits; it stays at the upstream pin plus the patch in
  its working tree (`.gitmodules` sets `ignore = dirty`).

## How the patch gets applied (`cmake/sdk_patches.cmake`)

Runs at every configure (included from `CMakeLists.txt`; also runnable with
`cmake -DTRIALSHD_ROOT=<repo> -P cmake/sdk_patches.cmake`). For each `patches/rexglue-sdk/*.patch`:

1. Already applied (`git apply --reverse --check` succeeds) → nothing to do.
2. Clean SDK tree → apply it.
3. Tree has changes → look for an **older committed version** of the patch file (git log)
   that the tree has exactly; reverse it and apply the current one ("Reversed SDK patch …
   as of <rev>"). This is what updates an existing clone after a pull.
4. Anything else → leave the tree alone ("SDK tree has local changes; not re-applying"):
   SDK work in progress.

A patch file change re-runs configure (`CMAKE_CONFIGURE_DEPENDS`).

## Changing the SDK

1. Check the tree matches the committed patch before you start:
   `git -C thirdparty/rexglue-sdk apply --reverse --check ../../patches/rexglue-sdk/trials-hd.patch`
   (no output = in sync). If it doesn't, don't export from it: make a separate
   `git worktree` of the SDK pin, apply the committed patch there, make your edits there,
   and export from it. Exporting from a stale tree silently drops other people's changes
   (this once lost ~3,600 lines of web-port changes).
2. Edit files under `thirdparty/rexglue-sdk/` directly.
3. Rebuild with `cmake --build <build> --target trials_hd_stage_libs` so the staged
   plugin/runtime libraries next to `trials_hd` are current.
4. Export: `tools/export_sdk_patch.sh` (dev branch) regenerates the diff below the header
   (it marks new files with `git add -N` so they're included). Add a bullet describing your
   change to the header by hand.
5. **Diff the new patch against the old one** (`git diff patches/`) and make sure only your
   change moved. Commit it with subject `sdk patch: …`.

Rules:

- Keep SDK changes small and generic where possible; game-specific renderer code belongs
  in `src/gpu_webgpu/`, game behavior in `src/` hooks.
- Trials fast paths in the Vulkan backend sit behind `vulkan_trials_fast_path` and keep a
  fallback (`renderer-vulkan.md`).
- Never export temporary instrumentation (e.g. `tools/profile/vk_gpu_prof.patch`) into
  `trials-hd.patch`.
- Don't refresh the SDK's XeSL resolve shader sources from Xenia HEAD (pinned to
  `0b2ffa31`). SPIR-V headers for shaders the patch adds are rebuilt with
  `tools/build_resolve_shaders.sh`.
- Web-only SDK code is behind `REX_PLATFORM_EMSCRIPTEN` / `__EMSCRIPTEN__` and must not
  change native builds.

## Where things are in the SDK

| Path (under `thirdparty/rexglue-sdk/`) | What |
| --- | --- |
| `src/codegen/`, `resources/templates/codegen/` | Recompiler and the templates of the generated code (e.g. `pch_h.inja`) |
| `include/rex/ppc/` | `PPCContext`, `REX_FUNC` |
| `src/ui/rex_app.cpp`, `include/rex/rex_app.h` | App base class and its hooks |
| `src/system/`, `src/kernel/` | Kernel/XAM HLE, memory (`xmemory.cpp`), threads |
| `src/graphics/` | Backend-agnostic GPU code (`command_processor.cpp`, shader translator, texture/RT models) |
| `src/graphics/vulkan/` | The Vulkan backend (`xenos` plugin) |
| `src/core/` | Platform layer (memory mapping, threading, FMA on the web) |
| `src/audio/`, `src/input/`, `src/ui/` | Audio (SDL, web), input (SDL, mnk, web), windowing/ImGui/overlays |
