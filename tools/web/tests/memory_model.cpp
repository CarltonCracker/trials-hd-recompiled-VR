// W-L04 check: the SDK's guest memory (rex::memory::Memory) with the web memory
// model, built for wasm64 and run under Node. No game content.
//
// - Writes through the 0xA0000000, 0xC0000000 and 0xE0000000 views, 0x7F000000
//   and TranslatePhysical land in the same physical bytes; 0x9xxxxxxx aliases
//   0x8xxxxxxx; heaps, TranslateVirtual, GuestPtr and the recompiler's
//   base + addr + fold agree for every 16 MiB block.
// - Dirty bits match the writes (store helpers, Memory::Zero/Fill/Copy,
//   MarkGuestWrite); virtual memory is never marked.
// - DeliverPhysicalWrites calls the invalidation callbacks for watched dirty
//   pages only, unwatches them (one-shot, as a native fault), and loses no
//   write under concurrent writers.

#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <random>
#include <thread>
#include <vector>

#include <rex/memory/web_guest.h>
#include <rex/system/xmemory.h>

using namespace rex::memory;
using namespace rex::memory::web;

#define CHECK(condition)                                                       \
  do {                                                                         \
    if (!(condition)) {                                                        \
      std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition); \
      std::abort();                                                            \
    }                                                                          \
  } while (0)

namespace {

uint32_t DirtyPageCount() {
  uint32_t count = 0;
  for (auto& word : g_dirty) {
    count += uint32_t(std::popcount(word.load()));
  }
  return count;
}

bool IsDirty(uint32_t physical_address) {
  uint32_t page = physical_address >> kPageShift;
  return (g_dirty[page >> 6].load() >> (page & 63)) & 1;
}

bool SummaryCoversDirty() {
  for (uint32_t word = 0; word < kPageCount / 64; ++word) {
    if (g_dirty[word].load() &&
        !((g_dirty_summary[word >> 6].load() >> (word & 63)) & 1)) {
      return false;
    }
  }
  return true;
}

struct Invalidation {
  uint32_t start;
  uint32_t length;
};
std::vector<Invalidation> invalidations;

std::pair<uint32_t, uint32_t> RecordInvalidation(void* context, uint32_t start, uint32_t length,
                                                 bool exact_range) {
  (void)context;
  (void)exact_range;
  invalidations.push_back({start, length});
  // Only what was invalidated may be unwatched (SharedMemory's contract).
  return {start, length};
}

bool Invalidated(uint32_t physical_address) {
  for (const Invalidation& i : invalidations) {
    if (physical_address - i.start < i.length) {
      return true;
    }
  }
  return false;
}

// Stress: every page's last write must reach a callback. One stress page per
// bitmap word (64 pages apart), so a lost summary bit is never healed by a
// neighbour's write.
constexpr uint32_t kStressPages = 256;
constexpr uint32_t kStressStride = 64 * 4096;
constexpr uint32_t kStressSize = kStressPages * kStressStride;
std::atomic<uint32_t> stress_sequence[kStressPages];
uint32_t stress_delivered[kStressPages];
uint32_t stress_base_physical = 0;

std::pair<uint32_t, uint32_t> RecordStress(void* context, uint32_t start, uint32_t length,
                                           bool exact_range) {
  (void)context;
  (void)exact_range;
  for (uint32_t delta = 0; delta < length; delta += 4096) {
    uint32_t offset = start + delta - stress_base_physical;
    uint32_t page = offset / kStressStride;
    if (offset % kStressStride < 4096 && page < kStressPages) {
      stress_delivered[page] = stress_sequence[page].load(std::memory_order_acquire);
    }
  }
  return {start, length};
}

}  // namespace

