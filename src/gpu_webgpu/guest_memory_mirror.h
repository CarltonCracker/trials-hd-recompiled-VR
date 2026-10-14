// rexgpu-webgpu - desktop guest memory, using the SDK's page watches (7.6.3).
#pragma once

#include <functional>

#include <webgpu/webgpu_cpp.h>
#include <rex/graphics/shared_memory.h>

namespace rex::graphics::webgpu {

class WebGpuSharedMemory final : public SharedMemory {
 public:
  // All calls, including destruction and inherited RequestRange(s), run on
  // the CP thread under the provider's device mutex. Memory must outlive us.
  // submit_pending must end/submit earlier encoders before a dirty upload:
  // WriteBuffer is queue work, not a copy inside the current command encoder.
  WebGpuSharedMemory(memory::Memory& memory, const wgpu::Instance& instance,
                     const wgpu::Device& device, std::function<bool()> submit_pending);
  ~WebGpuSharedMemory() override;

  bool Initialize();
  const wgpu::Buffer& buffer() const { return buffer_; }

 protected:
  bool UploadRanges(
      const std::vector<std::pair<uint32_t, uint32_t>>& upload_page_ranges) override;

 private:
  wgpu::Instance instance_;
  wgpu::Device device_;
  wgpu::Queue queue_;
  wgpu::Buffer buffer_;
  std::function<bool()> submit_pending_;
};

}  // namespace rex::graphics::webgpu
