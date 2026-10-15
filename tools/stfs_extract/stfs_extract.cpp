#include "stfs_extract.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>

namespace stfs {

namespace {

constexpr uint32_t kBlockSize = 0x1000;
constexpr uint32_t kEndOfChain = 0xFFFFFF;

inline uint16_t read_be16(const uint8_t* p) {
  return (static_cast<uint16_t>(p[0]) << 8) | static_cast<uint16_t>(p[1]);
}

inline uint32_t read_be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) |
         (static_cast<uint32_t>(p[1]) << 16) |
         (static_cast<uint32_t>(p[2]) << 8) |
         static_cast<uint32_t>(p[3]);
}

inline uint64_t read_be64(const uint8_t* p) {
  return (static_cast<uint64_t>(read_be32(p)) << 32) |
         static_cast<uint64_t>(read_be32(p + 4));
}

inline uint16_t read_le16(const uint8_t* p) {
  return static_cast<uint16_t>(p[0]) |
         (static_cast<uint16_t>(p[1]) << 8);
}

inline uint32_t read_u24le(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) |
         (static_cast<uint32_t>(p[1]) << 8) |
         (static_cast<uint32_t>(p[2]) << 16);
}

}  // namespace

struct StfsPackage::Impl {
  std::ifstream file;
};

StfsPackage::StfsPackage() : impl_(new Impl()) {}

StfsPackage::~StfsPackage() {
  Close();
  delete impl_;
}

void StfsPackage::Close() {
  if (impl_ && impl_->file.is_open()) {
    impl_->file.close();
  }
  cache_.clear();
  entries_.clear();
  licenses_.clear();
}

uint32_t StfsPackage::license_mask() const {
  uint32_t mask = 0;
  for (const auto& lic : licenses_) {
    mask |= lic.bits;
  }
  return mask;
}

