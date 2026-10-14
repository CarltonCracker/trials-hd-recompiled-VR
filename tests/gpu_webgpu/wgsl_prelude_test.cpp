// rexgpu-webgpu - Unit tests for wgsl_prelude.wgsl helpers (S-18).
// Differential tests executing WGSL helpers on GPU compute vs SDK C++ references.

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <rex/graphics/xenos.h>
#include <rex/logging.h>

#include "gpu_test.h"
#include "wgsl/wgsl_prelude.wgsl.h"

#define TEST_CHECK(cond)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      std::exit(1);                                                            \
    }                                                                          \
  } while (false)

using namespace rex::graphics;

namespace {

// Run a compute pipeline over an array of uint32_t inputs producing uint32_t outputs.
std::vector<uint32_t> RunComputeU32ToU32(GpuTest& gpu, const std::string& shader_code,
                                        const std::vector<uint32_t>& inputs) {
  auto module = gpu.Module(shader_code);

  wgpu::ComputePipelineDescriptor pipeline_desc;
  pipeline_desc.compute.module = module;
  pipeline_desc.compute.entryPoint = "main";
  auto pipeline = gpu.device.CreateComputePipeline(&pipeline_desc);
  GPU_CHECK(pipeline);

  size_t byte_size = inputs.size() * sizeof(uint32_t);
  auto in_buf = gpu.Buffer(byte_size, wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst, inputs.data());
  auto out_buf = gpu.Buffer(byte_size, wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc);

  wgpu::BindGroupEntry entries[2];
  entries[0].binding = 0;
  entries[0].buffer = in_buf;
  entries[0].size = byte_size;
  entries[1].binding = 1;
  entries[1].buffer = out_buf;
  entries[1].size = byte_size;

  wgpu::BindGroupDescriptor bg_desc;
  bg_desc.layout = pipeline.GetBindGroupLayout(0);
  bg_desc.entryCount = 2;
  bg_desc.entries = entries;
  auto bg = gpu.device.CreateBindGroup(&bg_desc);

  auto encoder = gpu.device.CreateCommandEncoder();
  auto pass = encoder.BeginComputePass();
  pass.SetPipeline(pipeline);
  pass.SetBindGroup(0, bg);
  pass.DispatchWorkgroups(uint32_t((inputs.size() + 63) / 64));
  pass.End();
  auto commands = encoder.Finish();
  gpu.device.GetQueue().Submit(1, &commands);

  auto raw_bytes = gpu.Read(out_buf, byte_size);
  std::vector<uint32_t> results(inputs.size());
  std::memcpy(results.data(), raw_bytes.data(), byte_size);
  return results;
}

// Run a compute pipeline over an array of uint32_t inputs producing float outputs.
std::vector<float> RunComputeU32ToF32(GpuTest& gpu, const std::string& shader_code,
                                     const std::vector<uint32_t>& inputs) {
  auto module = gpu.Module(shader_code);

  wgpu::ComputePipelineDescriptor pipeline_desc;
  pipeline_desc.compute.module = module;
  pipeline_desc.compute.entryPoint = "main";
  auto pipeline = gpu.device.CreateComputePipeline(&pipeline_desc);
  GPU_CHECK(pipeline);

  size_t in_size = inputs.size() * sizeof(uint32_t);
  size_t out_size = inputs.size() * sizeof(float);
  auto in_buf = gpu.Buffer(in_size, wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst, inputs.data());
  auto out_buf = gpu.Buffer(out_size, wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc);

  wgpu::BindGroupEntry entries[2];
  entries[0].binding = 0;
  entries[0].buffer = in_buf;
  entries[0].size = in_size;
  entries[1].binding = 1;
  entries[1].buffer = out_buf;
  entries[1].size = out_size;

  wgpu::BindGroupDescriptor bg_desc;
  bg_desc.layout = pipeline.GetBindGroupLayout(0);
  bg_desc.entryCount = 2;
  bg_desc.entries = entries;
  auto bg = gpu.device.CreateBindGroup(&bg_desc);

  auto encoder = gpu.device.CreateCommandEncoder();
  auto pass = encoder.BeginComputePass();
  pass.SetPipeline(pipeline);
  pass.SetBindGroup(0, bg);
  pass.DispatchWorkgroups(uint32_t((inputs.size() + 63) / 64));
  pass.End();
  auto commands = encoder.Finish();
  gpu.device.GetQueue().Submit(1, &commands);

  auto raw_bytes = gpu.Read(out_buf, out_size);
  std::vector<float> results(inputs.size());
  std::memcpy(results.data(), raw_bytes.data(), out_size);
  return results;
}

// Run a compute pipeline over an array of float inputs producing uint32_t outputs.
std::vector<uint32_t> RunComputeF32ToU32(GpuTest& gpu, const std::string& shader_code,
                                        const std::vector<float>& inputs) {
  auto module = gpu.Module(shader_code);

  wgpu::ComputePipelineDescriptor pipeline_desc;
  pipeline_desc.compute.module = module;
  pipeline_desc.compute.entryPoint = "main";
  auto pipeline = gpu.device.CreateComputePipeline(&pipeline_desc);
  GPU_CHECK(pipeline);

  size_t in_size = inputs.size() * sizeof(float);
  size_t out_size = inputs.size() * sizeof(uint32_t);
  auto in_buf = gpu.Buffer(in_size, wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst, inputs.data());
  auto out_buf = gpu.Buffer(out_size, wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc);

  wgpu::BindGroupEntry entries[2];
  entries[0].binding = 0;
  entries[0].buffer = in_buf;
  entries[0].size = in_size;
  entries[1].binding = 1;
  entries[1].buffer = out_buf;
  entries[1].size = out_size;

  wgpu::BindGroupDescriptor bg_desc;
  bg_desc.layout = pipeline.GetBindGroupLayout(0);
  bg_desc.entryCount = 2;
  bg_desc.entries = entries;
  auto bg = gpu.device.CreateBindGroup(&bg_desc);

  auto encoder = gpu.device.CreateCommandEncoder();
  auto pass = encoder.BeginComputePass();
  pass.SetPipeline(pipeline);
  pass.SetBindGroup(0, bg);
  pass.DispatchWorkgroups(uint32_t((inputs.size() + 63) / 64));
  pass.End();
  auto commands = encoder.Finish();
  gpu.device.GetQueue().Submit(1, &commands);

  auto raw_bytes = gpu.Read(out_buf, out_size);
  std::vector<uint32_t> results(inputs.size());
  std::memcpy(results.data(), raw_bytes.data(), out_size);
  return results;
}

// Run a compute pipeline over an array of uint32_t inputs producing vec4f (4 floats each) outputs.
std::vector<float> RunComputeU32ToVec4f(GpuTest& gpu, const std::string& shader_code,
                                       const std::vector<uint32_t>& inputs) {
  auto module = gpu.Module(shader_code);

  wgpu::ComputePipelineDescriptor pipeline_desc;
  pipeline_desc.compute.module = module;
  pipeline_desc.compute.entryPoint = "main";
  auto pipeline = gpu.device.CreateComputePipeline(&pipeline_desc);
  GPU_CHECK(pipeline);

  size_t in_size = inputs.size() * sizeof(uint32_t);
  size_t out_size = inputs.size() * 4 * sizeof(float);
  auto in_buf = gpu.Buffer(in_size, wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst, inputs.data());
  auto out_buf = gpu.Buffer(out_size, wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc);

  wgpu::BindGroupEntry entries[2];
  entries[0].binding = 0;
  entries[0].buffer = in_buf;
  entries[0].size = in_size;
  entries[1].binding = 1;
  entries[1].buffer = out_buf;
  entries[1].size = out_size;

  wgpu::BindGroupDescriptor bg_desc;
  bg_desc.layout = pipeline.GetBindGroupLayout(0);
  bg_desc.entryCount = 2;
  bg_desc.entries = entries;
  auto bg = gpu.device.CreateBindGroup(&bg_desc);

  auto encoder = gpu.device.CreateCommandEncoder();
  auto pass = encoder.BeginComputePass();
  pass.SetPipeline(pipeline);
  pass.SetBindGroup(0, bg);
  pass.DispatchWorkgroups(uint32_t((inputs.size() + 63) / 64));
  pass.End();
  auto commands = encoder.Finish();
  gpu.device.GetQueue().Submit(1, &commands);

  auto raw_bytes = gpu.Read(out_buf, out_size);
  std::vector<float> results(inputs.size() * 4);
  std::memcpy(results.data(), raw_bytes.data(), out_size);
  return results;
}

// Run a compute pipeline over an array of float inputs producing float outputs.
std::vector<float> RunComputeF32ToF32(GpuTest& gpu, const std::string& shader_code,
                                     const std::vector<float>& inputs) {
  auto module = gpu.Module(shader_code);

  wgpu::ComputePipelineDescriptor pipeline_desc;
  pipeline_desc.compute.module = module;
  pipeline_desc.compute.entryPoint = "main";
  auto pipeline = gpu.device.CreateComputePipeline(&pipeline_desc);
  GPU_CHECK(pipeline);

  size_t size = inputs.size() * sizeof(float);
  auto in_buf = gpu.Buffer(size, wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst, inputs.data());
  auto out_buf = gpu.Buffer(size, wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc);

  wgpu::BindGroupEntry entries[2];
  entries[0].binding = 0;
  entries[0].buffer = in_buf;
  entries[0].size = size;
  entries[1].binding = 1;
  entries[1].buffer = out_buf;
  entries[1].size = size;

  wgpu::BindGroupDescriptor bg_desc;
  bg_desc.layout = pipeline.GetBindGroupLayout(0);
  bg_desc.entryCount = 2;
  bg_desc.entries = entries;
  auto bg = gpu.device.CreateBindGroup(&bg_desc);

  auto encoder = gpu.device.CreateCommandEncoder();
  auto pass = encoder.BeginComputePass();
  pass.SetPipeline(pipeline);
  pass.SetBindGroup(0, bg);
  pass.DispatchWorkgroups(uint32_t((inputs.size() + 63) / 64));
  pass.End();
  auto commands = encoder.Finish();
  gpu.device.GetQueue().Submit(1, &commands);

  auto raw_bytes = gpu.Read(out_buf, size);
  std::vector<float> results(inputs.size());
  std::memcpy(results.data(), raw_bytes.data(), size);
  return results;
}

}  // namespace

