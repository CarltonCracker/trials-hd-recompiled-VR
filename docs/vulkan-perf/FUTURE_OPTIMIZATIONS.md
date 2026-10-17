# Vulkan renderer: future optimization leads

New leads found while executing [PLAN.md](PLAN.md). These are investigations, **not completed optimizations or measured FPS gains**. Existing board tasks remain in PLAN.md; this file does not silently enlarge their scope.

## 2026-10-16 — Make benchmark selection workload-aware

- **Evidence:** `tools/bench_vulkan.sh` calls `tools/bench_summary.py --threshold 61`. The first C9 baseline reported **275.5 FPS**, but those samples were menus with **72 draws and 1 resolve/frame**. The actual riding samples (over 1,000 draws and at least 18 resolves) had a **40.3 FPS** median during concurrent external builds. `tests/check_fps_log.py` correctly rejected the run. Artifact: `out/bench/vk-c9/before-1x-1/trials_hd.log` (local, ignored).
- **Lead:** select a stable riding window by time and draw/resolve workload, not an FPS threshold; reject menu/loading-only runs and report presented FPS alongside rendered FPS. Preserve an independent screenshot check.
- **Measurement hygiene:** serialize GPU runs and avoid concurrent compilation, including builds from other agents. Benchmark immutable staged binary/library snapshots. The shared harness now uses `.git/vk-bench.lock` to serialize participating agents; it still cannot stop unrelated builds.
- **Risk:** an FPS cutoff can hide slow gameplay and make an optimization appear dramatically faster without measuring gameplay at all. The invalid C9 baseline must not be used for before/after claims.

## 2026-10-16 — Timer callback re-entrancy at ring capacity

- **Evidence:** `thirdparty/rexglue-sdk/src/core/timer_queue.cpp`: callbacks execute on the sole consumer thread. `QueueTimer` synchronously calls `claim_strategy_.claim_one()` on the 512-entry ring. A callback that queues enough timers before returning can fill that ring and wait for the consumer—which is itself.
- **Related shutdown case (Claude Code review):** the destructor requests stop before enqueueing its kick item; if the ring is full and the consumer has already stopped, that enqueue may also block. Both cases predate C9.
- **Lead:** first instrument callback-origin timer scheduling and high-water occupancy in real gameplay. Only if it occurs, consider a dispatch-thread-local insertion path that preserves timer ordering and cancellation semantics.
- **Risk:** this is a pre-existing correctness/liveness limit, **not a demonstrated Trials HD bottleneck**. C9's blocking-wait change does not introduce or solve it. Do not replace the queue or add an unbounded allocation path without evidence.

## Validation still needed for C9

The fully idle queue already backs off effectively: the initial standalone probe measured about 0.465 ms CPU / 200 ms wall with the original spin strategy versus 0.02–0.038 ms with blocking. That alone does **not** substantiate PLAN.md's roughly half-core game-time estimate. A subsequent 1 kHz recurring-timer microprobe measured 21.201 ms CPU / 200 ms wall (199 ticks) with spinning versus 2.557 ms (200 ticks) with blocking, while other builds were active. This is evidence of reduced timer-wait CPU work, not an FPS result. Measure the in-game timer thread, simulation cadence and whole-game performance before claiming a game-time saving. C9 remains an active board task, not a future implementation lead.

## 2026-10-16 — Measure lightweight perf-counter cost separately from Tracy

- **Evidence (C8, Claude Code):** the SDK's independent `REXGLUE_ENABLE_PERF_COUNTERS` option defaults ON and enables counters in non-Release configurations (`thirdparty/rexglue-sdk/CMakeLists.txt:25–31`). `include/rex/perf/counter.h` still updates draw/texture/pipeline counters and calls `ResetFrameCounters` / `WriteCsvFrame` from `Profiler::Flip` when enabled. Disabling Tracy alone does not disable these counters.
- **Lead:** profile or A/B a no-counter RelWithDebInfo build after C8, including CPU time per draw, rather than assuming another gain. Keep counters available for overlays/CSV users.
- **Risk:** removing counters changes diagnostics; no performance gain is measured and no default is changed by this note.

## 2026-10-16 — Multi-config optimized profiling behavior

