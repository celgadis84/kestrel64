#include "system.hpp"
#ifdef _WIN32
#include <windows.h>
#endif
#include "../telemetry/server.hpp"
#include "../telemetry/hostprof.hpp"
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

// Leído una vez al arranque: como `static` local se comprobaba la variable-guarda de
// inicialización en CADA vuelta del bucle, o sea por instrucción emulada.
static const bool g_jitOn = std::getenv("KESTREL_JIT") != nullptr;

auto System::stepCpu(u64 n) -> u64 {
  const bool jitOn = g_jitOn;
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

// Hash of the VI framebuffer exactly as the headless dump reads it (origin, VI_WIDTH
// stride, bpp from VI_CTRL). Region size only — the point is "did the picture move",
// not what it looks like, so a cheap FNV-1a over the bytes is enough.
static auto framebufferHash(Memory& mem) -> u64 {
  u32 origin = mem.rcp.vi_origin & 0x00ff'ffff;
  u32 type   = mem.rcp.vi_ctrl & 3;                        // 2=16bpp, 3=32bpp
  if(type != 2 && type != 3) return 0;
  u32 w = mem.rcp.vi_width ? mem.rcp.vi_width : 320;
  if(w == 0 || w > 640) w = 320;
  u32 ysc = mem.rcp.vi_yscale & 0xfff;
  u32 baseH = ((mem.rcp.vi_vsync & 0x3ff) >= 550) ? 288 : 240;
  u32 h = ysc ? ((baseH * ysc) >> 10) : baseH;
  if(h == 0 || h > 576) h = baseH;
  usize bytes = (usize)w * h * (type == 2 ? 2 : 4);
  const auto& ram = mem.rdram;
  if((usize)origin + bytes > ram.size()) return 0;
  u64 hsh = 1469598103934665603ull ^ origin ^ ((u64)w << 32) ^ ((u64)type << 48);
  for(usize i = 0; i < bytes; i++) { hsh ^= ram[origin + i]; hsh *= 1099511628211ull; }
  return hsh;
}

auto System::run() -> void {
  // Watchdog opt-in (KESTREL_WATCHDOG=<segundos>): un livelock del guest y un hilo CPU
  // bloqueado en un handshake del RCP se ven IGUAL desde fuera. Esto los separa: si
  // retired avanza, gira el guest; si no avanza, la CPU esta parada esperando al RCP.
  std::thread wdog;
  if(const char* w = std::getenv("KESTREL_WATCHDOG")) {
    unsigned secs = (unsigned)std::strtoul(w, nullptr, 0); if(!secs) secs = 5;
#ifdef _WIN32
    // Si retired no avanza, el hilo CPU esta atascado DENTRO del emulador. Suspenderlo y
    // leer su RIP dice exactamente donde (se simboliza con scripts/hostprof.py).
    HANDLE cpuTh = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &cpuTh, 0, FALSE, DUPLICATE_SAME_ACCESS);
    u64 imgBase = (u64)GetModuleHandleW(nullptr);
#endif
    wdog = std::thread([this, secs
#ifdef _WIN32
                        , cpuTh, imgBase
#endif
                       ]{
      u64 last = 0;
      while(!shutdown.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(secs));
        u64 now = cpu.retired;
        std::fprintf(stderr, "[wdog] retired=%llu (+%llu) pc=%08x sp_status=%08x sp_pc=%03x rspRun=%u rspBusy=%u rspKick=%u rdpBusy=%u rdpQ=%u mi_intr=%02x mi_mask=%02x\n",
                     (unsigned long long)now, (unsigned long long)(now - last), (u32)cpu.pc,
                     memory.rcp.sp_status.load(), memory.rcp.sp_pc, (unsigned)memory.rsp.running,
                     (unsigned)memory.rspBusy.load(), (unsigned)memory.rspKick,
                     (unsigned)memory.rdpBusy.load(), (unsigned)memory.rdpQueue.size(),
                     memory.rcp.mi_intr.load(), memory.rcp.mi_mask);
        std::fflush(stderr);
#ifdef _WIN32
        if(now == last && cpuTh) {
          CONTEXT ctx{}; ctx.ContextFlags = CONTEXT_CONTROL;
          if(SuspendThread(cpuTh) != (DWORD)-1) {
            if(GetThreadContext(cpuTh, &ctx))
              std::fprintf(stderr, "[wdog] CPU thread stuck at rip +0x%llx (rsp=%llx)\n",
                           (unsigned long long)((u64)ctx.Rip - imgBase), (unsigned long long)ctx.Rsp);
            ResumeThread(cpuTh);
          }
        }
#endif
        last = now;
      }
    });
  }
  hostprof::start();   // opt-in host sampler; samples THIS (CPU) thread

  // M1: run the CPU in short batches when not paused, yielding coreMutex between
  // batches so telemetry can inspect/step. Paused → idle; the telemetry thread
  // drives stepCpu() directly under the same lock.
  using clock = std::chrono::steady_clock;
  auto  winT0 = clock::now();       // sliding speed window (recomputed ~4x/sec)
  u64   winInsn = 0, winRsp = rspCycles;
  auto  hbT0 = winT0, hbLast = winT0;   // heartbeat lifetime baseline
  bool  hbOn = std::getenv("KESTREL_HEARTBEAT") != nullptr;

  // KESTREL_STABLE=<insns per check>[,<checks>] — stop once the picture stops moving.
  //
  // A fixed instruction cap is the wrong yardstick for a screenshot oracle: ROMs that
  // decode an image on the CPU (DCT, Huffman, I4/I8, GRB, Mandelbrot) are still halfway
  // down the frame when the cap fires, and the reference compare then scores a partially
  // drawn picture — 33% for a third of the image, which reads like an emulation bug and
  // is not one. Raising the cap for everyone costs the whole suite. So: run with a high
  // cap and stop when the VI framebuffer has been byte-identical for N consecutive
  // checks, having changed at least once (otherwise a ROM that has not drawn yet counts
  // as "stable" while still black).
  //
  // KESTREL_MAXFLIPS=<n> is the companion bound for ROMs that never settle at all —
  // video playback, the rotating-primitive demos. Those animate, so "stable" never
  // arrives and only the instruction cap stops them, on whatever frame it happens to
  // land. Counting displayed-buffer swaps instead pins them to an early, reproducible
  // frame, and it does not touch the CPU decoders: those render one picture into one
  // buffer and never flip.
  // KESTREL_MAXSYNCS=<n> is the same idea for animation that never flips a buffer (the
  // rotating-primitive demos redraw one framebuffer in place): an RDP SYNC_FULL is the
  // end of a display list, so N syncs is N completed frames. CPU-side decoders never
  // touch the RDP, so it does not bound them either.
  u64  stableEvery = 0, stableNeed = 3, stableNext = 0, stableHash = 0;
  u32  stableRun = 0, maxFlips = 0, maxSyncs = 0;
  bool stableSawChange = false;
  if(const char* f = std::getenv("KESTREL_MAXFLIPS")) maxFlips = (u32)std::strtoul(f, nullptr, 0);
  if(const char* f = std::getenv("KESTREL_MAXSYNCS")) maxSyncs = (u32)std::strtoul(f, nullptr, 0);
  if(const char* s = std::getenv("KESTREL_STABLE")) {
    char* end = nullptr;
    stableEvery = std::strtoull(s, &end, 0);
    if(end && *end == ',') stableNeed = std::strtoull(end + 1, nullptr, 0);
    if(stableNeed == 0) stableNeed = 1;
    stableNext = stableEvery;
  }
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
      // Diagnostico de divergencia entre modos, opt-in. El md5 final solo dice "difieren";
      // estos dicen DONDE: KESTREL_FIELDHASH=1 imprime un FNV del estado CPU al cierre de
      // cada campo (primer campo distinto = ventana a bisecar) y KESTREL_FIELDDUMP=<n>
      // vuelca los registros enteros en ese campo. El getenv se lee una vez.
      static const u64  fieldDump = std::getenv("KESTREL_FIELDDUMP")
                                  ? std::strtoull(std::getenv("KESTREL_FIELDDUMP"), nullptr, 0) : 0;
      static const bool fieldHash = std::getenv("KESTREL_FIELDHASH") != nullptr;
      if(fieldDump || fieldHash) {
        static u64 nField = 0; nField++;
        if(nField == fieldDump) {
          for(int r = 0; r < 32; r++) std::fprintf(stderr, "[fd] gpr%02d=%016llx\n", r, (unsigned long long)cpu.gpr[r]);
          for(int r = 0; r < 32; r++) std::fprintf(stderr, "[fd] cop0_%02d=%016llx\n", r, (unsigned long long)cpu.cop0[r]);
          std::fprintf(stderr, "[fd] pc=%016llx next=%016llx retired=%llu\n", (unsigned long long)cpu.pc,
                       (unsigned long long)cpu.nextPc, (unsigned long long)cpu.retired);
        }
        if(fieldHash) {
          u64 h = 1469598103934665603ull;
          auto mix = [&](u64 v){ h ^= v; h *= 1099511628211ull; };
          for(int r = 0; r < 32; r++) mix((u64)cpu.gpr[r]);
          mix(cpu.pc); mix(cpu.nextPc);
          mix((u64)cpu.cop0[9]); mix((u64)cpu.cop0[11]); mix((u64)cpu.cop0[12]); mix((u64)cpu.cop0[13]);
          mix(cpu.retired);
          std::fprintf(stderr, "[fh] %llu %016llx\n", (unsigned long long)nField, (unsigned long long)h);
        }
        std::fflush(stderr);
      }
      memory.viTick();
    }
    retiredInsns.fetch_add(did, std::memory_order_relaxed);
    winInsn += did;

    if((maxFlips && memory.rcp.viFlips >= maxFlips) ||
       (maxSyncs && memory.rcp.dpSyncs >= maxSyncs)) {
      std::fprintf(stderr, "[frames] %u buffer swaps, %u RDP syncs after %lluM insns, stopping\n",
                   memory.rcp.viFlips, memory.rcp.dpSyncs,
                   (unsigned long long)(cpu.retired / 1'000'000));
      std::lock_guard<std::mutex> lk(coreMutex);
      cpu.maxInsn = cpu.retired + 1;
    }

    if(stableEvery && cpu.retired >= stableNext) {
      stableNext = cpu.retired + stableEvery;
      u64 h;
      { std::lock_guard<std::mutex> lk(coreMutex); h = framebufferHash(memory); }
      if(h != stableHash) { stableHash = h; stableRun = 0; stableSawChange = true; }
      else if(stableSawChange && ++stableRun >= stableNeed) {
        std::fprintf(stderr, "[stable] framebuffer unchanged for %llu insns after %lluM, stopping\n",
                     (unsigned long long)(stableEvery * stableNeed),
                     (unsigned long long)(cpu.retired / 1'000'000));
        // Reuse the cap path so the dump/report side stays in exactly one place.
        std::lock_guard<std::mutex> lk(coreMutex);
        cpu.maxInsn = cpu.retired + 1;
      }
    }

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
      // Worker occupancy as a share of wall time: which domain is the long pole.
      // >100% per worker is impossible, and cpuWait is how long the CPU thread sat
      // blocked on one of them — together they localize the bottleneck.
      double rdpPct  = memory.rdpBusyNs.load(std::memory_order_relaxed) / 1e9 / s * 100.0;
      double rspPct  = memory.rspBusyNs.load(std::memory_order_relaxed) / 1e9 / s * 100.0;
      double waitPct = memory.cpuWaitNs.load(std::memory_order_relaxed) / 1e9 / s * 100.0;
      std::fprintf(stderr, "[hb] %.0fM insns, %.2f Mips avg | N64 speed: CPU %.1f%%  RSP %.1f%%"
                           " | occupancy: rdp %.0f%% rsp %.0f%% cpuWait %.0f%%\n",
                   ri / 1e6, ri / 1e6 / s,
                   n64SpeedPct.load(std::memory_order_relaxed),
                   rspSpeedPct.load(std::memory_order_relaxed),
                   rdpPct, rspPct, waitPct);
      hbLast = now;
    }
  }
  hostprof::stop();
  if(wdog.joinable()) { shutdown.store(true); wdog.join(); }
}

}  // namespace kestrel
