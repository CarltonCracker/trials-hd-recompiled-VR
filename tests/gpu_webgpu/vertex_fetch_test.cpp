// Authored vertex-fetch microcode; no cached shaders or recorded game inputs.
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <random>

#include <rex/graphics/pipeline/shader/interpreter.h>
#include <rex/logging.h>
#include <rex/string/buffer.h>
#include <rex/system/xmemory.h>

#include "gpu_test.h"
#include "wgsl/wgsl_shader_translator.h"

using namespace rex::graphics;
using rex::graphics::webgpu::WgslShaderTranslator;

int main(int argc, char** argv) {
  rex::InitLogging();
  GpuTest gpu(argc > 1 ? unsigned(std::strtoul(argv[1], nullptr, 10)) : 0);
  rex::memory::Memory memory;
  GPU_CHECK(memory.Initialize());
  constexpr uint32_t address = 0xA0100000;
  // Pin a low physical address; ordinary heap allocations use the parent heap's top.
  auto* heap = memory.LookupHeap(address);
  GPU_CHECK(heap);
  GPU_CHECK(heap->AllocFixed(address, 4096, 4096,
      rex::memory::kMemoryAllocationReserve | rex::memory::kMemoryAllocationCommit,
      rex::memory::kMemoryProtectRead | rex::memory::kMemoryProtectWrite));
  const uint32_t physical = memory.GetPhysicalAddress(address);
  std::array<uint32_t, 1024> words{};
  std::mt19937 random(0xfeed);
  for (auto& word : words) { word = random(); }
  const std::array<uint32_t, 12> edges = {
      0x7fff7c00, 0xfc00ffff, 0x80018000, 0x03ff0001,
      0x04000400, 0x80000000, 0, 0xffffffff,
      0x7fffffff, 0x3f800000, 0xbf800000, 0x7f800000};
  std::copy(edges.begin(), edges.end(), words.begin());
  std::memcpy(memory.TranslateVirtual(address), words.data(), sizeof(words));
  // Preserve physical addresses in the shader while binding a compact synthetic buffer.
  std::vector<uint32_t> shared((physical >> 2) + words.size());
  std::copy(words.begin(), words.end(), shared.begin() + (physical >> 2));
  auto shared_buffer = gpu.Buffer(shared.size() * 4,
      wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst, shared.data());
  std::array<float, 32> inputs{};
  for (unsigned i = 0; i < inputs.size(); ++i) { inputs[i] = float(i) + 0.5f; }
  auto input_buffer = gpu.Buffer(sizeof(inputs),
      wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst, inputs.data());
  auto output_buffer = gpu.Buffer(inputs.size() * 16,
      wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc);
  RegisterFile registers;
  constexpr std::array formats = {
    xenos::VertexFormat::k_8_8_8_8, xenos::VertexFormat::k_2_10_10_10,
    xenos::VertexFormat::k_10_11_11, xenos::VertexFormat::k_11_11_10,
    xenos::VertexFormat::k_16_16, xenos::VertexFormat::k_16_16_16_16,
    xenos::VertexFormat::k_16_16_FLOAT, xenos::VertexFormat::k_16_16_16_16_FLOAT,
    xenos::VertexFormat::k_32, xenos::VertexFormat::k_32_32,
    xenos::VertexFormat::k_32_32_32_32, xenos::VertexFormat::k_32_FLOAT,
    xenos::VertexFormat::k_32_32_FLOAT, xenos::VertexFormat::k_32_32_32_FLOAT,
    xenos::VertexFormat::k_32_32_32_32_FLOAT};
  struct AddressCase {
    unsigned stride;
    int offset;
    unsigned size;
    bool rounded;
    unsigned constant;
    unsigned swizzle;
    int exponent;
    bool mini = false;
    bool address_only = false;
    unsigned index_mode = 0;  // 1: host index, 2: pulled u16, 3: pulled u32.
    unsigned index_endian = 0;
    int index_base = 0;
    unsigned index_min = 0;
    unsigned index_max = 0xffffff;
  };
  constexpr unsigned xyzw = 0u | (1u << 3) | (2u << 6) | (3u << 9);
  constexpr std::array cases = {
      AddressCase{4, 0, 1024, false, 0, xyzw, 0},
      AddressCase{0, 0, 1024, false, 0, xyzw, 0},
      AddressCase{1, 0, 1024, false, 0, xyzw, 0},
      AddressCase{255, 0, 1024, false, 0, xyzw, 0},
      AddressCase{4, -1, 1024, false, 0, xyzw, 0},
      AddressCase{4, 1022, 1024, false, 0, xyzw, 0},
      AddressCase{4, 4194303, 1024, false, 0, xyzw, 0},
      AddressCase{4, -4194304, 1024, false, 0, xyzw, 0},
      AddressCase{4, 0, 0, false, 0, xyzw, 0},
      AddressCase{4, 0, 1, false, 0, xyzw, 0},
      AddressCase{4, 0, 1024, true, 0, xyzw, 0},
      AddressCase{4, 0, 1024, false, 95, xyzw, 0},
      AddressCase{4, 0, 1024, false, 0, 7u | (4u << 3) | (5u << 6), 0},
      AddressCase{4, 0, 1024, false, 0, xyzw, -32},
      AddressCase{4, 0, 1024, false, 0, xyzw, 31},
      AddressCase{4, 1, 1024, false, 0, xyzw, 0, true},
      AddressCase{4, 1, 1024, false, 0, xyzw, 0, true, true},
      AddressCase{4, 0, 1024, false, 0, xyzw, 0, false, false, 1, 0},
      AddressCase{4, 0, 1024, false, 0, xyzw, 0, false, false, 1, 1},
      AddressCase{4, 0, 1024, false, 0, xyzw, 0, false, false, 1, 2},
      AddressCase{4, 0, 1024, false, 0, xyzw, 0, false, false, 1, 3},
      AddressCase{4, 0, 1024, false, 0, xyzw, 0, false, false, 2, 0},
      AddressCase{4, 0, 1024, false, 0, xyzw, 0, false, false, 2, 1},
      AddressCase{4, 0, 1024, false, 0, xyzw, 0, false, false, 3, 0},
      AddressCase{4, 0, 1024, false, 0, xyzw, 0, false, false, 3, 1},
      AddressCase{4, 0, 1024, false, 0, xyzw, 0, false, false, 3, 2},
      AddressCase{4, 0, 1024, false, 0, xyzw, 0, false, false, 3, 3},
      AddressCase{4, 0, 1024, false, 0, xyzw, 0, false, false, 1, 0, -5, 3, 24},
      AddressCase{4, 0, 1024, false, 0, xyzw, 0, false, false, 2, 1, -5, 3, 24},
      AddressCase{4, 0, 1024, false, 0, xyzw, 0, false, false, 3, 2, -5, 3, 24}};
  for (unsigned scenario = 0; scenario < cases.size(); ++scenario) {
    const auto& test = cases[scenario];
    rex::graphics::webgpu::WgslSystemConstants system;
    if (test.index_mode >= 2) {
      system.flags = system.kVertexIndexLoad;
      if (test.index_mode == 3) { system.flags |= system.kVertexIndexLoad32Bit; }
    }
    system.vertex_index_endian = test.index_endian;
    system.vertex_base_index = test.index_base;
    system.vertex_index_load_address = physical + (test.index_mode == 2 ? 512u : 544u) * 4;
    system.vertex_index_min = test.index_min; system.vertex_index_max = test.index_max;
    std::array<unsigned, 32> guest_indices{};
    for (unsigned i = 0; i < inputs.size(); ++i) {
      unsigned index = (i * 7) % 32;
      unsigned raw = xenos::GpuSwap(index, xenos::Endian(test.index_endian));
      if (test.index_mode == 2) {
        unsigned shift = (i & 1u) * 16;
        words[512 + i / 2] = (words[512 + i / 2] & ~(65535u << shift)) | (raw << shift);
      }
      words[544 + i] = raw;
      inputs[i] = test.index_mode == 1 ? float(raw) : float(i) + 0.5f;
      guest_indices[i] = std::clamp((index + unsigned(test.index_base)) & 0xffffffu,
                                    test.index_min, test.index_max);
    }
    std::memcpy(memory.TranslateVirtual(address), words.data(), sizeof(words));
    gpu.device.GetQueue().WriteBuffer(shared_buffer, physical, words.data(), sizeof(words));
    gpu.device.GetQueue().WriteBuffer(input_buffer, 0, inputs.data(), sizeof(inputs));
    for (auto format : formats) {
      if (scenario && format != xenos::VertexFormat::k_32_32_32_32_FLOAT) { continue; }
      for (unsigned mode = 0; mode < 8; ++mode) {
        if (scenario && mode != 2) { continue; }
      for (unsigned endian = 0; endian < 4; ++endian) {
        std::array<uint32_t, 192> fetch{};
        fetch[test.constant * 2] = (physical & ~3u) | 3u;
        fetch[test.constant * 2 + 1] = (test.size << 2) | endian;
        std::copy(fetch.begin(), fetch.end(), registers.values + XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0);
        std::vector<uint32_t> code = {
          1u | (1u << 12) | (1u << 16), uint32_t(ucode::ControlFlowOpcode::kExecEnd) << 12, 0u,
          (1u << 12) | (1u << 19) | ((test.constant / 3) << 20) |
              ((test.constant % 3) << 25),
          test.swizzle | ((mode & 1u) << 12) | (((mode >> 1) & 1u) << 13) |
              ((mode >> 2) << 14) | (unsigned(test.rounded) << 15) |
              (uint32_t(format) << 16) | ((uint32_t(test.exponent) & 63u) << 24),
          test.stride | ((uint32_t(test.offset) & 0x7fffffu) << 8)};
        if (test.mini) {
          code[0] = 1u | (2u << 12) | (5u << 16);
          code.insert(code.end(), {code[3] | (63u << 5) | (31u << 20) | (2u << 25),
                                  code[4] | (1u << 30), code[5]});
          code[5] = test.stride;
          if (test.address_only) { code[4] = (code[4] & ~4095u) | 4095u; }
        }
        Shader shader(xenos::ShaderType::kVertex, 0, code.data(), code.size(), std::endian::native);
        rex::string::StringBuffer disassembly;
        shader.AnalyzeUcode(disassembly);
        WgslShaderTranslator translator;
        auto* translation = shader.GetOrCreateTranslation(translator.GetDefaultVertexShaderModification(64));
        GPU_CHECK(translator.TranslateAnalyzedShader(*translation));
        GPU_CHECK(translation->errors().empty());
        std::string source = translation->GetTranslatedBinaryString();
        source += R"(
@group(1) @binding(1) var<storage, read> test_indices: array<f32>;
@group(1) @binding(2) var<storage, read_write> test_values: array<vec4f>;
@compute @workgroup_size(32)
fn test_fetch(@builtin(global_invocation_id) id: vec3u) {
  if (id.x >= arrayLength(&test_indices)) { return; }
  xe_r[0] = vec4f(test_indices[id.x]);
  xe_r[1] = vec4f(11.0, 22.0, 33.0, 44.0);
  xe_main_body();
  test_values[id.x] = xe_r[1];
})";
        if (test.index_mode) {
          constexpr auto body = "  xe_r[0] = vec4f(test_indices[id.x]);\n"
                                "  xe_r[1] = vec4f(11.0, 22.0, 33.0, 44.0);\n"
                                "  xe_main_body();";
          const auto position = source.find(body);
          GPU_CHECK(position != std::string::npos);
          source.replace(position, std::strlen(body),
                         "  xe_run_vertex(xe_vertex_index(u32(test_indices[id.x])));");
        }
        auto module = gpu.Module(source);
        wgpu::ComputePipelineDescriptor descriptor;
        descriptor.compute.module = module;
        descriptor.compute.entryPoint = "test_fetch";
        auto pipeline = gpu.device.CreateComputePipeline(&descriptor);
        auto fetch_buffer = gpu.Buffer(sizeof(fetch),
            wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst, fetch.data());
        std::array<wgpu::BindGroupEntry, 2> constant_entries;
        constant_entries[0].binding = 4;
        constant_entries[0].buffer = fetch_buffer; constant_entries[0].size = sizeof(fetch);
        wgpu::Buffer system_buffer;
        if (test.index_mode) {
          system_buffer = gpu.Buffer(sizeof(system),
              wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst, &system);
          constant_entries[1].binding = 0;
          constant_entries[1].buffer = system_buffer; constant_entries[1].size = sizeof(system);
        }
        wgpu::BindGroupDescriptor group;
        group.layout = pipeline.GetBindGroupLayout(0);
        group.entryCount = test.index_mode ? 2 : 1; group.entries = constant_entries.data();
        auto constants = gpu.device.CreateBindGroup(&group);
        std::array<wgpu::BindGroupEntry, 3> entries;
        entries[0].binding = 0; entries[0].buffer = shared_buffer; entries[0].size = shared.size() * 4;
        entries[1].binding = 1; entries[1].buffer = input_buffer; entries[1].size = sizeof(inputs);
        entries[2].binding = 2; entries[2].buffer = output_buffer; entries[2].size = inputs.size() * 16;
        group.layout = pipeline.GetBindGroupLayout(1);
        group.entryCount = entries.size(); group.entries = entries.data();
        auto buffers = gpu.device.CreateBindGroup(&group);
        auto encoder = gpu.device.CreateCommandEncoder();
        auto pass = encoder.BeginComputePass();
        pass.SetPipeline(pipeline); pass.SetBindGroup(0, constants); pass.SetBindGroup(1, buffers);
        pass.DispatchWorkgroups(1); pass.End();
        auto command = encoder.Finish(); gpu.device.GetQueue().Submit(1, &command);
        auto bytes = gpu.Read(output_buffer, inputs.size() * 16);
        ShaderInterpreter interpreter(registers, memory);
        interpreter.SetShader(shader);
        for (unsigned i = 0; i < inputs.size(); ++i) {
          std::fill_n(interpreter.temp_registers(), xenos::kMaxShaderTempRegisters * 4, 0.0f);
          interpreter.temp_registers()[0] = test.index_mode ? float(guest_indices[i]) : inputs[i];
          for (unsigned c = 0; c < 4; ++c) { interpreter.temp_registers()[4 + c] = 11.0f * (c + 1); }
          interpreter.Execute();
          std::array<float, 4> actual;
          std::memcpy(actual.data(), bytes.data() + i * 16, 16);
          for (unsigned c = 0; c < 4; ++c) {
            float expected = interpreter.temp_registers()[4 + c];
            if (format == xenos::VertexFormat::k_8_8_8_8) {
              uint32_t word = xenos::GpuSwap(words[4 * i], xenos::Endian(endian));
              uint8_t byte = uint8_t(word >> (8 * c));
              float unpacked = mode & 1u ? float(std::bit_cast<int8_t>(byte)) : float(byte);
              if (!(mode & 2u)) {
                if (!(mode & 1u)) { unpacked /= 255.0f; }
                else if (mode & 4u) { unpacked = (unpacked + 0.5f) * 2.0f / 255.0f; }
                else { unpacked = std::max(-1.0f, unpacked / 127.0f); }
              }
              GPU_CHECK(std::abs(expected - unpacked) < 1e-6f);
            }
            bool equal = std::isnan(expected) ? std::isnan(actual[c]) :
              !std::isfinite(expected) ? actual[c] == expected :
              std::abs(actual[c] - expected) <= 2e-5f * std::max(1.0f, std::abs(expected));
            if (!equal) {
              std::fprintf(stderr, "vfetch case%u format%u mode%u endian%u index%u c%u GPU %.9g CPU %.9g\n",
                  scenario, uint32_t(format), mode, endian, i, c, actual[c], expected);
              GPU_CHECK(false);
            }
          }
        }
      }
      }
    }
  }
  std::puts("PASS: 15 vertex formats, both signed-RF modes, signed/unsigned integer/normalized, four endians; strides, offsets, bounds, rounding, constants, swizzles, exponents, mini fetch; direct and pulled u16/u32 indices with base/limits");
}
