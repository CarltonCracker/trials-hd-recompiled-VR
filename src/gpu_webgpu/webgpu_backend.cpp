// rexgpu-webgpu - the WebGPU backend.

#include "webgpu_backend.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/graphics/pipeline/shader/shader.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/logging.h>
#include <rex/math.h>
#include <rex/ui/presenter.h>

#include "frame_recorder.h"
#include "guest_memory_mirror.h"
#include "pipeline_cache.h"
#include "primitive_converter.h"
#include "render_target_cache.h"
#include "texture_cache.h"
#include "webgpu_presenter.h"
#include "webgpu_provider.h"
#include "wgsl/blit.wgsl.h"
#include "wgsl/wgsl_shader_translator.h"

REXCVAR_DEFINE_INT32(webgpu_trace_frame, -1, "GPU/WebGPU",
                     "Log every draw, resolve and swap the backend receives in this guest frame "
                     "(-1 = off)");
REXCVAR_DEFINE_INT32(webgpu_dump_frame, -1, "GPU/WebGPU",
                     "First guest frame to dump per-draw probe records to webgpu_dump_file "
                     "(-1 = off)");
REXCVAR_DEFINE_INT32(webgpu_dump_count, 1, "GPU/WebGPU",
                     "Number of consecutive frames to dump (default 1)");
REXCVAR_DEFINE_STRING(webgpu_dump_file, "draws_webgpu.bin", "GPU/WebGPU",
                      "Output file for per-draw debug dump");

// Defined with the SDK's texture cache base (pipeline/texture/cache.cpp).
REXCVAR_DECLARE(int32_t, draw_resolution_scale_x);
REXCVAR_DECLARE(int32_t, draw_resolution_scale_y);

