// trials_hd - controller scrolling for the read-only achievements list.
#pragma once

#include <algorithm>
#include <cmath>
#include <rex/ui/overlay/achievements_overlay.h>

#if defined(__EMSCRIPTEN__)
#include <rex/input/web/web_input_driver.h>
#else
#include <SDL3/SDL.h>
#endif

namespace trials_hd {

class AchievementControls {
 public:
  rex::ui::AchievementsOverlayDialog::ScrollInput Poll() {
    bool up = false, down = false, lb = false, rb = false, a = false;
    float vertical = 0.0f;
    auto stick = [&](float value) {
      if (!std::isfinite(value) || std::abs(value) < 0.25f) return;
      const float scaled = std::copysign((std::abs(value) - 0.25f) / 0.75f, value);
      if (std::abs(scaled) > std::abs(vertical)) vertical = scaled;
    };
#if defined(__EMSCRIPTEN__)
    if (auto source = rex::input::web::GetGamepadSource()) {
      for (uint32_t i = 0; i < 4; ++i) {
        rex::input::web::WebGamepad pad;
        if (!source(i, pad)) return {};  // Don't turn a seqlock retry into a new button edge.
        if (!pad.connected) continue;
        up = up || (pad.buttons & (1u << 12));
        down = down || (pad.buttons & (1u << 13));
        lb = lb || (pad.buttons & (1u << 4));
        rb = rb || (pad.buttons & (1u << 5));
        a = a || (pad.buttons & 1u);
        stick(pad.axes[1]);
        stick(pad.axes[3]);
      }
    }
#else
    int count = 0;
    if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
      for (int i = 0; i < count; ++i) {
        SDL_Gamepad* pad = SDL_GetGamepadFromID(ids[i]);
        if (!pad) continue;
        up = up || SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_UP);
        down = down || SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN);
        lb = lb || SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
        rb = rb || SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
        a = a || SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_SOUTH);
        stick(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY) / 32767.0f);
        stick(SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_RIGHTY) / 32767.0f);
      }
      SDL_free(ids);
    }
#endif
    if (up != down) vertical = down ? 1.0f : -1.0f;
    rex::ui::AchievementsOverlayDialog::ScrollInput result;
    result.vertical = std::clamp(vertical, -1.0f, 1.0f);
    // The game may open this menu while A is still held. Require a fresh press
    // after the first poll, otherwise the opening action would close it again.
    result.page = initialized_ ? int(rb && !rb_was_down_) - int(lb && !lb_was_down_) : 0;
    result.close = initialized_ && a && !a_was_down_;
    initialized_ = true;
    lb_was_down_ = lb;
    rb_was_down_ = rb;
    a_was_down_ = a;
    return result;
  }

 private:
  bool initialized_ = false;
  bool lb_was_down_ = false, rb_was_down_ = false, a_was_down_ = false;
};

}  // namespace trials_hd
