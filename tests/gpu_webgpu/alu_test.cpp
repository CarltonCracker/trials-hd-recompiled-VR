// Differential ALU tests: authored synthetic microcode vs the SDK interpreter.
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <random>
#include <string>

#include <rex/graphics/pipeline/shader/interpreter.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>
#include <rex/string/buffer.h>

#include "gpu_test.h"
#include "wgsl/wgsl_shader_translator.h"

using namespace rex::graphics;
using rex::graphics::webgpu::WgslShaderTranslator;
using Vec = std::array<float, 4>;
struct Input { Vec r[3]; };
struct Output { Vec r[5]; };

namespace {

std::array<uint32_t, 3> Alu(uint32_t vector_op, uint32_t scalar_op, bool clamp) {
  return {3u | (4u << 8) | (15u << 16) | (15u << 20) |
              (uint32_t(clamp) << 24) | (uint32_t(clamp) << 25) | (scalar_op << 26),
          0u, 2u | (1u << 8) | (vector_op << 24) | (7u << 29)};
}

std::array<uint32_t, 18> Microcode(uint32_t vector_op, uint32_t scalar_op, bool clamp) {
  std::array<uint32_t, 18> words{};
  // ExecEnd, five ALUs at instruction address 1; second CF is unused NOP.
  words[0] = 1u | (5u << 12);
  words[1] = uint32_t(ucode::ControlFlowOpcode::kExecEnd) << 12;
  // Seed ps with a nonzero scalar result without writing any destination.
  words[3] = uint32_t(ucode::AluScalarOpcode::kAdds) << 26;
  words[5] = 2u | (7u << 29);
  auto first = Alu(vector_op, scalar_op, clamp);
  if (clamp) {
    // Cover vector source swizzles, abs and negate in the second variant.
    first[1] = (0x39u << 16) | (0x1Bu << 8) | (1u << 26);
    first[2] |= 0x80u << 16;
  }
  if (scalar_op >= 42 && scalar_op <= 47) {
    first[2] &= ~(1u << 29);  // constant/register scalar op: src3 is c2
  }
  std::copy(first.begin(), first.end(), words.begin() + 6);
  // Observe the predicate with true/false-predicated writes to r5 and r6.
  for (uint32_t i = 0; i < 2; ++i) {
    words[9 + 3 * i] = (5u + i) | (15u << 16) |
                       (uint32_t(ucode::AluScalarOpcode::kRetainPrev) << 26);
    words[10 + 3 * i] = (1u << 28) | ((1u - i) << 27);
    words[11 + 3 * i] = (i << 8) | (i << 16) |
                       (uint32_t(ucode::AluVectorOpcode::kMax) << 24);
  }
  // Observe a0 through a dynamically indexed constant read to r7.
  words[15] = 7u | (15u << 16) | (uint32_t(ucode::AluScalarOpcode::kRetainPrev) << 26);
  words[16] = (1u << 29) | (1u << 30) | (1u << 31);
  words[17] = uint32_t(ucode::AluVectorOpcode::kMax) << 24;
  return words;
}

bool Equal(float actual, float expected) {
  if (std::isnan(expected)) {
    return std::isnan(actual);
  }
  if (!std::isfinite(expected)) {
    return actual == expected;
  }
  return std::abs(actual - expected) <= 2e-5f * std::max(1.0f, std::abs(expected));
}

void Run(GpuTest& gpu, uint32_t vector_op, uint32_t scalar_op, bool clamp,
         const std::vector<Input>& inputs, const std::array<Vec, 256>& constants,
         RegisterFile& regs, rex::memory::Memory& memory) {
  auto ucode = Microcode(vector_op, scalar_op, clamp);
  Shader shader(xenos::ShaderType::kVertex, 0, ucode.data(), ucode.size(), std::endian::native);
  rex::string::StringBuffer disassembly;
  shader.AnalyzeUcode(disassembly);
  WgslShaderTranslator translator;
  auto modification = translator.GetDefaultVertexShaderModification(64);
  auto* translation = shader.GetOrCreateTranslation(modification);
  GPU_CHECK(translator.TranslateAnalyzedShader(*translation));
  GPU_CHECK(translation->errors().empty());
  std::string source = translation->GetTranslatedBinaryString();
  source += R"(
struct TestInput { r: array<vec4f, 3> }
struct TestOutput { r: array<vec4f, 5> }
@group(1) @binding(1) var<storage, read> test_inputs: array<TestInput>;
@group(1) @binding(2) var<storage, read_write> test_outputs: array<TestOutput>;
@compute @workgroup_size(64)
fn test_main(@builtin(global_invocation_id) id: vec3u) {
  if (id.x >= arrayLength(&test_inputs)) { return; }
  xe_r[0] = test_inputs[id.x].r[0];
  xe_r[1] = test_inputs[id.x].r[1];
  xe_r[2] = test_inputs[id.x].r[2];
  xe_main_body();
  for (var i = 0u; i < 5u; i++) { test_outputs[id.x].r[i] = xe_r[3u + i]; }
}
)";
  auto module = gpu.Module(source);
  wgpu::ComputePipelineDescriptor pipeline_desc;
  pipeline_desc.compute.module = module;
  pipeline_desc.compute.entryPoint = "test_main";
  auto pipeline = gpu.device.CreateComputePipeline(&pipeline_desc);

