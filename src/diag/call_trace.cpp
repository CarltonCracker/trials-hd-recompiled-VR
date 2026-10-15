// trials_hd - per-function call counters for reverse engineering.

#include "diag/call_trace.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <rex/logging.h>
#include <rex/ppc/context.h>

namespace trials_hd::diag {

namespace {

constexpr uint32_t kArgSamplesPerSecond = 4;
std::mutex g_args_mutex;
FILE* g_args_out = nullptr;
std::vector<uint32_t> g_arg_samples;  // per function, reset every second
std::atomic<uint32_t> g_second{0};

void EnableArgCapture() {
  const char* list = std::getenv("TRIALS_HD_CALL_TRACE_ARGS");
  const char* path = std::getenv("TRIALS_HD_CALL_TRACE_ARGS_CSV");
  if (!list || !*list || !path || !*path) {
    return;
  }
  g_args_out = std::fopen(path, "w");
  if (!g_args_out) {
    return;
  }
  std::fprintf(g_args_out, "second,address,lr,r3,r4,r5,r6,r7,r8\n");
  g_arg_samples.assign(kTracedCount, 0);
  std::stringstream in(list);
  std::string item;
  while (std::getline(in, item, ',')) {
    const uint32_t address = uint32_t(std::stoul(item, nullptr, 16));
    for (size_t i = 0; i < kTracedCount; ++i) {
      if (kTracedAddresses[i] == address) {
        g_capture_args[i] = true;
      }
    }
  }
}

}  // namespace

void CaptureArgs(size_t index, const ::PPCContext& ctx) {
  std::lock_guard lock(g_args_mutex);
  if (!g_args_out || g_arg_samples[index] >= kArgSamplesPerSecond) {
    return;
  }
  ++g_arg_samples[index];
  std::fprintf(g_args_out, "%u,0x%08X,0x%08X,0x%08X,0x%08X,0x%08X,0x%08X,0x%08X,0x%08X\n",
               g_second.load(), kTracedAddresses[index], uint32_t(ctx.lr), ctx.r3.u32,
               ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32);
}

void StartCallTraceReporter() {
  EnableArgCapture();
  const char* path = std::getenv("TRIALS_HD_CALL_TRACE_CSV");
  if (!path || !*path) {
    return;
  }
  FILE* out = std::fopen(path, "w");
  if (!out) {
    REXLOG_ERROR("call trace: cannot open {}", path);
    return;
  }
  std::fprintf(out, "second,address,calls\n");
  REXLOG_INFO("call trace: {} functions, writing {}", kTracedCount, path);

  std::thread([out] {
    std::vector<uint64_t> previous(kTracedCount, 0);
    auto next = std::chrono::steady_clock::now();
    for (uint32_t second = 0;; ++second) {
      next += std::chrono::seconds(1);
      std::this_thread::sleep_until(next);
      for (size_t i = 0; i < kTracedCount; ++i) {
        const uint64_t now = g_call_counts[i].load(std::memory_order_relaxed);
        if (now != previous[i]) {
          std::fprintf(out, "%u,0x%08X,%llu\n", second, kTracedAddresses[i],
                       static_cast<unsigned long long>(now - previous[i]));
          previous[i] = now;
        }
      }
      std::fflush(out);
      g_second = second + 1;
      std::lock_guard lock(g_args_mutex);
      if (g_args_out) {
        std::fill(g_arg_samples.begin(), g_arg_samples.end(), 0);
        std::fflush(g_args_out);
      }
    }
  }).detach();
}

}  // namespace trials_hd::diag
