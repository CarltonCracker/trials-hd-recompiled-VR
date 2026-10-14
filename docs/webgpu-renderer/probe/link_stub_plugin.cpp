// L-01 link experiment (docs/webgpu-renderer/probe/link_stub.sh, section 7.6.2):
// the smallest graphics system built only from the SDK's backend-agnostic sources.
#include <rex/graphics/command_processor.h>
#include <rex/graphics/graphics_system.h>
#include <rex/logging.h>
#include <rex/system/gpu_plugin.h>

namespace stub {
using namespace rex::graphics;
class StubCommandProcessor final : public CommandProcessor {
 public:
  using CommandProcessor::CommandProcessor;
  void IssueSwap(uint32_t, uint32_t, uint32_t) override {}
 protected:
  bool SetupContext() override { return true; }
  void ShutdownContext() override {}
  Shader* LoadShader(xenos::ShaderType, uint32_t, const uint32_t*, uint32_t) override {
    return nullptr;
  }
  bool IssueDraw(xenos::PrimitiveType, uint32_t, IndexBufferInfo*, bool) override { return true; }
  bool IssueCopy() override { return true; }
};
class StubGraphicsSystem final : public GraphicsSystem {
 public:
  std::string name() const override { return "stub"; }
 protected:
  void CreateProvider(bool) override {}
  std::unique_ptr<CommandProcessor> CreateCommandProcessor() override {
    return std::make_unique<StubCommandProcessor>(this, kernel_state_);
  }
};
}  // namespace stub

extern "C" REX_GPU_PLUGIN_EXPORT uint32_t rex_gpu_abi_version(void) {
  return rex::system::kGpuPluginAbiVersion;
}
extern "C" REX_GPU_PLUGIN_EXPORT rex::system::IGraphicsSystem* rex_gpu_create(
    uint32_t, const rex::system::GpuCreateInfo*) {
  return new stub::StubGraphicsSystem();
}
