// rexgpu-webgpu - texture cache.

#include "texture_cache.h"

#include <algorithm>
#include <array>
#include <condition_variable>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/hash.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory.h>
#include <rex/system/xmemory.h>

#include "frame_recorder.h"
#include "guest_memory_mirror.h"
#include "webgpu_provider.h"

namespace rex::graphics::webgpu {

namespace {

enum class Conversion {
  kCopy,         // Blocks copied as is (after the endian swap).
  kR5G6B5,       // To RGBA8.
  kR5G5B5A1,     // k_1_5_5_5 to RGBA8.
  kR4G4B4A4,     // To RGBA8.
  kFixed16,      // 16-bit fixed point to float32 (per component).
  kDepth24,      // 24-bit depth (unorm or 20e4) to float32.
  kUnsupported,
};

class TextureDecodePool {
 public:
  static TextureDecodePool& Instance() {
    static TextureDecodePool pool;
    return pool;
  }

  template <typename F>
  void ParallelFor(uint32_t count, F&& func) {
    if (count <= 1 || workers_.empty()) {
      for (uint32_t i = 0; i < count; ++i) {
        func(i);
      }
      return;
    }

    {
      std::unique_lock lock(mutex_);
      current_func_ = [&](uint32_t idx) { func(idx); };
      tasks_total_ = count;
      next_task_ = 0;
      tasks_remaining_ = count;
      generation_++;
      cv_start_.notify_all();
    }

    while (true) {
      uint32_t idx = next_task_.fetch_add(1, std::memory_order_relaxed);
      if (idx >= count) break;
      func(idx);
      if (tasks_remaining_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        std::lock_guard lk(mutex_);
        cv_done_.notify_one();
      }
    }

    std::unique_lock lock(mutex_);
    cv_done_.wait(lock, [&]() { return tasks_remaining_.load(std::memory_order_acquire) == 0; });
  }

 private:
  TextureDecodePool() {
    uint32_t threads = std::clamp(std::thread::hardware_concurrency(), 2u, 8u) - 1;
    stop_ = false;
    workers_.reserve(threads);
    for (uint32_t i = 0; i < threads; ++i) {
      workers_.emplace_back([this]() {
        uint64_t last_gen = 0;
        while (true) {
          std::unique_lock lock(mutex_);
          cv_start_.wait(lock, [this, &last_gen]() {
            return stop_ || generation_ > last_gen;
          });
          if (stop_) break;
          last_gen = generation_;
          lock.unlock();

          while (true) {
            uint32_t idx = next_task_.fetch_add(1, std::memory_order_relaxed);
            if (idx >= tasks_total_) break;
            current_func_(idx);
            if (tasks_remaining_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
              std::lock_guard lk(mutex_);
              cv_done_.notify_one();
            }
          }
        }
      });
    }
  }

  ~TextureDecodePool() {
    {
      std::lock_guard lock(mutex_);
      stop_ = true;
    }
    cv_start_.notify_all();
    for (auto& t : workers_) {
      if (t.joinable()) t.join();
    }
  }

