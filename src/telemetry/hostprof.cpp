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
#include <cstring>
#include <cwchar>
#include <thread>
#include <unordered_map>
#include <vector>
#include <algorithm>

namespace kestrel::hostprof {

namespace {
std::thread            g_thread;
std::atomic<bool>      g_stop{false};
HANDLE                 g_target = nullptr;
const std::atomic<bool>* g_gate = nullptr;   // ver hostprof.hpp: muestrear solo si esta a true
std::unordered_map<u64, u64> g_hits;   // module-relative RIP (16 B bucket) -> samples
u64                    g_samples = 0, g_base = 0, g_extern = 0;
// Fuera de la imagen del exe caen DOS cosas muy distintas: una DLL del sistema (espera,
// memcpy, driver de video) y el BUFFER RWX del dynarec, que es codigo nuestro y suele ser
// donde de verdad se va el tiempo. Meterlas en el mismo saco "extern" hacia ilegible el
// perfil justo cuando el JIT esta encendido. Se agrupa por AllocationBase de la region
// (una llamada a VirtualQuery por muestra, con el hilo ya suspendido) y el nombre se
// resuelve en el volcado: modulo si lo hay, "jit/anon" si la region no es un modulo.
std::unordered_map<u64, u64> g_ext;    // AllocationBase de la region -> muestras
std::unordered_map<u64, u64> g_extcall;  // retorno in-image mas cercano -> muestras
auto dump() -> void;

auto sampleLoop(unsigned periodMs) -> void {
  // Sampling by suspend/GetThreadContext: no symbol server, no debug info, works on
  // a plain Release build. The bucket is 16 bytes so a hot basic block lands in one
  // entry rather than smearing across every instruction address.
  CONTEXT ctx{};
  ctx.ContextFlags = CONTEXT_CONTROL;   // RIP + RSP: basta para el barrido de pila
  while(!g_stop.load(std::memory_order_relaxed)) {
    if(g_gate && !g_gate->load(std::memory_order_relaxed)) {
      std::this_thread::sleep_for(std::chrono::microseconds(200));
      continue;
    }
    if(SuspendThread(g_target) != (DWORD)-1) {
      if(GetThreadContext(g_target, &ctx)) {
        u64 rip = (u64)ctx.Rip;
        // Out-of-image RIP = inside a system DLL (memcpy, kernel32, the CRT); count
        // it in one bucket rather than polluting the histogram with load addresses.
        if(rip >= g_base && rip - g_base < (64ull << 20)) g_hits[(rip - g_base) & ~0xfull]++;
        else {
          g_extern++;
          MEMORY_BASIC_INFORMATION mbi{};
          if(VirtualQuery((LPCVOID)rip, &mbi, sizeof mbi)) g_ext[(u64)mbi.AllocationBase]++;
          else g_ext[0]++;
          // Saber que estamos "en ntdll" no dice nada por si solo: lo que hace falta es QUIEN
          // llamo. Con el hilo suspendido, se barre el principio de su pila buscando la primera
          // palabra que caiga dentro de la imagen: es la direccion de retorno del marco nuestro
          // mas cercano. No es un desenrollado exacto (puede coger un puntero a codigo guardado
          // en la pila), pero con miles de muestras el sesgo se diluye y el histograma resultante
          // se simboliza con el mismo scripts/hostprof.py que el de dentro de la imagen.
          u64 stk[64];
          SIZE_T got = 0;
          if(ReadProcessMemory(GetCurrentProcess(), (LPCVOID)ctx.Rsp, stk, sizeof stk, &got)) {
            for(usize k = 0; k < got / 8; k++) {
              if(stk[k] >= g_base && stk[k] - g_base < (64ull << 20)) {
                g_extcall[(stk[k] - g_base) & ~0xfull]++;
                break;
              }
            }
          }
        }
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

// Desglose de lo que cae fuera de la imagen. Sin esto, un perfil con el dynarec encendido
// dice "extern 58%" y no distingue esperar en una DLL de ejecutar el codigo que acabamos
// de emitir, que es la diferencia entre "el host no nos da nucleo" y "nuestro JIT es lento".
auto extdump(u64 shown) -> void {
  std::vector<std::pair<u64,u64>> v(g_ext.begin(), g_ext.end());
  std::sort(v.begin(), v.end(), [](auto& a, auto& b){ return a.second > b.second; });
  const double n = (double)(g_samples ? g_samples : 1);
  std::fprintf(stderr, "[hostprof] extern(fuera de imagen) %.2f%% | top-40 in-image cubre %.1f%%\n",
               100.0 * g_extern / n, 100.0 * shown / n);
  for(usize i = 0; i < v.size() && i < 8; i++) {
    wchar_t path[MAX_PATH] = {};
    const wchar_t* name = L"jit/anon";
    if(v[i].first && GetModuleFileNameW((HMODULE)v[i].first, path, MAX_PATH)) {
      const wchar_t* s = wcsrchr(path, (wchar_t)92);   // separador de ruta
      name = s ? s + 1 : path;
    }
    std::fprintf(stderr, "[hostprof]   extern %6.2f%%  %ls  @0x%llx (%llu)\n",
                 100.0 * v[i].second / n, name,
                 (unsigned long long)v[i].first, (unsigned long long)v[i].second);
  }
  std::vector<std::pair<u64,u64>> w(g_extcall.begin(), g_extcall.end());
  std::sort(w.begin(), w.end(), [](auto& a, auto& b){ return a.second > b.second; });
  for(usize i = 0; i < w.size() && i < 12; i++)
    std::fprintf(stderr, "[hostprof]   extern-desde %6.2f%%  +0x%llx  (%llu)\n",
                 100.0 * w[i].second / n, (unsigned long long)w[i].first,
                 (unsigned long long)w[i].second);
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
  extdump(shown);
  std::fflush(stderr);
}
}  // namespace

auto start(const char* label) -> void {
  const char* e = std::getenv("KESTREL_HOSTPROF");
  if(!e) return;
  const char* who = std::getenv("KESTREL_HOSTPROF_WHO");
  if(!who) who = "cpu";
  if(std::strcmp(who, label) != 0) return;   // este hilo no es el elegido
  if(g_thread.joinable()) return;            // ya hay un muestreador vivo
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

auto gate(const std::atomic<bool>* g) -> void { g_gate = g; }

auto stop() -> void {
  if(!g_thread.joinable()) return;
  g_stop = true;
  g_thread.join();
  dump();
}

}  // namespace kestrel::hostprof
#else
namespace kestrel::hostprof { auto start(const char*) -> void {} auto stop() -> void {}
                              auto gate(const std::atomic<bool>*) -> void {} }
#endif
