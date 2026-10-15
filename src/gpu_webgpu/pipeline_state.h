// rexgpu-webgpu - fixed-function pipeline state translation (section 5.4).
// Translates Xenos RB_BLENDCONTROL, RB_DEPTHCONTROL, RB_STENCILREFMASK,
// RB_COLOR_MASK, PA_SU_SC_MODE_CNTL and polygon offset to WebGPU structs.

#pragma once

#include <cstdint>

#include <webgpu/webgpu_cpp.h>

#include <rex/graphics/register_file.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/xenos.h>

namespace rex::graphics::webgpu {

// Blend state translations
wgpu::BlendFactor ToBlendFactor(xenos::BlendFactor factor);
wgpu::BlendFactor ToAlphaBlendFactor(xenos::BlendFactor factor);
wgpu::BlendOperation ToBlendOperation(xenos::BlendOp op);

// Translates RB_BLENDCONTROL into wgpu::BlendState.
// Returns false if blending is disabled (src=1, dst=0, op=add for both color and alpha).
bool TranslateBlendControl(reg::RB_BLENDCONTROL blend_control, wgpu::BlendState& out_blend);

// Compare and Stencil translations
wgpu::CompareFunction ToCompareFunction(xenos::CompareFunction function);
wgpu::StencilOperation ToStencilOperation(xenos::StencilOp op);

void TranslateStencilFace(xenos::CompareFunction func, xenos::StencilOp fail,
                          xenos::StencilOp zfail, xenos::StencilOp zpass,
                          wgpu::StencilFaceState& out_face);

void TranslateDepthStencilState(reg::RB_DEPTHCONTROL depth_control,
                                reg::RB_STENCILREFMASK stencil_mask,
                                wgpu::TextureFormat format,
                                int32_t depth_bias,
                                float depth_bias_slope_scale,
                                wgpu::DepthStencilState& out_state);

// Color write mask translation (RT 0..3 from RB_COLOR_MASK)
wgpu::ColorWriteMask TranslateColorWriteMask(uint32_t color_mask, uint32_t rt_index);

// Rasterizer state (cull mode and front face from PA_SU_SC_MODE_CNTL)
struct RasterizerState {
  wgpu::CullMode cull_mode = wgpu::CullMode::None;
  wgpu::FrontFace front_face = wgpu::FrontFace::CCW;
  bool all_culled = false;
};

RasterizerState TranslateRasterizerState(reg::PA_SU_SC_MODE_CNTL sc_mode, bool primitive_polygonal);

// Polygon offset (depth bias) calculation from registers and RT scale
struct DepthBiasState {
  int32_t depth_bias = 0;
  float depth_bias_slope_scale = 0.0f;
};

DepthBiasState CalculateDepthBias(const RegisterFile& regs, bool primitive_polygonal,
                                  uint32_t scale_x, uint32_t scale_y);

}  // namespace rex::graphics::webgpu
