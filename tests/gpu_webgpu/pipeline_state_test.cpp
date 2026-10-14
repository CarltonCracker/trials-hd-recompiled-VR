// rexgpu-webgpu - Unit tests for fixed-function pipeline state translation (S-25).
// Tests every value and rule in docs/webgpu-renderer/README.md section 5.4.

#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "gpu_test.h"
#include "pipeline_state.h"

#define TEST_CHECK(cond)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      std::exit(1);                                                            \
    }                                                                          \
  } while (false)

using namespace rex::graphics;
using namespace rex::graphics::webgpu;
using namespace rex::graphics::xenos;

void TestBlendFactors() {
  std::printf("Testing Blend Factors and Operations...\n");

  // Basic factors
  TEST_CHECK(ToBlendFactor(BlendFactor::kZero) == wgpu::BlendFactor::Zero);
  TEST_CHECK(ToBlendFactor(BlendFactor::kOne) == wgpu::BlendFactor::One);
  TEST_CHECK(ToBlendFactor(BlendFactor::kSrcColor) == wgpu::BlendFactor::Src);
  TEST_CHECK(ToBlendFactor(BlendFactor::kOneMinusSrcColor) == wgpu::BlendFactor::OneMinusSrc);
  TEST_CHECK(ToBlendFactor(BlendFactor::kSrcAlpha) == wgpu::BlendFactor::SrcAlpha);
  TEST_CHECK(ToBlendFactor(BlendFactor::kOneMinusSrcAlpha) == wgpu::BlendFactor::OneMinusSrcAlpha);
  TEST_CHECK(ToBlendFactor(BlendFactor::kDstColor) == wgpu::BlendFactor::Dst);
  TEST_CHECK(ToBlendFactor(BlendFactor::kOneMinusDstColor) == wgpu::BlendFactor::OneMinusDst);
  TEST_CHECK(ToBlendFactor(BlendFactor::kDstAlpha) == wgpu::BlendFactor::DstAlpha);
  TEST_CHECK(ToBlendFactor(BlendFactor::kOneMinusDstAlpha) == wgpu::BlendFactor::OneMinusDstAlpha);
  TEST_CHECK(ToBlendFactor(BlendFactor::kConstantColor) == wgpu::BlendFactor::Constant);
  TEST_CHECK(ToBlendFactor(BlendFactor::kOneMinusConstantColor) == wgpu::BlendFactor::OneMinusConstant);
  TEST_CHECK(ToBlendFactor(BlendFactor::kConstantAlpha) == wgpu::BlendFactor::Constant);
  TEST_CHECK(ToBlendFactor(BlendFactor::kOneMinusConstantAlpha) == wgpu::BlendFactor::OneMinusConstant);
  TEST_CHECK(ToBlendFactor(BlendFactor::kSrcAlphaSaturate) == wgpu::BlendFactor::SrcAlphaSaturated);

  // Alpha factors: promotion of color factors to alpha
  TEST_CHECK(ToAlphaBlendFactor(BlendFactor::kSrcColor) == wgpu::BlendFactor::SrcAlpha);
  TEST_CHECK(ToAlphaBlendFactor(BlendFactor::kOneMinusSrcColor) == wgpu::BlendFactor::OneMinusSrcAlpha);
  TEST_CHECK(ToAlphaBlendFactor(BlendFactor::kDstColor) == wgpu::BlendFactor::DstAlpha);
  TEST_CHECK(ToAlphaBlendFactor(BlendFactor::kOneMinusDstColor) == wgpu::BlendFactor::OneMinusDstAlpha);
  TEST_CHECK(ToAlphaBlendFactor(BlendFactor::kSrcAlphaSaturate) == wgpu::BlendFactor::One);
  TEST_CHECK(ToAlphaBlendFactor(BlendFactor::kSrcAlpha) == wgpu::BlendFactor::SrcAlpha);
  TEST_CHECK(ToAlphaBlendFactor(BlendFactor::kOneMinusSrcAlpha) == wgpu::BlendFactor::OneMinusSrcAlpha);
  TEST_CHECK(ToAlphaBlendFactor(BlendFactor::kConstantColor) == wgpu::BlendFactor::Constant);

  // Operations
  TEST_CHECK(ToBlendOperation(BlendOp::kAdd) == wgpu::BlendOperation::Add);
  TEST_CHECK(ToBlendOperation(BlendOp::kSubtract) == wgpu::BlendOperation::Subtract);
  TEST_CHECK(ToBlendOperation(BlendOp::kMin) == wgpu::BlendOperation::Min);
  TEST_CHECK(ToBlendOperation(BlendOp::kMax) == wgpu::BlendOperation::Max);
  TEST_CHECK(ToBlendOperation(BlendOp::kRevSubtract) == wgpu::BlendOperation::ReverseSubtract);

  std::printf("PASS: Blend Factors and Operations\n");
}

