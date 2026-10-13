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

#include <rex/audio/sdl/sdl_audio_system.h>

namespace trials_hd {

class FallbackAudioSystem : public rex::audio::sdl::SDLAudioSystem {
 public:
  using SDLAudioSystem::SDLAudioSystem;

  static std::unique_ptr<rex::audio::AudioSystem> Create(
      rex::runtime::FunctionDispatcher* function_dispatcher);

  rex::X_STATUS CreateDriver(size_t index, rex::thread::Semaphore* semaphore,
                             rex::audio::AudioDriver** out_driver) override;
  void DestroyDriver(rex::audio::AudioDriver* driver) override;
};

}  // namespace trials_hd
