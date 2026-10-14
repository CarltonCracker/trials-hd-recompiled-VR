// rexgpu-webgpu - desktop shared-memory mirror. Guest bytes stay big-endian.
#include "guest_memory_mirror.h"

#include <memory>
#include <string>
#include <utility>

#include <rex/logging.h>
#include <rex/system/xmemory.h>

namespace rex::graphics::webgpu {

WebGpuSharedMemory::WebGpuSharedMemory(memory::Memory& memory, const wgpu::Instance& instance,
                                     const wgpu::Device& device,
                                     std::function<bool()> submit_pending)
    : SharedMemory(memory), instance_(instance), device_(device), queue_(device.GetQueue()),
      submit_pending_(std::move(submit_pending)) {}

WebGpuSharedMemory::~WebGpuSharedMemory() {
  // Cancel guest watches before releasing GPU resources. ShutdownCommon is
  // idempotent (the base destructor also calls it).
  ShutdownCommon();
}

bool WebGpuSharedMemory::Initialize() {
  if (buffer_) {
    return true;
  }
  wgpu::Limits limits;
  if (!submit_pending_ || device_.GetLimits(&limits) != wgpu::Status::Success ||
      limits.maxBufferSize < kBufferSize || limits.maxStorageBufferBindingSize < kBufferSize) {
    // shortcut: require desktop 512 MiB limits; add windowed bindings for mobile/browser bring-up.
    REXGPU_ERROR("WebGPU shared memory requires 512 MiB buffer and storage-binding limits");
    return false;
  }

  device_.PushErrorScope(wgpu::ErrorFilter::Validation);
  device_.PushErrorScope(wgpu::ErrorFilter::OutOfMemory);
  device_.PushErrorScope(wgpu::ErrorFilter::Internal);
  wgpu::BufferDescriptor desc;
  desc.label = "guest physical memory (512 MiB)";
  desc.size = kBufferSize;
  desc.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::Index |
               wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::CopySrc;
  wgpu::Buffer buffer = device_.CreateBuffer(&desc);
  auto failed = std::make_shared<bool>(false);
  for (unsigned i = 0; i < 3; ++i) {
    auto future = device_.PopErrorScope(
        wgpu::CallbackMode::WaitAnyOnly,
        [failed](wgpu::PopErrorScopeStatus status, wgpu::ErrorType type, wgpu::StringView message) {
          if (status != wgpu::PopErrorScopeStatus::Success || type != wgpu::ErrorType::NoError) {
            *failed = true;
            REXGPU_ERROR("WebGPU shared-memory allocation failed: {}",
                         message.data ? std::string(message.data, message.length == WGPU_STRLEN
                                                                    ? std::char_traits<char>::length(message.data)
                                                                    : message.length)
                                      : std::string{});
          }
        });
    if (instance_.WaitAny(future, UINT64_MAX) != wgpu::WaitStatus::Success) {
      *failed = true;
    }
  }
  if (*failed || !buffer) {
    return false;
  }
  buffer_ = std::move(buffer);
  InitializeCommon();
  return true;
}

bool WebGpuSharedMemory::UploadRanges(
    const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) {
  if (upload_page_ranges.empty()) {
    return true;
  }
  if (!buffer_ || !submit_pending_()) {
    return false;
  }
  for (const auto& [first_page, page_count] : upload_page_ranges) {
    const uint32_t start = first_page << page_size_log2();
    const uint32_t length = page_count << page_size_log2();
    // Protect BEFORE Dawn copies the source, so a concurrent guest write
    // invalidates these pages instead of being hidden by a later valid mark.
    MakeRangeValid(start, length, false);
    queue_.WriteBuffer(buffer_, start, memory().TranslatePhysical(start), length);
  }
  return true;
}

}  // namespace rex::graphics::webgpu
