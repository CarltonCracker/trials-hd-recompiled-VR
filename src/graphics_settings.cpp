// trials_hd - player-facing graphics settings and the dialog that edits them.

#include "graphics_settings.h"

#include <algorithm>
#include <array>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>
#include <fmt/format.h>
#include <imgui.h>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_INT32(display_width, 0, "Display",
                     "Window width, or fullscreen mode width (0 = desktop resolution)")
    .range(0, 16384)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(display_height, 0, "Display",
                     "Window height, or fullscreen mode height (0 = desktop resolution)")
    .range(0, 16384)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(host_vsync, true, "Display",
                    "Synchronize presentation with the display refresh (no tearing)")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace trials_hd {

namespace {

// Trials HD renders a 1280x720 frame; the SDK scales its draws by an integer.
constexpr int kGuestWidth = 1280;
constexpr int kGuestHeight = 720;
constexpr int kMaxRenderScale = 3;

// Render caps are independent of the original fixed update clock.
constexpr std::array<int, 9> kFpsLimits = {0, 240, 165, 144, 120, 90, 60, 45, 30};

// Values in effect since startup, for "restart to apply" notes.
std::string g_startup_render_scale;
bool g_startup_vsync = true;
bool g_startup_fps_unlock = true;

int GetInt(std::string_view name) {
  try {
    return std::stoi(rex::cvar::GetFlagByName(name));
  } catch (...) {
    return 0;
  }
}

bool GetBool(std::string_view name) {
  return rex::cvar::GetFlagByName(name) == "true";
}

SDL_Window* FindGameWindow() {
  int count = 0;
  SDL_Window** windows = SDL_GetWindows(&count);
  SDL_Window* window = (windows && count > 0) ? windows[0] : nullptr;
  SDL_free(windows);
  return window;
}

// Distinct resolutions the window's display supports, largest first, plus
// common 16:9 sizes that fit on it (useful in windowed mode).
std::vector<std::pair<int, int>> AvailableResolutions(SDL_Window* window) {
  std::set<std::pair<int, int>, std::greater<>> sizes;
  SDL_DisplayID display = window ? SDL_GetDisplayForWindow(window) : SDL_GetPrimaryDisplay();
  int desktop_w = 0, desktop_h = 0;
  if (const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(display)) {
    desktop_w = desktop->w;
    desktop_h = desktop->h;
  }
  int count = 0;
  if (SDL_DisplayMode** modes = SDL_GetFullscreenDisplayModes(display, &count)) {
    for (int i = 0; i < count; ++i) {
      sizes.emplace(modes[i]->w, modes[i]->h);
    }
    SDL_free(modes);
  }
  for (auto [w, h] : {std::pair{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440},
                      {3840, 2160}}) {
    if (!desktop_w || (w <= desktop_w && h <= desktop_h)) {
      sizes.emplace(w, h);
    }
  }
  return {sizes.begin(), sizes.end()};
}

// Gamepad buttons forwarded to ImGui navigation, in gamepad_was_down_ order.
constexpr std::array<std::pair<SDL_GamepadButton, ImGuiKey>, 6> kNavButtons = {{
    {SDL_GAMEPAD_BUTTON_DPAD_UP, ImGuiKey_GamepadDpadUp},
    {SDL_GAMEPAD_BUTTON_DPAD_DOWN, ImGuiKey_GamepadDpadDown},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, ImGuiKey_GamepadDpadLeft},
    {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, ImGuiKey_GamepadDpadRight},
    {SDL_GAMEPAD_BUTTON_SOUTH, ImGuiKey_GamepadFaceDown},
    {SDL_GAMEPAD_BUTTON_EAST, ImGuiKey_GamepadFaceRight},
}};

}  // namespace

void ApplyStartupGraphicsSettings() {
  g_startup_vsync = REXCVAR_GET(host_vsync);
  g_startup_fps_unlock = GetBool("fps_unlock");
  // The presenter prefers immediate, then mailbox, then FIFO-relaxed, then FIFO.
  // VSync on removes the tearing / unsynchronized modes from that list.
  const char* allow = g_startup_vsync ? "false" : "true";
  rex::cvar::SetFlagByName("vulkan_allow_present_mode_immediate", allow);
  rex::cvar::SetFlagByName("vulkan_allow_present_mode_mailbox", allow);
}

