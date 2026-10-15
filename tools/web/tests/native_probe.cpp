#include <dlfcn.h>
#include <pthread.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace {

struct ThreadEntry {
  int id;
  pid_t tid;
  pthread_t pt;
  void* stackaddr;
  size_t stacksize;
  size_t max_touched_depth;
  size_t resident_bytes;
  char name[64];
  bool is_guest;
  bool is_main;
  bool finished;
};

#include <errno.h>

static bool g_is_trials_hd = false;

// Global state
std::mutex g_threads_mutex;
std::vector<ThreadEntry> g_threads;
std::atomic<int> g_next_thread_id{0};
std::atomic<int> g_total_threads_created{0};
std::atomic<int> g_active_threads{0};
std::atomic<int> g_peak_concurrent_threads{0};

std::atomic<uint64_t> g_fiber_calls{0};
std::atomic<uint64_t> g_mmio_faults_7f{0};
std::atomic<uint64_t> g_total_swaps{0};
std::atomic<double> g_first_swap_ms{0.0};
std::atomic<bool> g_has_first_swap{false};

std::chrono::steady_clock::time_point g_start_time;
std::atomic<bool> g_sampler_running{false};
pthread_t g_sampler_thread;
std::atomic<bool> g_reported{false};

// Real function pointers
int (*real_pthread_create)(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*) = nullptr;
int (*real_pthread_setname_np)(pthread_t, const char*) = nullptr;
bool (*real_ExceptionCallback)(void*, void*) = nullptr;
bool (*real_RefreshGuestOutput)(void*, uint32_t, uint32_t, uint32_t, uint32_t, void*) = nullptr;
void* (*real_FiberCreate)(unsigned long, void (*)(void*), void*) = nullptr;
void (*real_RegisterFiber)(void*, unsigned int, const void*) = nullptr;
void* (*real_rexcrt_CreateFiber)(unsigned int, void*, void*) = nullptr;
void* (*real_rexcrt_ConvertThreadToFiber)(void*) = nullptr;
void (*real_rexcrt_SwitchToFiber)(void*) = nullptr;
void (*real_rexcrt_DeleteFiber)(void*) = nullptr;

struct WrapArgs {
  void* (*orig_start)(void*);
  void* orig_arg;
  int thread_id;
};

void SampleThreadStack(ThreadEntry& entry) {
  if (!entry.stackaddr || entry.stacksize == 0) return;
  size_t num_pages = entry.stacksize / 4096;
  if (num_pages == 0) return;

  std::vector<unsigned char> vec(num_pages);
  if (mincore(entry.stackaddr, entry.stacksize, vec.data()) == 0) {
    size_t lowest = num_pages;
    size_t resident = 0;
    for (size_t i = 0; i < num_pages; ++i) {
      if (vec[i] & 1) {
        resident++;
        if (i < lowest) lowest = i;
      }
    }
    if (lowest < num_pages) {
      size_t depth = (num_pages - lowest) * 4096;
      if (depth > entry.max_touched_depth) {
        entry.max_touched_depth = depth;
      }
    }
    entry.resident_bytes = resident * 4096;
  }
}

void* ThreadWrapper(void* arg) {
  WrapArgs* wrap = static_cast<WrapArgs*>(arg);
  void* (*orig_start)(void*) = wrap->orig_start;
  void* orig_arg = wrap->orig_arg;
  int thread_id = wrap->thread_id;
  delete wrap;

  pid_t tid = static_cast<pid_t>(syscall(SYS_gettid));
  pthread_t pt = pthread_self();

  pthread_attr_t attr;
  void* stackaddr = nullptr;
  size_t stacksize = 0;
  if (pthread_getattr_np(pt, &attr) == 0) {
    pthread_attr_getstack(&attr, &stackaddr, &stacksize);
    pthread_attr_destroy(&attr);
  }

  {
    std::lock_guard<std::mutex> lock(g_threads_mutex);
    for (auto& entry : g_threads) {
      if (entry.id == thread_id) {
        entry.tid = tid;
        entry.pt = pt;
        entry.stackaddr = stackaddr;
        entry.stacksize = stacksize;
        // 16 MiB host stack indicates guest execution thread
        if (stacksize >= 16 * 1024 * 1024) {
          entry.is_guest = true;
        }
        break;
      }
    }
  }

  void* result = orig_start(orig_arg);

  {
    std::lock_guard<std::mutex> lock(g_threads_mutex);
    for (auto& entry : g_threads) {
      if (entry.id == thread_id) {
        SampleThreadStack(entry);
        entry.finished = true;
        break;
      }
    }
    g_active_threads--;
  }

  return result;
}

