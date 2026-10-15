// rexgpu-webgpu - presenter.

#include "webgpu_presenter.h"

#include <algorithm>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/surface.h>
#if defined(__APPLE__)
#include <rex/ui/surface_mac.h>
#else
#include <rex/ui/surface_gnulinux.h>
#endif

#include "webgpu_provider.h"
#include "wgsl/guest_output.wgsl.h"
#if !defined(__APPLE__)
#include "x11_display.h"
#endif

REXCVAR_DEFINE_BOOL(webgpu_allow_present_mode_immediate, true, "UI/WebGPU",
                    "Allow the immediate present mode (no vsync, may tear)");
REXCVAR_DEFINE_BOOL(webgpu_headless, false, "UI/WebGPU",
                    "Do not present to the window (headless tests and benchmarks; guest output "
                    "captures still work), like vulkan_headless_surface")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(webgpu_allow_present_mode_mailbox, true, "UI/WebGPU",
                    "Allow the mailbox present mode (no vsync, no tearing)");

namespace rex::ui::webgpu {

namespace {

bool IsPreferredSurfaceFormat(wgpu::TextureFormat format) {
  // Not sRGB: the guest output and ImGui colors are already gamma-encoded.
  return format == wgpu::TextureFormat::BGRA8Unorm || format == wgpu::TextureFormat::RGBA8Unorm;
}

}  // namespace

std::unique_ptr<WebGpuPresenter> WebGpuPresenter::Create(
    HostGpuLossCallback host_gpu_loss_callback, WebGpuProvider& provider) {
  std::unique_ptr<WebGpuPresenter> presenter(
      new WebGpuPresenter(host_gpu_loss_callback, provider));
  if (!presenter->InitializeSurfaceIndependent()) {
    return nullptr;
  }
  return presenter;
}

WebGpuPresenter::~WebGpuPresenter() {
  std::lock_guard lock(provider_.device_mutex());
  if (wgpu_surface_) {
    wgpu_surface_.Unconfigure();
    wgpu_surface_ = nullptr;
  }
}

bool WebGpuPresenter::InitializeSurfaceIndependent() {
  {
    std::lock_guard lock(provider_.device_mutex());
    const wgpu::Device& device = provider_.device();

    wgpu::SamplerDescriptor sampler_desc;
    sampler_desc.label = "guest output (linear clamp)";
    sampler_desc.magFilter = wgpu::FilterMode::Linear;
    sampler_desc.minFilter = wgpu::FilterMode::Linear;
    linear_clamp_sampler_ = device.CreateSampler(&sampler_desc);

    std::array<wgpu::BindGroupLayoutEntry, 2> entries;
    entries[0].binding = 0;
    entries[0].visibility = wgpu::ShaderStage::Fragment;
    entries[0].texture.sampleType = wgpu::TextureSampleType::Float;
    entries[0].texture.viewDimension = wgpu::TextureViewDimension::e2D;
    entries[1].binding = 1;
    entries[1].visibility = wgpu::ShaderStage::Fragment;
    entries[1].sampler.type = wgpu::SamplerBindingType::Filtering;
    wgpu::BindGroupLayoutDescriptor bgl_desc;
    bgl_desc.label = "guest output";
    bgl_desc.entryCount = entries.size();
    bgl_desc.entries = entries.data();
    guest_output_bind_group_layout_ = device.CreateBindGroupLayout(&bgl_desc);

    wgpu::PipelineLayoutDescriptor layout_desc;
    layout_desc.label = "guest output";
    layout_desc.bindGroupLayoutCount = 1;
    layout_desc.bindGroupLayouts = &guest_output_bind_group_layout_;
    guest_output_pipeline_layout_ = device.CreatePipelineLayout(&layout_desc);

    wgpu::ShaderSourceWGSL wgsl;
    wgsl.code = kWgsl_guest_output;
    wgpu::ShaderModuleDescriptor module_desc;
    module_desc.nextInChain = &wgsl;
    module_desc.label = "guest_output.wgsl";
    guest_output_shader_ = device.CreateShaderModule(&module_desc);
  }
  return InitializeCommonSurfaceIndependent();
}

wgpu::RenderPipeline WebGpuPresenter::GetGuestOutputPipeline(wgpu::TextureFormat format) {
  if (guest_output_pipeline_ && guest_output_pipeline_format_ == format) {
    return guest_output_pipeline_;
  }
  wgpu::ColorTargetState target;
  target.format = format;
  wgpu::FragmentState fragment;
  fragment.module = guest_output_shader_;
  fragment.entryPoint = "fs_main";
  fragment.targetCount = 1;
  fragment.targets = &target;
  wgpu::RenderPipelineDescriptor desc;
  desc.label = "guest output";
  desc.layout = guest_output_pipeline_layout_;
  desc.vertex.module = guest_output_shader_;
  desc.vertex.entryPoint = "vs_main";
  desc.fragment = &fragment;
  guest_output_pipeline_ = provider_.device().CreateRenderPipeline(&desc);
  guest_output_pipeline_format_ = format;
  return guest_output_pipeline_;
}

Surface::TypeFlags WebGpuPresenter::GetSupportedSurfaceTypes() const {
  if (!provider_.with_presentation() || REXCVAR_GET(webgpu_headless)) {
    return 0;
  }
#if defined(__APPLE__)
  return Surface::kTypeFlag_CAMetalLayer;
#else
  return Surface::kTypeFlag_XcbWindow | Surface::kTypeFlag_WaylandSurface;
#endif
}

Presenter::SurfacePaintConnectResult
WebGpuPresenter::ConnectOrReconnectPaintingToSurfaceFromUIThread(
    Surface& new_surface, uint32_t new_surface_width, uint32_t new_surface_height,
    bool was_paintable, bool& is_vsync_implicit_out) {
  std::lock_guard lock(provider_.device_mutex());
  const wgpu::Device& device = provider_.device();

  if (connected_surface_ != &new_surface || !wgpu_surface_) {
    if (wgpu_surface_) {
      wgpu_surface_.Unconfigure();
      wgpu_surface_ = nullptr;
    }
    connected_surface_ = nullptr;
    wgpu::SurfaceDescriptor surface_desc;
    surface_desc.label = "window";
#if defined(__APPLE__)
    wgpu::SurfaceSourceMetalLayer metal_source;
#else
    wgpu::SurfaceSourceXlibWindow xlib_source;
    wgpu::SurfaceSourceWaylandSurface wayland_source;
#endif
    switch (new_surface.GetType()) {
#if defined(__APPLE__)
      case Surface::kTypeIndex_CAMetalLayer:
        metal_source.layer =
            const_cast<void*>(static_cast<CAMetalLayerSurface&>(new_surface).layer());
        surface_desc.nextInChain = &metal_source;
        break;
#else
      case Surface::kTypeIndex_XcbWindow: {
        // Dawn implements X11 surfaces through Xlib only (x11_display.cpp).
        xlib_source.display = GetXlibDisplay();
        if (!xlib_source.display) {
          REXLOG_ERROR("WebGpuPresenter: can't open an Xlib connection to the X server");
          return SurfacePaintConnectResult::kFailureSurfaceUnusable;
        }
        xlib_source.window = static_cast<XcbWindowSurface&>(new_surface).window();
        surface_desc.nextInChain = &xlib_source;
        break;
      }
      case Surface::kTypeIndex_WaylandSurface: {
        auto& wayland = static_cast<WaylandSurface&>(new_surface);
        wayland_source.display = wayland.display();
        wayland_source.surface = wayland.surface();
        surface_desc.nextInChain = &wayland_source;
        break;
      }
#endif
      default:
        REXLOG_ERROR("WebGpuPresenter: unsupported surface type {}", int(new_surface.GetType()));
        return SurfacePaintConnectResult::kFailureSurfaceUnusable;
    }
    wgpu_surface_ = provider_.instance().CreateSurface(&surface_desc);
    if (!wgpu_surface_) {
      REXLOG_ERROR("WebGpuPresenter: failed to create the WebGPU surface");
      return SurfacePaintConnectResult::kFailureSurfaceUnusable;
    }
  }

  wgpu::SurfaceCapabilities caps;
  wgpu::Status caps_status = wgpu_surface_.GetCapabilities(provider_.adapter(), &caps);
  if (caps_status != wgpu::Status::Success || !caps.formatCount) {
    REXLOG_ERROR(
        "WebGpuPresenter: the adapter can't present to this surface (type {}, status {}, {} "
        "formats)",
        int(new_surface.GetType()), uint32_t(caps_status), caps.formatCount);
    wgpu_surface_ = nullptr;
    return SurfacePaintConnectResult::kFailureSurfaceUnusable;
  }
  wgpu::TextureFormat format = caps.formats[0];
  for (size_t i = 0; i < caps.formatCount; ++i) {
    if (IsPreferredSurfaceFormat(caps.formats[i])) {
      format = caps.formats[i];
      break;
    }
  }
  auto supports = [&caps](wgpu::PresentMode mode) {
    return std::find(caps.presentModes, caps.presentModes + caps.presentModeCount, mode) !=
           caps.presentModes + caps.presentModeCount;
  };
  // As in the Vulkan presenter: fastest acquisition first, pacing is done by
  // the command processor (present_fps_limit) and the FPS unlock.
  wgpu::PresentMode present_mode = wgpu::PresentMode::Fifo;
  if (REXCVAR_GET(webgpu_allow_present_mode_immediate) && supports(wgpu::PresentMode::Immediate)) {
    present_mode = wgpu::PresentMode::Immediate;
  } else if (REXCVAR_GET(webgpu_allow_present_mode_mailbox) &&
             supports(wgpu::PresentMode::Mailbox)) {
    present_mode = wgpu::PresentMode::Mailbox;
  }

  wgpu::SurfaceConfiguration config;
  config.device = device;
  config.format = format;
  config.usage = wgpu::TextureUsage::RenderAttachment;
  config.width = new_surface_width;
  config.height = new_surface_height;
  config.presentMode = present_mode;
  config.alphaMode = wgpu::CompositeAlphaMode::Auto;

  device.PushErrorScope(wgpu::ErrorFilter::Validation);
  wgpu_surface_.Configure(&config);
  bool configured = true;
  provider_.Wait(device.PopErrorScope(
      wgpu::CallbackMode::WaitAnyOnly,
      [&configured](wgpu::PopErrorScopeStatus status, wgpu::ErrorType type,
                    wgpu::StringView message) {
        if (status == wgpu::PopErrorScopeStatus::Success && type != wgpu::ErrorType::NoError) {
          configured = false;
          REXLOG_ERROR("WebGpuPresenter: surface configuration failed: {}",
                       WebGpuProvider::ToString(message));
        }
      }));
  if (!configured) {
    wgpu_surface_ = nullptr;
    return SurfacePaintConnectResult::kFailureSurfaceUnusable;
  }

  connected_surface_ = &new_surface;
  surface_config_ = config;
  is_vsync_implicit_out = present_mode == wgpu::PresentMode::Fifo;
  REXLOG_INFO("WebGpuPresenter: surface {}x{}, format {}, present mode {}", new_surface_width,
              new_surface_height, uint32_t(format), uint32_t(present_mode));
  return SurfacePaintConnectResult::kSuccess;
}

void WebGpuPresenter::DisconnectPaintingFromSurfaceFromUIThreadImpl() {
  std::lock_guard lock(provider_.device_mutex());
  if (wgpu_surface_) {
    wgpu_surface_.Unconfigure();
    wgpu_surface_ = nullptr;
  }
  connected_surface_ = nullptr;
}

bool WebGpuPresenter::RefreshGuestOutputImpl(
    uint32_t mailbox_index, uint32_t frontbuffer_width, uint32_t frontbuffer_height,
    std::function<bool(GuestOutputRefreshContext& context)> refresher, bool& is_8bpc_out_ref) {
  std::lock_guard lock(provider_.device_mutex());
  uint32_t max_size = provider_.limits().maxTextureDimension2D;
  if (frontbuffer_width > max_size || frontbuffer_height > max_size) {
    return false;
  }
  GuestOutputImage& image = guest_output_images_[mailbox_index];
  if (!image.texture || image.width != frontbuffer_width || image.height != frontbuffer_height) {
    wgpu::TextureDescriptor desc;
    desc.label = "guest output";
    desc.size = {frontbuffer_width, frontbuffer_height, 1};
    desc.format = kGuestOutputFormat;
    desc.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding |
                 wgpu::TextureUsage::CopySrc | wgpu::TextureUsage::CopyDst;
    image.texture = provider_.device().CreateTexture(&desc);
    image.view = image.texture.CreateView();
    image.width = frontbuffer_width;
    image.height = frontbuffer_height;
  }
  WebGpuGuestOutputRefreshContext context(is_8bpc_out_ref, image.texture, image.view);
  return refresher(context);
}

Presenter::PaintResult WebGpuPresenter::PaintAndPresentImpl(bool execute_ui_drawers) {
  std::lock_guard lock(provider_.device_mutex());
  if (provider_.is_device_lost()) {
    return PaintResult::kGpuLostExternally;
  }
  if (!wgpu_surface_) {
    return PaintResult::kNotPresentedConnectionOutdated;
  }

  wgpu::SurfaceTexture surface_texture;
  wgpu_surface_.GetCurrentTexture(&surface_texture);
  bool suboptimal = false;
  switch (surface_texture.status) {
    case wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal:
      break;
    case wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal:
      suboptimal = true;
      break;
    case wgpu::SurfaceGetCurrentTextureStatus::Timeout:
      return PaintResult::kNotPresented;
    case wgpu::SurfaceGetCurrentTextureStatus::Outdated:
    case wgpu::SurfaceGetCurrentTextureStatus::Lost:
      return PaintResult::kNotPresentedConnectionOutdated;
    default:
      return provider_.is_device_lost() ? PaintResult::kGpuLostExternally
                                        : PaintResult::kNotPresented;
  }
  const uint32_t width = surface_config_.width;
  const uint32_t height = surface_config_.height;

  GuestOutputProperties properties;
  GuestOutputPaintConfig paint_config;
  wgpu::TextureView guest_output_view;
  {
    uint32_t mailbox_index;
    std::unique_lock<std::mutex> consumer_lock(
        ConsumeGuestOutput(mailbox_index, &properties, &paint_config));
    if (mailbox_index != UINT32_MAX) {
      guest_output_view = guest_output_images_[mailbox_index].view;
    }
  }

  wgpu::CommandEncoder encoder = provider_.device().CreateCommandEncoder();
  wgpu::RenderPassColorAttachment color;
  color.view = surface_texture.texture.CreateView();
  color.loadOp = wgpu::LoadOp::Clear;
  color.storeOp = wgpu::StoreOp::Store;
  color.clearValue = {0.0, 0.0, 0.0, 1.0};
  wgpu::RenderPassDescriptor pass_desc;
  pass_desc.label = "paint";
  pass_desc.colorAttachmentCount = 1;
  pass_desc.colorAttachments = &color;
  wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&pass_desc);

