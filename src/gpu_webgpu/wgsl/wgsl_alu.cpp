// rexgpu-webgpu - Xenos ALU instructions to WGSL. Semantics follow
// ShaderInterpreter::ExecuteAluInstruction (SDK interpreter.cpp).

#include <fmt/format.h>

#include <rex/graphics/format/ucode.h>

#include "wgsl/wgsl_shader_translator.h"

namespace rex::graphics::webgpu {

void WgslShaderTranslator::ProcessAluInstruction(const ParsedAluInstruction& instr,
                                                 uint8_t memexport_eM_potentially_written_before) {
  if (instr.IsNop()) {
    return;
  }
  BeginPredicate(instr.is_predicated, instr.predicate_condition);

  // Both operations read their operands before either result is stored.
  std::string vector_result = ProcessVectorAluOperation(instr);
  std::string scalar_result = ProcessScalarAluOperation(instr);
  std::string vector_value;
  if (!vector_result.empty()) {
    vector_value = fmt::format("xe_vector_{}", unique_id_++);
    Line(fmt::format("let {} = {};", vector_value, vector_result));
  }
  if (!scalar_result.empty()) {
    Line("xe_ps = " + scalar_result + ";");
  }
  StoreResult(instr.vector_and_constant_result,
              vector_value.empty() ? std::string("vec4f(0.0)") : vector_value);
  StoreResult(instr.scalar_result, "vec4f(xe_ps)");

  // A predicate change ends instruction-level predication for the next
  // instruction (BeginPredicate re-checks it anyway).
  EndPredicate();
}

std::string WgslShaderTranslator::ProcessVectorAluOperation(const ParsedAluInstruction& instr) {
  const ucode::AluVectorOpcodeInfo& info = ucode::GetAluVectorOpcodeInfo(instr.vector_opcode);
  if (!instr.vector_and_constant_result.GetUsedResultComponents() && !info.changed_state) {
    return {};
  }
  std::string a, b, c;
  if (instr.vector_operand_count > 0) {
    a = OperandVector(instr.vector_operands[0]);
  }
  if (instr.vector_operand_count > 1) {
    b = OperandVector(instr.vector_operands[1]);
  }
  if (instr.vector_operand_count > 2) {
    c = OperandVector(instr.vector_operands[2]);
  }
  // Operands are loaded into lets so expressions using them several times
  // evaluate them once (and before any result is stored).
  uint32_t id = unique_id_++;
  auto bind = [&](std::string& operand, char name) {
    if (operand.empty()) {
      return;
    }
    std::string variable = fmt::format("xe_v{}{}", name, id);
    Line(fmt::format("let {} = {};", variable, operand));
    operand = variable;
  };
  bind(a, 'a');
  bind(b, 'b');
  bind(c, 'c');

  using Op = ucode::AluVectorOpcode;
  switch (instr.vector_opcode) {
    case Op::kAdd:
      return a + " + " + b;
    case Op::kMul:
      return "xe_mul4(" + a + ", " + b + ")";
    case Op::kMax:
      return "xe_max4v(" + a + ", " + b + ")";
    case Op::kMin:
      return "xe_min4v(" + a + ", " + b + ")";
    case Op::kSeq:
      return "select(vec4f(0.0), vec4f(1.0), " + a + " == " + b + ")";
    case Op::kSgt:
      return "select(vec4f(0.0), vec4f(1.0), " + a + " > " + b + ")";
    case Op::kSge:
      return "select(vec4f(0.0), vec4f(1.0), " + a + " >= " + b + ")";
    case Op::kSne:
      return "select(vec4f(0.0), vec4f(1.0), " + a + " != " + b + ")";
    case Op::kFrc:
      return a + " - floor(" + a + ")";
    case Op::kTrunc:
      return "trunc(" + a + ")";
    case Op::kFloor:
      return "floor(" + a + ")";
    case Op::kMad:
      return "xe_mul4(" + a + ", " + b + ") + " + c;
    case Op::kCndEq:
      return "select(" + c + ", " + b + ", " + a + " == vec4f(0.0))";
    case Op::kCndGe:
      return "select(" + c + ", " + b + ", " + a + " >= vec4f(0.0))";
    case Op::kCndGt:
      return "select(" + c + ", " + b + ", " + a + " > vec4f(0.0))";
    case Op::kDp4:
      return "vec4f(xe_dp(" + a + ", " + b + ", 4u))";
    case Op::kDp3:
      return "vec4f(xe_dp(" + a + ", " + b + ", 3u))";
    case Op::kDp2Add:
      return "vec4f(xe_dp(" + a + ", " + b + ", 2u) + " + c + ".x)";
    case Op::kCube:
      return "xe_cube(" + a + ")";
    case Op::kMax4:
      return "vec4f(xe_max4(" + a + "))";
    case Op::kSetpEqPush:
    case Op::kSetpNePush:
    case Op::kSetpGtPush:
    case Op::kSetpGePush: {
      const char* compare = instr.vector_opcode == Op::kSetpEqPush   ? "=="
                            : instr.vector_opcode == Op::kSetpNePush ? "!="
                            : instr.vector_opcode == Op::kSetpGtPush ? ">"
                                                                     : ">=";
      Line(fmt::format("xe_p0 = {}.w == 0.0 && {}.w {} 0.0;", a, b, compare));
      return fmt::format("vec4f(select({}.x + 1.0, 0.0, {}.x == 0.0 && {}.x {} 0.0))", a, a, b,
                         compare);
    }
    case Op::kKillEq:
    case Op::kKillGt:
    case Op::kKillGe:
    case Op::kKillNe: {
      const char* compare = instr.vector_opcode == Op::kKillEq   ? "=="
                            : instr.vector_opcode == Op::kKillGt ? ">"
                            : instr.vector_opcode == Op::kKillGe ? ">="
                                                                 : "!=";
      std::string kill = fmt::format("any({} {} {})", a, compare, b);
      if (is_pixel_shader()) {
        Open("if (" + kill + ")");
        Line("discard;");
        Close();
      }
      return fmt::format("vec4f(select(0.0, 1.0, {}))", kill);
    }
    case Op::kDst:
      return fmt::format("vec4f(1.0, xe_mul({}.y, {}.y), {}.z, {}.w)", a, b, a, b);
    case Op::kMaxA:
      Line(fmt::format("xe_a0 = xe_address({}.w, true);", a));
      return "xe_max4v(" + a + ", " + b + ")";
    default:
      EmitTranslationError(
          fmt::format("Unsupported vector ALU opcode {}", uint32_t(instr.vector_opcode)).c_str(),
          false);
      return "vec4f(0.0)";
  }
}

std::string WgslShaderTranslator::ProcessScalarAluOperation(const ParsedAluInstruction& instr) {
  using Op = ucode::AluScalarOpcode;
  if (instr.scalar_opcode == Op::kRetainPrev) {
    return {};
  }
  // Scalar operands: one operand with one or two components (.w / .wx), or
  // two operands (c#.w, r#.x).
  std::string a, b;
  if (instr.scalar_operand_count >= 1) {
    a = OperandScalar(instr.scalar_operands[0], 0);
    if (instr.scalar_operand_count >= 2) {
      b = OperandScalar(instr.scalar_operands[1], 0);
    } else if (instr.scalar_operands[0].component_count >= 2) {
      b = OperandScalar(instr.scalar_operands[0], 1);
    }
  }
  uint32_t id = unique_id_++;
  if (!a.empty()) {
    Line(fmt::format("let xe_sa{} = {};", id, a));
    a = fmt::format("xe_sa{}", id);
  }
  if (!b.empty()) {
    Line(fmt::format("let xe_sb{} = {};", id, b));
    b = fmt::format("xe_sb{}", id);
  }

  switch (instr.scalar_opcode) {
    case Op::kAdds:
    case Op::kAddsc0:
    case Op::kAddsc1:
      return a + " + " + b;
    case Op::kAddsPrev:
      return a + " + xe_ps";
    case Op::kMuls:
    case Op::kMulsc0:
    case Op::kMulsc1:
      return "xe_mul(" + a + ", " + b + ")";
    case Op::kMulsPrev:
      return "xe_mul(" + a + ", xe_ps)";
    case Op::kMulsPrev2:
      return fmt::format(
          "select(xe_mul({0}, xe_ps), -xe_flt_max, xe_ps == -xe_flt_max || xe_is_inf(xe_ps) || "
          "xe_ps != xe_ps || xe_is_inf({1}) || {1} != {1} || {1} <= 0.0)",
          a, b);
    case Op::kMaxs:
      return "xe_max(" + a + ", " + b + ")";
    case Op::kMins:
      return "xe_min(" + a + ", " + b + ")";
    case Op::kSeqs:
      return "select(0.0, 1.0, " + a + " == 0.0)";
    case Op::kSgts:
      return "select(0.0, 1.0, " + a + " > 0.0)";
    case Op::kSges:
      return "select(0.0, 1.0, " + a + " >= 0.0)";
    case Op::kSnes:
      return "select(0.0, 1.0, " + a + " != 0.0)";
    case Op::kFrcs:
      return a + " - floor(" + a + ")";
    case Op::kTruncs:
      return "trunc(" + a + ")";
    case Op::kFloors:
      return "floor(" + a + ")";
    case Op::kExp:
      return "exp2(" + a + ")";
    case Op::kLogc:
      return "xe_clamp_inf_to_max(log2(" + a + "))";
    case Op::kLog:
      return "log2(" + a + ")";
    case Op::kRcpc:
      return "xe_clamp_inf_to_max(1.0 / " + a + ")";
    case Op::kRcpf:
      return "xe_clamp_inf_to_zero(1.0 / " + a + ")";
    case Op::kRcp:
      return "1.0 / " + a;
    case Op::kRsqc:
      return "xe_clamp_inf_to_max(1.0 / sqrt(" + a + "))";
    case Op::kRsqf:
      return "xe_clamp_inf_to_zero(1.0 / sqrt(" + a + "))";
    case Op::kRsq:
      return "1.0 / sqrt(" + a + ")";
    case Op::kMaxAs:
      Line(fmt::format("xe_a0 = xe_address({}, true);", a));
      return "xe_max(" + a + ", " + b + ")";
    case Op::kMaxAsf:
      Line(fmt::format("xe_a0 = xe_address({}, false);", a));
      return "xe_max(" + a + ", " + b + ")";
    case Op::kSubs:
    case Op::kSubsc0:
    case Op::kSubsc1:
      return a + " - " + b;
    case Op::kSubsPrev:
      return a + " - xe_ps";
    case Op::kSetpEq:
      Line("xe_p0 = " + a + " == 0.0;");
      return "select(1.0, 0.0, xe_p0)";
    case Op::kSetpNe:
      Line("xe_p0 = " + a + " != 0.0;");
      return "select(1.0, 0.0, xe_p0)";
    case Op::kSetpGt:
      Line("xe_p0 = " + a + " > 0.0;");
      return "select(1.0, 0.0, xe_p0)";
    case Op::kSetpGe:
      Line("xe_p0 = " + a + " >= 0.0;");
      return "select(1.0, 0.0, xe_p0)";
    case Op::kSetpInv:
      Line("xe_p0 = " + a + " == 1.0;");
      return fmt::format("select(select({0}, 1.0, {0} == 0.0), 0.0, xe_p0)", a);
    case Op::kSetpPop:
      Line(fmt::format("xe_p0 = {} - 1.0 <= 0.0;", a));
      return fmt::format("select({} - 1.0, 0.0, xe_p0)", a);
    case Op::kSetpClr:
      Line("xe_p0 = false;");
      return "xe_flt_max";
    case Op::kSetpRstr:
      Line("xe_p0 = " + a + " == 0.0;");
      return fmt::format("select({}, 0.0, xe_p0)", a);
    case Op::kKillsEq:
    case Op::kKillsGt:
    case Op::kKillsGe:
    case Op::kKillsNe:
    case Op::kKillsOne: {
      std::string kill = instr.scalar_opcode == Op::kKillsEq   ? a + " == 0.0"
                         : instr.scalar_opcode == Op::kKillsGt ? a + " > 0.0"
                         : instr.scalar_opcode == Op::kKillsGe ? a + " >= 0.0"
                         : instr.scalar_opcode == Op::kKillsNe ? a + " != 0.0"
                                                               : a + " == 1.0";
      if (is_pixel_shader()) {
        Open("if (" + kill + ")");
        Line("discard;");
        Close();
      }
      return "select(0.0, 1.0, " + kill + ")";
    }
    case Op::kSqrt:
      return "sqrt(" + a + ")";
    case Op::kSin:
      return "sin(" + a + ")";
    case Op::kCos:
      return "cos(" + a + ")";
    default:
      EmitTranslationError(
          fmt::format("Unsupported scalar ALU opcode {}", uint32_t(instr.scalar_opcode)).c_str(),
          false);
      return "0.0";
  }
}

}  // namespace rex::graphics::webgpu
