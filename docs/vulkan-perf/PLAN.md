# Vulkan renderer: Trials HD specialization and performance plan

**Goal (user, 2026-10-17 — the only goal now):** **500 FPS at 2560x1440 (2x resolution
scale) on the RX 7800 XT** (RADV, Ryzen 7 5700X; the user's main PC, where the user runs
the benchmarks). That is a **2.0 ms frame budget** for the GPU and for every CPU thread on
the render path; 720p is no longer a target. Development measurements in this session
come from an RTX 2070 Super (section 1.7, about 3x slower GPU at 1440p), used as a proxy
for 1440p GPU time.

*Earlier goal (2026-10-16, superseded):* additionally 1000 FPS at 720p.

**Approach:** turn the Vulkan backend (`librexgpu-xenos`, the Xenia-derived code in
`thirdparty/rexglue-sdk/src/graphics/vulkan/`) from an "every Xbox 360 game" emulator into a
**Trials HD renderer**, the way the WebGPU backend (`src/gpu_webgpu/`) was built for this one
game. Every generic mechanism that Trials HD does not need, or needs only in a narrow form,
gets a fast path that assumes Trials HD's real workload. Section 5 of
[`docs/webgpu-renderer/README.md`](../webgpu-renderer/README.md) is the measured contract
for that workload (formats, MSAA modes, primitive types, resolves, shaders): read it before
any specialization task.

This file is the task board. Tasks are small enough for one agent each, have a claim
system (section 3), and are marked for small or large models.

---

## 1. Where the time goes today (measured 2026-10-16)

Harness: `tools/bench_vulkan.sh` (headless weston, muted, virtual pad riding *Middle Name:
Danger*), relwithdebinfo build at commit `c3a6791`. GPU numbers come from the timestamp
profiler in `tools/profile/vk_gpu_prof.patch` (section 4.2).

### 1.1 Frame rate

| Config | FPS | GPU busy | Bottleneck |
| --- | ---: | ---: | --- |
| 2560x1440, 2x scale | **168** (5.95 ms) | 100% | GPU |
| 1280x720, 1x scale | **~215** (4.65 ms) | ~77% | CPU: command-processor (CP) thread |
| WebGPU, 2560x1440, 2x (reference) | 214 | ~100% | GPU (4.6 ms) |

### 1.2 GPU time per frame by kind of work (profiler, riding)

| Category | 1440p (2x) | 720p (1x) | Count / frame | What it is |
| --- | ---: | ---: | ---: | --- |
| Render passes with ≥ 8 draws | **0.64 ms** | 0.50 ms | ~38 | the actual scene: G-buffer, shadows, lighting |
| Render passes with 1–7 draws | **2.53 ms** | 1.49 ms | ~238 | mostly the scene passes **chopped into pieces** (below) |
| Pipeline barriers | **1.15 ms** | 1.00 ms | **754** (767 buffer + 97 image barriers) | sync for uploads, resolves, texture loads |
| Compute dispatches | **1.44 ms** | 0.25 ms | 65 | resolve copies + texture loads (untiling), scale with resolution |
| Copies | 0.37 ms | 0.15 ms | 428 buffer + 16 buffer→image | shared-memory uploads (3.2 MB/frame) + texture loads |
| **Total** | **6.1 ms** | **3.4 ms** | | |

**The key fact:** there are **~275 render passes per frame, and ~256 of them re-open the
same render target as the pass before.** 214 of those splits happen because a
shared-memory upload (`VulkanSharedMemory::UploadRanges`: `SubmitBarriers(true)` → copy →
barrier back) lands between two draws; 30 more re-open the pass with nothing at all in
between. Real drawing is ~0.6 ms; the rest is overhead that a Trials-only renderer does
not need.

### 1.3 CPU (720p, perf, 10 s of riding)

| Thread | Busy | ≈ ms / frame | Top items |
| --- | ---: | ---: | --- |
| `GPU Commands` (CP: PM4 decode + Vulkan backend + replay) | ~85% | ~4.3 | Vulkan backend 58% of the thread (IssueDraw, `SharedMemory::RequestRanges`, `UpdateBindings`, RT cache `Update`, `UpdateSystemConstantValues`); RADV driver 22% (replay of the deferred command buffer, `vkUpdateDescriptorSets`); memcpy 6% |
| Guest render thread (`XThread92FFE6C0`) | ~70% | ~3.5 | game code: `sub_823485F0` 2.5%, `sub_8234FFC8`, `sub_825730C0`, `DrawInterpolated` (fps_unlock), page-fault handling |
| `rex::thread::TimerQueue` | ~50% of a core | — | busy-wait spin (`disruptorplus` spin_wait), steals SMT siblings |

Page faults (write watches): ~445/s (~2.6 per frame) — each one parses `/proc/self/maps`
with `sscanf` under the global lock (`mmio_handler.cpp:411` → `memory_posix.cpp`
`QueryProtect`).

### 1.4 Cvar experiments (1440p, 2x)

| Change | FPS | Conclusion |
| --- | ---: | --- |
| baseline | 168 | |
| `anisotropic_override=0` | 169 | texture filtering is not a cost |
| `render_target_path_vulkan=fsi` | 83 | host render targets are right; don't pursue FSI |
| `elide_rt_transfers=false` | 161.5 | transfer elision is worth ~0.25 ms; remaining transfers are small |
| `readback_resolve=none` | 166 | CPU readback of the luminance chain is not a cost |
| `direct_host_resolve=false` | 167.6 | the direct resolve path barely triggers; no gain today |

### 1.5 What the targets mean

| Target | Frame budget | Needs |
| --- | --- | --- |
| 500 FPS at 1440p | **2.0 ms** | GPU ≤ 2.0 ms **and** every CPU thread on the render path ≤ 2.0 ms (CPU work per frame does not shrink with resolution) |
| 1000 FPS at 720p | **1.0 ms** | GPU ≤ 1.0 ms and CP thread ≤ 1.0 ms and **guest render thread ≤ 1.0 ms** |

Honest projection: the GPU tasks below (G1–G6) plausibly take 1440p from 6.1 ms to
about 2–2.5 ms. The CPU tasks (C*) plausibly take the CP thread from ~4.3 to ~1.5–2 ms.
**The guest render thread (~3.5 ms of game code per frame) is the hard floor**: without
G-tier GPU work, 1440p stays near 170; with it but without the CPU work, both
resolutions stop near ~230 FPS. 500 at 1440p needs most of this plan. 1000 at 720p is
probably only reachable with one of the moonshots (X1/X2) that stop running the game's
render code or PM4 decoding for every frame. Measure after each task and update section 1.

---

### 1.6 Status after the first wave (2026-10-17)

