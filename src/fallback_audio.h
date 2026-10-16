// trials_hd - audio backend that degrades to silent output.
//
// When SDL cannot open an output device (headless servers, no sound card),
// the stock SDL backend fails XAudioRegisterRenderDriverClient. Trials HD
// does not handle that failure: its audio thread later calls a pure virtual
// on the half-initialized engine and the CRT aborts (R6025). This backend
// keeps SDL output when a device is available and otherwise hands the game a
// driver that drops frames at the real playback rate.

#pragma once

#include <memory>

#if defined(__EMSCRIPTEN__)
#include <rex/audio/audio_system.h>
#else
#include <rex/audio/sdl/sdl_audio_system.h>
#endif

namespace trials_hd {

#if defined(__EMSCRIPTEN__)
// Web: no SDL audio; the driver writes to the page's AudioWorklet (W-L12).
class FallbackAudioSystem : public rex::audio::AudioSystem {
 public:
  explicit FallbackAudioSystem(rex::runtime::FunctionDispatcher* function_dispatcher)
      : AudioSystem(function_dispatcher) {}

  static std::unique_ptr<rex::audio::AudioSystem> Create(
      rex::runtime::FunctionDispatcher* function_dispatcher);

  rex::X_STATUS CreateDriver(size_t index, rex::thread::Semaphore* semaphore,
                             rex::audio::AudioDriver** out_driver) override;
  void DestroyDriver(rex::audio::AudioDriver* driver) override;
};
#else
class FallbackAudioSystem : public rex::audio::sdl::SDLAudioSystem {
 public:
  using SDLAudioSystem::SDLAudioSystem;

  static std::unique_ptr<rex::audio::AudioSystem> Create(
      rex::runtime::FunctionDispatcher* function_dispatcher);

  rex::X_STATUS CreateDriver(size_t index, rex::thread::Semaphore* semaphore,
                             rex::audio::AudioDriver** out_driver) override;
  void DestroyDriver(rex::audio::AudioDriver* driver) override;
};
#endif

}  // namespace trials_hd
