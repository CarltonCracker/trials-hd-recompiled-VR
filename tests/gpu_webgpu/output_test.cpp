// Authored pixel-output tests: real fragment entry point, MRT, alpha test and depth.
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <limits>
#include <vector>
#include <rex/graphics/pipeline/shader/interpreter.h>
#include <rex/logging.h>
#include <rex/string/buffer.h>
#include <rex/system/xmemory.h>
#include "gpu_test.h"
#include "format_tables.h"
#include "wgsl/wgsl_shader_translator.h"

using namespace rex::graphics;
using namespace rex::graphics::webgpu;
using Vec = std::array<float, 4>;

struct Exports : ShaderInterpreter::ExportSink {
  std::array<Vec, 64> values{};
  void Export(ucode::ExportRegister reg, const float* value, uint32_t mask) override {
    unsigned index = unsigned(reg);
    GPU_CHECK(index < values.size());
    for (unsigned c = 0; c < 4; ++c) { if (mask & (1u << c)) { values[index][c] = value[c]; } }
  }
};

bool Equal(float actual, float expected) {
  if (std::isnan(expected)) { return std::isnan(actual); }
  if (!std::isfinite(expected)) { return actual == expected; }
  return std::abs(actual - expected) <= 2e-5f * std::max(1.0f, std::abs(expected));
}

bool AlphaPass(unsigned function, float alpha, float reference) {
  switch (function) {
    case 0: return false;
    case 1: return alpha < reference;
    case 2: return alpha == reference;
    case 3: return alpha <= reference;
    case 4: return alpha > reference;
    case 5: return alpha != reference;
    case 6: return alpha >= reference;
    default: return true;
  }
}

void CheckFixed16Fetch(GpuTest& gpu, std::string source) {
  source += R"(
@group(1) @binding(1) var<storage, read> test_raw: array<vec4f>;
@group(1) @binding(2) var<storage, read_write> test_decoded: array<vec4f>;
@compute @workgroup_size(16) fn test_fixed16(@builtin(global_invocation_id) id: vec3u) {
  test_decoded[id.x] = xe_texture_postprocess(test_raw[id.x], id.x / 4u);
})";
  auto module = gpu.Module(source);
  wgpu::ComputePipelineDescriptor pd; pd.compute.module = module; pd.compute.entryPoint = "test_fixed16";
  auto pipeline = gpu.device.CreateComputePipeline(&pd);
  std::array<Vec, 16> raw{};
  auto input = gpu.Buffer(sizeof(raw), wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst);
  auto output = gpu.Buffer(sizeof(raw), wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc);
  std::array<uint32_t, 192> fetch{};
  auto uniform = gpu.Buffer(sizeof(fetch), wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst);
  wgpu::BindGroupEntry entry; entry.binding = 4; entry.buffer = uniform; entry.size = sizeof(fetch);
  wgpu::BindGroupDescriptor bd; bd.layout = pipeline.GetBindGroupLayout(0); bd.entryCount = 1; bd.entries = &entry;
  auto constants = gpu.device.CreateBindGroup(&bd);
  std::array<wgpu::BindGroupEntry, 2> storage;
  storage[0].binding = 1; storage[0].buffer = input; storage[0].size = sizeof(raw);
  storage[1].binding = 2; storage[1].buffer = output; storage[1].size = sizeof(raw);
  bd.layout = pipeline.GetBindGroupLayout(1); bd.entryCount = storage.size(); bd.entries = storage.data();
  auto values = gpu.device.CreateBindGroup(&bd);
  constexpr float edges[] = {0, 32767, 32768, 65535};
  for (unsigned format = 0; format < 3; ++format) {
    unsigned components = 1u << format;
    for (unsigned i = 0; i < raw.size(); ++i) {
      for (unsigned c = 0; c < 4; ++c) { raw[i][c] = c < components ? edges[(i + c) % 4] : (c == 3 ? 1 : 0); }
    }
    gpu.device.GetQueue().WriteBuffer(input, 0, raw.data(), sizeof(raw));
    for (unsigned integer = 0; integer < 2; ++integer) {
      for (int exponent : {-2, 0, 3}) {
        for (unsigned sign = 0; sign < 4; ++sign) {
          fetch[sign * 6] = sign * 0x154u;
          fetch[sign * 6 + 1] = 24 + format;
          fetch[sign * 6 + 3] = integer | (0x688u << 1) | ((uint32_t(exponent) & 63) << 13);
        }
        gpu.device.GetQueue().WriteBuffer(uniform, 0, fetch.data(), sizeof(fetch));
        auto encoder = gpu.device.CreateCommandEncoder(); auto pass = encoder.BeginComputePass();
        pass.SetPipeline(pipeline); pass.SetBindGroup(0, constants); pass.SetBindGroup(1, values);
        pass.DispatchWorkgroups(1); pass.End(); auto command = encoder.Finish();
        gpu.device.GetQueue().Submit(1, &command); auto bytes = gpu.Read(output, sizeof(raw));
        for (unsigned i = 0; i < raw.size(); ++i) {
          unsigned sign = i / 4;
          for (unsigned c = 0; c < 4; ++c) {
            float expected = raw[i][c];
            if (c < components) {
              if (sign == 1 && expected >= 32768) { expected -= 65536; }
              if (!integer) { expected = sign == 1 ? std::max(expected / 32767, -1.0f) : expected / 65535; }
            }
            if (sign == 2) { expected = expected * 2 - 1; }
            expected = std::ldexp(expected, exponent);
            float actual; std::memcpy(&actual, bytes.data() + i * sizeof(Vec) + c * 4, 4);
            if (!Equal(actual, expected)) {
              std::fprintf(stderr, "fixed16 format%u integer%u exponent%d sign%u input%u c%u GPU%.9g CPU%.9g\n",
                  format, integer, exponent, sign, i, c, actual, expected);
              GPU_CHECK(false);
            }
          }
        }
      }
    }
  }
  std::puts("PASS: 288 fixed16 fetch vectors: signed/unsigned/biased, integer/fraction, exponent and missing components");
}

