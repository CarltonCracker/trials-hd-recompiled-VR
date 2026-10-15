// rexgpu-webgpu - render-target cache.

#include "render_target_cache.h"

#include <algorithm>

#include <fmt/format.h>

#include <rex/graphics/registers.h>
#include <rex/logging.h>

#include "format_tables.h"
#include "frame_recorder.h"
#include "webgpu_provider.h"
#include "wgsl/transfer.wgsl.h"
#include "wgsl/wgsl_prelude.wgsl.h"

namespace rex::graphics::webgpu {

namespace {

struct TransferConstants {
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
  }
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

  // The scissor bounds the rows the draw can write (EDRAM has no height).
  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor);
  uint32_t rows = scissor.offset[1] + scissor.extent[1];
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
}

bool RenderTargetCache::InitializeTransfer() {
  if (edram_) {
    return true;
  }
  const wgpu::Device& device = provider_.device();
  wgpu::BufferDescriptor buffer_desc;
  buffer_desc.label = "EDRAM transfer";
  buffer_desc.size = uint64_t(xenos::kEdramTileCount) * xenos::kEdramTileWidthSamples *
                     xenos::kEdramTileHeightSamples * 4 * scale_x_ * scale_y_;
  buffer_desc.usage = wgpu::BufferUsage::Storage;
  edram_ = device.CreateBuffer(&buffer_desc);
  std::string code = std::string(kWgsl_wgsl_prelude) + kWgsl_transfer;
  wgpu::ShaderSourceWGSL wgsl;
  wgsl.code = code.c_str();
  wgpu::ShaderModuleDescriptor module_desc;
  module_desc.nextInChain = &wgsl;
  module_desc.label = "transfer.wgsl";
  transfer_module_ = device.CreateShaderModule(&module_desc);
  static constexpr const char* kStoreEntryPoints[] = {"cs_store_color", "cs_store_color_ms",
                                                      "cs_store_depth", "cs_store_depth_ms"};
  for (uint32_t i = 0; i < 4; ++i) {
    wgpu::ComputePipelineDescriptor desc;
    desc.label = kStoreEntryPoints[i];
    desc.compute.module = transfer_module_;
    desc.compute.entryPoint = kStoreEntryPoints[i];
    store_pipelines_[i] = device.CreateComputePipeline(&desc);
  }
  return bool(edram_);
}

