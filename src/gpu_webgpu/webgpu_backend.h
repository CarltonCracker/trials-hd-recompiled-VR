// rexgpu-webgpu - the WebGPU backend: consumes DrawSnapshot, ResolveSnapshot
// and SwapSnapshot from a front-end (section 7.6.6). Called on one thread (the
// command processor's); takes the device mutex itself.

#pragma once

#include <cstdint>

#include "draw_snapshot.h"

namespace rex::ui {
class Presenter;
}  // namespace rex::ui
namespace rex::ui::webgpu {
class WebGpuProvider;
}  // namespace rex::ui::webgpu

namespace rex::graphics::webgpu {

class WebGpuBackend {
 public:
  WebGpuBackend(ui::webgpu::WebGpuProvider& provider, ui::Presenter* presenter)
      : provider_(provider), presenter_(presenter) {}

  // Returns false if the draw was dropped (logged).
  bool Draw(const DrawSnapshot& draw);
  bool Resolve(const ResolveSnapshot& resolve);
  void Swap(const SwapSnapshot& swap);

 private:
  bool IsTracedFrame(uint32_t frame_index) const;

  ui::webgpu::WebGpuProvider& provider_;
  ui::Presenter* presenter_;
};

}  // namespace rex::graphics::webgpu