namespace rex::graphics::webgpu {

namespace {

uint64_t ShaderHash(const Shader* shader) {
  return shader ? shader->ucode_data_hash() : 0;
}

// A color copy remembered for presenting the frontbuffer (section 8.8 case 4)
// until the texture cache owns resolve destinations (L-09/L-10).
struct ResolvedRegion {
  RenderTarget* target = nullptr;
  uint32_t x = 0, y = 0, width = 0, height = 0;  // host pixels
};

struct BlitResources {
  wgpu::ShaderModule module;
  wgpu::BindGroupLayout layout;
  wgpu::PipelineLayout pipeline_layout;
  wgpu::RenderPipeline pipeline;
  wgpu::Buffer constants;
  wgpu::Buffer gamma_ramp;
};

}  // namespace

struct WebGpuBackendState {
  std::unordered_map<uint32_t, ResolvedRegion> resolved_regions;
  BlitResources blit;
  // Texture bind groups of the current frame by (layout, views, samplers).
  std::unordered_map<uint64_t, wgpu::BindGroup> texture_bind_groups;
};

static WebGpuBackendState& State();

WebGpuBackend::WebGpuBackend(ui::webgpu::WebGpuProvider& provider, ui::Presenter* presenter)
    : provider_(provider), presenter_(presenter) {}

WebGpuBackend::~WebGpuBackend() {
  std::lock_guard lock(provider_.device_mutex());
  if (frame_recorder_) {
    frame_recorder_->Submit();
  }
  draw_bind_group_ = nullptr;
  shared_memory_bind_group_ = nullptr;
  State().texture_bind_groups.clear();
  texture_cache_.reset();
  render_target_cache_.reset();
  pipeline_cache_.reset();
  primitive_processor_.reset();
  shared_memory_.reset();
  frame_recorder_.reset();
  if (dump_file_) {
    std::fflush(dump_file_);
    std::fclose(dump_file_);
    dump_file_ = nullptr;
  }
}

static WebGpuBackendState& State() {
  // One backend per process (one GPU plugin, one command processor).
  static WebGpuBackendState state;
  return state;
}

bool WebGpuBackend::Initialize(memory::Memory& memory, const RegisterFile& register_file) {
  std::lock_guard lock(provider_.device_mutex());
  const wgpu::Device& device = provider_.device();

  frame_recorder_ = std::make_unique<FrameRecorder>(provider_);
  if (!frame_recorder_->Initialize()) {
    return false;
  }
  shared_memory_ = std::make_unique<WebGpuSharedMemory>(
      memory, provider_.instance(), device, [this](uint64_t size) {
        GuestMemoryUpload upload;
        if (size <= FrameRecorder::kIndexRingSize) {
          uint32_t offset = 0;
          upload.data = frame_recorder_->AllocateIndices(uint32_t(size), 4, offset);
          upload.offset = offset;
          upload.source = frame_recorder_->index_ring();
        } else {
          wgpu::BufferDescriptor desc;
          desc.label = "large guest memory upload";
          desc.size = size;
          desc.usage = wgpu::BufferUsage::CopySrc;
          desc.mappedAtCreation = true;
          upload.source = provider_.device().CreateBuffer(&desc);
          if (upload.source) { upload.data = upload.source.GetMappedRange(0, size); }
        }
        frame_recorder_->EndRenderPass();
        upload.encoder = frame_recorder_->encoder();
        return upload;
      });
  if (!shared_memory_->Initialize()) {
    return false;
  }
  primitive_processor_ = std::make_unique<WebGpuPrimitiveProcessor>(
      register_file, memory, *shared_memory_, provider_, *frame_recorder_);
  if (!primitive_processor_->Initialize()) {
    REXGPU_ERROR("WebGPU: failed to initialize the primitive processor");
    return false;
  }
  pipeline_cache_ = std::make_unique<PipelineCache>(provider_);
  if (!pipeline_cache_->Initialize()) {
    return false;
  }
  texture_cache_ = std::make_unique<TextureCache>(provider_, memory, *shared_memory_);
  if (!texture_cache_->Initialize()) {
    return false;
  }
  render_target_cache_ = std::make_unique<RenderTargetCache>(
      provider_, uint32_t(std::clamp(REXCVAR_GET(draw_resolution_scale_x), 1, 3)),
      uint32_t(std::clamp(REXCVAR_GET(draw_resolution_scale_y), 1, 3)));

  // Group 0: the uniform ring with one window per binding (7.6.5).
  static constexpr std::array<std::pair<uint32_t, uint64_t>, 5> kUniformWindows = {{
      {wgsl_binding::kSystemConstants, sizeof(WgslSystemConstants)},
      {wgsl_binding::kVertexFloatConstants, 4096},
      {wgsl_binding::kPixelFloatConstants, 4096},
      {wgsl_binding::kBoolLoopConstants, 160},
      {wgsl_binding::kFetchConstants, 768},
  }};
  std::array<wgpu::BindGroupEntry, 5> draw_entries;
  for (size_t i = 0; i < draw_entries.size(); ++i) {
    draw_entries[i].binding = kUniformWindows[i].first;
    draw_entries[i].buffer = frame_recorder_->uniform_ring();
    draw_entries[i].size = kUniformWindows[i].second;
  }
  wgpu::BindGroupDescriptor draw_desc;
  draw_desc.label = "draw uniforms";
  draw_desc.layout = pipeline_cache_->draw_bind_group_layout();
  draw_desc.entryCount = draw_entries.size();
  draw_desc.entries = draw_entries.data();
  draw_bind_group_ = device.CreateBindGroup(&draw_desc);

  wgpu::BindGroupEntry shared_entry;
  shared_entry.binding = wgsl_binding::kSharedMemory;
  shared_entry.buffer = shared_memory_->buffer();
  wgpu::BindGroupDescriptor shared_desc;
  shared_desc.label = "shared memory";
  shared_desc.layout = pipeline_cache_->shared_memory_bind_group_layout();
  shared_desc.entryCount = 1;
  shared_desc.entries = &shared_entry;
  shared_memory_bind_group_ = device.CreateBindGroup(&shared_desc);

  // Frontbuffer blit.
  BlitResources& blit = State().blit;
  wgpu::ShaderSourceWGSL wgsl;
  wgsl.code = kWgsl_blit;
  wgpu::ShaderModuleDescriptor module_desc;
  module_desc.nextInChain = &wgsl;
  module_desc.label = "blit.wgsl";
  blit.module = device.CreateShaderModule(&module_desc);
  std::array<wgpu::BindGroupLayoutEntry, 2> blit_entries;
  blit_entries[0].binding = 0;
  blit_entries[0].visibility = wgpu::ShaderStage::Fragment;
  blit_entries[0].texture.sampleType = wgpu::TextureSampleType::UnfilterableFloat;
  blit_entries[1].binding = 1;
  blit_entries[1].visibility = wgpu::ShaderStage::Fragment;
  blit_entries[1].buffer.type = wgpu::BufferBindingType::Uniform;
  wgpu::BindGroupLayoutEntry gamma_entry;
  gamma_entry.binding = 2;
  gamma_entry.visibility = wgpu::ShaderStage::Fragment;
  gamma_entry.buffer.type = wgpu::BufferBindingType::ReadOnlyStorage;
  wgpu::BindGroupLayoutDescriptor blit_layout_desc;
  std::array<wgpu::BindGroupLayoutEntry, 3> all_blit_entries = {blit_entries[0], blit_entries[1],
                                                                gamma_entry};
  blit_layout_desc.entryCount = all_blit_entries.size();
  blit_layout_desc.entries = all_blit_entries.data();
  blit.layout = device.CreateBindGroupLayout(&blit_layout_desc);
  wgpu::PipelineLayoutDescriptor blit_pipeline_layout_desc;
  blit_pipeline_layout_desc.bindGroupLayoutCount = 1;
  blit_pipeline_layout_desc.bindGroupLayouts = &blit.layout;
  blit.pipeline_layout = device.CreatePipelineLayout(&blit_pipeline_layout_desc);
  wgpu::ColorTargetState blit_target;
  blit_target.format = ui::webgpu::WebGpuPresenter::kGuestOutputFormat;
  wgpu::FragmentState blit_fragment;
  blit_fragment.module = blit.module;
  blit_fragment.entryPoint = "fs_main";
  blit_fragment.targetCount = 1;
  blit_fragment.targets = &blit_target;
  wgpu::RenderPipelineDescriptor blit_desc;
  blit_desc.label = "frontbuffer blit";
  blit_desc.layout = blit.pipeline_layout;
  blit_desc.vertex.module = blit.module;
  blit_desc.vertex.entryPoint = "vs_main";
  blit_desc.fragment = &blit_fragment;
  blit.pipeline = device.CreateRenderPipeline(&blit_desc);
  wgpu::BufferDescriptor blit_constants_desc;
  blit_constants_desc.size = 16;
  blit_constants_desc.usage = wgpu::BufferUsage::Uniform | wgpu::BufferUsage::CopyDst;
  blit.constants = device.CreateBuffer(&blit_constants_desc);
  wgpu::BufferDescriptor gamma_desc;
  gamma_desc.label = "gamma ramp";
  gamma_desc.size = 128 * 3 * 4;  // The larger of the two ramps.
  gamma_desc.usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopyDst;
  blit.gamma_ramp = device.CreateBuffer(&gamma_desc);
  return true;
}

uint64_t WebGpuBackend::submission_count() const {
  return frame_recorder_ ? frame_recorder_->submission_count() : 0;
}

bool WebGpuBackend::IsTracedFrame(uint32_t frame_index) const {
  int32_t traced = REXCVAR_GET(webgpu_trace_frame);
  return traced >= 0 && uint32_t(traced) == frame_index;
}

#pragma pack(push, 1)
struct ProbeHeader {
  char kind[4];
  uint32_t win = 0;
  uint32_t prim = 0;
  uint32_t count = 0;
  uint32_t major = 0;
  uint64_t vh = 0;
  uint64_t ph = 0;
};
static_assert(sizeof(ProbeHeader) == 36);

struct ProbeIndexBuffer {
  uint32_t format = 0;
  uint32_t endianness = 0;
  uint32_t count = 0;
  uint32_t guest_base = 0;
  uint64_t length = 0;
};
static_assert(sizeof(ProbeIndexBuffer) == 24);

struct ProbeRecord {
  ProbeHeader header;
  ProbeIndexBuffer ib;
  uint32_t ctx_regs[0x400]{};    // 0x2000..0x23FF (4096 B)
  uint32_t fetch_regs[0x200]{};  // 0x4800..0x49FF (2048 B)
};
static_assert(sizeof(ProbeRecord) == 6204);
#pragma pack(pop)

void WebGpuBackend::EnsureDumpOpen(uint32_t frame_index) {
  if (dump_file_) {
    return;
  }
  int32_t dump_start = REXCVAR_GET(webgpu_dump_frame);
  if (dump_start < 0) {
    return;
  }
  int32_t dump_count = std::max(1, REXCVAR_GET(webgpu_dump_count));
  if (int32_t(frame_index) < dump_start || int32_t(frame_index) >= dump_start + dump_count) {
    return;
  }
  std::string path = REXCVAR_GET(webgpu_dump_file);
  dump_file_ = std::fopen(path.c_str(), "wb");
  if (!dump_file_) {
    REXGPU_ERROR("Failed to open per-draw dump file: {}", path);
    return;
  }
  // Leading SWAP record (win = 0, prim = 0)
  ProbeRecord rec{};
  std::memcpy(rec.header.kind, "SWAP", 4);
  rec.header.win = 0;
  rec.header.prim = 0;
  std::fwrite(&rec, sizeof(rec), 1, dump_file_);
  dump_swaps_written_ = 1;
  dump_records_written_ = 1;
}

void WebGpuBackend::DumpDraw(const DrawSnapshot& draw) {
  int32_t dump_start = REXCVAR_GET(webgpu_dump_frame);
  if (dump_start < 0) {
    return;
  }
  int32_t dump_count = std::max(1, REXCVAR_GET(webgpu_dump_count));
  if (int32_t(draw.frame_index) < dump_start ||
      int32_t(draw.frame_index) >= dump_start + dump_count) {
    return;
  }
  EnsureDumpOpen(draw.frame_index);
  if (!dump_file_) {
    return;
  }

  ProbeRecord rec{};
  std::memcpy(rec.header.kind, "DRAW", 4);
  rec.header.win = 0;
  rec.header.prim = static_cast<uint32_t>(draw.primitive_type);
  rec.header.count = draw.index_count;
  rec.header.major = draw.major_mode_explicit ? 1 : 0;
  rec.header.vh = ShaderHash(draw.vertex_shader);
  rec.header.ph = ShaderHash(draw.pixel_shader);
  if (draw.indexed) {
    rec.ib.format = static_cast<uint32_t>(draw.index_buffer.format);
    rec.ib.endianness = static_cast<uint32_t>(draw.index_buffer.endianness);
    rec.ib.count = draw.index_buffer.count;
    rec.ib.guest_base = draw.index_buffer.guest_base;
    rec.ib.length = draw.index_buffer.length_bytes;
  }
  std::memcpy(rec.ctx_regs, &draw.regs->values[0x2000], sizeof(rec.ctx_regs));
  std::memcpy(rec.fetch_regs, &draw.regs->values[0x4800], sizeof(rec.fetch_regs));
  std::fwrite(&rec, sizeof(rec), 1, dump_file_);
  ++dump_records_written_;
}

void WebGpuBackend::DumpResolve(const ResolveSnapshot& resolve) {
  int32_t dump_start = REXCVAR_GET(webgpu_dump_frame);
  if (dump_start < 0) {
    return;
  }
  int32_t dump_count = std::max(1, REXCVAR_GET(webgpu_dump_count));
  if (int32_t(resolve.frame_index) < dump_start ||
      int32_t(resolve.frame_index) >= dump_start + dump_count) {
    return;
  }
  EnsureDumpOpen(resolve.frame_index);
  if (!dump_file_) {
    return;
  }

  ProbeRecord rec{};
  std::memcpy(rec.header.kind, "COPY", 4);
  rec.header.win = 0;
  rec.header.prim = 0;
  rec.header.count = 0;
  rec.header.major = 0;
  rec.header.vh = 0;
  rec.header.ph = 0;
  std::memcpy(rec.ctx_regs, &resolve.regs->values[0x2000], sizeof(rec.ctx_regs));
  std::memcpy(rec.fetch_regs, &resolve.regs->values[0x4800], sizeof(rec.fetch_regs));
  std::fwrite(&rec, sizeof(rec), 1, dump_file_);
  ++dump_records_written_;
}

void WebGpuBackend::DumpSwap(const SwapSnapshot& swap) {
  int32_t dump_start = REXCVAR_GET(webgpu_dump_frame);
  if (dump_start < 0) {
    return;
  }
  int32_t dump_count = std::max(1, REXCVAR_GET(webgpu_dump_count));
  if (int32_t(swap.frame_index) < dump_start ||
      int32_t(swap.frame_index) >= dump_start + dump_count) {
    return;
  }
  EnsureDumpOpen(swap.frame_index);
  if (!dump_file_) {
    return;
  }

  // Trailing SWAP record for the end of this frame
  ProbeRecord rec{};
  std::memcpy(rec.header.kind, "SWAP", 4);
  rec.header.win = 0;
  rec.header.prim = dump_swaps_written_++;
  std::fwrite(&rec, sizeof(rec), 1, dump_file_);
  ++dump_records_written_;

  if (int32_t(swap.frame_index) + 1 >= dump_start + dump_count) {
    std::fflush(dump_file_);
    std::fclose(dump_file_);
    dump_file_ = nullptr;
    REXGPU_INFO("webgpu dump: frame(s) [{}..{}] dumped to {} ({} records)",
                dump_start, swap.frame_index, REXCVAR_GET(webgpu_dump_file), dump_records_written_);
  }
}

namespace {

wgpu::PrimitiveTopology ToTopology(xenos::PrimitiveType type, bool& supported) {
  supported = true;
  switch (type) {
    case xenos::PrimitiveType::kPointList:
      return wgpu::PrimitiveTopology::PointList;
    case xenos::PrimitiveType::kLineList:
      return wgpu::PrimitiveTopology::LineList;
    case xenos::PrimitiveType::kLineStrip:
      return wgpu::PrimitiveTopology::LineStrip;
    case xenos::PrimitiveType::kTriangleList:
      return wgpu::PrimitiveTopology::TriangleList;
    case xenos::PrimitiveType::kTriangleStrip:
      return wgpu::PrimitiveTopology::TriangleStrip;
    default:
      supported = false;
      return wgpu::PrimitiveTopology::TriangleList;
  }
}

bool IsStripTopology(wgpu::PrimitiveTopology topology) {
  return topology == wgpu::PrimitiveTopology::LineStrip ||
         topology == wgpu::PrimitiveTopology::TriangleStrip;
}

// Copies the float constants a shader reads, packed as the translator expects.
uint32_t PackFloatConstants(const RegisterFile& regs, const Shader& shader, uint32_t base,
                            std::array<float, 1024>& out) {
  const Shader::ConstantRegisterMap& map = shader.constant_register_map();
  const auto* registers = reinterpret_cast<const float*>(&regs.values[XE_GPU_REG_SHADER_CONSTANT_000_X]);
  uint32_t count = 0;
  if (map.float_dynamic_addressing) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t index = base + i;
      if (index < 512) {
        std::memcpy(&out[i * 4], &registers[index * 4], 16);
      } else {
        std::memset(&out[i * 4], 0, 16);
      }
    }
    return 256;
  }
  for (uint32_t block = 0; block < 4; ++block) {
    uint64_t bits = map.float_bitmap[block];
    uint32_t bit;
    while (rex::bit_scan_forward(bits, &bit)) {
      bits &= ~(uint64_t(1) << bit);
      uint32_t index = base + block * 64 + bit;
      if (index < 512) {
        std::memcpy(&out[count * 4], &registers[index * 4], 16);
      } else {
        std::memset(&out[count * 4], 0, 16);
      }
      ++count;
    }
  }
  return std::max(count, uint32_t(1));
}

}  // namespace

