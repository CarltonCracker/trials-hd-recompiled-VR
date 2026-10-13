# Trials HD Recompiled

A static recompilation of **Trials HD** (Xbox 360 / XBLA, title ID `5841095A`) to native
Linux x86-64, built with the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk).

ReXGlue translates the game's PowerPC executable into C++ ahead of time and links it
against a native runtime (kernel HLE, Vulkan GPU backend, SDL input/audio).

> **This repository contains no game code or assets.** You must own Trials HD and supply
> your own copy. The C++ in `generated/` is produced on your machine from your own
> `default.xex` during the build, and must not be redistributed.

Status: **playable with a controller** at a steady **60 FPS at 1440p on an RX 6600**
(menus and gameplay, measured headless — see *Testing* below). Audio output is untested.
Linux (x86-64) is the only supported target for now; a native WebGPU renderer is in
development ([docs/webgpu-renderer.md](docs/webgpu-renderer.md)).

## Requirements

- Linux x86-64 with an SSE4.2-class CPU (x86-64-v2) and a Vulkan 1.2+ GPU/driver
- clang (19+), CMake 3.25+, Ninja, Python 3
- Debian/Ubuntu packages:

  ```sh
  sudo apt install clang cmake ninja-build pkg-config python3 \
    libx11-xcb-dev libxcb1-dev libwayland-dev libxkbcommon-dev libxext-dev \
    libxrandr-dev libxi-dev libxcursor-dev libxfixes-dev libxss-dev \
    libasound2-dev libpulse-dev
  ```

## 1. Provide your game files

You need the Trials HD XBLA package from your own console (an STFS/LIVE file, found
under `Content/0000000000000000/5841095A/000D0000/` on the Xbox 360 drive).

Extract it into `assets/`:

```sh
python3 tools/stfs_extract.py "/path/to/5841095A/000D0000/<package file>" assets
```

You should end up with `assets/default.xex`, `assets/data/*.pak` and
`assets/package_license.toml`. `assets/` is gitignored.

The extractor records the license stored in your package header, and the game reports
it at runtime exactly as your console would: a package licensed for the full game runs
as the full game, a trial package runs as a trial.

## 2. Build

```sh
git clone --recursive --shallow-submodules <this repo>
cd trails-hd-recomp
cmake --preset linux-amd64-relwithdebinfo
cmake --build out/build/linux-amd64-relwithdebinfo -j$(nproc)
```

The first build runs `rexglue codegen` on `assets/default.xex` (a few seconds) and
then compiles ~110 MB of generated C++, which takes a while and several GB of RAM.

### WebGPU renderer (in development)

See [docs/webgpu-renderer.md](docs/webgpu-renderer.md). It needs Dawn, built from
`thirdparty/dawn`:

```sh
cmake -S thirdparty/dawn -B out/dawn -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DDAWN_FETCH_DEPENDENCIES=ON -DDAWN_ENABLE_INSTALL=ON -DDAWN_BUILD_SAMPLES=OFF \
  -DDAWN_BUILD_TESTS=OFF -DTINT_BUILD_TESTS=OFF -DDAWN_USE_GLFW=OFF -DDAWN_USE_WAYLAND=OFF \
  -DCMAKE_INSTALL_PREFIX=$PWD/out/dawn-install
cmake --build out/dawn --target install
cmake --preset linux-amd64-relwithdebinfo -DTRIALSHD_WEBGPU=ON \
  -DCMAKE_PREFIX_PATH=$PWD/out/dawn-install
cmake --build out/build/linux-amd64-relwithdebinfo --target webgpu_smoke
```

## 3. Run

`tools/package.sh` assembles a self-contained folder (`dist/trials_hd-linux-x86_64`)
from the release build (`cmake --preset linux-amd64-release` then build); put your
`assets/` in it and run `./trials_hd` from there. Or run straight from the build tree:


```sh
./out/build/linux-amd64-relwithdebinfo/trials_hd
```

Game data is found automatically in an `assets/` folder next to the executable or in
the current directory. Point elsewhere with `--game_data_root=/path/to/extracted`.

On a headless machine, run under a virtual X server:

```sh
xvfb-run -s "-screen 0 1280x720x24" ./out/build/linux-amd64-relwithdebinfo/trials_hd
# or, with a debug log and screenshots every 5 s in out/run/:
tools/headless_run.sh 60
```

Play with an Xbox-style controller (SDL gamepad). Keyboard emulation exists in the
SDK (`--mnk_mode=true`) but is not supported by this port yet.

### Graphics settings

Press the controller's **Guide** button (or **Back + Start**, or **F1**) to open the
graphics menu. It is navigated with the D-pad / left stick, A and B.