  auto cb = gpu.Buffer(sizeof(constants), wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst,
                       constants.data());
  auto in = gpu.Buffer(inputs.size() * sizeof(Input),
                       wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst, inputs.data());
  auto out = gpu.Buffer(inputs.size() * sizeof(Output),
                        wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc);
  wgpu::BindGroupEntry c_entry;
  c_entry.binding = 1;
  c_entry.buffer = cb;
  c_entry.size = sizeof(constants);
  wgpu::BindGroupDescriptor bg_desc;
  bg_desc.layout = pipeline.GetBindGroupLayout(0);
  bg_desc.entryCount = 1;
  bg_desc.entries = &c_entry;
  auto c_bg = gpu.device.CreateBindGroup(&bg_desc);
  std::array<wgpu::BindGroupEntry, 2> entries;
  entries[0].binding = 1;
  entries[0].buffer = in;
  entries[0].size = inputs.size() * sizeof(Input);
  entries[1].binding = 2;
  entries[1].buffer = out;
  entries[1].size = inputs.size() * sizeof(Output);
  bg_desc.layout = pipeline.GetBindGroupLayout(1);
  bg_desc.entryCount = entries.size();
  bg_desc.entries = entries.data();
  auto io_bg = gpu.device.CreateBindGroup(&bg_desc);
  auto encoder = gpu.device.CreateCommandEncoder();
  auto pass = encoder.BeginComputePass();
  pass.SetPipeline(pipeline);
  pass.SetBindGroup(0, c_bg);
  pass.SetBindGroup(1, io_bg);
  pass.DispatchWorkgroups(uint32_t((inputs.size() + 63) / 64));
  pass.End();
  auto commands = encoder.Finish();
  gpu.device.GetQueue().Submit(1, &commands);
  auto bytes = gpu.Read(out, inputs.size() * sizeof(Output));

  ShaderInterpreter interpreter(regs, memory);
  interpreter.SetShader(shader);
  for (size_t i = 0; i < inputs.size(); ++i) {
    std::fill_n(interpreter.temp_registers(), xenos::kMaxShaderTempRegisters * 4, 0.0f);
    std::memcpy(interpreter.temp_registers(), &inputs[i], sizeof(Input));
    interpreter.Execute();
    Output actual;
    std::memcpy(&actual, bytes.data() + i * sizeof(Output), sizeof(Output));
    const float* expected = interpreter.temp_registers() + 3 * 4;
    for (unsigned r = 0; r < 5; ++r) {
      for (unsigned c = 0; c < 4; ++c) {
        if (!Equal(actual.r[r][c], expected[4 * r + c])) {
          std::fprintf(stderr, "ALU v%u s%u clamp%d sample%zu r%u.%u GPU %.9g CPU %.9g\n",
                       vector_op, scalar_op, clamp, i, r + 3, c, actual.r[r][c], expected[4 * r + c]);
          GPU_CHECK(false);
        }
      }
    }
  }
  std::printf("PASS v%u s%u clamp%d (%zu inputs)\n", vector_op, scalar_op, clamp, inputs.size());
}

