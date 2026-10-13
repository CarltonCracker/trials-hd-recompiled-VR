// trials_hd - keyboard controls.
//
// The SDK's keyboard driver (mnk_mode) turns key presses into controller input
// for player one, alongside any real controller. The port turns it on and
// gives it a Trials-style layout; every key is a keybind_* cvar, changed in the
// settings menu, in trials_hd.toml or on the command line.
//
// The driver reads the keybind_* strings on game threads without a lock, only
// while game input is active. The settings menu blocks game input while it is
// open, so only change them from there.

#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace trials_hd {

struct KeyboardBinding {
  std::string_view cvar;
  std::string_view keys;    // Comma-separated alternatives (SDK key names).
  std::string_view action;  // Shown in the settings menu; empty = not shown.
};

// The port's layout, in the order the settings menu lists it.
extern const KeyboardBinding kKeyboardBindings[];
extern const size_t kKeyboardBindingCount;

// Enables keyboard controls and applies the port's layout, except for settings
// the player made in the config file or on the command line. Call once the
// config is loaded.
void ApplyKeyboardDefaults();

// The keys bound to an action (the cvar's comma-separated alternatives).
std::vector<std::string> GetBindingKeys(std::string_view cvar);
// Puts `key` in alternative `slot` of an action; an empty key clears the slot.
void SetBindingKey(std::string_view cvar, size_t slot, std::string_view key);
// Restores the port's layout.
void ResetKeyboardBindings();

}  // namespace trials_hd
