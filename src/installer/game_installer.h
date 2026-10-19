// trials_hd - installs the player's own game and DLC packages into assets/.
//
// The game binary is built from the player's default.xex (codegen), but the
// rest of the game data is only read at run time. On first start the setup
// screen (installer_dialog.h) asks for the Trials HD package and optional DLC
// track packs and extracts them here; later starts find assets/ and boot.
// No UI and no logging in this file, so tests can link it on its own.

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

namespace trials_hd::installer {

inline constexpr uint32_t kTitleId = 0x5841095A;
// The title ID some marketplace DLC packages carry; they install like the rest.
inline constexpr uint32_t kDlcAltTitleId = 0x5842095A;
inline constexpr uint32_t kContentTypeGame = 0x000D0000;  // XBLA title package
inline constexpr uint32_t kContentTypeDlc = 0x00000002;   // marketplace content
// Saved games: Trials HD keeps each custom track in one (record.bin, ghost.bin,
// user_created_track.trk).
inline constexpr uint32_t kContentTypeSavedGame = 0x00000001;
// The SDK's one player profile (src/system/xam/user_profile.cpp). The game
// opens saved games under it, so custom tracks install there.
inline constexpr uint64_t kProfileXuid = 0xB13EBABEBABEBABE;
// The level inside a custom-track package; its other files are the creator's
// best run (record.bin) and its replay (ghost.bin), which are not installed.
inline constexpr std::string_view kTrackFile = "user_created_track.trk";

// assets/ holds the extracted game: default.xex plus the main data packs.
// Inline: the app's path lookup uses it in the web build too, which doesn't
// compile the installer.
inline bool IsGameInstalled(const std::filesystem::path& dir) {
  std::error_code ec;
  for (const char* name : {"default.xex", "data/data.pak", "data/data_menu.pak"}) {
    if (!std::filesystem::is_regular_file(dir / name, ec) ||
        std::filesystem::file_size(dir / name, ec) == 0) {
      return false;
    }
  }
  return true;
}

// Where a fresh install goes: assets/ next to the executable.
std::filesystem::path DefaultInstallDir();

// SHA-1 (lowercase hex) of the default.xex this binary was compiled from, or
// empty when the build did not record one (then any version is accepted).
std::string_view BuildXexSha1();

enum class SourceKind { kBaseGame, kDlcPackage, kDlcFolder, kCustomTrack };

struct Source {
  SourceKind kind = SourceKind::kBaseGame;
  std::filesystem::path path;
  std::string display_name;
  std::string package_id;  // DLC, track: its folder name under content/
  uint32_t license_mask = 1;
  uint64_t bytes = 0;      // what installing it writes
};

struct ScanResult {
  std::vector<Source> sources;        // usable packages: base game, DLC, then tracks
  std::vector<std::string> problems;  // why other candidates were skipped
};

// Looks for the base game, DLC and custom tracks at `path`: a package file, an
// extracted DLC folder (with pack.pak), or a folder searched recursively for
// packages.
// Checks that the base game's default.xex is the one this binary was built from.
ScanResult Scan(const std::filesystem::path& path, std::stop_token stop = {});

// Shared between the install thread and the UI.
class Progress {
 public:
  std::atomic<uint64_t> bytes_done{0};
  std::atomic<uint64_t> bytes_total{0};

  void SetStage(std::string stage) {
    std::lock_guard lock(mutex_);
    stage_ = std::move(stage);
  }
  std::string stage() const {
    std::lock_guard lock(mutex_);
    return stage_;
  }

 private:
  mutable std::mutex mutex_;
  std::string stage_;
};

struct Plan {
  std::optional<Source> base_game;
  std::vector<Source> dlc;
  std::vector<Source> tracks;
};

// Installs the plan into `assets_dir`. A first install is assembled in a
// sibling "<name>.installing" folder and renamed into place when complete, so
// an interrupted install is never mistaken for a game; an existing assets_dir
// (adding DLC, repairing) is written in place, file by file.
bool Install(const Plan& plan, const std::filesystem::path& assets_dir, Progress& progress,
             std::stop_token stop, std::string* error);

// The console's content header (.header) the SDK's content manager reads for
// an installed package (its name in game lists): 332 bytes, as
// tools/add_dlc.py writes it.
std::array<uint8_t, 332> MakeContentHeader(uint32_t content_type, std::string_view display_name,
                                           std::string_view package_id, uint32_t license_mask);

// Whether DLC with this package ID is installed in assets_dir (its header exists).
bool IsDlcInstalled(const std::filesystem::path& assets_dir, std::string_view package_id);

// Display names of the DLC installed in assets_dir, from their headers.
std::vector<std::string> InstalledDlcNames(const std::filesystem::path& assets_dir);

// Number of custom tracks installed in assets_dir.
size_t InstalledTrackCount(const std::filesystem::path& assets_dir);

// Package folder name: the package's own name when it is already a 42-digit
// hex ID (marketplace file names are), else a stable ID derived from it.
std::string DerivePackageId(const std::filesystem::path& path);

}  // namespace trials_hd::installer
