#pragma once
// kestrel64 — top-level system. Owns memory, the ROM, and the telemetry server.
// At M0 there is no CPU yet; the system loads a ROM, builds the memory map, and
// serves telemetry so the MCP can already inspect state.

#include "types.hpp"
#include "memory.hpp"
#include "rom.hpp"
#include "../cpu/cpu.hpp"
#include "../video/present.hpp"
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <memory>
#include <vector>

namespace kestrel {

namespace telemetry { struct Server; }

struct System {
  Rom    rom;
  Memory memory;
  CPU    cpu;

  // The CPU runs on the run() thread; telemetry handlers touch CPU/memory from
  // the telemetry thread. coreMutex serializes the two. Lock it for any access
  // that reads or mutates emulation state across the thread boundary.
  std::mutex coreMutex;

  std::atomic<bool> paused{true};    // start paused so single-stepping is deterministic
  std::atomic<bool> shutdown{false};
  // Corrida por lotes (--run sin vídeo): un halt de la CPU (cap de maxinsn, fatal) es el fin
  // de la sesión, así que se sale en vez de quedarse girando. En modo MCP NO: ahí el halt es
  // un punto de inspección y el proceso tiene que seguir vivo para el cliente.
  bool exitOnHalt = false;

  // Per-domain clock model. Nominal N64 rates; `oc` = overclock multiplier
  // (1.0 = stock console). A future UI can retune these live to model an
  // overclocked machine per region. Speed% is measured against the (possibly
  // overclocked) CPU target rate — 100% == running at real N64 speed.
  struct Clocks {
    double cpuHz   = 93'750'000.0;   // R4300i
    double rspHz   = 62'500'000.0;   // RSP
    double rdramHz = 250'000'000.0;  // RDRAM (DDR)
    double cpuOc = 1.0, rspOc = 1.0, rdramOc = 1.0;
    // Video field rate. NTSC halfline counter free-runs at 60 fields/s (59.94 exact);
    // everything that measures guest time hangs off this.
    double viFieldHz = 59.94;
    // Modelo de CPI del interprete. El unico anclaje duro que hay entre "instruccion
    // retirada" y "ciclo de CPU" es COP0 Count: en el VR4300 avanza a MEDIO reloj, y aqui
    // avanza +1 por instruccion retirada (CPU::step). Eso fija el modelo:
    //   1 instruccion retirada = 2 ciclos de CPU (CPI 2).
    // De ahi sale todo lo demas — cuantas instrucciones dura un campo de video y a que
    // ritmo hay que retirar para ir a tiempo real — y sale UNA sola vez, para que no
    // vuelva a haber dos relojes distintos en el mismo emulador (los habia: el tick del VI
    // contaba 750k instrucciones por campo y la lectura de VI_V_CURRENT contaba 1.56M,
    // asi que un juego que mezclara interrupcion y sondeo veia dos campos por cada uno).
    double cyclesPerInsn = 2.0;
    auto cpuTarget()   const -> double { return cpuHz   * cpuOc; }
    auto rspTarget()   const -> double { return rspHz   * rspOc; }
    auto rdramTarget() const -> double { return rdramHz * rdramOc; }
    // Instrucciones retiradas por segundo que equivalen a tiempo real (= ciclos / CPI).
    auto insnTarget()  const -> double { return cpuTarget() / cyclesPerInsn; }
    // Instrucciones retiradas que dura un campo de video.
    auto fieldInsns()  const -> u64    { return (u64)(insnTarget() / viFieldHz + 0.5); }
    // El bucle no puede correr un campo entero de golpe: hay ROMs (y juegos con efectos de
    // rastreo) que reprograman VI_INTR dentro del campo para que la interrupcion salte a
    // media pantalla. Se trocea el campo en subtramos y el VI se mira en cada uno, asi que
    // la interrupcion cae con precision de 1/viTicksPerField de campo (~1 ms) en vez de una
    // sola vez por campo. Subirlo afina la interrupcion y cuesta mas vueltas del bucle;
    // KESTREL_VITICKS lo mueve para experimentar.
    u32 viTicksPerField = 16;
    auto tickInsns()   const -> u64    { u64 n = fieldInsns() / viTicksPerField; return n ? n : 1; }
    // Instrucciones de RSP que entran en UNA instruccion retirada de CPU.
    //
    // NO es la razon de los dos relojes (62.5/93.75 = 2/3): esa compara ciclos con
    // ciclos, y la unidad con la que avanza el bucle son instrucciones retiradas. El
    // RSP retira una por ciclo suyo; la CPU, una cada cyclesPerInsn ciclos del suyo:
    //   62.5 MHz / (93.75 MHz / 2) = 4/3
    // Ponerlo en 2/3 -- que es lo que habia, en el interleave de Lockstep y en el
    // regulador de Threaded -- le daba al RSP la MITAD del tiempo relativo que implica
    // nuestro propio modelo de Count. Sale de aqui, del mismo sitio que el reloj de
    // video, para que no vuelva a haber dos modelos de CPI en el mismo emulador.
    auto rspInsnsPerCpuInsn() const -> double { return rspTarget() / insnTarget(); }
  } clocks;