void TestSection54BlendModes() {
  std::printf("Testing Section 5.4 Blend Modes...\n");

  wgpu::BlendState blend;

  // 1. Opaque 1,0 (default / disabled)
  {
    reg::RB_BLENDCONTROL c{};
    c.color_srcblend = BlendFactor::kOne;
    c.color_destblend = BlendFactor::kZero;
    c.color_comb_fcn = BlendOp::kAdd;
    c.alpha_srcblend = BlendFactor::kOne;
    c.alpha_destblend = BlendFactor::kZero;
    c.alpha_comb_fcn = BlendOp::kAdd;
    TEST_CHECK(!TranslateBlendControl(c, blend));
  }

  // 2. Additive 1,1 (~227/frame)
  {
    reg::RB_BLENDCONTROL c{};
    c.color_srcblend = BlendFactor::kOne;
    c.color_destblend = BlendFactor::kOne;
    c.color_comb_fcn = BlendOp::kAdd;
    c.alpha_srcblend = BlendFactor::kOne;
    c.alpha_destblend = BlendFactor::kOne;
    c.alpha_comb_fcn = BlendOp::kAdd;
    TEST_CHECK(TranslateBlendControl(c, blend));
    TEST_CHECK(blend.color.srcFactor == wgpu::BlendFactor::One);
    TEST_CHECK(blend.color.dstFactor == wgpu::BlendFactor::One);
    TEST_CHECK(blend.color.operation == wgpu::BlendOperation::Add);
    TEST_CHECK(blend.alpha.srcFactor == wgpu::BlendFactor::One);
    TEST_CHECK(blend.alpha.dstFactor == wgpu::BlendFactor::One);
    TEST_CHECK(blend.alpha.operation == wgpu::BlendOperation::Add);
  }

  // 3. Alpha blend Sa, 1-Sa (~126/frame)
  {
    reg::RB_BLENDCONTROL c{};
    c.color_srcblend = BlendFactor::kSrcAlpha;
    c.color_destblend = BlendFactor::kOneMinusSrcAlpha;
    c.color_comb_fcn = BlendOp::kAdd;
    c.alpha_srcblend = BlendFactor::kSrcAlpha;
    c.alpha_destblend = BlendFactor::kOneMinusSrcAlpha;
    c.alpha_comb_fcn = BlendOp::kAdd;
    TEST_CHECK(TranslateBlendControl(c, blend));
    TEST_CHECK(blend.color.srcFactor == wgpu::BlendFactor::SrcAlpha);
    TEST_CHECK(blend.color.dstFactor == wgpu::BlendFactor::OneMinusSrcAlpha);
    TEST_CHECK(blend.alpha.srcFactor == wgpu::BlendFactor::SrcAlpha);
    TEST_CHECK(blend.alpha.dstFactor == wgpu::BlendFactor::OneMinusSrcAlpha);
  }

  // 4. Multiply 0, Sc (~131/frame)
  {
    reg::RB_BLENDCONTROL c{};
    c.color_srcblend = BlendFactor::kZero;
    c.color_destblend = BlendFactor::kSrcColor;
    c.color_comb_fcn = BlendOp::kAdd;
    c.alpha_srcblend = BlendFactor::kZero;
    c.alpha_destblend = BlendFactor::kSrcColor;  // color factor in alpha channel
    c.alpha_comb_fcn = BlendOp::kAdd;
    TEST_CHECK(TranslateBlendControl(c, blend));
    TEST_CHECK(blend.color.srcFactor == wgpu::BlendFactor::Zero);
    TEST_CHECK(blend.color.dstFactor == wgpu::BlendFactor::Src);
    // Alpha must promote SrcColor -> SrcAlpha
    TEST_CHECK(blend.alpha.srcFactor == wgpu::BlendFactor::Zero);
    TEST_CHECK(blend.alpha.dstFactor == wgpu::BlendFactor::SrcAlpha);
  }

  // 5. 1-Da, 1 (~46/frame)
  {
    reg::RB_BLENDCONTROL c{};
    c.color_srcblend = BlendFactor::kOneMinusDstAlpha;
    c.color_destblend = BlendFactor::kOne;
    c.color_comb_fcn = BlendOp::kAdd;
    c.alpha_srcblend = BlendFactor::kOneMinusDstAlpha;
    c.alpha_destblend = BlendFactor::kOne;
    c.alpha_comb_fcn = BlendOp::kAdd;
    TEST_CHECK(TranslateBlendControl(c, blend));
    TEST_CHECK(blend.color.srcFactor == wgpu::BlendFactor::OneMinusDstAlpha);
    TEST_CHECK(blend.color.dstFactor == wgpu::BlendFactor::One);
    TEST_CHECK(blend.alpha.srcFactor == wgpu::BlendFactor::OneMinusDstAlpha);
    TEST_CHECK(blend.alpha.dstFactor == wgpu::BlendFactor::One);
  }

  // 6. Constant color and alpha factors
  {
    reg::RB_BLENDCONTROL c{};
    c.color_srcblend = BlendFactor::kConstantColor;
    c.color_destblend = BlendFactor::kOneMinusConstantColor;
    c.color_comb_fcn = BlendOp::kAdd;
    c.alpha_srcblend = BlendFactor::kConstantAlpha;
    c.alpha_destblend = BlendFactor::kOneMinusConstantAlpha;
    c.alpha_comb_fcn = BlendOp::kAdd;
    TEST_CHECK(TranslateBlendControl(c, blend));
    TEST_CHECK(blend.color.srcFactor == wgpu::BlendFactor::Constant);
    TEST_CHECK(blend.color.dstFactor == wgpu::BlendFactor::OneMinusConstant);
    TEST_CHECK(blend.alpha.srcFactor == wgpu::BlendFactor::Constant);
    TEST_CHECK(blend.alpha.dstFactor == wgpu::BlendFactor::OneMinusConstant);
  }

  // 7. WebGPU Min / Max rule: factors must be forced to One
  {
    reg::RB_BLENDCONTROL c{};
    c.color_srcblend = BlendFactor::kSrcAlpha;
    c.color_destblend = BlendFactor::kDstAlpha;
    c.color_comb_fcn = BlendOp::kMin;
    c.alpha_srcblend = BlendFactor::kZero;
    c.alpha_destblend = BlendFactor::kOneMinusSrcAlpha;
    c.alpha_comb_fcn = BlendOp::kMax;
    TEST_CHECK(TranslateBlendControl(c, blend));
    TEST_CHECK(blend.color.operation == wgpu::BlendOperation::Min);
    TEST_CHECK(blend.color.srcFactor == wgpu::BlendFactor::One);
    TEST_CHECK(blend.color.dstFactor == wgpu::BlendFactor::One);
    TEST_CHECK(blend.alpha.operation == wgpu::BlendOperation::Max);
    TEST_CHECK(blend.alpha.srcFactor == wgpu::BlendFactor::One);
    TEST_CHECK(blend.alpha.dstFactor == wgpu::BlendFactor::One);
  }

  std::printf("PASS: Section 5.4 Blend Modes\n");
}

