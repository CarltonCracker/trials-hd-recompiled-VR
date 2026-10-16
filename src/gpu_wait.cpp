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

#if defined(__x86_64__) || defined(_M_X64)
#include <immintrin.h>
#define REX_CPU_PAUSE() _mm_pause()
#elif defined(__aarch64__) || defined(_M_ARM64)
#define REX_CPU_PAUSE() __asm__ __volatile__("isb" ::: "memory")
#else
#define REX_CPU_PAUSE() ((void)0)
#endif

namespace {
// Progressive backoff: spin with CPU pause for short waits, yield to let the
// command processor finish without kernel sleep latency, and only sleep if the GPU
// is stalled for a prolonged duration.
constexpr uint32_t kPauseIterations = 256;
constexpr uint32_t kYieldIterations = 4096;
constexpr uint32_t kSpinYieldIterations = 65536;
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
  ++g_wait_iterations;
  if (g_wait_iterations <= kPauseIterations) {
    REX_CPU_PAUSE();
  } else if (g_wait_iterations <= kYieldIterations) {
    std::this_thread::yield();
  } else if (g_wait_iterations <= kSpinYieldIterations) {
    for (int i = 0; i < 8; ++i) {
      REX_CPU_PAUSE();
    }
    std::this_thread::yield();
  } else {
    std::this_thread::sleep_for(kSleep);
  }
}
