// rexgpu-webgpu - shader and pipeline caches (section 8.5) and the fixed
// bind-group layouts of section 7.6.5.

#pragma once

#include <cstdint>
#include <memory>
#include <unordered_map>

#include <webgpu/webgpu_cpp.h>

#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/string/buffer.h>

#include "wgsl/wgsl_shader_translator.h"

namespace rex::ui::webgpu {
class WebGpuProvider;
}  // namespace rex::ui::webgpu

namespace rex::graphics::webgpu {

// Everything a render pipeline depends on. Zero-initialized and compared and
// hashed as bytes, so keep it free of padding surprises (all uint32_t).
struct PipelineKey {
  uint64_t vertex_shader_hash;
  uint64_t vertex_shader_modification;
  uint64_t pixel_shader_hash;  // 0: no pixel shader
  uint64_t pixel_shader_modification;
  uint32_t topology;            // wgpu::PrimitiveTopology
  uint32_t strip_index_format;  // wgpu::IndexFormat
  uint32_t cull_mode;           // wgpu::CullMode
  uint32_t front_face;          // wgpu::FrontFace
  uint32_t unclipped_depth;
  uint32_t sample_count;
  uint32_t color_formats[4];  // wgpu::TextureFormat, Undefined: no attachment
  uint32_t color_write_masks[4];
  uint32_t blend_controls[4];  // RB_BLENDCONTROL, 0xFFFFFFFF: blending disabled
  uint32_t depth_stencil_format;
  uint32_t depth_control;  // normalized RB_DEPTHCONTROL
  uint32_t stencil_read_mask;
  uint32_t stencil_write_mask;
  int32_t depth_bias;
  float depth_bias_slope_scale;

  bool operator==(const PipelineKey& other) const;
};

struct PipelineKeyHash {
  size_t operator()(const PipelineKey& key) const;
};

class PipelineCache {
 public:
  explicit PipelineCache(ui::webgpu::WebGpuProvider& provider);
  ~PipelineCache();

  bool Initialize();

  // Returns the shader for guest ucode (big-endian dwords), analyzed.
  Shader* LoadShader(xenos::ShaderType type, const uint32_t* ucode_dwords, uint32_t dword_count);

  // The WGSL module for a shader and modification, translating on first use.
  // Null if translation failed (logged once).
  wgpu::ShaderModule GetShaderModule(Shader& shader, uint64_t modification);

  // The pipeline for the key, created synchronously on first use.
  wgpu::RenderPipeline GetPipeline(const PipelineKey& key, Shader& vertex_shader,
                                   Shader* pixel_shader);

  const wgpu::BindGroupLayout& draw_bind_group_layout() const { return draw_layout_; }
  const wgpu::BindGroupLayout& shared_memory_bind_group_layout() const {
    return shared_memory_layout_;
  }
  const wgpu::BindGroupLayout& texture_bind_group_layout() const { return texture_layout_; }
  const wgpu::BindGroup& empty_texture_bind_group() const { return empty_texture_group_; }

  size_t pipeline_count() const { return pipelines_.size(); }

 private:
  ui::webgpu::WebGpuProvider& provider_;
  WgslShaderTranslator translator_;
  string::StringBuffer disassembly_buffer_;
  std::unordered_map<uint64_t, std::unique_ptr<Shader>> shaders_;
  struct ModuleKey {
    uint64_t hash;
    uint64_t modification;
    bool operator==(const ModuleKey& other) const {
      return hash == other.hash && modification == other.modification;
    }
  };
  struct ModuleKeyHash {
    size_t operator()(const ModuleKey& key) const {
      return size_t(key.hash ^ (key.modification * 0x9E3779B97F4A7C15ull));
    }
  };
  std::unordered_map<ModuleKey, wgpu::ShaderModule, ModuleKeyHash> modules_;
  std::unordered_map<PipelineKey, wgpu::RenderPipeline, PipelineKeyHash> pipelines_;

  wgpu::BindGroupLayout draw_layout_;
  wgpu::BindGroupLayout shared_memory_layout_;
  wgpu::BindGroupLayout texture_layout_;
  wgpu::BindGroup empty_texture_group_;
  wgpu::PipelineLayout pipeline_layout_;
  wgpu::ShaderModule null_pixel_shader_;
};

}  // namespace rex::graphics::webgpu