void TestCompareAndStencil() {
  std::printf("Testing Compare and Stencil Translations...\n");

  // All 8 compare functions
  TEST_CHECK(ToCompareFunction(CompareFunction::kNever) == wgpu::CompareFunction::Never);
  TEST_CHECK(ToCompareFunction(CompareFunction::kLess) == wgpu::CompareFunction::Less);
  TEST_CHECK(ToCompareFunction(CompareFunction::kEqual) == wgpu::CompareFunction::Equal);
  TEST_CHECK(ToCompareFunction(CompareFunction::kLessEqual) == wgpu::CompareFunction::LessEqual);
  TEST_CHECK(ToCompareFunction(CompareFunction::kGreater) == wgpu::CompareFunction::Greater);
  TEST_CHECK(ToCompareFunction(CompareFunction::kNotEqual) == wgpu::CompareFunction::NotEqual);
  TEST_CHECK(ToCompareFunction(CompareFunction::kGreaterEqual) == wgpu::CompareFunction::GreaterEqual);
  TEST_CHECK(ToCompareFunction(CompareFunction::kAlways) == wgpu::CompareFunction::Always);

  // Stencil operations
  TEST_CHECK(ToStencilOperation(StencilOp::kKeep) == wgpu::StencilOperation::Keep);
  TEST_CHECK(ToStencilOperation(StencilOp::kZero) == wgpu::StencilOperation::Zero);
  TEST_CHECK(ToStencilOperation(StencilOp::kReplace) == wgpu::StencilOperation::Replace);
  TEST_CHECK(ToStencilOperation(StencilOp::kIncrementClamp) == wgpu::StencilOperation::IncrementClamp);
  TEST_CHECK(ToStencilOperation(StencilOp::kDecrementClamp) == wgpu::StencilOperation::DecrementClamp);
  TEST_CHECK(ToStencilOperation(StencilOp::kInvert) == wgpu::StencilOperation::Invert);
  TEST_CHECK(ToStencilOperation(StencilOp::kIncrementWrap) == wgpu::StencilOperation::IncrementWrap);
  TEST_CHECK(ToStencilOperation(StencilOp::kDecrementWrap) == wgpu::StencilOperation::DecrementWrap);

  // Depth control translation
  wgpu::DepthStencilState ds{};
  reg::RB_DEPTHCONTROL dc{};
  reg::RB_STENCILREFMASK sm{};

  // Disabled depth & stencil
  dc.z_enable = 0;
  dc.z_write_enable = 0;
  dc.stencil_enable = 0;
  TranslateDepthStencilState(dc, sm, wgpu::TextureFormat::Depth24PlusStencil8, 0, 0.0f, ds);
  TEST_CHECK(ds.depthWriteEnabled == wgpu::OptionalBool::False);
  TEST_CHECK(ds.depthCompare == wgpu::CompareFunction::Always);
  TEST_CHECK(ds.stencilReadMask == 0);
  TEST_CHECK(ds.stencilWriteMask == 0);

  // Reverse-Z active with write: zfunc GreaterEqual
  dc.z_enable = 1;
  dc.z_write_enable = 1;
  dc.zfunc = CompareFunction::kGreaterEqual;
  TranslateDepthStencilState(dc, sm, wgpu::TextureFormat::Depth32FloatStencil8, 10, 2.5f, ds);
  TEST_CHECK(ds.depthWriteEnabled == wgpu::OptionalBool::True);
  TEST_CHECK(ds.depthCompare == wgpu::CompareFunction::GreaterEqual);
  TEST_CHECK(ds.depthBias == 10);
  TEST_CHECK(ds.depthBiasSlopeScale == 2.5f);

  // Stencil active: single-sided
  dc.stencil_enable = 1;
  dc.backface_enable = 0;
  dc.stencilfunc = CompareFunction::kEqual;
  dc.stencilfail = StencilOp::kKeep;
  dc.stencilzfail = StencilOp::kIncrementClamp;
  dc.stencilzpass = StencilOp::kReplace;
  sm.stencilmask = 0xA5;
  sm.stencilwritemask = 0x5A;
  TranslateDepthStencilState(dc, sm, wgpu::TextureFormat::Depth24PlusStencil8, 0, 0.0f, ds);
  TEST_CHECK(ds.stencilFront.compare == wgpu::CompareFunction::Equal);
  TEST_CHECK(ds.stencilFront.failOp == wgpu::StencilOperation::Keep);
  TEST_CHECK(ds.stencilFront.depthFailOp == wgpu::StencilOperation::IncrementClamp);
  TEST_CHECK(ds.stencilFront.passOp == wgpu::StencilOperation::Replace);
  // Back face mirrors front face when backface_enable == 0
  TEST_CHECK(ds.stencilBack.compare == wgpu::CompareFunction::Equal);
  TEST_CHECK(ds.stencilBack.passOp == wgpu::StencilOperation::Replace);
  TEST_CHECK(ds.stencilReadMask == 0xA5);
  TEST_CHECK(ds.stencilWriteMask == 0x5A);

  // Stencil active: two-sided
  dc.backface_enable = 1;
  dc.stencilfunc_bf = CompareFunction::kGreater;
  dc.stencilfail_bf = StencilOp::kZero;
  dc.stencilzfail_bf = StencilOp::kInvert;
  dc.stencilzpass_bf = StencilOp::kDecrementWrap;
  TranslateDepthStencilState(dc, sm, wgpu::TextureFormat::Depth24PlusStencil8, 0, 0.0f, ds);
  TEST_CHECK(ds.stencilBack.compare == wgpu::CompareFunction::Greater);
  TEST_CHECK(ds.stencilBack.failOp == wgpu::StencilOperation::Zero);
  TEST_CHECK(ds.stencilBack.depthFailOp == wgpu::StencilOperation::Invert);
  TEST_CHECK(ds.stencilBack.passOp == wgpu::StencilOperation::DecrementWrap);

  std::printf("PASS: Compare and Stencil Translations\n");
}

