// webgpu_smoke - checks that the WebGPU stack works on this machine: opens an
// SDL window, creates a device and surface through src/webgpu, and draws a
// triangle for a number of frames.
//
// Usage: webgpu_smoke [frames=300] [--no-vsync]

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <SDL3/SDL.h>

#include "webgpu/webgpu_context.h"

namespace {

constexpr char kShader[] = R"(
struct VsOut {
  @builtin(position) pos : vec4f,
  @location(0) color : vec3f,
};

@vertex fn vs(@builtin(vertex_index) i : u32) -> VsOut {
  var p = array<vec2f, 3>(vec2f(0.0, 0.7), vec2f(-0.7, -0.6), vec2f(0.7, -0.6));
  var c = array<vec3f, 3>(vec3f(1.0, 0.45, 0.1), vec3f(0.1, 0.8, 0.4), vec3f(0.2, 0.4, 1.0));
  var out : VsOut;
  out.pos = vec4f(p[i], 0.0, 1.0);
  out.color = c[i];
  return out;
}

@fragment fn fs(in : VsOut) -> @location(0) vec4f {
  return vec4f(in.color, 1.0);
}
)";

}  // namespace

int main(int argc, char** argv) {
  int frames = argc > 1 && argv[1][0] != '-' ? std::atoi(argv[1]) : 300;
  trials_hd::webgpu::SurfaceOptions options;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--no-vsync")) {
      options.vsync = false;
    }
  }

  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 1;
  }
  SDL_Window* window = SDL_CreateWindow("webgpu_smoke", 1280, 720, 0);
  if (!window) {
    std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
    return 1;
  }

  trials_hd::webgpu::Context gpu;
  if (!gpu.Initialize() || !gpu.AttachWindow(window, options)) {
    std::fprintf(stderr, "WebGPU init failed: %s\n", gpu.error().c_str());
    return 1;
  }
  std::printf("adapter: %s\nsurface: %ux%u format %d vsync %d\n", gpu.adapter_name().c_str(),
              gpu.width(), gpu.height(), int(gpu.surface_format()), int(options.vsync));

  wgpu::ShaderSourceWGSL wgsl;
  wgsl.code = kShader;
  wgpu::ShaderModuleDescriptor module_desc;
  module_desc.nextInChain = &wgsl;
  wgpu::ShaderModule module = gpu.device().CreateShaderModule(&module_desc);

  wgpu::ColorTargetState target;
  target.format = gpu.surface_format();
  wgpu::FragmentState fragment;
  fragment.module = module;
  fragment.entryPoint = "fs";
  fragment.targetCount = 1;
  fragment.targets = &target;
  wgpu::RenderPipelineDescriptor pipeline_desc;
  pipeline_desc.vertex.module = module;
  pipeline_desc.vertex.entryPoint = "vs";
  pipeline_desc.fragment = &fragment;
  wgpu::RenderPipeline pipeline = gpu.device().CreateRenderPipeline(&pipeline_desc);

  const auto start = std::chrono::steady_clock::now();
  int presented = 0;
  for (int frame = 0; frame < frames; ++frame) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      if (event.type == SDL_EVENT_QUIT) {
        frame = frames;
      }
    }
    wgpu::Texture backbuffer = gpu.AcquireBackbuffer();
    if (!backbuffer) {
      continue;
    }
    wgpu::RenderPassColorAttachment color;
    color.view = backbuffer.CreateView();
    color.loadOp = wgpu::LoadOp::Clear;
    color.storeOp = wgpu::StoreOp::Store;
    color.clearValue = {0.08, 0.06, 0.05, 1.0};
    wgpu::RenderPassDescriptor pass_desc;
    pass_desc.colorAttachmentCount = 1;
    pass_desc.colorAttachments = &color;

    wgpu::CommandEncoder encoder = gpu.device().CreateCommandEncoder();
    wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&pass_desc);
    pass.SetPipeline(pipeline);
    pass.Draw(3);
    pass.End();
    wgpu::CommandBuffer commands = encoder.Finish();
    gpu.queue().Submit(1, &commands);
    gpu.Present();
    ++presented;
  }
  const double seconds =
      std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  std::printf("presented %d frames in %.2fs (%.1f fps)\n", presented, seconds,
              presented / seconds);

  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
