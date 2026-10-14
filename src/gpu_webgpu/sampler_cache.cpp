// rexgpu-webgpu - Sampler cache.

#include "sampler_cache.h"

#include <algorithm>
#include <mutex>

#include "webgpu_provider.h"

namespace rex::graphics::webgpu {

wgpu::AddressMode ToAddressMode(xenos::ClampMode mode) {
  switch (mode) {
    case xenos::ClampMode::kRepeat:
      return wgpu::AddressMode::Repeat;
    case xenos::ClampMode::kMirroredRepeat:
    case xenos::ClampMode::kMirrorClampToEdge:
    case xenos::ClampMode::kMirrorClampToHalfway:
    case xenos::ClampMode::kMirrorClampToBorder:
      return wgpu::AddressMode::MirrorRepeat;
    case xenos::ClampMode::kClampToEdge:
    case xenos::ClampMode::kClampToHalfway:
    case xenos::ClampMode::kClampToBorder:
    default:
      return wgpu::AddressMode::ClampToEdge;
  }
}

wgpu::FilterMode ToFilterMode(xenos::TextureFilter filter) {
  return filter == xenos::TextureFilter::kLinear ? wgpu::FilterMode::Linear
                                                 : wgpu::FilterMode::Nearest;
}

wgpu::MipmapFilterMode ToMipmapFilterMode(xenos::TextureFilter filter) {
  return filter == xenos::TextureFilter::kLinear ? wgpu::MipmapFilterMode::Linear
                                                 : wgpu::MipmapFilterMode::Nearest;
}

SamplerKey MakeSamplerKey(
    const xenos::xe_gpu_texture_fetch_t& fetch,
    const ParsedTextureFetchInstruction::Attributes& overrides,
    bool vertex_shader) {
  auto resolve_filter = [](xenos::TextureFilter instruction, xenos::TextureFilter constant) {
    return instruction == xenos::TextureFilter::kUseFetchConst ? constant : instruction;
  };

  xenos::TextureFilter mag = resolve_filter(overrides.mag_filter, fetch.mag_filter);
  xenos::TextureFilter min = resolve_filter(overrides.min_filter, fetch.min_filter);
  xenos::TextureFilter mip = resolve_filter(overrides.mip_filter, fetch.mip_filter);
  xenos::AnisoFilter aniso = overrides.aniso_filter == xenos::AnisoFilter::kUseFetchConst
                                 ? fetch.aniso_filter
                                 : overrides.aniso_filter;

  uint32_t mip_min_level = fetch.mip_min_level;
  uint32_t mip_max_level = std::max(fetch.mip_max_level, fetch.mip_min_level);
  if (mip == xenos::TextureFilter::kBaseMap) {
    mip_max_level = mip_min_level;
  }

  uint32_t max_anisotropy = 1;
  if (aniso != xenos::AnisoFilter::kDisabled && !vertex_shader) {
    max_anisotropy = uint32_t(1) << (uint32_t(aniso) - 1);
    if (max_anisotropy > 16) {
      max_anisotropy = 16;
    }
  }

  SamplerKey key;
  key.clamp_u = uint32_t(fetch.clamp_x);
  key.clamp_v = uint32_t(fetch.clamp_y);
  key.clamp_w = uint32_t(fetch.clamp_z);
  key.mag_filter = uint32_t(mag);
  key.min_filter = uint32_t(min);
  key.mip_filter = uint32_t(mip);
  key.max_anisotropy = max_anisotropy;
  key.mip_min_level = mip_min_level;
  key.mip_max_level = mip_max_level;
  key.border_color = uint32_t(fetch.border_color);
  key.lod_bias = int32_t(fetch.lod_bias);

  return key;
}

wgpu::SamplerDescriptor MakeSamplerDescriptor(const SamplerKey& key, const char* label) {
  wgpu::SamplerDescriptor desc;
  desc.label = label;
  desc.addressModeU = ToAddressMode(xenos::ClampMode(key.clamp_u));
  desc.addressModeV = ToAddressMode(xenos::ClampMode(key.clamp_v));
  desc.addressModeW = ToAddressMode(xenos::ClampMode(key.clamp_w));
  desc.magFilter = ToFilterMode(xenos::TextureFilter(key.mag_filter));
  desc.minFilter = ToFilterMode(xenos::TextureFilter(key.min_filter));
  desc.mipmapFilter = ToMipmapFilterMode(xenos::TextureFilter(key.mip_filter));
  desc.lodMinClamp = float(key.mip_min_level);
  desc.lodMaxClamp = float(key.mip_max_level);

  // In WebGPU, maxAnisotropy > 1 requires Linear min, mag, and mipmap filtering.
  if (key.max_anisotropy > 1 &&
      desc.magFilter == wgpu::FilterMode::Linear &&
      desc.minFilter == wgpu::FilterMode::Linear &&
      desc.mipmapFilter == wgpu::MipmapFilterMode::Linear) {
    desc.maxAnisotropy = uint16_t(key.max_anisotropy);
  } else {
    desc.maxAnisotropy = 1;
  }

  return desc;
}

wgpu::SamplerDescriptor MakeSamplerDescriptor(
    const xenos::xe_gpu_texture_fetch_t& fetch,
    const ParsedTextureFetchInstruction::Attributes& overrides,
    bool vertex_shader,
    SamplerKey* out_key,
    const char* label) {
  SamplerKey key = MakeSamplerKey(fetch, overrides, vertex_shader);
  if (out_key) {
    *out_key = key;
  }
  return MakeSamplerDescriptor(key, label);
}

SamplerCache::SamplerCache(ui::webgpu::WebGpuProvider& provider) : provider_(provider) {}

SamplerCache::~SamplerCache() {
  Clear();
}

wgpu::Sampler SamplerCache::GetOrCreate(
    const xenos::xe_gpu_texture_fetch_t& fetch,
    const ParsedTextureFetchInstruction::Attributes& overrides,
    bool vertex_shader) {
  SamplerKey key = MakeSamplerKey(fetch, overrides, vertex_shader);
  std::lock_guard lock(provider_.device_mutex());
  auto it = samplers_.find(key);
  if (it != samplers_.end()) {
    return it->second;
  }

  wgpu::SamplerDescriptor desc = MakeSamplerDescriptor(key, "guest sampler");
  wgpu::Sampler sampler = provider_.device().CreateSampler(&desc);
  samplers_.emplace(key, sampler);
  return sampler;
}

wgpu::Sampler SamplerCache::GetOrCreate(const SamplerKey& key,
                                       const wgpu::SamplerDescriptor& desc) {
  std::lock_guard lock(provider_.device_mutex());
  auto it = samplers_.find(key);
  if (it != samplers_.end()) {
    return it->second;
  }

  wgpu::Sampler sampler = provider_.device().CreateSampler(&desc);
  samplers_.emplace(key, sampler);
  return sampler;
}

wgpu::Sampler SamplerCache::Find(const SamplerKey& key) const {
  std::lock_guard lock(provider_.device_mutex());
  auto it = samplers_.find(key);
  if (it != samplers_.end()) {
    return it->second;
  }
  return nullptr;
}

void SamplerCache::Clear() {
  std::lock_guard lock(provider_.device_mutex());
  samplers_.clear();
}

}  // namespace rex::graphics::webgpu
