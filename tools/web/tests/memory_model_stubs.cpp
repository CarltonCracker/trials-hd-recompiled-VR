// Test-only stand-ins for the parts of the SDK's POSIX layer that the web
// threading port (W-L15) has not replaced yet. Not linked into the game.

#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>

#include <pthread.h>

#include <rex/platform/env.h>
#include <rex/thread.h>

namespace rex::platform::env {
std::optional<std::string> get(std::string_view name) {
  const char* value = std::getenv(std::string(name).c_str());
  if (!value) {
    return std::nullopt;
  }
  return std::string(value);
}
}  // namespace rex::platform::env

namespace rex::thread {
uint32_t current_thread_id() {
  return uint32_t(reinterpret_cast<uintptr_t>(pthread_self()));
}
}  // namespace rex::thread