void TestEndianSwaps(GpuTest& gpu) {
  std::printf("Testing Endian Swaps (8in16, 8in32, 16in32)...\n");

  std::string shader = std::string(kWgsl_wgsl_prelude) + R"(
@group(0) @binding(0) var<storage, read> in_data: array<vec2u>;
@group(0) @binding(1) var<storage, read_write> out_data: array<u32>;

@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) gid: vec3u) {
  let idx = gid.x;
  if (idx < arrayLength(&in_data)) {
    let val = in_data[idx].x;
    let mode = in_data[idx].y;
    out_data[idx] = xe_endian_swap(val, mode);
  }
}
)";

  std::vector<uint32_t> test_words = {
      0x11223344, 0xAABBCCDD, 0x01020304, 0x00000000, 0xFFFFFFFF, 0x12345678, 0xDEADBEEF,
  };

  std::vector<uint32_t> inputs;  // pairs of (val, mode)
  for (uint32_t word : test_words) {
    for (uint32_t mode = 0; mode < 4; ++mode) {
      inputs.push_back(word);
      inputs.push_back(mode);
    }
  }

  // Double size for pairs
  size_t count = inputs.size() / 2;
  auto module = gpu.Module(shader);
  wgpu::ComputePipelineDescriptor pipeline_desc;
  pipeline_desc.compute.module = module;
  pipeline_desc.compute.entryPoint = "main";
  auto pipeline = gpu.device.CreateComputePipeline(&pipeline_desc);
  GPU_CHECK(pipeline);

  size_t in_size = inputs.size() * sizeof(uint32_t);
  size_t out_size = count * sizeof(uint32_t);
  auto in_buf = gpu.Buffer(in_size, wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst, inputs.data());
  auto out_buf = gpu.Buffer(out_size, wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc);

  wgpu::BindGroupEntry entries[2];
  entries[0].binding = 0;
  entries[0].buffer = in_buf;
  entries[0].size = in_size;
  entries[1].binding = 1;
  entries[1].buffer = out_buf;
  entries[1].size = out_size;

  wgpu::BindGroupDescriptor bg_desc;
  bg_desc.layout = pipeline.GetBindGroupLayout(0);
  bg_desc.entryCount = 2;
  bg_desc.entries = entries;
  auto bg = gpu.device.CreateBindGroup(&bg_desc);

  auto encoder = gpu.device.CreateCommandEncoder();
  auto pass = encoder.BeginComputePass();
  pass.SetPipeline(pipeline);
  pass.SetBindGroup(0, bg);
  pass.DispatchWorkgroups(uint32_t((count + 63) / 64));
  pass.End();
  auto commands = encoder.Finish();
  gpu.device.GetQueue().Submit(1, &commands);

  auto raw_bytes = gpu.Read(out_buf, out_size);
  std::vector<uint32_t> gpu_out(count);
  std::memcpy(gpu_out.data(), raw_bytes.data(), out_size);

  size_t idx = 0;
  for (uint32_t word : test_words) {
    for (uint32_t mode = 0; mode < 4; ++mode) {
      uint32_t expected = word;
      if (mode == 1) {
        // 8in16
        expected = ((word & 0x00FF00FFu) << 8u) | ((word >> 8u) & 0x00FF00FFu);
      } else if (mode == 2) {
        // 8in32
        expected = __builtin_bswap32(word);
      } else if (mode == 3) {
        // 16in32
        expected = (word << 16u) | (word >> 16u);
      }
      TEST_CHECK(gpu_out[idx] == expected);
      ++idx;
    }
  }

  std::printf("PASS: Endian Swaps\n");
}