bool WebGpuBackend::Draw(const DrawSnapshot& draw) {
  const RegisterFile& regs = *draw.regs;
  if (IsTracedFrame(draw.frame_index)) {
    REXGPU_INFO(
        "webgpu trace: frame {} draw {}: prim {} count {} indexed {} (fmt {} endian {} base "
        "0x{:08X}) vs {:016X} ps {:016X} dirty 0x{:02X} surface_info 0x{:08X} color_info "
        "0x{:08X} depth_info 0x{:08X}",
        draw.frame_index, draw.draw_index, uint32_t(draw.primitive_type), draw.index_count,
        draw.indexed, uint32_t(draw.index_buffer.format), uint32_t(draw.index_buffer.endianness),
        draw.index_buffer.guest_base, ShaderHash(draw.vertex_shader),
        ShaderHash(draw.pixel_shader), draw.dirty, regs.values[XE_GPU_REG_RB_SURFACE_INFO],
        regs.values[XE_GPU_REG_RB_COLOR_INFO], regs.values[XE_GPU_REG_RB_DEPTH_INFO]);
  }
  DumpDraw(draw);
  std::lock_guard lock(provider_.device_mutex());
  if (!shared_memory_) {
    return false;
  }
  xenos::EdramMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode != xenos::EdramMode::kColorDepth && edram_mode != xenos::EdramMode::kDepthOnly) {
    return true;
  }
  Shader* vertex_shader = draw.vertex_shader;
  if (!vertex_shader) {
    return false;
  }
  bool primitive_polygonal = draw_util::IsPrimitivePolygonal(regs);
  if (!draw_util::IsRasterizationPotentiallyDone(regs, primitive_polygonal)) {
    // Memory export only (L-04g) or nothing visible.
    return true;
  }
  Shader* pixel_shader = draw.pixel_shader;
  if (edram_mode == xenos::EdramMode::kDepthOnly ||
      (pixel_shader &&
       !draw_util::IsPixelShaderNeededWithRasterization(*pixel_shader, regs))) {
    pixel_shader = nullptr;
  }

  // Guest memory read by the vertex shader. Anything that may submit (and
  // so flush staged uniforms and indices) happens before this draw stages
  // its own data.
  for (const Shader::VertexBinding& binding : vertex_shader->vertex_bindings()) {
    xenos::xe_gpu_vertex_fetch_t fetch = regs.GetVertexFetch(binding.fetch_constant);
    if (fetch.type != xenos::FetchConstantType::kVertex || !fetch.size) {
      continue;
    }
    shared_memory_->RequestRange(fetch.address << 2, fetch.size << 2);
  }
  // Textures (loading may also submit).
  TextureLayoutSignature texture_signature =
      TextureLayoutSignature::Get(*vertex_shader, pixel_shader);
  wgpu::BindGroup texture_bind_group = pipeline_cache_->empty_texture_bind_group();
  if (texture_signature.value) {
    std::vector<wgpu::BindGroupEntry> entries;
    uint64_t bind_group_hash = texture_signature.value;
    auto add_stage = [&](const Shader& shader, uint32_t base) {
      auto slots = WgslShaderTranslator::GetTextureSlots(shader);
      for (uint32_t i = 0; i < slots.size(); ++i) {
        xenos::xe_gpu_texture_fetch_t fetch = regs.GetTextureFetch(slots[i].fetch_constant);
        wgpu::TextureView view = texture_cache_->GetView(fetch, slots[i].is_cube);
        wgpu::Sampler sampler = texture_cache_->GetSampler(
            fetch, shader.texture_bindings()[slots[i].first_binding].fetch_instr.attributes,
            shader.type() == xenos::ShaderType::kVertex);
        wgpu::BindGroupEntry texture_entry;
        texture_entry.binding = base + i;
        texture_entry.textureView = view;
        entries.push_back(texture_entry);
        wgpu::BindGroupEntry sampler_entry;
        sampler_entry.binding = base + i + wgsl_binding::kSamplerOffset;
        sampler_entry.sampler = sampler;
        entries.push_back(sampler_entry);
        bind_group_hash = (bind_group_hash * 0x100000001B3ull) ^ uint64_t(uintptr_t(view.Get()));
        bind_group_hash = (bind_group_hash * 0x100000001B3ull) ^ uint64_t(uintptr_t(sampler.Get()));
      }
    };
    if (pixel_shader) {
      add_stage(*pixel_shader, 0);
    }
    add_stage(*vertex_shader, wgsl_binding::kVertexTextureBase);
    auto& bind_groups = State().texture_bind_groups;
    auto it = bind_groups.find(bind_group_hash);
    if (it != bind_groups.end()) {
      texture_bind_group = it->second;
    } else {
      wgpu::BindGroupDescriptor desc;
      desc.label = "textures";
      desc.layout = pipeline_cache_->GetTextureBindGroupLayout(texture_signature);
      desc.entryCount = entries.size();
      desc.entries = entries.data();
      texture_bind_group = provider_.device().CreateBindGroup(&desc);
      bind_groups.emplace(bind_group_hash, texture_bind_group);
    }
  }
  frame_recorder_->ReserveUniforms(4 * 4096);

  PrimitiveProcessor::ProcessingResult primitive;
  if (!primitive_processor_->Process(primitive)) {
    return false;
  }
  if (!primitive.host_draw_vertex_count) {
    return true;
  }
  bool topology_supported;
  wgpu::PrimitiveTopology topology =
      ToTopology(primitive.host_primitive_type, topology_supported);
  if ((primitive.host_vertex_shader_type != Shader::HostVertexShaderType::kVertex &&
       primitive.host_vertex_shader_type !=
           Shader::HostVertexShaderType::kRectangleListAsTriangleStrip) ||
      !topology_supported) {
    // Point sprite expansion and tessellation are not drawn.
    ++skipped_draws_;
    return true;
  }

  // Render targets.
  uint32_t pixel_shader_writes = pixel_shader ? pixel_shader->writes_color_targets() : 0;
  uint32_t color_mask = edram_mode == xenos::EdramMode::kColorDepth
                            ? draw_util::GetNormalizedColorMask(regs, pixel_shader_writes)
                            : 0;
  uint32_t color_targets_used = 0;
  for (uint32_t i = 0; i < 4; ++i) {
    if ((color_mask >> (4 * i)) & 0xF) {
      color_targets_used |= 1u << i;
    }
  }
  reg::RB_DEPTHCONTROL depth_control = draw_util::GetNormalizedDepthControl(regs);
  bool depth_used = depth_control.z_enable || depth_control.stencil_enable;
  RenderTargetCache::Binding targets;
  if (!render_target_cache_->Update(regs, color_targets_used, depth_used, targets)) {
    return true;
  }

  // Shader modifications.
  auto sq_program_cntl = regs.Get<reg::SQ_PROGRAM_CNTL>();
  uint32_t interpolator_mask = 0;
  uint32_t param_gen_pos = UINT32_MAX;
  if (pixel_shader) {
    interpolator_mask = pixel_shader->GetInterpolatorInputMask(
        sq_program_cntl, regs.Get<reg::SQ_CONTEXT_MISC>(), param_gen_pos);
  }
  WgslShaderTranslator::Modification vertex_modification;
  vertex_modification.dynamic_addressable_register_count =
      vertex_shader->GetDynamicAddressableRegisterCount(sq_program_cntl.vs_num_reg);
  vertex_modification.host_vertex_shader_type = uint32_t(primitive.host_vertex_shader_type);
  vertex_modification.interpolator_mask = interpolator_mask;
  WgslShaderTranslator::Modification pixel_modification;
  if (pixel_shader) {
    pixel_modification.dynamic_addressable_register_count =
        pixel_shader->GetDynamicAddressableRegisterCount(sq_program_cntl.ps_num_reg);
    pixel_modification.interpolator_mask = interpolator_mask;
    if (param_gen_pos < xenos::kMaxInterpolators) {
      pixel_modification.param_gen = 1;
      pixel_modification.param_gen_interpolator = param_gen_pos;
      pixel_modification.param_gen_point =
          uint32_t(regs.Get<reg::VGT_DRAW_INITIATOR>().prim_type ==
                   xenos::PrimitiveType::kPointList);
    }
    pixel_modification.color_targets = color_targets_used;
  }

  // Pipeline.
  PipelineKey key;
  std::memset(&key, 0, sizeof(key));
  key.vertex_shader_hash = vertex_shader->ucode_data_hash();
  key.texture_layout = texture_signature.value;
  key.vertex_shader_modification = vertex_modification.value;
  key.pixel_shader_hash = pixel_shader ? pixel_shader->ucode_data_hash() : 0;
  key.pixel_shader_modification = pixel_shader ? pixel_modification.value : 0;
  key.topology = uint32_t(topology);
  bool indexed = primitive.index_buffer_type != PrimitiveProcessor::kNone;
  wgpu::IndexFormat index_format = primitive.host_index_format == xenos::IndexFormat::kInt16
                                       ? wgpu::IndexFormat::Uint16
                                       : wgpu::IndexFormat::Uint32;
  key.strip_index_format = uint32_t((indexed && IsStripTopology(topology))
                                        ? index_format
                                        : wgpu::IndexFormat::Undefined);
  auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  wgpu::CullMode cull_mode = wgpu::CullMode::None;
  if (primitive_polygonal) {
    if (pa_su_sc_mode_cntl.cull_front && pa_su_sc_mode_cntl.cull_back) {
      return true;  // Everything is culled.
    }
    cull_mode = pa_su_sc_mode_cntl.cull_front  ? wgpu::CullMode::Front
                : pa_su_sc_mode_cntl.cull_back ? wgpu::CullMode::Back
                                               : wgpu::CullMode::None;
  }
  key.cull_mode = uint32_t(cull_mode);
  key.front_face =
      uint32_t(pa_su_sc_mode_cntl.face == 0 ? wgpu::FrontFace::CCW : wgpu::FrontFace::CW);
  key.unclipped_depth = regs.Get<reg::PA_CL_CLIP_CNTL>().clip_disable;
  key.sample_count = targets.sample_count;
  static constexpr Register kBlendControlRegisters[] = {
      XE_GPU_REG_RB_BLENDCONTROL0, XE_GPU_REG_RB_BLENDCONTROL1, XE_GPU_REG_RB_BLENDCONTROL2,
      XE_GPU_REG_RB_BLENDCONTROL3};
  WgslSystemConstants system;
  for (uint32_t i = 0; i < 4; ++i) {
    RenderTarget* target = targets.color[i];
    if (!target) {
      continue;
    }
    key.color_formats[i] = uint32_t(target->host_format);
    key.color_write_masks[i] = (color_mask >> (4 * i)) & 0xF;
    auto blend_control = regs.Get<reg::RB_BLENDCONTROL>(kBlendControlRegisters[i]);
    bool blending_disabled =
        blend_control.color_srcblend == xenos::BlendFactor::kOne &&
        blend_control.color_destblend == xenos::BlendFactor::kZero &&
        blend_control.color_comb_fcn == xenos::BlendOp::kAdd &&
        blend_control.alpha_srcblend == xenos::BlendFactor::kOne &&
        blend_control.alpha_destblend == xenos::BlendFactor::kZero &&
        blend_control.alpha_comb_fcn == xenos::BlendOp::kAdd;
    // Unorm/snorm/float formats used here are all blendable except 32-bit float.
    bool blendable = target->host_format != wgpu::TextureFormat::R32Float &&
                     target->host_format != wgpu::TextureFormat::RG32Float;
    key.blend_controls[i] = (blending_disabled || !blendable) ? UINT32_MAX : blend_control.value;
    static constexpr Register kColorInfoRegisters[] = {
        XE_GPU_REG_RB_COLOR_INFO, XE_GPU_REG_RB_COLOR1_INFO, XE_GPU_REG_RB_COLOR2_INFO,
        XE_GPU_REG_RB_COLOR3_INFO};
    int32_t exp_bias = regs.Get<reg::RB_COLOR_INFO>(kColorInfoRegisters[i]).color_exp_bias;
    system.color_output_scale[i] = std::ldexp(target->format_output_scale, exp_bias);
  }
  if (targets.depth) {
    key.depth_stencil_format = uint32_t(targets.depth->host_format);
    key.depth_control = depth_control.value;
    auto stencil_ref_mask = regs.Get<reg::RB_STENCILREFMASK>();
    key.stencil_read_mask = stencil_ref_mask.stencilmask;
    key.stencil_write_mask = stencil_ref_mask.stencilwritemask;
    float slope, constant;
    draw_util::GetPreferredFacePolygonOffset(regs, primitive_polygonal, slope, constant);
    constant *= regs.Get<reg::RB_DEPTH_INFO>().depth_format ==
                        xenos::DepthRenderTargetFormat::kD24S8
                    ? draw_util::kD3D10PolygonOffsetFactorUnorm24
                    : draw_util::kD3D10PolygonOffsetFactorFloat24;
    slope *= xenos::kPolygonOffsetScaleSubpixelUnit *
             float(std::max(render_target_cache_->scale_x(), render_target_cache_->scale_y()));
    key.depth_bias = int32_t(std::lround(constant));
    key.depth_bias_slope_scale = slope;
  }
  wgpu::RenderPipeline pipeline = pipeline_cache_->GetPipeline(key, *vertex_shader, pixel_shader);
  if (!pipeline) {
    ++skipped_draws_;
    return false;
  }

  // System constants.
  auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
  if (pa_cl_vte_cntl.vtx_xy_fmt) {
    system.flags |= WgslSystemConstants::kXYDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_z_fmt) {
    system.flags |= WgslSystemConstants::kZDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_w0_fmt) {
    system.flags |= WgslSystemConstants::kWNotReciprocal;
  }
  if (primitive_polygonal) {
    system.flags |= WgslSystemConstants::kPrimitivePolygonal;
  }
  auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  if (rb_colorcontrol.alpha_test_enable && pixel_shader &&
      rb_colorcontrol.alpha_func != xenos::CompareFunction::kAlways) {
    system.flags |= WgslSystemConstants::kAlphaTest;
    system.alpha_test_function = uint32_t(rb_colorcontrol.alpha_func);
    system.alpha_test_reference = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
  }
  system.vertex_index_endian = uint32_t(primitive.host_shader_index_endian);
  if (primitive.index_buffer_type == PrimitiveProcessor::kHostBuiltinForDMA) {
    // Expanded primitives read the guest indices in the vertex shader.
    system.flags |= WgslSystemConstants::kVertexIndexLoad;
    if (regs.Get<reg::VGT_DRAW_INITIATOR>().index_size == xenos::IndexFormat::kInt32) {
      system.flags |= WgslSystemConstants::kVertexIndexLoad32Bit;
    }
    system.vertex_index_load_address = primitive.guest_index_base;
  }
  system.vertex_base_index = regs.Get<int32_t>(XE_GPU_REG_VGT_INDX_OFFSET);
  system.vertex_index_min = regs.Get<uint32_t>(XE_GPU_REG_VGT_MIN_VTX_INDX);
  system.vertex_index_max = regs.Get<uint32_t>(XE_GPU_REG_VGT_MAX_VTX_INDX);
  draw_util::ViewportInfo viewport;
  draw_util::GetHostViewportInfo(regs, render_target_cache_->scale_x(),
                                 render_target_cache_->scale_y(), true, targets.width,
                                 targets.height, false, depth_control, false, false,
                                 pixel_shader && pixel_shader->writes_depth(), viewport);
  for (uint32_t i = 0; i < 3; ++i) {
    system.ndc_scale[i] = viewport.ndc_scale[i];
    system.ndc_offset[i] = viewport.ndc_offset[i];
  }
  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor);
  uint32_t scissor_x = std::min(scissor.offset[0] * render_target_cache_->scale_x(), targets.width);
  uint32_t scissor_y = std::min(scissor.offset[1] * render_target_cache_->scale_y(), targets.height);
  uint32_t scissor_width =
      std::min(scissor.extent[0] * render_target_cache_->scale_x(), targets.width - scissor_x);
  uint32_t scissor_height =
      std::min(scissor.extent[1] * render_target_cache_->scale_y(), targets.height - scissor_y);
  uint32_t viewport_x = std::min(viewport.xy_offset[0], targets.width);
  uint32_t viewport_y = std::min(viewport.xy_offset[1], targets.height);
  uint32_t viewport_width = std::min(viewport.xy_extent[0], targets.width - viewport_x);
  uint32_t viewport_height = std::min(viewport.xy_extent[1], targets.height - viewport_y);
  if (!scissor_width || !scissor_height || !viewport_width || !viewport_height) {
    return true;
  }

  // Uniform uploads (space reserved above, so no submission here).
  std::array<uint32_t, 5> offsets;
  offsets[0] = frame_recorder_->UploadUniforms(&system, sizeof(system));
  std::array<float, 1024> constants;
  uint32_t vertex_constant_count =
      PackFloatConstants(regs, *vertex_shader, regs.Get<reg::SQ_VS_CONST>().base, constants);
  offsets[1] = frame_recorder_->UploadUniforms(constants.data(), vertex_constant_count * 16);
  if (pixel_shader) {
    uint32_t pixel_constant_count =
        PackFloatConstants(regs, *pixel_shader, regs.Get<reg::SQ_PS_CONST>().base, constants);
    offsets[2] = frame_recorder_->UploadUniforms(constants.data(), pixel_constant_count * 16);
  } else {
    offsets[2] = offsets[1];
  }
  offsets[3] = frame_recorder_->UploadUniforms(
      &regs.values[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031], 40 * sizeof(uint32_t));
  offsets[4] = frame_recorder_->UploadUniforms(&regs.values[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0],
                                               192 * sizeof(uint32_t));

  // Render pass.
  RenderPassTargets pass_targets;
  for (uint32_t i = 0; i < 4; ++i) {
    if (RenderTarget* target = targets.color[i]) {
      pass_targets.color[i] = target->view;
      if (!target->initialized) {
        pass_targets.color_clear_mask |= 1u << i;
        target->initialized = true;
      }
    }
  }
  if (RenderTarget* target = targets.depth) {
    pass_targets.depth_stencil = target->view;
    pass_targets.depth_has_stencil =
        RenderTargetCache::HostDepthFormatHasStencil(target->host_format);
    if (!target->initialized) {
      pass_targets.depth_clear = true;
      target->initialized = true;
    }
  }
  const wgpu::RenderPassEncoder& pass = frame_recorder_->BeginRenderPass(pass_targets);
  pass.SetPipeline(pipeline);
  pass.SetBindGroup(0, draw_bind_group_, offsets.size(), offsets.data());
  pass.SetBindGroup(1, shared_memory_bind_group_);
  pass.SetBindGroup(2, texture_bind_group);
  pass.SetViewport(float(viewport_x), float(viewport_y), float(viewport_width),
                   float(viewport_height), viewport.z_min, viewport.z_max);
  pass.SetScissorRect(scissor_x, scissor_y, scissor_width, scissor_height);
  if (targets.depth) {
    pass.SetStencilReference(regs.Get<reg::RB_STENCILREFMASK>().stencilref);
  }
  wgpu::Color blend_constant = {regs.Get<float>(XE_GPU_REG_RB_BLEND_RED),
                                regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN),
                                regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE),
                                regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA)};
  pass.SetBlendConstant(&blend_constant);

  uint32_t count = primitive.host_draw_vertex_count;
  uint32_t index_size = index_format == wgpu::IndexFormat::Uint16 ? 2 : 4;
  switch (primitive.index_buffer_type) {
    case PrimitiveProcessor::kNone:
      pass.Draw(count);
      break;
    case PrimitiveProcessor::kGuestDMA:
      pass.SetIndexBuffer(shared_memory_->buffer(), index_format, primitive.guest_index_base,
                          uint64_t(count) * index_size);
      pass.DrawIndexed(count);
      break;
    case PrimitiveProcessor::kHostConverted:
      pass.SetIndexBuffer(frame_recorder_->index_ring(), index_format,
                          WebGpuPrimitiveProcessor::GetConvertedIndexBufferOffset(
                              primitive.host_index_buffer_handle),
                          uint64_t(count) * index_size);
      pass.DrawIndexed(count);
      break;
    case PrimitiveProcessor::kHostBuiltinForAuto:
    case PrimitiveProcessor::kHostBuiltinForDMA:
      pass.SetIndexBuffer(primitive_processor_->builtin_index_buffer(), index_format,
                          WebGpuPrimitiveProcessor::GetBuiltinIndexBufferOffset(
                              primitive.host_index_buffer_handle),
                          uint64_t(count) * index_size);
      pass.DrawIndexed(count);
      break;
  }
  return true;
}

