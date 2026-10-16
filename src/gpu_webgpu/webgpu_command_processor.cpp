// rexgpu-webgpu - command processor (PM4 front-end).

#include "webgpu_command_processor.h"

#include <algorithm>
#include <atomic>
#include <chrono>

#include <rex/cvar.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/registers.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>

#include "pipeline_cache.h"
#include "web_phase.h"
#include "webgpu_backend.h"
#include "webgpu_provider.h"

#if defined(__EMSCRIPTEN__)
#include <rex/thread/web.h>

#include "webgpu_graphics_system.h"
#include "webgpu_presenter.h"
#endif

// Same names as the Vulkan backend's: the app and the test scripts set them
// by name (docs/webgpu-renderer/README.md, 7.6.2).
REXCVAR_DEFINE_BOOL(log_guest_fps, false, "GPU",
                    "Log guest frames per second, draws and resolves per frame once a second");
REXCVAR_DEFINE_BOOL(present_fps_limit_guest_paced, false, "GPU",
                    "The host game loop already paces real frames to present_fps_limit");
REXCVAR_DEFINE_INT32(present_fps_limit, 0, "GPU",
                     "Maximum guest frames presented per second (0 = unlimited). Host-side "
                     "only: the guest keeps running at its own rate")
    .range(0, 1000)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Defined with the SDK's texture cache base (pipeline/texture/cache.cpp).
REXCVAR_DECLARE(int32_t, draw_resolution_scale_x);
REXCVAR_DECLARE(int32_t, draw_resolution_scale_y);

namespace rex::graphics::webgpu {

#if defined(__EMSCRIPTEN__)
std::atomic<const char*> g_web_phase{"setup"};
#endif

namespace {

std::atomic<uint32_t> g_draws_since_report{0};
std::atomic<uint32_t> g_copies_since_report{0};

#if defined(__EMSCRIPTEN__)
std::atomic<uint64_t> g_web_swaps{0};
std::atomic<uint64_t> g_web_draws{0};
// GPU worker time, for the frame-rate log: idle (ring empty) and returning
// to the browser's event loop after each swap.
using WebClock = std::chrono::steady_clock;
WebClock::duration g_web_idle_time{};
WebClock::duration g_web_yield_time{};
WebClock::time_point g_web_idle_start{};
#endif

}  // namespace

WebGpuCommandProcessor::WebGpuCommandProcessor(GraphicsSystem* graphics_system,
                                               system::KernelState* kernel_state)
    : CommandProcessor(graphics_system, kernel_state) {}

WebGpuCommandProcessor::~WebGpuCommandProcessor() = default;

bool WebGpuCommandProcessor::SetupContext() {
#if defined(__EMSCRIPTEN__)
  // This thread is the GPU worker: every WebGPU object lives here (7.9.5).
  if (!static_cast<WebGpuGraphicsSystem*>(graphics_system_)->InitializeOnGpuWorker()) {
    return false;
  }
#endif
  provider_ = static_cast<ui::webgpu::WebGpuProvider*>(graphics_system_->provider());
  if (!provider_) {
    return false;
  }
  backend_ = std::make_unique<WebGpuBackend>(*provider_, graphics_system_->presenter());
  if (!backend_->Initialize(*memory_, *register_file_)) {
    REXGPU_ERROR("WebGPU: failed to initialize the backend");
    return false;
  }
#if defined(__EMSCRIPTEN__)
  SetWebPhase("command processor");
  stall_watchdog_ = std::jthread([this](std::stop_token stop) { StallWatchdog(stop); });
#endif
  return true;
}

#if defined(__EMSCRIPTEN__)
void WebGpuCommandProcessor::PrepareForWait() {
  SetWebPhase(read_ptr_index_ == write_ptr_index_.load(std::memory_order_relaxed)
                  ? "idle (ring empty)"
                  : "WAIT_REG_MEM");
  g_web_idle_start = WebClock::now();
  CommandProcessor::PrepareForWait();
}

void WebGpuCommandProcessor::ReturnFromWait() {
  CommandProcessor::ReturnFromWait();
  g_web_idle_time += WebClock::now() - g_web_idle_start;
  SetWebPhase("command processor");
}

void WebGpuCommandProcessor::StallWatchdog(std::stop_token stop) {
  using clock = std::chrono::steady_clock;
  uint64_t last_swaps = g_web_swaps.load(std::memory_order_relaxed);
  clock::time_point last_change = clock::now();
  clock::time_point last_report{};
  while (!stop.stop_requested()) {
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    const clock::time_point now = clock::now();
    const uint64_t swaps = g_web_swaps.load(std::memory_order_relaxed);
    if (swaps != last_swaps) {
      last_swaps = swaps;
      last_change = now;
      continue;
    }
    if (now - last_change >= std::chrono::seconds(3) &&
        now - last_report >= std::chrono::seconds(5)) {
      last_report = now;
      REXGPU_WARN(
          "web: no guest swap for {:.1f} s; GPU worker: {}; ring read {} write {}; draws {}",
          std::chrono::duration<double>(now - last_change).count(),
          g_web_phase.load(std::memory_order_relaxed), read_ptr_index_,
          write_ptr_index_.load(std::memory_order_relaxed),
          g_web_draws.load(std::memory_order_relaxed));
    }
  }
}
#endif

void WebGpuCommandProcessor::ShutdownContext() {
#if defined(__EMSCRIPTEN__)
  stall_watchdog_ = {};
#endif
  backend_.reset();
#if defined(__EMSCRIPTEN__)
  provider_ = nullptr;
  static_cast<WebGpuGraphicsSystem*>(graphics_system_)->ShutdownOnGpuWorker();
#endif
}

Shader* WebGpuCommandProcessor::LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                                           const uint32_t* host_address, uint32_t dword_count) {
  return backend_->pipeline_cache().LoadShader(shader_type, host_address, dword_count);
}