  // Speed telemetry. The interpreter models 1 emulated CPU cycle per retired
  // instruction (CPI≈1 baseline); speed% = emulated-cycles/sec vs the CPU target
  // clock, sampled over a sliding window so it tracks *current* speed, not a
  // lifetime average. 100.0 == real N64 speed. retiredInsns is the lifetime
  // count for MIPS/heartbeat.
  std::atomic<double> n64SpeedPct{0.0};   // overall (CPU domain) % of realtime
  std::atomic<double> rspSpeedPct{0.0};   // RSP domain % (0 while RSP idle)
  std::atomic<double> rdramSpeedPct{0.0}; // RDRAM domain % (0 until modeled)
  // Ocupacion de los workers y ritmo de imagen, sobre la MISMA ventana deslizante que el
  // medidor de velocidad. Sin esto el unico numero visible es el de la CPU, y en modo
  // threaded ese numero sube cuando la CPU gira en el spin-wait: parece mejor y es peor.
  // Con la ocupacion al lado se ve quien es el palo largo (RDP, RSP o espera de la CPU).
  std::atomic<double> rdpBusyPct{0.0};    // % de tiempo de pared con el RDP rasterizando
  std::atomic<double> rspBusyPct{0.0};    // % de tiempo de pared con el RSP ejecutando
  std::atomic<double> cpuWaitPct{0.0};    // % del tiempo con el hilo CPU bloqueado en un worker
  std::atomic<double> fieldsPerSec{0.0};  // intercambios de buffer por segundo = fps reales
  std::atomic<u64>    retiredInsns{0};    // lifetime CPU instructions (=cycles)
  u64                 rspCycles = 0;      // lifetime RSP steps (=cycles), run-thread only

  static constexpr const char* kVersion = "0.0.1-M1";

  // Load a ROM, build memory, HLE-boot the CPU. Returns false with `error` set.
  auto init(const std::string& romPath, std::string& error) -> bool;

  // Start the telemetry server on its own thread (non-blocking).
  auto startTelemetry(u16 port) -> bool;

  // Arm the VI presentation window (M3.1). No-op if disabled (KESTREL_VIDEO).
  // The window loop itself runs on the main thread inside runLoop().
  auto startVideo(bool batch) -> void;

  // Top-level blocking loop. With video on: CPU on a worker thread, the GLFW/
  // Vulkan present loop on THIS (main) thread — GLFW requires the main thread.
  // Without video: just runs the CPU loop here. Returns on shutdown.
  auto runLoop() -> void;

  // Block the calling thread, running the CPU when not paused, until shutdown.
  auto run() -> void;

  // Step the CPU n instructions (caller holds coreMutex). Returns steps taken.
  // While the RSP is running, it is advanced interleaved with the CPU at
  // Clocks::rspInsnsPerCpuInsn() (4/3 stock) so the two cores make progress
  // together - required for CPU<->RSP SIGNAL handshakes.
  auto stepCpu(u64 n) -> u64;
  // Acumulador en punto fijo del ratio: por cada instruccion de CPU se suma rspStepNum
  // y se ejecuta una instruccion de RSP por cada rspStepDen acumulados. En enteros para
  // que el reparto sea identico en cada corrida; el constructor los fija desde clocks.
  u64 rspPhase = 0, rspStepNum = 87'381, rspStepDen = 65'536;   // 4/3 en 16.16

  // PC breakpoints (debug). stepCpu checks these only while the vector is
  // non-empty, so there is zero hot-path cost when nothing is set. A hit auto-
  // pauses the run loop and records the PC in lastBpHit. Guarded by coreMutex
  // like the rest of the core state; run_until pushes/pops a temporary entry.
  std::vector<u32> breakpoints;
  std::atomic<u32> lastBpHit{0};   // PC of the breakpoint that last fired (0 = none)

  auto requestShutdown() -> void { shutdown.store(true); }

  System();       // out-of-line: unique_ptr<Server> holds an incomplete type here
  ~System();

private:
  std::unique_ptr<telemetry::Server> tele;
  std::thread teleThread;
  Presenter presenter;
  bool videoOn = false;
};

}  // namespace kestrel
