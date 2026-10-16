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
#include <rex/system/achievement_manager.h>
#include <rex/system/achievement_store.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/overlay/achievements_overlay.h>
#include <rex/ui/presenter.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/system/gpu_plugin.h>
#include <rex/system/xtypes.h>
#include <fmt/format.h>
#include <fstream>
#include <vector>

#if defined(__linux__) || defined(__APPLE__)
#include <unistd.h>
#endif

#include "generated/default/trials_hd_init.h"
#include "fallback_audio.h"
#include "graphics_settings.h"
#include "keyboard_controls.h"
#include "package_license.h"
#include "virtual_pad.h"

class TrialsHdApp : public rex::ReXApp {
 public:
  TrialsHdApp(rex::ui::WindowedAppContext& ctx, std::string_view name,
              rex::PPCImageInfo ppc_info)
      : rex::ReXApp(ctx, name, ppc_info) {
    s_instance_ = this;
  }

  ~TrialsHdApp() override {
    if (s_instance_ == this) {
      s_instance_ = nullptr;
    }
  }

  static TrialsHdApp* Instance() { return s_instance_; }

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<TrialsHdApp>(new TrialsHdApp(ctx, "trials_hd",
        PPCImageConfig));
  }

  void Quit() {
    REXLOG_INFO("TrialsHdApp::Quit: requesting application exit");
    app_context().CallInUIThreadDeferred([this] {
      if (window()) {
        window()->RequestClose();
      }
      if (runtime() && runtime()->kernel_state()) {
        runtime()->kernel_state()->TerminateTitle();
      }
      rex::FlushLogging();
      std::_Exit(0);
    });
  }

  void ShowAchievements() {
    app_context().CallInUIThreadDeferred([this] {
      OpenAchievements();
    });
  }

  void ToggleAchievements() {
    if (achievements_dialog_) {
      CloseAchievements();
    } else {
      OpenAchievements();
    }
  }

  void OpenAchievements() {
    if (achievements_dialog_) {
      return;
    }
    achievements_dialog_ = CreateAchievementsOverlay();
    if (auto* overlay = dynamic_cast<rex::ui::AchievementsOverlayDialog*>(achievements_dialog_.get())) {
      overlay->SetOnClose([this] { CloseAchievements(); });
    }
    achievements_open_ = (achievements_dialog_ != nullptr);
  }

  void CloseAchievements() {
    if (achievements_dialog_) {
      achievements_dialog_.reset();
      achievements_open_ = false;
      hold_input_until_released_ = true;
    }
  }

  static void ReExecWithGpuPlugin(std::string_view new_plugin) {
#if defined(__linux__)
    std::vector<std::string> args;
    std::ifstream cmdline("/proc/self/cmdline", std::ios::binary);
    if (cmdline) {
      std::string arg;
      while (std::getline(cmdline, arg, '\0')) {
        if (!arg.empty()) {
          args.push_back(arg);
        }
      }
    }
    if (args.empty()) {
      return;
    }
    bool replaced = false;
    for (auto& a : args) {
      if (a.rfind("--gpu_plugin=", 0) == 0) {
        a = fmt::format("--gpu_plugin={}", new_plugin);
        replaced = true;
      }
    }
    if (!replaced) {
      args.push_back(fmt::format("--gpu_plugin={}", new_plugin));
    }
    std::vector<char*> c_argv;
    c_argv.reserve(args.size() + 1);
    for (auto& a : args) {
      c_argv.push_back(a.data());
    }
    c_argv.push_back(nullptr);

    execv("/proc/self/exe", c_argv.data());
    REXLOG_ERROR("execv /proc/self/exe failed: {}", strerror(errno));
#endif
  }

  // A GPU plugin (librexgpu-*.so, staged next to the binary) is required to
  // render anything. The WebGPU renderer is the default unless --gpu_plugin
  // overrides; if it cannot start, the Vulkan one (xenos) is used instead.
  // Audio falls back to a silent driver when no output device can be opened.
  void OnPreSetup(rex::RuntimeConfig& config) override {
#if defined(__APPLE__)
    // MoltenVK Metal optimizations for Apple Silicon:
    // Silence verbose warning floods that stall I/O, enable fast math in MSL shaders
    setenv("MVK_CONFIG_LOG_LEVEL", "1", 0);
    setenv("MVK_CONFIG_FAST_MATH_ENABLED", "1", 0);
    setenv("MVK_CONFIG_USE_METAL_ARGUMENT_BUFFERS", "1", 0);
#endif
    if (config.gpu_plugin.empty()) {
      config.gpu_plugin = "webgpu";
    }

    REXLOG_INFO("OnPreSetup: gpu_plugin={}", config.gpu_plugin);

    if (config.gpu_plugin == "webgpu") {
      bool force_failure = (std::getenv("TRIALSHD_FORCE_WEBGPU_FAILURE") != nullptr);
      std::unique_ptr<rex::system::IGraphicsSystem> graphics;
      bool init_ok = false;
      if (!force_failure) {
        graphics = rex::system::LoadGpuPlugin("webgpu");
        if (graphics) {
          rex::X_STATUS status = graphics->SetupPresentation(&app_context());
          if (XSUCCEEDED(status)) {
            init_ok = true;
          } else {
            REXLOG_WARN("WebGPU SetupPresentation failed: {:08X}", status);
          }
        } else {
          REXLOG_WARN("Failed to load GPU plugin 'webgpu'");
        }
      } else {
        REXLOG_WARN("TRIALSHD_FORCE_WEBGPU_FAILURE is set; forcing WebGPU failure for fallback test");
      }

      if (!init_ok) {
        const char* guard = std::getenv("TRIALSHD_WEBGPU_FALLBACK");
        if (guard && std::string_view(guard) == "1") {
          REXLOG_ERROR("WebGPU fallback loop detected; aborting fallback");
        } else {
          REXLOG_INFO("WebGPU initialization failed; falling back to xenos via re-exec");
          setenv("TRIALSHD_WEBGPU_FALLBACK", "1", 1);
          ReExecWithGpuPlugin("xenos");
        }
        config.gpu_plugin = "xenos";
        config.graphics = nullptr;
      } else {
        config.graphics = std::move(graphics);
      }
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

    // Keep all save data, player profiles, achievements, cache, and settings
    // self-contained in the build directory right next to the settings config
    // (trials_hd.toml), and never save anything to system/home directories.
    auto base_dir = paths.config_path.parent_path();
    if (base_dir.empty()) {
      base_dir = rex::filesystem::GetExecutableFolder();
    }

    if (rex::cvar::GetFlagSource("user_data_root") == rex::cvar::Source::kDefault) {
      paths.user_data_root = base_dir;
    }
    if (rex::cvar::GetFlagSource("cache_root") == rex::cvar::Source::kDefault) {
      paths.cache_root = base_dir / "cache";
    }

    // If legacy save data exists in the user home directory (~/.local/share/trials_hd),
    // copy it into the build directory so existing player progress is not lost.
    auto legacy_dir = rex::filesystem::GetUserFolder() / "trials_hd";
    if (std::filesystem::exists(legacy_dir) && legacy_dir != base_dir) {
      std::error_code ec;
      for (const auto& entry : std::filesystem::directory_iterator(legacy_dir, ec)) {
        auto dest = base_dir / entry.path().filename();
        if (!std::filesystem::exists(dest)) {
          std::filesystem::copy(entry.path(), dest,
                                std::filesystem::copy_options::recursive |
                                std::filesystem::copy_options::skip_existing,
                                ec);
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

  // Settings (controls and graphics): F1, the controller's Guide button, or
  // Back + Start. Achievements: F7 or the main menu Achievements button.
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    trials_hd::ApplyDisplayMode();
    rex::ui::RegisterBind("bind_graphics_settings", "F1", "Toggle settings",
                          [this] { ToggleGraphicsSettings(); });
    rex::ui::UnregisterBind("bind_achievements");
    rex::ui::RegisterBind("bind_achievements", "F7", "Toggle achievements",
                          [this] { ToggleAchievements(); });
    combo_watcher_ = std::jthread([this](std::stop_token stop) { WatchSettingsCombo(stop); });
    (void)drawer;
  }

  void OnPostSetup() override {
    REXLOG_INFO("OnPostSetup: draw_resolution_scale_x={}",
                rex::cvar::GetFlagByName("draw_resolution_scale_x"));
    // Guest vblank pacing must not re-cap the independent render loop at 60.
    // Host VSync still controls tearing; physics is paced by the fixed clock.
    const bool unlocked = rex::cvar::GetFlagByName("fps_unlock") == "true";
    rex::cvar::SetFlagByName("present_fps_limit_guest_paced", unlocked ? "true" : "false");
    if (unlocked) {
      rex::cvar::SetFlagByName("vsync", "false");
    }
#if defined(__APPLE__)
    // For Apple Silicon (MacBook Air), default to 60 FPS cap if not explicitly set.
    // Prevents running at hundreds of FPS which causes severe thermal throttling on fanless hardware.
    if (rex::cvar::GetFlagSource("present_fps_limit") == rex::cvar::Source::kDefault) {
      rex::cvar::SetFlagByName("present_fps_limit", "60");
    }
#endif
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

    // For Vulkan: avoid splitting each frame into multiple queue submissions.
    // Batching primary buffers into a single submission per frame (matching WebGPU)
    // eliminates vkQueueSubmit overhead, fence thrashing, and pipeline stalls.
    if (rex::cvar::GetFlagSource("vulkan_submit_on_primary_buffer_end") == rex::cvar::Source::kDefault) {
      rex::cvar::SetFlagByName("vulkan_submit_on_primary_buffer_end", "false");
    }
    REXLOG_INFO("vulkan_submit_on_primary_buffer_end = {}",
                rex::cvar::GetFlagByName("vulkan_submit_on_primary_buffer_end"));

    // Auto exposure: the game reads its small luminance resolves (8x8..64x64)
    // back on the CPU; without readback the exposure never adapts and lit
    // scenes blow out. Read back only small resolves, one frame late (no GPU
    // stall) - the large render target copies would cost a lot of bandwidth.
    if (rex::cvar::GetFlagSource("readback_resolve") == rex::cvar::Source::kDefault) {
      rex::cvar::SetFlagByName("readback_resolve", "fast");
    }
    if (rex::cvar::GetFlagSource("readback_resolve_max_bytes") == rex::cvar::Source::kDefault) {
      rex::cvar::SetFlagByName("readback_resolve_max_bytes", "65536");
    }
    REXLOG_INFO("readback_resolve = {} (max {} bytes)",
                rex::cvar::GetFlagByName("readback_resolve"),
                rex::cvar::GetFlagByName("readback_resolve_max_bytes"));

    trials_hd::ApplyKeyboardDefaults();
    RegisterDefaultAchievements();

    // Replaces the SDK's input gate: the game ignores the controller while the
    // settings or achievements menu is open. Called from game
    // threads, so it must not touch ImGui (not thread-safe).
    if (auto* input = static_cast<rex::input::InputSystem*>(runtime()->input_system())) {
      input->SetActiveCallback([this] {
        return !graphics_settings_.load() && !achievements_open_.load() && !hold_input_until_released_.load();
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
        imgui_drawer(), config_path(),
        [this] {
          graphics_settings_ = nullptr;
          // The button that closed the menu is still down; don't let the game
          // see it as a fresh press.
          hold_input_until_released_ = true;
        });
  }

  // Polls controllers for the menu shortcut (Guide, or Back + Start) and
  // toggles the menu on the UI thread; also ends the post-close input hold once
  // every button and key is up. SDL's gamepad getters lock internally and the
  // keyboard state is a plain array, so this is safe off the UI thread.
  void WatchSettingsCombo(std::stop_token stop) {
    bool was_down = false;
    bool ach_close_was_down = false;
    while (!stop.stop_requested()) {
      bool shortcut = false;
      bool any_button = false;
      bool ach_close = false;
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
          ach_close = ach_close || SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_EAST) ||
                      SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_BACK) ||
                      SDL_GetGamepadButton(pad, SDL_GAMEPAD_BUTTON_GUIDE);
          for (int b = 0; b < SDL_GAMEPAD_BUTTON_COUNT; ++b) {
            any_button = any_button || SDL_GetGamepadButton(pad, SDL_GamepadButton(b));
          }
        }
        SDL_free(ids);
      }
      int key_count = 0;
      if (const bool* keys = SDL_GetKeyboardState(&key_count)) {
        for (int k = 0; k < key_count && !any_button; ++k) {
          any_button = keys[k];
        }
        if (key_count > SDL_SCANCODE_ESCAPE && keys[SDL_SCANCODE_ESCAPE]) {
          ach_close = true;
        }
        if (key_count > SDL_SCANCODE_BACKSPACE && keys[SDL_SCANCODE_BACKSPACE]) {
          ach_close = true;
        }
      }
      if (shortcut && !was_down) {
        app_context().CallInUIThreadDeferred([this] { ToggleGraphicsSettings(); });
      }
      was_down = shortcut;

      if (achievements_open_.load() && ach_close && !ach_close_was_down) {
        app_context().CallInUIThreadDeferred([this] { CloseAchievements(); });
      }
      ach_close_was_down = ach_close;

      if (!any_button) {
        hold_input_until_released_ = false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }
  }

  void RegisterDefaultAchievements() {
    if (!achievements().ListAchievements().empty()) {
      return;
    }
    static const rex::system::AchievementInfo kDefaults[] = {
        {4, "The Challenger", "Unlock the first tournament.", "Unlock the first tournament.", "ach_001.png", 4, 10, 0},
        {5, "Leave Nothing in One Piece", "Completely destroy the bike and break all the bones of your rider in a single crash.", "Completely destroy the bike and break all the bones of your rider in a single crash.", "ach_002.png", 5, 5, 0},
        {6, "The Creator", "Create a custom track and save it.", "Create a custom track and save it.", "ach_003.png", 6, 5, 0},
        {7, "Bling", "Get at least 5 Gold Medals.", "Get at least 5 Gold Medals.", "ach_004.png", 7, 15, 0},
        {8, "Full Throttle", "Complete the track 'Container Rush' without releasing the throttle.", "Complete the track 'Container Rush' without releasing the throttle.", "ach_005.png", 8, 15, 0},
        {9, "Market Meltdown", "Skyrocket all the crates in 'Stock Market' skill game.", "Skyrocket all the crates in 'Stock Market' skill game.", "ach_006.png", 9, 15, 0},
        {10, "Strike!", "Get a strike in the 'Midnight Bowling' skill game.", "Get a strike in the 'Midnight Bowling' skill game.", "ach_007.png", 10, 15, 0},
        {11, "Trials Master", "Unlock all tracks in the game.", "Unlock all tracks in the game.", "ach_008.png", 11, 15, 0},
        {12, "Unyielding", "Complete the track 'Groundhog Returns' without changing the posture of the rider.", "Complete the track 'Groundhog Returns' without changing the posture of the rider.", "ach_009.png", 12, 15, 0},
        {13, "Life is Harsh", "Complete all Hard tracks.", "Complete all Hard tracks.", "ach_010.png", 13, 20, 0},
        {14, "Demon on Wheels", "Complete all Extreme tracks.", "Complete all Extreme tracks.", "ach_011.png", 14, 30, 0},
        {15, "Marathon", "Complete the 'Ultimate Endurance' tournament without a single fault.", "Complete the 'Ultimate Endurance' tournament without a single fault.", "ach_000.png", 15, 40, 0},
    };
    achievements().RegisterAchievements(kDefaults);
  }

  inline static TrialsHdApp* s_instance_{nullptr};
  std::atomic<trials_hd::GraphicsSettingsDialog*> graphics_settings_{nullptr};
  std::unique_ptr<rex::ui::ImGuiDialog> achievements_dialog_;
  std::atomic<bool> achievements_open_{false};
  std::atomic<bool> hold_input_until_released_{false};
  std::jthread combo_watcher_;
};
