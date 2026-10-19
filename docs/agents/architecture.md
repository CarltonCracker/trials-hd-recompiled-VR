# Architecture

How the game becomes a native (or browser) program, and where our code plugs in.

## From `default.xex` to `trials_hd`

```
out/xex/default.xex ──rexglue codegen──▶ generated/default/*.cpp  (PowerPC → C++, gitignored)
        ▲                     ▲                    │
  user's own copy      config/codegen.toml         ▼
                       config/functions.toml   compiled with src/*.cpp, linked to
                                               librexruntime (SDK) → trials_hd
                                               + GPU plugins staged next to it:
                                                 librexgpu-xenos  (Vulkan)
                                                 librexgpu-webgpu (WebGPU, ours)
```

- `trials_hd_manifest.toml` tells codegen where the xex is, where to write
  (`generated/default`) and which config files to include.
- `generated/rexglue.cmake` (tracked, SDK boilerplate) finds the SDK (`REXSDK_DIR`, set
  by the presets to `thirdparty/rexglue-sdk`) and includes `generated/default/sources.cmake`
  when it exists.
- `generated/default/` holds ~106 `trials_hd_recomp.N.cpp` partitions (one C++ function per
  guest function, named `sub_<ADDRESS>`), headers, `trials_hd_init.{h,cpp}` (image config
  `PPCImageConfig` and the address → function table) and `trials_hd_pch.h`.
- The codegen target is `trials_hd_codegen`. On a fresh checkout nothing exists to compile
  yet, so `CMakeLists.txt` marks `trials_hd` `EXCLUDE_FROM_ALL` and adds
  `trials_hd_first_codegen`; `build.sh` handles the codegen → reconfigure → build handoff
  (see `build.md`).
- `config/codegen.toml`: `setjmp`/`longjmp` addresses, so codegen emits host
  setjmp/longjmp (recompiling the CRT's own versions corrupts frames).
- `config/functions.toml`: function starts the analyzer misses (only reached by tail call
  or through data pointers). Regenerate the data-referenced list with
  `tools/find_missing_funcs.py` (`game-hooks.md`).

## Guest code model

- Every recompiled function has the signature `void sub_X(PPCContext& ctx, uint8_t* base)`
  (`REX_FUNC` in `thirdparty/rexglue-sdk/include/rex/ppc/func.h`). `ctx` holds the PowerPC
  registers (`ctx.r3.u32` is the first argument and the return value); `base` is the host
  address of guest address 0.
- Guest memory is big-endian: read and write it with
  `rex::memory::load_and_swap<T>(base + addr)` / `store_and_swap<T>`.
- Each generated function is a **weak** symbol aliasing `__imp__sub_X` (the original
  body; on macOS a weak tail-call thunk). Defining `sub_X` in `src/` overrides it for every
  caller; call `__imp__sub_X(ctx, base)` to run the original. That is how all game hooks
  work (`game-hooks.md`).
- Guest threads are host threads. The SDK emulates the kernel/XAM (`thirdparty/rexglue-sdk/src/kernel`,
  `src/system`), memory (`src/system/xmemory.cpp`), audio (XMA via FFmpeg) and input (SDL).

## The app (`src/trials_hd_app.h`, `src/main.cpp`)

`TrialsHdApp : rex::ReXApp`, registered by `REX_DEFINE_APP(trials_hd, ...)`. The SDK
(`thirdparty/rexglue-sdk/src/ui/rex_app.cpp`) calls its hooks in this order:

| Phase | Hook | What we do |
| --- | --- | --- |
| Environment | `SetupEnvironment` (web only) | mount OPFS / finish first import |
| | `OnConfigurePaths` | find an installed `assets/` (next to exe, then cwd); keep saves, cache and config next to the exe; copy legacy `~/.local/share/trials_hd` saves |
| | `OnPostInitLogging` | apply the package license (`package_license.cpp`) |
| Presentation | `OnPreSetup` | pick the GPU plugin (default `xenos`; if `webgpu` can't start, Linux re-execs itself with `--gpu_plugin=xenos`), fallback audio, startup graphics settings; web: register the static plugins |
| | `OnConfigureStyle` | ImGui theme (`ui_theme.cpp`) |
| | `OnCreateDialogs` | F1 settings, F7 achievements, F2 performance overlay, controller shortcut watcher thread |
| Paths | `OnFinalizePaths` | window and ImGui up, no runtime yet: without an installed game (or with `--install_game`/`--install_dlc`) show the setup screen (`src/installer/`) and return `nullopt`; when it has installed, it applies the license and calls `resume(paths)` (deferred to the next UI tick), which constructs the runtime and launches |
| Runtime | `OnPostSetup` | Trials-tuned cvar defaults (no memexport readback, small resolve readback, page state, Vulkan submit batching), keyboard defaults, achievements, input gate while menus are open, virtual pad (tests) |
| Exit | `OnShutdown` | stop the watcher thread |

Cvar defaults set in `OnPostSetup` only apply when the user didn't set the cvar
(`GetFlagSource(...) == kDefault`). Keep that pattern.

## GPU plugins

The SDK loads a renderer as a shared library (`rex::system::LoadGpuPlugin(name)` →
`librexgpu-<name>.so`/`.dylib` next to the binary). The guest writes PM4 command buffers;
the SDK's backend-agnostic `CommandProcessor` (thread "GPU Commands") decodes them, and the
plugin's backend draws.

- `xenos`: Xenia's GPU emulation (EDRAM model, SPIR-V shader translation). Our Trials fast
  paths live in the SDK patch. See `renderer-vulkan.md`.
- `webgpu`: ours (`src/gpu_webgpu/`): host render targets instead of EDRAM, Xenos → WGSL at
  runtime, compiled with hidden visibility so it doesn't share state with `xenos`. See
  `renderer-webgpu.md`.
- Web: no `dlopen`; `webgpu` and a drawing-less `null` plugin are linked in and registered
  with `RegisterStaticGpuPlugin`.

## Threads worth knowing

UI thread (window, ImGui, presenter paint), "GPU Commands" (command processor; records all
GPU work), the guest's own threads (game logic, the guest render thread, audio), vblank /
timer threads, and our settings-shortcut watcher. On the web, the command-processor thread
is also the GPU worker that owns every WebGPU object (`web-port.md`).

## Deep reference (dev branch)

- `docs/web-port/README.md` §3: layers, process model, how generated code touches guest
  memory, the guest memory map.
- `docs/webgpu-renderer/README.md` §3: how ReXGlue renders; §6: Trials' Direct3D runtime,
  reverse engineered.