void CheckPositions(GpuTest& gpu) {
  std::array<uint32_t, 6> code = {1u | (1u << 12),
      uint32_t(ucode::ControlFlowOpcode::kExecEnd) << 12, 0u,
      62u | (1u << 15) | (15u << 16) | (uint32_t(ucode::AluScalarOpcode::kRetainPrev) << 26),
      0u, (uint32_t(ucode::AluVectorOpcode::kMax) << 24) | (7u << 29)};
  Shader shader(xenos::ShaderType::kVertex, 0, code.data(), code.size(), std::endian::native);
  rex::string::StringBuffer disassembly; shader.AnalyzeUcode(disassembly);
  WgslShaderTranslator translator;
  auto* translation = shader.GetOrCreateTranslation(translator.GetDefaultVertexShaderModification(64));
  GPU_CHECK(translator.TranslateAnalyzedShader(*translation));
  GPU_CHECK(translation->errors().empty());
  std::string source = translation->GetTranslatedBinaryString();
  CheckFixed16Fetch(gpu, source);
  source += R"(
@group(1) @binding(1) var<storage, read> test_positions: array<vec4f>;
@group(1) @binding(2) var<storage, read_write> test_transformed: array<vec4f>;
@compute @workgroup_size(8) fn test_positions_main(@builtin(global_invocation_id) id: vec3u) {
  if (id.x < arrayLength(&test_positions)) {
    test_transformed[id.x] = xe_transform_position(test_positions[id.x]);
  }
})";
  auto module = gpu.Module(source);
  wgpu::ComputePipelineDescriptor pd; pd.compute.module = module; pd.compute.entryPoint = "test_positions_main";
  auto pipeline = gpu.device.CreateComputePipeline(&pd);
  const std::array<Vec, 8> positions = {{{-2, -1, 0, 1}, {-1, 2, 1, 2}, {2, 4, 0.25f, 0.5f},
      {0, -0.0f, 3, -2}, {12, 3, -1, 0.125f}, {-3, 7, 5, -0.5f},
      {1, 2, 0.5f, 4}, {1.25f, -0.25f, 1.5f, -0.125f}}};
  const std::array<Vec, 4> scales = {{{1, 1, 1, 0}, {1, -1, 1, 0},
      {1, -1, -1, 0}, {-2, 1.5f, 0.5f, 0}}};
  const std::array<Vec, 4> offsets = {{{0, 0, 0, 0}, {-1.0f / 1280, 1.0f / 720, 0, 0},
      {0, 0, 1, 0}, {0.125f, -0.25f, 0.5f, 0}}};
  auto input = gpu.Buffer(sizeof(positions), wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst, positions.data());
  auto output = gpu.Buffer(sizeof(positions), wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc);
  auto uniform = gpu.Buffer(sizeof(WgslSystemConstants), wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst);
  wgpu::BindGroupEntry entry; entry.binding = 0; entry.buffer = uniform; entry.size = sizeof(WgslSystemConstants);
  wgpu::BindGroupDescriptor bd; bd.layout = pipeline.GetBindGroupLayout(0); bd.entryCount = 1; bd.entries = &entry;
  auto draw = gpu.device.CreateBindGroup(&bd);
  std::array<wgpu::BindGroupEntry, 2> storage;
  storage[0].binding = 1; storage[0].buffer = input; storage[0].size = sizeof(positions);
  storage[1].binding = 2; storage[1].buffer = output; storage[1].size = sizeof(positions);
  bd.layout = pipeline.GetBindGroupLayout(1); bd.entryCount = storage.size(); bd.entries = storage.data();
  auto values = gpu.device.CreateBindGroup(&bd);
  for (unsigned flags = 0; flags < 8; ++flags) {
    for (unsigned viewport = 0; viewport < scales.size(); ++viewport) {
      WgslSystemConstants system; system.flags = flags;
      std::copy(scales[viewport].begin(), scales[viewport].end(), system.ndc_scale);
      std::copy(offsets[viewport].begin(), offsets[viewport].end(), system.ndc_offset);
      gpu.device.GetQueue().WriteBuffer(uniform, 0, &system, sizeof(system));
      auto encoder = gpu.device.CreateCommandEncoder(); auto pass = encoder.BeginComputePass();
      pass.SetPipeline(pipeline); pass.SetBindGroup(0, draw); pass.SetBindGroup(1, values); pass.DispatchWorkgroups(1); pass.End();
      auto command = encoder.Finish(); gpu.device.GetQueue().Submit(1, &command);
      auto bytes = gpu.Read(output, sizeof(positions));
      for (unsigned i = 0; i < positions.size(); ++i) {
        Vec expected = positions[i];
        // Same separate operations as the SDK SPIR-V position epilogue.
        if (!(flags & WgslSystemConstants::kWNotReciprocal)) { expected[3] = 1.0f / expected[3]; }
        for (unsigned c = 0; c < 3; ++c) {
          if (flags & (c < 2 ? WgslSystemConstants::kXYDividedByW : WgslSystemConstants::kZDividedByW)) {
            expected[c] *= expected[3];
          }
          expected[c] *= scales[viewport][c];
          expected[c] += offsets[viewport][c] * expected[3];
        }
        for (unsigned c = 0; c < 4; ++c) {
          float actual; std::memcpy(&actual, bytes.data() + (i * 4 + c) * 4, 4);
          if (!Equal(actual, expected[c])) {
            std::fprintf(stderr, "position flags%u viewport%u input%u c%u GPU%.9g CPU%.9g\n", flags, viewport, i, c, actual, expected[c]);
            GPU_CHECK(false);
          }
        }
      }
    }
  }
  std::puts("PASS: 256 position cases: all VTE formats, signed/reciprocal W, Y flip, half-pixel offsets and reverse-Z transform");
}

