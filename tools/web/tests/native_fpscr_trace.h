// Injected only by build_fpscr_probe.py; never part of the normal native build.
#pragma once
#include <cstdint>
#include <cstdio>
#include <simde/x86/sse.h>

inline void trials_web_trace_setcsr(uint32_t csr) {
  static thread_local unsigned seen_modes = 0;
  unsigned mode = (csr >> 13) & 3;  // x86 MXCSR: nearest, down, up, toward zero.
  if (!(seen_modes & (1u << mode))) {
    seen_modes |= 1u << mode;
    std::fprintf(stderr, "WEB_NATIVE_FPSCR_MODE %u\n", mode);
  }
  simde_mm_setcsr(csr);
}

// SIMD headers were included above, so only subsequent SDK call sites change.
#define simde_mm_setcsr(csr) trials_web_trace_setcsr(csr)
