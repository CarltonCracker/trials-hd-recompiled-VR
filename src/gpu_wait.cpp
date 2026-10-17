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

#if defined(__linux__)
#include <sys/prctl.h>
#endif

namespace {
#if defined(__linux__)
// The game and the command processor hand work back and forth many times per
// frame through short sleeps (these waits, WAIT_REG_MEM naps, event waits).
// Linux's default 50 us timer slack stretches every such sleep well past what
// was asked; with 1 us they wake on time. Set on the main thread before any
// other thread exists, so all threads inherit it.
[[maybe_unused]] const int g_timer_slack = prctl(PR_SET_TIMERSLACK, 1000UL, 0, 0, 0);
#endif
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
