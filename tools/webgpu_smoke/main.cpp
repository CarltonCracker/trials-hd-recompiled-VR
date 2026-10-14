// webgpu_smoke - checks that the WebGPU stack works on this machine: opens an
// SDL window, creates a device and surface through src/webgpu, and draws a
// triangle for a number of frames.
//
// Usage: webgpu_smoke [frames=300] [--vsync|--no-vsync] [--adapter=N]
//                     [--list-adapters] [--info] [--bench]

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <string>
#include <vector>

#include <SDL3/SDL.h>
#include <webgpu/webgpu_cpp.h>

#include "webgpu/webgpu_context.h"

namespace {

constexpr char kTriangleShader[] = R"(
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

void PrintAdapterList() {
  auto adapters = trials_hd::webgpu::Context::EnumerateAdapters();
  std::printf("Available WebGPU adapters (%zu found):\n", adapters.size());
  for (const auto& a : adapters) {
    std::printf("  [%d] %s (%s, %s, vendor 0x%04X, device 0x%04X, driver %s)\n", a.index,
                a.name.c_str(), AdapterTypeName(a.type), BackendName(a.backend), a.vendor_id,
                a.device_id, a.description.c_str());
  }
}

void PrintAdapterInfo(int target_adapter = -1) {
  auto adapters = trials_hd::webgpu::Context::EnumerateAdapters();
  if (adapters.empty()) {
    std::fprintf(stderr, "No WebGPU adapters found.\n");
    return;
  }

  for (size_t i = 0; i < adapters.size(); ++i) {
    if (target_adapter >= 0 && static_cast<int>(i) != target_adapter) {
      continue;
    }
    const auto& a = adapters[i];
    std::printf("\n==================================================\n");
    std::printf("Adapter [%zu]: %s\n", i, a.name.c_str());
    std::printf("  Type:        %s\n", AdapterTypeName(a.type));
    std::printf("  Backend:     %s\n", BackendName(a.backend));
    std::printf("  Vendor ID:   0x%04X\n", a.vendor_id);
    std::printf("  Device ID:   0x%04X\n", a.device_id);
    std::printf("  Description: %s\n", a.description.c_str());

    trials_hd::webgpu::Context ctx;
    trials_hd::webgpu::ContextOptions opts;
    opts.adapter_index = static_cast<int>(i);
    opts.enable_validation = true;
    if (!ctx.Initialize(opts)) {
      std::printf("  [Failed to initialize: %s]\n", ctx.error().c_str());
      continue;
    }

    const wgpu::Adapter& adapter = ctx.adapter();
    const wgpu::Device& device = ctx.device();

    std::printf("\n  [Key Features (Open Question 1 & Section 9.1)]:\n");
    auto check_feature = [&](const char* name, wgpu::FeatureName feat) {
      bool adapter_has = adapter.HasFeature(feat);
      bool device_has = device.HasFeature(feat);
      std::printf("    %-32s : %s\n", name,
                  adapter_has ? (device_has ? "SUPPORTED (active)" : "SUPPORTED (inactive)")
                              : "NOT SUPPORTED");
    };

    check_feature("texture-formats-tier1", wgpu::FeatureName::TextureFormatsTier1);
    check_feature("depth32float-stencil8", wgpu::FeatureName::Depth32FloatStencil8);
    check_feature("texture-compression-bc", wgpu::FeatureName::TextureCompressionBC);
    check_feature("timestamp-query", wgpu::FeatureName::TimestampQuery);
    check_feature("depth-clip-control", wgpu::FeatureName::DepthClipControl);
    check_feature("float32-filterable", wgpu::FeatureName::Float32Filterable);
    check_feature("rg11b10ufloat-renderable", wgpu::FeatureName::RG11B10UfloatRenderable);
    check_feature("unorm16-texture-formats", wgpu::FeatureName::Unorm16TextureFormats);
    check_feature("implicit-device-sync", wgpu::FeatureName::ImplicitDeviceSynchronization);

    wgpu::Limits limits;
    device.GetLimits(&limits);

    std::printf("\n  [Device Limits (Open Question 12 & Section 9.1)]:\n");
    std::printf("    maxBufferSize:                      %llu bytes (%llu MiB)\n",
                static_cast<unsigned long long>(limits.maxBufferSize),
                static_cast<unsigned long long>(limits.maxBufferSize >> 20));
    std::printf("    maxStorageBufferBindingSize:        %llu bytes (%llu MiB)\n",
                static_cast<unsigned long long>(limits.maxStorageBufferBindingSize),
                static_cast<unsigned long long>(limits.maxStorageBufferBindingSize >> 20));
    std::printf("    maxImmediateSize:                   %u bytes\n", limits.maxImmediateSize);
    std::printf("    maxTextureDimension2D:              %u\n", limits.maxTextureDimension2D);
    std::printf("    maxBindGroups:                      %u\n", limits.maxBindGroups);
    std::printf("    maxDynamicUniformBuffersPerLayout:  %u\n",
                limits.maxDynamicUniformBuffersPerPipelineLayout);
    std::printf("    maxDynamicStorageBuffersPerLayout:  %u\n",
                limits.maxDynamicStorageBuffersPerPipelineLayout);
    std::printf("    maxSampledTexturesPerShaderStage:   %u\n",
                limits.maxSampledTexturesPerShaderStage);
    std::printf("    maxSamplersPerShaderStage:          %u\n", limits.maxSamplersPerShaderStage);
    std::printf("    maxStorageBuffersPerShaderStage:    %u\n", limits.maxStorageBuffersPerShaderStage);
    std::printf("    maxStorageTexturesPerShaderStage:   %u\n", limits.maxStorageTexturesPerShaderStage);
    std::printf("    maxUniformBuffersPerShaderStage:    %u\n", limits.maxUniformBuffersPerShaderStage);
    std::printf("    maxUniformBufferBindingSize:        %llu bytes (%llu KiB)\n",
                static_cast<unsigned long long>(limits.maxUniformBufferBindingSize),
                static_cast<unsigned long long>(limits.maxUniformBufferBindingSize >> 10));
    std::printf("    minUniformBufferOffsetAlignment:    %u bytes\n",
                limits.minUniformBufferOffsetAlignment);
    std::printf("    minStorageBufferOffsetAlignment:    %u bytes\n",
                limits.minStorageBufferOffsetAlignment);
  }
  std::printf("==================================================\n\n");
}

void RunBenchmark(int adapter_index, int bench_frames) {
  struct BenchConfig {
    const char* name;
    bool validation;
    bool implicit_sync;
  };

  const BenchConfig configs[] = {
      {"Validation ON,  ImplicitSync OFF", true, false},
      {"Validation OFF (skip_validation), ImplicitSync OFF", false, false},
      {"Validation ON,  ImplicitSync ON", true, true},
      {"Validation OFF (skip_validation), ImplicitSync ON", false, true},
  };

  std::printf("\n==================================================\n");
  std::printf("Dawn CPU Overhead Benchmark (Task S-15, Section 7.5)\n");
  std::printf("Workload: 1,500 draws/frame across 30 render passes, 60 pipelines\n");
  std::printf("Binding model: 7.6.5 (5 dynamic offsets in group 0, storage in group 1, texture+sampler in group 2)\n");
  std::printf("Frames: %d (%d draws total)\n", bench_frames, bench_frames * 1500);
  std::printf("==================================================\n\n");

  for (const auto& cfg : configs) {
    trials_hd::webgpu::Context gpu;
    trials_hd::webgpu::ContextOptions opts;
    opts.adapter_index = adapter_index;
    opts.enable_validation = cfg.validation;
    opts.implicit_device_sync = cfg.implicit_sync;

    if (!gpu.Initialize(opts)) {
      std::fprintf(stderr, "Configuration '%s' failed to initialize: %s\n", cfg.name,
                   gpu.error().c_str());
      continue;
    }

    // Verify whether implicit sync was actually enabled if requested
    if (cfg.implicit_sync &&
        !gpu.device().HasFeature(wgpu::FeatureName::ImplicitDeviceSynchronization)) {
      std::printf("[%s]: Adapter does not support ImplicitDeviceSynchronization; skipping.\n",
                  cfg.name);
      continue;
    }

    // Render target (1280x720 offscreen)
    wgpu::TextureDescriptor target_desc;
    target_desc.label = "bench color target";
    target_desc.size = {1280, 720, 1};
    target_desc.format = wgpu::TextureFormat::RGBA8Unorm;
    target_desc.usage = wgpu::TextureUsage::RenderAttachment;
    wgpu::Texture target = gpu.device().CreateTexture(&target_desc);
    wgpu::TextureView color_view = target.CreateView();

    // Group 0 layout: 5 dynamic-offset uniform buffers
    std::array<wgpu::BindGroupLayoutEntry, 5> bgl0_entries{};
    bgl0_entries[0].binding = 0;
    bgl0_entries[0].visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
    bgl0_entries[0].buffer.type = wgpu::BufferBindingType::Uniform;
    bgl0_entries[0].buffer.hasDynamicOffset = true;
    bgl0_entries[0].buffer.minBindingSize = 256;

    bgl0_entries[1].binding = 1;
    bgl0_entries[1].visibility = wgpu::ShaderStage::Vertex;
    bgl0_entries[1].buffer.type = wgpu::BufferBindingType::Uniform;
    bgl0_entries[1].buffer.hasDynamicOffset = true;
    bgl0_entries[1].buffer.minBindingSize = 4096;

    bgl0_entries[2].binding = 2;
    bgl0_entries[2].visibility = wgpu::ShaderStage::Fragment;
    bgl0_entries[2].buffer.type = wgpu::BufferBindingType::Uniform;
    bgl0_entries[2].buffer.hasDynamicOffset = true;
    bgl0_entries[2].buffer.minBindingSize = 4096;

    bgl0_entries[3].binding = 3;
    bgl0_entries[3].visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
    bgl0_entries[3].buffer.type = wgpu::BufferBindingType::Uniform;
    bgl0_entries[3].buffer.hasDynamicOffset = true;
    bgl0_entries[3].buffer.minBindingSize = 256;

    bgl0_entries[4].binding = 4;
    bgl0_entries[4].visibility = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment;
    bgl0_entries[4].buffer.type = wgpu::BufferBindingType::Uniform;
    bgl0_entries[4].buffer.hasDynamicOffset = true;
    bgl0_entries[4].buffer.minBindingSize = 768;

    wgpu::BindGroupLayoutDescriptor bgl0_desc;
    bgl0_desc.label = "bgl0";
    bgl0_desc.entryCount = bgl0_entries.size();
    bgl0_desc.entries = bgl0_entries.data();
    wgpu::BindGroupLayout bgl0 = gpu.device().CreateBindGroupLayout(&bgl0_desc);

    // Group 1 layout: read-only storage buffer
    std::array<wgpu::BindGroupLayoutEntry, 1> bgl1_entries{};
    bgl1_entries[0].binding = 0;
    bgl1_entries[0].visibility = wgpu::ShaderStage::Vertex;
    bgl1_entries[0].buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
    bgl1_entries[0].buffer.hasDynamicOffset = false;
    bgl1_entries[0].buffer.minBindingSize = 1024;

    wgpu::BindGroupLayoutDescriptor bgl1_desc;
    bgl1_desc.label = "bgl1";
    bgl1_desc.entryCount = bgl1_entries.size();
    bgl1_desc.entries = bgl1_entries.data();
    wgpu::BindGroupLayout bgl1 = gpu.device().CreateBindGroupLayout(&bgl1_desc);

    // Group 2 layout: texture and sampler
    std::array<wgpu::BindGroupLayoutEntry, 2> bgl2_entries{};
    bgl2_entries[0].binding = 0;
    bgl2_entries[0].visibility = wgpu::ShaderStage::Fragment;
    bgl2_entries[0].texture.sampleType = wgpu::TextureSampleType::Float;
    bgl2_entries[0].texture.viewDimension = wgpu::TextureViewDimension::e2D;

    bgl2_entries[1].binding = 1;
    bgl2_entries[1].visibility = wgpu::ShaderStage::Fragment;
    bgl2_entries[1].sampler.type = wgpu::SamplerBindingType::Filtering;

    wgpu::BindGroupLayoutDescriptor bgl2_desc;
    bgl2_desc.label = "bgl2";
    bgl2_desc.entryCount = bgl2_entries.size();
    bgl2_desc.entries = bgl2_entries.data();
    wgpu::BindGroupLayout bgl2 = gpu.device().CreateBindGroupLayout(&bgl2_desc);

    // Pipeline Layout
    std::array<wgpu::BindGroupLayout, 3> layouts = {bgl0, bgl1, bgl2};
    wgpu::PipelineLayoutDescriptor pipe_layout_desc;
    pipe_layout_desc.label = "bench pipeline layout";
    pipe_layout_desc.bindGroupLayoutCount = layouts.size();
    pipe_layout_desc.bindGroupLayouts = layouts.data();
    wgpu::PipelineLayout pipeline_layout = gpu.device().CreatePipelineLayout(&pipe_layout_desc);

    // Buffers and BindGroups
    wgpu::BufferDescriptor buf0_desc;
    buf0_desc.label = "group 0 uniform buffer";
    buf0_desc.size = 65536;
    buf0_desc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
    wgpu::Buffer buf0 = gpu.device().CreateBuffer(&buf0_desc);

    std::array<wgpu::BindGroupEntry, 5> bg0_entries{};
    bg0_entries[0].binding = 0;
    bg0_entries[0].buffer = buf0;
    bg0_entries[0].offset = 0;
    bg0_entries[0].size = 256;

    bg0_entries[1].binding = 1;
    bg0_entries[1].buffer = buf0;
    bg0_entries[1].offset = 0;
    bg0_entries[1].size = 4096;

    bg0_entries[2].binding = 2;
    bg0_entries[2].buffer = buf0;
    bg0_entries[2].offset = 0;
    bg0_entries[2].size = 4096;

    bg0_entries[3].binding = 3;
    bg0_entries[3].buffer = buf0;
    bg0_entries[3].offset = 0;
    bg0_entries[3].size = 256;

    bg0_entries[4].binding = 4;
    bg0_entries[4].buffer = buf0;
    bg0_entries[4].offset = 0;
    bg0_entries[4].size = 768;

    wgpu::BindGroupDescriptor bg0_desc;
    bg0_desc.label = "bg0";
    bg0_desc.layout = bgl0;
    bg0_desc.entryCount = bg0_entries.size();
    bg0_desc.entries = bg0_entries.data();
    wgpu::BindGroup bg0 = gpu.device().CreateBindGroup(&bg0_desc);

    wgpu::BufferDescriptor buf1_desc;
    buf1_desc.label = "group 1 storage buffer";
    buf1_desc.size = 65536;
    buf1_desc.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
    wgpu::Buffer buf1 = gpu.device().CreateBuffer(&buf1_desc);

    std::array<wgpu::BindGroupEntry, 1> bg1_entries{};
    bg1_entries[0].binding = 0;
    bg1_entries[0].buffer = buf1;
    bg1_entries[0].offset = 0;
    bg1_entries[0].size = 65536;

    wgpu::BindGroupDescriptor bg1_desc;
    bg1_desc.label = "bg1";
    bg1_desc.layout = bgl1;
    bg1_desc.entryCount = bg1_entries.size();
    bg1_desc.entries = bg1_entries.data();
    wgpu::BindGroup bg1 = gpu.device().CreateBindGroup(&bg1_desc);

    wgpu::TextureDescriptor tex_desc;
    tex_desc.label = "group 2 texture";
    tex_desc.size = {4, 4, 1};
    tex_desc.format = wgpu::TextureFormat::RGBA8Unorm;
    tex_desc.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    wgpu::Texture tex2 = gpu.device().CreateTexture(&tex_desc);
    wgpu::TextureView tex2_view = tex2.CreateView();

    wgpu::SamplerDescriptor samp_desc;
    samp_desc.label = "group 2 sampler";
    samp_desc.minFilter = wgpu::FilterMode::Linear;
    samp_desc.magFilter = wgpu::FilterMode::Linear;
    wgpu::Sampler samp2 = gpu.device().CreateSampler(&samp_desc);

    std::array<wgpu::BindGroupEntry, 2> bg2_entries{};
    bg2_entries[0].binding = 0;
    bg2_entries[0].textureView = tex2_view;
    bg2_entries[1].binding = 1;
    bg2_entries[1].sampler = samp2;

    wgpu::BindGroupDescriptor bg2_desc;
    bg2_desc.label = "bg2";
    bg2_desc.layout = bgl2;
    bg2_desc.entryCount = bg2_entries.size();
    bg2_desc.entries = bg2_entries.data();
    wgpu::BindGroup bg2 = gpu.device().CreateBindGroup(&bg2_desc);

    // 60 distinct pipelines
    std::vector<wgpu::RenderPipeline> pipelines;
    pipelines.reserve(60);
    for (int i = 0; i < 60; ++i) {
      std::string wgsl_source = R"(
struct VsOut {
  @builtin(position) pos : vec4f,
  @location(0) uv : vec2f,
};

@group(0) @binding(0) var<uniform> sys: vec4f;
@group(0) @binding(1) var<uniform> vs_flt: array<vec4f, 64>;
@group(0) @binding(2) var<uniform> ps_flt: array<vec4f, 64>;
@group(0) @binding(3) var<uniform> bool_loop: vec4u;
@group(0) @binding(4) var<uniform> fetch: array<vec4u, 16>;

@group(1) @binding(0) var<storage, read> guest_mem: array<u32>;

@group(2) @binding(0) var tex: texture_2d<f32>;
@group(2) @binding(1) var smp: sampler;

const PIPE_ID: u32 = )" + std::to_string(i) + R"(u;

@vertex fn vs(@builtin(vertex_index) idx: u32) -> VsOut {
  var p = array<vec2f, 3>(vec2f(0.0, 0.5), vec2f(-0.5, -0.5), vec2f(0.5, -0.5));
  var out: VsOut;
  var offset = f32(PIPE_ID) * 0.0001;
  out.pos = vec4f(p[idx] + sys.xy + vs_flt[0].xy + vec2f(offset, offset), 0.0, 1.0);
  out.uv = p[idx] * 0.5 + 0.5;
  return out;
}

@fragment fn fs(in: VsOut) -> @location(0) vec4f {
  var color = textureSample(tex, smp, in.uv);
  return color + ps_flt[0] + vec4f(f32(PIPE_ID) * 0.001);
}
)";
      wgpu::ShaderSourceWGSL wgsl;
      wgsl.code = wgsl_source.c_str();
      wgpu::ShaderModuleDescriptor sm_desc;
      sm_desc.nextInChain = &wgsl;
      wgpu::ShaderModule module = gpu.device().CreateShaderModule(&sm_desc);

      wgpu::ColorTargetState color_target;
      color_target.format = wgpu::TextureFormat::RGBA8Unorm;

      wgpu::FragmentState frag_state;
      frag_state.module = module;
      frag_state.entryPoint = "fs";
      frag_state.targetCount = 1;
      frag_state.targets = &color_target;

      wgpu::RenderPipelineDescriptor rp_desc;
      rp_desc.layout = pipeline_layout;
      rp_desc.vertex.module = module;
      rp_desc.vertex.entryPoint = "vs";
      rp_desc.fragment = &frag_state;
      pipelines.push_back(gpu.device().CreateRenderPipeline(&rp_desc));
    }

    // Dynamic offsets for group 0
    const uint32_t offsets[5] = {0, 256, 4352, 8448, 8704};

    // Warm-up run (3 frames)
    for (int f = 0; f < 3; ++f) {
      wgpu::CommandEncoder enc = gpu.device().CreateCommandEncoder();
      for (int p = 0; p < 30; ++p) {
        wgpu::RenderPassColorAttachment att;
        att.view = color_view;
        att.loadOp = (p == 0) ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load;
        att.storeOp = wgpu::StoreOp::Store;
        att.clearValue = {0.0, 0.0, 0.0, 1.0};
        wgpu::RenderPassDescriptor rpd;
        rpd.colorAttachmentCount = 1;
        rpd.colorAttachments = &att;
        wgpu::RenderPassEncoder pass = enc.BeginRenderPass(&rpd);
        pass.SetBindGroup(1, bg1);
        pass.SetBindGroup(2, bg2);
        for (int d = 0; d < 50; ++d) {
          int pipe_idx = (p * 50 + d) % 60;
          pass.SetPipeline(pipelines[pipe_idx]);
          pass.SetBindGroup(0, bg0, 5, offsets);
          pass.Draw(3);
        }
        pass.End();
      }
      wgpu::CommandBuffer cmd = enc.Finish();
      gpu.queue().Submit(1, &cmd);
    }

    // Timed benchmark
    auto t_encode_total = std::chrono::nanoseconds(0);
    auto t_submit_total = std::chrono::nanoseconds(0);

    for (int f = 0; f < bench_frames; ++f) {
      auto t_enc_start = std::chrono::steady_clock::now();
      wgpu::CommandEncoder enc = gpu.device().CreateCommandEncoder();
      for (int p = 0; p < 30; ++p) {
        wgpu::RenderPassColorAttachment att;
        att.view = color_view;
        att.loadOp = (p == 0) ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load;
        att.storeOp = wgpu::StoreOp::Store;
        att.clearValue = {0.0, 0.0, 0.0, 1.0};
        wgpu::RenderPassDescriptor rpd;
        rpd.colorAttachmentCount = 1;
        rpd.colorAttachments = &att;
        wgpu::RenderPassEncoder pass = enc.BeginRenderPass(&rpd);
        pass.SetBindGroup(1, bg1);
        pass.SetBindGroup(2, bg2);
        for (int d = 0; d < 50; ++d) {
          int pipe_idx = (p * 50 + d) % 60;
          pass.SetPipeline(pipelines[pipe_idx]);
          pass.SetBindGroup(0, bg0, 5, offsets);
          pass.Draw(3);
        }
        pass.End();
      }
      wgpu::CommandBuffer cmd = enc.Finish();
      auto t_enc_end = std::chrono::steady_clock::now();
      t_encode_total += (t_enc_end - t_enc_start);

      auto t_sub_start = std::chrono::steady_clock::now();
      gpu.queue().Submit(1, &cmd);
      auto t_sub_end = std::chrono::steady_clock::now();
      t_submit_total += (t_sub_end - t_sub_start);
    }

    int total_draws = bench_frames * 1500;
    double enc_us_per_draw = (t_encode_total.count() / 1000.0) / total_draws;
    double sub_us_per_draw = (t_submit_total.count() / 1000.0) / total_draws;
    double total_us_per_draw = enc_us_per_draw + sub_us_per_draw;
    double enc_ms_per_frame = (t_encode_total.count() / 1000000.0) / bench_frames;
    double sub_ms_per_frame = (t_submit_total.count() / 1000000.0) / bench_frames;
    double total_ms_per_frame = enc_ms_per_frame + sub_ms_per_frame;

    std::printf("Result for [%s]:\n", cfg.name);
    std::printf("  Encode:  %6.3f µs/draw  (%5.2f ms/frame)\n", enc_us_per_draw, enc_ms_per_frame);
    std::printf("  Submit:  %6.3f µs/draw  (%5.2f ms/frame)\n", sub_us_per_draw, sub_ms_per_frame);
    std::printf("  Total:   %6.3f µs/draw  (%5.2f ms/frame)\n\n", total_us_per_draw,
                total_ms_per_frame);
  }
  std::printf("==================================================\n\n");
}

}  // namespace