- **Evidence (C8):** SDK profiling definitions use `$<NOT:$<CONFIG:Release>>`; a multi-config generator still compiles Tracy into RelWithDebInfo by default. C8 preserves the SDK default for such generators, since one configure-time boolean cannot independently toggle Debug and RelWithDebInfo. Current native presets are single-config Ninja and disable Tracy for their optimized builds.
- **Lead:** if multi-config builds become a supported workflow, make the SDK profiling compile/link definitions configuration-aware while retaining an explicit profiling opt-in. First confirm that the extra behavior is needed; do not redesign the build now.
- **Risk:** inconsistent definitions across the runtime, generated code and GPU plugins can leave missing symbols or incompatible profiling behavior.

## 2026-10-16 — Verify whether G8 actually enables shadow-target DCC

- **Evidence (Claude Code G8 source review):** `VulkanRenderTargetCache::CreateRenderTarget` makes an image mutable only when its transfer view differs (or an sRGB view is needed; currently false). Among the recorded Trials formats, this affects the `k_16_16` 4x-MSAA shadow/light targets, rendered as `R16G16_SNORM` (or the SFLOAT fallback) with an `R16G16_UINT` transfer view. The main 8888 and HDR formats are not made mutable by this code and are already eligible for compression.
- **Lead:** confirm the resulting shadow image's compression state with driver diagnostics/a capture, and compare `RADV_DEBUG=nodcc`, before attributing a timing change to G8. A view-format list is permission for a better layout, not proof that RADV accepts this numeric-format pair or enables MSAA DCC.
- **If compression remains off:** investigate a bit-preserving transfer path that avoids an incompatible numeric view, coordinated with G3/G10. Do not simply substitute a floating-point view: the existing transfer code explicitly preserves NaN payloads and both SNORM representations of −1.
- **Risk:** changing ownership-transfer formats without bit-exact tests can silently corrupt shadow/light data. No such format change is implemented, and G8 is not yet game-tested.

## 2026-10-16 — Identify timer CPU by thread ID / stack, not only its name

- **Evidence:** the valid C9 720p baseline profile attributes 5.41% of all process cycle samples to the timer queue's `_M_run` body, under the generic `trials_hd` command name. The queue asks `set_current_thread_name` for `rex::thread::TimerQueue`, while the direct POSIX helper forwards that string to `pthread_setname_np` without truncation; Linux's 15-character thread-name limit can reject it. The main UI thread is also called `trials_hd`.
- **Lead:** group profiles by thread ID and symbol when validating timer savings, and consider making long diagnostic thread names visible without conflating threads. This is a profiling-quality lead, not an FPS optimization.
- **Risk:** grouping only by command name can misattribute UI time to the timer or miss the timer entirely. The percentage is a share of sampled process cycles, not a percent of one CPU core.

## 2026-10-16 — Distinguish the FPS dispatcher from the hot guest render worker

- **Evidence (C11 preflight):** in the profiled 720p run, `fixed updates/sec` logs come from TID 45999 (Main XThread, about 0.8% of process-cycle samples), while the hot render worker is TID 46044 (about 31.3%). The `render pose: frame=` marker comes from that worker. CP is TID 45979 and UI/presenter is the process main TID 45968. A preflight assertion caught the wrong dispatcher-based identification before any placement measurements were accepted.
- **Lead:** use stack/profile-confirmed render-worker IDs for affinity experiments, guest render profiling and future direct-render/replay investigations; the thread reporting FPS is not necessarily the thread doing the expensive draw work.
- **Risk:** pinning or optimizing the lightweight dispatcher can look like a failed render-thread experiment. C11's temporary harness now uses the calibrated render-pose marker and verifies affinity masks after measurement.

## 2026-10-16 — Host occlusion queries stall the whole GPU (Claude Code, during G1/G2)

