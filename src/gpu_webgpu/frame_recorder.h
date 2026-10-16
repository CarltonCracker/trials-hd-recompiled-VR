// rexgpu-webgpu - frame recorder: the command encoder of the current
// submission, lazily opened render passes, and the uniform ring that carries
// per-draw constants (group 0 with dynamic offsets, section 7.6.5).
//
// Uniform data is staged on the CPU and written to the ring with one
// Queue::WriteBuffer right before the submission that uses it, so the ring can
// be reused from the start by every submission (queue operations are ordered).

#pragma once

#include <array>
#include <cstdint>
#include <string_view>
#include <vector>

#include <webgpu/webgpu_cpp.h>

namespace rex::ui::webgpu {
class WebGpuProvider;
}  // namespace rex::ui::webgpu

namespace rex::graphics::webgpu {

struct RenderPassTargets {
  std::array<wgpu::TextureView, 4> color;
  wgpu::TextureView depth_stencil;
  bool depth_has_stencil = false;
  // Clear instead of load (the texture's contents are undefined).
  uint32_t color_clear_mask = 0;
  bool depth_clear = false;

  bool SameAttachments(const RenderPassTargets& other) const {
    for (size_t i = 0; i < color.size(); ++i) {
      if (color[i].Get() != other.color[i].Get()) {
        return false;
      }
    }
    return depth_stencil.Get() == other.depth_stencil.Get();
  }
};

class FrameRecorder {
 public:
  static constexpr uint64_t kUniformRingSize = 8 << 20;
  static constexpr uint64_t kIndexRingSize = 8 << 20;
  // Largest single uniform binding window (float constants: 256 vec4).
  static constexpr uint32_t kMaxUniformBindingSize = 4096;
  static constexpr uint32_t kUniformAlignment = 256;

  explicit FrameRecorder(ui::webgpu::WebGpuProvider& provider) : provider_(provider) {}
  ~FrameRecorder();

  bool Initialize();

  const wgpu::Buffer& uniform_ring() const { return uniform_ring_; }
  const wgpu::Buffer& index_ring() const { return index_ring_; }

  // Space for converted indices of the current submission: returns a CPU
  // pointer to fill (valid until the next allocation) and the byte offset in
  // index_ring(). Submits first if the ring is full. Null if too large.
  void* AllocateIndices(uint32_t size, uint32_t alignment, uint32_t& offset_out);

  // Copies data into the ring (zero-padded to the alignment) and returns its
  // offset. Submits first if the ring is full, which ends the render pass.
  uint32_t UploadUniforms(const void* data, uint32_t size);
  // Compares uniform data previously uploaded at `offset` with `data`.
  bool CompareUniforms(uint32_t offset, const void* data, uint32_t size) const {
    if (uint64_t(offset) + size > uniform_staging_.size()) {
      return false;
    }
    return std::memcmp(uniform_staging_.data() + offset, data, size) == 0;
  }
  // Submits now if fewer than `size` bytes (plus alignment) are left, so the
  // following uploads of one draw never cause a submission.
  void ReserveUniforms(uint32_t size);

  // The encoder of the current submission (ends no pass).
  const wgpu::CommandEncoder& encoder();

  // Returns the open render pass for the targets, beginning a new one if the
  // attachments differ. If a new pass begins, it is labeled with `label` and
  // a debug group is pushed (popped automatically when the pass ends).
  const wgpu::RenderPassEncoder& BeginRenderPass(
      const RenderPassTargets& targets, std::string_view label = {},
      const wgpu::PassTimestampWrites* timestamp_writes = nullptr);
  void EndRenderPass();
  bool in_render_pass() const { return bool(pass_); }
  const RenderPassTargets& current_targets() const { return pass_targets_; }
  // Changes whenever a new render pass begins (dynamic state is per pass).
  uint64_t pass_serial() const { return pass_serial_; }

  // Ends the pass, writes the uniform ring and submits everything recorded.
  bool Submit();
  uint64_t submission_count() const { return submission_count_; }

 private:
  ui::webgpu::WebGpuProvider& provider_;
  wgpu::Buffer uniform_ring_;
  std::vector<uint8_t> uniform_staging_;
  wgpu::Buffer index_ring_;
  std::vector<uint8_t> index_staging_;
  wgpu::CommandEncoder encoder_;
  wgpu::RenderPassEncoder pass_;
  RenderPassTargets pass_targets_;
  bool has_debug_group_ = false;
  uint64_t pass_serial_ = 0;
  uint64_t submission_count_ = 0;
};

}  // namespace rex::graphics::webgpu
