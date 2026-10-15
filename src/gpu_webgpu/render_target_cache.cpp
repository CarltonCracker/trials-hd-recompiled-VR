// rexgpu-webgpu - render-target cache.

#include "render_target_cache.h"

#include <algorithm>

#include <fmt/format.h>

#include <rex/graphics/registers.h>
#include <rex/logging.h>

#include "format_tables.h"
#include "frame_recorder.h"
#include "gpu_profiler.h"
#include "webgpu_provider.h"
#include "wgsl/transfer.wgsl.h"
#include "wgsl/wgsl_prelude.wgsl.h"

namespace rex::graphics::webgpu {

namespace {

struct TransferTarget {
  uint32_t base_tiles;
  uint32_t pitch_tiles;
  uint32_t msaa_4x;
  uint32_t is_depth;
  uint32_t format;
  float output_scale;
  uint32_t scale_x;
  uint32_t scale_y;
  uint32_t tile_first;
  uint32_t tile_count;
  uint32_t row_first;
  uint32_t row_end;
  uint32_t width;
  uint32_t padding[3];
};

struct TransferConstants {
  TransferTarget dest;
  TransferTarget source;
};
static_assert(sizeof(TransferConstants) == 128);

enum SourceType : uint32_t { kColor, kColorMultisample, kDepth, kDepthMultisample };

}  // namespace

RenderTargetCache::RenderTargetCache(ui::webgpu::WebGpuProvider& provider,
                                     FrameRecorder& frame_recorder, uint32_t scale_x,
                                     uint32_t scale_y)
    : provider_(provider), frame_recorder_(frame_recorder), scale_x_(scale_x), scale_y_(scale_y) {}

RenderTargetCache::~RenderTargetCache() {
  std::lock_guard lock(provider_.device_mutex());
  targets_.clear();
}

wgpu::TextureFormat RenderTargetCache::GetColorHostFormat(xenos::ColorRenderTargetFormat format,
                                                          float* output_scale_out) const {
  auto features = format_tables::DeviceFeatures::FromProvider(provider_);
  auto info = format_tables::GetColorRenderTargetFormatInfo(format, features);
  if (output_scale_out) {
    *output_scale_out = info.output_scale;
  }
  return info.host_format;
}

wgpu::TextureFormat RenderTargetCache::GetDepthHostFormat(
    xenos::DepthRenderTargetFormat format) const {
  auto features = format_tables::DeviceFeatures::FromProvider(provider_);
  auto info = format_tables::GetDepthRenderTargetFormatInfo(format, features);
  return info.host_format;
}

bool RenderTargetCache::HostDepthFormatHasStencil(wgpu::TextureFormat format) {
  return format_tables::HostDepthFormatHasStencil(format);
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
  target->pitch_tiles = pitch_tiles;
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
  if (key.is_depth) {
    wgpu::TextureViewDescriptor depth_view;
    depth_view.aspect = wgpu::TextureAspect::DepthOnly;
    target->depth_only_view = target->texture.CreateView(&depth_view);
    if (HostDepthFormatHasStencil(target->host_format)) {
      depth_view.aspect = wgpu::TextureAspect::StencilOnly;
      target->stencil_only_view = target->texture.CreateView(&depth_view);
    }
  }
  REXGPU_DEBUG("WebGPU: created {} ({}x{})", label, target->width, target->height);
  RenderTarget* result = target.get();
  targets_.emplace(key.value, std::move(target));
  return result;
}

bool RenderTargetCache::Update(const RegisterFile& regs, uint32_t color_targets_used,
                               bool depth_stencil_used, uint32_t rows, Binding& binding_out) {
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

  for (RenderTarget* target : binding_out.color) {
    if (target) {
      Acquire(target, rows, &binding_out);
    }
  }
  if (binding_out.depth) {
    Acquire(binding_out.depth, rows, &binding_out);
  }
  return true;
}

RenderTarget* RenderTargetCache::GetForResolve(const RegisterFile& regs,
                                               const draw_util::ResolveInfo& info,
                                               bool is_depth) {
  auto surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  if (!surface_info.surface_pitch) {
    return nullptr;
  }
  const draw_util::ResolveEdramInfo& edram = is_depth ? info.depth_edram_info
                                                      : info.color_edram_info;
  RenderTargetKey key;
  key.base_tiles = is_depth ? info.depth_original_base : info.color_original_base;
  key.pitch_pixels = surface_info.surface_pitch;
  key.format = edram.format;
  key.is_depth = is_depth;
  key.msaa_samples = uint32_t(edram.msaa_samples);
  RenderTarget* target = GetOrCreate(key);
  uint32_t delta = (edram.base_tiles - key.base_tiles) & (xenos::kEdramTileCount - 1);
  uint32_t samples_y = edram.msaa_samples == xenos::MsaaSamples::k1X ? 1 : 2;
  uint32_t rows = (delta / std::max(uint32_t(edram.pitch_tiles), uint32_t(1)) + 1) *
                      xenos::kEdramTileHeightSamples / samples_y +
                  info.coordinate_info.edram_offset_y_div_8 * 8 + info.height_div_8 * 8;
  Acquire(target, rows, nullptr);
  return target;
}

void RenderTargetCache::Acquire(RenderTarget* target, uint32_t rows, const Binding* bound) {
  uint32_t samples_y = target->key.msaa_samples == uint32_t(xenos::MsaaSamples::k1X) ? 1 : 2;
  uint32_t tile_rows = (rows * samples_y + xenos::kEdramTileHeightSamples - 1) /
                       xenos::kEdramTileHeightSamples;
  uint32_t tile_count = std::min(tile_rows * target->pitch_tiles, xenos::kEdramTileCount);
  if (target->owned_epoch == ownership_epoch_ && tile_count <= target->owned_tiles) {
    return;  // Nothing changed owner since this target took these tiles.
  }
  bool changed = false;
  auto is_bound = [bound](const RenderTarget* other) {
    return bound && (other == bound->depth ||
                     std::find(bound->color.begin(), bound->color.end(), other) !=
                         bound->color.end());
  };
  // Transfer each run of tiles with the same previous owner.
  uint32_t run_first = 0;
  RenderTarget* run_owner = nullptr;
  for (uint32_t i = 0; i <= tile_count; ++i) {
    RenderTarget* owner = nullptr;
    if (i < tile_count) {
      RenderTarget*& slot = owners_[(target->key.base_tiles + i) % xenos::kEdramTileCount];
      owner = slot != target && !is_bound(slot) ? slot : nullptr;
      changed |= slot != target;
      slot = target;
    }
    if (owner == run_owner) {
      continue;
    }
    if (run_owner) {
      Transfer(run_owner, target, run_first, i - run_first);
    }
    run_owner = owner;
    run_first = i;
  }
  if (changed) {
    ++ownership_epoch_;
  }
  target->owned_epoch = ownership_epoch_;
  target->owned_tiles = tile_count;
}

bool RenderTargetCache::InitializeTransfer() {
  if (transfer_module_) {
    return true;
  }
  const wgpu::Device& device = provider_.device();
  std::string code = std::string(kWgsl_wgsl_prelude) + kWgsl_transfer;
  wgpu::ShaderSourceWGSL wgsl;
  wgsl.code = code.c_str();
  wgpu::ShaderModuleDescriptor module_desc;
  module_desc.nextInChain = &wgsl;
  module_desc.label = "transfer.wgsl";
  transfer_module_ = device.CreateShaderModule(&module_desc);
  // One layout per source type: constants (dynamic offset), the source view,
  // and the stencil view for depth.
  for (uint32_t i = 0; i < 4; ++i) {
    bool depth = i >= kDepth, multisampled = i & 1;
    std::array<wgpu::BindGroupLayoutEntry, 3> entries;
    entries[0].binding = 0;
    entries[0].visibility = wgpu::ShaderStage::Fragment;
    entries[0].buffer.type = wgpu::BufferBindingType::Uniform;
    entries[0].buffer.hasDynamicOffset = true;
    entries[0].buffer.minBindingSize = sizeof(TransferConstants);
    entries[1].binding = 1 + i;
    entries[1].visibility = wgpu::ShaderStage::Fragment;
    entries[1].texture.sampleType =
        depth ? wgpu::TextureSampleType::Depth : wgpu::TextureSampleType::UnfilterableFloat;
    entries[1].texture.multisampled = multisampled;
    entries[2].binding = 5 + (multisampled ? 1 : 0);
    entries[2].visibility = wgpu::ShaderStage::Fragment;
    entries[2].texture.sampleType = wgpu::TextureSampleType::Uint;
    entries[2].texture.multisampled = multisampled;
    wgpu::BindGroupLayoutDescriptor layout_desc;
    layout_desc.label = "EDRAM transfer";
    layout_desc.entryCount = depth ? 3 : 2;
    layout_desc.entries = entries.data();
    transfer_bind_group_layouts_[i] = device.CreateBindGroupLayout(&layout_desc);
    wgpu::PipelineLayoutDescriptor pipeline_layout_desc;
    pipeline_layout_desc.bindGroupLayoutCount = 1;
    pipeline_layout_desc.bindGroupLayouts = &transfer_bind_group_layouts_[i];
    transfer_pipeline_layouts_[i] = device.CreatePipelineLayout(&pipeline_layout_desc);
  }
  return bool(transfer_module_);
}

void RenderTargetCache::Transfer(RenderTarget* source, RenderTarget* dest, uint32_t tile_first,
                                 uint32_t tile_count) {
  auto is_64bpp = [](const RenderTarget* target) {
    return !target->key.is_depth &&
           xenos::IsColorRenderTargetFormat64bpp(xenos::ColorRenderTargetFormat(target->key.format));
  };
  // shortcut: 64bpp targets (unused by Trials) are not transferred.
  if (is_64bpp(source) || is_64bpp(dest) || !InitializeTransfer()) {
    return;
  }
  // The same tiles, relative to the source's base.
  uint32_t source_first = (dest->key.base_tiles + tile_first - source->key.base_tiles) %
                          xenos::kEdramTileCount;
  REXGPU_DEBUG("WebGPU: EDRAM transfer of {} tiles from 0x{:X} to 0x{:X}", tile_count,
               source->key.value, dest->key.value);
  const wgpu::Device& device = provider_.device();
  auto make_target = [&](const RenderTarget* target, uint32_t first) {
    TransferTarget constants = {};
    constants.base_tiles = target->key.base_tiles;
    constants.pitch_tiles = target->pitch_tiles;
    constants.msaa_4x = target->sample_count > 1;
    constants.is_depth = target->key.is_depth;
    constants.format = target->key.format;
    constants.output_scale = target->format_output_scale;
    constants.scale_x = scale_x_;
    constants.scale_y = scale_y_;
    constants.tile_first = first;
    constants.tile_count = tile_count;
    uint32_t samples_y = target->sample_count > 1 ? 2 : 1;
    uint32_t tile_height = xenos::kEdramTileHeightSamples * scale_y_ / samples_y;
    constants.row_first = first / target->pitch_tiles * tile_height;
    constants.row_end = std::min((first + tile_count + target->pitch_tiles - 1) /
                                     target->pitch_tiles * tile_height,
                                 target->height);
    constants.width = target->width;
    return constants;
  };
  TransferConstants constants;
  constants.dest = make_target(dest, tile_first);
  constants.source = make_target(source, source_first);
  if (constants.dest.row_first >= constants.dest.row_end) {
    return;
  }
  frame_recorder_.EndRenderPass();

  uint32_t source_type = (source->key.is_depth ? kDepth : kColor) + (source->sample_count > 1);
  bool dest_stencil = dest->key.is_depth && HostDepthFormatHasStencil(dest->host_format);
  // shortcut: stencil from a color source's low byte is not restored (always
  // 0); Trials's color-to-depth reinterpretations are overwritten anyway.
  bool restore_stencil = dest_stencil && source->key.is_depth && source->stencil_nonzero;
  bool passthrough = source->key.is_depth == dest->key.is_depth &&
                     source->key.format == dest->key.format &&
                     source->host_format == dest->host_format;
  // stencil_bit 8: the color or depth (and stencil = 0) pass; 0...7: one stencil bit.
  auto get_pipeline = [&](uint32_t stencil_bit) -> const wgpu::RenderPipeline& {
    uint64_t key = (uint64_t(passthrough) << 56) | (uint64_t(stencil_bit) << 48) |
                   (uint64_t(source_type) << 40) | (uint64_t(dest->host_format) << 8) |
                   dest->sample_count;
    auto it = transfer_pipelines_.find(key);
    if (it != transfer_pipelines_.end()) {
      return it->second;
    }
    static constexpr const char* kSourceNames[] = {"color", "color_ms", "depth", "depth_ms"};
    std::string entry_point =
        stencil_bit < 8 ? fmt::format("fs_stencil_bit_from_{}", kSourceNames[source_type])
                        : fmt::format("fs_{}_from_{}", dest->key.is_depth ? "depth" : "color",
                                      kSourceNames[source_type]);
    wgpu::ColorTargetState color_target;
    color_target.format = dest->host_format;
    wgpu::FragmentState fragment;
    fragment.module = transfer_module_;
    fragment.entryPoint = entry_point.c_str();
    std::array<wgpu::ConstantEntry, 2> pipeline_constants;
    pipeline_constants[0].key = "stencil_bit";
    pipeline_constants[0].value = double(stencil_bit < 8 ? stencil_bit : 0);
    pipeline_constants[1].key = "passthrough";
    pipeline_constants[1].value = passthrough ? 1.0 : 0.0;
    // Each entry point uses one of the two.
    fragment.constantCount = 1;
    fragment.constants = &pipeline_constants[stencil_bit < 8 ? 0 : 1];
    wgpu::DepthStencilState depth_stencil;
    wgpu::RenderPipelineDescriptor desc;
    desc.label = "EDRAM transfer";
    desc.layout = transfer_pipeline_layouts_[source_type];
    desc.vertex.module = transfer_module_;
    desc.vertex.entryPoint = "vs_main";
    desc.multisample.count = dest->sample_count;
    if (dest->key.is_depth) {
      depth_stencil.format = dest->host_format;
      depth_stencil.depthCompare = wgpu::CompareFunction::Always;
      if (stencil_bit < 8) {
        depth_stencil.depthWriteEnabled = wgpu::OptionalBool::False;
        depth_stencil.stencilWriteMask = 1u << stencil_bit;
      } else {
        depth_stencil.depthWriteEnabled = wgpu::OptionalBool::True;
        depth_stencil.stencilWriteMask = dest_stencil ? 0xFFu : 0u;
      }
      if (dest_stencil) {
        depth_stencil.stencilFront.compare = wgpu::CompareFunction::Always;
        depth_stencil.stencilFront.passOp = wgpu::StencilOperation::Replace;
        depth_stencil.stencilBack = depth_stencil.stencilFront;
      }
      desc.depthStencil = &depth_stencil;
    } else {
      fragment.targetCount = 1;
      fragment.targets = &color_target;
    }
    desc.fragment = &fragment;
    return transfer_pipelines_.emplace(key, device.CreateRenderPipeline(&desc)).first->second;
  };

  if (!source->transfer_source_group) {
    std::array<wgpu::BindGroupEntry, 3> entries;
    entries[0].binding = 0;
    entries[0].buffer = frame_recorder_.uniform_ring();
    entries[0].size = sizeof(TransferConstants);
    entries[1].binding = 1 + source_type;
    entries[1].textureView = source->key.is_depth ? source->depth_only_view : source->view;
    // shortcut: a depth target without stencil would need its own entry
    // points; all host depth formats here have stencil.
    entries[2].binding = 5 + (source->sample_count > 1 ? 1 : 0);
    entries[2].textureView = source->stencil_only_view;
    wgpu::BindGroupDescriptor group_desc;
    group_desc.layout = transfer_bind_group_layouts_[source_type];
    group_desc.entryCount = source->key.is_depth ? 3 : 2;
    group_desc.entries = entries.data();
    source->transfer_source_group = device.CreateBindGroup(&group_desc);
  }
  uint32_t constants_offset = frame_recorder_.UploadUniforms(&constants, sizeof(constants));

  wgpu::RenderPassColorAttachment color_attachment;
  wgpu::RenderPassDepthStencilAttachment depth_attachment;
  wgpu::RenderPassDescriptor pass_desc;
  pass_desc.label = "EDRAM transfer";
  if (dest->key.is_depth) {
    depth_attachment.view = dest->view;
    depth_attachment.depthLoadOp = dest->initialized ? wgpu::LoadOp::Load : wgpu::LoadOp::Clear;
    depth_attachment.depthStoreOp = wgpu::StoreOp::Store;
    depth_attachment.depthClearValue = 0.0f;
    if (dest_stencil) {
      depth_attachment.stencilLoadOp =
          dest->initialized ? wgpu::LoadOp::Load : wgpu::LoadOp::Clear;
      depth_attachment.stencilStoreOp = wgpu::StoreOp::Store;
    }
    pass_desc.depthStencilAttachment = &depth_attachment;
  } else {
    color_attachment.view = dest->view;
    color_attachment.loadOp = dest->initialized ? wgpu::LoadOp::Load : wgpu::LoadOp::Clear;
    color_attachment.storeOp = wgpu::StoreOp::Store;
    pass_desc.colorAttachmentCount = 1;
    pass_desc.colorAttachments = &color_attachment;
  }
  dest->initialized = true;
  wgpu::PassTimestampWrites timestamp_writes;
  if (gpu_profiler_ && gpu_profiler_->BeginPass(PassCategory::kTransfer, timestamp_writes)) {
    pass_desc.timestampWrites = &timestamp_writes;
  }
  wgpu::RenderPassEncoder pass = frame_recorder_.encoder().BeginRenderPass(&pass_desc);
  pass.SetPipeline(get_pipeline(8));
  pass.SetBindGroup(0, source->transfer_source_group, 1, &constants_offset);
  pass.SetScissorRect(0, constants.dest.row_first, constants.dest.width,
                      constants.dest.row_end - constants.dest.row_first);
  pass.Draw(3);
  if (restore_stencil) {
    dest->stencil_nonzero = true;
    pass.SetStencilReference(0xFF);
    for (uint32_t bit = 0; bit < 8; ++bit) {
      pass.SetPipeline(get_pipeline(bit));
      pass.Draw(3);
    }
  }
  pass.End();
}

}  // namespace rex::graphics::webgpu
