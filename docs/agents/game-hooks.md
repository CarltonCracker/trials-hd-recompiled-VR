# Game hooks and reverse engineering

Game behavior is changed by replacing recompiled functions, never by editing
`generated/`. The game has no symbols: functions are known by address (`sub_8212BC68`)
and by what they do.

## How a hook works

Every generated function `sub_X` is a weak symbol aliasing `__imp__sub_X`, the original
body (`architecture.md`). A strong definition in `src/` replaces it for all callers,
including calls through the address table:

```cpp
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

REX_EXTERN(__imp__sub_8212BC68);              // the original body

// One wait iteration; returns 0 when the GPU is considered hung.
extern "C" REX_FUNC(sub_8212BC68) {           // ctx = guest registers, base = guest memory
  __imp__sub_8212BC68(ctx, base);             // run the original…
  /* …then add host-side behavior */
}
```

- Arguments arrive in `ctx.r3`, `ctx.r4`, … (floats in `ctx.f1`…); return in `ctx.r3`.
- Guest memory is big-endian: `rex::memory::load_and_swap<uint32_t>(base + addr)`,
  `store_and_swap`. Host pointer: `rex::memory::GuestPtr(base, addr)`.
- To call another guest function, declare it with `REX_EXTERN(sub_Y)` and set `ctx`
  registers (and `ctx.lr` to the return address the guest code expects, as
  `fps_unlock.cpp` does).
- Name the address, what the function does and how you identified it in a comment (see
  existing files). Global addresses go in named `constexpr` constants.
- Add the new `.cpp` to `TRIALSHD_SOURCES` in `CMakeLists.txt` **and** to the web source
  list in `cmake/web/trials_hd_web.cmake` if it should run in the browser.
- The game version is fixed (v0.0.1.2); addresses are only valid for it.

## Existing hooks (`src/`)

| File | Hooks | What it does |
| --- | --- | --- |
| `fps_unlock.cpp` (+ `frame_clock.h`, `render_pose.h`) | `sub_82664F98` (main loop), `sub_825EBF48`, `sub_8210CAA8`/`sub_8210CBE8` (matrix upload), `sub_825E9220`, `sub_825E7FC0` | Replaces the main loop: physics/updates stay on the original 16,666 µs fixed tick (max 5 catch-up steps), rendering runs at the FPS limit with interpolated model/camera poses. Cvars `fps_unlock` (restart), `log_simulation_rate` |
| `crt_hooks.cpp` | `sub_822F81A0`, `sub_822F8628` (memcpy), `sub_822F8040` (memset), `sub_822F8E60` (strncmp), `sub_822F8ED8` (strncpy), `sub_822F8EB0` (u64 → double), `sub_822F7D30` (strlen helper) | Native CRT routines instead of byte-by-byte guest loops (memcpy alone was ~6% of the render thread). Web: marks written pages for GPU tracking |
| `gpu_wait.cpp` | `sub_8212BB58`, `sub_8212BC68` | The D3D runtime's fence-wait loop (~700k calls/frame): progressive spin → yield → 20 µs sleep. Also sets Linux timer slack to 1 µs |
| `offline_warning.cpp` | `sub_8261ECE0`, `sub_8263B570` | Clears the "offline warning" mask at `0x82768498` so no popup before each track |
| `custom_track_list.cpp` | `sub_825BED48`, `sub_825BCB78` | Lists every custom track: refills the track list's 301-item enumeration buffer with the next batch when the game reaches its end |
| `exit_button.cpp` | `sub_8265BA38`, `sub_8265BC90`, `sub_8265C648`, `sub_82124A38` | Main-menu "Download Content" becomes "PC SETTINGS" and opens the host settings dialog; "Return to Game Library" becomes "EXIT GAME" and quits cleanly |
| `achievements_hook.cpp` | `sub_82124618`, `__imp__XamShowAchievementsUI` | Opens the SDK's ImGui achievements overlay |

The PC SETTINGS entry uses the existing Download control at `menu + 68`; Exit is
at `menu + 72`. After the game's caption refresh (`sub_8265BC90`), send its
source-text message through `sub_8265BB90`. Message 2016 copies UTF-16 through
`sub_821A0498` → `sub_82188B18` into the control's own storage. The generic
`XuiText` and control-text setters do not change this button's visible caption.
`guest_ui_text.h` supplies an aligned temporary guest call frame, big-endian text
and a copied PPC context so the original initialization result/registers survive.
Only this control changes; no localization files or shared strings are patched.
The click hook skips the old Xbox LIVE/download path and queues an idempotent
`ShowGraphicsSettings()` on the UI thread. Other button actions pass through.

Non-hook game integration: `package_license.cpp` (license mask from the user's package),
`TrialsHdApp::RegisterDefaultAchievements` (achievement list), the cvar defaults in
`OnPostSetup` (`architecture.md`).

## FPS unlock in one paragraph

With `fps_unlock=true` (default) the startup sets `present_fps_limit_guest_paced`, and
the hooked main loop `sub_82664F98` runs `Tick` + update dispatch for each 60 Hz step the
`FrameClock` owes, then one render dispatch when the render deadline (`present_fps_limit`,
capped at the VSync refresh rate) has passed. The render pass gets an interpolation
fraction (`g_render_alpha`); the matrix-upload hooks blend each model's and the camera's
previous and current poses (`render_pose.h`, unit-tested in `tests/render_pose_test.cpp`).
At ≤ 60 FPS each frame shows a whole tick, as the original did. `tests/check_fps_log.py`
and `tests/check_render_pose_log.py` check the logs of a run.

## Reverse-engineering workflow (tools are on `dev`)

1. **Dump the image:** `cmake --build <build> --target xex_dump`, then
   `out/build/<preset>/xex_dump out/xex default.xex out/xex_dump` → `image.bin`,
   `sections.txt`. Keep dumps in `out/` (gitignored, derived from game files).
2. **Disassemble:** `tools/disasm.py out/xex_dump 0x8212BC68 [before] [after]` (needs
   `capstone`; VMX128 prints as `.long`). The generated C++ in
   `generated/default/trials_hd_recomp.*.cpp` (`DEFINE_REX_FUNC(sub_X)`) is often easier to
   read.
3. **Find what runs:** configure with `-DTRIALSHD_CALL_TRACE=ON` (needs a codegen'd tree;
   range via `-DTRIALSHD_CALL_TRACE_RANGE="0x8212B000;0x82170000"`, default = the D3D
   runtime). `tools/gen_trace_shims.py` generates counting wrappers; run with
   `TRIALS_HD_CALL_TRACE_CSV=out/calls.csv` for per-second call counts, and
   `TRIALS_HD_CALL_TRACE_ARGS=<funcs>` + `TRIALS_HD_CALL_TRACE_ARGS_CSV` to sample r3–r8 and
   the return address.
4. **Missed functions** (crash in `InvalidFunctionTrap`): `tools/find_missing_funcs.py
   out/xex_dump generated/default --toml` and add the entries to `config/functions.toml`.
   Tail-call targets reported by codegen as `UnresolvedCall` go there too.
5. **Shaders:** `tools/xsh_dump.py` parses the shader cache (`cache/*.xsh`).

## Deep reference (dev branch)

`docs/webgpu-renderer/README.md` §6 maps Trials' Direct3D runtime (draw/state/resolve
functions and their addresses); `docs/vulkan-perf/PLAN.md` notes the hot guest functions
(`DrawInterpolated`, the guest render thread).