void CheckFormats(GpuTest& gpu, Shader& shader, ShaderInterpreter& interpreter,
                  RegisterFile& regs, const wgpu::ShaderModule& vertex) {
  using F = xenos::ColorRenderTargetFormat;
  const std::array<F, 8> formats = {F::k_8_8_8_8, F::k_2_10_10_10, F::k_2_10_10_10_FLOAT,
      F::k_16_16, F::k_16_16_16_16, F::k_16_16_FLOAT, F::k_32_FLOAT, F::k_32_32_FLOAT};
  auto uniform = gpu.Buffer(sizeof(WgslSystemConstants), wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst);
  auto constants = gpu.Buffer(4 * sizeof(Vec), wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst);
  wgpu::TextureDescriptor td; td.size = {2, 2, 1};
  td.format = wgpu::TextureFormat::Depth32Float; td.usage = wgpu::TextureUsage::RenderAttachment;
  auto depth_texture = gpu.device.CreateTexture(&td);
  WgslShaderTranslator translator;
  WgslShaderTranslator::Modification modification;
  modification.value = translator.GetDefaultPixelShaderModification(64);
  modification.interpolator_mask = 15; modification.color_targets = 15;
  auto* translation = shader.GetOrCreateTranslation(modification.value);
  GPU_CHECK(translator.TranslateAnalyzedShader(*translation));
  GPU_CHECK(translation->errors().empty());
  auto fragment = gpu.Module(translation->GetTranslatedBinaryString());
  unsigned cases = 0;
  for (bool tier1_mapping : {false, true}) {
    auto features = format_tables::DeviceFeatures::FromDevice(gpu.device);
    features.texture_formats_tier1 = tier1_mapping;
    for (auto format : formats) {
      auto info = format_tables::GetColorRenderTargetFormatInfo(format, features);
      GPU_CHECK(info.is_supported);
      auto host = info.host_format;
      unsigned channels = host == wgpu::TextureFormat::R32Float ? 1 :
          (host == wgpu::TextureFormat::RG16Float || host == wgpu::TextureFormat::RG16Snorm ||
           host == wgpu::TextureFormat::RG32Float) ? 2 : 4;
      unsigned pixel_bytes = host == wgpu::TextureFormat::RGBA8Unorm || host == wgpu::TextureFormat::RGB10A2Unorm ? 4 :
          (host == wgpu::TextureFormat::R32Float || host == wgpu::TextureFormat::RG32Float) ? channels * 4 : channels * 2;
      std::array<wgpu::Texture, 4> targets;
      std::array<wgpu::ColorTargetState, 4> colors;
      td.format = host; td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
      for (unsigned i = 0; i < 4; ++i) { targets[i] = gpu.device.CreateTexture(&td); colors[i].format = host; }
      wgpu::FragmentState fs; fs.module = fragment; fs.entryPoint = "main";
      fs.targetCount = colors.size(); fs.targets = colors.data();
      wgpu::DepthStencilState ds; ds.format = wgpu::TextureFormat::Depth32Float;
      ds.depthWriteEnabled = true; ds.depthCompare = wgpu::CompareFunction::Always;
      wgpu::RenderPipelineDescriptor pd; pd.vertex.module = vertex; pd.vertex.entryPoint = "vs";
      pd.fragment = &fs; pd.depthStencil = &ds;
      auto pipeline = gpu.device.CreateRenderPipeline(&pd);
      std::array<wgpu::BindGroupEntry, 3> entries;
      entries[0].binding = 0; entries[0].buffer = uniform; entries[0].size = sizeof(WgslSystemConstants);
      entries[1].binding = 2; entries[1].buffer = constants; entries[1].size = 4 * sizeof(Vec);
      entries[2].binding = 7; entries[2].buffer = constants; entries[2].size = 4 * sizeof(Vec);
      wgpu::BindGroupDescriptor bd; bd.layout = pipeline.GetBindGroupLayout(0);
      bd.entryCount = entries.size(); bd.entries = entries.data();
      auto group = gpu.device.CreateBindGroup(&bd);
      for (float value : {-40.0f, -32.0f, -0.5f, 0.0f, 0.5f, 31.999f, 32.0f, 40.0f}) {
        const std::array<Vec, 4> input = {{{value, value * 0.5f, -value, 0.75f}, {-value, 1, 0.5f, value},
            {value * 2, 0.125f, -0.25f, 1}, {0.03125f, value, -8, 0.5f}}};
        WgslSystemConstants system;
        const std::array<float, 4> exponents = {0.125f, 1.0f, 8.0f, 0.5f};
        for (unsigned i = 0; i < 4; ++i) { system.color_output_scale[i] = info.output_scale * exponents[i]; }
        gpu.device.GetQueue().WriteBuffer(uniform, 0, &system, sizeof(system));
        gpu.device.GetQueue().WriteBuffer(constants, 0, input.data(), sizeof(input));
        std::memcpy(&regs.values[XE_GPU_REG_SHADER_CONSTANT_000_X], input.data(), sizeof(input));
        std::fill_n(interpreter.temp_registers(), xenos::kMaxShaderTempRegisters * 4, 0.0f);
        for (unsigned i = 0; i < 4; ++i) { std::copy(input[i].begin(), input[i].end(), interpreter.temp_registers() + i * 4); }
        Exports exports; interpreter.SetExportSink(&exports); interpreter.Execute();
        std::array<wgpu::RenderPassColorAttachment, 4> attachments;
        for (unsigned i = 0; i < 4; ++i) {
          attachments[i].view = targets[i].CreateView(); attachments[i].loadOp = wgpu::LoadOp::Clear;
          attachments[i].storeOp = wgpu::StoreOp::Store;
        }
        wgpu::RenderPassDepthStencilAttachment depth; depth.view = depth_texture.CreateView();
        depth.depthLoadOp = wgpu::LoadOp::Clear; depth.depthStoreOp = wgpu::StoreOp::Store; depth.depthClearValue = 1;
        wgpu::RenderPassDescriptor rp; rp.colorAttachmentCount = attachments.size();
        rp.colorAttachments = attachments.data(); rp.depthStencilAttachment = &depth;
        auto encoder = gpu.device.CreateCommandEncoder(); auto pass = encoder.BeginRenderPass(&rp);
        pass.SetPipeline(pipeline); pass.SetBindGroup(0, group); pass.Draw(3); pass.End();
        auto readback = gpu.Buffer(4 * 512, wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::CopySrc);
        for (unsigned i = 0; i < 4; ++i) {
          wgpu::TexelCopyTextureInfo from; from.texture = targets[i];
          wgpu::TexelCopyBufferInfo to; to.buffer = readback; to.layout.offset = i * 512;
          to.layout.bytesPerRow = 256; to.layout.rowsPerImage = 2;
          wgpu::Extent3D extent{2, 2, 1}; encoder.CopyTextureToBuffer(&from, &to, &extent);
        }
        auto command = encoder.Finish(); gpu.device.GetQueue().Submit(1, &command);
        auto bytes = gpu.Read(readback, 4 * 512);
        for (unsigned i = 0; i < 4; ++i) {
          const uint8_t* pixel = bytes.data() + i * 512 + 256 + pixel_bytes;
          for (unsigned c = 0; c < channels; ++c) {
            float expected = exports.values[i][c] * system.color_output_scale[i];
            float actual, tolerance = 2e-5f * std::max(1.0f, std::abs(expected));
            if (host == wgpu::TextureFormat::RGBA8Unorm || host == wgpu::TextureFormat::RGB10A2Unorm) {
              unsigned maximum = host == wgpu::TextureFormat::RGBA8Unorm ? 255 : c == 3 ? 3 : 1023;
              uint32_t packed; std::memcpy(&packed, pixel, 4);
              unsigned component = host == wgpu::TextureFormat::RGBA8Unorm ? pixel[c] : (packed >> (c * 10)) & maximum;
              actual = float(component) / maximum; expected = std::clamp(expected, 0.0f, 1.0f);
              tolerance = 0.51f / maximum + 2e-5f;
            } else if (host == wgpu::TextureFormat::RG16Snorm || host == wgpu::TextureFormat::RGBA16Snorm) {
              int16_t component; std::memcpy(&component, pixel + c * 2, 2);
              actual = std::max(-1.0f, float(component) / 32767.0f); expected = std::clamp(expected, -1.0f, 1.0f);
              tolerance = 0.51f / 32767.0f + 2e-5f;
            } else if (host == wgpu::TextureFormat::R32Float || host == wgpu::TextureFormat::RG32Float) {
              std::memcpy(&actual, pixel + c * 4, 4);
            } else {
              _Float16 component; std::memcpy(&component, pixel + c * 2, 2);
              actual = float(component);
              _Float16 nearest = _Float16(expected);
              float rounded = float(nearest), adjacent = rounded;
              // Vulkan attachment conversion permits either neighboring half, not only round-to-nearest.
              if (rounded != expected) {
                uint16_t bits; std::memcpy(&bits, &nearest, 2);
                bits = uint16_t(((rounded < expected) != std::signbit(expected)) ? bits + 1 : bits - 1);
                std::memcpy(&nearest, &bits, 2); adjacent = float(nearest);
              }
              expected = actual == adjacent ? adjacent : rounded;
              tolerance = 0;
            }
            if (!std::isfinite(actual) || std::abs(actual - expected) > tolerance) {
              std::fprintf(stderr, "RT format%u tier1%u input%.9g target%u c%u GPU%.9g CPU%.9g\n",
                  unsigned(format), unsigned(tier1_mapping), value, i, c, actual, expected);
              GPU_CHECK(false);
            }
          }
        }
        ++cases;
      }
    }
  }
  std::printf("PASS: %u MRT format-conversion cases, production Tier-1/fallback mappings and exponent scales\n", cases);
}