bool StfsPackage::Open(const std::filesystem::path& path, std::string* error) {
  Close();
  path_ = path;
  impl_->file.open(path, std::ios::binary);
  if (!impl_->file.is_open()) {
    if (error) *error = "Failed to open file: " + path.string();
    return false;
  }

  std::vector<uint8_t> hdr(0x971A);
  if (!impl_->file.read(reinterpret_cast<char*>(hdr.data()), hdr.size())) {
    if (error) *error = "Failed to read header (too short)";
    Close();
    return false;
  }

  std::string_view magic(reinterpret_cast<const char*>(hdr.data()), 4);
  if (magic != "LIVE" && magic != "PIRS" && magic != "CON ") {
    if (error) *error = "Not an STFS package (magic: " + std::string(magic) + ")";
    Close();
    return false;
  }

  header_size_ = read_be32(hdr.data() + 0x340);
  content_type_ = read_be32(hdr.data() + 0x344);
  volume_type_ = read_be32(hdr.data() + 0x3A9);
  if (volume_type_ != 0) {
    if (error) *error = "SVOD packages are not supported";
    Close();
    return false;
  }

  const uint8_t* vd = hdr.data() + 0x379;
  flags_ = vd[2];
  read_only_ = (flags_ & 1) != 0;
  root_active_ = (flags_ & 2) != 0;
  ft_block_count_ = read_le16(vd + 3);
  ft_block_ = read_u24le(vd + 5);
  total_blocks_ = read_be32(vd + 0x1C);
  title_id_ = read_be32(hdr.data() + 0x360);

  // Decode utf-16-be display name
  display_name_.clear();
  for (size_t i = 0; i < 0x80; i += 2) {
    uint16_t c = (static_cast<uint16_t>(hdr[0x411 + i]) << 8) |
                 static_cast<uint16_t>(hdr[0x411 + i + 1]);
    if (c == 0) break;
    if (c < 0x80) {
      display_name_ += static_cast<char>(c);
    } else {
      display_name_ += static_cast<char>(0xC0 | ((c >> 6) & 0x1F));
      display_name_ += static_cast<char>(0x80 | (c & 0x3F));
    }
  }

  // 16 license entries at 0x22C
  licenses_.clear();
  for (int i = 0; i < 16; ++i) {
    const uint8_t* lp = hdr.data() + 0x22C + 16 * i;
    uint64_t lid = read_be64(lp);
    uint32_t bits = read_be32(lp + 8);
    uint32_t lflags = read_be32(lp + 12);
    if (lid != 0) {
      licenses_.push_back({lid, bits, lflags});
    }
  }

  bpht_ = read_only_ ? 1 : 2;
  step0_ = 0xAA + bpht_;
  step1_ = 0x70E4 + (0xAA + 1) * bpht_;
  base_ = (header_size_ + kBlockSize - 1) & ~static_cast<uint64_t>(kBlockSize - 1);

  // Read file listing
  entries_.clear();
  uint32_t blk = ft_block_;
  std::vector<uint8_t> block_buf(kBlockSize);

  for (uint32_t i = 0; i < ft_block_count_; ++i) {
    uint64_t off = BlockToOffset(blk);
    impl_->file.seekg(off, std::ios::beg);
    if (!impl_->file.read(reinterpret_cast<char*>(block_buf.data()), kBlockSize)) {
      break;
    }

    for (int m = 0; m < 0x40; ++m) {
      const uint8_t* e = block_buf.data() + m * 0x40;
      if (e[0] == 0) {
        break;
      }
      uint32_t nl = e[0x28] & 0x3F;
      std::string name(reinterpret_cast<const char*>(e), nl);
      bool is_dir = (e[0x28] & 0x80) != 0;
      uint32_t start = read_u24le(e + 0x2F);
      uint16_t parent = read_be16(e + 0x32);
      uint32_t size = read_be32(e + 0x34);

      StfsEntry entry;
      entry.name = std::move(name);
      entry.is_dir = is_dir;
      entry.start_block = start;
      entry.parent = parent;
      entry.size = size;
      entries_.push_back(std::move(entry));
    }

    blk = NextBlock(blk);
    if (blk == kEndOfChain) {
      break;
    }
  }

  // Build full paths
  for (auto& entry : entries_) {
    std::vector<std::string> parts;
    parts.push_back(entry.name);
    uint16_t p = entry.parent;
    while (p != 0xFFFF && p < entries_.size()) {
      parts.insert(parts.begin(), entries_[p].name);
      p = entries_[p].parent;
    }
    std::string full_path;
    for (size_t k = 0; k < parts.size(); ++k) {
      if (k > 0) full_path += '/';
      full_path += parts[k];
    }
    entry.path = std::move(full_path);
  }

  return true;
}

uint64_t StfsPackage::BlockToOffset(uint32_t idx) const {
  uint64_t base_lvl = 0xAA;
  uint64_t block = idx;
  for (int i = 0; i < 3; ++i) {
    block += ((idx + base_lvl) / base_lvl) * bpht_;
    if (idx < base_lvl) break;
    base_lvl *= 0xAA;
  }
  return base_ + (block << 12);
}

uint32_t StfsPackage::HashBlockNumber(uint32_t idx, int level) const {
  if (level == 0) {
    if (idx < 0xAA) return 0;
    uint32_t block = (idx / 0xAA) * step0_;
    block += ((idx / 0x70E4) + 1) * bpht_;
    return (idx < 0x70E4) ? block : (block + bpht_);
  }
  if (level == 1) {
    if (idx < 0x70E4) return step0_;
    return (idx / 0x70E4) * step1_ + bpht_;
  }
  return step1_;
}

uint64_t StfsPackage::HashOffset(uint32_t idx, int level) const {
  return base_ + (static_cast<uint64_t>(HashBlockNumber(idx, level)) << 12);
}

const uint8_t* StfsPackage::GetTable(uint64_t off) {
  auto it = cache_.find(off);
  if (it != cache_.end()) {
    return it->second.data();
  }
  std::vector<uint8_t> buf(kBlockSize);
  impl_->file.seekg(off, std::ios::beg);
  if (!impl_->file.read(reinterpret_cast<char*>(buf.data()), kBlockSize)) {
    return nullptr;
  }
  auto res = cache_.emplace(off, std::move(buf));
  return res.first->second.data();
}