int main() {
  static_assert(REX_PLATFORM_EMSCRIPTEN && sizeof(void*) == 8);

  Memory memory;
  CHECK(memory.Initialize());
  uint8_t* base = memory.virtual_membase();
  CHECK((reinterpret_cast<uintptr_t>(base) & 0xFFFF) == 0);
  CHECK(memory.physical_membase() == base + kPhysicalWindowOffset);
  CHECK(g_physical_base == memory.physical_membase());
  CHECK(AllocateGuestWindow() == base);
  // Initialize writes only virtual (XEX) memory.
  CHECK(DirtyPageCount() == 0);
  CHECK(*memory.TranslateVirtual<uint32_t*>(0x8000001C) == __builtin_bswap32(0x2A6E3F38u));

  // Every 16 MiB block: one translation everywhere.
  for (uint64_t block = 0; block < 256; ++block) {
    for (uint32_t low : {0x0u, 0x123450u, 0xFFFFF0u}) {
      uint32_t address = uint32_t(block << 24) | low;
      uint8_t* folded = base + (int64_t(address) + kFold[address >> 24]);
      CHECK(memory.TranslateVirtual(address) == folded);
      CHECK(GuestPtr(base, address) == folded);
      const BaseHeap* heap = memory.LookupHeap(address);
      if (heap) {
        CHECK(heap->TranslateRelative(address - heap->heap_base()) == folded);
      }
      // The window (and its 64 KiB tail) holds every folded address.
      CHECK(folded >= base && folded + 16 <= base + kWindowSize + 0x10000);
    }
  }

  // Physical memory through each view.
  BaseHeap* heap_a0 = memory.LookupHeapByType(true, 64 * 1024);
  BaseHeap* heap_c0 = memory.LookupHeapByType(true, 16 * 1024 * 1024);
  BaseHeap* heap_e0 = memory.LookupHeapByType(true, 4096);
  CHECK(heap_a0->heap_base() == 0xA0000000 && heap_c0->heap_base() == 0xC0000000 &&
        heap_e0->heap_base() == 0xE0000000);
  uint32_t a0 = 0;
  CHECK(heap_a0->Alloc(0x40000, 0x10000, kMemoryAllocationReserve | kMemoryAllocationCommit,
                       kMemoryProtectRead | kMemoryProtectWrite, false, &a0));
  CHECK(a0 >= 0xA0000000 && a0 < 0xC0000000);
  uint32_t physical = memory.GetPhysicalAddress(a0);
  CHECK(physical == a0 - 0xA0000000);
  uint32_t c0 = 0xC0000000 + physical;
  uint32_t e0 = 0xE0000000 + physical - 0x1000;
  CHECK(memory.GetPhysicalAddress(c0) == physical && memory.GetPhysicalAddress(e0) == physical);
  CHECK(memory.TranslateVirtual(a0) == memory.TranslatePhysical(physical));
  CHECK(memory.TranslateVirtual(c0) == memory.TranslatePhysical(physical));
  CHECK(memory.TranslateVirtual(e0) == memory.TranslatePhysical(physical));

  Store32(memory.TranslateVirtual(a0 + 0x10), 0x11223344);
  Store16(GuestPtr(base, c0 + 0x20), 0x5566);
  Store64(base + (int64_t(e0 + 0x30) + kFold[(e0 + 0x30) >> 24]), 0x0102030405060708ull);
  uint8_t* host = memory.TranslatePhysical(physical);
  CHECK(__builtin_bswap32(*reinterpret_cast<uint32_t*>(host + 0x10)) == 0x11223344);
  CHECK(__builtin_bswap16(*reinterpret_cast<uint16_t*>(host + 0x20)) == 0x5566);
  CHECK(__builtin_bswap64(*reinterpret_cast<uint64_t*>(host + 0x30)) == 0x0102030405060708ull);
  CHECK(*memory.TranslateVirtual<uint32_t*>(e0 + 0x10) == *memory.TranslateVirtual<uint32_t*>(a0 + 0x10));
  CHECK(*memory.TranslateVirtual<uint64_t*>(a0 + 0x30) == *memory.TranslateVirtual<uint64_t*>(c0 + 0x30));
  // All three writes are in one physical page.
  CHECK(IsDirty(physical) && DirtyPageCount() == 1 && SummaryCoversDirty());

  // GPU writeback range: 0x7F000000 is physical 0.
  *reinterpret_cast<uint32_t*>(memory.TranslatePhysical(0x100)) = 0xCAFEF00D;
  CHECK(*memory.TranslateVirtual<uint32_t*>(0x7F000100) == 0xCAFEF00D);
  CHECK(*memory.TranslateVirtual<uint32_t*>(0xA0000100) == 0xCAFEF00D);
  CHECK(memory.LookupHeap(0x7F000100) == nullptr);

  // 0x9xxxxxxx aliases 0x8xxxxxxx; virtual memory is never marked.
  uint32_t before = DirtyPageCount();
  Store32(memory.TranslateVirtual(0x90000040), 0xDEADBEEF);
  CHECK(*memory.TranslateVirtual<uint32_t*>(0x80000040) == __builtin_bswap32(0xDEADBEEFu));
  uint32_t v40 = 0;
  BaseHeap* heap_v40 = memory.LookupHeapByType(false, 64 * 1024);
  CHECK(heap_v40->Alloc(0x10000, 0x10000, kMemoryAllocationReserve | kMemoryAllocationCommit,
                        kMemoryProtectRead | kMemoryProtectWrite, false, &v40));
  Store128(memory.TranslateVirtual(v40), simde_mm_set1_epi32(7));
  memory.Fill(v40 + 0x100, 0x2000, 0xAB);
  CHECK(DirtyPageCount() == before);

  // Reverse translation names physical memory by its 0xA0000000 view.
  CHECK(memory.HostToGuestVirtual(memory.TranslateVirtual(e0 + 4)) == a0 + 4);
  CHECK(memory.HostToGuestVirtual(memory.TranslateVirtual(c0 + 8)) == a0 + 8);
  CHECK(memory.HostToGuestVirtual(memory.TranslateVirtual(v40 + 12)) == v40 + 12);
  CHECK(memory.HostToGuestVirtual(memory.TranslateVirtual(0x82000000)) == 0x82000000);

  // Host writers mark every page they cover.
  memory.Zero(c0 + 0x1FF0, 0x20);  // two pages
  CHECK(IsDirty(physical + 0x1000) && IsDirty(physical + 0x2000) && !IsDirty(physical + 0x3000));
  memory.Fill(e0 + 0x3000, 1, 0x5A);
  CHECK(IsDirty(physical + 0x3000) && *memory.TranslatePhysical(physical + 0x3000) == 0x5A);
  memory.Copy(a0 + 0x8000, v40 + 0x100, 0x1001);  // two pages from virtual memory
  CHECK(IsDirty(physical + 0x8000) && IsDirty(physical + 0x9000) && !IsDirty(physical + 0xA000));
  CHECK(*memory.TranslatePhysical(physical + 0x9000) == 0xAB);
  MarkGuestWrite(e0 + 0xC000, 0);
  CHECK(!IsDirty(physical + 0xC000));
  MarkGuestWrite(0xA0000000 + physical + 0xCFFF, 2);
  CHECK(IsDirty(physical + 0xC000) && IsDirty(physical + 0xD000));
  Zero(memory.TranslateVirtual(a0 + 0xE000), 32);  // dcbz
  CHECK(IsDirty(physical + 0xE000));
  CHECK(!Cas32(memory.TranslateVirtual(a0 + 0xF000), 1, 2) && !IsDirty(physical + 0xF000));
  CHECK(Cas32(memory.TranslateVirtual(a0 + 0xF000), 0, 2) && IsDirty(physical + 0xF000));
  CHECK(SummaryCoversDirty());
  // Exactly the pages written: 0 (stores), 1-2 (Zero), 3 (Fill), 8-9 (Copy),
  // C-D (MarkGuestWrite), E (dcbz), F (CAS).
  for (uint32_t page : {0x0u, 0x1u, 0x2u, 0x3u, 0x8u, 0x9u, 0xCu, 0xDu, 0xEu, 0xFu}) {
    CHECK(IsDirty(physical + page * 4096));
  }
  uint32_t marked = DirtyPageCount();
  CHECK(marked == 10);

  // Nothing watched: delivery only clears the bits.
  void* callback = memory.RegisterPhysicalMemoryInvalidationCallback(RecordInvalidation, nullptr);
  memory.DeliverPhysicalWrites();
  CHECK(invalidations.empty() && DirtyPageCount() == 0);
  for (auto& word : g_dirty_summary) {
    CHECK(word.load() == 0);
  }

  // Watch the 0x40000 bytes (made through the 0xA0000000 heap). A write through
  // another view reaches it; unwatched dirty pages do not call back.
  memory.EnablePhysicalMemoryAccessCallbacks(physical, 0x40000, true, false);
  Store32(memory.TranslateVirtual(e0 + 0x5004), 1);       // watched, via 0xE0000000
  Store32(memory.TranslatePhysical(physical + 0x50000), 1);  // past the watched range
  memory.DeliverPhysicalWrites();
  CHECK(invalidations.size() == 1 && Invalidated(physical + 0x5000));
  CHECK(!Invalidated(physical + 0x50000) && DirtyPageCount() == 0);
  // One-shot: the delivered page is no longer watched...
  invalidations.clear();
  Store32(memory.TranslateVirtual(a0 + 0x5008), 2);
  memory.DeliverPhysicalWrites();
  CHECK(invalidations.empty());
  // ...until it is watched again. Runs of pages arrive as one range.
  memory.EnablePhysicalMemoryAccessCallbacks(physical, 0x40000, true, false);
  memory.Zero(c0 + 0x6000, 0x3000);
  memory.DeliverPhysicalWrites();
  CHECK(invalidations.size() == 1 && invalidations[0].start <= physical + 0x6000 &&
        invalidations[0].start + invalidations[0].length >= physical + 0x9000);
  // Clean: delivery is a no-op.
  invalidations.clear();
  memory.DeliverPhysicalWrites();
  CHECK(invalidations.empty());
  memory.UnregisterPhysicalMemoryInvalidationCallback(callback);

  // Concurrent writers through all views vs. a consumer that delivers and
  // re-watches, as the GPU front end does. No write may be lost.
  uint32_t stress = 0;
  CHECK(heap_a0->Alloc(kStressSize, 0x10000,
                       kMemoryAllocationReserve | kMemoryAllocationCommit,
                       kMemoryProtectRead | kMemoryProtectWrite, false, &stress));
  stress_base_physical = memory.GetPhysicalAddress(stress);
  void* stress_callback = memory.RegisterPhysicalMemoryInvalidationCallback(RecordStress, nullptr);
  memory.EnablePhysicalMemoryAccessCallbacks(stress_base_physical, kStressSize, true, false);
  std::atomic<bool> writers_done{false};
  std::vector<std::thread> writers;
  constexpr uint32_t kWriters = 4;
  constexpr uint32_t kWritesPerWriter = 200000;
  for (uint32_t w = 0; w < kWriters; ++w) {
    writers.emplace_back([&, w] {
      std::mt19937 random(w + 1);
      const uint32_t view_bases[3] = {0xA0000000, 0xC0000000, 0xE0000000 - 0x1000};
      for (uint32_t i = 0; i < kWritesPerWriter; ++i) {
        uint32_t page = random() % kStressPages;
        uint32_t address = view_bases[random() % 3] + stress_base_physical +
                           page * kStressStride + (random() % 1024) * 4;
        // Bump the page's sequence, then write and mark (Store32 marks after).
        stress_sequence[page].fetch_add(1, std::memory_order_release);
        Store32(memory.TranslateVirtual(address), i);
      }
    });
  }
  std::thread consumer([&] {
    while (!writers_done.load()) {
      memory.DeliverPhysicalWrites();
      memory.EnablePhysicalMemoryAccessCallbacks(stress_base_physical, kStressSize, true, false);
    }
  });
  for (auto& writer : writers) {
    writer.join();
  }
  writers_done = true;
  consumer.join();
  memory.EnablePhysicalMemoryAccessCallbacks(stress_base_physical, kStressSize, true, false);
  memory.DeliverPhysicalWrites();
  uint32_t total = 0;
  for (uint32_t page = 0; page < kStressPages; ++page) {
    uint32_t sequence = stress_sequence[page].load();
    total += sequence;
    if (stress_delivered[page] != sequence) {
      std::fprintf(stderr, "page %u: last write %u, last delivered %u\n", page, sequence,
                   stress_delivered[page]);
      std::abort();
    }
  }
  CHECK(total == kWriters * kWritesPerWriter && DirtyPageCount() == 0);
  memory.UnregisterPhysicalMemoryInvalidationCallback(stress_callback);

  std::printf("W-L04 memory model PASSED: 3 physical views + 0x7F/0x9 aliases agree over 256 "
              "blocks; %u marked pages matched; delivery one-shot; %u concurrent writes, "
              "none lost\n",
              marked, total);
  return 0;
}