void TestFloat20e4To32(GpuTest& gpu) {
  std::printf("Testing Float20e4 (depth) -> Float32 conversion on GPU vs CPU reference...\n");

  std::string shader = std::string(kWgsl_wgsl_prelude) + R"(
@group(0) @binding(0) var<storage, read> in_data: array<u32>;
@group(0) @binding(1) var<storage, read_write> out_data: array<f32>;

@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) gid: vec3u) {
  let idx = gid.x;
  if (idx < arrayLength(&in_data)) {
    out_data[idx] = xe_float20e4_to_32(in_data[idx]);
  }
}
)";

  std::vector<uint32_t> test_cases;
  // Edge values
  test_cases.push_back(0);
  test_cases.push_back(1);
  test_cases.push_back(0x7FFFF);       // max denormal
  test_cases.push_back(0x80000);       // min normal
  test_cases.push_back(0xFFFFFF);      // max 24-bit value (~1.999999)
  test_cases.push_back(0x100000);
  test_cases.push_back(0xC00000);      // 1.0 in 20e4
  test_cases.push_back(0x800000);      // 0.5 in 20e4

  // Sample across the full 24-bit space
  for (uint32_t step = 0; step < 1000; ++step) {
    test_cases.push_back((step * 16777) & 0xFFFFFF);
  }

  auto gpu_results = RunComputeU32ToF32(gpu, shader, test_cases);

  for (size_t i = 0; i < test_cases.size(); ++i) {
    float cpu_ref = xenos::Float20e4To32(test_cases[i]);
    float gpu_val = gpu_results[i];
    uint32_t cpu_bits, gpu_bits;
    std::memcpy(&cpu_bits, &cpu_ref, 4);
    std::memcpy(&gpu_bits, &gpu_val, 4);
    TEST_CHECK(cpu_bits == gpu_bits);
  }

  std::printf("PASS: Float20e4 -> Float32 (exact bit match)\n");
}