void* SamplerThreadMain(void*) {
  while (g_sampler_running.load()) {
    usleep(150000);  // 150 ms
    std::lock_guard<std::mutex> lock(g_threads_mutex);
    for (auto& entry : g_threads) {
      if (!entry.finished) {
        SampleThreadStack(entry);
        // Refresh name from /proc/self/task/<tid>/comm if still unnamed
        if (entry.name[0] == '\0' && entry.tid > 0) {
          char path[64];
          snprintf(path, sizeof(path), "/proc/self/task/%d/comm", entry.tid);
          FILE* f = fopen(path, "r");
          if (f) {
            if (fgets(entry.name, sizeof(entry.name), f)) {
              size_t len = strlen(entry.name);
              if (len > 0 && entry.name[len - 1] == '\n') entry.name[len - 1] = '\0';
            }
            fclose(f);
            if (strstr(entry.name, "XThread") || strstr(entry.name, "Guest")) {
              entry.is_guest = true;
            }
          }
        }
      }
    }
  }
  return nullptr;
}

void GenerateReport() {
  if (g_reported.exchange(true)) return;

  // Stop sampler thread
  g_sampler_running.store(false);

  // Final sample
  {
    std::lock_guard<std::mutex> lock(g_threads_mutex);
    for (auto& entry : g_threads) {
      SampleThreadStack(entry);
      if (entry.name[0] == '\0' && entry.tid > 0) {
        char path[64];
        snprintf(path, sizeof(path), "/proc/self/task/%d/comm", entry.tid);
        FILE* f = fopen(path, "r");
        if (f) {
          if (fgets(entry.name, sizeof(entry.name), f)) {
            size_t len = strlen(entry.name);
            if (len > 0 && entry.name[len - 1] == '\n') entry.name[len - 1] = '\0';
          }
          fclose(f);
        }
      }
      if (strstr(entry.name, "XThread") || strstr(entry.name, "Guest")) {
        entry.is_guest = true;
      }
    }
  }

  size_t max_depth_all = 0;
  size_t max_depth_guest = 0;
  int guest_thread_count = 0;

  {
    std::lock_guard<std::mutex> lock(g_threads_mutex);
    for (const auto& entry : g_threads) {
      if (entry.max_touched_depth > max_depth_all) {
        max_depth_all = entry.max_touched_depth;
      }
      if (entry.is_guest) {
        guest_thread_count++;
        if (entry.max_touched_depth > max_depth_guest) {
          max_depth_guest = entry.max_touched_depth;
        }
      }
    }
  }

  fprintf(stderr, "\n======================================================================\n");
  fprintf(stderr, "                 TRIALS HD NATIVE PROBE REPORT (W-S06 / W-S07 / W-S08)\n");
  fprintf(stderr, "======================================================================\n");
  fprintf(stderr, "First guest swap:         %.2f ms (swaps observed: %lu)\n",
          g_first_swap_ms.load(), (unsigned long)g_total_swaps.load());
  fprintf(stderr, "Total threads created:    %d\n", g_total_threads_created.load());
  fprintf(stderr, "Peak concurrent threads:  %d\n", g_peak_concurrent_threads.load());
  fprintf(stderr, "Guest threads identified: %d\n", guest_thread_count);
  fprintf(stderr, "Max host stack depth:     all=%zu bytes (%.2f KiB), guest=%zu bytes (%.2f KiB)\n",
          max_depth_all, max_depth_all / 1024.0, max_depth_guest, max_depth_guest / 1024.0);
  fprintf(stderr, "Fiber calls observed:     %lu\n", (unsigned long)g_fiber_calls.load());
  fprintf(stderr, "MMIO faults in 0x7F...:   %lu (fatal abort if > 0)\n", (unsigned long)g_mmio_faults_7f.load());
  fprintf(stderr, "----------------------------------------------------------------------\n");
  fprintf(stderr, "%-4s %-8s %-20s %-10s %-12s %-12s %-6s\n",
          "ID", "TID", "NAME", "STACK_SIZE", "MAX_DEPTH", "RESIDENT", "GUEST");
  fprintf(stderr, "----------------------------------------------------------------------\n");

  {
    std::lock_guard<std::mutex> lock(g_threads_mutex);
    for (const auto& entry : g_threads) {
      fprintf(stderr, "%-4d %-8d %-20.20s %-10zu %-12zu %-12zu %-6s\n",
              entry.id, entry.tid, entry.name[0] ? entry.name : "<unnamed>",
              entry.stacksize, entry.max_touched_depth, entry.resident_bytes,
              entry.is_guest ? "YES" : "no");
    }
  }
  fprintf(stderr, "======================================================================\n\n");
  fflush(stderr);

  // Write JSON output if requested
  const char* json_path = getenv("PROBE_OUTPUT_JSON");
  if (!json_path || json_path[0] == '\0') {
    json_path = "out/web-spikes/probe_report.json";
  }

  std::ofstream out(json_path);
  if (out.is_open()) {
    out << "{\n";
    out << "  \"first_swap_ms\": " << g_first_swap_ms.load() << ",\n";
    out << "  \"total_swaps\": " << g_total_swaps.load() << ",\n";
    out << "  \"total_threads_created\": " << g_total_threads_created.load() << ",\n";
    out << "  \"peak_concurrent_threads\": " << g_peak_concurrent_threads.load() << ",\n";
    out << "  \"guest_thread_count\": " << guest_thread_count << ",\n";
    out << "  \"max_host_stack_depth_all_bytes\": " << max_depth_all << ",\n";
    out << "  \"max_host_stack_depth_all_kib\": " << (max_depth_all / 1024.0) << ",\n";
    out << "  \"max_host_stack_depth_guest_bytes\": " << max_depth_guest << ",\n";
    out << "  \"max_host_stack_depth_guest_kib\": " << (max_depth_guest / 1024.0) << ",\n";
    out << "  \"fiber_calls\": " << g_fiber_calls.load() << ",\n";
    out << "  \"mmio_faults_in_7f\": " << g_mmio_faults_7f.load() << ",\n";
    out << "  \"threads\": [\n";

    std::lock_guard<std::mutex> lock(g_threads_mutex);
    for (size_t i = 0; i < g_threads.size(); ++i) {
      const auto& t = g_threads[i];
      out << "    {\n";
      out << "      \"id\": " << t.id << ",\n";
      out << "      \"tid\": " << t.tid << ",\n";
      out << "      \"name\": \"" << (t.name[0] ? t.name : "<unnamed>") << "\",\n";
      out << "      \"stack_size_bytes\": " << t.stacksize << ",\n";
      out << "      \"max_depth_bytes\": " << t.max_touched_depth << ",\n";
      out << "      \"max_depth_kib\": " << (t.max_touched_depth / 1024.0) << ",\n";
      out << "      \"resident_bytes\": " << t.resident_bytes << ",\n";
      out << "      \"is_guest\": " << (t.is_guest ? "true" : "false") << "\n";
      out << "    }" << (i + 1 < g_threads.size() ? "," : "") << "\n";
    }
    out << "  ]\n";
    out << "}\n";
    out.close();
    fprintf(stderr, "[PROBE] JSON report written to %s\n", json_path);
  }
}

