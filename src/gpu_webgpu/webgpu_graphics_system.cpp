// rexgpu-webgpu - graphics system.

#include "webgpu_graphics_system.h"

#include "webgpu_command_processor.h"
#include "webgpu_provider.h"

namespace rex::graphics::webgpu {

WebGpuGraphicsSystem::WebGpuGraphicsSystem() = default;
WebGpuGraphicsSystem::~WebGpuGraphicsSystem() = default;

std::string WebGpuGraphicsSystem::name() const {
  return "WebGPU";
}

void WebGpuGraphicsSystem::CreateProvider(bool with_presentation) {
  provider_ = ::rex::ui::webgpu::WebGpuProvider::Create(with_presentation);
}

std::unique_ptr<CommandProcessor> WebGpuGraphicsSystem::CreateCommandProcessor() {
  return std::make_unique<WebGpuCommandProcessor>(this, kernel_state_);
}

}  // namespace rex::graphics::webgpu