void TestFloat32To20e4(GpuTest& gpu) {
  std::printf("Testing Float32 -> Float20e4 conversion on GPU vs CPU reference...\n");

  std::string shader = std::string(kWgsl_wgsl_prelude) + R"(
@group(0) @binding(0) var<storage, read> in_data: array<f32>;
@group(0) @binding(1) var<storage, read_write> out_data: array<u32>;

@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) gid: vec3u) {
  let idx = gid.x;
  if (idx < arrayLength(&in_data)) {
    out_data[idx] = xe_float32_to_20e4(in_data[idx], true);
  }
}
)";

  std::vector<float> test_cases = {
      0.0f, -0.0f, -1.0f, 0.00001f, 0.1f, 0.5f, 0.75f, 0.9999f, 1.0f, 1.5f, 1.999f, 2.0f, 5.0f,
  };
  for (int i = 0; i < 500; ++i) {
    test_cases.push_back(float(i) / 250.0f);  // [0, 2.0]
  }

  auto gpu_results = RunComputeF32ToU32(gpu, shader, test_cases);

  for (size_t i = 0; i < test_cases.size(); ++i) {
    uint32_t cpu_ref = xenos::Float32To20e4(test_cases[i], true);
    uint32_t gpu_val = gpu_results[i];
    TEST_CHECK(cpu_ref == gpu_val);
  }

  std::printf("PASS: Float32 -> Float20e4 (exact bit match)\n");
}

