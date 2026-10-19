# Testing

All tests and test tools live on the `dev` branch. None need game files unless noted.
Never open the game window or play audio on the user's desktop: use the headless tools.

## ctest (built with the game, `TRIALSHD_BUILD_TESTING=ON`)

```sh
ctest --test-dir out/build/linux-amd64-relwithdebinfo --output-on-failure
```

| Test | Checks |
| --- | --- |
| `frame_clock` (`tests/frame_clock_test.cpp`) | Fixed 16,666 µs tick, 5-step hitch limit, interpolation fraction |
| `render_pose` (`tests/render_pose_test.cpp`) | Pose interpolation used by the FPS unlock |
| `imgui_sharpness` | Font sizes/DPI, atlas lifetime/retry, numeric top-left FPS counter, menu dimming, achievements sizing and controller page/close input (no GPU/window server) |
| `guest_ui_text` | Guest UI setter UTF-16 encoding, aligned call frame/backchain, caller preservation and invalid-input/error handling (no game files) |
| `vk_gpu_profile` | Bookkeeping of the Vulkan timestamp profiler (no device) |
| `fps_log`, `render_pose_log` | The log checkers `tests/check_fps_log.py` / `check_render_pose_log.py` against fixtures |
| `tracy_default` | Tracy is off by default in optimized builds and can still be turned on |

## Standalone Python tests

```sh
python3 tests/build_script_test.py      # build.sh with mocked tools: first build, flags, web, Bash 3.2
python3 tests/test_add_dlc.py           # DLC install
python3 tests/check_profiler_exit.py    # tools/profile sampler closes its pipes
python3 tools/stfs_extract/test_stfs_extract.py <stfs_extract binary | node + wasm CLI>
```

Run `tests/build_script_test.py` after any `build.sh` change.

## Separate CMake suites

- `tests/gpu_webgpu/`: synthetic WebGPU renderer tests vs the SDK shader interpreter
  (needs Dawn and a built SDK). See its README; each test takes an adapter index.
- `tests/threading/`: POSIX alertable waits and the SDK timer queue
  (`alertable_wait_test`, `timer_queue_test`, `trials_fast_path_test`). See its README.

## Headless game runs (needs `assets/`, Linux)

```sh
source .venv/bin/activate              # Pillow + python-xlib, or xsend.py silently does nothing
INPUT=tests/headless/keyboard_ride.txt tools/headless_run.sh 75 \
    --vulkan_device=1 --vulkan_headless_surface=true
```

`tools/headless_run.sh [seconds] [game args…]` starts Xvfb, forces SDL X11 and silent
audio (including from a Wayland desktop), runs the game with logs and
periodic screenshots in `out/run/` (`trials_hd.log`, `shot_NNN.png`), and plays `INPUT`
with `tools/xsend.py`. A process-local SDL filter excludes physical controllers
so the virtual pad gets player one. Set `SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT`
explicitly to override the default `0x0000/0x0000` VID/PID filter. This does not
change other applications or disconnect hardware. The harness also flushes logs
every second so termination does not lose the final hook/verification messages.
Renderer flags pick the renderer (`--webgpu_*` → WebGPU). On the original server `--vulkan_device=1` is the RX 6600. An intermittent `malloc_consolidate()`
abort at startup is a known bug: rerun once before investigating.

**Script format** (`tests/headless/*.txt`, one command per line, `#` comments; the header
comment says what the script does and how to run it):

```
wait <s>                   tap <keysym> [ms]          hold <keysym> <ms>
down <keysym>              up <keysym>                focus <window title>
pad <button> <0|1>         axis <axis> <value>        (virtual pad, src/virtual_pad.h)
capture <name>             the game's own frame → <out>/<name>.png (works headless)
shot <name>                Xvfb framebuffer → <out>/<name>.png
move <title> <x> <y>        place the window inside the virtual display
click <x> <y>               left-click at virtual-display coordinates
set <cvar> <value>         change a cvar at runtime
```