// Real fragment-stage discard, not just the kill opcode's numerical result.
void PixelKill(GpuTest& gpu, uint32_t vector_op, uint32_t scalar_op, float value,
               RegisterFile& regs, rex::memory::Memory& memory) {
  const bool scalar = scalar_op != uint32_t(ucode::AluScalarOpcode::kRetainPrev);
  std::array<uint32_t, 9> code = {
      1u | (2u << 12), uint32_t(ucode::ControlFlowOpcode::kExecEnd) << 12, 0u,
      (1u << 15) | (15u << (scalar ? 20 : 16)) | (scalar_op << 26),
      0u, 2u | (1u << 8) | (vector_op << 24),
      (1u << 15) | (15u << 16) | (uint32_t(ucode::AluScalarOpcode::kRetainPrev) << 26),
      (1u << 29) | (1u << 30) | (1u << 31),
      (3u << 8) | (3u << 16) | (uint32_t(ucode::AluVectorOpcode::kMax) << 24)};
  Shader shader(xenos::ShaderType::kPixel, 0, code.data(), code.size(), std::endian::native);
  rex::string::StringBuffer disassembly;
  shader.AnalyzeUcode(disassembly);
  WgslShaderTranslator translator;
  WgslShaderTranslator::Modification modification;
  modification.color_targets = 1;
  auto* translation = shader.GetOrCreateTranslation(modification.value);
  GPU_CHECK(translator.TranslateAnalyzedShader(*translation));
  GPU_CHECK(translation->errors().empty());
  auto fragment = gpu.Module(translation->GetTranslatedBinaryString());
  auto vertex = gpu.Module(R"(@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
    let p = array<vec2f, 3>(vec2f(-1.0, -1.0), vec2f(3.0, -1.0), vec2f(-1.0, 3.0));
    return vec4f(p[i], 0.0, 1.0);
  })");
  wgpu::ColorTargetState color;
  color.format = wgpu::TextureFormat::RGBA8Unorm;
  wgpu::FragmentState fs;
  fs.module = fragment;
  fs.entryPoint = "main";
  fs.targetCount = 1;
  fs.targets = &color;
  wgpu::RenderPipelineDescriptor desc;
  desc.vertex.module = vertex;
  desc.vertex.entryPoint = "vs";
  desc.fragment = &fs;
  auto pipeline = gpu.device.CreateRenderPipeline(&desc);
  std::array<Vec, 256> constants{};
  constants[0].fill(value);
  constants[2].fill(value);
  constants[3] = {0, 1, 0, 1};
  reg::SQ_PS_CONST ps{};
  ps.base = 256;
  ps.size = 255;
  regs.values[XE_GPU_REG_SQ_PS_CONST] = ps.value;
  std::memcpy(&regs.values[XE_GPU_REG_SHADER_CONSTANT_000_X + 256 * 4],
              constants.data(), sizeof(constants));
  struct Sink : ShaderInterpreter::ExportSink {
    bool first = true;
    bool killed = false;
    void Export(ucode::ExportRegister, const float* value, uint32_t mask) override {
      if (first && (mask & 1u)) { killed = value[0] != 0.0f; first = false; }
    }
  } sink;
  ShaderInterpreter interpreter(regs, memory);
  interpreter.SetShader(shader);
  interpreter.SetExportSink(&sink);
  interpreter.Execute();
  GPU_CHECK(!sink.first);
  webgpu::WgslSystemConstants system;
  auto system_buffer = gpu.Buffer(sizeof(system),
      wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst, &system);
  auto constants_buffer = gpu.Buffer(sizeof(constants),
      wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst, constants.data());
  std::array<wgpu::BindGroupEntry, 2> entries;
  entries[0].binding = 0; entries[0].buffer = system_buffer; entries[0].size = sizeof(system);
  entries[1].binding = 2; entries[1].buffer = constants_buffer; entries[1].size = sizeof(constants);
  wgpu::BindGroupDescriptor bg_desc;
  bg_desc.layout = pipeline.GetBindGroupLayout(0);
  bg_desc.entryCount = entries.size(); bg_desc.entries = entries.data();
  auto bg = gpu.device.CreateBindGroup(&bg_desc);
  wgpu::TextureDescriptor texture_desc;
  texture_desc.size = {1, 1, 1};
  texture_desc.format = color.format;
  texture_desc.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
  auto texture = gpu.device.CreateTexture(&texture_desc);
  auto result = gpu.Buffer(256, wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::CopySrc);
  auto encoder = gpu.device.CreateCommandEncoder();
  wgpu::RenderPassColorAttachment attachment;
  attachment.view = texture.CreateView();
  attachment.loadOp = wgpu::LoadOp::Clear;
  attachment.storeOp = wgpu::StoreOp::Store;
  attachment.clearValue = {1, 0, 1, 1};
  wgpu::RenderPassDescriptor pass_desc;
  pass_desc.colorAttachmentCount = 1; pass_desc.colorAttachments = &attachment;
  auto pass = encoder.BeginRenderPass(&pass_desc);
  pass.SetPipeline(pipeline); pass.SetBindGroup(0, bg); pass.Draw(3); pass.End();
  wgpu::TexelCopyTextureInfo from;
  from.texture = texture;
  wgpu::TexelCopyBufferInfo to;
  to.buffer = result; to.layout.bytesPerRow = 256; to.layout.rowsPerImage = 1;
  wgpu::Extent3D extent{1, 1, 1};
  encoder.CopyTextureToBuffer(&from, &to, &extent);
  auto commands = encoder.Finish();
  gpu.device.GetQueue().Submit(1, &commands);
  auto bytes = gpu.Read(result, 256);
  const std::array<uint8_t, 4> expected = sink.killed ? std::array<uint8_t, 4>{255, 0, 255, 255}
                                                   : std::array<uint8_t, 4>{0, 255, 0, 255};
  GPU_CHECK(std::equal(expected.begin(), expected.end(), bytes.begin()));
}

}  // namespace