- **Evidence:** `VulkanCommandProcessor::EndGuestOcclusionQuery` (`thirdparty/rexglue-sdk/src/graphics/vulkan/command_processor.cpp`, a ReXGlue addition — upstream Xenia fakes these) ends the submission and calls `AwaitAllQueueOperationsCompletion()` for **every** guest query (EVENT_WRITE_ZPD), and both begin and end call `EndRenderPass()`. **Trials HD does not issue any while riding** (the `occlusion_queries` counter in the `log_guest_fps` line stayed at 0), so this is *not* the source of the ~30 no-op pass re-opens and costs nothing today.
- **Lead:** only relevant if another scene (menus, other tracks, DLC) issues queries: make results asynchronous (write the guest result when the submission's fence completes) and begin/end host queries inside the pass. Check with the counter first.
- **Risk:** query results feed game logic; a guest that spins on the result needs the CP thread to keep polling fences.

## 2026-10-16 — CPU shadow of uploaded pages for the G1 fallback (Claude Code)

- **Evidence:** G1 hoists a shared-memory upload to the start of the submission only when the bytes earlier draws read in the same submission are unchanged; for pages valid from an earlier submission there is nothing to compare against (`fallbacks/frame: upload_read_unknown`).
- **Lead:** keep a CPU copy of the last uploaded contents of frequently re-uploaded pages so those can be compared and hoisted too.
- **Risk:** memory and memcpy cost; only worth it if `upload_read_unknown` stays high after G1.

## 2026-10-16 — Other copies between draws (Claude Code)

- **Evidence:** the same "copy between draws ⇒ pass split + 2 barriers" pattern as the shared-memory uploads exists in `IssueDraw` (guest DMA 32-bit index copy, only without `fullDrawIndexUint32`, so probably not on RADV), `VulkanPrimitiveProcessor::BeginSubmission` (once) and the texture cache loads.
- **Lead:** after G1, use the GPU profile's split reasons to see which still fire; move those copies to the pre-pass stream G1 adds (`VulkanSharedMemory::RecordHoistedUploads`).
- **Risk:** same ordering hazards as G1 (GPU writes earlier in the submission).

## 2026-10-16 — Batch contiguous mprotect calls during write-watch enable/trigger (antigravity-2)

- **Evidence:** `PhysicalHeap::EnableAccessCallbacks` (`thirdparty/rexglue-sdk/src/system/xmemory.cpp`) iterates through every 4KB page in the requested range and executes `memory::Protect(host_address, system_page_size_, memory::PageAccess::kReadOnly)` individually for each page. For multi-page buffers (e.g., dynamic vertex/index buffers or textures spanning tens or hundreds of pages), this issues dozens or hundreds of sequential `mprotect` system calls under lock.
- **Lead:** Detect contiguous runs of un-watched pages within the requested range and batch them into a single `memory::Protect(first_host_address, run_length * system_page_size_, memory::PageAccess::kReadOnly)` syscall, while keeping per-page bookkeeping in `system_page_write_watch_`. Similarly coalesce runs when un-protecting during `TriggerCallbacks`.
- **Risk:** Bookkeeping must accurately reflect partial overlapping watches and avoid modifying protection state on pages that are already protected or belong to different allocations.

## 2026-10-16 — Direct lookup or interval indexing for MMIO ranges on fault path (antigravity-2)

- **Evidence:** In `MMIOHandler::ExceptionCallback` (`thirdparty/rexglue-sdk/src/system/mmio_handler.cpp`), on every access violation, the handler iterates linearly through `mapped_ranges_` (`for (const auto& range : mapped_ranges_)`) to check if the faulting address belongs to a registered MMIO device before handling write watches.
- **Lead:** Replace the linear vector scan with an interval map or direct page table lookup mask to test in O(1) time whether a faulting address is MMIO, speeding up the fast rejection path for frequent write-watch faults.
- **Risk:** The number of registered MMIO ranges is small (~tens), so overhead is modest, but any latency reduction under signal handling directly benefits guest thread frame time.

## 2026-10-16 — Early color mask test in IsPixelShaderNeededWithRasterization (antigravity)

- **Evidence:** `draw_util::IsPixelShaderNeededWithRasterization` (`src/graphics/util/draw.cpp`) is evaluated per draw. Even when all color target writes are masked (`(regs[XE_GPU_REG_RB_COLOR_MASK] & 0xFFFF) == 0`), it still executes a loop scanning bits in `rts_remaining = shader.writes_color_targets()` with `rex::bit_scan_forward`, reading `RB_COLOR_INFO` for each target format.
- **Lead:** Test `(regs[XE_GPU_REG_RB_COLOR_MASK] & 0xFFFF) == 0` before entering the bit scan loop over render targets. If all color writes are disabled and the shader doesn't kill pixels, write depth, or write memexport, the pixel shader is definitively not needed.
- **Risk:** Alpha-to-coverage and alpha test side effects must be respected (the existing check handles coverage depending on alpha before the loop).

## 2026-10-16 — Skip memexport_ranges_.clear() when empty (antigravity)

- **Evidence:** In `VulkanCommandProcessor::IssueDraw`, `memexport_ranges_.clear()` is called unconditionally on all ~1350 draws per frame, while only ~18 draws per frame in Trials HD utilize memexport.
- **Lead:** Gate clearing on `if (!memexport_ranges_.empty()) memexport_ranges_.clear();` to avoid vector bookkeeping overhead on 98.7% of draws.
- **Risk:** Zero risk; preserves identical semantics.

## 2026-10-16 — Precomputed shader static property bitmask (antigravity)

- **Evidence:** C6 demonstrated that caching a precomputed flag (`is_dummy_point_vs_`) in `Shader` allows single-instruction fast-path decisions during draw dispatch without touching ucode analysis structures.
- **Lead:** Pack static shader traits analyzed at load time (writes depth, kills pixels, writes memexport, writes interpolators, uses texture fetches, is point VS) into a single 32-bit bitmask on `Shader`. Draw dispatch checks can test multiple properties with a single bitwise AND.
- **Risk:** None if flags are strictly derived during shader analysis and immutable thereafter.

## 2026-10-17 — Static library linkage for cross-DSO ThinLTO inlining (antigravity)

- **Evidence:** C13 proved that ThinLTO + PGO yields a massive +21.1% frame rate improvement (244.6 -> 296.1 FPS). However, `trials_hd`, `librexgpu-xenosrd.so`, and `librexruntimerd.so` are built as separate dynamic shared objects (DSOs) with `$ORIGIN` RPATHs. Under ELF dynamic linking, function calls crossing DSO boundaries (such as guest draw dispatches calling `VulkanCommandProcessor::IssueDraw`, or backend calls to `PhysicalHeap::IsHostPageWriteWatched`) must go through PLT/GOT stubs and cannot be inlined by LLVM ThinLTO.
- **Lead:** Add a monolithic build option (e.g. `-DTRIALSHD_MONOLITHIC=ON`) that links `rexgpu-xenos` and `rexruntime` as static archives directly into the main `trials_hd` binary. This enables ThinLTO whole-program devirtualization, cross-module inlining, and dead-code elimination across the entire engine and backend boundary.
- **Risk:** Staging helper scripts and dynamic plugin reloading would need fallback handling for static builds.

## 2026-10-17 — Post-link BOLT binary reordering on Zen 3 (antigravity)

- **Evidence:** With PGO branch weights now available, the hot code path is tightly clustered, but function and basic block layouts in the ELF text section still suffer from instruction cache alignment misses and cross-page branch penalties across large recompiled guest translation units.
- **Lead:** Apply LLVM BOLT (`llvm-bolt --reorder-blocks=ext-tsp --reorder-functions=hfsort+ --split-functions`) to the final linked binary. On Zen 3 (Ryzen 7 5700X), optimizing L1I cache line packing and branch target alignment frequently yields an additional 2–5% CPU throughput.
- **Risk:** Requires `perf` LBR (Last Branch Record) data or BOLT instrumentation instrumentation phase; does not apply to non-x86_64 targets.




## 2026-10-17 — What still re-opens the same render pass after G1 (Claude Code, G2 investigation)

- **Evidence:** temporary backtrace diagnostic (who ended a pass that the next pass re-opens on the same framebuffer), 1440p ride with G1: resolves ending the pass for `VulkanRenderTargetCache::DumpRenderTargets` (compute dump of the render target into the EDRAM buffer before the resolve copy) **~8.5/frame**; texture reloads between draws (`TextureCache::RequestTextures` → `LoadTextureDataFromResidentMemoryImpl` → `SubmitBarriers(true)`) **~2.9/frame**; pending barriers when entering the pass (`SubmitBarriersAndEnterRenderTargetCacheRenderPass` → `SubmitBarriers`) **~1.7/frame**. None of them were no-ops (commands were always recorded in between), so PLAN's G2 premise ("~30 no-op re-opens") no longer holds after G1.
- **Lead:** these belong to G3 (resolve straight into textures: no EDRAM dump, no re-untiling reload) and G4 (one reload per resolved texture). A remaining idea: when the game resolves a render target and keeps drawing into it, the resolve copy could be deferred to the end of that pass if nothing samples the destination before then.
- **Risk:** deferring resolves changes ordering against later draws that sample the resolved texture; needs per-texture tracking.

## 2026-10-17 — Filter redundant dynamic viewport, scissor, blend, and depth-bias computation in IssueDraw (antigravity)

- **Evidence:** In `VulkanCommandProcessor::IssueDraw` (`src/graphics/vulkan/command_processor.cpp:5790–5875`), on every single draw (~1,350 draws per frame), the backend recomputes:
  1. `draw_util::GetViewportInfo` from `PA_CL_VPORT_*` registers, unpacks floats, builds `VkViewport`, and tests 6 individual float inequalities in `SetViewport`.
  2. `draw_util::GetScissor` from `PA_SC_WINDOW_*` registers, performs resolution scale multiplications, builds `VkRect2D`, and tests 4 inequalities in `SetScissor`.
  3. `draw_util::GetPreferredFacePolygonOffset` from `PA_SU_POLY_OFFSET_*` registers, calculates polygon offset factors, and invokes `std::memcmp` twice for depth bias.
  4. Unpacks `RB_BLEND_*` registers and performs `std::memcmp` across 4 float blend constants.
  5. Computes stencil compare/write masks and references.
  In Trials HD, within a render pass (e.g., shadow passes with 554 draws or main scene with 318 draws), the viewport, scissor, blend constants, and depth bias stay identical across dozens or hundreds of consecutive draws.
- **Lead:** Implement dirty tracking or an MRU check on the backing register index ranges (`PA_CL_VPORT_*` 0x2210–0x2215, `PA_SC_WINDOW_*` 0x2205–0x2208, `RB_BLEND_*` 0x2200–0x2203, `PA_SU_POLY_OFFSET_*` 0x2216–0x2219). When none of these register ranges have been written since the previous draw (which is true for >90% of draws in Trials HD), completely skip `GetViewportInfo`, `GetScissor`, `GetPreferredFacePolygonOffset`, float math, and `std::memcmp` comparisons.
- **Risk:** Zero risk to rendering output; dynamic state update flags already ensure Vulkan commands are only recorded when values actually change.

## 2026-10-17 — G3 follow-ups: the resolves that still go through memory (Claude Code)

- **Depth resolve.** One 1280x672 D24FS8 → `k_24_8_FLOAT` resolve per frame (3.4 MB at 1x, fog / soft particles) still does dump → resolve → `DepthFloat` untile → buffer→image copy. A depth variant of `resolve_full_32bpp_image` needs the texture loader's float24 → float32 conversion of the depth bits (copy it from the `DepthFloat` load shader to stay bit-exact). Worth ~0.1–0.2 ms GPU at 1440p; the GPU is no longer the limit there (G3 notes), so low priority.
- **Small shadow maps read back to the CPU.** The 64x64 and 128x128 `k_16_16` shadow resolves (16 KB and 64 KB) fall under `readback_resolve_max_bytes = 65536`, so they are resolved to memory *and* copied back to guest memory every frame, although only the luminance chain (4–16 KB `k_8_8_8_8`) is known to be read by the CPU. A per-format rule (e.g. read back only 8_8_8_8 ≤ 16 KB) would let them go direct and drop their readback copies. Needs confirmation that the game never reads them on the CPU (watch the pages, or diff gameplay).
- **A resolve no texture ever samples.** Destination `1BB8C000` (983,040 bytes, 8_8_8_8 640-wide) is resolved every frame but no texture overlaps it (`g3_resolve_no_texture` ≈ 1/frame). If nothing reads it (CPU or GPU), skipping it saves a dump + resolve. Find the consumer first (memexport/vertex fetch, CPU).
- **Fast readback reads the other slot without a fence check.** `IssueCopy_ReadbackResolvePath` in `kFast` mode reads `1 - write_index` after only an invalidate. If that copy hasn't finished on the GPU the CPU sees stale or torn luminance values. Not a stall (good), a possible correctness race to check.
- **`GetTexturesOverlapping` scans all textures** for every resolve (≈20/frame × a few hundred textures). Negligible now; index textures by page if the texture count grows.

## 2026-10-17 — The game and the CP thread ping-pong (Claude Code, C15/C17; partly solved by C18)

- **Evidence:** at 1440p, stack samples of the guest render thread show ~22% in the Direct3D runtime's fence wait (`sub_821349A8`, polling the GPU-written fence at device+10896 via `sub_8212BC68`), ~9% blocked, and the CP thread ~14% in `WAIT_REG_MEM` waiting for values the game writes. Both sides wait on each other within each frame; 720p shows much less of it. After C17 (1 µs timer slack) 1440p is ~263 FPS while the GPU is ~87% busy and neither thread is saturated.
- **Lead:** find what the fence at device+10896 tracks and which packet writes it (EVENT_WRITE / MEM_WRITE in the PM4 stream, written as the CP *processes* it), and what the `WAIT_REG_MEM`s wait for (frame-start sync, ring space, dynamic buffer reuse). If the CP writes the fence only late in a frame (e.g. at swap), writing it as early as is safe, or letting the guest run one more frame ahead (deeper D3D frame/fence limit), would overlap the two threads instead of alternating them. Also consider spinning instead of napping in `ExecutePacketType3_WAIT_REG_MEM` when the game is expected to release the wait within microseconds.
- **Risk:** fence/wait semantics are game-visible; writing a fence before the GPU work it guards is complete breaks resource reuse (corrupted vertex/constant data). Needs the packet-level trace first.

## 2026-10-17 — After C18: what the game's fence wait is (Claude Code)

- **Evidence:** the sync diagnostic (C18) also shows ~30 register `WAIT_REG_MEM`s per frame on register `0x0A31` (all satisfied immediately, ~3 µs/frame total — cheap), ~8 `EVENT_WRITE_SHD` per frame to `1FC9C000`/`1FC9C004`, and scratch-register writebacks to `1FC9B000..1FC9B014`. The guest's Direct3D fence wait (`sub_821349A8`, device+10896 → the fence) is probably one of the `EVENT_WRITE_SHD` targets; it is written as soon as the CP *parses* the packet.
- **Lead:** re-measure the guest thread's fence-wait share after C18 (it was ~22% at 1440p before). If it is still large, the game is limited by how far ahead its D3D runtime lets it run of the CP (ring/segment fences), and the remaining lever is CP parsing speed between fences, not GPU work.
- **Risk:** none (measurement).

## 2026-10-17 — C2 notes: texture-set reuse doesn't pay, constants dominate (Claude Code)

- **Tried:** reusing the previous draw's vertex/pixel texture descriptor sets within a frame when the images and samplers are identical (the SDK's `TODO: Reuse texture and sampler bindings if not changed`). CP-thread profile at 720p: `radv_UpdateDescriptorSets` 252 → 221 samples, `WriteTransientTextureBindings` 40 → 26, but `UpdateBindings`' own time rose 797 → 954 (building and comparing the infos) — a net loss. **Reverted.**
- **What remains:** the driver's descriptor work is dominated by the **constant buffers** (`ac_set_buf_desc_word3`, `ac_build_raw_buffer_descriptor`), written on almost every draw because float constants change per object; `UpdateBindings`' own ~8% of the CP thread is mostly copying the used float constants one vec4 at a time with bit scans.
- **Lead:** (1) constant buffers as `UNIFORM_BUFFER_DYNAMIC` — one descriptor set per uniform-pool page, per-draw dynamic offsets, no allocate/update (needs the pool to keep the max binding range in bounds); (2) copy runs of consecutive float constants with one memcpy (as the WebGPU backend's `PackFloatConstants` does). Expected a few % of the CP thread each.
