// W-S04: WebGPU objects and an OffscreenCanvas owned by one pthread.
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <webgpu/webgpu_cpp.h>
#include <emscripten.h>
#include <emscripten/html5.h>
#include <emscripten/threading.h>

namespace {
wgpu::Instance instance;
wgpu::Adapter adapter;
wgpu::Device device;
wgpu::Surface surface;
wgpu::RenderPipeline pipeline;
unsigned frames = 0;
constexpr unsigned kWidth = 320, kHeight = 240;

EM_BOOL Frame(double, void*) {
  assert(!emscripten_is_main_browser_thread());
  wgpu::SurfaceTexture texture;
  surface.GetCurrentTexture(&texture);
  assert(texture.texture);
  wgpu::RenderPassColorAttachment color;
  color.view = texture.texture.CreateView();
  color.loadOp = wgpu::LoadOp::Clear;
  color.storeOp = wgpu::StoreOp::Store;
  color.clearValue = {0.05, 0.05, 0.1, 1};
  wgpu::RenderPassDescriptor pass;
  pass.colorAttachmentCount = 1;
  pass.colorAttachments = &color;
  auto encoder = device.CreateCommandEncoder();
  auto draw = encoder.BeginRenderPass(&pass);
  draw.SetPipeline(pipeline);
  draw.Draw(3);
  draw.End();
  auto commands = encoder.Finish();
  device.GetQueue().Submit(1, &commands);
  if (++frames == 5) {
    std::puts("WEBGPU_TRIANGLE_RESULT {\"frames\":5,\"gpuOnPthread\":true,\"offscreenCanvas\":true}");
  }
  return EM_TRUE;
}

void Initialize() {
  wgpu::EmscriptenSurfaceSourceCanvasHTMLSelector canvas;
  canvas.selector = "#canvas";
  wgpu::SurfaceDescriptor descriptor;
  descriptor.nextInChain = &canvas;
  surface = instance.CreateSurface(&descriptor);
  wgpu::SurfaceCapabilities capabilities;
  surface.GetCapabilities(adapter, &capabilities);
  assert(capabilities.formatCount > 0);
  auto format = capabilities.formats[0];
  wgpu::SurfaceConfiguration configuration;
  configuration.device = device;
  configuration.format = format;
  configuration.usage = wgpu::TextureUsage::RenderAttachment;
  configuration.width = kWidth;
  configuration.height = kHeight;
  configuration.alphaMode = wgpu::CompositeAlphaMode::Opaque;
  configuration.presentMode = wgpu::PresentMode::Fifo;
  surface.Configure(&configuration);

  wgpu::ShaderSourceWGSL wgsl;
  wgsl.code = R"(
    @vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
      let points = array(vec2f(0, 0.8), vec2f(-0.8, -0.8), vec2f(0.8, -0.8));
      return vec4f(points[i], 0, 1);
    }
    @fragment fn fs() -> @location(0) vec4f { return vec4f(1, 0.2, 0, 1); }
  )";
  wgpu::ShaderModuleDescriptor shader;
  shader.nextInChain = &wgsl;
  auto module = device.CreateShaderModule(&shader);
  wgpu::ColorTargetState target;
  target.format = format;
  wgpu::FragmentState fragment;
  fragment.module = module;
  fragment.entryPoint = "fs";
  fragment.targetCount = 1;
  fragment.targets = &target;
  wgpu::RenderPipelineDescriptor render;
  render.vertex.module = module;
  render.vertex.entryPoint = "vs";
  render.fragment = &fragment;
  render.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
  pipeline = device.CreateRenderPipeline(&render);
  assert(pipeline);
  emscripten_request_animation_frame_loop(Frame, nullptr);
}
}  // namespace

int main() {
  assert(!emscripten_is_main_browser_thread());
  assert(EM_ASM_INT({ return Module.canvas instanceof OffscreenCanvas; }));
  instance = wgpu::CreateInstance(nullptr);
  instance.RequestAdapter(nullptr, wgpu::CallbackMode::AllowSpontaneous,
      [](wgpu::RequestAdapterStatus status, wgpu::Adapter value, wgpu::StringView) {
    assert(!emscripten_is_main_browser_thread());
    assert(status == wgpu::RequestAdapterStatus::Success);
    adapter = value;
    wgpu::DeviceDescriptor descriptor;
    descriptor.SetUncapturedErrorCallback([](const wgpu::Device&, wgpu::ErrorType,
                                             wgpu::StringView message) {
      std::fprintf(stderr, "WebGPU error: %.*s\n", int(message.length), message.data);
      std::abort();
    });
    adapter.RequestDevice(&descriptor, wgpu::CallbackMode::AllowSpontaneous,
        [](wgpu::RequestDeviceStatus status, wgpu::Device value, wgpu::StringView) {
      assert(!emscripten_is_main_browser_thread());
      assert(status == wgpu::RequestDeviceStatus::Success);
      device = value;
      Initialize();
    });
  });
  emscripten_exit_with_live_runtime();
}
