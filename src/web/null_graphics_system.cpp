// trials_hd - web build: a graphics system that runs the guest's GPU command
// stream without drawing. The command processor still executes the ring
// buffer (register writes, fences, interrupts, swaps), so the guest runs as it
// would with a renderer; draws, resolves and presentation do nothing.

#include "web/null_graphics_system.h"

#include <chrono>
#include <memory>
#include <string>

#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/logging.h>
#include <rex/system/gpu_plugin.h>

namespace trials_hd::web {

namespace {

using rex::graphics::CommandProcessor;
using rex::graphics::GraphicsSystem;
using rex::graphics::Shader;
using rex::X_STATUS;

class NullCommandProcessor final : public CommandProcessor {
 public:
  NullCommandProcessor(GraphicsSystem* graphics_system, rex::system::KernelState* kernel_state)
      : CommandProcessor(graphics_system, kernel_state) {}

  void IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                 uint32_t frontbuffer_height) override {
    auto now = std::chrono::steady_clock::now();
    if (swaps_ == 0) {
      first_swap_ = now;
      REXLOG_INFO("web: first guest swap ({}x{} front buffer at {:08X})", frontbuffer_width,
                  frontbuffer_height, frontbuffer_ptr);
    }
    ++swaps_;
    if (swaps_ % 600 == 0) {
      double seconds = std::chrono::duration<double>(now - first_swap_).count();
      REXLOG_INFO("web: {} guest swaps, {:.1f} per second, {} draws", swaps_,
                  double(swaps_ - 1) / seconds, draws_);
    }
  }

 protected:
  bool SetupContext() override { return true; }
  void ShutdownContext() override {}

  Shader* LoadShader(rex::graphics::xenos::ShaderType shader_type, uint32_t guest_address,
                     const uint32_t* host_address, uint32_t dword_count) override {
    (void)shader_type;
    (void)guest_address;
    (void)host_address;
    (void)dword_count;
    return nullptr;
  }

  bool IssueDraw(rex::graphics::xenos::PrimitiveType prim_type, uint32_t index_count,
                 IndexBufferInfo* index_buffer_info, bool major_mode_explicit) override {
    (void)prim_type;
    (void)index_count;
    (void)index_buffer_info;
    (void)major_mode_explicit;
    ++draws_;
    return true;
  }

  bool IssueCopy() override { return true; }

 private:
  uint64_t swaps_ = 0;
  uint64_t draws_ = 0;
  std::chrono::steady_clock::time_point first_swap_;
};

class NullGraphicsSystem final : public GraphicsSystem {
 public:
  std::string name() const override { return "null"; }

  // Nothing to present to: succeed without a presenter, so the app shell
  // runs as it does with a renderer that has no window.
  X_STATUS SetupPresentation(rex::ui::WindowedAppContext* app_context) override {
    app_context_ = app_context;
    return X_STATUS_SUCCESS;
  }

 protected:
  void CreateProvider(bool with_presentation) override { (void)with_presentation; }

  std::unique_ptr<CommandProcessor> CreateCommandProcessor() override {
    return std::make_unique<NullCommandProcessor>(this, kernel_state_);
  }
};

uint32_t NullAbiVersion() {
  return rex::system::kGpuPluginAbiVersion;
}

rex::system::IGraphicsSystem* NullCreate(uint32_t abi_version,
                                         const rex::system::GpuCreateInfo* info) {
  (void)info;
  if (abi_version != rex::system::kGpuPluginAbiVersion) {
    return nullptr;
  }
  return new NullGraphicsSystem();
}

}  // namespace

void RegisterNullGpuPlugin() {
  rex::system::RegisterStaticGpuPlugin("null", NullAbiVersion, NullCreate);
}

}  // namespace trials_hd::web