  std::vector<std::thread> workers_;
  std::mutex mutex_;
  std::condition_variable cv_start_;
  std::condition_variable cv_done_;
  std::function<void(uint32_t)> current_func_;
  uint32_t tasks_total_ = 0;
  std::atomic<uint32_t> next_task_{0};
  std::atomic<uint32_t> tasks_remaining_{0};
  uint64_t generation_ = 0;
  bool stop_ = false;
};

struct HostFormat {
  wgpu::TextureFormat format = wgpu::TextureFormat::Undefined;
  Conversion conversion = Conversion::kUnsupported;
  uint32_t host_bytes_per_block = 0;
  uint32_t components = 4;  // For kFixed16.
};

HostFormat GetHostFormat(xenos::TextureFormat format, bool bc_supported) {
  using F = xenos::TextureFormat;
  switch (format) {
    case F::k_8_8_8_8:
    case F::k_8_8_8_8_A:
    case F::k_8_8_8_8_AS_16_16_16_16:
      return {wgpu::TextureFormat::RGBA8Unorm, Conversion::kCopy, 4};
    case F::k_2_10_10_10:
    case F::k_2_10_10_10_AS_16_16_16_16:
      return {wgpu::TextureFormat::RGB10A2Unorm, Conversion::kCopy, 4};
    case F::k_8:
    case F::k_8_A:
    case F::k_8_B:
      return {wgpu::TextureFormat::R8Unorm, Conversion::kCopy, 1};
    case F::k_8_8:
      return {wgpu::TextureFormat::RG8Unorm, Conversion::kCopy, 2};
    case F::k_5_6_5:
      return {wgpu::TextureFormat::RGBA8Unorm, Conversion::kR5G6B5, 4};
    case F::k_1_5_5_5:
      return {wgpu::TextureFormat::RGBA8Unorm, Conversion::kR5G5B5A1, 4};
    case F::k_4_4_4_4:
      return {wgpu::TextureFormat::RGBA8Unorm, Conversion::kR4G4B4A4, 4};
    case F::k_DXT1:
    case F::k_DXT1_AS_16_16_16_16:
      return bc_supported ? HostFormat{wgpu::TextureFormat::BC1RGBAUnorm, Conversion::kCopy, 8}
                          : HostFormat{};
    case F::k_DXT2_3:
    case F::k_DXT2_3_AS_16_16_16_16:
      return bc_supported ? HostFormat{wgpu::TextureFormat::BC2RGBAUnorm, Conversion::kCopy, 16}
                          : HostFormat{};
    case F::k_DXT4_5:
    case F::k_DXT4_5_AS_16_16_16_16:
      return bc_supported ? HostFormat{wgpu::TextureFormat::BC3RGBAUnorm, Conversion::kCopy, 16}
                          : HostFormat{};
    case F::k_DXN:
      return bc_supported ? HostFormat{wgpu::TextureFormat::BC5RGUnorm, Conversion::kCopy, 16}
                          : HostFormat{};
    case F::k_16:
      return {wgpu::TextureFormat::R32Float, Conversion::kFixed16, 4, 1};
    case F::k_16_16:
      return {wgpu::TextureFormat::RG32Float, Conversion::kFixed16, 8, 2};
    case F::k_16_16_16_16:
      return {wgpu::TextureFormat::RGBA32Float, Conversion::kFixed16, 16, 4};
    case F::k_16_FLOAT:
      return {wgpu::TextureFormat::R16Float, Conversion::kCopy, 2};
    case F::k_16_16_FLOAT:
      return {wgpu::TextureFormat::RG16Float, Conversion::kCopy, 4};
    case F::k_16_16_16_16_FLOAT:
      return {wgpu::TextureFormat::RGBA16Float, Conversion::kCopy, 8};
    case F::k_32_FLOAT:
      return {wgpu::TextureFormat::R32Float, Conversion::kCopy, 4};
    case F::k_32_32_FLOAT:
      return {wgpu::TextureFormat::RG32Float, Conversion::kCopy, 8};
    case F::k_32_32_32_32_FLOAT:
      return {wgpu::TextureFormat::RGBA32Float, Conversion::kCopy, 16};
    case F::k_24_8:
    case F::k_24_8_FLOAT:
      return {wgpu::TextureFormat::R32Float, Conversion::kDepth24, 4};
    default:
      return {};
  }
}

void ConvertBlock(Conversion conversion, xenos::TextureFormat format,
                  uint32_t components, const uint8_t* source,
                  uint32_t source_bytes, uint8_t* dest) {
  switch (conversion) {
    case Conversion::kCopy:
      std::memcpy(dest, source, source_bytes);
      break;
    case Conversion::kR5G6B5: {
      uint16_t v;
      std::memcpy(&v, source, 2);
      dest[0] = uint8_t(((v & 0x1F) * 255 + 15) / 31);
      dest[1] = uint8_t((((v >> 5) & 0x3F) * 255 + 31) / 63);
      dest[2] = uint8_t(((v >> 11) * 255 + 15) / 31);
      dest[3] = 255;
    } break;
    case Conversion::kR5G5B5A1: {
      uint16_t v;
      std::memcpy(&v, source, 2);
      dest[0] = uint8_t(((v & 0x1F) * 255 + 15) / 31);
      dest[1] = uint8_t((((v >> 5) & 0x1F) * 255 + 15) / 31);
      dest[2] = uint8_t((((v >> 10) & 0x1F) * 255 + 15) / 31);
      dest[3] = (v >> 15) ? 255 : 0;
    } break;
    case Conversion::kR4G4B4A4: {
      uint16_t v;
      std::memcpy(&v, source, 2);
      for (uint32_t i = 0; i < 4; ++i) {
        dest[i] = uint8_t(((v >> (4 * i)) & 0xF) * 17);
      }
    } break;
    case Conversion::kFixed16: {
      auto* out = reinterpret_cast<float*>(dest);
      for (uint32_t i = 0; i < components; ++i) {
        uint16_t raw;
        std::memcpy(&raw, source + 2 * i, 2);
        // Keep the bits until fetch-time signedness and normalization are known.
        out[i] = float(raw);
      }
    } break;
    case Conversion::kDepth24: {
      uint32_t v;
      std::memcpy(&v, source, 4);
      uint32_t depth24 = v >> 8;
      float value = format == xenos::TextureFormat::k_24_8_FLOAT
                        ? xenos::Float20e4To32(depth24)
                        : float(depth24) * (1.0f / float(0xFFFFFF));
      std::memcpy(dest, &value, 4);
    } break;
    default:
      break;
  }
}

}  // namespace

TextureCache::TextureCache(ui::webgpu::WebGpuProvider& provider, memory::Memory& memory,
                           WebGpuSharedMemory& shared_memory, FrameRecorder& frame_recorder)
    : provider_(provider), memory_(memory), shared_memory_(shared_memory),
      frame_recorder_(frame_recorder), sampler_cache_(provider) {}

TextureCache::~TextureCache() {
  std::lock_guard lock(provider_.device_mutex());
  auto alias_lock = alias_mutex_.Acquire();
  if (alias_watch_) { shared_memory_.UnregisterGlobalWatch(alias_watch_); }
  textures_.clear();
  resolve_aliases_.clear();
  sampler_cache_.Clear();
}

bool TextureCache::Initialize() {
  const wgpu::Device& device = provider_.device();
  for (bool cube : {false, true}) {
    wgpu::TextureDescriptor desc;
    desc.label = cube ? "black cube" : "black 2D";
    desc.size = {1, 1, cube ? 6u : 1u};
    desc.format = wgpu::TextureFormat::RGBA8Unorm;
    desc.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    wgpu::Texture texture = device.CreateTexture(&desc);
    const uint32_t black[6] = {0xFF000000u, 0xFF000000u, 0xFF000000u,
                               0xFF000000u, 0xFF000000u, 0xFF000000u};
    wgpu::TexelCopyTextureInfo destination;
    destination.texture = texture;
    wgpu::TexelCopyBufferLayout layout;
    layout.bytesPerRow = 4;
    layout.rowsPerImage = 1;
    wgpu::Extent3D extent = {1, 1, cube ? 6u : 1u};
    provider_.queue().WriteTexture(&destination, black, 4 * extent.depthOrArrayLayers, &layout,
                                   &extent);
    wgpu::TextureViewDescriptor view_desc;
    view_desc.dimension = cube ? wgpu::TextureViewDimension::Cube : wgpu::TextureViewDimension::e2D;
    (cube ? black_cube_ : black_2d_) = texture.CreateView(&view_desc);
  }
  if (!black_2d_ || !black_cube_) { return false; }
  alias_watch_ = shared_memory_.RegisterGlobalWatch(AliasWatchCallback, this);
  return true;
}

void TextureCache::WatchCallback(const std::unique_lock<std::recursive_mutex>& global_lock,
                                 void* context, void* data, uint64_t argument,
                                 bool invalidated_by_gpu) {
  // Guest thread, inside the global critical region: no WebGPU calls.
  auto* texture = static_cast<Texture*>(data);
  static_cast<TextureCache*>(context)->epoch_.fetch_add(1, std::memory_order_acq_rel);
  texture->dirty_chunks.fetch_or(uint64_t(1) << argument, std::memory_order_relaxed);
  texture->dirty.store(true, std::memory_order_release);
}

void TextureCache::AliasWatchCallback(const std::unique_lock<std::recursive_mutex>&,
    void* context, uint32_t first, uint32_t last, bool) {
  // Guest-thread callback: only invalidate metadata, never call WebGPU here.
  auto& cache = *static_cast<TextureCache*>(context);
  cache.epoch_.fetch_add(1, std::memory_order_acq_rel);
  for (auto& [address, alias] : cache.resolve_aliases_) {
    if (first < uint64_t(alias.guest_start) + alias.guest_length && last >= alias.guest_start) {
      alias.dirty = true;
    }
  }
}

wgpu::Texture TextureCache::GetOrCreateResolveAlias(uint32_t address, wgpu::TextureFormat format,
    uint32_t width, uint32_t height, uint32_t guest_start, uint32_t guest_length,
    uint32_t scale_x, uint32_t scale_y, wgpu::TextureView* view_out) {
  wgpu::Limits limits;
  provider_.device().GetLimits(&limits);
  if (!width || !height || width > limits.maxTextureDimension2D || height > limits.maxTextureDimension2D ||
      !guest_length || uint64_t(guest_start) + guest_length > (512ull << 20)) { return {}; }
  auto alias_lock = alias_mutex_.Acquire();
  epoch_.fetch_add(1, std::memory_order_acq_rel);
  // Protect guest pages so subsequent CPU writes invalidate the GPU-only alias.
  if (!shared_memory_.RequestRange(guest_start, guest_length)) { return {}; }
  address &= 0x1FFFFFFF;
  for (auto& [other_address, other] : resolve_aliases_) {
    if (other_address != address && guest_start < uint64_t(other.guest_start) + other.guest_length &&
        other.guest_start < uint64_t(guest_start) + guest_length) { other.dirty = true; }
  }
  ResolveAlias& alias = resolve_aliases_[address];
  if (!alias.texture || alias.width != width || alias.height != height || alias.texture.GetFormat() != format) {
    wgpu::TextureDescriptor desc;
    desc.label = "guest resolve alias"; desc.size = {width, height, 1}; desc.format = format;
    desc.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::TextureBinding |
        wgpu::TextureUsage::CopySrc;
    alias.texture = provider_.device().CreateTexture(&desc);
    if (!alias.texture) { return {}; }
    alias.view = alias.texture.CreateView(); alias.width = width; alias.height = height;
    alias.sampled_texture = nullptr; alias.sampled_view = nullptr;
    alias.guest_length = 0;
  }
  uint32_t end = guest_start + guest_length;
  if (alias.guest_length) {
    end = std::max(end, alias.guest_start + alias.guest_length);
    guest_start = std::min(guest_start, alias.guest_start);
  }
  alias.guest_start = guest_start; alias.guest_length = end - guest_start; alias.dirty = false;
  alias.scale_x = scale_x; alias.scale_y = scale_y; alias.sampled_dirty = true;
  if (view_out) {
    *view_out = alias.view;
  }
  return alias.texture;
}

wgpu::TextureView TextureCache::GetView(const xenos::xe_gpu_texture_fetch_t& fetch, bool is_cube,
                                        bool* resolution_scaled_out) {
  if (resolution_scaled_out) {
    *resolution_scaled_out = false;
  }
  wgpu::TextureView& placeholder = is_cube ? black_cube_ : black_2d_;
  if (fetch.type != xenos::FetchConstantType::kTexture) {
    return placeholder;
  }
  uint32_t width_minus_1, height_minus_1, depth_minus_1, base_page, mip_page, mip_min, mip_max;
  texture_util::GetSubresourcesFromFetchConstant(fetch, &width_minus_1, &height_minus_1,
                                                 &depth_minus_1, &base_page, &mip_page, &mip_min,
                                                 &mip_max);
  if (!is_cube) {
    auto alias_lock = alias_mutex_.Acquire();
    auto found = resolve_aliases_.find((fetch.base_address << 12) & 0x1FFFFFFF);
    if (found != resolve_aliases_.end() && !found->second.dirty) {
      ResolveAlias& alias = found->second;
      if (resolution_scaled_out) {
        *resolution_scaled_out = alias.scale_x > 1 || alias.scale_y > 1;
      }
      uint32_t width = (width_minus_1 + 1) * alias.scale_x;
      uint32_t height = (height_minus_1 + 1) * alias.scale_y;
      if (width == alias.width && height == alias.height) { return alias.view; }
      if (width > provider_.limits().maxTextureDimension2D ||
          height > provider_.limits().maxTextureDimension2D) { return placeholder; }
      if (!alias.sampled_texture || alias.sampled_width != width || alias.sampled_height != height) {
        wgpu::TextureDescriptor desc;
        desc.label = "resolve fetch extent"; desc.size = {width, height, 1};
        desc.format = alias.texture.GetFormat();
        desc.usage = wgpu::TextureUsage::CopyDst | wgpu::TextureUsage::TextureBinding;
        alias.sampled_texture = provider_.device().CreateTexture(&desc);
        if (!alias.sampled_texture) { return placeholder; }
        alias.sampled_view = alias.sampled_texture.CreateView();
        alias.sampled_width = width; alias.sampled_height = height; alias.sampled_dirty = true;
      }
      if (alias.sampled_dirty) {
        // Resolve pitches/heights are padded; normalized fetches use the declared extent.
        frame_recorder_.EndRenderPass();
        wgpu::TexelCopyTextureInfo from; from.texture = alias.texture;
        wgpu::TexelCopyTextureInfo to; to.texture = alias.sampled_texture;
        wgpu::Extent3D extent{std::min(width, alias.width), std::min(height, alias.height), 1};
        frame_recorder_.encoder().CopyTextureToTexture(&from, &to, &extent);
        alias.sampled_dirty = false;
      }
      return alias.sampled_view;
    }
  }
  if (!base_page && !mip_page) {
    return placeholder;
  }
  Key key;
  key.low = 0;
  key.high = 0;
  key.base_page = base_page;
  key.mip_page = mip_page;
  key.format = uint32_t(fetch.format);
  key.width_minus_1 = width_minus_1;
  key.height_minus_1 = height_minus_1;
  key.depth_minus_1 = depth_minus_1;
  key.pitch = fetch.pitch;
  key.tiled = fetch.tiled;
  key.endianness = uint32_t(fetch.endianness);
  key.dimension = uint32_t(fetch.dimension);
  key.packed_mips = fetch.packed_mips;
  key.mip_max_level = mip_max;
  key.is_signed = fetch.sign_x == xenos::TextureSign::kSigned;
  key.is_integer = fetch.num_format != 0;
  auto it = textures_.find(key);
  Texture* texture;
  if (it == textures_.end()) {
    auto created = std::make_unique<Texture>();
    created->key = key;
    texture = created.get();
    textures_.emplace(key, std::move(created));
  } else {
    texture = it->second.get();
  }
  if (texture->dirty.load(std::memory_order_acquire)) {
    if (!LoadTexture(*texture)) {
      return placeholder;
    }
  }
  if (is_cube) {
    return texture->view_cube ? texture->view_cube : placeholder;
  }
  return texture->view_2d ? texture->view_2d : placeholder;
}

bool TextureCache::LoadTexture(Texture& texture) {
  const Key& key = texture.key;
  xenos::TextureFormat format = xenos::TextureFormat(key.format);
  HostFormat host =
      GetHostFormat(format, provider_.HasFeature(wgpu::FeatureName::TextureCompressionBC));
  if (host.conversion == Conversion::kUnsupported) {
    static uint64_t logged_formats = 0;
    if (!(logged_formats & (uint64_t(1) << key.format))) {
      logged_formats |= uint64_t(1) << key.format;
      REXGPU_WARN("WebGPU: texture format {} is not supported yet", key.format);
    }
    texture.dirty.store(false);
    return false;
  }
  xenos::DataDimension dimension = xenos::DataDimension(key.dimension);
  if (dimension == xenos::DataDimension::k3D) {
    texture.dirty.store(false);
    return false;
  }
  const FormatInfo* info = FormatInfo::Get(format);
  uint32_t width = key.width_minus_1 + 1;
  uint32_t height = key.height_minus_1 + 1;
  uint32_t layers = key.depth_minus_1 + 1;
  bool is_cube = dimension == xenos::DataDimension::kCube;
  texture_util::TextureGuestLayout layout = texture_util::GetGuestTextureLayout(
      dimension, key.pitch, width, height, layers, key.tiled, format, key.packed_mips,
      key.base_page != 0, key.mip_max_level);
  uint32_t level_count = layout.max_level + 1;

  if (!texture.texture) {
    wgpu::TextureDescriptor desc;
    desc.label = "guest texture";
    // Block-compressed textures need block-aligned sizes on the host.
    uint32_t align_x = info->block_width, align_y = info->block_height;
    desc.size = {(width + align_x - 1) / align_x * align_x,
                 (height + align_y - 1) / align_y * align_y, layers};
    desc.format = host.format;
    desc.mipLevelCount = level_count;
    desc.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
    texture.texture = provider_.device().CreateTexture(&desc);
    wgpu::TextureViewDescriptor view_desc;
    view_desc.dimension = wgpu::TextureViewDimension::e2D;
    view_desc.arrayLayerCount = 1;
    texture.view_2d = texture.texture.CreateView(&view_desc);
    if (is_cube && layers == 6) {
      view_desc.dimension = wgpu::TextureViewDimension::Cube;
      view_desc.arrayLayerCount = 6;
      texture.view_cube = texture.texture.CreateView(&view_desc);
    }
  }

  // Keep the guest pages watched (and uploaded) so CPU writes invalidate us.
  uint32_t base_address = key.base_page << 12;
  uint32_t mip_address = key.mip_page << 12;
  if (key.base_page) {
    shared_memory_.RequestRange(base_address, layout.base.level_data_extent_bytes);
  }
  if (key.mip_page && layout.mips_total_extent_bytes) {
    shared_memory_.RequestRange(mip_address, layout.mips_total_extent_bytes);
  }
  // Watches and hashes per chunk (256 KB, the SDK's write-fault widening
  // block, or larger for big textures to stay within 64 chunks): CPU writes
  // next to a texture invalidate its edge chunks every frame, so only fired
  // chunks are rehashed, and the texture reloads only if one changed.
  uint32_t start = key.base_page ? base_address : mip_address;
  uint32_t end = key.base_page ? base_address + layout.base.level_data_extent_bytes : start;
  if (key.mip_page && layout.mips_total_extent_bytes) {
    start = std::min(start, mip_address);
    end = std::max(end, mip_address + layout.mips_total_extent_bytes);
  }
  uint32_t chunk_shift = 18;
  while (((end - 1) >> chunk_shift) - (start >> chunk_shift) >= 64) {
    ++chunk_shift;
  }
  uint32_t chunk_first = start >> chunk_shift;
  uint32_t chunk_count = ((end - 1) >> chunk_shift) - chunk_first + 1;
  // Clear before reading so a write during the upload is not lost.
  texture.dirty.store(false, std::memory_order_release);
  uint64_t fired = texture.dirty_chunks.exchange(0, std::memory_order_acq_rel);
  if (texture.chunk_hashes.size() != chunk_count) {
    texture.chunk_hashes.assign(chunk_count, 0);
    fired = ~uint64_t(0);
  }
  const uint8_t* guest = memory_.TranslatePhysical<const uint8_t*>(0);
  bool changed = !texture.loaded;
  for (uint32_t i = 0; i < chunk_count; ++i) {
    if (!(fired & (uint64_t(1) << i))) {
      continue;
    }
    uint32_t chunk_start = std::max(start, (chunk_first + i) << chunk_shift);
    uint32_t chunk_end = std::min(end, (chunk_first + i + 1) << chunk_shift);
    shared_memory_.WatchMemoryRange(chunk_start, chunk_end - chunk_start, WatchCallback, this,
                                    &texture, i);
    // Only the texture's own data: not the gap between the base and the mips.
    uint64_t hash = 0;
    auto hash_part = [&](uint32_t part_start, uint32_t part_length) {
      uint32_t first = std::max(chunk_start, part_start);
      uint32_t last = std::min(chunk_end, part_start + part_length);
      if (first < last) {
        hash = XXH3_64bits_withSeed(guest + first, last - first, hash);
      }
    };
    if (key.base_page) {
      hash_part(base_address, layout.base.level_data_extent_bytes);
    }
    if (key.mip_page && layout.mips_total_extent_bytes) {
      hash_part(mip_address, layout.mips_total_extent_bytes);
    }
    changed |= hash != texture.chunk_hashes[i];
    texture.chunk_hashes[i] = hash;
  }
  if (!changed) {
    return true;
  }
  texture.loaded = true;

  uint32_t bytes_per_block = info->bytes_per_block();
  if (bytes_per_block > 32) {
    return false;
  }
  uint32_t bytes_per_block_log2 = rex::log2_floor(bytes_per_block);
  alignas(16) std::array<uint8_t, 32> swapped{};
  for (uint32_t level = 0; level < level_count; ++level) {
    uint32_t level_width = std::max(width >> level, uint32_t(1));
    uint32_t level_height = std::max(height >> level, uint32_t(1));
    uint32_t blocks_x = (level_width + info->block_width - 1) / info->block_width;
    uint32_t blocks_y = (level_height + info->block_height - 1) / info->block_height;
    // Where the level is stored: its own level, or the packed mip tail.
    uint32_t storage_level = level;
    uint32_t offset_x = 0, offset_y = 0, offset_z = 0;
    if (key.packed_mips && layout.packed_level != UINT32_MAX && level >= layout.packed_level) {
      storage_level = layout.packed_level;
      texture_util::GetPackedMipOffset(width, height, 1, format, level, offset_x, offset_y,
                                       offset_z);
    }
    const texture_util::TextureGuestLayout::Level& storage =
        storage_level ? layout.mips[storage_level] : layout.base;
    uint32_t level_address = storage_level ? mip_address + layout.mip_offsets_bytes[storage_level]
                                           : base_address;
    if (storage_level ? !key.mip_page : !key.base_page) {
      continue;
    }
    uint32_t pitch_blocks = storage.row_pitch_bytes / bytes_per_block;
    uint32_t host_row_bytes = blocks_x * host.host_bytes_per_block;
    size_t slice_size = size_t(host_row_bytes) * blocks_y;
    staging_buffer_.resize(slice_size * layers);

    auto decode_slice = [&](uint32_t layer, uint32_t y_start, uint32_t y_end) {
      uint32_t layer_address = level_address + storage.array_slice_stride_bytes * layer;
      uint8_t* layer_data = staging_buffer_.data() + size_t(layer) * slice_size;
      if (!key.tiled && host.conversion == Conversion::kCopy) {
        uint32_t row_bytes = blocks_x * bytes_per_block;
        for (uint32_t y = y_start; y < y_end; ++y) {
          uint32_t sy = y + offset_y;
          uint32_t address = layer_address + sy * storage.row_pitch_bytes + offset_x * bytes_per_block;
          if (address + row_bytes <= 0x20000000) {
            texture_conversion::CopySwapBlock(xenos::Endian(key.endianness),
                                              layer_data + size_t(y) * host_row_bytes,
                                              guest + address, row_bytes);
          }
        }
      } else {
        alignas(16) std::array<uint8_t, 32> swapped{};
        for (uint32_t y = y_start; y < y_end; ++y) {
          for (uint32_t x = 0; x < blocks_x; ++x) {
            uint32_t sx = x + offset_x, sy = y + offset_y;
            uint32_t offset = key.tiled
                                  ? uint32_t(texture_util::GetTiledOffset2D(
                                        int32_t(sx), int32_t(sy), pitch_blocks, bytes_per_block_log2))
                                  : sy * storage.row_pitch_bytes + sx * bytes_per_block;
            uint32_t address = layer_address + offset;
            if (address + bytes_per_block > 0x20000000) {
              continue;
            }
            uint8_t* dest = layer_data + size_t(y) * host_row_bytes +
                            size_t(x) * host.host_bytes_per_block;
            if (host.conversion == Conversion::kCopy) {
              texture_conversion::CopySwapBlock(xenos::Endian(key.endianness), dest,
                                                guest + address, bytes_per_block);
            } else {
              texture_conversion::CopySwapBlock(xenos::Endian(key.endianness), swapped.data(),
                                                guest + address, bytes_per_block);
              ConvertBlock(host.conversion, format, host.components,
                           swapped.data(), bytes_per_block, dest);
            }
          }
        }
      }
    };

    if (layers > 1) {
      TextureDecodePool::Instance().ParallelFor(layers, [&](uint32_t layer) {
        decode_slice(layer, 0, blocks_y);
      });
    } else if (slice_size >= 64 * 1024 && blocks_y >= 32) {
      uint32_t chunk_rows = 16;
      uint32_t num_chunks = (blocks_y + chunk_rows - 1) / chunk_rows;
      TextureDecodePool::Instance().ParallelFor(num_chunks, [&](uint32_t chunk) {
        uint32_t y_start = chunk * chunk_rows;
        uint32_t y_end = std::min(y_start + chunk_rows, blocks_y);
        decode_slice(0, y_start, y_end);
      });
    } else {
      decode_slice(0, 0, blocks_y);
    }

    for (uint32_t layer = 0; layer < layers; ++layer) {
      uint8_t* layer_data = staging_buffer_.data() + size_t(layer) * slice_size;
      wgpu::TexelCopyTextureInfo destination;
      destination.texture = texture.texture;
      destination.mipLevel = level;
      destination.origin = {0, 0, layer};
      wgpu::TexelCopyBufferLayout data_layout;
      data_layout.bytesPerRow = host_row_bytes;
      data_layout.rowsPerImage = blocks_y;
      wgpu::Extent3D extent = {blocks_x * info->block_width, blocks_y * info->block_height, 1};
      provider_.queue().WriteTexture(&destination, layer_data, slice_size,
                                     &data_layout, &extent);
    }
  }
  return true;
}

wgpu::Sampler TextureCache::GetSampler(const xenos::xe_gpu_texture_fetch_t& fetch,
                                       const ParsedTextureFetchInstruction::Attributes& overrides,
                                       bool vertex_shader) {
  return sampler_cache_.GetOrCreate(fetch, overrides, vertex_shader);
}

}  // namespace rex::graphics::webgpu
