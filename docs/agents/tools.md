# Tools catalog

Each script's header comment has its full usage. **(main)** marks tools that are also on
`main` (needed to build, run or package the game); everything else is dev-only.

## Build, data, packaging

| Tool | What it does |
| --- | --- |
| `build.sh`, `run.sh`, `add_dlc.sh` (repo root) **(main)** | Build, run, install DLC (`build.md`, `game-data.md`) |
| `tools/stfs_extract.py` **(main)** | Extract an STFS package; writes `package_license.toml` for the base game |
| `tools/stfs_extract/` **(main)** | The same extractor in C++ (`stfs_extract` target, web build) + `test_stfs_extract.py` (dev) |
| `tools/add_dlc.py` **(main)** | Install DLC packages/folders into `assets/content/…` |
| `tools/package.sh [preset] [out]` **(main)** | Runnable release folder in `dist/` (no game files) |
| `tools/build_dawn.sh [commit]` **(main)** | Build pinned Dawn into `out/dawn-install` (run by the first configure) |
| `tools/gen_trace_shims.py` **(main)** | Call-counting wrappers for `-DTRIALSHD_CALL_TRACE=ON` |
| `tools/webgpu_smoke/` **(main)** | WebGPU window/surface/limits check and CPU benchmark |
| `tools/web/serve.py` **(main)** | Localhost server for the web build (COOP/COEP, `--assets`) |
| `tools/build.sh` | Old copy of `./build.sh` |
| `tools/export_sdk_patch.sh` | Regenerate `patches/rexglue-sdk/trials-hd.patch` from the SDK tree (`sdk-patches.md`) |
| `tools/build_resolve_shaders.sh` | Rebuild SPIR-V headers of shaders the SDK patch adds |

## Headless runs and images

| Tool | What it does |
| --- | --- |
| `tools/headless_run.sh [s] [args]` | Run the game under Xvfb with `INPUT=` script; output in `out/run/` |
| `tools/xsend.py` | Plays a `tests/headless/*.txt` script (X11 XTEST keys + virtual pad FIFO) |
| `tools/xwd_to_png.py` | Xvfb framebuffer (XWD) → PNG |
| `tools/compare_captures.py` | PSNR/SSIM between captures |
| `tools/run_golden_ci.py` | Vulkan vs WebGPU golden-scene parity (`tests/headless/golden.toml`) |
| `tools/check_native.sh baseline\|check` | Native-build guard for web-port changes |

## Performance

| Tool | What it does |
| --- | --- |
| `tools/bench_vulkan.sh <out> <w> <h> [args]` | Headless, muted benchmark ride (weston); `PERF=`, `CAPTURE=` |
| `tools/bench_summary.py`, `tools/bench_window.py` | Summarize a benchmark run / stable riding-window FPS |
| `tools/vk_claim.sh` | Claim tasks on the Vulkan task board |
| `tools/profile/thread_sample.sh`, `stack_top.py` | Per-thread CPU + `eu-stack` samples and their summary |
| `tools/profile/sample.sh`, `gdb_sampler.py`, `summarize.py` | gdb-based sampling profiler (when `perf` is unavailable) |
| `tools/profile/thread_cpu.py` | Per-thread CPU from `/proc` over an interval |
| `tools/profile/vk_gpu_prof.patch` | Temporary Vulkan timestamp profiler (apply, measure, reverse; never export) |

## Reverse engineering and GPU debugging

| Tool | What it does |
| --- | --- |
| `tools/xex_dump/` | `xex_dump` target: dump the loaded image + sections to `out/` |
| `tools/disasm.py <dump> <addr>` | Disassemble around an address (capstone) |
| `tools/find_missing_funcs.py <dump> generated/default --toml` | Function starts codegen missed → `config/functions.toml` |
| `tools/xsh_dump.py` | Parse the Xenos shader cache (`.xsh`) |
| `tools/xenos2wgsl/` | Offline Xenos → WGSL translation; `validate.sh` runs Tint on a folder |
| `tools/diff_draws.py` | Compare per-draw dumps (Vulkan probe vs `webgpu_dump_*`) |
| `docs/webgpu-renderer/probe/` | gdb draw/transfer probes for the Vulkan renderer, link-stub check for `src/gpu_webgpu` source lists |

## Web port (`tools/web/`)

| Tool | What it does |
| --- | --- |
| `cdp.py` | Drive headless Chromium: scripts, console capture, screenshots, `--eval`, profiles |
| `bidi.py` | Same idea for Firefox (WebDriver BiDi; deferred) |
| `build.sh`, `compile_generated.py` | Manual web build / compile only the generated code |
| `run_node.sh` | Run the `web-wasm64-node` build under Node |
| `test_serve.py` | Tests for `serve.py` |
| `perf/` | `profile_threads.py` (V8 CPU profiles per thread), `trace_v8.py`, `worker_stacks.py` |
| `tests/` | Browser input/storage/settings tests, JS unit tests, FMA/memory-model probes |
| `spikes/` | Early feasibility pages and programs (feature detection, threads, triangle) |
