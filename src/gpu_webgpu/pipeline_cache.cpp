// rexgpu-webgpu - shader and pipeline caches.

#include "pipeline_cache.h"

#include <array>
#include <cstring>
#include <mutex>

#include <fmt/format.h>
#include <xxhash.h>

#include <rex/cvar.h>
#include <rex/graphics/registers.h>
#include <rex/logging.h>

#include "webgpu_provider.h"

REXCVAR_DEFINE_BOOL(webgpu_untextured, false, "GPU/WebGPU",
                    "Debugging: texture fetches return white (geometry checks before texturing)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_STRING(webgpu_dump_shaders, "", "GPU/WebGPU",
                      "Folder to write every translated WGSL module to (debugging; the output "
                      "is derived from the game and must not be shared)");

namespace rex::graphics::webgpu {

namespace {

constexpr char kNullPixelShader[] = "@fragment fn main() {}\n";

wgpu::BlendFactor ToBlendFactor(xenos::BlendFactor factor) {
  using F = xenos::BlendFactor;
  switch (factor) {
    case F::kZero:
      return wgpu::BlendFactor::Zero;
    case F::kOne:
      return wgpu::BlendFactor::One;
    case F::kSrcColor:
      return wgpu::BlendFactor::Src;
    case F::kOneMinusSrcColor:
      return wgpu::BlendFactor::OneMinusSrc;
    case F::kSrcAlpha:
      return wgpu::BlendFactor::SrcAlpha;
    case F::kOneMinusSrcAlpha:
      return wgpu::BlendFactor::OneMinusSrcAlpha;
    case F::kDstColor:
      return wgpu::BlendFactor::Dst;
    case F::kOneMinusDstColor:
      return wgpu::BlendFactor::OneMinusDst;
    case F::kDstAlpha:
      return wgpu::BlendFactor::DstAlpha;
    case F::kOneMinusDstAlpha:
      return wgpu::BlendFactor::OneMinusDstAlpha;
    case F::kConstantColor:
    case F::kConstantAlpha:
      return wgpu::BlendFactor::Constant;
    case F::kOneMinusConstantColor:
    case F::kOneMinusConstantAlpha:
      return wgpu::BlendFactor::OneMinusConstant;
    case F::kSrcAlphaSaturate:
      return wgpu::BlendFactor::SrcAlphaSaturated;
    default:
      return wgpu::BlendFactor::One;
  }
}

// Alpha factors must not use color-only variants.
wgpu::BlendFactor ToAlphaBlendFactor(xenos::BlendFactor factor) {
  using F = xenos::BlendFactor;
  switch (factor) {
    case F::kSrcColor:
      return wgpu::BlendFactor::SrcAlpha;
    case F::kOneMinusSrcColor:
      return wgpu::BlendFactor::OneMinusSrcAlpha;
    case F::kDstColor:
      return wgpu::BlendFactor::DstAlpha;
    case F::kOneMinusDstColor:
      return wgpu::BlendFactor::OneMinusDstAlpha;
    case F::kSrcAlphaSaturate:
      return wgpu::BlendFactor::One;
    default:
      return ToBlendFactor(factor);
  }
}

wgpu::BlendOperation ToBlendOperation(xenos::BlendOp op) {
  switch (op) {
    case xenos::BlendOp::kSubtract:
      return wgpu::BlendOperation::Subtract;
    case xenos::BlendOp::kMin:
      return wgpu::BlendOperation::Min;
    case xenos::BlendOp::kMax:
      return wgpu::BlendOperation::Max;
    case xenos::BlendOp::kRevSubtract:
      return wgpu::BlendOperation::ReverseSubtract;
    default:
      return wgpu::BlendOperation::Add;
  }
}

wgpu::CompareFunction ToCompareFunction(xenos::CompareFunction function) {
  static constexpr wgpu::CompareFunction kFunctions[] = {
      wgpu::CompareFunction::Never,        wgpu::CompareFunction::Less,
      wgpu::CompareFunction::Equal,        wgpu::CompareFunction::LessEqual,
      wgpu::CompareFunction::Greater,      wgpu::CompareFunction::NotEqual,
      wgpu::CompareFunction::GreaterEqual, wgpu::CompareFunction::Always,
  };
  return kFunctions[uint32_t(function) & 7];
}

wgpu::StencilOperation ToStencilOperation(xenos::StencilOp op) {
  static constexpr wgpu::StencilOperation kOperations[] = {
      wgpu::StencilOperation::Keep,           wgpu::StencilOperation::Zero,
      wgpu::StencilOperation::Replace,        wgpu::StencilOperation::IncrementClamp,
      wgpu::StencilOperation::DecrementClamp, wgpu::StencilOperation::Invert,
      wgpu::StencilOperation::IncrementWrap,  wgpu::StencilOperation::DecrementWrap,
  };
  return kOperations[uint32_t(op) & 7];
}

}  // namespace

TextureLayoutSignature TextureLayoutSignature::Get(const Shader& vertex_shader,
                                                   const Shader* pixel_shader) {
  TextureLayoutSignature signature;
  auto vertex_slots = WgslShaderTranslator::GetTextureSlots(vertex_shader);
  signature.vertex_count = uint32_t(vertex_slots.size());
  for (uint32_t i = 0; i < vertex_slots.size(); ++i) {
    signature.vertex_cube_mask |= uint32_t(vertex_slots[i].is_cube) << i;
  }
  if (pixel_shader) {
    auto pixel_slots = WgslShaderTranslator::GetTextureSlots(*pixel_shader);
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
      wgpu::BindGroupLayoutEntry texture;
      texture.binding = base + i;
      texture.visibility = stage;
      texture.texture.sampleType = wgpu::TextureSampleType::Float;
      texture.texture.viewDimension = (cube_mask & (1u << i)) ? wgpu::TextureViewDimension::Cube
                                                              : wgpu::TextureViewDimension::e2D;
      entries.push_back(texture);
      wgpu::BindGroupLayoutEntry sampler;
      sampler.binding = base + i + wgsl_binding::kSamplerOffset;
      sampler.visibility = stage;
      sampler.sampler.type = wgpu::SamplerBindingType::Filtering;
      entries.push_back(sampler);
    }
  };
  add_stage(signature.pixel_count, signature.pixel_cube_mask, 0, wgpu::ShaderStage::Fragment);
  add_stage(signature.vertex_count, signature.vertex_cube_mask, wgsl_binding::kVertexTextureBase,
            wgpu::ShaderStage::Vertex);
  TextureLayouts layouts;
  wgpu::BindGroupLayoutDescriptor desc;
  desc.label = "textures";
  desc.entryCount = entries.size();
  desc.entries = entries.data();
  layouts.bind_group_layout = provider_.device().CreateBindGroupLayout(&desc);
  std::array<wgpu::BindGroupLayout, 3> groups = {draw_layout_, shared_memory_layout_,
                                                  layouts.bind_group_layout};
  wgpu::PipelineLayoutDescriptor layout_desc;
  layout_desc.label = "guest draw";
  layout_desc.bindGroupLayoutCount = groups.size();
  layout_desc.bindGroupLayouts = groups.data();
  layouts.pipeline_layout = provider_.device().CreatePipelineLayout(&layout_desc);
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
      {wgsl_binding::kSystemConstants, wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment,
       sizeof(WgslSystemConstants)},
      {wgsl_binding::kVertexFloatConstants, wgpu::ShaderStage::Vertex, 4096},
      {wgsl_binding::kPixelFloatConstants, wgpu::ShaderStage::Fragment, 4096},
      {wgsl_binding::kBoolLoopConstants, wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment,
       160},
      {wgsl_binding::kFetchConstants, wgpu::ShaderStage::Vertex | wgpu::ShaderStage::Fragment,
       768},
  };
  std::array<wgpu::BindGroupLayoutEntry, 5> draw_entries;
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
  wgpu::BindGroupLayoutEntry shared_entry;
  shared_entry.binding = wgsl_binding::kSharedMemory;
  shared_entry.visibility = wgpu::ShaderStage::Vertex;
  shared_entry.buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
  wgpu::BindGroupLayoutDescriptor shared_desc;
  shared_desc.label = "shared memory";
  shared_desc.entryCount = 1;
  shared_desc.entries = &shared_entry;
  shared_memory_layout_ = device.CreateBindGroupLayout(&shared_desc);

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
  wgpu::PipelineLayoutDescriptor layout_desc;
  layout_desc.label = "guest draw";
  layout_desc.bindGroupLayoutCount = layouts.size();
  layout_desc.bindGroupLayouts = layouts.data();
  texture_layouts_[0].pipeline_layout = device.CreatePipelineLayout(&layout_desc);

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
  return module;
}

