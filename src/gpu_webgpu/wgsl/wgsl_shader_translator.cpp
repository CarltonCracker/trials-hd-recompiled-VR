// rexgpu-webgpu - Xenos microcode to WGSL translator: control flow, operands,
// results, declarations and entry points. ALU: wgsl_alu.cpp; fetch:
// wgsl_fetch.cpp.

#include "wgsl/wgsl_shader_translator.h"

#include <fmt/format.h>

#include <rex/graphics/xenos.h>
#include <rex/math.h>

#include "wgsl/wgsl_prelude.wgsl.h"

namespace rex::graphics::webgpu {

namespace {
constexpr char kComponentChars[] = {'x', 'y', 'z', 'w'};
}  // namespace

uint64_t WgslShaderTranslator::GetDefaultVertexShaderModification(
    uint32_t dynamic_addressable_register_count,
    Shader::HostVertexShaderType host_vertex_shader_type) const {
  Modification modification;
  modification.dynamic_addressable_register_count = dynamic_addressable_register_count;
  modification.host_vertex_shader_type = uint32_t(host_vertex_shader_type);
  return modification.value;
}

uint64_t WgslShaderTranslator::GetDefaultPixelShaderModification(
    uint32_t dynamic_addressable_register_count) const {
  Modification modification;
  modification.dynamic_addressable_register_count = dynamic_addressable_register_count;
  modification.color_targets = 0b1111;
  return modification.value;
}

uint32_t WgslShaderTranslator::GetFloatConstantArraySize(const Shader& shader) {
  const Shader::ConstantRegisterMap& map = shader.constant_register_map();
  if (map.float_dynamic_addressing) {
    return 256;
  }
  return std::max(map.float_count, uint32_t(1));
}

void WgslShaderTranslator::Reset() {
  ShaderTranslator::Reset();
  body_.clear();
  indent_ = 1;
  exec_condition_open_ = false;
  predicate_open_ = false;
  in_switch_case_ = false;
  unique_id_ = 0;
  uses_vfetch_address_ = false;
  uses_shared_memory_ = false;
  uses_kill_ = false;
}

uint32_t WgslShaderTranslator::GetModificationRegisterCount() const {
  return modification().dynamic_addressable_register_count;
}

void WgslShaderTranslator::Line(const std::string& text) {
  body_.append(size_t(indent_) * 2, ' ');
  body_ += text;
  body_ += '\n';
}

void WgslShaderTranslator::Open(const std::string& text) {
  Line(text + " {");
  ++indent_;
}

void WgslShaderTranslator::Close() {
  --indent_;
  Line("}");
}

void WgslShaderTranslator::StartTranslation() {
  if (uses_pc()) {
    Line("var xe_pc: u32 = 0u;");
    Open("loop");
    Open("switch (xe_pc)");
    Open("case 0u:");
    in_switch_case_ = true;
  }
}

std::vector<uint8_t> WgslShaderTranslator::CompleteTranslation() {
  EndPredicate();
  EndExecCondition();
  if (uses_pc()) {
    Line("return;");
    Close();  // case
    Open("default:");
    Line("return;");
    Close();
    Close();  // switch
    Close();  // loop
  }

  std::string out;
  out.reserve(body_.size() + 8192);
  out += "// Translated from Xenos microcode by rexgpu-webgpu (not game data).\n";
  // Xenos (and Xenia) compute derivatives and texture LODs under any control
  // flow; WGSL's uniformity analysis would reject that.
  out += "diagnostic(off, derivative_uniformity);\n\n";
  out += kWgsl_wgsl_prelude;
  out += '\n';
  EmitDeclarations(out);
  out += "fn xe_main_body() {\n";
  out += body_;
  out += "}\n\n";
  if (is_vertex_shader()) {
    EmitVertexShaderEntryPoint(out);
  } else {
    EmitPixelShaderEntryPoint(out);
  }
  return std::vector<uint8_t>(out.begin(), out.end());
}

void WgslShaderTranslator::EmitDeclarations(std::string& out) const {
  const Shader& shader = current_shader();
  out += R"(struct XeSystem {
  flags: u32,
  vertex_index_endian: u32,
  vertex_base_index: i32,
  vertex_index_load_address: u32,
  vertex_index_min: u32,
  vertex_index_max: u32,
  alpha_test_function: u32,
  alpha_test_reference: f32,
  ndc_scale: vec4f,
  ndc_offset: vec4f,
  color_output_scale: vec4f,
}
)";
  out += fmt::format("@group({}) @binding({}) var<uniform> xe_system: XeSystem;\n",
                     wgsl_binding::kGroupDraw, wgsl_binding::kSystemConstants);
  out += fmt::format("@group({}) @binding({}) var<uniform> xe_c: array<vec4f, {}>;\n",
                     wgsl_binding::kGroupDraw,
                     is_vertex_shader() ? wgsl_binding::kVertexFloatConstants
                                        : wgsl_binding::kPixelFloatConstants,
                     GetFloatConstantArraySize(shader));
  out += fmt::format("@group({}) @binding({}) var<uniform> xe_bool_loop: array<vec4u, 10>;\n",
                     wgsl_binding::kGroupDraw, wgsl_binding::kBoolLoopConstants);
  out += fmt::format("@group({}) @binding({}) var<uniform> xe_fetch: array<vec4u, 48>;\n",
                     wgsl_binding::kGroupDraw, wgsl_binding::kFetchConstants);
  if (is_vertex_shader()) {
    out += fmt::format(
        "@group({}) @binding({}) var<storage, read> xe_shared_memory: array<u32>;\n",
        wgsl_binding::kGroupSharedMemory, wgsl_binding::kSharedMemory);
  }
  out += '\n';

  if (shader.constant_register_map().float_dynamic_addressing) {
    out += R"(fn xe_c_dynamic(index: i32) -> vec4f {
  if (index < 0 || index > 255) { return vec4f(0.0); }
  return xe_c[index];
}
)";
  }
  out += R"(fn xe_loop_constant(index: u32) -> u32 {
  return xe_bool_loop[(8u + index) >> 2u][(8u + index) & 3u];
}
fn xe_fetch_word(index: u32) -> u32 {
  return xe_fetch[index >> 2u][index & 3u];
}
)";
  if (is_vertex_shader()) {
    out += R"(fn xe_load_shared(address_dwords: i32) -> u32 {
  if (address_dwords < 0 || u32(address_dwords) >= arrayLength(&xe_shared_memory)) {
    return 0u;
  }
  return xe_shared_memory[address_dwords];
}
)";
  }
  out += '\n';

  uint32_t register_count = this->register_count();
  if (register_count) {
    out += fmt::format("var<private> xe_r: array<vec4f, {}>;\n", register_count);
  }
  out += "var<private> xe_p0: bool;\n";
  out += "var<private> xe_ps: f32;\n";
  out += "var<private> xe_a0: i32;\n";
  out += "var<private> xe_loop_count: vec4u;\n";
  out += "var<private> xe_aL: vec4i;\n";
  out += "var<private> xe_vfetch_address: i32;\n";
  if (is_vertex_shader()) {
    out += "var<private> xe_out_position: vec4f;\n";
    out += "var<private> xe_out_interpolators: array<vec4f, 16>;\n";
    out += "var<private> xe_out_point_misc: vec4f;\n";
    out += "var<private> xe_eA: vec4f;\n";
    out += "var<private> xe_eM: array<vec4f, 5>;\n";
  } else {
    out += "var<private> xe_out_color: array<vec4f, 4>;\n";
    out += "var<private> xe_out_depth: f32;\n";
  }
  out += '\n';
}

