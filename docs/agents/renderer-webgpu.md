# WebGPU renderer (`src/gpu_webgpu/`)

Our own Trials-specific renderer, plugin `rexgpu-webgpu` (`librexgpu-webgpu`). Natively it
runs on Dawn (Vulkan on Linux, Metal on macOS) with `--gpu_plugin=webgpu` and is kept to
develop the web port; in the browser it is the only renderer (same source, emdawnwebgpu).

## Design in brief

- **Native resource model, no EDRAM emulation:** real host render targets at any
  resolution, real textures, resolves as GPU copies.
- **The game's own Xenos shaders are translated to WGSL at runtime**, so no game content
  enters the repo (no hand-ported shaders).
- **Front-end / back-end split:** the SDK's PM4 `CommandProcessor` decodes the ring
  buffer; `WebGpuCommandProcessor` turns draws, resolves and swaps into snapshots
  (`draw_snapshot.h`); `WebGpuBackend` consumes only those. A future front-end could hook
  the game's Direct3D runtime and produce the same snapshots.
- **Separate plugin, hidden visibility:** it compiles its own copy of the SDK's
  backend-agnostic GPU sources (`REXGPU_WEBGPU_SDK_SOURCES` in `src/gpu_webgpu/CMakeLists.txt`,
  mirrored from the SDK's `REXGPU_XENOS_SOURCES`; keep them in sync) and links only
  `rexruntime`, so it shares no global state with `librexgpu-xenos`.
- **Threads:** the command-processor thread records all GPU work; the presenter paints on
  the UI thread. The device is created **without** Dawn's implicit synchronization: hold
  `WebGpuProvider::device_mutex()` for every WebGPU call. Guest threads never call WebGPU.

## Files

| File | Role |
| --- | --- |
| `plugin_main.cpp` | Plugin entry points (`rex_gpu_abi_version`, `rex_gpu_create`) |
| `webgpu_graphics_system.*` | `GraphicsSystem`: provider + command processor (MMIO, vsync, ring from the SDK) |
| `webgpu_provider.*` | Dawn instance/adapter/device, `device_mutex()`, features/limits |
| `webgpu_command_processor.*` | PM4 front-end → snapshots; tracks state changes between draws |
| `draw_snapshot.h` | The front-end/back-end contract |
| `webgpu_backend.*` | Draw/resolve/swap execution (the biggest file) |
| `frame_recorder.*` | Command encoder, lazy render passes, per-draw uniform ring |
| `guest_memory_mirror.*` | Guest memory on the GPU using the SDK's page watches |
| `pipeline_cache.*`, `pipeline_state.*` | Shader/pipeline caches, fixed bind-group layouts, blend/depth/stencil/cull translation |
| `primitive_converter.*` | Fans/loops/quads → lists (CPU), rect lists in the VS, big-endian indices swapped in the VS |
| `render_target_cache.*` | Host RTs keyed by EDRAM location/pitch/format/samples; converts when the game reinterprets tiles |
| `resolve_engine.*` | EDRAM resolves → GPU copies into textures (resolve aliases); small ones also to guest memory (auto exposure) |
| `texture_cache.*`, `sampler_cache.*` | CPU-untiled texture loads + page-watch invalidation; samplers from fetch constants |
| `format_tables.*` | Xenos → WebGPU formats with feature fallbacks |
| `webgpu_presenter.*`, `webgpu_immediate_drawer.*` | Surface, letterbox, gamma, guest output capture; ImGui drawing |
| `gpu_profiler.*` | Timestamp profiler per pass category (`webgpu_profile`) |
| `wgsl/` | Translator: `wgsl_shader_translator.*`, `wgsl_alu.cpp`, `wgsl_fetch.cpp`, `wgsl_prelude.wgsl` (shared helpers) |
| `shaders/*.wgsl` | Our own WGSL (texture loads, resolve, transfer, gamma, blit, index swap, immediate) embedded as headers by `embed_wgsl.cmake` |
| `x11_display.*` | Xlib connection for Linux window surfaces |
| `web_phase.h` | Web stall-watchdog phase markers (no-op natively) |

Translator semantics follow the SDK's `ShaderInterpreter` (ALU) and
`SpirvShaderTranslator` (fetch, outputs). When unsure how Xenos behaves, read those.

## Cvars (`webgpu_*`)

`webgpu_adapter` (index, -1 = default; listed in the log), `webgpu_headless` (no
presentation, for tests), `webgpu_validation` / `webgpu_backend_validation` (Dawn /
Vulkan-Metal layers), `webgpu_debug_labels`, `webgpu_profile`, `webgpu_untextured`
(textures read white), `webgpu_dump_shaders=<dir>` (write every WGSL module; output is
derived from game shaders, so keep it in `out/`), `webgpu_trace_frame=N`,
`webgpu_dump_frame` / `webgpu_dump_count` / `webgpu_dump_file` (per-draw records for
`tools/diff_draws.py`), `webgpu_allow_present_mode_{immediate,mailbox}`.

## Testing

- `tests/gpu_webgpu/`: synthetic GPU tests against the SDK interpreter (ALU, vertex
  fetch, outputs, formats, texture loads, resolves, transfers, gamma, samplers, pipeline
  state, prelude, shared memory). Separate CMake project, needs Dawn, no game files; see
  `tests/gpu_webgpu/README.md`. Run each test with an adapter index argument.
- `tools/xenos2wgsl/`: offline translator + `validate.sh` (Tint validation of a folder).
- `webgpu_smoke`: window/surface/limits check and a 1,500-draw CPU benchmark.
- Parity with Vulkan: `tools/run_golden_ci.py` + `tests/headless/golden.toml`;
  `tools/diff_draws.py` compares per-draw dumps; fallback check:
  `TRIALSHD_FORCE_WEBGPU_FAILURE=1` must land on `xenos`.

## Rules

- Value-initialize every `wgpu::` descriptor (`wgpu::RenderPassDescriptor desc{};`):
  emdawnwebgpu leaves some counts uninitialized (this hung the browser at track select).
- Dawn-native-only APIs go behind `#if !defined(__EMSCRIPTEN__)`.
- Don't grow the SDK patch with renderer code; touch the SDK only for small hooks.
- Never commit dumped shaders, WGSL from game shaders, draw dumps or captures.

## Deep reference (dev branch)

`docs/webgpu-renderer/README.md`: §5 the measured Trials workload (the contract for both
renderers), §7.6 integration design (threads, bindings, snapshot rules), §8 subsystem by
subsystem, §9 format mapping, §11 testing, §12 milestones, §16 task list (claim tasks
there). Its intro still says no code exists; that is outdated. M0–M5 are done;
listed open: M2 (offline translation of all shaders), M6 (memexport/queries), M7
performance, M8 settings/fallback/packaging (the renderer choice and the Vulkan fallback
already exist in code), M9 macOS. Check the code before trusting a status.