wgpu::RenderPipeline PipelineCache::GetPipeline(const PipelineKey& key, Shader& vertex_shader,
                                                Shader* pixel_shader) {
  auto it = pipelines_.find(key);
  if (it != pipelines_.end()) {
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
        wgpu::BlendState& blend = blends[i];
        blend.color.srcFactor = ToBlendFactor(blend_control.color_srcblend);
        blend.color.dstFactor = ToBlendFactor(blend_control.color_destblend);
        blend.color.operation = ToBlendOperation(blend_control.color_comb_fcn);
        blend.alpha.srcFactor = ToAlphaBlendFactor(blend_control.alpha_srcblend);
        blend.alpha.dstFactor = ToAlphaBlendFactor(blend_control.alpha_destblend);
        blend.alpha.operation = ToBlendOperation(blend_control.alpha_comb_fcn);
        // Min and max ignore the factors in WebGPU; they must be One.
        for (wgpu::BlendComponent* component : {&blend.color, &blend.alpha}) {
          if (component->operation == wgpu::BlendOperation::Min ||
              component->operation == wgpu::BlendOperation::Max) {
            component->srcFactor = wgpu::BlendFactor::One;
            component->dstFactor = wgpu::BlendFactor::One;
          }
        }
        target.blend = &blend;
      }
    }
    wgpu::FragmentState fragment;
    fragment.module = pixel_module;
    fragment.entryPoint = "main";
    fragment.targetCount = target_count;
    fragment.targets = targets.data();

    wgpu::DepthStencilState depth_stencil;
    if (wgpu::TextureFormat(key.depth_stencil_format) != wgpu::TextureFormat::Undefined) {
      reg::RB_DEPTHCONTROL depth_control;
      depth_control.value = key.depth_control;
      depth_stencil.format = wgpu::TextureFormat(key.depth_stencil_format);
      depth_stencil.depthWriteEnabled =
          depth_control.z_enable && depth_control.z_write_enable ? wgpu::OptionalBool::True
                                                                 : wgpu::OptionalBool::False;
      depth_stencil.depthCompare = depth_control.z_enable
                                       ? ToCompareFunction(depth_control.zfunc)
                                       : wgpu::CompareFunction::Always;
      if (depth_control.stencil_enable) {
        depth_stencil.stencilFront.compare = ToCompareFunction(depth_control.stencilfunc);
        depth_stencil.stencilFront.failOp = ToStencilOperation(depth_control.stencilfail);
        depth_stencil.stencilFront.depthFailOp = ToStencilOperation(depth_control.stencilzfail);
        depth_stencil.stencilFront.passOp = ToStencilOperation(depth_control.stencilzpass);
        if (depth_control.backface_enable) {
          depth_stencil.stencilBack.compare = ToCompareFunction(depth_control.stencilfunc_bf);
          depth_stencil.stencilBack.failOp = ToStencilOperation(depth_control.stencilfail_bf);
          depth_stencil.stencilBack.depthFailOp =
              ToStencilOperation(depth_control.stencilzfail_bf);
          depth_stencil.stencilBack.passOp = ToStencilOperation(depth_control.stencilzpass_bf);
        } else {
          depth_stencil.stencilBack = depth_stencil.stencilFront;
        }
        depth_stencil.stencilReadMask = key.stencil_read_mask;
        depth_stencil.stencilWriteMask = key.stencil_write_mask;
      } else {
        depth_stencil.stencilReadMask = 0;
        depth_stencil.stencilWriteMask = 0;
      }
      depth_stencil.depthBias = key.depth_bias;
      depth_stencil.depthBiasSlopeScale = key.depth_bias_slope_scale;
    }

    wgpu::RenderPipelineDescriptor desc;
    std::string label = fmt::format("{:016X}+{:016X}", key.vertex_shader_hash,
                                    key.pixel_shader_hash);
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
    pipeline = provider_.device().CreateRenderPipeline(&desc);
  }
  pipelines_.emplace(key, pipeline);
  return pipeline;
}

}  // namespace rex::graphics::webgpu
