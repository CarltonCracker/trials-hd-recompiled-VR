# Vulkan renderer (`xenos`)

The native default renderer: the SDK's Xenia-derived GPU emulation
(`thirdparty/rexglue-sdk/src/graphics/vulkan/`, plugin `librexgpu-xenos`). We don't fork it;
Trials-specific speedups are added to it through `patches/rexglue-sdk/trials-hd.patch`
(read `sdk-patches.md` first).

## Goal and state

- **Goal (user, 2026-10-17):** 500 FPS at 2560x1440 (2x render scale) on an RX 7800 XT
  (RADV, Ryzen 7 5700X), i.e. a 2.0 ms budget for the GPU and every render-path thread.
- Measured 2026-10-17 (RelWithDebInfo): 1440p ~310 FPS, 720p ~340 FPS, both CPU-bound. The
  bottleneck is the **guest render thread** (the game's own render code, ~3.5 ms/frame),
  not the GPU or the command-processor thread.
- User decision: **enhance the existing renderer, no rewrites** (the D3D front-end and
  frame-interpolation moonshots were dropped).

## Rules for Trials fast paths

1. **Fast path + fallback.** Each specialization checks its assumption (format, MSAA,
   primitive type, resolve shape…) and falls back to the generic Xenia code otherwise.
   Count fallbacks with `TRIALS_EXPECT(cond, counter)`
   (`include/rex/graphics/trials_fast_path.h`); non-zero counters print in the
   `log_guest_fps` line.
2. **One switch:** everything sits behind cvar `vulkan_trials_fast_path` (default true),
   plus per-feature cvars where useful, so A/B tests are a command-line flag.
3. The Trials workload contract (formats, MSAA modes, primitives, resolves, shader
   counts) is `docs/webgpu-renderer/README.md` §5. Record cases outside it.
4. Don't delete generic code; fast paths are additive.
5. Check the image (below). Shadows (signed-int `k_16_16` resolves), bloom, fog (float24
   depth resolve), auto exposure (small luminance readback) and GPU particles (memexport)
   are the usual casualties.

Done so far (see the patch header for one line each): hoisted shared-memory uploads (G1),
dropped no-op render-pass re-opens (G2), resolves straight into textures (G3, G12), async
replay/submit thread (C1, `vulkan_async_submit`), batched constants (C2), on-demand guest
vblank (C18), no wait for the submit thread at swap (C19), page-state retention (G11 →
`clear_memory_page_state=false` default in `OnPostSetup`), timer and fault-handler fixes.

## Measuring (headless and muted; never open the game on the user's desktop)

```sh
tools/bench_vulkan.sh out/bench/1440 2560 1440 --draw_resolution_scale_x=2 --draw_resolution_scale_y=2
tools/bench_vulkan.sh out/bench/720  1280 720  --draw_resolution_scale_x=1 --draw_resolution_scale_y=1
python3 tests/check_fps_log.py out/bench/1440/trials_hd.log   # presented >= 95% of guest FPS
```

- A private weston headless compositor + virtual pad ride on *Middle Name: Danger*
  (`tests/headless/bench_fps.txt`), ~80 s, prints the median guest FPS. Run 3× and take the
  median of medians; < ~2% is noise. Check it measured the ride (> 1,000 draws/frame),
  not the menu.
- Env: `EXE=`, `PERF=1|stat|threads` (CPU profiles; `threads` works in containers),
  `CAPTURE=1` (start/ride frames), `RIDE_SCRIPT=`.
- One benchmark at a time across all worktrees: it holds `flock` on
  `$(git rev-parse --git-common-dir)/vk-bench.lock`; wrap heavy builds in the same lock.
- GPU time by pass/dispatch: cvar `vulkan_gpu_profile` (hot-reload), or the richer
  temporary `tools/profile/vk_gpu_prof.patch` (apply to the SDK, `TRIALS_GPU_PROF=1`, then
  reverse it; never export it into the patch).
- Thread CPU and stacks: `tools/profile/` (`thread_sample.sh`, `stack_top.py`,
  `sample.sh` + `gdb_sampler.py` + `summarize.py`, `thread_cpu.py`).
- **Image check:** capture the same frame before and after (virtual pad `capture`, or
  `CAPTURE=1`) and compare with `tools/compare_captures.py` (PSNR/SSIM). Rides aren't
  frame-deterministic: compare the menu's 3D preview or a paused ride.
- Never edit a shell script in place while an agent may be running it (bash reads by
  offset): write a new file and `mv` it over.

**Done means:** ctest passes; the game boots, rides and renders correctly; before/after
benchmark (and GPU profile for GPU work) recorded in the claim and the notes log.

## Working on a task

The task board is `docs/vulkan-perf/PLAN.md` §5 (IDs F*, G*, C*), follow-ups in §6, the
notes log in §7, and leads not yet tasked in `docs/vulkan-perf/FUTURE_OPTIMIZATIONS.md`.
Claims are atomic directories in the git common dir, shared across worktrees:

```sh
tools/vk_claim.sh list
tools/vk_claim.sh claim G4 <agent-name>       # fails if taken
tools/vk_claim.sh status G4 "note"
tools/vk_claim.sh done G4 "1440p 310 -> 330 FPS"
tools/vk_claim.sh release G4
```

One task per agent; check **Depends on** / **Conflicts with**; use a worktree per agent
(`git worktree add ../th-<task> -b vk/<task>`). After changing the plugin, rebuild with
`--target trials_hd_stage_libs`.

Useful Vulkan cvars: `vulkan_device=N`, `vulkan_headless_surface=true`,
`vulkan_trials_fast_path`, `vulkan_async_submit`, `vulkan_gpu_profile`,
`vulkan_submit_on_primary_buffer_end` (false by default here), `log_guest_fps=true`.
