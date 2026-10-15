// rexgpu-webgpu - render-target cache (section 8.7): host textures keyed by
// the guest EDRAM location, pitch, format and sample count. The cache tracks
// which target last owned each EDRAM tile; a target taking over tiles from
// another (the game reinterprets 1x targets as 4x MSAA for downsampling, depth
// as color, and so on) gets the old contents converted sample by sample
// (shaders/transfer.wgsl).

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include <webgpu/webgpu_cpp.h>

#include <rex/graphics/register_file.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>

namespace rex::ui::webgpu {
class WebGpuProvider;
}  // namespace rex::ui::webgpu

namespace rex::graphics::webgpu {

class FrameRecorder;
class GpuProfiler;

union RenderTargetKey {
  uint64_t value = 0;
  struct {
    uint32_t base_tiles : 12;
    uint32_t pitch_pixels : 14;
    uint32_t format : 4;  // Color- or DepthRenderTargetFormat
    uint32_t is_depth : 1;
    uint32_t msaa_samples : 2;  // xenos::MsaaSamples
  };
};

struct RenderTarget {
  RenderTargetKey key;
  wgpu::Texture texture;
  wgpu::TextureView view;
  wgpu::TextureView depth_only_view;
  wgpu::TextureView stencil_only_view;  // Null without stencil.
  // Bind group reading this target in EDRAM transfers (created on first use).
  wgpu::BindGroup transfer_source_group;
  wgpu::TextureFormat host_format = wgpu::TextureFormat::Undefined;
  uint32_t sample_count = 1;
  // Host pixels (scaled).
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t pitch_tiles = 1;
  // Tiles from the base this target owned as of ownership epoch owned_epoch.
  uint64_t owned_epoch = UINT64_MAX;
  uint32_t owned_tiles = 0;
  // Multiplier for the pixel shader output (format range scale; the exponent
  // bias is per draw).
  float format_output_scale = 1.0f;
  // Contents are undefined until the first pass clears them.
  bool initialized = false;
  // Depth: stencil may be non-zero somewhere (written by a draw or a clear
  // since the last zero clear). Transfers skip the stencil bit passes if not.
  bool stencil_nonzero = false;
  // Incremented when draws or clears change the contents (not by transfers
  // in, which copy existing EDRAM data).
  uint64_t version = 0;
  // Targets this one received tiles from, at both versions: if neither
  // changed, giving the tiles back needs no transfer (the source's texture
  // still holds them), e.g. a 1x target viewed as 4x only to be resolved.
  struct CopyOrigin {
    const RenderTarget* source;
    uint64_t source_version;
    uint64_t version;
  };
  std::vector<CopyOrigin> copied_from;
};

class RenderTargetCache {
 public:
  struct Binding {
    std::array<RenderTarget*, 4> color = {};
    RenderTarget* depth = nullptr;
    uint32_t sample_count = 1;
    // Smallest common size of the bound targets (host pixels).
    uint32_t width = 0;
    uint32_t height = 0;
  };

  // A draw that fully overwrites the samples it covers in some targets (a
  // Direct3D 9 clear rectangle): transfers of tile rows it covers are skipped,
  // like the SDK's elide_rt_transfers.
  struct Overwrite {
    // Bit 0: depth/stencil, bit 1 + i: color target i.
    uint32_t mask = 0;
    // Covered guest pixels [x0, x1) x [y0, y1).
    int32_t x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  };

  RenderTargetCache(ui::webgpu::WebGpuProvider& provider, FrameRecorder& frame_recorder,
                    uint32_t scale_x, uint32_t scale_y);
  ~RenderTargetCache();

  void set_gpu_profiler(GpuProfiler* profiler) { gpu_profiler_ = profiler; }
  uint32_t scale_x() const { return scale_x_; }
  uint32_t scale_y() const { return scale_y_; }

  // Render targets for the draw: the color targets the pixel shader writes
  // (color_targets_used) and depth/stencil if enabled. False if nothing
  // would be drawn (no targets).
  // `rows`: guest pixel rows the draw can write (DrawExtentEstimator).
  bool Update(const RegisterFile& regs, uint32_t color_targets_used, bool depth_stencil_used,
              uint32_t rows, const Overwrite& overwrite, Binding& binding_out);

  // Host format for a guest color/depth format (with feature fallbacks).
  wgpu::TextureFormat GetColorHostFormat(xenos::ColorRenderTargetFormat format,
                                         float* output_scale_out = nullptr) const;
  wgpu::TextureFormat GetDepthHostFormat(xenos::DepthRenderTargetFormat format) const;
  static bool HostDepthFormatHasStencil(wgpu::TextureFormat format);

  // The target a resolve reads or clears (the resolve's own base, pitch,
  // format and sample count), holding the current contents of its tiles.
  // `for_clear`: the target is cleared as a whole right after, so its old
  // contents are not transferred in.
  RenderTarget* GetForResolve(const RegisterFile& regs, const draw_util::ResolveInfo& info,
                              bool is_depth, bool for_clear = false);

 private:
  RenderTarget* GetOrCreate(RenderTargetKey key);
  // Takes ownership of the target's tiles down to `rows` guest pixel rows,
  // transferring tiles owned by other targets (except those in `bound`).
  // `overwrite`: the draw fully overwrites this target in that rectangle.
  void Acquire(RenderTarget* target, uint32_t rows, const Binding* bound,
               const Overwrite* overwrite = nullptr);
  void Transfer(RenderTarget* source, RenderTarget* dest, uint32_t tile_first,
                uint32_t tile_count);
  bool InitializeTransfer();
  static bool HoldsUnchangedCopy(const RenderTarget& owner, const RenderTarget& target);
  static bool CoversTiles(const RenderTarget& target, uint32_t tile_first, uint32_t tile_end,
                          const Overwrite& overwrite);

  ui::webgpu::WebGpuProvider& provider_;
  FrameRecorder& frame_recorder_;
  GpuProfiler* gpu_profiler_ = nullptr;
  uint32_t scale_x_;
  uint32_t scale_y_;
  std::unordered_map<uint64_t, std::unique_ptr<RenderTarget>> targets_;
  // Last target that wrote each EDRAM tile.
  std::array<RenderTarget*, xenos::kEdramTileCount> owners_ = {};
  // Incremented whenever a tile changes owner.
  uint64_t ownership_epoch_ = 0;
  wgpu::ShaderModule transfer_module_;
  // By source type (color, color 4x, depth, depth 4x).
  std::array<wgpu::BindGroupLayout, 4> transfer_bind_group_layouts_;
  std::array<wgpu::PipelineLayout, 4> transfer_pipeline_layouts_;
  // By stencil bit pass, source type, destination format and samples.
  std::unordered_map<uint64_t, wgpu::RenderPipeline> transfer_pipelines_;
};

}  // namespace rex::graphics::webgpu