int main(int argc, char** argv) {
  rex::InitLogging();
  GpuTest gpu(argc > 1 ? unsigned(std::strtoul(argv[1], nullptr, 10)) : 0,
      {wgpu::FeatureName::TextureFormatsTier1});
  CheckPositions(gpu);
  rex::memory::Memory memory; // The authored pixel shader does not access guest memory.
  RegisterFile regs;
  reg::SQ_PS_CONST ps{}; ps.size = 255;
  regs.values[XE_GPU_REG_SQ_PS_CONST] = ps.value;
  std::array<uint32_t, 18> code{};
  code[0] = 1u | (5u << 12);
  code[1] = uint32_t(ucode::ControlFlowOpcode::kExecEnd) << 12;
  for (unsigned i = 0; i < 5; ++i) {
    unsigned dest = i == 4 ? unsigned(ucode::ExportRegister::kPSDepth) : i;
    unsigned source = i == 4 ? 0 : i;
    code[3 + i * 3] = dest | (1u << 15) | (15u << 16) |
        (uint32_t(ucode::AluScalarOpcode::kRetainPrev) << 26);
    code[5 + i * 3] = (source << 8) | (source << 16) |
        (uint32_t(ucode::AluVectorOpcode::kMax) << 24) | (1u << 29);
    // Color 3 consumes an interpolator (or generated facing/point parameters).
    if (i == 3) { code[5 + i * 3] |= (1u << 30) | (1u << 31); }
  }
  Shader shader(xenos::ShaderType::kPixel, 0, code.data(), code.size(), std::endian::native);
  rex::string::StringBuffer disassembly;
  shader.AnalyzeUcode(disassembly);
  GPU_CHECK(shader.writes_color_targets() == 15 && shader.writes_depth());
  ShaderInterpreter interpreter(regs, memory); interpreter.SetShader(shader);
  auto vertex = gpu.Module(R"(
@group(0) @binding(7) var<uniform> colors: array<vec4f, 4>;
struct Out {
  @builtin(position) position: vec4f,
  @location(0) c0: vec4f, @location(1) c1: vec4f,
  @location(2) c2: vec4f, @location(3) c3: vec4f,
}
@vertex fn vs(@builtin(vertex_index) id: u32) -> Out {
  let positions = array<vec2f, 3>(vec2f(-1.0, -1.0), vec2f(3.0, -1.0), vec2f(-1.0, 3.0));
  var result: Out;
  result.position = vec4f(positions[id], 0.0, 1.0);
  result.c0 = colors[0]; result.c1 = colors[1];
  result.c2 = colors[2]; result.c3 = colors[3];
  return result;
})");
  std::array<wgpu::Texture, 5> targets;
  for (unsigned i = 0; i < targets.size(); ++i) {
    wgpu::TextureDescriptor td;
    td.size = {2, 2, 1};
    td.format = i < 4 ? wgpu::TextureFormat::RGBA16Float : wgpu::TextureFormat::Depth32Float;
    td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
    targets[i] = gpu.device.CreateTexture(&td);
  }
  auto uniform = gpu.Buffer(sizeof(WgslSystemConstants), wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst);
  auto constants = gpu.Buffer(4 * sizeof(Vec), wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst);
  unsigned cases = 0;
  for (unsigned parameter_mode = 0; parameter_mode < 5; ++parameter_mode) {
    WgslShaderTranslator translator;
    WgslShaderTranslator::Modification modification;
    modification.value = translator.GetDefaultPixelShaderModification(64);
    modification.interpolator_mask = 15; modification.color_targets = 15;
    modification.param_gen = parameter_mode != 0;
    modification.param_gen_interpolator = 3;
    modification.param_gen_point = parameter_mode >= 3;
    auto* translation = shader.GetOrCreateTranslation(modification.value);
    GPU_CHECK(translator.TranslateAnalyzedShader(*translation));
    GPU_CHECK(translation->errors().empty());
    auto fragment = gpu.Module(translation->GetTranslatedBinaryString());
    std::array<wgpu::ColorTargetState, 4> colors;
    for (auto& target : colors) { target.format = wgpu::TextureFormat::RGBA16Float; }
    wgpu::FragmentState fs; fs.module = fragment; fs.entryPoint = "main";
    fs.targetCount = colors.size(); fs.targets = colors.data();
    wgpu::DepthStencilState ds;
    ds.format = wgpu::TextureFormat::Depth32Float; ds.depthWriteEnabled = true;
    ds.depthCompare = wgpu::CompareFunction::Always;
    wgpu::RenderPipelineDescriptor pd;
    pd.vertex.module = vertex; pd.vertex.entryPoint = "vs";
    pd.fragment = &fs; pd.depthStencil = &ds;
    pd.primitive.frontFace = parameter_mode == 2 || parameter_mode == 4 ? wgpu::FrontFace::CW : wgpu::FrontFace::CCW;
    auto pipeline = gpu.device.CreateRenderPipeline(&pd);
    std::array<wgpu::BindGroupEntry, 3> entries;
    entries[0].binding = 0; entries[0].buffer = uniform; entries[0].size = sizeof(WgslSystemConstants);
    entries[1].binding = 2; entries[1].buffer = constants; entries[1].size = 4 * sizeof(Vec);
    entries[2].binding = 7; entries[2].buffer = constants; entries[2].size = 4 * sizeof(Vec);
    wgpu::BindGroupDescriptor bd;
    bd.layout = pipeline.GetBindGroupLayout(0); bd.entryCount = entries.size(); bd.entries = entries.data();
    auto group = gpu.device.CreateBindGroup(&bd);
    const std::array<float, 9> alphas = {-1.0f, 0.0f, 0.25f, 0.5f, 0.75f, 1.0f,
        std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity()};
    const std::array<float, 9> depths = {-1.0f, 0.0f, 0.25f, 0.5f, 1.0f, 2.0f, 0.125f, 0.875f, 0.75f};
    for (unsigned comparison = 0; comparison < 18; ++comparison) {
      unsigned function = comparison % 9;
      float reference = comparison < 9 ? 0.5f : std::numeric_limits<float>::quiet_NaN();
      for (unsigned sample = 0; sample < alphas.size(); ++sample) {
        float alpha = alphas[sample];
        std::array<Vec, 4> input = {{{depths[sample], 0.25f, 1.5f, alpha}, {-1.0f, 2.0f, 0.5f, 0.25f},
            {4.0f, 8.0f, -0.25f, 0.0f}, {0.03125f, 1.0f, -8.0f, 0.5f}}};
        WgslSystemConstants system;
        system.flags = system.kPrimitivePolygonal | (function < 8 ? system.kAlphaTest : 0u);
        system.alpha_test_function = function; system.alpha_test_reference = reference;
        const std::array<float, 4> scales{4.0f, 0.5f, 32.0f, 0.03125f};
        std::copy(scales.begin(), scales.end(), system.color_output_scale);
        gpu.device.GetQueue().WriteBuffer(uniform, 0, &system, sizeof(system));
        gpu.device.GetQueue().WriteBuffer(constants, 0, input.data(), sizeof(input));
        std::memcpy(&regs.values[XE_GPU_REG_SHADER_CONSTANT_000_X], input.data(), sizeof(input));
        std::fill_n(interpreter.temp_registers(), xenos::kMaxShaderTempRegisters * 4, 0.0f);
        for (unsigned i = 0; i < 4; ++i) { std::copy(input[i].begin(), input[i].end(), interpreter.temp_registers() + i * 4); }
        if (parameter_mode) {
          // The sampled pixel is (1,1), so floor(frag_coord.xy) is (1,1).
          bool back = parameter_mode == 2 || parameter_mode == 4;
          Vec parameters{back ? -1.0f : 1.0f, parameter_mode >= 3 ? -1.0f : 1.0f, 0, 0};
          std::copy(parameters.begin(), parameters.end(), interpreter.temp_registers() + 12);
        }
        Exports exports; interpreter.SetExportSink(&exports); interpreter.Execute();
        bool pass_alpha = function == 8 || AlphaPass(function, exports.values[0][3], reference);
        std::array<wgpu::RenderPassColorAttachment, 4> attachments;
        for (unsigned i = 0; i < 4; ++i) {
          attachments[i].view = targets[i].CreateView(); attachments[i].loadOp = wgpu::LoadOp::Clear;
          attachments[i].storeOp = wgpu::StoreOp::Store; attachments[i].clearValue = {0.125, 0.125, 0.125, 0.125};
        }
        wgpu::RenderPassDepthStencilAttachment depth;
        depth.view = targets[4].CreateView(); depth.depthLoadOp = wgpu::LoadOp::Clear;
        depth.depthStoreOp = wgpu::StoreOp::Store; depth.depthClearValue = 0.875f;
        wgpu::RenderPassDescriptor rp;
        rp.colorAttachmentCount = attachments.size(); rp.colorAttachments = attachments.data(); rp.depthStencilAttachment = &depth;
        auto encoder = gpu.device.CreateCommandEncoder(); auto render = encoder.BeginRenderPass(&rp);
        render.SetPipeline(pipeline); render.SetBindGroup(0, group); render.Draw(3); render.End();
        auto readback = gpu.Buffer(5 * 512, wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::CopySrc);
        for (unsigned i = 0; i < 5; ++i) {
          wgpu::TexelCopyTextureInfo from; from.texture = targets[i];
          if (i == 4) { from.aspect = wgpu::TextureAspect::DepthOnly; }
          wgpu::TexelCopyBufferInfo to; to.buffer = readback; to.layout.offset = i * 512;
          to.layout.bytesPerRow = 256; to.layout.rowsPerImage = 2;
          wgpu::Extent3D extent{2, 2, 1}; encoder.CopyTextureToBuffer(&from, &to, &extent);
        }
        auto command = encoder.Finish(); gpu.device.GetQueue().Submit(1, &command);
        auto bytes = gpu.Read(readback, 5 * 512);
        for (unsigned target = 0; target < 4; ++target) {
          for (unsigned c = 0; c < 4; ++c) {
            _Float16 stored; std::memcpy(&stored, bytes.data() + target * 512 + 256 + 8 + c * 2, 2);
            float expected = pass_alpha ? exports.values[target][c] * scales[target] : 0.125f;
            expected = float(_Float16(expected));
            if (!Equal(float(stored), expected)) {
              std::fprintf(stderr, "output param%u alpha-fn%u alpha%.9g ref%.9g target%u c%u GPU%.9g CPU%.9g\n",
                  parameter_mode, function, alpha, reference, target, c, float(stored), expected);
              GPU_CHECK(false);
            }
          }
        }
        float actual_depth; std::memcpy(&actual_depth, bytes.data() + 4 * 512 + 256 + 4, 4);
        float expected_depth = pass_alpha ? std::clamp(exports.values[61][0], 0.0f, 1.0f) : 0.875f;
        GPU_CHECK(Equal(actual_depth, expected_depth));
        ++cases;
      }
    }
  }
  CheckFormats(gpu, shader, interpreter, regs, vertex);
  std::printf("PASS: %u output cases: 4 MRTs, exponent scales, alpha comparisons/discards, depth, interpolators and facing/point parameter signs\n", cases);
}
