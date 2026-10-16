// rexgpu-webgpu - graphics system.

#include "webgpu_graphics_system.h"

#include "webgpu_command_processor.h"
#include "webgpu_provider.h"

#if defined(__EMSCRIPTEN__)
#include <rex/logging.h>
#include <rex/thread/web.h>
#include <rex/ui/immediate_drawer.h>
#include <rex/ui/web.h>

#include "webgpu_presenter.h"
#endif

namespace rex::graphics::webgpu {

WebGpuGraphicsSystem::WebGpuGraphicsSystem() = default;
WebGpuGraphicsSystem::~WebGpuGraphicsSystem() = default;

std::string WebGpuGraphicsSystem::name() const {
  return "WebGPU";
}

void WebGpuGraphicsSystem::CreateProvider(bool with_presentation) {
#if defined(__EMSCRIPTEN__)
  // Created on the GPU worker instead (InitializeOnGpuWorker).
  (void)with_presentation;
#else
  provider_ = ::rex::ui::webgpu::WebGpuProvider::Create(with_presentation);
#endif
}

std::unique_ptr<CommandProcessor> WebGpuGraphicsSystem::CreateCommandProcessor() {
#if defined(__EMSCRIPTEN__)
  // The command processor's thread, created next, becomes the GPU worker and
  // receives the page's canvas.
  ::rex::thread::web::SetTransferredCanvasesForNextThread(
      ::rex::ui::webgpu::WebGpuPresenter::kCanvasSelector);
#endif
  return std::make_unique<WebGpuCommandProcessor>(this, kernel_state_);
}

#if defined(__EMSCRIPTEN__)
X_STATUS WebGpuGraphicsSystem::SetupPresentation(::rex::ui::WindowedAppContext* app_context) {
  app_context_ = app_context;
  return X_STATUS_SUCCESS;
}

bool WebGpuGraphicsSystem::InitializeOnGpuWorker() {
  auto provider = ::rex::ui::webgpu::WebGpuProvider::Create(true);
  if (!provider) {
    REXGPU_ERROR("WebGPU: no device on the GPU worker");
    return false;
  }
  web_presenter_ = provider->CreatePresenter();
  if (!web_presenter_) {
    REXGPU_ERROR("WebGPU: cannot create the presenter");
    return false;
  }
  provider_ = std::move(provider);
  // The app's overlays (ImGui) draw through an immediate drawer created here,
  // like every WebGPU object; this worker runs them in PaintToCanvas.
  ::rex::ui::web::NotifyPresenterReady(web_presenter_.get(),
                                       provider_->CreateImmediateDrawer());
  return true;
}

void WebGpuGraphicsSystem::ShutdownOnGpuWorker() {
  web_presenter_.reset();
  provider_.reset();
}
#endif

}  // namespace rex::graphics::webgpu
