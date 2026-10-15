// Standalone WASM regression checks; uses no generated game code.
#undef NDEBUG
#include <cassert>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <thread>
#include <rex/platform.h>
#include <rex/ppc/context.h>
#include <rex/ppc/intrinsics.h>
#include <rex/chrono/chrono.h>
#include <rex/memory/web_guest.h>

namespace rex::memory::web {
std::atomic<uint64_t> g_dirty[kPageCount / 64]{};
std::atomic<uint64_t> g_dirty_summary[kPageCount / 64 / 64]{};
uint8_t* g_physical_base = nullptr;
}

int main() {
  using namespace rex::memory::web;
  static_assert(REX_PLATFORM_EMSCRIPTEN && sizeof(void*) == 8);
  static_assert(0x7F000000ull + kFold[0x7F] == kPhysicalWindowOffset);
  static_assert(0x90000000ull + kFold[0x90] == 0x80000000ull);
  static_assert(0xA0000000ull + kFold[0xA0] == kPhysicalWindowOffset);
  static_assert(0xC0000000ull + kFold[0xC0] == kPhysicalWindowOffset);
  static_assert(0xE0000000ull + kFold[0xE0] == kPhysicalWindowOffset + 4096);
  static_assert(0xBFFFFFFFull + kFold[0xBF] == kPhysicalWindowOffset + kPhysicalSize - 1);

  MarkHostWrite(nullptr, 1);
  alignas(4096) uint8_t physical[8192]{};
  g_physical_base = physical;
  Zero(physical, 0);
  MarkHostWrite(reinterpret_cast<void*>(uintptr_t(physical) - 1), 4);
  for (auto& word : g_dirty) assert(word.load() == 0);
  MarkHostWrite(physical + 4095, 2);
  assert(g_dirty[0].exchange(0) == 3);
  assert(g_dirty_summary[0].exchange(0) == 1);
  Store32(physical, 0x11223344);
  assert(physical[0] == 0x11 && physical[3] == 0x44);
  assert(g_dirty[0].exchange(0) == 1);
  g_dirty_summary[0].store(0);
  assert(!Cas32(physical, 0, 1) && g_dirty[0].load() == 0);
  assert(Cas32(physical, __builtin_bswap32(0x11223344u), __builtin_bswap32(0x55667788u)));
  assert(physical[0] == 0x55 && g_dirty[0].exchange(0) == 1);
  Store64(physical + 8, 0x0102030405060708ull);
  assert(physical[8] == 1 && physical[15] == 8);
  g_dirty[0].store(0);
  assert(!Cas64(physical + 8, 0, 1) && g_dirty[0].load() == 0);
  assert(Cas64(physical + 8, __builtin_bswap64(0x0102030405060708ull), uint64_t(0)));
  assert(g_dirty[0].exchange(0) == 1);
  Store8(physical, 0xA5);
  Store16(physical + 2, 0xBEEF);
  assert(physical[0] == 0xA5 && physical[2] == 0xBE && physical[3] == 0xEF);
  Store128(physical, simde_mm_set1_epi8(0x5A));
  for (int i = 0; i < 16; ++i) assert(physical[i] == 0x5A);
  Zero(physical, 32);
  for (int i = 0; i < 32; ++i) assert(physical[i] == 0);
  MarkHostWrite(reinterpret_cast<void*>(uintptr_t(physical) + 64 * 4096), 1);
  assert(g_dirty[1].load() == 1 && (g_dirty_summary[0].load() & 2));
  MarkHostWrite(reinterpret_cast<void*>(uintptr_t(physical) + kPhysicalSize - 1), UINT_MAX);
  assert(g_dirty[kPageCount / 64 - 1].load() == (uint64_t(1) << 63));

  PPCCRRegister cr{};
  alignas(16) uint8_t bytes[16];
  std::memset(bytes, 0x80, sizeof(bytes));
  cr.setFromMask(simde_mm_load_si128(reinterpret_cast<simde__m128i*>(bytes)), 0xFFFF);
  assert(cr.lt && !cr.eq);
  alignas(16) uint32_t lanes[4] = {0x80, 0x80, 0x80, 0x80};
  cr.setFromMask(simde_mm_load_ps(reinterpret_cast<float*>(lanes)), 0xF);
  assert(!cr.lt && cr.eq);  // Float-lane mask is zero, but byte mask is not.

  PPCFPSCRRegister fpscr{};
  using FP = rex::platform::FPSCRPlatform;
  auto convert = [](double value) { return FP::CvtsdSi64(simde_mm_set_sd(value)); };
  const int64_t positive[] = {2, 1, 2, 1};
  const int64_t negative[] = {-2, -1, -1, -2};
  for (unsigned mode = 0; mode < 4; ++mode) {
    fpscr.storeFromGuest(mode);
    assert(fpscr.loadFromHost() == mode);
    assert(convert(1.5) == positive[mode] && convert(-1.5) == negative[mode]);
    fpscr.enableFlushMode();
    assert((FP::getcsr() & FP::FlushMask) != 0);
    fpscr.disableFlushMode();
    assert((FP::getcsr() & FP::FlushMask) == 0);
  }
  fpscr.storeFromGuest(0);
  assert(convert(2.5) == 2 && convert(-2.5) == -2);
  assert(convert(std::numeric_limits<double>::quiet_NaN()) == INT64_MIN);
  assert(convert(std::numeric_limits<double>::infinity()) == INT64_MIN);
  assert(convert(9223372036854775808.0) == INT64_MIN);
  fpscr.storeFromGuest(1);
  std::thread other([] { assert(FP::getcsr() == 0); FP::setcsr(2); });
  other.join();
  assert(fpscr.loadFromHost() == 1);  // Shadow rounding state is thread-local.

  alignas(16) uint8_t sequence[16], shifted[16];
  for (int i = 0; i < 16; ++i) sequence[i] = i + 1;
  auto vector = simde_mm_load_si128(reinterpret_cast<simde__m128i*>(sequence));
  auto shift = simde_mm_setzero_si128();
  assert(simde_mm_movemask_epi8(simde_mm_cmpeq_epi8(simde_mm_vsl(vector, shift), vector)) == 0xFFFF);
  shift = simde_mm_insert_epi8(shift, 8, 0);
  simde_mm_store_si128(reinterpret_cast<simde__m128i*>(shifted), simde_mm_vslo(vector, shift));
  assert(shifted[0] == 0 && shifted[1] == 1 && shifted[15] == 15);
  simde_mm_store_si128(reinterpret_cast<simde__m128i*>(shifted), simde_mm_vsro(vector, shift));
  assert(shifted[0] == 2 && shifted[14] == 16 && shifted[15] == 0);
  std::puts("SDK web header checks PASSED: folds, stores, dirty bits, masks, rounding, TLS, VMX");
}
