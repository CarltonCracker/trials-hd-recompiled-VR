// rexgpu-webgpu - command processor (PM4 front-end).

#include "webgpu_command_processor.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>

#include <rex/cvar.h>
#include <rex/graphics/graphics_system.h>
#include <rex/graphics/registers.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>

#include "pipeline_cache.h"
#include "webgpu_backend.h"
#include "webgpu_provider.h"

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

namespace {

std::atomic<uint32_t> g_draws_since_report{0};
std::atomic<uint32_t> g_copies_since_report{0};

constexpr uint32_t kContextFirst = 0x2000;
constexpr uint32_t kContextEnd = 0x4000;

// DrawDirty bits for each context register (0x2000-0x3FFF).
const std::array<uint8_t, kContextEnd - kContextFirst>& ContextRegisterDirtyBits() {
  static const auto table = [] {
    std::array<uint8_t, kContextEnd - kContextFirst> bits;
    bits.fill(uint8_t(kDirtyPipelineState));
    auto set = [&bits](uint32_t first, uint32_t last, uint32_t dirty) {
      for (uint32_t i = first; i <= last; ++i) {
        bits[i - kContextFirst] = uint8_t(dirty);
      }
    };
    set(XE_GPU_REG_RB_SURFACE_INFO, XE_GPU_REG_RB_COLOR3_INFO, kDirtyRenderTargets);
    set(XE_GPU_REG_RB_MODECONTROL, XE_GPU_REG_RB_MODECONTROL, kDirtyRenderTargets);
    set(XE_GPU_REG_PA_SC_SCREEN_SCISSOR_TL, XE_GPU_REG_PA_SC_SCREEN_SCISSOR_BR,
        kDirtyViewportScissor);
    set(XE_GPU_REG_PA_SC_WINDOW_OFFSET, XE_GPU_REG_PA_SC_WINDOW_SCISSOR_BR,
        kDirtyViewportScissor);
    set(XE_GPU_REG_PA_CL_VPORT_XSCALE, XE_GPU_REG_PA_CL_VPORT_ZOFFSET, kDirtyViewportScissor);
    set(XE_GPU_REG_PA_CL_VTE_CNTL, XE_GPU_REG_PA_CL_VTE_CNTL, kDirtyViewportScissor);
    return bits;
  }();
  return table;
}

uint32_t DirtyBitsForRange(uint32_t first, uint32_t count) {
  if (!count) {
    return 0;
  }
  const uint32_t last = first + count - 1;
  uint32_t dirty = 0;
  if (first < kContextEnd && last >= kContextFirst) {
    const auto& table = ContextRegisterDirtyBits();
    for (uint32_t i = std::max(first, kContextFirst), end = std::min(last, kContextEnd - 1);
         i <= end; ++i) {
      dirty |= table[i - kContextFirst];
    }
  }
  auto overlaps = [first, last](uint32_t range_first, uint32_t range_last) {
    return first <= range_last && last >= range_first;
  };
  if (overlaps(0x4000, 0x43FF)) {
    dirty |= kDirtyVsFloatConstants;
  }
  if (overlaps(0x4400, 0x47FF)) {
    dirty |= kDirtyPsFloatConstants;
  }
  if (overlaps(0x4800, 0x48BF)) {
    dirty |= kDirtyFetchConstants;
  }
  if (overlaps(0x4900, 0x4927)) {
    dirty |= kDirtyBoolLoopConstants;
  }
  return dirty;
}

}  // namespace

WebGpuCommandProcessor::WebGpuCommandProcessor(GraphicsSystem* graphics_system,
                                               system::KernelState* kernel_state)
    : CommandProcessor(graphics_system, kernel_state) {}

WebGpuCommandProcessor::~WebGpuCommandProcessor() = default;

bool WebGpuCommandProcessor::SetupContext() {
  provider_ = static_cast<ui::webgpu::WebGpuProvider*>(graphics_system_->provider());
  if (!provider_) {
    return false;
  }
  backend_ = std::make_unique<WebGpuBackend>(*provider_, graphics_system_->presenter());
  if (!backend_->Initialize(*memory_, *register_file_)) {
    REXGPU_ERROR("WebGPU: failed to initialize the backend");
    return false;
  }
  return true;
}

void WebGpuCommandProcessor::ShutdownContext() {
  backend_.reset();
}

void WebGpuCommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
  CommandProcessor::WriteRegister(index, value);
  pending_dirty_ |= DirtyBitsForRange(index, 1);
}

void WebGpuCommandProcessor::WriteRegistersFromMem(uint32_t start_index, uint32_t* base,
                                                   uint32_t num_registers) {
  // The base copies context-register runs in bulk without calling
  // WriteRegister, so mark the whole range here.
  CommandProcessor::WriteRegistersFromMem(start_index, base, num_registers);
  pending_dirty_ |= DirtyBitsForRange(start_index, num_registers);
}

Shader* WebGpuCommandProcessor::LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                                           const uint32_t* host_address, uint32_t dword_count) {
  pending_dirty_ |= kDirtyShaders;
  return backend_->pipeline_cache().LoadShader(shader_type, host_address, dword_count);
}

bool WebGpuCommandProcessor::IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                                       IndexBufferInfo* index_buffer_info,
                                       bool major_mode_explicit) {
  g_draws_since_report.fetch_add(1, std::memory_order_relaxed);
  // A resolve is a draw in EDRAM copy mode (as in the Vulkan backend, it also
  // counts as a draw in the log).
  if (register_file_->Get<reg::RB_MODECONTROL>().edram_mode == xenos::EdramMode::kCopy) {
    return IssueCopy();
  }
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
  draw.dirty = pending_dirty_;
  draw.frame_index = frame_index_;
  draw.draw_index = draw_index_++;
  pending_dirty_ = 0;
  return backend_->Draw(draw);
}

bool WebGpuCommandProcessor::IssueCopy() {
  g_copies_since_report.fetch_add(1, std::memory_order_relaxed);
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
  return backend_->Resolve(resolve);
}

void WebGpuCommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                       uint32_t frontbuffer_height) {
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
      swaps = 0;
      window_start = clock::now();
    }
  }

  SwapSnapshot swap;
  swap.frontbuffer_ptr = frontbuffer_ptr;
  swap.frontbuffer_width = frontbuffer_width;
  swap.frontbuffer_height = frontbuffer_height;
  swap.frame_index = frame_index_;
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
  backend_->Swap(swap);
}

}  // namespace rex::graphics::webgpu
