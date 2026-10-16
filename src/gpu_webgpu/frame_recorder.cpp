// rexgpu-webgpu - frame recorder.

#include "frame_recorder.h"

#include <cstring>
#include <mutex>
#include <string>

#include <fmt/format.h>
#include <rex/logging.h>

#include "webgpu_provider.h"

namespace rex::graphics::webgpu {

FrameRecorder::~FrameRecorder() {
  std::lock_guard lock(provider_.device_mutex());
  EndRenderPass();
  encoder_ = nullptr;
  uniform_ring_ = nullptr;
  index_ring_ = nullptr;
}

bool FrameRecorder::Initialize() {
  std::lock_guard lock(provider_.device_mutex());
  wgpu::BufferDescriptor desc;
  desc.label = "uniform ring";
  desc.size = kUniformRingSize;
  desc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
  uniform_ring_ = provider_.device().CreateBuffer(&desc);
  uniform_staging_.reserve(1 << 20);
  desc.label = "converted index ring";
  desc.size = kIndexRingSize;
  desc.usage = wgpu::BufferUsage::Index | wgpu::BufferUsage::CopyDst |
               wgpu::BufferUsage::CopySrc;
  index_ring_ = provider_.device().CreateBuffer(&desc);
  index_staging_.reserve(1 << 20);
  return uniform_ring_ && index_ring_;
}

uint32_t FrameRecorder::UploadUniforms(const void* data, uint32_t size) {
  // Every binding window must fit after its offset.
  if (uniform_staging_.size() + kMaxUniformBindingSize > kUniformRingSize) {
    Submit();
  }
  uint32_t offset = uint32_t(uniform_staging_.size());
  uint32_t aligned = (size + kUniformAlignment - 1) & ~(kUniformAlignment - 1);
  uniform_staging_.resize(offset + aligned);
  std::memcpy(uniform_staging_.data() + offset, data, size);
  if (aligned > size) {
    std::memset(uniform_staging_.data() + offset + size, 0, aligned - size);
  }
  return offset;
}

void FrameRecorder::ReserveUniforms(uint32_t size) {
  if (uniform_staging_.size() + size + 8 * kUniformAlignment + kMaxUniformBindingSize >
      kUniformRingSize) {
    Submit();
  }
}

void* FrameRecorder::AllocateIndices(uint32_t size, uint32_t alignment, uint32_t& offset_out) {
  if (size > kIndexRingSize) {
    return nullptr;
  }
  uint64_t offset = (index_staging_.size() + alignment - 1) / alignment * alignment;
  if (offset + size > kIndexRingSize) {
    Submit();
    offset = 0;
  }
  // WriteBuffer sizes must be multiples of 4.
  index_staging_.resize((offset + size + 3) & ~uint64_t(3));
  offset_out = uint32_t(offset);
  return index_staging_.data() + offset;
}

const wgpu::CommandEncoder& FrameRecorder::encoder() {
  if (!encoder_) {
    wgpu::CommandEncoderDescriptor desc;
    desc.label = "guest frame";
    encoder_ = provider_.device().CreateCommandEncoder(&desc);
  }
  return encoder_;
}

const wgpu::RenderPassEncoder& FrameRecorder::BeginRenderPass(
    const RenderPassTargets& targets, std::string_view label,
    const wgpu::PassTimestampWrites* timestamp_writes) {
  if (pass_ && pass_targets_.SameAttachments(targets)) {
    return pass_;
  }
  EndRenderPass();
  std::array<wgpu::RenderPassColorAttachment, 4> colors;
  uint32_t color_count = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    if (targets.color[i]) {
      color_count = i + 1;
    }
  }
  for (uint32_t i = 0; i < color_count; ++i) {
    wgpu::RenderPassColorAttachment& color = colors[i];
    if (!targets.color[i]) {
      continue;  // Sparse attachments keep a null view.
    }
    color.view = targets.color[i];
    color.loadOp = (targets.color_clear_mask & (1u << i)) ? wgpu::LoadOp::Clear
                                                          : wgpu::LoadOp::Load;
    color.storeOp = wgpu::StoreOp::Store;
    color.clearValue = {0.0, 0.0, 0.0, 0.0};
  }
  wgpu::RenderPassDepthStencilAttachment depth;
  wgpu::RenderPassDescriptor desc{};
  // Labels only when the caller made one (webgpu_debug_labels).
  std::string pass_label(label);
  if (!label.empty()) {
    desc.label = pass_label.c_str();
  }
  desc.colorAttachmentCount = color_count;
  desc.colorAttachments = colors.data();
  if (targets.depth_stencil) {
    depth.view = targets.depth_stencil;
    depth.depthLoadOp = targets.depth_clear ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load;
    depth.depthStoreOp = wgpu::StoreOp::Store;
    depth.depthClearValue = 0.0f;
    if (targets.depth_has_stencil) {
      depth.stencilLoadOp = targets.depth_clear ? wgpu::LoadOp::Clear : wgpu::LoadOp::Load;
      depth.stencilStoreOp = wgpu::StoreOp::Store;
      depth.stencilClearValue = 0;
    }
    desc.depthStencilAttachment = &depth;
  }
  desc.timestampWrites = timestamp_writes;
  pass_ = encoder().BeginRenderPass(&desc);
  if (!label.empty()) {
    pass_.PushDebugGroup(pass_label.c_str());
    has_debug_group_ = true;
  }
  pass_targets_ = targets;
  ++pass_serial_;
  return pass_;
}

void FrameRecorder::EndRenderPass() {
  if (pass_) {
    if (has_debug_group_) {
      pass_.PopDebugGroup();
      has_debug_group_ = false;
    }
    pass_.End();
    pass_ = nullptr;
    pass_targets_ = {};
  }
}

bool FrameRecorder::Submit() {
  EndRenderPass();
  if (!uniform_staging_.empty()) {
    provider_.queue().WriteBuffer(uniform_ring_, 0, uniform_staging_.data(),
                                  uniform_staging_.size());
    uniform_staging_.clear();
  }
  if (!index_staging_.empty()) {
    provider_.queue().WriteBuffer(index_ring_, 0, index_staging_.data(), index_staging_.size());
    index_staging_.clear();
  }
  if (encoder_) {
    wgpu::CommandBuffer commands = encoder_.Finish();
    encoder_ = nullptr;
    provider_.queue().Submit(1, &commands);
    ++submission_count_;
  }
  return true;
}

}  // namespace rex::graphics::webgpu