bool WebGpuCommandProcessor::IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                                       IndexBufferInfo* index_buffer_info,
                                       bool major_mode_explicit) {
  g_draws_since_report.fetch_add(1, std::memory_order_relaxed);
#if defined(__EMSCRIPTEN__)
  // CPU writes since the last delivery reach the caches watching them before
  // this draw reads guest memory, as the write faults would natively (7.9.3).
  memory_->DeliverPhysicalWrites();
#endif
#if defined(__EMSCRIPTEN__)
  g_web_draws.fetch_add(1, std::memory_order_relaxed);
#endif
  SetWebPhase("draw");
  DrawSnapshot draw;
  draw.regs = register_file_;
  draw.primitive_type = prim_type;
  draw.index_count = index_count;
  draw.major_mode_explicit = major_mode_explicit;
  if (index_buffer_info) {
    draw.indexed = true;
    draw.index_buffer.format = index_buffer_info->format;
    draw.index_buffer.endianness = index_buffer_info->endianness;
    draw.index_buffer.count = index_buffer_info->count;
    draw.index_buffer.guest_base = index_buffer_info->guest_base;
    draw.index_buffer.length_bytes = uint32_t(index_buffer_info->length);
  }
  draw.vertex_shader = active_vertex_shader();
  draw.pixel_shader = active_pixel_shader();
  // Not tracked (per-register bookkeeping cost ~4% of the CP thread and no
  // consumer uses it yet).
  draw.dirty = kDirtyAll;
  draw.frame_index = frame_index_;
  draw.draw_index = draw_index_++;

  // A resolve is a draw in EDRAM copy mode (as in the Vulkan backend, it also
  // counts as a draw in the log).
  if (register_file_->Get<reg::RB_MODECONTROL>().edram_mode == xenos::EdramMode::kCopy) {
    backend_->Draw(draw);
    return IssueCopy();
  }
  bool result = backend_->Draw(draw);
  SetWebPhase("command processor");
  return result;
}

bool WebGpuCommandProcessor::IssueCopy() {
  g_copies_since_report.fetch_add(1, std::memory_order_relaxed);
#if defined(__EMSCRIPTEN__)
  memory_->DeliverPhysicalWrites();
#endif
  SetWebPhase("resolve");
  ResolveSnapshot resolve;
  resolve.regs = register_file_;
  resolve.frame_index = frame_index_;
  resolve.resolve_index = resolve_index_++;
  // Full-range 16-bit fixed-point render targets (section 9.2), so not
  // truncated to -1...1.
  if (!draw_util::GetResolveInfo(*register_file_, *memory_,
                                 uint32_t(std::clamp(REXCVAR_GET(draw_resolution_scale_x), 1, 3)),
                                 uint32_t(std::clamp(REXCVAR_GET(draw_resolution_scale_y), 1, 3)),
                                 false, false, resolve.info)) {
    return true;  // Dropped, as in the other backends.
  }
  bool result = backend_->Resolve(resolve);
  SetWebPhase("command processor");
  return result;
}

void WebGpuCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                       uint32_t frontbuffer_height) {
#if defined(__EMSCRIPTEN__)
  memory_->DeliverPhysicalWrites();  // A CPU-written front buffer.
#endif
  if (REXCVAR_GET(log_guest_fps)) {
    // Same line format as the Vulkan backend (tests/check_fps_log.py).
    using clock = std::chrono::steady_clock;
    static clock::time_point window_start = clock::now();
    static uint32_t swaps = 0;
    ++swaps;
    const auto elapsed = clock::now() - window_start;
    if (elapsed >= std::chrono::seconds(1)) {
      const uint32_t draws = g_draws_since_report.exchange(0);
      const uint32_t copies = g_copies_since_report.exchange(0);
      static uint64_t last_submissions = 0;
      uint64_t submissions = backend_->submission_count();
      REXGPU_INFO(
          "guest fps: {:.1f} draws/frame: {:.1f} resolves/frame: {:.1f} skipped draws/frame: "
          "{:.1f} submissions/frame: {:.1f}",
          swaps / std::chrono::duration<double>(elapsed).count(), double(draws) / swaps,
          double(copies) / swaps, double(backend_->TakeSkippedDrawCount()) / swaps,
          double(submissions - last_submissions) / swaps);
      last_submissions = submissions;
#if defined(__EMSCRIPTEN__)
      // Web: first-use shader/pipeline creation is the usual cause of slow
      // seconds (no persistent pipeline cache yet, W-S16).
      if (auto stats = backend_->pipeline_cache().TakeCreationStats();
          stats.modules || stats.pipelines) {
        REXGPU_INFO("new shader modules: {} pipelines: {} ({:.1f} ms)", stats.modules,
                    stats.pipelines, stats.milliseconds);
      }
      const double window_ms = std::chrono::duration<double, std::milli>(elapsed).count();
      REXGPU_INFO("GPU worker: busy {:.0f}%, idle {:.0f}%, yielding {:.0f}%",
                  100.0 * (1.0 - std::chrono::duration<double, std::milli>(
                                     g_web_idle_time + g_web_yield_time).count() / window_ms),
                  100.0 * std::chrono::duration<double, std::milli>(g_web_idle_time).count() /
                      window_ms,
                  100.0 * std::chrono::duration<double, std::milli>(g_web_yield_time).count() /
                      window_ms);
      g_web_idle_time = {};
      g_web_yield_time = {};
#endif
      swaps = 0;
      window_start = clock::now();
    }
  }

  SwapSnapshot swap;
  swap.frontbuffer_ptr = frontbuffer_ptr;
  swap.frontbuffer_width = frontbuffer_width;
  swap.frontbuffer_height = frontbuffer_height;
  swap.frame_index = frame_index_;
  swap.gamma_ramp_table = reinterpret_cast<const uint32_t*>(gamma_ramp_256_entry_table());
  swap.gamma_ramp_pwl = reinterpret_cast<const uint32_t*>(gamma_ramp_pwl_rgb());
  swap.post_effect = uint32_t(GetActualSwapPostEffect());
  ++frame_index_;
  draw_index_ = 0;
  resolve_index_ = 0;

  // Host presentation cap, as in the Vulkan backend: skipping a present does
  // not change guest timing.
  if (int32_t fps_limit = REXCVAR_GET(present_fps_limit);
      fps_limit > 0 && !REXCVAR_GET(present_fps_limit_guest_paced)) {
    using clock = std::chrono::steady_clock;
    static clock::time_point next_present{};
    const auto period = std::chrono::nanoseconds(1'000'000'000 / fps_limit);
    const auto now = clock::now();
    if (now < next_present) {
      return;
    }
    next_present = (now - next_present > period) ? now + period : next_present + period;
  }

  system::X_VIDEO_MODE video_mode;
  kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
  swap.display_width = std::max(uint32_t(1), uint32_t(video_mode.display_width));
  swap.display_height = std::max(uint32_t(1), uint32_t(video_mode.display_height));
  SetWebPhase("swap");
  backend_->Swap(swap);
#if defined(__EMSCRIPTEN__)
  // Draw the frame to the canvas and return to the event loop once, which is
  // when the browser shows it (and runs pending WebGPU callbacks).
  if (auto* presenter =
          static_cast<ui::webgpu::WebGpuPresenter*>(graphics_system_->presenter())) {
    SetWebPhase("paint");
    presenter->PaintToCanvas();
  }
  SetWebPhase("yield to the event loop");
  const WebClock::time_point yield_start = WebClock::now();
  ::rex::thread::web::YieldToEventLoop();
  g_web_yield_time += WebClock::now() - yield_start;
  SetWebPhase("command processor");
  g_web_swaps.fetch_add(1, std::memory_order_relaxed);
#endif
}

}  // namespace rex::graphics::webgpu
