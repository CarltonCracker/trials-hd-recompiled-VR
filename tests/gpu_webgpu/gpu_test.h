// Minimal Dawn fixture for synthetic compute tests (no window or game).
#pragma once

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include <dawn/native/DawnNative.h>

#define GPU_CHECK(condition) do { if (!(condition)) { \
  std::fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #condition); \
  std::exit(1); } } while (false)

struct GpuTest {
  static wgpu::InstanceDescriptor InstanceDescriptor() {
    static constexpr auto feature = wgpu::InstanceFeatureName::TimedWaitAny;
    wgpu::InstanceDescriptor desc;
    desc.requiredFeatureCount = 1;
    desc.requiredFeatures = &feature;
    return desc;
  }
  wgpu::InstanceDescriptor desc = InstanceDescriptor();
  dawn::native::Instance native{&desc};
  wgpu::Instance instance{native.Get()};
  std::atomic<unsigned> errors{0};
  wgpu::Device device;

  explicit GpuTest(unsigned index) {
    wgpu::RequestAdapterOptions options;
    options.backendType = wgpu::BackendType::Vulkan;
    auto adapters = native.EnumerateAdapters(&options);
    GPU_CHECK(index < adapters.size());
    wgpu::Adapter adapter(adapters[index].Get());
    wgpu::AdapterInfo info;
    adapter.GetInfo(&info);
    std::printf("adapter %u: %.*s\n", index, int(info.device.length), info.device.data);
    wgpu::DeviceDescriptor device_desc;
    device_desc.SetUncapturedErrorCallback(
        [](const wgpu::Device&, wgpu::ErrorType, wgpu::StringView message, GpuTest* test) {
          ++test->errors;
          std::fprintf(stderr, "Dawn error: %.*s\n", int(message.length), message.data);
        }, this);
    Wait(adapter.RequestDevice(
        &device_desc, wgpu::CallbackMode::WaitAnyOnly,
        [this](wgpu::RequestDeviceStatus status, wgpu::Device result, wgpu::StringView) {
          GPU_CHECK(status == wgpu::RequestDeviceStatus::Success);
          device = std::move(result);
        }));
  }

  void Wait(wgpu::Future future) {
    GPU_CHECK(instance.WaitAny(future, UINT64_MAX) == wgpu::WaitStatus::Success);
  }
  wgpu::ShaderModule Module(const std::string& source) {
    wgpu::ShaderSourceWGSL wgsl;
    wgsl.code = source.c_str();
    wgpu::ShaderModuleDescriptor module_desc;
    module_desc.nextInChain = &wgsl;
    auto module = device.CreateShaderModule(&module_desc);
    Wait(module.GetCompilationInfo(
        wgpu::CallbackMode::WaitAnyOnly,
        [](wgpu::CompilationInfoRequestStatus status, const wgpu::CompilationInfo* info) {
          GPU_CHECK(status == wgpu::CompilationInfoRequestStatus::Success);
          for (size_t i = 0; i < info->messageCount; ++i) {
            if (info->messages[i].type == wgpu::CompilationMessageType::Error) {
              std::fprintf(stderr, "%.*s\n", int(info->messages[i].message.length),
                           info->messages[i].message.data);
              GPU_CHECK(false);
            }
          }
        }));
    return module;
  }
  wgpu::Buffer Buffer(uint64_t size, wgpu::BufferUsage usage, const void* data = nullptr) {
    wgpu::BufferDescriptor buffer_desc;
    buffer_desc.size = size;
    buffer_desc.usage = usage;
    auto buffer = device.CreateBuffer(&buffer_desc);
    if (data) {
      device.GetQueue().WriteBuffer(buffer, 0, data, size);
    }
    return buffer;
  }
  std::vector<uint8_t> Read(const wgpu::Buffer& buffer, size_t size) {
    auto readback = Buffer(size, wgpu::BufferUsage::CopyDst | wgpu::BufferUsage::MapRead);
    auto encoder = device.CreateCommandEncoder();
    encoder.CopyBufferToBuffer(buffer, 0, readback, 0, size);
    auto commands = encoder.Finish();
    device.GetQueue().Submit(1, &commands);
    Wait(readback.MapAsync(wgpu::MapMode::Read, 0, size, wgpu::CallbackMode::WaitAnyOnly,
                          [](wgpu::MapAsyncStatus status, wgpu::StringView) {
                            GPU_CHECK(status == wgpu::MapAsyncStatus::Success);
                          }));
    const auto* data = static_cast<const uint8_t*>(readback.GetConstMappedRange(0, size));
    GPU_CHECK(data);
    std::vector<uint8_t> result(data, data + size);
    readback.Unmap();
    GPU_CHECK(errors == 0);
    return result;
  }
};
