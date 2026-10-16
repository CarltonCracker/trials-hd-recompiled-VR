// rexgpu-webgpu - what the GPU worker is doing, for the web build's stall
// watchdog (WebGpuCommandProcessor::StallWatchdog). No-op natively.

#pragma once

#include <atomic>

namespace rex::graphics::webgpu {

#if defined(__EMSCRIPTEN__)
extern std::atomic<const char*> g_web_phase;
inline void SetWebPhase(const char* phase) {
  g_web_phase.store(phase, std::memory_order_relaxed);
}
#else
inline void SetWebPhase(const char*) {}
#endif

}  // namespace rex::graphics::webgpu
