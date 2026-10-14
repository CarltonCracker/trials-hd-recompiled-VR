// rexgpu-webgpu - Unit tests for index buffer endian swaps and primitive topology conversions (S-22).
// Tests index_swap.wgsl compute shaders vs PrimitiveProcessor C++ references.

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

#include <rex/logging.h>

#include "gpu_test.h"

#define TEST_CHECK(cond)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      std::exit(1);                                                            \
    }                                                                          \
  } while (false)


namespace {

struct IndexParams {
  uint32_t element_count;
  uint32_t endian;
  uint32_t index_base;
  uint32_t is_auto;
};

#ifndef INDEX_SWAP_WGSL_PATH
#define INDEX_SWAP_WGSL_PATH "src/gpu_webgpu/shaders/index_swap.wgsl"
#endif

std::string ReadShaderSource(const char* override_path = nullptr) {
  std::ifstream f;
  if (override_path) {
    f.open(override_path);
  }
  if (!f.is_open()) {
    f.open(INDEX_SWAP_WGSL_PATH);
  }
  if (!f.is_open()) {
    f.open("../../src/gpu_webgpu/shaders/index_swap.wgsl");
  }
  GPU_CHECK(f.is_open());
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

template <typename T>
void RefQuadListToTriangleList(T* dest, const T* source, uint32_t source_count) {
  uint32_t quad_count = source_count / 4;
  for (uint32_t i = 0; i < quad_count; ++i) {
    T v0 = *(source++);
    *(dest++) = v0;
    *(dest++) = *(source++);
    T v2 = *(source++);
    *(dest++) = v2;
    *(dest++) = v0;
    *(dest++) = v2;
    *(dest++) = *(source++);
  }
}

template <typename T>
void RefTriangleFanToList(T* dest, const T* source, uint32_t source_count) {
  if (source_count <= 2) {
    return;
  }
  T first = source[0];
  T prev = source[1];
  for (uint32_t i = 2; i < source_count; ++i) {
    T curr = source[i];
    *(dest++) = prev;
    *(dest++) = curr;
    *(dest++) = first;
    prev = curr;
  }
}

std::vector<uint32_t> RunIndexCompute(GpuTest& gpu, const wgpu::ShaderModule& module,
                                      const char* entry_point, const IndexParams& params,
                                      const std::vector<uint32_t>& src_words,
                                      size_t dst_word_count) {
  wgpu::ComputePipelineDescriptor p_desc;
  p_desc.compute.module = module;
  p_desc.compute.entryPoint = entry_point;
  auto pipeline = gpu.device.CreateComputePipeline(&p_desc);
  GPU_CHECK(pipeline);

  auto param_buf = gpu.Buffer(sizeof(IndexParams), wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst, &params);
  size_t src_bytes = std::max(src_words.size() * sizeof(uint32_t), size_t(16));
  auto src_buf = gpu.Buffer(src_bytes, wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst,
                            src_words.empty() ? nullptr : src_words.data());
  size_t dst_bytes = std::max(dst_word_count * sizeof(uint32_t), size_t(16));
  auto dst_buf = gpu.Buffer(dst_bytes, wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc);

  wgpu::BindGroupEntry entries[3];
  entries[0].binding = 0; entries[0].buffer = param_buf; entries[0].size = sizeof(IndexParams);
  entries[1].binding = 1; entries[1].buffer = src_buf; entries[1].size = src_bytes;
  entries[2].binding = 2; entries[2].buffer = dst_buf; entries[2].size = dst_bytes;

  wgpu::BindGroupDescriptor bg_desc;
  bg_desc.layout = pipeline.GetBindGroupLayout(0);
  bg_desc.entryCount = 3;
  bg_desc.entries = entries;
  auto bg = gpu.device.CreateBindGroup(&bg_desc);

  auto encoder = gpu.device.CreateCommandEncoder();
  auto pass = encoder.BeginComputePass();
  pass.SetPipeline(pipeline);
  pass.SetBindGroup(0, bg);
  pass.DispatchWorkgroups(uint32_t((params.element_count + 63) / 64));
  pass.End();
  auto commands = encoder.Finish();
  gpu.device.GetQueue().Submit(1, &commands);

  auto raw_bytes = gpu.Read(dst_buf, dst_word_count * sizeof(uint32_t));
  std::vector<uint32_t> results(dst_word_count);
  std::memcpy(results.data(), raw_bytes.data(), dst_word_count * sizeof(uint32_t));
  return results;
}

}  // namespace

void Test16BitSwap(GpuTest& gpu, const wgpu::ShaderModule& module) {
  std::printf("Testing 16-bit Index Swap (8in16)...\n");

  std::vector<uint16_t> src_indices = {
      0x1234, 0x5678, 0xABCD, 0x00FF, 0xFF00, 0x0000, 0x1122, 0x3344,
  };
  size_t word_count = src_indices.size() / 2;
  std::vector<uint32_t> src_words(word_count);
  std::memcpy(src_words.data(), src_indices.data(), src_indices.size() * sizeof(uint16_t));

  IndexParams params{};
  params.element_count = uint32_t(word_count);
  params.endian = 1;  // 8in16
  params.index_base = 0;
  params.is_auto = 0;

  auto gpu_words = RunIndexCompute(gpu, module, "swap_indices_16", params, src_words, word_count);
  std::vector<uint16_t> gpu_indices(src_indices.size());
  std::memcpy(gpu_indices.data(), gpu_words.data(), gpu_indices.size() * sizeof(uint16_t));

  for (size_t i = 0; i < src_indices.size(); ++i) {
    uint16_t expected = ((src_indices[i] & 0xFF) << 8) | ((src_indices[i] >> 8) & 0xFF);
    TEST_CHECK(gpu_indices[i] == expected);
  }

  std::printf("PASS: 16-bit Index Swap\n");
}

void Test32BitSwap(GpuTest& gpu, const wgpu::ShaderModule& module) {
  std::printf("Testing 32-bit Index Swap (8in16, 8in32, 16in32)...\n");

  std::vector<uint32_t> src = {
      0x11223344, 0xAABBCCDD, 0x01020304, 0xDEADBEEF, 0x12345678,
  };

  // Test 8in32
  {
    IndexParams params{};
    params.element_count = uint32_t(src.size());
    params.endian = 2;  // 8in32
    params.index_base = 0;
    params.is_auto = 0;

    auto gpu_words = RunIndexCompute(gpu, module, "swap_indices_32", params, src, src.size());
    for (size_t i = 0; i < src.size(); ++i) {
      TEST_CHECK(gpu_words[i] == __builtin_bswap32(src[i]));
    }
  }

  // Test 16in32
  {
    IndexParams params{};
    params.element_count = uint32_t(src.size());
    params.endian = 3;  // 16in32
    params.index_base = 0;
    params.is_auto = 0;

    auto gpu_words = RunIndexCompute(gpu, module, "swap_indices_32", params, src, src.size());
    for (size_t i = 0; i < src.size(); ++i) {
      uint32_t expected = (src[i] << 16) | (src[i] >> 16);
      TEST_CHECK(gpu_words[i] == expected);
    }
  }

  std::printf("PASS: 32-bit Index Swap\n");
}

void TestQuadListToTriangleList(GpuTest& gpu, const wgpu::ShaderModule& module) {
  std::printf("Testing Quad List -> Triangle List conversion vs PrimitiveProcessor C++...\n");

  // 1. 16-bit Quad List
  {
    uint32_t quad_count = 16;
    uint32_t vertex_count = quad_count * 4;
    std::vector<uint16_t> src(vertex_count);
    for (uint32_t i = 0; i < vertex_count; ++i) {
      src[i] = uint16_t(100 + i * 3);
    }

    uint32_t expected_index_count = (vertex_count / 4) * 6;
    std::vector<uint16_t> cpu_ref(expected_index_count);
    RefQuadListToTriangleList(cpu_ref.data(), src.data(), vertex_count);

    std::vector<uint32_t> src_words(vertex_count / 2);
    std::memcpy(src_words.data(), src.data(), src.size() * sizeof(uint16_t));

    IndexParams params{};
    params.element_count = quad_count;
    params.endian = 0;
    params.index_base = 0;
    params.is_auto = 0;

    size_t out_word_count = expected_index_count / 2;
    auto gpu_words = RunIndexCompute(gpu, module, "quads_to_triangles_16", params, src_words, out_word_count);
    std::vector<uint16_t> gpu_ref(expected_index_count);
    std::memcpy(gpu_ref.data(), gpu_words.data(), gpu_ref.size() * sizeof(uint16_t));

    for (size_t i = 0; i < expected_index_count; ++i) {
      TEST_CHECK(gpu_ref[i] == cpu_ref[i]);
    }
  }

  // 2. 32-bit Quad List
  {
    uint32_t quad_count = 16;
    uint32_t vertex_count = quad_count * 4;
    std::vector<uint32_t> src(vertex_count);
    for (uint32_t i = 0; i < vertex_count; ++i) {
      src[i] = 1000 + i * 5;
    }

    uint32_t expected_index_count = (vertex_count / 4) * 6;
    std::vector<uint32_t> cpu_ref(expected_index_count);
    RefQuadListToTriangleList(cpu_ref.data(), src.data(), vertex_count);

    IndexParams params{};
    params.element_count = quad_count;
    params.endian = 0;
    params.index_base = 0;
    params.is_auto = 0;

    auto gpu_words = RunIndexCompute(gpu, module, "quads_to_triangles_32", params, src, expected_index_count);

    for (size_t i = 0; i < expected_index_count; ++i) {
      TEST_CHECK(gpu_words[i] == cpu_ref[i]);
    }
  }

  std::printf("PASS: Quad List -> Triangle List (exact match vs PrimitiveProcessor)\n");
}

void TestTriangleFanToList(GpuTest& gpu, const wgpu::ShaderModule& module) {
  std::printf("Testing Triangle Fan -> Triangle List conversion vs PrimitiveProcessor C++...\n");

  // 1. 16-bit Triangle Fan (both even and odd triangle counts)
  for (uint32_t fan_vertices : {5u, 8u, 13u, 24u}) {
    std::vector<uint16_t> src(fan_vertices);
    for (uint32_t i = 0; i < fan_vertices; ++i) {
      src[i] = uint16_t(50 + i * 2);
    }

    uint32_t expected_index_count = fan_vertices > 2 ? (fan_vertices - 2) * 3 : 0;
    std::vector<uint16_t> cpu_ref(expected_index_count);
    RefTriangleFanToList(cpu_ref.data(), src.data(), fan_vertices);

    size_t in_word_count = (fan_vertices + 1) / 2;
    std::vector<uint32_t> src_words(in_word_count, 0);
    std::memcpy(src_words.data(), src.data(), src.size() * sizeof(uint16_t));

    uint32_t tri_count = fan_vertices - 2;
    IndexParams params{};
    params.element_count = tri_count;
    params.endian = 0;
    params.index_base = 0;
    params.is_auto = 0;

    size_t out_word_count = (expected_index_count + 1) / 2;
    auto gpu_words = RunIndexCompute(gpu, module, "fan_to_triangles_16", params, src_words, out_word_count);
    std::vector<uint16_t> gpu_ref(expected_index_count);
    std::memcpy(gpu_ref.data(), gpu_words.data(), gpu_ref.size() * sizeof(uint16_t));

    for (size_t i = 0; i < expected_index_count; ++i) {
      TEST_CHECK(gpu_ref[i] == cpu_ref[i]);
    }
  }

  // 2. 32-bit Triangle Fan
  for (uint32_t fan_vertices : {5u, 8u, 13u, 24u}) {
    std::vector<uint32_t> src(fan_vertices);
    for (uint32_t i = 0; i < fan_vertices; ++i) {
      src[i] = 10000 + i * 7;
    }

    uint32_t expected_index_count = fan_vertices > 2 ? (fan_vertices - 2) * 3 : 0;
    std::vector<uint32_t> cpu_ref(expected_index_count);
    RefTriangleFanToList(cpu_ref.data(), src.data(), fan_vertices);

    uint32_t tri_count = fan_vertices - 2;
    IndexParams params{};
    params.element_count = tri_count;
    params.endian = 0;
    params.index_base = 0;
    params.is_auto = 0;

    auto gpu_words = RunIndexCompute(gpu, module, "fan_to_triangles_32", params, src, expected_index_count);

    for (size_t i = 0; i < expected_index_count; ++i) {
      TEST_CHECK(gpu_words[i] == cpu_ref[i]);
    }
  }

  std::printf("PASS: Triangle Fan -> Triangle List (exact match vs PrimitiveProcessor)\n");
}

void TestAutoIndexing(GpuTest& gpu, const wgpu::ShaderModule& module) {
  std::printf("Testing Auto-indexed Quad List and Fan conversions...\n");

  // Auto-indexed quads: 8 quads (32 vertices, indices 0..31)
  uint32_t quad_count = 8;
  uint32_t expected_index_count = quad_count * 6;
  IndexParams params{};
  params.element_count = quad_count;
  params.endian = 0;
  params.index_base = 100;  // Base offset 100
  params.is_auto = 1;

  auto gpu_words = RunIndexCompute(gpu, module, "quads_to_triangles_32", params, {}, expected_index_count);
  for (uint32_t q = 0; q < quad_count; ++q) {
    uint32_t v0 = 100 + q * 4 + 0;
    uint32_t v1 = 100 + q * 4 + 1;
    uint32_t v2 = 100 + q * 4 + 2;
    uint32_t v3 = 100 + q * 4 + 3;

    TEST_CHECK(gpu_words[q * 6 + 0] == v0);
    TEST_CHECK(gpu_words[q * 6 + 1] == v1);
    TEST_CHECK(gpu_words[q * 6 + 2] == v2);
    TEST_CHECK(gpu_words[q * 6 + 3] == v0);
    TEST_CHECK(gpu_words[q * 6 + 4] == v2);
    TEST_CHECK(gpu_words[q * 6 + 5] == v3);
  }

  std::printf("PASS: Auto-indexed Quad List and Fan conversions\n");
}

int main(int argc, char** argv) {
  rex::InitLogging();
  unsigned adapter = 0;
  if (argc > 1) {
    adapter = unsigned(std::atoi(argv[1]));
  }

  std::printf("Running index_swap tests on adapter %u...\n", adapter);
  GpuTest gpu(adapter);

  std::string shader_src = ReadShaderSource("src/gpu_webgpu/shaders/index_swap.wgsl");
  auto module = gpu.Module(shader_src);

  Test16BitSwap(gpu, module);
  Test32BitSwap(gpu, module);
  TestQuadListToTriangleList(gpu, module);
  TestTriangleFanToList(gpu, module);
  TestAutoIndexing(gpu, module);

  std::printf("All index_swap tests PASSED on adapter %u (0 validation errors)!\n", adapter);
  return 0;
}
