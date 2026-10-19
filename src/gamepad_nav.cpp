// trials_hd - controller navigation for the app's ImGui menus (see gamepad_nav.h).

#include "gamepad_nav.h"

#include <algorithm>
#include <array>
#include <utility>

#include <SDL3/SDL.h>
#include <imgui.h>

#if defined(__EMSCRIPTEN__)
#include <cmath>
#include <rex/input/web/web_input_driver.h>
#endif

namespace trials_hd {

namespace {

// In GamepadNav::Button order.
constexpr std::array<std::pair<SDL_GamepadButton, ImGuiKey>, GamepadNav::kCount> kNavButtons = {{
    {SDL_GAMEPAD_BUTTON_DPAD_UP, ImGuiKey_GamepadDpadUp},
    {SDL_GAMEPAD_BUTTON_DPAD_DOWN, ImGuiKey_GamepadDpadDown},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, ImGuiKey_GamepadDpadLeft},
    {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, ImGuiKey_GamepadDpadRight},
    {SDL_GAMEPAD_BUTTON_SOUTH, ImGuiKey_GamepadFaceDown},
    {SDL_GAMEPAD_BUTTON_EAST, ImGuiKey_GamepadFaceRight},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, ImGuiKey_GamepadL1},
    {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, ImGuiKey_GamepadR1},
}};

}  // namespace

void GamepadNav::Feed(ImGuiIO& io) {
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
  io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
  bool down[kCount] = {};
#if defined(__EMSCRIPTEN__)
  if (auto source = rex::input::web::GetGamepadSource()) {
    constexpr uint32_t buttons[] = {12, 13, 14, 15, 0, 1, 4, 5};
    for (uint32_t i = 0; i < 4; ++i) {
      rex::input::web::WebGamepad pad;
      if (!source(i, pad)) {
        // Keep held navigation keys during a seqlock write.
        std::fill(std::begin(pressed_), std::end(pressed_), false);
        return;
      }
      if (!pad.connected) continue;
      for (size_t b = 0; b < kCount; ++b) {
        down[b] = down[b] || (pad.buttons & (1u << buttons[b]));
      }
      if (std::isfinite(pad.axes[1])) {
        down[kUp] = down[kUp] || pad.axes[1] < -0.5f;
        down[kDown] = down[kDown] || pad.axes[1] > 0.5f;
      }
      if (std::isfinite(pad.axes[0])) {
        down[kLeft] = down[kLeft] || pad.axes[0] < -0.5f;
        down[kRight] = down[kRight] || pad.axes[0] > 0.5f;
      }
    }
  }
#else
  int count = 0;
  if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
    for (int i = 0; i < count; ++i) {
      SDL_Gamepad* pad = SDL_GetGamepadFromID(ids[i]);
      if (!pad) {
        continue;
      }
      for (size_t b = 0; b < kCount; ++b) {
        down[b] = down[b] || SDL_GetGamepadButton(pad, kNavButtons[b].first);
      }
      // Left stick doubles as the d-pad.
      constexpr Sint16 kDeadzone = 16000;
      down[kUp] = down[kUp] || SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY) < -kDeadzone;
      down[kDown] = down[kDown] || SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY) > kDeadzone;
      down[kLeft] = down[kLeft] || SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX) < -kDeadzone;
      down[kRight] = down[kRight] || SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX) > kDeadzone;
    }
    SDL_free(ids);
  }
#endif
  if (ignore_held_) {
    ignore_held_ = false;
    std::copy(std::begin(down), std::end(down), std::begin(down_));
    std::fill(std::begin(pressed_), std::end(pressed_), false);
    return;
  }
  for (size_t b = 0; b < kCount; ++b) {
    pressed_[b] = down[b] && !down_[b];
    if (down[b] != down_[b]) {
      io.AddKeyEvent(kNavButtons[b].second, down[b]);
      down_[b] = down[b];
    }
  }
}

void GamepadNav::Release(ImGuiIO& io) {
  for (size_t b = 0; b < kCount; ++b) {
    if (down_[b]) {
      io.AddKeyEvent(kNavButtons[b].second, false);
      down_[b] = false;
    }
    pressed_[b] = false;
  }
}

}  // namespace trials_hd
