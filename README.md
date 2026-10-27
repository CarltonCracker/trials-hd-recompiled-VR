# Trials HD Recompiled

Placeholder, plan to explore windows and VR support

> [!WARNING]
> **Disclaimer:** This project was vibe coded with AI assistance. Expect rough edges and review the code before trusting it.

A static recompilation of **Trials HD** (Xbox 360 / XBLA, title ID `5841095A`) for Linux (x86-64) and macOS (Apple Silicon / Intel), built with the [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk).

ReXGlue translates the game's PowerPC executable into C++ ahead of time and links it against a native runtime (kernel HLE, WebGPU or Vulkan GPU backend, SDL input/audio).

> **Note:** This repository contains no game code or assets. You need your own legally obtained copy of Trials HD. **Game assets must never be redistributed:** nothing from the game package (`default.xex`, the `.pak` data, audio, or an installed `assets/` folder). The C++ code recompiled from `default.xex`, and builds of the game made from it, may be redistributed (for example as release binaries) as long as they include no assets: players install the game data from their own copy on first start.

**Status:** Playable with controller or keyboard, high-refresh-rate rendering (with the original fixed 60 Hz physics simulation), and native WebGPU or Vulkan graphics backends.

---

## Requirements

- **Linux x86-64** (x86-64-v2: any CPU with SSE4.2, e.g. Intel Nehalem / AMD Bulldozer and newer; Vulkan 1.2+) or **macOS 12+** (any Apple Silicon, or Intel x86-64)
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

2. Build, pointing at your copy of the game once:
   ```sh
   ./build.sh --game "/path/to/Trials HD (XBLA)"
   ```
   *`--game` accepts the game directory or STFS package file (title ID `5841095A`). The build only takes `default.xex` out of it (into `out/xex/`): the game code is generated from it. Later builds don't need `--game`.*

3. Run it:
   ```sh
   ./run.sh
   ```
   The first start shows a setup screen that asks for your game package (and the optional DLC), installs them and starts the game. See [Running](#running).

### Build Options

```sh
./build.sh --game "/path/to/Trials HD (XBLA)" # First build (takes default.xex from your package)
./build.sh                                    # Later builds
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

1. Configure, then take `default.xex` out of your game package into `out/xex/`:
   ```sh
   cmake --preset linux-amd64-relwithdebinfo  # or mac-arm64-relwithdebinfo / mac-amd64-relwithdebinfo
   cmake --build out/build/linux-amd64-relwithdebinfo --target stfs_extract
   out/build/linux-amd64-relwithdebinfo/stfs_extract "/path/to/5841095A/000D0000/<package file>" out/xex --only default.xex
   ```

2. Build (reconfiguring first records the xex's version for the setup screen):
   ```sh
   cmake -S . -B out/build/linux-amd64-relwithdebinfo
   cmake --build out/build/linux-amd64-relwithdebinfo -j$(nproc)
   # First build only: run a second time to compile generated sources
   cmake --build out/build/linux-amd64-relwithdebinfo -j$(nproc)
   ```
</details>

### Local browser build (experimental)

Build the native game first to generate C++ from your own `default.xex`, then install
[Emscripten](https://emscripten.org/docs/getting_started/downloads.html) (tested: 6.0.12):

```sh
./build.sh --web    # activates ~/emsdk (or $EMSDK) automatically
./run.sh --web      # serves it on localhost with your installed assets/ and prints the URL
```

Open the printed URL (**http://127.0.0.1:8091/...**) in desktop Chrome/Edge with hardware
WebGPU and JSPI support (Chromium 154-155 on Linux are the tested targets). F2 shows the
performance overlay. Linux may
require WebGPU flags; see [the web-port notes](https://github.com/snufflyyy/trials-hd-recompiled/blob/dev/docs/web-port/README.md#111-headless-chromium-with-hardware-webgpu-this-server).
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
[web-port task board](https://github.com/snufflyyy/trials-hd-recompiled/blob/dev/docs/web-port/README.md#16-task-breakdown-for-agents-small-and-large-models)
records the remaining gates.

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

### First start: installing the game

Without an installed game, the game opens a setup screen instead (mouse, keyboard or
controller). Choose your **Trials HD package** (the file in
`<content folder>/0000000000000000/5841095A/000D0000/`) or any folder that contains it,
with the built-in browser, the system file picker, or by dropping it onto the window.
DLC track packs (*Big Pack*, *Big Thrills*) in the same folder are found too, as packages
or extracted folders, and so are **custom tracks** (saved games in
`5841095A/00000001/`): only the levels are installed, without anyone's times, medals or
ghosts, so your progress still starts fresh. **Install and play** extracts them into
`assets/` and starts the game; later starts go straight to the game. The package must be the version this build
was made from (the setup screen says so otherwise).

To add DLC or custom tracks later, open the settings (**F1**, the controller's **Guide**,
or **Back + Start**), go to the **Game** tab and choose **Add DLC or tracks...**; restart
the game to play them.

The same from the command line, without clicking:

```sh
./run.sh --install_game="/path/to/package-or-folder"   # game (and DLC found with it)
./run.sh --install_dlc="/path/to/package-or-folder"    # add DLC to an installed game
```

`./run.sh` installs to the project's `assets/`; the binary on its own installs next to
itself. Point to another location with `--game_data_root=/path/to/assets`.

To create a self-contained folder of the build, e.g. to publish as a release (no game
data; its first start shows the setup screen):
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

## Legal

Trials HD is © RedLynx / Ubisoft. This project is not affiliated with or endorsed by RedLynx, Ubisoft, or Microsoft. It contains no copyrighted game code or assets; you need a legally obtained copy of the game to use it.

The code in this repository is licensed under the [MIT License](LICENSE). The ReXGlue SDK is licensed under the BSD 3-Clause license.