void WgslShaderTranslator::EmitVertexShaderEntryPoint(std::string& out) const {
  Modification modification = this->modification();
  out += "struct XeVertexOut {\n  @invariant @builtin(position) position: vec4f,\n";
  for (uint32_t i = 0; i < 16; ++i) {
    if (modification.interpolator_mask & (1u << i)) {
      out += fmt::format("  @location({}) interpolator_{}: vec4f,\n", i, i);
    }
  }
  out += "}\n\n";
  out += "@vertex\nfn main(@builtin(vertex_index) xe_vertex_index: u32) -> XeVertexOut {\n";
  out += R"(  var index = xe_vertex_index;
  if ((xe_system.flags & 8u) != 0u) {
    // Guest index buffer read in the shader (expanded primitives).
    let is_32bit = (xe_system.flags & 16u) != 0u;
    let address = xe_system.vertex_index_load_address + (index << select(1u, 2u, is_32bit));
    var loaded = xe_load_shared(i32(address >> 2u));
    if (!is_32bit) {
      loaded = extractBits(loaded, (address & 2u) << 3u, 16u);
    }
    index = xe_endian_swap(loaded, xe_system.vertex_index_endian);
  }
  index = clamp(u32(i32(index) + xe_system.vertex_base_index) & 0xFFFFFFu,
                xe_system.vertex_index_min, xe_system.vertex_index_max);
)";
  if (register_count()) {
    out += "  xe_r[0].x = f32(index);\n";
  }
  out += R"(  xe_main_body();
  var position = xe_out_position;
  if ((xe_system.flags & 4u) == 0u) {
    position.w = 1.0 / position.w;
  }
  if ((xe_system.flags & 1u) != 0u) {
    position = vec4f(position.xy * position.w, position.zw);
  }
  if ((xe_system.flags & 2u) != 0u) {
    position.z *= position.w;
  }
  position = vec4f(position.xyz * xe_system.ndc_scale.xyz + xe_system.ndc_offset.xyz * position.w,
                   position.w);
  var xe_output: XeVertexOut;
  xe_output.position = position;
)";
  for (uint32_t i = 0; i < 16; ++i) {
    if (modification.interpolator_mask & (1u << i)) {
      out += fmt::format("  xe_output.interpolator_{} = xe_out_interpolators[{}];\n", i, i);
    }
  }
  out += "  return xe_output;\n}\n";
}