| | Baseline (2026-10-16) | Now (main, relwithdebinfo) |
| --- | ---: | ---: |
| 1440p (2x) | 168 FPS, GPU-bound (6.1 ms GPU) | **~310 FPS**, CPU-bound (3.1 ms GPU) |
| 720p (1x) | ~215 FPS, CPU-bound | **~340 FPS**, CPU-bound (relwithdebinfo; C13's PGO build is faster still) |

Done: F0, F1, G1, G3, G8, C1, C5, C6, C8, C9, C10, C11 (no code), C13. **The guest render
thread (the game's own render code, ~3.5 ms per frame) is now the bottleneck at both
resolutions**: `eu-stack` samples show it running ~90% of the time, while the CP thread
waits for it in `WAIT_REG_MEM` ~20% of the time. More GPU or CP-thread work will not show
up as FPS until the guest thread gets faster: C12, C7 (write-watch faults on the guest
thread), `rex::perf` counters (see future leads) and `DrawInterpolated` in
`src/fps_unlock.cpp`. (X1/X2 were dropped by the user: enhancements only, no rewrites.)

### 1.7 Second test machine: RTX 2070 Super on PCIe x4, Ryzen 7 3700X (2026-10-17)

A headless Debian container (no `perf`: use `PERF=threads` in `tools/bench_vulkan.sh`,
which samples `/proc` thread times and `eu-stack`). The GPU sits in a chipset slot
(PCIe gen 2 x4), so CPU→GPU uploads are ~20x slower than on the RX 7800 XT.

| | Start of session | Now |
| --- | ---: | ---: |
| 720p (1x) | 154 FPS (GPU 6.0 ms, 2.4 ms of it uploads) | **~245 FPS** |
| 1440p (2x) | 75 FPS (GPU-bound) | **~95 FPS** (GPU 10.0 ms) |

1440p GPU breakdown now (ms/frame): scene passes 4.0, dispatches 2.5 (resolve dumps
1.06, resolves into textures 0.97), **ownership transfers 1.67** (26 passes), small
passes 1.25, copies 0.31, uploads 0.27, barriers 0.13. At 720p this machine is held back
by NVIDIA's Wayland WSI: `vkQueuePresentKHR` waits for the frame on the CPU (no explicit
sync under headless weston) while holding the graphics queue lock, so the GPU Submit
thread, then the CP thread at the swap and finally the game's fence wait all stall
behind it. That is specific to this NVIDIA/weston setup (RADV doesn't block there); a
separate present queue was tried and didn't help (see notes).

---

## 2. Rules for the Trials-only specialization

1. **Fast path + fallback, never silent breakage.** Each specialization checks its
   assumption (format, MSAA mode, primitive type, resolve shape…) and falls back to the
   existing generic Xenia code when it does not hold. Count the fallbacks (F1's
   `TRIALS_EXPECT` counters) and print them in the `log_guest_fps` line, so a regression
   shows up as a non-zero count instead of a broken frame.
2. **One switch:** all fast paths sit behind the cvar `vulkan_trials_fast_path` (default
   `true`, created by F1) plus, where useful, a per-feature cvar, so A/B benchmarking and
   bisecting is a command-line flag.
3. **Trials HD contract:** `docs/webgpu-renderer/README.md` section 5 (and the SDK patch
   notes there: signed-int `k_16_16` shadow resolves, exp bias from fetch word 3, small
   resolve readback). If you find a case outside it, record it in the notes log (section 7).
4. **Don't delete the generic code** (the D3D12 backend and other SDK users share it, and
   it is the fallback). Fast paths are additive.
5. **Correctness:** compare screenshots before and after (section 4.3). Trials' look
   depends on the shadow `k_16_16` signed-int resolves, the luminance readback (auto
   exposure), float24 depth resolve (fog/soft particles) and the GPU particle memexport.

---

## 3. How agents work on this

### 3.1 Claiming

Claims are an atomic `mkdir` in the git common dir, shared by all worktrees:

```sh
tools/vk_claim.sh list                       # who has what
tools/vk_claim.sh claim G1 opus-agent-1      # fails if taken; pick another task
tools/vk_claim.sh status G1 "uploads hoisted, measuring"
tools/vk_claim.sh done G1 "1440p 168 -> 231 FPS, GPU 6.1 -> 4.3 ms"
tools/vk_claim.sh release G1                 # abandoning: give it back
```

- Claim **before** reading code in depth. Check **Depends on** and **Conflicts with**
  first: don't start a task whose dependencies are not `done`, and avoid running two
  tasks with overlapping conflict zones at once unless they are on separate branches you
  will rebase.
- One task per agent at a time. If a task turns out bigger than described, finish a
  useful slice, mark it done with what is left, and add a follow-up task to section 6.
- When a task's branch is merged, the merger updates the **Status** column in section 5.

### 3.2 Model tiers

- **S** — small model OK (Haiku/Sonnet class): mechanical, local, with exact steps.
- **M** — medium: small model can do it **after** reading the steps carefully; a large
  model is safer if it touches synchronization.
- **L** — large model (Opus class): synchronization, resource lifetime, cross-subsystem
  design, or anything where a wrong assumption corrupts frames.

### 3.3 Working on the SDK

- Work in a git worktree per agent (`git worktree add ../th-<task> -b vk/<task>`) and
  build there (`./build.sh` or `cmake --preset linux-amd64-relwithdebinfo` + build twice
  on a fresh tree). The SDK is the submodule `thirdparty/rexglue-sdk`; its working tree
  carries the local changes from `patches/rexglue-sdk/trials-hd.patch`.
- **SDK patch export pitfall:** the submodule working tree is *not* identical to the
  committed patch (it lacks the web-port changes). Running `tools/export_sdk_patch.sh`
  directly drops ~3600 lines. Apply the committed patch to a `git worktree` of the pinned
  SDK commit, apply only your edits there, and export from that worktree. Diff the new
  patch against the old one before committing.
- Rebuilding only the plugin: `cmake --build <build> --target trials_hd_stage_libs`
  builds `rexgpu-xenos` and copies it next to `trials_hd` (the binary's RPATH is
  `$ORIGIN`). Building just `--target rexgpu-xenos` leaves the old copy in place.

---

## 4. Measuring (every task)

### 4.1 Benchmark (headless and muted — never open the game window or play audio on the
user's desktop)

```sh
tools/bench_vulkan.sh out/bench/1440 2560 1440 --draw_resolution_scale_x=2 --draw_resolution_scale_y=2
tools/bench_vulkan.sh out/bench/720  1280 720  --draw_resolution_scale_x=1 --draw_resolution_scale_y=1
python3 tests/check_fps_log.py out/bench/1440/trials_hd.log     # presented >= 95% of guest fps
```

Each run takes ~80 s and prints the median guest FPS of the ride. Run each config **3
times** and report the median of medians; differences under ~2% are noise. `PERF=1`
records a CPU profile (`perf report -i out/bench/720/perf.data --no-children --comm
"GPU Commands (F" --sort sym -g none`). Do not run two benchmarks at once.

**Builds skew benchmarks too.** `tools/bench_vulkan.sh` holds `flock` on
`$(git rev-parse --path-format=absolute --git-common-dir)/vk-bench.lock` (shared by all
worktrees) for the whole run. Wrap every heavy build in the same lock so it waits for
running benchmarks and benchmarks wait for it:

```sh
flock "$(git rev-parse --path-format=absolute --git-common-dir)/vk-bench.lock" \
  cmake --build out/build/linux-amd64-relwithdebinfo -j12
```

Check that the ride was actually measured: `bench_summary.py --threshold 61` can pick
menu samples (72 draws/frame) if the ride never started — the ride has >1,000
draws/frame and ~19 resolves/frame, and `tests/check_fps_log.py` must pass.

**Never edit a shell script in place while any agent may be running it** (bash reads
scripts by byte offset as it goes, so a running benchmark can execute the wrong lines).
Write the new version to a temporary file and `mv` it over the old one; running shells
keep the old file.

### 4.2 GPU profile

`tools/profile/vk_gpu_prof.patch` is a temporary timestamp profiler in
`DeferredCommandBuffer::Execute`: it attributes GPU time to render passes (by draw
count), dispatches (per compute pipeline), barriers and copies, and counts pass splits
and their causes. To use it:

```sh
git -C thirdparty/rexglue-sdk apply ../../tools/profile/vk_gpu_prof.patch
cmake --build out/build/linux-amd64-relwithdebinfo --target trials_hd_stage_libs
TRIALS_GPU_PROF=1 tools/bench_vulkan.sh out/bench/prof 2560 1440 --draw_resolution_scale_x=2 --draw_resolution_scale_y=2
grep GPU_PROF out/bench/prof/trials_hd.log | tail -2
git -C thirdparty/rexglue-sdk apply -R ../../tools/profile/vk_gpu_prof.patch   # never export it into trials-hd.patch
```

Line format: `rp_big=<ms>/<count>` etc. per frame; `split_same_rt=` is the number of
passes re-opening the previous pass's attachments, `reasons r<mask>` says what came in
between (1 = buffer copy, 2 = barrier, 4 = dispatch, 8 = buffer→image copy; r3 = upload).
Task F0 turns this into a permanent cvar. An older, richer version (category labels per
subsystem) existed as the `gpu_profile` cvar in commit `9f5bc21` and was stripped in
`85afc63`; it is a good starting point.

### 4.3 Image check

Capture frames before and after your change with the virtual pad's `capture <file.ppm>`
command (`TRIALS_HD_VIRTUAL_PAD` FIFO, see `src/virtual_pad.cpp`) at the same ride time
and compare with `tools/compare_captures.py` (PSNR/SSIM). Riding is not deterministic
frame-for-frame, so compare the menu's 3D preview or a paused ride, and look at the diff
image yourself. Shadows, bloom, fog and auto exposure are the usual casualties.

### 4.4 Done means

`ctest --test-dir <build>` passes; the game boots, reaches the ride and renders correctly
(4.3); the benchmark (4.1) and, for GPU tasks, the GPU profile (4.2) are recorded in the
claim note and in the notes log (section 7) with before/after numbers.

---

## 5. Task board

Estimated gains are per frame and are estimates from section 1; real numbers go in the
notes log.

### 5.0 Foundation (do first; F0 and F1 unblock most others)

| ID | Task | Tier | Status |
| --- | --- | :-: | --- |
| F0 | Permanent GPU profiler cvar | S | done; cvar vulkan_gpu_profile hot-reload, zero-cost off (720p 243.1, 1440p 167.6), image PSNR 55.20 dB / SSIM 0.9998, 6/6 ctests pass, pinned check pass (antigravity-2) |
| F1 | Trials fast-path switch and fallback counters | S | done; switch/counters and game gates pass (pi dependency takeover) |
| F2 | Re-verify the Trials workload contract on the Vulkan path | S | partly: render pass re-opens and ownership transfers listed per frame (notes 2026-10-17); formats/resolves still to recheck |

**F0 — Permanent GPU profiler cvar.** Port `tools/profile/vk_gpu_prof.patch` into the
SDK as cvar `vulkan_gpu_profile` (hot-reload; off by default; zero cost when off: one
branch per command). Use the device function table instead of `dlsym` (add
`vkCmdWriteTimestamp` and `vkGetQueryPoolResults` to
`include/rex/ui/vulkan/functions/device_1_0.inc`). Label dispatches by role, not pipeline
index: let `BindExternalComputePipeline` callers pass a tag (resolve copy, texture load,
memexport, swap). Report every 2 s. *Files:* `deferred_command_buffer.{h,cpp}`,
`command_processor.cpp`, `device_1_0.inc`. *Conflicts:* G1, C1 (same files — small
edits, rebase).

**F1 — Fast-path switch and fallback counters.** Add cvar `vulkan_trials_fast_path`
(bool, default true, restart-required is fine) and a tiny header
`include/rex/graphics/trials_fast_path.h` with `TRIALS_EXPECT(cond, counter_name)`
that increments a named atomic counter when `cond` is false. Print non-zero counters
in the `log_guest_fps` line (`fallbacks: g3_resolve=12 …`). Every specialization task
uses it. *Files:* new header, `command_processor.cpp` (log line), `flags.cpp`.

**F2 — Re-verify the contract.** Section 5 of the WebGPU doc was measured on
2026-10-13 at 1x on an RX 6600. Re-run its probe
(`docs/webgpu-renderer/probe/run_draw_probe.sh`) or add temporary counters at 2x on the
Vulkan path: RT formats/MSAA per pass, resolve source→dest formats and sizes, primitive
types, index formats, texture formats/dimensions, max textures per shader, memexport
draws. Write differences into section 7. Pure research, no committed code.

### 5.1 GPU (1440p target)

| ID | Task | Tier | Est. gain (1440p) | Depends | Status |
| --- | --- | :-: | --- | --- | --- |
| G1 | Upload shared memory outside render passes | L | **1.5–2.0 ms** | F0 helpful | **done** (1440p +43%) |
| G2 | Remove no-op render pass re-opens | M | 0.1–0.3 ms | — | **done** (merged at replay, 31/frame; passes 70 → 39) |
| G3 | Resolve straight into textures (resolve aliases) | L | **1.0–1.4 ms** | F1 | **done** (GPU −1.75 ms; now CPU-bound) |
| G4 | Don't reload the same resolved texture twice per frame | M | 0.2–0.5 ms | — | premise mostly gone after G3: texture loads 0.09 ms / 4 per frame at 1440p |
| G5 | Barrier diet | M | 0.3–0.6 ms (after G1) | G1 | low value now: barriers 0.13 ms / 91 per frame at 1440p (2070S) |
| G6 | Shadow maps at 1x while the scene is scaled | L | 0.2–0.5 ms | G3 | open |
| G7 | Render area and RT image sizes from Trials' real extents | M | 0–0.3 ms (measure) | F0 | open |
| G8 | Keep DCC on color render targets | S | 0–0.3 ms (measure) | — | done; 1440p 165.1 -> 167.4 (+1.4%), 720p 208.6, nodcc baseline 146.3 (-12.6%), image 53.25 dB / .9990, 0 fallbacks (antigravity) |
| G9 | One pass from frontbuffer to swapchain | M | 0.1–0.2 ms | G3 | open |
| G10 | Trials-specific ownership-transfer rules | M | 0.1–0.3 ms | F0 | measured: 1.67 ms / 26 passes at 1440p (2070S), ~38 transfers/frame, all needed (WebGPU doc 8.7); open: cheaper transfers, not fewer |

**G1 — Upload shared memory outside render passes.** *Evidence:* 214 of ~275 passes per
frame are split by uploads; 428 copies (3.2 MB) and ~430 of the 754 barriers per frame
come with them. *Today:* `IssueDraw` → `SharedMemory::RequestRange(s)` →
`VulkanSharedMemory::UploadRanges` (`src/graphics/vulkan/shared_memory.cpp` ~270–330)
does `Use(kTransferDestination)` + `SubmitBarriers(true)` (ends the pass), copies from the
upload pool, and the next draw's `Use(kGuestDrawReadOnly)` adds the barrier back and
re-opens the pass. *Do:* record uploads into a separate **pre-pass command stream**
(a second `DeferredCommandBuffer`, or a separate `VkCommandBuffer` executed first in the
same submission) with all copies merged into one `vkCmdCopyBuffer` (many regions) and
**one** barrier at its end; the draw path only marks ranges valid. The whole frame is
recorded before submission (`EndSubmission` replays it), so hoisting is possible.
*Hazards (why this is L):* (a) a range the GPU writes earlier in the same submission
(resolve to shared memory, memexport) and the CPU then rewrites must keep its order —
keep a per-submission "GPU-written pages" bitset and fall back to today's in-order
upload for those ranges; (b) a range uploaded twice in one submission with different
contents (an earlier draw must see the old bytes) — detect via the bitset of pages
already uploaded this submission and fall back; (c) mid-frame submissions
(`vulkan_submit_on_primary_buffer_end`, readbacks) start a new pre-pass stream. Count
fallbacks with F1. Expect most of the 214 splits and ~400 barriers to disappear.
*Trials angle:* the dynamic data is the D3D9 runtime's append-only vertex ring and
constant data (NOOVERWRITE/DISCARD semantics), so (b) should be rare.
*Files:* `vulkan/shared_memory.{h,cpp}`, `vulkan/command_processor.{h,cpp}`
(`BeginSubmission`, `EndSubmission`, `IssueDraw`), `deferred_command_buffer`.
*Conflicts:* G2, G5, C1, F0. *Verify:* `split_same_rt` drops from ~256 to < 60;
rp_small + barrier time falls by ≥ 1.5 ms.

**G2 — Remove no-op render pass re-opens.** *Evidence:* ~30 passes per frame end and
re-begin with identical attachments and **no** command in between (`r0` in the
profile). Candidates: `SubmitBarriers(true)` with nothing pending (calls `EndRenderPass`
anyway), callers of `EndRenderPass()` that then emit nothing outside a pass,
`SubmitBarriersAndEnterRenderTargetCacheRenderPass` comparing the `Framebuffer*` while
the views are the same (dynamic rendering only needs the views). *Do:* find which
(temporary log of the caller when a pass is re-opened with the same views), make the
end lazy: mark "pass should end" and only emit `EndRendering` when a command that needs
to be outside a pass actually arrives. *Files:* `vulkan/command_processor.cpp`
(`SubmitBarriers`, `EndRenderPass`, `SubmitBarriersAndEnterRenderTargetCacheRenderPass`).
*Conflicts:* G1, G5.

**G3 — Resolve straight into textures.** *Evidence:* dispatches cost 1.44 ms at 1440p
(0.25 at 720p): every resolve is a compute copy from the render target (or its EDRAM
dump) into the tiled guest layout in the scaled resolve buffer, and every texture that
samples it is then **re-untiled by another compute dispatch** into a scratch buffer and
copied into the image (`VulkanTextureCache::LoadTextureDataFromResidentMemoryImpl`,
~1216–1730). Trials does ~20 resolves per frame (table 5.7 of the WebGPU doc). *Do:*
port the WebGPU design (`src/gpu_webgpu/resolve_engine.cpp`,
`TextureCache::GetOrCreateResolveAlias`): for resolves whose destination is later read
as a texture of a known format/size, copy (or convert with a small fragment/compute
pass) from the host render target **directly into a texture image** registered as an
alias for the destination address; the texture cache returns the alias for fetch
constants at that address instead of loading from memory. Keep writing guest memory
only for the small luminance resolves the CPU reads (`readback_resolve_max_bytes`).
Required conversions for Trials: `k_16_16` 4x MSAA → signed-int `k_16_16` with exp bias
(shadows; also sampled from the particle **vertex** shader), D24FS8 → `k_24_8_FLOAT`,
`k_2_10_10_10_FLOAT` → `k_2_10_10_10` / `k_8_8_8_8`, `8_8_8_8` copies, frontbuffer.
Anything else → fallback (F1). Invalidate the alias when the CPU writes the range or a
different-format resolve lands there. *Files:* `vulkan/render_target_cache.cpp`
(`Resolve`), `vulkan/texture_cache.{h,cpp}`, `pipeline/texture/cache.cpp`
(`MarkRangeAsResolved`, lookup). *Conflicts:* G4, G6, G9, G10. Split it if needed:
G3a frontbuffer + 8888 copies, G3b shadow `k_16_16`, G3c depth and 7e3 formats.

**G4 — Don't reload the same resolved texture twice per frame.** *Evidence:* the WebGPU
doc measured the shadow atlas (`k_16_16`, 512x1536) reloaded 3x per frame; the profile
shows 16 buffer→image loads per frame. When several fetch constants (different
sub-rectangles / mip settings) cover the same resolved memory, each becomes its own
texture key and its own load. *Do:* log loads per frame (key, size) for a few frames;
for keys that alias the same base address and format, load once and create views, or
keep a per-frame "already loaded at generation N" stamp. Cheap stopgap before G3; still
useful for anything G3 falls back on. *Files:* `vulkan/texture_cache.cpp`,
`pipeline/texture/cache.cpp`. *Conflicts:* G3.

**G5 — Barrier diet.** After G1, re-profile. Then: merge buffer barriers on the same
buffer into one range barrier per `SubmitBarriers`; use exact stage masks instead of
`ALL_*`/`BOTTOM_OF_PIPE` defaults; skip `kGuestDrawReadOnly → kGuestDrawReadOnly` style
no-op transitions in `VulkanSharedMemory::Use` and the texture cache; consider
`VK_KHR_synchronization2` (`vkCmdPipelineBarrier2`) for per-barrier stage masks so one
call can carry unrelated barriers without widening stages. *Verify:* barrier count and
barrier ms. *Files:* `vulkan/command_processor.cpp` (`PushBufferMemoryBarrier`,
`SubmitBarriers`), `vulkan/shared_memory.cpp`, `vulkan/texture_cache.cpp`.

**G6 — Shadow maps at 1x while the scene is scaled.** Trials' shadow/light maps are
`k_16_16` 4x MSAA targets (pitch 520/280/160) drawn ~700 times per frame; at 2x scale
they are 4x the pixels for little visible gain. Add cvar `shadow_resolution_scale`
(default 1) so render targets in Trials' shadow configuration (`k_16_16` color + D24FS8,
4x MSAA, those pitches) and their resolves stay unscaled while everything else is
scaled. Needs per-render-target scale in the RT cache, viewport/scissor math per draw,
and resolve/texture paths that know the source scale — L. *Depends:* G3 (simpler once
resolves go straight to textures). Quality trade-off: keep the cvar so the user can turn
it off.

