// rexgpu-webgpu - resolve engine (section 8.8): EDRAM copies become GPU copies
// from a render target into a texture holding the values a texture fetch of
// the destination would read, registered with the texture cache as a resolve
// alias for the destination address. Small resolves (readback_resolve_max_bytes,
// the game's auto-exposure luminance chain) are also written to guest memory.

#pragma once

#include <atomic>
#include <cstdint>
#include <unordered_map>

#include <webgpu/webgpu_cpp.h>

#include <rex/graphics/util/draw.h>

namespace rex::memory {
class Memory;
}  // namespace rex::memory

namespace rex::ui::webgpu {
class WebGpuProvider;
}  // namespace rex::ui::webgpu

namespace rex::graphics::webgpu {

class FrameRecorder;
class GpuProfiler;
class RenderTargetCache;
class TextureCache;
struct RenderTarget;

class ResolveEngine {
 public:
  ResolveEngine(ui::webgpu::WebGpuProvider& provider, memory::Memory& memory,
                FrameRecorder& frame_recorder, RenderTargetCache& render_target_cache,
                TextureCache& texture_cache)
      : provider_(provider),
        memory_(memory),
        frame_recorder_(frame_recorder),
        render_target_cache_(render_target_cache),
        texture_cache_(texture_cache) {}

  bool Initialize();
  void set_gpu_profiler(GpuProfiler* profiler) { gpu_profiler_ = profiler; }

  // Performs the copy part of a resolve (not the clears) from the source
  // target (RenderTargetCache::GetForResolve). Returns false if it was dropped.
  bool Copy(const draw_util::ResolveInfo& info, RenderTarget& source);

  // Source rectangle of a copy in the render target, in host pixels.
  static void GetSourceRectangle(const draw_util::ResolveInfo& info, uint32_t scale_x,
                                 uint32_t scale_y, uint32_t& x, uint32_t& y, uint32_t& width,
                                 uint32_t& height);

 private:
  enum class SourceType : uint32_t { kColor, kColorMultisample, kDepth, kDepthMultisample };
  wgpu::RenderPipeline GetPipeline(SourceType source_type, wgpu::TextureFormat dest_format);
  // Writes the copied rectangle of the alias back to guest memory, in the
  // guest's tiled layout and number format, for the CPU to read.
  void ReadBack(const draw_util::ResolveInfo& info, const wgpu::Texture& alias, uint32_t dest_x,
                uint32_t dest_y, uint32_t width, uint32_t height, uint32_t scale_x,
                uint32_t scale_y);

  struct Readback {
    enum State : int { kIdle, kCopied, kMapping, kMapped };
    std::atomic<int> state = kIdle;
    wgpu::Buffer buffer;
    // Submission count when the copy was recorded (mapped after it is submitted).
    uint64_t submission = 0;
    draw_util::ResolveInfo info;
    uint32_t dest_x, dest_y, width, height, scale_x, scale_y, row_pitch, host_bytes;
  };
  void WriteToGuest(const Readback& readback);

  ui::webgpu::WebGpuProvider& provider_;
  memory::Memory& memory_;
  GpuProfiler* gpu_profiler_ = nullptr;
  FrameRecorder& frame_recorder_;
  RenderTargetCache& render_target_cache_;
  TextureCache& texture_cache_;
  wgpu::ShaderModule module_;
  wgpu::BindGroupLayout layouts_[4];
  wgpu::PipelineLayout pipeline_layouts_[4];
  std::unordered_map<uint64_t, wgpu::RenderPipeline> pipelines_;
  // By source view; a cached group keeps its view alive, so keys stay unique.
  std::unordered_map<WGPUTextureView, wgpu::BindGroup> bind_groups_;
  // By guest extent (start << 32 | length).
  std::unordered_map<uint64_t, Readback> readbacks_;
};

}  // namespace rex::graphics::webgpu
