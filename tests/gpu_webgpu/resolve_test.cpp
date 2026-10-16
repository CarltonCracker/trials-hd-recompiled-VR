// Authored resolve pixels: the production shader, no game inputs.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>
#include "gpu_test.h"

using Vec = std::array<float, 4>;
struct Constants {
  int32_t source[2] = {0, 0};
  int32_t destination[2] = {1, 1};
  float scale = 1;
  uint32_t mode = 0, flags = 1, padding = 0;
};
static_assert(sizeof(Constants) == 32);

int main(int argc, char** argv) {
  GpuTest gpu(argc > 1 ? unsigned(std::strtoul(argv[1], nullptr, 10)) : 0);
  std::ifstream file(SHADER_DIR "/resolve.wgsl");
  GPU_CHECK(bool(file));
  std::string source{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
  auto module = gpu.Module(source);
  const std::array<Vec, 4> pixels = {{{-0.6f, 0.2f, 0.8f, 1.4f},
      {-40000, 2.2f, 65536, 32770}, {0, -1.2f, 0.001f, 1}, {0.7f, 0.3f, 0.1f, -2}}};
  wgpu::TextureDescriptor td; td.size = {2, 2, 1}; td.format = wgpu::TextureFormat::RGBA32Float;
  td.usage = wgpu::TextureUsage::TextureBinding | wgpu::TextureUsage::CopyDst;
  auto input = gpu.device.CreateTexture(&td);
  wgpu::TexelCopyTextureInfo destination; destination.texture = input;
  wgpu::TexelCopyBufferLayout layout; layout.bytesPerRow = 32; layout.rowsPerImage = 2;
  wgpu::Extent3D extent{2, 2, 1};
  gpu.device.GetQueue().WriteTexture(&destination, pixels.data(), sizeof(pixels), &layout, &extent);
  td.size = {4, 4, 1}; td.usage = wgpu::TextureUsage::RenderAttachment | wgpu::TextureUsage::CopySrc;
  auto output = gpu.device.CreateTexture(&td);
  auto constants = gpu.Buffer(sizeof(Constants), wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst);
  std::array<wgpu::BindGroupLayoutEntry, 2> le;
  le[0].binding = 0; le[0].visibility = wgpu::ShaderStage::Fragment;
  le[0].texture.sampleType = wgpu::TextureSampleType::UnfilterableFloat;
  le[1].binding = 4; le[1].visibility = wgpu::ShaderStage::Fragment;
  le[1].buffer.type = wgpu::BufferBindingType::Uniform;
  wgpu::BindGroupLayoutDescriptor ld; ld.entryCount = le.size(); ld.entries = le.data();
  auto group_layout = gpu.device.CreateBindGroupLayout(&ld);
  wgpu::PipelineLayoutDescriptor pld; pld.bindGroupLayoutCount = 1; pld.bindGroupLayouts = &group_layout;
  auto pipeline_layout = gpu.device.CreatePipelineLayout(&pld);
  wgpu::ColorTargetState color; color.format = td.format;
  wgpu::FragmentState fs; fs.module = module; fs.entryPoint = "fs_color"; fs.targetCount = 1; fs.targets = &color;
  wgpu::RenderPipelineDescriptor pd; pd.layout = pipeline_layout;
  pd.vertex.module = module; pd.vertex.entryPoint = "vs_main"; pd.fragment = &fs;
  auto pipeline = gpu.device.CreateRenderPipeline(&pd);
  std::array<wgpu::BindGroupEntry, 2> entries;
  entries[0].binding = 0; entries[0].textureView = input.CreateView();
  entries[1].binding = 4; entries[1].buffer = constants; entries[1].size = sizeof(Constants);
  wgpu::BindGroupDescriptor bd; bd.layout = group_layout; bd.entryCount = entries.size(); bd.entries = entries.data();
  auto group = gpu.device.CreateBindGroup(&bd);
  unsigned cases = 0;
  for (unsigned mode = 0; mode < 4; ++mode) {
    for (float scale : {1.0f, 32.0f, 1024.0f}) {
      for (bool swap : {false, true}) {
        Constants data; data.mode = mode; data.scale = scale; data.flags |= swap ? 16u : 0u;
        gpu.device.GetQueue().WriteBuffer(constants, 0, &data, sizeof(data));
        wgpu::RenderPassColorAttachment attachment;
        attachment.view = output.CreateView(); attachment.loadOp = wgpu::LoadOp::Clear;
        attachment.storeOp = wgpu::StoreOp::Store; attachment.clearValue = {-3, -3, -3, -3};
        wgpu::RenderPassDescriptor rp; rp.colorAttachmentCount = 1; rp.colorAttachments = &attachment;
        auto encoder = gpu.device.CreateCommandEncoder(); auto pass = encoder.BeginRenderPass(&rp);
        pass.SetPipeline(pipeline); pass.SetBindGroup(0, group);
        pass.SetViewport(1, 1, 2, 2, 0, 1); pass.SetScissorRect(1, 1, 2, 2); pass.Draw(3); pass.End();
        auto readback = gpu.Buffer(1024, wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::CopySrc);
        wgpu::TexelCopyTextureInfo from; from.texture = output;
        wgpu::TexelCopyBufferInfo to; to.buffer = readback; to.layout.bytesPerRow = 256; to.layout.rowsPerImage = 4;
        wgpu::Extent3D size{4, 4, 1}; encoder.CopyTextureToBuffer(&from, &to, &size);
        auto command = encoder.Finish(); gpu.device.GetQueue().Submit(1, &command);
        auto bytes = gpu.Read(readback, 1024);
        for (unsigned y = 0; y < 4; ++y) {
          for (unsigned x = 0; x < 4; ++x) {
            Vec expected{-3, -3, -3, -3};
            if (x >= 1 && x < 3 && y >= 1 && y < 3) {
              expected = pixels[(y - 1) * 2 + x - 1];
              for (float& v : expected) {
                v *= scale;
                if (mode == 0) { v = std::clamp(v, 0.0f, 1.0f); }
                if (mode == 1) {
                  v = std::clamp(std::nearbyint(v), -32768.0f, 32767.0f);
                  if (v < 0) { v += 65536; }
                }
                if (mode == 2) { v = std::clamp(std::nearbyint(v), 0.0f, 65535.0f); }
              }
              if (swap) { std::swap(expected[0], expected[2]); }
            }
            for (unsigned c = 0; c < 4; ++c) {
              float actual; std::memcpy(&actual, bytes.data() + y * 256 + x * 16 + c * 4, 4);
              if (!std::isfinite(actual) || std::abs(actual - expected[c]) > 2e-5f * std::max(1.0f, std::abs(expected[c]))) {
                std::fprintf(stderr, "resolve mode%u scale%.9g swap%u x%u y%u c%u GPU%.9g CPU%.9g\n",
                    mode, scale, unsigned(swap), x, y, c, actual, expected[c]);
                GPU_CHECK(false);
              }
            }
          }
        }
        ++cases;
      }
    }
  }
  std::printf("PASS: %u production resolve shader cases, offsets, bounds, scales, integer modes and red/blue swap\n", cases);
}
