// trials_hd - installs the player's own game and DLC packages (see game_installer.h).

#include "installer/game_installer.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <unordered_set>

#include <fmt/format.h>
#include <TinySHA1.hpp>

#include <rex/filesystem.h>

#include "installer/stfs_package.h"
#include "trials_hd_build_info.h"

namespace trials_hd::installer {

namespace fs = std::filesystem;

namespace {

constexpr std::string_view kXuidDir = "0000000000000000";

bool Fail(std::string* error, std::string message) {
  if (error) *error = std::move(message);
  return false;
}

std::string HexDigest(sha1::SHA1& hash) {
  sha1::SHA1::digest32_t digest;
  hash.finalize(digest);
  return fmt::format("{:08x}{:08x}{:08x}{:08x}{:08x}", digest[0], digest[1], digest[2],
                     digest[3], digest[4]);
}

std::string ToUpper(std::string text) {
  std::ranges::transform(text, text.begin(), [](unsigned char c) { return std::toupper(c); });
  return text;
}

bool IsHex(std::string_view text) {
  return std::ranges::all_of(text, [](unsigned char c) { return std::isxdigit(c); });
}

// The 4-byte magic, content type and title ID from an STFS header, without
// parsing the rest (scanning a folder probes every file).
struct Probe {
  uint32_t content_type = 0;
  uint32_t title_id = 0;
};

std::optional<Probe> ProbePackage(const fs::path& path) {
  std::error_code ec;
  if (!fs::is_regular_file(path, ec) || fs::file_size(path, ec) < 0x971A) return std::nullopt;
  std::ifstream file(path, std::ios::binary);
  uint8_t header[0x364];
  if (!file.read(reinterpret_cast<char*>(header), sizeof(header))) return std::nullopt;
  std::string_view magic(reinterpret_cast<const char*>(header), 4);
  if (magic != "LIVE" && magic != "PIRS" && magic != "CON ") return std::nullopt;
  auto be32 = [&](size_t offset) {
    return (uint32_t(header[offset]) << 24) | (uint32_t(header[offset + 1]) << 16) |
           (uint32_t(header[offset + 2]) << 8) | uint32_t(header[offset + 3]);
  };
  return Probe{be32(0x344), be32(0x360)};
}

bool IsDlcProbe(const Probe& probe) {
  return probe.content_type == kContentTypeDlc &&
         (probe.title_id == kTitleId || probe.title_id == kDlcAltTitleId);
}

std::string FileLabel(const fs::path& path) { return path.filename().string(); }

// Base game package: right title, content type and data, and the default.xex
// this binary was compiled from (other versions would run the wrong code).
std::optional<Source> CheckBaseGame(const fs::path& path, std::vector<std::string>& problems) {
  stfs::StfsPackage package;
  std::string error;
  if (!package.Open(path, &error)) {
    problems.push_back(fmt::format("{}: {}", FileLabel(path), error));
    return std::nullopt;
  }
  const auto* xex = package.FindEntry("default.xex");
  if (!xex || !package.FindEntry("data/data.pak") || !package.FindEntry("data/data_menu.pak")) {
    problems.push_back(fmt::format("{}: the package is missing game files", FileLabel(path)));
    return std::nullopt;
  }
  if (!BuildXexSha1().empty()) {
    std::vector<uint8_t> data;
    if (!package.ReadFile(*xex, data, &error)) {
      problems.push_back(fmt::format("{}: {}", FileLabel(path), error));
      return std::nullopt;
    }
    sha1::SHA1 hash;
    hash.processBytes(data.data(), data.size());
    if (HexDigest(hash) != BuildXexSha1()) {
      problems.push_back(fmt::format(
          "{}: a different version of Trials HD than this build was made for", FileLabel(path)));
      return std::nullopt;
    }
  }
  Source source;
  source.kind = SourceKind::kBaseGame;
  source.path = path;
  source.display_name = package.display_name().empty() ? "Trials HD" : package.display_name();
  source.license_mask = package.license_mask();
  source.bytes = package.total_file_bytes();
  return source;
}

std::optional<Source> CheckDlcPackage(const fs::path& path, std::vector<std::string>& problems) {
  stfs::StfsPackage package;
  std::string error;
  if (!package.Open(path, &error)) {
    problems.push_back(fmt::format("{}: {}", FileLabel(path), error));
    return std::nullopt;
  }
  Source source;
  source.kind = SourceKind::kDlcPackage;
  source.path = path;
  source.package_id = DerivePackageId(path);
  source.display_name = package.display_name().empty()
                            ? fmt::format("Trials HD DLC ({})", source.package_id.substr(0, 8))
                            : package.display_name();
  source.license_mask = package.license_mask() ? package.license_mask() : 1;
  source.bytes = package.total_file_bytes();
  return source;
}

Source DlcFolderSource(const fs::path& dir) {
  Source source;
  source.kind = SourceKind::kDlcFolder;
  source.path = dir;
  source.package_id = DerivePackageId(dir);
  source.display_name = fmt::format("Trials HD DLC ({})", dir.filename().string());
  std::error_code ec;
  for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::end(it);
       it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    source.bytes += it->file_size(ec);
    // The pack's string table names it (pack1: Big Pack, pack3: Big Thrills).
    const auto name = it->path().filename().string();
    if (name.ends_with(".xus")) {
      if (name.find("pack1") != std::string::npos) source.display_name = "Trials HD - Big Pack";
      if (name.find("pack3") != std::string::npos) source.display_name = "Trials HD - Big Thrills";
    }
  }
  return source;
}

void AddCandidate(const fs::path& path, ScanResult& result) {
  auto probe = ProbePackage(path);
  if (!probe) return;
  std::optional<Source> source;
  if (probe->title_id == kTitleId && probe->content_type == kContentTypeGame) {
    source = CheckBaseGame(path, result.problems);
  } else if (IsDlcProbe(*probe)) {
    source = CheckDlcPackage(path, result.problems);
  } else {
    return;  // Saves, profiles, another game's packages: not ours to install.
  }
  if (source) result.sources.push_back(std::move(*source));
}

bool WriteBytes(const fs::path& path, const void* data, size_t size, std::string* error) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(static_cast<const char*>(data), std::streamsize(size));
  out.close();
  return out || Fail(error, "Failed to write " + path.string());
}

