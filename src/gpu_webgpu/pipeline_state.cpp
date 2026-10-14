// rexgpu-webgpu - fixed-function pipeline state translation.

#include "pipeline_state.h"

#include <algorithm>
#include <cmath>

#include <rex/graphics/util/draw.h>

namespace rex::graphics::webgpu {

wgpu::BlendFactor ToBlendFactor(xenos::BlendFactor factor) {
  using F = xenos::BlendFactor;
  switch (factor) {
    case F::kZero:
      return wgpu::BlendFactor::Zero;
    case F::kOne:
      return wgpu::BlendFactor::One;
    case F::kSrcColor:
      return wgpu::BlendFactor::Src;
    case F::kOneMinusSrcColor:
      return wgpu::BlendFactor::OneMinusSrc;
    case F::kSrcAlpha:
      return wgpu::BlendFactor::SrcAlpha;
    case F::kOneMinusSrcAlpha:
      return wgpu::BlendFactor::OneMinusSrcAlpha;
    case F::kDstColor:
      return wgpu::BlendFactor::Dst;
    case F::kOneMinusDstColor:
      return wgpu::BlendFactor::OneMinusDst;
    case F::kDstAlpha:
      return wgpu::BlendFactor::DstAlpha;
    case F::kOneMinusDstAlpha:
      return wgpu::BlendFactor::OneMinusDstAlpha;
    case F::kConstantColor:
    case F::kConstantAlpha:
      return wgpu::BlendFactor::Constant;
    case F::kOneMinusConstantColor:
    case F::kOneMinusConstantAlpha:
      return wgpu::BlendFactor::OneMinusConstant;
    case F::kSrcAlphaSaturate:
      return wgpu::BlendFactor::SrcAlphaSaturated;
    default:
      return wgpu::BlendFactor::One;
  }
}

wgpu::BlendFactor ToAlphaBlendFactor(xenos::BlendFactor factor) {
  using F = xenos::BlendFactor;
  switch (factor) {
    case F::kSrcColor:
      return wgpu::BlendFactor::SrcAlpha;
    case F::kOneMinusSrcColor:
      return wgpu::BlendFactor::OneMinusSrcAlpha;
    case F::kDstColor:
      return wgpu::BlendFactor::DstAlpha;
    case F::kOneMinusDstColor:
      return wgpu::BlendFactor::OneMinusDstAlpha;
    case F::kSrcAlphaSaturate:
      return wgpu::BlendFactor::One;
    default:
      return ToBlendFactor(factor);
  }
}

wgpu::BlendOperation ToBlendOperation(xenos::BlendOp op) {
  switch (op) {
    case xenos::BlendOp::kSubtract:
      return wgpu::BlendOperation::Subtract;
    case xenos::BlendOp::kMin:
      return wgpu::BlendOperation::Min;
    case xenos::BlendOp::kMax:
      return wgpu::BlendOperation::Max;
    case xenos::BlendOp::kRevSubtract:
      return wgpu::BlendOperation::ReverseSubtract;
    default:
      return wgpu::BlendOperation::Add;
  }
}

bool TranslateBlendControl(reg::RB_BLENDCONTROL blend_control, wgpu::BlendState& out_blend) {
  bool blending_disabled =
      blend_control.color_srcblend == xenos::BlendFactor::kOne &&
      blend_control.color_destblend == xenos::BlendFactor::kZero &&
      blend_control.color_comb_fcn == xenos::BlendOp::kAdd &&
      blend_control.alpha_srcblend == xenos::BlendFactor::kOne &&
      blend_control.alpha_destblend == xenos::BlendFactor::kZero &&
      blend_control.alpha_comb_fcn == xenos::BlendOp::kAdd;

  if (blending_disabled) {
    return false;
  }

  out_blend.color.srcFactor = ToBlendFactor(blend_control.color_srcblend);
  out_blend.color.dstFactor = ToBlendFactor(blend_control.color_destblend);
  out_blend.color.operation = ToBlendOperation(blend_control.color_comb_fcn);
  out_blend.alpha.srcFactor = ToAlphaBlendFactor(blend_control.alpha_srcblend);
  out_blend.alpha.dstFactor = ToAlphaBlendFactor(blend_control.alpha_destblend);
  out_blend.alpha.operation = ToBlendOperation(blend_control.alpha_comb_fcn);

  // Min and max ignore the factors in WebGPU; the WebGPU specification requires them to be One.
  for (wgpu::BlendComponent* component : {&out_blend.color, &out_blend.alpha}) {
    if (component->operation == wgpu::BlendOperation::Min ||
        component->operation == wgpu::BlendOperation::Max) {
      component->srcFactor = wgpu::BlendFactor::One;
      component->dstFactor = wgpu::BlendFactor::One;
    }
  }

  return true;
}

wgpu::CompareFunction ToCompareFunction(xenos::CompareFunction function) {
  static constexpr wgpu::CompareFunction kFunctions[] = {
      wgpu::CompareFunction::Never,        wgpu::CompareFunction::Less,
      wgpu::CompareFunction::Equal,        wgpu::CompareFunction::LessEqual,
      wgpu::CompareFunction::Greater,      wgpu::CompareFunction::NotEqual,
      wgpu::CompareFunction::GreaterEqual, wgpu::CompareFunction::Always,
  };
  return kFunctions[uint32_t(function) & 7];
}

wgpu::StencilOperation ToStencilOperation(xenos::StencilOp op) {
  static constexpr wgpu::StencilOperation kOperations[] = {
      wgpu::StencilOperation::Keep,           wgpu::StencilOperation::Zero,
      wgpu::StencilOperation::Replace,        wgpu::StencilOperation::IncrementClamp,
      wgpu::StencilOperation::DecrementClamp, wgpu::StencilOperation::Invert,
      wgpu::StencilOperation::IncrementWrap,  wgpu::StencilOperation::DecrementWrap,
  };
  return kOperations[uint32_t(op) & 7];
}

void TranslateStencilFace(xenos::CompareFunction func, xenos::StencilOp fail,
                          xenos::StencilOp zfail, xenos::StencilOp zpass,
                          wgpu::StencilFaceState& out_face) {
  out_face.compare = ToCompareFunction(func);
  out_face.failOp = ToStencilOperation(fail);
  out_face.depthFailOp = ToStencilOperation(zfail);
  out_face.passOp = ToStencilOperation(zpass);
}

void TranslateDepthStencilState(reg::RB_DEPTHCONTROL depth_control,
                                reg::RB_STENCILREFMASK stencil_mask,
                                wgpu::TextureFormat format,
                                int32_t depth_bias,
                                float depth_bias_slope_scale,
                                wgpu::DepthStencilState& out_state) {
  out_state.format = format;
  out_state.depthWriteEnabled =
      depth_control.z_enable && depth_control.z_write_enable ? wgpu::OptionalBool::True
                                                             : wgpu::OptionalBool::False;
  out_state.depthCompare = depth_control.z_enable
                               ? ToCompareFunction(depth_control.zfunc)
                               : wgpu::CompareFunction::Always;

  if (depth_control.stencil_enable) {
    TranslateStencilFace(depth_control.stencilfunc, depth_control.stencilfail,
                         depth_control.stencilzfail, depth_control.stencilzpass,
                         out_state.stencilFront);
    if (depth_control.backface_enable) {
      TranslateStencilFace(depth_control.stencilfunc_bf, depth_control.stencilfail_bf,
                           depth_control.stencilzfail_bf, depth_control.stencilzpass_bf,
                           out_state.stencilBack);
    } else {
      out_state.stencilBack = out_state.stencilFront;
    }
    out_state.stencilReadMask = stencil_mask.stencilmask;
    out_state.stencilWriteMask = stencil_mask.stencilwritemask;
  } else {
    out_state.stencilReadMask = 0;
    out_state.stencilWriteMask = 0;
  }

  out_state.depthBias = depth_bias;
  out_state.depthBiasSlopeScale = depth_bias_slope_scale;
}

wgpu::ColorWriteMask TranslateColorWriteMask(uint32_t color_mask, uint32_t rt_index) {
  uint32_t mask = (color_mask >> (4 * rt_index)) & 0xF;
  wgpu::ColorWriteMask result = wgpu::ColorWriteMask::None;
  if (mask & 0x1) result |= wgpu::ColorWriteMask::Red;
  if (mask & 0x2) result |= wgpu::ColorWriteMask::Green;
  if (mask & 0x4) result |= wgpu::ColorWriteMask::Blue;
  if (mask & 0x8) result |= wgpu::ColorWriteMask::Alpha;
  return result;
}

RasterizerState TranslateRasterizerState(reg::PA_SU_SC_MODE_CNTL sc_mode, bool primitive_polygonal) {
  RasterizerState state;
  state.front_face = (sc_mode.face == 0) ? wgpu::FrontFace::CCW : wgpu::FrontFace::CW;
  if (primitive_polygonal) {
    if (sc_mode.cull_front && sc_mode.cull_back) {
      state.all_culled = true;
      state.cull_mode = wgpu::CullMode::None;
    } else if (sc_mode.cull_front) {
      state.cull_mode = wgpu::CullMode::Front;
    } else if (sc_mode.cull_back) {
      state.cull_mode = wgpu::CullMode::Back;
    } else {
      state.cull_mode = wgpu::CullMode::None;
    }
  } else {
    state.cull_mode = wgpu::CullMode::None;
  }
  return state;
}

DepthBiasState CalculateDepthBias(const RegisterFile& regs, bool primitive_polygonal,
                                  uint32_t scale_x, uint32_t scale_y) {
  DepthBiasState state;
  float slope = 0.0f;
  float constant = 0.0f;
  draw_util::GetPreferredFacePolygonOffset(regs, primitive_polygonal, slope, constant);
  auto depth_format = regs.Get<reg::RB_DEPTH_INFO>().depth_format;
  constant *= (depth_format == xenos::DepthRenderTargetFormat::kD24S8)
                  ? draw_util::kD3D10PolygonOffsetFactorUnorm24
                  : draw_util::kD3D10PolygonOffsetFactorFloat24;
  slope *= xenos::kPolygonOffsetScaleSubpixelUnit * float(std::max(scale_x, scale_y));
  state.depth_bias = int32_t(std::lround(constant));
  state.depth_bias_slope_scale = slope;
  return state;
}

}  // namespace rex::graphics::webgpu
