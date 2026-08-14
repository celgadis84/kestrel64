#include "system.hpp"
#include "../telemetry/server.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace kestrel {

System::System() = default;

System::~System() {
  requestShutdown();
  memory.flushSaveFile();   // persist battery/flash save to disk on exit
  memory.stopRcpThreads();
  if(tele) tele->stop();
  if(teleThread.joinable()) teleThread.join();
}

auto System::init(const std::string& romPath, std::string& error) -> bool {
  memory.reset(/*expansionPak=*/true);
  if(!rom.loadFile(romPath, error)) return false;
  memory.loadRom(rom.data);  // exposes CART_ROM region for telemetry
  memory.attachSaveFile(romPath);  // load existing .eep/.sra/.fla, if any, next to the ROM
  std::printf("[system] loaded \"%s\" (%s, %.2f MB, entry 0x%08x)\n",
              rom.header.name.c_str(),
              rom.originalOrder == Rom::Order::Z64 ? "z64" :
              rom.originalOrder == Rom::Order::N64 ? "n64" :
              rom.originalOrder == Rom::Order::V64 ? "v64" : "?",
              rom.data.size() / (1024.0 * 1024.0), rom.header.entryPoint);
  if(const char* w = std::getenv("KESTREL_WATCH")) {
    memory.watchAddr = (u32)std::strtoul(w, nullptr, 0) & 0x1fff'ffff;
    if(const char* l = std::getenv("KESTREL_WATCHLEN")) memory.watchLen = (u32)std::strtoul(l, nullptr, 0);
    std::printf("[watch] store watchpoint at phys 0x%08x len %u\n", memory.watchAddr, memory.watchLen);
  }
  cpu.connect(&memory);
  cpu.fastBoot(rom.header.entryPoint);  // HLE IPL3: boot segment in RDRAM, PC at entry
  std::printf("[cpu] HLE boot, pc=0x%08x\n", (u32)cpu.pc);
  if(std::getenv("KESTREL_THREADS")) {
    memory.rcpMode = Memory::RcpMode::Threaded;
    memory.startRcpThreads();
    std::printf("[system] RCP threading ON — RDP rasterizes on its own thread\n");
  }
  return true;
}

auto System::stepCpu(u64 n) -> u64 {
  static int jitOn = std::getenv("KESTREL_JIT") ? 1 : 0;
  u64 i = 0;
  while(i < n && !cpu.halted) {
    // Dynarec: intenta un bloque de ops seguras. Declina (0) cuando el RSP corre, cerca
    // de un borde de timer/interrupt, o ante una op no soportada → cae al intérprete.
    // El bloque solo se toma con el RSP parado, así que no altera el interleave 2:3.
    if(jitOn) {
      // Ventana que le queda a esta llamada: una cadena de bloques enlazados no puede
      // pasarse de aquí, o el bucle de arriba tickearía el VI tarde (campo estirado).
      cpu.jitOpsBudget = (u32)((n - i) > 0xFFFF'FFFFull ? 0xFFFF'FFFFull : (n - i));
      u32 k = cpu.jitTryBlock();
      if(k) { i += k; continue; }
    }
    cpu.step();
    i++;
    // Debug breakpoints: fire when the *next* PC to execute matches (standard
    // "stop before executing addr" semantics). Only scanned while armed → the
    // common no-breakpoint case pays a single empty()-check per instruction.
    if(!breakpoints.empty()) {
      u32 pcv = (u32)cpu.pc;
      for(u32 b : breakpoints) {
        if(b == pcv) { lastBpHit.store(pcv); paused.store(true); return i; }
      }
    }
    // Lockstep: interleave the RSP at ~2/3 the CPU rate (62.5 MHz vs 93.75 MHz).
    // Threaded: the RSP runs to completion on its own worker (see rspWorkerLoop),
    // so the CPU thread must NOT also step it — that would double-execute the core.
    if(memory.rcpMode == Memory::RcpMode::Lockstep && memory.rsp.running) {
      rspPhase += 2;
      while(rspPhase >= 3) { rspPhase -= 3; memory.rsp.step(1); rspCycles++; if(!memory.rsp.running) break; }
    }
  }
  return i;
}

