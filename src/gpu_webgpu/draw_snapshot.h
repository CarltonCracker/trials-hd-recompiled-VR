// rexgpu-webgpu - the interface between a front-end (today the PM4 command
// processor, later possibly hooks on the game's Direct3D runtime) and the
// WebGPU backend. The backend sees guest GPU state only through these structs.
// Spec and rules: docs/webgpu-renderer/README.md, section 7.6.6.

#pragma once

#include <cstdint>

#include <rex/graphics/register_file.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>

namespace rex::graphics {
class Shader;
}  // namespace rex::graphics

namespace rex::graphics::webgpu {

// State that changed since the previous DrawSnapshot. A set bit may be a false
// positive; a clear bit must be exact. Producers set kDirtyAll when unsure.
enum DrawDirty : uint32_t {
  kDirtyVsFloatConstants = 1u << 0,   // 0x4000-0x43FF
  kDirtyPsFloatConstants = 1u << 1,   // 0x4400-0x47FF
  kDirtyFetchConstants = 1u << 2,     // 0x4800-0x48BF
  kDirtyBoolLoopConstants = 1u << 3,  // 0x4900-0x4927
  kDirtyRenderTargets = 1u << 4,      // RB_SURFACE_INFO, RB_COLOR*_INFO, RB_DEPTH_INFO, RB_MODECONTROL
  kDirtyPipelineState = 1u << 5,      // other context registers (blend, depth, raster, ...)
  kDirtyViewportScissor = 1u << 6,    // PA_CL_VPORT_*, PA_CL_VTE_CNTL, PA_SC_*SCISSOR*, window offset
  kDirtyShaders = 1u << 7,            // the bound vertex or pixel shader
  kDirtyAll = ~0u,
};

// Guest index buffer of an indexed draw (CommandProcessor::IndexBufferInfo is
// protected there).
struct GuestIndexBuffer {
  xenos::IndexFormat format = xenos::IndexFormat::kInt16;
  xenos::Endian endianness = xenos::Endian::kNone;
  uint32_t count = 0;
  uint32_t guest_base = 0;  // physical address
  uint32_t length_bytes = 0;
};

struct DrawSnapshot {
  // Full register state at the draw, read-only. Valid only during the call.
  const RegisterFile* regs = nullptr;
  xenos::PrimitiveType primitive_type = xenos::PrimitiveType::kNone;
  uint32_t index_count = 0;  // VGT_DRAW_INITIATOR num_indices
  bool indexed = false;      // false: auto-indexed, index_buffer unused
  bool major_mode_explicit = false;
  GuestIndexBuffer index_buffer;
  Shader* vertex_shader = nullptr;  // from the backend's shader cache
  Shader* pixel_shader = nullptr;   // null: no pixel shader
  uint32_t dirty = kDirtyAll;       // DrawDirty bits since the previous DrawSnapshot
  uint32_t frame_index = 0;         // swaps since start
  uint32_t draw_index = 0;          // within the frame
};

struct ResolveSnapshot {
  const RegisterFile* regs = nullptr;  // RB_COPY_*, RB_SURFACE_INFO, RB_*_INFO, RB_*_CLEAR*
  draw_util::ResolveInfo info;         // from draw_util::GetResolveInfo
  uint32_t frame_index = 0;
  uint32_t resolve_index = 0;  // within the frame
};

struct SwapSnapshot {
  uint32_t frontbuffer_ptr = 0;
  uint32_t frontbuffer_width = 0;
  uint32_t frontbuffer_height = 0;
  uint32_t display_width = 0;  // guest video mode, for the aspect ratio
  uint32_t display_height = 0;
  uint32_t frame_index = 0;
  // Gamma ramps (valid during the call): 256 DC_LUT_30_COLOR values and
  // 128 x RGB DC_LUT_PWL_DATA values.
  const uint32_t* gamma_ramp_table = nullptr;
  const uint32_t* gamma_ramp_pwl = nullptr;
  // 0: none, 1: FXAA, 2: FXAA extreme (CommandProcessor::SwapPostEffect).
  uint32_t post_effect = 0;
};

}  // namespace rex::graphics::webgpu
