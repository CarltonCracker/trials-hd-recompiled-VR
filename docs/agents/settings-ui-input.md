# Settings, UI, input and audio

## Cvars are the settings

Every setting is a cvar (`rex/cvar.h`): defined with `REXCVAR_DEFINE_<TYPE>(name, default,
"Category", "help")`, optionally `.range(...)` and `.lifecycle(kHotReload |
kRequiresRestart)`. Cvars load from `<exe dir>/trials_hd.toml` and from the command line
(`--name=value`, which wins); `rex::cvar::SaveConfig(path)` writes them back.

- Read: `REXCVAR_GET(name)` (own cvars), `rex::cvar::GetFlagByName("name")` /
  `rex::cvar::Query<T>("name")` (any cvar). Write: `rex::cvar::SetFlagByName`.
- **Changing a default in code:** only when `rex::cvar::GetFlagSource("x") ==
  rex::cvar::Source::kDefault`, so the user's config and command line still win
  (`OnPostSetup` in `src/trials_hd_app.h`).
- A GPU plugin's cvars exist only after the plugin loads; set them in `OnPostSetup`.

Cvars defined in this repo: `display_width`, `display_height`, `host_vsync`
(`graphics_settings.cpp`); `fps_unlock`, `log_simulation_rate` (`fps_unlock.cpp`);
`show_performance_overlay` (`performance_overlay.cpp`); `web_assets_url` (web); the
`webgpu_*` cvars (`src/gpu_webgpu/`, `renderer-webgpu.md`). Important SDK cvars:
`gpu_plugin`, `present_fps_limit`, `draw_resolution_scale_x/_y`, `fullscreen`, `vsync`,
`mnk_mode`, `keybind_*`, `vulkan_device`, `log_guest_fps`, `game_data_root`,
`user_data_root`, `cache_root`, `license_mask`.

## Settings menu (`src/graphics_settings.{h,cpp}`)

`GraphicsSettingsDialog` (ImGui), opened with **F1**, controller **Guide** or **Back +
Start** (a watcher thread in `TrialsHdApp::WatchSettingsCombo` polls controllers every
30 ms). Tabs:

- **Controls:** keyboard on/off (`mnk_mode`), per-action key slots, Reset Keys.
- **Graphics:** renderer (Vulkan/WebGPU → `gpu_plugin`, restart), WebGPU adapter, display
  mode, resolution, render scale (1x–3x of 1280x720), unlock FPS (restart), FPS limit
  (Unlimited/240/165/144/120/90/60/30), VSync (restart; also switches the present-mode
  cvars), performance overlay.

**Save** writes `trials_hd.toml`. Values needing a restart show a note
(`g_startup_*` keep what's in effect). `ApplyStartupGraphicsSettings` runs in
`OnPreSetup` (VSync picks the swapchain present mode), `ApplyDisplayMode` in
`OnCreateDialogs`. On the web the page owns canvas size and fullscreen.

While the settings or achievements menu is open the game gets no input
(`InputSystem::SetActiveCallback`), and after closing it input stays blocked until every
button is released (`hold_input_until_released_`).

## Keyboard (`src/keyboard_controls.{h,cpp}`)

The SDK's keyboard driver (`mnk_mode`) maps keys to player-one controller input,
alongside real controllers. `kKeyboardBindings` is the port's layout: each `keybind_*`
cvar holds comma-separated alternatives (`"Up,W"`). Arrows/WASD both ride and navigate
menus; the game ignores the D-pad while riding and the triggers in menus, so shared keys
never act twice. `ApplyKeyboardDefaults` applies the layout except where the user
configured keys. **Change bindings only from the settings menu**: game threads read
the strings without a lock while input is active.

Other keys: **F2** performance overlay, **F7** achievements, double-click toggles
fullscreen (web).

## Overlays and theme

- `performance_overlay.{h,cpp}`: frame rate, frame times and a graph from
  `rex/perf/frame_timing.h`; cvar `show_performance_overlay`. It must not keep the window
  repainting on its own.
- `ui_theme.{h,cpp}`: ImGui colors sampled from the main menu (brown panels, rust-orange
  accents, tan text), applied in `OnConfigureStyle` after the SDK defaults.
- Achievements: the SDK's `AchievementsOverlayDialog`; the list is in
  `RegisterDefaultAchievements` (`trials_hd_app.h`) and `metadata/achievements.toml`.

## Audio (`src/fallback_audio.{h,cpp}`)

`FallbackAudioSystem` keeps SDL output when a device opens, otherwise hands the game a
driver that drops frames at the real playback rate. Without it, a machine with no sound
device crashes: the game doesn't handle `XAudioRegisterRenderDriverClient` failing
(pure-virtual call, R6025). Set in `OnPreSetup`. Web audio is `web/audio*.js` + the SDK's
web driver (`web-port.md`).

## Virtual gamepad for tests (`src/virtual_pad.{h,cpp}`)

With `TRIALS_HD_VIRTUAL_PAD=<fifo>`, an SDL virtual gamepad reads commands from the FIFO:
`btn <a|b|x|y|back|guide|start|ls|rs|lb|rb|up|down|left|right> <0|1>`,
`axis <lx|ly|rx|ry|lt|rt> <value>`, `capture <path>` (guest frame as PPM, works with a
headless surface), `set <cvar> <value>`. It goes through the real SDL controller path.
`tools/headless_run.sh` + `tools/xsend.py` drive it (`testing.md`).
