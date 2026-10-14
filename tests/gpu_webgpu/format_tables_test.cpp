// rexgpu-webgpu - Unit tests for format tables and feature fallbacks (S-23).

#include <cassert>
#include <cstdio>
#include <cstdlib>

#include "format_tables.h"

#define TEST_CHECK(cond)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      std::exit(1);                                                            \
    }                                                                          \
  } while (false)

using namespace rex::graphics::webgpu::format_tables;
using namespace rex::graphics::xenos;

void TestColorRenderTargetFormats() {
  std::printf("Testing Color Render Target Formats...\n");

  DeviceFeatures f_tier1;
  f_tier1.texture_formats_tier1 = true;

  DeviceFeatures f_no_tier1;
  f_no_tier1.texture_formats_tier1 = false;

  // 1. k_8_8_8_8 (Trials HD main scene MRT, luminance chain)
  {
    auto info = GetColorRenderTargetFormatInfo(ColorRenderTargetFormat::k_8_8_8_8, f_tier1);
    TEST_CHECK(info.is_supported);
    TEST_CHECK(info.host_format == wgpu::TextureFormat::RGBA8Unorm);
    TEST_CHECK(info.output_scale == 1.0f);
    TEST_CHECK(IsTrialsColorRenderTargetFormat(ColorRenderTargetFormat::k_8_8_8_8));
  }

  // 2. k_8_8_8_8_GAMMA
  {
    auto info = GetColorRenderTargetFormatInfo(ColorRenderTargetFormat::k_8_8_8_8_GAMMA, f_tier1);
    TEST_CHECK(info.is_supported);
    TEST_CHECK(info.host_format == wgpu::TextureFormat::RGBA8Unorm);
    TEST_CHECK(info.output_scale == 1.0f);
  }

  // 3. k_2_10_10_10 (Trials HD bloom chain)
  {
    auto info = GetColorRenderTargetFormatInfo(ColorRenderTargetFormat::k_2_10_10_10, f_tier1);
    TEST_CHECK(info.is_supported);
    TEST_CHECK(info.host_format == wgpu::TextureFormat::RGB10A2Unorm);
    TEST_CHECK(info.output_scale == 1.0f);
    TEST_CHECK(IsTrialsColorRenderTargetFormat(ColorRenderTargetFormat::k_2_10_10_10));
  }

  // 4. k_2_10_10_10_FLOAT (Trials HD HDR composite / additive effects, 7e3)
  {
    auto info = GetColorRenderTargetFormatInfo(ColorRenderTargetFormat::k_2_10_10_10_FLOAT, f_tier1);
    TEST_CHECK(info.is_supported);
    TEST_CHECK(info.host_format == wgpu::TextureFormat::RGBA16Float);
    TEST_CHECK(info.output_scale == 1.0f);
    TEST_CHECK(IsTrialsColorRenderTargetFormat(ColorRenderTargetFormat::k_2_10_10_10_FLOAT));
  }

  // 5. k_2_10_10_10_FLOAT_AS_16_16_16_16 (Trials HD 4x MSAA pass)
  {
    auto info = GetColorRenderTargetFormatInfo(
        ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16, f_tier1);
    TEST_CHECK(info.is_supported);
    TEST_CHECK(info.host_format == wgpu::TextureFormat::RGBA16Float);
    TEST_CHECK(info.output_scale == 1.0f);
    TEST_CHECK(IsTrialsColorRenderTargetFormat(
        ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16));
  }

  // 6. k_16_16 (Trials HD shadow / light maps)
  // With tier1: RG16Snorm with 1/32 range scale (-32..32)
  {
    auto info = GetColorRenderTargetFormatInfo(ColorRenderTargetFormat::k_16_16, f_tier1);
    TEST_CHECK(info.is_supported);
    TEST_CHECK(info.host_format == wgpu::TextureFormat::RG16Snorm);
    TEST_CHECK(info.output_scale == 1.0f / 32.0f);
    TEST_CHECK(IsTrialsColorRenderTargetFormat(ColorRenderTargetFormat::k_16_16));
  }
  // Without tier1 fallback: RG16Float with 1.0 scale
  {
    auto info = GetColorRenderTargetFormatInfo(ColorRenderTargetFormat::k_16_16, f_no_tier1);
    TEST_CHECK(info.is_supported);
    TEST_CHECK(info.host_format == wgpu::TextureFormat::RG16Float);
    TEST_CHECK(info.output_scale == 1.0f);
  }

  // 7. k_16_16_16_16
  // With tier1: RGBA16Snorm with 1/32 range scale
  {
    auto info = GetColorRenderTargetFormatInfo(ColorRenderTargetFormat::k_16_16_16_16, f_tier1);
    TEST_CHECK(info.is_supported);
    TEST_CHECK(info.host_format == wgpu::TextureFormat::RGBA16Snorm);
    TEST_CHECK(info.output_scale == 1.0f / 32.0f);
  }
  // Without tier1 fallback: RGBA16Float with 1.0 scale
  {
    auto info = GetColorRenderTargetFormatInfo(ColorRenderTargetFormat::k_16_16_16_16, f_no_tier1);
    TEST_CHECK(info.is_supported);
    TEST_CHECK(info.host_format == wgpu::TextureFormat::RGBA16Float);
    TEST_CHECK(info.output_scale == 1.0f);
  }

  // 8. Other float formats
  {
    auto info = GetColorRenderTargetFormatInfo(ColorRenderTargetFormat::k_16_16_FLOAT, f_tier1);
    TEST_CHECK(info.is_supported);
    TEST_CHECK(info.host_format == wgpu::TextureFormat::RG16Float);

    info = GetColorRenderTargetFormatInfo(ColorRenderTargetFormat::k_32_FLOAT, f_tier1);
    TEST_CHECK(info.is_supported);
    TEST_CHECK(info.host_format == wgpu::TextureFormat::R32Float);

    info = GetColorRenderTargetFormatInfo(ColorRenderTargetFormat::k_32_32_FLOAT, f_tier1);
    TEST_CHECK(info.is_supported);
    TEST_CHECK(info.host_format == wgpu::TextureFormat::RG32Float);
  }

  std::printf("PASS: Color Render Target Formats\n");
}

