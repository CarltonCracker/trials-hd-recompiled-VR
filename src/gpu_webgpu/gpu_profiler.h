// rexgpu-webgpu - GPU timestamp profiler (section 8.12, task S-28).
// Measures GPU execution time per pass category (shadow, scene, post, resolve, UI)
// using WebGPU timestamp queries, and reports per-category milliseconds once per second.

#pragma once

#include <array>
#include <chrono>
#include <cstdint>
#include <vector>

#include <webgpu/webgpu_cpp.h>

namespace rex::ui::webgpu {
class WebGpuProvider;
}

namespace rex::graphics::webgpu {

enum class PassCategory : uint32_t {
  kShadow = 0,
  kScene,
  kPost,
  kResolve,
  kUI,
  kTransfer,   // EDRAM ownership transfers
  kMemExport,  // memory export compute
  kCount
};

const char* PassCategoryName(PassCategory category);

class GpuProfiler {
 public:
  static constexpr uint32_t kMaxQueriesPerFrame = 512;
  static constexpr uint32_t kRingSlots = 8;

  explicit GpuProfiler(ui::webgpu::WebGpuProvider& provider);
  ~GpuProfiler();

  bool Initialize();

  // Whether profiling is enabled and supported by the active WebGPU adapter.
  bool is_enabled() const;

  // Prepares timestamp writes for the beginning and end of a pass in `category`.
  // Returns true and populates `writes_out` if a query pair is available.
  bool BeginPass(PassCategory category, wgpu::PassTimestampWrites& writes_out);

  // Resolves queries into the resolve buffer for the current frame.
  void EndFrame(const wgpu::CommandEncoder& encoder);

  // Called after the command encoder is submitted to initiate async map readback.
  void OnFrameSubmitted();

 private:
  struct PassInterval {
    PassCategory category;
    uint32_t begin_query;
    uint32_t end_query;
  };

  struct FrameSlot {
    wgpu::Buffer readback_buffer;
    std::vector<PassInterval> intervals;
    uint32_t query_count = 0;
    bool in_flight = false;
  };

  void ProcessReadback(uint32_t slot_index);

  ui::webgpu::WebGpuProvider& provider_;
  bool supported_ = false;

  wgpu::QuerySet query_set_;
  wgpu::Buffer resolve_buffer_;
  std::array<FrameSlot, kRingSlots> slots_;
  uint32_t current_slot_ = 0;
  uint32_t submitted_slot_ = UINT32_MAX;

  // Accumulated profiling statistics over the reporting window (~1 second).
  std::array<uint64_t, static_cast<size_t>(PassCategory::kCount)> category_ns_ = {};
  uint64_t total_ns_ = 0;
  uint32_t frames_counted_ = 0;
  std::chrono::steady_clock::time_point last_report_time_;
};

}  // namespace rex::graphics::webgpu
