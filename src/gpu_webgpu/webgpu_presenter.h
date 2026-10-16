// rexgpu-webgpu - presenter: guest output images, the window surface,
// letterboxing and the UI overlay (rex::ui::Presenter contract).
//
// WebGPU has one queue and reference-counted resources, so unlike the Vulkan
// presenter no fences or submission tracking are needed: work submitted by
// the guest output refresher is ordered before any later paint, and textures
// stay alive until the GPU is done with them.

#pragma once

#include <array>
#include <cstdint>
#include <memory>

#include <webgpu/webgpu_cpp.h>

#include <rex/ui/presenter.h>

namespace rex::ui::webgpu {

class WebGpuProvider;

// The render pass the UI drawers record into while painting.
class WebGpuUIDrawContext final : public UIDrawContext {
 public:
  WebGpuUIDrawContext(Presenter& presenter, uint32_t render_target_width,
                      uint32_t render_target_height, const wgpu::RenderPassEncoder& pass,
                      wgpu::TextureFormat render_target_format, uint64_t paint_serial)
      : UIDrawContext(presenter, render_target_width, render_target_height),
        pass_(pass),
        render_target_format_(render_target_format),
        paint_serial_(paint_serial) {}

  const wgpu::RenderPassEncoder& pass() const { return pass_; }
  wgpu::TextureFormat render_target_format() const { return render_target_format_; }
  // Distinct for every paint; lets the immediate drawer reuse its buffers.
  uint64_t paint_serial() const { return paint_serial_; }

 private:
  const wgpu::RenderPassEncoder& pass_;
  wgpu::TextureFormat render_target_format_;
  uint64_t paint_serial_;
};

class WebGpuPresenter final : public Presenter {
 public:
  // Usable as a render attachment, a texture binding and a copy source.
  static constexpr wgpu::TextureFormat kGuestOutputFormat = wgpu::TextureFormat::RGBA8Unorm;

  class WebGpuGuestOutputRefreshContext final : public GuestOutputRefreshContext {
   public:
    WebGpuGuestOutputRefreshContext(bool& is_8bpc_out_ref, const wgpu::Texture& texture,
                                    const wgpu::TextureView& view)
        : GuestOutputRefreshContext(is_8bpc_out_ref), texture_(texture), view_(view) {}
    // kGuestOutputFormat, sized as requested by the refresher.
    const wgpu::Texture& texture() const { return texture_; }
    const wgpu::TextureView& view() const { return view_; }

   private:
    const wgpu::Texture& texture_;
    const wgpu::TextureView& view_;
  };

  static std::unique_ptr<WebGpuPresenter> Create(HostGpuLossCallback host_gpu_loss_callback,
                                                 WebGpuProvider& provider);
  ~WebGpuPresenter() override;

  WebGpuProvider& provider() const { return provider_; }

#if defined(__EMSCRIPTEN__)
  // Web: the page's canvas, owned by the GPU worker (docs/web-port 7.9.7).
  static constexpr const char* kCanvasSelector = "#canvas";
  // Paints the newest guest output to the canvas from the GPU worker,
  // configuring the canvas surface on first use or after a resize. The
  // browser shows it when the worker next returns to its event loop.
  bool PaintToCanvas();
#endif

  Surface::TypeFlags GetSupportedSurfaceTypes() const override;
  bool CaptureGuestOutput(RawImage& image_out) override;

 protected:
  SurfacePaintConnectResult ConnectOrReconnectPaintingToSurfaceFromUIThread(
      Surface& new_surface, uint32_t new_surface_width, uint32_t new_surface_height,
      bool was_paintable, bool& is_vsync_implicit_out) override;
  void DisconnectPaintingFromSurfaceFromUIThreadImpl() override;
  bool RefreshGuestOutputImpl(uint32_t mailbox_index, uint32_t frontbuffer_width,
                              uint32_t frontbuffer_height,
                              std::function<bool(GuestOutputRefreshContext& context)> refresher,
                              bool& is_8bpc_out_ref) override;
  PaintResult PaintAndPresentImpl(bool execute_ui_drawers) override;

 private:
  struct GuestOutputImage {
    wgpu::Texture texture;
    wgpu::TextureView view;
    uint32_t width = 0;
    uint32_t height = 0;
  };

  WebGpuPresenter(HostGpuLossCallback host_gpu_loss_callback, WebGpuProvider& provider)
      : Presenter(host_gpu_loss_callback), provider_(provider) {}
  bool InitializeSurfaceIndependent();
  wgpu::RenderPipeline GetGuestOutputPipeline(wgpu::TextureFormat format);

  WebGpuProvider& provider_;

  std::array<GuestOutputImage, kGuestOutputMailboxSize> guest_output_images_;

  wgpu::Sampler linear_clamp_sampler_;
  wgpu::BindGroupLayout guest_output_bind_group_layout_;
  wgpu::PipelineLayout guest_output_pipeline_layout_;
  wgpu::ShaderModule guest_output_shader_;
  wgpu::RenderPipeline guest_output_pipeline_;
  wgpu::TextureFormat guest_output_pipeline_format_ = wgpu::TextureFormat::Undefined;

  // Painting connection (UI thread, or the guest output thread when it paints).
  Surface* connected_surface_ = nullptr;
  wgpu::Surface wgpu_surface_;
  wgpu::SurfaceConfiguration surface_config_;
  uint64_t paint_serial_ = 0;
};

}  // namespace rex::ui::webgpu
