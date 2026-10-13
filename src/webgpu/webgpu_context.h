// trials_hd - WebGPU device and window surface.
//
// Written against the standard webgpu.h C++ bindings (webgpu_cpp.h), so the same
// code builds natively with Dawn and for the web with Emscripten's emdawnwebgpu.

#pragma once

#include <cstdint>
#include <string>

#include <webgpu/webgpu_cpp.h>

struct SDL_Window;

namespace trials_hd::webgpu {

struct SurfaceOptions {
  bool vsync = true;  // FIFO when on; immediate/mailbox when off (if supported)
};

class Context {
 public:
  // Creates the instance, picks a high-performance adapter and a device.
  // Returns false (with error() set) if WebGPU is unavailable.
  bool Initialize();

  // Creates and configures a presentation surface for an SDL window.
  bool AttachWindow(SDL_Window* window, const SurfaceOptions& options);

  // Reconfigures the surface, e.g. after a resize or a VSync change.
  void ConfigureSurface(uint32_t width, uint32_t height, const SurfaceOptions& options);

  // Current backbuffer, or a null texture if it is not available this frame
  // (minimized, outdated). Call Present() after submitting work that uses it.
  wgpu::Texture AcquireBackbuffer();
  void Present();

  const wgpu::Instance& instance() const { return instance_; }
  const wgpu::Adapter& adapter() const { return adapter_; }
  const wgpu::Device& device() const { return device_; }
  const wgpu::Queue& queue() const { return queue_; }
  wgpu::TextureFormat surface_format() const { return surface_format_; }
  uint32_t width() const { return width_; }
  uint32_t height() const { return height_; }
  const std::string& adapter_name() const { return adapter_name_; }
  const std::string& error() const { return error_; }

 private:
  wgpu::Instance instance_;
  wgpu::Adapter adapter_;
  wgpu::Device device_;
  wgpu::Queue queue_;
  wgpu::Surface surface_;
  wgpu::TextureFormat surface_format_ = wgpu::TextureFormat::Undefined;
  uint32_t width_ = 0;
  uint32_t height_ = 0;
  std::string adapter_name_;
  std::string error_;
};

}  // namespace trials_hd::webgpu
