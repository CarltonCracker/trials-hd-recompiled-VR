// trials_hd - WebGPU device and window surface.

#include "webgpu_context.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string_view>

#include <SDL3/SDL.h>

namespace trials_hd::webgpu {

namespace {

std::string ToString(wgpu::StringView view) {
  return view.data ? std::string(view.data, view.length == WGPU_STRLEN ? std::strlen(view.data)
                                                                       : view.length)
                   : std::string();
}

}  // namespace

bool Context::Initialize() {
  static constexpr auto kTimedWaitAny = wgpu::InstanceFeatureName::TimedWaitAny;
  wgpu::InstanceDescriptor instance_desc;
  instance_desc.requiredFeatureCount = 1;
  instance_desc.requiredFeatures = &kTimedWaitAny;
  instance_ = wgpu::CreateInstance(&instance_desc);
  if (!instance_) {
    error_ = "wgpuCreateInstance failed";
    return false;
  }

  wgpu::RequestAdapterOptions adapter_options;
  adapter_options.powerPreference = wgpu::PowerPreference::HighPerformance;
  instance_.WaitAny(
      instance_.RequestAdapter(
          &adapter_options, wgpu::CallbackMode::WaitAnyOnly,
          [this](wgpu::RequestAdapterStatus status, wgpu::Adapter adapter, wgpu::StringView msg) {
            if (status == wgpu::RequestAdapterStatus::Success) {
              adapter_ = std::move(adapter);
            } else {
              error_ = "no WebGPU adapter: " + ToString(msg);
            }
          }),
      UINT64_MAX);
  if (!adapter_) {
    return false;
  }
  wgpu::AdapterInfo info;
  adapter_.GetInfo(&info);
  adapter_name_ = ToString(info.device);

  wgpu::DeviceDescriptor device_desc;
  device_desc.SetUncapturedErrorCallback(
      [](const wgpu::Device&, wgpu::ErrorType type, wgpu::StringView message) {
        std::fprintf(stderr, "[webgpu] error %d: %s\n", int(type), ToString(message).c_str());
      });
  device_desc.SetDeviceLostCallback(
      wgpu::CallbackMode::AllowSpontaneous,
      [](const wgpu::Device&, wgpu::DeviceLostReason reason, wgpu::StringView message) {
        if (reason != wgpu::DeviceLostReason::Destroyed) {
          std::fprintf(stderr, "[webgpu] device lost (%d): %s\n", int(reason),
                       ToString(message).c_str());
        }
      });
  instance_.WaitAny(
      adapter_.RequestDevice(
          &device_desc, wgpu::CallbackMode::WaitAnyOnly,
          [this](wgpu::RequestDeviceStatus status, wgpu::Device device, wgpu::StringView msg) {
            if (status == wgpu::RequestDeviceStatus::Success) {
              device_ = std::move(device);
            } else {
              error_ = "device request failed: " + ToString(msg);
            }
          }),
      UINT64_MAX);
  if (!device_) {
    return false;
  }
  queue_ = device_.GetQueue();
  return true;
}

bool Context::AttachWindow(SDL_Window* window, const SurfaceOptions& options) {
  SDL_PropertiesID props = SDL_GetWindowProperties(window);
  wgpu::SurfaceDescriptor surface_desc;
#if defined(__EMSCRIPTEN__)
  wgpu::EmscriptenSurfaceSourceCanvasHTMLSelector canvas;
  canvas.selector = "#canvas";
  surface_desc.nextInChain = &canvas;
#else
  wgpu::SurfaceSourceXlibWindow xlib;
  xlib.display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
  xlib.window = uint64_t(SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));
  if (!xlib.display || !xlib.window) {
    error_ = "window is not an X11 window (Wayland surfaces are not wired up yet)";
    return false;
  }
  surface_desc.nextInChain = &xlib;
#endif
  surface_ = instance_.CreateSurface(&surface_desc);
  if (!surface_) {
    error_ = "CreateSurface failed";
    return false;
  }
  int w = 0, h = 0;
  SDL_GetWindowSizeInPixels(window, &w, &h);
  ConfigureSurface(uint32_t(std::max(w, 1)), uint32_t(std::max(h, 1)), options);
  return surface_format_ != wgpu::TextureFormat::Undefined;
}

void Context::ConfigureSurface(uint32_t width, uint32_t height, const SurfaceOptions& options) {
  wgpu::SurfaceCapabilities caps;
  surface_.GetCapabilities(adapter_, &caps);
  if (!caps.formatCount) {
    error_ = "surface has no supported formats";
    return;
  }
  surface_format_ = caps.formats[0];

  auto supports = [&](wgpu::PresentMode mode) {
    return std::find(caps.presentModes, caps.presentModes + caps.presentModeCount, mode) !=
           caps.presentModes + caps.presentModeCount;
  };
  wgpu::PresentMode present_mode = wgpu::PresentMode::Fifo;  // Always available.
  if (!options.vsync) {
    if (supports(wgpu::PresentMode::Immediate)) {
      present_mode = wgpu::PresentMode::Immediate;
    } else if (supports(wgpu::PresentMode::Mailbox)) {
      present_mode = wgpu::PresentMode::Mailbox;
    }
  }

  wgpu::SurfaceConfiguration config;
  config.device = device_;
  config.format = surface_format_;
  config.usage = wgpu::TextureUsage::RenderAttachment;
  config.width = width;
  config.height = height;
  config.presentMode = present_mode;
  config.alphaMode = wgpu::CompositeAlphaMode::Auto;
  surface_.Configure(&config);
  width_ = width;
  height_ = height;
}

wgpu::Texture Context::AcquireBackbuffer() {
  wgpu::SurfaceTexture surface_texture;
  surface_.GetCurrentTexture(&surface_texture);
  switch (surface_texture.status) {
    case wgpu::SurfaceGetCurrentTextureStatus::SuccessOptimal:
    case wgpu::SurfaceGetCurrentTextureStatus::SuccessSuboptimal:
      return surface_texture.texture;
    default:
      return nullptr;
  }
}

void Context::Present() {
  surface_.Present();
}

}  // namespace trials_hd::webgpu
