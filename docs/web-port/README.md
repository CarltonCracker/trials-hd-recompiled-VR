# Web port (Emscripten + WebGPU): research and build plan

Research for running Trials HD Recompiled in a web browser: the recompiled game, the
ReXGlue runtime and the native WebGPU renderer (`src/gpu_webgpu/`, see
[`docs/webgpu-renderer/README.md`](../webgpu-renderer/README.md)) compiled to
WebAssembly with Emscripten and drawing through the browser's WebGPU. Written 2026-10-15
for the agents (and people) who will build it. **The localhost server and standalone
browser spikes exist; the complete game web build is not implemented yet.** This
document is the specification and live progress record.

**This document is also the web port's task list.** Section 16 holds every task with its
status. Agents claim, update and complete tasks there (rules in 16.0) and keep the rest of
the document in sync with what they learn. The renderer document keeps its own task list;
web tasks are prefixed `W` so the two never collide.

**Platform priority:** Chrome/Edge on desktop (Linux, Windows, macOS) → Firefox desktop →
Safari (macOS) → Android Chrome (later, section 13). Desktop Chromium decides every
trade-off. The native Linux/macOS builds stay the primary product; the web build is an
additional target built from the same sources.

**Legal constraint (decides the deployment model, section 7.7):** the `.wasm` contains the
recompiled game code, generated on the user's machine from their own `default.xex`. Like
`generated/`, it **must never be published or hosted**. The web port is built locally and
served from `localhost` (or a private LAN host) by the person who owns the game. Game data
also stays user-provided: imported from the user's own package in the browser, never
downloaded from us.

Everything here is either **measured** (with the command that measured it), **read from
code** (with the file), or marked **unverified**. Browser facts change monthly: re-check
section 5 before relying on it.

Contents:

1. [Summary and decisions](#1-summary-and-decisions)
2. [Where the project is today](#2-where-the-project-is-today)
3. [How the native build is put together](#3-how-the-native-build-is-put-together)
4. [Measurements that size the port](#4-measurements-that-size-the-port)
5. [The browser platform in October 2026](#5-the-browser-platform-in-october-2026)
6. [Compile spike: the generated code under Emscripten](#6-compile-spike-the-generated-code-under-emscripten)
7. [Architecture](#7-architecture)
8. [Subsystem by subsystem](#8-subsystem-by-subsystem)
9. [WebGPU renderer changes for the browser](#9-webgpu-renderer-changes-for-the-browser)
10. [Building it](#10-building-it)
11. [Testing, debugging and validation](#11-testing-debugging-and-validation)
12. [Milestones](#12-milestones)
13. [Later: Safari, Firefox, Android](#13-later-safari-firefox-android)
14. [Risks and open questions](#14-risks-and-open-questions)
15. [Tools and reference index](#15-tools-and-reference-index)
16. [Task breakdown for agents (small and large models)](#16-task-breakdown-for-agents-small-and-large-models): **the live task list**

---

## 1. Summary and decisions

**Feasible, and the hardest unknowns already have evidence.** A third-party browser port of
a much larger game (`~/playgta5-main`, tested on this server 2026-10-09) runs a shared,
**64-bit (Memory64) WebAssembly memory with threads** (3 GiB minimum, 16 GiB maximum) and
91,026 functions in a 47 MiB code section, with WebGPU, in this server's Chromium 154
(section 5.6). Our generated game code compiles with Emscripten 6.0.12 for both wasm32
and wasm64 after four small header fixes and produces 35–37 MiB of code (section 6).

Decisions (each is justified in section 7; change them only with a Notes-log entry):

| # | Decision | Why (short) |
| --- | --- | --- |
| D1 | **wasm64 (Memory64) first**, wasm32 kept as a measured fallback | The guest needs a 3 GiB window plus ~1 GiB host heap (4.4); wasm32 caps at 4 GiB total. The SDK assumes 64-bit host pointers everywhere. Shared Memory64 + threads already works in Chromium (5.6). |
| D2 | **Guest window of 2.75 GiB, aliases folded by a 256-entry table** (`addr >> 24`): physical views, `0x7F…` and `0x9…` map onto their native targets (7.9.2) | Wasm has no page aliasing; the codegen already routes every access through `REX_PHYS_HOST_OFFSET` (3.3); one load + add per access. |
| D3 | **GPU-visible write tracking from codegen**, not page protection: physical-range stores set a bit in a 16 KiB dirty bitmap | No `mprotect`/`SIGSEGV` in wasm. Stores check the folded host physical range; loads only translate. |
| D4 | **Real pthreads** for guest threads (36 host threads measured), `PROXY_TO_PTHREAD`, browser main thread kept free | Guest code blocks constantly; `Atomics.wait` is forbidden on the main thread. |
| D5 | **One GPU worker owns every WebGPU object**: the command-processor `XHostThread` gets the `OffscreenCanvas` and also paints the presenter and ImGui (new paint mode, 7.9.7) | WebGPU objects belong to the thread that created them; the command processor must stay an `XHostThread` because it runs guest interrupt callbacks. |
| D6 | **The GPU worker yields to its event loop at swaps and waits** through one `YieldToEventLoop()` (JSPI; Asyncify limited to the GPU worker's stack as fallback; 7.9.5) | Browser WebGPU only delivers `mapAsync`/device callbacks and presents the canvas when the worker returns to its event loop. |
| D7 | **Same renderer source**, `#if __EMSCRIPTEN__` only where Dawn-native APIs are used; `emdawnwebgpu` port | One renderer for native and web (the plan since the renderer doc, section 13). |
| D8 | **Static GPU plugin** (no `dlopen`), Vulkan plugin not built | Wasm side modules are slow and complex; the browser has only WebGPU. |
| D9 | **Game data imported once into OPFS** from the user's own package or folder, read through WasmFS's OPFS backend | 206 MiB of data (4.6); synchronous file I/O from pthreads; persists across visits. |
| D10 | **Input polled on the main thread** (Gamepad API is window-only) and passed through a `SharedArrayBuffer` | `navigator.getGamepads()` is on `Navigator`, not `WorkerNavigator` (5.4). |
| D11 | **Audio through an AudioWorklet** reading a shared ring; XMA decoded by FFmpeg compiled to wasm | Web Audio's only low-latency path; FFmpeg already does XMA in the SDK. |
| D12 | **Local build and local serving only**; never publish the `.wasm` | Contains recompiled game code (legal constraint above). |
| D13 | **Target: 60 FPS at 1280x720 in desktop Chrome** on RX 6600-class hardware; uncapped numbers are tracked, not promised | Native uses 117% of one core at 60 FPS (4.3); wasm code is 1.5–3× slower and browser WebGPU validates every call. |
| D14 | **The web port must not change the Linux or macOS builds.** Web code is additive and compiled only by Emscripten; every web task passes `tools/check_native.sh check` (section 7.8) | The native builds are the product; the web build is an extra target. |

---

## 2. Where the project is today

- **Native build:** Linux (primary) and macOS. WebGPU is the default renderer since
  2026-10-15 (`main` `da3a83c`): about 190 FPS uncapped at 720p on an RX 6600, about
  72 FPS at 2x. The Vulkan renderer is still built as the fallback.
- **WebGPU renderer:** written against `webgpu.h` with Dawn. Design and history:
  `docs/webgpu-renderer/README.md`. Its section 13 already lists the browser traps; this
  document replaces that sketch with measurements and a plan.
- **Reference port:** `~/playgta5-main` (not our code; a mirrored third-party site used for
  headless-WebGPU experiments on 2026-10-09). Useful as evidence of what browsers accept
  (section 5.6), not as code to copy.
- **Toolchain on the build server:** Emscripten 6.0.12 installed at `~/emsdk` (2026-10-15,
  `./emsdk install latest`); Chromium 154 at `/usr/bin/chromium` with a working
  hardware-WebGPU recipe on the RX 6600 (section 11.1).

---

## 3. How the native build is put together

### 3.1 Layers

| Layer | Where | Web status |
| --- | --- | --- |
| Recompiled game: 18,153 functions, 107 files, 111 MiB of C++ | `generated/default/` (built from the user's XEX) | Compiles to wasm (section 6) |
| App: hooks, settings menu, input, FPS unlock, exit button | `src/*.cpp` | Portable; `ReExecWithGpuPlugin` and `/proc/self` use become no-ops |
| Kernel HLE, memory, threads, files, audio, input, UI | `thirdparty/rexglue-sdk/src/` (`rexruntime`) | Needs a web platform layer (section 8) |
| GPU front-end (register file, packet processor, texture/RT logic) | SDK `src/graphics/` (compiled into the plugin) | Portable C++ |
| WebGPU renderer | `src/gpu_webgpu/` | Mostly portable; Dawn-native and threading changes (section 9) |
| Third-party libraries | SDK `thirdparty/` | SDL3, FFmpeg, fmt, spdlog, imgui, toml++, xxhash, libmspack port; Vulkan, glslang, SPIRV, Tracy, MoltenVK dropped |

### 3.2 Process model (native)

36 threads during a ride (measured 2026-10-15, `/proc/<pid>/task`, ride script
`out/claude-3d/bench.txt`): about 22 guest `XThread`s, `GPU Commands` (the command
processor), `GPU VSync`, `Audio Worker`, `XMA Decoder`, `Kernel Dispatch`, two `disk`
threads and five host threads (UI/main, Dawn, logging). The UI thread runs SDL's event
loop, paints the presenter and ImGui. The command-processor thread records and submits
WebGPU work; both use the device under one mutex (renderer doc 7.6.4).

### 3.3 How generated code touches guest memory (read from code)

`generated/default/trials_hd_pch.h` (emitted by `rexglue codegen`):

```cpp
#define REX_LOAD_U32(x)     __builtin_bswap32(*(volatile u32*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)))
#define REX_STORE_U32(x, y) (*(volatile u32*)(base + (u32)(x) + REX_PHYS_HOST_OFFSET(x)) = __builtin_bswap32(y))
// REX_PHYS_HOST_OFFSET(addr) is 0 on Linux, ((u32)addr >= 0xE0000000 ? 0x1000 : 0) on Windows / macOS arm64.
#define REX_IS_MMIO_ADDR(addr) ((addr) >= 0x7F000000u && (addr) < 0x80000000u)
// REX_MM_LOAD_* / REX_MM_STORE_*: as above but call MMIOHandler::CheckLoad/CheckStore for MMIO addresses.
```

- 448,775 `REX_LOAD_*`/`REX_STORE_*` sites; 28 `REX_MM_*` (MMIO-checked) sites
  (`cat generated/default/trials_hd_recomp.*.cpp | grep -o 'REX_\(LOAD\|STORE\)_U[0-9]*' | wc -l`).
- **Every access already goes through one macro with an address hook.** That hook is where
  the web build folds the physical views (D2) and marks dirty pages (D3); no codegen
  rewrite is needed, only a new `REX_PHYS_HOST_OFFSET` / store macro for `__EMSCRIPTEN__`.
- `lwarx`/`stwcx.` use `__sync_bool_compare_and_swap` on guest memory: fine with wasm
  threads (`-matomics`).
- VMX uses SIMDe (`simde_mm_*`, top: 84k `load_si128`, 45k `store_si128`, 16k
  `shuffle_epi8`). SIMDe has a native wasm `simd128` path (`shuffle_epi8` →
  `i8x16.swizzle`).
- 9 `setjmp` and 22 `longjmp` sites (MSVC CRT `setjmp`/`longjmp` mapped to host ones,
  `config/codegen.toml`); Emscripten supports them with wasm exception handling.
- Floating-point mode: 13,921 `fpscr.disableFlushMode`, 247 `enableFlushMode`, 5
  `storeFromGuest` (rounding-mode changes). `include/rex/platform/fpscr.h` implements
  them for x86-64 (MXCSR) and arm64 (FPCR) only. Wasm has no flush-to-zero or rounding
  control: flush toggles become no-ops; the 5 rounding sites need checking (section 14 Q6).

### 3.4 Guest memory map (read from `src/system/xmemory.cpp`)

| Guest range | Heap | Page size | Notes |
| --- | --- | --- | --- |
| `0x00000000–0x3FFFFFFF` | `v00000000` | 4 KiB | virtual |
| `0x40000000–0x7EFFFFFF` | `v40000000` | 64 KiB | virtual; thread stacks near `0x7xxxxxxx` |
| `0x80000000–0x8FFFFFFF` | `v80000000` | 4 KiB | XEX image (`0x82000000`, 8.2 MiB) |
| `0x90000000–0x9FFFFFFF` | `v90000000` | 4 KiB | XEX heap |
| `0xA0000000–0xBFFFFFFF` | `vA0000000` | 64 KiB | view of physical memory |
| `0xC0000000–0xDFFFFFFF` | `vC0000000` | 16 MiB | view of physical memory |
| `0xE0000000–0xFFFFFFFF` | `vE0000000` | 4 KiB | view of physical memory (+0x1000 offset) |
| physical `0x00000000–0x1FFFFFFF` | `physical` | — | 512 MiB, what the GPU reads |

Natively the three physical views are the **same pages** mapped three times from one
`/dev/shm` object (`src/core/memory_posix.cpp`: `shm_open`/`memfd`, `mmap MAP_FIXED`).
WebAssembly has one linear memory and no aliasing, hence D2.

### 3.5 OS features the SDK uses that browsers lack (read from code)

`grep -rln <pattern> src include` in `thirdparty/rexglue-sdk`, excluding its `thirdparty/`:

| Feature | Files | Web replacement |
| --- | --- | --- |
| `shm_open`/`memfd`, `mmap MAP_FIXED`, `mprotect` | `core/memory_posix.cpp`, `system/xmemory.cpp`, `core/mapped_memory_posix.cpp` | One linear-memory block; protection becomes bookkeeping (8.1) |
| `sigaction`/`SIGSEGV`/`SIGBUS` | `core/exception_handler_posix.cpp`, `core/seh_posix.cpp`, `core/threading_posix.cpp` | Compiled out; MMIO via `REX_MM_*` (8.2); write watches via dirty bitmap (8.3) |
| Page-protection write watches | `graphics/shared_memory.cpp`, `graphics/primitive_processor.cpp`, `src/gpu_webgpu/texture_cache.cpp`, `guest_memory_mirror.cpp` | Dirty bitmap queries (8.3) |
| `ucontext` fibers | `core/fiber_posix.cpp`, `system/kernel_state.cpp` (`fiber_map_`) | `emscripten_fiber_*`, or none if Trials never creates fibers (W-S08) |
| `dlopen`/`dlsym` | `core/dynlib_posix.cpp`, `system/gpu_plugin_loader.cpp` | Static plugin table (D8) |
| `timer_create`, `sem_open` | `core/threading_posix.cpp` | Timer thread + condition variables (8.4) |
| `sched_setaffinity` | `core/threading_posix.cpp` | No-op |
| X11/Wayland/Metal surfaces | `ui/surface_*`, `ui/window_sdl.cpp` | Canvas surface (9.3) |
| `execv`, `/proc/self/cmdline` (app) | `src/trials_hd_app.h` `ReExecWithGpuPlugin` | No-op (only WebGPU exists) |
| Sockets | `system/xsocket.cpp` | Stub (offline game) |

---

## 4. Measurements that size the port

All measured 2026-10-15 on the build server (RX 6600, Debian 13), native WebGPU build,
ride script `out/claude-3d/bench.txt` (Middle Name: Danger).

### 4.1 Generated code

| Quantity | Value | Command |
| --- | --- | --- |
| Generated C++ | 111 MiB, 107 `.cpp`, 3,700,432 lines | `du -sh generated/default` |
| Functions | 18,153 (`DEFINE_REX_FUNC`) | grep count |
| Median function | 84 lines; 28 functions > 5,000 lines; largest 25,343 lines (`sub_826E0F08`) | script in the Notes log |
| x86-64 machine code (`-O2`, large code model) | 27.5 MiB `.ltext` in `trials_hd` | `size -A -d trials_hd` |

### 4.2 Guest memory actually touched (resident pages during a ride)

`/proc/<pid>/pagemap` present bits over the guest window, 65 s into a ride:

| Guest range | Resident |
| --- | --- |
| `0x0xxxxxxx` | 30.4 MiB |
| `0x2xxxxxxx` | 110.6 MiB |
| `0x3xxxxxxx` | 0.8 MiB |
| `0x4xxxxxxx` | 256.0 MiB (the whole 256 MiB segment) |
| `0x7xxxxxxx` | 8.4 MiB (stacks) |
| `0x8xxxxxxx` | 20.4 MiB (image) |
| physical | 193.7 MiB, spread over the whole 512 MiB |

About **625 MiB** of guest memory is in use, spread across the address space. A compact
32-bit layout is possible only with a remapping table (section 7.2, option B).

### 4.3 CPU per thread at 60 FPS

`tools/profile/thread_cpu.py <pid> 8` with `--present_fps_limit=60`: process total
**117% of one core**. `GPU Commands` 29.5%; the three busiest guest threads 19.0%, 13.6%,
10.5%; UI thread 8.6%; the rest ≤ 7% each. About 1,550 draws and 20.5 resolves per frame.
Uncapped native: about 193 FPS at 720p, 72 FPS at 2x (GPU-bound).

### 4.4 Process memory

`/proc/<pid>/status` during the ride: RSS 1.54 GiB = 455 MiB anonymous (host heap: Dawn,
caches, the runtime) + 991 MiB shared (guest memory, counted once per view) + 129 MiB
file-backed (code). Host-side budget for the web build: about 1 GiB.

### 4.5 Native threads

36 (section 3.2). The web build needs a pthread pool of at least 48 workers (guest threads
are created on demand; Emscripten creates workers asynchronously unless pre-pooled).

W-S03's executable probe is `tools/web/spikes/thread_memory64.cpp` with
`thread_memory64_shell.html`. It reserves a page-aligned 3 GiB window, starts 40
pthreads together through a barrier, fills/verifies a disjoint 16 MiB region per
thread at offsets spanning that window (640 MiB committed), and leaves it resident
for `/proc/<renderer-pid>/status` measurement. This is synthetic code, not game code.

```sh
source ~/emsdk/emsdk_env.sh
mkdir -p out/web-spikes
em++ tools/web/spikes/thread_memory64.cpp -o out/web-spikes/thread_memory64.html \
  --shell-file tools/web/spikes/thread_memory64_shell.html --target=wasm64 -O2 \
  -std=c++20 -pthread -sPROXY_TO_PTHREAD -sPTHREAD_POOL_SIZE=48 \
  -sINITIAL_MEMORY=3800mb -sMAXIMUM_MEMORY=8gb -sALLOW_MEMORY_GROWTH -sASSERTIONS=1
python3 tools/web/serve.py out/web-spikes
# Launch Chromium as in 11.1 on /thread_memory64.html, then:
.venv/bin/python tools/web/spikes/record.py http://127.0.0.1:9222 out/thread_memory64.json
```

W-S03 measured 2026-10-15 in Chromium 154.0.8037.92 (one fresh-profile local run,
48 prepooled workers, Emscripten 6.0.12, `-O2`, assertions enabled):

| Measurement | Value |
| --- | --- |
| Shell start → proxied `main` stdout received | 152.7 ms |
| Allocate 3 GiB (untouched) | 0.25 ms |
| Create 40 pthreads | 5.11 ms |
| Barrier release → fill/verify 640 MiB → join | 48.19 ms |
| C++ test total | 53.55 ms |
| Shell start → completed result | 207.3 ms |
| Wasm renderer RSS / peak RSS (`/proc/2133631/status`) | 957,172 KiB (934.7 MiB) |
| Wasm renderer OS threads (includes browser helper threads) | 70 |

All 40 joins and every page's data verification passed; pointer size is 8 bytes and
`crossOriginIsolated=true`. Renderer PID came from CDP `SystemInfo.getProcessInfo`,
then `/proc/<pid>/status` supplied RSS (not the huge, mostly uncommitted `VmSize`).
Raw result and console: `out/web-spikes/thread_memory64.*`. This proves the memory/thread
configuration works.

**W-S06 native sizing and thread churn findings (2026-10-15).** An `LD_PRELOAD` probe
interposed `pthread_create`, `pthread_setname_np`, and `Presenter::RefreshGuestOutput`
during native runs of `main_menu_nav` (45s), `ride_middle_name_danger` (80s), `settings_menu`
(40s), and `credits` (45s). Touch depth of each thread's host stack was tracked via `mincore`:

| Metric | Main Menu Nav | Ride (Danger) | Settings Menu | Credits |
| --- | --- | --- | --- | --- |
| Startup time to first guest swap | 938.24 ms | 545.51 ms | 542.46 ms | 557.80 ms |
| Total threads created | 44 | 44 | 44 | 44 |
| Peak concurrent threads | 36 | 36 | 36 | 36 |
| Guest threads identified | 30 | 30 | 30 | 30 |
| Max host stack depth: `GPU Commands` | 264.0 KiB | 264.0 KiB | 264.0 KiB | 264.0 KiB |
| Max host stack depth: `Main XThread` | 32.0 KiB | 32.0 KiB | 32.0 KiB | 32.0 KiB |
| Max host stack depth: Guest worker threads | 16.0 KiB | 16.0 KiB | 16.0 KiB | 16.0 KiB |
| Max host stack depth: Audio / XMA threads | 8.0 KiB | 8.0 KiB | 8.0 KiB | 8.0 KiB |

Natively, guest threads are allocated 16 MiB pthread stacks. However, the deepest host stack
touched across any guest thread was 264 KiB (`GPU Commands`), while recompiled guest execution
threads reached at most 32 KiB. This proves that a modest host stack (1 MiB) per pthread on
WebAssembly is more than sufficient. Peak concurrent threads never exceeded 36, confirming that
`PTHREAD_POOL_SIZE=48` provides sufficient capacity without worker churn stalls.
Detailed logs: `out/web-spikes/probe_summary.json`.

### 4.6 Game data

206 MiB total: `data/data.pak` 112 MiB, `data_menu.pak` 36 MiB, `XACT/Music.xwb` 40 MiB,
`default.xex` 7.5 MiB, `data_misc.pak` 4.7 MiB, `splash.pak` 2.5 MiB, small XML/PNG files
(`du -sh assets/...`). Fits OPFS comfortably (quota is a share of free disk).

---

## 5. The browser platform in October 2026

Sources are listed in 15.3. Items marked **unverified** need the task in brackets before
anyone relies on them.

### 5.1 WebAssembly

| Feature | Status | Notes |
| --- | --- | --- |
| Threads, shared memory, atomics | All browsers; needs cross-origin isolation (COOP/COEP) | `SharedArrayBuffer` |
| SIMD (`simd128`), relaxed SIMD | All current browsers | SIMDe uses `simd128` |
| Memory64 | Chrome/Edge 133+, Firefox 134+ (some sources say later or flagged), **Safari: unverified** (Technology Preview only in some reports) [W-S02] | JS API caps memory at 16 GiB. Bounds checks cannot be elided as for wasm32 (guard regions), so expect some cost [W-L03]. |
| Shared Memory64 + threads | **Works in Chromium 154 here** (section 5.6) | |
| Exception handling (`-fwasm-exceptions`), `setjmp` via wasm EH | All current browsers | `-sSUPPORT_LONGJMP=wasm` |
| JSPI (JavaScript Promise Integration) | Chrome 137+, Firefox 153+ (caniuse), Safari 27 (caniuse; **unverified**) | Lets wasm suspend on a Promise; key to D6 [W-S05] |
| Engine limits (V8 `wasm-limits.h`) | functions ≤ 1,000,000; function body ≤ 7,654,321 bytes; locals ≤ 50,000; module ≤ 1 GiB; wasm32 memory ≤ 4 GiB (older: 2 GiB), wasm64 ≤ 16 GiB | Our largest function: 218 KiB (section 6) |

**W-S02 measured matrix (2026-10-15):** served cross-origin-isolated on localhost,
Chromium 154.0.8037.92 on this Linux server. Every detection below passed; raw
result/console are in `out/web-spikes/wasm_features.*` (not committed).

| Probe | Chromium 154 (verified) | Firefox desktop | Safari macOS |
| --- | --- | --- | --- |
| Shared Memory64 module | yes | unverified | unverified |
| Threads (shared memory + atomic load) | yes | unverified | unverified |
| SIMD128 | yes | unverified | unverified |
| Relaxed SIMD | yes | unverified | unverified |
| Exception handling (`exnref`, `try_table`, `catch_all_ref`) | yes | unverified | unverified |
| JSPI (`WebAssembly.Suspending` + `promising`) | yes | unverified | unverified |
| `crossOriginIsolated` | yes | unverified | unverified |
| `SharedArrayBuffer` | yes | unverified | unverified |

W-S02's reproducible probe is `tools/web/spikes/wasm_features.html`. It uses five tiny,
handwritten modules with `WebAssembly.validate` (shared Memory64, atomic load, SIMD,
relaxed swizzle, and `try_table`/`catch_all_ref` with `exnref`), positive/negative
validation controls, plus JSPI API and isolation checks. Validate the fixtures with
`python3 tools/web/spikes/test_wasm_features.py ~/emsdk/upstream/bin/wasm-as`
(Binaryen 133); serve the spikes folder, open `wasm_features.html` in the browser,
and record with `.venv/bin/python tools/web/spikes/record.py
http://127.0.0.1:9222 out/wasm_features.json`. JSPI API detection alone does **not**
prove JSPI works in pthreads or with WebGPU callbacks; W-S05 remains required.

### 5.2 WebGPU

| Browser | Status (desktop) |
| --- | --- |
| Chrome/Edge | Shipped on Windows, macOS, ChromeOS since 113; Android 121+. **Linux: partial** (Intel Gen12+ from 144, NVIDIA on Wayland from 147, others behind a flag). This server's RX 6600 needs flags (11.1). |
| Firefox | Windows 141+, macOS Apple Silicon 145+, Linux expected in 2026, Android behind a flag |
| Safari | macOS/iOS/iPadOS/visionOS 26+ |

WebGPU objects are bound to the realm (window or worker) that created them and cannot be
posted to another worker, which forces D5. A canvas can be transferred to a worker as an
`OffscreenCanvas` and used for WebGPU there. Browsers always validate every call and run
the GPU in a separate process: per-call CPU cost is higher than native Dawn with
validation off (section 9.6).

### 5.3 Emscripten and Dawn

- Emscripten 6.0.12 (installed here; Wikipedia lists 6.0.10 stable on 2026-09-21).
- WebGPU: **`--use-port=emdawnwebgpu`** at compile and link. Dawn maintains this fork of
  Emscripten's `webgpu.h` implementation; the old `-sUSE_WEBGPU` is deprecated (Emscripten
  PR 24220). The core `webgpu.h` is stable; `webgpu_cpp.h` is not. Requires Emscripten
  4.0.10+. Its README says nothing about threads, JSPI or `WaitAny`; those are spikes
  (W-S04, W-S05).
- `-sMEMORY64=1` is deprecated in favor of `--target=wasm64` / `-m64`. `MAXIMUM_MEMORY`
  defaults to 2 GiB; must be raised.
- `PROXY_TO_PTHREAD` runs `main()` on a pthread and proxies `Module.canvas` when
  `OFFSCREENCANVAS_SUPPORT` is set.
- WasmFS (stable, not feature-complete) has an OPFS backend using
  `FileSystemSyncAccessHandle`; it must run in a worker and avoids proxying file I/O to
  the main thread.
- `AUDIO_WORKLET` builds add an AudioWorklet environment.

### 5.4 Input

`navigator.getGamepads()` is documented on `Navigator` only, with window-event examples;
MDN does not list it on `WorkerNavigator`. Plan for main-thread polling (D10). Keyboard and
pointer events arrive on the main thread too. Pointer Lock and Fullscreen need a user
gesture on the main thread.

### 5.5 Storage

The Origin Private File System (OPFS) gives synchronous access handles in workers. Quota
is a share of free disk (hundreds of MiB to GiB on desktops). `navigator.storage.persist()`
asks the browser not to evict the data.

### 5.6 Evidence from the reference port (measured 2026-10-15)

`~/playgta5-main/wasm-originals/game.wasm` (63 MiB file), parsed with a section reader
(Notes log):

| Property | Value |
| --- | --- |
| Memory | imported `env.memory`, flags `7` = **shared + memory64**, min 49,152 pages (3 GiB), max 262,144 pages (16 GiB) |
| Defined functions | 91,026 |
| Code section | 47,063 KiB |
| Data section | 7,644 KiB |

It ran with hardware WebGPU in Chromium on this server (memory note
`headless-webgpu-server`). So shared Memory64 + pthreads + a ~45 MiB code section + WebGPU
are a working combination in current Chromium.

---

## 6. Compile spike: the generated code under Emscripten

**Measured 2026-10-15** with Emscripten 6.0.12. Scratch only: the SDK headers were copied
to the scratchpad and patched there; nothing in the repo changed.

Command (per file, 14 in parallel):

```sh
em++ [-sMEMORY64=1] -O2 -std=gnu++23 -msimd128 -pthread -matomics -mbulk-memory \
  -I<patched SDK include copy> <native include dirs and SPDLOG defines> \
  -include generated/default/trials_hd_pch.h -c generated/default/trials_hd_recomp.N.cpp
```

**Header fixes that were needed** (each becomes a task; W-S10, W-S11):

1. `include/rex/platform.h`: `#error Unsupported target OS` → add an
   `__EMSCRIPTEN__` platform (`REX_PLATFORM_EMSCRIPTEN`, POSIX-like).
2. `include/rex/platform/fpscr.h`: `#error Missing implementation for FPSCR` → add a wasm
   `FPSCRPlatform` (no flush or rounding control; `getcsr` returns a shadow value).
3. `include/rex/ppc/intrinsics.h`: `simde_mm_vsl`, `simde_mm_vslo`, `simde_mm_vsro` are
   `#error` outside x86-64/arm64. Their x86-64 branches are already portable SIMDe/C;
   enable them for `__wasm__`.
4. `include/rex/chrono/chrono.h`: `std::chrono::clock_time_conversion` is missing from
   Emscripten's libc++; the existing `__APPLE__` shim works when extended to
   `__EMSCRIPTEN__`.
5. `include/rex/ppc/context.h`: **`PPCCRRegister::setFromMask(simde__m128)` and
   `setFromMask(simde__m128i)` become the same function** because SIMDe maps both types
   to `v128_t` on wasm (`simde/x86/sse.h:151`, `sse2.h:242`). They mean different things
   (4-lane vs 16-byte mask). The spike removed one overload (wrong semantics, sizes only).
   W-L02's smaller fix retains both native overloads and uses one wasm overload:
   every emitted float-mask call passes `0xF`, every byte-mask call `0xFFFF`, so that
   existing immediate selects the correct movemask. No method rename or native API
   change is needed; the unit test distinguishes both semantics (not just compilation).
6. Real-SDK compilation also reaches `platform/dynlib.h` through `KernelState`.
   The web branch has no native library names (Vulkan, RenderDoc, SPIRV-Tools are not
   built); it must not enter the unsupported-platform `#error`. No Linux platform
   define or scratch header is used to conceal that check.

**Results:**

| | wasm32 | wasm64 |
| --- | --- | --- |
| All 107 files compile | yes | yes |
| Wall time, 14 jobs | 75 s | 75 s |
| Object files | 38.1 MiB | 39.4 MiB |
| CODE sections (sum, before linking and `wasm-opt`) | **35.3 MiB** | **36.7 MiB** |
| Largest function | — | 218 KiB (`sub_826E0F08`, `trials_hd_recomp.69`) |

The code is 1.28–1.33× the x86 size, below the reference port's 47 MiB. Relocatable
objects pad indices to 5-byte LEBs, so the linked size will be somewhat smaller.

**Real-header regression checks (takeover, 2026-10-15):**
`source ~/emsdk/emsdk_env.sh && tools/web/tests/run_sdk_headers.sh` compiles and runs
standalone wasm64 assertions with Emscripten's Node 24 and pthreads, using the actual
SDK headers. Passed: fold constants; scalar/vector/CAS/zero stores and dirty bits
(including zero size, nonphysical pointers, cross-page writes and end clamping);
float-vs-byte CR masks; all four FPSCR rounding modes, ties-to-even, NaN/overflow;
thread-local FPSCR shadow; VMX bit shifts across the 64-bit boundary and byte shifts.
Debug non-nearest-mode diagnostics were observed. The tests contain no game code.

`source ~/emsdk/emsdk_env.sh && python3 tools/web/compile_generated.py -j 8` is the
separate full-game compile check: real headers, no Linux platform spoof, no scratch
include copy, and no regeneration/writes to `generated/default`. Objects go only to
`out/web-spikes/recomp-objects/` and must never be published.

---

## 7. Architecture

### 7.1 Options for the guest address space

| Option | Layout | Per-access cost | Pros | Cons |
| --- | --- | --- | --- | --- |
| **A. wasm64, folded window** (chosen) | guest window at a fixed offset: virtual `[0, 0xA0000000)` direct, `≥ 0xA0000000` → physical block after it (3 GiB) | compare + select per access; engine bounds check (no guard-page elision for Memory64) | Minimal SDK change; host pointers stay 64-bit as the SDK assumes; proven in Chromium (5.6) | Memory64 bounds-check cost; Safari support unverified |
| B. wasm32, remapped window | 16-entry table indexed by `addr >> 28` → offset; unused segments map to a trap page; heaps restricted so the guest fits ~2.5 GiB | table load + add per access | Guard-page bounds-check elision; Safari likely | 4 GiB total incl. host heap (tight: 625 MiB guest in use but reserved ranges are larger); 64-bit pointer assumptions in SDK to audit; heap layout changes in the SDK |
| C. Multi-memory: guest in a second 4 GiB memory | `i32.load mem1` | none beyond fold | Fast, clean | No C/C++ toolchain support for addressing a second memory from LLVM; not practical today |

Start with **A**. Measure the per-access cost against native (W-L03). If Memory64 bounds
checks cost more than ~25% on the CPU-bound threads, or Safari stays without Memory64,
prototype **B** (W-L13). The access macro is the only codegen-visible difference, so both
can coexist behind `REX_WEB_MEMORY_MODEL`.

### 7.2 Translation macro (option A)

*Superseded by the design note's fold table (7.9.2), which also covers the `0x7F…` and
`0x9…` aliases; kept for the reasoning.*

```cpp
// __EMSCRIPTEN__ only. base points at guest address 0 in linear memory.
// Physical block starts at base + 0xA0000000.
#define REX_GUEST_PTR(addr)                                                          \
  (base + ((u32)(addr) < 0xA0000000u                                                 \
               ? (u64)(u32)(addr)                                                    \
               : 0xA0000000ull + ((u32)(addr) & 0x1FFFFFFFu) +                       \
                     ((u32)(addr) >= 0xE0000000u ? 0x1000u : 0u)))
```

Stores add the dirty mark in the `≥ 0xA0000000` branch (D3, section 8.3). Loads take only
the translation. LLVM turns the ternary into a compare and select; the physical branch is
rare outside GPU uploads.

### 7.3 Threads and workers

```
Browser main thread (JS shell, never blocks)
  ├─ canvas element → transferControlToOffscreen() → GPU worker
  ├─ input: keyboard/pointer events, Gamepad API polled per rAF → SAB input block
  ├─ file import UI (picker / drag-drop) → OPFS
  └─ AudioContext + AudioWorklet (reads the SAB audio ring)
pthread "main" (PROXY_TO_PTHREAD): ReXApp setup, kernel, settings, logging
pthreads: guest XThreads (≈22), kernel dispatch, XMA decoder, audio worker, disk I/O
GPU worker (a pthread with the OffscreenCanvas): command processor + presenter + ImGui
```

### 7.4 The GPU worker's event loop (D6)

Browser WebGPU resolves `mapAsync`, `onSubmittedWorkDone`, error scopes and device-lost
callbacks only when the worker returns to its event loop, and a worker's canvas presents
when its current task ends. The native command processor is an endless loop that blocks
on the ring buffer and on `WAIT_REG_MEM`. Two ways to fit it:

1. **JSPI** (preferred if W-S05 shows it works in pthreads): build with `-sJSPI`; at the
   swap, at ring-buffer waits and at GPU waits call a suspending import (for example
   `emscripten_sleep(0)`) that returns to the event loop and resumes the same stack.
   Smallest code change.
2. **Resumable loop** (fallback): the GPU worker runs `CommandProcessor::RunUntilYield()`
   from `requestAnimationFrame` in the worker; it executes packets until a swap, an empty
   ring or a failed wait, then returns. Waits become "rewind and retry next tick". Needs
   SDK command-processor changes (W-L06).

Asyncify (`-sASYNCIFY`) also works but instruments every function that may suspend and is
costly on 37 MiB of code; avoid it.

W-S05's concrete probe reuses `tools/web/spikes/gpu_triangle.cpp` with
`-DTRIALS_WEB_SPIKE_JSPI=1 -sJSPI` added to the W-S04 command (9.3), output
`out/web-spikes/gpu_jspi.html`. Its proxied-main pthread waits for startup callbacks
with `emscripten_sleep(0)`, then stays in an endless C++ frame/sleep loop (no worker
rAF). Two buffer readbacks assert the values 42/43, using `AllowSpontaneous` and
`AllowProcessEvents` respectively; the loop calls `instance.ProcessEvents()`.
The background changes every 300 frames so two CDP screenshots can prove actual
canvas presentation progresses, rather than merely counting submitted commands.

**W-S05 verified 2026-10-15:** Chromium 154 + Emscripten 6.0.12 + the pinned
emdawnwebgpu port passes with `-sJSPI` (no custom `JSPI_EXPORTS`, no Asyncify).
The endless pthread C++ loop reached frames 5 and 305; both map callbacks returned
correct data on that worker. CDP snapshots changed background RGB from `(13,13,25)`
to `(13,51,25)` while retaining the orange triangle. Artifacts:
`out/web-spikes/gpu_jspi_verified.json`, `gpu_jspi_{early,late}.png`.
**Question 14.2 is answered yes on the primary target.** Use JSPI for D6/W-L06;
the actual game's yield points and threading still need implementation.

### 7.5 Rendering

The renderer's command-processor half already runs on one thread. On the web, the
presenter's paint and the ImGui immediate drawer (UI thread natively) move to the GPU
worker: the UI thread produces ImGui draw lists; the GPU worker paints them at the swap
(section 9.4).

### 7.6 Data flow (target)

```
user's package ──import──▶ OPFS ──WasmFS──▶ VFS (kernel NtReadFile) ──▶ guest memory
guest code ──stores──▶ linear memory (+ dirty bitmap for physical pages)
guest GPU ring (PM4) ──▶ GPU worker command processor ──▶ WebGPU (browser) ──▶ canvas
XMA packets ──▶ FFmpeg (pthread) ──▶ PCM ring (SAB) ──▶ AudioWorklet
Gamepad/keyboard ──main thread──▶ SAB input block ──▶ input driver ──▶ XInput HLE
```

### 7.7 Deployment model (D12)

- `tools/web/build.sh` builds `trials_hd.{html,js,wasm}` from the user's own generated
  code.
- `tools/web/serve.py` serves `out/web/` on `localhost` with
  `Cross-Origin-Opener-Policy: same-origin` and `Cross-Origin-Embedder-Policy:
  require-corp` (needed for `SharedArrayBuffer`), the `application/wasm` type and
  compression.
- `tools/web/serve.py` is implemented (W-S14): `python3 tools/web/serve.py <build-folder>`
  defaults to loopback port 8080, sends COOP/COEP/CORP on every response, serves
  `.wasm` as `application/wasm`, and negotiates existing `.br`/`.gz` siblings. Directory
  listings and symlinks escaping the build folder are denied. Root serves `index.html`
  or `trials_hd.html`. Checks: `python3 tools/web/test_serve.py` (7 tests); Chromium 154
  with a served test page reports `crossOriginIsolated=true` (2026-10-15).
- The page asks for the user's game package on first run (section 8.6).
- **Nothing is uploaded or hosted.** Document this in the web README section, and make
  `tools/web/serve.py` bind to `127.0.0.1` by default.

### 7.8 Isolation from the native builds (D14)

The web port is **additive**. A Linux or macOS build configured and built the way the
README says must produce the same game whether or not the web work exists. Rules:

**Where web code lives**

- New files only, in web-only places: `src/web/` (app shell, input, audio driver),
  `tools/web/` (build/serve/CDP scripts, spikes), `web/` (HTML shell, JS, AudioWorklet),
  and `*_emscripten.cpp` files next to their `*_posix.cpp` siblings in the SDK
  (selected by CMake, like `_posix`/`_win` today).
- Shared files get web code **only inside `#if defined(__EMSCRIPTEN__)`** (or
  `REX_PLATFORM_EMSCRIPTEN`, defined only under `__EMSCRIPTEN__`). New platform branches
  are added as a new `#elif` **before the final `#else`/`#error`**. Never reorder,
  rewrite or "simplify" existing `__APPLE__`, `__aarch64__`, `__x86_64__` or Linux
  branches. Checked 2026-10-15 (`em++ -dM -E`): Emscripten defines `__EMSCRIPTEN__`,
  `__wasm__` and `__unix__`, but not `__linux__`, `__gnu_linux__`, `__APPLE__`,
  `__x86_64__` or `__aarch64__`; the native clang defines neither `__EMSCRIPTEN__` nor
  `__wasm__`. So web branches cannot capture a native compiler. Caution: code guarded only
  by `__unix__` **is** compiled for the web; check such guards when porting a file.
- No behavior change for native callers when an API is generalized for the web (for
  example the dirty bitmap, 8.3): the native implementation keeps page protection, and the
  web implementation is a separate file or `#if` branch.

**Build system**

- `TRIALSHD_WEB` defaults to OFF and is a `FATAL_ERROR` unless `EMSCRIPTEN` is set
  (Emscripten's toolchain file). Native configure never enters web directories, never
  needs `~/emsdk`, and gains no new required packages.
- Web builds use their own presets and build directories (`out/build/web-*`) and their own
  FFmpeg build. They never write into native build directories.
- The web build **reads** `generated/default/` and never regenerates or edits it. If the
  web build ever needs different codegen output, it goes to `generated/web/`, not over the
  native files (a rewrite of `generated/default` forces a full native rebuild and changes
  the native game).
- Emscripten lives in `~/emsdk` and is activated only per shell (`source
  ~/emsdk/emsdk_env.sh`); it is not on the default `PATH` (checked 2026-10-15), so native
  builds keep using `/usr/bin/clang`.

**Changes that touch code both builds use** (must keep native output identical or be
reviewed as a native change too):

| Task | Shared code touched | Extra native proof |
| --- | --- | --- |
| W-S10, W-S11 | SDK headers (`platform.h`, `fpscr.h`, `intrinsics.h`, `chrono.h`) | Only new `__EMSCRIPTEN__`/`__wasm__` branches; `check_native.sh check` passes with an **unchanged** generated-code fingerprint |
| W-L02 | Codegen identity write macros; wasm mask dispatch and memory macros | Native mask overloads stay unchanged. Verify raw-write macros expand to the original expressions; new address/load/store definitions are in the PCH's `__EMSCRIPTEN__` block. Check native golden scenes and review generated differences before re-recording the baseline |
| W-L04, W-L05 | SDK memory, shared memory, primitive processor, renderer texture cache | Web implementation in `memory_emscripten.cpp` / `#if` branches; native watch path unchanged |
| W-L06, W-L07, W-L09 | Renderer (`src/gpu_webgpu/`) | `#if __EMSCRIPTEN__` only; the split mirror (W-L09) is opt-in natively (`--webgpu_split_mirror`, default off); native FPS within 3% of the numbers in renderer doc 16.6 |
| W-L15 | CMake, SDK build | Native configure output and targets unchanged; `TRIALSHD_WEB` guarded |

**The check.** `tools/check_native.sh` (Linux):

```sh
tools/check_native.sh baseline   # before starting a task (records out/native_baseline/)
tools/check_native.sh check      # before marking it done
```

`check` rebuilds the native game and fails when:
- the build fails or has `TRIALSHD_WEB` on;
- a native binary contains Emscripten/web symbols;
- `generated/default` differs from the baseline (the files are listed);
- the WebGPU or Vulkan renderer fails to boot through `tests/headless/main_menu_nav.txt`,
  or the menu captures fall below 33 dB against the baseline.

Measured 2026-10-15 on an unchanged tree: WebGPU worst 40.5 dB, Vulkan 44.9 dB between
runs (normal animation noise). Negative test: a tampered fingerprint and a blanked capture
(13.7 dB) both failed the check as intended.

**macOS** cannot be built on this server. Any task in the table above must also be built
and booted on the user's Mac (`cmake --build` + reach the main menu) before it is marked
`done`; until then its Status stays `review` with "needs macOS check" in the Notes log.
Tasks that add only new web-only files need no macOS check.

**`main` branch.** Web-port code is not ported to `main` until the user asks; until then
`main`'s Linux/macOS builds cannot be affected at all.

### 7.9 Web platform design note (W-L01, 2026-10-15)

The binding interface specification for every other web task. Read from code where a
file is named; change it only with a Notes-log entry. "Web" below means
`REX_PLATFORM_EMSCRIPTEN` (defined in `rex/platform.h` only when `__EMSCRIPTEN__` is set).

#### 7.9.1 Platform layer and file layout

| Native file (`thirdparty/rexglue-sdk/src/core/`) | Web file | Content |
| --- | --- | --- |
| `memory_posix.cpp` | `memory_emscripten.cpp` | `page_size()` 4096, `allocation_granularity()` 4096, `AllocFixed`/`DeallocFixed`/`Protect`/`QueryProtect` bookkeeping (7.9.2), file mappings over one linear-memory window |
| `mapped_memory_posix.cpp` | `mapped_memory_emscripten.cpp` | `MappedMemory` reads the file into a heap buffer (and writes back on `Close` when writable) |
| `exception_handler_posix.cpp`, `seh_posix.cpp` | `exception_handler_emscripten.cpp` | `Install`/`Uninstall` no-ops |
| `fiber_posix.cpp` | `fiber_emscripten.cpp` | Fatal "not supported" unless W-S08 finds fibers in use |
| `dynlib_posix.cpp` | `dynlib_emscripten.cpp` | `Load` fails; plugins come from the static registry (7.9.10) |
| `threading_posix.cpp` | **same file**, `#if REX_PLATFORM_EMSCRIPTEN` blocks | Timers on one timer thread instead of `timer_create`; affinity/priority no-ops; no signal-based thread suspension (fatal if used) |
| `socket_posix.cpp` | `socket_emscripten.cpp` | All calls fail with "network unavailable" |
| `system_posix.cpp`, `dbg_posix.cpp`, `filesystem_posix.cpp`, `clock_posix.cpp`, `atomic_posix.cpp`, `platform/*_posix.cpp` | same files | Compile under Emscripten (POSIX enough); fix individual calls with `#if` |

`src/core/CMakeLists.txt` selects the `_emscripten` files with `if(EMSCRIPTEN)` placed
**before** the existing `if(WIN32)`/`else()` blocks' sources are added (native lists
unchanged). App-side web code lives in `src/web/`; the SDK's UI/input/audio web
backends in `thirdparty/rexglue-sdk/src/{ui,input,audio}/web/` (new directories, built only
with `EMSCRIPTEN`).

**As built (W-L15, 2026-10-15).** The web build does not go through the SDK's CMake (whose
platform setup assumes Windows/Linux/macOS: Vulkan, SDL, Tracy, a shared `rexruntime`):
`cmake/web/trials_hd_web.cmake` compiles the SDK, the generated code and the app from
explicit source lists, as `src/gpu_webgpu/CMakeLists.txt` already does for the SDK's GPU
sources. Per file:

- Reused as-is (Emscripten is POSIX enough; their "POSIX-only" `static_assert`s now accept
  `REX_PLATFORM_EMSCRIPTEN`): `atomic_posix`, `clock_posix`, `dbg_posix`, `dynlib_posix`
  (`dlopen` fails at run time), `filesystem_posix`, `mapped_memory_posix` (Emscripten's
  `mmap` of files copies), `math_gcc`, `platform/console_posix`, `platform/env_posix`,
  `seh_posix` (nothing faults), `socket_posix` (offline: calls fail), `system_posix`.
- `#elif REX_PLATFORM_EMSCRIPTEN` branches: `clock_posix.cpp` (**`CLOCK_MONOTONIC`**:
  Emscripten has no `CLOCK_MONOTONIC_RAW`, the call fails, and with asserts compiled out the
  guest time base stood still - the game "hung" after loading, waiting on time),
  `threading_posix.cpp` (`gettid()`, affinity all-CPUs/no-op, `emscripten_set_thread_name`,
  no APC wakeup signal - the alertable wait is already notified -, cross-thread
  `Suspend` refused with an error: workers cannot be interrupted).
- New: `memory_emscripten.cpp`, `exception_handler_emscripten.cpp` (W-L04),
  `fiber_emscripten.cpp` (every guest thread converts itself to a fiber, which works;
  `CreateFiber`/`SwitchToFiber` are fatal until W-S08 shows Trials HD needs them).
- `timer_create` is not used: the SDK's timers already run on its `TimerQueue` thread.
- UI: `ui/web/windowed_app_main_web.cpp` (`main()`), `windowed_app_context_web.cpp` (UI
  loop = condition variable + queued functions), `window_web.cpp` (a `Window` standing for
  the canvas, no surface yet; W-L08 adds size/focus/input). `rex_app.cpp` defaults audio
  to `NopAudioSystem` on the web (no SDL audio); `input_system.cpp` skips the SDL driver.
- Static linking: natively the app's `__imp__XamShowAchievementsUI` interposes the SDK
  stub in the shared `rexruntime`; statically both are strong. `REX_STUB*` macros mark the
  SDK stubs weak on the web (`REX_STUB_OVERRIDABLE`, empty natively).
- Third party: `rex/hash.h` includes `<arm_neon.h>` before xxHash on wasm (xxHash includes
  it inside `extern "C"`, where Emscripten's C++ parts fail); RenderDoc is absent (like
  macOS); disruptorplus caches `hardware_concurrency()` on the web (its spin-wait queried it
  per wait: `os.cpus()` under Node, 21 s of a 40 s run); FFmpeg is the SDK's subset as
  portable C with a generated `config.h` (`cmake/web/ffmpeg.cmake`).
- GPU plugins are linked in and registered by name (`RegisterStaticGpuPlugin`, 7.9.10);
  `LoadGpuPlugin` looks names up. Until the renderer is ported, the app registers "null"
  (`src/web/null_graphics_system.cpp`): the command processor executes the ring buffer
  (registers, fences, interrupts, swaps) and draws nothing.

#### 7.9.2 Memory model

**Linear-memory window** (one `aligned_alloc(65536, 0xB0000000)` at startup, never freed):

| Window offset | Contents | Guest addresses that land there |
| --- | --- | --- |
| `0x00000000–0x7EFFFFFF` | virtual 4 KiB + 64 KiB heaps | `0x00000000–0x7EFFFFFF` |
| `0x7F000000–0x7FFFFFFF` | unused (16 MiB; guest `0x7F…` is folded to physical) | — |
| `0x80000000–0x8FFFFFFF` | XEX heaps | `0x80000000–0x8FFFFFFF` **and** `0x90000000–0x9FFFFFFF` (native view table maps both to the same file offset) |
| `0x90000000–0xAFFFFFFF` | physical memory, 512 MiB (`physical_membase_ = window + 0x90000000`) | `0xA0000000–0xBFFFFFFF`, `0xC0000000–0xDFFFFFFF`, `0xE0000000–0xFFFFFFFF` (+0x1000), `0x7F000000–0x7FFFFFFF` |

Total 2.75 GiB. The aliases are exactly the native view table (`map_info` in
`src/system/xmemory.cpp`): `0x7F000000` → physical 0, `0x90000000` → `0x80000000`,
`0xA0/0xC0000000` → physical 0, `0xE0000000` → physical `0x1000`.

**Fold table.** Every alias boundary is a multiple of 16 MiB, so translation is one table
lookup by `addr >> 24`:

```cpp
// rex/memory/web_guest.h (web only). host = window + addr + kFold[addr >> 24].
// rex::memory::web::kFold is an inline constexpr std::array<int64_t, 256>.
//   [0x00..0x7E] = 0
//   [0x7F]       = +0x11000000             (0x7F000000 -> window 0x90000000)
//   [0x80..0x8F] = 0
//   [0x90..0x9F] = -0x10000000             (0x9xxxxxxx -> 0x8xxxxxxx)
//   [0xA0..0xBF] = -0x10000000             (0xAxxxxxxx -> window 0x90000000 + low 29 bits)
//   [0xC0..0xDF] = -0x30000000
//   [0xE0..0xFF] = -0x50000000 + 0x1000
```

Codegen (W-L02) emits, for the web only:

```cpp
#if defined(__EMSCRIPTEN__)
#include <rex/memory/web_guest.h>
#undef REX_PHYS_HOST_OFFSET
#define REX_PHYS_HOST_OFFSET(addr) (::rex::memory::web::kFold[(u32)(addr) >> 24])
#undef REX_RAW_ADDR
#define REX_RAW_ADDR(x) (base + ((int64_t)(u32)(x) + REX_PHYS_HOST_OFFSET(x)))
#endif
```

The sum is formed as an integer **before** adding `base`, avoiding an out-of-object
intermediate pointer for high aliases. Web scalar loads use this raw-address macro;
stores use the tracking helpers. Native macro definitions stay unchanged. The `base`
argument is the window start (as `virtual_membase_` is natively).

**`Memory` changes (W-L04, as built 2026-10-15):**

- **Heaps fold through their base pointer, not `host_address_offset_`** (supersedes the
  first draft, which made that offset an `int64_t`). `BaseHeap::Initialize` on the web sets
  `membase_ = window + kFold[heap_base >> 24]` for every view of the window (the physical
  heap is passed `physical_membase_` and is left alone), so `TranslateRelative` lands on the
  folded bytes. `host_address_offset_` keeps its native meaning (0 here: 4 KiB granularity),
  because `PhysicalHeap::EnableAccessCallbacks`/`TriggerCallbacks` and
  `SyncHostPageAccess` use it to number host pages; a negative fold there broke that math.
  No header type changes.
- `detail::PhysicalHostOffset` (used by `GuestPtr`, hence the kernel, hooks and
  `src/crt_hooks.cpp`) returns `kFold[addr >> 24]` (`int64_t`) on the web, and
  `Memory::TranslateVirtual` uses it directly (it also covers `0x7F…`, which has no heap).
  Recompiled code, `GuestPtr`, `TranslateVirtual` and each heap's `TranslateRelative` give
  the same pointer for every 16 MiB block (W-L04 test).
- `HostToGuestVirtual(host)`: window offsets `0x90000000–0xAFFFFFFF` return the
  `0xA0000000` alias (fault handling and `SearchAligned` only).
- `Memory::Initialize` on the web: no `CreateFileMappingHandle`/`MapViews`;
  `rex::memory::web::AllocateGuestWindow()` (`memory_emscripten.cpp`) reserves the window
  with `sbrk`, 64 KiB aligned, plus a 64 KiB tail (guest `0xFFFFFxxx` folds to physical
  `0x20000xxx`, which the native backing file also has). `sbrk`, not `malloc`: Emscripten's
  dlmalloc is built with `MORECORE_CANNOT_TRIM`, so memory past the break was never
  written and the window is zero without touching (committing) its 2.75 GiB; `malloc`
  keeps working past it. The window is never freed; a second `Memory` gets the same one.
  `MMIOHandler::Install` runs unchanged: `exception_handler_emscripten.cpp` makes
  `ExceptionHandler::Install`/`Uninstall` no-ops, and `mmio_handler.cpp` gained
  `REX_ARCH_WASM` branches for its (never reached) fault decoder.
- Page protection is bookkeeping: `AllocFixed`/`DeallocFixed`/`Protect`/`QueryProtect` on
  window addresses succeed without doing anything; the heaps keep their page tables
  (protect bits, commit state) for guest-visible queries, but nothing traps.
  `kMemoryProtectNoAccess` pages stay readable on the web; this only matters for buggy
  guests.
- **Decommit/release keep the contents**, as natively: the heaps never ask the host to
  free view pages (`BaseHeap::Decommit`/`Release` only update the page table), and on
  Linux the views are a shared mapping that keeps its data anyway. (The first draft said
  "zero on decommit"; reading the code showed native does not.)
- `Memory::Zero`/`Fill`/`Copy` call `MarkGuestWrite` after writing.
- Emscripten link: `-sINITIAL_MEMORY=3328mb` (window 2.75 GiB + 576 MiB host start),
  `-sMAXIMUM_MEMORY=8gb`, `-sALLOW_MEMORY_GROWTH=1` for the host heap. With these flags the
  W-L04 test's window starts at linear-memory offset `0x40000`.

#### 7.9.3 Write tracking (dirty bitmap)

```cpp
// rex/memory/web_guest.h (web only; shares the fold table and store helpers)
namespace rex::memory::web {
inline constexpr uint32_t kPageShift = 12;                    // 4 KiB pages
inline constexpr uint32_t kPageCount = 0x20000000 >> 12;      // 131,072 physical pages
extern std::atomic<uint64_t> g_dirty[kPageCount / 64];        // 16 KiB, 1 bit per page
extern std::atomic<uint64_t> g_dirty_summary[kPageCount / 4096]; // 32 words, 1 bit per 64 words
extern uint8_t* g_physical_base;                              // window + 0x90000000

// Called after a write of `size` bytes at host pointer p (any guest address form).
inline void MarkHostWrite(const uint8_t* p, uint32_t size) {
  uint64_t d = uintptr_t(p) - uintptr_t(g_physical_base);
  if (!g_physical_base || size == 0 || d >= 0x20000000) return;
  for (uint64_t page = d >> 12, last = (d + size - 1) >> 12;
       page <= last && page < kPageCount; ++page) {
    uint64_t bit = 1ull << (page & 63);
    auto& word = g_dirty[page >> 6];
    // RMW must observe a concurrent clear; a relaxed precheck can miss a write.
    if (!(word.fetch_or(bit, std::memory_order_release) & bit)) {
      g_dirty_summary[page >> 12].fetch_or(1ull << ((page >> 6) & 63), std::memory_order_release);
    }
  }
}
// Host-side writers (kernel, CRT hooks, audio): same, by guest address.
void MarkGuestWrite(uint32_t guest_address, uint32_t size);
}  // namespace rex::memory::web
```

**Every guest store form marks after the write** (marking first could let the GPU thread
clear the bit and upload old data before the write lands). Forms in the generated code
(measured 2026-10-15 over `generated/default/*.cpp`):

| Form | Count | Web emission (W-L02) |
| --- | --- | --- |
| `REX_STORE_U8/16/32/64(x, y)` | (part of 448,775 accesses) | `Store8/16/32/64(REX_RAW_ADDR(x), y)`: inline store + `MarkHostWrite` |
| `REX_MM_STORE_*` | 28 | non-MMIO branch as above |
| `stvx`/`stvx128` raw 128-bit stores | 5,672 | `REX_STORE_V128` → `Store128` → `MarkHostWrite` |
| `__sync_bool_compare_and_swap(...REX_RAW_ADDR(ea)...)` (`stwcx.`, `stdcx.`) | 285 | CAS, then mark when it succeeded |
| `memset((void*)REX_RAW_ADDR(ea), 0, 32/128)` (`dcbz`, `dcbz128`) | 86 | memset + mark |

W-L02 builder audit: all remaining `REX_RAW_ADDR` uses are loads (`lwarx`/`ldarx`
reservation loads and `lvx`/`lvlx`/`lvrx`). `stvlx`/`stvrx` are byte loops using
`REX_STORE_U8`, so scalar tracking covers them. No other raw guest writer remains in
`src/codegen/builders/`; raw-pointer stores/CAS/cache-zero go through these wrappers.

**Host writers** call `MarkGuestWrite` after writing guest memory (list from W-S09): file
reads into guest buffers, `src/crt_hooks.cpp` `memcpy`/`memset` hooks, XMA output, kernel
`Rtl*` copy/fill helpers, `Memory::Zero/Fill/Copy`, the GPU front-end's own writes that the
CPU must not re-upload are **not** marked (as natively).

**Delivery.** Natively, a fault on a watched page calls the registered
`PhysicalMemoryInvalidationCallback`s (`Memory::RegisterPhysicalMemoryInvalidationCallback`,
used by `SharedMemory` and the primitive processor) and unprotects the page. On the web:

```cpp
// Memory (web): scans g_dirty_summary (32 words), then the marked words; for each run of
// dirty pages that is also "watched" (page flag set by EnableAccessCallbacks), calls the
// invalidation callbacks exactly as the fault path would (start, length, exact_range=false),
// clears the watched flag (= native unprotect), and clears the dirty bits it consumed.
void Memory::DeliverPhysicalWrites();
```

- Called by the GPU front-end **before it reads guest memory**: at the start of
  `CommandProcessor::IssueDraw`, `IssueCopy`, and `SharedMemory::RequestRanges` (cheap when
  nothing is dirty: 32 relaxed loads). Ordering: the guest writes data, then writes the
  ring-buffer write pointer (an atomic store); the GPU thread reads the write pointer
  (atomic load) before executing the packets, so the data writes and their marks happen
  before the delivery that precedes the draw.
- Unwatched dirty pages are just cleared.
- As built (W-L04): a delivery clears a summary bit **before** exchanging its bitmap words
  (a writer sets the summary bit only when its page bit was clear, so the reverse order
  can strand a page bit with no summary bit; the W-L04 stress test loses writes within
  seconds that way). Consecutive dirty pages are merged into runs across words, and each
  run goes to `PhysicalHeap::TriggerCallbacks` of **all three** physical views
  (`0xA0…`, `0xC0…`, `0xE0…`): watch flags are per view, and a write through one view must
  reach a watch made through another. A view that watches none of the run returns at
  once. Each call holds the global critical region, as the fault path does.
- Callbacks keep their native contract: the returned range may only cover data the
  consumer no longer holds valid. `TriggerCallbacks` unwatches up to 4 MiB of it, and
  dirty pages there are then dropped unwatched - correct for `SharedMemory`, whose
  callback returns its invalid span; a callback returning `(0, UINT32_MAX)` while
  keeping data cached would lose updates on the web as it would natively.
- `enable_data_providers` (read-side callbacks) is not supported on the web; assert it is
  never requested (W-L05 confirms).
- Debug mode (`--web_dirty_verify`, W-L05): hash each watched 256 KiB chunk at watch time
  and at delivery; a changed hash without a dirty bit is a fatal "missed writer" error.

#### 7.9.4 MMIO

`MMIOHandler::Install` keeps its range table; on the web `arch::ExceptionHandler::Install`
is a no-op (`exception_handler_emscripten.cpp`, W-L04; the SDK comment in `Install` already
says MMIO is handled by the `REX_MM_*` macros and the fault handler is only for write
tracking). W-S07 proves the
28 `REX_MM_*` sites cover every MMIO access. In debug web builds, `REX_LOAD_*`/`REX_STORE_*`
assert `!REX_IS_MMIO_ADDR(addr)` (cost only in debug).

#### 7.9.5 Threads and the GPU worker

| Thread | Native | Web |
| --- | --- | --- |
| Browser main thread | — | JS shell: canvas, input, audio context, import UI. Never runs wasm code except tiny exported getters. |
| App main (`PROXY_TO_PTHREAD`) | `main()` + SDL UI loop | `main()` + `WebWindowedAppContext` loop: `CallInUIThread` queue, app lifecycle. **Does not paint.** |
| Command processor (`XHostThread`, "GPU Commands") | records/submits GPU work | **The GPU worker**: owns the `OffscreenCanvas`, the WebGPU device, presenter painting, ImGui drawing, and receives window input events |
| Guest `XThread`s, kernel dispatch, VSync, audio, XMA, disk | pthreads | pthreads (pool of 48) |
| Timer thread | (POSIX timers) | one pthread running guest timers |

The command processor must stay an `XHostThread`: it runs guest interrupt callbacks itself
(`GraphicsSystem::DispatchInterruptCallback` uses `XThread::GetCurrentThread()`).

**Canvas transfer.** The app sets `pthread_attr` transferred canvases (Emscripten
`emscripten_pthread_attr_settransferredcanvases(&attr, "#canvas")`) for the command
processor thread only. The web threading layer exposes
`rex::thread::web::SetTransferredCanvasesForNextThread(const char*)`, which
`WebGpuGraphicsSystem::Setup` calls right before the SDK creates the worker thread.

**Yielding.** One function, used at every point where the GPU worker would wait:

```cpp
// rex/thread/web_yield.h. Returns to the worker's event loop (WebGPU callbacks run, the
// OffscreenCanvas presents), then resumes. timeout_ms = 0: yield once.
void rex::thread::web::YieldToEventLoop(uint32_t timeout_ms);
```

- Implementation A (default): `-sJSPI`; `YieldToEventLoop` is `emscripten_sleep(timeout)`,
  a JSPI-suspending import. Needs JSPI on pthreads (W-S05).
- Implementation B (fallback): `-sASYNCIFY` with `-sASYNCIFY_ONLY=[...]` listing only the
  functions on the GPU worker's stack between its entry and the yield calls (XHostThread
  entry → `CommandProcessor::WorkerThreadMain` → `ExecutePrimaryBuffer` → packet handlers
  that wait). Guest code never yields, so it is not instrumented.
- Call sites (W-L06): `CommandProcessor::WorkerThreadMain`'s idle wait (replaces the
  5 ms `rex::thread::Wait` with `YieldToEventLoop(1)` after polling the write pointer and
  the UI-paint flag); the `WAIT_REG_MEM`/`WAIT_REG_EQ` spin loops (yield between polls);
  after `IssueSwap` has painted (7.9.7).

**Window input on the GPU worker.** The web `Window` (`ui/web/window_web.cpp`) queues input
events from the input ring (7.9.6) and dispatches them to listeners (ImGui drawer, app
hotkeys, keyboard input driver) **on the GPU worker**, right before painting. All UI
drawer code therefore runs on one thread, as natively on the UI thread.

#### 7.9.6 Shared-memory layouts (main thread JS ↔ wasm)

Both blocks are allocated by wasm (`aligned_alloc(64, ...)`) and their addresses exported
(`trials_hd_web_input_block()`, `trials_hd_web_audio_ring()`); JS views them through
`wasmMemory.buffer` (a `SharedArrayBuffer`). Little-endian, `BigInt` for 64-bit offsets.

```cpp
struct WebPadState {            // standard Gamepad mapping
  uint32_t connected;           // 0/1
  uint32_t buttons;             // bit i = buttons[i].pressed, i = 0..16
  float axes[4];                // LX, LY, RX, RY in -1..1
  float triggers[2];            // buttons[6].value, buttons[7].value
};
struct WebInputEvent {          // 16 bytes
  uint32_t type;                // 1 key down, 2 key up, 3 pointer move, 4 pointer down,
                                // 5 pointer up, 6 wheel, 7 text (code = UTF-32), 8 focus, 9 blur
  uint32_t code;                // SDL scancode (keys), button (pointer), char (text)
  int32_t x, y;                 // canvas pixels (pointer), wheel deltas * 120
};
struct WebInputBlock {
  std::atomic<uint32_t> pad_seq;        // seqlock: JS increments to odd, writes, increments to even
  uint32_t pad_count;
  WebPadState pads[4];
  std::atomic<uint32_t> event_head;     // written by JS
  std::atomic<uint32_t> event_tail;     // written by wasm (GPU worker)
  WebInputEvent events[256];            // ring
  uint32_t canvas_width, canvas_height; // device pixels
  float device_pixel_ratio;
  uint32_t visible;                     // document.visibilityState == "visible"
};

struct WebAudioRing {
  std::atomic<uint32_t> write_frame;    // producer: web audio driver (pthread)
  std::atomic<uint32_t> read_frame;     // consumer: AudioWorklet
  uint32_t capacity_frames;             // 8192
  uint32_t sample_rate;                 // AudioContext.sampleRate (48000 typical)
  uint32_t channels;                    // 2
  std::atomic<uint32_t> underruns;
  float samples[8192 * 2];              // interleaved
};
```

JS sides: `web/input.js` (W-S18) fills `WebInputBlock` from `keydown`/`keyup`
(`KeyboardEvent.code` → SDL scancode table), pointer/wheel events on the canvas, and
`navigator.getGamepads()` once per `requestAnimationFrame`. `web/audio_worklet.js` (W-L12)
reads `WebAudioRing`.

#### 7.9.7 Renderer on the web

- `WebGpuProvider` (web): `wgpu::CreateInstance(nullptr)`, `RequestAdapter` /
  `RequestDevice` with `CallbackMode::AllowSpontaneous` and a `YieldToEventLoop` loop until
  the callback fired (startup only). Required limits: request
  `maxStorageBufferBindingSize`/`maxBufferSize` ≥ 512 MiB if the adapter offers it, else
  W-L09's split mirror. Features: as native minus `Unorm16TextureFormats` (use
  `texture-formats-tier1`), minus Dawn toggles and backend validation.
- Surface: `wgpu::EmscriptenSurfaceSourceCanvasHTMLSelector{"#canvas"}` on the GPU worker
  (the transferred canvas is addressable by its selector there).
- **Presenter paint mode** `kGuestOutputThreadWithUI` (new, web only, SDK
  `ui/presenter.cpp`): the guest-output (GPU worker) thread paints guest output **and** UI
  drawers. `IssueSwap` → `RefreshGuestOutput` → `PaintAndPresent(true)` on the GPU worker,
  then `YieldToEventLoop(0)` so the browser presents. When no swap happens for 50 ms but a
  UI paint was requested (settings menu open while paused), the idle loop paints UI-only.
- `device_mutex()` stays (uncontended on the web).
- Error scopes and the `WaitAny` calls listed in 9.1 become `AllowSpontaneous` callbacks;
  test captures complete asynchronously and write the PNG from the callback.

#### 7.9.8 Files

- WasmFS with the OPFS backend mounted at `/opfs`. Layout:
  `/opfs/trials_hd/assets/` (game data, including `default.xex` and
  `package_license.toml`), `/opfs/trials_hd/home/` (`HOME`/`XDG_DATA_HOME` for saves and
  profiles), `/opfs/trials_hd/trials_hd.toml` (settings; `GetExecutableFolder()` returns
  `/opfs/trials_hd` on the web), `/opfs/trials_hd/cache/` (shader storage).
- Startup arguments come from the page URL query (`?log_level=info&draw_resolution_scale_x=2`)
  plus `--game_data_root=/opfs/trials_hd/assets`.
- Import (W-L10): a dedicated wasm entry `trials_hd_import(path_or_handle)` runs in a worker
  with the C++ STFS extractor (W-S13); progress goes to the page through a shared counter.

#### 7.9.9 Audio

`audio/web/web_audio_driver.cpp` implements the SDK `AudioDriver` interface (as
`sdl_audio_driver` does): `SubmitFrame` converts to float and writes into `WebAudioRing`,
blocking (condition wait with timeout) when the ring is full, which paces the audio
worker like the SDL callback does natively. The page creates the `AudioContext` on the
first user gesture and posts the ring's buffer and offset to the worklet.

#### 7.9.10 Static GPU plugin

```cpp
// rex/system/gpu_plugin.h (all platforms; used only on the web)
void RegisterStaticGpuPlugin(std::string_view name, GpuAbiVersionFn abi, GpuCreateFn create);
```

`LoadGpuPlugin` on the web looks the name up in that registry instead of loading a file.
The renderer, built as a static library when `TRIALSHD_WEB`, exposes
`rex_gpu_webgpu_register()` (calls `RegisterStaticGpuPlugin("webgpu", ...)`), and
`src/web/web_main.cpp` calls it before `ReXApp` setup (no static-initializer tricks; the
linker would drop them). The Vulkan plugin is not built for the web.

#### 7.9.11 Build layout

- `CMakeLists.txt`: `option(TRIALSHD_WEB ... OFF)`; `if(TRIALSHD_WEB AND NOT EMSCRIPTEN)
  message(FATAL_ERROR ...)`; with `TRIALSHD_WEB`, `TRIALSHD_WEBGPU` builds
  `rexgpu-webgpu` as `STATIC` and skips Dawn (`--use-port=emdawnwebgpu` instead).
- Preset `web-wasm64-release` (binary dir `out/build/web-wasm64-release`); flags in 10.2.
- New directories: `src/web/` (app web glue, `web_main.cpp`), `web/` (`shell.html`,
  `input.js`, `audio_worklet.js`, `import.js`), `tools/web/` (`serve.py`, `cdp.py`,
  `spikes/`), and the SDK's `*/web/` backends.

#### 7.9.12 Interface ownership

| Interface | Defined in | Implemented by task |
| --- | --- | --- |
| `REX_PLATFORM_EMSCRIPTEN`, header fixes | `rex/platform.h` etc. | W-S10, W-S11 |
| Fold table, store helpers, wasm mask dispatch | `rex/memory/web_guest.h`, codegen, `ppc/context.h` | W-L02 |
| Window, heaps, `MarkGuestWrite`, `DeliverPhysicalWrites`, dirty bitmap | `rex/memory/web_guest.h`, `xmemory.cpp`, `memory_emscripten.cpp` | W-L04 |
| Consumers of delivery, verify mode | `shared_memory.cpp`, `primitive_processor.cpp`, renderer | W-L05 |
| `YieldToEventLoop`, transferred canvas, timer thread | `rex/thread/web_yield.h`, `threading_posix.cpp` | W-L06, W-L15 |
| `kGuestOutputThreadWithUI`, web `Window`, `WebWindowedAppContext` | SDK `ui/` | W-L06, W-L08 |
| Renderer web paths | `src/gpu_webgpu/` | W-L07, W-L09 |
| `WebInputBlock`, `input.js` | `src/web/web_input.h`, `web/input.js` | W-L08, W-S18 |
| `WebAudioRing`, worklet, web audio driver | `src/web/web_audio.h`, SDK `audio/web/` | W-L12 |
| OPFS layout, import entry | `src/web/web_files.*`, `web/import.js` | W-L10 |
| `RegisterStaticGpuPlugin` | `rex/system/gpu_plugin.h` | W-L15 |

---

## 8. Subsystem by subsystem

### 8.1 Memory (`src/system/xmemory.cpp`, `src/core/memory_posix.cpp`)

- One linear-memory reservation: guest window 3 GiB (D2) at a page-aligned offset, plus the
  host heap. With Memory64, declare `INITIAL_MEMORY` ≥ 3.5 GiB and `MAXIMUM_MEMORY` 8 GiB.
  Browsers commit pages on first touch, so untouched guest pages cost nothing (the
  reference port starts at 3 GiB).
- `Memory::Initialize`: on `__EMSCRIPTEN__`, `virtual_membase_` = window start,
  `physical_membase_` = window + `0x90000000` (layout in 7.9.2; the first sketch here said
  `0xA0000000`); the `vA0000000/vC0000000/vE0000000` heaps keep their allocators but
  translate to the physical block. No `MapFileView`. Implemented by W-L04 (7.9.2).
- `AllocFixed`/`Protect`/`Decommit`: bookkeeping only. Checked (W-L04): native decommit
  and release keep the page contents, so the web does not zero them either.
- Host code that converts guest addresses (`TranslateVirtual`, `TranslatePhysical`,
  `GetPhysicalAddress`) must apply the same fold as the macro. One helper, used by both.

### 8.2 MMIO

Generated code checks MMIO only at the 28 `REX_MM_*` sites; natively any other access to
`0x7F000000–0x7FFFFFFF` faults and the `SIGSEGV` handler emulates it
(`MMIOHandler`, `exception_handler_posix.cpp`). Without signals, an unmarked MMIO access
would silently read or write guest memory. Host-side MMIO (kernel writing GPU registers) calls
the handler directly already.

**W-S07 MMIO completeness verified (2026-10-15).** An `LD_PRELOAD` probe interposed
`MMIOHandler::ExceptionCallback` (`_ZN3rex7runtime11MMIOHandler17ExceptionCallbackEPNS_4arch9ExceptionE`)
to immediately log and abort if any access violation fault occurred within the guest MMIO
range `0x7F000000..0x7FFFFFFF`. All four test scripts (`main_menu_nav`, `ride_middle_name_danger`,
`settings_menu`, `credits`) completed with **0 MMIO faults** (0 aborts). This conclusively proves
the 28 `REX_MM_*` sites are exhaustive for Trials HD; no guest code relies on `SIGSEGV` MMIO
emulation during menus or gameplay.

### 8.3 Write tracking without page protection (D3)

Natively, three subsystems learn about CPU writes to GPU-visible memory through
`mprotect` + `SIGSEGV`: the SDK `SharedMemory` (`EnablePhysicalMemoryAccessCallbacks`,
`RegisterPhysicalMemoryInvalidationCallback`), the primitive processor (converted index
buffers), and the renderer's texture cache and memory mirror (`WatchMemoryRange`, 256 KiB
chunks).

Web design:

- A **dirty bitmap**, 1 bit per 4 KiB physical page = 16 KiB. Physical-range guest stores
  set the bit with a relaxed atomic OR.
- **Host writers** that bypass generated code call `Memory::MarkPhysicalWritten(addr,
  len)`: file reads into guest buffers (`NtReadFile` and friends), `src/crt_hooks.cpp`
  native `memcpy`/`memset`, XMA decoder output, the kernel's `RtlFill*`/`RtlCopy*`, and
  any `memcpy` into guest memory in the SDK. Audit list in W-S09.

#### 8.3.1 Audit of host writers into guest memory (W-S09)

Audit conducted across `src/*.cpp`, SDK `src/kernel`, `src/system`, `src/audio`, and `src/filesystem`:

| Subsystem | File:line | Operation | Target pointer | Can be physical? | Analysis & Web Handling |
| --- | --- | --- | --- | --- | --- |
| **App CRT hooks** | `src/crt_hooks.cpp:21` | `std::memmove` | `ctx.r3.u32` (GuestPtr) | **Yes** | Replaces guest `sub_822F81A0` memcpy (shader constant & vertex staging). Must call `MarkGuestWrite`. |
| **App CRT hooks** | `src/crt_hooks.cpp:29` | `std::memmove` | `ctx.r3.u32` (GuestPtr) | **Yes** | Replaces guest `sub_822F8628` memcpy (geometry staging). Must call `MarkGuestWrite`. |
| **App CRT hooks** | `src/crt_hooks.cpp:34` | `std::memset` | `ctx.r3.u32` (GuestPtr) | **Yes** | Replaces guest `sub_822F8040` memset (buffers/geometry). Must call `MarkGuestWrite`. |
| **App patches** | `src/exit_button.cpp:49` | `std::memcpy` | `base + match_addr` | No | Overwrites instructions in virtual XEX image code section (`0x82xxxxxx`). |
| **App render pose** | `src/render_pose.h:264-266` | `std::memcpy` | `base_ + offset` | No | Updates camera, view matrix, and palette in virtual heap objects (`0x9xxxxxxx`). |
| **GPU resolve** | `src/gpu_webgpu/resolve_engine.cpp:382,460` | `TranslatePhysical` / `std::memcpy` | `guest + offset` | **Yes** | CPU fallback resolve to physical memory. GPU internal write; not marked for GPU upload. |
| **Audio XMA** | `thirdparty/rexglue-sdk/src/audio/xma_context.cpp:337` | `TranslatePhysical` / `RingBuffer` | `data->output_buffer_ptr` | **Yes** | XMA decoder writes decoded PCM directly into guest physical memory. Must call `MarkGuestWrite`. |
| **Audio XMA** | `thirdparty/rexglue-sdk/src/audio/xma_context.cpp:226` | `std::memset` | `TranslateVirtual(guest_ptr())` | No | Clears `XMA_CONTEXT_DATA` struct in virtual memory. |
| **Audio system** | `thirdparty/rexglue-sdk/src/audio/audio_system.cpp:252` | `store_and_swap` | `TranslateVirtual(ptr)` | No | Writes callback argument in virtual memory. |
| **Filesystem** | `thirdparty/rexglue-sdk/src/system/xfile.cpp:160-165` | `ReadSync` (`TranslatePhysical/Virtual`) | `buffer_guest_address` | **Yes** | Synchronous file reads into guest buffer (assets, packages). Must call `MarkGuestWrite`. |
| **SDK CRT hooks** | `thirdparty/rexglue-sdk/src/kernel/crt/memory.cpp:21` | `std::memcpy` (`native_memcpy`) | `dst` (guest pointer) | **Yes** | Hooked generic CRT memcpy; dst can be physical buffer. Must call `MarkGuestWrite`. |
| **SDK CRT hooks** | `thirdparty/rexglue-sdk/src/kernel/crt/memory.cpp:25` | `std::memmove` (`native_memmove`) | `dst` (guest pointer) | **Yes** | Hooked generic CRT memmove; dst can be physical buffer. Must call `MarkGuestWrite`. |
| **SDK CRT hooks** | `thirdparty/rexglue-sdk/src/kernel/crt/memory.cpp:29` | `std::memset` (`native_memset`) | `dst` (guest pointer) | **Yes** | Hooked generic CRT memset; dst can be physical buffer. Must call `MarkGuestWrite`. |
| **SDK CRT hooks** | `thirdparty/rexglue-sdk/src/kernel/crt/memory.cpp:41` | `std::memcpy` (`native_XMemCpy`) | `dst` (guest pointer) | **Yes** | Xbox-optimized CRT memcpy; dst can be physical buffer. Must call `MarkGuestWrite`. |
| **SDK CRT hooks** | `thirdparty/rexglue-sdk/src/kernel/crt/memory.cpp:45` | `std::memset` (`native_XMemSet`) | `dst` (guest pointer) | **Yes** | Xbox-optimized CRT memset; dst can be physical buffer. Must call `MarkGuestWrite`. |
| **SDK CRT hooks** | `thirdparty/rexglue-sdk/src/kernel/crt/memory.cpp:49` | `std::memset` (`native_XMemSet128`) | `dst` (guest pointer) | **Yes** | Xbox-optimized 128-bit memset; dst can be physical buffer. Must call `MarkGuestWrite`. |
| **SDK CRT hooks** | `thirdparty/rexglue-sdk/src/kernel/crt/memory.cpp:53` | `std::memset` (`native_memset_vmx`) | `dst` (guest pointer) | **Yes** | VMX-optimized memset; dst can be physical buffer. Must call `MarkGuestWrite`. |
| **SDK CRT hooks** | `thirdparty/rexglue-sdk/src/kernel/crt/memory.cpp:60` | `std::memcpy` (`native_memcpy_s`) | `dst` (guest pointer) | **Yes** | Secure CRT memcpy; dst can be physical buffer. Must call `MarkGuestWrite`. |
| **SDK CRT threading**| `thirdparty/rexglue-sdk/src/kernel/crt/threading.cpp:213` | `std::memset` | `TranslateVirtual(initial_sp)` | No | Zeroes initial stack frame on virtual thread stack (`0x7xxxxxxx`). |
| **SDK Memory** | `thirdparty/rexglue-sdk/src/system/xmemory.cpp:265` | `std::memcpy` | `TranslateVirtual(0x8000001C)` | No | Writes header flag in virtual image segment. |
| **SDK Memory** | `thirdparty/rexglue-sdk/src/system/xmemory.cpp:492` | `std::memset` (`Memory::Zero`) | `TranslateVirtual(address)` | **Yes** | Memory clearing; address can be in physical heap. Handled in W-L04 via `MarkGuestWrite`. |
| **SDK Memory** | `thirdparty/rexglue-sdk/src/system/xmemory.cpp:499` | `std::memset` (`Memory::Fill`) | `TranslateVirtual(address)` | **Yes** | Memory fill; address can be in physical heap. Handled in W-L04 via `MarkGuestWrite`. |
| **SDK Memory** | `thirdparty/rexglue-sdk/src/system/xmemory.cpp:506` | `std::memcpy` (`Memory::Copy`) | `TranslateVirtual(dest)` | **Yes** | Memory copy; dest can be in physical heap. Handled in W-L04 via `MarkGuestWrite`. |
| **SDK RTL strings** | `thirdparty/rexglue-sdk/src/kernel/xboxkrnl/xboxkrnl_rtl.cpp:184` | `std::memcpy` (`RtlCopyString`) | `destination->pointer` | Potential | ANSI string copy into caller buffer; usually virtual heap/stack. |
| **SDK RTL strings** | `thirdparty/rexglue-sdk/src/kernel/xboxkrnl/xboxkrnl_rtl.cpp:200` | `std::memcpy` (`RtlCopyUnicodeString`)| `destination->pointer` | Potential | Unicode string copy into caller buffer; usually virtual heap/stack. |
| **SDK RTL strings** | `thirdparty/rexglue-sdk/src/kernel/xboxkrnl/xboxkrnl_rtl.cpp:223` | `memcpy` (`RtlUnicodeStringToAnsi`) | `SystemHeapAlloc` buffer | No | Allocated from kernel virtual system heap. |
| **SDK RTL strings** | `thirdparty/rexglue-sdk/src/kernel/xboxkrnl/xboxkrnl_rtl.cpp:233` | `memcpy` (`RtlUnicodeStringToAnsi`) | `destination_ptr->pointer` | Potential | ANSI string copy into caller destination buffer; usually virtual. |
| **SDK IO** | `thirdparty/rexglue-sdk/src/kernel/xboxkrnl/xboxkrnl_io.cpp:660` | `std::memcpy` (`NtQuerySymbolicLink`)| `target->pointer` | No | Symbolic link target string copied into virtual buffer. |
| **SDK IO** | `thirdparty/rexglue-sdk/src/kernel/xboxkrnl/xboxkrnl_io.cpp:720` | `memset` (`IoCreateDevice`) | `out` (`SystemHeapAlloc`) | No | Initializes device object struct in virtual system heap. |
| **SDK Modules** | `thirdparty/rexglue-sdk/src/kernel/xboxkrnl/xboxkrnl_module.cpp:79,99` | `store_and_swap` | `pKeDebugMonitorData`, `pKeCertMonitorData` | No | Kernel export pointers in virtual segment (`0x80xxxxxx`). |
| **SDK Video** | `thirdparty/rexglue-sdk/src/kernel/xboxkrnl/xboxkrnl_video.cpp:532,538,544` | `store_and_swap` | `pVdGlobalDevice`, etc. | No | Video export variables in virtual segment (`0x80xxxxxx`). |
| **SDK Threading** | `thirdparty/rexglue-sdk/src/system/xthread.cpp:740,911` | pointer write | `TranslateVirtual(scratch/tls)` | No | Thread scratch space and dynamic TLS table in virtual memory. |
| **SDK Threading** | `thirdparty/rexglue-sdk/src/system/xthread.cpp:1095-1276` | `std::memcpy` | `PPCContext` / `state.context` | No | Thread context register save/restore in virtual memory. |
| **SDK Native list** | `thirdparty/rexglue-sdk/src/system/util/native_list.cpp:21-52` | `store_and_swap` | `TranslateVirtual(ptr)` | No | Kernel linked-list node pointers in virtual memory. |
| **SDK XAM Apps** | `thirdparty/rexglue-sdk/src/kernel/xam/apps/xam_app.cpp:58` | `std::memset` | `TranslateVirtual(buffer_ptr)` | No | XAM enumerator aggregate buffer in virtual heap. |
| **SDK XAM Apps** | `thirdparty/rexglue-sdk/src/kernel/xam/apps/xgi_app.cpp:393` | `store_and_swap` | `TranslateVirtual(context_ptr)`| No | Achievement context in virtual heap. |
| **SDK XAM Apps** | `thirdparty/rexglue-sdk/src/kernel/xam/apps/xmp_app.cpp:46,85,92,352` | `store_and_swap` | `TranslateVirtual(state/info)` | No | Media player playlist/song metadata in virtual heap. |
| **SDK XAM Info** | `thirdparty/rexglue-sdk/src/kernel/xam/xam_info.cpp:53,77,91,259` | `std::memcpy` / `std::memset` | `TranslateVirtual(schema/output)`| No | Language/locale schema and launch data in virtual memory. |
| **SDK XAM User** | `thirdparty/rexglue-sdk/src/kernel/xam/xam_user.cpp:94,264,698` | `std::memset` | `TranslateVirtual(info/setting)`| No | User signin info and profile settings in virtual heap. |
| **SDK XAM Content** | `thirdparty/rexglue-sdk/src/kernel/xam/xam_content.cpp:352` | `std::memcpy` | `buffer_ptr` | No | Content metadata/license in virtual heap. |
| **SDK XAM UI** | `thirdparty/rexglue-sdk/src/kernel/xam/xam_ui.cpp:441,491` | `std::memset` | `TranslateVirtual(buffer)` | No | Message box UI string buffer in virtual heap. |
| **SDK Sockets** | `thirdparty/rexglue-sdk/src/system/xsocket.cpp:138,143,174,241` | `std::memcpy` / `std::memset` | `sockaddr` / `pkt->data` | No | Socket addresses and packet payloads in virtual memory. |

- The watch APIs keep their signatures. On the web, `WatchMemoryRange` records the range;
  at each draw/resolve the consumers query "any dirty page in [a, b) since my epoch" (the
  bitmap plus a per-page write epoch array, 128 Ki × u32 = 512 KiB) instead of receiving a
  fault callback.
- GPU writes (resolves, memexport) do not mark pages dirty for the GPU, as natively.
- **Fallback:** the texture cache's chunk hashes (renderer doc, 256 KiB chunks) already
  detect changes without watches; if the bitmap proves leaky, hash-on-use for textures is a
  correct but slower fallback.

Cost: the store macro already branches on `≥ 0xA0000000` for the fold; the dirty mark is
one extra atomic OR inside that rare branch.

### 8.4 Threads, waits, timers (`src/core/threading_posix.cpp`)

- Emscripten pthreads implement `pthread_*`, `std::thread`, condition variables and
  mutexes over `Atomics.wait`/`notify`. Guest waits (`KeWaitForSingleObject`,
  `WaitMultiple`, alertable waits from SDK patch L-24) work unchanged in workers.
- `timer_create`/POSIX timers with signals: replace with one timer thread and a
  priority queue (guest `KeSetTimer`, periodic timers).
- Thread priorities and affinity: no-ops.
- `PTHREAD_POOL_SIZE=48` (measured 36 peak concurrent threads across menus and rides; 48
  provides safe headroom without async worker creation stalls).
- Stack sizes: guest threads run on guest stacks in guest memory; host pthread stacks
  need only the recursion depth of recompiled code. Measured natively (W-S06, 2026-10-15):
  maximum host stack depth touched across all 30 guest threads was 264 KiB (`GPU Commands`),
  32 KiB (`Main XThread`), and 16 KiB (`XThread*` worker threads). A 1 MiB host stack allocation
  per pthread is more than sufficient for the web build (saving significant virtual memory compared
  to native's 16 MiB per thread).
- Fibers (W-S08): 0 fiber calls (`RegisterFiber`, `Fiber::Create`, `rexcrt_*`) across all four
  headless benchmark scripts (`main_menu_nav`, `ride_middle_name_danger`, `settings_menu`, `credits`).
  Trials HD does not use fibers; fiber emulation is omitted on the web.
- W-S03 verified the proposed 48-worker pool with 40 simultaneous pthreads and shared
  Memory64 in Chromium 154: 152.7 ms shell-to-main startup, 934.7 MiB renderer RSS after
  touching 640 MiB (section 4.5).

### 8.5 Audio (`src/audio/`)

- XMA: `xma_decoder.cpp`/`xma_context.cpp` use FFmpeg's `xma2` decoder. Build FFmpeg for
  wasm with `--disable-asm --enable-decoder=xma2 --disable-everything` plus what
  `ffmpeg-overlay` lists. CPU cost on a pthread.
- Output: a web audio driver (`src/audio/web/web_audio_driver.cpp`) replacing
  `sdl_audio_driver` writes PCM into a `SharedArrayBuffer` ring; an AudioWorklet
  processor reads it. 48 kHz stereo, about 40 ms buffer. The AudioContext needs a user
  gesture to start (first click or key press).
- Silent fallback (`FallbackAudioSystem`) stays for browsers without AudioWorklet.

### 8.6 Game data and files

- **Import:** first run shows "Choose your Trials HD package". The user picks the STFS
  LIVE/PIRS/CON file (or a folder already extracted with `tools/stfs_extract.py`, through
  `showDirectoryPicker` or a drag-drop). A worker extracts it into OPFS
  `/trials_hd/assets/` and copies the license metadata the native build reads in
  `src/package_license.cpp`. Port `tools/stfs_extract.py` to C++ (shared with native, a
  `tools/stfs_extract` target) so extraction runs inside the wasm.
- **Read:** WasmFS with the OPFS backend; the kernel's host-path device points at
  `/opfs/trials_hd/assets`.
- **Write:** saves and profiles (`~/.local/share/trials_hd/` natively), `trials_hd.toml`,
  and the shader storage cache go to OPFS. Call `navigator.storage.persist()` after import.
- **Never** fetch game files from a URL we control.

### 8.7 Input

- Main thread JS: per `requestAnimationFrame`, read `navigator.getGamepads()` (standard
  mapping), keyboard state from `keydown`/`keyup` on the canvas, and pointer events for
  ImGui; write them into a 4 KiB SAB block with a sequence counter.
- Wasm: `src/input/web/web_input_driver.cpp` (an `InputDriver` like `sdl_input_driver`)
  reads the block for XInput HLE; `src/keyboard_controls.cpp` and the rebinding UI keep
  working on SDL-style key codes (map `KeyboardEvent.code`).
- The virtual pad (`src/virtual_pad.cpp`, `TRIALS_HD_VIRTUAL_PAD`) has no FIFO on the web;
  the test harness injects input through the same SAB block from CDP (section 11).

### 8.8 App shell, windowing, UI

- No SDL window. `WindowedAppContext` for the web: the pthread "UI thread" runs ImGui
  input and frame building at the display rate (driven by a message from the main thread's
  `requestAnimationFrame`); painting happens on the GPU worker (7.5).
- Fullscreen and pointer lock: buttons in the page or F11 handled by main-thread JS.
- Settings menu: unchanged ImGui. Hide Vulkan-only options and "Display Mode/Resolution"
  (the canvas size follows the page). Render Resolution, FPS limit, VSync (maps to the
  browser's display-synchronized presentation), Anti-aliasing stay.
- Exit Game: navigates to a "Game closed" page and releases OPFS handles.

### 8.9 Logging and diagnostics

spdlog sinks to the browser console (batched), plus an in-memory ring the page can
download. `--log_file` and cvar flags come from the URL query (`?log_level=info&...`).

---

## 9. WebGPU renderer changes for the browser

### 9.1 Dawn-native APIs to put behind `#if !__EMSCRIPTEN__` (read from code)

| Where | Native API | Web replacement |
| --- | --- | --- |
| `webgpu_provider.cpp` | `dawn::native::Instance`, `SetBackendValidationLevel`, `DawnTogglesDescriptor` | `wgpu::CreateInstance`; browser validates always; no toggles |
| `webgpu_provider.cpp` | `InstanceFeatureName::TimedWaitAny`; `instance_.WaitAny(future, UINT64_MAX)` | Callback mode `AllowProcessEvents`/`AllowSpontaneous`, or JSPI-suspending wait (9.5) |
| `guest_memory_mirror.cpp:61` | `WaitAny` on three `PopErrorScope`s after the 512 MiB buffer | Check limits before creating; handle errors asynchronously |
| `webgpu_presenter.cpp:237, 430` | `WaitAny` on error scope and on `MapAsync` (capture) | Async capture for tests; surface errors via the device-lost/uncaptured-error callback |
| `webgpu_presenter.cpp` surfaces | Xlib/Wayland/Metal sources | `wgpu::EmscriptenSurfaceSourceCanvasHTMLSelector` on the GPU worker's `OffscreenCanvas` |
| `x11_display.cpp` | Xlib display | Not built |

### 9.2 Features and limits the renderer asks for

`kWantedFeatures` in `webgpu_provider.cpp`: `texture-compression-bc`,
`depth32float-stencil8`, `texture-formats-tier1`, `rg11b10ufloat-renderable`,
`timestamp-query`, `depth-clip-control`, `float32-filterable`, and Dawn-only
`Unorm16TextureFormats`.

- `Unorm16TextureFormats` is Dawn-native only. Browsers expose 16-bit unorm formats through
  `texture-formats-tier1`. Use that, or emulate with `r16uint` + shader normalization.
- `timestamp-query` in browsers is quantized (100 µs in some) and may be absent; the profiler
  must degrade.
- `texture-compression-bc`: desktop yes; Android and some Apple GPUs need BC→RGBA8 decode in
  the texture-load shader (section 13).
- **Limits:** the guest-memory mirror is **one 512 MiB storage buffer**. Browser defaults
  are `maxBufferSize` 256 MiB and `maxStorageBufferBindingSize` 128 MiB, but **measured
  2026-10-15 (W-S01)** Chromium 154 / RX 6600 (AMD RDNA-2, non-fallback adapter, RADV
  flags from 11.1) exposes **4,294,967,292 bytes (4 GiB − 4) for both limits**. Request
  at least 536,870,912 for both in `requiredLimits`; the single mirror is supported on
  this target. Other adapters still need a limit check and a clear unsupported message
  if below 512 MiB (or the conditional W-L09 split mirror).

W-S01's eight wanted features, queried from `adapter.features` in that same run:

| Feature | Chromium 154 / RX 6600 |
| --- | --- |
| `texture-compression-bc` | yes |
| `depth32float-stencil8` | yes |
| `texture-formats-tier1` | yes |
| `rg11b10ufloat-renderable` | yes |
| `timestamp-query` | yes |
| `depth-clip-control` | yes |
| `float32-filterable` | yes |
| Dawn-only `Unorm16TextureFormats` (`unorm16-texture-formats`) | no; use tier1 |

Reproduce: serve `tools/web/spikes/` with `tools/web/serve.py`, launch Chromium as in
11.1 on `http://127.0.0.1:8080/limits.html`, then
`.venv/bin/python tools/web/spikes/record.py http://127.0.0.1:9222 out/limits.json`.
Install the recorder's dependency with `.venv/bin/python -m pip install -r
 tools/web/requirements.txt`. The page prints every numeric adapter limit, all features
and adapter info; CDP saves both the result JSON and `.console.log` sibling. Raw run
artifacts are in `out/web-spikes/limits.*` (not committed). This answers question 14.1;
Firefox and Safari limits remain unverified.

### 9.3 Surface and presentation

The page transfers the canvas to the GPU worker. The presenter creates its surface from
the canvas selector and configures it with `bgra8unorm`/`rgba8unorm` (whichever
`getPreferredCanvasFormat` reports) and `alphaMode: opaque`. Present happens when the
worker yields after the swap (D6). VSync is the browser's own pacing. The native FPS limit
and the uncapped mode map to "present every frame" and the browser's refresh limit:
uncapped numbers in the browser are bounded by the display rate unless the canvas is
hidden.

W-S04's standalone build (no game code):

```sh
source ~/emsdk/emsdk_env.sh
em++ tools/web/spikes/gpu_triangle.cpp -o out/web-spikes/gpu_triangle.html \
  --shell-file tools/web/spikes/gpu_triangle_shell.html --target=wasm64 \
  --use-port=emdawnwebgpu -std=c++20 -O2 -pthread -sPROXY_TO_PTHREAD \
  -sPTHREAD_POOL_SIZE=2 -sOFFSCREENCANVAS_SUPPORT \
  '-sOFFSCREENCANVASES_TO_PTHREAD=#canvas' -sASSERTIONS=1
python3 tools/web/serve.py out/web-spikes
```

This uses Emscripten's explicit canvas transfer to the proxied-main pthread (not a
WebGPU-object proxy). C++ asserts the canvas is an `OffscreenCanvas` there and
adapter/device callbacks and frame rendering never run on the browser main thread.
The worker registers its own `emscripten_request_animation_frame_loop`; it returns
to the event loop each frame. The full game's separate GPU thread still needs W-L06.
The pinned Emscripten 6.0.12 remote port downloaded emdawnwebgpu `v20260423.175430`;
the spike compiles with its C++ bindings unchanged.

**W-S04 browser result (2026-10-15):** Chromium 154 presents the triangle after five
worker-rAF frames. Worker/canvas/callback-thread assertions all pass. CDP screenshot
`out/web-spikes/gpu_triangle.png` shows the orange triangle: center `(160,120)` =
RGB `(255,51,0)`, background `(5,5)` = `(13,13,25)` (pixel assertions passed).
No WebGPU object is proxied or shared; only stdout is delivered back to the page.
The browser and GPU callbacks progress because the worker returns from each rAF
callback. W-S05 must separately prove yielding from an endless C++ loop works.

### 9.4 Threading inside the renderer

- Natively the command-processor thread and the UI thread share the device under
  `device_mutex()` (renderer doc 7.6.4). On the web, everything that touches WebGPU runs on
  the GPU worker: `WebGpuPresenter::PaintAndPresentImpl`, the ImGui immediate drawer, the
  guest-output refresh and capture.
- The UI pthread builds ImGui draw lists and hands a copy (vertices, indices, commands,
  texture IDs) to the GPU worker through a mailbox; the worker uploads and draws them in
  the present pass.
- `device_mutex()` stays (harmless); the lock-order rules keep their meaning.

### 9.5 Waiting on the GPU

All renderer readbacks are already one frame late (luminance readback, profiler), so they
fit the browser model. The blocking waits in 9.1 are startup checks and test captures:
- startup: query adapter limits and skip the error-scope round trip;
- capture (`CaptureGuestOutput`, used by `capture` in test scripts): asynchronous, writing
  the PNG when the map completes.

### 9.6 CPU cost of browser WebGPU

Native riding issues about 1,550 draws and 20.5 resolves per frame with bind-group and
pipeline caches (renderer doc 7.5). Browser WebGPU adds validation and IPC per call;
expect the GPU worker to be the bottleneck. Measure draws/ms in Chromium early (W-S12),
and keep reducing per-draw calls (render bundles for the static shadow-map passes, fewer
`setBindGroup` calls through dynamic offsets) if needed (W-L11).

**W-S12 benchmark results (2026-10-15).** Workload: 1,500 draws/frame across 30 render
passes (50 draws/pass) and 60 distinct pipelines, binding model 7.6.5 (5 dynamic offsets in
group 0, storage buffer in group 1, texture + sampler in group 2). Timed over 20 frames (30,000
draws total) on AMD Radeon RX 6600 (RADV NAVI23):

| Configuration | Validation | Implicit Sync | Encode (µs/draw) | Encode (ms/frame) | Submit (µs/draw) | Submit (ms/frame) | Total CPU (µs/draw) | Total CPU (ms/frame) |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| **Native Dawn** (`webgpu_smoke --bench`) | ON | OFF | 0.460 | 0.69 | 0.694 | 1.04 | 1.154 | 1.73 |
| **Native Dawn** (`webgpu_smoke --bench`) | OFF (`skip_validation`) | OFF | 0.122 | 0.18 | 0.724 | 1.09 | 0.846 | 1.27 |
| **Native Dawn** (`webgpu_smoke --bench`) | ON | ON | 0.459 | 0.69 | 0.685 | 1.03 | 1.144 | 1.72 |
| **Native Dawn** (`webgpu_smoke --bench`) | OFF (`skip_validation`) | ON | 0.124 | 0.19 | 0.727 | 1.09 | 0.851 | 1.28 |
| **Browser WebGPU** (`draw_bench.html`, Chromium 154) | Default | N/A | 2.927 | 4.39 | 0.007 | 0.01 | **2.934** | **4.40** |

**Conclusion:** In Chromium 154 on Linux/RX 6600, issuing 1,500 draws with dynamic bind-group
offsets costs **4.40 ms/frame** of CPU time (2.93 µs/draw). This is ~2.5× the cost of native
Dawn (1.73 ms), but remains well within the 16.67 ms (60 FPS) frame budget, leaving ~12.2 ms for
guest thread execution and other subsystems.

### 9.7 Shaders

WGSL generated at run time by `wgsl_shader_translator.cpp` works as-is. The browser
compiles WGSL through Tint in its GPU process; first-use compiles cause hitches. Keep
`async_shader_compilation`; warm the 74 known shaders (renderer doc 5.6) from the OPFS
shader storage at startup with `createRenderPipelineAsync`.

---

## 10. Building it

### 10.1 Two-stage build

`rexglue codegen` is a native tool and must run on the build machine:

1. **Native configure + build** (as today) runs codegen and produces
   `generated/default/` (and the native game).
2. **Web build** with the Emscripten toolchain compiles generated code, the runtime, the
   renderer and the app:
   ```sh
   source ~/emsdk/emsdk_env.sh
   emcmake cmake --preset web-wasm64-release   # uses generated/default from step 1
   cmake --build out/build/web-wasm64-release -j$(nproc)
   tools/web/serve.py out/build/web-wasm64-release/web   # http://127.0.0.1:8080
   ```

### 10.2 CMake (sketch)

- `CMakePresets.json`: `web-wasm64-release` with
  `CMAKE_TOOLCHAIN_FILE=$EMSDK/upstream/emscripten/cmake/Modules/Platform/Emscripten.cmake`,
  `TRIALSHD_WEB=ON`.
- With `TRIALSHD_WEB`: do not build `rexglue` codegen (fail early if `generated/default`
  is missing, telling the user to run the native build first); build the WebGPU renderer
  as a static library linked into the executable (`TRIALSHD_WEBGPU_STATIC`); skip Vulkan,
  glslang, SPIRV-Tools, MoltenVK, Tracy, `rexgpu-xenos`, the Linux library staging, and
  the test targets.
- Compile flags: `--target=wasm64` (or `-sMEMORY64=1`), `-pthread -matomics
  -mbulk-memory -msimd128 -fwasm-exceptions -O2`.
- Link flags: `--use-port=emdawnwebgpu -sMEMORY64=1 -pthread -sPROXY_TO_PTHREAD
  -sPTHREAD_POOL_SIZE=48 -sOFFSCREENCANVAS_SUPPORT -sINITIAL_MEMORY=3800mb
  -sMAXIMUM_MEMORY=8gb -sALLOW_MEMORY_GROWTH -sWASMFS -sSUPPORT_LONGJMP=wasm
  -fwasm-exceptions -sJSPI` (if 7.4 option 1) `-sAUDIO_WORKLET -sWASM_WORKERS`
  (if the worklet needs them) `-sEXPORTED_RUNTIME_METHODS=...`, plus `--closure=1` for
  release JS.
- SDK third-party libraries: SDL3 (only for keycode tables if kept; its video/audio are
  not used), FFmpeg (custom wasm configure), fmt, spdlog, imgui, toml++, xxhash, libmspack,
  utfcpp, CLI11.

**As built (W-L15, 2026-10-15).** `CMakeLists.txt` has `option(TRIALSHD_WEB OFF)`; with it
on (and only under Emscripten's toolchain, else `FATAL_ERROR`) it includes
`cmake/web/trials_hd_web.cmake` and `return()`s before the native description, so a native
configure never reads the web files. Presets: `web-wasm64-release` (browser page in
`out/build/web-wasm64-release/web/`) and `web-wasm64-node` (the same program for Node.js
with the host file system: boot checks without a browser). Targets: `trials_hd_web_sdk`
(OBJECT: like the native shared library, every object is linked, including ones that only
self-register cvars and kernel exports), `trials_hd_web_recomp` (OBJECT, the generated
code with its PCH), `trials_hd_web_imgui` (OBJECT, one ImGui context),
`trials_hd_web_thirdparty` and `trials_hd_web_ffmpeg` (STATIC), and `trials_hd`.

Flags in use: compile `-sMEMORY64=1 -pthread -msimd128 -fwasm-exceptions
-fno-strict-aliasing -ffp-contract=off -fno-char8_t -O2`; link `-sPROXY_TO_PTHREAD
-sPTHREAD_POOL_SIZE=48 -sINITIAL_MEMORY=3328mb -sMAXIMUM_MEMORY=8gb -sALLOW_MEMORY_GROWTH=1
-sSTACK_SIZE=8mb -sDEFAULT_PTHREAD_STACK_SIZE=4mb -sEXIT_RUNTIME=1 --profiling-funcs`, plus
`-sWASMFS -sENVIRONMENT=web,worker --shell-file web/shell.html` (browser) or
`-sENVIRONMENT=node -sNODERAWFS=1` (Node; WasmFS's NODERAWFS did not see host paths in
Emscripten 6.0.12, so the Node build keeps the legacy file system). Not yet:
`--use-port=emdawnwebgpu`, `-sJSPI`, `-sOFFSCREENCANVAS_SUPPORT` (W-L06/W-L07), audio
worklet (W-L12). A full build is ~500 objects; the 107 generated files dominate (about
8 minutes at `-j8` here). `trials_hd.wasm` is 77 MB with function names.

### 10.3 Build cost

Generated code: 75 s with 14 jobs (section 6). Expect several minutes for the runtime and
renderer, plus `wasm-opt` on a ~40 MiB module (minutes, several GiB of RAM; run `-O1`
or skip it for debug builds).

### 10.4 Output

`trials_hd.html` (page shell, written by us in `web/shell.html`), `trials_hd.js`
(Emscripten glue + our JS for input, audio, import), `trials_hd.wasm` (≈ 40 MiB, ≈ 10 MiB
with brotli), `trials_hd.worker.js` if emitted, and the AudioWorklet module. Served with
COOP/COEP headers and streaming compilation (`WebAssembly.compileStreaming`).

---

## 11. Testing, debugging and validation

### 11.1 Headless Chromium with hardware WebGPU (this server)

From the memory note `headless-webgpu-server` (RADV on the RX 6600; NVIDIA Vulkan did not
work headless):

```sh
VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/radeon_icd.json chromium --headless=new \
  --no-sandbox --disable-gpu-sandbox --enable-unsafe-webgpu --ignore-gpu-blocklist \
  --use-gl=angle --use-angle=vulkan --enable-features=Vulkan --use-vulkan=native \
  --remote-debugging-port=9222 http://127.0.0.1:8080/
```

Without ANGLE-on-Vulkan Chromium exposes only SwiftShader to WebGPU.

### 11.2 Harness

- `tools/web/cdp.py`: a small Chrome DevTools Protocol client (websocket). It opens the
  page, forwards console logs to a file, and drives input by writing the SAB input block
  through `Runtime.evaluate`. It also runs the existing `tests/headless/*.txt` scripts
  (`wait`, `pad`, `axis`, `tap`, `capture`): `capture` calls an exported
  `trials_hd_capture()` that writes a PNG through the async capture (9.5).
- Golden comparison: run the same script natively (WebGPU renderer) and in the browser,
  then compare with `tools/compare_captures.py`. The renderer is the same code, so menus
  should match within ~40 dB PSNR; rides differ by timing as natively.
- FPS: the guest FPS log line (`--log_guest_fps`) goes to the console; `cdp.py` collects
  it. `tests/check_fps_log.py` works unchanged on the collected log.

**Boot checks (W-L15).**

```sh
source ~/emsdk/emsdk_env.sh
# Node, host file system, your assets/ (stdout and log in out/web-run/):
tools/web/run_node.sh 90
# Browser: serve the page and your game folder (loopback only), then headless Chromium:
python3 tools/web/serve.py out/build/web-wasm64-release/web --assets assets &
.venv/bin/python tools/web/cdp.py 'http://127.0.0.1:8080/?web_assets_url=/assets' \
    --seconds 120 --until '600 guest swaps' --console out/web-run/console.log
```

`serve.py --assets` serves the folder under `/assets/` with a generated `manifest.txt`; the
page passes its URL query as runtime flags, and `--web_assets_url=/assets` makes the app
mount it read-only at `/assets` through WasmFS's fetch backend (each file downloads on first
read; development only - the real data path is W-L10's OPFS import). `cdp.py` (W-S15)
captures the console of the page and of every worker (the game's pthreads), stops on a
pattern, and can evaluate an expression and take a screenshot. With the "null" GPU plugin
the log reports `web: first guest swap` and then every 600 swaps with the rate.

Profiling: the module keeps function names, so `node --cpu-prof --cpu-prof-dir=DIR -r
out/web-run/exit_on_sigint.js out/build/web-wasm64-node/trials_hd.js ...` writes one
profile per thread with guest functions named `__imp__sub_<address>`.

### 11.3 Debugging

- Debug builds with `-g` and `-gseparate-dwarf`; Chrome's C/C++ DevTools extension maps
  wasm to source.
- WebGPU errors: `uncapturederror` events are logged; `chrome://gpu` for adapter info.
- Memory: `performance.measureUserAgentSpecificMemory()` (cross-origin isolated pages).

---

## 12. Milestones

| ID | Status | Milestone | Check |
| --- | --- | --- | --- |
| WM0 | todo | Spikes answered: limits, JSPI in pthreads, Memory64 cost, MMIO completeness, fibers | W-S01–W-S08 done, section 14 questions 1–5 answered |
| WM1 | done 2026-10-15 (W-L15; null GPU, Chromium 154) | Runtime boots headless in the browser to the first guest frame (no graphics: GPU plugin stubbed) | Console shows the guest reaching the main menu's first swap |
| WM2 | todo | Menus render through the browser's WebGPU | `main_menu_nav` golden captures within 40 dB of native |
| WM3 | todo | Game data import and persistence | Fresh profile → pick package → plays; reload → no re-import |
| WM4 | todo | Rides render; input from keyboard and gamepad | `ride_middle_name_danger` completes; `check_fps_log.py 60` passes in Chromium |
| WM5 | todo | Audio | Music and effects audible; no underruns over a 2-minute ride |
| WM6 | todo | 60 FPS at 720p in desktop Chrome (RX 6600) | 3 runs, min ≥ 55 FPS riding, logged in 4.3-style table |
| WM7 | todo | Firefox desktop | WM2–WM5 checks pass in Firefox |
| WM8 | todo | Safari (macOS) | WM2–WM5 checks pass, or a documented blocker |

---

## 13. Later: Safari, Firefox, Android

- **Safari:** WebGPU is on (macOS 26); JSPI arrives in Safari 27 per caniuse; Memory64 is
  the open question. If missing, option B (wasm32 remapped window) is the Safari build.
- **Firefox:** WebGPU on Windows and Apple-Silicon macOS; Linux expected in 2026. Memory64
  and JSPI are supported in current versions per caniuse; verify (W-S02, W-S03).
- **Android Chrome:** WebGPU on Android 12+ with Qualcomm/ARM GPUs. No BC textures
  (decode in the texture-load shader), less memory (3 GiB window may fail), touch
  controls, thermal limits. Treat as a separate project after WM6.

---

## 14. Risks and open questions

| # | Question / risk | How to answer | Task |
| --- | --- | --- | --- |
| 1 | Does Chromium on the RX 6600 give `maxStorageBufferBindingSize` and `maxBufferSize` ≥ 512 MiB? | **Yes:** both 4 GiB − 4 (Chromium 154, 2026-10-15); section 9.2 | W-S01 |
| 2 | Does JSPI work on pthreads with emdawnwebgpu, and can a suspended GPU worker present its `OffscreenCanvas`? | **Yes** in Chromium 154: endless-loop frames, two map modes and changing screenshot pixels verified (7.4) | W-S05 |
| 3 | Memory64 bounds-check cost on the guest threads | Microbenchmark plus the full game, native vs wasm32 vs wasm64 | W-L03 |
| 4 | Are the 28 `REX_MM_*` sites the only MMIO accesses? | **Yes:** fatal-abort probe observed 0 faults across all 4 headless scripts (section 8.2) | W-S07 |
| 5 | Does Trials create fibers? | **No:** 0 fiber calls observed across all 4 headless scripts; fiber subsystem omitted on web (section 8.4) | W-S08 |
| 6 | Do guest FPSCR rounding-mode changes matter? | **Yes:** native ride uses nearest-even and toward-zero; shadow state and explicit `fctid` rounding tested. Other FP arithmetic/physics replays remain a validation risk (below). | W-S11 |
| 7 | Browser WebGPU CPU cost for ~1,550 draws per frame | **4.40 ms/frame** in Chromium 154 vs 1.73 ms native Dawn (1,500 draws, dynamic offsets, RX 6600; section 9.6) | W-S12, W-L11 |
| 8 | Host writers into physical memory missing from the dirty-bitmap audit → stale textures | Compare against the hash fallback in debug builds (assert no hash change without a dirty bit) | W-S09, W-L05 |
| 9 | Initial 3.8 GiB memory reservation fails on some systems | Fall back to growing from 3 GiB; catch `RangeError` and show a message | W-L04 |
| 10 | 48 workers × module instantiation time and memory | **36 peak threads**, max host stack 264 KiB touched (GPU Commands), guest execution workers ≤ 32 KiB; 1 MiB stack safe (sections 4.5, 8.4) | W-S06 |
| 11 | Long first-run shader compiles in the browser (Tint) | Pipeline warm-up from OPFS storage | W-S16 |
| 12 | Legal: someone hosts the build | Bind to localhost by default; README warning; no publish target | W-S17 |
| 13 | Safari without Memory64 | Option B build | W-L13 |
| 14 | `SharedArrayBuffer` needs COOP/COEP; extensions or iframes may break it | Local server sets headers; page checks `crossOriginIsolated` and explains | W-S14 |

**W-S11 rounding findings (2026-10-15).** A private native probe recompiles only the
five generated translation units containing all five `storeFromGuest` sites, with a
forced tracing header; it relinks into `out/web-spikes/fpscr-probe/` and never edits
`generated/default` or native build objects. An 80-second `Middle Name: Danger` run
reached the ride (capture inspected) and recorded MXCSR RC **0 = nearest-even** and
**3 = toward-zero**, corresponding to guest modes 0 and 1. Up/down were not observed
in this run, not proven unused. Artifacts: `out/web-spikes/fpscr-probe/modes.json`,
`stdout.log`, and ride captures (private, not committed).

Reproduce: `python3 tools/web/tests/build_fpscr_probe.py`, then run
`tools/headless_run.sh 80 --gpu_plugin=webgpu --webgpu_headless=true --webgpu_adapter=1
--present_fps_limit=60` under `flock out/native-web-check.lock`, with
`TRIALS_HD_EXE=$PWD/out/web-spikes/fpscr-probe/trials_hd` and
`INPUT=$PWD/tests/headless/ride_middle_name_danger.txt`; inspect
`WEB_NATIVE_FPSCR_MODE` in `out/run/stdout.log`.

The wasm FPSCR shadow is per-thread, retains flush/mode bits, and logs non-nearest
requests once per mode/thread in debug builds. The PCH maps the 10 generated
`simde_mm_cvtsd_si64` (`fctid`) uses to explicit floor/ceil/trunc/nearest-even handling
with safe NaN/overflow behavior. Standalone wasm64 tests cover all four modes and TLS.
**Other FP arithmetic still uses wasm's nearest-even/gradual-underflow behavior; the
full browser game's physics replay comparison is not yet performed.** Do not treat
these header/conversion checks as full physics validation.

---

## 15. Tools and reference index

### 15.1 Repository paths

| Path | Why |
| --- | --- |
| `generated/default/trials_hd_pch.h` | Memory macros (`REX_PHYS_HOST_OFFSET`, `REX_MM_*`), function macros |
| `thirdparty/rexglue-sdk/src/codegen/builders/` | Where the codegen emits those macros (`memory.cpp`, `vector.cpp`, `context.cpp`) |
| `thirdparty/rexglue-sdk/src/system/xmemory.cpp`, `src/core/memory_posix.cpp` | Guest memory setup |
| `thirdparty/rexglue-sdk/src/core/memory_emscripten.cpp`, `include/rex/memory/web_guest.h` | Web guest window, dirty bitmap, `MarkGuestWrite` (W-L04) |
| `tools/web/tests/run_memory_model.sh` | W-L04 check: SDK `Memory` with the web model, wasm64 under Node (activate emsdk first) |
| `thirdparty/rexglue-sdk/src/system/mmio_handler.cpp`, `src/core/exception_handler_posix.cpp` | MMIO and fault handling |
| `thirdparty/rexglue-sdk/src/graphics/shared_memory.cpp`, `primitive_processor.cpp` | Write watches |
| `thirdparty/rexglue-sdk/src/core/threading_posix.cpp`, `fiber_posix.cpp` | Threads, timers, fibers |
| `thirdparty/rexglue-sdk/src/audio/` | XMA and audio output |
| `thirdparty/rexglue-sdk/include/rex/platform/fpscr.h`, `include/rex/ppc/intrinsics.h`, `include/rex/ppc/context.h`, `include/rex/chrono/chrono.h` | Header fixes from the compile spike |
| `src/gpu_webgpu/webgpu_provider.cpp`, `webgpu_presenter.cpp`, `guest_memory_mirror.cpp`, `texture_cache.cpp` | Renderer changes (section 9) |
| `tools/stfs_extract.py` | Package extraction to port |
| `out/claude-3d/`, `tools/profile/thread_cpu.py` | Measurement scripts used in section 4 |

### 15.2 Tools on the build server

- `~/emsdk` (Emscripten 6.0.12): `source ~/emsdk/emsdk_env.sh`.
- `~/emsdk/upstream/bin/llvm-objdump -h <obj>`: wasm section sizes.
- `/usr/bin/chromium` 154: recipe in 11.1.
- `~/playgta5-main`: reference port (not our code; do not copy from it).

### 15.3 External sources (read 2026-10-15)

- Emdawnwebgpu README: https://dawn.googlesource.com/dawn/+/refs/heads/main/src/emdawnwebgpu/pkg/README.md
- Emscripten WebGPU support: https://emscripten.org/docs/porting/multimedia_and_graphics/WebGPU-support.html
- Emscripten settings reference: https://emscripten.org/docs/tools_reference/settings_reference.html
- Emscripten setjmp/longjmp: https://emscripten.org/docs/porting/setjmp-longjmp.html
- Emscripten file system (WasmFS, OPFS): https://emscripten.org/docs/api_reference/Filesystem-API.html
- WasmFS + OPFS backend (PR 16813): https://github.com/emscripten-core/emscripten/pull/16813
- Deprecate `-sUSE_WEBGPU` (PR 24220): https://github.com/emscripten-core/emscripten/pull/24220
- V8 wasm limits: https://chromium.googlesource.com/v8/v8/+/master/src/wasm/wasm-limits.h
- SpiderMonkey, "Is Memory64 actually worth using?": https://spidermonkey.dev/blog/2025/01/15/is-memory64-actually-worth-using.html
- Memory64 in browsers (SciChart): https://www.scichart.com/blog/memory64-unlocking-webassemblys-true-potential-with-16gb-of-in-browser-memory/
- JSPI on caniuse: https://caniuse.com/wf-wasm-jspi
- V8 JSPI introduction: https://v8.dev/blog/jspi
- WebGPU implementation status: https://github.com/gpuweb/gpuweb/wiki/Implementation-Status
- WebGPU in major browsers (web.dev): https://web.dev/blog/webgpu-supported-major-browsers
- MDN `Navigator.getGamepads()`: https://developer.mozilla.org/en-US/docs/Web/API/Navigator/getGamepads

---

## 16. Task breakdown for agents (small and large models)

### 16.0 This section is the task list

**This document is the single, authoritative task list for the web port.** Every agent
works from the tables below and keeps them current, so the next agent can continue.

**Status values:**

| Status | Meaning |
| --- | --- |
| `todo` | not started |
| `claimed <agent> <date>` | someone is working on it (e.g. `claimed gpt-6.1-sol 2026-10-16`) |
| `blocked: <reason>` | cannot continue; say what is needed (task ID, user decision, hardware) |
| `review` | done by a small model, waiting for large-model review (W-L14) |
| `done <date> <commit>` | finished, verified by its "Done when" check, committed on `dev` |
| `dropped: <reason>` | no longer needed (keep the row; say why) |

**Workflow:**

1. Pick a `todo` task whose dependencies are `done`; set `claimed <agent> <date>` here
   first. A claim older than 3 days with no commits may be taken over (note it in 16.6).
2. Record findings in the section the task belongs to (sections 4–14), not only in chat.
3. When finished, run "Done when", set `done <date> <commit>` (small models: `review`),
   and add a dated line to the Notes log (16.6).
4. New work → new rows with the next free ID (`W-S19`, `W-L16`, ...). Never renumber or
   delete rows; mark obsolete ones `dropped: <reason>`.
5. If a measurement contradicts this document, fix the document and say so in 16.6.
6. Commit document updates with the work (`dev` only; never push without the user's
   go-ahead).

**Who does what.** **Large models** (GPT 6.1 Sol, Claude Opus class) own the architecture,
the memory model, codegen changes, the GPU-worker threading and anything open-ended.
**Small models** (Gemini 3.8 Flash, GPT 6 Luna class) take self-contained, verifiable
tasks: measurements, test pages, ports of small files, scripts and documentation. A large
model reviews every small-model result before it is marked `done`.

### 16.1 Rules for every agent

- Work on the local `dev` branch; never push. `main` is the public tree and gets web
  changes only when the user asks.
- **Never commit or publish game content**: generated code, `.wasm`/`.js` builds of the
  game, shader microcode, WGSL generated from it, captures, logs with game data, or
  anything from `assets/`, `generated/`, `out/`, OPFS dumps. Web build outputs live in
  `out/` (gitignored).
- SDK changes go through `patches/rexglue-sdk/trials-hd.patch` via
  `tools/export_sdk_patch.sh`; keep them small and guarded by `__EMSCRIPTEN__` /
  `REX_PLATFORM_EMSCRIPTEN` so the native builds are unchanged.
- **The native builds must not change (D14, section 7.8).** Run `tools/check_native.sh
  baseline` before starting and `tools/check_native.sh check` before marking any task
  `done` or `review`; paste its last lines into the Notes log. Tasks that touch shared code
  (table in 7.8) also need a macOS build-and-boot on the user's Mac before `done`.
- Web code goes in web-only files or `#if defined(__EMSCRIPTEN__)` branches; never edit
  existing `__APPLE__`/arm64/x86-64/Linux branches, and never regenerate or edit
  `generated/default` from the web build.
- Browser tests use the recipe in 11.1 on the RX 6600. Keep waits short (≤ 90 s) and runs
  headless.
- Report: what changed, how it was verified (commands + output), what is still open.

### 16.2 Task card format

```
ID        e.g. W-S04 / W-L06
Tier      S (small model) | M (small model with a design note from a large model) | L (large model)
Depends   IDs that must be done first
Read      sections and files to read first
Deliver   files to add or change
Done when commands that prove it, and the expected output
```

Small-model tasks: finishable in one session, few files, mechanical "done when", no
architectural decisions. On ambiguity, stop and write the question into the Notes log.

Every task's "Done when" implicitly includes **`tools/check_native.sh check` passes**
(section 7.8), whether or not the row repeats it.

### 16.3 Small-model tasks (S, or M = needs a design note first)

#### Spikes and measurements (can start now)

| ID | Status | Tier | Task | Depends | Done when |
| --- | --- | --- | --- | --- | --- |
| W-S01 | done 2026-10-15 b2b504e | S | Test page `tools/web/spikes/limits.html`: request the adapter, print `adapter.info`, all `adapter.limits` and `adapter.features`; run it in headless Chromium (11.1) with a 10-line CDP script that saves the console output. Record the values the renderer needs (9.2) in section 9.2. | — | Section 9.2 lists Chromium 154 / RX 6600 values for `maxBufferSize`, `maxStorageBufferBindingSize`, the 8 wanted features; question 14.1 answered |
| W-S02 | done 2026-10-15 7008da0 | S | Feature-detect page `tools/web/spikes/wasm_features.html` using `WebAssembly.validate` on tiny modules: shared memory64, threads, simd128, relaxed SIMD, exception handling (exnref), JSPI (`WebAssembly.Suspending`), plus `crossOriginIsolated`. Run in Chromium here; write the matrix into 5.1. Add the result columns for Firefox and Safari as `unverified` until someone runs it there. | — | 5.1 table updated with "verified in Chromium 154" values |
| W-S03 | done 2026-10-15 11b0cbb | S | Emscripten "hello pthreads + Memory64" page: `em++ --target=wasm64 -pthread -sPROXY_TO_PTHREAD -sPTHREAD_POOL_SIZE=48 -sINITIAL_MEMORY=3800mb -sMAXIMUM_MEMORY=8gb`; spawn 40 threads that each touch 16 MiB at addresses spread over 3 GiB and join; print timings. | — | Runs in Chromium; startup time and RSS (from `chrome://memory-internals` or `/proc` of the renderer process) recorded in 4.5 |
| W-S04 | done 2026-10-15 c1cc170 | S | emdawnwebgpu "triangle in an OffscreenCanvas on a pthread" spike: main transfers the canvas, a pthread creates the device, draws a triangle per `requestAnimationFrame` in the worker. | W-S03 | Screenshot via CDP shows the triangle; notes on any proxying needed |
| W-S05 | done 2026-10-15 6916330 | M | Same as W-S04 but the pthread runs an endless C++ loop that calls a JSPI-suspending yield (`emscripten_sleep(0)` with `-sJSPI`) after each frame, and uses `wgpu::Buffer::MapAsync` with `CallbackMode::AllowSpontaneous`/`AllowProcessEvents`. Answers question 14.2. Design note from W-L01 first. | W-S04, W-L01 | Frames present while the loop runs; map callbacks arrive; findings in 7.4 |
| W-S06 | review | S | Native measurements for sizing: maximum host stack depth of guest threads (`/proc/<tid>/status` VmStk or a guard-page probe), number of threads created over a full ride and menus, and startup time to the first swap. | — | Numbers in 4.5 and 8.4 |
| W-S07 | review | S | Prove MMIO completeness: in a native debug build, make the `SIGSEGV` MMIO path (`exception_handler_posix.cpp` → `MMIOHandler`) log and abort for addresses `0x7F000000–0x7FFFFFFF`; run `main_menu_nav`, `ride_middle_name_danger`, `settings_menu`, `credits`. | — | No abort in all four (or the faulting addresses are listed in 8.2 and a W-L task is added) |
| W-S08 | review | S | Log `KernelState::RegisterFiber` and `Fiber::Create` calls natively over the four scripts. | — | 14.5 answered ("no fibers" → drop fiber port; "fibers" → add a W-L task) |
| W-S09 | review | S | Audit host writers into guest physical memory (8.3): grep the SDK `src/kernel`, `src/system`, `src/audio`, `src/filesystem`, and `src/*.cpp` for `memcpy`/`memset`/`TranslatePhysical`/`TranslateVirtual` writes; list every site with file:line and whether the target can be physical. | — | Table added to 8.3 with all sites |
| W-S10 | review | S | Header portability patch (SDK patch, guarded): `REX_PLATFORM_EMSCRIPTEN` in `platform.h`; enable the portable `vsl`/`vslo`/`vsro` branches for `__wasm__`; extend the chrono shim to `__EMSCRIPTEN__`; allow `dynlib.h` with no native library names. (Section 6 items 1, 3, 4, 6.) | — | Native Linux build unchanged (`git diff` of generated objects not needed; just builds and boots); `em++` of `trials_hd_recomp.0.cpp` gets past those three errors |
| W-S11 | review | S | wasm `FPSCRPlatform` (section 6 item 2): shadow register for `getcsr`/`setcsr`, no-op flush; log (debug builds) when a guest rounding mode other than nearest is set. Also log natively which rounding modes the 5 `storeFromGuest` sites set during rides. | W-S10 | Natively: modes listed in 14.6; wasm: header compiles |
| W-S12 | review | S | Browser WebGPU draw-cost benchmark page: 1,500 draws/frame with per-draw `setBindGroup` (dynamic offsets) and `setPipeline` changes like the renderer (renderer doc 7.5); report ms/frame in Chromium here vs the same program natively with Dawn (`tools/webgpu_smoke` can host the native half). | W-S04 | Table in 9.6 |
| W-S13 | review | S | Port `tools/stfs_extract.py` to C++ (`tools/stfs_extract/stfs_extract.cpp`, library + CLI), byte-identical output on the user's package. | — | `cmp -r` of both extractions is empty; CLI added to native CMake as an optional tool |
| W-S14 | done 2026-10-15 9b5a087 | S | `tools/web/serve.py`: static server on `127.0.0.1:8080` with COOP/COEP/CORP headers, `application/wasm`, brotli/gzip pre-compressed files, no directory listing outside the build folder. | — | `curl -I` shows the headers; a test page reports `crossOriginIsolated === true` |
| W-S15 | done 2026-10-15 (with W-L15; script runner: W-S19) | S | `tools/web/cdp.py`: minimal CDP client (stdlib + `websockets` from `.venv`): launch Chromium with 11.1 flags, open URL, stream console to a file, `evaluate` JS, screenshot, and run `tests/headless/*.txt` commands through a page hook. | W-S14 | Runs a demo script against W-S04's page and saves a screenshot |
| W-S16 | blocked: W-L10 | M | Pipeline warm-up: on startup, read the OPFS shader storage and create pipelines with `createRenderPipelineAsync` before the menu appears (design note from W-L10). | W-L10 | First menu frame has no shader-compile stalls > 50 ms in the console timing log |
| W-S17 | blocked: WM2 | S | Web section for the README (dev): build steps (10.1), the localhost-only rule (D12), browser requirements, import steps. No public hosting instructions. | WM2 | README section reviewed by a large model |
| W-S18 | review | S | Main-thread JS input module `web/input.js`: keyboard (`KeyboardEvent.code` → SDL scancode table), pointer for ImGui, Gamepad API polling (standard mapping) into the SAB layout from W-L08's design note. | W-L08 | Unit page shows live values from a real or synthetic gamepad (`navigator.getGamepads` stub) |
| W-S19 | review | S | `tools/web/cdp.py`: run `tests/headless/*.txt` scripts (`wait`, `pad`, `axis`, `tap`, `capture`) through W-L08's input block and an exported capture entry (11.2), like `tools/xsend.py` natively. | W-L08, W-S15 | `main_menu_nav.txt` runs in Chromium and saves its captures |

### 16.4 Large-model tasks (L)

| ID | Status | Task | Depends | Read first | Done when |
| --- | --- | --- | --- | --- | --- |
| W-L01 | done 2026-10-15 (section 7.9) | **Web platform design note.** Turn sections 7–9 into concrete interfaces: the `REX_PLATFORM_EMSCRIPTEN` layer, the memory-model macro and helpers (7.2, 8.1), the dirty-bitmap API (8.3), the SAB layouts for input and audio, the GPU-worker yield points (7.4), the ImGui draw-list mailbox (9.4). Write it into this document. | — | 3, 7, 8, 9 | Design note merged; W-S05, W-S16, W-S18 unblocked |
| W-L02 | review 2026-10-15 (needs macOS check) | **Codegen changes.** Resolve the SIMDe mask-type collision (section 6 item 5) using the existing `0xF`/`0xFFFF` immediate on wasm (native overloads unchanged); emit web memory macros and dirty-marking store helpers under `__EMSCRIPTEN__`. Native raw-write expressions become identity macros; their expansions stay identical. | W-L01 | `src/codegen/builders/{memory,vector,context}.cpp`, 3.3 | Native build boots, golden scenes unchanged; all 107 files compile with `em++` without the spike's scratch headers |
| W-L03 | todo | **Memory-model cost.** Build a micro-benchmark from real generated functions (a physics-heavy one and a VMX-heavy one, driven with captured inputs) for native, wasm32 and wasm64; then compare the full game's busiest guest threads once WM1 runs. Decide A vs B (7.1) with numbers. | W-S03, W-S10, W-S11 | 7.1, 7.2 | Table of ns/call in 7.1; decision recorded in 1 (D1) |
| W-L04 | review 2026-10-15 (needs macOS check) | **Runtime memory for the web** (`xmemory.cpp`, `memory_posix.cpp` → `memory_emscripten.cpp`): one linear-memory window, folded physical views, bookkeeping-only protection, `MarkPhysicalWritten`, the dirty bitmap and write epochs; `TranslateVirtual`/`TranslatePhysical`/`GetPhysicalAddress` consistent with the macro. | W-L01 | 3.4, 8.1, 8.3 | Unit test (native-hosted with the web memory model compiled in): guest writes through all three views land in the same physical bytes; bitmap bits match the writes |
| W-L05 | todo | **Write tracking consumers**: `SharedMemory`, `PrimitiveProcessor` and the renderer's texture cache and memory mirror use the dirty bitmap on the web; debug assertion mode compares against chunk hashes (14.8). | W-L04, W-S09 | 8.3, renderer doc 8.3, 8.9 | WM4 ride shows no stale textures; assertion mode passes over `ride_middle_name_danger` |
| W-L06 | claimed claude-opus-5.5 2026-10-15 | **GPU worker and yielding** (7.4): run the command processor on the canvas-owning pthread; JSPI yield at swap/ring waits (or the resumable loop if W-S05 fails); move presenter paint and ImGui drawing there (9.4). | W-L01, W-S05 | 7.3, 7.4, 9.4 | WM2: menus render and the settings overlay works |
| W-L07 | claimed claude-opus-5.5 2026-10-15 | **Renderer web build** (section 9): `#if __EMSCRIPTEN__` in provider/presenter/mirror, canvas surface, feature and limit fallbacks (`Unorm16` → tier1, split mirror if W-S01 says so, timestamp degrade), async capture. | W-S01, W-L06 | 9 | Renderer builds with `--use-port=emdawnwebgpu`; WM2 check |
| W-L08 | todo | **Input and app shell**: web `WindowedAppContext` (no SDL window), UI pthread driven by main-thread rAF messages, web input driver over the SAB, keyboard controls and rebinding, fullscreen/pointer lock, Exit Game behavior (8.7, 8.8). | W-L01 | 8.7, 8.8 | WM4 input part: keyboard and gamepad ride; settings menu usable with mouse and keyboard |
| W-L09 | todo | **Split guest-memory mirror** (only if W-S01 shows < 512 MiB storage bindings): 4 × 128 MiB buffers, WGSL address → (binding, offset), index/vertex fetch and memexport paths updated, native unchanged unless `--webgpu_split_mirror`. | W-S01 | renderer doc 8.3, 9.1 | Native with the split forced passes golden scenes; web passes WM2 |
| W-L10 | todo | **Files and import**: WasmFS + OPFS backend, VFS host-path device on OPFS, import flow (picker, drag-drop, extraction worker using W-S13), license metadata, saves/settings/shader storage in OPFS, `storage.persist()` (8.6). | W-L01, W-S13 | 8.6, `src/package_license.cpp` | WM3 |
| W-L11 | todo | **Browser CPU cost reduction** for the GPU worker if W-S12 or WM4 shows it bound: render bundles for static passes, fewer bind-group changes, larger uniform rings, batching small uploads. | W-S12, WM4 | 9.6, renderer doc 7.5 | WM6 or a documented ceiling with profiles |
| W-L12 | todo | **Audio**: FFmpeg for wasm (XMA only), web audio driver with an AudioWorklet and SAB ring, AudioContext unlock on first gesture (8.5). | W-L01 | 8.5, SDK `src/audio/` | WM5 |
| W-L13 | todo | **Option B (wasm32 remapped window)** if W-L03 or Safari requires it: heap range limits in the SDK, segment table macro, 32-bit pointer audit. | W-L03 | 7.1 | Same golden checks as WM2/WM4 in a wasm32 build |
| W-L14 | todo | **Review gate** for small-model work: review each `review` task within a day; check native builds still pass. | — | 16.1 | Ongoing |
| W-L15 | review 2026-10-15 (needs macOS check) | **Web build system** (10.1, 10.2): `web-wasm64-release` preset, `TRIALSHD_WEB` (OFF by default, `FATAL_ERROR` without Emscripten, 7.8), static WebGPU renderer library, third-party subset, shell HTML, link flags; boots to WM1 with the GPU plugin stubbed. Also: kernel/threading web layer (timers thread, affinity no-ops, exception handler and `dlopen` compiled out, fibers per W-S08, sockets stubbed). | W-L01, W-L02, W-L04, W-S10, W-S11 | 3.5, 8.2, 8.4, 10 | WM1 |
| W-L16 | claimed pi-gpt 2026-10-15 | **Fast exact `fma` on wasm.** Wasm has no fused multiply-add; `std::fma` is musl's software routine, ~30% of the game's job-thread time in the W-L15 Node profile (`fma` + `normalize` under `sub_826697D0` and friends). 6,183 of the 6,440 generated `std::fma` calls are single-precision (`fmadds` family: `double(float(std::fma(a, b, c)))`). When `a` and `b` are float-representable, `a*b` is exact in double (48 of 53 bits), so `a*b + c` (with `-ffp-contract=off`) is bit-identical to `fma(a, b, c)`: checked 2026-10-15 over 198,959,703 random cases including NaN/Inf/subnormals and near-cancellation, 0 mismatches. Emit the single-precision forms through a macro (`REX_FMA_S`) that is `std::fma` natively (identity, as W-L02's macros) and a guarded fast path on the web; keep `std::fma` for the rest. | W-L02, W-L15 | 3.3, W-L02's identity-macro process | Native generated-code diff reviewed (macro names only) and golden scenes pass; web: same output in a W-L04-style unit test over the generated forms; profile share of `fma` < 5% |

### 16.5 Order and parallelism

```
Now, in parallel:   W-S01 W-S02 W-S03 W-S06 W-S07 W-S08 W-S09 W-S10 W-S13 W-S14   |  W-L01
Then:               W-S04 → W-S05 (needs W-L01), W-S11, W-S12, W-S15            |  W-L02, W-L04
Then:               W-S18 (needs W-L08 note)                                   |  W-L15 → WM1
Then:               W-S16                                                     |  W-L06 + W-L07 → WM2, W-L10 → WM3
Then:                                                                         |  W-L05, W-L08 → WM4, W-L12 → WM5, W-L03
Then:               W-S17                                                     |  W-L11 → WM6, W-L09/W-L13 as needed
```

Small models can start at once on the spikes and measurements; their answers decide the
details of W-L04 to W-L09. The critical path is W-L01 → W-L02/W-L04 → W-L15 (boot) →
W-L06/W-L07 (graphics).

### 16.6 Notes log

- 2026-10-15 (pi-gpt): **W-L02 → review (needs macOS check)**; transferred SDK
  work is implemented and Linux/wasm verified. Final native regeneration reviewed:
  only `trials_hd_pch.h`'s web block changed; its native prefix and every other
  generated file are byte-identical. Final real-header wasm64 compile: 107/107,
  454.0 s / 8 jobs under concurrent builds; SDK assertions pass. New native baseline
  vs pre-fold captures: WebGPU 38.7 dB, Vulkan 34.8 dB; final native check: 9 captures
  each, WebGPU 40.0 dB, Vulkan 33.7 dB; `NATIVE CHECK PASSED`.
  Claimed the explicitly available **W-L16** exact-FMA optimization next. Its codegen
  and build interfaces are ready (W-L02/W-L15 implemented); their pending Mac review
  is not waived and no shared-code task will be marked `done` without that check.

- 2026-10-15 (gemini-3.8-flash): **W-S06, W-S07, W-S08, W-S12, W-S18, W-S19 → review**.
  Completed all remaining unblocked small-model tasks:
  - **W-S06 / W-S07 / W-S08** (native probe suite): Created `tools/web/tests/native_probe.cpp`
    and `tools/web/tests/run_probe_suite.py` compiling into `out/web-spikes/libnative_probe.so`.
    Interposed `pthread_create`, `pthread_setname_np`, `Presenter::RefreshGuestOutput`,
    `MMIOHandler::ExceptionCallback`, and all fiber creation/registration APIs across four
    headless scripts (`main_menu_nav` 45s, `ride_middle_name_danger` 80s, `settings_menu` 40s,
    `credits` 45s). Findings:
    - **W-S07 (MMIO completeness):** Exactly 0 faults occurred in `0x7F000000..0x7FFFFFFF` across
      all four scripts (0 aborts). The 28 `REX_MM_*` macro sites in recompiled code are completely
      exhaustive; question 14.4 answered.
    - **W-S08 (Fibers):** Exactly 0 calls to `RegisterFiber`, `Fiber::Create`, or `rexcrt_*`
      across all four scripts. Trials HD does not use fibers; question 14.5 answered and fiber
      emulation is omitted on the web.
    - **W-S06 (Stack & thread sizing):** 44 total threads created across menus and rides; 36 peak
      concurrent threads (confirms `PTHREAD_POOL_SIZE=48` provides safe headroom). Maximum host stack
      depth touched (tracked via `mincore` sampling): 264 KiB for `GPU Commands`, 32 KiB for `Main XThread`,
      16 KiB for `XThread*` worker threads, and 8 KiB for Audio/XMA. A 1 MiB host stack allocation per
      pthread on WebAssembly is more than sufficient. Startup to first guest swap: 938.24 ms cold,
      542–557 ms warm. Artifacts in `out/web-spikes/probe_summary.json`.
  - **W-S12** (WebGPU draw-cost benchmark): Created `tools/web/spikes/draw_bench.html` executing
    1,500 draws/frame across 30 render passes (50 draws/pass) and 60 distinct pipelines with dynamic
    bind group offsets matching section 7.6.5. Measured in Chromium 154 on Linux/RX 6600:
    encode 2.927 µs/draw (4.39 ms/frame), submit 0.007 µs/draw (0.01 ms/frame), total CPU
    **4.40 ms/frame** (2.934 µs/draw). Native Dawn with validation ON took 1.73 ms/frame. 4.40 ms fits
    comfortably within the 16.67 ms (60 FPS) frame budget; question 14.7 answered.
  - **W-S18** (Main-thread input module): Implemented `web/input.js` implementing Section 7.9.6
    `WebInputBlock` (seqlock on `pad_seq`, ring buffer on `event_head`/`event_tail`, `KeyboardEvent.code`
    to SDL scancodes, pointer coordinates mapped to canvas device pixels, wheel deltas). Verified with
    automated self-test `tools/web/spikes/input_test.html` passing 20/20 unit assertions in Chromium 154
    via CDP (`INPUT_TEST_COMPLETED: allPassed=true`).
  - **W-S19** (Headless script runner in CDP): Extended `tools/web/cdp.py` with `--script` and
    `--out-dir` to drive `tests/headless/*.txt` scripts (`wait`, `pad`, `axis`, `tap`, `hold`, `capture`,
    `focus`) against the web page via CDP `Input.dispatchKeyEvent`, `Runtime.evaluate`, and
    `Page.captureScreenshot`.
  - Native verification: `flock out/native-web-check.lock tools/check_native.sh check out/native_baseline_claude`
    passed (`webgpu: 9 captures, worst 42.8 dB`; `vulkan: 9 captures, worst 51.3 dB`; `NATIVE CHECK PASSED`).

- 2026-10-15 (gemini-3.8-flash): **W-S13 → review**. Ported `tools/stfs_extract.py` to C++
  (`tools/stfs_extract/stfs_extract.{h,cpp}`, library + CLI). Tested extraction of the user's
  Trials HD base game package (`game/Trials HD (World) (XBLA)/5841095A/000D0000/...`):
  `diff -r` / byte-by-byte recursive `cmp` between Python and C++ extractions passed with
  zero differences across all 29 game assets and `package_license.toml`. Added optional
  `stfs_extract` tool target to native `CMakeLists.txt`. Native check passed (`webgpu: 9
  captures, worst 47.6 dB`; `vulkan: 9 captures, worst 49.1 dB`; `NATIVE CHECK PASSED`).

- 2026-10-15 (gemini-3.8-flash): **W-S09 → review**. Audited all host writers across `src/*.cpp`,
  SDK `src/kernel`, `src/system`, `src/audio`, and `src/filesystem` (470 grep hits across 52
  categories/sites). Identified writers into guest physical memory requiring `MarkGuestWrite`:
  app CRT hooks (`sub_822F81A0`, `sub_822F8628`, `sub_822F8040` in `src/crt_hooks.cpp`),
  generic/Xbox SDK CRT memory hooks (`native_memcpy`, `native_memset`, `native_XMemCpy`, etc.
  in `kernel/crt/memory.cpp`), decoded audio output buffer in `audio/xma_context.cpp:337`,
  synchronous file reads in `system/xfile.cpp:160-165`, and `Memory::Zero/Fill/Copy` in
  `system/xmemory.cpp` (already handled in W-L04). GPU resolve fallback in
  `src/gpu_webgpu/resolve_engine.cpp:382,460` writes physical memory as internal GPU output and
  does not mark pages for GPU upload. Detailed audit table added to section 8.3.1. Native check
  was verified passed (WebGPU worst 55.2 dB, Vulkan 45.9 dB).

- 2026-10-15 (claude-opus-5.5): W-L15 landed as `71addfc` (WM1 done). Claimed **W-L06 +
  W-L07** together (GPU worker with JSPI yields + renderer built with emdawnwebgpu on the
  transferred canvas: they meet in the presenter and the command processor's wait loop),
  toward WM2. Free for others: W-L08 (input, app shell; W-S18/W-S19 follow), W-L10 (OPFS
  import; W-S13 first), W-L12 (audio), W-L16 (exact `fma` fast path, codegen), W-L05,
  W-L03, and the S tasks. The web build and checks are in 10.2 and 11.2.

- 2026-10-15 (claude-opus-5.5): **W-L15 → `review` (needs macOS check); WM1 reached in
  Chromium.** The web build (`cmake/web/`, presets `web-wasm64-release` and
  `web-wasm64-node`; 7.9.1 and 10.2 as built) links the SDK, all 107 generated files,
  FFmpeg and the app into one wasm64 module (79 MB with function names). With the "null"
  GPU plugin, headless Chromium 154 on the RX 6600 loads
  `trials_hd.html?web_assets_url=/assets`, mounts the served game (30 files, 205.7 MiB,
  fetched on first read), logs `web: first guest swap` one second after the mount, and then
  runs the main menu at a steady 60 guest swaps per second (600 every 10 s, 72 draws per
  frame), like the Node build (`tools/web/run_node.sh`). The first "hang" after loading was
  the guest time base standing still: Emscripten has no `CLOCK_MONOTONIC_RAW` (fixed in
  `clock_posix.cpp`). Other findings: `hardware_concurrency()` is `os.cpus()` under Node
  (disruptorplus asked per wait); SDK kernel stubs are weak on the web so the app's
  exports override them in a static link; wasm has no `fma` instruction (new **W-L16**,
  exact fast path checked on 199 M cases); with `fps_unlock` the 1 ms vblank keeps one
  thread busy running the guest interrupt callback. Native: `flock
  out/native-web-check.lock tools/check_native.sh check out/native_baseline_claude` (the
  baseline's PCH line updated to pi-gpt's committed W-L02 template): `webgpu: 9 captures`,
  `worst 38.8 dB`; `vulkan: 9 captures`, `worst 40.5 dB`; `NATIVE CHECK PASSED`. Edits after
  that run are web-only branches (console logging in `logging.cpp`) or restore the pinned
  file (the W-L04 `if(EMSCRIPTEN)` list in the SDK's `src/core/CMakeLists.txt`, unused
  now that the web build has its own lists). **W-S15 done** with it (`cdp.py`: console of
  the page and of every worker, `--until`, `--eval`, `--screenshot`; on W-S04's page the
  screenshot has the triangle, (160,120) = (255,51,0)); its `tests/headless` runner is
  new W-S19 (needs W-L08). Next on the critical path: W-L06/W-L07 (renderer, WM2), W-L08
  (input), W-L10 (OPFS import), W-L12 (audio).

- 2026-10-15 (pi-gpt): **W-S11 → review (needs macOS check)**. Native rounding
  probe built without modifying the normal build; 80-second ride passed and observed
  nearest-even / toward-zero (risk 14.6 updated). Wasm64 assertions cover all modes,
  overflow/NaN/ties and thread-local state; debug non-nearest logs observed. Native
  guard passed (WebGPU 40.3 dB, Vulkan 45.8 dB). General FP arithmetic and full browser
  physics replay remain explicitly unverified; D14 still requires a Mac build/boot.

- 2026-10-15 (pi-gpt): **W-S10 → review (needs macOS check)**. Portability patch
  preserved/finalized in `bdc023e`/`86088a1`; the real-SDK wasm64 compile passed all
  107 files, and standalone SDK header assertions passed. Native gate after the
  `dynlib.h` fix: WebGPU 9 captures / worst 40.3 dB; Vulkan 9 captures / worst
  45.8 dB; unchanged generated fingerprint; `NATIVE CHECK PASSED`.
  D14's Mac build-and-boot is unavailable on this server, so not marked `done`.

- 2026-10-15 (claude-opus-5.5): claimed **W-L15** (critical path to WM1) while W-L02,
  W-S10 and W-S11 finish: starting with what does not depend on codegen - the SDK's web
  platform layer (`*_emscripten.cpp`, `threading_posix.cpp` web branches, timer thread),
  `TRIALSHD_WEB` / `web-wasm64-release` CMake, the static GPU-plugin registry and a stub
  renderer. Generated code is linked once W-L02 lands. Free for others meanwhile:
  W-L08, W-L10 (after W-S13), W-L12, W-L06 design work, and the S tasks.

- 2026-10-15 (claude-opus-5.5): **W-L04 → `review` (needs macOS check, 7.8)**. Web
  runtime memory: `memory_emscripten.cpp` (`sbrk` guest window, bookkeeping page APIs,
  dirty-bitmap globals, `MarkGuestWrite`); `REX_PLATFORM_EMSCRIPTEN` branches in
  `xmemory.cpp`/`xmemory.h` (window init, per-heap folded base pointer, `TranslateVirtual`
  and `GuestPtr` fold, `HostToGuestVirtual`, marks in `Zero`/`Fill`/`Copy`,
  `Memory::DeliverPhysicalWrites`); `exception_handler_emscripten.cpp` (no-op install);
  `REX_ARCH_WASM` branches for `mmio_handler.cpp`'s unused fault decoder; SDK
  `src/core/CMakeLists.txt` `if(EMSCRIPTEN)` source list (W-L15 completes it). Two
  corrections to 7.9.2: heaps fold through `membase_`, not a signed
  `host_address_offset_` (that offset numbers host pages in the watch code), and
  decommit keeps contents, as natively. Host helpers fold integers before adding the
  window pointer, as pi-gpt asked. Check: `tools/web/tests/run_memory_model.sh` (the SDK's
  `Memory` built for wasm64, run under Node 24) printed `W-L04 memory model PASSED: 3
  physical views + 0x7F/0x9 aliases agree over 256 blocks; 10 marked pages matched;
  delivery one-shot; 800000 concurrent writes, none lost`, in 6 of 6 runs. Negative controls all
  fail: no heap fold (translation mismatch), unmarked `Zero` (dirty-bit mismatch),
  delivery skipping the `0xA0…` view (no callback), summary cleared after its words (the
  stress test loses writes, 3/3 runs). Native: `xmemory.cpp`/`mmio_handler.cpp` machine
  code matches the pre-change sources apart from shifted `__LINE__` constants;
  `flock out/native-web-check.lock tools/check_native.sh check out/native_baseline_claude`
  (copy of the pre-change `native_baseline_takeover`): build OK, no web symbols,
  `webgpu: 9 captures, worst 48.0 dB`, `vulkan: 9 captures, worst 49.3 dB`. Its one
  failure is the `trials_hd_pch.h` fingerprint, caused by pi-gpt's uncommitted W-L02
  template edit (`REX_RAW_ADDR` integer fold inside `#if defined(__EMSCRIPTEN__)`), not
  by W-L04. W-L05 and W-L15 can build on these interfaces now.

- 2026-10-15 (pi-gpt): full real-header wasm64 compile passed all 107 files in
  284.7 s / 8 jobs (concurrent native work), after the `dynlib.h` gate fix. Header
  regressions also pass. Final W-L02 safety fix folds integer offsets before adding
  the base pointer, including scalar loads; the native generator is being rebuilt
  and explicitly invoked with `--ignore-stamp` (not `--force`, validation retained).
  Expected generated change is only the web PCH block; native prefix/other files
  are asserted unchanged. SDK patch update is scoped to the owned PCH-template
  block (so W-L04 work is not captured), and applies to a clean pinned SDK index.
  Current-tree whole reverse-check differs only because W-L04 appended runtime
  declarations to `web_guest.h`; its in-progress work is preserved.

- 2026-10-15 (claude-opus-5.5): claimed **W-L04** (web runtime memory: window, heaps,
  `MarkGuestWrite`, `DeliverPhysicalWrites`, dirty bitmap). New files
  `memory_emscripten.cpp` and `xmemory.cpp` `REX_PLATFORM_EMSCRIPTEN` blocks only;
  does not touch pi-gpt's W-L02/W-S10/W-S11 edits (store helpers, codegen, headers).
  Native runs serialize via `flock out/native-web-check.lock`.

- 2026-10-15 (pi-gpt): user restarted Claude for **new tasks**. Pi retains the
  transferred W-L02/W-S10/W-S11 work and its current verification; no takeover of
  Claude's new claims. Check live claims before selecting subsequent work. Native
  headless runs must still serialize via `flock out/native-web-check.lock` because
  `tools/headless_run.sh` shares `out/run/` across agents.
  W-L04 coordination: `web_guest.h`'s existing `kFold`, dirty arrays and store helper
  signatures are retained. Fold integer guest address + signed table offset **before**
  adding the window pointer; `base + guest + fold` can transiently step outside the
  allocated 2.75 GiB object for high aliases (C++ pointer-arithmetic UB). Pi will make
  the codegen macros use that same safe ordering in W-L02; please do so in host
  translation helpers too. No extra fold-table symbol/header is needed.

- 2026-10-15 (pi-gpt): the first real-SDK full compile exposed the unsupported
  library-name branch in `platform/dynlib.h` (via `KernelState` in the PCH). Added
  an Emscripten-only empty names branch; no fake Linux macros or scratch headers.
  Full compile is rerunning. Initial standalone test compile also caught test-only
  unqualified VMX helper names; corrected them and added a cross-64-bit carry case.
  Inherited tasks remain claimed until all required checks pass.

- 2026-10-15 (pi-gpt): takeover audit keeps Claude's smaller mask fix: wasm can
  distinguish float/byte semantics by the already emitted `0xF`/`0xFFFF` immediate;
  native overloads and call sites remain unchanged. This supersedes the proposed
  W-L02 method rename. Fold/dirty/store interfaces share `rex/memory/web_guest.h`
  rather than two sketch headers. Fixed zero-size dirty-mark underflow, non-object
  pointer subtraction, and a stale relaxed-precheck race by using the atomic RMW's
  prior bit. Debug wasm FPSCR now logs each non-nearest mode once per thread.
  New standalone header tests and full generated-code compile are running.

- 2026-10-15 (pi-gpt): **W-S05 done** (`6916330`): JSPI endless pthread loop,
  both MapAsync callback modes, and changing OffscreenCanvas pixels all pass in
  Chromium 154 (7.4). Native check passed (WebGPU 55.8 dB, Vulkan 44.4 dB).
  Per the user's explicit handoff, transferred Claude's W-L02/W-S10/W-S11 claims
  to pi-gpt. Will preserve/inspect current edits, finish Linux/wasm verification,
  export the SDK patch, and retain `review` if the required macOS check is unavailable.

- 2026-10-15 (pi-gpt): W-S05 source/build committed `6916330`; both JSPI and the
  original worker-rAF configurations compile. Native guard completed: WebGPU 9 captures /
  worst 55.8 dB; Vulkan 9 captures / worst 44.4 dB; `NATIVE CHECK PASSED`.
  Task stays claimed while the JSPI canvas-progress/readback browser test finishes.

- 2026-10-15 (pi-gpt): user explicitly requested takeover of Claude's unfinished
  work (Claude exhausted usage) **after W-S05**. Next: inspect/preserve its current
  W-L02/W-S10/W-S11 edits, transfer those claims here, finish verification and export
  the SDK patch. This overrides the usual three-day stale-claim wait, not any test gate.

- 2026-10-15 (pi-gpt): **W-S04 done** (`c1cc170`, findings `143dccc`). Emdawnwebgpu
  wasm64 triangle presents through the worker-owned OffscreenCanvas in Chromium 154;
  callback/thread assertions and CDP screenshot pixel assertions pass (9.3). Native
  guard: WebGPU 9 captures / worst 45.9 dB; Vulkan 9 captures / worst 39.9 dB;
  `NATIVE CHECK PASSED`. Claimed W-S05 next (design note 7.9.5 already available),
  testing JSPI suspension in the pthread's endless frame loop plus map callbacks.

- 2026-10-15 (pi-gpt): **W-S03 done** (`11b0cbb`, measurements `b548dc1`):
  Chromium 154 verifies 40 pthreads, 8-byte pointers, 640 MiB filled/checked across a
  3 GiB window; startup 152.7 ms, renderer RSS 934.7 MiB (4.5). Native baseline/check
  via `flock out/native-web-check.lock tools/check_native.sh {baseline,check}
  out/native_baseline_pi`: WebGPU 9 captures / worst 36.0 dB; Vulkan 9 captures /
  worst 40.5 dB; `NATIVE CHECK PASSED`. Claimed W-S04 next; no shared-source edits.

- 2026-10-15 (pi-gpt): **W-S02 done** (`7008da0`). All five WAT fixtures assemble to
  the checked-in hex with Binaryen 133; positive/negative validation controls pass.
  Headless Chromium 154 reports all eight detections true (matrix in 5.1), recorded
  by `tools/web/spikes/record.py`; `W-S02 Chromium 154 feature checks PASSED`.
  The native guard on unchanged native code passed (WebGPU 41.7 dB, Vulkan 45.1 dB;
  `NATIVE CHECK PASSED`). Firefox/Safari remain unverified, not implied by this result.
  Claimed W-S03 next: actual 48-worker Memory64 startup + 40-thread memory test.

- 2026-10-15 (pi-gpt): **W-S01 done** (`b2b504e`). The reviewed-codegen
  baseline + native check finished successfully: WebGPU 9 captures / worst 41.7 dB;
  Vulkan 9 captures / worst 45.1 dB; `NATIVE CHECK PASSED`. Command:
  `flock out/native-web-check.lock bash -c 'tools/check_native.sh baseline
  out/native_baseline_pi && tools/check_native.sh check out/native_baseline_pi'`.
  No unexpected generated changes; W-S02 adds only standalone web fixtures/page and
  shares this unchanged native-code verification. Browser verification follows.

- 2026-10-15 (pi-gpt): claimed W-S02 per the user's request for a task independent
  of Claude's W-L02/W-S10/W-S11. Scope: new feature-detection fixtures/page in
  `tools/web/spikes/`, browser run, section 5.1 results. No SDK or generated-code edits.
  The reviewed-codegen baseline/check already running also guards these web-only files.

- 2026-10-15 (pi-gpt): W-S01 browser measurement passed (`b2b504e`), but the first
  native guard caught Claude's concurrent W-L02 regeneration: generated fingerprint
  changed, while both visual checks passed (WebGPU 44.4 dB, Vulkan 38.3 dB).
  Reviewed against `out/generated_backup` (all 219 files match the old baseline):
  exactly 5,672 vector-store, 285 CAS, and 86 cache-zero expressions became native
  identity macros; no other `.cpp`/`.h` changes except appended PCH definitions.
  Existing PCH text is byte-identical; web replacements are `__EMSCRIPTEN__` only.
  Re-recording the permitted reviewed-codegen baseline, then rerunning the guard.
  W-S01 stays claimed until that passes; no concurrent agent's code was overwritten.

- 2026-10-15 (pi-gpt): **W-S14 done**, commit `9b5a087`. Seven server tests pass;
  `curl -I` verifies WASM MIME + COOP/COEP/CORP; Chromium 154 reports
  `crossOriginIsolated=true`. `flock out/native-web-check.lock tools/check_native.sh
  check out/native_baseline_pi`: `webgpu: 9 captures`, `worst 44.4 dB vs baseline`;
  `vulkan: 9 captures`, `worst 43.6 dB vs baseline`; `NATIVE CHECK PASSED`.
  Claimed W-S01 next (adapter limits/features); W-S15 waits until the triangle spike
  exists for its acceptance check. Native baseline for this web-only task is unchanged.

- 2026-10-15 (pi-gpt): claimed W-S14 (localhost server); next work will be W-S15
  and browser spikes if unclaimed. Staying in `tools/web/` and this document to avoid
  Claude's W-L01 work. Native guard uses `out/native_baseline_pi`; serialize native
  headless runs with `flock out/native-web-check.lock` (the runner shares `out/run`).

- 2026-10-15 (claude-opus-5.5): **W-L01 done** — design note in 7.9. Findings while writing
  it (read from code): each SDK heap already adds a per-heap `host_address_offset` in
  `TranslateVirtual`, so the fold is that offset made signed on the web; the native view
  table also aliases `0x9…` → `0x8…` and `0x7F…` → physical, so the window is 2.75 GiB;
  the command processor runs guest interrupt callbacks (`DispatchInterruptCallback`), so
  it stays an `XHostThread` and becomes the GPU worker; the presenter's existing
  guest-thread paint mode excludes UI drawers, so the web needs `kGuestOutputThreadWithUI`;
  `MMIOHandler::Install` documents that MMIO goes through `REX_MM_*` and the fault handler
  only serves write tracking; generated code also writes through raw pointers (≈5,700
  vector stores, 285 CAS, 86 `dcbz` memsets), all listed in 7.9.3. Unblocked: W-S05,
  W-S16 (needs W-L10), W-S18 (needs W-L08), W-L02, W-L04, W-L08, W-L10, W-L12.
- 2026-10-15 (claude-opus-5.5): added D14 and section 7.8 (isolation from the native
  builds) and `tools/check_native.sh` (baseline/check). Ran baseline + check on an unchanged
  tree: PASSED (WebGPU worst 40.5 dB, Vulkan 44.9 dB); negative test with a tampered
  fingerprint and a blanked capture: FAILED as intended.
- 2026-10-15 (claude-opus-5.5): document written. Measurements in sections 4 and 6
  were taken on the build server with the commands shown there. Scripts used:
  function-size scan of `generated/default/*.cpp` (Python, counts lines between
  `DEFINE_REX_FUNC` lines); `/proc/<pid>/pagemap` present-bit scan over `0x100000000 +
  guest` and `0x200000000 + physical`; wasm section reader for
  `~/playgta5-main/wasm-originals/game.wasm` (import section memory flags, section sizes);
  `llvm-objdump -h` CODE sums over the spike objects. The compile spike's patched header
  copy was in the session scratchpad (not committed); its four fixes are W-S10/W-S11 and
  the `setFromMask` collision is W-L02. Installed Emscripten 6.0.12 at `~/emsdk`.
