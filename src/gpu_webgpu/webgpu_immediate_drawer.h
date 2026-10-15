// rexgpu-webgpu - immediate drawer (ImGui and other UI) for the presenter.

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include <webgpu/webgpu_cpp.h>

#include <rex/ui/immediate_drawer.h>

namespace rex::ui::webgpu {

class WebGpuProvider;

class WebGpuImmediateDrawer final : public ImmediateDrawer {
 public:
  static std::unique_ptr<WebGpuImmediateDrawer> Create(WebGpuProvider& provider);
  ~WebGpuImmediateDrawer() override;

  std::unique_ptr<ImmediateTexture> CreateTexture(uint32_t width, uint32_t height,
                                                  ImmediateTextureFilter filter, bool is_repeated,
                                                  const uint8_t* data) override;

  void Begin(UIDrawContext& ui_draw_context, float coordinate_space_width,
             float coordinate_space_height) override;
  void BeginDrawBatch(const ImmediateDrawBatch& batch) override;
  void Draw(const ImmediateDraw& draw) override;
  void EndDrawBatch() override;
  void End() override;

 private:
  // Vertex layout of the pipeline: position already in clip space.
  struct Vertex {
    float x, y;
    float u, v;
    uint32_t color;
  };

  explicit WebGpuImmediateDrawer(WebGpuProvider& provider) : provider_(provider) {}
  bool Initialize();
  bool EnsurePipelines(wgpu::TextureFormat format);
  // Appends data to the per-paint buffer (growing it if needed) and returns
  // the byte offset, or UINT64_MAX on failure.
  uint64_t Append(const void* data, uint64_t size);
  wgpu::BindGroup CreateTextureBindGroup(const wgpu::TextureView& view,
                                         ImmediateTextureFilter filter, bool is_repeated);

  WebGpuProvider& provider_;

  wgpu::ShaderModule shader_;
  wgpu::BindGroupLayout bind_group_layout_;
  wgpu::PipelineLayout pipeline_layout_;
  // [filter][repeat]
  std::array<std::array<wgpu::Sampler, 2>, 2> samplers_;
  wgpu::TextureFormat pipeline_format_ = wgpu::TextureFormat::Undefined;
  wgpu::RenderPipeline pipeline_triangles_;
  wgpu::RenderPipeline pipeline_lines_;
  wgpu::BindGroup white_bind_group_;

  // Vertex and index data of the current paint.
  wgpu::Buffer buffer_;
  uint64_t buffer_size_ = 0;
  uint64_t buffer_used_ = 0;
  uint64_t buffer_paint_serial_ = UINT64_MAX;
  std::vector<Vertex> vertex_scratch_;

  // State between Begin and End.
  const wgpu::RenderPassEncoder* pass_ = nullptr;
  bool batch_open_ = false;
  bool batch_indexed_ = false;
  const wgpu::RenderPipeline* current_pipeline_ = nullptr;
  const wgpu::BindGroup* current_bind_group_ = nullptr;
  std::array<uint32_t, 4> current_scissor_ = {};
};

}  // namespace rex::ui::webgpu