void TestDepthRenderTargetFormats() {
  std::printf("Testing Depth Render Target Formats...\n");

  DeviceFeatures f_depth32;
  f_depth32.depth32float_stencil8 = true;

  DeviceFeatures f_no_depth32;
  f_no_depth32.depth32float_stencil8 = false;

  // 1. kD24FS8 (all depth in Trials HD)
  // With depth32float-stencil8: Depth32FloatStencil8
  {
    auto info = GetDepthRenderTargetFormatInfo(DepthRenderTargetFormat::kD24FS8, f_depth32);
    TEST_CHECK(info.is_supported);
    TEST_CHECK(info.host_format == wgpu::TextureFormat::Depth32FloatStencil8);
    TEST_CHECK(info.has_stencil);
    TEST_CHECK(HostDepthFormatHasStencil(info.host_format));
    TEST_CHECK(IsTrialsDepthRenderTargetFormat(DepthRenderTargetFormat::kD24FS8));
  }

  // Without depth32float-stencil8 fallback: Depth24PlusStencil8
  {
    auto info = GetDepthRenderTargetFormatInfo(DepthRenderTargetFormat::kD24FS8, f_no_depth32);
    TEST_CHECK(info.is_supported);
    TEST_CHECK(info.host_format == wgpu::TextureFormat::Depth24PlusStencil8);
    TEST_CHECK(info.has_stencil);
    TEST_CHECK(HostDepthFormatHasStencil(info.host_format));
  }

  // 2. kD24S8
  {
    auto info = GetDepthRenderTargetFormatInfo(DepthRenderTargetFormat::kD24S8, f_depth32);
    TEST_CHECK(info.is_supported);
    TEST_CHECK(info.host_format == wgpu::TextureFormat::Depth24PlusStencil8);
    TEST_CHECK(info.has_stencil);
    TEST_CHECK(!IsTrialsDepthRenderTargetFormat(DepthRenderTargetFormat::kD24S8));
  }

  std::printf("PASS: Depth Render Target Formats\n");
}

