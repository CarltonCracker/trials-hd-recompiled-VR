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
`show_performance_overlay` (`performance_overlay.cpp`); `install_game`, `install_dlc`
(`src/installer/installer_dialog.cpp`, `game-data.md`); `web_assets_url` (web); the
`webgpu_*` cvars (`src/gpu_webgpu/`, `renderer-webgpu.md`). Important SDK cvars:
`gpu_plugin`, `present_fps_limit`, `draw_resolution_scale_x/_y`, `fullscreen`, `vsync`,
`mnk_mode`, `keybind_*`, `vulkan_device`, `log_guest_fps`, `game_data_root`,
`user_data_root`, `cache_root`, `license_mask`.

## Settings menu (`src/graphics_settings.{h,cpp}`)

`GraphicsSettingsDialog` (ImGui), opened from the main menu's **PC SETTINGS** entry
(formerly Download Content), with **F1**, controller **Guide** or **Back + Start**
(a watcher thread in `TrialsHdApp::WatchSettingsCombo` polls controllers every
30 ms). The game-menu action queues an idempotent UI-thread open, not a toggle,
so repeated activation cannot close it. Tabs:

- **Controls:** keyboard on/off (`mnk_mode`), per-action key slots, Reset Keys.
- **Graphics:** renderer (Vulkan/WebGPU → `gpu_plugin`, restart), WebGPU adapter, display
  mode, resolution, render scale (1x–3x of 1280x720), unlock FPS (restart), FPS limit
  (Unlimited/240/165/144/120/90/60/30), VSync (restart; also switches the present-mode
  cvars), **FPS Counter**.
- **Game** (native, once the game runs): installed DLC (`installer::InstalledDlcNames`,
  from the content headers), the number of custom tracks, and **Add DLC or tracks...**,
  which closes the menu and opens the setup screen over the game without the base game (`InstallerDialog::Options::in_game`,
  `TrialsHdApp::OpenDlcInstaller`). The game's input is gated while it is open
  (`installer_open_`); the game sees new DLC after a restart, which its done screen says.

Controller navigation (d-pad/left stick, A, B, LB/RB) is `GamepadNav`
(`src/gamepad_nav.{h,cpp}`), shared with the setup screen: it reads every controller
(SDL, or the web page's snapshots) and feeds ImGui gamepad keys; `Pressed(kBack)` closes
the menu. The menu stays closed while the setup screen is up.

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

Other keys: **F2** FPS counter, **F7** achievements, double-click toggles
fullscreen (web).

## Overlays and theme

- `performance_overlay.{h,cpp}`: a number-only FPS counter at the top-left, with
  no units, frame-time statistics or graph. It reads the measured frame rate from
  `rex/perf/frame_timing.h`, keeps the existing color thresholds and crisp font
  scaling, and does not dim the game or keep the window repainting on its own.
  Settings calls it **FPS Counter**. Keep cvar `show_performance_overlay` and
  binding `bind_performance_overlay` stable for saved preferences/shortcuts.
- `ui_theme.{h,cpp}`: ImGui colors sampled from the main menu (brown panels, rust-orange
  accents, tan text), applied in `OnConfigureStyle` after the SDK defaults.
  The theme also configures the shared menu scale (`1.6 × max(1, height/720)`).
  Settings and achievements use the same font scale and base width; achievements
  scales icons/spacing too and scrolls within 92% of the viewport height.
- Achievements: the SDK's `AchievementsOverlayDialog`; the list is in
  `RegisterDefaultAchievements` (`trials_hd_app.h`) and `metadata/achievements.toml`.
  Its Close/X callback must defer `CloseAchievements()` with
  `CallInUIThreadDeferred`: the dialog and icon textures must stay alive until
  the current drawing/presentation stack returns. Resetting the owning
  `unique_ptr` inside `OnDraw` causes use-after-free and can freeze/crash the GPU.
  Do not use the base self-deleting `Close()` on this externally owned dialog.
  `achievements_controls.h` supplies controller input to the SDK list: D-pad or
  either stick scrolls, LB/RB pages, A closes, and the existing B/Back shortcut
  closes too. The first input poll consumes an already-held A so selecting the
  game menu entry does not immediately dismiss the overlay. Keyboard arrows and
  Page Up/Down also scroll. Hints are shown above the list; no child-window focus
  step is required. Scrolling, paging and A apply only while achievements has focus.

Verified 2026-10-19 with CPU UI tests and 1440p headless captures: the counter
contains only digits at the top-left, the counter toggle is present in Graphics, and
Close/X/Escape plus controller A/B closing allow reopening without a crash.
D-pad/stick scrolling reaches the last achievement; LB/RB page and returning to
the top works. Local artifacts are in `out/fps-counter/`,
`out/achievements-controller/` and `out/achievements-close/final/`.

**Menu backdrop:** interactive dialogs request `WantsBackgroundDim()` by default.
The drawer adds one 60%-black rectangle on its background draw list, behind all
ImGui windows. Multiple menus do not compound the dimming. Performance overlays
and achievement notifications opt out; the persistent idle notifier must never
leave the game dimmed. The Trials theme disables ImGui's separate modal dim color
so native modal dialogs do not double-dim. Closing the last menu removes the
backdrop even when the performance overlay remains visible.

Verified 2026-10-18 with 1440p and 720p headless screenshots in local
`out/ui-readability/{1440p,720p}/`. A fixed game-background patch measures 39.9%
brightness with Settings, achievements, or both open, and 100% with only the FPS
overlay or after closing the menus. Achievements uses the same 32px text / 1664px
width as Settings at 1440p (16px / 832px at 720p).

**Font sharpness (2026-10-18):** the SDK patch enables ImGui 1.92 dynamic texture
requests in `ImGuiDrawer`. Settings asks for 32px text at 1440p; the old renderer
stretched its initial 10px atlas with linear filtering. Fonts now rasterize at the
requested size and framebuffer DPI density. Atlas updates replace the immediate
texture only when glyphs change; the renderer retains responsibility for in-flight
GPU resource lifetime. Keep the existing font, colors and layout size. Do not
replace linear filtering with nearest filtering to hide undersized glyphs.

Verified Controls, Graphics, FPS and achievements screenshots at 2560x1440 using
Xvfb + software Vulkan; native-size comparisons are in the gitignored local
`out/ui-sharpness/{settings,fps}-comparison.png`. The CPU-only `imgui_sharpness`
ctest covers sizes 16–120px, DPI 100/125/150/200%, upload retry, stable frames,
and detach/reattach/destruction. See `testing.md` for screenshot commands.

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
`tools/headless_run.sh` + `tools/xsend.py` drive it (`testing.md`). It starts in
`OnPostSetup`, once the runtime's input system listens for new controllers. When the
first-start setup screen shows, it starts there instead so scripts can drive that screen;
the game then doesn't get pad input after the install (attached before the input system).
