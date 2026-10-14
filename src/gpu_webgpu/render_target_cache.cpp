// rexgpu-webgpu - render-target cache.

#include "render_target_cache.h"

#include <algorithm>

#include <fmt/format.h>

#include <rex/graphics/registers.h>
#include <rex/logging.h>

#include "webgpu_provider.h"

namespace rex::graphics::webgpu {

RenderTargetCache::RenderTargetCache(ui::webgpu::WebGpuProvider& provider, uint32_t scale_x,
                                     uint32_t scale_y)
    : provider_(provider), scale_x_(scale_x), scale_y_(scale_y) {}

RenderTargetCache::~RenderTargetCache() {
  std::lock_guard lock(provider_.device_mutex());
  targets_.clear();
}

wgpu::TextureFormat RenderTargetCache::GetColorHostFormat(xenos::ColorRenderTargetFormat format,
                                                          float* output_scale_out) const {
  using F = xenos::ColorRenderTargetFormat;
  float output_scale = 1.0f;
  wgpu::TextureFormat host = wgpu::TextureFormat::RGBA8Unorm;
  bool snorm16 = provider_.HasFeature(wgpu::FeatureName::TextureFormatsTier1);
  switch (format) {
    case F::k_8_8_8_8:
    case F::k_8_8_8_8_GAMMA:
      host = wgpu::TextureFormat::RGBA8Unorm;
      break;
    case F::k_2_10_10_10:
    case F::k_2_10_10_10_AS_10_10_10_10:
      host = wgpu::TextureFormat::RGB10A2Unorm;
      break;
    case F::k_2_10_10_10_FLOAT:
    case F::k_2_10_10_10_FLOAT_AS_16_16_16_16:
    case F::k_16_16_16_16_FLOAT:
      host = wgpu::TextureFormat::RGBA16Float;
      break;
    case F::k_16_16:
      // Xenos fixed point -32...32 (section 9.2).
      host = snorm16 ? wgpu::TextureFormat::RG16Snorm : wgpu::TextureFormat::RG16Float;
      output_scale = snorm16 ? 1.0f / 32.0f : 1.0f;
      break;
    case F::k_16_16_16_16:
      host = snorm16 ? wgpu::TextureFormat::RGBA16Snorm : wgpu::TextureFormat::RGBA16Float;
      output_scale = snorm16 ? 1.0f / 32.0f : 1.0f;
      break;
    case F::k_16_16_FLOAT:
      host = wgpu::TextureFormat::RG16Float;
      break;
    case F::k_32_FLOAT:
      host = wgpu::TextureFormat::R32Float;
      break;
    case F::k_32_32_FLOAT:
      host = wgpu::TextureFormat::RG32Float;
      break;
    default:
      break;
  }
  if (output_scale_out) {
    *output_scale_out = output_scale;
  }
  return host;
}

wgpu::TextureFormat RenderTargetCache::GetDepthHostFormat(
    xenos::DepthRenderTargetFormat format) const {
  if (format == xenos::DepthRenderTargetFormat::kD24FS8 &&
      provider_.HasFeature(wgpu::FeatureName::Depth32FloatStencil8)) {
    return wgpu::TextureFormat::Depth32FloatStencil8;
  }
  return wgpu::TextureFormat::Depth24PlusStencil8;
}

bool RenderTargetCache::HostDepthFormatHasStencil(wgpu::TextureFormat format) {
  return format == wgpu::TextureFormat::Depth32FloatStencil8 ||
         format == wgpu::TextureFormat::Depth24PlusStencil8;
}

RenderTarget* RenderTargetCache::GetOrCreate(RenderTargetKey key) {
  auto it = targets_.find(key.value);
  if (it != targets_.end()) {
    return it->second.get();
  }
  auto target = std::make_unique<RenderTarget>();
  target->key = key;
  xenos::MsaaSamples msaa = xenos::MsaaSamples(key.msaa_samples);
  // WebGPU supports 1 and 4 samples; 2x is drawn at 4x.
  target->sample_count = msaa == xenos::MsaaSamples::k1X ? 1 : 4;
  if (key.is_depth) {
    target->host_format = GetDepthHostFormat(xenos::DepthRenderTargetFormat(key.format));
  } else {
    target->host_format = GetColorHostFormat(xenos::ColorRenderTargetFormat(key.format),
                                             &target->format_output_scale);
  }
  // Without EDRAM emulation the height is not known: allocate everything the
  // EDRAM could hold at this pitch (64bpp formats use two tiles per row).
  bool is_64bpp = !key.is_depth && xenos::IsColorRenderTargetFormat64bpp(
                                       xenos::ColorRenderTargetFormat(key.format));
  uint32_t samples_x = msaa == xenos::MsaaSamples::k4X ? 2 : 1;
  uint32_t samples_y = msaa == xenos::MsaaSamples::k1X ? 1 : 2;
  uint32_t pitch_tiles =
      std::max(uint32_t(1), (key.pitch_pixels * samples_x * (is_64bpp ? 2 : 1) +
                             xenos::kEdramTileWidthSamples - 1) /
                                xenos::kEdramTileWidthSamples);
  uint32_t edram_rows = xenos::kEdramTileCount / pitch_tiles * xenos::kEdramTileHeightSamples;
  uint32_t guest_height = std::min(edram_rows / samples_y, uint32_t(2048));
  uint32_t max_size = provider_.limits().maxTextureDimension2D;
  target->width = std::min(std::max(key.pitch_pixels, uint32_t(1)) * scale_x_, max_size);
  target->height = std::min(guest_height * scale_y_, max_size);

  wgpu::TextureDescriptor desc;
  std::string label =
      fmt::format("{} RT base {} pitch {} format {} msaa {}", key.is_depth ? "depth" : "color",
                  uint32_t(key.base_tiles), uint32_t(key.pitch_pixels), uint32_t(key.format),
                  target->sample_count);
  desc.label = label.c_str();
  desc.size = {target->width, target->height, 1};
  desc.format = target->host_format;
  desc.sampleCount = target->sample_count;
  desc.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding;
  if (target->sample_count == 1) {
    desc.usage |= wgpu::TextureUsage::CopySrc | wgpu::TextureUsage::CopyDst;
  }
  target->texture = provider_.device().CreateTexture(&desc);
  target->view = target->texture.CreateView();
  REXGPU_DEBUG("WebGPU: created {} ({}x{})", label, target->width, target->height);
  RenderTarget* result = target.get();
  targets_.emplace(key.value, std::move(target));
  return result;
}

bool RenderTargetCache::Update(const RegisterFile& regs, uint32_t color_targets_used,
                               bool depth_stencil_used, Binding& binding_out) {
  binding_out = {};
  auto surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  uint32_t pitch = surface_info.surface_pitch;
  if (!pitch) {
    return false;
  }
  RenderTargetKey key;
  key.pitch_pixels = pitch;
  key.msaa_samples = uint32_t(surface_info.msaa_samples);

  static constexpr Register kColorInfoRegisters[] = {
      XE_GPU_REG_RB_COLOR_INFO, XE_GPU_REG_RB_COLOR1_INFO, XE_GPU_REG_RB_COLOR2_INFO,
      XE_GPU_REG_RB_COLOR3_INFO};
  bool any = false;
  uint32_t width = UINT32_MAX, height = UINT32_MAX;
  for (uint32_t i = 0; i < 4; ++i) {
    if (!(color_targets_used & (1u << i))) {
      continue;
    }
    auto color_info = regs.Get<reg::RB_COLOR_INFO>(kColorInfoRegisters[i]);
    key.base_tiles = color_info.color_base;
    key.format = uint32_t(xenos::GetStorageColorFormat(color_info.color_format));
    key.is_depth = 0;
    RenderTarget* target = GetOrCreate(key);
    binding_out.color[i] = target;
    uint32_t base = key.base_tiles;
    auto [last_it, inserted] = last_color_at_base_.try_emplace(base, target);
    if (!inserted && last_it->second != target) {
      REXGPU_DEBUG("WebGPU: EDRAM base {} reused as a different color target (no transfer)",
                   base);
      last_it->second = target;
    }
    width = std::min(width, target->width);
    height = std::min(height, target->height);
    any = true;
  }
  if (depth_stencil_used) {
    auto depth_info = regs.Get<reg::RB_DEPTH_INFO>();
    key.base_tiles = depth_info.depth_base;
    key.format = uint32_t(depth_info.depth_format);
    key.is_depth = 1;
    RenderTarget* target = GetOrCreate(key);
    binding_out.depth = target;
    last_depth_at_base_[uint32_t(key.base_tiles)] = target;
    width = std::min(width, target->width);
    height = std::min(height, target->height);
    any = true;
  }
  if (!any) {
    return false;
  }
  binding_out.sample_count =
      surface_info.msaa_samples == xenos::MsaaSamples::k1X ? 1 : 4;
  binding_out.width = width;
  binding_out.height = height;
  return true;
}

RenderTarget* RenderTargetCache::FindLastUsed(uint32_t base_tiles, bool is_depth) const {
  const auto& map = is_depth ? last_depth_at_base_ : last_color_at_base_;
  auto it = map.find(base_tiles);
  return it != map.end() ? it->second : nullptr;
}

}  // namespace rex::graphics::webgpu