void TestFloat7e3(GpuTest& gpu) {
  std::printf("Testing Float7e3 ↔ Float32 on GPU vs CPU reference...\n");

  // 1. Exhaustive 7e3 -> 32 for all 1024 values
  {
    std::string shader = std::string(kWgsl_wgsl_prelude) + R"(
@group(0) @binding(0) var<storage, read> in_data: array<u32>;
@group(0) @binding(1) var<storage, read_write> out_data: array<f32>;

@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) gid: vec3u) {
  let idx = gid.x;
  if (idx < arrayLength(&in_data)) {
    out_data[idx] = xe_float7e3_to_32(in_data[idx]);
  }
}
)";

    std::vector<uint32_t> all_1024(1024);
    for (uint32_t i = 0; i < 1024; ++i) {
      all_1024[i] = i;
    }

    auto gpu_results = RunComputeU32ToF32(gpu, shader, all_1024);
    for (uint32_t i = 0; i < 1024; ++i) {
      float cpu_ref = xenos::Float7e3To32(i);
      float gpu_val = gpu_results[i];
      uint32_t cpu_bits, gpu_bits;
      std::memcpy(&cpu_bits, &cpu_ref, 4);
      std::memcpy(&gpu_bits, &gpu_val, 4);
      TEST_CHECK(cpu_bits == gpu_bits);
    }
    std::printf("PASS: Float7e3 -> Float32 (all 1024 values match bit-for-bit)\n");
  }

  // 2. Float32 -> 7e3
  {
    std::string shader = std::string(kWgsl_wgsl_prelude) + R"(
@group(0) @binding(0) var<storage, read> in_data: array<f32>;
@group(0) @binding(1) var<storage, read_write> out_data: array<u32>;

@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) gid: vec3u) {
  let idx = gid.x;
  if (idx < arrayLength(&in_data)) {
    out_data[idx] = xe_float32_to_7e3(in_data[idx]);
  }
}
)";

    std::vector<float> test_floats;
    for (uint32_t i = 0; i < 1024; ++i) {
      test_floats.push_back(xenos::Float7e3To32(i));
    }
    test_floats.push_back(0.0f);
    test_floats.push_back(-1.0f);
    test_floats.push_back(35.0f);  // clamps to 31.875

    auto gpu_results = RunComputeF32ToU32(gpu, shader, test_floats);
    // Values converted from 7e3 should round-trip back to the same 7e3
    for (uint32_t i = 0; i < 1024; ++i) {
      TEST_CHECK(gpu_results[i] == i);
    }
    TEST_CHECK(gpu_results[1024] == 0u);
    TEST_CHECK(gpu_results[1025] == 0u);
    TEST_CHECK(gpu_results[1026] == 0x3FFu);  // clamped max
    std::printf("PASS: Float32 -> Float7e3\n");
  }
}

