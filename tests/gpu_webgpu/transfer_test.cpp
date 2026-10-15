// EDRAM transfer shader (src/gpu_webgpu/shaders/transfer.wgsl): pack/unpack
// round trips per render-target format, and 1x / 4x / depth addressing that
// covers every EDRAM sample of the transferred tiles exactly once. Stencil
// travels in bits 0:7 of depth samples.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
#include "gpu_test.h"

struct Constants {
  uint32_t base_tiles = 0, pitch_tiles = 1, msaa_4x = 0, is_depth = 0, format = 0;
  float output_scale = 1;
  uint32_t scale_x = 1, scale_y = 1, tile_first = 0, tile_count = 0;
  uint32_t row_first = 0, row_end = 0, width = 0, padding[3] = {};
};
static_assert(sizeof(Constants) == 64);

static const char* kTestEntryPoints = R"(
@group(1) @binding(0) var<storage, read> test_in: array<vec4f>;
@group(1) @binding(1) var<storage, read_write> test_out: array<vec4f>;
@group(1) @binding(2) var<storage, read_write> test_hits: array<atomic<u32>>;

@compute @workgroup_size(64)
fn test_round_trip(@builtin(global_invocation_id) id: vec3u) {
  if (id.x < arrayLength(&test_in)) {
    // Depth formats also carry stencil 0xA5 in bits 0:7; z gets it back.
    let packed = pack(test_in[id.x], select(0u, 0xA5u, c.is_depth != 0u));
    test_out[id.x] = unpack(packed);
    if (c.is_depth != 0u) {
      test_out[id.x].z = f32(packed & 0xFFu);
    }
  }
}

@compute @workgroup_size(8, 8)
fn test_coverage(@builtin(global_invocation_id) id: vec3u) {
  if (id.x >= c.width || id.y >= c.row_end) {
    return;
  }
  for (var i = 0u; i < select(1u, 4u, c.msaa_4x != 0u); i++) {
    let address = edram_address(id.xy, i);
    if (address != 0xFFFFFFFFu) {
      atomicAdd(&test_hits[address], 1u);
    }
  }
}
)";

