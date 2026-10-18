// trials_hd - performance overlay (see performance_overlay.h).

#include "performance_overlay.h"

#include <algorithm>
#include <memory>
#include <string>

#include <fmt/format.h>
#include <imgui.h>

#include <rex/cvar.h>
#include <rex/perf/frame_timing.h>

REXCVAR_DEFINE_BOOL(show_performance_overlay, false, "Display",
                    "Show the performance overlay (measured FPS and frame times; F2)")
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
  const size_t n = rex::perf::GetRecentFrameTimes(frame_times_.data(), frame_times_.size());

  // Smooth for the frame rate the player asked for (at most 60): green at it,
  // yellow when visibly short of it, red when far below.
  const int limit = FpsLimit();
  const double target = limit > 0 ? std::min(limit, 60) : 60;
  const ImVec4 color = t.fps >= target * 0.95   ? ImVec4(0.45f, 1.00f, 0.45f, 1.0f)
                       : t.fps >= target * 0.75 ? ImVec4(1.00f, 0.85f, 0.30f, 1.0f)
                                                : ImVec4(1.00f, 0.40f, 0.40f, 1.0f);

  const float scale = std::max(1.0f, io.DisplaySize.y / 720.0f);
  const float pad = 10.0f * scale;
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x - pad, pad), ImGuiCond_Always, ImVec2(1, 0));
  ImGui::SetNextWindowBgAlpha(0.55f);
  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * scale * 1.1f);
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                                 ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoFocusOnAppearing |
                                 ImGuiWindowFlags_AlwaysAutoResize;
  if (ImGui::Begin("##performance_overlay", nullptr, flags)) {
    ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * scale * 2.0f);
    ImGui::TextColored(color, "%.0f FPS", t.fps);
    ImGui::PopFont();
    ImGui::Text("frame %.1f ms   worst %.1f ms", t.avg_frame_ms, t.worst_frame_ms);
    ImGui::Text("1%% low %.0f FPS", t.low_1pct_fps);

    // Frame times, newest on the right; the scale grows with spikes.
    float peak = 0;
    for (size_t i = 0; i < n; ++i) {
      peak = std::max(peak, frame_times_[i]);
    }
    const float graph_max = std::clamp(peak * 1.15f, 1000.0f / 30.0f, 200.0f);
    const std::string caption = fmt::format("frame time (0-{:.0f} ms)", graph_max);
    ImGui::PlotLines("##frame_times", frame_times_.data(), int(n), 0, caption.c_str(), 0.0f,
                     graph_max, ImVec2(230.0f * scale, 55.0f * scale));
    ImGui::TextDisabled("%s", limit > 0 ? fmt::format("limit {} FPS", limit).c_str()
                                        : "no FPS limit");
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
