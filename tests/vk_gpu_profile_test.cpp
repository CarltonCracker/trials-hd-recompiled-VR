// Bookkeeping of the vulkan_gpu_profile timestamp profiler (no Vulkan device needed):
// query slots are reused only after their submission completed, failed recordings keep
// their slot, and intervals are charged to the right category, role and pass split.
#include <rex/graphics/vulkan/deferred_command_buffer.h>

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <vector>

using rex::graphics::vulkan::ExternalComputeRole;
using Profiler = rex::graphics::vulkan::VulkanGpuProfiler;

#define CHECK(condition)                                                  \
  do {                                                                    \
    if (!(condition)) {                                                   \
      std::cerr << "vk_gpu_profile check failed, line " << __LINE__ << ": " \
                << #condition << "\n";                                    \
      std::exit(1);                                                       \
    }                                                                     \
  } while (0)

static bool Near(double a, double b) { return std::abs(a - b) < 1e-9; }

static void SlotLifecycle() {
  Profiler p;
  CHECK(!p.AnySlotInFlight());
  // Disabled path: nothing pending, submitting is a no-op.
  p.SlotSubmitted(1);
  CHECK(!p.AnySlotInFlight());

  uint32_t slot = p.AcquireSlot();
  CHECK(slot == 0);
  CHECK(!p.IsSlotFree(0));
  // Recording failed before vkQueueSubmit: the retry reuses the pending slot.
  CHECK(p.AcquireSlot() == 0);
  p.SlotSubmitted(5);
  CHECK(p.AnySlotInFlight());
  CHECK(!p.IsSlotFree(0));
  // An in-flight slot is never handed out again.
  CHECK(p.AcquireSlot() == 1);
  p.ReleasePendingSlot();  // turned off before submitting: slot 1 is free again
  CHECK(p.IsSlotFree(1));

  int harvested = 0;
  p.RetireCompleted(4, [&](uint32_t) { ++harvested; });
  CHECK(harvested == 0);
  CHECK(!p.IsSlotFree(0));
  p.RetireCompleted(5, [&](uint32_t s) {
    CHECK(s == 0);
    ++harvested;
  });
  CHECK(harvested == 1);
  CHECK(p.IsSlotFree(0));
  CHECK(!p.AnySlotInFlight());
}

static void RingExhaustion() {
  Profiler p;
  // Submissions 10, 9, ... so the slot order differs from the completion order.
  for (uint32_t i = 0; i < Profiler::kSlotCount; ++i) {
    CHECK(p.AcquireSlot() == i);
    p.SlotSubmitted(10 + Profiler::kSlotCount - i);
  }
  CHECK(p.AcquireSlot() == Profiler::kNoSlot);
  CHECK(p.window().skipped == 1);
  std::vector<uint32_t> order;
  p.RetireCompleted(10 + 3, [&](uint32_t s) { order.push_back(s); });
  // Submissions 11, 12, 13 live in slots 15, 14, 13 and retire oldest first.
  CHECK(order.size() == 3);
  CHECK(order[0] == 15 && order[1] == 14 && order[2] == 13);
  CHECK(p.AcquireSlot() == 13);
  CHECK(p.IsSlotFree(14) && p.IsSlotFree(15) && !p.IsSlotFree(12));
}

