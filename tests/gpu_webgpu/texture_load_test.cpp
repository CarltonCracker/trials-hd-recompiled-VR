// rexgpu-webgpu - Unit tests for texture load compute shaders (S-19).
// Tests texture_load_32bpb, _64bpb, _128bpb, _8bpb, and _depth_float WGSL compute
// shaders vs SDK C++ untiling references (GetTiledOffset2D, GetTiledOffset3D, Float20e4To32).

#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <vector>

#include <rex/graphics/pipeline/texture/util.h>
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
using namespace rex::graphics::texture_util;

namespace {

struct alignas(16) LoadConstants {
  uint32_t is_tiled_3d_endian_scale;
  uint32_t guest_offset;
  uint32_t guest_pitch_aligned;
  uint32_t guest_z_stride_block_rows_aligned;
  uint32_t size_blocks[3];
  uint32_t host_offset;
  uint32_t host_pitch;
  uint32_t height_texels;
  uint32_t pad[2];
};
static_assert(sizeof(LoadConstants) == 48);

#ifndef SHADER_DIR
#define SHADER_DIR "src/gpu_webgpu/shaders"
#endif

std::string ReadShader(const char* filename) {
  std::string path = std::string(SHADER_DIR) + "/" + filename;
  std::ifstream f(path);
  if (!f.is_open()) {
    path = std::string("../../src/gpu_webgpu/shaders/") + filename;
    f.open(path);
  }
  GPU_CHECK(f.is_open());
  std::stringstream ss;
  ss << f.rdbuf();
  return ss.str();
}

// Executes a texture load compute pipeline on the GPU and reads back the destination buffer.
std::vector<uint8_t> RunTextureLoad(GpuTest& gpu, const wgpu::ShaderModule& module,
                                    const LoadConstants& constants, uint32_t group_count_x,
                                    uint32_t group_count_y, uint32_t group_count_z,
                                    const void* src_data, size_t src_size_bytes,
                                    size_t dst_size_bytes) {
  wgpu::ComputePipelineDescriptor p_desc;
  p_desc.compute.module = module;
  p_desc.compute.entryPoint = "main";
  auto pipeline = gpu.device.CreateComputePipeline(&p_desc);
  GPU_CHECK(pipeline);

  auto const_buf = gpu.Buffer(sizeof(LoadConstants),
                              wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
                              &constants);
  auto src_buf = gpu.Buffer(src_size_bytes,
                            wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst,
                            src_data);
  auto dst_buf = gpu.Buffer(dst_size_bytes,
                            wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc);

  wgpu::BindGroupEntry entries[3];
  entries[0].binding = 0; entries[0].buffer = const_buf; entries[0].size = sizeof(LoadConstants);
  entries[1].binding = 1; entries[1].buffer = src_buf; entries[1].size = src_size_bytes;
  entries[2].binding = 2; entries[2].buffer = dst_buf; entries[2].size = dst_size_bytes;

  wgpu::BindGroupDescriptor bg_desc;
  bg_desc.layout = pipeline.GetBindGroupLayout(0);
  bg_desc.entryCount = 3;
  bg_desc.entries = entries;
  auto bg = gpu.device.CreateBindGroup(&bg_desc);

  auto encoder = gpu.device.CreateCommandEncoder();
  auto pass = encoder.BeginComputePass();
  pass.SetPipeline(pipeline);
  pass.SetBindGroup(0, bg);
  pass.DispatchWorkgroups(group_count_x, group_count_y, group_count_z);
  pass.End();
  auto commands = encoder.Finish();
  gpu.device.GetQueue().Submit(1, &commands);

  return gpu.Read(dst_buf, dst_size_bytes);
}

// C++ reference for 32-bit swap
uint32_t Swap32(uint32_t v, uint32_t endian) {
  if (endian == 1) { // 8in16
    return ((v & 0x00FF00FFu) << 8u) | ((v & 0xFF00FF00u) >> 8u);
  } else if (endian == 2) { // 8in32
    return __builtin_bswap32(v);
  } else if (endian == 3) { // 16in32
    return (v << 16u) | (v >> 16u);
  }
  return v;
}

}  // namespace

