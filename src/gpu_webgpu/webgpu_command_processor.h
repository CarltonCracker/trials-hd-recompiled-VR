// rexgpu-webgpu - command processor: the PM4 front-end. The SDK base decodes
// the ring buffer; this class turns draws, resolves and swaps into snapshots
// (draw_snapshot.h) for the WebGPU backend, and tracks which state changed
// between draws.

#pragma once

#include <cstdint>
#include <memory>

#include <rex/graphics/command_processor.h>

#include "draw_snapshot.h"

namespace rex::ui::webgpu {
class WebGpuProvider;
}  // namespace rex::ui::webgpu

namespace rex::graphics::webgpu {

class WebGpuBackend;

class WebGpuCommandProcessor final : public CommandProcessor {
 public:
  WebGpuCommandProcessor(GraphicsSystem* graphics_system, system::KernelState* kernel_state);
  ~WebGpuCommandProcessor() override;

  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                 uint32_t frontbuffer_height) override;

 protected:
  bool SetupContext() override;
  void ShutdownContext() override;

  Shader* LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                     const uint32_t* host_address, uint32_t dword_count) override;

  bool IssueDraw(xenos::PrimitiveType prim_type, uint32_t index_count,
                 IndexBufferInfo* index_buffer_info, bool major_mode_explicit) override;
  bool IssueCopy() override;

 private:
  ui::webgpu::WebGpuProvider* provider_ = nullptr;
  std::unique_ptr<WebGpuBackend> backend_;

  uint32_t frame_index_ = 0;
  uint32_t draw_index_ = 0;
  uint32_t resolve_index_ = 0;
};

}  // namespace rex::graphics::webgpu
