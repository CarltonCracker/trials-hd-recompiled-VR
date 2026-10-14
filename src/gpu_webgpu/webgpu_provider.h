// rexgpu-webgpu - graphics provider: the Dawn instance, adapter and device.
//
// Threading (docs/webgpu-renderer/README.md, 7.6.4): the device is created
// without Dawn's implicit synchronization, so every WebGPU call must be made
// with device_mutex() held. Lock order: the presenter's own mutexes (paint
// mode, guest output consumer) come first, so never call into the presenter
// while holding device_mutex(); the presenter takes it itself. The mutex is
// recursive so helpers can lock it again inside a locked region.

#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

#include <webgpu/webgpu_cpp.h>

#include <rex/ui/graphics_provider.h>

namespace rex::ui::webgpu {

class WebGpuProvider final : public GraphicsProvider {
 public:
  static std::unique_ptr<WebGpuProvider> Create(bool with_presentation);
  ~WebGpuProvider() override;

  std::unique_ptr<Presenter> CreatePresenter(
      Presenter::HostGpuLossCallback host_gpu_loss_callback =
          Presenter::FatalErrorHostGpuLossCallback) override;
  std::unique_ptr<ImmediateDrawer> CreateImmediateDrawer() override;

  bool with_presentation() const { return with_presentation_; }
  const wgpu::Instance& instance() const { return instance_; }
  const wgpu::Adapter& adapter() const { return adapter_; }
  const wgpu::Device& device() const { return device_; }
  const wgpu::Queue& queue() const { return queue_; }
  const wgpu::Limits& limits() const { return limits_; }
  bool HasFeature(wgpu::FeatureName feature) const { return device_.HasFeature(feature); }
  wgpu::BackendType backend_type() const { return backend_type_; }

  // Held around every WebGPU call (7.6.4).
  std::recursive_mutex& device_mutex() const { return device_mutex_; }

  // Set from Dawn's device-lost callback (any thread).
  bool is_device_lost() const { return device_lost_.load(std::memory_order_relaxed); }

  // Runs completed callbacks (MapAsync, OnSubmittedWorkDone). Hold the mutex.
  void ProcessEvents() const { instance_.ProcessEvents(); }

  // Blocks until the future completes. Hold the mutex.
  bool Wait(wgpu::Future future) const;

  static std::string ToString(wgpu::StringView view);

 private:
  explicit WebGpuProvider(bool with_presentation) : with_presentation_(with_presentation) {}
  bool Initialize();
  bool SelectAdapter();

  bool with_presentation_;
  wgpu::Instance instance_;
  wgpu::Adapter adapter_;
  wgpu::Device device_;
  wgpu::Queue queue_;
  wgpu::Limits limits_;
  wgpu::BackendType backend_type_ = wgpu::BackendType::Undefined;
  mutable std::recursive_mutex device_mutex_;
  std::atomic<bool> device_lost_{false};
};

}  // namespace rex::ui::webgpu