void TestUnpack10_10_10_2(GpuTest& gpu) {
  std::printf("Testing 10:10:10:2 Unpack (unorm and snorm)...\n");

  std::string shader = std::string(kWgsl_wgsl_prelude) + R"(
@group(0) @binding(0) var<storage, read> in_data: array<u32>;
@group(0) @binding(1) var<storage, read_write> out_data: array<vec4f>;

@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) gid: vec3u) {
  let idx = gid.x;
  if (idx < arrayLength(&in_data)) {
    out_data[idx] = xe_unpack_10_10_10_2_unorm(in_data[idx]);
  }
}
)";

  std::vector<uint32_t> test_cases = {
      0x00000000,
      0xFFFFFFFF,
      (3u << 30) | (1023u << 20) | (1023u << 10) | 1023u,
      (1u << 30) | (512u << 20) | (256u << 10) | 128u,
  };

  auto gpu_unorm = RunComputeU32ToVec4f(gpu, shader, test_cases);
  // Check 0
  TEST_CHECK(gpu_unorm[0] == 0.0f && gpu_unorm[1] == 0.0f && gpu_unorm[2] == 0.0f && gpu_unorm[3] == 0.0f);
  // Check max
  TEST_CHECK(std::abs(gpu_unorm[4] - 1.0f) < 1e-5f);
  TEST_CHECK(std::abs(gpu_unorm[5] - 1.0f) < 1e-5f);
  TEST_CHECK(std::abs(gpu_unorm[6] - 1.0f) < 1e-5f);
  TEST_CHECK(std::abs(gpu_unorm[7] - 1.0f) < 1e-5f);

  // Check snorm
  std::string snorm_shader = std::string(kWgsl_wgsl_prelude) + R"(
@group(0) @binding(0) var<storage, read> in_data: array<u32>;
@group(0) @binding(1) var<storage, read_write> out_data: array<vec4f>;

@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) gid: vec3u) {
  let idx = gid.x;
  if (idx < arrayLength(&in_data)) {
    out_data[idx] = xe_unpack_10_10_10_2_snorm(in_data[idx]);
  }
}
)";

  // Positive max (511) and negative min (-512)
  uint32_t pos_max = 511u | (511u << 10) | (511u << 20) | (1u << 30);
  uint32_t neg_min = 512u | (512u << 10) | (512u << 20) | (2u << 30);
  std::vector<uint32_t> snorm_inputs = {0u, pos_max, neg_min};
  auto gpu_snorm = RunComputeU32ToVec4f(gpu, snorm_shader, snorm_inputs);

  // 0 -> 0.0
  TEST_CHECK(gpu_snorm[0] == 0.0f && gpu_snorm[1] == 0.0f && gpu_snorm[2] == 0.0f && gpu_snorm[3] == 0.0f);
  // pos_max -> 1.0
  TEST_CHECK(std::abs(gpu_snorm[4] - 1.0f) < 1e-5f);
  TEST_CHECK(std::abs(gpu_snorm[5] - 1.0f) < 1e-5f);
  TEST_CHECK(std::abs(gpu_snorm[6] - 1.0f) < 1e-5f);
  TEST_CHECK(std::abs(gpu_snorm[7] - 1.0f) < 1e-5f);
  // neg_min -> -1.0
  TEST_CHECK(std::abs(gpu_snorm[8] - (-1.0f)) < 1e-5f);
  TEST_CHECK(std::abs(gpu_snorm[9] - (-1.0f)) < 1e-5f);
  TEST_CHECK(std::abs(gpu_snorm[10] - (-1.0f)) < 1e-5f);
  TEST_CHECK(std::abs(gpu_snorm[11] - (-1.0f)) < 1e-5f);

  std::printf("PASS: 10:10:10:2 Unpack\n");
}

