// rexgpu-webgpu - Unit tests for SamplerCache and key/descriptor mapping (S-24).

#include <cassert>
#include <cstdio>
#include <cstdlib>

#include "gpu_test.h"
#include "sampler_cache.h"

#define TEST_CHECK(cond)                                                       \
  do {                                                                         \
    if (!(cond)) {                                                             \
      std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);     \
      std::exit(1);                                                            \
    }                                                                          \
  } while (false)

using namespace rex::graphics::webgpu;
using namespace rex::graphics::xenos;

void TestAddressModes() {
  std::printf("Testing Address Modes...\n");

  TEST_CHECK(ToAddressMode(ClampMode::kRepeat) == wgpu::AddressMode::Repeat);
  TEST_CHECK(ToAddressMode(ClampMode::kMirroredRepeat) == wgpu::AddressMode::MirrorRepeat);
  TEST_CHECK(ToAddressMode(ClampMode::kMirrorClampToEdge) == wgpu::AddressMode::MirrorRepeat);
  TEST_CHECK(ToAddressMode(ClampMode::kMirrorClampToHalfway) == wgpu::AddressMode::MirrorRepeat);
  TEST_CHECK(ToAddressMode(ClampMode::kMirrorClampToBorder) == wgpu::AddressMode::MirrorRepeat);
  TEST_CHECK(ToAddressMode(ClampMode::kClampToEdge) == wgpu::AddressMode::ClampToEdge);
  TEST_CHECK(ToAddressMode(ClampMode::kClampToHalfway) == wgpu::AddressMode::ClampToEdge);
  TEST_CHECK(ToAddressMode(ClampMode::kClampToBorder) == wgpu::AddressMode::ClampToEdge);

  std::printf("PASS: Address Modes\n");
}

void TestFilterModes() {
  std::printf("Testing Filter Modes...\n");

  TEST_CHECK(ToFilterMode(TextureFilter::kPoint) == wgpu::FilterMode::Nearest);
  TEST_CHECK(ToFilterMode(TextureFilter::kLinear) == wgpu::FilterMode::Linear);
  TEST_CHECK(ToFilterMode(TextureFilter::kBaseMap) == wgpu::FilterMode::Nearest);

  TEST_CHECK(ToMipmapFilterMode(TextureFilter::kPoint) == wgpu::MipmapFilterMode::Nearest);
  TEST_CHECK(ToMipmapFilterMode(TextureFilter::kLinear) == wgpu::MipmapFilterMode::Linear);
  TEST_CHECK(ToMipmapFilterMode(TextureFilter::kBaseMap) == wgpu::MipmapFilterMode::Nearest);

  std::printf("PASS: Filter Modes\n");
}

