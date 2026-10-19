# AGENTS.md

Start here. This file is the project overview for AI agents (and people). It is the
same on `main` and `dev`. Detailed topic docs live on the **`dev` branch** in
[`docs/agents/`](#topic-docs); read only the one for the area you are working on.

## What this is

A static recompilation of **Trials HD** (Xbox 360 / XBLA, title ID `5841095A`,
`default.xex` v0.0.1.2) for Linux x86-64, macOS (arm64 / x86-64) and, experimentally,
desktop Chromium (WebAssembly + WebGPU). It is built on the
[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) (submodule `thirdparty/rexglue-sdk`,
v0.10.0): `rexglue codegen` translates the game's PowerPC code into C++ at build time
(`generated/default/`), and that code is linked against the SDK runtime (kernel/XAM HLE,
memory, threads, audio, input, ImGui UI) plus a GPU plugin:

- `xenos` (`librexgpu-xenos`): the SDK's Xenia-derived Vulkan renderer, with Trials-only
  fast paths from our SDK patch. **Native default.**
- `webgpu` (`librexgpu-webgpu`, `src/gpu_webgpu/`): our own Trials-specific WebGPU
  renderer on Dawn (native) or the browser (web). Natively selected with
  `--gpu_plugin=webgpu`; the web build's only renderer.

Our code on top of the SDK (`src/`) is the app shell, game-specific function hooks
(FPS unlock, CRT, menus), settings UI, input and the web glue.

## Hard rules

1. **No game content in git, ever.** The user supplies `default.xex` and data; it goes
   in `assets/` (gitignored). Never commit `assets/`, `generated/default/`, `out/`,
   `*.xex`, `*.pak`, `*.xwb`, shader microcode, WGSL generated from it, disassembly,
   dumps or screenshots. Config files hold **addresses only**.
2. **Never publish or host a web build.** Its `.wasm` contains the user's recompiled game.
   The included server binds to localhost.
3. **Don't edit `generated/`.** It is rewritten by codegen. Change behavior with hooks in
   `src/` or codegen config in `config/`.
4. **SDK changes go through `patches/rexglue-sdk/trials-hd.patch`**, never as commits in
   the submodule. See `docs/agents/sdk-patches.md` before touching the SDK.
5. **Branches:** work on `dev`. `main` is the minimal public tree: only what builds and
   runs the game, plus this file and the README. Dev tools, tests and docs stay on `dev`;
   when merging `dev` into `main`, keep them out. Never push without the user's go-ahead.
6. **Don't change the native build for the web port.** Web code is additive, behind
   `__EMSCRIPTEN__` / `TRIALSHD_WEB`.

## Repository map

| Path | What it is |
| --- | --- |
| `src/` | App (`trials_hd_app.h`, `main.cpp`), game hooks, settings/input/UI, audio fallback |
| `src/gpu_webgpu/` | WebGPU renderer plugin (`rexgpu-webgpu`): Xenos → WGSL translator, caches, resolves, presenter |
| `src/webgpu/` | Small WebGPU device/surface wrapper used by `tools/webgpu_smoke` |
| `src/web/`, `web/`, `cmake/web/` | Browser build: storage/input glue, page JS (input, audio, import), Emscripten CMake |
| `src/diag/` | Optional call-trace counters (`-DTRIALSHD_CALL_TRACE=ON`) |
| `config/` | Codegen settings (`codegen.toml`) and missed function starts (`functions.toml`) |
| `trials_hd_manifest.toml` | ReXGlue project manifest (xex path, output dir, config includes) |
| `generated/rexglue.cmake` | SDK CMake glue (tracked). `generated/default/` is generated, gitignored |
| `patches/rexglue-sdk/` | Our changes to the SDK, applied to the submodule at configure time |
| `metadata/` | Achievement definitions read by the SDK |
| `tools/` | Asset extraction, DLC install, packaging, Dawn build, web server (+ many dev tools on `dev`) |
| `tests/` (dev) | ctest unit tests, headless input scripts, GPU/threading suites |
| `docs/` (dev) | Agent topic docs and the long research/task-board docs |

## Build, run, test

```sh
./build.sh --game "/path/to/Trials HD (XBLA)"   # first time: extract assets/, then build
./build.sh                                      # later builds (RelWithDebInfo; --release/--debug)
./build.sh --no-webgpu                          # skip Dawn/WebGPU (Vulkan only)
./run.sh                                        # run (args pass through, e.g. --gpu_plugin=webgpu)
./build.sh --web && ./run.sh --web              # browser build (needs a native build + Emscripten)
ctest --test-dir out/build/linux-amd64-relwithdebinfo --output-on-failure   # dev branch
```

Needs Clang 19+, CMake 3.25+, Ninja, Python 3, Git. Output: `out/build/<preset>/`.
Presets: `linux-amd64-*`, `mac-arm64-*`, `mac-amd64-*` (`debug`/`release`/`relwithdebinfo`),
`web-wasm32-release` (what `--web` uses), `web-wasm64-release`, `web-wasm64-node`.

## Gotchas that cost time

- **First build generates code, then compiles it.** CMake configures before
  `generated/default/` exists; `build.sh` runs codegen and reconfigures before building
  the game. With plain CMake, run the build command twice.
- **The first configure builds Dawn** (`tools/build_dawn.sh` → `out/dawn-install`), which
  downloads and takes a long time. `--no-webgpu` skips it.
- **Rebuild a renderer with `--target trials_hd_stage_libs`**, not just `rexgpu-xenos`:
  plugins are copied next to `trials_hd` (RPATH `$ORIGIN`) only by that target.
- **The renderer default is Vulkan (`xenos`)** in code (`OnPreSetup` in
  `src/trials_hd_app.h`). Some older text (a `CMakeLists.txt` comment, the web-port and
  WebGPU research docs) says WebGPU; the code wins.
- **Game settings are cvars** saved to `trials_hd.toml` next to the binary; saves, cache
  and logs also live next to the binary (not in `~`). Any cvar works as `--name=value`.
- **Headless runs** need the repo `.venv` active (Pillow, python-xlib) or
  `tools/xsend.py` silently does nothing. Never open the game window or play audio on
  the user's desktop: use the headless tools.

## Topic docs

On `dev`: `docs/agents/<file>`. On `main`, read one without switching branches:
`git show origin/dev:docs/agents/<file>`.

| Working on… | Read |
| --- | --- |
| How the pieces fit: codegen, app lifecycle, plugins, guest memory | `architecture.md` |
| `build.sh` / `run.sh`, presets, CMake options, Dawn, packaging, macOS | `build.md` |
| Changing the ReXGlue SDK, the patch file and how it is applied | `sdk-patches.md` |
| Game behavior: hooks on recompiled functions, FPS unlock, reverse engineering | `game-hooks.md` |
| Settings menu, cvars, keyboard/controller, overlays, audio fallback | `settings-ui-input.md` |
| Assets, STFS extraction, DLC, license, where saves/config go | `game-data.md` |
| Vulkan renderer performance work (SDK `xenos` plugin) | `renderer-vulkan.md` |
| WebGPU renderer (`src/gpu_webgpu/`) | `renderer-webgpu.md` |
| Browser build (`src/web/`, `web/`, `cmake/web/`, `tools/web/`) | `web-port.md` |
| Tests, headless scripts, golden images | `testing.md` |
| What every script in `tools/` does | `tools.md` |

The long research and task-board documents (`docs/web-port/README.md`,
`docs/webgpu-renderer/README.md`, `docs/vulkan-perf/PLAN.md`) are the deep reference and
the live task lists; the topic docs point into them by section.

## Conventions

- C++23, Clang. Match the surrounding code: comments explain *why* (often with a
  measurement), file headers start `// trials_hd - <what it is>`.
- Commit subjects: `area: what changed` (`vulkan:`, `webgpu:`, `web port:`, `sdk patch:`,
  `cmake:`, `build.sh:`, `ui:`), body explains why. Keep measured numbers in commits and
  docs, dated.
- Record findings in the doc that owns the area, not only in chat.
