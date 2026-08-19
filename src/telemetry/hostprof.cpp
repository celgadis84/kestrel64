// Host-side sampling profiler. The guest-PC profiler (MCP prof.*) says which N64
// code runs; this says where the *emulator* burns host cycles, which is the only
// way to aim optimisation work. A watcher thread suspends the sampled thread at a
// fixed interval, reads its RIP, and buckets it by module-relative address; the
// dump is symbolised offline (scripts/hostprof.py, via nm on the exe).
//
// Opt-in: KESTREL_HOSTPROF=<ms> (0/unset = off, thread never starts).
#include "hostprof.hpp"

#ifdef _WIN32
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <unordered_map>
#include <vector>
#include <algorithm>

namespace kestrel::hostprof {

namespace {
std::thread            g_thread;
std::atomic<bool>      g_stop{false};
HANDLE                 g_target = nullptr;
std::unordered_map<u64, u64> g_hits;   // module-relative RIP (16 B bucket) -> samples
u64                    g_samples = 0, g_base = 0, g_extern = 0;
auto dump() -> void;

auto sampleLoop(unsigned periodMs) -> void {
  // Sampling by suspend/GetThreadContext: no symbol server, no debug info, works on
  // a plain Release build. The bucket is 16 bytes so a hot basic block lands in one
  // entry rather than smearing across every instruction address.
  CONTEXT ctx{};
  ctx.ContextFlags = CONTEXT_CONTROL;
  while(!g_stop.load(std::memory_order_relaxed)) {
    if(SuspendThread(g_target) != (DWORD)-1) {
      if(GetThreadContext(g_target, &ctx)) {
        u64 rip = (u64)ctx.Rip;
        // Out-of-image RIP = inside a system DLL (memcpy, kernel32, the CRT); count
        // it in one bucket rather than polluting the histogram with load addresses.
        if(rip >= g_base && rip - g_base < (64ull << 20)) g_hits[(rip - g_base) & ~0xfull]++;
        else g_extern++;
        g_samples++;
      }
      ResumeThread(g_target);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(periodMs));
    // Periodic dump: the process is normally stopped with taskkill, so waiting for
    // an orderly stop() would lose the whole profile.
    if((g_samples % 2000) == 0 && g_samples) dump();
  }
}

auto dump() -> void {
  std::vector<std::pair<u64,u64>> v(g_hits.begin(), g_hits.end());
  std::sort(v.begin(), v.end(), [](auto& a, auto& b){ return a.second > b.second; });
  std::fprintf(stderr, "[hostprof] %llu samples\n", (unsigned long long)g_samples);
  u64 shown = 0;
  for(usize i = 0; i < v.size() && i < 40; i++) {
    std::fprintf(stderr, "[hostprof] %6.2f%%  +0x%llx  (%llu)\n",
                 100.0 * v[i].second / (g_samples ? g_samples : 1),
                 (unsigned long long)v[i].first, (unsigned long long)v[i].second);
    shown += v[i].second;
  }
  std::fprintf(stderr, "[hostprof] extern(DLL/CRT) %.2f%% | top-40 covers %.1f%%\n",
               100.0 * g_extern / (g_samples ? g_samples : 1),
               100.0 * shown / (g_samples ? g_samples : 1));
  std::fflush(stderr);
}
}  // namespace

auto start() -> void {
  const char* e = std::getenv("KESTREL_HOSTPROF");
  if(!e) return;
  unsigned ms = (unsigned)std::strtoul(e, nullptr, 0);
  if(ms == 0) ms = 1;
  g_base = (u64)GetModuleHandleW(nullptr);
  // Sample the caller — this is meant to be started from the thread that runs the CPU.
  if(!DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(),
                      &g_target, 0, FALSE, DUPLICATE_SAME_ACCESS)) return;
  g_stop = false;
  g_thread = std::thread([ms]{ sampleLoop(ms); });
  std::fprintf(stderr, "[hostprof] sampling every %u ms, image base 0x%llx\n",
               ms, (unsigned long long)g_base);
}

auto stop() -> void {
  if(!g_thread.joinable()) return;
  g_stop = true;
  g_thread.join();
  dump();
}

}  // namespace kestrel::hostprof
#else
namespace kestrel::hostprof { auto start() -> void {} auto stop() -> void {} }
#endif