void SignalHandler(int signum) {
  if (!g_is_trials_hd) return;
  fprintf(stderr, "[PROBE] Signal %d caught in trials_hd, dumping report...\n", signum);
  GenerateReport();
  _exit(0);
}

}  // namespace

// Constructor: initialize symbols and main thread tracking
__attribute__((constructor)) static void ProbeInit() {
  real_pthread_create = reinterpret_cast<int (*)(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*)>(
      dlsym(RTLD_NEXT, "pthread_create"));
  real_pthread_setname_np = reinterpret_cast<int (*)(pthread_t, const char*)>(
      dlsym(RTLD_NEXT, "pthread_setname_np"));

  if (!program_invocation_short_name || strcmp(program_invocation_short_name, "trials_hd") != 0) {
    return;
  }
  g_is_trials_hd = true;
  g_start_time = std::chrono::steady_clock::now();

  real_ExceptionCallback = reinterpret_cast<bool (*)(void*, void*)>(
      dlsym(RTLD_NEXT, "_ZN3rex7runtime11MMIOHandler17ExceptionCallbackEPNS_4arch9ExceptionE"));
  real_RefreshGuestOutput = reinterpret_cast<bool (*)(void*, uint32_t, uint32_t, uint32_t, uint32_t, void*)>(
      dlsym(RTLD_NEXT, "_ZN3rex2ui9Presenter18RefreshGuestOutputEjjjjSt8functionIFbRNS1_25GuestOutputRefreshContextEEE"));
  real_FiberCreate = reinterpret_cast<void* (*)(unsigned long, void (*)(void*), void*)>(
      dlsym(RTLD_NEXT, "_ZN3rex6thread5Fiber6CreateEmPFvPvES2_"));
  real_RegisterFiber = reinterpret_cast<void (*)(void*, unsigned int, const void*)>(
      dlsym(RTLD_NEXT, "_ZN3rex6system11KernelState13RegisterFiberEjRKNS0_9FiberInfoE"));
  real_rexcrt_CreateFiber = reinterpret_cast<void* (*)(unsigned int, void*, void*)>(
      dlsym(RTLD_NEXT, "rexcrt_CreateFiber"));
  real_rexcrt_ConvertThreadToFiber = reinterpret_cast<void* (*)(void*)>(
      dlsym(RTLD_NEXT, "rexcrt_ConvertThreadToFiber"));
  real_rexcrt_SwitchToFiber = reinterpret_cast<void (*)(void*)>(
      dlsym(RTLD_NEXT, "rexcrt_SwitchToFiber"));
  real_rexcrt_DeleteFiber = reinterpret_cast<void (*)(void*)>(
      dlsym(RTLD_NEXT, "rexcrt_DeleteFiber"));

  // Track main thread (ID 0)
  pthread_t pt = pthread_self();
  pid_t tid = static_cast<pid_t>(syscall(SYS_gettid));
  pthread_attr_t attr;
  void* stackaddr = nullptr;
  size_t stacksize = 0;
  if (pthread_getattr_np(pt, &attr) == 0) {
    pthread_attr_getstack(&attr, &stackaddr, &stacksize);
    pthread_attr_destroy(&attr);
  }

  ThreadEntry main_thread{};
  main_thread.id = 0;
  main_thread.tid = tid;
  main_thread.pt = pt;
  main_thread.stackaddr = stackaddr;
  main_thread.stacksize = stacksize;
  main_thread.is_main = true;
  main_thread.is_guest = false;
  snprintf(main_thread.name, sizeof(main_thread.name), "MainThread");
  g_threads.push_back(main_thread);
  g_next_thread_id = 1;
  g_total_threads_created = 1;
  g_active_threads = 1;
  g_peak_concurrent_threads = 1;

  // Signal handlers for clean shutdown capture
  struct sigaction sa{};
  sa.sa_handler = SignalHandler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGTERM, &sa, nullptr);
  sigaction(SIGINT, &sa, nullptr);

  // atexit hook
  atexit(GenerateReport);

  // Start background sampler thread
  g_sampler_running.store(true);
  if (real_pthread_create) {
    real_pthread_create(&g_sampler_thread, nullptr, SamplerThreadMain, nullptr);
  }

  fprintf(stderr, "[PROBE] Native probe initialized for trials_hd (W-S06 / W-S07 / W-S08)\n");
}