fs::path ContentDir(const fs::path& root) {
  return root / "content" / kXuidDir / fmt::format("{:08X}", kTitleId);
}

fs::path DlcHeaderDir(const fs::path& root) {
  return ContentDir(root) / "Headers" / fmt::format("{:08X}", kContentTypeDlc);
}

// One DLC: files under content/<xuid>/<title>/00000002/<id>/, its header under
// content/<xuid>/<title>/Headers/00000002/<id>.header (the console's layout).
bool InstallDlc(const Source& source, const fs::path& root, uint64_t offset, Progress& progress,
                std::stop_token stop, std::string* error) {
  const auto type_dir = fmt::format("{:08X}", kContentTypeDlc);
  const auto package_dir = ContentDir(root) / type_dir / source.package_id;
  const auto header_dir = DlcHeaderDir(root);
  std::error_code ec;
  fs::create_directories(package_dir, ec);
  if (!ec) fs::create_directories(header_dir, ec);
  if (ec) return Fail(error, "Failed to create " + package_dir.string() + ": " + ec.message());

  if (source.kind == SourceKind::kDlcPackage) {
    stfs::StfsPackage package;
    if (!package.Open(source.path, error)) return false;
    bool ok = package.ExtractAll(
        package_dir, /*write_license=*/false,
        [&](const stfs::StfsEntry& entry, uint64_t done, uint64_t) {
          progress.SetStage(fmt::format("{}: {}", source.display_name, entry.path));
          progress.bytes_done = offset + done;
          return !stop.stop_requested();
        },
        error);
    if (!ok) return false;
  } else {
    uint64_t done = 0;
    for (auto it = fs::recursive_directory_iterator(source.path, ec); !ec && it != fs::end(it);
         it.increment(ec)) {
      if (stop.stop_requested()) return Fail(error, "Cancelled");
      const auto relative = fs::relative(it->path(), source.path, ec);
      if (ec) break;
      const auto destination = package_dir / relative;
      if (it->is_directory(ec)) {
        fs::create_directories(destination, ec);
      } else if (it->is_regular_file(ec) && !it->is_symlink(ec)) {
        progress.SetStage(fmt::format("{}: {}", source.display_name, relative.string()));
        fs::create_directories(destination.parent_path(), ec);
        if (!ec) fs::copy_file(it->path(), destination, fs::copy_options::overwrite_existing, ec);
        done += it->file_size(ec);
        progress.bytes_done = offset + done;
      }
      if (ec) break;
    }
    if (ec) return Fail(error, "Failed to copy " + source.path.string() + ": " + ec.message());
  }

  auto header = MakeDlcHeader(source.display_name, source.package_id, source.license_mask);
  return WriteBytes(header_dir / (source.package_id + ".header"), header.data(), header.size(),
                    error);
}

}  // namespace

