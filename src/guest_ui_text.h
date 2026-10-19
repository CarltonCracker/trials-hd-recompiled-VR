// trials_hd - call a copying guest UI text setter without changing caller registers.
#pragma once

#include <cstddef>
#include <cstdint>

#include <rex/memory/utils.h>
#include <rex/ppc/context.h>

namespace trials_hd {

template <size_t N>
bool SetGuestControlText(const PPCContext& caller, uint8_t* base, uint32_t control,
                         const char16_t (&text)[N], PPCFunc& setter) {
  constexpr uint32_t kFrameSize = 0x100;
  constexpr uint32_t kTextOffset = 0x80;
  static_assert(sizeof(text) <= kFrameSize - kTextOffset);
  if (!control || caller.r1.u32 < kFrameSize || (caller.r1.u32 & 0xF) || text[N - 1] != 0) {
    return false;
  }

  PPCContext call = caller;
  call.r1.u64 -= kFrameSize;
  rex::memory::store_and_swap<uint32_t>(base + call.r1.u32, caller.r1.u32);
  const uint32_t label = call.r1.u32 + kTextOffset;
  for (size_t i = 0; i < N; ++i) {
    rex::memory::store_and_swap<uint16_t>(base + label + i * 2, uint16_t(text[i]));
  }
  call.r3.u64 = control;
  call.r4.u64 = label;
  setter(call, base);
  // The setter must copy the string before this temporary guest frame expires.
  return call.r3.s32 >= 0;
}

}  // namespace trials_hd
