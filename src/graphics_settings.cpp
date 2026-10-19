// trials_hd - player-facing settings and the dialog that edits them.

#include "graphics_settings.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>
#include <fmt/format.h>
#include <imgui.h>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "gamepad_nav.h"
#include "keyboard_controls.h"
#include "performance_overlay.h"

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
REXCVAR_DECLARE(std::string, gpu_plugin);

namespace trials_hd {

namespace {

// Trials HD renders a 1280x720 frame; the SDK scales its draws by an integer.
constexpr int kGuestWidth = 1280;
constexpr int kGuestHeight = 720;
constexpr int kMaxRenderScale = 3;

// Render caps are independent of the original fixed update clock.
constexpr std::array<int, 8> kFpsLimits = {0, 240, 165, 144, 120, 90, 60, 30};

// Values in effect since startup, for "restart to apply" notes.
std::string g_startup_render_scale;
std::string g_startup_renderer;
int g_startup_webgpu_adapter = -1;
bool g_startup_vsync = true;
bool g_startup_fps_unlock = true;
// Refresh rate of the display mode the window is presented in (UI thread writes).
std::atomic<float> g_refresh_rate{0.0f};

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

// Web: the page owns the canvas size and fullscreen state (W-L08), and SDL
// video is not used, so the display queries below are native only.
SDL_Window* FindGameWindow() {
#if defined(__EMSCRIPTEN__)
  return nullptr;
#else
  int count = 0;
  SDL_Window** windows = SDL_GetWindows(&count);
  SDL_Window* window = (windows && count > 0) ? windows[0] : nullptr;
  SDL_free(windows);
  return window;
#endif
}

// Distinct resolutions the window's display supports, largest first, plus
// common 16:9 sizes that fit on it (useful in windowed mode).
std::vector<std::pair<int, int>> AvailableResolutions(SDL_Window* window) {
  std::set<std::pair<int, int>, std::greater<>> sizes;
  int desktop_w = 0, desktop_h = 0;
#if defined(__EMSCRIPTEN__)
  (void)window;
#else
  SDL_DisplayID display = window ? SDL_GetDisplayForWindow(window) : SDL_GetPrimaryDisplay();
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
#endif
  for (auto [w, h] : {std::pair{1280, 720}, {1600, 900}, {1920, 1080}, {2560, 1440},
                      {3840, 2160}}) {
    if (!desktop_w || (w <= desktop_w && h <= desktop_h)) {
      sizes.emplace(w, h);
    }
  }
  return {sizes.begin(), sizes.end()};
}

// Keys that can be bound, with their SDK names (keybind_* values). Leaves out
// modifiers (a bare modifier bind never fires) and the overlay keys (F1 and
// the SDK's F3 / F4 / F7 / backtick).
const char* BindableKeyName(ImGuiKey key) {
  static const std::pair<ImGuiKey, const char*> kNames[] = {
      {ImGuiKey_UpArrow, "Up"}, {ImGuiKey_DownArrow, "Down"}, {ImGuiKey_LeftArrow, "Left"},
      {ImGuiKey_RightArrow, "Right"}, {ImGuiKey_Enter, "Return"}, {ImGuiKey_Space, "Space"},
      {ImGuiKey_Backspace, "Backspace"}, {ImGuiKey_Escape, "Escape"}, {ImGuiKey_Tab, "Tab"},
      {ImGuiKey_Insert, "Insert"}, {ImGuiKey_Home, "Home"}, {ImGuiKey_End, "End"},
      {ImGuiKey_PageUp, "PageUp"}, {ImGuiKey_PageDown, "PageDown"},
      {ImGuiKey_Minus, "Minus"}, {ImGuiKey_Equal, "Plus"}, {ImGuiKey_Comma, "Comma"},
      {ImGuiKey_Period, "Period"}, {ImGuiKey_Semicolon, "Semicolon"},
      {ImGuiKey_Slash, "Slash"}, {ImGuiKey_Backslash, "Backslash"},
      {ImGuiKey_LeftBracket, "LBracket"}, {ImGuiKey_RightBracket, "RBracket"},
      {ImGuiKey_Apostrophe, "Quote"}, {ImGuiKey_KeypadAdd, "NumpadPlus"},
      {ImGuiKey_KeypadSubtract, "NumpadMinus"}, {ImGuiKey_KeypadMultiply, "NumpadStar"},
      {ImGuiKey_KeypadDivide, "NumpadSlash"},
  };
  static const char* const kLetters[] = {"A", "B", "C", "D", "E", "F", "G", "H", "I",
                                         "J", "K", "L", "M", "N", "O", "P", "Q", "R",
                                         "S", "T", "U", "V", "W", "X", "Y", "Z"};
  static const char* const kDigits[] = {"0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
  static const char* const kNumpad[] = {"Numpad0", "Numpad1", "Numpad2", "Numpad3",
                                        "Numpad4", "Numpad5", "Numpad6", "Numpad7",
                                        "Numpad8", "Numpad9"};
  if (key >= ImGuiKey_A && key <= ImGuiKey_Z) {
    return kLetters[key - ImGuiKey_A];
  }
  if (key >= ImGuiKey_0 && key <= ImGuiKey_9) {
    return kDigits[key - ImGuiKey_0];
  }
  if (key >= ImGuiKey_Keypad0 && key <= ImGuiKey_Keypad9) {
    return kNumpad[key - ImGuiKey_Keypad0];
  }
  for (const auto& [k, name] : kNames) {
    if (k == key) {
      return name;
    }
  }
  return nullptr;
}

// Settings row label; the control that follows fills the rest of the row.
void Label(const char* text, float label_width) {
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(text);
  ImGui::SameLine(label_width);
  ImGui::SetNextItemWidth(-1);
}

void RestartNote(bool pending) {
  if (pending) {
    ImGui::SameLine();
    ImGui::TextDisabled("(restart to apply)");
  }
}

}  // namespace

void ApplyStartupGraphicsSettings() {
  // Record the renderer chosen at startup (may have come from config or CLI).
  std::string renderer = REXCVAR_GET(gpu_plugin);
  if (renderer.empty()) {
#if defined(__EMSCRIPTEN__)
    renderer = "webgpu";
#else
    renderer = "xenos";  // Vulkan is the native default (TrialsHdApp::OnPreSetup).
#endif
  }
  g_startup_renderer = renderer;

  std::string adapter_str = rex::cvar::GetFlagByName("webgpu_adapter");
  try {
    g_startup_webgpu_adapter = adapter_str.empty() ? -1 : std::stoi(adapter_str);
  } catch (...) {
    g_startup_webgpu_adapter = -1;
  }

  g_startup_vsync = REXCVAR_GET(host_vsync);
  g_startup_fps_unlock = GetBool("fps_unlock");
  // The presenter prefers immediate, then mailbox, then FIFO-relaxed, then FIFO.
  // VSync on removes the tearing / unsynchronized modes from that list.
  const char* allow = g_startup_vsync ? "false" : "true";
  rex::cvar::SetFlagByName("vulkan_allow_present_mode_immediate", allow);
  rex::cvar::SetFlagByName("vulkan_allow_present_mode_mailbox", allow);
  // Apply the same present-mode policy for WebGPU when it is the active renderer.
  if (renderer == "webgpu") {
    rex::cvar::SetFlagByName("webgpu_allow_present_mode_immediate", allow);
    rex::cvar::SetFlagByName("webgpu_allow_present_mode_mailbox", allow);
  }
}

void ApplyDisplayMode() {
  if (g_startup_render_scale.empty()) {
    g_startup_render_scale = rex::cvar::GetFlagByName("draw_resolution_scale_x");
  }
#if !defined(__EMSCRIPTEN__)
  SDL_Window* window = FindGameWindow();
  if (!window) {
    return;
  }
  const int w = REXCVAR_GET(display_width);
  const int h = REXCVAR_GET(display_height);
  const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
  const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(display);
  float refresh_rate = desktop ? desktop->refresh_rate : 0.0f;
  if (GetBool("fullscreen")) {
    SDL_DisplayMode mode;
    if (w > 0 && h > 0 &&
        SDL_GetClosestFullscreenDisplayMode(display, w, h, 0.0f, true, &mode)) {
      SDL_SetWindowFullscreenMode(window, &mode);  // Exclusive at the chosen size.
      refresh_rate = mode.refresh_rate;
    } else {
      SDL_SetWindowFullscreenMode(window, nullptr);  // Borderless at desktop size.
    }
  } else if (w > 0 && h > 0) {
    SDL_SetWindowSize(window, w, h);
  }
  g_refresh_rate.store(refresh_rate);
#endif
}

float DisplayRefreshRate() {
  float rate = g_refresh_rate.load();
#if defined(__EMSCRIPTEN__)
  // The browser exposes no display modes: unknown, pace by present_fps_limit.
  return rate;
#endif
  if (rate <= 0.0f) {
    if (SDL_Window* window = FindGameWindow()) {
      const SDL_DisplayID display = SDL_GetDisplayForWindow(window);
      if (const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(display)) {
        rate = desktop->refresh_rate;
        if (rate > 0.0f) {
          g_refresh_rate.store(rate);
        }
      }
    } else {
      if (const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(SDL_GetPrimaryDisplay())) {
        rate = desktop->refresh_rate;
        if (rate > 0.0f) {
          g_refresh_rate.store(rate);
        }
      }
    }
  }
  return rate;
}

float VsyncRefreshRate() {
  return g_startup_vsync ? DisplayRefreshRate() : 0.0f;
}

GraphicsSettingsDialog::GraphicsSettingsDialog(rex::ui::ImGuiDrawer* drawer,
                                               std::filesystem::path config_path,
                                               std::function<void()> on_closed,
                                               GameTab game_tab)
    : ImGuiDialog(drawer),
      config_path_(std::move(config_path)),
      on_closed_(std::move(on_closed)),
      game_tab_(std::move(game_tab)) {}

void GraphicsSettingsDialog::OnClose() {
  // Release any gamepad keys still held so ImGui doesn't keep them pressed.
  ImGuiIO& io = GetIO();
  io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
  nav_.Release(io);
  if (on_closed_) {
    on_closed_();
  }
}

void GraphicsSettingsDialog::FeedGamepadNavigation(ImGuiIO& io) {
  // Keyboard navigation too, except while waiting for a key to bind (the arrow
  // keys would move the focus instead).
  if (capture_binding_ >= 0) {
    io.ConfigFlags &= ~ImGuiConfigFlags_NavEnableKeyboard;
  } else {
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  }
  nav_.Feed(io);
  // B with no combo popup open closes the dialog; inside a popup ImGui uses it
  // to close the popup.
  const bool b_pressed = nav_.Pressed(GamepadNav::kBack);
  if (b_pressed && capture_binding_ >= 0) {
    capture_binding_ = -1;  // Cancels waiting for a key.
  } else if (b_pressed && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
    Close();
  }
}

// Controls: keyboard emulation of the controller (a real controller keeps
// working too) and its keys.
void GraphicsSettingsDialog::DrawControlsTab(float label_width, bool& key_bound) {
  const float kLabelWidth = label_width;
  auto label = [&](const char* text) { Label(text, label_width); };
  bool keyboard = GetBool("mnk_mode");
  label("Keyboard");
  if (ImGui::Checkbox("##keyboard", &keyboard)) {
    rex::cvar::SetFlagByName("mnk_mode", keyboard ? "true" : "false");
  }
  if (capture_binding_ >= 0 && ImGui::GetFrameCount() > capture_frame_) {
    // A fresh press (not the one that clicked the slot): bind it. Delete
    // clears the slot.
    for (ImGuiKey key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END;
         key = ImGuiKey(key + 1)) {
      if (!ImGui::IsKeyPressed(key, false)) {
        continue;
      }
      const char* name = key == ImGuiKey_Delete ? "" : BindableKeyName(key);
      if (name) {
        SetBindingKey(kKeyboardBindings[capture_binding_].cvar, capture_slot_, name);
        capture_binding_ = -1;
        key_bound = true;
        break;
      }
    }
  }
  if (keyboard) {
    const float slot_width =
        (ImGui::GetContentRegionAvail().x - kLabelWidth - ImGui::GetStyle().ItemSpacing.x) /
        2;
    for (size_t i = 0; i < kKeyboardBindingCount; ++i) {
      const KeyboardBinding& binding = kKeyboardBindings[i];
      if (binding.action.empty()) {
        continue;
      }
      const std::vector<std::string> keys = GetBindingKeys(binding.cvar);
      ImGui::AlignTextToFramePadding();
      ImGui::TextUnformatted(std::string(binding.action).c_str());
      for (size_t slot = 0; slot < 2; ++slot) {
        ImGui::SameLine(slot ? 0.0f : kLabelWidth);
        const bool waiting = capture_binding_ == int(i) && capture_slot_ == slot;
        const std::string text =
            waiting ? "Press a key..." : (slot < keys.size() ? keys[slot] : "-");
        // "###": the ID ignores the label, which changes while waiting.
        if (ImGui::Button(fmt::format("{}###key{}_{}", text, i, slot).c_str(),
                          ImVec2(slot_width, 0))) {
          if (waiting) {
            capture_binding_ = -1;
          } else {
            capture_binding_ = int(i);
            capture_slot_ = slot;
            capture_frame_ = ImGui::GetFrameCount();
          }
        }
      }
    }
    if (ImGui::Button("Reset Keys")) {
      ResetKeyboardBindings();
      capture_binding_ = -1;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Select a key, then press the new one (Delete clears)");
  }
}

// Game: the installed DLC track packs, and Add DLC (the installer screen; the
// game sees new DLC after a restart).
void GraphicsSettingsDialog::DrawGameTab(float label_width) {
  Label("DLC", label_width);
  if (game_tab_.installed_dlc.empty()) {
    ImGui::TextDisabled("None installed");
  } else {
    ImGui::BeginGroup();
    for (const auto& name : game_tab_.installed_dlc) {
      ImGui::TextUnformatted(name.c_str());
    }
    ImGui::EndGroup();
  }
  ImGui::Spacing();
  if (ImGui::Button("Add DLC...")) {
    game_tab_.on_add_dlc();
  }
  ImGui::SameLine();
  ImGui::TextDisabled("Big Pack, Big Thrills");
}

void GraphicsSettingsDialog::DrawGraphicsTab(float label_width) {
  auto label = [&](const char* text) { Label(text, label_width); };
  auto restart_note = RestartNote;

#if defined(__EMSCRIPTEN__)
  label("Renderer");
  ImGui::TextUnformatted("WebGPU (browser-selected adapter)");
  ImGui::TextDisabled("Resize the browser window; double-click the canvas for fullscreen.");
#else
  // Renderer (Vulkan / WebGPU)
  std::string cur_renderer = REXCVAR_GET(gpu_plugin);
  if (cur_renderer.empty()) {
    cur_renderer = "xenos";
  }
  label("Renderer");
  if (ImGui::BeginCombo("##renderer", cur_renderer == "webgpu" ? "WebGPU" : "Vulkan")) {
    if (ImGui::Selectable("Vulkan", cur_renderer == "xenos") && cur_renderer != "xenos") {
      rex::cvar::SetFlagByName("gpu_plugin", "xenos");
    }
    if (ImGui::Selectable("WebGPU", cur_renderer == "webgpu") && cur_renderer != "webgpu") {
      rex::cvar::SetFlagByName("gpu_plugin", "webgpu");
    }
    ImGui::EndCombo();
  }
  restart_note(cur_renderer != g_startup_renderer);

  // WebGPU Adapter
  if (cur_renderer == "webgpu" || g_startup_renderer == "webgpu") {
    int cur_adapter = -1;
    std::string adapter_str = rex::cvar::GetFlagByName("webgpu_adapter");
    if (!adapter_str.empty()) {
      try {
        cur_adapter = std::stoi(adapter_str);
      } catch (...) {}
    }
    auto adapter_label = [](int a) {
      return a >= 0 ? fmt::format("Adapter {}", a) : std::string("Default (-1)");
    };
    label("WebGPU Adapter");
    if (ImGui::BeginCombo("##webgpu_adapter", adapter_label(cur_adapter).c_str())) {
      if (ImGui::Selectable("Default (-1)", cur_adapter < 0)) {
        rex::cvar::SetFlagByName("webgpu_adapter", "-1");
      }
      for (int a = 0; a <= 3; ++a) {
        if (ImGui::Selectable(fmt::format("Adapter {}", a).c_str(), cur_adapter == a)) {
          rex::cvar::SetFlagByName("webgpu_adapter", std::to_string(a));
        }
      }
      ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) {
      ImGui::SetTooltip("WebGPU adapter index (-1 for default discrete GPU)");
    }
    restart_note(cur_adapter != g_startup_webgpu_adapter);
  }

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
#if !defined(__EMSCRIPTEN__)
  if (const SDL_DisplayMode* desktop = SDL_GetDesktopDisplayMode(
          window ? SDL_GetDisplayForWindow(window) : SDL_GetPrimaryDisplay())) {
    desktop_label = fmt::format("Desktop ({}x{})", desktop->w, desktop->h);
  }
#endif
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

#endif

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

#if defined(__EMSCRIPTEN__)
  label("VSync");
  ImGui::TextUnformatted("Browser managed");
#else
  // VSync
  bool vsync = REXCVAR_GET(host_vsync);
  label("VSync");
  if (ImGui::Checkbox("##vsync", &vsync)) {
    rex::cvar::SetFlagByName("host_vsync", vsync ? "true" : "false");
  }
  restart_note(vsync != g_startup_vsync);
#endif

  bool overlay = GetBool("show_performance_overlay");
  label("Performance Overlay");
  if (ImGui::Checkbox("##performance_overlay", &overlay)) {
    rex::cvar::SetFlagByName("show_performance_overlay", overlay ? "true" : "false");
    SyncPerformanceOverlay(imgui_drawer());
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip("Measured FPS, frame times and a frame-time graph (F2)");
  }
}

void GraphicsSettingsDialog::OnDraw(ImGuiIO& io) {
  FeedGamepadNavigation(io);

  const ImVec2 center(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f);
  ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  // Sized for reading from a couch: scale with the output height.
  const float ui_scale = imgui_drawer()->style().MenuScale(io.DisplaySize.y);
  ImGui::SetNextWindowSize(ImVec2(520 * ui_scale, 0), ImGuiCond_Always);
  // Taller than small screens with the key list: scroll instead.
  ImGui::SetNextWindowSizeConstraints(ImVec2(0, 0),
                                      ImVec2(FLT_MAX, io.DisplaySize.y * 0.92f));
  // Larger text for the whole dialog, combo popups included (1.92 font stack).
  ImGui::PushFont(nullptr, ImGui::GetStyle().FontSizeBase * ui_scale);
  bool open = true;
  if (!ImGui::Begin("Settings", &open,
                    ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                        ImGuiWindowFlags_NoSavedSettings)) {
    ImGui::End();
    ImGui::PopFont();
    return;
  }
  if (ImGui::IsWindowAppearing()) {
    ImGui::SetKeyboardFocusHere();
  }
  const float label_width = 170.0f * ui_scale;
  bool key_bound = false;
  // Tabs: click them, or LB / RB on a controller.
  if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) {
    requested_tab_ = (tab_ + TabCount() - 1) % TabCount();
  } else if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) {
    requested_tab_ = (tab_ + 1) % TabCount();
  }
  auto tab = [&](int index, const char* name) {
    const bool shown = ImGui::BeginTabItem(
        name, nullptr, requested_tab_ == index ? ImGuiTabItemFlags_SetSelected : 0);
    if (shown) {
      tab_ = index;
    }
    return shown;
  };
  if (ImGui::BeginTabBar("##settings_tabs")) {
    if (tab(0, "Controls")) {
      DrawControlsTab(label_width, key_bound);
      ImGui::EndTabItem();
    }
    if (tab(1, "Graphics")) {
      capture_binding_ = -1;  // Key capture belongs to the Controls tab.
      DrawGraphicsTab(label_width);
      ImGui::EndTabItem();
    }
    if (game_tab_.on_add_dlc && tab(2, "Game")) {
      capture_binding_ = -1;
      DrawGameTab(label_width);
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }
  requested_tab_ = -1;

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

  // Escape closes the menu, unless it was just bound to an action.
  if (capture_binding_ < 0 && !key_bound && ImGui::IsKeyPressed(ImGuiKey_Escape, false) &&
      !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId)) {
    open = false;
  }
  if (!open) {
    Close();
  }
}

}  // namespace trials_hd