void RenderTargetCache::Transfer(RenderTarget* source, RenderTarget* dest, uint32_t tile_first,
                                 uint32_t tile_count) {
  auto is_64bpp = [](const RenderTarget* target) {
    return !target->key.is_depth &&
           xenos::IsColorRenderTargetFormat64bpp(xenos::ColorRenderTargetFormat(target->key.format));
  };
  // shortcut: 64bpp targets (unused by Trials) and stencil are not transferred.
  if (is_64bpp(source) || is_64bpp(dest) || !InitializeTransfer()) {
    return;
  }
  // The same tiles, relative to the source's base.
  uint32_t source_first = (dest->key.base_tiles + tile_first - source->key.base_tiles) %
                          xenos::kEdramTileCount;
  REXGPU_DEBUG("WebGPU: EDRAM transfer of {} tiles from 0x{:X} to 0x{:X}", tile_count,
               source->key.value, dest->key.value);
  const wgpu::Device& device = provider_.device();
  auto make_constants = [&](const RenderTarget* target, uint32_t first) {
    TransferConstants constants = {};
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
  frame_recorder_.EndRenderPass();

  // Store the source's samples.
  TransferConstants store_constants = make_constants(source, source_first);
  if (store_constants.row_first >= store_constants.row_end) {
    return;
  }
  uint32_t store_offset = frame_recorder_.UploadUniforms(&store_constants, sizeof(store_constants));
  uint32_t store_index = (source->key.is_depth ? 2 : 0) + (source->sample_count > 1 ? 1 : 0);
  const wgpu::ComputePipeline& store_pipeline = store_pipelines_[store_index];
  std::array<wgpu::BindGroupEntry, 3> store_entries;
  store_entries[0].binding = 0;
  store_entries[0].buffer = frame_recorder_.uniform_ring();
  store_entries[0].offset = store_offset;
  store_entries[0].size = sizeof(TransferConstants);
  store_entries[1].binding = 1;
  store_entries[1].buffer = edram_;
  store_entries[2].binding = 3 + store_index;
  store_entries[2].textureView = source->key.is_depth ? source->depth_only_view : source->view;
  wgpu::BindGroupDescriptor store_group_desc;
  store_group_desc.layout = store_pipeline.GetBindGroupLayout(0);
  store_group_desc.entryCount = store_entries.size();
  store_group_desc.entries = store_entries.data();
  wgpu::BindGroup store_group = device.CreateBindGroup(&store_group_desc);
  {
    wgpu::ComputePassDescriptor pass_desc;
    pass_desc.label = "EDRAM transfer store";
    wgpu::ComputePassEncoder pass = frame_recorder_.encoder().BeginComputePass(&pass_desc);
    pass.SetPipeline(store_pipeline);
    pass.SetBindGroup(0, store_group);
    pass.DispatchWorkgroups((store_constants.width + 7) / 8,
                            (store_constants.row_end - store_constants.row_first + 7) / 8);
    pass.End();
  }

  // Load them into the destination.
  TransferConstants load_constants = make_constants(dest, tile_first);
  if (load_constants.row_first >= load_constants.row_end) {
    return;
  }
  uint64_t load_key = (uint64_t(dest->host_format) << 8) | dest->sample_count;
  auto pipeline_it = load_pipelines_.find(load_key);
  if (pipeline_it == load_pipelines_.end()) {
    wgpu::ColorTargetState color_target;
    color_target.format = dest->host_format;
    wgpu::FragmentState fragment;
    fragment.module = transfer_module_;
    wgpu::DepthStencilState depth_stencil;
    wgpu::RenderPipelineDescriptor desc;
    desc.label = "EDRAM transfer load";
    desc.vertex.module = transfer_module_;
    desc.vertex.entryPoint = "vs_main";
    desc.multisample.count = dest->sample_count;
    if (dest->key.is_depth) {
      fragment.entryPoint = "fs_load_depth";
      depth_stencil.format = dest->host_format;
      depth_stencil.depthWriteEnabled = wgpu::OptionalBool::True;
      depth_stencil.depthCompare = wgpu::CompareFunction::Always;
      desc.depthStencil = &depth_stencil;
    } else {
      fragment.entryPoint = "fs_load_color";
      fragment.targetCount = 1;
      fragment.targets = &color_target;
    }
    desc.fragment = &fragment;
    pipeline_it = load_pipelines_.emplace(load_key, device.CreateRenderPipeline(&desc)).first;
  }
  const wgpu::RenderPipeline& load_pipeline = pipeline_it->second;
  uint32_t load_offset = frame_recorder_.UploadUniforms(&load_constants, sizeof(load_constants));
  std::array<wgpu::BindGroupEntry, 2> load_entries;
  load_entries[0].binding = 0;
  load_entries[0].buffer = frame_recorder_.uniform_ring();
  load_entries[0].offset = load_offset;
  load_entries[0].size = sizeof(TransferConstants);
  load_entries[1].binding = 2;
  load_entries[1].buffer = edram_;
  wgpu::BindGroupDescriptor load_group_desc;
  load_group_desc.layout = load_pipeline.GetBindGroupLayout(0);
  load_group_desc.entryCount = load_entries.size();
  load_group_desc.entries = load_entries.data();
  wgpu::BindGroup load_group = device.CreateBindGroup(&load_group_desc);

  wgpu::RenderPassColorAttachment color_attachment;
  wgpu::RenderPassDepthStencilAttachment depth_attachment;
  wgpu::RenderPassDescriptor pass_desc;
  pass_desc.label = "EDRAM transfer load";
  if (dest->key.is_depth) {
    depth_attachment.view = dest->view;
    depth_attachment.depthLoadOp = dest->initialized ? wgpu::LoadOp::Load : wgpu::LoadOp::Clear;
    depth_attachment.depthStoreOp = wgpu::StoreOp::Store;
    depth_attachment.depthClearValue = 0.0f;
    if (HostDepthFormatHasStencil(dest->host_format)) {
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
  wgpu::RenderPassEncoder pass = frame_recorder_.encoder().BeginRenderPass(&pass_desc);
  pass.SetPipeline(load_pipeline);
  pass.SetBindGroup(0, load_group);
  pass.SetScissorRect(0, load_constants.row_first, load_constants.width,
                      load_constants.row_end - load_constants.row_first);
  pass.Draw(3);
  pass.End();
}

}  // namespace rex::graphics::webgpu
