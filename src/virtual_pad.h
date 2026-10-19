// trials_hd - scriptable virtual gamepad for headless testing.

#pragma once

#include <functional>
#include <string>

namespace trials_hd {

// When the TRIALS_HD_VIRTUAL_PAD environment variable names a FIFO, attaches
// an SDL virtual gamepad and drives it from text commands read from the FIFO:
//   btn <a|b|x|y|back|guide|start|ls|rs|lb|rb|up|down|left|right> <0|1>
//   axis <lx|ly|rx|ry|lt|rt> <-32768..32767>
//   capture <path>   (passed to on_capture: save the current guest frame)
//   set <cvar> <v>   (change a cvar at runtime, e.g. "set vsync false")
// The pad goes through the same SDL controller path as a physical one, so test
// runs exercise real controller handling. Does nothing when the variable is
// unset, or after the first call.
void StartVirtualPadFromEnvironment(std::function<void(const std::string&)> on_capture);

}  // namespace trials_hd
