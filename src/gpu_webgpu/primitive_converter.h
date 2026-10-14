// rexgpu-webgpu - primitive conversion (section 8.6): the SDK's
// PrimitiveProcessor with WebGPU index buffers. Triangle fans, line loops and
// quad lists become lists on the CPU; rectangle lists are expanded in the
// vertex shader; 16-bit big-endian guest indices are read from the shared
// memory buffer and swapped in the vertex shader.

#pragma once

#include <webgpu/webgpu_cpp.h>

#include <rex/graphics/primitive_processor.h>

namespace rex::ui::webgpu {
class WebGpuProvider;
}  // namespace rex::ui::webgpu

namespace rex::graphics::webgpu {

class FrameRecorder;

class WebGpuPrimitiveProcessor final : public PrimitiveProcessor {
 public:
  WebGpuPrimitiveProcessor(const RegisterFile& register_file, memory::Memory& memory,
                           SharedMemory& shared_memory, ui::webgpu::WebGpuProvider& provider,
                           FrameRecorder& frame_recorder)
      : PrimitiveProcessor(register_file, memory, shared_memory),
        provider_(provider),
        frame_recorder_(frame_recorder) {}
  ~WebGpuPrimitiveProcessor() override;

  bool Initialize();
  void Shutdown();
  // Call between frames.
  void EndFrame() { ClearPerFrameCache(); }

  // The index buffer for a ProcessingResult's host_index_buffer_handle:
  // builtin buffer (kHostBuiltin*) or the frame recorder's ring (converted).
  const wgpu::Buffer& builtin_index_buffer() const { return builtin_index_buffer_; }
  static uint64_t GetConvertedIndexBufferOffset(size_t handle) { return handle; }
  static uint64_t GetBuiltinIndexBufferOffset(size_t handle) {
    return GetBuiltinIndexBufferOffsetBytes(handle);
  }

 protected:
  bool InitializeBuiltinIndexBuffer(size_t size_bytes,
                                    std::function<void(void*)> fill_callback) override;
  void* RequestHostConvertedIndexBufferForCurrentFrame(xenos::IndexFormat format,
                                                       uint32_t index_count, bool coalign_for_simd,
                                                       uint32_t coalignment_original_address,
                                                       size_t& backend_handle_out) override;

 private:
  ui::webgpu::WebGpuProvider& provider_;
  FrameRecorder& frame_recorder_;
  wgpu::Buffer builtin_index_buffer_;
};

}  // namespace rex::graphics::webgpu
