// trials_hd - performance overlay (see performance_overlay.h).

#include "performance_overlay.h"

#include <algorithm>
#include <memory>
#include <string>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/perf/frame_timing.h>

REXCVAR_DEFINE_BOOL(show_performance_overlay, false, "Display",
                    "Show the FPS counter (F2)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace trials_hd {

namespace {

std::unique_ptr<PerformanceOverlay> g_overlay;

int FpsLimit() {
  try {
    return std::stoi(rex::cvar::GetFlagByName("present_fps_limit"));
  } catch (...) {
    return 0;
  }
}

}  // namespace

void PerformanceOverlay::OnDraw(ImGuiIO& io) {
  const rex::perf::FrameTiming t = rex::perf::GetFrameTiming();

  // Smooth for the frame rate the player asked for (at most 60): green at it,
  // yellow when visibly short of it, red when far below.
  const int limit = FpsLimit();
  const double target = limit > 0 ? std::min(limit, 60) : 60;
  const ImVec4 color = t.fps >= target * 0.95   ? ImVec4(0.45f, 1.00f, 0.45f, 1.0f)
                       : t.fps >= target * 0.75 ? ImVec4(1.00f, 0.85f, 0.30f, 1.0f)
                                                : ImVec4(1.00f, 0.40f, 0.40f, 1.0f);

  const float scale = std::max(1.0f, io.DisplaySize.y / 720.0f);
  const float pad = 10.0f * scale;
  ImGui::SetNextWindowPos(ImVec2(pad, pad), ImGuiCond_Always);
  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * scale * 2.0f);
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                 ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoFocusOnAppearing |
                                 ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_AlwaysAutoResize;
  if (ImGui::Begin("##performance_overlay", nullptr, flags)) {
    ImGui::TextColored(color, "%.0f", t.fps);
  }
  ImGui::End();
  ImGui::PopFont();
}

void SyncPerformanceOverlay(rex::ui::ImGuiDrawer* drawer) {
  const bool show = REXCVAR_GET(show_performance_overlay);
  if (show && !g_overlay && drawer) {
    g_overlay = std::make_unique<PerformanceOverlay>(drawer);
  } else if (!show && g_overlay) {
    g_overlay.reset();
  }
}

void TogglePerformanceOverlay(rex::ui::ImGuiDrawer* drawer) {
  rex::cvar::SetFlagByName("show_performance_overlay",
                           REXCVAR_GET(show_performance_overlay) ? "false" : "true");
  SyncPerformanceOverlay(drawer);
}

}  // namespace trials_hd
