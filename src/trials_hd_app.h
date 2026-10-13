// trials_hd - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <atomic>
#include <cstdio>
#include <chrono>
#include <filesystem>
#include <thread>

#include <SDL3/SDL.h>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/rex_app.h>
#include <rex/runtime.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/presenter.h>
#include <rex/system/interfaces/graphics.h>

#include "fallback_audio.h"
#include "graphics_settings.h"
#include "package_license.h"
#include "virtual_pad.h"

class TrialsHdApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<TrialsHdApp>(new TrialsHdApp(ctx, "trials_hd",
        PPCImageConfig));
  }

  // The Xenos GPU plugin (librexgpu-xenos*.so, staged next to the binary) is
  // required to render anything; default to it unless --gpu_plugin overrides.
  // Audio falls back to a silent driver when no output device can be opened.
  void OnPreSetup(rex::RuntimeConfig& config) override {
    if (config.gpu_plugin.empty()) {
      config.gpu_plugin = "xenos";
    }
    config.audio_factory = REX_AUDIO_BACKEND(trials_hd::FallbackAudioSystem);
    trials_hd::ApplyStartupGraphicsSettings();
  }

  // Game content is never bundled: the player extracts their own copy of
  // Trials HD into an `assets` folder (see README). Without --game_data_root,
  // look next to the executable first, then in the working directory.
  void OnConfigurePaths(rex::PathConfig& paths) override {
    if (paths.game_data_root.empty()) {
      for (const auto& dir : {rex::filesystem::GetExecutableFolder() / "assets",
                              std::filesystem::current_path() / "assets"}) {
        if (std::filesystem::is_regular_file(dir / "default.xex")) {
          paths.game_data_root = dir;
          break;
        }
      }
    }
  }

  // Report the license stored in the player's own package (full or trial).
  // Runs after the config file is loaded so an explicit setting there wins.
  void OnPostInitLogging() override {
    if (!game_data_root().empty()) {
      trials_hd::ApplyPackageLicense(game_data_root());
    }
  }

  // Graphics settings: F1, the controller's Guide button, or Back + Start.
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    trials_hd::ApplyDisplayMode();
    rex::ui::RegisterBind("bind_graphics_settings", "F1", "Toggle graphics settings",
                          [this] { ToggleGraphicsSettings(); });
    combo_watcher_ = std::jthread([this](std::stop_token stop) { WatchSettingsCombo(stop); });
    (void)drawer;
  }

  void OnPostSetup() override {
    // Trials HD's memexport draws (GPU point-list passes) are only consumed by
    // later GPU work. Reading them back to guest memory forces a full CPU/GPU
    // sync per draw and caps gameplay at 15-30 FPS; without it the game holds
    // 60 FPS at 1440p on an RX 6600. Set here because the cvar belongs to the
    // GPU plugin, which is loaded after logging/config setup. Still
    // overridable in the config.
    if (rex::cvar::GetFlagSource("readback_memexport") == rex::cvar::Source::kDefault) {
      rex::cvar::SetFlagByName("readback_memexport", "false");
    }
    REXLOG_INFO("readback_memexport = {}", rex::cvar::GetFlagByName("readback_memexport"));

    // Replaces the SDK's input gate: the game ignores the controller while the
    // settings menu is open (it navigates the menu instead).
    if (auto* input = static_cast<rex::input::InputSystem*>(runtime()->input_system())) {
      input->SetActiveCallback([this] {
        if (graphics_settings_.load() || hold_input_until_released_.load()) {
          return false;
        }
        auto* drawer = imgui_drawer();
        return !drawer || !drawer->GetIO().WantCaptureMouse;
      });
    }
    // Test harness only: no-op unless TRIALS_HD_VIRTUAL_PAD is set.
    trials_hd::StartVirtualPadFromEnvironment([this](const std::string& path) {
      app_context().CallInUIThreadDeferred([this, path] { CaptureGuestFrame(path); });
    });
  }

  void OnShutdown() override {
    combo_watcher_.request_stop();
    if (combo_watcher_.joinable()) {
      combo_watcher_.join();
    }
  }

 private:
  // Saves the current guest output (what the game rendered, before overlays and
  // window scaling) as a binary PPM. Works with the headless surface too.
  void CaptureGuestFrame(const std::string& path) {
    auto* graphics = runtime() ? runtime()->graphics_system() : nullptr;
    auto* presenter = graphics ? graphics->presenter() : nullptr;
    rex::ui::RawImage image;
    if (!presenter || !presenter->CaptureGuestOutput(image)) {
      return;
    }
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
      std::fprintf(f, "P6\n%u %u\n255\n", image.width, image.height);
      for (uint32_t y = 0; y < image.height; ++y) {
        const uint8_t* row = image.data.data() + y * image.stride;
        for (uint32_t x = 0; x < image.width; ++x) {
          std::fwrite(row + x * 4, 1, 3, f);
        }
      }
      std::fclose(f);
    }
  }

  void ToggleGraphicsSettings() {
    if (auto* dialog = graphics_settings_.load()) {
      dialog->RequestClose();
      return;
    }
    if (!imgui_drawer()) {
      return;
    }
    graphics_settings_ = new trials_hd::GraphicsSettingsDialog(
        imgui_drawer(), rex::filesystem::GetExecutableFolder() / "trials_hd.toml",
        [this] {
          graphics_settings_ = nullptr;
          // The button that closed the menu is still down; don't let the game
          // see it as a fresh press.
          hold_input_until_released_ = true;
        });
  }

  // Polls controllers for the menu shortcut (Guide, or Back + Start) and
  // toggles the menu on the UI thread; also ends the post-close input hold once
  // every button is up. SDL's gamepad getters lock internally, so this is safe
  // off the UI thread.
  void WatchSettingsCombo(std::stop_token stop) {
    bool was_down = false;
    while (!stop.stop_requested()) {
      bool shortcut = false;
      bool any_button = false;
      int count = 0;
      if (SDL_JoystickID* ids = SDL_GetGamepads(&count)) {
        for (int i = 0; i < count; ++i) {
          SDL_Gamepad* pad = SDL_GetGamepadFromID(ids[i]);
          if (!pad) {
            continue;
          }
          shortcut = shortcut || SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_GUIDE) ||
                     (SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK) &&
                      SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_START));
          for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) {
            any_button = any_button || SDL_GetGamepadButton(pad, SDL_GamepadButton(b));
          }
        }
        SDL_free(ids);
      }
      if (shortcut && !was_down) {
        app_context().CallInUIThreadDeferred([this] { ToggleGraphicsSettings(); });
      }
      was_down = shortcut;
      if (!any_button) {
        hold_input_until_released_ = false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
  }

  std::atomic<trials_hd::GraphicsSettingsDialog*> graphics_settings_{nullptr};
  std::atomic<bool> hold_input_until_released_{false};
  std::jthread combo_watcher_;
};
