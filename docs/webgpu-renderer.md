# WebGPU renderer — design

Goal: one renderer, written against the standard `webgpu.h` API, that runs

- **natively on Linux** through [Dawn](https://dawn.googlesource.com/dawn) (Vulkan
  underneath), and
- **in a browser** later, compiled with Emscripten's `emdawnwebgpu` port, which exposes
  the same header.

The current renderer (ReXGlue's emulated Xenos GPU on Vulkan) stays available as a
fallback, selectable in the settings, until the WebGPU renderer covers the whole game.

## Constraints that shape the design

- **WGSL only on the web.** Browsers accept WGSL shaders, not SPIR-V. The game's GPU
  shaders (Xenos microcode, loaded from the player's own game data) are translated at
  runtime: Xenos microcode → SPIR-V (ReXGlue's existing translator) → WGSL (Tint,
  Dawn's shader compiler, linked into the app so it also works in WebAssembly).
  Translated shaders are cached.
- **No game content in the repository.** Shaders are translated from the player's
  files at runtime; nothing derived from them is committed. (This is why hand-porting
  the game's shaders, as some native renderers do, is not an option here.)
- **WebGPU feature set.** No fragment-shader interlock, no geometry shaders, limited
  texture formats, no 64-bit atomics by default, strict resource binding limits.
  Anything that relies on those must be redesigned rather than ported.
- **Big-endian guest data.** Vertex/index buffers and textures in guest memory are
  big-endian, textures are tiled; conversion happens on upload (compute passes where
  possible).

## Shared foundation (needed by either architecture)

1. **Device and presentation** (`src/webgpu/`): instance, adapter, device, surface from
   the SDL window (X11 natively; canvas on the web), surface configuration driven by
   the graphics settings (VSync → present mode, FPS cap, window/fullscreen size).
2. **Shader pipeline**: microcode → SPIR-V → WGSL via Tint, with a persistent cache.
3. **Format and upload layer**: guest texture formats → WebGPU formats (with
   conversions where WebGPU has no equivalent), untiling, endian swaps.
4. **Integration**: ReXGlue *detached mode* — no `gpu_plugin`, the app owns the
   window surface and the present loop (`ReXApp::OnCreateImmediateDrawer`), and the
   ImGui overlays (settings menu) draw through the WebGPU device.

## Architecture options

### A. WebGPU backend for the emulated GPU

Add a WebGPU implementation of ReXGlue's GPU backend interfaces (command processor,
render-target cache, texture cache, pipeline cache, shared memory), alongside the
existing Vulkan and D3D12 ones. The game's command stream (PM4 packets) is interpreted
exactly as today.

- \+ Generic: everything the emulated renderer draws today draws on WebGPU, for this and
  any other ReXGlue game. Reaches full-game coverage soonest.
- \+ Mechanical work: port of an existing, working backend.
- − Keeps emulation overhead; no gains beyond what the emulated renderer offers.
- − The Vulkan backend's EDRAM emulation path that needs fragment-shader interlock has
  no WebGPU equivalent; only the "host render targets" path can be ported.

### B. Native (API-level) renderer

Hook the game's statically linked Direct3D functions (draws, state, resources,
resolve, present) and implement them directly on WebGPU, skipping the command stream
and EDRAM emulation entirely — the approach of other "native renderer" recomps.

- \+ Best performance; render targets become real textures at any resolution, which
  enables true high resolutions, aspect ratios, and frame interpolation for >60 FPS.
- − Requires reverse engineering every Direct3D function the game uses (no symbols),
  per game version. Long tail of rendering bugs until coverage is complete.

A and B share the whole foundation above; B can also be introduced incrementally on top
of A, replacing hot paths one at a time.

**Decision (2026-10-12): B, a full native renderer**, to allow high frame rates and high
resolutions. The emulated Vulkan renderer remains the fallback until B covers the game.

## Milestones

| | Milestone | Status |
| --- | --- | --- |
| M0 | Dawn builds in-tree; `webgpu_smoke` opens a window and presents | done (Dawn `901f57556`, Vulkan/X11) |
| M1 | Shader pipeline: microcode → SPIR-V → WGSL with cache, validated on the game's shaders | |
| M2 | Detached-mode integration: WebGPU owns presentation, overlays draw through it | |
| M3a | Direct3D survey: map the runtime's GPU-facing functions ([d3d-map.md](d3d-map.md)) | done: draw/texture/stream/resolve/present entry points, register shadow layout (draft) |
| M3b | Frame recorder: per-draw state from hooks + register shadow, validated against the command processor | next |
| M3+ | Native renderer (B): hooks, state/resource translation | |
| Web | Emscripten build of the renderer; see the web-port notes below | |

## Web port notes (outside the renderer)

The renderer is not the hard part of a browser build. The ReXGlue runtime reserves the
Xbox 360's 4 GB guest address space at a fixed host address and relies on page-fault
signals for memory-mapped I/O; WebAssembly offers neither (wasm32 cannot address 4 GB
plus host memory; wasm64/Memory64 is required). The game is heavily multithreaded
(SharedArrayBuffer + cross-origin isolation), and the generated code is ~110 MB of C++.
Game files must be provided by the player in the browser (file picker / OPFS).
