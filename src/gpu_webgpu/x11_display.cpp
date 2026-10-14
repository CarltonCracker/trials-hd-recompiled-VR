// rexgpu-webgpu - Xlib connection for window surfaces. Kept apart from the
// WebGPU code because Xlib's macros (Status, None, Bool, ...) clash with it.

#include "x11_display.h"

#include <X11/Xlib.h>

namespace rex::ui::webgpu {

void* GetXlibDisplay() {
  // Dawn takes X11 windows only as Xlib (Display*, Window), while the SDK's
  // surface is XCB. Window IDs are server-side, so a separate connection to
  // the same server works. libX11 1.8+ makes it thread-safe by default; it
  // stays open for the life of the process.
  static Display* display = XOpenDisplay(nullptr);
  return display;
}

}  // namespace rex::ui::webgpu