// -----------------------------------------------------------------------------
// 1. Test texture_load_32bpb
// -----------------------------------------------------------------------------
void TestTextureLoad32bpb(GpuTest& gpu) {
  std::printf("Testing texture_load_32bpb (2D tiled, 3D tiled, linear, endian swaps)...\n");
  auto module = gpu.Module(ReadShader("texture_load_32bpb.wgsl"));

  const uint32_t width = 64;
  const uint32_t height = 64;
  const uint32_t pitch_blocks = 64;
  const size_t total_blocks = width * height;
  const size_t byte_count = total_blocks * 4;

  // 1a. 2D Tiled with various endian modes
  for (uint32_t endian : {0u, 1u, 2u, 3u}) {
    std::vector<uint32_t> src_words(total_blocks);
    for (size_t i = 0; i < total_blocks; ++i) {
      src_words[i] = uint32_t(0x10000000 + i * 7 + endian);
    }

    LoadConstants constants{};
    constants.is_tiled_3d_endian_scale = 1u | (endian << 2u);  // tiled 2D
    constants.guest_offset = 0;
    constants.guest_pitch_aligned = pitch_blocks;
    constants.guest_z_stride_block_rows_aligned = 0;
    constants.size_blocks[0] = width;
    constants.size_blocks[1] = height;
    constants.size_blocks[2] = 1;
    constants.host_offset = 0;
    constants.host_pitch = width * 4;
    constants.height_texels = height;

    uint32_t group_x = (width + 31) / 32;
    uint32_t group_y = (height + 31) / 32;
    auto dst_bytes = RunTextureLoad(gpu, module, constants, group_x, group_y, 1,
                                    src_words.data(), byte_count, byte_count);

    const uint32_t* dst_words = reinterpret_cast<const uint32_t*>(dst_bytes.data());

    for (uint32_t y = 0; y < height; ++y) {
      for (uint32_t x = 0; x < width; ++x) {
        int32_t src_byte_offset = GetTiledOffset2D(int32_t(x), int32_t(y), pitch_blocks, 2);
        uint32_t src_word = src_words[src_byte_offset / 4];
        uint32_t expected = Swap32(src_word, endian);
        uint32_t actual = dst_words[y * width + x];
        TEST_CHECK(actual == expected);
      }
    }
  }

  // 1b. Linear
  {
    std::vector<uint32_t> src_words(total_blocks);
    for (size_t i = 0; i < total_blocks; ++i) {
      src_words[i] = uint32_t(0xA0000000 + i * 3);
    }

    LoadConstants constants{};
    constants.is_tiled_3d_endian_scale = 0;  // linear
    constants.guest_offset = 0;
    constants.guest_pitch_aligned = width * 4;  // bytes for linear
    constants.guest_z_stride_block_rows_aligned = 0;
    constants.size_blocks[0] = width;
    constants.size_blocks[1] = height;
    constants.size_blocks[2] = 1;
    constants.host_offset = 0;
    constants.host_pitch = width * 4;
    constants.height_texels = height;

    uint32_t group_x = (width + 31) / 32;
    uint32_t group_y = (height + 31) / 32;
    auto dst_bytes = RunTextureLoad(gpu, module, constants, group_x, group_y, 1,
                                    src_words.data(), byte_count, byte_count);

    const uint32_t* dst_words = reinterpret_cast<const uint32_t*>(dst_bytes.data());
    for (size_t i = 0; i < total_blocks; ++i) {
      TEST_CHECK(dst_words[i] == src_words[i]);
    }
  }

  // 1c. 3D Tiled (32x32x4)
  {
    const uint32_t d3_w = 32;
    const uint32_t d3_h = 32;
    const uint32_t d3_d = 4;
    const size_t d3_blocks = d3_w * d3_h * d3_d;
    const size_t d3_bytes = d3_blocks * 4;

    std::vector<uint32_t> src_words(d3_blocks);
    for (size_t i = 0; i < d3_blocks; ++i) {
      src_words[i] = uint32_t(0x3D000000 + i);
    }

    LoadConstants constants{};
    constants.is_tiled_3d_endian_scale = 1u | 2u;  // tiled 3D
    constants.guest_offset = 0;
    constants.guest_pitch_aligned = d3_w;
    constants.guest_z_stride_block_rows_aligned = d3_h;
    constants.size_blocks[0] = d3_w;
    constants.size_blocks[1] = d3_h;
    constants.size_blocks[2] = d3_d;
    constants.host_offset = 0;
    constants.host_pitch = d3_w * 4;
    constants.height_texels = d3_h;

    uint32_t group_x = (d3_w + 31) / 32;
    uint32_t group_y = (d3_h + 31) / 32;
    auto dst_bytes = RunTextureLoad(gpu, module, constants, group_x, group_y, d3_d,
                                    src_words.data(), d3_bytes, d3_bytes);

    const uint32_t* dst_words = reinterpret_cast<const uint32_t*>(dst_bytes.data());
    for (uint32_t z = 0; z < d3_d; ++z) {
      for (uint32_t y = 0; y < d3_h; ++y) {
        for (uint32_t x = 0; x < d3_w; ++x) {
          int32_t src_byte_offset = GetTiledOffset3D(int32_t(x), int32_t(y), int32_t(z), d3_w, d3_h, 2);
          uint32_t expected = src_words[src_byte_offset / 4];
          uint32_t actual = dst_words[(z * d3_h + y) * d3_w + x];
          TEST_CHECK(actual == expected);
        }
      }
    }
  }

  std::printf("PASS: texture_load_32bpb\n");
}

