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

int main(int argc, char** argv) {
  rex::InitLogging();
  GpuTest gpu(argc > 1 ? unsigned(std::strtoul(argv[1], nullptr, 10)) : 0);
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
  std::printf("PASS: %u output cases: 4 MRTs, exponent scales, alpha comparisons/discards, depth, interpolators and facing/point parameter signs\n", cases);
}
