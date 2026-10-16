// rexgpu-webgpu - shader and pipeline caches.

#include "pipeline_cache.h"

#include <array>
#include <chrono>
#include <cstring>
#include <mutex>

#include <fmt/format.h>
#include <xxhash.h>

#include <rex/cvar.h>
#include <rex/graphics/registers.h>
#include <rex/logging.h>

#include "pipeline_state.h"
#include "webgpu_provider.h"

REXCVAR_DEFINE_BOOL(webgpu_untextured, false, "GPU/WebGPU",
                    "Debugging: texture fetches return white (geometry checks before texturing)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(webgpu_dump_shaders, "", "GPU/WebGPU",
                      "Folder to write every translated WGSL module to (debugging; the output "
                      "is derived from the game and must not be shared)");

namespace rex::graphics::webgpu {

namespace {
// Vertex shader resources are also read by their memory export compute variant.
constexpr wgpu::ShaderStage kVertexStages = wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Compute;
}  // namespace

namespace {

constexpr char kNullPixelShader[] = "@fragment fn main() {}\n";

}  // namespace

TextureLayoutSignature TextureLayoutSignature::Get(const Shader& vertex_shader,
                                                   const Shader* pixel_shader) {
  TextureLayoutSignature signature;
  const auto& vertex_slots = WgslShaderTranslator::GetTextureSlots(vertex_shader);
  signature.vertex_count = uint32_t(vertex_slots.size());
  for (uint32_t i = 0; i < vertex_slots.size(); ++i) {
    signature.vertex_cube_mask |= uint32_t(vertex_slots[i].is_cube) << i;
  }
  if (pixel_shader) {
    const auto& pixel_slots = WgslShaderTranslator::GetTextureSlots(*pixel_shader);
    signature.pixel_count = uint32_t(pixel_slots.size());
    for (uint32_t i = 0; i < pixel_slots.size(); ++i) {
      signature.pixel_cube_mask |= uint32_t(pixel_slots[i].is_cube) << i;
    }
  }
  return signature;
}

const PipelineCache::TextureLayouts& PipelineCache::GetTextureLayouts(
    TextureLayoutSignature signature) {
  auto it = texture_layouts_.find(signature.value);
  if (it != texture_layouts_.end()) {
    return it->second;
  }
  std::vector<wgpu::BindGroupLayoutEntry> entries;
  auto add_stage = [&entries](uint32_t count, uint32_t cube_mask, uint32_t base,
                              wgpu::ShaderStage stage) {
    for (uint32_t i = 0; i < count; ++i) {
      wgpu::BindGroupLayoutEntry texture{};
      texture.binding = base + i;
      texture.visibility = stage;
      texture.texture.sampleType = wgpu::TextureSampleType::Float;
      texture.texture.viewDimension = (cube_mask & (1u << i)) ? wgpu::TextureViewDimension::Cube
                                                              : wgpu::TextureViewDimension::e2D;
      entries.push_back(texture);
      wgpu::BindGroupLayoutEntry sampler{};
      sampler.binding = base + i + wgsl_binding::kSamplerOffset;
      sampler.visibility = stage;
      sampler.sampler.type = wgpu::SamplerBindingType::Filtering;
      entries.push_back(sampler);
    }
  };
  add_stage(signature.pixel_count, signature.pixel_cube_mask, 0, wgpu::ShaderStage::Fragment);
  add_stage(signature.vertex_count, signature.vertex_cube_mask, wgsl_binding::kVertexTextureBase,
            kVertexStages);
  TextureLayouts layouts;
  wgpu::BindGroupLayoutDescriptor desc;
  desc.label = "textures";
  desc.entryCount = entries.size();
  desc.entries = entries.data();
  layouts.bind_group_layout = provider_.device().CreateBindGroupLayout(&desc);
  std::array<wgpu::BindGroupLayout, 3> groups = {draw_layout_, shared_memory_layout_,
                                                  layouts.bind_group_layout};
  wgpu::PipelineLayoutDescriptor layout_desc{};
  layout_desc.label = "guest draw";
  layout_desc.bindGroupLayoutCount = groups.size();
  layout_desc.bindGroupLayouts = groups.data();
  layouts.pipeline_layout = provider_.device().CreatePipelineLayout(&layout_desc);
  groups[1] = shared_memory_rw_layout_;
  layout_desc.label = "guest memory export";
  layouts.memexport_pipeline_layout = provider_.device().CreatePipelineLayout(&layout_desc);
  return texture_layouts_.emplace(signature.value, std::move(layouts)).first->second;
}

const wgpu::BindGroupLayout& PipelineCache::GetTextureBindGroupLayout(
    TextureLayoutSignature signature) {
  return GetTextureLayouts(signature).bind_group_layout;
}

bool PipelineKey::operator==(const PipelineKey& other) const {
  return std::memcmp(this, &other, sizeof(*this)) == 0;
}

size_t PipelineKeyHash::operator()(const PipelineKey& key) const {
  return size_t(XXH3_64bits(&key, sizeof(key)));
}

PipelineCache::PipelineCache(ui::webgpu::WebGpuProvider& provider) : provider_(provider) {}

PipelineCache::~PipelineCache() {
  std::lock_guard lock(provider_.device_mutex());
  last_pipeline_ = nullptr;
  pipelines_.clear();
  modules_.clear();
}

bool PipelineCache::Initialize() {
  std::lock_guard lock(provider_.device_mutex());
  translator_.set_untextured_fetch_value(REXCVAR_GET(webgpu_untextured) ? 1.0f : 0.0f);
  const wgpu::Device& device = provider_.device();

  // Group 0: per-draw uniforms with dynamic offsets (7.6.5).
  struct UniformEntry {
    uint32_t binding;
    wgpu::ShaderStage visibility;
    uint64_t size;
  };
  static constexpr UniformEntry kUniforms[] = {
      {wgsl_binding::kSystemConstants, kVertexStages | wgpu::ShaderStage::Fragment,
       sizeof(WgslSystemConstants)},
      {wgsl_binding::kVertexFloatConstants, kVertexStages, 4096},
      {wgsl_binding::kPixelFloatConstants, wgpu::ShaderStage::Fragment, 4096},
      {wgsl_binding::kBoolLoopConstants, kVertexStages | wgpu::ShaderStage::Fragment, 160},
      {wgsl_binding::kFetchConstants, kVertexStages | wgpu::ShaderStage::Fragment, 768},
  };
  std::array<wgpu::BindGroupLayoutEntry, 5> draw_entries{};
  for (size_t i = 0; i < draw_entries.size(); ++i) {
    draw_entries[i].binding = kUniforms[i].binding;
    draw_entries[i].visibility = kUniforms[i].visibility;
    draw_entries[i].buffer.type = wgpu::BufferBindingType::Uniform;
    draw_entries[i].buffer.hasDynamicOffset = true;
    draw_entries[i].buffer.minBindingSize = kUniforms[i].size;
  }
  wgpu::BindGroupLayoutDescriptor draw_desc;
  draw_desc.label = "draw uniforms";
  draw_desc.entryCount = draw_entries.size();
  draw_desc.entries = draw_entries.data();
  draw_layout_ = device.CreateBindGroupLayout(&draw_desc);

  // Group 1: guest memory for vertex pulling.
  wgpu::BindGroupLayoutEntry shared_entry{};
  shared_entry.binding = wgsl_binding::kSharedMemory;
  shared_entry.visibility = wgpu::ShaderStage::Vertex;
  shared_entry.buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
  wgpu::BindGroupLayoutDescriptor shared_desc;
  shared_desc.label = "shared memory";
  shared_desc.entryCount = 1;
  shared_desc.entries = &shared_entry;
  shared_memory_layout_ = device.CreateBindGroupLayout(&shared_desc);
  // Writable for the memory export compute variant of vertex shaders.
  shared_entry.visibility = wgpu::ShaderStage::Compute;
  shared_entry.buffer.type = wgpu::BufferBindingType::Storage;
  shared_desc.label = "shared memory (writable)";
  shared_memory_rw_layout_ = device.CreateBindGroupLayout(&shared_desc);

  // Group 2: textures (L-04d); empty until texture fetch is translated.
  wgpu::BindGroupLayoutDescriptor texture_desc;
  texture_desc.label = "textures (none)";
  texture_layout_ = device.CreateBindGroupLayout(&texture_desc);
  wgpu::BindGroupDescriptor empty_desc;
  empty_desc.layout = texture_layout_;
  empty_texture_group_ = device.CreateBindGroup(&empty_desc);

  // Shaders without textures share the empty group 2 layout.
  texture_layouts_[0] = {texture_layout_, nullptr};
  std::array<wgpu::BindGroupLayout, 3> layouts = {draw_layout_, shared_memory_layout_,
                                                   texture_layout_};
  wgpu::PipelineLayoutDescriptor layout_desc{};
  layout_desc.label = "guest draw";
  layout_desc.bindGroupLayoutCount = layouts.size();
  layout_desc.bindGroupLayouts = layouts.data();
  texture_layouts_[0].pipeline_layout = device.CreatePipelineLayout(&layout_desc);
  layouts[1] = shared_memory_rw_layout_;
  texture_layouts_[0].memexport_pipeline_layout = device.CreatePipelineLayout(&layout_desc);

  wgpu::ShaderSourceWGSL wgsl;
  wgsl.code = kNullPixelShader;
  wgpu::ShaderModuleDescriptor module_desc;
  module_desc.nextInChain = &wgsl;
  module_desc.label = "null pixel shader";
  null_pixel_shader_ = device.CreateShaderModule(&module_desc);
  return bool(texture_layouts_[0].pipeline_layout);
}

Shader* PipelineCache::LoadShader(xenos::ShaderType type, const uint32_t* ucode_dwords,
                                  uint32_t dword_count) {
  uint64_t hash = XXH3_64bits(ucode_dwords, size_t(dword_count) * sizeof(uint32_t));
  auto it = shaders_.find(hash);
  if (it != shaders_.end()) {
    return it->second.get();
  }
  auto shader = std::make_unique<Shader>(type, hash, ucode_dwords, dword_count);
  shader->AnalyzeUcode(disassembly_buffer_);
  Shader* result = shader.get();
  shaders_.emplace(hash, std::move(shader));
  return result;
}

wgpu::ShaderModule PipelineCache::GetShaderModule(Shader& shader, uint64_t modification) {
  ModuleKey key{shader.ucode_data_hash(), modification};
  auto it = modules_.find(key);
  if (it != modules_.end()) {
    return it->second;
  }
  const auto start = std::chrono::steady_clock::now();
  Shader::Translation* translation = shader.GetOrCreateTranslation(modification);
  if (!translation->is_translated()) {
    translator_.TranslateAnalyzedShader(*translation);
  }
  wgpu::ShaderModule module;
  if (translation->is_valid()) {
    std::string wgsl = translation->GetTranslatedBinaryString();
    const std::string& dump_dir = REXCVAR_GET(webgpu_dump_shaders);
    if (!dump_dir.empty()) {
      translation->Dump(dump_dir, "webgpu");
    }
    wgpu::ShaderSourceWGSL source;
    source.code = wgsl.c_str();
    wgpu::ShaderModuleDescriptor desc;
    std::string label = fmt::format("{:016X}.{}.{:X}", shader.ucode_data_hash(),
                                    shader.type() == xenos::ShaderType::kVertex ? "vs" : "ps",
                                    modification);
    desc.label = label.c_str();
    desc.nextInChain = &source;
    module = provider_.device().CreateShaderModule(&desc);
  } else {
    REXGPU_ERROR("WebGPU: failed to translate shader {:016X}", shader.ucode_data_hash());
  }
  modules_.emplace(key, module);
  ++creation_stats_.modules;
  creation_stats_.milliseconds +=
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
  return module;
}

wgpu::ComputePipeline PipelineCache::GetMemExportPipeline(Shader& vertex_shader,
                                                          uint64_t modification,
                                                          TextureLayoutSignature signature) {
  ModuleKey key{vertex_shader.ucode_data_hash() ^ signature.value, modification};
  auto it = memexport_pipelines_.find(key);
  if (it != memexport_pipelines_.end()) {
    return it->second;
  }
  wgpu::ComputePipeline pipeline;
  if (wgpu::ShaderModule module = GetShaderModule(vertex_shader, modification)) {
    wgpu::ComputePipelineDescriptor desc;
    desc.label = "guest memory export";
    desc.layout = GetTextureLayouts(signature).memexport_pipeline_layout;
    desc.compute.module = module;
    desc.compute.entryPoint = "main";
    pipeline = provider_.device().CreateComputePipeline(&desc);
  }
  memexport_pipelines_.emplace(key, pipeline);
  return pipeline;
}

wgpu::RenderPipeline PipelineCache::GetPipeline(const PipelineKey& key, Shader& vertex_shader,
                                                Shader* pixel_shader) {
  if (last_pipeline_ && last_key_ == key) {
    return last_pipeline_;
  }
  auto it = pipelines_.find(key);
  if (it != pipelines_.end()) {
    last_key_ = key;
    last_pipeline_ = it->second;
    return it->second;
  }
  wgpu::ShaderModule vertex_module =
      GetShaderModule(vertex_shader, key.vertex_shader_modification);
  wgpu::ShaderModule pixel_module =
      pixel_shader ? GetShaderModule(*pixel_shader, key.pixel_shader_modification)
                   : null_pixel_shader_;
  wgpu::RenderPipeline pipeline;
  if (vertex_module && pixel_module) {
    std::array<wgpu::ColorTargetState, 4> targets;
    std::array<wgpu::BlendState, 4> blends;
    uint32_t target_count = 0;
    for (uint32_t i = 0; i < 4; ++i) {
      if (wgpu::TextureFormat(key.color_formats[i]) != wgpu::TextureFormat::Undefined) {
        target_count = i + 1;
      }
    }
    for (uint32_t i = 0; i < target_count; ++i) {
      wgpu::ColorTargetState& target = targets[i];
      target.format = wgpu::TextureFormat(key.color_formats[i]);
      if (target.format == wgpu::TextureFormat::Undefined) {
        continue;
      }
      target.writeMask = wgpu::ColorWriteMask(key.color_write_masks[i]);
      if (key.blend_controls[i] != UINT32_MAX) {
        reg::RB_BLENDCONTROL blend_control;
        blend_control.value = key.blend_controls[i];
        if (TranslateBlendControl(blend_control, blends[i])) {
          target.blend = &blends[i];
        }
      }
    }
    wgpu::FragmentState fragment{};
    fragment.module = pixel_module;
    fragment.entryPoint = "main";
    fragment.targetCount = target_count;
    fragment.targets = targets.data();

    wgpu::DepthStencilState depth_stencil;
    if (wgpu::TextureFormat(key.depth_stencil_format) != wgpu::TextureFormat::Undefined) {
      reg::RB_DEPTHCONTROL depth_control;
      depth_control.value = key.depth_control;
      reg::RB_STENCILREFMASK stencil_mask;
      stencil_mask.stencilmask = key.stencil_read_mask;
      stencil_mask.stencilwritemask = key.stencil_write_mask;
      TranslateDepthStencilState(depth_control, stencil_mask,
                                 wgpu::TextureFormat(key.depth_stencil_format),
                                 key.depth_bias, key.depth_bias_slope_scale, depth_stencil);
    }

    wgpu::RenderPipelineDescriptor desc;
    std::string label = fmt::format("pipeline VS:{:016X} PS:{:016X} (topo:{})",
                                    key.vertex_shader_hash, key.pixel_shader_hash,
                                    key.topology);
    desc.label = label.c_str();
    TextureLayoutSignature signature;
    signature.value = key.texture_layout;
    desc.layout = GetTextureLayouts(signature).pipeline_layout;
    desc.vertex.module = vertex_module;
    desc.vertex.entryPoint = "main";
    desc.primitive.topology = wgpu::PrimitiveTopology(key.topology);
    desc.primitive.stripIndexFormat = wgpu::IndexFormat(key.strip_index_format);
    desc.primitive.cullMode = wgpu::CullMode(key.cull_mode);
    desc.primitive.frontFace = wgpu::FrontFace(key.front_face);
    desc.primitive.unclippedDepth =
        key.unclipped_depth && provider_.HasFeature(wgpu::FeatureName::DepthClipControl);
    if (depth_stencil.format != wgpu::TextureFormat::Undefined) {
      desc.depthStencil = &depth_stencil;
    }
    desc.multisample.count = key.sample_count;
    desc.fragment = &fragment;
    const auto start = std::chrono::steady_clock::now();
    pipeline = provider_.device().CreateRenderPipeline(&desc);
    creation_stats_.milliseconds +=
        std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
            .count();
  }
  pipelines_.emplace(key, pipeline);
  ++creation_stats_.pipelines;
  last_key_ = key;
  last_pipeline_ = pipeline;
  return pipeline;
}

}  // namespace rex::graphics::webgpu
