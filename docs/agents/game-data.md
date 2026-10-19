# Game data, DLC, license and storage

Nothing here may be committed: `assets/`, packages, extracted files and saves are the
user's own copyrighted data (`/assets/`, `/game/`, `/dlc/`, `*.xex`, `*.pak`, `*.xwb` are
gitignored).

## Getting the assets

`./build.sh --game PATH` finds the base-game STFS package (magic `LIVE`/`PIRS`/`CON `,
title ID `0x5841095A`, content type `0x000D0000`) in `PATH` (a file or a folder searched
recursively) and runs:

```sh
python3 tools/stfs_extract.py <package> assets      # --list to only list
```

`build.sh` treats `assets/` as complete when it contains non-empty `default.xex`,
`data/data.pak`, `data/data_menu.pak` and `package_license.toml`; otherwise it asks for
`--game` (or use `--force-extract`). `tools/stfs_extract/` is the same extractor in C++
(the `stfs_extract` target; the web build compiles it too); its block/hash math mirrors the
SDK's `src/filesystem/devices/stfs_container_device.cpp`.

## License

For the base package, `stfs_extract.py` writes `assets/package_license.toml`
(`license_mask = 0x…`, from the package's license block). At startup
`ApplyPackageLicense` (`src/package_license.cpp`, called from `OnPostInitLogging`) sets
the SDK's `license_mask` cvar from it, unless the user set `license_mask`. Without the
file, the game runs as the **trial**.

## DLC

`./add_dlc.sh <addon folder or package> [assets_dir]` (wraps `tools/add_dlc.py`) installs
the *Big Pack* / *Big Thrills* track packs: a single STFS package, an addon folder, a
folder of addons, or an extracted folder with `pack.pak`. Files go to
`assets/content/0000000000000000/5841095A/<content type>/<package id>/`, with the package
header under `.../Headers/` (the console's content layout).

## Where the game looks

`OnConfigurePaths` (`src/trials_hd_app.h`): `--game_data_root=PATH` if set, else the
first `assets/` containing `default.xex`, next to the executable, then in the current
directory.

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