void WgslShaderTranslator::EmitPixelShaderEntryPoint(std::string& out) const {
  Modification modification = this->modification();
  const Shader& shader = current_shader();
  out += "struct XePixelIn {\n";
  out += "  @builtin(position) frag_coord: vec4f,\n";
  out += "  @builtin(front_facing) front_facing: bool,\n";
  for (uint32_t i = 0; i < 16; ++i) {
    if (modification.interpolator_mask & (1u << i)) {
      out += fmt::format("  @location({}) interpolator_{}: vec4f,\n", i, i);
    }
  }
  out += "}\n\n";
  uint32_t color_outputs = shader.writes_color_targets() & modification.color_targets;
  bool writes_depth = shader.writes_depth();
  if (color_outputs || writes_depth) {
    out += "struct XePixelOut {\n";
    for (uint32_t i = 0; i < 4; ++i) {
      if (color_outputs & (1u << i)) {
        out += fmt::format("  @location({}) color_{}: vec4f,\n", i, i);
      }
    }
    if (writes_depth) {
      out += "  @builtin(frag_depth) depth: f32,\n";
    }
    out += "}\n\n";
    out += "@fragment\nfn main(xe_in: XePixelIn) -> XePixelOut {\n";
  } else {
    out += "@fragment\nfn main(xe_in: XePixelIn) {\n";
  }
  uint32_t param_gen_register =
      modification.param_gen ? modification.param_gen_interpolator : UINT32_MAX;
  for (uint32_t i = 0; i < std::min(register_count(), uint32_t(16)); ++i) {
    if (i != param_gen_register && (modification.interpolator_mask & (1u << i))) {
      out += fmt::format("  xe_r[{}] = xe_in.interpolator_{};\n", i, i);
    }
  }
  if (param_gen_register < register_count()) {
    // X: pixel X, back-facing in the sign bit. Y: pixel Y, point in the sign
    // bit. Z, W: point coordinates (not supported: 0).
    out += "  {\n";
    out += "    var param_x = abs(floor(xe_in.frag_coord.x));\n";
    out += "    let param_y = abs(floor(xe_in.frag_coord.y));\n";
    out +=
        "    if ((xe_system.flags & 32u) != 0u && !xe_in.front_facing) { param_x = -param_x; }\n";
    out += fmt::format("    xe_r[{}] = vec4f(param_x, {}param_y, 0.0, 0.0);\n", param_gen_register,
                       modification.param_gen_point ? "-" : "");
    out += "  }\n";
  }
  out += "  xe_main_body();\n";
  if (color_outputs & 1) {
    // Alpha test (RB_COLORCONTROL) on the first color output.
    out += R"(  if ((xe_system.flags & 64u) != 0u) {
    let alpha = xe_out_color[0].a;
    let reference = xe_system.alpha_test_reference;
    var alpha_pass = true;
    switch (xe_system.alpha_test_function) {
      case 0u: { alpha_pass = false; }
      case 1u: { alpha_pass = alpha < reference; }
      case 2u: { alpha_pass = alpha == reference; }
      case 3u: { alpha_pass = alpha <= reference; }
      case 4u: { alpha_pass = alpha > reference; }
      case 5u: { alpha_pass = alpha != reference; }
      case 6u: { alpha_pass = alpha >= reference; }
      default: { alpha_pass = true; }
    }
    if (!alpha_pass) { discard; }
  }
)";
  }
  if (color_outputs || writes_depth) {
    out += "  var xe_output: XePixelOut;\n";
    for (uint32_t i = 0; i < 4; ++i) {
      if (color_outputs & (1u << i)) {
        out += fmt::format("  xe_output.color_{} = xe_out_color[{}] * xe_system.color_output_scale[{}];\n",
                           i, i, i);
      }
    }
    if (writes_depth) {
      out += "  xe_output.depth = saturate(xe_out_depth);\n";
    }
    out += "  return xe_output;\n";
  }
  out += "}\n";
}

