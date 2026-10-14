// rexgpu-webgpu - Format tables and feature fallbacks.

#include "format_tables.h"

#include "webgpu_provider.h"

namespace rex::graphics::webgpu::format_tables {

DeviceFeatures DeviceFeatures::FromDevice(const wgpu::Device& device) {
  DeviceFeatures f;
  if (!device) {
    return f;
  }
  f.texture_formats_tier1 = device.HasFeature(wgpu::FeatureName::TextureFormatsTier1);
  f.depth32float_stencil8 = device.HasFeature(wgpu::FeatureName::Depth32FloatStencil8);
  f.texture_compression_bc = device.HasFeature(wgpu::FeatureName::TextureCompressionBC);
  f.float32_filterable = device.HasFeature(wgpu::FeatureName::Float32Filterable);
  f.rg11b10ufloat_renderable = device.HasFeature(wgpu::FeatureName::RG11B10UfloatRenderable);
  f.depth_clip_control = device.HasFeature(wgpu::FeatureName::DepthClipControl);
  f.unorm16_texture_formats = device.HasFeature(wgpu::FeatureName::Unorm16TextureFormats);
  return f;
}

DeviceFeatures DeviceFeatures::FromProvider(const ui::webgpu::WebGpuProvider& provider) {
  DeviceFeatures f;
  f.texture_formats_tier1 = provider.HasFeature(wgpu::FeatureName::TextureFormatsTier1);
  f.depth32float_stencil8 = provider.HasFeature(wgpu::FeatureName::Depth32FloatStencil8);
  f.texture_compression_bc = provider.HasFeature(wgpu::FeatureName::TextureCompressionBC);
  f.float32_filterable = provider.HasFeature(wgpu::FeatureName::Float32Filterable);
  f.rg11b10ufloat_renderable = provider.HasFeature(wgpu::FeatureName::RG11B10UfloatRenderable);
  f.depth_clip_control = provider.HasFeature(wgpu::FeatureName::DepthClipControl);
  f.unorm16_texture_formats = provider.HasFeature(wgpu::FeatureName::Unorm16TextureFormats);
  return f;
}

// =============================================================================
// Section 9.2: Render Target Formats
// =============================================================================

ColorRenderTargetFormatInfo GetColorRenderTargetFormatInfo(
    xenos::ColorRenderTargetFormat format,
    const DeviceFeatures& features) {
  using F = xenos::ColorRenderTargetFormat;
  ColorRenderTargetFormatInfo info;
  info.is_supported = true;
  info.output_scale = 1.0f;

  switch (format) {
    case F::k_8_8_8_8:
    case F::k_8_8_8_8_GAMMA:
      info.host_format = wgpu::TextureFormat::RGBA8Unorm;
      break;

    case F::k_2_10_10_10:
    case F::k_2_10_10_10_AS_10_10_10_10:
      info.host_format = wgpu::TextureFormat::RGB10A2Unorm;
      break;

    case F::k_2_10_10_10_FLOAT:
    case F::k_2_10_10_10_FLOAT_AS_16_16_16_16:
    case F::k_16_16_16_16_FLOAT:
      info.host_format = wgpu::TextureFormat::RGBA16Float;
      break;

    case F::k_16_16:
      if (features.texture_formats_tier1) {
        info.host_format = wgpu::TextureFormat::RG16Snorm;
        info.output_scale = 1.0f / 32.0f;  // Xenos fixed point range -32..32
      } else {
        info.host_format = wgpu::TextureFormat::RG16Float;
        info.output_scale = 1.0f;
      }
      break;

    case F::k_16_16_16_16:
      if (features.texture_formats_tier1) {
        info.host_format = wgpu::TextureFormat::RGBA16Snorm;
        info.output_scale = 1.0f / 32.0f;
      } else {
        info.host_format = wgpu::TextureFormat::RGBA16Float;
        info.output_scale = 1.0f;
      }
      break;

    case F::k_16_16_FLOAT:
      info.host_format = wgpu::TextureFormat::RG16Float;
      break;

    case F::k_32_FLOAT:
      info.host_format = wgpu::TextureFormat::R32Float;
      break;

    case F::k_32_32_FLOAT:
      info.host_format = wgpu::TextureFormat::RG32Float;
      break;

    default:
      info.host_format = wgpu::TextureFormat::Undefined;
      info.is_supported = false;
      break;
  }

  return info;
}

DepthRenderTargetFormatInfo GetDepthRenderTargetFormatInfo(
    xenos::DepthRenderTargetFormat format,
    const DeviceFeatures& features) {
  DepthRenderTargetFormatInfo info;
  info.is_supported = true;
  info.has_stencil = true;

  switch (format) {
    case xenos::DepthRenderTargetFormat::kD24FS8:
      if (features.depth32float_stencil8) {
        info.host_format = wgpu::TextureFormat::Depth32FloatStencil8;
      } else {
        info.host_format = wgpu::TextureFormat::Depth24PlusStencil8;
      }
      break;

    case xenos::DepthRenderTargetFormat::kD24S8:
      info.host_format = wgpu::TextureFormat::Depth24PlusStencil8;
      break;

    default:
      info.host_format = wgpu::TextureFormat::Undefined;
      info.has_stencil = false;
      info.is_supported = false;
      break;
  }

  return info;
}

bool HostDepthFormatHasStencil(wgpu::TextureFormat format) {
  return format == wgpu::TextureFormat::Depth32FloatStencil8 ||
         format == wgpu::TextureFormat::Depth24PlusStencil8;
}

bool IsTrialsColorRenderTargetFormat(xenos::ColorRenderTargetFormat format) {
  using F = xenos::ColorRenderTargetFormat;
  switch (format) {
    case F::k_8_8_8_8:
    case F::k_16_16:
    case F::k_2_10_10_10:
    case F::k_2_10_10_10_FLOAT:
    case F::k_2_10_10_10_FLOAT_AS_16_16_16_16:
      return true;
    default:
      return false;
  }
}

bool IsTrialsDepthRenderTargetFormat(xenos::DepthRenderTargetFormat format) {
  return format == xenos::DepthRenderTargetFormat::kD24FS8;
}

// =============================================================================
// Section 9.3: Texture Formats
// =============================================================================

TextureFormatInfo GetTextureFormatInfo(
    xenos::TextureFormat format,
    const DeviceFeatures& features) {
  using F = xenos::TextureFormat;
  TextureFormatInfo info;
  info.is_supported = true;

  switch (format) {
    // -------------------------------------------------------------------------
    // Block Compressed (BC) formats
    // -------------------------------------------------------------------------
    case F::k_DXT1:
    case F::k_DXT1_AS_16_16_16_16:
      if (features.texture_compression_bc) {
        info.host_format = wgpu::TextureFormat::BC1RGBAUnorm;
        info.load_shader = TextureLoadShader::k64bpb;
        info.conversion = TextureConversion::kCopy;
        info.host_bytes_per_block = 8;
        info.components = 4;
        info.is_compressed = true;
      } else {
        info.host_format = wgpu::TextureFormat::Undefined;
        info.is_supported = false;
      }
      break;

    case F::k_DXT2_3:
    case F::k_DXT2_3_AS_16_16_16_16:
      if (features.texture_compression_bc) {
        info.host_format = wgpu::TextureFormat::BC2RGBAUnorm;
        info.load_shader = TextureLoadShader::k128bpb;
        info.conversion = TextureConversion::kCopy;
        info.host_bytes_per_block = 16;
        info.components = 4;
        info.is_compressed = true;
      } else {
        info.host_format = wgpu::TextureFormat::Undefined;
        info.is_supported = false;
      }
      break;

    case F::k_DXT4_5:
    case F::k_DXT4_5_AS_16_16_16_16:
      if (features.texture_compression_bc) {
        info.host_format = wgpu::TextureFormat::BC3RGBAUnorm;
        info.load_shader = TextureLoadShader::k128bpb;
        info.conversion = TextureConversion::kCopy;
        info.host_bytes_per_block = 16;
        info.components = 4;
        info.is_compressed = true;
      } else {
        info.host_format = wgpu::TextureFormat::Undefined;
        info.is_supported = false;
      }
      break;

    case F::k_DXN:
      if (features.texture_compression_bc) {
        info.host_format = wgpu::TextureFormat::BC5RGUnorm;
        info.load_shader = TextureLoadShader::k128bpb;
        info.conversion = TextureConversion::kCopy;
        info.host_bytes_per_block = 16;
        info.components = 2;
        info.is_compressed = true;
      } else {
        info.host_format = wgpu::TextureFormat::Undefined;
        info.is_supported = false;
      }
      break;

    // -------------------------------------------------------------------------
    // 8-bit unorm formats
    // -------------------------------------------------------------------------
    case F::k_8:
    case F::k_8_A:
    case F::k_8_B:
      info.host_format = wgpu::TextureFormat::R8Unorm;
      info.load_shader = TextureLoadShader::k8bpb;
      info.conversion = TextureConversion::kCopy;
      info.host_bytes_per_block = 1;
      info.components = 1;
      break;

    case F::k_8_8:
      info.host_format = wgpu::TextureFormat::RG8Unorm;
      info.load_shader = TextureLoadShader::k32bpb;
      info.conversion = TextureConversion::kCopy;
      info.host_bytes_per_block = 2;
      info.components = 2;
      break;

    case F::k_8_8_8_8:
    case F::k_8_8_8_8_AS_16_16_16_16:
    case F::k_8_8_8_8_A:
      info.host_format = wgpu::TextureFormat::RGBA8Unorm;
      info.load_shader = TextureLoadShader::k32bpb;
      info.conversion = TextureConversion::kCopy;
      info.host_bytes_per_block = 4;
      info.components = 4;
      break;

    // -------------------------------------------------------------------------
    // 10:10:10:2 formats
    // -------------------------------------------------------------------------
    case F::k_2_10_10_10:
      info.host_format = wgpu::TextureFormat::RGB10A2Unorm;
      info.load_shader = TextureLoadShader::k32bpb;
      info.conversion = TextureConversion::kCopy;
      info.host_bytes_per_block = 4;
      info.components = 4;
      break;

    case F::k_2_10_10_10_AS_16_16_16_16:
      if (features.texture_formats_tier1) {
        info.host_format = wgpu::TextureFormat::RGBA16Unorm;
        info.load_shader = TextureLoadShader::k64bpb;
        info.conversion = TextureConversion::kCopy;
        info.host_bytes_per_block = 8;
        info.components = 4;
      } else {
        // Fallback: 10:10:10:2 copy or RGBA16Float expand
        info.host_format = wgpu::TextureFormat::RGB10A2Unorm;
        info.load_shader = TextureLoadShader::k64bpb;
        info.conversion = TextureConversion::kCopy;
        info.host_bytes_per_block = 4;
        info.components = 4;
      }
      break;

    // -------------------------------------------------------------------------
    // 16-bit packed color formats (converted on CPU / compute to RGBA8)
    // -------------------------------------------------------------------------
    case F::k_5_6_5:
      info.host_format = wgpu::TextureFormat::RGBA8Unorm;
      info.load_shader = TextureLoadShader::k32bpb;
      info.conversion = TextureConversion::kR5G6B5;
      info.host_bytes_per_block = 4;
      info.components = 4;
      break;

    case F::k_1_5_5_5:
      info.host_format = wgpu::TextureFormat::RGBA8Unorm;
      info.load_shader = TextureLoadShader::k32bpb;
      info.conversion = TextureConversion::kR5G5B5A1;
      info.host_bytes_per_block = 4;
      info.components = 4;
      break;

    case F::k_4_4_4_4:
      info.host_format = wgpu::TextureFormat::RGBA8Unorm;
      info.load_shader = TextureLoadShader::k32bpb;
      info.conversion = TextureConversion::kR4G4B4A4;
      info.host_bytes_per_block = 4;
      info.components = 4;
      break;

    // -------------------------------------------------------------------------
    // 16-bit fixed point formats (converted to float32)
    // -------------------------------------------------------------------------
    case F::k_16:
      info.host_format = wgpu::TextureFormat::R32Float;
      info.load_shader = TextureLoadShader::k32bpb;
      info.conversion = TextureConversion::kFixed16;
      info.host_bytes_per_block = 4;
      info.components = 1;
      break;

    case F::k_16_16:
      info.host_format = wgpu::TextureFormat::RG32Float;
      info.load_shader = TextureLoadShader::k32bpb;
      info.conversion = TextureConversion::kFixed16;
      info.host_bytes_per_block = 8;
      info.components = 2;
      break;

    case F::k_16_16_16_16:
      info.host_format = wgpu::TextureFormat::RGBA32Float;
      info.load_shader = TextureLoadShader::k64bpb;
      info.conversion = TextureConversion::kFixed16;
      info.host_bytes_per_block = 16;
      info.components = 4;
      break;

    // -------------------------------------------------------------------------
    // Floating point formats
    // -------------------------------------------------------------------------
    case F::k_16_FLOAT:
      info.host_format = wgpu::TextureFormat::R16Float;
      info.load_shader = TextureLoadShader::k16bpb;
      info.conversion = TextureConversion::kCopy;
      info.host_bytes_per_block = 2;
      info.components = 1;
      break;

    case F::k_16_16_FLOAT:
      info.host_format = wgpu::TextureFormat::RG16Float;
      info.load_shader = TextureLoadShader::k32bpb;
      info.conversion = TextureConversion::kCopy;
      info.host_bytes_per_block = 4;
      info.components = 2;
      break;

    case F::k_16_16_16_16_FLOAT:
      info.host_format = wgpu::TextureFormat::RGBA16Float;
      info.load_shader = TextureLoadShader::k64bpb;
      info.conversion = TextureConversion::kCopy;
      info.host_bytes_per_block = 8;
      info.components = 4;
      break;

    case F::k_32_FLOAT:
      info.host_format = wgpu::TextureFormat::R32Float;
      info.load_shader = TextureLoadShader::k32bpb;
      info.conversion = TextureConversion::kCopy;
      info.host_bytes_per_block = 4;
      info.components = 1;
      break;

    case F::k_32_32_FLOAT:
      info.host_format = wgpu::TextureFormat::RG32Float;
      info.load_shader = TextureLoadShader::k64bpb;
      info.conversion = TextureConversion::kCopy;
      info.host_bytes_per_block = 8;
      info.components = 2;
      break;

    case F::k_32_32_32_32_FLOAT:
      info.host_format = wgpu::TextureFormat::RGBA32Float;
      info.load_shader = TextureLoadShader::k128bpb;
      info.conversion = TextureConversion::kCopy;
      info.host_bytes_per_block = 16;
      info.components = 4;
      break;

    // -------------------------------------------------------------------------
    // Depth formats
    // -------------------------------------------------------------------------
    case F::k_24_8:
    case F::k_24_8_FLOAT:
      info.host_format = wgpu::TextureFormat::R32Float;
      info.load_shader = TextureLoadShader::kDepthFloat;
      info.conversion = TextureConversion::kDepth24;
      info.host_bytes_per_block = 4;
      info.components = 1;
      break;

    default:
      info.host_format = wgpu::TextureFormat::Undefined;
      info.load_shader = TextureLoadShader::kNone;
      info.conversion = TextureConversion::kCopy;
      info.host_bytes_per_block = 0;
      info.components = 0;
      info.is_compressed = false;
      info.is_supported = false;
      break;
  }

  return info;
}

const char* GetLoadShaderName(TextureLoadShader shader) {
  switch (shader) {
    case TextureLoadShader::kNone:
      return "none";
    case TextureLoadShader::k8bpb:
      return "texture_load_8bpb";
    case TextureLoadShader::k16bpb:
      return "texture_load_16bpb";
    case TextureLoadShader::k32bpb:
      return "texture_load_32bpb";
    case TextureLoadShader::k64bpb:
      return "texture_load_64bpb";
    case TextureLoadShader::k128bpb:
      return "texture_load_128bpb";
    case TextureLoadShader::kDepthFloat:
      return "texture_load_depth_float";
    default:
      return "unknown";
  }
}

const char* GetConversionName(TextureConversion conversion) {
  switch (conversion) {
    case TextureConversion::kCopy:
      return "copy";
    case TextureConversion::kR5G6B5:
      return "r5g6b5";
    case TextureConversion::kR5G5B5A1:
      return "r5g5b5a1";
    case TextureConversion::kR4G4B4A4:
      return "r4g4b4a4";
    case TextureConversion::kFixed16:
      return "fixed16";
    case TextureConversion::kDepth24:
      return "depth24";
    default:
      return "unknown";
  }
}

bool IsTrialsTextureFormat(xenos::TextureFormat format) {
  using F = xenos::TextureFormat;
  switch (format) {
    case F::k_DXT4_5:
    case F::k_16_16:
    case F::k_8_8_8_8:
    case F::k_24_8_FLOAT:
    case F::k_DXT1:
    case F::k_8:
    case F::k_2_10_10_10_AS_16_16_16_16:
      return true;
    default:
      return false;
  }
}

}  // namespace rex::graphics::webgpu::format_tables
