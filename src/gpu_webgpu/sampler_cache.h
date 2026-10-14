// rexgpu-webgpu - Sampler cache (section 8.9).
//
// Translates Xenos texture fetch constants and shader instruction overrides
// to WebGPU SamplerKey and wgpu::SamplerDescriptor (address modes, filtering,
// anisotropy, LOD min/max, LOD bias), caching unique wgpu::Sampler instances.

#pragma once

#include <cstdint>
#include <functional>
#include <unordered_map>

#include <webgpu/webgpu_cpp.h>

#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/xenos.h>

namespace rex::ui::webgpu {
class WebGpuProvider;
}  // namespace rex::ui::webgpu

namespace rex::graphics::webgpu {

// Compact POD key uniquely representing a sampler state.
union SamplerKey {
  uint64_t value = 0;
  struct {
    uint32_t clamp_u : 3;         // xenos::ClampMode
    uint32_t clamp_v : 3;         // xenos::ClampMode
    uint32_t clamp_w : 3;         // xenos::ClampMode
    uint32_t mag_filter : 2;      // xenos::TextureFilter
    uint32_t min_filter : 2;      // xenos::TextureFilter
    uint32_t mip_filter : 2;      // xenos::TextureFilter
    uint32_t max_anisotropy : 5;  // 1..16 (0 treated as 1)
    uint32_t mip_min_level : 4;   // 0..15
    uint32_t mip_max_level : 4;   // 0..15
    uint32_t border_color : 2;    // xenos::BorderColor
    int32_t lod_bias : 10;        // 10-bit signed (-512..511), fixed 5 fractional bits
  };

  bool operator==(const SamplerKey& other) const { return value == other.value; }
  bool operator!=(const SamplerKey& other) const { return value != other.value; }
};

struct SamplerKeyHash {
  size_t operator()(const SamplerKey& k) const {
    return std::hash<uint64_t>{}(k.value);
  }
};

// Converts Xenos clamp mode to WebGPU address mode.
wgpu::AddressMode ToAddressMode(xenos::ClampMode mode);

// Converts Xenos texture filter to WebGPU filter mode.
wgpu::FilterMode ToFilterMode(xenos::TextureFilter filter);

// Converts Xenos texture filter to WebGPU mipmap filter mode.
wgpu::MipmapFilterMode ToMipmapFilterMode(xenos::TextureFilter filter);

// Extracts a SamplerKey from a fetch constant, instruction attributes, and shader stage.
SamplerKey MakeSamplerKey(
    const xenos::xe_gpu_texture_fetch_t& fetch,
    const ParsedTextureFetchInstruction::Attributes& overrides,
    bool vertex_shader);

// Builds a wgpu::SamplerDescriptor from a SamplerKey.
wgpu::SamplerDescriptor MakeSamplerDescriptor(
    const SamplerKey& key,
    const char* label = "guest sampler");

// Builds a wgpu::SamplerDescriptor directly from fetch constant and overrides.
wgpu::SamplerDescriptor MakeSamplerDescriptor(
    const xenos::xe_gpu_texture_fetch_t& fetch,
    const ParsedTextureFetchInstruction::Attributes& overrides,
    bool vertex_shader,
    SamplerKey* out_key = nullptr,
    const char* label = "guest sampler");

class SamplerCache {
 public:
  explicit SamplerCache(ui::webgpu::WebGpuProvider& provider);
  ~SamplerCache();

  // Looks up or creates a cached wgpu::Sampler.
  wgpu::Sampler GetOrCreate(
      const xenos::xe_gpu_texture_fetch_t& fetch,
      const ParsedTextureFetchInstruction::Attributes& overrides,
      bool vertex_shader);

  // Looks up or creates a cached wgpu::Sampler using an explicit key and descriptor.
  wgpu::Sampler GetOrCreate(const SamplerKey& key, const wgpu::SamplerDescriptor& desc);

  // Looks up an existing sampler in the cache, or returns an empty sampler.
  wgpu::Sampler Find(const SamplerKey& key) const;

  // Clears all cached samplers under device lock.
  void Clear();

  size_t size() const { return samplers_.size(); }

 private:
  ui::webgpu::WebGpuProvider& provider_;
  std::unordered_map<SamplerKey, wgpu::Sampler, SamplerKeyHash> samplers_;
};

}  // namespace rex::graphics::webgpu