// -----------------------------------------------------------------------------
// 2. Test texture_load_64bpb
// -----------------------------------------------------------------------------
void TestTextureLoad64bpb(GpuTest& gpu) {
  std::printf("Testing texture_load_64bpb (BC1/DXT1 8 bytes per block)...\n");
  auto module = gpu.Module(ReadShader("texture_load_64bpb.wgsl"));

  const uint32_t width_blocks = 32;
  const uint32_t height_blocks = 32;
  const size_t total_blocks = width_blocks * height_blocks;
  const size_t byte_count = total_blocks * 8;

  std::vector<uint64_t> src_blocks(total_blocks);
  for (size_t i = 0; i < total_blocks; ++i) {
    src_blocks[i] = 0x6464000000000000ULL | uint64_t(i * 13);
  }

  LoadConstants constants{};
  constants.is_tiled_3d_endian_scale = 1u;  // tiled 2D, endian none
  constants.guest_offset = 0;
  constants.guest_pitch_aligned = width_blocks;
  constants.guest_z_stride_block_rows_aligned = 0;
  constants.size_blocks[0] = width_blocks;
  constants.size_blocks[1] = height_blocks;
  constants.size_blocks[2] = 1;
  constants.host_offset = 0;
  constants.host_pitch = width_blocks * 8;
  constants.height_texels = height_blocks * 4;

  uint32_t group_x = (width_blocks + 15) / 16;
  uint32_t group_y = (height_blocks + 31) / 32;
  auto dst_bytes = RunTextureLoad(gpu, module, constants, group_x, group_y, 1,
                                  src_blocks.data(), byte_count, byte_count);

  const uint64_t* dst_blocks = reinterpret_cast<const uint64_t*>(dst_bytes.data());
  for (uint32_t y = 0; y < height_blocks; ++y) {
    for (uint32_t x = 0; x < width_blocks; ++x) {
      int32_t src_byte_offset = GetTiledOffset2D(int32_t(x), int32_t(y), width_blocks, 3);
      uint64_t expected = src_blocks[src_byte_offset / 8];
      uint64_t actual = dst_blocks[y * width_blocks + x];
      TEST_CHECK(actual == expected);
    }
  }

  std::printf("PASS: texture_load_64bpb\n");
}