void TestTextureFormats() {
  std::printf("Testing Texture Formats...\n");

  DeviceFeatures f_all;
  f_all.texture_compression_bc = true;
  f_all.texture_formats_tier1 = true;
  f_all.depth32float_stencil8 = true;

  DeviceFeatures f_no_bc;
  f_no_bc.texture_compression_bc = false;
  f_no_bc.texture_formats_tier1 = true;

  DeviceFeatures f_no_tier1;
  f_no_tier1.texture_compression_bc = true;
  f_no_tier1.texture_formats_tier1 = false;

  // 1. DXT4_5 / BC3 (Trials HD: most common texture format)
  {
    auto info = GetTextureFormatInfo(TextureFormat::k_DXT4_5, f_all);
    TEST_CHECK(info.is_supported);
    TEST_CHECK(info.host_format == wgpu::TextureFormat::BC3RGBAUnorm);
    TEST_CHECK(info.load_shader == TextureLoadShader::k128bpb);
    TEST_CHECK(info.conversion == TextureConversion::kCopy);
    TEST_CHECK(info.host_bytes_per_block == 16);
    TEST_CHECK(info.components == 4);
    TEST_CHECK(info.is_compressed);
    TEST_CHECK(IsTrialsTextureFormat(TextureFormat::k_DXT4_5));

    // Fallback when BC is unsupported
    auto info_no_bc = GetTextureFormatInfo(TextureFormat::k_DXT4_5, f_no_bc);
    TEST_CHECK(!info_no_bc.is_supported);
  }

  // 2. DXT1 / BC1 (Trials HD)
  {
    auto info = GetTextureFormatInfo(TextureFormat::k_DXT1, f_all);
    TEST_CHECK(info.is_supported);
    TEST_CHECK(info.host_format == wgpu::TextureFormat::BC1RGBAUnorm);
    TEST_CHECK(info.load_shader == TextureLoadShader::k64bpb);
    TEST_CHECK(info.conversion == TextureConversion::kCopy);
    TEST_CHECK(info.host_bytes_per_block == 8);
    TEST_CHECK(info.components == 4);
    TEST_CHECK(info.is_compressed);
    TEST_CHECK(IsTrialsTextureFormat(TextureFormat::k_DXT1));

    // Fallback when BC is unsupported
    auto info_no_bc = GetTextureFormatInfo(TextureFormat::k_DXT1, f_no_bc);
    TEST_CHECK(!info_no_bc.is_supported);
  }

  // 3. k_8_8_8_8 (Trials HD 2D and cube textures)
  {
    auto info = GetTextureFormatInfo(TextureFormat::k_8_8_8_8, f_all);
    TEST_CHECK(info.is_supported);
    TEST_CHECK(info.host_format == wgpu::TextureFormat::RGBA8Unorm);
    TEST_CHECK(info.load_shader == TextureLoadShader::k32bpb);
    TEST_CHECK(info.conversion == TextureConversion::kCopy);
    TEST_CHECK(info.host_bytes_per_block == 4);
    TEST_CHECK(info.components == 4);
    TEST_CHECK(!info.is_compressed);
    TEST_CHECK(IsTrialsTextureFormat(TextureFormat::k_8_8_8_8));
  }

  // 4. k_16_16 (Trials HD: resolved shadow/light depth, exp adjust -15, signed integer)
  {
    auto info = GetTextureFormatInfo(TextureFormat::k_16_16, f_all);
    TEST_CHECK(info.is_supported);
    TEST_CHECK(info.host_format == wgpu::TextureFormat::RG32Float);
    TEST_CHECK(info.load_shader == TextureLoadShader::k32bpb);
    TEST_CHECK(info.conversion == TextureConversion::kFixed16);
    TEST_CHECK(info.host_bytes_per_block == 8);
    TEST_CHECK(info.components == 2);
    TEST_CHECK(!info.is_compressed);
    TEST_CHECK(IsTrialsTextureFormat(TextureFormat::k_16_16));
  }

  // 5. k_24_8_FLOAT (Trials HD: resolved float24 depth sampled for fog / soft particles)
  {
    auto info = GetTextureFormatInfo(TextureFormat::k_24_8_FLOAT, f_all);
    TEST_CHECK(info.is_supported);
    TEST_CHECK(info.host_format == wgpu::TextureFormat::R32Float);
    TEST_CHECK(info.load_shader == TextureLoadShader::kDepthFloat);
    TEST_CHECK(info.conversion == TextureConversion::kDepth24);
    TEST_CHECK(info.host_bytes_per_block == 4);
    TEST_CHECK(info.components == 1);
    TEST_CHECK(!info.is_compressed);
    TEST_CHECK(IsTrialsTextureFormat(TextureFormat::k_24_8_FLOAT));
  }

  // 6. k_8 (Trials HD font or mask)
  {
    auto info = GetTextureFormatInfo(TextureFormat::k_8, f_all);
    TEST_CHECK(info.is_supported);
    TEST_CHECK(info.host_format == wgpu::TextureFormat::R8Unorm);
    TEST_CHECK(info.load_shader == TextureLoadShader::k8bpb);
    TEST_CHECK(info.conversion == TextureConversion::kCopy);
    TEST_CHECK(info.host_bytes_per_block == 1);
    TEST_CHECK(info.components == 1);
    TEST_CHECK(!info.is_compressed);
    TEST_CHECK(IsTrialsTextureFormat(TextureFormat::k_8));
  }

  // 7. k_2_10_10_10_AS_16_16_16_16 (Trials HD bloom chain)
  // With tier1: RGBA16Unorm
  {
    auto info = GetTextureFormatInfo(TextureFormat::k_2_10_10_10_AS_16_16_16_16, f_all);
    TEST_CHECK(info.is_supported);
    TEST_CHECK(info.host_format == wgpu::TextureFormat::RGBA16Unorm);
    TEST_CHECK(info.load_shader == TextureLoadShader::k64bpb);
    TEST_CHECK(info.host_bytes_per_block == 8);
    TEST_CHECK(info.components == 4);
    TEST_CHECK(IsTrialsTextureFormat(TextureFormat::k_2_10_10_10_AS_16_16_16_16));
  }
  // Without tier1: fallback to RGB10A2Unorm
  {
    auto info = GetTextureFormatInfo(TextureFormat::k_2_10_10_10_AS_16_16_16_16, f_no_tier1);
    TEST_CHECK(info.is_supported);
    TEST_CHECK(info.host_format == wgpu::TextureFormat::RGB10A2Unorm);
    TEST_CHECK(info.load_shader == TextureLoadShader::k64bpb);
    TEST_CHECK(info.host_bytes_per_block == 4);
  }

  // 8. Other general Xenos formats
  {
    // DXT2_3
    auto info_dxt2 = GetTextureFormatInfo(TextureFormat::k_DXT2_3, f_all);
    TEST_CHECK(info_dxt2.is_supported);
    TEST_CHECK(info_dxt2.host_format == wgpu::TextureFormat::BC2RGBAUnorm);

    // DXN
    auto info_dxn = GetTextureFormatInfo(TextureFormat::k_DXN, f_all);
    TEST_CHECK(info_dxn.is_supported);
    TEST_CHECK(info_dxn.host_format == wgpu::TextureFormat::BC5RGUnorm);

    // 5_6_5
    auto info_565 = GetTextureFormatInfo(TextureFormat::k_5_6_5, f_all);
    TEST_CHECK(info_565.is_supported);
    TEST_CHECK(info_565.conversion == TextureConversion::kR5G6B5);

    // 1_5_5_5
    auto info_1555 = GetTextureFormatInfo(TextureFormat::k_1_5_5_5, f_all);
    TEST_CHECK(info_1555.is_supported);
    TEST_CHECK(info_1555.conversion == TextureConversion::kR5G5B5A1);

    // 4_4_4_4
    auto info_4444 = GetTextureFormatInfo(TextureFormat::k_4_4_4_4, f_all);
    TEST_CHECK(info_4444.is_supported);
    TEST_CHECK(info_4444.conversion == TextureConversion::kR4G4B4A4);

    // Float formats
    auto info_f16 = GetTextureFormatInfo(TextureFormat::k_16_FLOAT, f_all);
    TEST_CHECK(info_f16.is_supported);
    TEST_CHECK(info_f16.host_format == wgpu::TextureFormat::R16Float);

    auto info_f16x2 = GetTextureFormatInfo(TextureFormat::k_16_16_FLOAT, f_all);
    TEST_CHECK(info_f16x2.is_supported);
    TEST_CHECK(info_f16x2.host_format == wgpu::TextureFormat::RG16Float);

    auto info_f16x4 = GetTextureFormatInfo(TextureFormat::k_16_16_16_16_FLOAT, f_all);
    TEST_CHECK(info_f16x4.is_supported);
    TEST_CHECK(info_f16x4.host_format == wgpu::TextureFormat::RGBA16Float);

    auto info_f32 = GetTextureFormatInfo(TextureFormat::k_32_FLOAT, f_all);
    TEST_CHECK(info_f32.is_supported);
    TEST_CHECK(info_f32.host_format == wgpu::TextureFormat::R32Float);

    auto info_f32x2 = GetTextureFormatInfo(TextureFormat::k_32_32_FLOAT, f_all);
    TEST_CHECK(info_f32x2.is_supported);
    TEST_CHECK(info_f32x2.host_format == wgpu::TextureFormat::RG32Float);

    auto info_f32x4 = GetTextureFormatInfo(TextureFormat::k_32_32_32_32_FLOAT, f_all);
    TEST_CHECK(info_f32x4.is_supported);
    TEST_CHECK(info_f32x4.host_format == wgpu::TextureFormat::RGBA32Float);
  }

  // 9. Helper names
  TEST_CHECK(std::string(GetLoadShaderName(TextureLoadShader::k32bpb)) == "texture_load_32bpb");
  TEST_CHECK(std::string(GetLoadShaderName(TextureLoadShader::kDepthFloat)) == "texture_load_depth_float");
  TEST_CHECK(std::string(GetConversionName(TextureConversion::kR5G6B5)) == "r5g6b5");
  TEST_CHECK(std::string(GetConversionName(TextureConversion::kFixed16)) == "fixed16");

  std::printf("PASS: Texture Formats\n");
}

int main() {
  TestColorRenderTargetFormats();
  TestDepthRenderTargetFormats();
  TestTextureFormats();
  std::printf("All Format Tables unit tests passed successfully!\n");
  return 0;
}
