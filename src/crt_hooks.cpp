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

// Natively a write into GPU-watched memory faults into the runtime's handler;
// on the web these copies mark the pages themselves (docs/web-port 8.3.1).
#if defined(__EMSCRIPTEN__)
#define TRIALSHD_MARK_GUEST_WRITE(address, size) rex::memory::web::MarkGuestWrite(address, size)
#else
#define TRIALSHD_MARK_GUEST_WRITE(address, size) ((void)0)
#endif

extern "C" REX_FUNC(sub_822F81A0) {
  std::memmove(rex::memory::GuestPtr(base, ctx.r3.u32), rex::memory::GuestPtr(base, ctx.r4.u32),
               ctx.r5.u32);
  TRIALSHD_MARK_GUEST_WRITE(ctx.r3.u32, ctx.r5.u32);
}

// memmove for both copies: as fast as memcpy here, and defined if a caller
// ever overlaps the ranges (the forward guest copy smears only when the
// destination is above the source, which no copy can rely on).
extern "C" REX_FUNC(sub_822F8628) {
  std::memmove(rex::memory::GuestPtr(base, ctx.r3.u32), rex::memory::GuestPtr(base, ctx.r4.u32),
               ctx.r5.u32);
  TRIALSHD_MARK_GUEST_WRITE(ctx.r3.u32, ctx.r5.u32);
}

extern "C" REX_FUNC(sub_822F8040) {
  std::memset(rex::memory::GuestPtr(base, ctx.r3.u32), int(ctx.r4.u32 & 0xFF), ctx.r5.u32);
  TRIALSHD_MARK_GUEST_WRITE(ctx.r3.u32, ctx.r5.u32);
}
