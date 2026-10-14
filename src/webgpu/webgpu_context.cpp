// trials_hd - WebGPU device and window surface.

#include "webgpu/webgpu_context.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string_view>
#include <vector>

#include <SDL3/SDL.h>
#include <dawn/native/DawnNative.h>

namespace trials_hd::webgpu {

namespace {

std::string ToString(wgpu::StringView view) {
  return view.data ? std::string(view.data, view.length == WGPU_STRLEN ? std::strlen(view.data)
                                                                       : view.length)
                   : std::string();
}

constexpr std::array kWantedFeatures = {
    wgpu::FeatureName::TextureCompressionBC,
    wgpu::FeatureName::Depth32FloatStencil8,
    wgpu::FeatureName::TextureFormatsTier1,
    wgpu::FeatureName::RG11B10UfloatRenderable,
    wgpu::FeatureName::TimestampQuery,
    wgpu::FeatureName::DepthClipControl,
    wgpu::FeatureName::Float32Filterable,
    wgpu::FeatureName::Unorm16TextureFormats,
};

}  // namespace

Context::Context() = default;

Context::~Context() {
  if (surface_) {
    surface_.Unconfigure();
    surface_ = nullptr;
  }
  queue_ = nullptr;
  if (device_) {
    device_.Destroy();
    device_ = nullptr;
  }
  adapter_ = nullptr;
  instance_ = nullptr;
  native_instance_.reset();
}

std::vector<AdapterDetails> Context::EnumerateAdapters() {
  static constexpr auto kTimedWaitAny = wgpu::InstanceFeatureName::TimedWaitAny;
  wgpu::InstanceDescriptor instance_desc;
  instance_desc.requiredFeatureCount = 1;
  instance_desc.requiredFeatures = &kTimedWaitAny;
  auto native_instance = std::make_unique<dawn::native::Instance>(&instance_desc);

  std::vector<AdapterDetails> results;
  wgpu::RequestAdapterOptions options;
#if defined(__APPLE__)
  options.backendType = wgpu::BackendType::Metal;
#else
  options.backendType = wgpu::BackendType::Vulkan;
#endif
  options.powerPreference = wgpu::PowerPreference::HighPerformance;

  std::vector<dawn::native::Adapter> adapters = native_instance->EnumerateAdapters(&options);
  results.reserve(adapters.size());
  for (size_t i = 0; i < adapters.size(); ++i) {
    wgpu::Adapter a(adapters[i].Get());
    wgpu::AdapterInfo info;
    a.GetInfo(&info);
    AdapterDetails d;
    d.index = static_cast<int>(i);
    d.name = ToString(info.device);
    d.type = info.adapterType;
    d.backend = info.backendType;
    d.vendor_id = info.vendorID;
    d.device_id = info.deviceID;
    d.description = ToString(info.description);
    results.push_back(std::move(d));
  }
  return results;
}

bool Context::Initialize(const ContextOptions& options) {
  static constexpr auto kTimedWaitAny = wgpu::InstanceFeatureName::TimedWaitAny;
  wgpu::InstanceDescriptor instance_desc;
  instance_desc.requiredFeatureCount = 1;
  instance_desc.requiredFeatures = &kTimedWaitAny;
  native_instance_ = std::make_unique<dawn::native::Instance>(&instance_desc);
  instance_ = wgpu::Instance(native_instance_->Get());
  if (!instance_) {
    error_ = "wgpuCreateInstance failed";
    return false;
  }

  wgpu::RequestAdapterOptions adapter_options;
#if defined(__APPLE__)
  adapter_options.backendType = wgpu::BackendType::Metal;
#else
  adapter_options.backendType = wgpu::BackendType::Vulkan;
#endif
  adapter_options.powerPreference = wgpu::PowerPreference::HighPerformance;

  std::vector<dawn::native::Adapter> adapters = native_instance_->EnumerateAdapters(&adapter_options);
  if (adapters.empty()) {
    error_ = "no WebGPU adapters found";
    return false;
  }

  int chosen = -1;
  if (options.adapter_index >= 0 && size_t(options.adapter_index) < adapters.size()) {
    chosen = options.adapter_index;
  } else {
    for (size_t i = 0; i < adapters.size(); ++i) {
      wgpu::Adapter a(adapters[i].Get());
      wgpu::AdapterInfo info;
      a.GetInfo(&info);
      if (info.adapterType == wgpu::AdapterType::DiscreteGPU) {
        chosen = static_cast<int>(i);
        break;
      }
    }
    if (chosen < 0) {
      chosen = 0;
    }
  }

  adapter_ = wgpu::Adapter(adapters[chosen].Get());
  if (!adapter_) {
    error_ = "selected adapter is null";
    return false;
  }
  wgpu::AdapterInfo info;
  adapter_.GetInfo(&info);
  adapter_name_ = ToString(info.device);

  std::vector<wgpu::FeatureName> features;
  for (wgpu::FeatureName f : kWantedFeatures) {
    if (adapter_.HasFeature(f)) {
      features.push_back(f);
    }
  }
  if (options.implicit_device_sync &&
      adapter_.HasFeature(wgpu::FeatureName::ImplicitDeviceSynchronization)) {
    features.push_back(wgpu::FeatureName::ImplicitDeviceSynchronization);
  }

  wgpu::Limits adapter_limits;
  adapter_.GetLimits(&adapter_limits);

  std::vector<const char*> enabled_toggles;
  if (!options.enable_validation) {
    enabled_toggles.push_back("skip_validation");
  }

  wgpu::DawnTogglesDescriptor toggles;
  toggles.enabledToggleCount = enabled_toggles.size();
  toggles.enabledToggles = enabled_toggles.data();

  wgpu::DeviceDescriptor device_desc;
  if (!enabled_toggles.empty()) {
    device_desc.nextInChain = &toggles;
  }
  device_desc.label = "trials_hd::webgpu";
  device_desc.requiredFeatureCount = features.size();
  device_desc.requiredFeatures = features.data();
  device_desc.requiredLimits = &adapter_limits;
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
  surface_desc.label = "webgpu_smoke surface";

#if defined(__EMSCRIPTEN__)
  wgpu::EmscriptenSurfaceSourceCanvasHTMLSelector canvas;
  canvas.selector = "#canvas";
  surface_desc.nextInChain = &canvas;
#elif defined(__APPLE__)
  wgpu::SurfaceSourceMetalLayer metal;
  metal.layer = SDL_Metal_GetLayer(SDL_Metal_CreateView(window));
  surface_desc.nextInChain = &metal;
#else
  void* wl_display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, nullptr);
  void* wl_surface = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, nullptr);
  void* x11_display = SDL_GetPointerProperty(props, SDL_PROP_WINDOW_X11_DISPLAY_POINTER, nullptr);
  uint64_t x11_window = uint64_t(SDL_GetNumberProperty(props, SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0));

  wgpu::SurfaceSourceWaylandSurface wayland;
  wgpu::SurfaceSourceXlibWindow xlib;

  if (wl_display && wl_surface) {
    wayland.display = wl_display;
    wayland.surface = wl_surface;
    surface_desc.nextInChain = &wayland;
  } else if (x11_display && x11_window) {
    xlib.display = x11_display;
    xlib.window = x11_window;
    surface_desc.nextInChain = &xlib;
  } else {
    error_ = "window is neither an X11 window nor a Wayland surface";
    return false;
  }
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
