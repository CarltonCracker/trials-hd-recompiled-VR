// rexgpu-webgpu - the WebGPU backend.

#include "webgpu_backend.h"

#include <mutex>

#include <rex/cvar.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/logging.h>
#include <rex/ui/presenter.h>

#include "webgpu_presenter.h"
#include "webgpu_provider.h"

REXCVAR_DEFINE_INT32(webgpu_trace_frame, -1, "GPU/WebGPU",
                     "Log every draw, resolve and swap the backend receives in this guest frame "
                     "(-1 = off)");

namespace rex::graphics::webgpu {

namespace {

uint64_t ShaderHash(const Shader* shader) {
  return shader ? shader->ucode_data_hash() : 0;
}

}  // namespace

bool WebGpuBackend::IsTracedFrame(uint32_t frame_index) const {
  int32_t traced = REXCVAR_GET(webgpu_trace_frame);
  return traced >= 0 && uint32_t(traced) == frame_index;
}

bool WebGpuBackend::Draw(const DrawSnapshot& draw) {
  if (IsTracedFrame(draw.frame_index)) {
    const RegisterFile& regs = *draw.regs;
    REXGPU_INFO(
        "webgpu trace: frame {} draw {}: prim {} count {} indexed {} (fmt {} endian {} base "
        "0x{:08X}) vs {:016X} ps {:016X} dirty 0x{:02X} surface_info 0x{:08X} color_info "
        "0x{:08X} depth_info 0x{:08X}",
        draw.frame_index, draw.draw_index, uint32_t(draw.primitive_type), draw.index_count,
        draw.indexed, uint32_t(draw.index_buffer.format), uint32_t(draw.index_buffer.endianness),
        draw.index_buffer.guest_base, ShaderHash(draw.vertex_shader),
        ShaderHash(draw.pixel_shader), draw.dirty, regs.values[XE_GPU_REG_RB_SURFACE_INFO],
        regs.values[XE_GPU_REG_RB_COLOR_INFO], regs.values[XE_GPU_REG_RB_DEPTH_INFO]);
  }
  // Rendering starts with milestone M3 (L-04*, L-06, L-08, L-11).
  return true;
}

bool WebGpuBackend::Resolve(const ResolveSnapshot& resolve) {
  if (IsTracedFrame(resolve.frame_index)) {
    const draw_util::ResolveInfo& info = resolve.info;
    REXGPU_INFO(
        "webgpu trace: frame {} resolve {}: copy_control 0x{:08X} dest_info 0x{:08X} dest_base "
        "0x{:08X} color_base {} depth_base {} height_div_8 {}",
        resolve.frame_index, resolve.resolve_index, info.rb_copy_control.value,
        info.copy_dest_info.value, info.copy_dest_base, info.color_original_base,
        info.depth_original_base, info.height_div_8);
  }
  // The resolve engine is L-09.
  return true;
}

void WebGpuBackend::Swap(const SwapSnapshot& swap) {
  if (IsTracedFrame(swap.frame_index)) {
    REXGPU_INFO("webgpu trace: frame {} swap: frontbuffer 0x{:08X} {}x{}, display {}x{}",
                swap.frame_index, swap.frontbuffer_ptr, swap.frontbuffer_width,
                swap.frontbuffer_height, swap.display_width, swap.display_height);
  }
  if (!presenter_) {
    return;
  }
  uint32_t output_width = swap.frontbuffer_width ? swap.frontbuffer_width : swap.display_width;
  uint32_t output_height =
      swap.frontbuffer_height ? swap.frontbuffer_height : swap.display_height;
  // Never hold the device mutex here: the presenter takes it itself, after
  // its own locks (lock order in webgpu_provider.h).
  presenter_->RefreshGuestOutput(
      output_width, output_height, swap.display_width, swap.display_height,
      [this](ui::Presenter::GuestOutputRefreshContext& context) -> bool {
        // Nothing is rendered yet (milestone M1): a solid color shows that the
        // presenter, letterboxing and captures work.
        auto& webgpu_context =
            static_cast<ui::webgpu::WebGpuPresenter::WebGpuGuestOutputRefreshContext&>(context);
        context.SetIs8bpc(true);
        std::lock_guard lock(provider_.device_mutex());
        wgpu::RenderPassColorAttachment color;
        color.view = webgpu_context.view();
        color.loadOp = wgpu::LoadOp::Clear;
        color.storeOp = wgpu::StoreOp::Store;
        color.clearValue = {0.10, 0.12, 0.18, 1.0};
        wgpu::RenderPassDescriptor pass_desc;
        pass_desc.label = "guest output (placeholder)";
        pass_desc.colorAttachmentCount = 1;
        pass_desc.colorAttachments = &color;
        wgpu::CommandEncoder encoder = provider_.device().CreateCommandEncoder();
        encoder.BeginRenderPass(&pass_desc).End();
        wgpu::CommandBuffer commands = encoder.Finish();
        provider_.queue().Submit(1, &commands);
        return true;
      });

  std::lock_guard lock(provider_.device_mutex());
  provider_.ProcessEvents();
}

}  // namespace rex::graphics::webgpu
