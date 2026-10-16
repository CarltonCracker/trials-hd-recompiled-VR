// trials_hd - web build: where the game data comes from.

#include "web/web_files.h"

#include <cerrno>
#include <charconv>
#include <cstring>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(TRIALSHD_WEB_WASMFS)
#include <emscripten/emscripten.h>
#include <emscripten/wasmfs.h>
#include "tools/stfs_extract/stfs_extract.h"
#endif

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(web_assets_url, "", "Web",
                      "Development: URL of your game folder served by tools/web/serve.py --assets "
                      "(for example /assets); mounted read-only at /assets");

namespace trials_hd::web {

namespace {

constexpr const char* kMountPoint = "/assets";
#if defined(TRIALSHD_WEB_WASMFS)
constexpr const char* kStorageRoot = "/opfs/trials_hd";

void StorageStatus(const std::string& text, bool error = false) {
  MAIN_THREAD_EM_ASM({ window.trialsHd.filesStatus?.(UTF8ToString($0), !!$1); },
                     text.c_str(), error);
}
#endif

bool ReadWholeFile(const std::string& path, std::string& out) {
  int fd = open(path.c_str(), O_RDONLY | O_CREAT, 0444);
  if (fd < 0) {
    return false;
  }
  char buffer[16384];
  ssize_t count;
  while ((count = read(fd, buffer, sizeof(buffer))) > 0) {
    out.append(buffer, size_t(count));
  }
  close(fd);
  return count == 0;
}

}  // namespace

std::filesystem::path PersistentDataRoot() {
#if defined(TRIALSHD_WEB_WASMFS)
  return kStorageRoot;
#else
  return {};
#endif
}

bool InitializeStorage() {
#if defined(TRIALSHD_WEB_WASMFS)
  try {
    auto backend = wasmfs_create_opfs_backend();
    if (!backend || wasmfs_create_directory("/opfs", 0777, backend) != 0) {
      throw std::runtime_error("Cannot mount browser storage");
    }
    const std::filesystem::path root(kStorageRoot);
    for (const auto* dir : {"home", "cache", "logs"}) {
      std::filesystem::create_directories(root / dir);
    }
    // Served assets remain a developer option; saves still persist in OPFS.
    if (REXCVAR_GET(web_assets_url).empty()) {
      const auto ready = root / "import-ready";
      if (!std::filesystem::exists(ready)) {
        const auto source = root / "package.stfs";
        if (std::filesystem::exists(source)) {
          stfs::StfsPackage package;
          std::string error;
          StorageStatus("Checking package...");
          if (!package.Open(source, &error)) throw std::runtime_error(error);
          if (package.title_id() != 0x5841095A || package.content_type() != 0x000D0000) {
            throw std::runtime_error("Select the Trials HD base game, not a save or DLC package");
          }
          if (!package.ExtractAll(root / "assets", true,
                [](const stfs::StfsEntry& entry, size_t index, size_t total) {
                  StorageStatus("Extracting " + std::to_string(index + 1) + "/" +
                                std::to_string(total) + ": " + entry.path);
                }, &error)) throw std::runtime_error(error);
          package.Close();
          std::filesystem::remove(source);
        }
        for (const auto* name : {"default.xex", "data/data.pak", "data/data_menu.pak"}) {
          if (!std::filesystem::is_regular_file(root / "assets" / name) ||
              std::filesystem::file_size(root / "assets" / name) == 0) {
            throw std::runtime_error(std::string("Incomplete game import: missing ") + name);
          }
        }
        // Commit last, outside paths controlled by the imported package.
        std::ofstream marker(ready);
        marker << "1\n";
        marker.close();
        if (!marker) throw std::runtime_error("Cannot save import completion marker");
      }
    }
    setenv("HOME", (root / "home").c_str(), 1);
    setenv("XDG_DATA_HOME", (root / "home").c_str(), 1);
    if (chdir(kStorageRoot) != 0) throw std::runtime_error("Cannot enter browser storage");
    MAIN_THREAD_EM_ASM({
      window.trialsHd.filesReady = true;
      document.getElementById('import-panel').hidden = true;
    });
    return true;
  } catch (const std::exception& error) {
    StorageStatus(std::string("Import/storage failed: ") + error.what() +
                  ". Reload to try again; existing saves are retained.", true);
    REXLOG_ERROR("web storage: {}", error.what());
    return false;
  }
#else
  return true;
#endif
}

std::filesystem::path MountServedAssets() {
  std::string url = REXCVAR_GET(web_assets_url);
  if (url.empty()) {
    const auto root = PersistentDataRoot();
    return root.empty() ? root : root / "assets";
  }
#if !defined(TRIALSHD_WEB_WASMFS)
  // The Node development build reads the host file system (--game_data_root).
  REXLOG_ERROR("web: --web_assets_url needs the browser build (WasmFS)");
  return {};
#else
  while (!url.empty() && url.back() == '/') {
    url.pop_back();
  }
  // Files of a fetch backend download from <url>/<path inside the mount>.
  backend_t backend = wasmfs_create_fetch_backend(url.c_str(), 0);
  if (!backend || wasmfs_create_directory(kMountPoint, 0777, backend) != 0) {
    REXLOG_ERROR("web: cannot mount {} at {}: {}", url, kMountPoint, std::strerror(errno));
    return {};
  }
  // The manifest lists "<size> <path>" per file; creating a file in the mount
  // declares it, and its contents arrive on the first read.
  std::string manifest;
  if (!ReadWholeFile(std::string(kMountPoint) + "/manifest.txt", manifest) || manifest.empty()) {
    REXLOG_ERROR("web: no file list at {}/manifest.txt (serve the game with --assets)", url);
    return {};
  }
  std::istringstream lines(manifest);
  std::string line;
  size_t files = 0;
  uint64_t bytes = 0;
  while (std::getline(lines, line)) {
    size_t space = line.find(' ');
    uint64_t size = 0;
    const auto parsed = std::from_chars(
        line.data(), line.data() + (space == std::string::npos ? 0 : space), size);
    const std::filesystem::path relative(space == std::string::npos ? "" : line.substr(space + 1));
    bool safe = !relative.empty() && !relative.is_absolute() &&
                relative.string().find_first_of("\\:") == std::string::npos &&
                relative.string().find('\0') == std::string::npos;
    for (const auto& part : relative) {
      if (part == "." || part == ".." || part.empty()) safe = false;
    }
    if (space == std::string::npos || parsed.ec != std::errc{} ||
        parsed.ptr != line.data() + space || !safe || size > UINT64_MAX - bytes) {
      REXLOG_ERROR("web: invalid asset manifest entry");
      return {};
    }
    std::filesystem::path path = std::filesystem::path(kMountPoint) / relative;
    std::string dir;
    for (const auto& part : path.parent_path()) {
      dir = dir == "/" || dir.empty() ? "/" + part.string() : dir + "/" + part.string();
      if (dir != "/") {
        if (mkdir(dir.c_str(), 0777) != 0 && errno != EEXIST) {
          REXLOG_ERROR("web: cannot create {}: {}", dir, std::strerror(errno));
          return {};
        }
      }
    }
    int fd = open(path.c_str(), O_WRONLY | O_CREAT, 0444);
    if (fd < 0) {
      REXLOG_ERROR("web: cannot declare {}: {}", path.string(), std::strerror(errno));
      return {};
    }
    close(fd);
    ++files;
    bytes += size;
  }
  REXLOG_INFO("web: mounted {} ({} files, {:.1f} MiB, fetched on first read) at {}", url, files,
              double(bytes) / (1024 * 1024), kMountPoint);
  return kMountPoint;
#endif
}

}  // namespace trials_hd::web
