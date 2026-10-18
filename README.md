# Trials HD Recompiled

> [!WARNING]
> **Disclaimer:** This project was vibe coded with AI assistance. Expect rough edges and review the code before trusting it.

A static recompilation of **Trials HD** (Xbox 360 / XBLA, title ID `5841095A`) for Linux (x86-64) and macOS (Apple Silicon / Intel), built with the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk).

ReXGlue translates the game's PowerPC executable into C++ ahead of time and links it against a native runtime (kernel HLE, WebGPU or Vulkan GPU backend, SDL input/audio).

> **Note:** This repository contains no game code or assets. You must provide your own legally obtained copy of Trials HD (`default.xex`). The recompiled C++ code is generated locally during the build and must not be redistributed.

**Status:** Playable with controller or keyboard, high-refresh-rate rendering (with the original fixed 60 Hz physics simulation), and native WebGPU or Vulkan graphics backends.

---

## Requirements

- **Linux x86-64** (SSE4.2 / Vulkan 1.2+) or **macOS 12+** (Apple Silicon arm64 or Intel x86-64)
- Clang 19+, CMake 3.25+, Ninja, Python 3, Git

**Ubuntu / Debian:**
```sh
sudo apt install clang cmake ninja-build pkg-config python3 \
  libx11-xcb-dev libxcb1-dev libwayland-dev libxkbcommon-dev libxext-dev \
  libxrandr-dev libxi-dev libxcursor-dev libxfixes-dev libxss-dev \
  libasound2-dev libpulse-dev
```

**macOS (Homebrew):**
```sh
brew install cmake ninja
```

---

## Building

### Quick Start

1. Clone the repository with submodules:
   ```sh
   git clone --recursive --shallow-submodules https://github.com/snufflyyy/trials-hd-recompiled.git
   cd trials-hd-recompiled
   ```

2. Build and extract your game files in one step:
   ```sh
   ./build.sh --game "/path/to/Trials HD (XBLA)"
   ```
   *`--game` accepts the game directory or STFS package file (title ID `5841095A`). Asset extraction only needs to run once.*

### Build Options

```sh
./build.sh                                    # Build with existing assets/
./build.sh --game "/path/to/Trials HD (XBLA)" # Extract game package and build
./build.sh --release                          # Build Release configuration (default: RelWithDebInfo)
./build.sh --debug                            # Build Debug configuration
./build.sh --no-webgpu                        # Build Vulkan renderer only (skips Dawn/WebGPU)
./build.sh --clean                            # Clean preset build directory before compiling
./build.sh -j8                                # Limit parallel compile jobs (default: all CPU cores)
```

Optimized native presets disable Tracy profiling. To enable it, configure with
`cmake --preset linux-amd64-relwithdebinfo -DREXGLUE_ENABLE_TRACY=ON` before building
(use the matching macOS preset there). A plain preset configure disables it again.

<details>
<summary>Manual CMake Instructions</summary>

If you prefer building directly with CMake:

1. Extract assets manually into `assets/`:
   ```sh
   python3 tools/stfs_extract.py "/path/to/5841095A/000D0000/<package file>" assets
   ```

2. Configure and build:
   ```sh
   cmake --preset linux-amd64-relwithdebinfo  # or mac-arm64-relwithdebinfo / mac-amd64-relwithdebinfo
   cmake --build out/build/linux-amd64-relwithdebinfo -j$(nproc)
   # First build only: run a second time to compile generated sources
   cmake --build out/build/linux-amd64-relwithdebinfo -j$(nproc)
   ```
</details>

### Local browser build (experimental)