**G7 — Render area and RT image sizes.** Every pass begins with
`renderArea = framebuffer->host_extent`, the full EDRAM-height image (e.g. 2560x4096 at
2x for a 1280-wide pitch), and RT images are allocated that tall
(`GetRenderTargetHeight`). Trials' main frame is 1280x672. *Do:* first measure (F0) with
the render area clamped to the scissor/viewport union of the pass's draws (the deferred
command buffer can patch the `BeginRendering` area at `EndRendering`, since it is
replayed later); then, behind F1, size Trials' known render targets to their real height
(fallback to EDRAM height when a draw exceeds it). Gains are uncertain on RADV with
`LOAD`/`STORE` ops — skip if F0 shows nothing.

**G8 — Keep DCC on color render targets.** Color RTs are created with
`VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT` (for the ownership-transfer view format) without a
`VkImageFormatListCreateInfo`, which makes RADV disable DCC compression for them
(`render_target_cache.cpp` `CreateRenderTarget`, ~1860–1905). *Do:* chain a format list
with exactly the formats used (render format + transfer format); benchmark. Also try
`RADV_DEBUG=nodcc` on the baseline to see how much DCC matters at all. Small, local,
measurable.

**G9 — One pass from frontbuffer to swapchain.** Today the frontbuffer is resolved,
re-loaded as a texture, copied/gamma-converted into the guest output image
(`IssueSwap` refresher, compute or graphics), then painted to the swapchain by the
presenter. With G3 the frontbuffer is an alias texture; let the presenter's paint pass
sample it and apply the gamma ramp directly (Trials' ramp is fixed — confirm and, if it
is identity, skip it behind F1). *Files:* `vulkan/command_processor.cpp` (`IssueSwap`),
`src/ui/vulkan/vulkan_presenter.cpp`. *Depends:* G3a.

**G10 — Trials-specific ownership-transfer rules.** Transfers cost ~0.25 ms more without
`elide_rt_transfers`; what remains is unmeasured. With F0 tagging transfer passes, list
the remaining transfers per frame (source/dest RT keys). Where Trials only reinterprets
EDRAM between two of its known configurations and the destination is fully overwritten
or cleared next (e.g. the `k_16_16` shadow passes reusing EDRAM after the scene), drop or
replace them with clears behind F1. *Files:* `pipeline/render_target/cache.cpp`,
`vulkan/render_target_cache.cpp`.

