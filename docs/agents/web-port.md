# Web port (Emscripten + WebGPU)

The same game, runtime and WebGPU renderer compiled to WebAssembly, running in desktop
Chromium. **Experimental.** The native builds are the product: the web port must not
change them.

**Legal rule:** the `.wasm` holds the user's recompiled game. Build and serve it locally
only (`127.0.0.1`); never publish, host or upload a build. Game data is imported by the
user in their own browser.

## Build and run

```sh
./build.sh              # native build first: it runs codegen into generated/default/
./build.sh --web        # preset web-wasm32-release; finds ~/emsdk or $EMSDK (tested 6.0.12)
./run.sh --web          # tools/web/serve.py on 127.0.0.1:8091 with assets/; --name=value → page flags
```

Open `http://127.0.0.1:8091/trials_hd.html` in Chrome/Edge with hardware WebGPU and JSPI
(Chromium 154–155 on Linux tested; Linux may need WebGPU flags, see
`docs/web-port/README.md` §11.1). `?web_assets_url=/assets` mounts the served `assets/`
(dev); without it the page shows the OPFS import. URL query = runtime cvars
(`&log_guest_fps=true`); `#log` shows the log over the game. Needs a secure context
(localhost/HTTPS) for `SharedArrayBuffer`; `serve.py` sends COOP/COEP/CORP.

Other presets: `web-wasm64-release` (Memory64), `web-wasm64-node` (Node with host files:
`tools/web/run_node.sh`). `web-wasm32` is the default because Chromium bounds-checks every
Memory64 access (~1.5x slower); the game fits in 4 GiB (3.6 GiB peak).

## How it is built

`CMakeLists.txt` hands off to `cmake/web/trials_hd_web.cmake` when `TRIALSHD_WEB=ON` under
`emcmake`, then returns, so a native configure never reads it. It compiles the SDK, the
generated code and the app from **explicit source lists** (not the SDK's CMake) and never
runs codegen. Options: `TRIALSHD_WEB_WASM32`, `TRIALSHD_WEB_WEBGPU`, `TRIALSHD_WEB_NODE`,
`TRIALSHD_WEB_RECOMP_OPT`, and `TRIALSHD_WEB_SDK_*` source groups. FFmpeg (XMA audio) is
built by `cmake/web/ffmpeg.cmake`. **A new app `.cpp` must be added to the web source list
here too.**

## Key design decisions (`docs/web-port/README.md` §1)

- Guest address window folded by a 256-entry table (`addr >> 24`): wasm has no page
  aliasing.
- No `mprotect`/`SIGSEGV`: GPU-visible writes are tracked by codegen (a dirty bitmap);
  host-side copies call `rex::memory::web::MarkGuestWrite` (see `src/crt_hooks.cpp`).
- Real pthreads (`PROXY_TO_PTHREAD`); the browser main thread stays free.
- **One GPU worker owns every WebGPU object**: the command-processor thread gets the
  `OffscreenCanvas` and also paints the presenter and ImGui. Release objects from other
  threads with `WebGpuProvider::ReleaseOnGpuWorker`; UI drawers run under
  `rex::ui::web::UIMutex()`.
- The GPU worker yields to its event loop at swaps/waits (JSPI) so `mapAsync` and
  presentation progress.
- Static GPU plugins (`webgpu`, plus `null` that runs the command stream without drawing:
  `?gpu_plugin=null`).
- Input polled on the main thread (Gamepad API is window-only), passed through a
  `SharedArrayBuffer` seqlock; audio through an AudioWorklet reading a shared ring.

## Code map

| Path | Role |
| --- | --- |
| `src/web/web_files.*` | OPFS mount (`/opfs/trials_hd`), first-run import, dev asset mount (`/assets`) |
| `src/web/web_input.*` | Keyboard/gamepad snapshots shared with `web/input.js` |
| `src/web/null_graphics_system.*` | The `null` GPU plugin |
| `web/shell.html` | Page template |
| `web/input.js` | Main-thread input: Gamepad API + key events → shared memory |
| `web/audio.js`, `web/audio_worklet.js` | 48 kHz AudioContext + worklet draining the SDK's `WebAudioRing` (start exactly one) |
| `web/import.js` | Local first-run import of the user's package/folder (no uploads) |
| `web/webgpu_compat.js` | `--pre-js` browser shims for emdawnwebgpu (e.g. Firefox `setBindGroup`) |
| SDK (patch) | `rex/ui/web.h`, `src/ui/web/`, `src/input/web/`, `src/audio/web/`, `src/core/fma_emscripten.cpp` |
| `tools/web/` (dev) | `serve.py` (main too), `cdp.py` (headless Chromium driver), `bidi.py` (Firefox), `build.sh`, `run_node.sh`, `compile_generated.py`, `perf/`, `tests/`, `spikes/` |

## Testing (dev branch)

- `tools/web/cdp.py URL --script tests/headless/<script>.txt --out-dir DIR --console LOG`
  plays the native headless scripts in headless Chromium (options: `--viewport WxH@DPR`,
  `--profile DIR`, `--chromium-flag`, `--eval`, `--until PATTERN`).
- FPS: run `ride_middle_name_danger` with `&log_guest_fps=true&log_simulation_rate=true`,
  then `tests/check_fps_log.py LOG 60`.
- Audio: `--eval 'JSON.stringify(window.trialsHd.audio.stats())'` (underruns must stay 0).
- OPFS: `tools/web/tests/storage_browser.py URL assets --folder`. Unit tests:
  `node tools/web/tests/{import,input,shell}.mjs`, `tools/web/test_serve.py`.
- **Before committing shared code:** `tools/check_native.sh check <baseline dir>` (after
  `tools/check_native.sh baseline` on the unchanged tree) proves the native game is
  unchanged: no web symbols, same generated code, both renderers boot with menu captures
  ≥ 33 dB PSNR.
- Stalls: with `log_guest_fps=true` the log shows pipeline-creation time and GPU-worker
  busy %; after 3 s without a guest swap it logs the worker's phase (`web_phase.h`).

## Status (2026-10-18) and where to resume

Menus, settings, OPFS import, keyboard/gamepad rides, audio, and a steady 60 FPS while
riding (wasm32) work in desktop Chromium. Open: re-run OPFS import and the remaining
input scripts on wasm32, first-use shader compile stalls, boot time (~2x native),
macOS review gates. Firefox/Safari deferred by the user. **Start with
`docs/web-port/README.md` §16.H**, then the notes log (§16.6, newest first). Web tasks are
prefixed `W` and claimed in §16.
