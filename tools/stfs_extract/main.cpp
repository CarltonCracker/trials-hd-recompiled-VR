#include "stfs_extract.h"

#include <iomanip>
#include <iostream>
#include <string>

int main(int argc, char* argv[]) {
  if (argc < 3) {
    std::cerr << "Extract files from an Xbox 360 STFS (LIVE/PIRS/CON) package.\n\n"
              << "Usage: stfs_extract <package> <out_dir> [--list]\n";
    return 1;
  }

  std::string pkg_path = argv[1];
  std::string out_dir = argv[2];
  bool list_only = false;
  for (int i = 3; i < argc; ++i) {
    if (std::string(argv[i]) == "--list") {
      list_only = true;
    }
  }

  stfs::StfsPackage pkg;
  std::string error;
  if (!pkg.Open(pkg_path, &error)) {
    std::cerr << "Error: " << error << "\n";
    return 1;
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