__attribute__((destructor)) static void ProbeFini() {
  if (!g_is_trials_hd) return;
  GenerateReport();
}

// Intercept pthread_create
extern "C" int pthread_create(pthread_t* thread, const pthread_attr_t* attr,
                             void* (*start_routine)(void*), void* arg) {
  if (!real_pthread_create) {
    real_pthread_create = reinterpret_cast<int (*)(pthread_t*, const pthread_attr_t*, void* (*)(void*), void*)>(
        dlsym(RTLD_NEXT, "pthread_create"));
  }

  if (!g_is_trials_hd) {
    return real_pthread_create(thread, attr, start_routine, arg);
  }

  int id = g_next_thread_id.fetch_add(1);
  g_total_threads_created.fetch_add(1);
  int cur_active = g_active_threads.fetch_add(1) + 1;
  int peak = g_peak_concurrent_threads.load();
  while (cur_active > peak && !g_peak_concurrent_threads.compare_exchange_weak(peak, cur_active)) {}

  ThreadEntry entry{};
  entry.id = id;
  entry.is_main = false;
  entry.finished = false;

  {
    std::lock_guard<std::mutex> lock(g_threads_mutex);
    g_threads.push_back(entry);
  }

  WrapArgs* wrap = new WrapArgs{start_routine, arg, id};
  return real_pthread_create(thread, attr, ThreadWrapper, wrap);
}