void TestKeyAndDescriptorGeneration() {
  std::printf("Testing Key and Descriptor Generation...\n");

  xe_gpu_texture_fetch_t fetch{};
  fetch.clamp_x = ClampMode::kRepeat;
  fetch.clamp_y = ClampMode::kClampToEdge;
  fetch.clamp_z = ClampMode::kMirroredRepeat;
  fetch.mag_filter = TextureFilter::kLinear;
  fetch.min_filter = TextureFilter::kLinear;
  fetch.mip_filter = TextureFilter::kLinear;
  fetch.aniso_filter = AnisoFilter::kMax_4_1;  // 4:1
  fetch.mip_min_level = 1;
  fetch.mip_max_level = 8;
  fetch.lod_bias = 32;  // +1.0 in 5-fractional-bit fixed point
  fetch.border_color = BorderColor::k_ABGR_Black;

  rex::graphics::ParsedTextureFetchInstruction::Attributes overrides{};
  overrides.mag_filter = TextureFilter::kUseFetchConst;
  overrides.min_filter = TextureFilter::kUseFetchConst;
  overrides.mip_filter = TextureFilter::kUseFetchConst;
  overrides.aniso_filter = AnisoFilter::kUseFetchConst;

  // 1. Fragment stage: aniso allowed with linear filters
  {
    SamplerKey key = MakeSamplerKey(fetch, overrides, /*vertex_shader=*/false);
    TEST_CHECK(key.clamp_u == uint32_t(ClampMode::kRepeat));
    TEST_CHECK(key.clamp_v == uint32_t(ClampMode::kClampToEdge));
    TEST_CHECK(key.clamp_w == uint32_t(ClampMode::kMirroredRepeat));
    TEST_CHECK(key.mag_filter == uint32_t(TextureFilter::kLinear));
    TEST_CHECK(key.min_filter == uint32_t(TextureFilter::kLinear));
    TEST_CHECK(key.mip_filter == uint32_t(TextureFilter::kLinear));
    TEST_CHECK(key.max_anisotropy == 4);
    TEST_CHECK(key.mip_min_level == 1);
    TEST_CHECK(key.mip_max_level == 8);
    TEST_CHECK(key.lod_bias == 32);

    wgpu::SamplerDescriptor desc = MakeSamplerDescriptor(key);
    TEST_CHECK(desc.addressModeU == wgpu::AddressMode::Repeat);
    TEST_CHECK(desc.addressModeV == wgpu::AddressMode::ClampToEdge);
    TEST_CHECK(desc.addressModeW == wgpu::AddressMode::MirrorRepeat);
    TEST_CHECK(desc.magFilter == wgpu::FilterMode::Linear);
    TEST_CHECK(desc.minFilter == wgpu::FilterMode::Linear);
    TEST_CHECK(desc.mipmapFilter == wgpu::MipmapFilterMode::Linear);
    TEST_CHECK(desc.lodMinClamp == 1.0f);
    TEST_CHECK(desc.lodMaxClamp == 8.0f);
    TEST_CHECK(desc.maxAnisotropy == 4);
  }

  // 2. Vertex stage: anisotropy forced to 1
  {
    SamplerKey key_vs = MakeSamplerKey(fetch, overrides, /*vertex_shader=*/true);
    TEST_CHECK(key_vs.max_anisotropy == 1);

    wgpu::SamplerDescriptor desc_vs = MakeSamplerDescriptor(key_vs);
    TEST_CHECK(desc_vs.maxAnisotropy == 1);
  }

  // 3. Instruction overrides: override filters and aniso
  {
    rex::graphics::ParsedTextureFetchInstruction::Attributes instr_overrides = overrides;
    instr_overrides.mag_filter = TextureFilter::kPoint;
    instr_overrides.aniso_filter = AnisoFilter::kMax_2_1;

    SamplerKey key_override = MakeSamplerKey(fetch, instr_overrides, /*vertex_shader=*/false);
    TEST_CHECK(key_override.mag_filter == uint32_t(TextureFilter::kPoint));
    TEST_CHECK(key_override.max_anisotropy == 2);

    // In WebGPU: since magFilter is Nearest, maxAnisotropy must be clamped to 1 in descriptor!
    wgpu::SamplerDescriptor desc_override = MakeSamplerDescriptor(key_override);
    TEST_CHECK(desc_override.magFilter == wgpu::FilterMode::Nearest);
    TEST_CHECK(desc_override.maxAnisotropy == 1);
  }

  // 4. Base map: forces mip_max_level = mip_min_level
  {
    xe_gpu_texture_fetch_t fetch_base = fetch;
    fetch_base.mip_filter = TextureFilter::kBaseMap;
    fetch_base.mip_min_level = 2;
    fetch_base.mip_max_level = 7;

    SamplerKey key_base = MakeSamplerKey(fetch_base, overrides, /*vertex_shader=*/false);
    TEST_CHECK(key_base.mip_min_level == 2);
    TEST_CHECK(key_base.mip_max_level == 2);  // Clamped to min

    wgpu::SamplerDescriptor desc_base = MakeSamplerDescriptor(key_base);
    TEST_CHECK(desc_base.lodMinClamp == 2.0f);
    TEST_CHECK(desc_base.lodMaxClamp == 2.0f);
  }

  // 5. Hashing and equality
  {
    SamplerKey key1 = MakeSamplerKey(fetch, overrides, false);
    SamplerKey key2 = MakeSamplerKey(fetch, overrides, false);
    TEST_CHECK(key1 == key2);
    TEST_CHECK(SamplerKeyHash{}(key1) == SamplerKeyHash{}(key2));

    xe_gpu_texture_fetch_t fetch_diff = fetch;
    fetch_diff.clamp_x = ClampMode::kClampToEdge;
    SamplerKey key3 = MakeSamplerKey(fetch_diff, overrides, false);
    TEST_CHECK(key1 != key3);
    TEST_CHECK(key1.value != key3.value);
  }

  std::printf("PASS: Key and Descriptor Generation\n");
}