auto System::startTelemetry(u16 port) -> bool {
  tele = std::make_unique<telemetry::Server>(*this);
  teleThread = std::thread([this, port] { tele->serve(port); });
  return true;
}

auto System::startVideo() -> void {
  if(!std::getenv("KESTREL_VIDEO")) return;   // opt-in until the RDP fills a framebuffer
  videoOn = presenter.start(&memory, &shutdown, &n64SpeedPct, &rspSpeedPct, &rdramSpeedPct,
                            rom.valid() ? rom.header.name.c_str() : nullptr);
  if(videoOn) std::printf("[video] VI presentation armed (window opens on main thread)\n");
}

auto System::runLoop() -> void {
  if(!videoOn) { run(); return; }
  // Bring up Vulkan/GLFW on THIS (main) thread BEFORE the CPU worker starts —
  // a CPU-bound sibling stalls the driver's device bring-up on this box.
  if(!presenter.open()) { std::printf("[video] init failed; running headless\n"); run(); return; }
  std::thread cpuThread([this] { run(); });
  while(!shutdown.load() && presenter.pumpFrame()) {}
  shutdown.store(true);      // window closed or stop → unwind the CPU worker
  cpuThread.join();
  presenter.close();
}

auto System::run() -> void {
  // M1: run the CPU in short batches when not paused, yielding coreMutex between
  // batches so telemetry can inspect/step. Paused → idle; the telemetry thread
  // drives stepCpu() directly under the same lock.
  using clock = std::chrono::steady_clock;
  auto  winT0 = clock::now();       // sliding speed window (recomputed ~4x/sec)
  u64   winInsn = 0, winRsp = rspCycles;
  auto  hbT0 = winT0, hbLast = winT0;   // heartbeat lifetime baseline
  bool  hbOn = std::getenv("KESTREL_HEARTBEAT") != nullptr;
  while(!shutdown.load()) {
    if(cpu.halted && exitOnHalt) { shutdown.store(true); break; }
    if(paused.load() || cpu.halted) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      winT0 = clock::now(); winInsn = 0; winRsp = rspCycles;  // don't fold idle time into speed
      continue;
    }
    u64 did;
    {
      std::lock_guard<std::mutex> lk(coreMutex);
      // ~one video field of CPU work per tick, then a VI field boundary. Keeps
      // main loops that block on the VI interrupt progressing.
      did = stepCpu(750000);
      memory.viTick();
    }
    retiredInsns.fetch_add(did, std::memory_order_relaxed);
    winInsn += did;

    // Recompute the realtime-% meter over a short window. Emulated cycles/sec vs
    // each domain's (overclock-scaled) target clock. 100% == real N64 speed.
    auto now = clock::now();
    double ws = std::chrono::duration<double>(now - winT0).count();
    if(ws >= 0.25) {
      double cpuCps = winInsn / ws;                          // CPI≈1 baseline
      double rspCps = (rspCycles - winRsp) / ws;
      n64SpeedPct.store(cpuCps / clocks.cpuTarget() * 100.0, std::memory_order_relaxed);
      rspSpeedPct.store(rspCps / clocks.rspTarget() * 100.0, std::memory_order_relaxed);
      // RDRAM has no per-transaction cycle model yet → leave at 0 (unmodeled).
      winT0 = now; winInsn = 0; winRsp = rspCycles;
    }

    if(hbOn && now - hbLast >= std::chrono::seconds(5)) {
      double s  = std::chrono::duration<double>(now - hbT0).count();
      u64    ri = retiredInsns.load(std::memory_order_relaxed);
      std::fprintf(stderr, "[hb] %.0fM insns, %.2f Mips avg | N64 speed: CPU %.1f%%  RSP %.1f%%\n",
                   ri / 1e6, ri / 1e6 / s,
                   n64SpeedPct.load(std::memory_order_relaxed),
                   rspSpeedPct.load(std::memory_order_relaxed));
      hbLast = now;
    }
  }
}

}  // namespace kestrel