Build the native game first to generate C++ from your own `default.xex`, then install
[Emscripten](https://emscripten.org/docs/getting_started/downloads.html) (tested: 6.0.12):

```sh
# Activates ~/emsdk automatically, or use an already activated toolchain.
tools/web/build.sh
python3 tools/web/serve.py out/build/web-wasm64-release/web
```

Open **http://127.0.0.1:8080** in desktop Chrome/Edge with hardware WebGPU, shared
Memory64 and JSPI support (Chromium 154 on Linux is the tested target). Linux may
require WebGPU flags; see [the web-port notes](docs/web-port/README.md#111-headless-chromium-with-hardware-webgpu-this-server).
Other browsers and mobile are not verified.

On first visit, select your **Trials HD base-game package** or extracted assets folder.
Include `package_license.toml` in a folder import to preserve its package license;
otherwise it runs as a trial. Import stays local; later visits reuse browser storage.
Saves, profiles, settings, logs and caches also persist there. **Clearing site data
deletes saves; changing the server origin/port uses separate storage.** Keep important
saves backed up. Only one tab can use the game storage at a time.

Keyboard and standard gamepads are supported; **F1** opens settings and a double-click
on the canvas toggles fullscreen. Audio starts after a key press or click.

**Never publish or host the build output:** its `.wasm` contains your recompiled game
code. The included server binds to localhost. No game assets are bundled or uploaded.
Browser gameplay, storage and audio acceptance checks are still in progress; the
[web-port task board](docs/web-port/README.md#16-task-breakdown-for-agents-small-and-large-models)
records the remaining gates.

### Adding DLC / Addons (Optional)

Install DLC track packs (*Big Pack* and *Big Thrills*) into `assets/`:

```sh
./add_dlc.sh "/path/to/addon_folder_or_package"
```

---

## Running

Launch the game with the runner script:

```sh
./run.sh
```

Or run the binary directly from the build tree:

```sh
./out/build/linux-amd64-relwithdebinfo/trials_hd
```

Game assets are loaded automatically from the `assets/` folder in the project root or next to the executable. Point to another location using `--game_data_root=/path/to/assets`.

To create a self-contained release package (excluding game assets):
```sh
tools/package.sh
```

---

## Controls

Supports Xbox-style controllers (SDL) and keyboard simultaneously for player one:

| Action | Keyboard | Controller |
| --- | --- | --- |
| Throttle | Up / W | RT |
| Brake / Reverse | Down / S | LT |
| Lean back / forward | Left / A, Right / D | Left stick |
| Checkpoint / Back | Escape / Backspace | B |
| Restart track | R | Back |
| Pause | P | Start |
| Bailout | Y | Y |
| Menu navigation | Arrows / WASD | D-pad |
| Menu select | Enter / Space | A |
| Secondary | X, Q, E | X, LB, RB |

Key bindings can be remapped in the in-game settings menu (**F1**) or in `trials_hd.toml`. Keyboard input can also be disabled (`mnk_mode = false`).

---

## Configuration & Settings

Press **F1** (or controller **Guide** / **Back + Start**) to open the settings menu:

- **Controls:** Customize keyboard key assignments or restore defaults.
- **Graphics:**
  - **Renderer:** Vulkan (default) or WebGPU (`--gpu_plugin=xenos` / `--gpu_plugin=webgpu`).
    WebGPU is the web port's renderer; natively it is kept for developing it.
  - **Display Mode:** Windowed, fullscreen, or desktop borderless.
  - **Render Resolution:** Internal resolution (1280x720, 2560x1440, 3840x2160).
  - **Unlock FPS & FPS Limit:** Decouples rendering frame rate from the fixed 60 Hz physics simulation.
  - **VSync:** Toggle tear-free presentation.

Settings are saved to `trials_hd.toml` and can also be passed as command-line arguments (e.g. `--present_fps_limit=144 --host_vsync=false`).

---

## Save Data & Storage

Native builds store all game saves, user profiles, achievements, shader caches, logs, and configuration settings locally in the build directory, right next to the executable and `trials_hd.toml` (browser storage is described above):

- **Configuration:** `trials_hd.toml` (graphics settings, display modes, keybindings)
- **Save Data & Profiles:** `5841095A/` (track times, medals, unlocked bikes, and custom tracks)
- **Achievements:** `achievements/5841095A.toml` (unlocked achievement progress)
- **Shader Cache:** `cache/` (compiled graphics pipelines)
- **Logs:** `logs/` (`trials_hd.log`)

No data is written to system or user home directories (e.g. `~/.local/share/trials_hd` or `~/Library/Application Support/trials_hd`). Everything is self-contained in the build folder next to the settings config. Custom storage paths can still be specified if desired using `--user_data_root=/path/to/data` or `--cache_root=/path/to/cache`.

---

## Headless Testing

Automated headless tests and benchmarks run under Xvfb:

```sh
# Run a scripted test ride
INPUT=tests/headless/keyboard_ride.txt tools/headless_run.sh 60

# Run timing and regression tests
ctest --test-dir out/build/linux-amd64-relwithdebinfo --output-on-failure
```

---

## Legal

Trials HD is © RedLynx / Ubisoft. This project is not affiliated with or endorsed by RedLynx, Ubisoft, or Microsoft. It contains no copyrighted game code or assets; you need a legally obtained copy of the game to use it.

The code in this repository is licensed under the [MIT License](LICENSE). The ReXGlue SDK is licensed under the BSD 3-Clause license.
