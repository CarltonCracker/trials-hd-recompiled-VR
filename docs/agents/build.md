# Building and running

## `build.sh` (the normal way)

Detects the OS/arch, checks or extracts `assets/`, configures the matching preset and
builds `trials_hd`.

| Option | Effect |
| --- | --- |
| `--game, -g PATH` | Extract the base-game folder/STFS package into `assets/` first (once) |
| `--force-extract` | Re-extract even if `assets/` verifies |
| `--release` / `--debug` / `--relwithdebinfo` | Configuration (default RelWithDebInfo) |
| `-j N`, `--jobs N` | Parallel jobs (default: all cores) |
| `--clean` / `--clean-all` | Remove the preset's build dir / all of `out/build` |
| `--no-webgpu` | `-DTRIALSHD_WEBGPU=OFF`: Vulkan only, no Dawn |
| `--target T` | Build another target (no codegen handoff) |
| `--web` | Browser build, preset `web-wasm32-release`; needs `~/emsdk` (or `$EMSDK`) and a native build first |
| `-D...` | Passed to CMake configure (forces a reconfigure) |

Assets verify when `assets/default.xex`, `assets/data/data.pak` and the other expected files
exist. Without them, `build.sh` asks for `--game`.

**First build:** on a fresh tree the configure writes `trials_hd_codegen_ready.stamp` in
the build dir (no generated sources yet). `build.sh` then builds `trials_hd_codegen`,
reconfigures with `cmake -S . -B <dir>` (keeps the cache and any `-D` flags) and removes the
stamp before building `trials_hd`. This applies on every native platform. Covered by
`tests/build_script_test.py` (mocked tools; also runs under macOS's Bash 3.2).
With plain CMake, run `cmake --build` twice instead.

## `run.sh`

`./run.sh [--release|--debug|--relwithdebinfo] [--bin PATH] [game args…]` runs
`out/build/<preset>/trials_hd` with `assets/`. `--web [--port N] [--no-assets]` serves the
browser build with `tools/web/serve.py` (default `127.0.0.1:8091`); `--name=value` args become
page flags.

## Presets (`CMakePresets.json`)

`linux-amd64-{debug,release,relwithdebinfo}`, `mac-arm64-*`, `mac-amd64-*`, all with
`binaryDir = out/build/<preset>` and `REXSDK_DIR = thirdparty/rexglue-sdk`. Web:
`web-wasm32-release` (default for `--web`), `web-wasm64-release`, `web-wasm64-node`
(runs under Node with host files).

## CMake options

| Option | Default | Meaning |
| --- | --- | --- |
| `TRIALSHD_WEBGPU` | ON | Build `rexgpu-webgpu`, `trials_hd_webgpu`, `webgpu_smoke`; needs Dawn |
| `TRIALSHD_WEB` | OFF | Browser build; only under `emcmake`, then `cmake/web/trials_hd_web.cmake` takes over |
| `TRIALSHD_BUILD_TESTING` | ON | Project tests (only if `tests/` exists, i.e. on `dev`) |
| `TRIALSHD_CALL_TRACE` (+ `TRIALSHD_CALL_TRACE_RANGE`) | OFF | Per-function call counters (`game-hooks.md`); needs generated code |
| `REXGLUE_ENABLE_TRACY`, `REXGLUE_ENABLE_PERF_COUNTERS` | OFF in optimized single-config builds | Profiling; re-enable explicitly (a plain preset configure turns them off again) |
| `REXGLUE_RECOMP_DEBUG_INFO` | `line-tables-only` | Debug info for generated code (`full`, `none`) |

## Targets

| Target | What |
| --- | --- |
| `trials_hd` | The game (default) |
| `trials_hd_codegen` | Run codegen only |
| `trials_hd_stage_libs` | Build `rexruntime` and the GPU plugins and copy them next to `trials_hd`. **Use this after SDK/renderer changes**; building `rexgpu-xenos` alone leaves the old copy in place |
| `webgpu_smoke` | Standalone WebGPU check: window, surface, limits, CPU benchmark |
| `stfs_extract` | Native STFS extractor CLI (`EXCLUDE_FROM_ALL`) |
| `xex_dump` | Dumps the loaded XEX image for reverse engineering (dev; `EXCLUDE_FROM_ALL`) |
| `xenos2wgsl` | Offline shader translator check (dev; built when `tools/xenos2wgsl` exists) |

## Dawn

The first configure with `TRIALSHD_WEBGPU=ON` and no Dawn found runs
`tools/build_dawn.sh`: it clones Dawn into `thirdparty/dawn` at a pinned commit, builds it in
`out/dawn-build` and installs it to `out/dawn-install` (long; downloads). Dawn's exported
targets need `find_package(Threads)` first; the target is `dawn::webgpu_dawn`.

## Outputs and packaging

- `out/build/<preset>/trials_hd` plus `librexruntime*`, `librexgpu-xenos*`,
  `librexgpu-webgpu*` (RelWithDebInfo libs carry an `rd` suffix).
- At runtime the binary's folder also receives `trials_hd.toml`, saves (`5841095A/`),
  `achievements/`, `cache/`, `logs/` (`game-data.md`).
- `tools/package.sh [preset] [out]` assembles a runnable folder (default
  `dist/trials_hd-linux-x86_64`, from `linux-amd64-release`) with `stfs_extract.py` and a
  README; no game files.

## macOS notes

- Vulkan through MoltenVK (staged by the SDK); `OnPreSetup` sets `MVK_CONFIG_*` env vars.
- `present_fps_limit` defaults to 60 on macOS (fanless thermal throttling).
- `build.sh` must work with the stock Bash 3.2: no empty-array expansion under `set -u`
  (see the `CMAKE_EXTRA_ARGS` handling).
- The call-graph hooks use weak tail-call thunks on Mach-O instead of aliases
  (`architecture.md`).

## Deep reference (dev branch)

`docs/webgpu-renderer/README.md` §10 (Dawn pin, CMake wiring, macOS),
`docs/web-port/README.md` §10 (the two-stage web build).