fs::path DefaultInstallDir() { return rex::filesystem::GetExecutableFolder() / "assets"; }

std::string_view BuildXexSha1() { return TRIALSHD_BUILD_XEX_SHA1; }

ScanResult Scan(const fs::path& path, std::stop_token stop) {
  ScanResult result;
  std::error_code ec;
  if (!fs::exists(path, ec)) {
    result.problems.push_back(fmt::format("{} does not exist", path.string()));
    return result;
  }
  if (fs::is_regular_file(path, ec)) {
    AddCandidate(path, result);
    if (result.sources.empty() && result.problems.empty()) {
      result.problems.push_back(
          fmt::format("{}: not a Trials HD game or DLC package", FileLabel(path)));
    }
  } else if (fs::is_regular_file(path / "pack.pak", ec)) {
    result.sources.push_back(DlcFolderSource(path));
  } else {
    // Packages sit a few levels down in a console content folder
    // (<xuid>/5841095A/000D0000/<file>); deeper is not worth walking.
    constexpr int kMaxDepth = 6;
    auto it = fs::recursive_directory_iterator(path, fs::directory_options::skip_permission_denied,
                                               ec);
    for (; !ec && it != fs::end(it); it.increment(ec)) {
      if (stop.stop_requested()) break;
      if (it->is_directory(ec)) {
        if (it.depth() >= kMaxDepth || it->is_symlink(ec)) {
          it.disable_recursion_pending();
        } else if (fs::is_regular_file(it->path() / "pack.pak", ec)) {
          result.sources.push_back(DlcFolderSource(it->path()));
          it.disable_recursion_pending();
        }
        continue;
      }
      AddCandidate(it->path(), result);
    }
  }
  // One base game (the first usable one) first, then each DLC once.
  std::vector<Source> sources;
  std::unordered_set<std::string> dlc_ids;
  for (auto& source : result.sources) {
    if (source.kind == SourceKind::kBaseGame) {
      if (sources.empty() || sources.front().kind != SourceKind::kBaseGame) {
        sources.insert(sources.begin(), std::move(source));
      }
    } else if (dlc_ids.insert(source.package_id).second) {
      sources.push_back(std::move(source));
    }
  }
  result.sources = std::move(sources);
  return result;
}

bool Install(const Plan& plan, const fs::path& assets_dir, Progress& progress,
             std::stop_token stop, std::string* error) {
  if (!plan.base_game && plan.dlc.empty()) return Fail(error, "Nothing to install");
  uint64_t total = plan.base_game ? plan.base_game->bytes : 0;
  for (const auto& dlc : plan.dlc) total += dlc.bytes;
  progress.bytes_total = total;
  progress.bytes_done = 0;

  std::error_code ec;
  const auto parent = assets_dir.parent_path();
  fs::create_directories(parent, ec);
  if (ec) return Fail(error, "Cannot create " + parent.string() + ": " + ec.message());
  // Some headroom for the file system; the exact figure doesn't matter.
  constexpr uint64_t kSlack = 64ull << 20;
  if (auto space = fs::space(parent, ec); !ec && space.available < total + kSlack) {
    return Fail(error, fmt::format("Not enough free space in {}: {} MB needed, {} MB free",
                                   parent.string(), (total + kSlack) >> 20,
                                   space.available >> 20));
  }

  const bool fresh = !fs::exists(assets_dir, ec);
  auto work = assets_dir;
  if (fresh) {
    work = parent / (assets_dir.filename().string() + ".installing");
    fs::remove_all(work, ec);  // Left by an earlier, interrupted install.
    fs::create_directories(work, ec);
    if (ec) return Fail(error, "Cannot create " + work.string() + ": " + ec.message());
  }
  auto abandon = [&](bool ok) {
    if (!ok && fresh) {
      std::error_code ignored;
      fs::remove_all(work, ignored);
    }
    return ok;
  };

  uint64_t offset = 0;
  if (plan.base_game) {
    const auto& game = *plan.base_game;
    stfs::StfsPackage package;
    if (!package.Open(game.path, error)) return abandon(false);
    bool ok = package.ExtractAll(
        work, /*write_license=*/true,
        [&](const stfs::StfsEntry& entry, uint64_t done, uint64_t) {
          progress.SetStage(fmt::format("{}: {}", game.display_name, entry.path));
          progress.bytes_done = done;
          return !stop.stop_requested();
        },
        error);
    if (!ok) return abandon(false);
    offset += game.bytes;
  }
  for (const auto& dlc : plan.dlc) {
    if (!InstallDlc(dlc, work, offset, progress, stop, error)) return abandon(false);
    offset += dlc.bytes;
  }
  if (fresh) {
    fs::rename(work, assets_dir, ec);
    if (ec) {
      return abandon(Fail(error, "Cannot move the installed game to " + assets_dir.string() +
                                     ": " + ec.message()));
    }
  }
  progress.bytes_done = total;
  progress.SetStage("Done");
  return true;
}

