// rexgpu-webgpu - Format tables and feature fallbacks (sections 9.2, 9.3).
//
// Translates Xenos color/depth render target formats and texture formats to
// WebGPU host formats, computing output scales, conversion routines, and
// compute load shaders with feature-dependent fallbacks.

#pragma once

#include <cstdint>
#include <webgpu/webgpu_cpp.h>

#include <rex/graphics/xenos.h>

namespace rex::ui::webgpu {
class WebGpuProvider;
}  // namespace rex::ui::webgpu

namespace rex::graphics::webgpu::format_tables {

// Hardware / driver features that dictate format selection and fallbacks.
struct DeviceFeatures {
  // Enables rg16snorm and rgba16snorm (Xenos k_16_16 RTs) and rgba16unorm.
  bool texture_formats_tier1 = false;
  // Enables depth32float-stencil8 (all depth in Trials HD).
  bool depth32float_stencil8 = false;
  // Enables BC1, BC2, BC3, BC5 texture compression (all BC textures in Trials HD).
  bool texture_compression_bc = false;
  // Enables linear filtering on r32float textures.
  bool float32_filterable = false;
  // Enables rg11b10ufloat renderable.
  bool rg11b10ufloat_renderable = false;
  // Enables unclipped depth.
  bool depth_clip_control = false;
  // Enables unorm16-texture-formats (r16unorm, rg16unorm, rgba16unorm).
  bool unorm16_texture_formats = false;

  static DeviceFeatures FromDevice(const wgpu::Device& device);
  static DeviceFeatures FromProvider(const ui::webgpu::WebGpuProvider& provider);
};

// =============================================================================
// Section 9.2: Render Target Formats
// =============================================================================

struct ColorRenderTargetFormatInfo {
  wgpu::TextureFormat host_format = wgpu::TextureFormat::Undefined;
  // Multiplier for pixel shader output (1.0f default; 1/32 for k_16_16 when
  // mapped to rg16snorm [-32..32] host format).
  float output_scale = 1.0f;
  bool is_supported = false;
};

// Maps Xenos ColorRenderTargetFormat to host wgpu::TextureFormat and output scale.
ColorRenderTargetFormatInfo GetColorRenderTargetFormatInfo(
    xenos::ColorRenderTargetFormat format,
    const DeviceFeatures& features);

struct DepthRenderTargetFormatInfo {
  wgpu::TextureFormat host_format = wgpu::TextureFormat::Undefined;
  bool has_stencil = false;
  bool is_supported = false;
};

// Maps Xenos DepthRenderTargetFormat to host wgpu::TextureFormat.
DepthRenderTargetFormatInfo GetDepthRenderTargetFormatInfo(
    xenos::DepthRenderTargetFormat format,
    const DeviceFeatures& features);

// Returns true if the host depth format has a stencil component.
bool HostDepthFormatHasStencil(wgpu::TextureFormat format);

// True if this color RT format is used in Trials HD (Section 5.3).
bool IsTrialsColorRenderTargetFormat(xenos::ColorRenderTargetFormat format);

// True if this depth RT format is used in Trials HD (Section 5.3).
bool IsTrialsDepthRenderTargetFormat(xenos::DepthRenderTargetFormat format);

// =============================================================================
// Section 9.3: Texture Formats
// =============================================================================

// Compute shader used to untile / convert texture data (Section 8.9).
enum class TextureLoadShader {
  kNone = 0,
  k8bpb,        // 8 bits per block: k_8, k_8_A, k_8_B
  k16bpb,       // 16 bits per block: k_16_FLOAT
  k32bpb,       // 32 bits per block: k_8_8_8_8, k_2_10_10_10, k_16_16, etc.
  k64bpb,       // 64 bits per block: DXT1, k_2_10_10_10_AS_16_16_16_16, etc.
  k128bpb,      // 128 bits per block: DXT2_3, DXT4_5, DXN, float4
  kDepthFloat,  // float24 / 20e4 depth conversion: k_24_8, k_24_8_FLOAT
};

// CPU-side conversion routine for untiled blocks.
enum class TextureConversion {
  kCopy = 0,
  kR5G6B5,
  kR5G5B5A1,
  kR4G4B4A4,
  kFixed16,
  kDepth24,
};

struct TextureFormatInfo {
  wgpu::TextureFormat host_format = wgpu::TextureFormat::Undefined;
  TextureLoadShader load_shader = TextureLoadShader::kNone;
  TextureConversion conversion = TextureConversion::kCopy;
  uint32_t host_bytes_per_block = 0;
  uint32_t components = 4;
  bool is_compressed = false;
  bool is_supported = false;
};

// Maps Xenos TextureFormat to host wgpu::TextureFormat, load shader, and conversion.
TextureFormatInfo GetTextureFormatInfo(
    xenos::TextureFormat format,
    const DeviceFeatures& features);

// Returns a human-readable name for a TextureLoadShader.
const char* GetLoadShaderName(TextureLoadShader shader);

// Returns a human-readable name for a TextureConversion.
const char* GetConversionName(TextureConversion conversion);

// True if this texture format is used in Trials HD (Section 5.5).
bool IsTrialsTextureFormat(xenos::TextureFormat format);

}  // namespace rex::graphics::webgpu::format_tables
