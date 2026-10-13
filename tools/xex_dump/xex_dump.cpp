// xex_dump - write the loaded (decrypted, decompressed) image of a XEX to disk.
//
// Development aid for writing codegen overrides in trials_hd_manifest.toml:
// the output is meant for a disassembler, and it is derived from the user's own
// game files, so keep it out of the repository (out/ is gitignored).
//
// Usage: xex_dump <game_root> <xex relative to game_root> <out_dir>
// Writes <out_dir>/image.bin (base address .. base + image size) and
// <out_dir>/sections.txt (name, address, size, flags).

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

#include <rex/kernel/init.h>
#include <rex/runtime.h>
#include <rex/system/kernel_state.h>
#include <rex/system/user_module.h>
#include <rex/system/xex_module.h>

using rex::X_STATUS;

int main(int argc, char** argv) {
  if (argc != 4) {
    std::fprintf(stderr, "usage: %s <game_root> <xex> <out_dir>\n", argv[0]);
    return 1;
  }
  namespace fs = std::filesystem;
  fs::path out_dir = argv[3];
  fs::create_directories(out_dir);

  auto runtime = std::make_unique<rex::Runtime>(fs::canonical(argv[1]).string());
  if (runtime->Setup(rex::RuntimeConfig{
          .kernel_init = rex::kernel::InitializeKernel,
          .tool_mode = true,
      }) != X_STATUS_SUCCESS) {
    std::fprintf(stderr, "runtime setup failed\n");
    return 1;
  }

  std::string rel = argv[2];
  std::replace(rel.begin(), rel.end(), '/', '\\');
  if (runtime->LoadXexImage("game:\\" + rel) != X_STATUS_SUCCESS) {
    std::fprintf(stderr, "failed to load %s\n", argv[2]);
    return 1;
  }

  auto* xex = runtime->kernel_state()->GetExecutableModule()->xex_module();
  uint32_t base = xex->base_address();
  uint32_t size = xex->image_size();

  std::ofstream sections(out_dir / "sections.txt");
  sections << std::hex;
  sections << "base " << base << " size " << size << " entry " << xex->entry_point() << "\n";
  for (const auto& s : xex->binary_sections()) {
    sections << s.name << " " << s.virtual_address << " " << s.virtual_size
             << (s.executable ? " x" : "") << (s.writable ? " w" : "") << "\n";
  }

  // Sections are contiguous in guest memory; translate through the first one.
  const auto first = xex->binary_sections().front();
  const uint8_t* image = first.host_data - (first.virtual_address - base);
  std::ofstream(out_dir / "image.bin", std::ios::binary)
      .write(reinterpret_cast<const char*>(image), size);

  std::printf("base 0x%08X size 0x%X entry 0x%08X -> %s\n", base, size, xex->entry_point(),
              out_dir.string().c_str());
  return 0;
}