int main(int argc, char** argv) {
  rex::InitLogging();
  GpuTest gpu(argc > 1 ? unsigned(std::strtoul(argv[1], nullptr, 10)) : 0);
  rex::memory::Memory memory;  // ALU-only tests never access guest memory.
  RegisterFile regs;
  reg::SQ_VS_CONST vs{};
  vs.base = 0;
  vs.size = 255;
  regs.values[XE_GPU_REG_SQ_VS_CONST] = vs.value;
  std::array<Vec, 256> constants;
  for (unsigned i = 0; i < constants.size(); ++i) {
    constants[i].fill(float(i + 1));
  }
  std::memcpy(&regs.values[XE_GPU_REG_SHADER_CONSTANT_000_X], constants.data(), sizeof(constants));
  std::vector<Input> inputs(128);
  std::mt19937 random(0x5841095A);
  std::uniform_real_distribution<float> dist(-4.0f, 4.0f);
  for (auto& input : inputs) {
    for (auto& r : input.r) {
      for (auto& x : r) { x = dist(random); }
    }
    // Valid finite arguments for sqrt/log/rsq/rcp; negatives remain in vectors.
    input.r[2][3] = std::abs(input.r[2][3]) + 0.125f;
  }
  for (unsigned i = 0; i < 4; ++i) {
    for (auto& r : inputs[i].r) { r.fill(float(i)); }
    inputs[i].r[2][3] = float(i) + 0.125f;
  }
  unsigned opcodes = 0;
  for (uint32_t op = 0; op < 32; ++op) {
    if (std::string_view(ucode::GetAluVectorOpcodeInfo(ucode::AluVectorOpcode(op)).name)
            .starts_with("opcode_")) { continue; }
    for (bool clamp : {false, true}) {
      Run(gpu, op, uint32_t(ucode::AluScalarOpcode::kRetainPrev), clamp, inputs, constants, regs, memory);
    }
    ++opcodes;
  }
  for (uint32_t op = 0; op < 64; ++op) {
    if (std::string_view(ucode::GetAluScalarOpcodeInfo(ucode::AluScalarOpcode(op)).name)
            .starts_with("opcode_")) { continue; }
    for (bool clamp : {false, true}) {
      Run(gpu, uint32_t(ucode::AluVectorOpcode::kMaxA), op, clamp, inputs, constants, regs, memory);
    }
    ++opcodes;
  }
  // Explicit IEEE edge inputs: zero/denormal reciprocal, rsqrt and logarithm.
  // Keep maxa's source finite so this isolates the scalar operation.
  auto edge_inputs = std::vector<Input>(inputs.begin(), inputs.begin() + 4);
  edge_inputs[0].r[2][3] = 0.0f;
  edge_inputs[1].r[2][3] = -0.0f;
  edge_inputs[2].r[2][3] = std::bit_cast<float>(1u);
  edge_inputs[3].r[2][3] = std::bit_cast<float>(0x80000001u);
  for (uint32_t op = 13; op <= 22; ++op) {
    Run(gpu, uint32_t(ucode::AluVectorOpcode::kMaxA), op, false,
        edge_inputs, constants, regs, memory);
  }
  auto multiply_inputs = edge_inputs;
  for (auto& input : multiply_inputs) {
    input.r[0][0] = std::bit_cast<float>(0x7F800000u);
    input.r[1][0] = 0.0f;
    input.r[0][1] = 0.0f;
    input.r[1][1] = std::bit_cast<float>(0x7FC00001u);
    input.r[0][2] = std::bit_cast<float>(0x007FFFFFu);
    input.r[1][2] = std::bit_cast<float>(0x7E800000u);
  }
  Run(gpu, uint32_t(ucode::AluVectorOpcode::kMul),
      uint32_t(ucode::AluScalarOpcode::kRetainPrev), false,
      multiply_inputs, constants, regs, memory);
  auto nan_inputs = edge_inputs;
  nan_inputs[0].r[0][0] = std::bit_cast<float>(0x7FC00001u);
  nan_inputs[1].r[1][0] = std::bit_cast<float>(0x7FC00001u);
  nan_inputs[2].r[0][3] = std::bit_cast<float>(0x7FC00001u);
  nan_inputs[3].r[0][0] = std::bit_cast<float>(0x7F800000u);
  for (auto op : {ucode::AluVectorOpcode::kMax, ucode::AluVectorOpcode::kMin,
                  ucode::AluVectorOpcode::kMax4}) {
    for (bool clamp : {false, true}) {
      Run(gpu, uint32_t(op), uint32_t(ucode::AluScalarOpcode::kRetainPrev), clamp,
          nan_inputs, constants, regs, memory);
    }
  }
  auto state_inputs = edge_inputs;
  state_inputs[0].r[2][3] = std::bit_cast<float>(0x7FC00001u);
  state_inputs[1].r[2][0] = std::bit_cast<float>(0x7FC00001u);
  state_inputs[2].r[2][3] = std::bit_cast<float>(0x7F800000u);
  state_inputs[3].r[2][3] = -2.0f;
  for (auto op : {ucode::AluScalarOpcode::kMulsPrev2, ucode::AluScalarOpcode::kMaxAs,
                  ucode::AluScalarOpcode::kMaxAsf, ucode::AluScalarOpcode::kRsq}) {
    Run(gpu, uint32_t(ucode::AluVectorOpcode::kMaxA), uint32_t(op), false,
        state_inputs, constants, regs, memory);
  }
  for (float value : {-1.0f, 0.0f, 1.0f}) {
    for (uint32_t op = 24; op <= 27; ++op) {
      PixelKill(gpu, op, uint32_t(ucode::AluScalarOpcode::kRetainPrev), value, regs, memory);
    }
    for (uint32_t op = 35; op <= 39; ++op) {
      PixelKill(gpu, uint32_t(ucode::AluVectorOpcode::kAdd), op, value, regs, memory);
    }
  }
  GPU_CHECK(gpu.errors == 0);
  std::printf("PASS: %u ALU opcodes, clamped/unclamped, 128 inputs each, IEEE edges, 27 pixel kills\n", opcodes);
}
