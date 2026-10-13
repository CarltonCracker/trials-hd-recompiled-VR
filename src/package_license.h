// trials_hd - apply the license recorded from the player's own game package.

#pragma once

#include <filesystem>

namespace trials_hd {

// Reads <game_data_root>/package_license.toml (written by tools/stfs_extract.py
// from the package header) and reports its license mask to the game through
// the SDK's license_mask cvar, unless the user already set that cvar.
void ApplyPackageLicense(const std::filesystem::path& game_data_root);

}  // namespace trials_hd