// -----------------------------------------------------------------------------
// 3. Test texture_load_128bpb
// -----------------------------------------------------------------------------
void TestTextureLoad128bpb(GpuTest& gpu) {
  std::printf("Testing texture_load_128bpb (BC2/3/5 16 bytes per block)...\n");
  auto module = gpu.Module(ReadShader("texture_load_128bpb.wgsl"));

  const uint32_t width_blocks = 32;
  const uint32_t height_blocks = 32;
  const size_t total_blocks = width_blocks * height_blocks;
  const size_t byte_count = total_blocks * 16;

  struct Block128 { uint64_t lo; uint64_t hi; };
  std::vector<Block128> src_blocks(total_blocks);
  for (size_t i = 0; i < total_blocks; ++i) {
    src_blocks[i].lo = 0x1280000000000000ULL | uint64_t(i * 17);
    src_blocks[i].hi = 0xABCDEF0000000000ULL | uint64_t(i * 19);
  }

  LoadConstants constants{};
  constants.is_tiled_3d_endian_scale = 1u;  // tiled 2D
  constants.guest_offset = 0;
  constants.guest_pitch_aligned = width_blocks;
  constants.guest_z_stride_block_rows_aligned = 0;
  constants.size_blocks[0] = width_blocks;
  constants.size_blocks[1] = height_blocks;
  constants.size_blocks[2] = 1;
  constants.host_offset = 0;
  constants.host_pitch = width_blocks * 16;
  constants.height_texels = height_blocks * 4;

  uint32_t group_x = (width_blocks + 7) / 8;
  uint32_t group_y = (height_blocks + 31) / 32;
  auto dst_bytes = RunTextureLoad(gpu, module, constants, group_x, group_y, 1,
                                  src_blocks.data(), byte_count, byte_count);

  const Block128* dst_blocks = reinterpret_cast<const Block128*>(dst_bytes.data());
  for (uint32_t y = 0; y < height_blocks; ++y) {
    for (uint32_t x = 0; x < width_blocks; ++x) {
      int32_t src_byte_offset = GetTiledOffset2D(int32_t(x), int32_t(y), width_blocks, 4);
      Block128 expected = src_blocks[src_byte_offset / 16];
      Block128 actual = dst_blocks[y * width_blocks + x];
      TEST_CHECK(actual.lo == expected.lo && actual.hi == expected.hi);
    }
  }

  std::printf("PASS: texture_load_128bpb\n");
}

// -----------------------------------------------------------------------------
// 4. Test texture_load_8bpb
// -----------------------------------------------------------------------------
void TestTextureLoad8bpb(GpuTest& gpu) {
  std::printf("Testing texture_load_8bpb (k_8 1 byte per block)...\n");
  auto module = gpu.Module(ReadShader("texture_load_8bpb.wgsl"));

  const uint32_t width_blocks = 64;
  const uint32_t height_blocks = 32;
  const size_t dst_byte_count = width_blocks * height_blocks;
  const size_t src_byte_count = GetTiledAddressUpperBound2D(width_blocks, height_blocks, width_blocks, 0);

  std::vector<uint8_t> src_bytes(src_byte_count);
  for (size_t i = 0; i < src_byte_count; ++i) {
    src_bytes[i] = uint8_t((i * 11 + 3) & 0xFF);
  }

  LoadConstants constants{};
  constants.is_tiled_3d_endian_scale = 1u;  // tiled 2D
  constants.guest_offset = 0;
  constants.guest_pitch_aligned = width_blocks;
  constants.guest_z_stride_block_rows_aligned = 0;
  constants.size_blocks[0] = width_blocks;
  constants.size_blocks[1] = height_blocks;
  constants.size_blocks[2] = 1;
  constants.host_offset = 0;
  constants.host_pitch = width_blocks;
  constants.height_texels = height_blocks;

  uint32_t group_x = (width_blocks + 63) / 64;
  uint32_t group_y = (height_blocks + 31) / 32;
  auto dst_bytes = RunTextureLoad(gpu, module, constants, group_x, group_y, 1,
                                  src_bytes.data(), src_byte_count, dst_byte_count);

  for (uint32_t y = 0; y < height_blocks; ++y) {
    for (uint32_t x = 0; x < width_blocks; ++x) {
      int32_t src_byte_offset = GetTiledOffset2D(int32_t(x), int32_t(y), width_blocks, 0);
      uint8_t expected = src_bytes[src_byte_offset];
      uint8_t actual = dst_bytes[y * width_blocks + x];
      TEST_CHECK(actual == expected);
    }
  }

  std::printf("PASS: texture_load_8bpb\n");
}

