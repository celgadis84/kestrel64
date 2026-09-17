#pragma once
// kestrel64 — top-level system. Owns memory, the ROM, and the telemetry server.
// At M0 there is no CPU yet; the system loads a ROM, builds the memory map, and
// serves telemetry so the MCP can already inspect state.

#include "types.hpp"
#include "memory.hpp"
#include "rom.hpp"
#include "cheats.hpp"
#include "rewind.hpp"
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
  // Motor de trucos (GameShark). Vacio y sin coste cuando no hay fichero de codigos.
  Cheats cheats;
  // Rebobinado. Apagado de fabrica: cuesta una foto del estado cada pocos campos.
  rewind::Engine rewinder;

  // The CPU runs on the run() thread; telemetry handlers touch CPU/memory from
  // the telemetry thread. coreMutex serializes the two. Lock it for any access
  // that reads or mutates emulation state across the thread boundary.
  std::mutex coreMutex;

  std::atomic<bool> paused{true};    // start paused so single-stepping is deterministic
  std::atomic<bool> shutdown{false};
  // Avance por fotogramas (TAS). Campos de video que el bucle debe correr AUNQUE este en
  // pausa; al cerrar cada campo se descuenta uno y al llegar a cero la pausa vuelve a
  // mandar. El cuanto es el CAMPO de video, no la instruccion ni el volteo de buffer: es
  // la unidad en la que el juego lee el mando (una lectura de joybus por campo en casi
  // todos), asi que un campo = una entrada de la pelicula, que es lo que hace falta para
  // colocar una pulsacion en el sitio exacto.
  std::atomic<u32> stepFields{0};
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
    // Lo fija System a partir de CPU::cpi256 (KESTREL_CPI); de fabrica 1,4. El 2.0 de
    // aqui es el valor HISTORICO, el que ata un tick de Count a cada instruccion.
    double cyclesPerInsn = 2.0;
    auto cpiIsHistoric() const -> bool { return cyclesPerInsn == 2.0; }   // = el modelo viejo
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
  // Cierra el servidor y LIBERA EL PUERTO antes de que se destruya el System. Hace falta
  // para relanzarse desde el menu: el proceso nuevo se levanta mientras este sigue vivo, y
  // se encontraria el 9128 ocupado. Idempotente.
  auto stopTelemetry() -> void;

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

  // Ruta de la ROM cargada. La necesita el nombre de las ranuras de estado (rom.stN).
  std::string romPath;

  // Peticiones de estado guardado. Las pone quien sea (ventana, telemetria) y las atiende
  // el bucle de ejecucion, que es el unico sitio donde se puede parar el RCP en un punto
  // limpio: entre campos, con el RDP drenado y sin tarea de RSP a medias. Tomar el estado
  // desde el hilo de la ventana con el RDP rasterizando daria un fichero que no se puede
  // recargar en el otro modo de RCP. -1 = nada pendiente, si no = numero de ranura.
  std::atomic<int> stateSaveReq{-1}, stateLoadReq{-1};
  std::atomic<int> stateSlot{0};        // ranura activa (la que mueven las teclas)
  // Pasos de rebobinado pedidos y aun no dados. Es un CONTADOR y no una bandera porque la
  // tecla se mantiene apretada: cada vuelta de la ventana suma uno y el bucle los gasta en
  // orden, de modo que el rebobinado va al ritmo del que puede el nucleo y no al de los
  // eventos del sistema de ventanas.
  std::atomic<u32> rewindReq{0};
  std::string      stateMsg;            // ultimo resultado, para telemetria/registro
  std::mutex       stateMsgMutex;
  // Cuenta de partes publicados. El buzon se vacia al EMPEZAR a atender la peticion (hay
  // que leerlo bajo coreMutex y de una vez), asi que ver el buzon vacio no significa que el
  // trabajo este hecho: el cliente que mirara solo eso se llevaria el parte ANTERIOR. Este
  // contador sube una vez, al final, con el mensaje ya escrito; quien espera se apunta el
  // valor antes de pedir y espera a que cambie.
  std::atomic<u64> stateSeq{0};

  // Atiende una peticion de estado pendiente. La llama el bucle de ejecucion al principio
  // de cada vuelta -- tambien estando en pausa, que es cuando mas se guarda.
  auto serviceStateReq() -> void;
  // Deja el RCP quieto de verdad (RDP drenado, tarea de RSP terminada). Es la condicion
  // para fotografiar la maquina; la comparten el estado guardado y el rebobinado.
  auto quiesceRcp() -> void;
  // Punto de reposo del INVITADO: sin tarea de RSP, diario DPC aplicado, horario del RDP
  // cerrado y visible en el reloj actual. Solo ahi se puede fotografiar la maquina sin
  // cambiarla: quiesceRcp ya no tiene nada que adelantar. Ver serviceStateReq.
  auto rcpAtRest() -> bool;
  auto stateReqPending() const -> bool {
    return stateSaveReq.load(std::memory_order_relaxed) >= 0 ||
           stateLoadReq.load(std::memory_order_relaxed) >= 0 ||
           rewindReq.load(std::memory_order_relaxed) != 0;
  }
  u32  stateWaitSlices = 0;   // subtramos corridos esperando reposo para una peticion
  bool rewindDue = false;     // foto de rebobinado pedida por un cierre de campo y aun no hecha
  u32  rewindWaitSlices = 0;

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
