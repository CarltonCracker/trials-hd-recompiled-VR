// rexgpu-webgpu - texture cache (section 8.9).
//
// Textures the CPU wrote are loaded on the CPU: untiled, endian-swapped and
// converted from guest memory, then uploaded with WriteTexture. A page watch
// on the guest range marks a texture dirty when the CPU writes it again.
// Textures that are resolve destinations come from render targets instead:
// the backend registers a resolve alias (a host texture already holding the
// values a fetch would read) under the destination address (section 8.8).

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include <webgpu/webgpu_cpp.h>

#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/xenos.h>
#include <rex/thread/mutex.h>

#include "sampler_cache.h"

namespace rex::memory {
class Memory;
}  // namespace rex::memory
namespace rex::ui::webgpu {
class WebGpuProvider;
}  // namespace rex::ui::webgpu

namespace rex::graphics::webgpu {

class WebGpuSharedMemory;
class FrameRecorder;

class TextureCache {
 public:
  TextureCache(ui::webgpu::WebGpuProvider& provider, memory::Memory& memory,
               WebGpuSharedMemory& shared_memory, FrameRecorder& frame_recorder);
  ~TextureCache();

  bool Initialize();

  // View for a texture fetch constant (2D or cube), loading or reloading the
  // texture if needed. Never null: missing or unsupported textures get a
  // black placeholder.
  // resolution_scaled_out: set if the view is a resolve at the draw resolution
  // scale (above 1x).
  wgpu::TextureView GetView(const xenos::xe_gpu_texture_fetch_t& fetch, bool is_cube,
                            bool* resolution_scaled_out = nullptr);

  wgpu::Sampler GetSampler(const xenos::xe_gpu_texture_fetch_t& fetch,
                           const ParsedTextureFetchInstruction::Attributes& overrides,
                           bool vertex_shader);

  // A host texture standing in for the guest memory at a resolve destination
  // (physical address). Replaces any previous alias at the address.
  wgpu::Texture GetOrCreateResolveAlias(uint32_t address, wgpu::TextureFormat format,
      uint32_t width, uint32_t height, uint32_t guest_start, uint32_t guest_length,
      uint32_t scale_x, uint32_t scale_y);
  // Resolve aliases are dropped when the CPU writes the memory again.

  size_t texture_count() const { return textures_.size(); }
  SamplerCache& sampler_cache() { return sampler_cache_; }
  const SamplerCache& sampler_cache() const { return sampler_cache_; }

 private:
  union Key {
    struct {
      uint64_t low;
      uint64_t high;
    };
    struct {
      uint32_t base_page : 17;
      uint32_t mip_page : 17;
      uint32_t format : 6;
      uint32_t width_minus_1 : 13;
      uint32_t height_minus_1 : 13;
      uint32_t depth_minus_1 : 10;
      uint32_t pitch : 9;
      uint32_t tiled : 1;
      uint32_t endianness : 2;
      uint32_t dimension : 2;
      uint32_t packed_mips : 1;
      uint32_t mip_max_level : 4;
      // How 16-bit fixed-point formats are converted on the CPU.
      uint32_t is_signed : 1;
      uint32_t is_integer : 1;
    };
    bool operator==(const Key& other) const { return low == other.low && high == other.high; }
  };
  struct KeyHash {
    size_t operator()(const Key& key) const {
      return size_t(key.low * 0x9E3779B97F4A7C15ull ^ key.high);
    }
  };
  struct Texture {
    Key key;
    wgpu::Texture texture;
    wgpu::TextureView view_2d;
    wgpu::TextureView view_cube;
    std::atomic<bool> dirty{true};
    // Chunks whose watch fired (and needs re-registering); see LoadTexture.
    std::atomic<uint64_t> dirty_chunks{~uint64_t(0)};
    // Guest data hashes per chunk at the last load.
    bool loaded = false;
    std::vector<uint64_t> chunk_hashes;
    uint32_t data_start = 0;
    uint32_t data_length = 0;
  };
  struct ResolveAlias {
    wgpu::Texture texture;
    wgpu::TextureView view;
    uint32_t width = 0;
    uint32_t height = 0;
    wgpu::Texture sampled_texture;
    wgpu::TextureView sampled_view;
    uint32_t sampled_width = 0, sampled_height = 0;
    uint32_t scale_x = 1, scale_y = 1;
    bool sampled_dirty = true;
    uint32_t guest_start = 0;
    uint32_t guest_length = 0;
    bool dirty = false;
  };

  static void AliasWatchCallback(const std::unique_lock<std::recursive_mutex>& global_lock,
      void* context, uint32_t first, uint32_t last, bool invalidated_by_gpu);
  bool LoadTexture(Texture& texture);
  static void WatchCallback(const std::unique_lock<std::recursive_mutex>& global_lock,
                            void* context, void* data, uint64_t argument, bool invalidated_by_gpu);

  ui::webgpu::WebGpuProvider& provider_;
  memory::Memory& memory_;
  WebGpuSharedMemory& shared_memory_;
  FrameRecorder& frame_recorder_;
  std::unordered_map<Key, std::unique_ptr<Texture>, KeyHash> textures_;
  std::unordered_map<uint32_t, ResolveAlias> resolve_aliases_;
  rex::thread::global_critical_region alias_mutex_;
  void* alias_watch_ = nullptr;
  SamplerCache sampler_cache_;
  wgpu::TextureView black_2d_;
  wgpu::TextureView black_cube_;
};

}  // namespace rex::graphics::webgpu
