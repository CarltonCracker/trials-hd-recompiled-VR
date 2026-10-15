// rexgpu-webgpu - GPU timestamp profiler (section 8.12, task S-28).

#include "gpu_profiler.h"

#include <algorithm>
#include <cstring>
#include <mutex>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>

#include "webgpu_provider.h"

REXCVAR_DEFINE_BOOL(webgpu_profile, false, "GPU/WebGPU",
                    "Print GPU timestamp profiling per pass category once per second");

namespace rex::graphics::webgpu {

const char* PassCategoryName(PassCategory category) {
  switch (category) {
    case PassCategory::kShadow:
      return "shadow";
    case PassCategory::kScene:
      return "scene";
    case PassCategory::kPost:
      return "post";
    case PassCategory::kResolve:
      return "resolve";
    case PassCategory::kUI:
      return "UI";
    default:
      return "other";
  }
}

GpuProfiler::GpuProfiler(ui::webgpu::WebGpuProvider& provider) : provider_(provider) {}

GpuProfiler::~GpuProfiler() {
  std::lock_guard lock(provider_.device_mutex());
  for (auto& slot : slots_) {
    slot.readback_buffer = nullptr;
    slot.intervals.clear();
    slot.query_count = 0;
    slot.in_flight = false;
  }
  resolve_buffer_ = nullptr;
  query_set_ = nullptr;
}

bool GpuProfiler::Initialize() {
  supported_ = provider_.HasFeature(wgpu::FeatureName::TimestampQuery);
  if (!supported_) {
    REXGPU_WARN("WebGPU: adapter does not support timestamp-query; GPU profiling disabled");
    return false;
  }

  std::lock_guard lock(provider_.device_mutex());
  wgpu::QuerySetDescriptor qdesc;
  qdesc.label = "gpu profiler timestamps";
  qdesc.type = wgpu::QueryType::Timestamp;
  qdesc.count = kMaxQueriesPerFrame;
  query_set_ = provider_.device().CreateQuerySet(&qdesc);
  if (!query_set_) {
    REXGPU_WARN("WebGPU: failed to create timestamp QuerySet; GPU profiling disabled");
    supported_ = false;
    return false;
  }

  wgpu::BufferDescriptor resolve_desc;
  resolve_desc.label = "gpu profiler timestamp resolve";
  resolve_desc.size = kMaxQueriesPerFrame * sizeof(uint64_t);
  resolve_desc.usage = wgpu::BufferUsage::QueryResolve | wgpu::BufferUsage::CopySrc;
  resolve_buffer_ = provider_.device().CreateBuffer(&resolve_desc);
  if (!resolve_buffer_) {
    query_set_ = nullptr;
    supported_ = false;
    return false;
  }

  wgpu::BufferDescriptor readback_desc;
  readback_desc.label = "gpu profiler timestamp readback";
  readback_desc.size = kMaxQueriesPerFrame * sizeof(uint64_t);
  readback_desc.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
  for (size_t i = 0; i < slots_.size(); ++i) {
    slots_[i].readback_buffer = provider_.device().CreateBuffer(&readback_desc);
    slots_[i].intervals.reserve(64);
    if (!slots_[i].readback_buffer) {
      query_set_ = nullptr;
      resolve_buffer_ = nullptr;
      supported_ = false;
      return false;
    }
  }

  last_report_time_ = std::chrono::steady_clock::now();
  return true;
}

bool GpuProfiler::is_enabled() const {
  if (!supported_) {
    return false;
  }
  if (REXCVAR_GET(webgpu_profile)) {
    return true;
  }
  std::string legacy = rex::cvar::GetFlagByName("gpu_profile");
  return legacy == "true" || legacy == "1";
}

bool GpuProfiler::BeginPass(PassCategory category, wgpu::PassTimestampWrites& writes_out) {
  if (!is_enabled()) {
    return false;
  }
  if (slots_[current_slot_].in_flight) {
    for (size_t i = 1; i < kRingSlots; ++i) {
      uint32_t candidate = (current_slot_ + i) % kRingSlots;
      if (!slots_[candidate].in_flight) {
        current_slot_ = candidate;
        break;
      }
    }
  }
  FrameSlot& slot = slots_[current_slot_];
  if (slot.in_flight || slot.query_count + 2 > kMaxQueriesPerFrame) {
    return false;
  }
  uint32_t begin_query = slot.query_count++;
  uint32_t end_query = slot.query_count++;
  slot.intervals.push_back({category, begin_query, end_query});

  writes_out.querySet = query_set_;
  writes_out.beginningOfPassWriteIndex = begin_query;
  writes_out.endOfPassWriteIndex = end_query;
  return true;
}

void GpuProfiler::EndFrame(const wgpu::CommandEncoder& encoder) {
  if (!is_enabled()) {
    return;
  }
  FrameSlot& slot = slots_[current_slot_];
  if (slot.query_count == 0 || slot.in_flight) {
    slot.query_count = 0;
    slot.intervals.clear();
    return;
  }

  uint64_t bytes = uint64_t(slot.query_count) * sizeof(uint64_t);
  encoder.ResolveQuerySet(query_set_, 0, slot.query_count, resolve_buffer_, 0);
  encoder.CopyBufferToBuffer(resolve_buffer_, 0, slot.readback_buffer, 0, bytes);
  slot.in_flight = true;
  submitted_slot_ = current_slot_;
}

void GpuProfiler::OnFrameSubmitted() {
  if (!is_enabled() || submitted_slot_ == UINT32_MAX) {
    return;
  }
  uint32_t slot_idx = submitted_slot_;
  submitted_slot_ = UINT32_MAX;
  FrameSlot& slot = slots_[slot_idx];
  uint64_t bytes = uint64_t(slot.query_count) * sizeof(uint64_t);
  slot.readback_buffer.MapAsync(
      wgpu::MapMode::Read, 0, bytes, wgpu::CallbackMode::AllowSpontaneous,
      [this, slot_idx](wgpu::MapAsyncStatus status, wgpu::StringView message) {
        if (status == wgpu::MapAsyncStatus::Success) {
          ProcessReadback(slot_idx);
        } else {
          slots_[slot_idx].in_flight = false;
          slots_[slot_idx].query_count = 0;
          slots_[slot_idx].intervals.clear();
        }
      });

  current_slot_ = (current_slot_ + 1) % kRingSlots;
  if (slots_[current_slot_].in_flight) {
    for (size_t i = 1; i < kRingSlots; ++i) {
      uint32_t candidate = (current_slot_ + i) % kRingSlots;
      if (!slots_[candidate].in_flight) {
        current_slot_ = candidate;
        break;
      }
    }
  }
}

void GpuProfiler::ProcessReadback(uint32_t slot_index) {
  FrameSlot& slot = slots_[slot_index];
  uint64_t bytes = uint64_t(slot.query_count) * sizeof(uint64_t);
  const void* mapped = slot.readback_buffer.GetConstMappedRange(0, bytes);
  if (mapped) {
    const uint64_t* timestamps = static_cast<const uint64_t*>(mapped);
    uint64_t frame_total_ns = 0;
    for (const auto& interval : slot.intervals) {
      if (interval.begin_query < slot.query_count && interval.end_query < slot.query_count) {
        uint64_t t0 = timestamps[interval.begin_query];
        uint64_t t1 = timestamps[interval.end_query];
        if (t1 >= t0) {
          uint64_t diff = t1 - t0;
          category_ns_[static_cast<size_t>(interval.category)] += diff;
          frame_total_ns += diff;
        }
      }
    }
    total_ns_ += frame_total_ns;
    ++frames_counted_;
    slot.readback_buffer.Unmap();
  }

  slot.in_flight = false;
  slot.query_count = 0;
  slot.intervals.clear();

  auto now = std::chrono::steady_clock::now();
  auto elapsed_ms =
      std::chrono::duration_cast<std::chrono::milliseconds>(now - last_report_time_).count();
  if (elapsed_ms >= 1000 && frames_counted_ > 0) {
    double frames = double(frames_counted_);
    double total_ms = (double(total_ns_) / 1e6) / frames;
    auto cat_ms = [&](PassCategory cat) {
      return (double(category_ns_[static_cast<size_t>(cat)]) / 1e6) / frames;
    };
    REXGPU_INFO(
        "gpu {:.2f} ms/frame (shadow {:.2f}, scene {:.2f}, post {:.2f}, resolve {:.2f}, UI {:.2f}, {} frames)",
        total_ms, cat_ms(PassCategory::kShadow), cat_ms(PassCategory::kScene),
        cat_ms(PassCategory::kPost), cat_ms(PassCategory::kResolve), cat_ms(PassCategory::kUI),
        frames_counted_);

    total_ns_ = 0;
    std::fill(category_ns_.begin(), category_ns_.end(), 0);
    frames_counted_ = 0;
    last_report_time_ = now;
  }
}

}  // namespace rex::graphics::webgpu
