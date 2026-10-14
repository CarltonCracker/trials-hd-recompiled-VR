// rexgpu-webgpu - render-target cache (section 8.7): host textures keyed by
// the guest EDRAM location, pitch, format and sample count. No EDRAM
// emulation: a render target is a texture, and overlapping EDRAM ranges with
// different formats are separate textures (aliasing is logged, not copied).

#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <unordered_map>

#include <webgpu/webgpu_cpp.h>

#include <rex/graphics/register_file.h>
#include <rex/graphics/xenos.h>

namespace rex::ui::webgpu {
class WebGpuProvider;
}  // namespace rex::ui::webgpu

namespace rex::graphics::webgpu {

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
  wgpu::TextureFormat host_format = wgpu::TextureFormat::Undefined;
  uint32_t sample_count = 1;
  // Host pixels (scaled).
  uint32_t width = 0;
  uint32_t height = 0;
  // Multiplier for the pixel shader output (format range scale; the exponent
  // bias is per draw).
  float format_output_scale = 1.0f;
  // Contents are undefined until the first pass clears them.
  bool initialized = false;
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

  RenderTargetCache(ui::webgpu::WebGpuProvider& provider, uint32_t scale_x, uint32_t scale_y);
  ~RenderTargetCache();

  uint32_t scale_x() const { return scale_x_; }
  uint32_t scale_y() const { return scale_y_; }

  // Render targets for the draw: the color targets the pixel shader writes
  // (color_targets_used) and depth/stencil if enabled. False if nothing
  // would be drawn (no targets).
  bool Update(const RegisterFile& regs, uint32_t color_targets_used, bool depth_stencil_used,
              Binding& binding_out);

  // Host format for a guest color/depth format (with feature fallbacks).
  wgpu::TextureFormat GetColorHostFormat(xenos::ColorRenderTargetFormat format,
                                         float* output_scale_out = nullptr) const;
  wgpu::TextureFormat GetDepthHostFormat(xenos::DepthRenderTargetFormat format) const;
  static bool HostDepthFormatHasStencil(wgpu::TextureFormat format);

  // The most recently used target at an EDRAM base (for resolves), or null.
  RenderTarget* FindLastUsed(uint32_t base_tiles, bool is_depth) const;

 private:
  RenderTarget* GetOrCreate(RenderTargetKey key);

  ui::webgpu::WebGpuProvider& provider_;
  uint32_t scale_x_;
  uint32_t scale_y_;
  std::unordered_map<uint64_t, std::unique_ptr<RenderTarget>> targets_;
  // Last target used at each EDRAM base, color and depth separately.
  std::unordered_map<uint32_t, RenderTarget*> last_color_at_base_;
  std::unordered_map<uint32_t, RenderTarget*> last_depth_at_base_;
};

}  // namespace rex::graphics::webgpu