std::string WgslShaderTranslator::BoolConstantCondition(uint32_t bool_constant_index,
                                                        bool condition) const {
  uint32_t word = bool_constant_index >> 5;
  return fmt::format("(((xe_bool_loop[{}][{}] >> {}u) & 1u) {} 0u)", word >> 2, word & 3,
                     bool_constant_index & 31, condition ? "!=" : "==");
}

std::string WgslShaderTranslator::PredicateCondition(bool condition) const {
  return condition ? "xe_p0" : "!xe_p0";
}

void WgslShaderTranslator::BeginExecCondition(ParsedExecInstruction::Type type,
                                              uint32_t bool_constant_index, bool condition) {
  EndPredicate();
  EndExecCondition();
  switch (type) {
    case ParsedExecInstruction::Type::kConditional:
      Open("if " + BoolConstantCondition(bool_constant_index, condition));
      exec_condition_open_ = true;
      break;
    case ParsedExecInstruction::Type::kPredicated:
      Open("if (" + PredicateCondition(condition) + ")");
      exec_condition_open_ = true;
      break;
    default:
      break;
  }
}

void WgslShaderTranslator::EndExecCondition() {
  if (exec_condition_open_) {
    Close();
    exec_condition_open_ = false;
  }
}

void WgslShaderTranslator::BeginPredicate(bool predicated, bool condition) {
  EndPredicate();
  if (predicated) {
    Open("if (" + PredicateCondition(condition) + ")");
    predicate_open_ = true;
  }
}

void WgslShaderTranslator::EndPredicate() {
  if (predicate_open_) {
    Close();
    predicate_open_ = false;
  }
}

void WgslShaderTranslator::ProcessLabel(uint32_t cf_index) {
  if (cf_index == 0) {
    return;
  }
  EndPredicate();
  EndExecCondition();
  // Fall through to the next case through the dispatch loop.
  Line(fmt::format("xe_pc = {}u;", cf_index));
  Close();
  Open(fmt::format("case {}u:", cf_index));
}

void WgslShaderTranslator::ProcessExecInstructionBegin(const ParsedExecInstruction& instr) {
  BeginExecCondition(instr.type, instr.bool_constant_index, instr.condition);
}

void WgslShaderTranslator::ProcessExecInstructionEnd(const ParsedExecInstruction& instr) {
  EndPredicate();
  if (instr.is_end) {
    Line("return;");
  }
  EndExecCondition();
}