void TestLiveDawnSamplerCreation(unsigned adapter_index) {
  std::printf("Testing Live Dawn Sampler Creation on adapter %u...\n", adapter_index);

  GpuTest test(adapter_index);

  // 1. Basic linear repeat sampler
  {
    SamplerKey key{};
    key.clamp_u = uint32_t(ClampMode::kRepeat);
    key.clamp_v = uint32_t(ClampMode::kRepeat);
    key.clamp_w = uint32_t(ClampMode::kRepeat);
    key.mag_filter = uint32_t(TextureFilter::kLinear);
    key.min_filter = uint32_t(TextureFilter::kLinear);
    key.mip_filter = uint32_t(TextureFilter::kLinear);
    key.max_anisotropy = 1;
    key.mip_min_level = 0;
    key.mip_max_level = 10;

    wgpu::SamplerDescriptor desc = MakeSamplerDescriptor(key);
    wgpu::Sampler sampler = test.device.CreateSampler(&desc);
    TEST_CHECK(sampler != nullptr);
    TEST_CHECK(test.errors == 0);
  }

  // 2. Anisotropic sampler (2:1 and 4:1)
  {
    SamplerKey key{};
    key.clamp_u = uint32_t(ClampMode::kClampToEdge);
    key.clamp_v = uint32_t(ClampMode::kClampToEdge);
    key.clamp_w = uint32_t(ClampMode::kClampToEdge);
    key.mag_filter = uint32_t(TextureFilter::kLinear);
    key.min_filter = uint32_t(TextureFilter::kLinear);
    key.mip_filter = uint32_t(TextureFilter::kLinear);
    key.max_anisotropy = 4;
    key.mip_min_level = 0;
    key.mip_max_level = 8;

    wgpu::SamplerDescriptor desc = MakeSamplerDescriptor(key);
    wgpu::Sampler sampler = test.device.CreateSampler(&desc);
    TEST_CHECK(sampler != nullptr);
    TEST_CHECK(test.errors == 0);
  }

  // 3. Nearest clamp sampler
  {
    SamplerKey key{};
    key.clamp_u = uint32_t(ClampMode::kClampToEdge);
    key.clamp_v = uint32_t(ClampMode::kClampToEdge);
    key.clamp_w = uint32_t(ClampMode::kClampToEdge);
    key.mag_filter = uint32_t(TextureFilter::kPoint);
    key.min_filter = uint32_t(TextureFilter::kPoint);
    key.mip_filter = uint32_t(TextureFilter::kPoint);
    key.max_anisotropy = 1;

    wgpu::SamplerDescriptor desc = MakeSamplerDescriptor(key);
    wgpu::Sampler sampler = test.device.CreateSampler(&desc);
    TEST_CHECK(sampler != nullptr);
    TEST_CHECK(test.errors == 0);
  }

  // 4. Mirrored repeat sampler
  {
    SamplerKey key{};
    key.clamp_u = uint32_t(ClampMode::kMirroredRepeat);
    key.clamp_v = uint32_t(ClampMode::kMirroredRepeat);
    key.clamp_w = uint32_t(ClampMode::kMirroredRepeat);
    key.mag_filter = uint32_t(TextureFilter::kLinear);
    key.min_filter = uint32_t(TextureFilter::kLinear);
    key.mip_filter = uint32_t(TextureFilter::kPoint);
    key.max_anisotropy = 1;

    wgpu::SamplerDescriptor desc = MakeSamplerDescriptor(key);
    wgpu::Sampler sampler = test.device.CreateSampler(&desc);
    TEST_CHECK(sampler != nullptr);
    TEST_CHECK(test.errors == 0);
  }

  std::printf("PASS: Live Dawn Sampler Creation (0 Dawn errors)\n");
}

int main(int argc, char** argv) {
  TestAddressModes();
  TestFilterModes();
  TestKeyAndDescriptorGeneration();

  unsigned adapter_index = 0;
  if (argc > 1) {
    adapter_index = static_cast<unsigned>(std::atoi(argv[1]));
  }
  TestLiveDawnSamplerCreation(adapter_index);

  std::printf("All Sampler Cache unit tests passed successfully!\n");
  return 0;
}
