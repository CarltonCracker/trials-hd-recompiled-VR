// rexgpu-webgpu - Xenos microcode to WGSL translator.
//
// One WGSL module per shader and modification. Binding model and design:
// docs/webgpu-renderer/README.md, sections 7.6.5 and 8.4. Semantics follow the
// SDK's ShaderInterpreter (ALU) and SpirvShaderTranslator (fetch, outputs).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <rex/graphics/pipeline/shader/translator.h>

namespace rex::graphics::webgpu {

// Bindings (group, binding), shared with the pipeline layout (7.6.5).
namespace wgsl_binding {
constexpr uint32_t kGroupDraw = 0;
constexpr uint32_t kSystemConstants = 0;
constexpr uint32_t kVertexFloatConstants = 1;
constexpr uint32_t kPixelFloatConstants = 2;
constexpr uint32_t kBoolLoopConstants = 3;
constexpr uint32_t kFetchConstants = 4;
constexpr uint32_t kGroupSharedMemory = 1;
constexpr uint32_t kSharedMemory = 0;
constexpr uint32_t kGroupTextures = 2;
constexpr uint32_t kFirstSampler = 16;
}  // namespace wgsl_binding

// Per-draw values the shaders read (group 0, binding 0). Layout matches the
// XeSystem struct in the generated WGSL; 256 bytes, 16-byte aligned members.
struct WgslSystemConstants {
  enum Flags : uint32_t {
    kXYDividedByW = 1u << 0,
    kZDividedByW = 1u << 1,
    kWNotReciprocal = 1u << 2,
    kVertexIndexLoad = 1u << 3,
    kVertexIndexLoad32Bit = 1u << 4,
    kPrimitivePolygonal = 1u << 5,
    kAlphaTest = 1u << 6,
  };
  uint32_t flags = 0;
  uint32_t vertex_index_endian = 0;
  int32_t vertex_base_index = 0;
  uint32_t vertex_index_load_address = 0;  // bytes, in shared memory
  uint32_t vertex_index_min = 0;
  uint32_t vertex_index_max = 0xFFFFFF;
  uint32_t alpha_test_function = 7;  // xenos::CompareFunction
  float alpha_test_reference = 0.0f;
  float ndc_scale[4] = {1.0f, 1.0f, 1.0f, 0.0f};
  float ndc_offset[4] = {};
  // Per color target: multiplier applied to the shader output (exponent bias
  // times the host format's range scale).
  float color_output_scale[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  uint32_t padding[44] = {};
};
static_assert(sizeof(WgslSystemConstants) == 256);

class WgslShaderTranslator : public ShaderTranslator {
 public:
  // Translation modification bits (Shader::Translation::modification()).
  union Modification {
    uint64_t value = 0;
    struct {
      // Registers needed for dynamic addressing (0 if not used).
      uint32_t dynamic_addressable_register_count : 7;
      // Vertex shaders: Shader::HostVertexShaderType.
      uint32_t host_vertex_shader_type : Shader::kHostVertexShaderTypeBitCount;
      // Interpolators linked between the vertex and the pixel shader.
      uint32_t interpolator_mask : 16;
      // Pixel shaders: PsParamGen enabled, its register, and point mode.
      uint32_t param_gen : 1;
      uint32_t param_gen_interpolator : 4;
      uint32_t param_gen_point : 1;
      // Pixel shaders: color targets bound in the pipeline (outputs declared).
      uint32_t color_targets : 4;
    };
  };
  static_assert(sizeof(Modification) == sizeof(uint64_t));

  WgslShaderTranslator() = default;

  uint64_t GetDefaultVertexShaderModification(
      uint32_t dynamic_addressable_register_count,
      Shader::HostVertexShaderType host_vertex_shader_type =
          Shader::HostVertexShaderType::kVertex) const override;
  uint64_t GetDefaultPixelShaderModification(
      uint32_t dynamic_addressable_register_count) const override;

