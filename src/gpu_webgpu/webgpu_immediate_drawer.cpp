// rexgpu-webgpu - immediate drawer.

#include "webgpu_immediate_drawer.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

#include <rex/logging.h>

#include "webgpu_presenter.h"
#include "webgpu_provider.h"
#include "wgsl/immediate.wgsl.h"

namespace rex::ui::webgpu {

namespace {

class WebGpuImmediateTexture final : public ImmediateTexture {
 public:
  WebGpuImmediateTexture(uint32_t width, uint32_t height, WebGpuProvider& provider,
                         wgpu::Texture texture, wgpu::BindGroup bind_group)
      : ImmediateTexture(width, height),
        provider_(provider),
        texture_(std::move(texture)),
        bind_group_(std::move(bind_group)) {}
  ~WebGpuImmediateTexture() override {
#if defined(__EMSCRIPTEN__)
    // UI textures can be freed by the UI thread (closing a dialog).
    if (!provider_.IsOnGpuWorker()) {
      provider_.ReleaseOnGpuWorker(std::move(bind_group_));
      provider_.ReleaseOnGpuWorker(std::move(texture_));
      return;
    }
#endif
    // Releasing the last reference destroys the objects: a WebGPU call.
    std::lock_guard lock(provider_.device_mutex());
    bind_group_ = nullptr;
    texture_ = nullptr;
  }
  const wgpu::BindGroup& bind_group() const { return bind_group_; }

