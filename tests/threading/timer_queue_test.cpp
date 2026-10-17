// Exercise the real queue without linking a second process-global runtime queue.
#include <rex/thread/timer_queue.h>
#include <disruptorplus/blocking_wait_strategy.hpp>

#include <cassert>
#include <condition_variable>
#include <ctime>
#include <future>
#include <iostream>
#include <mutex>
#include <string_view>
#include <thread>
#include <vector>

namespace rex::thread {
void set_current_thread_name(std::string_view) {}
}  // namespace rex::thread

using namespace std::chrono_literals;
using namespace rex::thread;
using Clock = TimerQueueWaitItem::clock;

int main() {
  // Instantiate both timed overloads (the ring itself uses absolute deadlines).
  disruptorplus::blocking_wait_strategy strategy;
  std::atomic<disruptorplus::sequence_t> sequence{0};
  const std::atomic<disruptorplus::sequence_t>* sequences[] = {&sequence};
  assert(strategy.wait_until_published(1, 1, sequences, 1ms) == 0);
  assert(strategy.wait_until_published(1, 1, sequences, Clock::now() + 1ms) == 0);

  const auto cpu_start = std::clock();
  std::this_thread::sleep_for(200ms);
  const double idle_ms = 1000.0 * (std::clock() - cpu_start) / CLOCKS_PER_SEC;
  std::cout << "idle queue CPU: " << idle_ms << " ms / 200 ms wall\n";
  assert(idle_ms < 100.0);

  std::atomic<int> ticks{0};
  const auto ticking_start = std::clock();
  auto ticking = QueueTimerRecurring([&](void*) { ++ticks; }, nullptr,
                                     Clock::now() + 1ms, 1ms);
  std::this_thread::sleep_for(200ms);
  ticking.lock()->Disarm();
  const double ticking_ms = 1000.0 * (std::clock() - ticking_start) / CLOCKS_PER_SEC;
  std::cout << "1 kHz queue CPU: " << ticking_ms << " ms / 200 ms wall, " << ticks << " ticks\n";
  assert(ticks >= 10);
  assert(ticking_ms < 100.0);

  // Inserting an earlier timer wakes a queue already sleeping for a later one.
  std::promise<void> late;
  std::promise<Clock::time_point> early;
  const auto start = Clock::now();
  auto late_item = QueueTimerOnce([&](void*) { late.set_value(); }, nullptr, start + 500ms);
  std::this_thread::sleep_for(10ms);
  QueueTimerOnce([&](void*) { early.set_value(Clock::now()); }, nullptr, start + 30ms);
  auto early_result = early.get_future();
  assert(early_result.wait_for(1s) == std::future_status::ready);
  const auto fired = early_result.get();
  assert(fired >= start + 30ms && fired < start + 250ms);
  auto late_live = late_item.lock();
  assert(late_live);
  late_live->Disarm();
  assert(late.get_future().wait_for(550ms) == std::future_status::timeout);

  // Self-disarming recurring callbacks must not be rescheduled.
  std::promise<void> recurring_done, recurring_published;
  auto published = recurring_published.get_future();
  std::atomic<int> recurring_count{0};
  std::weak_ptr<TimerQueueWaitItem> recurring;
  recurring = QueueTimerRecurring([&](void*) {
    published.wait();
    if (++recurring_count == 3) {
      recurring.lock()->Disarm();
      recurring_done.set_value();
    }
  }, nullptr, Clock::now() + 30ms, 5ms);
  recurring_published.set_value();
  assert(recurring_done.get_future().wait_for(1s) == std::future_status::ready);
  std::this_thread::sleep_for(30ms);
  assert(recurring_count == 3);

  // Disarm waits for an in-flight callback before its owner may be destroyed.
  std::promise<void> entered, release;
  std::atomic<bool> callback_exited{false};
  auto released = release.get_future();
  auto in_flight = QueueTimerOnce([&](void*) {
    entered.set_value();
    released.wait();
    callback_exited = true;
  }, nullptr, Clock::now() + 10ms);
  assert(entered.get_future().wait_for(1s) == std::future_status::ready);
  auto item = in_flight.lock();
  assert(item);
  auto disarm = std::async(std::launch::async, [item] { item->Disarm(); });
  assert(disarm.wait_for(20ms) == std::future_status::timeout);
  release.set_value();
  assert(disarm.wait_for(1s) == std::future_status::ready);
  assert(callback_exited);

  // More than the ring capacity, from concurrent producers: no lost wakeups.
  std::atomic<int> callbacks{0};
  std::mutex mutex;
  std::condition_variable cv;
  std::vector<std::jthread> producers;
  for (int i = 0; i < 4; ++i) {
    producers.emplace_back([&] {
      for (int n = 0; n < 512; ++n) {
        QueueTimerOnce([&](void*) {
          std::lock_guard lock(mutex);
          ++callbacks;
          cv.notify_all();
        }, nullptr, Clock::now());
      }
    });
  }
  producers.clear();
  std::unique_lock lock(mutex);
  assert(cv.wait_for(lock, 5s, [&] { return callbacks == 2048; }));
  // Process exit also exercises waking and joining the idle queue.
}
