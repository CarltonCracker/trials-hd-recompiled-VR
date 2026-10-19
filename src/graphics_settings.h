// trials_hd - player-facing settings (keyboard controls, display mode,
// resolution, render resolution, FPS limit, VSync) and the dialog that edits them.
//
// The settings are cvars, so they load from and save to the same config file as
// the SDK's own settings (<exe dir>/trials_hd.toml) and can also be passed on
// the command line, e.g. --display_width=1920 --display_height=1080.

#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

#include <rex/ui/imgui_dialog.h>

#include "gamepad_nav.h"

namespace trials_hd {

// Applies settings that must be in place before the GPU presenter is created
// (the VSync choice selects the swapchain present mode). Call from OnPreSetup.
void ApplyStartupGraphicsSettings();

// Sizes the window (windowed) or picks the display mode (fullscreen) from the
// resolution settings. UI thread only, once the window exists.
void ApplyDisplayMode();

// Refresh rate (Hz) of the display the game presents to with VSync on, or 0
// with VSync off or before the window exists. Any thread.
float VsyncRefreshRate();

// Refresh rate (Hz) of the active desktop display the game presents to (e.g. 240 Hz),
// or 0 before the window or display exists. Any thread.
float DisplayRefreshRate();

// Owns itself once shown: Close() deletes it after the current frame, and
// on_closed runs just before that so the owner can drop its pointer.
class GraphicsSettingsDialog : public rex::ui::ImGuiDialog {
 public:
  // The Game tab: installed DLC and Add DLC (opens the installer, src/installer/).
  // Without on_add_dlc (the web build) there is no Game tab.
  struct GameTab {
    std::vector<std::string> installed_dlc;
    std::function<void()> on_add_dlc;
  };

  GraphicsSettingsDialog(rex::ui::ImGuiDrawer* drawer, std::filesystem::path config_path,
                         std::function<void()> on_closed, GameTab game_tab = {});

  // Closes the dialog after the current frame (it then deletes itself).
  void RequestClose() { Close(); }

 protected:
  void OnDraw(ImGuiIO& io) override;
  void OnClose() override;

 private:
  void FeedGamepadNavigation(ImGuiIO& io);
  // Tab contents; key_bound reports a key binding made this frame.
  void DrawControlsTab(float label_width, bool& key_bound);
  void DrawGraphicsTab(float label_width);
  void DrawGameTab(float label_width);
  int TabCount() const { return game_tab_.on_add_dlc ? 3 : 2; }

  int tab_ = 0;            // shown tab: 0 Controls, 1 Graphics, 2 Game
  int requested_tab_ = -1;  // tab to select on the next frame (LB / RB)

  std::filesystem::path config_path_;
  std::function<void()> on_closed_;
  GameTab game_tab_;
  GamepadNav nav_;
  // Keyboard binding waiting for a key press (index into kKeyboardBindings,
  // -1 = none), its alternative slot and the frame the wait started.
  int capture_binding_ = -1;
  size_t capture_slot_ = 0;
  int capture_frame_ = 0;
  bool saved_ = false;
};

}  // namespace trials_hd
