// rexgpu-webgpu - resolve engine.

#include "resolve_engine.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <vector>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#endif

#include <rex/graphics/flags.h>
#include <rex/graphics/pipeline/texture/util.h>
#include <rex/graphics/xenos.h>
#include <rex/logging.h>
#include <rex/memory.h>

#include "frame_recorder.h"
#include "gpu_profiler.h"
#include "render_target_cache.h"
#include "texture_cache.h"
#include "webgpu_provider.h"
#include "wgsl/resolve.wgsl.h"

namespace rex::graphics::webgpu {

namespace {

struct ResolveConstants {
  int32_t source_offset[2];
  int32_t dest_offset[2];
  float scale;
  uint32_t mode;
  uint32_t flags;
  uint32_t padding;
};

enum Mode : uint32_t { kUnorm = 0, kSignedInteger = 1, kUnsignedInteger = 2, kFloat = 3 };

struct DestFormat {
  wgpu::TextureFormat format;
  Mode mode;
  float number_scale = 1.0f;
};

DestFormat GetDestFormat(xenos::TextureFormat format, xenos::SurfaceNumberFormat number) {
  using F = xenos::TextureFormat;
  bool fixed_signed = number == xenos::SurfaceNumberFormat::kSignedInteger ||
                      number == xenos::SurfaceNumberFormat::kSignedRepeatingFraction;
  Mode fixed_mode = fixed_signed ? kSignedInteger : kUnsignedInteger;
  float fixed_scale = number == xenos::SurfaceNumberFormat::kSignedRepeatingFraction ? 32767.0f
                    : number == xenos::SurfaceNumberFormat::kUnsignedRepeatingFraction ? 65535.0f
                    : 1.0f;
  switch (format) {
    case F::k_8_8_8_8:
    case F::k_8_8_8_8_A:
    case F::k_8_8_8_8_AS_16_16_16_16:
      return {wgpu::TextureFormat::RGBA8Unorm, kUnorm};
    case F::k_2_10_10_10:
    case F::k_2_10_10_10_AS_16_16_16_16:
      return {wgpu::TextureFormat::RGB10A2Unorm, kUnorm};
    case F::k_8:
    case F::k_8_A:
    case F::k_8_B:
      return {wgpu::TextureFormat::R8Unorm, kUnorm};
    case F::k_16:
      return {wgpu::TextureFormat::R32Float, fixed_mode, fixed_scale};
    case F::k_16_16:
      return {wgpu::TextureFormat::RG32Float, fixed_mode, fixed_scale};
    case F::k_16_16_16_16:
      return {wgpu::TextureFormat::RGBA32Float, fixed_mode, fixed_scale};
    case F::k_32_FLOAT:
    case F::k_24_8:
    case F::k_24_8_FLOAT:
      return {wgpu::TextureFormat::R32Float, kFloat};
    case F::k_32_32_FLOAT:
      return {wgpu::TextureFormat::RG32Float, kFloat};
    default:
      return {wgpu::TextureFormat::RGBA16Float, kFloat};
  }
}

uint32_t GetSampleMask(xenos::CopySampleSelect select) {
  switch (select) {
    case xenos::CopySampleSelect::k0:
      return 0b0001;
    case xenos::CopySampleSelect::k1:
      return 0b0010;
    case xenos::CopySampleSelect::k2:
      return 0b0100;
    case xenos::CopySampleSelect::k3:
      return 0b1000;
    case xenos::CopySampleSelect::k01:
      return 0b0011;
    case xenos::CopySampleSelect::k23:
      return 0b1100;
    default:
      return 0b1111;
  }
}

}  // namespace

bool ResolveEngine::Initialize() {
  const wgpu::Device& device = provider_.device();
  wgpu::ShaderSourceWGSL wgsl;
  wgsl.code = kWgsl_resolve;
  wgpu::ShaderModuleDescriptor module_desc;
  module_desc.nextInChain = &wgsl;
  module_desc.label = "resolve.wgsl";
  module_ = device.CreateShaderModule(&module_desc);
  for (uint32_t i = 0; i < 4; ++i) {
    std::array<wgpu::BindGroupLayoutEntry, 2> entries{};
    entries[0].binding = i;
    entries[0].visibility = wgpu::ShaderStage::Fragment;
    SourceType type = SourceType(i);
    bool multisample = type == SourceType::kColorMultisample || type == SourceType::kDepthMultisample;
    bool depth = type == SourceType::kDepth || type == SourceType::kDepthMultisample;
    entries[0].texture.sampleType =
        depth ? wgpu::TextureSampleType::Depth : wgpu::TextureSampleType::UnfilterableFloat;
    entries[0].texture.multisampled = multisample;
    entries[1].binding = 4;
    entries[1].visibility = wgpu::ShaderStage::Fragment;
    entries[1].buffer.type = wgpu::BufferBindingType::Uniform;
    entries[1].buffer.hasDynamicOffset = true;
    entries[1].buffer.minBindingSize = sizeof(ResolveConstants);
    wgpu::BindGroupLayoutDescriptor desc;
    desc.label = "resolve";
    desc.entryCount = entries.size();
    desc.entries = entries.data();
    layouts_[i] = device.CreateBindGroupLayout(&desc);
    wgpu::PipelineLayoutDescriptor layout_desc{};
    layout_desc.bindGroupLayoutCount = 1;
    layout_desc.bindGroupLayouts = &layouts_[i];
    pipeline_layouts_[i] = device.CreatePipelineLayout(&layout_desc);
  }
  return bool(module_);
}

wgpu::RenderPipeline ResolveEngine::GetPipeline(SourceType source_type,
                                                wgpu::TextureFormat dest_format) {
  uint64_t key = (uint64_t(source_type) << 32) | uint64_t(dest_format);
  auto it = pipelines_.find(key);
  if (it != pipelines_.end()) {
    return it->second;
  }
  static constexpr const char* kEntryPoints[] = {"fs_color", "fs_color_ms", "fs_depth",
                                                 "fs_depth_ms"};
  wgpu::ColorTargetState target;
  target.format = dest_format;
  wgpu::FragmentState fragment{};
  fragment.module = module_;
  fragment.entryPoint = kEntryPoints[uint32_t(source_type)];
  fragment.targetCount = 1;
  fragment.targets = &target;
  wgpu::RenderPipelineDescriptor desc;
  desc.label = "resolve";
  desc.layout = pipeline_layouts_[uint32_t(source_type)];
  desc.vertex.module = module_;
  desc.vertex.entryPoint = "vs_main";
  desc.fragment = &fragment;
  wgpu::RenderPipeline pipeline = provider_.device().CreateRenderPipeline(&desc);
  pipelines_.emplace(key, pipeline);
  return pipeline;
}

void ResolveEngine::GetSourceRectangle(const draw_util::ResolveInfo& info, uint32_t scale_x,
                                       uint32_t scale_y, uint32_t& x, uint32_t& y,
                                       uint32_t& width, uint32_t& height) {
  bool is_depth = info.IsCopyingDepth();
  const draw_util::ResolveEdramInfo& edram = is_depth ? info.depth_edram_info
                                                      : info.color_edram_info;
  uint32_t original_base = is_depth ? info.depth_original_base : info.color_original_base;
  uint32_t samples_x = edram.msaa_samples == xenos::MsaaSamples::k4X ? 2 : 1;
  uint32_t samples_y = edram.msaa_samples == xenos::MsaaSamples::k1X ? 1 : 2;
  uint32_t tile_width =
      xenos::kEdramTileWidthSamples / samples_x / (edram.format_is_64bpp ? 2 : 1);
  uint32_t tile_height = xenos::kEdramTileHeightSamples / samples_y;
  uint32_t pitch_tiles = std::max(uint32_t(edram.pitch_tiles), uint32_t(1));
  uint32_t delta = (edram.base_tiles - original_base) & (xenos::kEdramTileCount - 1);
  x = ((delta % pitch_tiles) * tile_width + info.coordinate_info.edram_offset_x_div_8 * 8) *
      scale_x;
  y = ((delta / pitch_tiles) * tile_height + info.coordinate_info.edram_offset_y_div_8 * 8) *
      scale_y;
  width = info.coordinate_info.width_div_8 * 8 * scale_x;
  height = info.height_div_8 * 8 * scale_y;
}

bool ResolveEngine::Copy(const draw_util::ResolveInfo& info, RenderTarget& source_target) {
  if (info.rb_copy_control.copy_command == xenos::CopyCommand::kNull || !info.height_div_8 ||
      !info.coordinate_info.width_div_8 || !info.copy_dest_extent_length) {
    return false;
  }
  bool is_depth = info.IsCopyingDepth();
  RenderTarget* source = &source_target;
  uint32_t scale_x = render_target_cache_.scale_x();
  uint32_t scale_y = render_target_cache_.scale_y();
  uint32_t source_x, source_y, width, height;
  GetSourceRectangle(info, scale_x, scale_y, source_x, source_y, width, height);
  if (source_x >= source->width || source_y >= source->height) {
    return false;
  }
  width = std::min(width, source->width - source_x);
  height = std::min(height, source->height - source_y);

  // Destination texture (its full guest extent, at the render scale).
  const draw_util::ResolveCopyDestCoordinateInfo& dest = info.copy_dest_coordinate_info;
  uint32_t dest_width = dest.pitch_aligned_div_32 * 32 * scale_x;
  uint32_t dest_height = dest.height_aligned_div_32 * 32 * scale_y;
  uint32_t dest_x = dest.offset_x_div_8 * 8 * scale_x;
  uint32_t dest_y = dest.offset_y_div_8 * 8 * scale_y;
  if (!dest_width || !dest_height || dest_x >= dest_width || dest_y >= dest_height) {
    return false;
  }
  width = std::min(width, dest_width - dest_x);
  height = std::min(height, dest_height - dest_y);
  DestFormat dest_format = GetDestFormat(xenos::TextureFormat(info.copy_dest_info.copy_dest_format),
                                         info.copy_dest_info.copy_dest_number);
  wgpu::TextureView alias_view;
  wgpu::Texture alias = texture_cache_.GetOrCreateResolveAlias(
      info.copy_dest_base, dest_format.format, dest_width, dest_height,
      info.copy_dest_extent_start, info.copy_dest_extent_length, scale_x, scale_y, &alias_view);
  if (!alias || !alias_view) {
    return false;
  }

  ResolveConstants constants = {};
  constants.source_offset[0] = int32_t(source_x);
  constants.source_offset[1] = int32_t(source_y);
  constants.dest_offset[0] = int32_t(dest_x);
  constants.dest_offset[1] = int32_t(dest_y);
  // Render targets store value * format_output_scale (e.g. k_16_16 on snorm).
  constants.scale = std::ldexp(is_depth ? 1.0f : 1.0f / source->format_output_scale,
                               info.copy_dest_info.copy_dest_exp_bias);
  constants.scale *= dest_format.number_scale;
  constants.mode = dest_format.mode;
  constants.flags = GetSampleMask(info.rb_copy_control.copy_sample_select) |
                    (info.copy_dest_info.copy_dest_swap ? 16u : 0u);
  // At 2x/3x the half-pixel offset leaves the first host column and row of
  // full-screen passes undrawn; copy the next one there, as the SDK does.
  if ((is_depth ? info.depth_edram_info : info.color_edram_info).fill_half_pixel_offset) {
    constants.flags |= (std::min(scale_x >> 1, 3u) << 8) | (std::min(scale_y >> 1, 3u) << 10);
  }
  uint32_t constants_offset = frame_recorder_.UploadUniforms(&constants, sizeof(constants));

  bool multisample = source->sample_count > 1;
  SourceType source_type = is_depth ? (multisample ? SourceType::kDepthMultisample
                                                   : SourceType::kDepth)
                                    : (multisample ? SourceType::kColorMultisample
                                                   : SourceType::kColor);
  wgpu::RenderPipeline pipeline = GetPipeline(source_type, dest_format.format);
  // One bind group per source view (the constants use a dynamic offset).
  const wgpu::TextureView& source_view = is_depth ? source->depth_only_view : source->view;
  wgpu::BindGroup& bind_group = bind_groups_[source_view.Get()];
  if (!bind_group) {
    std::array<wgpu::BindGroupEntry, 2> entries{};
    entries[0].binding = uint32_t(source_type);
    entries[0].textureView = source_view;
    entries[1].binding = 4;
    entries[1].buffer = frame_recorder_.uniform_ring();
    entries[1].size = sizeof(ResolveConstants);
    wgpu::BindGroupDescriptor bind_group_desc;
    bind_group_desc.layout = layouts_[uint32_t(source_type)];
    bind_group_desc.entryCount = entries.size();
    bind_group_desc.entries = entries.data();
    bind_group = provider_.device().CreateBindGroup(&bind_group_desc);
  }

  frame_recorder_.EndRenderPass();
  wgpu::RenderPassColorAttachment attachment;
  attachment.view = alias_view;
  attachment.loadOp = wgpu::LoadOp::Load;
  attachment.storeOp = wgpu::StoreOp::Store;
  wgpu::RenderPassDescriptor pass_desc{};
  pass_desc.label = "resolve";
  pass_desc.colorAttachmentCount = 1;
  pass_desc.colorAttachments = &attachment;
  wgpu::PassTimestampWrites timestamp_writes;
  if (gpu_profiler_ && gpu_profiler_->BeginPass(PassCategory::kResolve, timestamp_writes,
                                                source->key.value)) {
    pass_desc.timestampWrites = &timestamp_writes;
  }
  wgpu::RenderPassEncoder pass = frame_recorder_.encoder().BeginRenderPass(&pass_desc);
  pass.SetPipeline(pipeline);
  pass.SetBindGroup(0, bind_group, 1, &constants_offset);
  pass.SetViewport(float(dest_x), float(dest_y), float(width), float(height), 0.0f, 1.0f);
  pass.SetScissorRect(dest_x, dest_y, width, height);
  pass.Draw(3);
  pass.End();

  int32_t readback_max_bytes = REXCVAR_GET(readback_resolve_max_bytes);
  if (REXCVAR_GET(readback_resolve) != "none" && readback_max_bytes > 0 &&
      info.copy_dest_extent_length <= uint32_t(readback_max_bytes)) {
    ReadBack(info, alias, dest_x, dest_y, width, height, scale_x, scale_y);
  }
  return true;
}

void ResolveEngine::ReadBack(const draw_util::ResolveInfo& info, const wgpu::Texture& alias,
                             uint32_t dest_x, uint32_t dest_y, uint32_t width, uint32_t height,
                             uint32_t scale_x, uint32_t scale_y) {
  using F = xenos::TextureFormat;
  uint32_t host_bytes;
  switch (F(info.copy_dest_info.copy_dest_format)) {
    case F::k_8_8_8_8:
    case F::k_8_8_8_8_A:
    case F::k_8_8_8_8_AS_16_16_16_16:
    case F::k_2_10_10_10:
    case F::k_2_10_10_10_AS_16_16_16_16:
    case F::k_32_FLOAT:
      host_bytes = 4;
      break;
    case F::k_16_16:
      host_bytes = 8;  // RG32Float holding the raw 16-bit values.
      break;
    default:
      return;  // Only 32bpp destinations; Trials reads back k_8_8_8_8.
  }
  if (info.copy_dest_info.copy_dest_array || uint32_t(info.copy_dest_info.copy_dest_endian) > 3) {
    return;
  }

  // Like the Vulkan "fast" mode, guest memory gets the data of an earlier
  // frame's copy of the same resolve, so the GPU is never waited for.
  provider_.instance().ProcessEvents();
  for (auto& [other_key, other] : readbacks_) {
    if (other.state == Readback::kCopied &&
        frame_recorder_.submission_count() > other.submission) {
      other.state = Readback::kMapping;
      std::atomic<int>* state = &other.state;
      other.buffer.MapAsync(wgpu::MapMode::Read, 0, other.buffer.GetSize(),
                            wgpu::CallbackMode::AllowSpontaneous,
                            [state](wgpu::MapAsyncStatus status, wgpu::StringView) {
                              *state = status == wgpu::MapAsyncStatus::Success
                                           ? Readback::kMapped
                                           : Readback::kIdle;
                            });
    }
  }
  Readback& readback =
      readbacks_[(uint64_t(info.copy_dest_extent_start) << 32) | info.copy_dest_extent_length];
  if (readback.state == Readback::kMapped) {
    WriteToGuest(readback);
    readback.buffer.Unmap();
    readback.state = Readback::kIdle;
  }
  if (readback.state != Readback::kIdle) {
    return;  // The previous copy is still in flight.
  }
  uint32_t row_pitch = (width * host_bytes + 255) & ~uint32_t(255);
  uint64_t size = uint64_t(row_pitch) * height;
  if (!readback.buffer || readback.buffer.GetSize() != size) {
    wgpu::BufferDescriptor buffer_desc{};
    buffer_desc.label = "resolve readback";
    buffer_desc.size = size;
    buffer_desc.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
    readback.buffer = provider_.device().CreateBuffer(&buffer_desc);
  }
  wgpu::TexelCopyTextureInfo from;
  from.texture = alias;
  from.origin = {dest_x, dest_y, 0};
  wgpu::TexelCopyBufferInfo to;
  to.buffer = readback.buffer;
  to.layout.bytesPerRow = row_pitch;
  to.layout.rowsPerImage = height;
  wgpu::Extent3D extent{width, height, 1};
  frame_recorder_.encoder().CopyTextureToBuffer(&from, &to, &extent);
  readback.info = info;
  readback.dest_x = dest_x;
  readback.dest_y = dest_y;
  readback.width = width;
  readback.height = height;
  readback.scale_x = scale_x;
  readback.scale_y = scale_y;
  readback.row_pitch = row_pitch;
  readback.host_bytes = host_bytes;
  readback.submission = frame_recorder_.submission_count();
  readback.state = Readback::kCopied;
}

void ResolveEngine::WriteToGuest(const Readback& readback) {
  const draw_util::ResolveInfo& info = readback.info;
  uint8_t* guest = memory_.TranslatePhysical(info.copy_dest_extent_start);
  if (!guest) {
    return;
  }
  const auto* host = static_cast<const uint8_t*>(readback.buffer.GetConstMappedRange());
  auto format = xenos::TextureFormat(info.copy_dest_info.copy_dest_format);
  bool is_16_16 = format == xenos::TextureFormat::k_16_16;
  bool is_10_10_10_2 = format == xenos::TextureFormat::k_2_10_10_10 ||
                       format == xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16;
  bool is_32_float = format == xenos::TextureFormat::k_32_FLOAT;
  auto endian = xenos::Endian(info.copy_dest_info.copy_dest_endian);
  uint32_t pitch = info.copy_dest_coordinate_info.pitch_aligned_div_32 * 32;
  uint32_t start = info.copy_dest_extent_start - (info.copy_dest_base & 0x1FFFFFFF);
  // At a render scale above 1x, the host pixels of each guest pixel are
  // averaged (each holds a differently positioned sample of the same guest
  // pixel; a single one biases the game's luminance measurement).
  uint32_t scale_x = readback.scale_x, scale_y = readback.scale_y;
  // 1x Native fast-path: no downsampling or averaging required.
  if (scale_x == 1 && scale_y == 1) {
    for (uint32_t y = 0; y < readback.height; ++y) {
      const uint8_t* row = host + y * readback.row_pitch;
      const int32_t dest_y = int32_t(readback.dest_y + y);
      for (uint32_t x = 0; x < readback.width; ++x) {
        uint32_t value;
        if (is_16_16) {
          float components[2];
          std::memcpy(components, row + x * readback.host_bytes, sizeof(components));
          value = (uint32_t(components[0]) & 0xFFFF) | (uint32_t(components[1]) << 16);
        } else {
          std::memcpy(&value, row + x * 4, 4);
        }
        int32_t offset =
            texture_util::GetTiledOffset2D(int32_t(readback.dest_x + x),
                                           dest_y, pitch, 2) -
            int32_t(start);
        if (offset < 0 || uint32_t(offset) + 4 > info.copy_dest_extent_length) {
          continue;
        }
        value = xenos::GpuSwap(value, endian);
        std::memcpy(guest + offset, &value, 4);
      }
    }
    return;
  }

  // 2x Downsampling fast-path: accelerated 2x2 box filtering using SIMD/bit shifts.
  if (scale_x == 2 && scale_y == 2) {
    const uint32_t guest_h = readback.height >> 1;
    const uint32_t guest_w = readback.width >> 1;
    for (uint32_t y = 0; y < guest_h; ++y) {
      const uint8_t* row0 = host + (y * 2) * readback.row_pitch;
      const uint8_t* row1 = host + (y * 2 + 1) * readback.row_pitch;
      const int32_t dest_y = int32_t((readback.dest_y >> 1) + y);
      for (uint32_t x = 0; x < guest_w; ++x) {
        uint32_t value;
        if (is_16_16) {
          float components[2];
          std::memcpy(components, row0 + (x * 2) * readback.host_bytes, sizeof(components));
          value = (uint32_t(components[0]) & 0xFFFF) | (uint32_t(components[1]) << 16);
        } else if (is_10_10_10_2) {
          uint32_t p0, p1, p2, p3;
          std::memcpy(&p0, row0 + x * 8, 4);
          std::memcpy(&p1, row0 + x * 8 + 4, 4);
          std::memcpy(&p2, row1 + x * 8, 4);
          std::memcpy(&p3, row1 + x * 8 + 4, 4);
          uint32_t s0 = (p0 & 0x3FF) + (p1 & 0x3FF) + (p2 & 0x3FF) + (p3 & 0x3FF);
          uint32_t s1 = ((p0 >> 10) & 0x3FF) + ((p1 >> 10) & 0x3FF) + ((p2 >> 10) & 0x3FF) + ((p3 >> 10) & 0x3FF);
          uint32_t s2 = ((p0 >> 20) & 0x3FF) + ((p1 >> 20) & 0x3FF) + ((p2 >> 20) & 0x3FF) + ((p3 >> 20) & 0x3FF);
          uint32_t s3 = (p0 >> 30) + (p1 >> 30) + (p2 >> 30) + (p3 >> 30);
          value = ((s0 + 2) >> 2) | (((s1 + 2) >> 2) << 10) |
                  (((s2 + 2) >> 2) << 20) | (((s3 + 2) >> 2) << 30);
        } else if (is_32_float) {
          float f0, f1, f2, f3;
          std::memcpy(&f0, row0 + x * 8, 4);
          std::memcpy(&f1, row0 + x * 8 + 4, 4);
          std::memcpy(&f2, row1 + x * 8, 4);
          std::memcpy(&f3, row1 + x * 8 + 4, 4);
          float sum = (f0 + f1 + f2 + f3) * 0.25f;
          value = std::bit_cast<uint32_t>(sum);
        } else {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
          uint32_t p0, p1, p2, p3;
          std::memcpy(&p0, row0 + x * 8, 4);
          std::memcpy(&p1, row0 + x * 8 + 4, 4);
          std::memcpy(&p2, row1 + x * 8, 4);
          std::memcpy(&p3, row1 + x * 8 + 4, 4);
          __m128i a = _mm_cvtsi32_si128(p0);
          __m128i b = _mm_cvtsi32_si128(p1);
          __m128i c = _mm_cvtsi32_si128(p2);
          __m128i d = _mm_cvtsi32_si128(p3);
          __m128i avg_row0 = _mm_avg_epu8(a, b);
          __m128i avg_row1 = _mm_avg_epu8(c, d);
          __m128i avg = _mm_avg_epu8(avg_row0, avg_row1);
          value = _mm_cvtsi128_si32(avg);
#else
          uint32_t sums[4];
          for (uint32_t c = 0; c < 4; ++c) {
            sums[c] = uint32_t(row0[x * 8 + c]) + uint32_t(row0[x * 8 + 4 + c]) +
                      uint32_t(row1[x * 8 + c]) + uint32_t(row1[x * 8 + 4 + c]);
          }
          value = ((sums[0] + 2) >> 2) |
                  (((sums[1] + 2) >> 2) << 8) |
                  (((sums[2] + 2) >> 2) << 16) |
                  (((sums[3] + 2) >> 2) << 24);
#endif
        }
        int32_t offset =
            texture_util::GetTiledOffset2D(int32_t((readback.dest_x >> 1) + x),
                                           dest_y, pitch, 2) -
            int32_t(start);
        if (offset < 0 || uint32_t(offset) + 4 > info.copy_dest_extent_length) {
          continue;
        }
        value = xenos::GpuSwap(value, endian);
        std::memcpy(guest + offset, &value, 4);
      }
    }
    return;
  }

  for (uint32_t y = 0; y < readback.height / scale_y; ++y) {
    for (uint32_t x = 0; x < readback.width / scale_x; ++x) {
      uint32_t value;
      if (is_16_16) {
        // Integer data: no averaging.
        const uint8_t* texel = host + (y * scale_y) * readback.row_pitch +
                               (x * scale_x) * readback.host_bytes;
        float components[2];
        std::memcpy(components, texel, sizeof(components));
        value = (uint32_t(components[0]) & 0xFFFF) | (uint32_t(components[1]) << 16);
      } else if (is_10_10_10_2) {
        uint32_t sums[4] = {};
        for (uint32_t sy = 0; sy < scale_y; ++sy) {
          for (uint32_t sx = 0; sx < scale_x; ++sx) {
            uint32_t texel_value;
            std::memcpy(&texel_value, host + (y * scale_y + sy) * readback.row_pitch +
                                          (x * scale_x + sx) * 4, 4);
            sums[0] += texel_value & 0x3FF;
            sums[1] += (texel_value >> 10) & 0x3FF;
            sums[2] += (texel_value >> 20) & 0x3FF;
            sums[3] += texel_value >> 30;
          }
        }
        uint32_t n = scale_x * scale_y;
        value = ((sums[0] + n / 2) / n) | (((sums[1] + n / 2) / n) << 10) |
                (((sums[2] + n / 2) / n) << 20) | (((sums[3] + n / 2) / n) << 30);
      } else if (is_32_float) {
        float sum = 0.0f;
        for (uint32_t sy = 0; sy < scale_y; ++sy) {
          for (uint32_t sx = 0; sx < scale_x; ++sx) {
            float texel_value;
            std::memcpy(&texel_value, host + (y * scale_y + sy) * readback.row_pitch +
                                          (x * scale_x + sx) * 4, 4);
            sum += texel_value;
          }
        }
        value = std::bit_cast<uint32_t>(sum / float(scale_x * scale_y));
      } else {
        uint32_t sums[4] = {};
        for (uint32_t sy = 0; sy < scale_y; ++sy) {
          const uint8_t* row = host + (y * scale_y + sy) * readback.row_pitch;
          for (uint32_t sx = 0; sx < scale_x; ++sx) {
            for (uint32_t c = 0; c < 4; ++c) {
              sums[c] += row[(x * scale_x + sx) * 4 + c];
            }
          }
        }
        uint32_t n = scale_x * scale_y;
        value = 0;
        for (uint32_t c = 0; c < 4; ++c) {
          value |= ((sums[c] + n / 2) / n) << (8 * c);
        }
      }
      int32_t offset =
          texture_util::GetTiledOffset2D(int32_t(readback.dest_x / scale_x + x),
                                         int32_t(readback.dest_y / scale_y + y), pitch, 2) -
          int32_t(start);
      if (offset < 0 || uint32_t(offset) + 4 > info.copy_dest_extent_length) {
        continue;
      }
      value = xenos::GpuSwap(value, endian);
      std::memcpy(guest + offset, &value, 4);
    }
  }
}

}  // namespace rex::graphics::webgpu
