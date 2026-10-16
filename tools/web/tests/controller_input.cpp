// Exercises the real shared-memory source and web XInput driver; no game data.
#undef NDEBUG
#include <cassert>
#include <cmath>
#include <cstdio>
#include <limits>
#include <vector>
#include <rex/chrono/clock.h>
#include <rex/input/web/web_input_driver.h>
#include <rex/ui/web.h>
#include "src/web/web_input.h"

bool& FLAGS_guide_button_storage_() { static bool enabled = false; return enabled; }
uint32_t rex::chrono::Clock::QueryGuestUptimeMillis() { return 0; }
extern "C" uintptr_t trials_hd_web_input_block();

int main() {
  using namespace rex::input;
  using rex::X_RESULT;
  using rex::X_STATUS;
  auto& block = *reinterpret_cast<trials_hd::web::WebInputBlock*>(trials_hd_web_input_block());
  trials_hd::web::InitializeInput();
  const auto source = rex::ui::web::GetInputSource();
  rex::ui::web::InputEvent events[2]{};
  assert(source.drain_events(events, 2) == 0);
  block.event_head = 1;
  block.event_tail = 0xffffffff;
  block.events[255] = {1, 4, 12, 24};
  block.events[0] = {2, 4, 12, 24};
  assert(source.drain_events(events, 1) == 1);
  assert(events[0].type == 1 && events[0].code == 4 && events[0].x == 12);
  assert(block.event_tail == 0);
  assert(trials_hd::web::IsKeyDown(4) && trials_hd::web::AnyKeyDown());
  assert(source.drain_events(events, 2) == 1 && events[0].type == 2);
  assert(!trials_hd::web::AnyKeyDown() && !trials_hd::web::IsKeyDown(999));
  block.event_tail = 0;
  block.event_head = 2;
  block.events[0] = {1, 41, 0, 0};
  block.events[1] = {9, 0, 0, 0};
  assert(source.drain_events(events, 1) == 1 && trials_hd::web::IsKeyDown(41));
  assert(source.drain_events(events, 1) == 1 && !trials_hd::web::AnyKeyDown());
  block.event_head = 1000;
  assert(source.drain_events(events, 2) == 0 && block.event_tail == 1000);
  rex::ui::web::CanvasState canvas;
  assert(!source.get_canvas_state(canvas));
  block.canvas_width = 2560;
  block.canvas_height = 1440;
  block.device_pixel_ratio = 2;
  block.visible = 1;
  assert(source.get_canvas_state(canvas) && canvas.width == 2560 &&
         canvas.height == 1440 && canvas.device_pixel_ratio == 2 && canvas.visible);
  web::WebInputDriver driver(nullptr, 0, web::GetGamepadSource());
  assert(driver.Setup() == X_STATUS_SUCCESS);
  block.visible = 1;
  block.pads[0].connected = 1;
  std::vector<DeviceInfo> devices;
  driver.EnumerateDevices(devices);
  assert(devices.size() == 1);
  auto id = devices.front().id;
  constexpr uint16_t expected[] = {X_INPUT_GAMEPAD_A, X_INPUT_GAMEPAD_B, X_INPUT_GAMEPAD_X,
      X_INPUT_GAMEPAD_Y, X_INPUT_GAMEPAD_LEFT_SHOULDER, X_INPUT_GAMEPAD_RIGHT_SHOULDER,
      0, 0, X_INPUT_GAMEPAD_BACK, X_INPUT_GAMEPAD_START, X_INPUT_GAMEPAD_LEFT_THUMB,
      X_INPUT_GAMEPAD_RIGHT_THUMB, X_INPUT_GAMEPAD_DPAD_UP, X_INPUT_GAMEPAD_DPAD_DOWN,
      X_INPUT_GAMEPAD_DPAD_LEFT, X_INPUT_GAMEPAD_DPAD_RIGHT, 0};
  X_INPUT_STATE state{};
  for (size_t i = 0; i < 17; ++i) {
    block.pads[0].buttons = 1u << i;
    assert(driver.GetDeviceState(id, &state) == X_ERROR_SUCCESS);
    assert(uint16_t(state.gamepad.buttons) == expected[i]);
  }
  block.pads[0].buttons = 1;
  block.pads[0].axes[0] = -1;
  block.pads[0].axes[1] = -1;
  block.pads[0].axes[2] = 1;
  block.pads[0].axes[3] = 1;
  block.pads[0].triggers[0] = 0.5f;
  block.pads[0].triggers[1] = 1;
  driver.GetDeviceState(id, &state);
  assert(int16_t(state.gamepad.thumb_lx) == -32768 && int16_t(state.gamepad.thumb_ly) == 32767);
  assert(int16_t(state.gamepad.thumb_rx) == 32767 && int16_t(state.gamepad.thumb_ry) == -32768);
  assert(state.gamepad.left_trigger == 128 && state.gamepad.right_trigger == 255);
  auto packet = uint32_t(state.packet_number);
  driver.GetDeviceState(id, &state);
  assert(uint32_t(state.packet_number) == packet);
  block.pad_seq = 1; // A half-written snapshot must retain the previous state.
  block.pads[0].buttons = 2;
  driver.GetDeviceState(id, &state);
  assert(uint16_t(state.gamepad.buttons) == X_INPUT_GAMEPAD_A);
  block.pad_seq = 2;
  driver.GetDeviceState(id, &state);
  assert(uint16_t(state.gamepad.buttons) == X_INPUT_GAMEPAD_B);
  block.visible = 0;
  driver.GetDeviceState(id, &state);
  assert(uint16_t(state.gamepad.buttons) == 0 && state.gamepad.right_trigger == 0);
  block.visible = 1;
  block.pads[0] = {};
  block.pads[0].connected = 1;
  block.pads[0].buttons = 1;
  block.pads[0].axes[0] = std::numeric_limits<float>::quiet_NaN();
  block.pads[0].triggers[0] = std::numeric_limits<float>::infinity();
  driver.GetDeviceState(id, &state);
  assert(int16_t(state.gamepad.thumb_lx) == 0 && state.gamepad.left_trigger == 0);
  X_INPUT_KEYSTROKE key{};
  assert(driver.GetDeviceKeystroke(id, 0, &key) == X_ERROR_SUCCESS);
  assert(uint16_t(key.virtual_key) == uint16_t(rex::ui::VirtualKey::kXInputPadA));
  block.pads[0].connected = 0;
  assert(driver.GetDeviceState(id, &state) == X_ERROR_DEVICE_NOT_CONNECTED);
  block.pads[0].connected = 1;
  assert(driver.GetDeviceKeystroke(id, 0, &key) == X_ERROR_SUCCESS);
  assert(uint16_t(key.flags) == X_INPUT_KEYSTROKE_KEYDOWN);
  std::puts("CONTROLLER INPUT PASSED: buttons, sticks, triggers, packets, seqlock, visibility, reconnect, event ring, canvas");
}
