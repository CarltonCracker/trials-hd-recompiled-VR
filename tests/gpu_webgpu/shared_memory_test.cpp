// Synthetic guest bytes only: no game, shaders or captures are needed.
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <dawn/native/DawnNative.h>
#include <rex/logging.h>
#include <rex/system/xmemory.h>

#include "guest_memory_mirror.h"

#define CHECK(condition) do { if (!(condition)) { \
  std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
  std::exit(1); } } while (false)

namespace {

void Wait(const wgpu::Instance& instance, wgpu::Future future) {
  CHECK(instance.WaitAny(future, UINT64_MAX) == wgpu::WaitStatus::Success);
}

wgpu::Device CreateDevice(const wgpu::Instance& instance, const wgpu::Adapter& adapter,
                          const wgpu::Limits* limits, std::atomic<unsigned>& errors) {
  wgpu::DeviceDescriptor desc;
  desc.requiredLimits = limits;
  desc.SetUncapturedErrorCallback(
      [](const wgpu::Device&, wgpu::ErrorType, wgpu::StringView message, std::atomic<unsigned>* errors) {
        ++*errors;
        std::fprintf(stderr, "Dawn error: %.*s\n", int(message.length), message.data);
      }, &errors);
  wgpu::Device device;
  Wait(instance, adapter.RequestDevice(
      &desc, wgpu::CallbackMode::WaitAnyOnly,
      [&device](wgpu::RequestDeviceStatus status, wgpu::Device result, wgpu::StringView message) {
        if (status != wgpu::RequestDeviceStatus::Success) {
          std::fprintf(stderr, "RequestDevice failed: %.*s\n", int(message.length), message.data);
        }
        CHECK(status == wgpu::RequestDeviceStatus::Success);
        device = std::move(result);
      }));
  return device;
}

wgpu::Buffer ReadbackBuffer(const wgpu::Device& device, uint64_t size) {
  wgpu::BufferDescriptor desc;
  desc.size = size;
  desc.usage = wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead;
  return device.CreateBuffer(&desc);
}

std::vector<uint8_t> Map(const wgpu::Instance& instance, const wgpu::Buffer& buffer, size_t size) {
  Wait(instance, buffer.MapAsync(
      wgpu::MapMode::Read, 0, size, wgpu::CallbackMode::WaitAnyOnly,
      [](wgpu::MapAsyncStatus status, wgpu::StringView) {
        CHECK(status == wgpu::MapAsyncStatus::Success);
      }));
  const auto* data = static_cast<const uint8_t*>(buffer.GetConstMappedRange(0, size));
  CHECK(data);
  std::vector<uint8_t> result(data, data + size);
  buffer.Unmap();
  return result;
}

std::vector<uint8_t> Read(const wgpu::Instance& instance, const wgpu::Device& device,
                          const wgpu::Buffer& source, uint32_t offset, size_t size) {
  auto result = ReadbackBuffer(device, size);
  auto encoder = device.CreateCommandEncoder();
  encoder.CopyBufferToBuffer(source, offset, result, 0, size);
  auto commands = encoder.Finish();
  device.GetQueue().Submit(1, &commands);
  return Map(instance, result, size);
}

rex::graphics::webgpu::GuestMemoryUpload MappedUpload(
    const wgpu::Device& device, const wgpu::CommandEncoder& encoder, uint64_t size) {
  rex::graphics::webgpu::GuestMemoryUpload upload;
  wgpu::BufferDescriptor desc;
  desc.size = size;
  desc.usage = wgpu::BufferUsage::CopySrc;
  desc.mappedAtCreation = true;
  upload.source = device.CreateBuffer(&desc);
  upload.data = upload.source.GetMappedRange(0, size);
  upload.encoder = encoder;
  return upload;
}

}  // namespace