bool IsDlcInstalled(const fs::path& assets_dir, std::string_view package_id) {
  std::error_code ec;
  return fs::is_regular_file(DlcHeaderDir(assets_dir) / (std::string(package_id) + ".header"),
                             ec);
}

std::vector<std::string> InstalledDlcNames(const fs::path& assets_dir) {
  std::vector<std::string> names;
  std::error_code ec;
  for (auto it = fs::directory_iterator(DlcHeaderDir(assets_dir), ec); !ec && it != fs::end(it);
       it.increment(ec)) {
    if (it->path().extension() != ".header") continue;
    // Display name: UTF-16 big endian at 0x08 (MakeDlcHeader).
    std::ifstream file(it->path(), std::ios::binary);
    uint8_t raw[256] = {};
    file.seekg(0x08);
    file.read(reinterpret_cast<char*>(raw), sizeof(raw));
    std::string name;
    for (size_t i = 0; i + 1 < sizeof(raw); i += 2) {
      const uint16_t c = uint16_t(raw[i] << 8 | raw[i + 1]);
      if (c == 0) break;
      if (c < 0x80) {
        name += char(c);
      } else if (c < 0x800) {
        name += char(0xC0 | (c >> 6));
        name += char(0x80 | (c & 0x3F));
      } else {
        name += char(0xE0 | (c >> 12));
        name += char(0x80 | ((c >> 6) & 0x3F));
        name += char(0x80 | (c & 0x3F));
      }
    }
    names.push_back(name.empty() ? it->path().stem().string() : name);
  }
  std::ranges::sort(names);
  return names;
}

std::array<uint8_t, 332> MakeDlcHeader(std::string_view display_name, std::string_view package_id,
                                       uint32_t license_mask) {
  std::array<uint8_t, 332> header{};
  auto be32 = [&](size_t offset, uint32_t value) {
    for (int i = 0; i < 4; ++i) header[offset + i] = uint8_t(value >> (24 - 8 * i));
  };
  be32(0x000, 1);  // device: hard drive
  be32(0x004, kContentTypeDlc);
  // Display name: UTF-16 big endian, up to 127 characters plus the terminator.
  size_t out = 0x008;
  for (size_t i = 0; i < display_name.size() && out < 0x008 + 254;) {
    auto byte = [&](size_t k) { return uint8_t(display_name[k]); };
    uint32_t c = byte(i);
    size_t length = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : 4;
    if (i + length > display_name.size()) break;
    if (length == 2) c = ((c & 0x1F) << 6) | (byte(i + 1) & 0x3F);
    if (length == 3) c = ((c & 0x0F) << 12) | ((byte(i + 1) & 0x3F) << 6) | (byte(i + 2) & 0x3F);
    if (length == 4) c = '?';  // Outside the BMP; never in these names.
    header[out] = uint8_t(c >> 8);
    header[out + 1] = uint8_t(c);
    out += 2;
    i += length;
  }
  for (size_t i = 0; i < package_id.size() && i < 42; ++i) {
    header[0x108 + i] = uint8_t(package_id[i]);
  }
  be32(0x138, 0);  // XUID, high and low words
  be32(0x13C, 0);
  be32(0x140, kTitleId);
  // License mask: little endian, as the SDK's content manager reads it.
  for (int i = 0; i < 4; ++i) header[0x148 + i] = uint8_t(license_mask >> (8 * i));
  return header;
}

std::string DerivePackageId(const fs::path& path) {
  const auto name = path.filename().string();
  if (name.size() == 42 && IsHex(name)) return ToUpper(name);
  if (name.size() == 40 && IsHex(name)) return ToUpper(name + "58");
  // Stable for the same package: a hash of its first 64 KiB (the header),
  // or of a folder's name.
  sha1::SHA1 hash;
  std::error_code ec;
  if (fs::is_regular_file(path, ec)) {
    std::vector<char> head(0x10000);
    std::ifstream file(path, std::ios::binary);
    file.read(head.data(), std::streamsize(head.size()));
    hash.processBytes(head.data(), size_t(file.gcount()));
  } else {
    hash.processBytes(name.data(), name.size());
  }
  return ToUpper(HexDigest(hash) + "58").substr(0, 42);
}

}  // namespace trials_hd::installer
