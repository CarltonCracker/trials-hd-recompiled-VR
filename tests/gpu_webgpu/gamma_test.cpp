// rexgpu-webgpu - Unit tests for gamma application shaders (S-20).
// Tests table and PWL gamma shaders on GPU compute via Dawn vs CPU references.

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <rex/graphics/xenos.h>
#include <rex/logging.h>

#include "gpu_test.h"

#define TEST_CHECK(cond)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      std::exit(1);                                                            \
    }                                                                          \
  } while (false)

using namespace rex::graphics;

namespace {

// Embedded WGSL sources for self-contained testing.
const char* kApplyGammaTableWgsl = R"(
struct GammaConstants {
  size: vec2u,
};

@group(0) @binding(0) var<uniform> constants: GammaConstants;
@group(0) @binding(1) var<storage, read> in_pixels: array<vec4f>;
@group(0) @binding(2) var<storage, read> ramp_table: array<vec4f>;
@group(0) @binding(3) var<storage, read_write> out_pixels: array<vec4f>;

fn apply_gamma_table(color: vec3f) -> vec3f {
  let indices = vec3u(clamp(color, vec3f(0.0), vec3f(1.0)) * 255.0 + 0.5);
  let r = ramp_table[indices.x].z;
  let g = ramp_table[indices.y].y;
  let b = ramp_table[indices.z].x;
  return vec3f(r, g, b);
}

@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) gid: vec3u) {
  let idx = gid.x;
  if (idx < constants.size.x) {
    let src = in_pixels[idx];
    let gamma_rgb = apply_gamma_table(src.rgb);
    out_pixels[idx] = vec4f(gamma_rgb, src.a);
  }
}
)";

const char* kApplyGammaPwlWgsl = R"(
struct GammaConstants {
  size: vec2u,
};

struct PwlEntry {
  base: u32,
  slope: u32,
};

@group(0) @binding(0) var<uniform> constants: GammaConstants;
@group(0) @binding(1) var<storage, read> in_pixels: array<vec4f>;
@group(0) @binding(2) var<storage, read> ramp_pwl: array<PwlEntry>;
@group(0) @binding(3) var<storage, read_write> out_pixels: array<vec4f>;

fn apply_gamma_pwl_channel(c: f32, channel_idx: u32) -> f32 {
  let c_clamped = clamp(c, 0.0, 1.0);
  let c_1023 = u32(c_clamped * 1023.0 + 0.5);
  let segment = c_1023 >> 3u;
  let fraction = c_1023 & 7u;
  let entry_idx = segment * 3u + channel_idx;
  let entry = ramp_pwl[entry_idx];
  let val = f32(entry.base) + f32(fraction * entry.slope) * 0.125;
  return clamp(val * 1.52737048e-5, 0.0, 1.0);
}

fn apply_gamma_pwl(color: vec3f) -> vec3f {
  let r = apply_gamma_pwl_channel(color.r, 2u);
  let g = apply_gamma_pwl_channel(color.g, 1u);
  let b = apply_gamma_pwl_channel(color.b, 0u);
  return vec3f(r, g, b);
}

fn xe_linear_to_pwl_gamma(linear: f32) -> f32 {
  let l = clamp(linear, 0.0, 1.0);
  var scale = 1023.0;
  var offset = 0.0;
  if (l >= 128.0 / 1023.0) {
    if (l >= 512.0 / 1023.0) {
      scale = 1023.0 / 8.0;
      offset = 128.0 / 255.0;
    } else {
      scale = 1023.0 / 4.0;
      offset = 64.0 / 255.0;
    }
  } else {
    if (l >= 64.0 / 1023.0) {
      scale = 1023.0 / 2.0;
      offset = 32.0 / 255.0;
    } else {
      scale = 1023.0;
      offset = 0.0;
    }
  }
  return trunc(l * scale) * (1.0 / 255.0) + offset;
}

@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) gid: vec3u) {
  let idx = gid.x;
  if (idx < constants.size.x) {
    let src = in_pixels[idx];
    let gamma_rgb = apply_gamma_pwl(src.rgb);
    let pwl_direct = xe_linear_to_pwl_gamma(src.r);
    out_pixels[idx] = vec4f(gamma_rgb.x, gamma_rgb.y, gamma_rgb.z, pwl_direct);
  }
}
)";

struct Vec4 {
  float x, y, z, w;
};

struct PwlEntryCpu {
  uint32_t base;
  uint32_t slope;
};

}  // namespace

