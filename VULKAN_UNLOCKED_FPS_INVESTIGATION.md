# Trials HD Recompiled — Vulkan Renderer High-FPS Performance Investigation & Architectural Report

**Target Audience:** Advanced AI model / senior graphics engineer reviewing the Vulkan (`librexgpu-xenos`) performance bottleneck.  
**Date:** October 16, 2026  
**Repository:** `trials-hd-recompiled`  
**Status:** **Resolved (October 16, 2026)** — see "Resolution" below. The analysis in sections 3–6 was written before the cause was found; most of its hypotheses (queue submission count, SDL event hop, Wayland WSI, resolve passes) turned out not to be the cause.

---

## Resolution

**Root cause:** an upstream ReXGlue SDK bug in `VulkanPresenter::PaintAndPresentImpl` (`thirdparty/rexglue-sdk/src/ui/vulkan/vulkan_presenter.cpp`). Before drawing the guest output to the swapchain, the presenter checks whether the final-effect pipeline was created for the current swapchain format:

```cpp
if (swapchain_effect_pipeline.swapchain_pipeline != VK_NULL_HANDLE &&
    swapchain_effect_pipeline.swapchain_format != paint_context_.swapchain_render_pass_format) {
  paint_context_.submission_tracker.AwaitSubmissionCompletion(
      paint_context_.guest_output_image_paint_last_submission);
  // destroy the pipeline, then recreate it below
```