  // Debugging (M3 silhouettes): texture fetches return 1 instead of 0 until
  // texture fetch is translated. Part of the translation, so set it before
  // translating anything.
  void set_untextured_fetch_value(float value) { untextured_fetch_value_ = value; }

  // Size of the float constant array the module declares (packed or 256).
  static uint32_t GetFloatConstantArraySize(const Shader& shader);

 protected:
  void Reset() override;
  uint32_t GetModificationRegisterCount() const override;
  void StartTranslation() override;
  std::vector<uint8_t> CompleteTranslation() override;

  void ProcessLabel(uint32_t cf_index) override;
  void ProcessExecInstructionBegin(const ParsedExecInstruction& instr) override;
  void ProcessExecInstructionEnd(const ParsedExecInstruction& instr) override;
  void ProcessLoopStartInstruction(const ParsedLoopStartInstruction& instr) override;
  void ProcessLoopEndInstruction(const ParsedLoopEndInstruction& instr) override;
  void ProcessCallInstruction(const ParsedCallInstruction& instr) override;
  void ProcessReturnInstruction(const ParsedReturnInstruction& instr) override;
  void ProcessJumpInstruction(const ParsedJumpInstruction& instr) override;
  void ProcessAllocInstruction(const ParsedAllocInstruction& instr, uint8_t export_eM) override;

  void ProcessVertexFetchInstruction(const ParsedVertexFetchInstruction& instr) override;
  void ProcessTextureFetchInstruction(const ParsedTextureFetchInstruction& instr) override;
  void ProcessAluInstruction(const ParsedAluInstruction& instr,
                             uint8_t memexport_eM_potentially_written_before) override;

 private:
  Modification modification() const { return Modification{current_translation().modification()}; }
  bool uses_pc() const { return !current_shader().label_addresses().empty(); }

  // Body text helpers.
  void Line(const std::string& text);
  void Open(const std::string& text);  // "text {" and indent
  void Close();                        // "}" and outdent

  // Condition expressions.
  std::string BoolConstantCondition(uint32_t bool_constant_index, bool condition) const;
  std::string PredicateCondition(bool condition) const;
  // Opens `if` blocks for exec conditions and instruction predicates.
  void BeginExecCondition(ParsedExecInstruction::Type type, uint32_t bool_constant_index,
                          bool condition);
  void EndExecCondition();
  void BeginPredicate(bool predicated, bool condition);
  void EndPredicate();

  // Operands and results.
  std::string OperandStorage(const InstructionOperand& operand) const;
  // The operand as a vec4f with its swizzle, abs and negation applied.
  std::string OperandVector(const InstructionOperand& operand) const;
  // One component of the operand (with modifiers) as an f32.
  std::string OperandScalar(const InstructionOperand& operand, uint32_t component) const;
  std::string StorageIndex(InstructionStorageAddressingMode mode, uint32_t index) const;
  // Stores `value` (a vec4f expression of the result before swizzling) to the
  // result target with the write mask, swizzle, constants and saturation.
  void StoreResult(const InstructionResult& result, const std::string& value);

  std::string ProcessVectorAluOperation(const ParsedAluInstruction& instr);
  std::string ProcessScalarAluOperation(const ParsedAluInstruction& instr);

  void EmitVertexShaderEntryPoint(std::string& out) const;
  void EmitPixelShaderEntryPoint(std::string& out) const;
  void EmitDeclarations(std::string& out) const;

  float untextured_fetch_value_ = 0.0f;
  std::string body_;
  uint32_t indent_ = 0;
  bool exec_condition_open_ = false;
  bool predicate_open_ = false;
  bool in_switch_case_ = false;
  uint32_t unique_id_ = 0;
  bool uses_vfetch_address_ = false;
  bool uses_shared_memory_ = false;
  bool uses_kill_ = false;
};

}  // namespace rex::graphics::webgpu
