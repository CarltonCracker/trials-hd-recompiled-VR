// Trials HD v0.0.1.2: run the original updates independently of real rendering.
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ppc/context.h>
#include <rex/ppc/func.h>

#include "frame_clock.h"

REXCVAR_DEFINE_BOOL(fps_unlock, true, "Display",
                    "Render independently of the original fixed physics tick")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(log_simulation_rate, false, "Display",
                    "Log fixed updates and real render frames once per second");

REX_EXTERN(__imp__sub_82664F98);
REX_EXTERN(sub_82668BA8);
REX_EXTERN(sub_825CB980);
REX_EXTERN(sub_82124BF0);
REX_EXTERN(sub_82124660);
REX_EXTERN(sub_822F65A8);
REX_EXTERN(__imp__sub_825EBF48);
REX_EXTERN(__imp__sub_825E9220);
REX_EXTERN(__imp__sub_825E7FC0);

namespace {
constexpr uint32_t kApplication = 0x827683A0;
constexpr uint32_t kTickCount = 0x8276A400;
constexpr uint32_t kRenderCount = 0x8276A404;
constexpr uint32_t kFloatConstants = 0x820BD300;
constexpr uint32_t kScrollPhase = 0x8276AC60;
constexpr uint32_t kScrollInitialized = 0x8276AC68;

float Float(uint8_t* base, uint32_t address) {
  return rex::memory::load_and_swap<float>(base + address);
}
void StoreFloat(uint8_t* base, uint32_t address, float value) {
  rex::memory::store_and_swap<float>(base + address, value);
}

std::atomic_bool g_unlocked_loop{false};

bool Unlocked() { return g_unlocked_loop.load(); }

uint32_t Game(uint8_t* base) {
  return rex::memory::load_and_swap<uint32_t>(base + kApplication);
}

void CallVirtual(PPCContext& ctx, uint8_t* base, uint32_t object, uint32_t slot,
                 uint32_t argument, uint32_t return_address) {
  if (!object) return;
  const auto table = rex::memory::load_and_swap<uint32_t>(base + object);
  const auto function = rex::memory::load_and_swap<uint32_t>(base + table + slot);
  ctx.r3.u64 = object;
  ctx.r4.u64 = argument;
  ctx.lr = return_address;
  rex::runtime::ResolveIndirectFunction(function)(ctx, base);
}

bool Running(uint8_t* base) {
  return rex::memory::load<uint8_t>(base + Game(base) + 4) != 0;
}

uint32_t Scene(uint8_t* base) {
  return rex::memory::load_and_swap<uint32_t>(base + Game(base) + 20);
}

// The two +20 workers also advance animation/particle time (825F2118).
// Keep all four update workers at fixed rate; only +28 submits real drawing.
void Dispatch(PPCContext& ctx, uint8_t* base, bool render) {
  constexpr uint32_t updates[] = {0x827683B8, 0x827683C4, 0x827683BC, 0x827683C0};
  constexpr uint32_t update_done[] = {0x827683CC, 0x827683D8, 0x827683D0, 0x827683D4};
  constexpr uint32_t renders[] = {0x827683B4};
  constexpr uint32_t render_done[] = {0x827683C8};
  auto call = [&](uint32_t global, uint32_t slot) {
    CallVirtual(ctx, base, rex::memory::load_and_swap<uint32_t>(base + global),
                slot, 0, 0x82663808);
  };
  if (render) {
    for (auto global : renders) call(global, 4);
    for (auto global : render_done) call(global, 0);
    const auto count = rex::memory::load_and_swap<uint32_t>(base + kRenderCount);
    rex::memory::store_and_swap<uint32_t>(base + kRenderCount, count + 1);
  } else {
    for (auto global : updates) call(global, 4);
    for (auto global : update_done) call(global, 0);
  }
}

// Temporal work embedded in 825EBF48/825E7FC0 must also run between low-FPS renders.
void TickVisuals(PPCContext& ctx, uint8_t* base) {
  const auto scene = Scene(base);
  if (!rex::memory::load_and_swap<uint32_t>(base + scene + 12)) return;
  const auto world = rex::memory::load_and_swap<uint32_t>(base + scene + 20);
  const auto mode = rex::memory::load_and_swap<uint32_t>(base + world + 115564);
  if (mode != 3 && !rex::memory::load_and_swap<uint32_t>(base + world + 115448) &&
      rex::memory::load_and_swap<uint32_t>(base + world + 115064) != 2) {
    const float amount = Float(base, kFloatConstants + 224);
    const auto filtered = world + 248760;
    StoreFloat(base, filtered, float(std::fma(double(Float(base, filtered)),
               double(Float(base, kFloatConstants + 784)), double(amount))));
    ctx.r3.u64 = world;
    ctx.f1.f64 = amount;
    ctx.lr = 0x825EC160;
    __imp__sub_825E9220(ctx, base);
  }
  if (mode >= 1 && mode <= 3) {
    auto flags = rex::memory::load_and_swap<uint32_t>(base + kScrollInitialized);
    if (!(flags & 1)) {
      StoreFloat(base, kScrollPhase, 0);
      StoreFloat(base, kScrollPhase + 4, 0);
      rex::memory::store_and_swap<uint32_t>(base + kScrollInitialized, flags | 1);
      ctx.r3.u64 = 0x8269D8C8;
      ctx.lr = 0x825E80E8;
      sub_822F65A8(ctx, base);  // Preserve the guest's static-destructor registration.
    }
    for (uint32_t i = 0; i < 2; ++i) {
      const auto address = kScrollPhase + i * 4;
      StoreFloat(base, address, trials_hd::AdvanceScrollPhase(
          Float(base, address), Float(base, kFloatConstants + 264 + i * 4)));
    }
  }
}

void Tick(PPCContext& ctx, uint8_t* base) {
  ctx.r3.u64 = Scene(base);
  ctx.lr = 0x826650B8;
  sub_82668BA8(ctx, base);
  auto scene = Scene(base);
  auto state = rex::memory::load_and_swap<uint32_t>(base + scene + 12);
  CallVirtual(ctx, base, state, 36, 0, 0x826650E4);
  scene = Scene(base);
  state = rex::memory::load_and_swap<uint32_t>(base + scene + 12);
  CallVirtual(ctx, base, state, 36, 1, 0x82665110);
  scene = Scene(base);
  state = rex::memory::load_and_swap<uint32_t>(base + scene + 12);
  CallVirtual(ctx, base, state, 40, 0, 0x82665138);
  ctx.r3.u64 = rex::memory::load_and_swap<uint32_t>(base + scene + 96);
  ctx.lr = 0x82665140;
  sub_825CB980(ctx, base);
}
}  // namespace

