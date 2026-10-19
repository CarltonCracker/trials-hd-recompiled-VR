# Game data, DLC, license and storage

Nothing here may be committed: `assets/`, `out/xex/`, packages, extracted files and saves
are the user's own copyrighted data (`/assets/`, `/game/`, `/dlc/`, `*.xex`, `*.pak`, `*.xwb` are
gitignored).

## Building: only `default.xex`

The game code is generated from `default.xex`, so the build needs it before anything else:
`./build.sh --game PATH` finds the base-game STFS package (magic `LIVE`/`PIRS`/`CON `,
title ID `0x5841095A`, content type `0x000D0000`) in `PATH` (a file or a folder searched
recursively) and runs `stfs_extract <package> out/xex --only default.xex` (`build.md`).
Nothing else of the game is extracted at build time.

## Installing: the setup screen (`src/installer/`)

On a start without an installed game, the game itself installs it (like other recomps):

- `TrialsHdApp::OnFinalizePaths` (`src/trials_hd_app.h`) runs once the window and ImGui
  exist and before the runtime. If `game_data_root` holds an installed game
  (`installer::IsGameInstalled`: non-empty `default.xex`, `data/data.pak`,
  `data/data_menu.pak`) and no install flag is given, it returns the paths and the game
  boots. Otherwise it shows `InstallerDialog` and returns `nullopt`; when the install
  finishes, the dialog's callback applies the license and calls `resume(paths)` on the
  next UI tick (not inside the ImGui frame), which builds the runtime and launches.
- **Install target:** `--game_data_root` if set (`run.sh` sets the repo's `assets/`), else
  `assets/` next to the executable (`installer::DefaultInstallDir`).
- **Finding packages** (`installer::Scan`, on a worker thread): a package file, an
  extracted DLC folder (contains `pack.pak`), or a folder walked up to 6 levels deep
  (symlinked folders not followed) probing each file's STFS header. Base game:
  title `5841095A`, type `000D0000`, containing `default.xex`, `data/data.pak`,
  `data/data_menu.pak`, **and a `default.xex` whose SHA-1 equals the build's**
  (`TRIALSHD_BUILD_XEX_SHA1`, recorded at configure time): another version would run the
  wrong code, so it is refused with a message. DLC: type `00000002`, title `5841095A` or
  `5842095A`. Other packages (saves, other games) are ignored.
- **Installing** (`installer::Install`, worker thread, cancellable, progress in bytes):
  checks free space, then extracts with `StfsPackage::ExtractAll` (each file streamed to a
  temp name and renamed when complete). A first install is assembled in
  `<assets>.installing/` and renamed to `assets/` at the end, so an interrupted install is
  never taken for a game (the next attempt deletes the leftover). An existing `assets/`
  (adding DLC, repairing) is written in place.
- **UI** (`installer_dialog.cpp`): game and DLC found (DLC with checkboxes), problems,
  Browse (a built-in ImGui file browser: places Home, `/`, `/run/media/$USER`, `/media`,
  `/mnt`, `/Volumes`; a typed path), the desktop's file/folder picker
  (`SDL_ShowOpenFileDialog`, which answers on another thread through a shared inbox; it
  needs xdg-desktop-portal or zenity on Linux), and files dropped on the window
  (`TrialsHdApp::OnFileDrop`). Controller: `GamepadNav` (`src/gamepad_nav.h`, shared with
  the settings menu) with its own `SDL_INIT_GAMEPAD`, since the runtime's input system
  doesn't exist yet. Each new screen focuses its main button with the nav cursor shown
  (ImGui ignores A while the cursor is hidden). F1 settings stay closed meanwhile.
- **Command line:** `--install_game=PATH` and `--install_dlc=PATH` (cvars defined in
  `installer_dialog.cpp`) prefill the screen and install without a click. They are
  cleared once read, so a settings save never keeps them in `trials_hd.toml`.

## License

For the base package, the installer writes `assets/package_license.toml`
(`license_mask = 0x…`, from the package's license block). At startup
`ApplyPackageLicense` (`src/package_license.cpp`, called from `OnPostInitLogging`, and
after a first install from the setup screen's callback) sets the SDK's `license_mask`
cvar from it, unless the user set `license_mask`. Without the file, the game runs as the
**trial**.

## DLC

The *Big Pack* / *Big Thrills* track packs install from the setup screen, from the
settings menu while the game runs (Game tab, **Add DLC...**: the same screen with
`in_game`, Cancel instead of Quit, "restart to play" when done), or with
`--install_dlc=PATH` into an installed game. DLC already installed shows as such and
starts unticked; a base-game package found then is skipped unless `--install_game` asked
for a reinstall. Files go to
`assets/content/0000000000000000/5841095A/00000002/<package id>/`, with a 332-byte content
header at `.../Headers/00000002/<package id>.header` (the console's layout; the SDK's
content manager reads `<game data>/content`). The package ID is the package's file name
when that is a 42-digit hex ID (marketplace names are), else SHA-1 of its first 64 KiB
(folders: of the name) + `58`. This layout, the IDs and the header bytes are the same as
the older `tools/add_dlc.py` (dev), which `tests/installer/test_installer.py` checks;
the Python tool also made an `assets/0000000000000000` copy/link that the SDK doesn't need.

## Where the game looks

`OnConfigurePaths` (`src/trials_hd_app.h`): `--game_data_root=PATH` if set, else the
first installed `assets/` next to the executable, then in the current directory. If none,
the setup screen (above) installs one.

## Where it writes (native)

Everything sits next to the executable (`OnConfigurePaths` points `user_data_root` and
`cache_root` at the config file's folder unless the user set them):

| Path | Contents |
| --- | --- |
| `trials_hd.toml` | All cvars/settings (`settings-ui-input.md`) |
| `5841095A/` | Saves and profiles: times, medals, bikes, custom tracks |
| `achievements/5841095A.toml` | Unlocked achievements |
| `cache/` | Shader/pipeline caches (`.xsh` shader cache) |
| `logs/trials_hd.log` | Log |

Saves found in the old location `~/.local/share/trials_hd` are copied over once (files
already present are not overwritten). Override with `--user_data_root` / `--cache_root`.

## Browser

The page imports the user's base package or extracted folder once into OPFS (`web/import.js`,
`src/web/web_files.cpp`); include `package_license.toml` in a folder import or the game runs
as a trial. Saves, settings, logs and caches persist under `/opfs/trials_hd` (per origin:
another port means another storage; clearing site data deletes saves). Development builds
can mount `assets/` served by `tools/web/serve.py --assets` read-only at `/assets` with
`?web_assets_url=/assets`. See `web-port.md`.
