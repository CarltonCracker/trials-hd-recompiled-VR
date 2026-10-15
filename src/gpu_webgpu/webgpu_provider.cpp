// rexgpu-webgpu - graphics provider.

#include "webgpu_provider.h"

#include <array>
#include <cstring>
#include <vector>

#include <dawn/native/DawnNative.h>
#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "webgpu_immediate_drawer.h"
#include "webgpu_presenter.h"

REXCVAR_DEFINE_INT32(webgpu_adapter, -1, "GPU/WebGPU",
                     "Index of the WebGPU adapter to use, as listed in the log at startup "
                     "(-1 = the first discrete GPU, else the first adapter)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(webgpu_validation, false, "GPU/WebGPU",
                    "Validate WebGPU commands in Dawn (debugging; off uses the faster "
                    "skip_validation toggle)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(webgpu_backend_validation, false, "GPU/WebGPU",
                    "Enable the Vulkan or Metal validation layers under Dawn (debugging)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::ui::webgpu {

namespace {

// Optional features the renderer uses when the adapter has them (section 9.1).
constexpr std::array kWantedFeatures = {
    wgpu::FeatureName::TextureCompressionBC,  wgpu::FeatureName::Depth32FloatStencil8,
    wgpu::FeatureName::TextureFormatsTier1,   wgpu::FeatureName::RG11B10UfloatRenderable,
    wgpu::FeatureName::TimestampQuery,        wgpu::FeatureName::DepthClipControl,
    wgpu::FeatureName::Float32Filterable,     wgpu::FeatureName::Unorm16TextureFormats,
};

const char* BackendName(wgpu::BackendType type) {
  switch (type) {
    case wgpu::BackendType::Vulkan:
      return "Vulkan";
    case wgpu::BackendType::Metal:
      return "Metal";
    case wgpu::BackendType::D3D12:
      return "D3D12";
    case wgpu::BackendType::D3D11:
      return "D3D11";
    case wgpu::BackendType::OpenGL:
      return "OpenGL";
    case wgpu::BackendType::OpenGLES:
      return "OpenGLES";
    case wgpu::BackendType::Null:
      return "Null";
    default:
      return "unknown";
  }
}

const char* AdapterTypeName(wgpu::AdapterType type) {
  switch (type) {
    case wgpu::AdapterType::DiscreteGPU:
      return "discrete";
    case wgpu::AdapterType::IntegratedGPU:
      return "integrated";
    case wgpu::AdapterType::CPU:
      return "CPU";
    default:
      return "unknown";
  }
}

#if defined(__APPLE__)
constexpr wgpu::BackendType kPreferredBackend = wgpu::BackendType::Metal;
#else
constexpr wgpu::BackendType kPreferredBackend = wgpu::BackendType::Vulkan;
#endif

// Owns the Dawn native instance, which keeps the enumerated adapters alive.
std::unique_ptr<dawn::native::Instance>& NativeInstance() {
  static std::unique_ptr<dawn::native::Instance> instance;
  return instance;
}

}  // namespace

std::string WebGpuProvider::ToString(wgpu::StringView view) {
  if (!view.data) {
    return {};
  }
  return std::string(view.data, view.length == WGPU_STRLEN ? std::strlen(view.data) : view.length);
}

std::unique_ptr<WebGpuProvider> WebGpuProvider::Create(bool with_presentation) {
  std::unique_ptr<WebGpuProvider> provider(new WebGpuProvider(with_presentation));
  if (!provider->Initialize()) {
    return nullptr;
  }
  return provider;
}

WebGpuProvider::~WebGpuProvider() {
  std::lock_guard lock(device_mutex_);
  queue_ = nullptr;
  if (device_) {
    device_.Destroy();
    device_ = nullptr;
  }
  adapter_ = nullptr;
  instance_ = nullptr;
}

bool WebGpuProvider::Initialize() {
  static constexpr auto kTimedWaitAny = wgpu::InstanceFeatureName::TimedWaitAny;
  wgpu::InstanceDescriptor instance_desc;
  instance_desc.requiredFeatureCount = 1;
  instance_desc.requiredFeatures = &kTimedWaitAny;
  auto& native_instance = NativeInstance();
  native_instance = std::make_unique<dawn::native::Instance>(&instance_desc);
  if (REXCVAR_GET(webgpu_backend_validation)) {
    native_instance->SetBackendValidationLevel(dawn::native::BackendValidationLevel::Full);
  }
  instance_ = wgpu::Instance(native_instance->Get());
  if (!instance_) {
    REXLOG_ERROR("WebGPU: failed to create the Dawn instance");
    return false;
  }
  if (!SelectAdapter()) {
    return false;
  }

  wgpu::AdapterInfo adapter_info;
  adapter_.GetInfo(&adapter_info);
  backend_type_ = adapter_info.backendType;

  std::vector<wgpu::FeatureName> features;
  for (wgpu::FeatureName feature : kWantedFeatures) {
    if (adapter_.HasFeature(feature)) {
      features.push_back(feature);
    }
  }

  // Ask for everything the adapter allows: the 512 MiB shared-memory buffer
  // needs large buffer and storage binding sizes (7.6.3).
  wgpu::Limits adapter_limits;
  adapter_.GetLimits(&adapter_limits);

  std::vector<const char*> enabled_toggles;
  enabled_toggles.push_back("use_user_defined_labels_in_backend");
  if (!REXCVAR_GET(webgpu_validation)) {
    enabled_toggles.push_back("skip_validation");
  }
  wgpu::DawnTogglesDescriptor toggles;
  toggles.enabledToggleCount = enabled_toggles.size();
  toggles.enabledToggles = enabled_toggles.data();

  wgpu::DeviceDescriptor device_desc;
  device_desc.nextInChain = &toggles;
  device_desc.label = "rexgpu-webgpu";
  device_desc.requiredFeatureCount = features.size();
  device_desc.requiredFeatures = features.data();
  device_desc.requiredLimits = &adapter_limits;
  device_desc.SetUncapturedErrorCallback(
      [](const wgpu::Device&, wgpu::ErrorType type, wgpu::StringView message) {
        REXGPU_ERROR("WebGPU error ({}): {}", uint32_t(type), ToString(message));
      });
  device_desc.SetDeviceLostCallback(
      wgpu::CallbackMode::AllowSpontaneous,
      [this](const wgpu::Device&, wgpu::DeviceLostReason reason, wgpu::StringView message) {
        if (reason == wgpu::DeviceLostReason::Destroyed) {
          return;
        }
        device_lost_.store(true, std::memory_order_relaxed);
        REXGPU_ERROR("WebGPU device lost ({}): {}", uint32_t(reason), ToString(message));
      });
  Wait(adapter_.RequestDevice(
      &device_desc, wgpu::CallbackMode::WaitAnyOnly,
      [this](wgpu::RequestDeviceStatus status, wgpu::Device device, wgpu::StringView message) {
        if (status == wgpu::RequestDeviceStatus::Success) {
          device_ = std::move(device);
        } else {
          REXLOG_ERROR("WebGPU: device request failed: {}", ToString(message));
        }
      }));
  if (!device_) {
    return false;
  }
  device_.SetLoggingCallback([](wgpu::LoggingType type, wgpu::StringView message) {
    switch (type) {
      case wgpu::LoggingType::Error:
        REXGPU_ERROR("Dawn: {}", ToString(message));
        break;
      case wgpu::LoggingType::Warning:
        REXGPU_WARN("Dawn: {}", ToString(message));
        break;
      default:
        REXGPU_DEBUG("Dawn: {}", ToString(message));
        break;
    }
  });
  queue_ = device_.GetQueue();
  device_.GetLimits(&limits_);

  std::string feature_list;
  for (wgpu::FeatureName feature : kWantedFeatures) {
    if (device_.HasFeature(feature)) {
      feature_list += fmt::format(" 0x{:X}", uint32_t(feature));
    }
  }
  REXLOG_INFO(
      "WebGPU: device on {} ({}), validation {}; optional features:{}; maxBufferSize {} MiB, "
      "maxStorageBufferBindingSize {} MiB, maxTextureDimension2D {}, maxImmediateSize {}",
      ToString(adapter_info.device), BackendName(backend_type_),
      REXCVAR_GET(webgpu_validation) ? "on" : "off", feature_list,
      limits_.maxBufferSize >> 20, limits_.maxStorageBufferBindingSize >> 20,
      limits_.maxTextureDimension2D, limits_.maxImmediateSize);
  return true;
}

bool WebGpuProvider::SelectAdapter() {
  wgpu::RequestAdapterOptions options;
  options.backendType = kPreferredBackend;
  options.powerPreference = wgpu::PowerPreference::HighPerformance;
  std::vector<dawn::native::Adapter> adapters = NativeInstance()->EnumerateAdapters(&options);
  if (adapters.empty()) {
    REXLOG_ERROR("WebGPU: no {} adapters found", BackendName(kPreferredBackend));
    return false;
  }

  int32_t chosen = -1;
  for (size_t i = 0; i < adapters.size(); ++i) {
    wgpu::Adapter adapter(adapters[i].Get());
    wgpu::AdapterInfo info;
    adapter.GetInfo(&info);
    REXLOG_INFO("WebGPU adapter {}: {} ({}, {}, vendor 0x{:04X} device 0x{:04X}, driver {})", i,
                ToString(info.device), AdapterTypeName(info.adapterType),
                BackendName(info.backendType), info.vendorID, info.deviceID,
                ToString(info.description));
    if (chosen < 0 && info.adapterType == wgpu::AdapterType::DiscreteGPU) {
      chosen = int32_t(i);
    }
  }
  int32_t requested = REXCVAR_GET(webgpu_adapter);
  if (requested >= 0) {
    if (size_t(requested) < adapters.size()) {
      chosen = requested;
    } else {
      REXLOG_WARN("WebGPU: webgpu_adapter={} is out of range, using the default", requested);
    }
  }
  if (chosen < 0) {
    chosen = 0;
  }
  adapter_ = wgpu::Adapter(adapters[size_t(chosen)].Get());
  REXLOG_INFO("WebGPU: using adapter {}", chosen);
  return true;
}

bool WebGpuProvider::Wait(wgpu::Future future) const {
  return instance_.WaitAny(future, UINT64_MAX) == wgpu::WaitStatus::Success;
}

std::unique_ptr<Presenter> WebGpuProvider::CreatePresenter(
    Presenter::HostGpuLossCallback host_gpu_loss_callback) {
  return WebGpuPresenter::Create(host_gpu_loss_callback, *this);
}

std::unique_ptr<ImmediateDrawer> WebGpuProvider::CreateImmediateDrawer() {
  return WebGpuImmediateDrawer::Create(*this);
}

}  // namespace rex::ui::webgpu
