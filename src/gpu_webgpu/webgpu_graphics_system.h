// rexgpu-webgpu - graphics system: MMIO, vsync and the ring buffer come from
// the SDK's GraphicsSystem; this class supplies the WebGPU provider and the
// command processor.

#pragma once

#include <memory>
#include <string>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>

namespace rex::graphics::webgpu {

class WebGpuGraphicsSystem : public GraphicsSystem {
 public:
  WebGpuGraphicsSystem();
  ~WebGpuGraphicsSystem() override;

  std::string name() const override;

#if defined(__EMSCRIPTEN__)
  // Web (docs/web-port/README.md 7.9.5, D5): WebGPU objects belong to the
  // worker that creates them, so the device and the presenter are created
  // by the command processor's thread (the GPU worker, which owns the canvas)
  // when it starts, not by SetupPresentation on the UI thread.
  X_STATUS SetupPresentation(::rex::ui::WindowedAppContext* app_context) override;
  ::rex::ui::Presenter* presenter() const override { return web_presenter_.get(); }
  // On the GPU worker: creates the device and the presenter / releases them.
  bool InitializeOnGpuWorker();
  void ShutdownOnGpuWorker();
#endif

 protected:
  void CreateProvider(bool with_presentation) override;

 private:
  std::unique_ptr<CommandProcessor> CreateCommandProcessor() override;

#if defined(__EMSCRIPTEN__)
  std::unique_ptr<::rex::ui::Presenter> web_presenter_;
#endif
};

}  // namespace rex::graphics::webgpu
