// rexgpu-webgpu - desktop shared-memory mirror. Guest bytes stay big-endian.
#include "guest_memory_mirror.h"

#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include <rex/logging.h>
#include <rex/system/xmemory.h>

namespace rex::graphics::webgpu {

WebGpuSharedMemory::WebGpuSharedMemory(memory::Memory& memory, const wgpu::Instance& instance,
                                     const wgpu::Device& device,
                                     std::function<GuestMemoryUpload(uint64_t)> allocate_upload)
    : SharedMemory(memory), instance_(instance), device_(device),
      allocate_upload_(std::move(allocate_upload)) {}

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
  if (!allocate_upload_ || device_.GetLimits(&limits) != wgpu::Status::Success ||
      limits.maxBufferSize < kBufferSize || limits.maxStorageBufferBindingSize < kBufferSize) {
    // shortcut: require desktop 512 MiB limits; add windowed bindings for mobile/browser bring-up.
    REXGPU_ERROR("WebGPU shared memory requires 512 MiB buffer and storage-binding limits");
    return false;
  }

  device_.PushErrorScope(wgpu::ErrorFilter::Validation);
  device_.PushErrorScope(wgpu::ErrorFilter::OutOfMemory);
  device_.PushErrorScope(wgpu::ErrorFilter::Internal);
  wgpu::BufferDescriptor desc{};
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
            const std::string text =
                !message.data ? std::string{} :
                message.length == WGPU_STRLEN ? std::string(message.data) :
                                               std::string(message.data, message.length);
            REXGPU_ERROR("WebGPU shared-memory allocation failed: {}", text);
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
  if (!buffer_) {
    return false;
  }
  uint64_t size = 0;
  for (const auto& [first_page, page_count] : upload_page_ranges) {
    size += uint64_t(page_count) << page_size_log2();
  }
  auto upload = allocate_upload_(size);
  if (!upload.data || !upload.source || !upload.encoder) {
    REXGPU_ERROR("WebGPU shared-memory upload allocation failed ({} bytes)", size);
    return false;
  }
  auto* data = static_cast<uint8_t*>(upload.data);
  uint64_t offset = 0;
  for (const auto& [first_page, page_count] : upload_page_ranges) {
    const uint32_t start = first_page << page_size_log2();
    const uint32_t length = page_count << page_size_log2();
    // Protect BEFORE copying, so concurrent guest writes invalidate the pages.
    MakeRangeValid(start, length, false);
    std::memcpy(data + offset, memory().TranslatePhysical(start), length);
    offset += length;
  }
  if (upload.source.GetMapState() == wgpu::BufferMapState::Mapped) {
    upload.source.Unmap();
  }
  offset = upload.offset;
  for (const auto& [first_page, page_count] : upload_page_ranges) {
    const uint32_t start = first_page << page_size_log2();
    const uint32_t length = page_count << page_size_log2();
    upload.encoder.CopyBufferToBuffer(upload.source, offset, buffer_, start, length);
    offset += length;
  }
  // Encoded commands retain the source; its slices remain unique until submission.
  return true;
}

}  // namespace rex::graphics::webgpu
