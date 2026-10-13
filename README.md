# Trials HD Recompiled

> [!WARNING]
> ## ⚠️ Disclaimer: this project was 100% vibe coded
>
> **This project was 100% vibe coded, mostly with Claude Opus 5.5 and ChatGPT 6.1
> Sol. It wasn't actually made by me.** AI models wrote the code, the fixes and
> this README; I only gave directions and tested the results. Expect rough edges and
> bugs, and read the code before you trust it.

A static recompilation of **Trials HD** (Xbox 360 / XBLA, title ID `5841095A`) to native
Linux x86-64, built with the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk).

ReXGlue translates the game's PowerPC executable into C++ ahead of time and links it
against a native runtime (kernel HLE, Vulkan GPU backend, SDL input/audio).

> **This repository contains no game code or assets.** You must own Trials HD and supply
> your own copy. The C++ in `generated/` is produced on your machine from your own
> `default.xex` during the build, and must not be redistributed.

Status: **playable with a controller or the keyboard**, with **unlocked real rendering** and the
original ~60 Hz simulation. Headless RX 6600 gameplay checks pass at 30, 60 and
120 render FPS — see *Testing* below. Audio output is untested.
Linux (x86-64) is the only supported target for now.

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
git clone --recursive --shallow-submodules https://github.com/snufflyyy/trials-hd-recompiled.git
cd trials-hd-recompiled
cmake --preset linux-amd64-relwithdebinfo
cmake --build out/build/linux-amd64-relwithdebinfo -j$(nproc)
# First build only: that generated the game code; build again to compile it.
cmake --build out/build/linux-amd64-relwithdebinfo -j$(nproc)
```

The first `cmake --build` runs `rexglue codegen` on `assets/default.xex` (a few
seconds) and asks you to build again: CMake can only add the generated sources once
they exist. The second build re-runs CMake by itself and compiles ~110 MB of generated
C++, which takes a while and several GB of RAM. Later builds need a single command.

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

Play with an Xbox-style controller (SDL gamepad) or the keyboard; both control
player one and work at the same time. Default keys:

| Action | Keys | Controller |
| --- | --- | --- |
| Throttle | Up / W | RT |
| Brake | Down / S | LT |
| Lean back / forward | Left / A, Right / D | Left stick |
| Return to last checkpoint (menus: back) | Backspace | B |
| Restart track | R | Back |
| Pause | Escape | Start |
| Bailout | Y | Y |
| Menu navigation | Arrows / WASD | D-pad |
| Menu select | Return / Space | A |
| X, LB, RB | X, Q, E | X, LB, RB |

Keys can be changed in the settings menu (below), or with the `keybind_*` settings in
`trials_hd.toml`. The keyboard can be turned off there too (`mnk_mode = false`).

### Settings

Press **F1** (or the controller's **Guide** button, or **Back + Start**) to open the
settings menu. It is navigated with the mouse, the arrow keys / Return / Escape, or the
D-pad / left stick, A and B.

**Controls** come first: the Keyboard switch and two key slots per action. Select a
slot, then press the new key (Delete clears the slot, B or selecting the slot again
cancels); **Reset Keys** restores the default layout. Changes apply immediately.

**Graphics**:

| Setting | What it does |
| --- | --- |
| Display Mode | Windowed or fullscreen (applies immediately) |
| Resolution | Window size, or the fullscreen display mode; *Desktop* = borderless at desktop resolution |
| Render Resolution | Internal resolution: 1280x720, 2560x1440 or 3840x2160 (restart to apply) |
| Unlock FPS | Independent real rendering with the original fixed physics tick (on by default; restart to apply) |
| FPS Limit | Unlimited, 240, 165, 144, 120, 90, 60, 45 or 30 real rendered frames per second; independent of physics with Unlock FPS enabled |
| VSync | Tear-free presentation (restart to apply) |

**Save** writes the settings (keys included) to `trials_hd.toml` next to the executable. They are
ordinary settings, so they also work on the command line, e.g.
`--display_width=1920 --display_height=1080 --draw_resolution_scale_x=2
--draw_resolution_scale_y=2 --present_fps_limit=60 --host_vsync=false`.

### FPS unlock

`--fps_unlock=true --present_fps_limit=0 --host_vsync=false` enables uncapped
real rendering. VSync can still limit output to the display refresh rate.
`--fps_unlock=false` restores the original game loop for comparison.

The patch in `src/fps_unlock.cpp` replaces the game's combined frame gate and
separates its update-worker and render-worker synchronization. Game updates keep
the original **16,666 µs fixed step**, including the original five-step hitch
limit; the physics timestep is not divided by the render rate. No optical-flow
frames, framebuffer interpolation, or duplicated presentations are used.
Physics-driven transforms still change on fixed ticks; this patch does not add
sub-tick transform interpolation. Long stalls beyond the five-step catch-up
budget can still lose simulation time, as in the original game.

### Main menu

The main menu is the game's own, with all of its buttons. Leaderboards,
Achievements and Download Content front Xbox LIVE services, which are not
available: the game shows its own offline / not-signed-in behavior for them.

With no audio device the game continues with silent audio. With several GPUs, pick
one with `--vulkan_device=N` (the device list is printed at startup).

### Performance defaults

`readback_memexport` defaults to `false` in this port. Trials HD's memexport passes
(GPU point-list draws used by its effects) are only consumed by later GPU work; reading
them back into guest memory forced a full CPU/GPU sync per draw and held gameplay at
15–30 FPS. Disabling it avoids that readback penalty. Set `readback_memexport = true` in
`trials_hd.toml` to restore the SDK default.

## Testing

Headless runs use Xvfb for the window and keyboard input, a scriptable virtual
controller (`tools/xsend.py`, driven through `TRIALS_HD_VIRTUAL_PAD`), and optionally a Vulkan
*headless surface* so the GPU renders at full speed without a display. Scripts live in
`tests/headless/`; each starts with a comment explaining how to run it.

```sh
# 1440p benchmark on the second GPU, logging the frame rate once per second:
LOG_LEVEL=info INPUT=tests/headless/bench_fps.txt tools/headless_run.sh 80 \
  --vulkan_device=1 --vulkan_headless_surface=true --log_guest_fps=true \
  --log_simulation_rate=true --host_vsync=false \
  --draw_resolution_scale_x=2 --draw_resolution_scale_y=2
