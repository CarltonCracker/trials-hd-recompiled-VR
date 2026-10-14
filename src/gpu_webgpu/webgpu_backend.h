// rexgpu-webgpu - the WebGPU backend: consumes DrawSnapshot, ResolveSnapshot
// and SwapSnapshot from a front-end (section 7.6.6). Called on one thread (the
// command processor's); takes the device mutex itself.

#pragma once

#include <cstdint>
#include <cstdio>
#include <memory>

#include <webgpu/webgpu_cpp.h>

#include "draw_snapshot.h"

namespace rex::memory {
class Memory;
}  // namespace rex::memory
namespace rex::ui {
class Presenter;
}  // namespace rex::ui
namespace rex::ui::webgpu {
class WebGpuProvider;
}  // namespace rex::ui::webgpu

namespace rex::graphics::webgpu {

class FrameRecorder;
class PipelineCache;
class RenderTargetCache;
class TextureCache;
class WebGpuPrimitiveProcessor;
class WebGpuSharedMemory;

class WebGpuBackend {
 public:
  WebGpuBackend(ui::webgpu::WebGpuProvider& provider, ui::Presenter* presenter);
  ~WebGpuBackend();

  // register_file is the front-end's register state, read by the primitive
  // processor during Draw.
  bool Initialize(memory::Memory& memory, const RegisterFile& register_file);

  PipelineCache& pipeline_cache() const { return *pipeline_cache_; }

  // Returns false if the draw was dropped (logged).
  bool Draw(const DrawSnapshot& draw);
  bool Resolve(const ResolveSnapshot& resolve);
  void Swap(const SwapSnapshot& swap);

  uint64_t submission_count() const;

  // Draws dropped because a feature is not implemented yet (for the log).
  uint32_t TakeSkippedDrawCount() {
    uint32_t count = skipped_draws_;
    skipped_draws_ = 0;
    return count;
  }

 private:
  bool IsTracedFrame(uint32_t frame_index) const;
  void EnsureDumpOpen(uint32_t frame_index);
  void DumpDraw(const DrawSnapshot& draw);
  void DumpResolve(const ResolveSnapshot& resolve);
  void DumpSwap(const SwapSnapshot& swap);

  FILE* dump_file_ = nullptr;
  uint32_t dump_swaps_written_ = 0;
  uint32_t dump_records_written_ = 0;

  ui::webgpu::WebGpuProvider& provider_;
  ui::Presenter* presenter_;
  std::unique_ptr<FrameRecorder> frame_recorder_;
  std::unique_ptr<WebGpuSharedMemory> shared_memory_;
  std::unique_ptr<WebGpuPrimitiveProcessor> primitive_processor_;
  std::unique_ptr<PipelineCache> pipeline_cache_;
  std::unique_ptr<RenderTargetCache> render_target_cache_;
  std::unique_ptr<TextureCache> texture_cache_;
  wgpu::BindGroup draw_bind_group_;
  wgpu::BindGroup shared_memory_bind_group_;
  uint32_t skipped_draws_ = 0;
};

}  // namespace rex::graphics::webgpu