// Intercept pthread_setname_np
extern "C" int pthread_setname_np(pthread_t thread, const char* name) {
  if (!real_pthread_setname_np) {
    real_pthread_setname_np = reinterpret_cast<int (*)(pthread_t, const char*)>(
        dlsym(RTLD_NEXT, "pthread_setname_np"));
  }

  if (!g_is_trials_hd) {
    return real_pthread_setname_np(thread, name);
  }

  if (name) {
    std::lock_guard<std::mutex> lock(g_threads_mutex);
    for (auto& entry : g_threads) {
      if (pthread_equal(entry.pt, thread)) {
        snprintf(entry.name, sizeof(entry.name), "%s", name);
        if (strstr(name, "XThread") || strstr(name, "Guest")) {
          entry.is_guest = true;
        }
        break;
      }
    }
  }

  return real_pthread_setname_np(thread, name);
}

// Intercept MMIOHandler::ExceptionCallback (W-S07)
extern "C" bool _ZN3rex7runtime11MMIOHandler17ExceptionCallbackEPNS_4arch9ExceptionE(
    void* this_ptr, void* ex) {
  uint8_t* virtual_membase = *reinterpret_cast<uint8_t**>(reinterpret_cast<uintptr_t>(this_ptr) + 8);
  uint8_t* physical_membase = *reinterpret_cast<uint8_t**>(reinterpret_cast<uintptr_t>(this_ptr) + 16);
  uint64_t fault_address = *reinterpret_cast<uint64_t*>(reinterpret_cast<uintptr_t>(ex) + 0x18);

  if (fault_address >= reinterpret_cast<uintptr_t>(virtual_membase) &&
      fault_address < reinterpret_cast<uintptr_t>(physical_membase)) {
    uint64_t guest_addr = fault_address - reinterpret_cast<uintptr_t>(virtual_membase);
    if (guest_addr >= 0x7F000000ULL && guest_addr <= 0x7FFFFFFFULL) {
      g_mmio_faults_7f.fetch_add(1);
      fprintf(stderr, "\n[W-S07 FATAL] MMIO fault in 0x7F000000-0x7FFFFFFF: guest_addr=0x%08lX (fault_host=%p, virtual_membase=%p)!\n",
              static_cast<unsigned long>(guest_addr), reinterpret_cast<void*>(fault_address),
              reinterpret_cast<void*>(virtual_membase));
      fflush(stderr);
      abort();
    }
  }

  if (!real_ExceptionCallback) {
    real_ExceptionCallback = reinterpret_cast<bool (*)(void*, void*)>(
        dlsym(RTLD_NEXT, "_ZN3rex7runtime11MMIOHandler17ExceptionCallbackEPNS_4arch9ExceptionE"));
  }
  return real_ExceptionCallback(this_ptr, ex);
}

// Intercept Presenter::RefreshGuestOutput for first swap timing (W-S06)
extern "C" bool _ZN3rex2ui9Presenter18RefreshGuestOutputEjjjjSt8functionIFbRNS1_25GuestOutputRefreshContextEEE(
    void* this_ptr, uint32_t mailbox_index, uint32_t frontbuffer_width, uint32_t frontbuffer_height,
    uint32_t mailbox_count, void* context_fn) {
  uint64_t prev = g_total_swaps.fetch_add(1);
  if (prev == 0) {
    auto now = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(now - g_start_time).count();
    g_first_swap_ms.store(ms);
    g_has_first_swap.store(true);
    fprintf(stderr, "[W-S06 SWAP] First guest swap at %.2f ms\n", ms);
  }

  if (!real_RefreshGuestOutput) {
    real_RefreshGuestOutput = reinterpret_cast<bool (*)(void*, uint32_t, uint32_t, uint32_t, uint32_t, void*)>(
        dlsym(RTLD_NEXT, "_ZN3rex2ui9Presenter18RefreshGuestOutputEjjjjSt8functionIFbRNS1_25GuestOutputRefreshContextEEE"));
  }
  return real_RefreshGuestOutput(this_ptr, mailbox_index, frontbuffer_width, frontbuffer_height, mailbox_count, context_fn);
}