void TestColorWriteMask() {
  std::printf("Testing Color Write Mask...\n");

  // Single RT masks
  TEST_CHECK(TranslateColorWriteMask(0x0, 0) == wgpu::ColorWriteMask::None);
  TEST_CHECK(TranslateColorWriteMask(0x1, 0) == wgpu::ColorWriteMask::Red);
  TEST_CHECK(TranslateColorWriteMask(0x2, 0) == wgpu::ColorWriteMask::Green);
  TEST_CHECK(TranslateColorWriteMask(0x4, 0) == wgpu::ColorWriteMask::Blue);
  TEST_CHECK(TranslateColorWriteMask(0x8, 0) == wgpu::ColorWriteMask::Alpha);
  TEST_CHECK(TranslateColorWriteMask(0xF, 0) == wgpu::ColorWriteMask::All);
  TEST_CHECK(TranslateColorWriteMask(0x3, 0) == (wgpu::ColorWriteMask::Red | wgpu::ColorWriteMask::Green));
  TEST_CHECK(TranslateColorWriteMask(0x7, 0) == (wgpu::ColorWriteMask::Red | wgpu::ColorWriteMask::Green | wgpu::ColorWriteMask::Blue));

  // Multi-RT shifting: RT0=0xF, RT1=0x3, RT2=0x8, RT3=0x0 -> mask = 0x083F
  uint32_t mask = 0x083F;
  TEST_CHECK(TranslateColorWriteMask(mask, 0) == wgpu::ColorWriteMask::All);
  TEST_CHECK(TranslateColorWriteMask(mask, 1) == (wgpu::ColorWriteMask::Red | wgpu::ColorWriteMask::Green));
  TEST_CHECK(TranslateColorWriteMask(mask, 2) == wgpu::ColorWriteMask::Alpha);
  TEST_CHECK(TranslateColorWriteMask(mask, 3) == wgpu::ColorWriteMask::None);

  std::printf("PASS: Color Write Mask\n");
}

