#include "keyboard_controls.h"

#include <algorithm>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>

namespace trials_hd {

// The arrows / WASD ride the bike and also move through menus (the D-pad).
// Trials HD ignores the D-pad while riding and the triggers in menus, so the
// shared keys never act twice.
const KeyboardBinding kKeyboardBindings[] = {
    {"keybind_right_trigger", "Up,W", "Throttle"},
    {"keybind_left_trigger", "Down,S", "Brake"},
    {"keybind_lstick_left", "Left,A", "Lean Back"},
    {"keybind_lstick_right", "Right,D", "Lean Forward"},
    {"keybind_b", "Backspace", "Checkpoint / Back"},
    {"keybind_back", "R", "Restart Track"},
    {"keybind_start", "Escape", "Pause"},
    {"keybind_dpad_up", "Up,W", "Menu Up"},
    {"keybind_dpad_down", "Down,S", "Menu Down"},
    {"keybind_dpad_left", "Left,A", "Menu Left"},
    {"keybind_dpad_right", "Right,D", "Menu Right"},
    {"keybind_a", "Return,Space", "Menu Select"},
    {"keybind_x", "X", "X Button"},
    {"keybind_y", "Y", "Bailout"},
    {"keybind_left_shoulder", "Q", "LB"},
    {"keybind_right_shoulder", "E", "RB"},
    // Unused by the game; cleared so the SDK's defaults don't double up keys.
    {"keybind_lstick_up", "", ""},
    {"keybind_lstick_down", "", ""},
    {"keybind_lstick_press", "", ""},
    {"keybind_rstick_up", "", ""},
    {"keybind_rstick_down", "", ""},
    {"keybind_rstick_left", "", ""},
    {"keybind_rstick_right", "", ""},
    {"keybind_rstick_press", "", ""},
    {"keybind_guide", "", ""},
};
const size_t kKeyboardBindingCount = std::size(kKeyboardBindings);

void ApplyKeyboardDefaults() {
  auto set_default = [](std::string_view name, std::string_view value) {
    const std::string flag(name);
    if (rex::cvar::GetFlagSource(flag) == rex::cvar::Source::kDefault) {
      rex::cvar::SetFlagByName(flag, std::string(value));
    }
  };
  set_default("mnk_mode", "true");
  for (const KeyboardBinding& binding : kKeyboardBindings) {
    set_default(binding.cvar, binding.keys);
  }
  REXLOG_INFO("Keyboard controls: {}", rex::cvar::GetFlagByName("mnk_mode") == "true"
                                           ? "on"
                                           : "off");
}

std::vector<std::string> GetBindingKeys(std::string_view cvar) {
  std::vector<std::string> keys;
  const std::string value = rex::cvar::GetFlagByName(std::string(cvar));
  size_t start = 0;
  while (start <= value.size()) {
    size_t end = value.find(',', start);
    if (end == std::string::npos) {
      end = value.size();
    }
    std::string key = value.substr(start, end - start);
    key.erase(0, key.find_first_not_of(' '));
    key.erase(key.find_last_not_of(' ') + 1);
    if (!key.empty()) {
      keys.push_back(std::move(key));
    }
    start = end + 1;
  }
  return keys;
}

void SetBindingKey(std::string_view cvar, size_t slot, std::string_view key) {
  std::vector<std::string> keys = GetBindingKeys(cvar);
  std::vector<std::string> updated;
  for (size_t i = 0; i < std::max(keys.size(), slot + 1); ++i) {
    std::string k = i == slot ? std::string(key) : (i < keys.size() ? keys[i] : "");
    // Skip empty slots and a key the action already has.
    if (!k.empty() && std::find(updated.begin(), updated.end(), k) == updated.end()) {
      updated.push_back(std::move(k));
    }
  }
  std::string value;
  for (const std::string& k : updated) {
    value += (value.empty() ? "" : ",") + k;
  }
  rex::cvar::SetFlagByName(std::string(cvar), value);
}

void ResetKeyboardBindings() {
  for (const KeyboardBinding& binding : kKeyboardBindings) {
    rex::cvar::SetFlagByName(std::string(binding.cvar), std::string(binding.keys));
  }
}

}  // namespace trials_hd
