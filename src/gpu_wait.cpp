// trials_hd - host-friendly waits for the GPU.
//
// The game's Direct3D runtime waits for GPU fences (frame pacing, ring buffer
// space) by calling its wait-iteration helper sub_8212BC68 in a tight loop -
// GPU hang detection plus a yield hint per iteration, about 700,000 calls per
// frame - which keeps a whole host core busy. After a short spin, each
// iteration now also sleeps a few microseconds: fence progress is still seen
// within tens of microseconds, and the core is free for the command processor
// and the rest of the game.

#include <chrono>
#include <cstdint>
#include <thread>

#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

REX_EXTERN(__imp__sub_8212BB58);
REX_EXTERN(__imp__sub_8212BC68);

namespace {
// Iterations of pure spinning before sleeping (short waits stay as fast as
// before).
constexpr uint32_t kSpinIterations = 256;
constexpr auto kSleep = std::chrono::microseconds(20);
thread_local uint32_t g_wait_iterations = 0;
}  // namespace

// Starts a wait (initializes the wait state the iterations update).
extern "C" REX_FUNC(sub_8212BB58) {
  g_wait_iterations = 0;
  __imp__sub_8212BB58(ctx, base);
}

// One wait iteration; returns 0 when the GPU is considered hung.
extern "C" REX_FUNC(sub_8212BC68) {
  __imp__sub_8212BC68(ctx, base);
  if (++g_wait_iterations > kSpinIterations) {
    std::this_thread::sleep_for(kSleep);
  }
}