int main(int argc, char** argv) {
  int frames = 300;
  int adapter_index = -1;
  int width = 1280;
  int height = 720;
  bool list_adapters_flag = false;
  bool info_flag = false;
  bool bench_flag = false;
  bool headless_flag = false;
  trials_hd::webgpu::SurfaceOptions surface_options;
  surface_options.vsync = true;

  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--list-adapters")) {
      list_adapters_flag = true;
    } else if (!std::strcmp(argv[i], "--info")) {
      info_flag = true;
    } else if (!std::strcmp(argv[i], "--bench")) {
      bench_flag = true;
    } else if (!std::strcmp(argv[i], "--headless")) {
      headless_flag = true;
    } else if (!std::strcmp(argv[i], "--no-vsync")) {
      surface_options.vsync = false;
    } else if (!std::strcmp(argv[i], "--vsync")) {
      surface_options.vsync = true;
    } else if (!std::strncmp(argv[i], "--adapter=", 10)) {
      adapter_index = std::atoi(argv[i] + 10);
    } else if (!std::strncmp(argv[i], "--width=", 8)) {
      width = std::atoi(argv[i] + 8);
    } else if (!std::strncmp(argv[i], "--height=", 9)) {
      height = std::atoi(argv[i] + 9);
    } else if (argv[i][0] != '-') {
      frames = std::atoi(argv[i]);
    }
  }

  if (list_adapters_flag) {
    PrintAdapterList();
    return 0;
  }

  if (info_flag) {
    PrintAdapterInfo(adapter_index);
    return 0;
  }

  if (bench_flag) {
    int bench_frames = (frames == 300) ? 20 : frames;
    RunBenchmark(adapter_index, bench_frames);
    return 0;
  }

  if (headless_flag) {
    trials_hd::webgpu::Context gpu;
    trials_hd::webgpu::ContextOptions ctx_options;
    ctx_options.adapter_index = adapter_index;
    if (!gpu.Initialize(ctx_options)) {
      std::fprintf(stderr, "WebGPU init failed: %s\n", gpu.error().c_str());
      return 1;
    }
    std::printf("adapter: %s (headless offscreen rendering %ux%u)\n",
                gpu.adapter_name().c_str(), width, height);

    wgpu::TextureDescriptor tex_desc;
    tex_desc.label = "headless offscreen target";
    tex_desc.size = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
    tex_desc.format = wgpu::TextureFormat::RGBA8Unorm;
    tex_desc.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
    wgpu::Texture offscreen = gpu.device().CreateTexture(&tex_desc);

    wgpu::ShaderSourceWGSL wgsl;
    wgsl.code = kTriangleShader;
    wgpu::ShaderModuleDescriptor module_desc;
    module_desc.nextInChain = &wgsl;
    wgpu::ShaderModule module = gpu.device().CreateShaderModule(&module_desc);

    wgpu::ColorTargetState target;
    target.format = wgpu::TextureFormat::RGBA8Unorm;
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
    for (int frame = 0; frame < frames; ++frame) {
      wgpu::RenderPassColorAttachment color;
      color.view = offscreen.CreateView();
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
    }
    const double seconds =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::printf("rendered %d frames in %.2fs (%.1f fps)\n", frames, seconds,
                frames / (seconds > 0 ? seconds : 1.0));

    // Readback verification of triangle pixels
    uint32_t bytes_per_row = (width * 4 + 255) & ~255;
    wgpu::BufferDescriptor buf_desc;
    buf_desc.label = "readback buffer";
    buf_desc.size = bytes_per_row * height;
    buf_desc.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
    wgpu::Buffer readback = gpu.device().CreateBuffer(&buf_desc);

    wgpu::CommandEncoder copy_enc = gpu.device().CreateCommandEncoder();
    wgpu::TexelCopyTextureInfo src_tex;
    src_tex.texture = offscreen;
    wgpu::TexelCopyBufferInfo dst_buf;
    dst_buf.buffer = readback;
    dst_buf.layout.bytesPerRow = bytes_per_row;
    dst_buf.layout.rowsPerImage = height;
    wgpu::Extent3D copy_size = {static_cast<uint32_t>(width), static_cast<uint32_t>(height), 1};
    copy_enc.CopyTextureToBuffer(&src_tex, &dst_buf, &copy_size);
    wgpu::CommandBuffer copy_cmd = copy_enc.Finish();
    gpu.queue().Submit(1, &copy_cmd);

    bool map_done = false;
    wgpu::Future fut = readback.MapAsync(
        wgpu::MapMode::Read, 0, buf_desc.size, wgpu::CallbackMode::WaitAnyOnly,
        [&](wgpu::MapAsyncStatus status, wgpu::StringView) {
          if (status == wgpu::MapAsyncStatus::Success) {
            map_done = true;
          }
        });
    wgpu::FutureWaitInfo wait_info = {fut, false};
    gpu.instance().WaitAny(1, &wait_info, UINT64_MAX);

    if (map_done) {
      const uint8_t* pixels = static_cast<const uint8_t*>(readback.GetConstMappedRange());
      int cx = width / 2;
      int cy = height / 2;
      const uint8_t* center_px = pixels + cy * bytes_per_row + cx * 4;
      const uint8_t* corner_px = pixels + 10 * bytes_per_row + 10 * 4;
      std::printf("Center pixel (triangle):   RGBA (%u, %u, %u, %u)\n", center_px[0],
                  center_px[1], center_px[2], center_px[3]);
      std::printf("Corner pixel (background): RGBA (%u, %u, %u, %u)\n", corner_px[0],
                  corner_px[1], corner_px[2], corner_px[3]);
      readback.Unmap();
      if (center_px[0] > corner_px[0] || center_px[1] > corner_px[1] ||
          center_px[2] > corner_px[2]) {
        std::printf("Pixel verification: SUCCESS (triangle rendered correctly)\n");
      } else {
        std::printf("Pixel verification: FAILED (center pixel is background)\n");
        return 1;
      }
    }
    return 0;
  }

  // Normal windowed smoke test
  if (!SDL_Init(SDL_INIT_VIDEO)) {
    std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
    return 1;
  }

  SDL_Window* window = SDL_CreateWindow("webgpu_smoke", width, height, 0);
  if (!window) {
    std::fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
    SDL_Quit();
    return 1;
  }

  trials_hd::webgpu::Context gpu;
  trials_hd::webgpu::ContextOptions ctx_options;
  ctx_options.adapter_index = adapter_index;

  if (!gpu.Initialize(ctx_options) || !gpu.AttachWindow(window, surface_options)) {
    std::fprintf(stderr, "WebGPU init failed: %s\n", gpu.error().c_str());
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 1;
  }

  std::printf("adapter: %s\nsurface: %ux%u format %d vsync %d\n", gpu.adapter_name().c_str(),
              gpu.width(), gpu.height(), int(gpu.surface_format()), int(surface_options.vsync));

  wgpu::ShaderSourceWGSL wgsl;
  wgsl.code = kTriangleShader;
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
              presented / (seconds > 0 ? seconds : 1.0));

  SDL_DestroyWindow(window);
  SDL_Quit();
  return 0;
}