// -----------------------------------------------------------------------------
// 5. Test texture_load_depth_float
// -----------------------------------------------------------------------------
void TestTextureLoadDepthFloat(GpuTest& gpu) {
  std::printf("Testing texture_load_depth_float (Float24 20e4 -> IEEE float32)...\n");
  auto module = gpu.Module(ReadShader("texture_load_depth_float.wgsl"));

  const uint32_t width = 64;
  const uint32_t height = 64;
  const uint32_t pitch_blocks = 64;
  const size_t total_blocks = width * height;
  const size_t byte_count = total_blocks * 4;

  std::vector<uint32_t> src_words(total_blocks);
  for (size_t i = 0; i < total_blocks; ++i) {
    // Generate valid Float24 (20e4) values in upper 24 bits:
    // Format: 20 bits mantissa, 4 bits exponent, packed in bits 8..31
    uint32_t mant = uint32_t(i * 101) & 0x000FFFFFu;
    uint32_t exp = uint32_t(i % 16) & 0xFu;
    uint32_t f24 = (exp << 20u) | mant;
    uint32_t stencil = uint32_t(i & 0xFF);
    src_words[i] = (f24 << 8u) | stencil;
  }

  LoadConstants constants{};
  constants.is_tiled_3d_endian_scale = 1u;  // tiled 2D, endian none
  constants.guest_offset = 0;
  constants.guest_pitch_aligned = pitch_blocks;
  constants.guest_z_stride_block_rows_aligned = 0;
  constants.size_blocks[0] = width;
  constants.size_blocks[1] = height;
  constants.size_blocks[2] = 1;
  constants.host_offset = 0;
  constants.host_pitch = width * 4;
  constants.height_texels = height;

  uint32_t group_x = (width + 31) / 32;
  uint32_t group_y = (height + 31) / 32;
  auto dst_bytes = RunTextureLoad(gpu, module, constants, group_x, group_y, 1,
                                  src_words.data(), byte_count, byte_count);

  const uint32_t* dst_words = reinterpret_cast<const uint32_t*>(dst_bytes.data());
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      int32_t src_byte_offset = GetTiledOffset2D(int32_t(x), int32_t(y), pitch_blocks, 2);
      uint32_t raw_word = src_words[src_byte_offset / 4];
      uint32_t f24 = raw_word >> 8u;
      float expected_float = xenos::Float20e4To32(f24);
      uint32_t expected_bits;
      std::memcpy(&expected_bits, &expected_float, 4);

      uint32_t actual_bits = dst_words[y * width + x];
      TEST_CHECK(actual_bits == expected_bits);
    }
  }

  std::printf("PASS: texture_load_depth_float\n");
}

int main(int argc, char** argv) {
  rex::InitLogging();
  unsigned adapter = 0;
  if (argc > 1) {
    adapter = unsigned(std::atoi(argv[1]));
  }

  std::printf("Running texture_load tests on adapter %u...\n", adapter);
  GpuTest gpu(adapter);

  TestTextureLoad32bpb(gpu);
  TestTextureLoad64bpb(gpu);
  TestTextureLoad128bpb(gpu);
  TestTextureLoad8bpb(gpu);
  TestTextureLoadDepthFloat(gpu);

  std::printf("All texture_load tests PASSED on adapter %u (0 validation errors)!\n", adapter);
  return 0;
}