void TestUnpack16_16(GpuTest& gpu) {
  std::printf("Testing 16-bit Unpack (float, snorm, unorm)...\n");

  std::string shader = std::string(kWgsl_wgsl_prelude) + R"(
@group(0) @binding(0) var<storage, read> in_data: array<u32>;
@group(0) @binding(1) var<storage, read_write> out_data: array<vec4f>;

@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) gid: vec3u) {
  let idx = gid.x;
  if (idx < arrayLength(&in_data)) {
    let f = xe_unpack_16_16_float(in_data[idx]);
    let s = xe_unpack_16_16_snorm(in_data[idx]);
    out_data[idx] = vec4f(f.x, f.y, s.x, s.y);
  }
}
)";

  // Half float 1.0 is 0x3C00, 2.0 is 0x4000.
  // Snorm16 32767 is 1.0, -32768 is -1.0.
  uint32_t val1 = 0x3C00 | (0x4000 << 16);
  uint32_t val2 = 32767u | (0x8000u << 16);
  std::vector<uint32_t> inputs = {val1, val2};

  auto gpu_results = RunComputeU32ToVec4f(gpu, shader, inputs);
  // Float 1.0 and 2.0
  TEST_CHECK(std::abs(gpu_results[0] - 1.0f) < 1e-4f);
  TEST_CHECK(std::abs(gpu_results[1] - 2.0f) < 1e-4f);
  // Snorm 1.0 and -1.0
  TEST_CHECK(std::abs(gpu_results[6] - 1.0f) < 1e-4f);
  TEST_CHECK(std::abs(gpu_results[7] - (-1.0f)) < 1e-4f);

  std::printf("PASS: 16-bit Unpack\n");
}

void TestK16_16Scaling(GpuTest& gpu) {
  std::printf("Testing Xenos k_16_16 RT ±32 Scaling...\n");

  std::string shader = std::string(kWgsl_wgsl_prelude) + R"(
@group(0) @binding(0) var<storage, read> in_data: array<f32>;
@group(0) @binding(1) var<storage, read_write> out_data: array<f32>;

@compute @workgroup_size(64)
fn main(@builtin(global_invocation_id) gid: vec3u) {
  let idx = gid.x;
  if (idx < arrayLength(&in_data)) {
    let v2 = vec2f(in_data[idx], in_data[idx]);
    let to_host = xe_scale_k_16_16_to_host(v2);
    let from_host = xe_scale_k_16_16_from_host(to_host);
    out_data[idx] = from_host.x;
  }
}
)";

  std::vector<float> test_values = {-32.0f, -16.0f, -1.0f, 0.0f, 0.5f, 1.0f, 16.0f, 32.0f};
  auto gpu_results = RunComputeF32ToF32(gpu, shader, test_values);

  for (size_t i = 0; i < test_values.size(); ++i) {
    TEST_CHECK(std::abs(gpu_results[i] - test_values[i]) < 1e-5f);
  }

  std::printf("PASS: Xenos k_16_16 RT ±32 Scaling (exact roundtrip)\n");
}

int main(int argc, char** argv) {
  rex::InitLogging();
  unsigned adapter = 0;
  if (argc > 1) {
    adapter = unsigned(std::atoi(argv[1]));
  }

  std::printf("Running wgsl_prelude tests on adapter %u...\n", adapter);
  GpuTest gpu(adapter);

  TestEndianSwaps(gpu);
  TestFloat20e4To32(gpu);
  TestFloat32To20e4(gpu);
  TestFloat7e3(gpu);
  TestUnpack10_10_10_2(gpu);
  TestUnpack16_16(gpu);
  TestK16_16Scaling(gpu);

  std::printf("All wgsl_prelude tests PASSED on adapter %u (0 validation errors)!\n", adapter);
  return 0;
}