void TestRasterizerState() {
  std::printf("Testing Rasterizer State...\n");

  reg::PA_SU_SC_MODE_CNTL sc{};

  // Front face winding
  sc.face = 0;  // CCW
  TEST_CHECK(TranslateRasterizerState(sc, true).front_face == wgpu::FrontFace::CCW);
  sc.face = 1;  // CW
  TEST_CHECK(TranslateRasterizerState(sc, true).front_face == wgpu::FrontFace::CW);

  // Non-polygonal (points / lines): always no culling
  sc.cull_front = 1;
  sc.cull_back = 1;
  RasterizerState non_poly = TranslateRasterizerState(sc, false);
  TEST_CHECK(non_poly.cull_mode == wgpu::CullMode::None);
  TEST_CHECK(!non_poly.all_culled);

  // Polygonal culling
  sc.cull_front = 0;
  sc.cull_back = 0;
  TEST_CHECK(TranslateRasterizerState(sc, true).cull_mode == wgpu::CullMode::None);

  sc.cull_front = 0;
  sc.cull_back = 1;  // back-face culling (Section 5.4 most common)
  TEST_CHECK(TranslateRasterizerState(sc, true).cull_mode == wgpu::CullMode::Back);

  sc.cull_front = 1;
  sc.cull_back = 0;
  TEST_CHECK(TranslateRasterizerState(sc, true).cull_mode == wgpu::CullMode::Front);

  sc.cull_front = 1;
  sc.cull_back = 1;  // all culled
  RasterizerState all_cull = TranslateRasterizerState(sc, true);
  TEST_CHECK(all_cull.all_culled);

  std::printf("PASS: Rasterizer State\n");
}