static std::string ReadFile(const char* path) {
  std::ifstream file(path);
  GPU_CHECK(bool(file));
  return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

int main(int argc, char** argv) {
  GpuTest gpu(argc > 1 ? unsigned(std::strtoul(argv[1], nullptr, 10)) : 0);
  auto module = gpu.Module(ReadFile(SHADER_DIR "/../wgsl/wgsl_prelude.wgsl") +
                           ReadFile(SHADER_DIR "/transfer.wgsl") + kTestEntryPoints);
  auto pipeline = [&](const char* entry_point) {
    wgpu::ComputePipelineDescriptor desc;
    desc.compute.module = module;
    desc.compute.entryPoint = entry_point;
    return gpu.device.CreateComputePipeline(&desc);
  };
  auto round_trip_pipeline = pipeline("test_round_trip");
  auto coverage_pipeline = pipeline("test_coverage");
  auto uniforms = gpu.Buffer(sizeof(Constants), wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst);
  auto run = [&](const wgpu::ComputePipeline& p, const Constants& constants,
                 std::vector<wgpu::BindGroupEntry> entries, uint32_t x, uint32_t y) {
    gpu.device.GetQueue().WriteBuffer(uniforms, 0, &constants, sizeof(constants));
    wgpu::BindGroupEntry uniform_entry;
    uniform_entry.binding = 0;
    uniform_entry.buffer = uniforms;
    wgpu::BindGroupDescriptor group0;
    group0.layout = p.GetBindGroupLayout(0);
    group0.entryCount = 1;
    group0.entries = &uniform_entry;
    wgpu::BindGroupDescriptor group1;
    group1.layout = p.GetBindGroupLayout(1);
    group1.entryCount = entries.size();
    group1.entries = entries.data();
    auto encoder = gpu.device.CreateCommandEncoder();
    auto pass = encoder.BeginComputePass();
    pass.SetPipeline(p);
    pass.SetBindGroup(0, gpu.device.CreateBindGroup(&group0));
    pass.SetBindGroup(1, gpu.device.CreateBindGroup(&group1));
    pass.DispatchWorkgroups(x, y);
    pass.End();
    auto commands = encoder.Finish();
    gpu.device.GetQueue().Submit(1, &commands);
  };

  // Round trips: values exactly representable in each format.
  struct Case {
    uint32_t is_depth, format;
    float output_scale;
    std::array<float, 4> value;
    uint32_t components;
  };
  const Case cases[] = {
      {0, 0, 1, {0, 1, 128 / 255.0f, 7 / 255.0f}, 4},                // k_8_8_8_8
      {0, 2, 1, {1, 0, 512 / 1023.0f, 2 / 3.0f}, 4},                 // k_2_10_10_10
      {0, 3, 1, {0.5f, 31, 2.25f, 1 / 3.0f}, 4},                     // k_2_10_10_10_FLOAT (7e3)
      {0, 3, 1, {0, 0.0078125f, 1.0f / 64, 1}, 4},                   // 7e3 small values
      {0, 4, 1 / 32.0f, {-1, 16384 / 32767.0f, 0, 1}, 2},            // k_16_16 on snorm
      {0, 4, 1, {-32 * 32767 / 32767.0f, 3200 * 32 / 32767.0f, 0, 1}, 2},  // k_16_16 on float
      {0, 6, 1, {-2.5f, 1024, 0, 1}, 2},                             // k_16_16_FLOAT
      {0, 14, 1, {-1.0e10f, 0, 0, 1}, 1},                            // k_32_FLOAT
      {1, 1, 1, {0.5f, 0, 0, 0}, 1},                                 // D24FS8
      {1, 1, 1, {1.0f / 3, 0, 0, 0}, 1},                             // D24FS8, rounded below
      {1, 0, 1, {4096 / 16777215.0f, 0, 0, 0}, 1},                   // D24S8
  };
  for (const Case& test_case : cases) {
    Constants constants;
    constants.is_depth = test_case.is_depth;
    constants.format = test_case.format;
    constants.output_scale = test_case.output_scale;
    auto input = gpu.Buffer(16, wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst,
                            test_case.value.data());
    auto output = gpu.Buffer(16, wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc);
    std::vector<wgpu::BindGroupEntry> entries(2);
    entries[0].binding = 0;
    entries[0].buffer = input;
    entries[1].binding = 1;
    entries[1].buffer = output;
    run(round_trip_pipeline, constants, entries, 1, 1);
    auto bytes = gpu.Read(output, 16);
    float result[4];
    std::memcpy(result, bytes.data(), 16);
    if (test_case.is_depth && result[2] != 165.0f) {
      std::fprintf(stderr, "FAIL depth format %u stencil: %g\n", test_case.format, result[2]);
      return 1;
    }
    for (uint32_t i = 0; i < test_case.components; ++i) {
      float expected = test_case.value[i];
      // 20e4 keeps 20 mantissa bits, so 1/3 is not exact.
      if (std::abs(result[i] - expected) > 1.0e-6f * std::max(1.0f, std::abs(expected))) {
        std::fprintf(stderr, "FAIL round trip depth %u format %u component %u: %.9g != %.9g\n",
                     test_case.is_depth, test_case.format, i, result[i], expected);
        return 1;
      }
    }
  }

  // Coverage: 4 tiles across the end of EDRAM (bases 2046...1), 2 tiles per
  // row, at resolution scales 1 and 2.
  for (uint32_t scale = 1; scale <= 2; ++scale) {
    const uint32_t kSamples = 2048 * 80 * 16 * scale * scale;
    for (uint32_t msaa_4x = 0; msaa_4x < 2; ++msaa_4x) {
      for (uint32_t is_depth = 0; is_depth < 2; ++is_depth) {
        Constants constants;
        constants.base_tiles = 2046;
        constants.pitch_tiles = 2;
        constants.msaa_4x = msaa_4x;
        constants.is_depth = is_depth;
        constants.tile_first = 0;
        constants.tile_count = 4;
        constants.scale_x = scale;
        constants.scale_y = scale;
        constants.width = (msaa_4x ? 80 : 160) * scale;
        constants.row_end = (msaa_4x ? 16 : 32) * scale;
        std::vector<uint32_t> zeros(kSamples);
        auto hits = gpu.Buffer(kSamples * 4, wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc |
                                                   wgpu::BufferUsage::CopyDst,
                               zeros.data());
        std::vector<wgpu::BindGroupEntry> entries(1);
        entries[0].binding = 2;
        entries[0].buffer = hits;
        run(coverage_pipeline, constants, entries, constants.width / 8, constants.row_end / 8);
        auto bytes = gpu.Read(hits, kSamples * 4);
        std::vector<uint32_t> counts(kSamples);
        std::memcpy(counts.data(), bytes.data(), bytes.size());
        for (uint32_t sample = 0; sample < kSamples; ++sample) {
          uint32_t tile = sample / (1280 * scale * scale);
          uint32_t expected = tile >= 2046 || tile < 2 ? 1 : 0;
          if (counts[sample] != expected) {
            std::fprintf(stderr,
                         "FAIL coverage scale %u msaa_4x %u depth %u: sample %u hit %u times\n",
                         scale, msaa_4x, is_depth, sample, counts[sample]);
            return 1;
          }
        }
      }
    }
  }
  std::printf("PASS: %zu format round trips, 1x/4x color/depth EDRAM coverage at 1x/2x scale\n",
              std::size(cases));
  return 0;
}
