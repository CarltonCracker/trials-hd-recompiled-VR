// trials_hd - web build: where the game data comes from.

#pragma once

#if !defined(__EMSCRIPTEN__)
#error "web_files.h is for the web build only"
#endif

#include <filesystem>

namespace trials_hd::web {

// Mount OPFS and finish the first import before configuration/runtime setup.
// Node development builds do nothing. Failures are shown in the page and log.
bool InitializeStorage();

// Empty for Node; browser saves/settings/cache live below this directory.
std::filesystem::path PersistentDataRoot();

// Development (WM1-WM2 checks): when --web_assets_url is set, mounts the game
// folder that tools/web/serve.py --assets serves there at /assets, read-only;
// each file is downloaded when the game first reads it. Returns the mount
// point, or the imported OPFS assets when the URL is empty.
std::filesystem::path MountServedAssets();

}  // namespace trials_hd::web
