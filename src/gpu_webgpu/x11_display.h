// rexgpu-webgpu - Xlib connection for window surfaces (Linux).

#pragma once

namespace rex::ui::webgpu {

// The process-wide Xlib Display* for $DISPLAY, or null if it can't be opened.
void* GetXlibDisplay();

}  // namespace rex::ui::webgpu
