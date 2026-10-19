// trials_hd - number-only FPS counter at the top-left of the game.
//
// F2 toggles it, as does Settings > FPS Counter. Keep the internal cvar/bind
// names stable so saved show_performance_overlay preferences still work.

#pragma once

#include <rex/ui/imgui_dialog.h>

namespace trials_hd {

class PerformanceOverlay : public rex::ui::ImGuiDialog {
 public:
  explicit PerformanceOverlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}

  // New game frames repaint it; it must not keep the window repainting on its own.
  bool WantsContinuousPaint() const override { return false; }
  bool WantsBackgroundDim() const override { return false; }

 protected:
  void OnDraw(ImGuiIO& io) override;

};

// Shows or hides the overlay to match show_performance_overlay. UI thread only.
void SyncPerformanceOverlay(rex::ui::ImGuiDrawer* drawer);

// Flips show_performance_overlay and applies it. UI thread only.
void TogglePerformanceOverlay(rex::ui::ImGuiDrawer* drawer);

}  // namespace trials_hd
