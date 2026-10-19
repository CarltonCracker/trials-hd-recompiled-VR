// trials_hd - command-line front end of the game's STFS reader
// (src/installer/stfs_package.h). build.sh uses it to take default.xex out of
// the player's package for codegen; it also lists and extracts whole packages.

#include "installer/stfs_package.h"

#include <fstream>
#include <iomanip>
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
  if (argc < 3) {
    std::cerr << "Extract files from an Xbox 360 STFS (LIVE/PIRS/CON) package.\n\n"
              << "Usage: stfs_extract <package> <out_dir> [--list] [--only <path in package>]\n";
    return 1;
  }

  std::string pkg_path = argv[1];
  std::string out_dir = argv[2];
  bool list_only = false;
  std::string only;
  for (int i = 3; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--list") {
      list_only = true;
    } else if (arg == "--only" && i + 1 < argc) {
      only = argv[++i];
    }
  }

  stfs::StfsPackage pkg;
  std::string error;
  if (!pkg.Open(pkg_path, &error)) {
    std::cerr << "Error: " << error << "\n";
    return 1;
  }

  if (!only.empty()) {
    const auto* entry = pkg.FindEntry(only);
    std::vector<uint8_t> data;
    if (!entry || entry->is_dir) {
      std::cerr << "Error: " << only << " is not in the package\n";
      return 1;
    }
    if (!pkg.ReadFile(*entry, data, &error)) {
      std::cerr << "Error: " << error << "\n";
      return 1;
    }
    std::error_code ec;
    const auto destination = std::filesystem::path(out_dir) / entry->name;
    std::filesystem::create_directories(out_dir, ec);
    const auto temporary = destination.string() + ".tmp";
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(data.data()), std::streamsize(data.size()));
    out.close();
    if (out) std::filesystem::rename(temporary, destination, ec);
    if (!out || ec) {
      std::filesystem::remove(temporary, ec);
      std::cerr << "Error: failed to write " << destination.string() << "\n";
      return 1;
    }
    std::cout << destination.string() << "\n";
    return 0;
  }

  std::cout << "title " << std::uppercase << std::hex << std::setw(8) << std::setfill('0')
            << pkg.title_id() << " content " << std::setw(8) << std::setfill('0')
            << pkg.content_type() << std::nouppercase << std::dec << " '" << pkg.display_name()
            << "' read_only=" << (pkg.read_only() ? "True" : "False")
            << " blocks=" << pkg.total_blocks() << "\n";

  for (const auto& e : pkg.entries()) {
    if (e.is_dir) continue;
    std::cout << std::setw(12) << std::setfill(' ') << e.size << "  " << e.path << "\n";
  }

  uint32_t mask = pkg.license_mask();
  std::cout << "license entries: " << pkg.licenses().size()
            << "  mask: 0x" << std::uppercase << std::hex << mask << std::dec << "\n";

  if (!list_only) {
    if (!pkg.ExtractAll(out_dir, true, nullptr, &error)) {
      std::cerr << "Extraction failed: " << error << "\n";
      return 1;
    }
  }

  return 0;
}
