// trials_hd - controller navigation for the app's ImGui menus (settings, setup
// screen): d-pad or left stick moves, A activates, B goes back, LB / RB switch tabs.

#pragma once

#include <cstddef>

struct ImGuiIO;

namespace trials_hd {

class GamepadNav {
 public:
  enum Button { kUp, kDown, kLeft, kRight, kAccept, kBack, kPreviousTab, kNextTab, kCount };

  // Reads every connected controller and forwards button changes to ImGui as
  // gamepad keys (enables ImGui's gamepad navigation). Call once per frame
  // before drawing the menu.
  void Feed(ImGuiIO& io);

  // Whether the button went down in the last Feed.
  bool Pressed(Button button) const { return pressed_[button]; }

  // Releases the keys still held, so ImGui doesn't keep them pressed after the
  // menu closes.
  void Release(ImGuiIO& io);

  // The next Feed takes buttons already down as held, not pressed: the A that
  // opened this menu from another one must not also press its first button.
  void IgnoreHeld() { ignore_held_ = true; }

 private:
  bool ignore_held_ = false;
  bool down_[kCount] = {};
  bool pressed_[kCount] = {};
};

}  // namespace trials_hd