bool WebGpuBackend::Resolve(const ResolveSnapshot& resolve) {
  const draw_util::ResolveInfo& info = resolve.info;
  if (IsTracedFrame(resolve.frame_index)) {
    REXGPU_INFO(
        "webgpu trace: frame {} resolve {}: copy_control 0x{:08X} dest_info 0x{:08X} dest_base "
        "0x{:08X} color_base {} depth_base {} height_div_8 {}",
        resolve.frame_index, resolve.resolve_index, info.rb_copy_control.value,
        info.copy_dest_info.value, info.copy_dest_base, info.color_original_base,
        info.depth_original_base, info.height_div_8);
  }
  DumpResolve(resolve);
  std::lock_guard lock(provider_.device_mutex());
  if (!render_target_cache_) {
    return false;
  }
  uint32_t scale_x = render_target_cache_->scale_x();
  uint32_t scale_y = render_target_cache_->scale_y();

  // Copy: remember the source region for the frontbuffer (L-09 replaces this
  // with real resolve destinations in the texture cache).
  if (info.rb_copy_control.copy_command != xenos::CopyCommand::kNull && !info.IsCopyingDepth() &&
      info.height_div_8) {
    RenderTarget* source = render_target_cache_->FindLastUsed(info.color_original_base, false);
    if (source) {
      const draw_util::ResolveEdramInfo& edram = info.color_edram_info;
      uint32_t samples_x = edram.msaa_samples == xenos::MsaaSamples::k4X ? 2 : 1;
      uint32_t samples_y = edram.msaa_samples == xenos::MsaaSamples::k1X ? 1 : 2;
      uint32_t tile_width = xenos::kEdramTileWidthSamples / samples_x /
                            (edram.format_is_64bpp ? 2 : 1);
      uint32_t tile_height = xenos::kEdramTileHeightSamples / samples_y;
      uint32_t pitch_tiles = std::max(uint32_t(edram.pitch_tiles), uint32_t(1));
      uint32_t delta = (edram.base_tiles - info.color_original_base) & (xenos::kEdramTileCount - 1);
      ResolvedRegion region;
      region.target = source;
      region.x = ((delta % pitch_tiles) * tile_width +
                  info.coordinate_info.edram_offset_x_div_8 * 8) *
                 scale_x;
      region.y = ((delta / pitch_tiles) * tile_height +
                  info.coordinate_info.edram_offset_y_div_8 * 8) *
                 scale_y;
      region.width = info.coordinate_info.width_div_8 * 8 * scale_x;
      region.height = info.height_div_8 * 8 * scale_y;
      State().resolved_regions[info.copy_dest_base] = region;
    }
  }

  // Clears (whole render target; partial clears are not handled yet).
  bool clear_color = info.rb_copy_control.color_clear_enable;
  bool clear_depth = info.rb_copy_control.depth_clear_enable;
  if (clear_color || clear_depth) {
    RenderPassTargets clear_targets;
    std::array<float, 4> color_value = {};
    RenderTarget* color_target =
        clear_color ? render_target_cache_->FindLastUsed(info.color_original_base, false) : nullptr;
    RenderTarget* depth_target =
        clear_depth ? render_target_cache_->FindLastUsed(info.depth_original_base, true) : nullptr;
    frame_recorder_->EndRenderPass();
    if (color_target) {
      uint32_t value = info.rb_color_clear;
      for (uint32_t i = 0; i < 4; ++i) {
        color_value[i] = float((value >> (8 * i)) & 0xFF) / 255.0f;
      }
      wgpu::RenderPassColorAttachment attachment;
      attachment.view = color_target->view;
      attachment.loadOp = wgpu::LoadOp::Clear;
      attachment.storeOp = wgpu::StoreOp::Store;
      attachment.clearValue = {color_value[0], color_value[1], color_value[2], color_value[3]};
      wgpu::RenderPassDescriptor desc;
      desc.label = "guest color clear";
      desc.colorAttachmentCount = 1;
      desc.colorAttachments = &attachment;
      frame_recorder_->encoder().BeginRenderPass(&desc).End();
      color_target->initialized = true;
    }
    if (depth_target) {
      uint32_t depth24 = info.rb_depth_clear >> 8;
      bool depth_is_float =
          depth_target->key.format == uint32_t(xenos::DepthRenderTargetFormat::kD24FS8);
      float depth = depth_is_float
                        ? xenos::Float20e4To32(depth24)
                        : float(depth24) * (1.0f / float(0xFFFFFF));
      wgpu::RenderPassDepthStencilAttachment attachment;
      attachment.view = depth_target->view;
      attachment.depthLoadOp = wgpu::LoadOp::Clear;
      attachment.depthStoreOp = wgpu::StoreOp::Store;
      attachment.depthClearValue = std::clamp(depth, 0.0f, 1.0f);
      if (RenderTargetCache::HostDepthFormatHasStencil(depth_target->host_format)) {
        attachment.stencilLoadOp = wgpu::LoadOp::Clear;
        attachment.stencilStoreOp = wgpu::StoreOp::Store;
        attachment.stencilClearValue = info.rb_depth_clear & 0xFF;
      }
      wgpu::RenderPassDescriptor desc;
      desc.label = "guest depth clear";
      desc.depthStencilAttachment = &attachment;
      frame_recorder_->encoder().BeginRenderPass(&desc).End();
      depth_target->initialized = true;
    }
  }
  return true;
}

