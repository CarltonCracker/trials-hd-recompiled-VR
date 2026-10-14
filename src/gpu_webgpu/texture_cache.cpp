// rexgpu-webgpu - texture cache.

#include "texture_cache.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <vector>

#include <rex/graphics/pipeline/texture/conversion.h>
#include <rex/graphics/pipeline/texture/info.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/memory.h>
#include <rex/system/xmemory.h>

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

void ConvertBlock(Conversion conversion, xenos::TextureFormat format, bool is_signed,
                  bool is_integer, uint32_t components, const uint8_t* source,
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
        float value;
        if (is_signed) {
          value = float(int16_t(raw));
          if (!is_integer) {
            value = std::max(value / 32767.0f, -1.0f);
          }
        } else {
          value = float(raw);
          if (!is_integer) {
            value /= 65535.0f;
          }
        }
        out[i] = value;
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

wgpu::AddressMode ToAddressMode(xenos::ClampMode mode) {
  switch (mode) {
    case xenos::ClampMode::kRepeat:
      return wgpu::AddressMode::Repeat;
    case xenos::ClampMode::kMirroredRepeat:
    case xenos::ClampMode::kMirrorClampToEdge:
    case xenos::ClampMode::kMirrorClampToHalfway:
    case xenos::ClampMode::kMirrorClampToBorder:
      return wgpu::AddressMode::MirrorRepeat;
    default:
      return wgpu::AddressMode::ClampToEdge;
  }
}

}  // namespace

TextureCache::TextureCache(ui::webgpu::WebGpuProvider& provider, memory::Memory& memory,
                           WebGpuSharedMemory& shared_memory)
    : provider_(provider), memory_(memory), shared_memory_(shared_memory) {}

TextureCache::~TextureCache() {
  std::lock_guard lock(provider_.device_mutex());
  textures_.clear();
  resolve_aliases_.clear();
  samplers_.clear();
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
  return black_2d_ && black_cube_;
}

void TextureCache::WatchCallback(const std::unique_lock<std::recursive_mutex>& global_lock,
                                 void* context, void* data, uint64_t argument,
                                 bool invalidated_by_gpu) {
  // Guest thread, inside the global critical region: no WebGPU calls.
  auto* texture = static_cast<Texture*>(data);
  texture->watched = false;
  texture->dirty.store(true, std::memory_order_release);
}

void TextureCache::SetResolveAlias(uint32_t address, wgpu::Texture texture, uint32_t width,
                                   uint32_t height) {
  ResolveAlias& alias = resolve_aliases_[address & 0x1FFFFFFF];
  alias.texture = std::move(texture);
  alias.view = alias.texture.CreateView();
  alias.width = width;
  alias.height = height;
}