void ApplyDisplayMode() {
  if (g_startup_render_scale.empty()) {
    g_startup_render_scale = rex::cvar::GetFlagByName("draw_resolution_scale_x");
  }
  SDL_Window* window = FindGameWindow();
  if (!window) {
    return;
  }
  const int w = REXCVAR_GET(display_width);
  const int h = REXCVAR_GET(display_height);
  if (GetBool("fullscreen")) {
    SDL_DisplayMode mode;
    if (w > 0 && h > 0 &&
        SDL_GetClosestFullscreenDisplayMode(SDL_GetDisplayForWindow(window), w, h, 0.0f, true,
                                            &mode)) {
      SDL_SetWindowFullscreenMode(window, &mode);  // Exclusive at the chosen size.
    } else {
      SDL_SetWindowFullscreenMode(window, nullptr);  // Borderless at desktop size.
    }
  } else if (w > 0 && h > 0) {
    SDL_SetWindowSize(window, w, h);
  }
}

GraphicsSettingsDialog::GraphicsSettingsDialog(rex::ui::ImGuiDrawer* drawer,
                                               std::filesystem::path config_path,
                                               std::function<void()> on_closed)
    : ImGuiDialog(drawer),
      config_path_(std::move(config_path)),
      on_closed_(std::move(on_closed)) {}

void GraphicsSettingsDialog::OnClose() {
  // Release any gamepad keys still held so ImGui doesn't keep them pressed.
  ImGuiIO& io = GetIO();
  for (size_t b = 0; b < kNavButtons.size(); ++b) {
    if (gamepad_was_down_[b]) {
      io.AddKeyEvent(kNavButtons[b].second, false);
    }
  }
  if (on_closed_) {
    on_closed_();
  }
}

void GraphicsSettingsDialog::FeedGamepadNavigation(ImGuiIO& io) {
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableGamepad;
  io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
  bool down[kNavButtons.size()] = {};
  int count = 0;
  if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
    for (int i = 0; i < count; ++i) {
      SDL_Gamepad* pad = SDL_GetGamepadFromID(ids[i]);
      if (!pad) {
        continue;
      }
      for (size_t b = 0; b < kNavButtons.size(); ++b) {
        down[b] = down[b] || SDL_GetGamepadButton(pad, kNavButtons[b].first);
      }
      // Left stick doubles as the d-pad.
      constexpr Sint16 kDeadzone = 16000;
      down[0] = down[0] || SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY) < -kDeadzone;
      down[1] = down[1] || SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTY) > kDeadzone;
      down[2] = down[2] || SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX) < -kDeadzone;
      down[3] = down[3] || SDL_GetGamepadAxis(pad, SDL_GAMEPAD_AXIS_LEFTX) > kDeadzone;
    }
    SDL_free(ids);
  }
  // B with no combo popup open closes the dialog; inside a popup ImGui uses it
  // to close the popup.
  const bool b_pressed = down[5] && !gamepad_was_down_[5];
  for (size_t b = 0; b < kNavButtons.size(); ++b) {
    if (down[b] != gamepad_was_down_[b]) {
      io.AddKeyEvent(kNavButtons[b].second, down[b]);
      gamepad_was_down_[b] = down[b];
    }
  }
  if (b_pressed && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
    Close();
  }
}

