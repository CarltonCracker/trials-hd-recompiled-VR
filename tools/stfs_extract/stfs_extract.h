#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace stfs {

struct StfsLicense {
  uint64_t id = 0;
  uint32_t bits = 0;
  uint32_t flags = 0;
};

struct StfsEntry {
  std::string name;
  std::string path;
  bool is_dir = false;
  uint32_t start_block = 0;
  uint16_t parent = 0xFFFF;
  uint32_t size = 0;
};

class StfsPackage {
 public:
  StfsPackage();
  ~StfsPackage();

  bool Open(const std::filesystem::path& path, std::string* error = nullptr);
  void Close();

  uint32_t header_size() const { return header_size_; }
  uint32_t content_type() const { return content_type_; }
  uint32_t volume_type() const { return volume_type_; }
  uint32_t title_id() const { return title_id_; }
  const std::string& display_name() const { return display_name_; }
  bool read_only() const { return read_only_; }
  uint32_t total_blocks() const { return total_blocks_; }
  const std::vector<StfsLicense>& licenses() const { return licenses_; }
  uint32_t license_mask() const;

  const std::vector<StfsEntry>& entries() const { return entries_; }

  bool ReadFile(const StfsEntry& entry, std::vector<uint8_t>& out_data, std::string* error = nullptr);
  bool ExtractAll(const std::filesystem::path& out_dir,
                  bool write_license = true,
                  const std::function<void(const StfsEntry&, size_t, size_t)>& progress = nullptr,
                  std::string* error = nullptr);

 private:
  uint64_t BlockToOffset(uint32_t idx) const;
  uint32_t HashBlockNumber(uint32_t idx, int level) const;
  uint64_t HashOffset(uint32_t idx, int level) const;
  const uint8_t* GetTable(uint64_t off);
  uint32_t NextBlock(uint32_t idx);

  std::filesystem::path path_;
  struct Impl;
  Impl* impl_ = nullptr;

  uint32_t header_size_ = 0;
  uint32_t content_type_ = 0;
  uint32_t volume_type_ = 0;
  uint8_t flags_ = 0;
  bool read_only_ = false;
  bool root_active_ = false;
  uint16_t ft_block_count_ = 0;
  uint32_t ft_block_ = 0;
  uint32_t total_blocks_ = 0;
  uint32_t title_id_ = 0;
  std::string display_name_;
  std::vector<StfsLicense> licenses_;
  uint32_t bpht_ = 1;
  uint32_t step0_ = 0;
  uint32_t step1_ = 0;
  uint64_t base_ = 0;
  std::vector<StfsEntry> entries_;
  std::unordered_map<uint64_t, std::vector<uint8_t>> cache_;
};

}  // namespace stfs
