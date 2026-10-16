// Browser input shared with web/input.js (docs/web-port/README.md 7.9.6).
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace trials_hd::web {
struct WebPadState {
  uint32_t connected;
  uint32_t buttons;
  float axes[4];
  float triggers[2];
};
struct WebInputEvent {
  uint32_t type, code;
  int32_t x, y;
};
struct WebInputBlock {
  std::atomic<uint32_t> pad_seq;
  uint32_t pad_count;
  WebPadState pads[4];
  std::atomic<uint32_t> event_head, event_tail;
  WebInputEvent events[256];
  uint32_t canvas_width, canvas_height;
  float device_pixel_ratio;
  uint32_t visible;
};
static_assert(sizeof(WebPadState) == 32 && sizeof(WebInputBlock) == 4256);
static_assert(offsetof(WebInputBlock, pads) == 8);
static_assert(offsetof(WebInputBlock, event_head) == 136);
static_assert(offsetof(WebInputBlock, events) == 144);
static_assert(offsetof(WebInputBlock, canvas_width) == 4240);

void InitializeInput();
// Keys already delivered to the window, for the overlay close/release gate.
bool IsKeyDown(uint32_t scancode);
bool AnyKeyDown();
}  // namespace trials_hd::web
