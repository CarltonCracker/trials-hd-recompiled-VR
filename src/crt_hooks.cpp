// trials_hd - native replacements for the game's CRT memory routines.
//
// The recompiled versions copy and fill byte by byte and word by word; the
// render thread alone spends ~6% of its busy time in the memcpy (shader
// constant and vertex staging). Identified by shape in the CRT block next to
// setjmp/longjmp (config/codegen.toml):
//   sub_822F81A0 memcpy(dst, src, n)   cache-hinted, doubleword copy
//   sub_822F8628 memcpy(dst, src, n)   simple forward byte/word copy
//   sub_822F8040 memset(dst, c, n)     replicates the byte into words
// All return dst in r3, which is left untouched (no host-pointer round trip).
// Writes into write-watched GPU memory fault into the runtime's handler just
// like the recompiled stores do.

#include <cstdint>
#include <cstring>

#include <rex/ppc/context.h>
#include <rex/ppc/func.h>
#include <rex/system/xmemory.h>

extern "C" REX_FUNC(sub_822F81A0) {
  std::memmove(rex::memory::GuestPtr(base, ctx.r3.u32), rex::memory::GuestPtr(base, ctx.r4.u32),
               ctx.r5.u32);
}

// memmove for both copies: as fast as memcpy here, and defined if a caller
// ever overlaps the ranges (the forward guest copy smears only when the
// destination is above the source, which no copy can rely on).
extern "C" REX_FUNC(sub_822F8628) {
  std::memmove(rex::memory::GuestPtr(base, ctx.r3.u32), rex::memory::GuestPtr(base, ctx.r4.u32),
               ctx.r5.u32);
}

extern "C" REX_FUNC(sub_822F8040) {
  std::memset(rex::memory::GuestPtr(base, ctx.r3.u32), int(ctx.r4.u32 & 0xFF), ctx.r5.u32);
}

// sub_822F8E60: strncmp(s1, s2, n)
extern "C" REX_FUNC(sub_822F8E60) {
  if (ctx.r5.u32 == 0) {
    ctx.r3.s64 = 0;
    return;
  }
  const char* s1 = reinterpret_cast<const char*>(rex::memory::GuestPtr(base, ctx.r3.u32));
  const char* s2 = reinterpret_cast<const char*>(rex::memory::GuestPtr(base, ctx.r4.u32));
  ctx.r3.s64 = std::strncmp(s1, s2, ctx.r5.u32);
}

// sub_822F8ED8: strncpy(dst, src, n)
extern "C" REX_FUNC(sub_822F8ED8) {
  if (ctx.r5.u32 == 0) return;
  char* dst = reinterpret_cast<char*>(rex::memory::GuestPtr(base, ctx.r3.u32));
  const char* src = reinterpret_cast<const char*>(rex::memory::GuestPtr(base, ctx.r4.u32));
  std::strncpy(dst, src, ctx.r5.u32);
}

// sub_822F8EB0: __int2double(u64)
extern "C" REX_FUNC(sub_822F8EB0) {
  ctx.f1.f64 = static_cast<double>(ctx.r3.u64);
}

REX_EXTERN(sub_822FDCA8);
REX_EXTERN(sub_822FD658);

// sub_822F7D30: strlen + string descriptor helper
extern "C" REX_FUNC(sub_822F7D30) {
  uint32_t str_addr = ctx.r4.u32;
  uint32_t r4_val = ctx.r5.u32;
  uint32_t r5_val = ctx.r6.u32;
  uint32_t r6_val = ctx.r7.u32;
  uint32_t callback = ctx.r3.u32;

  uint32_t saved_lr = ctx.lr;
  rex::memory::store_and_swap<uint32_t>(base + ctx.r1.u32 - 8, saved_lr);
  uint32_t frame = ctx.r1.u32 - 128;
  rex::memory::store_and_swap<uint32_t>(base + frame, ctx.r1.u32);
  ctx.r1.u32 = frame;

  if (str_addr == 0 || r4_val == 0) {
    ctx.lr = 0x822F7D74;
    sub_822FDCA8(ctx, base);
    rex::memory::store_and_swap<uint32_t>(base + ctx.r3.u32, 22);
    ctx.r3.s64 = 0;
    ctx.r4.s64 = 0;
    ctx.r5.s64 = 0;
    ctx.r6.s64 = 0;
    ctx.r7.s64 = 0;
    ctx.lr = 0x822F7D94;
    sub_822FD658(ctx, base);
    ctx.r3.s64 = -1;
  } else {
    const char* str = reinterpret_cast<const char*>(rex::memory::GuestPtr(base, str_addr));
    size_t len = std::strlen(str);
    uint32_t r9 = uint32_t(std::min<size_t>(len, 0x7FFFFFFFu));

    rex::memory::store_and_swap<uint32_t>(base + frame + 80, str_addr);
    rex::memory::store_and_swap<uint32_t>(base + frame + 88, str_addr);
    rex::memory::store_and_swap<uint32_t>(base + frame + 92, 73);
    rex::memory::store_and_swap<uint32_t>(base + frame + 84, r9);

    ctx.ctr.u64 = callback;
    ctx.r4.u64 = r4_val;
    ctx.r5.u64 = r5_val;
    ctx.r6.u64 = r6_val;
    ctx.r3.s64 = frame + 80;
    ctx.lr = 0x822F7DDC;
    rex::runtime::ResolveIndirectFunction(ctx.ctr.u32)(ctx, base);
  }

  ctx.r1.s64 += 128;
  ctx.lr = rex::memory::load_and_swap<uint32_t>(base + ctx.r1.u32 - 8);
}
