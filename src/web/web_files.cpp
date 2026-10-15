// trials_hd - web build: where the game data comes from.

#include "web/web_files.h"

#include <cerrno>
#include <cstring>
#include <sstream>
#include <string>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#if defined(TRIALSHD_WEB_WASMFS)
#include <emscripten/wasmfs.h>
#endif

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_STRING(web_assets_url, "", "Web",
                      "Development: URL of your game folder served by tools/web/serve.py --assets "
                      "(for example /assets); mounted read-only at /assets");

namespace trials_hd::web {

namespace {

constexpr const char* kMountPoint = "/assets";

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

std::filesystem::path MountServedAssets() {
  std::string url = REXCVAR_GET(web_assets_url);
  if (url.empty()) {
    return {};
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
    if (space == std::string::npos) {
      continue;
    }
    std::filesystem::path path = std::filesystem::path(kMountPoint) / line.substr(space + 1);
    std::string dir;
    for (const auto& part : path.parent_path()) {
      dir = dir == "/" || dir.empty() ? "/" + part.string() : dir + "/" + part.string();
      if (dir != "/") {
        mkdir(dir.c_str(), 0777);  // EEXIST is fine.
      }
    }
    int fd = open(path.c_str(), O_WRONLY | O_CREAT, 0444);
    if (fd < 0) {
      REXLOG_ERROR("web: cannot declare {}: {}", path.string(), std::strerror(errno));
      return {};
    }
    close(fd);
    ++files;
    bytes += std::stoull(line.substr(0, space));
  }
  REXLOG_INFO("web: mounted {} ({} files, {:.1f} MiB, fetched on first read) at {}", url, files,
              double(bytes) / (1024 * 1024), kMountPoint);
  return kMountPoint;
#endif
}

}  // namespace trials_hd::web
