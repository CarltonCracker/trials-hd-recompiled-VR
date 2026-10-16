// rexgpu-webgpu - primitive conversion.

#include "primitive_converter.h"

#include <vector>

#include <rex/logging.h>

#include "frame_recorder.h"
#include "webgpu_provider.h"

namespace rex::graphics::webgpu {

WebGpuPrimitiveProcessor::~WebGpuPrimitiveProcessor() {
  Shutdown();
}

bool WebGpuPrimitiveProcessor::Initialize() {
  // Full 32-bit indices: WebGPU index buffers take uint32 without limits.
  // No fans, loops or quads on the host; no point sprite expansion (WebGPU
  // points are 1 pixel; Trials' point draws are dummies or memexport);
  // rectangle lists are expanded in the vertex shader.
  return InitializeCommon(true, false, false, false, true, false);
}

void WebGpuPrimitiveProcessor::Shutdown() {
  ShutdownCommon();
  builtin_index_buffer_ = nullptr;
}

bool WebGpuPrimitiveProcessor::InitializeBuiltinIndexBuffer(
    size_t size_bytes, std::function<void(void*)> fill_callback) {
  std::vector<uint8_t> data((size_bytes + 3) & ~size_t(3));
  fill_callback(data.data());
  wgpu::BufferDescriptor desc{};
  desc.label = "builtin index buffer";
  desc.size = data.size();
  desc.usage = wgpu::BufferUsage::Index | wgpu::BufferUsage::CopyDst;
  builtin_index_buffer_ = provider_.device().CreateBuffer(&desc);
  if (!builtin_index_buffer_) {
    return false;
  }
  provider_.queue().WriteBuffer(builtin_index_buffer_, 0, data.data(), data.size());
  return true;
}

void* WebGpuPrimitiveProcessor::RequestHostConvertedIndexBufferForCurrentFrame(
    xenos::IndexFormat format, uint32_t index_count, bool coalign_for_simd,
    uint32_t coalignment_original_address, size_t& backend_handle_out) {
  uint32_t index_size = format == xenos::IndexFormat::kInt16 ? 2 : 4;
  uint32_t size = index_size * index_count;
  // Room to move the start forward for SIMD co-alignment with the source.
  uint32_t padding = coalign_for_simd ? XE_GPU_PRIMITIVE_PROCESSOR_SIMD_SIZE : 0;
  uint32_t offset;
  auto* mapping = static_cast<uint8_t*>(
      frame_recorder_.AllocateIndices(size + padding, std::max(index_size, uint32_t(4)), offset));
  if (!mapping) {
    return nullptr;
  }
  if (coalign_for_simd) {
    ptrdiff_t shift = GetSimdCoalignmentOffset(mapping, coalignment_original_address);
    mapping += shift;
    offset += uint32_t(shift);
  }
  backend_handle_out = offset;
  return mapping;
}

}  // namespace rex::graphics::webgpu