wgpu::TextureView TextureCache::GetView(const xenos::xe_gpu_texture_fetch_t& fetch, bool is_cube) {
  wgpu::TextureView& placeholder = is_cube ? black_cube_ : black_2d_;
  if (fetch.type != xenos::FetchConstantType::kTexture) {
    return placeholder;
  }
  if (!is_cube) {
    auto alias = resolve_aliases_.find((fetch.base_address << 12) & 0x1FFFFFFF);
    if (alias != resolve_aliases_.end()) {
      return alias->second.view;
    }
  }
  uint32_t width_minus_1, height_minus_1, depth_minus_1, base_page, mip_page, mip_min, mip_max;
  texture_util::GetSubresourcesFromFetchConstant(fetch, &width_minus_1, &height_minus_1,
                                                 &depth_minus_1, &base_page, &mip_page, &mip_min,
                                                 &mip_max);
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
  // Clear before reading so a write during the upload is not lost.
  texture.dirty.store(false, std::memory_order_release);
  if (!texture.watched) {
    uint32_t start = key.base_page ? base_address : mip_address;
    uint32_t end = key.base_page ? base_address + layout.base.level_data_extent_bytes : start;
    if (key.mip_page && layout.mips_total_extent_bytes) {
      start = std::min(start, mip_address);
      end = std::max(end, mip_address + layout.mips_total_extent_bytes);
    }
    shared_memory_.WatchMemoryRange(start, end - start, WatchCallback, this, &texture, 0);
    texture.watched = true;
  }

  uint32_t bytes_per_block = info->bytes_per_block();
  uint32_t bytes_per_block_log2 = rex::log2_floor(bytes_per_block);
  std::vector<uint8_t> swapped(bytes_per_block);
  std::vector<uint8_t> level_data;
  const uint8_t* guest = memory_.TranslatePhysical<const uint8_t*>(0);
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
    level_data.resize(size_t(host_row_bytes) * blocks_y);
    for (uint32_t layer = 0; layer < layers; ++layer) {
      uint32_t layer_address = level_address + storage.array_slice_stride_bytes * layer;
      for (uint32_t y = 0; y < blocks_y; ++y) {
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
          texture_conversion::CopySwapBlock(xenos::Endian(key.endianness), swapped.data(),
                                            guest + address, bytes_per_block);
          ConvertBlock(host.conversion, format, key.is_signed, key.is_integer, host.components,
                       swapped.data(), bytes_per_block,
                       level_data.data() + size_t(y) * host_row_bytes +
                           size_t(x) * host.host_bytes_per_block);
        }
      }
      wgpu::TexelCopyTextureInfo destination;
      destination.texture = texture.texture;
      destination.mipLevel = level;
      destination.origin = {0, 0, layer};
      wgpu::TexelCopyBufferLayout data_layout;
      data_layout.bytesPerRow = host_row_bytes;
      data_layout.rowsPerImage = blocks_y;
      wgpu::Extent3D extent = {blocks_x * info->block_width, blocks_y * info->block_height, 1};
      provider_.queue().WriteTexture(&destination, level_data.data(), level_data.size(),
                                     &data_layout, &extent);
    }
  }
  return true;
}

wgpu::Sampler TextureCache::GetSampler(const xenos::xe_gpu_texture_fetch_t& fetch,
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
  }
  uint64_t sampler_key = uint64_t(fetch.clamp_x) | (uint64_t(fetch.clamp_y) << 3) |
                         (uint64_t(fetch.clamp_z) << 6) | (uint64_t(mag) << 9) |
                         (uint64_t(min) << 11) | (uint64_t(mip) << 13) |
                         (uint64_t(max_anisotropy) << 15) | (uint64_t(mip_min_level) << 21) |
                         (uint64_t(mip_max_level) << 25);
  auto it = samplers_.find(sampler_key);
  if (it != samplers_.end()) {
    return it->second;
  }
  wgpu::SamplerDescriptor desc;
  desc.label = "guest sampler";
  desc.addressModeU = ToAddressMode(fetch.clamp_x);
  desc.addressModeV = ToAddressMode(fetch.clamp_y);
  desc.addressModeW = ToAddressMode(fetch.clamp_z);
  desc.magFilter =
      mag == xenos::TextureFilter::kLinear ? wgpu::FilterMode::Linear : wgpu::FilterMode::Nearest;
  desc.minFilter =
      min == xenos::TextureFilter::kLinear ? wgpu::FilterMode::Linear : wgpu::FilterMode::Nearest;
  desc.mipmapFilter = mip == xenos::TextureFilter::kLinear ? wgpu::MipmapFilterMode::Linear
                                                           : wgpu::MipmapFilterMode::Nearest;
  desc.lodMinClamp = float(mip_min_level);
  desc.lodMaxClamp = float(mip_max_level);
  // Anisotropy needs linear filtering everywhere in WebGPU.
  if (max_anisotropy > 1 && desc.magFilter == wgpu::FilterMode::Linear &&
      desc.minFilter == wgpu::FilterMode::Linear &&
      desc.mipmapFilter == wgpu::MipmapFilterMode::Linear) {
    desc.maxAnisotropy = uint16_t(max_anisotropy);
  }
  wgpu::Sampler sampler = provider_.device().CreateSampler(&desc);
  samplers_.emplace(sampler_key, sampler);
  return sampler;
}

}  // namespace rex::graphics::webgpu