void WgslShaderTranslator::ProcessLoopStartInstruction(const ParsedLoopStartInstruction& instr) {
  EndPredicate();
  EndExecCondition();
  Open("");
  Line(fmt::format("let loop_constant = xe_loop_constant({}u);", instr.loop_constant_index));
  Line("let loop_count = loop_constant & 0xFFu;");
  Line("xe_loop_count = vec4u(loop_count, xe_loop_count.xyz);");
  if (instr.is_repeat) {
    Line("xe_aL = vec4i(xe_aL.x, xe_aL.xyz);");
  } else {
    Line("xe_aL = vec4i(i32((loop_constant >> 8u) & 0xFFu), xe_aL.xyz);");
  }
  Open("if (loop_count == 0u)");
  Line(fmt::format("xe_pc = {}u;", instr.loop_skip_address));
  Line("break;");
  Close();
  Close();
}

void WgslShaderTranslator::ProcessLoopEndInstruction(const ParsedLoopEndInstruction& instr) {
  EndPredicate();
  EndExecCondition();
  Open("");
  Line("let loop_count = xe_loop_count.x - 1u;");
  std::string continue_condition = "loop_count != 0u";
  if (instr.is_predicated_break) {
    // Break if the predicate matches the condition.
    continue_condition += " && " + PredicateCondition(!instr.predicate_condition);
  }
  Open("if (" + continue_condition + ")");
  Line("xe_loop_count.x = loop_count;");
  Line(fmt::format("xe_aL.x += extractBits(bitcast<i32>(xe_loop_constant({}u)), 16u, 8u);",
                   instr.loop_constant_index));
  Line(fmt::format("xe_pc = {}u;", instr.loop_body_address));
  Line("break;");
  Close();
  Line("xe_loop_count = vec4u(xe_loop_count.yzw, 0u);");
  Line("xe_aL = vec4i(xe_aL.yzw, 0);");
  Close();
}

void WgslShaderTranslator::ProcessCallInstruction(const ParsedCallInstruction& instr) {
  // Not supported by the other translators either; not used by known games.
  EmitTranslationError("Control flow call is not supported", false);
}

void WgslShaderTranslator::ProcessReturnInstruction(const ParsedReturnInstruction& instr) {
  EmitTranslationError("Control flow return is not supported", false);
}

void WgslShaderTranslator::ProcessJumpInstruction(const ParsedJumpInstruction& instr) {
  ParsedExecInstruction::Type type = ParsedExecInstruction::Type::kUnconditional;
  if (instr.type == ParsedJumpInstruction::Type::kConditional) {
    type = ParsedExecInstruction::Type::kConditional;
  } else if (instr.type == ParsedJumpInstruction::Type::kPredicated) {
    type = ParsedExecInstruction::Type::kPredicated;
  }
  BeginExecCondition(type, instr.bool_constant_index, instr.condition);
  Line(fmt::format("xe_pc = {}u;", instr.target_address));
  Line("break;");
  EndExecCondition();
}

void WgslShaderTranslator::ProcessAllocInstruction(const ParsedAllocInstruction& instr,
                                                   uint8_t export_eM) {
  // Memory export (eA/eM# writes) is emitted by the memexport compute path
  // (task L-04g); for drawing, the exports are dropped.
}

std::string WgslShaderTranslator::StorageIndex(InstructionStorageAddressingMode mode,
                                               uint32_t index) const {
  switch (mode) {
    case InstructionStorageAddressingMode::kAddressRegisterRelative:
      return fmt::format("({} + xe_a0)", index);
    case InstructionStorageAddressingMode::kLoopRelative:
      return fmt::format("({} + xe_aL.x)", index);
    default:
      return fmt::format("{}", index);
  }
}

std::string WgslShaderTranslator::OperandStorage(const InstructionOperand& operand) const {
  switch (operand.storage_source) {
    case InstructionStorageSource::kRegister:
      if (!register_count()) {
        return "vec4f(0.0)";
      }
      return "xe_r[" + StorageIndex(operand.storage_addressing_mode, operand.storage_index) + "]";
    case InstructionStorageSource::kConstantFloat: {
      const Shader::ConstantRegisterMap& map = current_shader().constant_register_map();
      if (map.float_dynamic_addressing) {
        return "xe_c_dynamic(" +
               StorageIndex(operand.storage_addressing_mode, operand.storage_index) + ")";
      }
      uint32_t packed = map.GetPackedFloatConstantIndex(operand.storage_index);
      if (packed == UINT32_MAX) {
        return "vec4f(0.0)";
      }
      return fmt::format("xe_c[{}]", packed);
    }
    default:
      // Fetch constants are not ALU operands.
      return "vec4f(0.0)";
  }
}

