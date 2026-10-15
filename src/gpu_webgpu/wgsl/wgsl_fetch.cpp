// rexgpu-webgpu - Xenos fetch instructions to WGSL. Vertex fetch reads guest
// memory directly (vertex pulling) with the semantics of
// SpirvShaderTranslator::ProcessVertexFetchInstruction.

#include <cmath>

#include <fmt/format.h>

#include <rex/graphics/format/ucode.h>
#include <rex/graphics/xenos.h>
#include <rex/math.h>

#include "wgsl/wgsl_shader_translator.h"

namespace rex::graphics::webgpu {

void WgslShaderTranslator::ProcessVertexFetchInstruction(
    const ParsedVertexFetchInstruction& instr) {
  if (!is_vertex_shader()) {
    // Vertex fetch from pixel shaders is not supported (no shared memory
    // binding in the fragment stage).
    StoreResult(instr.result, "vec4f(0.0)");
    return;
  }
  BeginPredicate(instr.is_predicated, instr.predicate_condition);

  uint32_t used_result_components = instr.result.GetUsedResultComponents();
  xenos::VertexFormat format = instr.attributes.data_format;
  uint32_t needed_words = xenos::GetVertexFormatNeededWords(format, used_result_components);
  uint32_t fetch_word_0 = instr.operands[1].storage_index << 1;

  if (!instr.is_mini_fetch) {
    // Base address (dwords) from bits 2:31 of the fetch constant, plus the
    // index times the stride; kept for vfetch_mini.
    std::string address = fmt::format("i32(xe_fetch_word({}u) >> 2u)", fetch_word_0);
    if (instr.attributes.stride) {
      std::string index = OperandScalar(instr.operands[0], 0);
      if (instr.attributes.is_index_rounded) {
        index = "(" + index + " + 0.5)";
      }
      address += fmt::format(" + i32(floor({})) * {}", index, instr.attributes.stride);
    }
    Line("xe_vfetch_address = " + address + ";");
  }
  if (!needed_words) {
    StoreResult(instr.result, "vec4f(0.0)");
    EndPredicate();
    return;
  }

  uint32_t id = unique_id_++;
  std::string words = fmt::format("xe_words{}", id);
  // Words of the element, endian-swapped (endianness in bits 0:1 of word 1).
  Line(fmt::format("var {}: vec4u;", words));
  Line(fmt::format("let xe_endian{} = xe_fetch_word({}u) & 3u;", id, fetch_word_0 + 1));
  Line(fmt::format("let xe_base{0} = xe_fetch_word({1}u) >> 2u;", id, fetch_word_0));
  Line(fmt::format("let xe_end{0} = xe_base{0} + ((xe_fetch_word({1}u) >> 2u) & 0xFFFFFFu);",
                  id, fetch_word_0 + 1));
  for (uint32_t i = 0; i < 4; ++i) {
    if (needed_words & (1u << i)) {
      int32_t offset = instr.attributes.offset + int32_t(i);
      Line(fmt::format("if (u32(xe_vfetch_address + {0}) >= xe_base{1} && "
                       "u32(xe_vfetch_address + {0}) < xe_end{1}) {{", offset, id));
      Line(fmt::format(
          "{}.{} = xe_endian_swap(xe_load_shared(xe_vfetch_address + {}), xe_endian{});", words,
          "xyzw"[i], offset, id));
      Line("}");
    }
  }

  std::string result;
  bool is_signed = instr.attributes.is_signed;
  bool normalize = !instr.attributes.is_integer;
  bool no_zero = instr.attributes.signed_rf_mode == xenos::SignedRepeatingFractionMode::kNoZero;
  int widths[4] = {}, offsets[4] = {}, word_of[4] = {};
  bool packed = false;
  switch (format) {
    case xenos::VertexFormat::k_8_8_8_8:
      packed = true;
      widths[0] = widths[1] = widths[2] = widths[3] = 8;
      offsets[1] = 8;
      offsets[2] = 16;
      offsets[3] = 24;
      break;
    case xenos::VertexFormat::k_2_10_10_10:
      packed = true;
      widths[0] = widths[1] = widths[2] = 10;
      widths[3] = 2;
      offsets[1] = 10;
      offsets[2] = 20;
      offsets[3] = 30;
      break;
    case xenos::VertexFormat::k_10_11_11:
      packed = true;
      widths[0] = widths[1] = 11;
      widths[2] = 10;
      offsets[1] = 11;
      offsets[2] = 22;
      break;
    case xenos::VertexFormat::k_11_11_10:
      packed = true;
      widths[0] = 10;
      widths[1] = widths[2] = 11;
      offsets[1] = 10;
      offsets[2] = 21;
      break;
    case xenos::VertexFormat::k_16_16:
      packed = true;
      widths[0] = widths[1] = 16;
      offsets[1] = 16;
      break;
    case xenos::VertexFormat::k_16_16_16_16:
      packed = true;
      widths[0] = widths[1] = widths[2] = widths[3] = 16;
      offsets[1] = offsets[3] = 16;
      word_of[2] = word_of[3] = 1;
      break;
    case xenos::VertexFormat::k_16_16_FLOAT:
      result = fmt::format("vec4f(xe_unpack_vertex_half({}.x), 0.0, 0.0)", words);
      break;
    case xenos::VertexFormat::k_16_16_16_16_FLOAT:
      result = fmt::format("vec4f(xe_unpack_vertex_half({0}.x), xe_unpack_vertex_half({0}.y))", words);
      break;
    case xenos::VertexFormat::k_32:
    case xenos::VertexFormat::k_32_32:
    case xenos::VertexFormat::k_32_32_32_32:
      if (is_signed) {
        result = fmt::format("vec4f(bitcast<vec4i>({}))", words);
        if (normalize) {
          result = no_zero ? fmt::format("({} + vec4f(0.5)) * (1.0 / 2147483647.5)", result)
                           : fmt::format("{} * (1.0 / 2147483647.0)", result);
        }
      } else {
        result = fmt::format("vec4f({})", words);
        if (normalize) {
          result = fmt::format("{} * (1.0 / 4294967295.0)", result);
        }
      }
      break;
    case xenos::VertexFormat::k_32_FLOAT:
    case xenos::VertexFormat::k_32_32_FLOAT:
    case xenos::VertexFormat::k_32_32_32_FLOAT:
    case xenos::VertexFormat::k_32_32_32_32_FLOAT:
      result = fmt::format("bitcast<vec4f>({})", words);
      break;
    default:
      EmitTranslationError(
          fmt::format("Unsupported vertex format {}", uint32_t(format)).c_str(), false);
      result = "vec4f(0.0)";
      break;
  }

  if (packed) {
    std::string components[4];
    uint32_t component_count = xenos::GetVertexFormatComponentCount(format);
    for (uint32_t i = 0; i < 4; ++i) {
      if (i >= component_count) {
        components[i] = "0.0";
        continue;
      }
      std::string word = fmt::format("{}.{}", words, "xyzw"[word_of[i]]);
      std::string value =
          is_signed ? fmt::format("f32(extractBits(bitcast<i32>({}), {}u, {}u))", word,
                                  offsets[i], widths[i])
                    : fmt::format("f32(extractBits({}, {}u, {}u))", word, offsets[i], widths[i]);
      if (normalize) {
        if (is_signed) {
          float scale_inv = float((1u << (widths[i] - 1)) - 1);
          if (no_zero) {
            scale_inv += 0.5f;
            value = fmt::format("(({} + 0.5) * {:.9g})", value, 1.0f / scale_inv);
          } else {
            value = fmt::format("max({} * {:.9g}, -1.0)", value, 1.0f / scale_inv);
          }
        } else {
          value = fmt::format("({} * {:.9g})", value, 1.0f / float((1u << widths[i]) - 1));
        }
      }
      components[i] = value;
    }
    result = fmt::format("vec4f({}, {}, {}, {})", components[0], components[1], components[2],
                         components[3]);
  }

  if (instr.attributes.exp_adjust) {
    result = fmt::format("({} * {:.9g})", result, std::ldexp(1.0f, instr.attributes.exp_adjust));
  }
  // Components missing from the format read as 0 (as in the other translators).
  uint32_t format_components = (1u << xenos::GetVertexFormatComponentCount(format)) - 1;
  if (used_result_components & ~format_components) {
    std::string mask;
    for (uint32_t i = 0; i < 4; ++i) {
      mask += (format_components & (1u << i)) ? "1.0" : "0.0";
      mask += i < 3 ? ", " : "";
    }
    result = fmt::format("({} * vec4f({}))", result, mask);
  }
  StoreResult(instr.result, result);
  EndPredicate();
}

void WgslShaderTranslator::ProcessTextureFetchInstruction(
    const ParsedTextureFetchInstruction& instr) {
  BeginPredicate(instr.is_predicated, instr.predicate_condition);
  switch (instr.opcode) {
    case ucode::FetchOpcode::kSetTextureLod:
      Line("xe_texture_lod = " + OperandScalar(instr.operands[0], 0) + ";");
      EndPredicate();
      return;
    case ucode::FetchOpcode::kSetTextureGradientsHorz:
    case ucode::FetchOpcode::kSetTextureGradientsVert:
      // Register gradients are not used (computed or explicit LOD only).
      EndPredicate();
      return;
    case ucode::FetchOpcode::kTextureFetch:
      break;
    default:
      // getWeights, getGradients, getCompTexLOD, getBorderColorFrac.
      StoreResult(instr.result, "vec4f(0.0)");
      EndPredicate();
      return;
  }
  if (!instr.result.GetUsedResultComponents()) {
    StoreResult(instr.result, "vec4f(0.0)");
    EndPredicate();
    return;
  }
  if (untextured_fetch_value_ != 0.0f) {
    StoreResult(instr.result, fmt::format("vec4f({:.1f})", untextured_fetch_value_));
    EndPredicate();
    return;
  }
  uint32_t fetch = instr.operands[1].storage_index;
  std::vector<TextureSlot> slots = GetTextureSlots(current_shader());
  uint32_t slot = UINT32_MAX;
  for (uint32_t i = 0; i < slots.size(); ++i) {
    if (slots[i].fetch_constant == fetch) {
      slot = i;
      break;
    }
  }
  bool is_cube = instr.dimension == xenos::FetchOpDimension::kCube;
  if (slot == UINT32_MAX || slots[slot].is_cube != is_cube ||
      instr.dimension == xenos::FetchOpDimension::k3DOrStacked) {
    // 3D and stacked textures are not used by Trials; mismatched dimensions
    // would need a second binding.
    StoreResult(instr.result, "vec4f(0.0)");
    EndPredicate();
    return;
  }

  uint32_t id = unique_id_++;
  Line(fmt::format("let xe_tc{} = {};", id, OperandVector(instr.operands[0])));
  std::string coordinates;
  const auto& attributes = instr.attributes;
  if (is_cube) {
    std::string face = fmt::format("xe_tc{}.z", id);
    if (attributes.offset_z != 0.0f) {
      face = fmt::format("({} + {:.9g})", face, attributes.offset_z);
    }
    coordinates =
        fmt::format("xe_cube_direction(vec3f(xe_tc{}.xy, {}))", id, face);
  } else {
    std::string uv = fmt::format("xe_tc{}.xy", id);
    bool needs_size = attributes.unnormalized_coordinates || attributes.offset_x != 0.0f ||
                      attributes.offset_y != 0.0f;
    if (needs_size) {
      Line(fmt::format("let xe_ts{} = xe_texture_size({}u);", id, fetch));
      if (attributes.offset_x != 0.0f || attributes.offset_y != 0.0f) {
        uv = fmt::format("({} + vec2f({:.9g}, {:.9g}) * xe_texture_offset_scale({}u){})", uv,
                         attributes.offset_x, attributes.offset_y, fetch,
                         attributes.unnormalized_coordinates ? "" : fmt::format(" / xe_ts{}", id));
      }
      if (attributes.unnormalized_coordinates) {
        uv = fmt::format("({} / xe_ts{})", uv, id);
      }
    }
    coordinates = uv;
  }
  std::string bias = fmt::format("({:.9g} + xe_texture_lod_bias({}u))", attributes.lod_bias, fetch);
  std::string sample;
  if (is_pixel_shader() && attributes.use_computed_lod && !attributes.use_register_lod) {
    sample = fmt::format("textureSampleBias(xe_texture_{}, xe_sampler_{}, {}, clamp({}, -16.0, 15.0))",
                         slot, slot, coordinates, bias);
  } else {
    std::string lod = attributes.use_register_lod ? "xe_texture_lod" : "0.0";
    sample = fmt::format("textureSampleLevel(xe_texture_{}, xe_sampler_{}, {}, {} + {})", slot,
                         slot, coordinates, lod, bias);
  }
  StoreResult(instr.result, fmt::format("xe_texture_postprocess({}, {}u)", sample, fetch));
  EndPredicate();
}

}  // namespace rex::graphics::webgpu