void TestDepthBias() {
  std::printf("Testing Depth Bias Calculation...\n");

  RegisterFile regs{};
  // Enable polygon offset on front
  auto sc = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  sc.poly_offset_front_enable = 1;
  sc.poly_offset_back_enable = 1;
  regs.values[XE_GPU_REG_PA_SU_SC_MODE_CNTL] = sc.value;

  // Front offset: scale = 2.0, offset = 1.0
  regs.values[XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE] = 0x40000000;   // float 2.0
  regs.values[XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET] = 0x3F800000;  // float 1.0

  // D24FS8 depth format
  regs.values[XE_GPU_REG_RB_DEPTH_INFO] = (1u << 16);  // D24FS8

  DepthBiasState b1 = CalculateDepthBias(regs, true, 1, 1);
  TEST_CHECK(b1.depth_bias > 0);
  TEST_CHECK(b1.depth_bias_slope_scale > 0.0f);

  // 2x scale: slope scale doubles
  DepthBiasState b2 = CalculateDepthBias(regs, true, 2, 2);
  TEST_CHECK(std::abs(b2.depth_bias_slope_scale - 2.0f * b1.depth_bias_slope_scale) < 1e-4f);

  // Non-polygonal: depth bias disabled (0)
  DepthBiasState b_non_poly = CalculateDepthBias(regs, false, 1, 1);
  TEST_CHECK(b_non_poly.depth_bias == 0);
  TEST_CHECK(b_non_poly.depth_bias_slope_scale == 0.0f);

  std::printf("PASS: Depth Bias Calculation\n");
}

void TestLiveDawnPipelines(unsigned adapter_index) {
  std::printf("Testing Live Dawn Pipeline Creation on Adapter %u...\n", adapter_index);

  GpuTest test(adapter_index);

  std::string vs_source = R"(
@vertex fn main() -> @builtin(position) vec4f {
  return vec4f(0.0, 0.0, 0.5, 1.0);
}
)";
  std::string fs_source = R"(
