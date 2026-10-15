// trials_hd - web build: where the game data comes from.

#pragma once

#if !defined(__EMSCRIPTEN__)
#error "web_files.h is for the web build only"
#endif

#include <filesystem>

namespace trials_hd::web {

// Development (WM1-WM2 checks): when --web_assets_url is set, mounts the game
// folder that tools/web/serve.py --assets serves there at /assets, read-only;
// each file is downloaded when the game first reads it. Returns the mount
// point, or an empty path. The real import into OPFS is W-L10.
std::filesystem::path MountServedAssets();

}  // namespace trials_hd::web
