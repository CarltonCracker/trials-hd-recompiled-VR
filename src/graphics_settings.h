// trials_hd - player-facing settings (keyboard controls, display mode,
// resolution, render resolution, FPS limit, VSync) and the dialog that edits them.
//
// The settings are cvars, so they load from and save to the same config file as
// the SDK's own settings (<exe dir>/trials_hd.toml) and can also be passed on
// the command line, e.g. --display_width=1920 --display_height=1080.

#pragma once

#include <filesystem>
#include <functional>

#include <rex/ui/imgui_dialog.h>

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
  GraphicsSettingsDialog(rex::ui::ImGuiDrawer* drawer, std::filesystem::path config_path,
                         std::function<void()> on_closed);

  // Closes the dialog after the current frame (it then deletes itself).
  void RequestClose() { Close(); }

 protected:
  void OnDraw(ImGuiIO& io) override;
  void OnClose() override;

 private:
  void FeedGamepadNavigation(ImGuiIO& io);

  std::filesystem::path config_path_;
  std::function<void()> on_closed_;
  bool gamepad_was_down_[8] = {};
  // Keyboard binding waiting for a key press (index into kKeyboardBindings,
  // -1 = none), its alternative slot and the frame the wait started.
  int capture_binding_ = -1;
  size_t capture_slot_ = 0;
  int capture_frame_ = 0;
  bool saved_ = false;
};

}  // namespace trials_hd