tools/bench_summary.py out/run/trials_hd.log

# Keyboard-only ride (real X key events through the keyboard driver):
LOG_LEVEL=info INPUT=tests/headless/keyboard_ride.txt tools/headless_run.sh 75 \
  --vulkan_device=1 --vulkan_headless_surface=true

# Timing regression test: run the same ride at 30, 60, 120 and unlimited FPS.
# Activate the local Python environment first if Pillow / python-xlib are there.
LOG_LEVEL=info INPUT=tests/headless/ride_middle_name_danger.txt tools/headless_run.sh 85 \
  --vulkan_device=1 --vulkan_headless_surface=true --host_vsync=false \
  --present_fps_limit=120 --log_guest_fps=true --log_simulation_rate=true
python3 tests/check_fps_log.py out/run/trials_hd.log 120
ctest --test-dir out/build/linux-amd64-relwithdebinfo --no-tests=error --output-on-failure
```

Frames captured by a script's `capture <name>` lines (taken from the game's own output,
so they work with the headless surface) land in `out/run/<name>.png`.

Verified on Linux with an RX 6600, headless Vulkan with 1280×720 captured output,
host VSync off, and an 85-second scripted ride on *Middle Name: Danger*:

| Render limit | Median real GPU swaps/sec | Mean fixed updates/sec |
| --- | ---: | ---: |
| 30 | 30.0 | 60.00 |
| 60 | 60.0 | 60.07 |
| 120 | 120.0 | 60.05 |
| Unlimited | 203.9 | 59.95 |

At the 2560×1440 render resolution (2×) the same RX 6600 run renders about 87 FPS
uncapped (GPU-bound, 78 FPS median including menus and loading), with fixed updates
still at 60 Hz; the 120 FPS limit needs the 1× render resolution on this GPU.

These are the last 15 logged samples, with positive game draws and resolves in
every sample. Gameplay captures show normal race-timer progression, riding,
explosions and crashes; an original-loop 60-FPS run provides the baseline.
This is a one-track smoke test, not bit-identical replay or full-track coverage.
Audio uses the silent fallback in this environment. Startup heap-release warnings
also occur with the original loop; no guest access violations occurred in these runs.

## Layout

| Path | What |
| --- | --- |
| `trials_hd_manifest.toml` | Codegen manifest |
| `config/codegen.toml` | Codegen settings (CRT `setjmp`/`longjmp` addresses) |
| `config/functions.toml` | Function starts the analyzer misses (addresses only) |
| `src/` | Host-side app code: asset path lookup, license from your package, GPU plugin default, audio fallback, fixed-tick FPS unlock, GPU-wait sleeping, settings menu, keyboard layout |
| `generated/rexglue.cmake` | SDK build glue (from `rexglue init`) |
| `generated/default/` | Recompiled game code; generated at build time, gitignored |
| `thirdparty/rexglue-sdk` | ReXGlue SDK (git submodule) |
| `patches/rexglue-sdk/` | Local SDK changes (one patch, regenerated with `tools/export_sdk_patch.sh`), applied automatically at configure time: rendering fixes (16-bit integer resolves, texture exponent bias), render target transfer elision, small-resolve readback, host FPS cap + guest FPS logging, keyboard keystrokes for menus, data race fixes, guest-memory shm cleanup, Vulkan headless surface |
| `tests/headless/` | Scripted headless test and benchmark runs (`INPUT=… tools/headless_run.sh`) |
| `tools/stfs_extract.py` | Extracts files from Xbox 360 STFS (LIVE/PIRS/CON) packages |
| `tools/xex_dump/`, `tools/disasm.py` | Dump the loaded XEX image and disassemble addresses (output stays in `out/`) |
| `tools/find_missing_funcs.py` | Finds code addresses referenced from data/code that codegen did not register |
| `tools/headless_run.sh`, `tools/xwd_to_png.py` | Run under Xvfb with logging and periodic screenshots |
| `tools/xsend.py` | Scripted key and controller input for headless runs (`INPUT=script tools/headless_run.sh`; keys go through X, buttons through a virtual gamepad) |
| `tools/bench_summary.py`, `tests/check_fps_log.py` | Summarize benchmark logs; check fixed updates stay at 60 Hz |
| `tools/package.sh` | Assembles a runnable build folder in `dist/` (no game data) |

## Legal

Trials HD is © RedLynx / Ubisoft. This project is not affiliated with or endorsed by
them or by Microsoft. It contains no copyrighted game material; you need a legally
obtained copy of the game to use it.

The code in this repository is released under the [MIT License](LICENSE). The ReXGlue
SDK (`thirdparty/rexglue-sdk`) and the changes to it in `patches/rexglue-sdk/` fall
under the SDK's own license (BSD 3-Clause).