// Intercept Fiber calls (W-S08)
extern "C" {

void* _ZN3rex6thread5Fiber6CreateEmPFvPvES2_(unsigned long stack_size, void (*start_routine)(void*), void* param) {
  g_fiber_calls.fetch_add(1);
  fprintf(stderr, "[W-S08 FIBER CALL] Fiber::Create called (stack_size=%lu)!\n", stack_size);
  if (!real_FiberCreate) {
    real_FiberCreate = reinterpret_cast<void* (*)(unsigned long, void (*)(void*), void*)>(
        dlsym(RTLD_NEXT, "_ZN3rex6thread5Fiber6CreateEmPFvPvES2_"));
  }
  return real_FiberCreate(stack_size, start_routine, param);
}

void _ZN3rex6system11KernelState13RegisterFiberEjRKNS0_9FiberInfoE(
    void* this_ptr, unsigned int fiber_address, const void* fiber_info) {
  g_fiber_calls.fetch_add(1);
  fprintf(stderr, "[W-S08 FIBER CALL] KernelState::RegisterFiber called (addr=0x%08x)!\n", fiber_address);
  if (!real_RegisterFiber) {
    real_RegisterFiber = reinterpret_cast<void (*)(void*, unsigned int, const void*)>(
        dlsym(RTLD_NEXT, "_ZN3rex6system11KernelState13RegisterFiberEjRKNS0_9FiberInfoE"));
  }
  real_RegisterFiber(this_ptr, fiber_address, fiber_info);
}

void* rexcrt_CreateFiber(unsigned int stack_size, void* start_address, void* parameter) {
  g_fiber_calls.fetch_add(1);
  fprintf(stderr, "[W-S08 FIBER CALL] rexcrt_CreateFiber called!\n");
  if (!real_rexcrt_CreateFiber) {
    real_rexcrt_CreateFiber = reinterpret_cast<void* (*)(unsigned int, void*, void*)>(
        dlsym(RTLD_NEXT, "rexcrt_CreateFiber"));
  }
  return real_rexcrt_CreateFiber(stack_size, start_address, parameter);
}

void* rexcrt_ConvertThreadToFiber(void* param) {
  g_fiber_calls.fetch_add(1);
  fprintf(stderr, "[W-S08 FIBER CALL] rexcrt_ConvertThreadToFiber called!\n");
  if (!real_rexcrt_ConvertThreadToFiber) {
    real_rexcrt_ConvertThreadToFiber = reinterpret_cast<void* (*)(void*)>(
        dlsym(RTLD_NEXT, "rexcrt_ConvertThreadToFiber"));
  }
  return real_rexcrt_ConvertThreadToFiber(param);
}

void rexcrt_SwitchToFiber(void* fiber) {
  g_fiber_calls.fetch_add(1);
  fprintf(stderr, "[W-S08 FIBER CALL] rexcrt_SwitchToFiber called!\n");
  if (!real_rexcrt_SwitchToFiber) {
    real_rexcrt_SwitchToFiber = reinterpret_cast<void (*)(void*)>(
        dlsym(RTLD_NEXT, "rexcrt_SwitchToFiber"));
  }
  real_rexcrt_SwitchToFiber(fiber);
}

void rexcrt_DeleteFiber(void* fiber) {
  g_fiber_calls.fetch_add(1);
  fprintf(stderr, "[W-S08 FIBER CALL] rexcrt_DeleteFiber called!\n");
  if (!real_rexcrt_DeleteFiber) {
    real_rexcrt_DeleteFiber = reinterpret_cast<void (*)(void*)>(
        dlsym(RTLD_NEXT, "rexcrt_DeleteFiber"));
  }
  real_rexcrt_DeleteFiber(fiber);
}

}  // extern "C"
