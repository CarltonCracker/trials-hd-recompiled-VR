// trials_hd - audio backend that degrades to silent output.

#include "fallback_audio.h"

#include <atomic>
#include <chrono>
#include <thread>

#include <rex/audio/audio_driver.h>
#include <rex/logging.h>
#include <rex/thread.h>

namespace trials_hd {

using rex::X_STATUS;

namespace {

// Consumes submitted frames at the console's playback rate and releases the
// client semaphore once per frame, the same contract SDLAudioDriver keeps from
// its device callback, so the game's audio thread runs at normal speed.
class NullAudioDriver : public rex::audio::AudioDriver {
 public:
  NullAudioDriver(rex::memory::Memory* memory, rex::thread::Semaphore* semaphore)
      : AudioDriver(memory), semaphore_(semaphore), worker_([this] { Run(); }) {}

  ~NullAudioDriver() override {
    running_ = false;
    worker_.join();
  }

  void SubmitFrame(uint32_t /*samples_ptr*/) override { pending_.fetch_add(1); }

 private:
  // 256 samples per channel per frame at 48 kHz.
  static constexpr auto kFramePeriod = std::chrono::microseconds(256 * 1'000'000 / 48'000);

  void Run() {
    auto next = std::chrono::steady_clock::now();
    while (running_) {
      next += kFramePeriod;
      std::this_thread::sleep_until(next);
      uint32_t pending = pending_.load();
      while (pending && !pending_.compare_exchange_weak(pending, pending - 1)) {
      }
      if (pending) {
        semaphore_->Release(1, nullptr);
      }
    }
  }

  rex::thread::Semaphore* semaphore_;
  std::atomic<uint32_t> pending_{0};
  std::atomic<bool> running_{true};
  std::thread worker_;
};

}  // namespace

std::unique_ptr<rex::audio::AudioSystem> FallbackAudioSystem::Create(
    rex::runtime::FunctionDispatcher* function_dispatcher) {
  return std::make_unique<FallbackAudioSystem>(function_dispatcher);
}

X_STATUS FallbackAudioSystem::CreateDriver(size_t index, rex::thread::Semaphore* semaphore,
                                           rex::audio::AudioDriver** out_driver) {
#if defined(__EMSCRIPTEN__)
  (void)index;
#else
  if (SDLAudioSystem::CreateDriver(index, semaphore, out_driver) == X_STATUS_SUCCESS) {
    return X_STATUS_SUCCESS;
  }
  REXLOG_WARN("No audio output device available; continuing with silent audio");
#endif
  *out_driver = new NullAudioDriver(memory_, semaphore);
  return X_STATUS_SUCCESS;
}

void FallbackAudioSystem::DestroyDriver(rex::audio::AudioDriver* driver) {
  if (auto* null_driver = dynamic_cast<NullAudioDriver*>(driver)) {
    delete null_driver;
    return;
  }
#if !defined(__EMSCRIPTEN__)
  SDLAudioSystem::DestroyDriver(driver);
#endif
}

}  // namespace trials_hd
