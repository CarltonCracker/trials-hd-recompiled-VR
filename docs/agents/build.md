# Building and running

## `build.sh` (the normal way)

Detects the OS/arch, makes sure `out/xex/default.xex` (the codegen input) exists,
configures the matching preset and builds `trials_hd`. It does not install the game data:
the game's first start does (`game-data.md`).

| Option | Effect |
| --- | --- |
| `--game, -g PATH` | Take `default.xex` out of the base-game folder/STFS package (first build) |
| `--force-extract` | Take it out again even if `out/xex/default.xex` exists |
| `--release` / `--debug` / `--relwithdebinfo` | Configuration (default RelWithDebInfo) |
| `-j N`, `--jobs N` | Parallel jobs (default: all cores) |
| `--clean` / `--clean-all` | Remove the preset's build dir / all of `out/build` |
| `--no-webgpu` | `-DTRIALSHD_WEBGPU=OFF`: Vulkan only, no Dawn |
| `--target T` | Build another target (no codegen handoff) |
| `--web` | Browser build, preset `web-wasm32-release`; needs `~/emsdk` (or `$EMSDK`) and a native build first |
| `-D...` | Passed to CMake configure (forces a reconfigure) |

**The xex:** `trials_hd_manifest.toml` points codegen at `out/xex/default.xex`. If it is
missing, `build.sh` copies `assets/default.xex` (trees from before the setup screen) or,
with `--game`, finds the package (inline Python `locate_package`), configures, builds the
small `stfs_extract` target and runs `stfs_extract <package> out/xex --only default.xex`.
After a new xex it reconfigures: `CMakeLists.txt` records the xex's SHA-1 at configure
time (`file(SHA1)` → `build_info/trials_hd_build_info.h`, `TRIALSHD_BUILD_XEX_SHA1`), and
the setup screen only installs a package with that `default.xex`. Without `--game` and no
xex, `build.sh` stops and asks for it.

**First build:** on a fresh tree the configure writes `trials_hd_codegen_ready.stamp` in
the build dir (no generated sources yet). `build.sh` then builds `trials_hd_codegen`,
reconfigures with `cmake -S . -B <dir>` (keeps the cache and any `-D` flags) and removes the
stamp before building `trials_hd`. This applies on every native platform. Covered by
`tests/build_script_test.py` (mocked tools; also runs under macOS's Bash 3.2).
With plain CMake, run `cmake --build` twice instead.

## `run.sh`

`./run.sh [--release|--debug|--relwithdebinfo] [--bin PATH] [game args…]` runs
`out/build/<preset>/trials_hd` with `--game_data_root=<repo>/assets` (unless given), so the
setup screen installs there rather than into the build dir (`--clean` would delete it). `--web [--port N] [--no-assets]` serves the
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
| `stfs_extract` | The installer's STFS reader as a CLI (`EXCLUDE_FROM_ALL`; `build.sh` uses `--only default.xex`) |
| `installer_cli` | Test driver for the installer (`tests/installer/`, dev) |
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
  `dist/trials_hd-linux-x86_64`, from `linux-amd64-release`) with a README; no game files.
  Its first start shows the setup screen and installs to `<folder>/assets`. The folder
  may be published (e.g. as a release); never with an `assets/` in it (README notice).
- CPU targets (`CMakePresets.json`): `-march=x86-64-v2` (SSE4.2, matches `build.sh`'s
  message; the SDK adds `-msse4.1` to the generated code anyway) and `-mcpu=apple-m1` (all
  Apple Silicon). They were `x86-64-v3` / `apple-m3` until 2026-10-19, which needed AVX2 /
  an M3-class CPU; no measured speed-up was recorded for them.

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
