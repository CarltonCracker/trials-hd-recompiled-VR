// xenos2wgsl - translate every shader in a Xenos shader cache (.xsh, written
// by the SDK's pipeline cache) to WGSL with the rexgpu-webgpu translator.
// Output stays on the player's machine (generated from their game files);
// never commit it.
//
// Usage: xenos2wgsl <cache.xsh> <output_dir>
// Writes <hash>.vs.wgsl / <hash>.ps.wgsl plus the ucode disassembly, and
// prints one line per shader with any translation errors. Validate with
// tools/xenos2wgsl/validate.sh (Tint).

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

#include <xxhash.h>

#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/string/buffer.h>

#include "wgsl/wgsl_shader_translator.h"

using rex::graphics::Shader;
using rex::graphics::webgpu::WgslShaderTranslator;

namespace {

#pragma pack(push, 1)
struct StoredHeader {
  uint64_t ucode_data_hash;
  uint32_t ucode_dword_count : 31;
  uint32_t type : 1;
};
#pragma pack(pop)

constexpr uint32_t kMagic = 0x48534558;  // 'XESH'
constexpr uint32_t kVersion = 0x20201219;

uint32_t ByteSwap(uint32_t v) { return __builtin_bswap32(v); }

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: %s <cache.xsh> <output_dir>\n", argv[0]);
    return 2;
  }
  std::FILE* file = std::fopen(argv[1], "rb");
  if (!file) {
    std::fprintf(stderr, "can't open %s\n", argv[1]);
    return 1;
  }
  uint32_t file_header[2];
  if (std::fread(file_header, sizeof(file_header), 1, file) != 1 || file_header[0] != kMagic ||
      ByteSwap(file_header[1]) != kVersion) {
    std::fprintf(stderr, "%s is not a shader storage file of version %08X\n", argv[1], kVersion);
    return 1;
  }
  std::filesystem::path output_dir = argv[2];
  std::filesystem::create_directories(output_dir);

  WgslShaderTranslator translator;
  rex::string::StringBuffer disassembly_buffer;
  uint32_t shader_count = 0, failed_count = 0;
  StoredHeader header;
  std::vector<uint32_t> ucode;
  while (std::fread(&header, sizeof(header), 1, file) == 1) {
    ucode.resize(header.ucode_dword_count);
    if (header.ucode_dword_count &&
        std::fread(ucode.data(), header.ucode_dword_count * 4, 1, file) != 1) {
      break;
    }
    if (XXH3_64bits(ucode.data(), ucode.size() * 4) != header.ucode_data_hash) {
      std::fprintf(stderr, "hash mismatch, stopping\n");
      break;
    }
    auto type = header.type ? rex::graphics::xenos::ShaderType::kPixel
                            : rex::graphics::xenos::ShaderType::kVertex;
    Shader shader(type, header.ucode_data_hash, ucode.data(), ucode.size());
    shader.AnalyzeUcode(disassembly_buffer);

    WgslShaderTranslator::Modification modification;
    modification.dynamic_addressable_register_count =
        shader.uses_register_dynamic_addressing() ? 64 : 0;
    if (type == rex::graphics::xenos::ShaderType::kVertex) {
      modification.interpolator_mask = shader.writes_interpolators();
    } else {
      // Every interpolator the pixel shader could read.
      uint32_t count = std::min(shader.register_static_address_bound(), uint32_t(16));
      modification.interpolator_mask = (uint32_t(1) << count) - 1;
      modification.color_targets = 0b1111;
    }
    Shader::Translation* translation = shader.GetOrCreateTranslation(modification.value);
    bool ok = translator.TranslateAnalyzedShader(*translation);

    const char* suffix = type == rex::graphics::xenos::ShaderType::kVertex ? "vs" : "ps";
    char name[64];
    std::snprintf(name, sizeof(name), "%016llX.%s", (unsigned long long)header.ucode_data_hash,
                  suffix);
    std::ofstream(output_dir / (std::string(name) + ".wgsl"))
        << translation->GetTranslatedBinaryString();
    std::ofstream(output_dir / (std::string(name) + ".ucode.txt")) << shader.ucode_disassembly();

    if (type == rex::graphics::xenos::ShaderType::kVertex) {
      // Also the rectangle-list expansion variant (L-04f).
      WgslShaderTranslator::Modification rectangle = modification;
      rectangle.host_vertex_shader_type =
          uint32_t(Shader::HostVertexShaderType::kRectangleListAsTriangleStrip);
      Shader::Translation* rectangle_translation =
          shader.GetOrCreateTranslation(rectangle.value);
      if (!translator.TranslateAnalyzedShader(*rectangle_translation)) {
        ok = false;
      }
      std::ofstream(output_dir / (std::string(name) + ".rect.wgsl"))
          << rectangle_translation->GetTranslatedBinaryString();
    }
    ++shader_count;
    std::printf("%s %s %zu dwords", name, ok ? "ok" : "FAILED", ucode.size());
    for (const Shader::Error& error : translation->errors()) {
      std::printf(" [%s%s]", error.is_fatal ? "fatal: " : "", error.message.c_str());
    }
    std::printf("\n");
    failed_count += ok ? 0 : 1;
  }
  std::fclose(file);
  std::printf("%u shaders, %u failed\n", shader_count, failed_count);
  return failed_count ? 1 : 0;
}
