// Synthetic host callbacks only: no game, graphics device or captured inputs.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <time.h>

#include <rex/logging.h>
#include <rex/thread.h>

#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL %s:%d: %s\n", \
  __FILE__, __LINE__, #x); std::exit(1); } } while (0)

using namespace rex::thread;
using namespace std::chrono_literals;

uint64_t CpuNs() {
  timespec t{};
  CHECK(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &t) == 0);
  return uint64_t(t.tv_sec) * 1000000000 + t.tv_nsec;
}

int main() {
  rex::InitLogging();
  auto* current = Thread::GetCurrentThread();
  auto signaled = Event::CreateManualResetEvent(true);
  auto never = Event::CreateManualResetEvent(false);
  CHECK(Wait(signaled.get(), true, 0ms) == WaitResult::kSuccess);
  CHECK(Wait(never.get(), true, 0ms) == WaitResult::kTimeout);
  unsigned before = 0;
  current->QueueUserCallback([&] { ++before; });
  CHECK(Wait(signaled.get(), true, 0ms) == WaitResult::kUserCallback);
  CHECK(before == 1);
  CHECK(Wait(signaled.get(), true, 0ms) == WaitResult::kSuccess);

  // Exercise registration/notify/teardown for every public alertable wait form.
  double max_latency_ms = 0;
  for (int mode = 0; mode != 4; ++mode) {
    auto ready = Event::CreateAutoResetEvent(false);
    auto signal = Event::CreateAutoResetEvent(false);
    auto second = Event::CreateManualResetEvent(false);
    std::atomic<unsigned> callbacks{0};
    auto target = Thread::Create({}, [&] {
      ready->Set();
      WaitHandle* objects[] = {never.get(), second.get()};
      WaitResult result;
      if (mode == 0) {
        result = Wait(never.get(), true);
      } else if (mode == 1) {
        result = SignalAndWait(signal.get(), never.get(), true);
      } else {
        result = WaitMultiple(objects, 2, mode == 3, true).first;
      }
      CHECK(result == WaitResult::kUserCallback);
      CHECK(callbacks == 2);
    });
    CHECK(target);
    CHECK(Wait(ready.get(), false, 2s) == WaitResult::kSuccess);
    std::this_thread::sleep_for(30ms);
    auto start = std::chrono::steady_clock::now();
    target->QueueUserCallback([&] {
      ++callbacks;
      CHECK(Wait(signaled.get(), true, 0ms) == WaitResult::kSuccess);
      // A nested wait must restore the outer alertable state.
      Thread::GetCurrentThread()->QueueUserCallback([&] { ++callbacks; });
      CHECK(callbacks == 2);
    });
    CHECK(Wait(target.get(), false, 2s) == WaitResult::kSuccess);
    auto latency = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - start).count();
    max_latency_ms = std::max(max_latency_ms, latency);
    CHECK(latency < 50);
    if (mode == 1) { CHECK(Wait(signal.get(), false, 0ms) == WaitResult::kSuccess); }
  }

  // Interruption must not consume any wait-all semaphore/event state.
  auto semaphore = Semaphore::Create(1, 1);
  auto ready = Event::CreateAutoResetEvent(false);
  auto target = Thread::Create({}, [&] {
    WaitHandle* objects[] = {semaphore.get(), never.get()};
    ready->Set();
    CHECK(WaitMultiple(objects, 2, true, true).first == WaitResult::kUserCallback);
  });
  CHECK(Wait(ready.get(), false, 2s) == WaitResult::kSuccess);
  target->QueueUserCallback([] {});
  CHECK(Wait(target.get(), false, 2s) == WaitResult::kSuccess);
  CHECK(Wait(semaphore.get(), false, 0ms) == WaitResult::kSuccess);
  CHECK(Wait(semaphore.get(), false, 0ms) == WaitResult::kTimeout);

  // Hundreds of queue/register races: every callback runs once, in FIFO order.
  constexpr unsigned count = 1000;
  std::atomic<unsigned> completed{0};
  target = Thread::Create({}, [&] {
    while (completed != count) {
      CHECK(Wait(never.get(), true, 2s) == WaitResult::kUserCallback);
    }
  });
  for (unsigned i = 0; i < count; ++i) {
    target->QueueUserCallback([&, i] { CHECK(completed == i); ++completed; });
  }
  CHECK(Wait(target.get(), false, 3s) == WaitResult::kSuccess);
  CHECK(completed == count);

  // Real signaling wakes single/multiple waits; timeout remains a deadline.
  for (bool all : {false, true}) {
    auto a = Event::CreateManualResetEvent(false);
    auto b = Event::CreateManualResetEvent(true);
    target = Thread::Create({}, [&] {
      WaitHandle* objects[] = {a.get(), b.get()};
      auto result = WaitMultiple(objects, 2, all, true, 2s);
      CHECK(result.first == WaitResult::kSuccess);
      CHECK(result.second == (all ? 0u : 1u));
    });
    if (all) { a->Set(); }
    CHECK(Wait(target.get(), false, 3s) == WaitResult::kSuccess);
  }
  auto contested = Event::CreateManualResetEvent(false);
  std::atomic<bool> stop{false};
  target = Thread::Create({}, [&] {
    while (!stop) { contested->Set(); contested->Reset(); }
  });
  for (unsigned i = 0; i < 1000; ++i) {
    auto result = Wait(contested.get(), true, 0ms);
    CHECK(result == WaitResult::kSuccess || result == WaitResult::kTimeout);
  }
  stop = true;
  CHECK(Wait(target.get(), false, 2s) == WaitResult::kSuccess);
  uint64_t idle_ns = 0;
  target = Thread::Create({}, [&] {
    const uint64_t start = CpuNs();
    CHECK(Wait(never.get(), true, 150ms) == WaitResult::kTimeout);
    idle_ns = CpuNs() - start;
  });
  CHECK(Wait(target.get(), false, 2s) == WaitResult::kSuccess);
  CHECK(idle_ns < 3000000);
  uint64_t short_wait_ns = 0;
  target = Thread::Create({}, [&] {
    const uint64_t start = CpuNs();
    for (unsigned i = 0; i < 200; ++i) {
      CHECK(Wait(never.get(), true, 1ms) == WaitResult::kTimeout);
    }
    short_wait_ns = CpuNs() - start;
  });
  CHECK(Wait(target.get(), false, 2s) == WaitResult::kSuccess);
  CHECK(short_wait_ns < 10000000);  // no last-submillisecond deadline spin
  std::printf("PASS: single/signal/multiple APC waits, nested state, FIFO stress; "
              "idle CPU %.3f ms / 150 ms, 200 short waits %.3f ms CPU, max callback latency %.3f ms\n",
              double(idle_ns) / 1000000, double(short_wait_ns) / 1000000, max_latency_ms);
}
