// rexgpu-webgpu - desktop guest memory, using the SDK's page watches (7.6.3).
#pragma once

#include <functional>

#include <webgpu/webgpu_cpp.h>
#include <rex/graphics/shared_memory.h>

namespace rex::graphics::webgpu {

struct GuestMemoryUpload {
  void* data = nullptr;  // CPU space valid until the next allocation.
  wgpu::Buffer source;
  uint64_t offset = 0;
  wgpu::CommandEncoder encoder;
};

class WebGpuSharedMemory final : public SharedMemory {
 public:
  // All calls, including destruction and inherited RequestRange(s), run on
  // the CP thread under the provider's device mutex. Memory must outlive us.
  // allocate_upload reserves unique source bytes, ends any render pass and
  // returns the encoder. Ring bytes must be uploaded before its submission.
  WebGpuSharedMemory(memory::Memory& memory, const wgpu::Instance& instance,
                     const wgpu::Device& device,
                     std::function<GuestMemoryUpload(uint64_t)> allocate_upload);
  ~WebGpuSharedMemory() override;

  bool Initialize();
  const wgpu::Buffer& buffer() const { return buffer_; }

 protected:
  bool UploadRanges(
      const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) override;

 private:
  wgpu::Instance instance_;
  wgpu::Device device_;
  wgpu::Buffer buffer_;
  std::function<GuestMemoryUpload(uint64_t)> allocate_upload_;
};

}  // namespace rex::graphics::webgpu