int main(int argc, char** argv) {
  rex::InitLogging();
  // Pass an adapter index to test either physical GPU independently.
  const unsigned adapter_index = argc > 1 ? unsigned(std::strtoul(argv[1], nullptr, 10)) : 0;
  constexpr std::array features = {wgpu::InstanceFeatureName::TimedWaitAny,
                                   wgpu::InstanceFeatureName::MultipleDevicesPerAdapter};
  wgpu::InstanceDescriptor desc;
  desc.requiredFeatureCount = features.size();
  desc.requiredFeatures = features.data();
  dawn::native::Instance native(&desc);
  wgpu::Instance instance(native.Get());
  wgpu::RequestAdapterOptions options;
  options.backendType = wgpu::BackendType::Vulkan;
  auto adapters = native.EnumerateAdapters(&options);
  CHECK(adapter_index < adapters.size());
  wgpu::Adapter adapter(adapters[adapter_index].Get());
  wgpu::AdapterInfo info;
  CHECK(adapter.GetInfo(&info) == wgpu::Status::Success);
  std::printf("adapter %u: %.*s\n", adapter_index, int(info.device.length), info.device.data);
  wgpu::Limits limits;
  CHECK(adapter.GetLimits(&limits) == wgpu::Status::Success);
  std::printf("buffer/binding limits: %llu / %llu MiB\n",
              static_cast<unsigned long long>(limits.maxBufferSize >> 20),
              static_cast<unsigned long long>(limits.maxStorageBufferBindingSize >> 20));

  rex::memory::Memory memory;
  CHECK(memory.Initialize());
  using rex::graphics::webgpu::WebGpuSharedMemory;
  std::atomic<unsigned> errors{0};
  {
    auto limited_device = CreateDevice(instance, adapter, nullptr, errors);
    WebGpuSharedMemory limited(memory, instance, limited_device,
                               [](uint64_t) { return rex::graphics::webgpu::GuestMemoryUpload{}; });
    CHECK(!limited.Initialize());  // default 128 MiB storage binding is insufficient
    CHECK(!limited.buffer());
  }

  auto device = CreateDevice(instance, adapter, &limits, errors);
  const uint32_t page_size = uint32_t(rex::memory::page_size());
  const uint32_t size = page_size * 4;
  const uint32_t address = memory.SystemHeapAlloc(size, page_size, rex::memory::kSystemHeapPhysical);
  CHECK(address);
  const uint32_t physical = memory.GetPhysicalAddress(address);
  CHECK(physical != UINT32_MAX);
  auto* guest = memory.TranslateVirtual(address);
  std::vector<uint8_t> expected(size);
  for (size_t i = 0; i < size; ++i) {
    expected[i] = uint8_t(i * 37 + 11);
  }
  std::memcpy(guest, expected.data(), size);

  for (bool use_ring : {false, true}) {
    unsigned uploads = 0;
    unsigned submissions = 0;
    bool allow_upload = true;
    wgpu::CommandEncoder pending;
    wgpu::BufferDescriptor ring_desc;
    ring_desc.size = size * 4;
    ring_desc.usage = wgpu::BufferUsage::CopySrc | wgpu::BufferUsage::CopyDst;
    auto ring = device.CreateBuffer(&ring_desc);
    std::vector<uint8_t> ring_data;
    auto submit = [&] {
      CHECK(pending);
      if (!ring_data.empty()) {
        device.GetQueue().WriteBuffer(ring, 0, ring_data.data(), ring_data.size());
        ring_data.clear();
      }
      auto commands = pending.Finish();
      device.GetQueue().Submit(1, &commands);
      pending = nullptr;
      ++submissions;
    };
    WebGpuSharedMemory mirror(memory, instance, device, [&](uint64_t length) {
      ++uploads;
      if (!allow_upload) {
        return rex::graphics::webgpu::GuestMemoryUpload{};
      }
      if (!pending) { pending = device.CreateCommandEncoder(); }
      if (!use_ring) { return MappedUpload(device, pending, length); }
      rex::graphics::webgpu::GuestMemoryUpload upload;
      upload.offset = ring_data.size();
      CHECK(upload.offset + length <= ring.GetSize());
      ring_data.resize(upload.offset + length);
      upload.data = ring_data.data() + upload.offset;
      upload.source = ring;
      upload.encoder = pending;
      return upload;
    });
    auto read = [&](uint32_t offset, size_t length) {
      if (pending) { submit(); }
      return Read(instance, device, mirror.buffer(), offset, length);
    };
    CHECK(mirror.Initialize());
    CHECK(mirror.Initialize());  // doesn't register duplicate guest watches
    CHECK(mirror.buffer().GetSize() == WebGpuSharedMemory::kBufferSize);
    CHECK(mirror.RequestRange(physical + 3, size - 6));
    CHECK(uploads == 1);
    CHECK(submissions == 0);  // uploads only encode copies, never submit
    CHECK(read(physical, size) == expected);
    CHECK(mirror.RequestRange(physical, size));
    CHECK(uploads == 1);  // clean pages require neither encoder access nor upload
    CHECK(!mirror.RequestRange(WebGpuSharedMemory::kBufferSize, 1));
    CHECK(!mirror.RequestRange(WebGpuSharedMemory::kBufferSize - 4, 8));
    CHECK(mirror.RequestRange(WebGpuSharedMemory::kBufferSize, 0));

    // An earlier encoded consumer must see the old bytes, not the later upload.
    auto old = ReadbackBuffer(device, size);
    pending = device.CreateCommandEncoder();
    pending.CopyBufferToBuffer(mirror.buffer(), physical, old, 0, size);
    guest[7] ^= 0xFF;  // actual guest-alias fault, not manual invalidation
    auto previous = expected;
    expected[7] ^= 0xFF;
    CHECK(mirror.RequestRange(physical, size));
    CHECK(uploads == 2);
    auto middle = ReadbackBuffer(device, size);
    pending.CopyBufferToBuffer(mirror.buffer(), physical, middle, 0, size);
    auto intermediate = expected;
    guest[9] ^= 0xAA;
    expected[9] ^= 0xAA;
    CHECK(mirror.RequestRange(physical, size));
    CHECK(uploads == 3);
    CHECK(submissions == 1);  // two source slices, two old consumers, no submit
    submit();
    CHECK(Map(instance, old, size) == previous);
    CHECK(Map(instance, middle, size) == intermediate);
    CHECK(read(physical, size) == expected);

    // Preserve partial-page GPU writes on a clean request.
    const std::array<uint8_t, 4> gpu_bytes = {1, 2, 3, 4};
    device.GetQueue().WriteBuffer(mirror.buffer(), physical + 16, gpu_bytes.data(), gpu_bytes.size());
    mirror.RangeWrittenByGpu(physical + 16, uint32_t(gpu_bytes.size()));
    CHECK(mirror.RequestRange(physical, size));
    CHECK(uploads == 3);
    auto gpu_expected = expected;
    std::memcpy(gpu_expected.data() + 16, gpu_bytes.data(), gpu_bytes.size());
    CHECK(read(physical, size) == gpu_expected);
    mirror.ClearCache();
    CHECK(mirror.RequestRange(physical, size));
    CHECK(uploads == 4);
    CHECK(read(physical, size) == gpu_expected);

    // A CPU reuse of that GPU-written page must restore the guest's bytes.
    guest[24] ^= 0x55;
    expected[24] ^= 0x55;
    allow_upload = false;
    CHECK(!mirror.RequestRange(physical, size));
    allow_upload = true;
    CHECK(mirror.RequestRange(physical, size));
    CHECK(uploads == 6);
    CHECK(read(physical, size) == expected);

    mirror.InvalidateAllPages();
    const std::pair<uint32_t, uint32_t> ranges[] = {
        {physical + page_size * 3, page_size}, {physical, page_size},
        {physical + 16, 32},  // overlapping requests, out of order
    };
    CHECK(mirror.RequestRanges(ranges, std::size(ranges)));
    CHECK(uploads == 7);  // one staging allocation for the whole dirty request
    CHECK(read(physical, size) == expected);
  }
  // Destruction unregisters callbacks; writing/reinitializing must remain safe.
  guest[0] = 42;
  {
    auto encoder = device.CreateCommandEncoder();
    WebGpuSharedMemory mirror(memory, instance, device,
                               [&](uint64_t size) { return MappedUpload(device, encoder, size); });
    CHECK(mirror.Initialize());
    CHECK(mirror.RequestRange(physical, page_size));
    auto commands = encoder.Finish();
    device.GetQueue().Submit(1, &commands);
    CHECK(Read(instance, device, mirror.buffer(), physical, 4)[0] == 42);
  }
  memory.SystemHeapFree(address);
  instance.ProcessEvents();
  CHECK(errors == 0);
  std::puts("PASS: uploads, caching, alias faults, queue ordering, GPU writes, retry and teardown");
}