static void Attribution() {
  Profiler p;
  p.SetTimestampFormat(2.0, 64);
  std::vector<uint32_t> tags(Profiler::kMaxQueriesPerSlot);
  uint32_t slot = p.AcquireSlot();
  p.BeginRecording(slot, tags.data());
  CHECK(p.Mark(Profiler::kCategoryOther) == 0);
  const uint64_t key = 0x1234;
  CHECK(p.MarkPassBegin(key) == 1);
  for (int i = 0; i < 10; ++i) p.MarkDraw();
  p.MarkPassEnd();
  p.MarkPassEnd();  // no pass open: ignored
  CHECK(tags[1] == Profiler::kCategoryPassBig);
  CHECK(p.MarkBarrier(1, 2) == 2);
  CHECK(p.MarkPassBegin(key) == 3);  // same attachments after a barrier: split r2
  p.MarkDraw();
  p.MarkPassEnd();
  CHECK(tags[3] == Profiler::kCategoryPassSmall);
  p.MarkBindCompute(ExternalComputeRole::kTextureLoad);
  CHECK(p.MarkDispatch() == 4);
  CHECK(p.MarkCopyBuffer(100) == 5);
  CHECK(p.MarkPassBegin(key) == 6);  // after a dispatch and a copy: split r5
  p.MarkPassEnd();
  CHECK(tags[6] == Profiler::kCategoryPassEmpty);
  CHECK(p.EndRecording() == 7);
  CHECK(p.slot_query_count(slot) == 8);
  p.SlotSubmitted(1);

  const uint64_t ts[] = {100, 110, 130, 135, 145, 160, 161, 171};
  p.RetireCompleted(1, [&](uint32_t s) { p.Accumulate(s, tags.data(), ts); });
  const Profiler::Window& w = p.window();
  CHECK(Near(w.category_ns[Profiler::kCategoryOther], 20));
  CHECK(Near(w.category_ns[Profiler::kCategoryPassBig], 40));
  CHECK(Near(w.category_ns[Profiler::kCategoryBarrier], 10));
  CHECK(Near(w.category_ns[Profiler::kCategoryPassSmall], 20));
  CHECK(Near(w.category_ns[Profiler::kCategoryDispatch], 30));
  CHECK(Near(w.category_ns[Profiler::kCategoryCopy], 2));
  CHECK(Near(w.category_ns[Profiler::kCategoryPassEmpty], 20));
  CHECK(Near(w.total_ns, 142));
  CHECK(Near(w.role_ns[size_t(ExternalComputeRole::kTextureLoad)], 30));
  CHECK(w.role_count[size_t(ExternalComputeRole::kTextureLoad)] == 1);
  CHECK(w.category_count[Profiler::kCategoryPassBig] == 1);
  const Profiler::Counters& c = w.counters;
  CHECK(c.draws == 11 && c.passes == 3 && c.split_passes == 2);
  CHECK(c.split_reasons[2] == 1 && c.split_reasons[5] == 1);
  CHECK(c.pass_draws[0] == 1 && c.pass_draws[1] == 1 && c.pass_draws[2] == 1);
  CHECK(c.buffer_barriers == 1 && c.image_barriers == 2);
  CHECK(c.copy_buffer == 1 && c.copy_buffer_bytes == 100 && c.truncated == 0);
  CHECK(w.submissions == 1);

  // A dropped slot (results unavailable) contributes nothing but the drop count.
  CHECK(p.AcquireSlot() == slot);
  p.BeginRecording(slot, tags.data());
  p.Mark(Profiler::kCategoryOther);
  p.MarkDraw();
  p.EndRecording();
  p.SlotSubmitted(2);
  p.RetireCompleted(2, [&](uint32_t) { p.Drop(); });
  CHECK(p.window().dropped == 1 && p.window().counters.draws == 11);
  CHECK(p.window().submissions == 1);
  p.ResetWindow();
  CHECK(p.window().total_ns == 0 && p.window().counters.draws == 0);
}

static void TimestampBits() {
  Profiler p;
  p.SetTimestampFormat(1.0, 36);
  const uint64_t mask = (uint64_t(1) << 36) - 1;
  std::vector<uint32_t> tags(Profiler::kMaxQueriesPerSlot);
  uint32_t slot = p.AcquireSlot();
  p.BeginRecording(slot, tags.data());
  p.Mark(Profiler::kCategoryBarrier);
  p.Mark(Profiler::kCategoryCopy);
  p.EndRecording();
  // Wraps around the 36-bit counter, then goes backwards (garbage, counted as 0).
  const uint64_t ts[] = {mask - 5, 4, 2};
  p.Accumulate(slot, tags.data(), ts);
  CHECK(Near(p.window().category_ns[Profiler::kCategoryBarrier], 10));
  CHECK(Near(p.window().category_ns[Profiler::kCategoryCopy], 0));
}

static void Truncation() {
  Profiler p;
  std::vector<uint32_t> tags(Profiler::kMaxQueriesPerSlot);
  uint32_t slot = p.AcquireSlot();
  p.BeginRecording(slot, tags.data());
  for (uint32_t i = 0; i < Profiler::kMaxQueriesPerSlot - 1; ++i) {
    CHECK(p.MarkBarrier(0, 1) == i);
  }
  CHECK(p.MarkDispatch() == Profiler::kNoQuery);
  CHECK(p.MarkPassBegin(1) == Profiler::kNoQuery);
  p.MarkDraw();
  p.MarkPassEnd();  // the pass had no query: nothing to retag
  CHECK(p.EndRecording() == Profiler::kMaxQueriesPerSlot - 1);
  CHECK(p.slot_query_count(slot) == Profiler::kMaxQueriesPerSlot);
  CHECK(tags[Profiler::kMaxQueriesPerSlot - 1] == Profiler::kCategoryOther);
  std::vector<uint64_t> ts(Profiler::kMaxQueriesPerSlot);
  for (size_t i = 0; i < ts.size(); ++i) ts[i] = i;
  p.SlotSubmitted(1);
  p.RetireCompleted(1, [&](uint32_t s) { p.Accumulate(s, tags.data(), ts.data()); });
  CHECK(p.window().counters.truncated == 2);
  CHECK(p.window().category_count[Profiler::kCategoryBarrier] ==
        Profiler::kMaxQueriesPerSlot - 1);
  CHECK(p.window().counters.image_barriers == Profiler::kMaxQueriesPerSlot - 1);
}

int main() {
  SlotLifecycle();
  RingExhaustion();
  Attribution();
  TimestampBits();
  Truncation();
  std::cout << "vk_gpu_profile: all checks passed\n";
  return 0;
}