// Preserve fixed visual state while the original function submits every real frame.
extern "C" REX_FUNC(sub_825EBF48) {
  if (!Unlocked()) {
    __imp__sub_825EBF48(ctx, base);
    return;
  }
  const auto filtered = ctx.r3.u32 + 248760;
  const float value = Float(base, filtered);
  __imp__sub_825EBF48(ctx, base);
  StoreFloat(base, filtered, value);
}

extern "C" REX_FUNC(sub_825E9220) {
  // The other caller uses alpha=1 for initialization; do not suppress it.
  if (!Unlocked() || ctx.lr != 0x825EC160)
    __imp__sub_825E9220(ctx, base);
}

extern "C" REX_FUNC(sub_825E7FC0) {
  if (!Unlocked() ||
      !(rex::memory::load_and_swap<uint32_t>(base + kScrollInitialized) & 1)) {
    __imp__sub_825E7FC0(ctx, base);
    return;
  }
  float phases[2];
  for (uint32_t i = 0; i < 2; ++i) {
    phases[i] = Float(base, kScrollPhase + i * 4);
    // Cancel its legacy per-render advance so drawing reads the fixed-tick phase.
    StoreFloat(base, kScrollPhase + i * 4,
               phases[i] - Float(base, kFloatConstants + 264 + i * 4));
  }
  __imp__sub_825E7FC0(ctx, base);
  for (uint32_t i = 0; i < 2; ++i) StoreFloat(base, kScrollPhase + i * 4, phases[i]);
}

