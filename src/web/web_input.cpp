#include "web_input.h"

#include <algorithm>
#include <array>
#include <bit>
#include <emscripten/emscripten.h>
#include <rex/input/web/web_input_driver.h>
#include <rex/ui/web.h>

namespace trials_hd::web {
namespace {
alignas(64) WebInputBlock input{};
std::array<std::atomic<bool>, 512> keys{};

size_t DrainEvents(rex::ui::web::InputEvent* out, size_t max) {
  const auto tail = input.event_tail.load(std::memory_order_relaxed);
  const auto head = input.event_head.load(std::memory_order_acquire);
  const uint32_t available = head - tail;
  if (available > 256) {
    input.event_tail.store(head, std::memory_order_release);
    return 0;
  }
  const auto count = std::min(size_t(available), max);
  for (size_t i = 0; i < count; ++i) {
    const auto& event = input.events[(tail + i) & 255];
    if ((event.type == rex::ui::web::kKeyDown || event.type == rex::ui::web::kKeyUp) &&
        event.code < keys.size()) {
      keys[event.code].store(event.type == rex::ui::web::kKeyDown, std::memory_order_relaxed);
    } else if (event.type == rex::ui::web::kBlur) {
      for (auto& key : keys) key.store(false, std::memory_order_relaxed);
    }
    out[i] = {event.type, event.code, event.x, event.y};
  }
  input.event_tail.store(tail + uint32_t(count), std::memory_order_release);
  return count;
}

bool ReadCanvas(rex::ui::web::CanvasState& out) {
  out.width = __atomic_load_n(&input.canvas_width, __ATOMIC_RELAXED);
  out.height = __atomic_load_n(&input.canvas_height, __ATOMIC_RELAXED);
  out.device_pixel_ratio = std::bit_cast<float>(__atomic_load_n(
      reinterpret_cast<const uint32_t*>(&input.device_pixel_ratio), __ATOMIC_RELAXED));
  out.visible = __atomic_load_n(&input.visible, __ATOMIC_RELAXED) != 0;
  return out.width != 0 && out.height != 0;
}

bool ReadGamepad(uint32_t index, rex::input::web::WebGamepad& out) {
  if (index >= 4) return false;
  auto sequence = input.pad_seq.load(std::memory_order_acquire);
  if (sequence & 1) return false;
  // Atomic word loads prevent the compiler from reusing a torn seqlock snapshot.
  const auto* source = reinterpret_cast<const uint32_t*>(&input.pads[index]);
  uint32_t words[8];
  for (size_t i = 0; i < 8; ++i) words[i] = __atomic_load_n(source + i, __ATOMIC_RELAXED);
  std::atomic_thread_fence(std::memory_order_acquire);
  if (sequence != input.pad_seq.load(std::memory_order_relaxed)) return false;
  out = {};
  out.connected = words[0] != 0;
  if (!__atomic_load_n(&input.visible, __ATOMIC_RELAXED)) return true;
  out.buttons = words[1];
  for (size_t i = 0; i < 4; ++i) out.axes[i] = std::bit_cast<float>(words[2 + i]);
  for (size_t i = 0; i < 2; ++i) out.triggers[i] = std::bit_cast<float>(words[6 + i]);
  return true;
}
}  // namespace

bool IsKeyDown(uint32_t scancode) {
  return scancode < keys.size() && keys[scancode].load(std::memory_order_relaxed);
}

bool AnyKeyDown() {
  for (const auto& key : keys) {
    if (key.load(std::memory_order_relaxed)) return true;
  }
  return false;
}

void InitializeInput() {
  rex::input::web::SetGamepadSource(ReadGamepad);
  rex::ui::web::SetInputSource({DrainEvents, ReadCanvas});
}

extern "C" EMSCRIPTEN_KEEPALIVE uintptr_t trials_hd_web_input_block() {
  return reinterpret_cast<uintptr_t>(&input);
}
}  // namespace trials_hd::web