 private:
  WebGpuProvider& provider_;
  wgpu::Texture texture_;
  wgpu::BindGroup bind_group_;
};

constexpr uint64_t kInitialBufferSize = 1 << 20;

}  // namespace

std::unique_ptr<WebGpuImmediateDrawer> WebGpuImmediateDrawer::Create(WebGpuProvider& provider) {
  std::unique_ptr<WebGpuImmediateDrawer> drawer(new WebGpuImmediateDrawer(provider));
  if (!drawer->Initialize()) {
    return nullptr;
  }
  return drawer;
}

WebGpuImmediateDrawer::~WebGpuImmediateDrawer() {
#if defined(__EMSCRIPTEN__)
  // The app (UI thread) owns this drawer on the web.
  if (!provider_.IsOnGpuWorker()) {
    provider_.ReleaseOnGpuWorker(std::move(white_bind_group_));
    provider_.ReleaseOnGpuWorker(std::move(pipeline_triangles_));
    provider_.ReleaseOnGpuWorker(std::move(pipeline_lines_));
    provider_.ReleaseOnGpuWorker(std::move(buffer_));
    provider_.ReleaseOnGpuWorker(std::move(samplers_));
    provider_.ReleaseOnGpuWorker(std::move(pipeline_layout_));
    provider_.ReleaseOnGpuWorker(std::move(bind_group_layout_));
    provider_.ReleaseOnGpuWorker(std::move(shader_));
    return;
  }
#endif
  std::lock_guard lock(provider_.device_mutex());
  white_bind_group_ = nullptr;
  pipeline_triangles_ = nullptr;
  pipeline_lines_ = nullptr;
  buffer_ = nullptr;
  samplers_ = {};
  pipeline_layout_ = nullptr;
  bind_group_layout_ = nullptr;
  shader_ = nullptr;
}

bool WebGpuImmediateDrawer::Initialize() {
  std::lock_guard lock(provider_.device_mutex());
  const wgpu::Device& device = provider_.device();

  wgpu::ShaderSourceWGSL wgsl;
  wgsl.code = kWgsl_immediate;
  wgpu::ShaderModuleDescriptor module_desc;
  module_desc.nextInChain = &wgsl;
  module_desc.label = "immediate.wgsl";
  shader_ = device.CreateShaderModule(&module_desc);

  std::array<wgpu::BindGroupLayoutEntry, 2> entries;
  entries[0].binding = 0;
  entries[0].visibility = wgpu::ShaderStage::Fragment;
  entries[0].texture.sampleType = wgpu::TextureSampleType::Float;
  entries[0].texture.viewDimension = wgpu::TextureViewDimension::e2D;
  entries[1].binding = 1;
  entries[1].visibility = wgpu::ShaderStage::Fragment;
  entries[1].sampler.type = wgpu::SamplerBindingType::Filtering;
  wgpu::BindGroupLayoutDescriptor bgl_desc;
  bgl_desc.label = "immediate texture";
  bgl_desc.entryCount = entries.size();
  bgl_desc.entries = entries.data();
  bind_group_layout_ = device.CreateBindGroupLayout(&bgl_desc);

  wgpu::PipelineLayoutDescriptor layout_desc;
  layout_desc.label = "immediate";
  layout_desc.bindGroupLayoutCount = 1;
  layout_desc.bindGroupLayouts = &bind_group_layout_;
  pipeline_layout_ = device.CreatePipelineLayout(&layout_desc);

  for (int filter = 0; filter < 2; ++filter) {
    for (int repeat = 0; repeat < 2; ++repeat) {
      wgpu::SamplerDescriptor desc;
      desc.label = "immediate";
      wgpu::FilterMode mode = filter ? wgpu::FilterMode::Linear : wgpu::FilterMode::Nearest;
      desc.magFilter = mode;
      desc.minFilter = mode;
      wgpu::AddressMode address =
          repeat ? wgpu::AddressMode::Repeat : wgpu::AddressMode::ClampToEdge;
      desc.addressModeU = address;
      desc.addressModeV = address;
      samplers_[filter][repeat] = device.CreateSampler(&desc);
    }
  }

  // Draws without a texture sample this 1x1 white texture.
  wgpu::TextureDescriptor white_desc;
  white_desc.label = "immediate white";
  white_desc.size = {1, 1, 1};
  white_desc.format = wgpu::TextureFormat::RGBA8Unorm;
  white_desc.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
  wgpu::Texture white = device.CreateTexture(&white_desc);
  const uint32_t white_pixel = 0xFFFFFFFF;
  wgpu::TexelCopyTextureInfo destination;
  destination.texture = white;
  wgpu::TexelCopyBufferLayout layout;
  layout.bytesPerRow = 4;
  wgpu::Extent3D extent = {1, 1, 1};
  provider_.queue().WriteTexture(&destination, &white_pixel, sizeof(white_pixel), &layout,
                                 &extent);
  white_bind_group_ =
      CreateTextureBindGroup(white.CreateView(), ImmediateTextureFilter::kNearest, false);
  return shader_ && pipeline_layout_;
}

wgpu::BindGroup WebGpuImmediateDrawer::CreateTextureBindGroup(const wgpu::TextureView& view,
                                                              ImmediateTextureFilter filter,
                                                              bool is_repeated) {
  std::array<wgpu::BindGroupEntry, 2> entries;
  entries[0].binding = 0;
  entries[0].textureView = view;
  entries[1].binding = 1;
  entries[1].sampler =
      samplers_[filter == ImmediateTextureFilter::kLinear ? 1 : 0][is_repeated ? 1 : 0];
  wgpu::BindGroupDescriptor desc;
  desc.layout = bind_group_layout_;
  desc.entryCount = entries.size();
  desc.entries = entries.data();
  return provider_.device().CreateBindGroup(&desc);
}

std::unique_ptr<ImmediateTexture> WebGpuImmediateDrawer::CreateTexture(
    uint32_t width, uint32_t height, ImmediateTextureFilter filter, bool is_repeated,
    const uint8_t* data) {
  std::lock_guard lock(provider_.device_mutex());
  wgpu::TextureDescriptor desc;
  desc.label = "immediate";
  desc.size = {width, height, 1};
  desc.format = wgpu::TextureFormat::RGBA8Unorm;
  desc.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
  wgpu::Texture texture = provider_.device().CreateTexture(&desc);
  if (data) {
    wgpu::TexelCopyTextureInfo destination;
    destination.texture = texture;
    wgpu::TexelCopyBufferLayout layout;
    layout.bytesPerRow = width * 4;
    layout.rowsPerImage = height;
    wgpu::Extent3D extent = {width, height, 1};
    provider_.queue().WriteTexture(&destination, data, size_t(width) * height * 4, &layout,
                                   &extent);
  }
  wgpu::BindGroup bind_group = CreateTextureBindGroup(texture.CreateView(), filter, is_repeated);
  return std::make_unique<WebGpuImmediateTexture>(width, height, provider_, std::move(texture),
                                                  std::move(bind_group));
}

bool WebGpuImmediateDrawer::EnsurePipelines(wgpu::TextureFormat format) {
  if (pipeline_format_ == format && pipeline_triangles_) {
    return true;
  }
  std::array<wgpu::VertexAttribute, 3> attributes;
  attributes[0].format = wgpu::VertexFormat::Float32x2;
  attributes[0].offset = offsetof(Vertex, x);
  attributes[0].shaderLocation = 0;
  attributes[1].format = wgpu::VertexFormat::Float32x2;
  attributes[1].offset = offsetof(Vertex, u);
  attributes[1].shaderLocation = 1;
  attributes[2].format = wgpu::VertexFormat::Unorm8x4;
  attributes[2].offset = offsetof(Vertex, color);
  attributes[2].shaderLocation = 2;
  wgpu::VertexBufferLayout vertex_layout;
  vertex_layout.arrayStride = sizeof(Vertex);
  vertex_layout.attributeCount = attributes.size();
  vertex_layout.attributes = attributes.data();

  // Premultiplied-style UI blending, as in the Vulkan drawer.
  wgpu::BlendState blend;
  blend.color.srcFactor = wgpu::BlendFactor::SrcAlpha;
  blend.color.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
  blend.alpha.srcFactor = wgpu::BlendFactor::One;
  blend.alpha.dstFactor = wgpu::BlendFactor::OneMinusSrcAlpha;
  wgpu::ColorTargetState target;
  target.format = format;
  target.blend = &blend;
  wgpu::FragmentState fragment;
  fragment.module = shader_;
  fragment.entryPoint = "fs_main";
  fragment.targetCount = 1;
  fragment.targets = &target;

  wgpu::RenderPipelineDescriptor desc;
  desc.layout = pipeline_layout_;
  desc.vertex.module = shader_;
  desc.vertex.entryPoint = "vs_main";
  desc.vertex.bufferCount = 1;
  desc.vertex.buffers = &vertex_layout;
  desc.fragment = &fragment;
  desc.label = "immediate triangles";
  desc.primitive.topology = wgpu::PrimitiveTopology::TriangleList;
  pipeline_triangles_ = provider_.device().CreateRenderPipeline(&desc);
  desc.label = "immediate lines";
  desc.primitive.topology = wgpu::PrimitiveTopology::LineList;
  pipeline_lines_ = provider_.device().CreateRenderPipeline(&desc);
  pipeline_format_ = format;
  return pipeline_triangles_ && pipeline_lines_;
}

uint64_t WebGpuImmediateDrawer::Append(const void* data, uint64_t size) {
  // WriteBuffer needs 4-byte sizes and offsets.
  const uint64_t aligned_size = (size + 3) & ~uint64_t(3);
  if (!buffer_ || buffer_used_ + aligned_size > buffer_size_) {
    // Draws already recorded keep their reference to the old buffer.
    uint64_t new_size = std::max(buffer_size_ ? buffer_size_ * 2 : kInitialBufferSize,
                                 aligned_size);
    wgpu::BufferDescriptor desc;
    desc.label = "immediate vertices and indices";
    desc.size = new_size;
    desc.usage =
        wgpu::BufferUsage::Vertex | wgpu::BufferUsage::Index | wgpu::BufferUsage::CopyDst;
    buffer_ = provider_.device().CreateBuffer(&desc);
    if (!buffer_) {
      return UINT64_MAX;
    }
    buffer_size_ = new_size;
    buffer_used_ = 0;
  }
  const uint64_t offset = buffer_used_;
  if (aligned_size == size) {
    provider_.queue().WriteBuffer(buffer_, offset, data, size);
  } else {
    std::vector<uint8_t> padded(aligned_size, 0);
    std::memcpy(padded.data(), data, size);
    provider_.queue().WriteBuffer(buffer_, offset, padded.data(), aligned_size);
  }
  buffer_used_ += aligned_size;
  return offset;
}

void WebGpuImmediateDrawer::Begin(UIDrawContext& ui_draw_context, float coordinate_space_width,
                                  float coordinate_space_height) {
  ImmediateDrawer::Begin(ui_draw_context, coordinate_space_width, coordinate_space_height);
  auto& context = static_cast<WebGpuUIDrawContext&>(ui_draw_context);
  provider_.device_mutex().lock();  // Until End.
  // The queue orders this paint's buffer writes after the previous paint's
  // draws, so the buffer can be reused from the start for every paint.
  if (buffer_paint_serial_ != context.paint_serial()) {
    buffer_paint_serial_ = context.paint_serial();
    buffer_used_ = 0;
  }
  pass_ = EnsurePipelines(context.render_target_format()) ? &context.pass() : nullptr;
  current_pipeline_ = nullptr;
  current_bind_group_ = nullptr;
  current_scissor_ = {UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX};
}

void WebGpuImmediateDrawer::BeginDrawBatch(const ImmediateDrawBatch& batch) {
  batch_open_ = false;
  if (!pass_ || batch.vertex_count <= 0) {
    return;
  }
  // Coordinate space → clip space (y up).
  const float scale_x = 2.0f / coordinate_space_width();
  const float scale_y = -2.0f / coordinate_space_height();
  vertex_scratch_.resize(size_t(batch.vertex_count));
  for (int i = 0; i < batch.vertex_count; ++i) {
    const ImmediateVertex& in = batch.vertices[i];
    Vertex& out = vertex_scratch_[size_t(i)];
    out.x = in.x * scale_x - 1.0f;
    out.y = in.y * scale_y + 1.0f;
    out.u = in.u;
    out.v = in.v;
    out.color = in.color;
  }
  uint64_t vertex_size = sizeof(Vertex) * uint64_t(batch.vertex_count);
  uint64_t vertex_offset = Append(vertex_scratch_.data(), vertex_size);
  if (vertex_offset == UINT64_MAX) {
    return;
  }
  wgpu::Buffer vertex_buffer = buffer_;
  batch_indexed_ = batch.indices && batch.index_count > 0;
  if (batch_indexed_) {
    uint64_t index_size = sizeof(uint16_t) * uint64_t(batch.index_count);
    uint64_t index_offset = Append(batch.indices, index_size);
    if (index_offset == UINT64_MAX) {
      return;
    }
    pass_->SetIndexBuffer(buffer_, wgpu::IndexFormat::Uint16, index_offset, index_size);
  }
  // Set after the indices: appending them may have replaced buffer_.
  pass_->SetVertexBuffer(0, vertex_buffer, vertex_offset, vertex_size);
  batch_open_ = true;
}

void WebGpuImmediateDrawer::Draw(const ImmediateDraw& draw) {
  if (!batch_open_ || draw.count <= 0) {
    return;
  }
  uint32_t left, top, width, height;
  if (!ScissorToRenderTarget(draw, left, top, width, height)) {
    return;
  }
  if (current_scissor_ != std::array<uint32_t, 4>{left, top, width, height}) {
    current_scissor_ = {left, top, width, height};
    pass_->SetScissorRect(left, top, width, height);
  }
  const wgpu::RenderPipeline* pipeline =
      draw.primitive_type == ImmediatePrimitiveType::kLines ? &pipeline_lines_
                                                            : &pipeline_triangles_;
  if (current_pipeline_ != pipeline) {
    current_pipeline_ = pipeline;
    pass_->SetPipeline(*pipeline);
  }
  const wgpu::BindGroup* bind_group = &white_bind_group_;
  if (draw.texture) {
    bind_group = &static_cast<WebGpuImmediateTexture*>(draw.texture)->bind_group();
  }
  if (current_bind_group_ != bind_group) {
    current_bind_group_ = bind_group;
    pass_->SetBindGroup(0, *bind_group);
  }
  if (batch_indexed_) {
    pass_->DrawIndexed(uint32_t(draw.count), 1, uint32_t(draw.index_offset),
                       int32_t(draw.base_vertex), 0);
  } else {
    pass_->Draw(uint32_t(draw.count), 1, uint32_t(draw.base_vertex), 0);
  }
}

void WebGpuImmediateDrawer::EndDrawBatch() {
  batch_open_ = false;
}

void WebGpuImmediateDrawer::End() {
  if (pass_) {
    // Leave the full-target scissor for whatever the presenter draws next.
    pass_->SetScissorRect(0, 0, ui_draw_context()->render_target_width(),
                          ui_draw_context()->render_target_height());
  }
  pass_ = nullptr;
  batch_open_ = false;
  provider_.device_mutex().unlock();
  ImmediateDrawer::End();
}

}  // namespace rex::ui::webgpu