// Strong override: generated code and address registration both resolve here.
extern "C" REX_FUNC(sub_82664F98) {
  // Latch the applied startup mode, not fps_unlock's pending restart-required value.
  g_unlocked_loop.store(rex::cvar::GetFlagByName("present_fps_limit_guest_paced") == "true");
  if (!Unlocked()) {
    __imp__sub_82664F98(ctx, base);
    return;
  }
  const PPCContext saved = ctx;
  // Reserve the same ABI frame: callees use r1+80.. for their parameter area.
  ctx.r1.u32 -= 240;
  rex::memory::store_and_swap<uint32_t>(base + ctx.r1.u32, saved.r1.u32);
  trials_hd::FrameClock clock;
  auto previous = std::chrono::steady_clock::now();
  auto report_time = previous;
  auto next_render = previous;
  int last_limit = 0;
  uint32_t ticks = 0, renders = 0;
  uint32_t last_state_table = 0;
  bool initialized = false;
  REXLOG_INFO("FPS unlock: real rendering, original 16,666 us fixed updates");
  while (Running(base)) {
    // The SDK's cap skips presentation, not rendering. Pace the real work here.
    const int limit = std::stoi(rex::cvar::GetFlagByName("present_fps_limit"));
    if (limit != last_limit) {
      next_render = std::chrono::steady_clock::now();
      last_limit = limit;
    }
    if (limit > 0 && initialized) {
      std::this_thread::sleep_until(std::min(next_render, previous + clock.UntilNextStep()));
    }
    const auto now = std::chrono::steady_clock::now();
    const auto steps = clock.Advance(now - previous);
    previous = now;
    for (uint32_t i = 0; i < steps && Running(base); ++i) {
      Tick(ctx, base);
      Dispatch(ctx, base, false);
      if (Running(base)) TickVisuals(ctx, base);
      const auto state = rex::memory::load_and_swap<uint32_t>(base + Scene(base) + 12);
      const auto table = state ? rex::memory::load_and_swap<uint32_t>(base + state) : 0;
      if (REXCVAR_GET(log_simulation_rate) && table && table != last_state_table) {
        REXLOG_INFO("FPS state table {:08X}: update {:08X}, jobs {:08X}, prepare {:08X}, draw {:08X}",
                    table, rex::memory::load_and_swap<uint32_t>(base + table + 16),
                    rex::memory::load_and_swap<uint32_t>(base + table + 20),
                    rex::memory::load_and_swap<uint32_t>(base + table + 24),
                    rex::memory::load_and_swap<uint32_t>(base + table + 28));
        last_state_table = table;
      }
      auto count = rex::memory::load_and_swap<uint32_t>(base + kTickCount);
      rex::memory::store_and_swap<uint32_t>(base + kTickCount, count + 1);
      ++ticks;
      initialized = true;
    }
    if (!Running(base)) break;
    if (initialized && (limit <= 0 || now >= next_render)) {
      Dispatch(ctx, base, true);
      ++renders;
      if (limit > 0) {
        const auto period = std::chrono::nanoseconds(1'000'000'000 / limit);
        next_render = now - next_render > period ? now + period : next_render + period;
      }
    }
    ctx.r3.u64 = ctx.r1.u32 + 96;
    ctx.lr = 0x8266518C;
    sub_82124BF0(ctx, base);
    if (now - report_time >= std::chrono::seconds(1)) {
      const double seconds = std::chrono::duration<double>(now - report_time).count();
      if (REXCVAR_GET(log_simulation_rate)) {
        REXLOG_INFO("fixed updates/sec: {:.1f} real renders/sec: {:.1f}",
                    ticks / seconds, renders / seconds);
      }
      ticks = renders = 0;
      report_time = now;
    }
    ctx.r3.u64 = 0;
    ctx.lr = 0x826651A0;
    sub_82124660(ctx, base);
  }
  g_unlocked_loop.store(false);
  ctx = saved;
}