  if (guest_output_view) {
    uint32_t max_size = provider_.limits().maxTextureDimension2D;
    GuestOutputPaintFlow flow =
        GetGuestOutputPaintFlow(properties, width, height, max_size, max_size, paint_config);
    if (flow.effect_count) {
      // One bilinear pass from the guest output straight to the final
      // letterboxed rectangle. The CAS/FSR effects of the Vulkan presenter
      // are not ported.
      const std::pair<uint32_t, uint32_t>& size = flow.effect_output_sizes[flow.effect_count - 1];
      std::array<wgpu::BindGroupEntry, 2> entries;
      entries[0].binding = 0;
      entries[0].textureView = guest_output_view;
      entries[1].binding = 1;
      entries[1].sampler = linear_clamp_sampler_;
      wgpu::BindGroupDescriptor bg_desc;
      bg_desc.layout = guest_output_bind_group_layout_;
      bg_desc.entryCount = entries.size();
      bg_desc.entries = entries.data();
      pass.SetPipeline(GetGuestOutputPipeline(surface_config_.format));
      pass.SetBindGroup(0, provider_.device().CreateBindGroup(&bg_desc));
      pass.SetViewport(float(flow.output_x), float(flow.output_y), float(size.first),
                       float(size.second), 0.0f, 1.0f);
      pass.Draw(3);
      pass.SetViewport(0.0f, 0.0f, float(width), float(height), 0.0f, 1.0f);
    }
  }

