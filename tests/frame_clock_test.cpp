#include "frame_clock.h"

#include <cmath>
#include <cstdlib>
#include <iostream>

using trials_hd::FrameClock;
using namespace std::chrono_literals;

void Check(bool condition) {
  if (!condition) {
    std::cerr << "fixed clock check failed\n";
    std::exit(1);
  }
}

int main() {
  // Ten seconds of wall time produce identical ticks at every render cadence.
  for (int fps : {30, 45, 60, 90, 120, 144, 165, 240, 1000}) {
    FrameClock clock;
    uint32_t ticks = 0;
    float phase = 0;
    auto previous = 0ns;
    for (int frame = 1; frame <= fps * 10; ++frame) {
      auto now = FrameClock::Duration(10s) * frame / (fps * 10);
      const auto steps = clock.Advance(now - previous);
      ticks += steps;
      for (uint32_t i = 0; i < steps; ++i)
        phase = trials_hd::AdvanceScrollPhase(phase, 0.00023f);
      previous = now;
    }
    Check(ticks == 600);
    Check(std::abs(phase - 600 * 0.00023f) < 0.00001f);
  }
  // Low render caps must wake for physics deadlines, not discard fixed ticks.
  for (int fps : {1, 5, 10}) {
    FrameClock clock;
    auto now = 0ns;
    auto next_render = FrameClock::Duration(1s) / fps;
    uint32_t ticks = 0, renders = 0;
    while (now < 10s) {
      auto wake = std::min(next_render, now + clock.UntilNextStep());
      ticks += clock.Advance(wake - now);
      now = wake;
      if (now >= next_render) {
        ++renders;
        next_render += FrameClock::Duration(1s) / fps;
      }
    }
    Check(ticks == 600 && renders == uint32_t(fps * 10));
  }
  Check(trials_hd::AdvanceScrollPhase(0.75f, 0.25f) == 1.0f);
  Check(trials_hd::AdvanceScrollPhase(1.0f, 0.25f) == 0.25f);
  FrameClock clock;
  Check(clock.Advance(0ns) == 0);
  Check(clock.Advance(-1ms) == 0);
  Check(clock.Advance(FrameClock::kStep - 1ns) == 0);
  Check(clock.Advance(1ns) == 1);
  Check(clock.Advance(10s) == FrameClock::kMaxSteps);
  Check(clock.Advance(0ns) == 0);  // No catch-up spiral after a stall.
  Check(clock.Advance(FrameClock::kStep) == 1);
  std::cout << "fixed clock: render-rate independence and hitch checks passed\n";
}