@fragment fn main() -> @location(0) vec4f {
  return vec4f(1.0, 0.5, 0.25, 1.0);
}
)";
  auto vs_module = test.Module(vs_source);
  auto fs_module = test.Module(fs_source);

  // Test every Section 5.4 blend mode as a real WebGPU RenderPipeline
  struct BlendCase {
    const char* name;
    xenos::BlendFactor src_c, dst_c;
    xenos::BlendOp op_c;
    xenos::BlendFactor src_a, dst_a;
    xenos::BlendOp op_a;
  };
  static constexpr BlendCase kCases[] = {
      {"Additive 1,1", BlendFactor::kOne, BlendFactor::kOne, BlendOp::kAdd,
       BlendFactor::kOne, BlendFactor::kOne, BlendOp::kAdd},
      {"Alpha Sa,1-Sa", BlendFactor::kSrcAlpha, BlendFactor::kOneMinusSrcAlpha, BlendOp::kAdd,
       BlendFactor::kSrcAlpha, BlendFactor::kOneMinusSrcAlpha, BlendOp::kAdd},
      {"Multiply 0,Sc", BlendFactor::kZero, BlendFactor::kSrcColor, BlendOp::kAdd,
       BlendFactor::kZero, BlendFactor::kSrcAlpha, BlendOp::kAdd},
      {"DstAlpha 1-Da,1", BlendFactor::kOneMinusDstAlpha, BlendFactor::kOne, BlendOp::kAdd,
       BlendFactor::kOneMinusDstAlpha, BlendFactor::kOne, BlendOp::kAdd},
      {"Constant", BlendFactor::kConstantColor, BlendFactor::kOneMinusConstantColor, BlendOp::kAdd,
       BlendFactor::kConstantAlpha, BlendFactor::kOneMinusConstantAlpha, BlendOp::kAdd},
      {"Min (factors forced to One)", BlendFactor::kSrcAlpha, BlendFactor::kDstAlpha, BlendOp::kMin,
       BlendFactor::kOne, BlendFactor::kOne, BlendOp::kMin},
      {"Max (factors forced to One)", BlendFactor::kSrcAlpha, BlendFactor::kDstAlpha, BlendOp::kMax,
       BlendFactor::kOne, BlendFactor::kOne, BlendOp::kMax},
  };

  for (const auto& c : kCases) {
    reg::RB_BLENDCONTROL bc{};
    bc.color_srcblend = c.src_c;
    bc.color_destblend = c.dst_c;
    bc.color_comb_fcn = c.op_c;
    bc.alpha_srcblend = c.src_a;
    bc.alpha_destblend = c.dst_a;
    bc.alpha_comb_fcn = c.op_a;

    wgpu::BlendState blend;
    TEST_CHECK(TranslateBlendControl(bc, blend));

    wgpu::ColorTargetState target;
    target.format = wgpu::TextureFormat::RGBA8Unorm;
    target.blend = &blend;
    target.writeMask = wgpu::ColorWriteMask::All;

    wgpu::FragmentState fragment;
    fragment.module = fs_module;
    fragment.entryPoint = "main";
    fragment.targetCount = 1;
    fragment.targets = &target;

    wgpu::DepthStencilState ds;
    reg::RB_DEPTHCONTROL dc{};
    dc.z_enable = 1;
    dc.z_write_enable = 1;
    dc.zfunc = CompareFunction::kGreaterEqual;
    reg::RB_STENCILREFMASK sm{};
    TranslateDepthStencilState(dc, sm, wgpu::TextureFormat::Depth24PlusStencil8, 0, 0.0f, ds);

    wgpu::RenderPipelineDescriptor desc;
    desc.label = c.name;
    desc.vertex.module = vs_module;
    desc.vertex.entryPoint = "main";
    desc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
    desc.primitive.cullMode = wgpu::CullMode::Back;
    desc.primitive.frontFace = wgpu::FrontFace::CCW;
    desc.depthStencil = &ds;
    desc.fragment = &fragment;

    auto pipeline = test.device.CreateRenderPipeline(&desc);
    TEST_CHECK(pipeline);
    TEST_CHECK(test.errors == 0);
  }

  std::printf("PASS: Live Dawn Pipelines on Adapter %u (0 validation errors)\n", adapter_index);
}

int main(int argc, char** argv) {
  TestBlendFactors();
  TestSection54BlendModes();
  TestCompareAndStencil();
  TestColorWriteMask();
  TestRasterizerState();
  TestDepthBias();

  unsigned adapter = 0;
  if (argc > 1) {
    adapter = unsigned(std::atoi(argv[1]));
  }
  TestLiveDawnPipelines(adapter);

  std::printf("All pipeline_state tests PASSED!\n");
  return 0;
}