void TestTableGamma(GpuTest& gpu) {
  std::printf("Testing 256-entry Table Gamma on GPU compute vs CPU reference...\n");

  auto module = gpu.Module(kApplyGammaTableWgsl);
  wgpu::ComputePipelineDescriptor p_desc;
  p_desc.compute.module = module;
  p_desc.compute.entryPoint = "main";
  auto pipeline = gpu.device.CreateComputePipeline(&p_desc);
  GPU_CHECK(pipeline);

  // Set up 256-entry ramp table (with a known curve: pow(x, 1/2.2) for R, pow(x, 1/2.0) for G, linear for B)
  std::vector<Vec4> ramp(256);
  for (size_t i = 0; i < 256; ++i) {
    float x = float(i) / 255.0f;
    // Xenos ramp layout: B in .z, G in .y, R in .x
    ramp[i].z = std::pow(x, 1.0f / 2.2f);  // R
    ramp[i].y = std::pow(x, 1.0f / 2.0f);  // G
    ramp[i].x = x;                         // B
    ramp[i].w = 1.0f;
  }

  // Create test input pixels across [0, 1]
  uint32_t pixel_count = 512;
  std::vector<Vec4> in_pixels(pixel_count);
  for (uint32_t i = 0; i < pixel_count; ++i) {
    float t = float(i) / float(pixel_count - 1);
    in_pixels[i] = {t, t, t, 1.0f};
  }

  uint32_t constants[2] = {pixel_count, 1};
  auto const_buf = gpu.Buffer(sizeof(constants), wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst, constants);
  auto in_buf = gpu.Buffer(in_pixels.size() * sizeof(Vec4), wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst, in_pixels.data());
  auto ramp_buf = gpu.Buffer(ramp.size() * sizeof(Vec4), wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst, ramp.data());
  auto out_buf = gpu.Buffer(in_pixels.size() * sizeof(Vec4), wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc);

  wgpu::BindGroupEntry entries[4];
  entries[0].binding = 0; entries[0].buffer = const_buf; entries[0].size = sizeof(constants);
  entries[1].binding = 1; entries[1].buffer = in_buf; entries[1].size = in_pixels.size() * sizeof(Vec4);
  entries[2].binding = 2; entries[2].buffer = ramp_buf; entries[2].size = ramp.size() * sizeof(Vec4);
  entries[3].binding = 3; entries[3].buffer = out_buf; entries[3].size = in_pixels.size() * sizeof(Vec4);

  wgpu::BindGroupDescriptor bg_desc;
  bg_desc.layout = pipeline.GetBindGroupLayout(0);
  bg_desc.entryCount = 4;
  bg_desc.entries = entries;
  auto bg = gpu.device.CreateBindGroup(&bg_desc);

  auto encoder = gpu.device.CreateCommandEncoder();
  auto pass = encoder.BeginComputePass();
  pass.SetPipeline(pipeline);
  pass.SetBindGroup(0, bg);
  pass.DispatchWorkgroups((pixel_count + 63) / 64);
  pass.End();
  auto commands = encoder.Finish();
  gpu.device.GetQueue().Submit(1, &commands);

  auto bytes = gpu.Read(out_buf, in_pixels.size() * sizeof(Vec4));
  std::vector<Vec4> results(pixel_count);
  std::memcpy(results.data(), bytes.data(), bytes.size());

  // Compare with CPU table lookup
  for (uint32_t i = 0; i < pixel_count; ++i) {
    uint32_t idx = uint32_t(in_pixels[i].x * 255.0f + 0.5f);
    float exp_r = ramp[idx].z;
    float exp_g = ramp[idx].y;
    float exp_b = ramp[idx].x;

    TEST_CHECK(std::abs(results[i].x - exp_r) < 1e-5f);
    TEST_CHECK(std::abs(results[i].y - exp_g) < 1e-5f);
    TEST_CHECK(std::abs(results[i].z - exp_b) < 1e-5f);
    TEST_CHECK(results[i].w == 1.0f);
  }

  std::printf("PASS: 256-entry Table Gamma (CPU and GPU match perfectly)\n");
}