void WebGpuBackend::Swap(const SwapSnapshot& swap) {
  if (IsTracedFrame(swap.frame_index)) {
    REXGPU_INFO("webgpu trace: frame {} swap: frontbuffer 0x{:08X} {}x{}, display {}x{}",
                swap.frame_index, swap.frontbuffer_ptr, swap.frontbuffer_width,
                swap.frontbuffer_height, swap.display_width, swap.display_height);
  }
  DumpSwap(swap);
  ResolvedRegion region;
  {
    std::lock_guard lock(provider_.device_mutex());
    // Everything drawn this frame goes to the GPU before the output refresh.
    frame_recorder_->Submit();
    primitive_processor_->EndFrame();
    // Bind groups keep their views alive; start over every frame.
    State().texture_bind_groups.clear();
    auto it = State().resolved_regions.find(swap.frontbuffer_ptr & 0x1FFFFFFF);
    if (it == State().resolved_regions.end()) {
      it = State().resolved_regions.find(swap.frontbuffer_ptr);
    }
    if (it != State().resolved_regions.end()) {
      region = it->second;
    }
  }
  if (!presenter_) {
    return;
  }
  uint32_t output_width = region.target ? region.width : swap.frontbuffer_width;
  uint32_t output_height = region.target ? region.height : swap.frontbuffer_height;
  if (!output_width || !output_height) {
    output_width = swap.display_width;
    output_height = swap.display_height;
  }
  // Never hold the device mutex here: the presenter takes it itself, after
  // its own locks (lock order in webgpu_provider.h).
  presenter_->RefreshGuestOutput(
      output_width, output_height, swap.display_width, swap.display_height,
      [this, region, &swap](ui::Presenter::GuestOutputRefreshContext& context) -> bool {
        auto& webgpu_context =
            static_cast<ui::webgpu::WebGpuPresenter::WebGpuGuestOutputRefreshContext&>(context);
        context.SetIs8bpc(true);
        std::lock_guard lock(provider_.device_mutex());
        wgpu::RenderPassColorAttachment color;
        color.view = webgpu_context.view();
        color.loadOp = wgpu::LoadOp::Clear;
        color.storeOp = wgpu::StoreOp::Store;
        color.clearValue = {0.10, 0.12, 0.18, 1.0};
        wgpu::RenderPassDescriptor pass_desc;
        pass_desc.label = "guest output";
        pass_desc.colorAttachmentCount = 1;
        pass_desc.colorAttachments = &color;
        wgpu::CommandEncoder encoder = provider_.device().CreateCommandEncoder();
        wgpu::RenderPassEncoder pass = encoder.BeginRenderPass(&pass_desc);
        if (region.target && region.target->sample_count == 1) {
          BlitResources& blit = State().blit;
          // The 256-entry table for 8-bit frontbuffers, PWL for 10-bit ones
          // (as in the Vulkan backend).
          xenos::ColorRenderTargetFormat format =
              xenos::ColorRenderTargetFormat(region.target->key.format);
          bool pwl = format == xenos::ColorRenderTargetFormat::k_2_10_10_10 ||
                     format == xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10;
          const uint32_t* ramp = pwl ? swap.gamma_ramp_pwl : swap.gamma_ramp_table;
          uint32_t gamma_mode = ramp ? (pwl ? 2u : 1u) : 0u;
          if (ramp) {
            provider_.queue().WriteBuffer(blit.gamma_ramp, 0, ramp,
                                          (pwl ? 128 * 3 : 256) * sizeof(uint32_t));
          }
          int32_t constants[4] = {int32_t(region.x), int32_t(region.y), int32_t(gamma_mode), 0};
          provider_.queue().WriteBuffer(blit.constants, 0, constants, sizeof(constants));
          std::array<wgpu::BindGroupEntry, 3> entries;
          entries[0].binding = 0;
          entries[0].textureView = region.target->view;
          entries[1].binding = 1;
          entries[1].buffer = blit.constants;
          entries[2].binding = 2;
          entries[2].buffer = blit.gamma_ramp;
          wgpu::BindGroupDescriptor bind_group_desc;
          bind_group_desc.layout = blit.layout;
          bind_group_desc.entryCount = entries.size();
          bind_group_desc.entries = entries.data();
          pass.SetPipeline(blit.pipeline);
          pass.SetBindGroup(0, provider_.device().CreateBindGroup(&bind_group_desc));
          pass.Draw(3);
        }
        pass.End();
        wgpu::CommandBuffer commands = encoder.Finish();
        provider_.queue().Submit(1, &commands);
        return true;
      });

  std::lock_guard lock(provider_.device_mutex());
  provider_.ProcessEvents();
}

}  // namespace rex::graphics::webgpu