Scripts: menu walks (`main_menu_nav`, `keyboard_menu_nav`, `credits`), rides
(`ride_tutorial`, `ride_hello_world`, `ride_the_breakdown`, `ride_middle_name_danger`,
`keyboard_ride`, `controller_ride`), settings (`settings_menu`, `controller_settings`,
`settings_mouse`), `exit_button_test`, and `bench_fps` (the benchmark ride). The browser
harness `tools/web/cdp.py --script` plays the same files.

### UI sharpness screenshots

Run with a 2560x1440 virtual display and matching game window:

```sh
HEADLESS_WIDTH=2560 HEADLESS_HEIGHT=1440 INPUT=tests/headless/ui_sharpness.txt \
    tools/headless_run.sh 45 --display_width=2560 --display_height=1440 \
    --fullscreen=false --show_performance_overlay=true --vulkan_device=1
```

Select the software Vulkan device if hardware Vulkan cannot present to Xvfb
(no DRI3). Its index depends on the machine; it was device 1 for the 2026-10-18
verification. For 720p, change both display and window dimensions to 1280x720.

This captures the FPS counter alone, Controls, Graphics, achievements,
two menus together, and the game after closing them. The script moves the resized
window to (0,0) and waits for repaint so Xvfb's lack of a window manager does not
crop it or leave stale pixels. Key transitions include delays for software
rendering. Use `shot`, **not** `capture`: guest captures omit the host ImGui pass.
Keep screenshots in gitignored output directories; never commit them.

`tests/headless/achievements_close.txt` uses the same 1440p display/window flags
and a 45-second run to click Close (Esc), reopen, click the title-bar X, reopen,
and press Escape. It requires the real host framebuffer (`shot`), not a guest
capture. Check the resulting screenshots with:

```sh
python3 tests/check_achievements_close.py out/run
```

This checks that the static game background undims after each close and dims
again after reopening; a stale/frozen menu fails. The mouse coordinates are
specific to the 2560x1440 layout.

`tests/headless/achievements_controller.txt` uses the same 45-second 1440p run.
It checks LB/RB paging, D-pad/left-stick scrolling, right-stick return to the
start, and A/B closing. Run `python3 tests/check_achievements_controller.py out/run`
to check content movement, return-to-top and background restoration.

`tests/headless/pc_settings_menu.txt` uses a 50-second 1440p run with keyboard
input enabled (`--mnk_mode=true`). It selects the former Download Content slot,
opens Settings via controller A, checks the Graphics tab, closes it and reopens
via keyboard Return. Run `python3 tests/check_pc_settings_menu.py out/run` to check
both host-hook dispatches and menu/background transitions. Retain all
`trials_hd*.log` files when copying evidence: debug logging can rotate between
activations. Inspect `pc_main_menu`
and `pc_settings_graphics` captures for the PC SETTINGS and FPS Counter captions.

## Images and FPS

- `tools/compare_captures.py`: PSNR/SSIM between PNG pairs (thresholds from a TOML such as `golden.toml`).
- `tools/run_golden_ci.py [--run]`: Vulkan vs WebGPU parity on the scenes in
  `tests/headless/golden.toml` (script, capture, `min_psnr`, `min_ssim`); prints a pass/fail
  table. `--self-test` checks the tool itself.
- `tests/check_fps_log.py LOG [fps]`: from a `--log_guest_fps=true` log, checks presented vs
  guest FPS. `tests/check_render_pose_log.py`: interpolation log sanity.
- Rides aren't frame-deterministic: compare menus, the menu's 3D preview or a paused ride.

## Guards

- `tools/check_native.sh baseline|check [dir]`: proves a change left the native game
  unchanged (same generated code, no web symbols, both renderers boot, captures
  ≥ 33 dB). Required for web-port changes to shared code. `CHECK_GPU_INDEX` picks the GPU.
- Vulkan performance work: the benchmark and image check in `renderer-vulkan.md`.