### 5.2 CPU — command-processor thread (both targets)

| ID | Task | Tier | Est. gain (CP thread) | Depends | Status |
| --- | --- | :-: | --- | --- | --- |
| C1 | Replay and submit on a separate thread | L | **0.8–1.2 ms** | — | **done** (+3.8% at 720p; guest thread now limits) |
| C2 | Descriptor model without per-draw allocate/update | L | 0.3–0.5 ms | F1 | **done** for constants (dynamic uniform buffers; 720p +2.2% on the 3700X); texture sets: reuse tried and reverted (notes) |
| C3 | Skip texture/sampler binding work when fetch constants didn't change | M | 0.2–0.4 ms | — | open |
| C4 | Trials-only system constant update | M | 0.1–0.3 ms | F1 | open |
| C5 | MRU fast path for pipeline and shader-modification lookup | S | 0.1–0.2 ms | — | done; 720p 201.3 -> 237.1 FPS (+17.8%), 1440p 164.7 -> 166.6 (+1.2%), image PSNR 53.34 dB / .9989, 5/5 ctest pass (antigravity-2) |
| C6 | Drop the dummy point draws before any backend work | S | 0.1–0.2 ms | — | done; 720p 201.3 -> 211.4 FPS (+5.0%), 1440p 165.1 FPS, image 51.71 dB / .9988, 169.0 skipped draws verified (antigravity) |
| C7 | Stop write-watching Trials' per-frame dynamic buffers | L | 0.2–0.4 ms CP + guest faults | G1 | open; faults are ~2.3% of the guest thread at 720p after G11 |
| C8 | Build without Tracy in release | S | ~0.05 ms | — | done; 720p 236.3 -> 244.6 FPS (+3.5%), 1440p 166.4 (GPU-bound); ldd verified 0 Tracy links; ctest 5/5 (antigravity) |

**C1 — Replay and submit on a separate thread.** `EndSubmission` replays the whole
frame from `DeferredCommandBuffer` into a `VkCommandBuffer` and submits, on the CP
thread — ~20% of its time plus most of the RADV share. Move `Execute` +
`vkEndCommandBuffer` + `vkQueueSubmit` to a dedicated submit thread: double-buffer the
deferred streams (the CP thread starts recording frame N+1 while N replays); everything
the stream references (descriptor sets, transient pools, upload buffers, scratch
buffers) must stay alive until the submit thread has *submitted* it, not just until the
CP thread closed it — today lifetimes are tied to `submission_current_` / fence
completion, so add a "replayed" index or have the submit thread own the fence signal.
Sparse binds and readback waits (`IssueCopy_ReadbackResolvePath` awaiting a fence) must
wait for the submit thread first. *Files:* `vulkan/command_processor.{h,cpp}`
(`BeginSubmission`, `EndSubmission`, `CheckSubmissionFenceAndDeviceLoss`),
`deferred_command_buffer`. *Conflicts:* G1, F0.

**C2 — Descriptor model.** Every draw that changes constants allocates transient
descriptor sets and calls `vkUpdateDescriptorSets` (`UpdateBindings` ~6453–6840,
`WriteTransientTextureBindings`). Trials uses at most 8 textures per shader and a fixed
constant layout. *Do (pick one, measure):* (a) constant buffers as
`UNIFORM_BUFFER_DYNAMIC` bound once per uniform-pool page with dynamic offsets per draw
(no allocate/update at all for constants); (b) `VK_KHR_push_descriptor` for the texture
sets; (c) a cache of texture descriptor sets keyed by (views, samplers) like the WebGPU
backend's bind-group cache. *Files:* `vulkan/command_processor.cpp`,
`vulkan/pipeline_cache.cpp` (layouts). *Conflicts:* C3, C4.

**C3 — Texture binding dirty tracking.** `TextureCache::RequestTextures`,
`BindingInfoFromFetchConstant`, `GetSubresourcesFromFetchConstant`, the `TextureKey`
hash map lookup and `GetSamplerParameters` run for every used slot of every draw. Keep a
per-slot copy of the 6 fetch-constant dwords and the resolved binding; when the dwords
and the texture's validity generation are unchanged since the last draw, reuse the
binding and skip the lookups. *Files:* `pipeline/texture/cache.cpp`,
`vulkan/texture_cache.cpp`, `vulkan/command_processor.cpp`.

**C4 — Trials-only system constants.** `UpdateSystemConstantValues` (~5953–6450)
recomputes everything per draw (user clip planes, point sprites, tessellation, FSI EDRAM
blend/stencil state, gamma, texture swizzles/signs, alpha test…). Trials uses no user
clip planes, no tessellation, no FSI, 1x/4x MSAA only. Behind F1: skip the branches
that can't apply, and compute groups only when their source registers changed (dirty
bits set in `WriteRegister` for the register ranges each group reads).