void GraphicsSettingsDialog::OnDraw(ImGuiIO& io) {
  FeedGamepadNavigation(io);

  const ImVec2 center(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
  ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  // Sized for reading from a couch: scale with the output height.
  const float ui_scale = std::max(1.0f, io.DisplaySize.y / 720.0f) * 1.6f;
  ImGui::SetNextWindowSize(ImVec2(520 * ui_scale, 0), ImGuiCond_Always);
  // Larger text for the whole dialog, combo popups included (1.92 font stack).
  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * ui_scale);
  bool open = true;
  if (!ImGui::Begin("Graphics Settings", &open,
                    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                        ImGuiWindowFlags_NoSavedSettings)) {
    ImGui::End();
    ImGui::PopFont();
    return;
  }
  if (ImGui::IsWindowAppearing()) {
    ImGui::SetKeyboardFocusHere();
  }
  const float kLabelWidth = 170.0f * ui_scale;
  auto label = [&](const char* text) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(text);
    ImGui::SameLine(kLabelWidth);
    ImGui::SetNextItemWidth(-1);
  };
  auto restart_note = [](bool pending) {
    if (pending) {
      ImGui::SameLine();
      ImGui::TextDisabled("(restart to apply)");
    }
  };

  // Display mode
  const bool fullscreen = GetBool("fullscreen");
  label("Display Mode");
  if (ImGui::BeginCombo("##mode", fullscreen ? "Fullscreen" : "Windowed")) {
    for (bool fs : {false, true}) {
      if (ImGui::Selectable(fs ? "Fullscreen" : "Windowed", fs == fullscreen) && fs != fullscreen) {
        // The SDK toggles the window from this cvar's change callback.
        rex::cvar::SetFlagByName("fullscreen", fs ? "true" : "false");
        ApplyDisplayMode();
      }
    }
    ImGui::EndCombo();
  }

  // Resolution (window size, or the exclusive fullscreen mode)
  SDL_Window* window = FindGameWindow();
  const int cur_w = REXCVAR_GET(display_width), cur_h = REXCVAR_GET(display_height);
  std::string desktop_label = "Desktop";
  if (const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(
          window ? SDL_GetDisplayForWindow(window) : SDL_GetPrimaryDisplay())) {
    desktop_label = fmt::format("Desktop ({}x{})", desktop->w, desktop->h);
  }
  const std::string cur_label =
      (cur_w > 0 && cur_h > 0) ? fmt::format("{}x{}", cur_w, cur_h) : desktop_label;
  label("Resolution");
  if (ImGui::BeginCombo("##resolution", cur_label.c_str())) {
    auto choose = [&](int w, int h) {
      rex::cvar::SetFlagByName("display_width", std::to_string(w));
      rex::cvar::SetFlagByName("display_height", std::to_string(h));
      ApplyDisplayMode();
    };
    if (ImGui::Selectable(desktop_label.c_str(), cur_w <= 0 || cur_h <= 0)) {
      choose(0, 0);
    }
    for (auto [w, h] : AvailableResolutions(window)) {
      if (ImGui::Selectable(fmt::format("{}x{}", w, h).c_str(), w == cur_w && h == cur_h)) {
        choose(w, h);
      }
    }
    ImGui::EndCombo();
  }

  // Render resolution (internal draw scale of the 1280x720 guest frame)
  const int scale = std::clamp(GetInt("draw_resolution_scale_x"), 1, kMaxRenderScale);
  auto scale_label = [](int s) {
    return fmt::format("{}x{} ({}x)", kGuestWidth * s, kGuestHeight * s, s);
  };
  label("Render Resolution");
  if (ImGui::BeginCombo("##render", scale_label(scale).c_str())) {
    for (int s = 1; s <= kMaxRenderScale; ++s) {
      if (ImGui::Selectable(scale_label(s).c_str(), s == scale)) {
        rex::cvar::SetFlagByName("draw_resolution_scale_x", std::to_string(s));
        rex::cvar::SetFlagByName("draw_resolution_scale_y", std::to_string(s));
      }
    }
    ImGui::EndCombo();
  }
  restart_note(!g_startup_render_scale.empty() &&
               rex::cvar::GetFlagByName("draw_resolution_scale_x") != g_startup_render_scale);

  bool fps_unlock = GetBool("fps_unlock");
  label("Unlock FPS");
  if (ImGui::Checkbox("##unlock", &fps_unlock)) {
    rex::cvar::SetFlagByName("fps_unlock", fps_unlock ? "true" : "false");
  }
  restart_note(fps_unlock != g_startup_fps_unlock);

  // FPS limit (real render cap)
  const int fps = GetInt("present_fps_limit");
  auto fps_label = [](int f) { return f > 0 ? fmt::format("{} FPS", f) : std::string("Unlimited"); };
  label("FPS Limit");
  if (ImGui::BeginCombo("##fps", fps_label(fps).c_str())) {
    for (int f : kFpsLimits) {
      if (ImGui::Selectable(fps_label(f).c_str(), f == fps)) {
        rex::cvar::SetFlagByName("present_fps_limit", std::to_string(f));
      }
    }
    ImGui::EndCombo();
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "Caps real rendered frames. With Unlock FPS enabled, physics keeps\n"
        "its original fixed tick independently of this cap. VSync can also\n"
        "limit rendering to your display refresh rate.");
  }

  // VSync
  bool vsync = REXCVAR_GET(host_vsync);
  label("VSync");
  if (ImGui::Checkbox("##vsync", &vsync)) {
    rex::cvar::SetFlagByName("host_vsync", vsync ? "true" : "false");
  }
  restart_note(vsync != g_startup_vsync);

  ImGui::Separator();
  if (ImGui::Button("Save")) {
    rex::cvar::SaveConfig(config_path_);
    saved_ = true;
    REXLOG_INFO("Graphics settings saved to {}", config_path_.string());
  }
  ImGui::SameLine();
  if (ImGui::Button("Close")) {
    open = false;
  }
  if (saved_) {
    ImGui::SameLine();
    ImGui::TextDisabled("Saved");
  }
  ImGui::SameLine();
  ImGui::TextDisabled("  A: select   B: back");
  ImGui::End();
  ImGui::PopFont();

  if (!open) {
    Close();
  }
}

}  // namespace trials_hd