void TestPwlGamma(GpuTest& gpu) {
  std::printf("Testing 128-segment PWL Gamma on GPU compute vs CPU reference...\n");

  auto module = gpu.Module(kApplyGammaPwlWgsl);
  wgpu::ComputePipelineDescriptor p_desc;
  p_desc.compute.module = module;
  p_desc.compute.entryPoint = "main";
  auto pipeline = gpu.device.CreateComputePipeline(&p_desc);
  GPU_CHECK(pipeline);

  // Build a standard sRGB / linear PWL ramp with 128 segments.
  // 128 segments * 3 channels (BGR) = 384 PwlEntry entries.
  std::vector<PwlEntryCpu> ramp(128 * 3);
  for (uint32_t seg = 0; seg < 128; ++seg) {
    for (uint32_t ch = 0; ch < 3; ++ch) {
      uint32_t base = seg * 512;
      uint32_t slope = 512;
      ramp[seg * 3 + ch] = {base, slope};
    }
  }

  uint32_t pixel_count = 512;
  std::vector<Vec4> in_pixels(pixel_count);
  for (uint32_t i = 0; i < pixel_count; ++i) {
    float t = float(i) / float(pixel_count - 1);
    in_pixels[i] = {t, t, t, 1.0f};
  }

  uint32_t constants[2] = {pixel_count, 1};
  auto const_buf = gpu.Buffer(sizeof(constants), wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst, constants);
  auto in_buf = gpu.Buffer(in_pixels.size() * sizeof(Vec4), wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst, in_pixels.data());
  auto ramp_buf = gpu.Buffer(ramp.size() * sizeof(PwlEntryCpu), wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst, ramp.data());
  auto out_buf = gpu.Buffer(in_pixels.size() * sizeof(Vec4), wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc);

  wgpu::BindGroupEntry entries[4];
  entries[0].binding = 0; entries[0].buffer = const_buf; entries[0].size = sizeof(constants);
  entries[1].binding = 1; entries[1].buffer = in_buf; entries[1].size = in_pixels.size() * sizeof(Vec4);
  entries[2].binding = 2; entries[2].buffer = ramp_buf; entries[2].size = ramp.size() * sizeof(PwlEntryCpu);
  entries[3].binding = 3; entries[3].buffer = out_buf; entries[3].size = in_pixels.size() * sizeof(Vec4);

  wgpu::BindGroupDescriptor bg_desc;
  bg_desc.layout = pipeline.GetBindGroupLayout(0);
  bg_desc.entryCount = 4;
  bg_desc.entries = entries;
  auto bg = gpu.device.CreateBindGroup(&bg_desc);

  auto encoder = gpu.device.CreateCommandEncoder();
  auto pass = encoder.BeginComputePass();
  pass.SetPipeline(pipeline);
  pass.SetBindGroup(0, bg);
  pass.DispatchWorkgroups((pixel_count + 63) / 64);
  pass.End();
  auto commands = encoder.Finish();
  gpu.device.GetQueue().Submit(1, &commands);

  auto bytes = gpu.Read(out_buf, in_pixels.size() * sizeof(Vec4));
  std::vector<Vec4> results(pixel_count);
  std::memcpy(results.data(), bytes.data(), bytes.size());

  // Verify GPU PWL evaluation against CPU reference
  for (uint32_t i = 0; i < pixel_count; ++i) {
    float c = in_pixels[i].x;
    uint32_t c_1023 = uint32_t(c * 1023.0f + 0.5f);
    uint32_t seg = c_1023 >> 3;
    uint32_t frac = c_1023 & 7;
    float expected = std::clamp((float(seg * 512) + float(frac * 512) * 0.125f) * 1.52737048e-5f, 0.0f, 1.0f);

    TEST_CHECK(std::abs(results[i].x - expected) < 1e-4f);
    TEST_CHECK(std::abs(results[i].y - expected) < 1e-4f);
    TEST_CHECK(std::abs(results[i].z - expected) < 1e-4f);

    // Verify mathematical LinearToPWLGamma matches SDK C++ reference LinearToPWLGamma exactly
    float cpu_pwl = xenos::LinearToPWLGamma(c);
    float gpu_pwl = results[i].w;
    TEST_CHECK(std::abs(gpu_pwl - cpu_pwl) < 1e-5f);
  }

  std::printf("PASS: 128-segment PWL Gamma (CPU LinearToPWLGamma and GPU compute match)\n");
}

int main(int argc, char** argv) {
  rex::InitLogging();
  unsigned adapter = 0;
  if (argc > 1) {
    adapter = unsigned(std::atoi(argv[1]));
  }

  std::printf("Running gamma tests on adapter %u...\n", adapter);
  GpuTest gpu(adapter);

  TestTableGamma(gpu);
  TestPwlGamma(gpu);

  std::printf("All gamma tests PASSED on adapter %u (0 validation errors)!\n", adapter);
  return 0;
}