std::string WgslShaderTranslator::OperandVector(const InstructionOperand& operand) const {
  std::string value = OperandStorage(operand);
  bool identity = true;
  std::string swizzle;
  for (uint32_t i = 0; i < 4; ++i) {
    SwizzleSource component = operand.GetComponent(i);
    identity &= component == GetSwizzleFromComponentIndex(i);
    swizzle += GetCharForSwizzle(component);
  }
  if (!identity) {
    value += "." + swizzle;
  }
  if (operand.is_absolute_value) {
    value = "abs(" + value + ")";
  }
  if (operand.is_negated) {
    value = "(-" + value + ")";
  }
  return value;
}

std::string WgslShaderTranslator::OperandScalar(const InstructionOperand& operand,
                                                uint32_t component) const {
  std::string value =
      OperandStorage(operand) + "." + GetCharForSwizzle(operand.GetComponent(component));
  if (operand.is_absolute_value) {
    value = "abs(" + value + ")";
  }
  if (operand.is_negated) {
    value = "(-" + value + ")";
  }
  return value;
}

void WgslShaderTranslator::StoreResult(const InstructionResult& result, const std::string& value) {
  uint32_t used_write_mask = result.GetUsedWriteMask();
  if (!used_write_mask) {
    return;
  }
  std::string target;
  switch (result.storage_target) {
    case InstructionStorageTarget::kRegister:
      if (!register_count()) {
        return;
      }
      target = "xe_r[" + StorageIndex(result.storage_addressing_mode, result.storage_index) + "]";
      break;
    case InstructionStorageTarget::kInterpolator:
      if (!is_vertex_shader()) {
        return;
      }
      target = fmt::format("xe_out_interpolators[{}]", result.storage_index);
      break;
    case InstructionStorageTarget::kPosition:
      if (!is_vertex_shader()) {
        return;
      }
      target = "xe_out_position";
      break;
    case InstructionStorageTarget::kPointSizeEdgeFlagKillVertex:
      if (!is_vertex_shader()) {
        return;
      }
      target = "xe_out_point_misc";
      break;
    case InstructionStorageTarget::kExportAddress:
      if (!is_vertex_shader()) {
        return;
      }
      target = "xe_eA";
      break;
    case InstructionStorageTarget::kExportData:
      if (!is_vertex_shader() || result.storage_index >= 5) {
        return;
      }
      target = fmt::format("xe_eM[{}]", result.storage_index);
      break;
    case InstructionStorageTarget::kColor:
      if (is_vertex_shader()) {
        return;
      }
      target = fmt::format("xe_out_color[{}]", result.storage_index);
      break;
    case InstructionStorageTarget::kDepth:
      if (is_vertex_shader()) {
        return;
      }
      target = "xe_out_depth";
      break;
    default:
      return;
  }

  std::string temp;
  if (result.GetUsedResultComponents()) {
    temp = fmt::format("xe_store_{}", unique_id_++);
    Line(fmt::format("let {} = {};", temp, value));
  }
  for (uint32_t i = 0; i < 4; ++i) {
    if (!(used_write_mask & (1u << i))) {
      continue;
    }
    std::string component_value;
    switch (result.components[i]) {
      case SwizzleSource::k0:
        component_value = "0.0";
        break;
      case SwizzleSource::k1:
        component_value = "1.0";
        break;
      default:
        component_value = temp + "." + GetCharForSwizzle(result.components[i]);
        if (result.is_clamped) {
          component_value = "saturate(" + component_value + ")";
        }
        break;
    }
    if (result.storage_target == InstructionStorageTarget::kDepth) {
      Line(target + " = " + component_value + ";");
    } else {
      Line(fmt::format("{}.{} = {};", target, kComponentChars[i], component_value));
    }
  }
}

}  // namespace rex::graphics::webgpu