| Setting | What it does |
| --- | --- |
| Display Mode | Windowed or fullscreen (applies immediately) |
| Resolution | Window size, or the fullscreen display mode; *Desktop* = borderless at desktop resolution |
| Render Resolution | Internal resolution: 1280x720, 2560x1440 or 3840x2160 (restart to apply) |
| FPS Limit | Caps frames presented to the screen. Trials HD advances its simulation by a fixed 1/60 s per frame, so this never changes game speed |
| VSync | Tear-free presentation (restart to apply) |

**Save** writes the settings to `trials_hd.toml` next to the executable. They are
ordinary settings, so they also work on the command line, e.g.
`--display_width=1920 --display_height=1080 --draw_resolution_scale_x=2
--draw_resolution_scale_y=2 --present_fps_limit=60 --host_vsync=false`.

### Main menu

Leaderboards, Achievements and Download Content are removed from the main menu
(they front Xbox LIVE services). The game's own menu scene is not modified; the
buttons are hidden at runtime and the remaining ones move up (`src/main_menu.cpp`).

With no audio device the game continues with silent audio. With several GPUs, pick
one with `--vulkan_device=N` (the device list is printed at startup).

### Performance defaults

`readback_memexport` defaults to `false` in this port. Trials HD's memexport passes
(GPU point-list draws used by its effects) are only consumed by later GPU work; reading
them back into guest memory forced a full CPU/GPU sync per draw and held gameplay at
15–30 FPS. Without it the game holds 60 FPS. Set `readback_memexport = true` in
`trials_hd.toml` to restore the SDK default.

## Testing

Headless runs use Xvfb for the window and input, a scriptable virtual controller
(`tools/xsend.py`, driven through `TRIALS_HD_VIRTUAL_PAD`), and optionally a Vulkan
*headless surface* so the GPU renders at full speed without a display. Scripts live in
`tests/headless/`; each starts with a comment explaining how to run it.

```sh
# 1440p benchmark on the second GPU, logging the game's frame rate once per second:
LOG_LEVEL=info INPUT=tests/headless/ride_middle_name_danger.txt tools/headless_run.sh 85 \
  --vulkan_device=1 --vulkan_headless_surface=true --log_guest_fps=true \
  --draw_resolution_scale_x=2 --draw_resolution_scale_y=2
grep "guest fps" out/run/trials_hd*.log
```

Frames captured by a script's `capture <name>` lines (taken from the game's own output,
so they work with the headless surface) land in `out/run/<name>.png`.

## Layout

| Path | What |
| --- | --- |
| `trials_hd_manifest.toml` | Codegen manifest |
| `config/codegen.toml` | Codegen settings (CRT `setjmp`/`longjmp` addresses) |
| `config/functions.toml` | Function starts the analyzer misses (addresses only) |
| `src/` | Host-side app code: asset path lookup, GPU plugin default, audio fallback |
| `generated/rexglue.cmake` | SDK build glue (from `rexglue init`) |
| `generated/default/` | Recompiled game code; generated at build time, gitignored |
| `thirdparty/rexglue-sdk` | ReXGlue SDK (git submodule) |
| `patches/rexglue-sdk/` | Local SDK changes, applied automatically at configure time: keyboard keystroke events, host presentation FPS cap + guest FPS logging, guest-memory shm cleanup, Vulkan headless surface, quieter per-query GPU logging |
| `tests/headless/` | Scripted headless test and benchmark runs (`INPUT=… tools/headless_run.sh`) |
| `tools/stfs_extract.py` | Extracts files from Xbox 360 STFS (LIVE/PIRS/CON) packages |
| `tools/xex_dump/`, `tools/disasm.py` | Dump the loaded XEX image and disassemble addresses (output stays in `out/`) |
| `tools/find_missing_funcs.py` | Finds code addresses referenced from data/code that codegen did not register |
| `tools/headless_run.sh`, `tools/xwd_to_png.py` | Run under Xvfb with logging and periodic screenshots |
| `tools/xsend.py` | Scripted controller/key input for headless runs (`INPUT=script tools/headless_run.sh`, drives a virtual gamepad) |
| `tools/package.sh` | Assembles a runnable build folder in `dist/` (no game data) |
| `src/webgpu/`, `tools/webgpu_smoke/` | WebGPU renderer foundation (Dawn natively, emdawnwebgpu on the web) and its smoke test |
| `docs/webgpu-renderer.md`, `docs/d3d-map.md` | WebGPU renderer design/milestones; map of the game's Direct3D runtime |
| `tools/d3d_survey.py` | Finds the functions that emit GPU command packets (Direct3D runtime survey) |
| `tools/gen_trace_shims.py`, `src/diag/` | Diagnostic build (`-DTRIALSHD_CALL_TRACE=ON`): per-function call counts and argument samples, for mapping the Direct3D API |
| `thirdparty/dawn` | Dawn (git submodule), only needed with `-DTRIALSHD_WEBGPU=ON` |

## Legal

Trials HD is © RedLynx / Ubisoft. This project is not affiliated with or endorsed by
them or by Microsoft. It contains no copyrighted game material; you need a legally
obtained copy of the game to use it.