`swapchain_format` was never assigned after creating the pipeline (Xenia's original code does), so it stayed `VK_FORMAT_UNDEFINED` and the check fired on **every paint**. Each paint therefore blocked the UI thread until the *previous paint* finished on the GPU, which sits behind every guest frame queued since. Then it recreated the pipeline.

- With GPU headroom (120 FPS cap, ~70% busy), that wait is short, so nothing looks wrong.
- When GPU-bound, the wait is about two guest frames (≈11.7 ms at 2560x1440 on the RX 7800 XT). The presenter's mailbox shows only the newest guest output, so **every other rendered frame was dropped**: the guest rendered ~169 FPS (GPU 100% busy), but only ~85 FPS were presented. That is the 70–90 FPS MangoHud showed.
- WebGPU presents from its own backend and never ran this code.

How it was found: a headless Wayland compositor (`weston --backend=headless --renderer=gl`, 2560x1440 @ 240 Hz, real GPU) plus the virtual pad riding *Middle Name: Danger*, with per-stage timing of `PaintAndPresentImpl` and `eu-stack` samples of the UI thread. The samples showed it blocked in `vkWaitForFences` under `AwaitSubmissionCompletion`, and in one sample inside `CreateGuestOutputPaintPipeline` during gameplay.

**Fix** (in `patches/rexglue-sdk/trials-hd.patch`): record `swapchain_format` once the pipeline is created. Also:
- The presenter counts presented frames (`Presenter::presented_frame_count()`), and the `log_guest_fps` line now ends with `presented fps: N`. `tests/check_fps_log.py` fails if presented FPS falls below 95% of guest FPS.
- `present_fps_limit = 0` ("Unlimited" in the settings UI) is uncapped again (`src/fps_unlock.cpp`). An earlier attempt to fix this bug had silently capped it at the display refresh rate. VSync still caps at the refresh rate.

**Results** (riding, 2560x1440 swapchain, RX 7800 XT, IMMEDIATE mode, guest / presented FPS):

| Configuration | Before | After |
|---|---|---|
| Vulkan, 2x scale, unlimited | 169 / 85 | 168–170 / 168–170 |
| Vulkan, 2x scale, 165 cap | collapsed | 165 / 165 |
| Vulkan, 2x scale, 120 cap | 120 / 120 | 120 / 120 |
| Vulkan, 1x scale, unlimited | capped at 240 | 250–264 / 250–264 |
| WebGPU, 2x scale, unlimited | 216 | 214 |

**Still open (separate from this bug):** at 2x scale, Vulkan costs ~5.9 ms of GPU time per frame versus ~4.6 ms for WebGPU (both GPU-bound at 98–100%). That is a renderer-efficiency gap, not a pacing or presentation problem.

---

## 1. Executive Summary

When running Trials HD Recompiled on an **AMD Radeon RX 7800 XT** (Linux x86_64, Mesa RADV 26.2.2, Wayland / Hyprland, 240 Hz display at 2560x1440):

- **Locked at 120 FPS (`present_fps_limit = 120`):**
  - Rock-solid 120 FPS.
  - GPU usage is ~70–80%.
  - Frametimes are flat and smooth (~8.33 ms).
- **Locked at 144, 165, 240 FPS or Unlocked (`present_fps_limit = 0`):**
  - Framerate **collapses from 120 FPS down to 75–90 FPS**.
  - GPU usage spikes to **100%**.
  - Severe frametime jitter and stutter.
- **WebGPU Renderer (`--gpu_plugin=webgpu`) under identical conditions:**
  - Delivers **200+ FPS** (up to 240 FPS monitor limit) smoothly when unlocked.
  - Normal GPU utilization (~80–90%).

Initial attempts to address this via CPU frame pacing (`DisplayRefreshRate` in `src/fps_unlock.cpp`), ring buffer spin-wait tuning (`src/gpu_wait.cpp`), and disabling primary buffer submissions (`vulkan_submit_on_primary_buffer_end = false`) verified that the CPU pacing loop correctly commands 240 FPS (`FPS unlock: rendering at up to 240.00 FPS`), yet the actual GPU/presentation pipeline still throttles to 75–90 FPS at 100% GPU busy time.

This document details the root causes identified across the ReXGlue SDK Vulkan backend (`thirdparty/rexglue-sdk/src/graphics/vulkan/`), the Vulkan Presenter (`thirdparty/rexglue-sdk/src/ui/vulkan/`), and the UI thread presentation pipeline, explaining exactly why WebGPU succeeds and what concrete changes are required to fix the Vulkan renderer.

---

## 2. Hardware & Runtime Environment

| Component | Specification |
|---|---|
| **GPU** | AMD Radeon RX 7800 XT (Navi 32, 60 CUs, 3840 stream processors, 16 GB GDDR6) |
| **Driver** | Mesa RADV 26.2.2 (LLVM 22.1.8 / ACO) |
| **Vulkan API** | 1.4.341 / Conformance 1.4.5.3 |
| **OS / Window Manager** | Linux x86_64, Wayland (Hyprland compositor) |
| **Displays** | Dual 2560x1440 @ 239.97 Hz |
| **Resolution Scale** | `draw_resolution_scale_x = 2`, `draw_resolution_scale_y = 2` (2560x1440 internal framebuffer) |
| **Overlay / Profiling** | MangoHud, `log_guest_fps = true` |

---

## 3. Mathematical Analysis: Why 120 FPS Works but Higher Targets Collapse

At 120 FPS, the frame interval is:
$$\Delta t_{120} = \frac{1000 \text{ ms}}{120} \approx 8.33 \text{ ms}$$

The user observed **70%–80% GPU utilization** at 120 FPS. This implies the GPU workload per frame in Vulkan is:
$$T_{\text{GPU}} \approx 8.33 \text{ ms} \times 0.75 \approx 6.25 \text{ ms}$$

Theoretical maximum throughput based purely on GPU execution time:
$$\text{FPS}_{\text{max}} = \frac{1000 \text{ ms}}{6.25 \text{ ms}} \approx 160 \text{ FPS}$$

However, when setting the FPS limit to 144, 165, or 240 FPS:
- If this were simply a GPU bound workload, the framerate should smoothly cap at **~150–160 FPS** at 100% GPU utilization.
- Instead, the framerate **drops sharply to 75–90 FPS**, and GPU usage reports 100%.

This throughput collapse from 160 FPS down to 75–90 FPS is the hallmark of a **pipeline serialization convoy / producer-consumer lockup**:
1. When $\Delta t_{\text{target}} < T_{\text{GPU}}$ (e.g. 4.16 ms for 240 FPS vs 6.25 ms GPU execution), frames pile up in the graphics queue.
2. The Vulkan command processor enforces `kMaxFramesInFlight = 3`.
3. Frame $N$ blocks in `dfn.vkWaitForFences(device, ..., UINT64_MAX)` waiting for Frame $N-3$.
4. While the command processor thread is blocked in `vkWaitForFences`, the guest PPC thread continues generating draw commands until the 512 KB command ring buffer fills up.
5. Once the ring buffer is full, the guest thread blocks in `sub_8212BC68`.
6. When the GPU finally completes Frame $N-3$, the command processor thread wakes up, but the pipeline has stalled. The GPU starves for work while the CPU catches up, introducing pipeline bubbles.
7. In AMDGPU / RADV, when command queues alternate rapidly between fence stalls and bursts of multi-submission flushes, the GPU engines remain 100% busy in wait/synchronization states, and dynamic power management downclocks the GPU core frequency due to intermittent idle periods, exacerbating the problem.

In contrast, **WebGPU takes $\le 4.5$ ms per frame** on the exact same hardware, easily sustaining **200+ FPS**.

---

## 4. Key Architectural Findings: Vulkan vs. WebGPU

### Finding 1: 4 Separate `vkQueueSubmit` Calls Per Frame in Vulkan vs. 1 in WebGPU

In the WebGPU backend (`src/gpu_webgpu/webgpu_backend.cpp`):
- All draws, resolves, and presentation passes are recorded into command encoders.
- **Exactly 1 submission** is sent to the GPU queue per frame:
  ```cpp
  provider_.queue().Submit(1, &commands);
  wgpu_surface_.Present();
  ```
- No fence waits occur on the command processor thread.

In the Vulkan backend (`thirdparty/rexglue-sdk/src/graphics/vulkan/` & `src/ui/vulkan/`):
Each frame triggers **up to 4 independent queue submissions**:
1. **Primary Frame Submission:** `VulkanCommandProcessor::EndSubmission` in `command_processor.cpp`:
   ```cpp
   dfn.vkQueueSubmit(queue, 1, &submit_info, fence);
   ```
2. **Refresher Signal Submission:** `VulkanPresenter::RefreshGuestOutputImpl` in `vulkan_presenter.cpp` (lines 1114–1118):
   ```cpp
   // Submits an empty batch (0 command buffers) purely to advance a fence!
   dfn.vkQueueSubmit(queue_acquisition.queue(), 0, nullptr, fence_acqusition.fence());
   ```
3. **Presenter Swapchain Paint Submission:** `VulkanPresenter::PaintAndPresentImpl` in `vulkan_presenter.cpp` (lines 2213–2216):
   ```cpp
   dfn.vkQueueSubmit(queue_acquisition.queue(), 1, &submit_info, fence_acqusition.fence());
   ```
4. **UI Fence Signal Submission:** `VulkanPresenter::PaintAndPresentImpl` in `vulkan_presenter.cpp` (lines 2218–2222):
   ```cpp
   // Another empty submission (0 command buffers) for UI fence synchronization!
   dfn.vkQueueSubmit(queue_acquisition.queue(), 0, nullptr, ui_fence_acquisition.fence());
   ```

On AMD RADV, 4 queue submissions per frame at 144+ FPS generates **over 600 `vkQueueSubmit` calls and kernel ioctls per second**. Each submission incurs command processor synchronization, cache flushes, and kernel scheduler roundtrips.

---

### Finding 2: UI-Thread Hop via SDL Event Queue (`PaintMode::kUIThreadOnRequest`)

In `thirdparty/rexglue-sdk/src/ui/presenter.cpp`:
```cpp
Presenter::PaintMode Presenter::GetDesiredPaintModeFromUIThread(bool is_paintable) const {
  ...
  if (!ui_drawers_.empty()) {
    // The UI can be drawn only by the UI thread, and it needs to be drawn -
    // paint in the UI thread.
    return PaintMode::kUIThreadOnRequest;
  }
  return PaintMode::kGuestOutputThreadImmediately;
}
```
- Because ImGui (`imgui_drawer.cpp`) is registered, `ui_drawers_.empty()` is `false`.
- Therefore, `paint_mode_` is **always `PaintMode::kUIThreadOnRequest`**.
- In `Presenter::RefreshGuestOutput`:
  ```cpp
  case PaintMode::kUIThreadOnRequest:
    RequestPaintOrConnectionRecoveryViaWindow(true);
    break;
  ```
- This calls `window_->RequestPaint()`, which executes:
  ```cpp
  SDL_Event event{};
  event.type = sdl_app_context().paint_event_type();
  event.user.windowID = sdl_window_id_;
  SDL_PushEvent(&event);
  ```
- **Consequence:** The actual presentation (`vkAcquireNextImageKHR`, paint pass, and `vkQueuePresentKHR`) does NOT happen on the render thread. It must wait for the SDL event loop on the main thread to dispatch the custom user event.
- At 60 or 120 FPS, the SDL event loop latency is absorbed by the frame period. At 144–240 FPS (frame times of 4.16–6.94 ms), the SDL event pump round-trip introduces severe latency jitter, delaying image acquisition and stalling `RefreshGuestOutput`.

---

### Finding 3: Swapchain Acquisition & Wayland WSI Presentation Mode

In `thirdparty/rexglue-sdk/src/ui/vulkan/vulkan_presenter.cpp`:
```cpp
dfn.vkAcquireNextImageKHR(device, paint_context_.swapchain, UINT64_MAX, acquire_semaphore,
                          VK_NULL_HANDLE, &swapchain_image_index);
```
- Swapchain images are acquired with `UINT64_MAX` (blocking timeout).
- Presentation mode negotiation (lines 1398–1404) selects:
  `swapchain_create_info.presentMode = VK_PRESENT_MODE_IMMEDIATE_KHR;` (Logged as `presentation mode 0`).
- **Wayland Complication:**
  - On Wayland (Hyprland / wlroots), tearing is disallowed by default unless the client specifically negotiates the Wayland tearing protocol (`wp_tearing_control_v1`).
  - Without tearing protocol integration in the window subsystem, Mesa RADV's Wayland WSI implements `VK_PRESENT_MODE_IMMEDIATE_KHR` by queueing or dropping buffers, or falling back to mailbox behavior with internal frame pacing.
  - If the compositor holds onto swapchain images for compositing, `vkAcquireNextImageKHR` blocks the UI thread until a buffer is released, backpressuring the entire pipeline.

---

### Finding 4: EDRAM Resolve Downscaling & Memory Readback Overhead

In Trials HD, auto-exposure (luminance adaptation) performs **~8 small resolves per frame** (8x8 to 64x64):
- In WebGPU, small resolves are read back using a lightweight CPU staging path or cached resolve engine.
- In Vulkan (`VulkanCommandProcessor::IssueCopy_ReadbackResolvePath` in `command_processor.cpp`, lines 4568–4718):
  - When `draw_resolution_scale_x = 2` (2x scaling):
    - Binds `resolve_downscale_pipeline_`.
    - Allocates a transient descriptor set via `AllocateSingleTransientDescriptor`.
    - Updates descriptor sets with `dfn.vkUpdateDescriptorSets`.
    - Emits pre-barrier `CmdVkPipelineBarrier(TRANSFER | COMPUTE -> COMPUTE)`.
    - Dispatches compute: `CmdVkDispatch(tile_count, 1, 1)`.
    - Emits post-barrier `CmdVkPipelineBarrier(COMPUTE -> TRANSFER)`.
    - Copies buffer: `CmdVkCopyBuffer(resolve_downscale_buffer_, readback.buffers[write_index])`.
    - Emits buffer memory barrier `PushBufferMemoryBarrier(TRANSFER -> HOST)`.
    - Invalidates mapped memory: `dfn.vkInvalidateMappedMemoryRanges`.
    - Performs CPU `std::memcpy` into guest memory.
- Executing this compute dispatch + barrier sequence 8 times per frame adds significant GPU execution time and CPU overhead.

---

### Finding 5: Transient Descriptor Allocation on Every Draw

In `VulkanCommandProcessor::WriteTransientTextureBindings` (`command_processor.cpp`, lines 6830–6880):
- For draw calls with varying texture/sampler layouts, descriptor sets are allocated from `transient_descriptor_allocator_textures_`.
- On every draw, `dfn.vkUpdateDescriptorSets` is called with new image/sampler descriptors.
- In WebGPU, `GetOrCreateTextureBindGroup` caches bind groups across draw calls in a flat hash map (`texture_bind_groups_`), avoiding per-draw API descriptor writes.

---

## 5. Review of Previous Fix Attempts

The following changes were implemented and verified during earlier sessions:

1. **Display Refresh Rate Frame Pacing (`src/graphics_settings.cpp`, `src/fps_unlock.cpp`):**
   - *Change:* Implemented `trials_hd::DisplayRefreshRate()` to query `SDL_GetDesktopDisplayMode` (returning 239.97 Hz). Configured `sub_82664F98` to target 239.97 FPS when `present_fps_limit = 0`.
   - *Result:* Headless log confirmed: `FPS unlock: rendering at up to 239.97 FPS`. Headless throughput reached 186.2 FPS.
   - *Why it didn't fix the user's issue:* When presenting to a real Wayland surface, the bottleneck is on the GPU/WSI presentation side, not the guest tick clock. Pacing the CPU to 240 FPS when the Vulkan pipeline can only deliver 75–90 FPS does not resolve the GPU/queue bottleneck.

2. **Single-Submission Batching Flag (`src/trials_hd_app.h`):**
   - *Change:* Set `vulkan_submit_on_primary_buffer_end = false` to batch primary buffers until `IssueSwap`.
   - *Result:* Reduced mid-frame submissions from guest primary buffers, but the 3 additional submissions in `RefreshGuestOutputImpl` and `PaintAndPresentImpl` remained.

3. **Progressive Spin-Yield Backoff (`src/gpu_wait.cpp`):**
   - *Change:* Replaced immediate `sleep_for(20us)` after 512 spins with 256 pauses, 4096 yields, and 65536 spin-yields.
   - *Result:* Prevented premature 50–100 microsecond kernel sleep deschedules during transient waits, but did not eliminate the underlying fence stall when all 3 frames in flight are saturated.

4. **Guarded Redundant Submission in `IssueSwap` (`command_processor.cpp`):**
   - *Change:* Added `if (frame_open_)` guard around trailing `EndSubmission(true)`.

---

## 6. Actionable Fix Proposals for the Reviewing Model

### Priority 1: Eliminate Redundant Queue Submissions in Vulkan Presenter
**File:** [`thirdparty/rexglue-sdk/src/ui/vulkan/vulkan_presenter.cpp`](file:///home/braden/Documents/Programming/CPP/trials-hd-recompiled/thirdparty/rexglue-sdk/src/ui/vulkan/vulkan_presenter.cpp)

1. **Remove Empty Fence Signal Submissions:**
   - In `RefreshGuestOutputImpl` (lines 1110–1118): The refresher lambda has already closed the frame and submitted via `EndSubmission(true)`. Submitting a second empty `vkQueueSubmit` just to signal `fence_acqusition.fence()` adds unnecessary driver overhead. Attach the fence to the actual frame submission instead of performing a standalone submission.
   - In `PaintAndPresentImpl` (lines 2217–2222): Do not submit a separate empty batch for `ui_fence_acquisition`. Combine the UI fence into `submit_info` or track completion via the main paint fence.

### Priority 2: Bypass UI Thread SDL Event Dispatch for Direct Presentation
**Files:** [`thirdparty/rexglue-sdk/src/ui/presenter.cpp`](file:///home/braden/Documents/Programming/CPP/trials-hd-recompiled/thirdparty/rexglue-sdk/src/ui/presenter.cpp), [`thirdparty/rexglue-sdk/src/ui/vulkan/vulkan_presenter.cpp`](file:///home/braden/Documents/Programming/CPP/trials-hd-recompiled/thirdparty/rexglue-sdk/src/ui/vulkan/vulkan_presenter.cpp)

- In `Presenter::GetDesiredPaintModeFromUIThread`:
  When ImGui is idle (no active dialogs or overlays open, or `imgui_drawer->WantsContinuousPaint()` is false), allow `PaintMode::kGuestOutputThreadImmediately`.
- Alternatively, execute `PaintAndPresent(false)` directly on the guest output / command processor thread when `host_present_from_non_ui_thread` is enabled, eliminating the `SDL_PushEvent` round-trip through the UI thread message pump.

### Priority 3: Increase Flight Depth or Implement Non-Blocking Frame Retirement
**File:** [`thirdparty/rexglue-sdk/src/graphics/vulkan/command_processor.cpp`](file:///home/braden/Documents/Programming/CPP/trials-hd-recompiled/thirdparty/rexglue-sdk/src/graphics/vulkan/command_processor.cpp)

- Currently, `kMaxFramesInFlight = 3`.
- In `BeginSubmission`:
  ```cpp
  uint64_t await_submission =
      is_opening_frame ? closed_frame_submissions_[frame_current_ % kMaxFramesInFlight] : 0;
  CheckSubmissionFenceAndDeviceLoss(await_submission);
  ```
  If the GPU is slightly behind, the command processor thread blocks unconditionally with `UINT64_MAX` timeout in `vkWaitForFences`.
- Evaluate increasing `kMaxFramesInFlight` to 4 (matching modern high-refresh swapchains), or implementing non-blocking fence polling before falling back to sleep/wait, allowing the ring buffer to absorb brief bursts without stalling.

### Priority 4: Test Presentation Mode Override (`vulkan_allow_present_mode_mailbox`)
**File:** Configuration / CVar

- In `vulkan_presenter.cpp`:
  `vulkan_allow_present_mode_immediate` (mode 0) is preferred over `vulkan_allow_present_mode_mailbox` (mode 1).
- On Wayland/Hyprland, test launching with:
  ```bash
  ./run.sh --gpu_plugin=xenos --vulkan_allow_present_mode_immediate=false --vulkan_allow_present_mode_mailbox=true
  ```
  `VK_PRESENT_MODE_MAILBOX_KHR` is natively supported by Wayland compositors (it drops old unpresented frames without tearing or blocking), whereas `IMMEDIATE` without `wp_tearing_control_v1` can cause severe compositor queue contention.

### Priority 5: Optimize Resolve Downscale Passes
**File:** [`thirdparty/rexglue-sdk/src/graphics/vulkan/command_processor.cpp`](file:///home/braden/Documents/Programming/CPP/trials-hd-recompiled/thirdparty/rexglue-sdk/src/graphics/vulkan/command_processor.cpp)

- In `IssueCopy_ReadbackResolvePath`:
  Filter out non-luminance small resolves or bypass the compute downscaling shader when 1x resolves are requested, matching the optimizations implemented in `src/gpu_webgpu/resolve_engine.cpp`.

---

## 7. Relevant Code Index

| Component | File | Key Functions / Locations |
|---|---|---|
| **Vulkan Command Processor** | `thirdparty/rexglue-sdk/src/graphics/vulkan/command_processor.cpp` | `IssueSwap` (L2320–L2988), `IssueCopy_ReadbackResolvePath` (L4482–L4773), `BeginSubmission` / `EndSubmission` (L5180–L5550), `CheckSubmissionFenceAndDeviceLoss` (L5029–L5100) |
| **Vulkan Presenter** | `thirdparty/rexglue-sdk/src/ui/vulkan/vulkan_presenter.cpp` | `RefreshGuestOutputImpl` (L1061–L1120), `PaintAndPresentImpl` (L2140–L2287), Swapchain Creation & Present Mode (L1308–L1435) |
| **Common Presenter** | `thirdparty/rexglue-sdk/src/ui/presenter.cpp` | `RefreshGuestOutput` (L549–L650), `GetDesiredPaintModeFromUIThread` (L1306–L1330) |
| **SDL Window Paint Pump** | `thirdparty/rexglue-sdk/src/ui/window_sdl.cpp` | `RequestPaint` / `HandlePaintEvent` (L430–L441) |
| **WebGPU Reference Backend** | `src/gpu_webgpu/webgpu_backend.cpp`, `webgpu_presenter.cpp` | `WebGpuBackend::Swap` (L1340+), `WebGpuPresenter::PaintAndPresentImpl` (L294–L388) |
| **Guest Game Loop & Pacing** | `src/fps_unlock.cpp` | `sub_82664F98` (L380–L460), `AccurateWaitUntil` (L350–L375) |
| **GPU Ring Buffer Wait Hook** | `src/gpu_wait.cpp` | `sub_8212BC68` (L45–L63) |
| **SDK Patch File** | `patches/rexglue-sdk/trials-hd.patch` | Unified patch containing all local SDK modifications |