**C5 — MRU fast paths.** `VulkanPipelineCache::ConfigurePipeline`,
`GetCurrentVertexShaderModification` / `GetCurrentPixelShaderModification`,
`GetOrCreateTranslation` run per draw. Remember the last inputs → outputs and return early
when equal (the WebGPU backend's pipeline MRU gave 60–80% hits). Purely local. *Files:*
`vulkan/pipeline_cache.cpp`, `vulkan/command_processor.cpp` (IssueDraw).

**C6 — Drop dummy draws earlier.** ~169 draws per frame have no observable effect and
are skipped in `VulkanCommandProcessor::IssueDraw` only after shader lookup and other
setup. Their signature is known (vertex shader `B6C9863F710683EC`, point list, no pixel
shader/depth/stencil/memexport). Move the check to the top of `IssueDraw` (or into
`CommandProcessor::ExecutePacketType3Draw` before index/primitive work), using a
precomputed flag on the shader object. Verify the skipped count stays ~169.

**C7 — Write-watching Trials' dynamic buffers.** Shared memory is kept coherent by
write-protecting guest pages (`EnableAccessCallbacks`, `mprotect`) and faulting on CPU
writes. Pages the game rewrites every frame (vertex ring, particle/HUD buffers) pay a
fault on the guest thread plus re-protection and a re-upload on the CP thread every
frame. *Do:* detect pages invalidated in K consecutive frames and switch them to
"always upload when requested, no watch" (compare against a CPU shadow copy or just
upload), with a decay back to watching. Coordinate with G1 (the upload path).
*Files:* `src/graphics/shared_memory.cpp`, `vulkan/shared_memory.cpp`,
`src/system/xmemory.cpp` (`PhysicalHeap::EnableAccessCallbacks`).

**C8 — No Tracy in release.** `libTracyClientrd.so` is linked and
`tracy::IsProfilerStarted` shows up per draw (~0.4% of samples). Add a CMake option
(default OFF for release/relwithdebinfo) that builds the SDK with Tracy disabled, keeping
it available for profiling builds. *Files:* `CMakeLists.txt`, SDK CMake.

### 5.3 CPU — runtime and guest threads

| ID | Task | Tier | Est. gain | Depends | Status |
| --- | --- | :-: | --- | --- | --- |
| C9 | TimerQueue busy-spin → timed wait | S | frees ~½ core (SMT relief) | — | done; lower timer CPU, no measurable FPS gain (pi) |
| C10 | Fault handler: no `/proc/self/maps` parsing | S | ~0.1–0.25 ms guest thread | — | done; 720p 201.3 -> 236.3 (+17.4%), 1440p 164.7 -> 170.7 (+3.6%); bypass /proc/self/maps (antigravity-2) |
| C11 | Thread placement for the render path | S | 0–5% (measure) | C9 | done; measured 9 interleaved runs (default 219.7, low 224.1, high 217.2 FPS); <3% gain; no code kept per plan (antigravity) |
| C12 | Profile and native-replace hot guest render functions | L | 0.5–1.5 ms guest thread | — | open |
| C13 | PGO/LTO coverage for the SDK libraries | S | 3–10% CPU (measure) | — | done; 720p 244.6 -> 296.1 FPS (+21.1%); ride PGO training (26.9B counts) across trials_hd, xenos, runtime; ThinLTO PGO-use; ctest 6/6 pass (antigravity) |

**C9 — TimerQueue spin.** `src/core/timer_queue.cpp` `TimerThreadMain` waits via
`disruptorplus` `wait_until_published` with a spinning wait strategy (the local patch
touches `thirdparty/disruptorplus/include/disruptorplus/spin_wait.hpp`); perf shows the
thread at ~50% of a core. Replace with a condition-variable / futex wait until the next
due time or a new item (keep the API). Verify timers (vblank, guest timers) still fire
on time: `log_simulation_rate=true` must still show 60 updates/s.

**C10 — Fault handler.** `MMIOHandler` (`src/system/mmio_handler.cpp` ~404–420) calls
`memory::QueryProtect` on every access violation that is not an MMIO range (every
write-watch fault), and on Linux `QueryProtect` opens and `sscanf`-parses
`/proc/self/maps` (`src/core/memory_posix.cpp`). Track the protection of watched pages
in the heap's own page table (the physical heap already knows which pages it protected)
and answer the "already unprotected by another thread?" question from that. Keep the
`/proc` path only as a last resort.

**C11 — Thread placement.** The render path is three busy threads (guest render thread,
CP thread, presenter/submit). On the 8-core 5700X, measure pinning them to different
physical cores (no SMT siblings shared) via `pthread_setaffinity_np`, behind a cvar.
Keep only if ≥ 3% better.

**C12 — Hot guest render functions.** The guest render thread is ~3.5 ms per frame at
720p and is the floor for both targets. Profile it (`PERF=1`, comm `XThread…` of the
render thread; it is recompiled C++). Top: `sub_823485F0` (2.5% of all samples),
`sub_8234FFC8`, `sub_825730C0`, `sub_8255D5F8`, `sub_8256DF28`, `sub_823488C0`,
`DrawInterpolated` (`src/fps_unlock.cpp`). Identify what they are (the D3D runtime lives
around `0x8234xxxx`; see section 6 of the WebGPU doc for the reverse-engineered D3D
runtime) and replace the hot ones with native implementations via `REX_FUNC` overrides
(as `src/crt_hooks.cpp` does), e.g. constant-buffer writes into the command buffer,
matrix math, culling. Also check `DrawInterpolated`'s per-node map lookups. Split into
sub-tasks per function once identified; add them to section 6.

**C13 — PGO/LTO coverage.** The `linux-amd64-pgo-*` presets exist (`tools/pgo_workflow.sh`).
Check whether `librexgpu-xenos` and `librexruntime` are built with the profile (they are
separate shared libraries); if not, extend the workflow to them, train on the ride
benchmark, and measure 720p FPS.

### 5.4 Moonshots — dropped (2026-10-17, user decision: enhance the existing renderer, no rewrites)

| ID | Task | Tier | Status |
| --- | --- | :-: | --- |
| X1 | Direct3D front-end: skip PM4 for Trials | L | **dropped** (user: no rewrites, only renderer enhancements) |
| X2 | Interpolated frames without re-running the game's render code | L | **dropped** (user: no rewrites, only renderer enhancements) |

**X1 — D3D front-end.** WebGPU doc section 7.2: hook Trials' D3D9-style runtime
(section 6 of that doc) so draws reach the backend as structured calls instead of PM4
packets that are built by guest code and then decoded again by the CP thread. Removes
register decoding and much of the per-draw state reconstruction; cuts guest render
thread work that only builds packets. Start with a design note in section 7 and get the
user's go-ahead.

**X2 — Interpolated-frame replay.** With FPS unlock the simulation runs at 60 Hz and the
other frames only move objects and the camera (`DrawInterpolated` rewrites matrices).
Investigate recording one frame's command stream per simulation tick and, for the
in-between frames, replaying it with only the matrix constants patched (constants are
uploaded per draw, so the patch points are known), skipping the guest render code and
PM4 decode entirely. Hard parts: particles (memexport, per-frame time), UI, anything
whose draw *count* depends on the interpolated camera (culling). Research note first.

---

## 6. Follow-up tasks (agents add here)

| ID | Task | Tier | From | Status |
| --- | --- | :-: | --- | --- |
| C14 | Perf counters off in optimized builds (`REXGLUE_ENABLE_PERF_COUNTERS`, like C8 did for Tracy): `rex::perf::IncrementCounter` is ~2.4% of the guest render thread (the new bottleneck); only the SDK debug overlay reads the counters | S | C1 profile (Claude Code) | **done** |
| C15 | `DrawInterpolated` (`src/fps_unlock.cpp`, ~7.5% of the guest render thread): compute the camera pose, view, view×projection and the other camera matrices once per (renderer, camera, frame) instead of for every drawn node; keep the per-node model poses | M | C1 profile (Claude Code) | **done** |
| C17 | 1 µs timer slack for the whole process (`prctl(PR_SET_TIMERSLACK)` in `src/gpu_wait.cpp`): the game and the CP thread hand off through short sleeps many times per frame, and the default 50 µs slack stretched each of them | S | 1440p stack samples (Claude Code) | **done** (1440p +3.7%) |
| C18 | On-demand guest vblank (vsync off): Trials' command stream waits every frame (`WAIT_REG_MEM` on memory) for its vblank interrupt handler to release a semaphore, and the vblank timer ticks only every 1 ms | M | sync diagnostic (Claude Code) | **done** (1440p +16%, 720p +21%) |
| C16 | Index textures by page for G3's `GetTexturesOverlapping` (a full texture-map scan per resolve, ~1.6% of the CP thread) | S | C1 profile (Claude Code) | **done** |
| G12 | Resolve straight from the render target (skip the EDRAM dump) for G3's resolves whose source is owned by one render target with the resolve's own key | M | 1440p GPU profile (Claude Code) | **done** (dumps 19.5 → 8 per frame; 2070S 1440p GPU 10.0 → 9.55 ms, FPS 95 → 100 with G2) |
| G11 | Don't re-upload every CPU-uploaded page each frame: `clear_memory_page_state` (SDK default true) dropped the valid state of all CPU-uploaded pages at every frame end, so every vertex/index buffer a frame used was uploaded again (~600 pages, 2.4 MB per frame); Trials defaults it to false | S | 2070S PCIe x4 profile (Claude Code) | **done** (720p 154 → 228, 1440p 75 → 95 on the 2070S) |
| C19 | Don't wait for the submission thread at the swap (`vulkan_async_swap`, hot reload): the presenter's refresh fence goes through the submission thread and the consumers of the guest output (paint, capture) wait for the swap to be on the queue; the swap only waits until the presenter has taken the previous frame, so every frame is still presented | M | 720p thread profile (Claude Code) | **done**; no change on the 2070S (its present is the ceiling), measure on the 7800 XT |

---

## 7. Notes log (append; newest last)

- 2026-10-16 — Baseline and GPU breakdown in section 1 (Opus 5.5). GPU profiler patch:
  `tools/profile/vk_gpu_prof.patch`; benchmark: `tools/bench_vulkan.sh`.
- 2026-10-16 — C8 (Claude Code + pi): integrated optimized-native Tracy defaults, retained explicit profiling opt-in, updated the link probe and added `tests/tracy_default_test.py`. All 18 configuration cases and the probe ON/OFF self-check pass. Full no-Tracy build and game benchmarks remain pending; **no FPS gain claimed**.
- 2026-10-16 — C9 (pi, isolated `../th-vk-pi`; Claude Code read-only review): switched to the existing blocking strategy, fixed its timed-overload argument ordering, and used an untimed wait for an empty queue. Focused timer tests pass (deadlines, earlier insertion, cancellation, recurring self-disarm, in-flight Disarm, 2,048 multi-producer callbacks and shutdown). A 1 kHz probe used 21.201 ms CPU / 200 ms wall before and 2.557 ms after; this is **not** an FPS result. The first game baseline was rejected (concurrent external builds; riding median 40.3 FPS, FPS check failed). `bench_summary --threshold 61` selected menu samples instead; see [future leads](FUTURE_OPTIMIZATIONS.md). Isolated runtime build, focused queue tests and all four project ctests pass; game/image validation and valid A/B measurements remain pending; C9 is **not done**.
- 2026-10-16 — G8 (Claude Code, `../th-vk-g8`; parent source review): a format-list candidate preserves all RT view formats and existing pNext links, gated by device format-list support and F1's global switch. Syntax checks pass with/without `NDEBUG`; no full link or game benchmark yet. Incremental handoff: `../th-vk-g8/out/g8-delta.patch`. It needs F1 integrated before linking. Source analysis narrows the recorded Trials benefit to mutable 16_16 shadow targets; actual DCC enablement must be measured (see [future leads](FUTURE_OPTIMIZATIONS.md)). No performance gain claimed.
- 2026-10-16 — C9 baseline recovered: all **six** headless runs pass `tests/check_fps_log.py` (fixed simulation ~60 Hz, rendered frames presented). Stable riding window = last 15 samples with >1,000 draws and ≥18 resolves/frame, not an FPS cutoff. 720p medians: **209.2 / 201.3 / 194.0**, median **201.3 FPS**; 1440p: **168.8 / 166.4 / 162.2**, median **166.4 FPS**. Local machine-readable evidence: `out/bench/vk-c9/baseline_summary.json`, logs and 10 s CPU profiles in `baseline-{1,2}x-{1,2,3}/`. One 720p profile attributes 5.41% of process cycle samples to the timer queue. After-runs and stationary paused-ride captures are queued through the shared benchmark lock; no before/after result yet.
- 2026-10-16 — C9 image-harness setup: the Xvfb-based attempt failed before game startup (`Xvfb: command not found`, SDL X11 unavailable). No image or renderer correctness result was accepted from it. Replacement uses the already-working private headless Weston benchmark harness with FIFO `capture` commands, muted audio, separate output directories and the shared lock; no package installation needed.
- 2026-10-16 — C9 image validation now passes: real stationary paused ride before/after, **PSNR 56.12 dB / SSIM 0.9998**, above explicit 30 dB / 0.98 thresholds; parent inspected both images and amplified diff. Within-run pairs: 64.82 and 67.91 dB. Evidence: `out/bench/vk-c9/weston-capture-{baseline,after}/` and `paused-reviewed-diff.png`. Stable 720p after medians: 212.7 / 198.8 / 188.5 → **198.8 FPS**, versus 201.3 before (**−1.2%, no measured FPS gain**). The timer queue `_M_run` body changes from 5.41% to 0.01% of sampled process cycles in the second 720p profile pair; not a direct whole-thread CPU-time measurement. Final 1440p run and integration still pending.
- 2026-10-16 — **C9 accepted and integrated.** Final 1440p after medians: 172.4 / 163.9 / 164.7 → **164.7 FPS**, versus 166.4 before (−1.0%, noise). All 12 rate checks pass; image gate above passes. Main-tree `trials_hd_stage_libs` build/staging passes, project ctest **5/5** and isolated POSIX/timer ctest **2/2** pass. Full SDK patch preserves all 108 original sections plus the two C9 sections and passes `git apply --check` on a clean pinned SDK checkout. Final local evidence: `out/bench/vk-c9/summary.json`. This is a timer-wait CPU reduction, **not an FPS gain**; section 1's timer busy-spin observation is pre-C9.
- 2026-10-16 — F1 dependency takeover (user-authorized because current G8 needs it): integrated the switch/header/counter functions and only the FPS-log suffix from `../th-claude`, leaving G1/G2 and temporary diagnostics untouched. Corrected rare fallback reporting to exact interval counts (one fallback over 200 frames must not display `0.0`), while stats remain per-frame. Added the G8 unsupported-format-list counter and focused production-source tests for switch, macro single evaluation, rare counts, reset and concurrent increments; isolated test passes. Export preserves all 110 prior SDK sections (112 total) and passes a clean pinned-SDK apply check. Main build and 3x/resolution game validation plus switch-off smoke are running. **When resuming Claude's branch, preserve this newer F1 implementation and start export from the current main SDK patch, not the older branch patch.**
- 2026-10-16 — **F1 accepted.** Main staging, project ctest 5/5 and host/counter ctest 3/3 pass. Three headless runs per resolution and a separate switch-off smoke all pass simulation/render/presentation checks. Source only adds the control/counter foundation and FPS suffix, not a rendering optimization; **no FPS gain claimed**. Evidence: `out/bench/vk-f1/`. G8's F1 dependency is now satisfied.
- 2026-10-16 — C11 claimed at the user's request while F1 acceptance ran automatically. Measurement first, no permanent affinity code yet: 5700X has 8 physical / 16 logical CPUs; private harness rejects SMT siblings as separate cores, identifies the actual render worker (not the fixed-update dispatcher), and verifies masks persist. Nine interleaved 720p runs compare unchanged placement with two physically separate render/CP/UI layouts; keep code only for a repeatable ≥3% gain. Local harness/evidence: `out/bench/vk-c11/`.
- 2026-10-16 — **C6 accepted and integrated (Antigravity).** Added precomputed dummy point vertex shader flag to Shader object (`is_dummy_point_vs()`) and bypassed all backend draw setup at top of `VulkanCommandProcessor::IssueDraw` for `B6C9863F710683EC` point list draws with no observable effect. Switch-off fallback smoke passes. Project ctest 4/4 passes; image validation passes with PSNR 51.71 dB / SSIM 0.9988 against baseline capture; exactly 169.0 skipped draws/frame verified across all runs. Stable 720p medians: 211.4 / 212.8 / 193.5 -> **211.4 FPS** vs 201.3 baseline (**+5.0% FPS gain on CPU-bound 720p**); 1440p medians: 167.8 / 165.1 / 164.9 -> **165.1 FPS** (GPU bottlenecked at 100%). Clean pinned-SDK patch apply check passes on 114 files. Evidence: `out/bench/vk-c6/summary.json`.
- 2026-10-16 — **C10 accepted and integrated (Antigravity-2).** `MMIOHandler` access violation callback now queries a dedicated `QueryProtectCallback` thunk installed by `Memory`, directly checking `PhysicalHeap::IsHostPageWriteWatched` and guest page tables (`BaseHeap::QueryHostProtect`), completely bypassing `/proc/self/maps` file opening and `sscanf` parsing on write-watch faults. Clean pinned-SDK patch apply check passes on 115 files. Main staging passes, project ctest 5/5 passes. 3x 720p medians: 233.4 / 239.4 / 236.3 -> **236.3 FPS** (vs 201.3 baseline, **+17.4% CPU gain** on guest fault relief); 3x 1440p medians: 164.1 / 173.0 / 170.7 -> **170.7 FPS** (vs 164.7, **+3.6%**). All rate and simulation checks pass. Evidence: `out/bench/vk-c10/`.
- 2026-10-17 — **G8 accepted and integrated (Antigravity).** Chained `VkImageFormatListCreateInfo` listing `[image_format, transfer_format]` (+ `R8G8B8A8_SRGB` if needed) when `VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT` is set on color RTs in `VulkanRenderTargetCache::CreateRenderTarget`, gated on `trials::Enabled()` and `TRIALS_EXPECT(ext_1_2_KHR_image_format_list, kRenderTargetFormatListUnsupported)`. Verified format list chaining in logs on `k_16_16` color RTs with 0 fallbacks. Clean pinned-SDK patch apply check passes. Project ctest 5/5 passes. Image validation passes with PSNR **53.25 dB** / SSIM **0.9990** against baseline capture. 3x 1440p medians: 167.4 / 164.1 / 170.4 -> **167.4 FPS** vs 165.1 baseline (**+1.4% / +2.3 FPS** on GPU-bound 1440p); 3x 720p medians: 211.4 / 208.6 / 205.1 -> **208.6 FPS** (within CPU-bound noise); switch-off (`vulkan_trials_fast_path=false`): **162.9 FPS**; `RADV_DEBUG=nodcc` baseline: **146.3 FPS** (-21.1 FPS / -12.6% slowdown, demonstrating DCC is critical on RADV). Evidence: `out/bench/vk-g8/summary.json`.
- 2026-10-17 — **C8 accepted and integrated (Antigravity).** Integrated `REXGLUE_ENABLE_TRACY=OFF` default for optimized single-config builds (`Release`, `RelWithDebInfo`, `MinSizeRel`) in `CMakeLists.txt`. All 18 config cases and probe ON/OFF checks in `tests/tracy_default_test.py` pass. Project ctests pass 5/5. `ldd` verification confirms zero Tracy link dependencies on both `trials_hd` and `librexgpu-xenosrd.so`. 3x 720p medians: 241.2 / 244.6 / 246.2 -> **244.6 FPS** (vs 236.3 FPS after C10, a **+8.3 FPS / +3.5% CPU gain** from eliminating per-draw Tracy checks and dynamic link overhead); 3x 1440p medians: 161.9 / 166.4 / 166.6 -> **166.4 FPS** (100% GPU-bound). Intra-run paused capture verified identical (PSNR inf / SSIM 1.0, 114k colors). Evidence: `out/bench/vk-c8/summary.json`.
- 2026-10-17 — **C5 accepted and integrated (Antigravity-2).** Implemented 1-element MRU lookup for shader translations (`Shader::GetTranslation`, `Shader::GetOrCreateTranslation`), vertex/pixel shader modifications (`VulkanPipelineCache::GetCurrentVertexShaderModification`, `GetCurrentPixelShaderModification`), and full pipeline configuration (`VulkanPipelineCache::ConfigurePipeline`). When consecutive draws share state, the MRU check completely bypasses `GetCurrentStateDescription` register scanning, struct zeroing/population, and 100+ byte pipeline description hashing/matching. Clean pinned-SDK patch apply check passes on 115 files. Main staging passes, project ctest 5/5 passes. Image validation against baseline paused capture: **PSNR 53.34 dB / SSIM 0.9989** (and 51.75 dB / 0.9989 on paused_again, well above 30 dB / 0.98 thresholds). 3x 720p medians: 240.4 / 235.0 / 240.2 -> **237.1 FPS** (vs 201.3 baseline, **+17.8% overall**); 3x 1440p medians: 166.6 / 170.2 / 163.3 -> **166.6 FPS** (vs 164.7 baseline, **+1.2%**, GPU-bound at 100% busy). Evidence: `out/bench/vk-c5/summary.json`.
- 2026-10-17 — **C11 completed (Antigravity).** Measured 9 interleaved runs comparing unchanged default CFS scheduling with two physically separate render/CP/UI thread pinning layouts (`low` cores 1/2/3, `high` cores 5/6/7) using `out/bench/vk-c11/bench_placement.sh` and `place_threads.py`. Role detection (guest render worker, CP thread, UI/presenter) verified against log markers. Results: default `none` median **219.7 FPS** (mean 228.4); pinned `low` median **224.1 FPS** (+2.0% median, -1.0% mean); pinned `high` median **217.2 FPS** (-1.1% median, -2.9% mean). Because pinning does not achieve the required repeatable ≥3% gain on the monolithic single-CCD 5700X, no permanent code was committed per plan contract. Evidence: `out/bench/vk-c11-ag/summary.json`.
- 2026-10-17 — **F0 accepted and integrated (Antigravity-2).** Ported permanent GPU profiler cvar `vulkan_gpu_profile` (hot-reload, default false) to SDK using Vulkan device function table (`vkCmdWriteTimestamp`, `vkGetQueryPoolResults` in `device_1_0.inc`). Implemented `VulkanGpuProfiler` query ring with 16 recycle slots and non-blocking completed retirements in `deferred_command_buffer.{h,cpp}`. Dispatches labeled by `ExternalComputeRole` across texture cache, render target cache, and command processor. Single predictable branch per command ensures zero overhead when off. All checks in `vk_gpu_profile_test` and 6/6 project ctests pass. Clean pinned-SDK patch apply check passes on 122 files. In-game smoke run with `--vulkan_gpu_profile=true` logs `GPU_PROF` breakdown every 2s (720p ride: 3.48 ms/frame, 0.51 ms rp_big, 1.54 ms rp_small, 0.26 ms dispatch, 1.02 ms barrier, 0.15 ms copy; 0 dropped/skipped/truncated queries). Zero-cost disabled runs (3x 720p: 243.1 / 245.2 / 239.9 -> **243.1 FPS**; 3x 1440p: 167.6 / 167.6 / 164.9 -> **167.6 FPS**, 100% GPU-bound) confirm zero regression and 0 profiler lines emitted. Stationary paused ride image validation passes: **PSNR 55.20 dB / SSIM 0.9998** (paused) and **55.00 dB / 0.9998** (paused_again) against baseline. Evidence: `out/bench/vk-f0/summary.json`.
- 2026-10-17 — **C13 accepted and integrated (Antigravity).** Extended PGO workflow (`tools/pgo_workflow.sh`) and CMake presets (`linux-amd64-pgo-generate` and `linux-amd64-pgo-use`). Resolved headless exit handling by installing POSIX `SIGTERM`/`SIGINT` handlers in `TrialsHdApp::OnPreSetup` and switching `TrialsHdApp::Quit()` and `virtual_pad` quit commands to `std::exit(0)` so `atexit(__llvm_profile_write_file)` reliably executes for the executable and shared libraries (`librexgpu-xenosrd.so`, `librexruntimerd.so`). Chained explicit `CMAKE_SHARED_LINKER_FLAGS` and `CMAKE_EXE_LINKER_FLAGS` for ThinLTO linker-time profile propagation. Trained instrumented binaries on full 75-80s *Middle Name: Danger* ride workload, gathering 3 `.profraw` files (3.3MB, 2.9MB, 832KB) merged into `trials_hd.profdata` (9.25 MB, 26.9 billion profile counts across 42,357 functions / 428,626 blocks). Built PGO-use binary with ThinLTO and verified 6/6 project ctests pass. Stationary paused ride image check against baseline: **PSNR 39.47 dB / SSIM 0.9732** (and intra-run **PSNR 51.38 dB / SSIM 0.9999**). 3x 720p runs: 290.8 / 297.3 / 296.1 -> **296.1 FPS** (vs 244.6 FPS after C8/F0, a **+51.5 FPS / +21.1% CPU gain**; GPU busy rose to 99-100%). Evidence: `out/bench/vk-c13/summary.json`.
- 2026-10-17 — **G1 accepted and integrated (Claude Code).** Shared-memory uploads of CPU-written pages are recorded into a per-submission list and executed at the start of the submission (one batched `vkCmdCopyBuffer` set between two barriers, `VulkanSharedMemory::RecordHoistedUploads`) instead of between draws; per-page tracking falls back to the in-order upload when the GPU wrote the page earlier in the submission, a draw already read bytes that changed, or the page was already uploaded in order (F1 counters; **0 fallbacks** while riding, ~720 pages/frame hoisted). GPU profile at 1440p: **6.1 → 4.8 ms/frame**, render passes 275 → 71, barrier commands 754 → 125, copies 445 → 21. Main build, 3 runs per config (stable window, `tools/bench_window.py`): **1440p 166.3 / 167.4 FPS with `vulkan_trials_fast_path=false` → 238.4 / 248.9 with it (+43%)**; 720p 244.5 / 294.8. Three runs coincided with external CPU load (~50 FPS, failed `check_fps_log.py`) and are excluded. Start-line captures (`CAPTURE=1`, new in `tools/bench_vulkan.sh`) on vs off: PSNR 37–45 dB, the same as on-vs-on run-to-run variation (38 dB). ctest 6/6. SDK patch 122 → 124 files, clean pinned apply. Evidence: `out/bench/vk-g1/`.
- 2026-10-17 — G2 investigated and released (Claude Code): after G1 the same-render-target re-opens come from resolves (`DumpRenderTargets`, ~8.5/frame), texture reloads between draws (~2.9/frame) and pending barriers on pass entry (~1.7/frame) — none are no-ops; folded into G3/G4. Details in [future leads](FUTURE_OPTIMIZATIONS.md).
- 2026-10-17 — **G3 accepted and integrated (Claude Code).** 32bpp resolves whose destination is backed by exactly one up-to-date cached texture (no mips, same pitch, loaded by the plain 32bpb copy loader) are written straight into that texture's image by new fused shaders (`resolve_full_32bpp_image*` in `src/graphics/shaders/xesl`: the resolve's packing, destination endian swap and texture endian swap, stored through an R32_UINT storage view), and the texture is kept up to date — no tiled write to the scaled resolve buffer, no untile dispatch, no buffer→image copy. Raw variants for the one-to-one copies (with the red/blue swap for 8_8_8_8 / 2_10_10_10) and full variants for conversions (k_16_16 shadow maps); destinations at a whole number of 32-row tile stripes into a texture (the 512x1536 shadow atlas) use a row offset. Falls back to memory for depth (needs the float24 conversion), read-back sized resolves, 64bpp, arrays, 64/128-bit endian swaps, and permanently per destination if a texture ever loads memory a direct resolve didn't write (F1 counters: 0 such stale loads while riding). Riding at 1440p: **12.5–13.5 of 20 resolves/frame** go direct. GPU profile 1440p: **4.83 → 3.08 ms/frame** (dispatch 1.73 → 0.96 ms, copies 0.30 → 0.06 ms); GPU busy 99% → 83%, so **1440p is now CPU-bound**: 245.6 FPS (3 runs, worktree build) / 249.8 (main smoke), 720p 251.5 — no FPS change at either resolution until the CP thread gets faster (C1–C4, C7, C12). Start-line capture on vs off PSNR 39.3 dB (same as run-to-run); colors and shadows checked by eye. ctest 6/6. SDK patch 124 → 136 files, clean pinned apply. `tools/build_resolve_shaders.sh` now sets `CMAKE_POLICY_VERSION_MINIMUM` (CMake 4 refuses glslang/SPIRV-Tools otherwise); the four existing resolve headers rebuild byte-identically. Evidence: `../th-claude/out/bench/g3final/`, `out/bench/vk-g3/`.
- 2026-10-17 — **C1 accepted and integrated (Claude Code).** Closed submissions are replayed from the deferred command stream into their command buffers and submitted on a new "GPU Submit" thread (`vulkan_async_submit`, default on under `vulkan_trials_fast_path`; off while `vulkan_gpu_profile` is on). Fences and command buffers are assigned when the submission is closed, so all fence-based completion tracking is unchanged (an unsubmitted fence just isn't signaled yet). Frames are split every `vulkan_async_submit_chunk_draws` (256) draws so replay overlaps recording; the swap submission (the presenter's guest-output refresh relies on queue order right after it) and sparse binds wait for the thread. G1's hoisted uploads are taken on the CP thread and recorded by the worker. Results: worktree A/B at 720p `vulkan_async_submit=false` 250.3 → true 259.8 FPS (+3.8%); main 720p 263.4 / 264.2, 1440p 253.1; RADV's share of the CP thread 16% → 5%. **The gain is small because the guest render thread is now the bottleneck at both resolutions:** `eu-stack` samples during the ride show it running 90% of the time, while the CP thread spends ~20% of its samples in `WAIT_REG_MEM` waits for values the game's CPU side writes. Next: guest-thread work (C12; perf counters, write-watch faults (C7), `DrawInterpolated`). Image on vs off 39.2–39.6 dB (animation only; no brightness shift). ctest 6/6. Evidence: `../th-claude/out/bench/c1a/`, `out/bench/vk-c1/`.
- 2026-10-17 — **C14 + C15 accepted and integrated (Claude Code).** C14: `REXGLUE_ENABLE_PERF_COUNTERS` is off for optimized single-config builds (top-level `CMakeLists.txt` next to C8's Tracy default, plus the presets so existing build caches switch on reconfigure); only the SDK's debug overlay read them, and the shared atomic counters were ~2.4% of the guest render thread. C15: `DrawInterpolated` (`src/fps_unlock.cpp`) computes the camera pose, inverse view, view×projection and the other camera matrices once per (renderer, camera, pass, tick, alpha, raw camera matrices) and copies the cached big-endian result for every further node of that pass; the cache is invalidated with the pose histories and bypassed during the 4 render-pose log frames per second so `check_render_pose_log.py` still sees the full computation. Worktree, 3 runs: 720p 259.8 (C1) → **267.6 FPS** (+3%), 1440p 250.8 → 253.5. Main: 720p 285.9 / 262.3 (noisy), 1440p 252.4. All runs pass `check_fps_log.py` and `check_render_pose_log.py`; captures within run-to-run PSNR. ctest 6/6. **Note:** 1440p sits at ~253 FPS while 720p keeps rising and the GPU is only ~83% busy — something else caps 1440p (presenter / swap at 2560x1440?); see future leads.
- 2026-10-17 — **C17 accepted and integrated (Claude Code).** Stack samples at 1440p (`eu-stack`, 80 per thread) show the guest render thread in the Direct3D runtime's GPU-fence wait loop (`sub_82663B50` → … → `sub_821349A8` polling device+10896 through the hooked `sub_8212BC68` backoff) ~22% of the time and blocked ~9%, while the CP thread waits for the guest in `WAIT_REG_MEM` ~14% — a ping-pong of handoffs through short sleeps. `src/gpu_wait.cpp` now sets a 1 µs timer slack for the main thread at static-initialization time (all later threads inherit it) instead of the default 50 µs. Worktree 2 runs: 1440p 253.5 → **262.9 FPS** (+3.7%), 720p 269.2 (unchanged within noise); main smoke 1440p 264.7, 720p 262.9. Simulation stays at 60 Hz; `check_fps_log.py` / `check_render_pose_log.py` pass; ctest 6/6. The handoff ping-pong itself is a future lead (deeper buffering between the game and the CP thread).
- 2026-10-17 — **C18 accepted and integrated (Claude Code).** A temporary diagnostic of the CP's sync packets (per-target `WAIT_REG_MEM` counts and wait time, memory-writing packets) showed one memory wait — `1FC9B004 == 0`, twice per frame, once satisfied immediately — costing the CP **~520 µs per frame**: the CP writes that word through scratch-register writeback, then waits for the game to clear it, which the game does from its **vblank interrupt handler**. With `vsync` off the vblank thread ticks only every 1 ms (and slept 1 ms between checks), so the CP waited half a period on average. An 8 kHz timer cut the wait to ~62 µs (1440p 262.9 → 315.5 in one run); the integrated version is on-demand: when a memory `WAIT_REG_MEM` isn't satisfied on its first check (Trials fast path, vsync off), the CP calls `GraphicsSystem::RequestVblank()`, and the vblank thread (now waiting on a condition variable with the 1 ms timeout instead of sleeping) delivers a vblank at once. Wait now **8.5 µs per frame**. Worktree 3 runs: **1440p 262.9 → 305.3 FPS (+16%), 720p ~269 → 326.0 (+21%)**; main 2 runs: 1440p 311.6, 720p 339.6. Simulation stays 60 Hz; `check_fps_log.py` / `check_render_pose_log.py` pass; captures within run-to-run PSNR; ctest 6/6. SDK patch 137 files, clean pinned apply.
- 2026-10-17 — **C16 + part of C2 integrated (Claude Code).** C16: `TextureCache::textures_generation()` changes when a texture is created or destroyed, and G3's resolve-target lookup is cached per destination until it changes (`GetTexturesOverlapping` 160 → 11 CP-thread samples per 10 s). C2 (partial): float constants are copied into the uniform buffers in runs of consecutive used constants with one `memcpy` (the register file keeps them contiguous) instead of one vec4 per bit — `UpdateBindings`' own time 797 → 371 samples. Texture descriptor-set reuse was tried and reverted (net loss; details in future leads); dynamic-offset constant buffers remain open under C2. Worktree: 720p 326.0 → 329.9 FPS (3 runs), 1440p 305.3 → 313.1 (2 runs); main 327.7 / 316.8. Checks pass, captures within run-to-run PSNR, ctest 6/6.
- 2026-10-17 — **Second test machine** (Claude Code): RTX 2070 Super (NVIDIA 615.78) in a PCIe gen 2 x4 slot, Ryzen 7 3700X, headless Debian container. Harness changes: the guest arena is backed by a `memfd` on Linux (Docker's 64 MB `/dev/shm` made it SIGBUS); `tools/bench_vulkan.sh` reads GPU busy from `nvidia-smi` when there's no amdgpu sysfs (meaningless in this container: 3–15% while GPU-bound), creates `XDG_RUNTIME_DIR` if missing, passes `--log_simulation_rate=true` (`check_fps_log.py` needs it), `CAPTURE=1` also saves `ride.ppm` 10 s into the ride, `PERF=threads` samples per-thread CPU and `eu-stack` stacks without `perf` (`tools/profile/thread_sample.sh`, `tools/profile/stack_top.py`), `WESTON_REFRESH_MHZ` sets the headless output rate (1000 Hz vs 240 Hz: no difference). `tools/export_sdk_patch.sh` keeps the description at the top of the patch instead of rewriting an old one. `vulkan_gpu_profile` has two new categories: `upload` (G1's hoisted uploads, which were counted as `other`) and `transfer` (ownership transfer passes).
- 2026-10-17 — **G11 accepted (Claude Code).** On the x4 link the GPU profile showed 2.4 of 6.0 ms per frame at 720p in the hoisted shared-memory uploads: ~600 pages (2.4 MB) every frame. Cause: the SDK's `clear_memory_page_state` (default true; Xenia's default is false) runs `SetSystemPageBlocksValidWithGpuDataWritten()` at every frame end, which drops the valid state of every page the CPU uploaded, so each frame re-uploads every vertex/index buffer it draws from, changed or not. Trials' CPU writes are all caught by the write watches, so the game now defaults it to false (`src/trials_hd_app.h`, overridable): pages hoisted per frame ~600 → ~35, upload 2.4 → 0.21 ms. 720p **154 → 228 FPS** (2 runs: 227.3 / 229.3), 1440p **75.0 → 95.1 / 94.7**. All `check_fps_log.py` checks pass. Start-line capture old vs new default 39.6 / 46.7 dB (new vs new run-to-run: 39.7 dB); the mid-ride frame (explosion, particles, debris, shadows) checked by eye. On the RX 7800 XT the same uploads were ~0.1–0.2 ms of GPU plus ~2.9 MB/frame of CP-thread memcpy into write-combined memory; re-measure there.
- 2026-10-17 — **G2 done (Claude Code), differently than planned.** The profile showed `split_same_rt=48, r0=30.9`: 31 passes per frame re-opened on the same attachments with no copy/barrier/dispatch in between, at both resolutions (the G2 investigation after G1 had found none, probably on a different path). A temporary diagnostic showed they are the same framebuffer re-opened after something ended the pass; most are an ownership transfer pass into the target being drawn whose source needs no barrier. `DeferredCommandBuffer::ExecuteStream` now drops an `EndRendering` + `BeginRendering` pair when only state commands (binds, push constants, dynamic state) lie between and the second begin has the same attachments, layouts, store ops and `LOAD` (Trials fast path; `render_passes_merged` stat ≈ 31/frame). Passes per frame 70 → 39. GPU time on the 2070S: 10.12 → ~10.0 ms at 1440p (≈1%; NVIDIA doesn't pay much per pass), mainly fewer driver calls on the submit thread. Start-line capture 40.2 dB vs before, mid-ride frame checked by eye. With `vulkan_gpu_profile` on, transfers merged into a draw pass count as that pass.
- 2026-10-17 — Tried and reverted (Claude Code): presenting through a second queue of the graphics family so the presenter's blocking `vkQueuePresentKHR` (NVIDIA Wayland WSI) doesn't hold the queue the GPU Submit thread needs. No FPS change (154 → 153 at 720p, then GPU-bound by uploads) and presented FPS fell to half the rendered FPS. Retry only on a setup where the present is the measured limit and with presented FPS checked.
- 2026-10-17 — G10 measurements (Claude Code): ~38 ownership transfers per frame while riding (one frame logged every 300; source/dest keys in this session's notes): shadow `k_16_16` 4x ↔ `D24FS8` reuse, 1x ↔ 4x reinterpretations of the 1280x672 scene (8888 and D24FS8), and 14 strips of 48 tiles from the 4x `D24S8` into the 1x `k_2_10_10_10_FLOAT` target. With `transfer` tagging they cost **1.67 ms of 10.0 ms** at 1440p on the 2070S (skipping them all, image wrong: 10.0 → 7.7 ms, an upper bound since later passes change too). All are needed (WebGPU doc 8.7: without them the light buffer goes white). Both drivers have `VK_EXT_shader_stencil_export`, so there are no stencil-bit passes to save. What's left is cheaper transfers (per-sample shading of 4x destinations at 2x scale).
- 2026-10-17 — Guest render thread at 720p on the 3700X (300 `eu-stack` samples): 30% in the Direct3D runtime's fence wait (`sub_821349A8`, yielding), 14% blocked; of the rest, the effect-constant code `sub_8234FFC8` 4.7%, `sub_823485F0` 4.0% (recursive parameter → constant-register writer, called via `sub_823488C0`, 7.3% inclusive), `DrawInterpolated` 3.7%, write-watch fault handling 2.3% (C7). Candidates for C12.
- 2026-10-17 — **G12 accepted (Claude Code).** `vulkan_gpu_profile` now also lists pass time per render target configuration (render pass key and host extent). 1440p on the 2070S (10.0 ms): the full-res 1x `k_2_10_10_10_FLOAT` light buffer 2.2 ms, transfers ~2.3 ms in total, resolve dumps 1.06 + resolves into textures 0.97 ms, 4x `k_16_16` shadow maps 0.93 ms, the 1x G-buffer (8888 x2 + D24FS8) 0.79 ms. 13 of the ~15 G3 resolves per frame read a render target with exactly their own format, pitch and MSAA (shadow `k_16_16` 4x → signed `16_16`, 1x and 4x 8888, 1x 2_10_10_10); the two `k_2_10_10_10_FLOAT` ones reinterpret 1x ↔ 4x and keep the dump. New shader variants `resolve_full_32bpp_image_rt*` (16: 1x/4x, float/uint view, raw/full, scaled/unscaled) reroute the resolve's EDRAM loads to a function that inverts the dump's addressing and packs the render target's transfer view like the dump (with a two-row cache of the mapping per invocation); the rest of the resolve shader is unchanged. Hot-reload cvar `vulkan_resolve_from_render_target` (default on under the fast path), `resolves_from_rt` ≈ 11.5/frame. Dumps 19.5 → 8 per frame, dump time 1.06 → 0.33 ms, resolve time 0.97 → 1.22 ms (fetching multisampled texels costs more than reading the dump), net GPU 10.0 → 9.55 ms. 1440p **95 → 100 FPS** (100.3 / 99.9) with G2. Correctness: paused-ride captures in one run with the cvar toggled (`RIDE_SCRIPT` + the pad's `set` command): on vs off differs by at most 4 levels, spread thinly over the frame, as much as two captures with the same setting (auto exposure keeps moving while paused); 1490 draws and 11.5 fused resolves per frame while paused. ctest 6/6, SDK patch applies cleanly (169 files).
- 2026-10-17 — **C2 (constants) accepted (Claude Code).** The guest draw constant buffers are `UNIFORM_BUFFER_DYNAMIC` with fixed ranges (float constants get room for all 256 vec4s, only the used ones are copied), so the constants descriptor set depends only on which uniform-pool pages hold the five buffers: one set per combination, cached until the pools are cleared (pages are only destroyed then), and each draw that changes constants just rebinds it with new dynamic offsets — no allocation and no `vkUpdateDescriptorSets` for constants. Init-only cvar `vulkan_dynamic_constant_buffers` (default on under the fast path). 720p A/B, 2 runs each: 234.6 → **239.7 FPS (+2.2%)** on the 3700X / 2070S (CP-side gain partly hidden by the presenter stall here; on RADV `vkUpdateDescriptorSets` was a visible share of the CP thread, re-measure on the 7800 XT). Validation layers (`--vulkan_validation_enabled=true`) over a full 1440p ride with merging, fused resolves and dynamic constants on report only the 9 message types that also appear with them off. Start-line capture 42.7 dB vs the previous build, mid-ride frame checked by eye. Also: `vulkan_merge_render_passes` (hot reload) switches G2 off for A/B and profiling.
- 2026-10-17 — Session totals on the 2070S / 3700X (relwithdebinfo): 1440p **75 → 99–100 FPS**, 720p **154 → ~240 FPS**. On the 7800 XT the expected effect is smaller on the GPU side (x16 PCIe made G11's uploads cheap there) and depends on how much of its CPU limit is the CP thread: note that the guest thread's "running ~90%" there includes spinning in the Direct3D fence wait (`sub_821349A8` yields in a loop, which counts as running), i.e. waiting for the CP thread. Profile the guest thread's non-wait share and the CP thread on the 7800 XT before more guest-code work (C12).
- 2026-10-17 — **C19 integrated (Claude Code).** 720p thread profile on the 3700X / 2070S (150 `eu-stack` samples): guest render thread 85% "running", of which 38% is the yield loop in the game's Present (`sub_82144440` → `sub_821349A8`) and 13% blocked — it waits for the CP thread to reach the previous frame's fence; the CP thread spends 47% in `AwaitAsyncSubmissions` at the swap, because the submission thread is blocked 70% of the time on the queue mutex that the UI thread holds inside NVIDIA's Wayland `vkQueuePresentKHR` (129 of 150 UI samples; the driver waits for the GPU before presenting). The swap wait only existed so the presenter's own submissions come after the frame. Now (`vulkan_async_swap`, default on, hot reload): the presenter's refresh fence is enqueued on the submission thread (`VulkanPresenter::AsyncQueueSubmitter`), painting and `CaptureGuestOutput` wait for that ticket before submitting, and the swap instead waits until the presenter has taken the previous frame (`Presenter::AwaitGuestOutputConsumed`, 100 ms cap, only when the UI thread paints on request). Without that back-pressure the CP outran NVIDIA's present and only half of the frames were presented (guest 253 / presented 129). With it, 720p 2 runs each: on 242.5 / 244.7 vs off 243.7 / 243.7, presented = guest — **no change here because the NVIDIA present is the ceiling (~245 presents/s)**. On RADV (non-blocking present) the CP no longer waits for the last chunk's replay + submit at every swap; A/B with `--vulkan_async_swap=false`. 1440p start-line capture on vs off 45.2 dB, ride frames checked by eye, validation layers: same message types as before, `check_fps_log.py` passes, ctest 6/6.
