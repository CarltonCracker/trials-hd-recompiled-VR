// trials_hd - drives the game installer (src/installer/game_installer.h) for
// tests/installer/test_installer.py.
//
//   installer_cli scan <path>                  prints what Scan finds
//   installer_cli install <path> <assets_dir>  installs everything Scan finds

#include <cstdio>
#include <string>

#include "installer/game_installer.h"

using namespace trials_hd::installer;

namespace {

const char* KindName(SourceKind kind) {
  switch (kind) {
    case SourceKind::kBaseGame:
      return "game";
    case SourceKind::kDlcPackage:
      return "dlc-package";
    case SourceKind::kDlcFolder:
      return "dlc-folder";
    case SourceKind::kCustomTrack:
      return "track";
  }
  return "?";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: installer_cli scan <path> | install <path> <assets_dir>\n");
    return 2;
  }
  const std::string command = argv[1];
  auto result = Scan(argv[2]);
  for (const auto& source : result.sources) {
    std::printf("source %s %s %s %llu\n", KindName(source.kind), source.package_id.c_str(),
                source.display_name.c_str(), static_cast<unsigned long long>(source.bytes));
  }
  for (const auto& problem : result.problems) {
    std::printf("problem %s\n", problem.c_str());
  }
  if (command == "scan") return 0;
  if (command != "install" || argc < 4) return 2;

  Plan plan;
  for (const auto& source : result.sources) {
    if (source.kind == SourceKind::kBaseGame) {
      plan.base_game = source;
    } else if (source.kind == SourceKind::kCustomTrack) {
      plan.tracks.push_back(source);
    } else {
      plan.dlc.push_back(source);
    }
  }
  Progress progress;
  std::string error;
  if (!Install(plan, argv[3], progress, {}, &error)) {
    std::printf("error %s\n", error.c_str());
    return 1;
  }
  std::printf("installed %llu/%llu\n", static_cast<unsigned long long>(progress.bytes_done.load()),
              static_cast<unsigned long long>(progress.bytes_total.load()));
  return IsGameInstalled(argv[3]) || !plan.base_game ? 0 : 1;
}