uint32_t StfsPackage::NextBlock(uint32_t idx) {
  uint32_t sec = root_active_ ? kBlockSize : 0;
  if (read_only_) {
    sec = 0;
  } else {
    if (total_blocks_ > 0xAA) {
      if (total_blocks_ > 0x70E4) {
        const uint8_t* t2 = GetTable(HashOffset(idx, 2) + sec);
        if (!t2) return kEndOfChain;
        uint32_t record = (idx / 0x70E4) % 0xAA;
        uint8_t st = t2[record * 0x18 + 0x14];
        sec = (st & 0x40) ? kBlockSize : 0;
      }
      const uint8_t* t1 = GetTable(HashOffset(idx, 1) + sec);
      if (!t1) return kEndOfChain;
      uint32_t record = (idx / 0xAA) % 0xAA;
      uint8_t st = t1[record * 0x18 + 0x14];
      sec = (st & 0x40) ? kBlockSize : 0;
    }
  }
  const uint8_t* t0 = GetTable(HashOffset(idx, 0) + sec);
  if (!t0) return kEndOfChain;
  uint32_t record = idx % 0xAA;
  const uint8_t* e = t0 + record * 0x18;
  return (static_cast<uint32_t>(e[0x15]) << 16) |
         (static_cast<uint32_t>(e[0x16]) << 8) |
         static_cast<uint32_t>(e[0x17]);
}

bool StfsPackage::ReadFile(const StfsEntry& entry, std::vector<uint8_t>& out_data, std::string* error) {
  out_data.resize(entry.size);
  uint32_t remaining = entry.size;
  uint32_t blk = entry.start_block;
  uint32_t offset = 0;

  while (remaining > 0 && blk != kEndOfChain) {
    uint32_t n = std::min<uint32_t>(kBlockSize, remaining);
    uint64_t file_offset = BlockToOffset(blk);
    impl_->file.seekg(file_offset, std::ios::beg);
    if (!impl_->file.read(reinterpret_cast<char*>(out_data.data() + offset), n)) {
      if (error) *error = "Failed to read block " + std::to_string(blk);
      return false;
    }
    remaining -= n;
    offset += n;
    blk = NextBlock(blk);
  }

  if (remaining > 0) {
    if (error) {
      *error = entry.path + ": chain ended with " + std::to_string(remaining) + " bytes missing";
    }
    return false;
  }
  return true;
}

bool StfsPackage::ExtractAll(const std::filesystem::path& out_dir,
                             bool write_license,
                             const std::function<void(const StfsEntry&, size_t, size_t)>& progress,
                             std::string* error) {
  std::error_code ec;
  std::filesystem::create_directories(out_dir, ec);

  std::vector<uint8_t> buffer;
  for (size_t i = 0; i < entries_.size(); ++i) {
    const auto& e = entries_[i];
    if (progress) progress(e, i, entries_.size());
    if (e.is_dir) {
      continue;
    }
    std::filesystem::path dst = out_dir / e.path;
    std::filesystem::create_directories(dst.parent_path(), ec);

    if (!ReadFile(e, buffer, error)) {
      return false;
    }

    std::ofstream out(dst, std::ios::binary);
    if (!out) {
      if (error) *error = "Failed to create " + dst.string();
      return false;
    }
    out.write(reinterpret_cast<const char*>(buffer.data()), buffer.size());
  }

  for (const auto& e : entries_) {
    if (e.is_dir) {
      std::filesystem::path dst = out_dir / e.path;
      std::filesystem::create_directories(dst, ec);
    }
  }

  if (write_license && content_type_ == 0x000D0000) {
    uint32_t mask = license_mask();
    std::filesystem::path lic_path = out_dir / "package_license.toml";
    std::ofstream lic(lic_path);
    if (!lic) {
      if (error) *error = "Failed to write package_license.toml";
      return false;
    }
    lic << "# Written by tools/stfs_extract.py from the package's license block.\n";
    lic << "license_mask = 0x" << std::uppercase << std::hex << mask << "\n";
  }

  return true;
}

}  // namespace stfs