  if (execute_ui_drawers) {
    WebGpuUIDrawContext ui_draw_context(*this, width, height, pass, surface_config_.format,
                                        ++paint_serial_);
    ExecuteUIDrawersFromUIThread(ui_draw_context);
  }

  pass.End();
  wgpu::CommandBuffer commands = encoder.Finish();
  provider_.queue().Submit(1, &commands);
  if (wgpu_surface_.Present() != wgpu::Status::Success) {
    return PaintResult::kNotPresentedConnectionOutdated;
  }
  return suboptimal ? PaintResult::kPresentedSuboptimal : PaintResult::kPresented;
}

bool WebGpuPresenter::CaptureGuestOutput(RawImage& image_out) {
  std::lock_guard lock(provider_.device_mutex());
  wgpu::Texture texture;
  uint32_t width = 0, height = 0;
  {
    uint32_t mailbox_index;
    std::unique_lock<std::mutex> consumer_lock(ConsumeGuestOutput(mailbox_index, nullptr, nullptr));
    if (mailbox_index != UINT32_MAX) {
      const GuestOutputImage& image = guest_output_images_[mailbox_index];
      texture = image.texture;
      width = image.width;
      height = image.height;
    }
  }
  if (!texture) {
    return false;
  }

  const wgpu::Device& device = provider_.device();
  const uint32_t bytes_per_row = (width * 4 + 255) & ~uint32_t(255);
  const uint64_t buffer_size = uint64_t(bytes_per_row) * height;
  wgpu::BufferDescriptor buffer_desc;
  buffer_desc.label = "guest output capture";
  buffer_desc.size = buffer_size;
  buffer_desc.usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst;
  wgpu::Buffer buffer = device.CreateBuffer(&buffer_desc);

  wgpu::TexelCopyTextureInfo source;
  source.texture = texture;
  wgpu::TexelCopyBufferInfo destination;
  destination.buffer = buffer;
  destination.layout.bytesPerRow = bytes_per_row;
  destination.layout.rowsPerImage = height;
  wgpu::Extent3D extent = {width, height, 1};
  wgpu::CommandEncoder encoder = device.CreateCommandEncoder();
  encoder.CopyTextureToBuffer(&source, &destination, &extent);
  wgpu::CommandBuffer commands = encoder.Finish();
  provider_.queue().Submit(1, &commands);

  bool mapped = false;
  provider_.Wait(buffer.MapAsync(
      wgpu::MapMode::Read, 0, buffer_size, wgpu::CallbackMode::WaitAnyOnly,
      [&mapped](wgpu::MapAsyncStatus status, wgpu::StringView message) {
        mapped = status == wgpu::MapAsyncStatus::Success;
        if (!mapped) {
          REXLOG_ERROR("WebGpuPresenter: capture readback failed: {}",
                       WebGpuProvider::ToString(message));
        }
      }));
  if (!mapped) {
    return false;
  }
  const auto* source_bytes = static_cast<const uint8_t*>(buffer.GetConstMappedRange(0, buffer_size));
  image_out.width = width;
  image_out.height = height;
  image_out.stride = size_t(width) * 4;
  image_out.data.resize(image_out.stride * height);
  // kGuestOutputFormat is R8G8B8A8, the same byte order as RawImage's R8G8B8X8.
  for (uint32_t y = 0; y < height; ++y) {
    std::memcpy(image_out.data.data() + image_out.stride * y,
                source_bytes + size_t(bytes_per_row) * y, image_out.stride);
  }
  buffer.Unmap();
  return true;
}

}  // namespace rex::ui::webgpu
