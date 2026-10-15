// W-S03: synthetic code only; no generated game code or game data.
#undef NDEBUG
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pthread.h>

#include <emscripten.h>

namespace {
constexpr size_t kMiB = 1024 * 1024;
constexpr size_t kWindow = 3072 * kMiB;
constexpr size_t kTouch = 16 * kMiB;
constexpr int kThreads = 40;
pthread_barrier_t ready;

struct Work {
  unsigned char* data;
  int index;
};

void* Touch(void* argument) {
  const auto& work = *static_cast<Work*>(argument);
  int status = pthread_barrier_wait(&ready);
  assert(status == 0 || status == PTHREAD_BARRIER_SERIAL_THREAD);
  std::memset(work.data, work.index + 1, kTouch);
  for (size_t offset = 0; offset < kTouch; offset += 4096) {
    assert(work.data[offset] == work.index + 1);
  }
  assert(work.data[kTouch - 1] == work.index + 1);
  return nullptr;
}
}  // namespace

int main() {
  static_assert(sizeof(void*) == 8, "Build this spike for wasm64");
  std::puts("THREAD_MEMORY64_MAIN");
  double start = emscripten_get_now();
  auto* window = static_cast<unsigned char*>(std::aligned_alloc(65536, kWindow));
  assert(window && "3 GiB window allocation failed");
  double allocated = emscripten_get_now();
  assert(pthread_barrier_init(&ready, nullptr, kThreads + 1) == 0);
  pthread_t threads[kThreads];
  Work work[kThreads];
  for (int i = 0; i < kThreads; ++i) {
    size_t offset = ((kWindow - kTouch) * i / (kThreads - 1)) & ~size_t(65535);
    work[i] = {window + offset, i};
    assert(pthread_create(&threads[i], nullptr, Touch, &work[i]) == 0);
  }
  double created = emscripten_get_now();
  int status = pthread_barrier_wait(&ready);
  assert(status == 0 || status == PTHREAD_BARRIER_SERIAL_THREAD);
  for (auto& thread : threads) assert(pthread_join(thread, nullptr) == 0);
  assert(pthread_barrier_destroy(&ready) == 0);
  double end = emscripten_get_now();
  std::printf("THREAD_MEMORY64_RESULT {\"threads\":%d,\"pointerBytes\":%zu,"
              "\"windowMiB\":3072,\"touchedMiB\":640,\"allocationMs\":%.3f,"
              "\"createMs\":%.3f,\"touchJoinMs\":%.3f,\"totalMs\":%.3f,\"verified\":true}\n",
              kThreads, sizeof(void*), allocated - start, created - allocated,
              end - created, end - start);
  // Keep the window resident until the browser closes so CDP can sample RSS.
  return 0;
}
