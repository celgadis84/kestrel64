// kestrel64 — entry point.
// Usage: kestrel64 <rom.z64|.n64|.v64> [--port N]

#include "core/system.hpp"
#include "cpu/jit.hpp"
#include "rsp/rsp.hpp"
#include "audio/audio.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <csignal>

#ifdef _WIN32
#include <windows.h>
#endif

static kestrel::System* g_system = nullptr;

static void onSignal(int) {
  if(g_system) g_system->requestShutdown();
}

#ifdef _WIN32
// Host-crash forensics: on an access violation, dump the guest CPU state so we
// can map the host fault back to the interpreter path / guest PC that caused it.
static LONG WINAPI crashFilter(EXCEPTION_POINTERS* ep) {
  auto* rec = ep->ExceptionRecord;
  std::fprintf(stderr, "\n==== HOST EXCEPTION 0x%08lx at host RIP %p ====\n",
               (unsigned long)rec->ExceptionCode, rec->ExceptionAddress);
  if(rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
    std::fprintf(stderr, "  access violation: %s at host addr 0x%llx\n",
                 rec->ExceptionInformation[0] == 0 ? "READ" :
                 rec->ExceptionInformation[0] == 1 ? "WRITE" : "EXEC",
                 (unsigned long long)rec->ExceptionInformation[1]);
  }
  if(g_system) {
    auto& c = g_system->cpu;
    std::fprintf(stderr, "  guest pc=0x%016llx nextPc=0x%016llx halted=%d\n",
                 (unsigned long long)c.pc, (unsigned long long)c.nextPc, c.halted);
    // Jump log: last control transfers, newest first.
    std::fprintf(stderr, "  --- jumplog (newest first) ---\n");
    for(int k = 0; k < kestrel::CPU::kJumpLog; k++) {
      kestrel::u32 i = (c.jlogIdx - 1 - k) % kestrel::CPU::kJumpLog;
      if(c.jlogSrc[i] == 0 && c.jlogDst[i] == 0) continue;
      std::fprintf(stderr, "    [%02d] op=0x%02x  src=0x%08llx -> dst=0x%08llx\n",
                   k, c.jlogOp[i],
                   (unsigned long long)c.jlogSrc[i], (unsigned long long)c.jlogDst[i]);
    }
  }
  std::fflush(stderr);
  return EXCEPTION_EXECUTE_HANDLER;  // let the process die after we've logged
}
#endif

int main(int argc, char** argv) {
  std::setvbuf(stderr, nullptr, _IONBF, 0);   // diagnósticos (heartbeat/jitstats) en streaming
  std::string romPath;
  kestrel::u16 port = 9128;
  bool freeRun = false;

  for(int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if(a == "--port" && i + 1 < argc) { port = (kestrel::u16)std::atoi(argv[++i]); }
    else if(a == "--run") { freeRun = true; }  // start unpaused (default: paused for stepping)
    else if(a == "--rspfuzz") {                 // differential VU fuzz: scalar vs SSE, then exit
      unsigned long long iters = (i + 1 < argc && argv[i + 1][0] != '-') ? std::strtoull(argv[++i], nullptr, 10) : 20000000ull;
      kestrel::Rsp rsp;
      std::fprintf(stderr, "[rspfuzz] running %llu iterations...\n", iters);
      kestrel::u64 fails = rsp.fuzzVU(iters);
      return fails == 0 ? 0 : 1;
    }
    else if(a == "--vubench") {                 // time scalar vs SSE VU path, then exit
      unsigned long long iters = (i + 1 < argc && argv[i + 1][0] != '-') ? std::strtoull(argv[++i], nullptr, 10) : 100000000ull;
      kestrel::Rsp rsp;
      rsp.benchVU(iters);
      return 0;
    }
    else if(a == "--help" || a == "-h") {
      std::printf("kestrel64 %s\nusage: %s <rom> [--port N] [--run]\n", kestrel::System::kVersion, argv[0]);
      return 0;
    }
    else if(!a.empty() && a[0] != '-') { romPath = a; }
  }

  std::printf("kestrel64 %s - N64 emulator\n", kestrel::System::kVersion);

  // Dynarec foundation self-test (Etapa 1): verifies the RWX code buffer + x86-64
  // emitter produce runnable code. Gated; does not touch the interpreter path.
  if(std::getenv("KESTREL_JIT_SMOKE")) {
    bool ok = kestrel::jit::smokeTest();
    // SRA $3,$4,4  (rd=3 rt=4 sa=4, funct=3): esperado 0xfffffffff89abcde para rt=...89abcdef
    kestrel::u32 sra = (0u<<26)|(0u<<21)|(4u<<16)|(3u<<11)|(4u<<6)|3u;
    kestrel::jit::opSelfTest(sra, 0, 0x0123456789abcdefull, 3);
    // SRAV $3,$4,$5 (rd=3 rt=4 rs=5 funct=7): shift = rs&31
    kestrel::u32 srav = (0u<<26)|(5u<<21)|(4u<<16)|(3u<<11)|(0u<<6)|7u;
    kestrel::jit::opSelfTest(srav, 4, 0x0123456789abcdefull, 3);
    // Etapa 2b: prueba call-C + bail condicional con patch de rel32 (aún sin cablear).
    bool cb = kestrel::jit::callBailSelfTest();
    ok = ok && cb;
    if(!romPath.empty()) std::fprintf(stderr, "[jit] smoke test %s; continuing\n", ok ? "ok" : "FAILED");
    else return ok ? 0 : 1;
  }

  if(romPath.empty()) {
    std::fprintf(stderr, "error: no ROM given.\nusage: %s <rom> [--port N]\n", argv[0]);
    return 1;
  }

  kestrel::System system;
  g_system = &system;
  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);
#ifdef _WIN32
  SetUnhandledExceptionFilter(crashFilter);
#endif

  std::string error;
  if(!system.init(romPath, error)) {
    std::fprintf(stderr, "error: %s\n", error.c_str());
    return 1;
  }

  system.paused.store(!freeRun);
  system.startTelemetry(port);
  system.startVideo();
  std::printf("[system] running (M1: CPU interpreter, %s). Ctrl-C to quit.\n",
              freeRun ? "free-run" : "paused — step over MCP");
  std::fflush(stdout);

  system.runLoop();
  kestrel::audio::shutdown();
  std::printf("[system] shutdown.\n");
  return 0;
}
