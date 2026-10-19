// trials_hd - apply the license recorded from the player's own game package.

#include "package_license.h"

#include <string>

#include <toml++/toml.hpp>

#include <rex/cvar.h>
#include <rex/logging.h>

namespace trials_hd {

void ApplyPackageLicense(const std::filesystem::path& game_data_root) {
  if (rex::cvar::GetFlagSource("license_mask") != rex::cvar::Source::kDefault) {
    return;  // Explicitly configured by the user.
  }
  auto path = game_data_root / "package_license.toml";
  if (!std::filesystem::is_regular_file(path)) {
    REXLOG_INFO("No package_license.toml in game data; the game will run as a trial. "
                "Install the game from its package (--install_game=PATH) to record its "
                "license.");
    return;
  }
  try {
    auto table = toml::parse_file(path.string());
    auto mask = table["license_mask"].value<int64_t>();
    if (!mask) {
      REXLOG_WARN("{}: missing license_mask", path.string());
      return;
    }
    rex::cvar::SetFlagByName("license_mask", std::to_string(*mask));
    REXLOG_INFO("Package license mask: {:#x}", *mask);
  } catch (const toml::parse_error& e) {
    REXLOG_WARN("{}: {}", path.string(), e.what());
  }
}

}  // namespace trials_hd
