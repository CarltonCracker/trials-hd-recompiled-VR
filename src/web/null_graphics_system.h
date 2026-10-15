// trials_hd - web build: a graphics system that runs the guest's GPU command
// stream without drawing (docs/web-port/README.md milestone WM1: the runtime
// boots in the browser before the WebGPU renderer is ported, W-L06/W-L07).

#pragma once

#if !defined(__EMSCRIPTEN__)
#error "null_graphics_system.h is for the web build only"
#endif

namespace trials_hd::web {

// Registers "null" with rex::system::RegisterStaticGpuPlugin.
void RegisterNullGpuPlugin();

}  // namespace trials_hd::web
