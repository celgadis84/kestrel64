#pragma once
// Loom core — physical memory map and big-endian bus.
//
// RAM-backed regions (RDRAM, SP DMEM/IMEM, PIF RAM) + cartridge ROM, plus the RCP
// MMIO register blocks (MI/SP/DPC/VI/AI/PI/RI/SI) with their DMA engines and
// interrupt aggregation (M2). RSP/RDP task completion is HLE-faked here until the
// LLE cores land, so the boot ROM clears its hardware handshake. Named RAM regions
// are exposed to the telemetry server for MCP inspection.

#include "types.hpp"
#include "../rdp/rdp.hpp"
#include "../rsp/rsp.hpp"
#include <vector>
#include <string>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <atomic>
#include <chrono>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <deque>

namespace kestrel {

// MI interrupt bits (Cause IP2 is asserted when (mi_intr & mi_mask) != 0).
enum MiIntr : u32 { MI_SP=1<<0, MI_SI=1<<1, MI_AI=1<<2, MI_VI=1<<3, MI_PI=1<<4, MI_DP=1<<5 };

// The RCP register file. Plain state; side effects live in Memory::mmioWrite32.
struct Rcp {
  // MI — mi_intr is RMW'd by the RDP/RSP worker threads (raise) and the CPU thread
  // (read/clear), so it is atomic. The raise doubles as the release barrier that
  // publishes the producer's RDRAM writes before the CPU sees the interrupt.
  // Linea de cache propia: el hilo CPU consulta (mi_intr & mi_mask) en cada entrada de bloque
  // JIT, mientras que los workers escriben aqui solo al levantar una IRQ (unas cientos por
  // segundo). Sin el aislamiento comparten linea con dpc_clock/bufbusy/pipebusy, que el worker
  // RDP incrementa por cada span rasterizado, y la consulta barata se vuelve un fallo coherente.
  alignas(64) u32 mi_mode = 0, mi_mask = 0;
  std::atomic<u32> mi_intr{0};
  // RDRAM "init/repeat" mode (MI_MODE bit8 arms it, bit7 clears). While armed, the
  // next CPU store to RDRAM is broadcast/repeated across a byte span. Used by the
  // boot code to blank RDRAM; exposed as an observable quirk by n64-systemtest.
  bool mi_repeat_on = false;
  u32  mi_repeat_len = 0;   // repeat span in bytes (init_length + 1)
  char miPad_[44] = {};     // cierra la linea de MI
  // SP
  u32 sp_mem_addr = 0, sp_dram_addr = 0, sp_rd_len = 0, sp_wr_len = 0;
  // SP_STATUS is one register on one RCP, but in threaded mode two threads update it:
  // the CPU (halt/break/signal control writes) and the RSP worker (HALT|BROKE at BREAK).
  // As a plain u32 those read-modify-writes lose each other's updates — a lost BREAK
  // leaves the CPU polling forever, a lost clear relaunches nothing. Atomic RMW is the
  // only faithful model of a single register seen by both sides.
  std::atomic<u32> sp_status{1};   // start halted (bit0 = HALT)
  u32 sp_semaphore = 0, sp_pc = 0;
  bool sp_intr_on_break = false;  // SP_STATUS interrupt-on-break latch
  // DPC (RDP command buffer). dpc_status is read by the CPU while the RDP worker
  // thread clears GCLK/PIPE_BUSY on SYNC_FULL → atomic.
  u32 dpc_start = 0, dpc_end = 0;
  // CURRENT es el puntero de LECTURA del rasterizador, no el de escritura del que
  // encola: el microcodigo grafico lo consulta para no pisar comandos que el RDP
  // todavia no ha leido. Con el RDP en su propio hilo tiene que reflejar el avance
  // real, asi que lo publica el rasterizador (de ahi el atomico).
  std::atomic<u32> dpc_current{0};
  // Cuantas veces el microcodigo ha mirado DPC_CURRENT. Es el unico mecanismo de
  // control de flujo del FIFO: si sale 0, el ucode no comprueba nada y la seguridad
  // tiene que venir de fuera (el juego esperando el DP done).
  std::atomic<u64> dpcCurReads{0};
  u32 dpc_submitted = 0;   // hasta donde se ha encolado ya (vista del productor)
  std::atomic<u32> dpc_status{0};
  // DPC performance counters (24-bit, free-running). The RDP worker accumulates
  // them while rasterizing and the CPU reads/clears them, so they are atomic.
  // DPC_STATUS write bits 6..9 clear TMEM/PIPE/BUF/CLOCK respectively.
  alignas(64) std::atomic<u32> dpc_clock{0}, dpc_bufbusy{0}, dpc_pipebusy{0}, dpc_tmem{0};
  // El MISMO reloj GCLK que dpc_clock, pero monotono y fuera del alcance del invitado: el
  // juego puede poner a cero los contadores de rendimiento cuando quiera (bits 6..9 de
  // DPC_STATUS) y el regulador no puede depender de un contador que le borran debajo. Es la
  // medida de "cuanto trabajo de RDP se ha hecho ya" contra la que se frena la CPU, igual
  // que Rsp::cyclesRun lo es para el RSP. No va al savestate: es tiempo, no estado.
  std::atomic<u64> rdpGclk{0};
  char dpcPad_[40] = {};
  // DPS (Display Processor Span, 0x0420_0000): el puerto de test con el que la CPU puede
  // leer y escribir el buffer de spans interno del RDP. DPS_TEST_MODE lo abre, BUFTEST_ADDR
  // elige la palabra y BUFTEST_DATA la mueve. El registro de direccion es de 7 bits, asi que
  // la ventana da la vuelta cada 128 palabras. El buffer guarda 32 entradas de span de tres
  // campos: dos palabras completas de 32 bits y una tercera de solo 8 bits (la cobertura);
  // la cuarta ranura de cada entrada no tiene registro fisico detras -- se lee como cero y
  // las escrituras se pierden. Aqui solo se modela el almacenamiento, que es lo que ve la
  // CPU; el rasterizador no lo alimenta (ver docs/GAPS.md).
  u32 dps_tbist = 0, dps_test_mode = 0, dps_buftest_addr = 0;
  u32 dps_span[128] = {};
  // VI
  u32 vi_ctrl = 0, vi_origin = 0, vi_width = 0, vi_intr = 256, vi_current = 0;
  u32 viFlips = 0;    // VI_ORIGIN changed to a different address = displayed buffer swapped
  u32 viFields = 0;   // campos de video emitidos = reloj de tiempo del guest (59.94 Hz)
  u32 dpSyncs = 0;    // RDP SYNC_FULL count = display lists completed
  u32 vi_burst = 0, vi_vsync = 0, vi_hsync = 0, vi_leap = 0, vi_hstart = 0;
  u32 vi_vstart = 0, vi_vburst = 0, vi_xscale = 0, vi_yscale = 0;
  // Medias-lineas que dura un campo de video. VI_V_SYNC guarda ese total (NTSC 525,
  // PAL 625); a 0 el registro aun no esta programado y se asume NTSC. Un solo sitio: el
  // tick del VI, la lectura de VI_V_CURRENT y el alto del framebuffer tenian tres copias
  // de esta expresion con mascaras distintas (0x3ff / 0x3fe) y dos valores por defecto.
  auto viHalflines() const -> u32 { u32 t = vi_vsync & 0x3ff; return t >= 2 ? t : 525; }
  // AI — models a 2-deep DMA buffer FIFO so the audio driver blocks (STATUS
  // FIFO_FULL) instead of generating frames forever, and gets MI_AI when a
  // buffer drains. Without this the audio thread spins in the frame builder and
  // starves the gfx thread (no video).
  u32 ai_dram = 0, ai_len = 0, ai_ctrl = 0, ai_status = 0, ai_dacrate = 0, ai_bitrate = 0;
  u32 ai_fifo_addr[2] = {}, ai_fifo_len[2] = {};
  u32 ai_fifo_count = 0;     // buffers queued (0..2)
  u32 ai_play_remaining = 0; // bytes left in the currently playing buffer
  // Reloj del DAC de audio. El AI no consume "un bufer por campo": consume 4 bytes por
  // muestra a `rate` muestras/s, continuamente, asi que en un campo caben MAS de un bufer
  // cuando el juego los encola cortos (y menos de uno cuando los encola largos). Estos dos
  // campos llevan la cuenta en el mismo reloj que todo lo demas (instrucciones retiradas):
  // `aiLastRetired` es la ultima marca vista y `aiAcc` el resto fraccionario, en unidades de
  // byte*(viFieldInsns*viFieldHzMilli), para que no se pierda ni un byte por redondeo.
  u64 aiLastRetired = 0, aiAcc = 0;
  // PI
  u32 pi_dram_addr = 0, pi_cart_addr = 0, pi_rd_len = 0, pi_wr_len = 0, pi_status = 0;
  u32 pi_bsd[8] = {};
  // RI / SI
  u32 ri_mode = 0, ri_config = 0, ri_select = 0x14, ri_refresh = 0;
  u32 si_dram_addr = 0, si_status = 0;
};

// One inspectable, optionally RAM-backed region of the physical address space.
struct Region {
  std::string name;
  u32   base = 0;          // physical base address
  u32   size = 0;          // byte length
  u8*   data = nullptr;    // backing store, or nullptr for a stubbed MMIO block

  auto contains(u32 addr) const -> bool { return addr >= base && addr - base < size; }
};

struct StateVisitor;

struct Memory {
  friend struct StateVisitor;   // savestate: punteros de reanudacion del FIFO del RDP
  static constexpr u32 RDRAM_SIZE_EXPANDED = 0x0080'0000;  // 8 MB (Expansion Pak)
  static constexpr u32 DMEM_SIZE = 0x1000;                 // 4 KB
  static constexpr u32 IMEM_SIZE = 0x1000;                 // 4 KB
  static constexpr u32 PIFRAM_SIZE = 0x40;                 // 64 B

  GuestBytes rdram;   // alineada a pagina: la importa Vulkan sin copia (ver AlignedAllocator)
  // Noveno bit por byte de RDRAM. Los chips RDRAM del N64 son de 9 bits; el RCP usa esos
  // bits sobrantes como plano OCULTO donde el RDP guarda los 2 bits bajos de la cobertura
  // del pixel (el bit alto viaja en el bit 0 de la palabra RGBA5551). El VI los lee para
  // el filtro de antialias. Un byte por palabra de 16 bits, que es la granularidad con la
  // que el RDP los escribe.
  std::vector<u8> rdramHidden;
  std::vector<u8> dmem;
  std::vector<u8> imem;
  std::vector<u8> pifram;
  std::vector<u8> eeprom;  // EEPROM save backing, joybus channel 4 (size follows saveType)
  std::vector<u8> saveRam; // SRAM / FlashRAM backing, PI domain-2 at physical 0x08000000
  std::vector<u8> rom;     // cartridge ROM image (already byte-normalized to big-endian)

  // --- cartridge save device --------------------------------------------------
  // The N64 backup save is a physical property of the cartridge, not encoded in the
  // ROM header, so (like every emulator) we resolve it per game code. Five device
  // classes exist across the library:
  //   EEPROM 4 kbit  (512 B)   — joybus channel 4, status byte 0x80
  //   EEPROM 16 kbit (2048 B)  — joybus channel 4, status byte 0xC0
  //   SRAM  256 kbit (32 KiB)  — PI domain 2, directly bus-addressable at 0x08000000
  //   SRAM  768 kbit (96 KiB)  — as above, three 32 KiB banks (Dezaemon 3D)
  //   FlashRAM 1 Mbit (128 KiB)— PI domain 2, command/status state machine at 0x08000000
  // The EEPROM type still matters for a libultra quirk: __osEepRead verifies the
  // joybus-reported chip type against the size it expects and, on mismatch, returns
  // WITHOUT releasing the SI access mutex — deadlocking SI. Reporting the true type
  // (below) avoids it. Resolved in loadRom(); KESTREL_SAVETYPE env forces one.
  enum class SaveType { None, Eeprom4k, Eeprom16k, Sram256k, Sram768k, Flash1m };
  SaveType saveType = SaveType::Eeprom16k;   // safe default; overridden per cartridge
  auto isEeprom()  const -> bool { return saveType == SaveType::Eeprom4k || saveType == SaveType::Eeprom16k; }
  auto isSram()    const -> bool { return saveType == SaveType::Sram256k || saveType == SaveType::Sram768k; }
  auto isFlash()   const -> bool { return saveType == SaveType::Flash1m; }
  auto eepromTypeByte() const -> u8 { return saveType == SaveType::Eeprom4k ? 0x80 : 0xC0; }
  auto saveSize()  const -> u32;             // backing size in bytes for the current type
  auto resolveSaveType() -> void;            // pick type + size backing from the loaded ROM

  // On-disk persistence: a battery/flash save survives power-off on real hardware, so
  // the backing is mirrored to a file next to the ROM (.eep/.sra/.fla, the mupen/ares
  // convention). attachSaveFile() derives the path and loads any existing image into the
  // active backing; flushSaveFile() writes it back. No-op when saveType == None.
  std::string saveFilePath;                  // "" until a ROM is attached
  bool        saveDirty = false;             // guest wrote the backing → worth flushing
  auto attachSaveFile(const std::string& romPath) -> void;
  auto flushSaveFile() const -> void;

  // --- Los cuatro puertos de mando --------------------------------------------
  // La consola tiene cuatro conectores y el juego pregunta por los cuatro; un conector
  // vacio contesta "no hay nada" (bit NO_DEVICE), que es distinto de un mando sin
  // accesorio. Todo lo que define un puerto vive junto porque el joybus lo consulta junto.
  //
  // Accesorio (`accessory`): 0 = ranura vacia, 1 = Controller Pak, 2 = Rumble Pak.
  //  - Controller Pak: 32 KiB de RAM con bateria DENTRO del mando, no de la cartuchera —
  //    existe aunque el juego no tenga partida guardada. Se lee y escribe por joybus en
  //    bloques de 32 bytes con CRC de direccion (5 bits) y de datos (8 bits); el SDK
  //    reintenta tres veces y da el pak por ausente si el CRC falla. Se guarda en un .mpk
  //    aparte (convencion mupen/ares), uno por puerto.
  //  - Rumble Pak: no tiene RAM. Se identifica porque la ventana 0x8000 devuelve 0x80 en
  //    los 32 bytes, y el motor se enciende y se apaga escribiendo en 0xC000.
  struct PadPort {
    bool connected = false;          // hay un mando enchufado en este conector
    u8   accessory = 0;              // 0 nada / 1 Controller Pak / 2 Rumble Pak
    u32  buttons = 0;                // byte0<<8|byte1 (A=0x8000 ... C-derecha=0x0001)
    s8   stickX = 0, stickY = 0;     // puerta octogonal del mando: +-85 en eje, +-69 en diagonal
    bool rumble = false;             // ultimo estado del motor que ha pedido el juego
    std::vector<u8> mempak;          // vacia hasta que hace falta; 32 KiB formateados
    std::string     mempakPath;      // "" hasta que se engancha una ROM
    bool            mempakDirty = false;   // el juego escribio: merece la pena volcarlo
  };
  PadPort padPort[4];

  // FlashRAM command/status state machine (PI domain 2). Reads at 0x08000000 return
  // the status/silicon-id doubleword in Status mode or array data in Read mode; the
  // command register at 0x08010000 drives mode changes, erase/write offsets, and the
  // execute (commit) step. A 128-byte page buffer stages writes before commit.
  enum class FlashMode { Status, Read, Erase, Write };
  FlashMode flashMode      = FlashMode::Status;
  u64       flashStatus    = 0x1111'8001'00C2'001Eull;  // Macronix MX29L1101 silicon id
  u32       flashErasePage = 0;                          // 128-byte page index for erase
  u32       flashWritePage = 0;                          // 128-byte page index for write commit
  u8        flashPageBuf[128] = {};                      // staged page for the next write
  auto flashCommand(u32 cmd) -> void;                    // 0x08010000 command register write

  std::vector<Region> regions;  // for telemetry; built by initMap()

  Rcp rcp;                 // RCP MMIO register file + DMA/interrupt state
  SoftRdp softRdp;         // software rasterizer (M3.2), consumes the DPC FIFO
  // Decodificador DEDICADO al modelo de coste. Es una maquina de estados aparte porque el
  // paseo de coste recorre el mismo tramo de FIFO que el que pinta, y compartir estado
  // significaria que el que pinta arranca con el estado que dejo el otro AL FINAL del tramo
  // (scissor, tile, modos): los comandos anteriores al primer SET_SCISSOR del tramo se
  // pintarian con el scissor equivocado. Dos decodificadores, cada uno viendo la misma
  // secuencia completa en orden, es lo unico que conserva la semantica.
  SoftRdp softCost;
  Rsp rsp;                 // low-level RSP interpreter (M3.3), runs microcode

  // --- RCP threading (multihilo rebuild) -------------------------------------
  // Lockstep (default): the RDP rasterizes synchronously inside the DPC_END store,
  // exactly as before — the deterministic path systemtest validates at 0/3721.
  // Threaded (KESTREL_THREADS=1): DPC_END only enqueues a job; a worker thread
  // rasterizes async and raises MI_DP on SYNC_FULL, matching real hardware where
  // the RDP chews the FIFO while the CPU keeps running. Same *results*, async timing.
  enum class RcpMode { Lockstep, Threaded };
  // Se fija una vez al arrancar y luego solo se LEE — pero el hilo CPU la lee en cada entrada
  // de bloque JIT, y sin aislar cae en la misma linea de cache que rdpQueue/rdpMx/rdpBusy, que
  // el worker RDP escribe en cada job. Esa lectura constante-pero-invalidada costaba 12.7% del
  // tiempo total del emulador (hostprof, SM64 threaded+JIT). Linea propia = hit L1 siempre.
  alignas(64) RcpMode rcpMode = RcpMode::Lockstep;
  char rcpModePad_[60] = {};

  // `gen` = generacion del buffer de comandos (ver rdpShadow). Cambia cuando el juego
  // instala un START fresco, o sea otro FIFO.
  struct RdpJob { u32 current, end; bool xbus; u8 gen; u64 ops; };
  std::deque<RdpJob>      rdpQueue;
  u64                     dpJobOps = 0;   // instante de invitado del job en vuelo
  // Ocupacion del command DMA del RDP, en trabajos: los encolados mas el que el worker
  // tenga en vuelo. Es lo que alimenta DMA_BUSY / END_VALID / CMD_BUSY en DPC_STATUS, que
  // es el UNICO flow-control que mira rspq/rdpq de libdragon al reciclar sus dos buffers
  // dinamicos (include/rsp_queue.inc, RSPQCmd_RdpSetBuffer): espera END_VALID=0 para poder
  // dejar un buffer pendiente, y ademas DMA_BUSY=0 cuando el START nuevo es el MISMO que el
  // actual, o sea cuando va a reescribir el buffer que el RDP puede estar leyendo. Sin estos
  // bits el RSP nunca esperaba, reescribia el buffer bajo el rasterizador y el RDP acababa
  // decodificando basura (SET_COLOR_IMAGE addr=0 pintando encima del codigo del guest).
  std::atomic<u32>        dpPending{0};
  // INSTANTANEA DEL FIFO. El command processor real lee los comandos del anillo mientras
  // rasteriza, y el productor no los pisa porque mira DPC_CURRENT... dentro del MISMO
  // buffer. Al instalar uno nuevo (START fresco) el juego ya no mira nada, y con el RDP en
  // su hilo el worker todavia puede llevar tramos del anterior encolados: acababa leyendo
  // comandos que el microcodigo ya habia reescrito (ver docs/PD-DERAIL.md). Al encolar
  // copiamos los bytes del tramo -- que el productor YA escribio antes del kick, asi que es
  // un instante de lectura legal para el hardware -- y el rasterizador consume la copia.
  // Dos generaciones alternas bastan: dentro de una, el flow-control del propio juego
  // (DPC_CURRENT, que publicamos honesto) impide que se pise lo no consumido; entre una y
  // la siguiente, la copia vive en el otro buffer. Se reservan al vuelo (tamano RDRAM).
  std::vector<u8>         rdpShadow[2];
  u8                      rdpGen = 0;          // solo lo toca el productor
  std::mutex              rdpMx;
  std::condition_variable rdpCv;
  std::thread             rdpWorker;
  bool                    rdpStop = false;      // worker exit flag (guarded by rdpMx)
  // true SOLO mientras el worker del RDP esta dormido en rdpCv. Con el mutex cogido, un
  // productor que ve false sabe que el worker sigue en su bucle y volvera a mirar la cola:
  // puede saltarse el notify_all, que con esperador es una llamada al kernel. SM64 emite
  // ~38.000 DPC_END por segundo, asi que ese ahorro no es cosmetico.
  bool                    rdpWaiting = false;   // guarded by rdpMx
  std::atomic<bool>       rdpBusy{false};        // true while a job is queued or running

  // RSP worker: in Threaded mode a CPU SP-release runs the whole microcode task to
  // BREAK on this thread (it raises MI_SP itself), instead of the CPU interleaving
  // rsp.step(). The producer/consumer contract is the same as the RDP: the game
  // waits on the SP interrupt before reading the RSP's output, so byte-level
  // coherency between threads isn't needed — the interrupt is the release barrier.
  std::mutex              rspMx;
  std::condition_variable rspCv;
  // Numero de hilos dormidos en rspCv. El worker publica su progreso cada 8 K instrucciones
  // de microcodigo (Rsp::step) y notificaba SIEMPRE: con esperador eso es una llamada al
  // kernel, y sin esperador es trabajo tirado. Se incrementa con rspMx cogido ANTES de
  // comprobar el predicado, y el notificador publica cyclesRun ANTES de leerlo, las dos con
  // orden secuencial: es el patron de Dekker, no hay wakeup perdido. El wait_for de 500 us
  // del regulador sigue ahi como red de seguridad.
  std::atomic<int>        rspWaiters{0};
  std::thread             rspWorker;
  bool                    rspStop = false;      // worker exit flag (guarded by rspMx)
  bool                    rspKick = false;      // pending run request (guarded by rspMx)
  bool                    rspIdleWaiting = false;  // worker dormido en rspCv (guarded by rspMx)
  std::atomic<bool>       rspBusy{false};        // true from kick until the task breaks

  // Worker occupancy telemetry. Wall time actually spent inside a job, so the
  // heartbeat can say which domain is the long pole instead of guessing from
  // host-thread CPU time. Relaxed: read-only diagnostics, never a control input.
  std::atomic<u64>        rdpBusyNs{0}, rspBusyNs{0};
  // Sueno del aparcamiento del RSP (rspParkWait). Cae DENTRO de rsp.step(), asi que sin
  // descontarlo rspBusyNs cuenta como "trabajo" un hilo dormido: con el aparcamiento puesto
  // DK64 marcaba rsp 92% de ocupacion con 2% de CPU real, y "rsp X Mips busy" salia dividido
  // por ese tiempo muerto. Se descuenta en rspWorkerLoop.
  std::atomic<u64>        rspParkNs{0};
  std::atomic<u64>        rdpJobsRun{0}, rspJobsRun{0};
  std::atomic<u64>        cpuWaitNs{0};   // CPU thread blocked on an RCP worker
  // Tiempo de CPU REALMENTE consumido por cada worker, frente al tiempo de pared que ya
  // miden rspBusyNs/rdpBusyNs. La diferencia entre los dos es la unica forma de separar
  // "emulamos despacio" de "al hilo no le dan nucleo": si un worker esta 80% de la pared
  // dentro de un trabajo pero solo ha gastado 40% de CPU, el problema es el planificador
  // (o el hermano SMT), no el emulador. Los rellena sampleWorkerCpu() desde el heartbeat.
  void*                   rspThreadH = nullptr;   // HANDLE duplicado del worker de RSP
  void*                   rdpThreadH = nullptr;   // idem del RDP
  std::atomic<u64>        rspCpuNs{0}, rdpCpuNs{0};
  // Y lo mismo para el hilo de CPU. Es el que separa "emular cuesta" de "el hilo esta
  // dormido/esperando": un hilo de CPU al 100% de pared y al 25% de CPU no esta emulando.
  void*                   cpuThreadH = nullptr;
  std::atomic<u64>        cpuCpuNs{0};
  auto sampleWorkerCpu() -> void;

  // --- ocupacion del bus de RDRAM -------------------------------------------
  // La consola tiene UN bus de RDRAM (2 chips de 9 bits a 250 MHz DDR = 4,5 Gbit/s =
  // 562,5 MB/s de pico) que el RCP arbitra entre siete maestros. En muchos juegos el palo
  // largo no es ningun chip sino ese bus, asi que el medidor cuenta BYTES por maestro y
  // los divide entre el pico POR SEGUNDO DE TIEMPO DE INVITADO: el numero sale igual
  // corra el emulador rapido o despacio, porque es una propiedad del juego.
  // Es un SUELO, no una cota: no se cuenta el refresco de RDRAM, ni el noveno bit
  // (paridad/cobertura oculta), ni las lecturas del latch del cartucho (que no tocan
  // RDRAM), ni los accesos no cacheados que emite el dynarec por su camino rapido.
  static constexpr double kRdramPeakBps = 562'500'000.0;
  std::atomic<u64>        ramBytesRsp{0};   // motor de DMA del SP: DMEM/IMEM <-> RDRAM
  std::atomic<u64>        ramBytesRdp{0};   // color/z por chunk del buffer de tramo + TMEM
  std::atomic<u64>        ramBytesVi{0};    // barrido de video: el framebuffer entero por campo
  std::atomic<u64>        ramBytesPi{0};    // cartucho/save <-> RDRAM
  std::atomic<u64>        ramBytesAi{0};    // buffers de audio hacia el DAC
  std::atomic<u64>        ramBytesSi{0};    // bloque de 64 B del PIF
  // Suma de todos los maestros MENOS la CPU, que lleva sus bytes en un contador liso
  // (CPU::ramCpuBytes) por estar en el camino caliente del interprete.
  auto ramBytesRcp() const -> u64 {
    return ramBytesRsp.load(std::memory_order_relaxed) + ramBytesRdp.load(std::memory_order_relaxed)
         + ramBytesVi .load(std::memory_order_relaxed) + ramBytesPi .load(std::memory_order_relaxed)
         + ramBytesAi .load(std::memory_order_relaxed) + ramBytesSi .load(std::memory_order_relaxed);
  }

  auto startRcpThreads() -> void;   // spawn workers if rcpMode==Threaded (idempotent)
  auto stopRcpThreads()  -> void;   // join workers on shutdown
  auto rdpSubmit(u32 current, u32 end, bool xbus) -> void;  // enqueue (threaded)
  auto rdpDrain() -> void;          // block until the RDP queue is fully consumed
  // preCosted = el modelo de coste y el horario de invitado de este tramo ya los hizo el
  // hilo que lo lanzo (dpScheduleSpan). Entonces aqui SOLO se pinta.
  auto rdpRunJob(u32 current, u32 end, bool xbus, const u8* cmdSrc, bool preCosted = false) -> void;
  auto rdpSnapshot(u32 current, u32 end) -> void;   // copia el tramo a rdpShadow[rdpGen]
  auto rdpPublishCurrent(u32 fallback) -> void;  // rasterize + DP bookkeeping
  auto rspAwaitIdle() -> void;      // block until the RSP worker has published its task result
  // Parte periodico mientras una de esas esperas se alarga (ver awaitReporting en el .cpp).
  auto reportRspStall(unsigned round, const char* esperando) -> void;
  auto rspSubmitKick() -> void;     // wake the RSP worker to run the armed task (threaded)

  // --- fin de tarea del RCP en tiempo de INVITADO ---------------------------------------
  // En Threaded los workers terminaban cuando terminaban en tiempo de PARED y levantaban
  // MI_SP / MI_DP alli mismo. El invitado veia la interrupcion en una instruccion retirada
  // distinta en cada corrida y el juego divergia: DK64 daba 72/70/73/73 intercambios de buffer
  // en cuatro corridas identicas mientras Lockstep clavaba 74 (docs/GAPS.md 3b).
  //
  // El coste de las dos tareas YA esta modelado y es monotono -- Rsp::cyclesRun para el RSP y
  // rcp.rdpGclk para el RDP, los dos escritos por un unico hilo -- y el ratio a instrucciones-
  // equivalentes de CPU ya existe (paceCpuNum/paceCpuDen, el mismo del interleave de Lockstep).
  // Asi que el fin de tarea se ARMA con un plazo en el mismo reloj que todos los demas plazos
  // del emulador, cartNow(), y lo publica el HILO DE CPU cuando ese reloj llega. Misma regla
  // que el plazo del SI: un plazo nace y muere en el mismo reloj.
  //
  // El instante de lanzamiento lo toma siempre el hilo de CPU (rspSubmitKick / rdpSubmit), que
  // es donde el invitado escribe el registro; el coste lo mide el worker sobre su propio
  // contador. Ninguno de los dos depende de la velocidad del anfitrion.
  //
  // Solo se aplica en Threaded: en Lockstep el fin ya cae en un punto determinista porque lo
  // ejecuta el propio hilo de CPU. KESTREL_RCPDEADLINE=0 lo apaga para bisecar.
  static auto rcpDeadlineOn() -> bool;
  std::atomic<u32> rcpPend{0};   // bit0 = fin de SP armado, bit1 = fin de DP armado,
                                 // bit2 = trabajo de RDP en vuelo (ver dpBarrierAt),
                                 // bit3 = barrera del RSP activa (ver spBarrierAt)
                                 // bit4 = escrituras DPC del RSP por aplicar (ver dpLogPush)
  // DIARIO DE ESCRITURAS DPC DEL RSP (solo Threaded). Una escritura del microcodigo a DPC
  // es estado compartido con la CPU, que puede ir por detras en tiempo de invitado y aun
  // tener escrituras DPC suyas anteriores. Antes el RSP se paraba en cada una hasta que la
  // CPU llegaba a su instante (cita spReadSync): 351k citas en junkrunner64, el 31 % de la
  // pared. Ahora el RSP apunta {instante, registro, valor} y sigue; la CPU la aplica en SU
  // hilo justo al llegar a ese instante (rcpRetire, con el plazo en rcpDueIn), o antes de
  // tocar ella misma un registro DPC. El orden de invitado sale identico al de la cita: la
  // barrera del SP no deja a la CPU pasar del reloj publicado del RSP, y el RSP publica el
  // reloj exacto antes de apuntar, asi que el instante apuntado nunca queda por detras de
  // la CPU. Todo lo del RSP que puede ver el efecto de una escritura pendiente (lecturas de
  // DPC, DMA) espera antes a que el diario quede vacio (dpLogWait).
  // KESTREL_DPLOG=0 vuelve a la cita.
  struct DpLogEnt { u64 at; u32 reg; u32 v; };
  static constexpr u32 kDpLogN = 1u << 12, kDpLogM = kDpLogN - 1;
  DpLogEnt dpLog[kDpLogN]{};
  std::atomic<u32> dpLogHead{0}, dpLogTail{0};   // head = consumidor, tail = RSP
  std::mutex dpLogMx;                            // un solo aplicador a la vez
  std::atomic<bool> rspLogWait{false};           // el RSP esta parado en dpLogWait
  std::atomic<u64> dpLogPushes{0}, dpLogWaits{0};
  std::atomic<u32> dpLogWaives{0};
  // Escrituras del RSP a SP_STATUS en el mismo diario (reg = 8|4). El microcodigo cambia
  // SIG0..SIG7 con la CPU por detras en tiempo de invitado; aplicadas al registro en el acto,
  // la vuelta en que el bucle de sondeo de la CPU las veia la decidia el anfitrion (junkrunner64:
  // SIG4->SIG3 una lectura antes o despues segun la corrida, y con ello el statehash).
  // spLogPend = entradas SP aun sin aplicar (el RSP no puede leer SP_STATUS con ellas dentro);
  // spLogCrit = las que tocan INTR_ON_BREAK, que el RSP consulta en su BREAK.
  std::atomic<u32> spLogPend{0}, spLogCrit{0};
  static auto dpLogOn() -> bool;
  auto dpLogPending() const -> bool {
    return dpLogHead.load(std::memory_order_acquire) != dpLogTail.load(std::memory_order_acquire);
  }
  auto dpLogPush(u64 at, u32 reg, u32 v) -> void;   // SOLO hilo del RSP
  auto dpLogApply(u64 upTo) -> void;                // aplica lo fechado hasta upTo
  auto dpLogWait(u64 now, bool clock) -> void;      // SOLO hilo del RSP
  auto rspDmaRdpWait(u32 lo, u32 hi) -> void;          // SOLO hilo del RSP (ver spDma)
  std::atomic<u64> rspDmaRdpWaits{0};
  // Zona que puede estar pintando el RDP (ver rspDmaRdpWait): SoftRdp::kWrSlots intervalos
  // publicados bajo seqlock (dpWrSeq impar = a medias).
  std::atomic<u32> dpWrLo[4] = {~0u, ~0u, ~0u, ~0u}, dpWrHi[4] = {0, 0, 0, 0};
  std::atomic<u32> dpWrSeq{0};
  auto dpWrReset() -> void;
  u64  spKickOps = 0, spKickCycles = 0;   // instante de invitado / ciclos de RSP al lanzar
  // Flanco del reloj del RCP en el que cae el lanzamiento: rcpOpsToCycles(spKickOps). El reloj
  // del RCP es ABSOLUTO (62,5 MHz libre desde el arranque), asi que el ciclo j de una tarea no
  // cae j ciclos despues del CLEAR_HALT sino en el flanco spKickEdge + j. Lockstep y Threaded
  // miden los dos contra este flanco (ver spKickAt y System::stepCpu) y por eso ven MI_SP en la
  // misma instruccion.
  u64  spKickEdge = 0;
  auto spMarkKick() -> void {
    spKickOps    = cartNow();
    spKickEdge   = rcpOpsToCycles(spKickOps);
    spKickCycles = rsp.cyclesRun.load(std::memory_order_relaxed);
  }
  u64  spDoneAt  = 0, dpDoneAt = 0;       // plazos, en cartNow()
  auto spEndArm(u64 cyclesUsed) -> void;             // desde el worker del RSP
  auto dpEndArm(u64 kickOps, u64 gclkUsed) -> void;
  auto dpEndArmAt(u64 at) -> void;   // plazo de fin de tarea del RDP en instante absoluto
  auto rcpRetire() -> void;               // publica lo vencido (SOLO hilo de CPU)
  auto rcpFlushPending(u32 bits) -> void; // publica ya, sin mirar el plazo
  // BARRERA DE INVITADO DEL RDP -- lo que hace que el plazo de arriba no llegue nunca tarde.
  //
  // Armar el fin de tarea con un plazo no basta por si solo: el worker mide el coste cuando
  // TERMINA, y si para entonces el reloj del invitado ya ha pasado del plazo, el plazo nace
  // vencido y MI_DP se publica donde el anfitrion haya llegado. Medido: 2 a 4 de cada 55
  // tareas de DK64 nacian vencidas, y dos campos de video despues la partida ya era otra.
  //
  // Con el paseo de coste DELANTE del dibujado (ver rdpRunJob) el coste del tramo entero se
  // conoce nada mas empezar la tarea, asi que se puede publicar el instante de invitado en
  // que esa tarea termina ANTES de hacerla. La CPU corre libre hasta ese instante y ahi se
  // para hasta que el trabajo existe de verdad. Es la semantica del bus: el invitado no
  // puede ver tiempo posterior al fin del trabajo del RDP antes de que ese fin ocurra.
  //
  // ~0 = no hay tarea en vuelo. Mientras la hay vale, de menos a mas preciso: el instante de
  // lanzamiento (encolada, aun sin coste) y luego lanzamiento + coste ya cobrado.
  // KESTREL_DPBARRIER=0 la apaga para bisecar.
  // El horario del trabajo en vuelo, en tiempo de invitado. Lo escribe el worker al sacar
  // el trabajo de la cola (seqlock: dpSchedGen impar = escritura en curso) y lo lee cualquiera.
  //
  //   dpBarrierAt() = dpJobOps0 + ops(rdpGclk - dpJobGclk0)
  //
  // o sea: hasta que instante de invitado ha trabajado el RDP DE VERDAD. Es la gemela exacta
  // de spBarrierAt() y, como aquella, no hace falta publicar nada aparte porque rdpGclk ya es
  // monotono y ya lo escribe un solo hilo. Con parallel-RDP el paseo de coste va delante del
  // dibujado, asi que rdpGclk salta al coste completo nada mas empezar y la barrera vale ya el
  // instante final: la GPU dibuja solapada y la CPU solo se para al final si llega antes. Con
  // SoftRDP en hilo el rasterizador cobra segun pinta, asi que la barrera sigue el avance real
  // y el solape se conserva igual -- con el valor publicado de antes (el instante de
  // LANZAMIENTO) ese camino se quedaba serializado todo el trabajo.
  // Instante de invitado en que el motor termina TODO lo que se le ha mandado. Lo escribe
  // dpScheduleSpan (siempre con rdpMx cogido, o sea un solo escritor a la vez) y lo lee
  // cualquiera. Un solo u64 atomico: no hace falta seqlock porque ya no hay pareja de campos
  // que leer coherente -- el arranque de cada tramo vive en el anillo.
  std::atomic<u64> dpSchedEnd{0};
  // HORARIO DE TRABAJOS DEL RDP EN TIEMPO DE INVITADO.
  //
  // Todo lo que el invitado puede ver del motor -- ocupado/libre, DPC_CURRENT, END_VALID -- se
  // deduce de este anillo y de SU propio instante, nunca del estado del anfitrion. Cada trabajo
  // deja dos marcas: la de apertura la pone el hilo de CPU al lanzarlo (que ya es un instante
  // de invitado) y la de cierre la pone el worker cuando el modelo de coste sabe cuanto ha
  // durado. Un lector con reloj `now` cuenta cuantos cierres han pasado ya y de ahi sale todo.
  //
  // Por que hace falta: antes esos tres bits salian de `dpPending` (la cola del anfitrion) y
  // de `rcp.dpc_current` (lo que la GPU llevaba consumido en tiempo de PARED). El microcodigo
  // grafico sondea DPC en bucle para saber cuanto FIFO puede reutilizar, asi que el numero de
  // vueltas de ese bucle -- y por tanto el coste de la tarea de RSP, y por tanto cuando cae
  // MI_SP -- dependia de lo rapido que fuese el PC. Medido en SM64: la primera corrida despues
  // de compilar (cache frio) divergia de las siguientes en la partida entera.
  //
  // Los cierres son monotonos (el motor termina en orden y el instante de cierre nunca
  // retrocede), asi que contar los ya pasados es recorrer el anillo hacia atras.
  //
  // El anillo va por TRAMO DE FIFO LANZADO, no por trabajo de la cola del anfitrion. Son cosas
  // distintas: la cola une dos escrituras de DPC_END consecutivas en un solo trabajo para no
  // despertar al worker dos veces, y esa union depende de si el worker ya habia sacado el
  // anterior -- o sea, de lo rapido que vaya el PC. Mientras el anillo se llenaba desde la
  // cola, el numero de tramos que el invitado veia, sus fronteras de DPC_CURRENT y los bits
  // DMA_BUSY/END_VALID cambiaban con el anfitrion: era la ultima fuga de determinismo (DK64
  // divergia en el campo 192, justo donde arranca el 3D). Ahora cada escritura de DPC_END
  // anota su tramo pase lo que pase, y unir o no unir es solo reparto de trabajo interno.
  static constexpr u32 kDpRingN = 256;
  static constexpr u32 kDpRingM = kDpRingN - 1;
  // Tramos lanzados FUERA DE ORDEN DE INVITADO: su instante de lanzamiento cae antes del
  // arranque del tramo anterior. Solo puede pasar cuando la CPU y el RSP lanzan a la vez y
  // el orden lo decide quien coge rdpMx primero, que es orden de PARED. Contarlos dice si
  // hace falta ordenar el FIFO por reloj de invitado o si la ventana no existe en la practica.
  std::atomic<u32> dpOoo{0}, dpOooC{0}, dpOooR{0};
  u64 dpLastKick = 0;   // ultimo instante de lanzamiento visto (bajo rdpMx)
  // Instante de invitado MAS ALTO al que ya se ha consultado el horario. Si despues
  // entra un tramo con lanzamiento anterior a ese instante, la respuesta que se dio ya
  // era falsa: el lector vio un FIFO al que le faltaba trabajo suyo.
  std::atomic<u64> dpMaxQuery{0};
  std::atomic<u32> dpStale{0}, dpStaleC{0}, dpStaleR{0};
  std::atomic<u64> dpSubSeq{0};    // tramos lanzados y YA fechados (escritores: CPU y RSP, bajo rdpMx)
  std::atomic<u64> dpCompSeq{0};   // trabajos pintados por el anfitrion (solo detector de cambio)
  u64 dpJobStartG[kDpRingN]{};     // instante de invitado de arranque
  u64 dpJobEndG[kDpRingN]{};       // instante de invitado de cierre
  u64 dpJobKickG[kDpRingN]{};      // instante de invitado en que se escribio DPC_END
  // SENALES DE SP_STATUS EN TIEMPO DE INVITADO (Threaded). La CPU va por delante del RSP en
  // tiempo de invitado (hasta kRdvLead), y sus escrituras de SIG0..SIG7 caian en el registro
  // en tiempo de pared. El microcodigo sondea SIG0 (osSpTaskYield) en bucle, asi que el ciclo
  // en que cedia dependia del anfitrion: DK64 acababa una tarea en 55876 ciclos en Lockstep y
  // en 66060 o 201692 en Threaded. Cada escritura de la CPU apunta aqui desde que instante
  // existe y que bits tenian antes; el RSP deshace las que aun son futuras para el.
  // stamp = instante en que el RSP la ve (redondeado al grano), raw = flanco real de la
  // escritura, flags: 1 = llevaba CLEAR_HALT con el RSP en marcha, 2 = llevaba CLEAR_BROKE.
  struct SpSigWr { u64 stamp; u64 raw; u32 mask; u32 prev; u32 flags; };
  static constexpr u32 kSpSigN = 32;
  SpSigWr spSigRing[kSpSigN]{};
  u32 spSigHead = 0, spSigCount = 0;       // bajo spSigMx
  std::mutex spSigMx;
  auto spStatusForRsp(u64 now) -> u32;     // SOLO quien ejecuta el RSP
  // Grano de visibilidad de las senales que escribe la CPU para el RSP (potencia de 2, en ops
  // de CPU). 1 = exacto. Ver Memory::spSigQuant.
  static auto spSigQuant() -> u64;
  auto spSigAtKick() -> void;              // lanzamiento: lo pendiente pasa a su instante real
  auto spLateClearHalt(u64 now) -> u32;    // SOLO quien ejecuta el RSP, en BREAK (ver rsp.cpp)
  auto spReadSync(u64 now) -> void;        // SOLO hilo del RSP: la CPU llega a `now`, sin adelanto
  std::atomic<u32> spRdv{0}, spRdvWaives{0}, spLateHalts{0};
  // La CPU esta dentro de dpBarrierWait: la retiene el RDP (trabajo del anfitrion, p. ej. la GPU
  // compilando pipelines), no el RSP. Una cita del RSP que espera a la CPU no puede soltarse
  // por reloj de pared mientras dure: el RDP no depende del RSP, asi que no hay bloqueo mutuo
  // que romper, y soltarla deja al RSP leer por delante de la CPU -- con eso volvia a entrar el
  // anfitrion. La barrera del RDP tiene su propio salvavidas (kBarrierMaxWait).
  std::atomic<bool> cpuDpBarWait{false};
  // Hilo de CPU dentro de una espera sobre un worker del RCP (rspPace, rdpPace, rdpDrain,
  // rspAwaitIdle, spBarrierWait). Solo ahi puede una cita del RSP ser un bloqueo mutuo; ver
  // rdvWaiveDue.
  std::atomic<u32> cpuRcpWait{0};
  struct RcpWaitMark {
    std::atomic<u32>& n;
    explicit RcpWaitMark(std::atomic<u32>& c) : n(c) { n.fetch_add(1, std::memory_order_release); }
    ~RcpWaitMark() { n.fetch_sub(1, std::memory_order_release); }
  };
  auto rdvWaiveDue(bool& timing, std::chrono::steady_clock::time_point& t0,
                   std::chrono::steady_clock::time_point& t1) -> bool;
  std::atomic<bool> dpLogFlush{false};   // System::quiesceRcp: aplicar el diario sin esperar
  u32 dpJobAddr[kDpRingN]{};       // DPC_CURRENT al abrir
  u32 dpJobEndAddr[kDpRingN]{};    // DPC_CURRENT al cerrar
  // Instante de invitado en que ARRANCA un trabajo lanzado en `ops`. El motor es UNO: un
  // buffer encolado detras de otro no puede empezar antes de que el anterior drene, asi que
  // el arranque es el maximo entre su propio lanzamiento y el cierre del anterior. Sin este
  // maximo el plazo de fin de un trabajo corto encolado detras de uno largo nacia ANTES del
  // instante en que la CPU ya estaba parada por el largo -- plazo vencido de nacimiento, y
  // con el toda la cadena de MI_DP se volvia a publicar en tiempo de anfitrion.
  auto dpJobStartAt(u64 ops) const -> u64 {
    u64 e = dpSchedEnd.load(std::memory_order_relaxed);
    return e > ops ? e : ops;
  }
  // Cuantos trabajos ha cerrado el motor YA, en el reloj del que pregunta.
  // Todos los tramos lanzados tienen ya fecha de cierre (se calcula al lanzarlos), asi que
  // la cuenta arranca en dpSubSeq: no hay que esperar a que el anfitrion pinte nada.
  // Cuantos tramos EXISTEN ya para ese reloj: los que se lanzaron en `now` o antes. El FIFO
  // tiene dos escritores en hilos distintos (CPU y RSP) y cualquiera de los dos puede ir por
  // delante del otro en tiempo de invitado; un tramo con lanzamiento posterior al lector es
  // una escritura que en la consola todavia no ha ocurrido y no puede verse en DPC_STATUS ni
  // en DPC_CURRENT. Contando dpSubSeq a pelo, el RSP de Threaded veia el FIFO ocupado unas
  // instrucciones antes que en Lockstep y la tarea de DK64 acababa 6 ciclos antes.
  auto dpVisibleAt(u64 now) const -> u64 {
    u64 c = dpSubSeq.load(std::memory_order_acquire);
    for(u32 i = 0; i < kDpRingN && c > 0; ++i) {
      if(dpJobKickG[(c - 1) & kDpRingM] <= now) break;
      --c;
    }
    return c;
  }
  auto dpCompletedAt(u64 now) const -> u64 {
    u64 c = dpSubSeq.load(std::memory_order_acquire);
    for(u32 i = 0; i < kDpRingN && c > 0; ++i) {
      if(dpJobEndG[(c - 1) & kDpRingM] <= now) break;
      --c;
    }
    return c;
  }
  // Hasta que instante de invitado tiene trabajo el motor. Es un valor FIJO desde que se
  // lanza el tramo: ya no hay nada que "esperar a que se publique", porque el coste se cobra
  // en el mismo hilo que escribe DPC_END y antes de que el anfitrion pinte un solo pixel.
  auto dpBarrierAt() const -> u64 { return dpSchedEnd.load(std::memory_order_acquire); }
  // El motor esta drenado en `now` si el ultimo tramo con fecha ya cerro. Entonces
  // DPC_CURRENT vale una constante -- la direccion de cierre de ese tramo -- y no puede
  // cambiar hasta que la CPU meta otro. Leer un dpSchedEnd atrasado aqui es inofensivo:
  // cualquier tramo que entre despues nace con kick = cartNow() >= now, o sea que tambien
  // cierra despues de `now`.
  auto dpDrainedAt(u64 now) const -> bool {
    return dpSchedEnd.load(std::memory_order_acquire) <= now;
  }
  // Fecha un tramo de FIFO: le pasa el modelo de coste por encima, le pone instante de
  // arranque y de cierre en el anillo y adelanta dpSchedEnd. SOLO con rdpMx cogido.
  auto dpScheduleSpan(u32 current, u32 end, bool xbus, const u8* src, u64 kick) -> void;
  // El RDP esta ocupado, en tiempo de INVITADO, si quedan trabajos sin cerrar para ese reloj.
  auto dpBusyAt(u64 now) const -> bool {
    return dpSubSeq.load(std::memory_order_acquire) > dpCompletedAt(now);
  }
  static auto dpBarrierOn() -> bool;
  auto dpBarrierWait(u64 now) -> void;   // SOLO hilo de CPU
  auto dpSpinUntil(u64 bar, u64 comp) -> bool;   // giro previo a dormir en rdpCv
  u64  dpBarWaivedAt = ~0ull;            // salvavidas: barrera soltada para este valor
  // Lecturas de DPC evaluadas en el reloj de QUIEN lee: el hilo de CPU pasa cartNow() y el
  // del RSP su propio instante (rspGuestNow), que es lo que hace que el microcodigo vea
  // siempre lo mismo corra el anfitrion como corra.
  static auto dpGuestOn() -> bool;       // KESTREL_DPGUEST=0 vuelve al modelo de anfitrion
  // `who`: 0 = hilo de CPU, 1 = hilo del RSP (solo elige contador, no cambia el valor).
  auto dpcCurrentFor(u64 now, u32 who) -> u32;
  auto dpcStatusFor(u64 now, u32 who) -> u32;
  auto rdpAwaitGuest(u64 now) -> void;   // esperar a que el RDP alcance ese instante

  // CITA DE LECTURA DEL FIFO -- el RSP no puede leer el estado del RDP en un instante de
  // invitado al que la CPU todavia no ha llegado. El FIFO tiene DOS escritores y el otro es
  // ella: el microcodigo grafico gira en el control de flujo de F3DEX2 mirando DPC_CURRENT
  // y quien lo desbloquea es la CPU instalando el siguiente buffer de comandos.
  //
  // Medido con el contador `stale=` de [det] sobre DK64, 300 campos: los 42-47 tramos que
  // instala la CPU llegaban TODOS (C.../R0) despues de que el microcodigo ya hubiera
  // preguntado por instantes posteriores. O sea que cada respuesta dada en esa ventana se
  // calculo contra un FIFO al que le faltaba trabajo que le pertenecia. Cuantas vueltas daba
  // el bucle de espera antes de que la escritura aterrizase era una carrera de tiempo de
  // pared entre los dos hilos, y esa carrera era TODA la divergencia que quedaba: `ret`/`ops`
  // salen identicos campo a campo y lo unico que se mueve es en QUE campo cae la
  // interrupcion de SP/DP.
  //
  // La cura es la simetrica de la barrera del SP. Antes de responder, esperar a que la CPU
  // haya retirado hasta ese instante. No puede bloquear: la barrera del SP deja a la CPU
  // llegar exactamente hasta donde el RSP ha publicado, asi que basta con publicar el reloj
  // exacto antes de esperar. La espera es activa porque el retardo tipico es de decenas de
  // instrucciones de CPU; dormirse ahi convertiria cada cita en un viaje de milisegundos.
  // KESTREL_DPRDV=0 la apaga para bisecar.
  static auto dpRdvOn() -> bool;
  auto dpReadAhead(u64 now) const -> bool {
    return rcpMode == RcpMode::Threaded && cartNow() < now;
  }
  auto dpReadSync(u64 now) -> void;     // SOLO hilo del RSP
  // ADELANTO DURANTE LA CITA. Mientras el RSP esta parado en dpReadSync no puede terminar
  // su tarea, asi que la barrera del SP no tiene nada que proteger ahi: lo unico que hace es
  // pegar a la CPU al instante exacto del RSP, y entonces la siguiente vuelta del bucle de
  // espera vuelve a pedir cita. Seis millones de citas por corrida, una cada ~10 instrucciones
  // de microcodigo. Dandole a la CPU un adelanto FIJO en tiempo de invitado mientras dura la
  // cita, las ~270 vueltas siguientes ya se responden sin esperar a nadie.
  //
  // Adelantarse NO puede romper la lectura del FIFO: la condicion que hay que cumplir es
  // `instante leido <= cartNow()`, y el adelanto la cumple con MAS holgura, no con menos. Un
  // tramo que instale la CPU durante el adelanto nace con `kick` POSTERIOR al instante del
  // RSP, o sea delante de todo lo que ya se ha respondido. Lo unico que el adelanto puede
  // estropear es que el plazo de fin de tarea del SP nazca vencido, y eso se ve en `spArm`
  // (segunda cifra) de [det]: tiene que seguir siendo 0.
  static constexpr u64 kRdvLead = 49152;
  // Grano de la cita. Lo OBLIGATORIO es llegar a `now`; pedir un poco mas es gratis (la
  // condicion de correccion es `instante leido <= cartNow()`, y pasarse la cumple mejor) y
  // ahorra las ~200 vueltas siguientes del bucle de espera. Tiene que caber en kRdvLead,
  // que es hasta donde la barrera del SP deja llegar a la CPU mientras dura la cita.
  static constexpr u64 kRdvGrain = 32768;
  std::atomic<u64> rspRdvAt{0};          // instante del RSP mientras esta en la cita (0 = no)
  std::atomic<u32> rspSyncWait{0};       // RSP en spReadSync esperando a la CPU (ver rspPace)
  auto spBarrierEff() const -> u64 {
    // Con el RSP aparcado la barrera no protege nada: no hay evento que pueda nacer tarde
    // porque el RSP no puede generar ninguno hasta que la CPU le eche trabajo.
    // Con el RSP aparcado la barrera se abre, pero NO del todo. Si se abriera entera y el
    // aparcamiento acabara por salvavidas, lo lejos que hubiera llegado la CPU seria tiempo de
    // anfitrion puro, y con el se iria el determinismo. Con tope, hasta el salvavidas deja a
    // los dos hilos en un instante de invitado exacto.
    // Y en cuanto hay fecha de despertar publicada el aparcamiento SE ACABO, aunque el
    // worker todavia no se haya enterado: la puso el propio hilo de CPU al archivar el tramo
    // (dpScheduleSpan), asi que desde ese instante el RSP va a reanudar justo donde esta la
    // CPU y la barrera vuelve a mandar. Sin esto la CPU seguia corriendo libre durante toda
    // la latencia de despertar del anfitrion, la tarea acababa en un instante ya rebasado y
    // la fecha de fin del SP nacia tarde -- una vez cada dos o tres corridas de DK64, que es
    // justo la forma que tenia la divergencia que quedaba.
    if(u64 pk = rspPark.load(std::memory_order_acquire))
      if(!rspParkWake.load(std::memory_order_acquire)) return rspParkCap.load(std::memory_order_acquire);
    return spBarrierAt() + (rspRdvAt.load(std::memory_order_acquire) ? kRdvLead : 0);
  }
  // Aparcamiento del RSP en la espera del FIFO. Ver Memory::rspParkWait.
  std::atomic<u64> rspPark{0};       // instante de invitado en que quedo aparcado (0 = no)
  std::atomic<u64> rspParkWake{0};   // lanzamiento que lo despierta (0 = aun ninguno)
  std::atomic<u64> rspParkCap{0};    // hasta donde puede correr la CPU con el RSP aparcado
  std::atomic<u32> rspParks{0}, rspParkWv{0}, rspParkMiss{0};
  static constexpr u64 kParkLead = 1ull << 24;   // cuanto puede adelantarse la CPU con el RSP aparcado
  std::mutex parkMx;
  std::condition_variable parkCv;
  // seq0 = dpSubSeq visto por idleSkip al comprobar que el motor estaba drenado; sirve
  // para cazar el tramo que se cuele entre esa comprobacion y la publicacion del aparcamiento.
  // `until` != 0: el motor NO esta drenado y el valor leido solo vale hasta ese instante
  // (proximo cierre o lanzamiento de tramo), asi que la CPU no puede pasar de ahi.
  auto rspParkWait(u64 now, u64 seq0, u64 until = 0) -> u64;
  // Primer instante de invitado posterior a `now` en que DPC_STATUS puede cambiar por el
  // horario ya fechado: cierre del primer tramo abierto o lanzamiento del primero aun no
  // visible. 0 si no hay ninguno.
  auto dpNextChangeAt(u64 now) const -> u64 {
    const u64 sub = dpSubSeq.load(std::memory_order_acquire);
    const u64 c = dpCompletedAt(now), vis = dpVisibleAt(now);
    u64 e = 0;
    if(c < sub)   e = dpJobEndG[c & kDpRingM];
    if(vis < sub) { const u64 k = dpJobKickG[vis & kDpRingM]; if(!e || k < e) e = k; }
    return e > now ? e : 0;
  }
  // Despierta a un RSP aparcado sin que nadie archive un tramo. Hace falta cuando la CPU se
  // para en seco -- savestate, rebobinado, apagado -- porque entonces ni entra tramo nuevo ni
  // avanza cartNow hasta el tope, y la espera solo acabaria por el salvavidas de 200 ms. Le da
  // el MISMO destino que le habria dado el tope, que es un instante de invitado: no se mete
  // tiempo de anfitrion en el reloj del RSP.
  auto rspParkNudge() -> void {
    u64 pk = rspPark.load(std::memory_order_acquire);
    if(!pk) return;
    u64 exp = 0;
    rspParkWake.compare_exchange_strong(exp, rspParkCap.load(std::memory_order_acquire), std::memory_order_acq_rel);
    parkCv.notify_all();
  }
  // Deja el horario del RCP como recien arrancado. Se llama al CARGAR un estado: el anillo,
  // dpSchedEnd y los anclajes de las barreras son instantes de invitado de la partida que
  // acaba de dejar de existir, y contra el reloj restaurado son basura -- un dpSchedEnd del
  // futuro deja el motor eternamente "ocupado" y empuja todo tramo nuevo detras de el. No se
  // serializan porque no son estado de la maquina sino horario derivado, y el estado se toma
  // siempre con el RCP en reposo (System::quiesceRcp): motor drenado y tarea terminada. Lo
  // que el invitado SI puede ver -- DPC_CURRENT, DPC_STATUS, los punteros del FIFO -- va en
  // el fichero por su cuenta, y con el anillo a cero dpcCurrentFor cae justo en ellos.
  auto rcpSchedReset() -> void;
  std::atomic<u64> dpRdv{0};            // citas pedidas
  std::atomic<u32> dpRdvWaives{0};      // veces que el salvavidas la solto
  auto rspGuestNow() -> u64 {
    return ((rcpMode == RcpMode::Threaded && rspBusy.load(std::memory_order_acquire)) || lockRspExec)
             ? spBarrierAt() : cartNow();
  }
  // Lockstep: el bucle del sistema esta dando ciclos al RSP AHORA (ver rspInterleave en
  // system.cpp). Lo que el microcodigo toque ahi se sella con el reloj del RSP, igual que en
  // Threaded lo hace su hilo: si no, un DPC_END escrito por el RSP se fechaba en el final de
  // la instruccion de CPU y no en el ciclo exacto del RSP que lo escribio.
  bool lockRspExec = false;
  // Igual, pero con los ciclos de RSP que pasa quien llama. Lo usa el propio hilo del RSP,
  // que sabe exactamente cuantas instrucciones lleva retiradas en esta instruccion (ver
  // Rsp::exactCycles) mientras que el contador publicado va a saltos de tanda.
  auto rspGuestNowAt(u64 rspCycles) -> u64 {
    return ((rcpMode == RcpMode::Threaded && rspBusy.load(std::memory_order_acquire)) || lockRspExec)
             ? spCycleAt(rspCycles - spKickCycles) : cartNow();
  }
  // BARRERA DE INVITADO DEL RSP -- la gemela de la del RDP, para el otro dominio.
  //
  // Mismo fallo, misma cura. El plazo de fin de tarea del RSP (spEndArm) se arma cuando el
  // microcodigo llega a BREAK, con el coste total ya medido; si para entonces el reloj del
  // invitado se ha pasado, el plazo nace vencido y MI_SP cae donde el anfitrion haya llegado.
  // Medido en SM64: 1 de cada ~370 tareas nacia vencida, y bastaba para que una corrida
  // arrancase a dibujar 24 campos antes que otra del mismo binario (docs/GAPS.md 3b).
  //
  // A diferencia del RDP aqui no hace falta un paseo de coste previo: Rsp::cyclesRun ya es
  // monotono y el worker lo publica cada pocos miles de instrucciones, asi que en todo momento
  // se sabe hasta que instante de invitado ha trabajado el RSP DE VERDAD. Esa es la barrera:
  //
  //   spBarrierAt() = spKickOps + ops(cyclesRun - spKickCycles)
  //
  // La CPU no puede pasar de ahi. Como cyclesRun solo crece y acaba valiendo exactamente el
  // coste con el que se arma el plazo, al llegar spEndArm se cumple now <= spDoneAt SIEMPRE:
  // el plazo ya no puede nacer vencido. Es la misma semantica del bus que impone rspPace, pero
  // anclada al instante de LANZAMIENTO (spKickOps/spKickCycles, el reloj del plazo) en vez de
  // a una ventana relativa con holgura -- y la holgura es justo lo que dejaba pasar la CPU de
  // largo. rspPace sigue existiendo como regulador de rendimiento; la barrera es la que manda.
  //
  // KESTREL_SPBARRIER=0 la apaga para bisecar.
  static auto spBarrierOn() -> bool;
  auto spBarrierAt() const -> u64 {
    return spCycleAt(rsp.cyclesRun.load(std::memory_order_acquire) - spKickCycles);
  }
  auto spBarrierWait(u64 now) -> void;   // SOLO hilo de CPU
  u64  spBarWaivedAt = ~0ull;
  std::atomic<u32> spArms{0}, dpArms{0}, spLate{0}, dpLate{0};  // diagnostico del plazo
  // Retiros: veces que el fin de SP/DP se ha hecho VISIBLE al invitado. spArms/dpArms cuentan
  // el ARMADO del plazo, y en Threaded lo arma el hilo del RSP/RDP cuando termina el trabajo
  // en tiempo de PARED: dos corridas del mismo binario suben ese contador en campos distintos
  // aunque el invitado ejecute exactamente lo mismo. Estos otros los sube siempre el hilo que
  // publica la interrupcion, en el instante de invitado que le toca, asi que van clavados. El
  // rastro por campo (KESTREL_FIELDTRACE) se compara por md5 entre corridas: tiene que mirar
  // los retiros, no los armados. Los armados siguen en la linea [det], que no se compara.
  std::atomic<u32> spRets{0}, dpRets{0};
  std::atomic<u64> spBarBlockNs{0};
  std::atomic<u32> spBarWaives{0};
  std::atomic<u64> dpBarBlockNs{0};    // tiempo de pared parado en la barrera
  std::atomic<u32> dpBarWaives{0};     // veces que el salvavidas la solto
  // Diagnostico del sondeo de DPC desde el RSP (docs/GAPS.md 3b): cuantas lecturas hace el
  // microcodigo, cuantas ven el motor ocupado y cuantas ven END_VALID. Si dos corridas dan
  // numeros distintos, por ahi se cuela el anfitrion.
  std::atomic<u64> dpLateMax{0};
  // Por lector: [0] hilo de CPU, [1] hilo del RSP. Cada contador tiene UN solo escritor y se
  // incrementa sin prefijo LOCK (bumpOwned): el microcodigo sondea DPC millones de veces por
  // corrida y cada fetch_add era un LOCK XADD. Las cuentas siguen exactas.
  std::atomic<u32> dpcRdCur[2]{}, dpcRdSt[2]{}, dpcRdBusy[2]{}, dpcRdEndV[2]{}, dpcRdOpen[2]{};
  std::atomic<u32> dpAwaits{0};
  std::atomic<u32> dpWvBar{0}, dpWvAwait{0}, dpWvSched{0};   // quien suelta el salvavidas
  std::atomic<u32> dpcRdRsp{0};   // lecturas de DPC_CURRENT/STATUS desde el RSP (solo escribe el RSP)
  // Ciclos del RCP (62,5 MHz, RSP y RDP igual) -> instrucciones-equivalentes de CPU.
  auto rcpCyclesToOps(u64 cyc) const -> u64 {
    return paceDivDen.div(cyc * paceCpuNum + paceCpuDen - 1);
  }
  // La inversa, redondeando A LA BAJA: se usa para saber cuantos ciclos de RCP caben en un
  // hueco de invitado sin pasarse de el.
  auto rcpOpsToCycles(u64 ops) const -> u64 {
    return paceDivNum.div(ops * paceCpuDen);
  }
  // Primer instante de invitado en que la tarea del RSP lanzada en spKickOps lleva `cyc` ciclos
  // hechos: el flanco absoluto spKickEdge + cyc pasado a ops. Nunca antes de spKickOps + 1 si
  // cyc >= 1, porque el flanco spKickEdge + 1 esta estrictamente despues del lanzamiento.
  auto spCycleAt(u64 cyc) const -> u64 { return rcpCyclesToOps(spKickEdge + cyc); }
  // Ops que faltan para el plazo mas cercano, o ~0 si no hay ninguno armado. El dynarec la
  // mira igual que mira siDueIn: un bloque no puede tragarse el instante de la interrupcion.
  auto rcpDueIn(u64 now) const -> u64 {
    u32 pend = rcpPend.load(std::memory_order_relaxed);
    if(!pend) return ~0ull;
    u64 d = ~0ull;
    if(pend & 1u) d = spDoneAt > now ? spDoneAt - now : 0;
    if(pend & 2u) { u64 e = dpDoneAt > now ? dpDoneAt - now : 0; if(e < d) d = e; }
    // La barrera es un plazo mas del mismo reloj: un bloque del dynarec no puede tragarsela,
    // o la CPU se pasaria de largo del fin de la tarea sin haberse parado.
    if(pend & 4u) {
      u64 b = dpBarrierAt();
      u64 e = b > now ? b - now : 0;
      if(e < d) d = e;
    }
    if(pend & 8u) {
      // spBarrierEff, no spBarrierAt: el plazo que un bloque no puede tragarse es aquel en el
      // que la CPU se va a PARAR de verdad, y quien decide eso es spBarrierWait, que mira la
      // version efectiva. Con el RSP aparcado (rspParkWait) su reloj esta congelado, asi que
      // spBarrierAt() se queda clavado detras del invitado y esto devolvia 0 durante todo el
      // aparcamiento: el dynarec declinaba cada bloque y la CPU emulaba a intervalo de una
      // instruccion justo en la ventana en que es la unica que puede desatascar la escena.
      // Medido en DK64 (300 campos, Parallel-RDP): 43 M entradas al driver del JIT con el
      // aparcamiento contra menos de 1 M sin el, y 7,4 s de pared contra 4,0 s -- con las dos
      // corridas retirando las MISMAS 402 M instrucciones. Con la barrera efectiva el plazo
      // vale rspPark + kParkLead mientras dura el aparcamiento, que es exactamente hasta
      // donde la CPU tiene permiso para correr.
      u64 b = spBarrierEff();
      u64 e = b > now ? b - now : 0;
      if(e < d) d = e;
    }
    if(pend & 16u) {
      const u32 h = dpLogHead.load(std::memory_order_acquire);
      if(h != dpLogTail.load(std::memory_order_acquire)) {
        const u64 at = dpLog[h & kDpLogM].at;
        u64 e = at > now ? at - now : 0;
        if(e < d) d = e;
      }
    }
    return d;
  }


  // --- regulador de velocidad CPU<->RSP (solo Threaded) -----------------------
  // Estado privado del HILO CPU (nadie mas lo toca), salvo los contadores atomicos
  // que son telemetria. Ver Memory::rcpPace en memory.cpp para el razonamiento.
  u64  paceCpu0 = 0, paceRsp0 = 0;   // CPU en tiempo de invitado / ciclos RSP al enganchar
  bool pacePrimed = false;           // hay episodio enganchado
  bool paceGiveUp = false;           // salvavidas: freno suelto en este episodio
  u64  paceWaitedNs = 0;             // bloqueado en el episodio actual
  std::atomic<u64> paceBlockNs{0}, paceEpisodes{0}, paceHolds{0};
  // Lo mismo para el RDP. Estado propio porque los dos dominios van por su cuenta: puede
  // haber tarea de RSP en vuelo sin trabajo de RDP y al reves.
  u64  dpaceCpu0 = 0, dpaceGclk0 = 0;
  bool dpacePrimed = false;
  bool dpaceGiveUp = false;
  u64  dpaceWaitedNs = 0;
  std::atomic<u64> dpaceBlockNs{0}, dpaceEpisodes{0}, dpaceHolds{0};
  // Byte de guardia del camino rapido de store del dynarec (CPU::stGuard). El bus pone y
  // quita aqui el bit del modo repeticion de MI_MODE para que el codigo emitido no tenga que
  // leer mem->rcp.mi_repeat_on (una carga de puntero mas) en cada escritura. Nulo sin CPU atada.
  u8* cpuStGuard = nullptr;

  // Frena la CPU si adelanta al RSP en vuelo, y DEVUELVE el permiso que le queda al camino
  // rapido del prologo del JIT (ops de CPU antes de tener que volver a preguntar). Las dos
  // cosas salen de los mismos dos valores -- `rspBusy` y `rsp.cyclesRun` -- y esos dos viven
  // en lineas que el worker del RSP reescribe constantemente: leerlas es un fallo de cache
  // compartida, no una lectura local. Por eso van juntas: antes eran dos llamadas seguidas
  // (rcpPace + paceAllowance) que releian el mismo par de lineas. Quien no quiera el permiso
  // ignora el retorno.
  auto rcpPace(u64 cpuOps) -> u32;
 private:
  auto paceGrant(u64 ahead, u64 allow) -> u32;   // permiso a partir de lo ya leido
  auto rdpPace(u64 cpuOps) -> u32;           // freno del dominio RDP (ver memory.cpp)
  auto rdpCostPass(u32 current, u32 stop, bool xbus, const u8* cmdSrc) -> bool;
  static auto rdpCostOn() -> bool;   // KESTREL_RDPCOST=0 apaga el modelo de coste
  auto rspPace(u64 cpuOps) -> u32;           // freno del dominio RSP
 public:
  // Publico: System espera aqui antes de abrir la ventana, porque el presentador comparte el
  // contexto Vulkan del backend. Lo LEVANTA el hilo del RDP (Granite ata su estado por hilo),
  // asi que esto solo espera; devuelve false si expira el plazo o si el backend no esta pedido.
  auto vrdpWaitReady(u32 timeoutMs) -> bool;
private:
  // Puntero de lectura propio del consumidor del FIFO (el "CURRENT" del command processor).
  // Cuando el ultimo comando de un span esta partido, el RDP se para DELANTE de el y lo
  // reanuda con el span siguiente; esto lo recuerda. Solo lo toca rdpRunJob (un unico hilo
  // consumidor), asi que no compite con dpc_submitted, que es del productor.
  u32  rdpResume = 0, rdpLastEnd = 0;
  bool rdpHasResume = false;
  // El paseo de coste (rdpCostPass) tiene SU PROPIO punto de corte: recorre el mismo tramo de
  // FIFO que la GPU pero con el decodificador de SoftRdp, y aunque los dos midan los comandos
  // igual, no se les obliga a coincidir. Con punteros separados, cada uno reanuda donde paro
  // el y ningun comando se cobra dos veces ni se queda sin cobrar.
  u32  rdpCostResume = 0, rdpCostLastEnd = 0;
  bool rdpCostHasResume = false;

  auto rdpWorkerLoop() -> void;
  auto vrdpBringUp() -> void;      // trae parallel-rdp arriba una sola vez
  std::once_flag vrdpOnce;
  auto rspWorkerLoop() -> void;
public:

  // --- debug store watchpoint ------------------------------------------------
  u32  watchAddr = 0;      // physical byte address to watch (0 = disabled)
  u32  watchLen  = 4;      // watched window length in bytes
  u64  storePc   = 0;      // CPU sets this to its curPc before each store
  auto watchHit(u32 physAddr, u32 nbytes, u64 value, bool dma) -> void;

  // Estado de los mandos, publicado por la capa de video/entrada (o por KESTREL_BUTTONS
  // cuando no hay ventana) y leido por el joybus con la orden 0x01. Vive en padPort[]; esto
  // son atajos al puerto 1 para no reescribir cada sitio que solo mira al jugador 1.
  // Atajos al puerto 1, que es el que usan la telemetria y los modos sin ventana.
  auto padButtons() -> u32& { return padPort[0].buttons; }
  auto padStickX()  -> s8&  { return padPort[0].stickX; }
  auto padStickY()  -> s8&  { return padPort[0].stickY; }

  // Cuantas veces ha leido el juego el mando 1 por el joybus. Es la frecuencia REAL de
  // sondeo que percibe el juego, que no tiene por que ser la de campos de video: si baja,
  // una pulsacion corta cae entre dos sondeos y se pierde. Se publica en emu.status para
  // poder medirlo desde fuera en vez de suponerlo.
  std::atomic<u64> padPolls{0};

  // Mando inyectado por telemetria (MCP `pad.set`). Mientras queden sondeos el estado del
  // mando 1 lo manda la red y no el teclado/gamepad del anfitrion: el bucle de la ventana
  // reescribe padButtons cada cuadro, asi que sin esta capa una pulsacion inyectada se
  // perderia antes de que el juego llegase a sondear el joybus. El contador va en SONDEOS
  // del joybus, no en milisegundos: asi una pulsacion dura lo mismo en lockstep, en
  // threaded y con el emulador corriendo a 30% o a 200% de tiempo real, y el juego ve un
  // flanco de bajada de verdad (que es lo que esperan los menus). -1 = hasta nueva orden.
  std::atomic<u32> padRemoteButtons[4]{};
  std::atomic<s32> padRemoteStick[4]{};    // (u8)x | (u8)y << 8
  std::atomic<s32> padRemotePolls[4]{};    // sondeos que quedan; 0 = apagado, -1 = infinito

  // Debug: a CPU store hit a physical address it should never touch (e.g. SP DMA
  // registers from a stack overflow). The CPU polls pendingTrap and halts.
  bool        pendingTrap = false;
  std::string trapMsg;
  bool        trapSpRegStore = false;   // enabled by KESTREL_TRAPSPREG

  // IS-Viewer debug channel: homebrew / n64-systemtest text output overlaid at
  // physical 0x13FF0000. Chars are staged at 0x13FF0020; a length write to
  // 0x13FF0014 flushes them to stdout. Returns true if the address was handled.
  std::vector<u8> isv;                  // staging buffer (lazily sized on first use)
  u8  isvHdr[0x20] = {};                // the window's header registers (magic, pointers)
  auto isvWrite(u32 phys, u32 value, u32 nbytes) -> bool;
  auto isvRead(u32 phys, u32 nbytes, u32& out) -> bool;   // header/buffer readback

  // PI/cartridge write-latch. Writing to cart space does not reach ROM; it latches
  // the value onto the PI bus, and the very next cart read returns it before the
  // latch decays (~a couple hundred CPU instructions). Only the first write matters
  // until it is consumed. A Bug's Life depends on this. Sub-word cart reads are also
  // mangled by the 16-bit-wide PI bus (only the upper half of each 32-bit word is
  // reachable), which cartRead models.
  u32  cartLatch = 0;
  bool cartLatchValid = false;
  u64  cartLatchExpiry = 0;
  const u64* cartClock = nullptr;       // CPU retired-instruction counter (decay clock)
  // Ops ya ejecutadas por una cadena de bloques del JIT y aun sin sumar a `retired` (el
  // commit es diferido, por cadena). Sin esto el reloj de decaimiento se congela dentro de
  // una cadena larga y el latch del PI sobrevive mucho mas de lo que debe.
  const u32* cartClockPend = nullptr;
  // Permiso de la cadena del JIT (CPU::jitGuard). Quien arma un plazo NUEVO desde el hilo de
  // CPU lo pone a 0: el permiso se calculo antes de que existiera, asi que la cadena se lo
  // tragaria. A 0 el siguiente prologo vuelve al trampolin, que ya lo ve (siDueIn).
  u32* jitGuardPtr = nullptr;
  const u64* cartStall = nullptr;       // ops equivalentes a las paradas de cache (CPU::stallOps)
  auto cartNow() const -> u64 {
    return (cartClock ? *cartClock : 0) + (cartClockPend ? (u64)*cartClockPend : 0)
         + (cartStall ? *cartStall : 0);
  }
  // Instrucciones retiradas por campo de video. La fija System desde Clocks::fieldInsns()
  // (unico reloj de tiempo del emulador) y la usa la lectura de VI_V_CURRENT, para que el
  // sondeo de medias-lineas y la interrupcion del VI midan EL MISMO tiempo. Antes habia
  // dos relojes distintos (750k por campo en el tick, 1.5625M en la lectura).
  u64 viFieldInsns = 782'000;
  // Campos de video por segundo en mili-hercios (59.94 Hz = 59940). Lo fija System junto a
  // viFieldInsns; el drenaje del AI lo necesita para pasar de instrucciones a segundos de
  // guest sin meter coma flotante en una ruta que tiene que ser bit-identica entre modos.
  u32 viFieldHzMilli = 59'940;

  // Reloj de video del RCP, del que cuelga el DAC de audio: rate = aiVidClock/(dacrate+1).
  // Depende de la norma de television de la maquina (NTSC 48.681812 MHz, PAL 49.656530 MHz,
  // PAL-M 48.628316 MHz), asi que un cartucho PAL programa el mismo dacrate para otra
  // frecuencia de muestreo. Lo fija System junto al resto del modelo de reloj.
  u32 aiVidClock = 48'681'812;
  // Instrucciones de CPU que le tocan por cada instruccion de RSP, en fraccion. La fija
  // System desde Clocks::rspInsnsPerCpuInsn() (3/4 con relojes de serie) y la usa el
  // regulador rcpPace: es el MISMO ratio que el interleave de Lockstep, invertido.
  u64 paceCpuNum = 3, paceCpuDen = 4;
  // Reciproco exacto de los dos divisores. Con relojes de serie NO son 3 y 4: System los fija
  // en 65536 y round(0,6667*65536) = 43691, asi que `((x * num) / den)` es una division de 64
  // bits de verdad -- unas setenta veces el coste de un multiplicar, y sin encauzar. rspPace y
  // rdpPace la hacen UNA POR LLAMADA y entre las dos salen el 7 % de las muestras del hilo de
  // CPU (perfil de SM64, 2026-09-11).
  //
  // Los dos divisores se fijan una sola vez al arrancar, asi que se sustituye la division por
  // un multiplicar de 128 bits y un desplazamiento. Con m = ceil(2^79/d) y e = m*d - 2^79 (que
  // cumple 0 < e <= d), la identidad floor(x*m / 2^79) == floor(x/d) vale EXACTA para todo
  // x <= floor(2^79/e); por encima de ese limite se divide de verdad, y tambien si m no cabe
  // en 64 bits. O sea que no es una aproximacion: o da el mismo numero, o no se usa.
  struct PaceDiv {
    u64 d = 1, m = 0, maxX = 0;
    auto set(u64 den) -> void {
      d = den ? den : 1;
      const __uint128_t k = (__uint128_t)1 << 79;
      __uint128_t mm = (k + d - 1) / d;                 // ceil(2^79/d)
      if(mm >> 64) { m = 0; maxX = 0; return; }         // no cabe: division de verdad
      m = (u64)mm;
      const __uint128_t e = (__uint128_t)m * d - k;     // 0 <= e <= d
      if(!e) { maxX = ~0ull; return; }                  // d potencia de dos: exacto siempre
      const __uint128_t b = k / e;
      maxX = (b >> 64) ? ~0ull : (u64)b;
    }
    auto div(u64 x) const -> u64 {
      if(m && x <= maxX) return (u64)(((__uint128_t)x * m) >> 79);
      return x / d;
    }
  };
  PaceDiv paceDivNum, paceDivDen;
  // Unico sitio que toca el ratio. System lo llama al fijar el modelo de reloj.
  auto setPaceRatio(u64 num, u64 den) -> void {
    paceCpuNum = num ? num : 1; paceCpuDen = den ? den : 1;
    paceDivNum.set(paceCpuNum); paceDivDen.set(paceCpuDen);
  }
  static constexpr u64 CART_LATCH_TTL = 200;
  auto isCart(u32 phys) const -> bool { return !rom.empty() && phys >= 0x1000'0000 && phys < 0x1fc0'0000; }
  auto cartRom32(u32 phys) -> u32;               // aligned 32-bit ROM word (0 if past image)
  auto cartRead(u32 phys, u32 nbytes) -> u32;    // CPU read from cart space (latch + 16-bit mux)
  auto cartWrite(u32 phys, u64 value, u32 nbytes) -> void;  // CPU write to cart space (latch)
  auto piIoDecay() -> void;                      // deja caer IO_BUSY cuando vence el latch
  auto dpsSpanRead(u32 idx) const -> u32;        // puerto de test al buffer de spans del RDP
  auto dpsSpanWrite(u32 idx, u32 v) -> void;

  // PI domain 2 (0x08000000-0x0FFFFFFF): the SRAM/FlashRAM save device. Both CPU
  // direct access and PI DMA route through these. saveOffset maps a domain-2 physical
  // address to a linear backing offset (identity for SRAM; the 3-bank layout for
  // 768 kbit). saveRead/saveWrite handle CPU-side byte/word access; the flash status
  // window and command register are decoded there.
  auto isSaveDomain(u32 phys) const -> bool { return phys >= 0x0800'0000 && phys < 0x1000'0000; }
  auto saveOffset(u32 phys) const -> u32;        // domain-2 phys -> backing offset
  auto saveRead(u32 phys, u32 nbytes) -> u32;    // CPU read from save space
  auto saveWrite(u32 phys, u64 value, u32 nbytes) -> void;  // CPU write to save space

  auto reset(bool expansionPak = true) -> void;
  auto initMap() -> void;                       // (re)builds the regions table
  auto loadRom(std::vector<u8> image) -> void;  // takes a normalized big-endian ROM

  // Telemetry helpers: find a region by exact or substring name (case-insensitive).
  auto findRegion(const std::string& query) -> Region*;

  // Big-endian bus access, by physical address. RAM-backed regions read/write the
  // backing store; RCP MMIO dispatches to the register handlers; unmapped reads
  // return 0 and writes are dropped.
  auto read8 (u32 addr) -> u8;
  auto read16(u32 addr) -> u16;
  auto read32(u32 addr) -> u32;
  auto read64(u32 addr) -> u64;
  auto write8 (u32 addr, u8  value) -> void;
  auto write16(u32 addr, u16 value) -> void;
  auto write32(u32 addr, u32 value) -> void;
  auto write64(u32 addr, u64 value) -> void;
  // SP DMEM/IMEM and PIF RAM are 32-bit-only devices with no byte-enables: a
  // sub-word CPU store (SB/SH/SD) overwrites the whole aligned 32-bit word with the
  // full source register shifted into place. Returns true if `phys` hit such a
  // device and the store was handled here (caller must then skip the normal path).
  auto wordStoreQuirk(u32 phys, u64 reg, u32 width) -> bool;

  // RDRAM init/repeat broadcast: replicate a store of `sz` bytes across the span
  // [phys, (phys&~7)+len), wrapping within the current 2 KiB page. Non-driven byte
  // lanes are zeroed (the store's byte-enables gate a 4-byte unit for SB/SH/SW, an
  // 8-byte unit for SD). Consumed by the store; caller then skips the normal write.
  auto miRepeatStore(u32 phys, u64 value, u32 sz) -> void;

  // --- anillo de eventos del RCP (KESTREL_EVLOG=1) ---------------------------
  // Un cuelgue del guest en modo threaded se ve desde fuera como "la CPU gira y nadie le
  // levanta nada". Lo que falta saber es la SECUENCIA: quien encolo, quien termino, quien
  // levanto o bajo que bit de MI, y en que punto del reloj de instrucciones. Esto lo graba
  // sin bloquear (indice atomico, escritura de una entrada) y el watchdog lo vuelca.
  struct Ev { const char* tag; u32 a, b; u64 clk; };
  static constexpr u32 kEvN = 16384;                 // potencia de 2: la mascara es el modulo
  Ev               evRing[kEvN] = {};
  std::atomic<u32> evIdx{0};
  bool             evOn = false;
  int              evLvl = 0;   // 2 = sin eventos de RDP (ahogan el anillo)
  // El VI se levanta y se baja cada campo: en el anillo ahoga todo lo demas. Se cuenta.
  u64              evVi = 0, evViClr = 0;
  auto ev(const char* tag, u32 a = 0, u32 b = 0) -> void {
    if(!evOn) return;
    if(evLvl >= 2 && tag[0] == 0x64) return;   // 'd' = dp.sub / dp.done
    u32 i = evIdx.fetch_add(1, std::memory_order_relaxed) & (kEvN - 1);
    evRing[i] = Ev{ tag, a, b, cartClock ? *cartClock : 0 };
  }
  auto evDump(u32 n = 80) -> void;

  // Contabilidad de interrupciones MI. En HW cada fuente tiene un latch de un bit: si el
  // RCP vuelve a levantar una linea que aun no ha sido reconocida, el segundo aviso se
  // funde con el primero. Aqui se cuenta ese colapso por fuente para poder demostrar si
  // un despertar se pierde (worker adelantado al ack del huesped) en modo threaded.
  std::atomic<u32> miRaise[6]{}, miMerge[6]{}, miClear[6]{}, miClearIdle[6]{};
  static auto miIdx(u32 bit) -> int {
    return bit==MI_SP?0:bit==MI_SI?1:bit==MI_AI?2:bit==MI_VI?3:bit==MI_PI?4:5; }
  auto miDump() -> void {
    static const char* nm[6] = { "SP","SI","AI","VI","PI","DP" };
    std::fprintf(stderr, "[mi] fuente  raise  fundidas  clear  clear-en-vacio\n");
    for(int k = 0; k < 6; k++)
      std::fprintf(stderr, "[mi]   %-4s %7u %9u %6u %10u\n", nm[k], miRaise[k].load(),
                   miMerge[k].load(), miClear[k].load(), miClearIdle[k].load());
  }

  // --- interrupt aggregation (MI) --------------------------------------------
  auto raiseIntr(u32 bit) -> void {
    static int irqt = std::getenv("KESTREL_IRQTRACE") ? 1 : 0;
    if(irqt) { const char* nm = bit==MI_SP?"SP":bit==MI_SI?"SI":bit==MI_AI?"AI":bit==MI_VI?"VI":bit==MI_PI?"PI":bit==MI_DP?"DP":"?";
      static u32 cnt[6]={}; int idx = bit==MI_SP?0:bit==MI_SI?1:bit==MI_AI?2:bit==MI_VI?3:bit==MI_PI?4:5;
      if(cnt[idx]++ < 12) std::fprintf(stderr,"[irq] %s #%u mask=%02x intr=%02x\n",nm,cnt[idx],rcp.mi_mask,rcp.mi_intr|bit); }
    if(bit == MI_VI) { evVi++; if(evLvl >= 2) ev("mi+", bit, rcp.mi_mask); } else ev("mi+", bit, rcp.mi_mask);
    {  // fetch_or: el valor previo dice si el latch ya estaba puesto (aviso fundido).
      u32 prev = rcp.mi_intr.fetch_or(bit, std::memory_order_release);
      int k = miIdx(bit);
      miRaise[k].fetch_add(1, std::memory_order_relaxed);
      if(prev & bit) miMerge[k].fetch_add(1, std::memory_order_relaxed);
    }
    if((bit & MI_SP) && spTrace()) { static u32 n=0; if(n++<40) std::fprintf(stderr,"[mi] raise SP #%u mask=%02x intr=%02x\n",n,rcp.mi_mask,rcp.mi_intr.load()); }
    if((bit & MI_DP) && spTrace()) { static u32 n=0; if(n++<40) std::fprintf(stderr,"[mi] raise DP #%u mask=%02x intr=%02x\n",n,rcp.mi_mask,rcp.mi_intr.load()); }
  }
  auto clearIntr(u32 bit) -> void {
    if((bit & MI_SP) && spTrace()) { static u32 n=0; if(n++<40) std::fprintf(stderr,"[mi] clear SP #%u\n",n); }
    if((bit & MI_DP) && spTrace()) { static u32 n=0; if(n++<40) std::fprintf(stderr,"[mi] clear DP #%u\n",n); }
    if(bit == MI_VI) { evViClr++; if(evLvl >= 2) ev("mi-", bit, rcp.mi_mask); } else ev("mi-", bit, rcp.mi_mask);
    {  // fetch_and: reconocer una linea que ya estaba baja indica un ack sin evento.
      u32 prev = rcp.mi_intr.fetch_and(~bit, std::memory_order_acq_rel);
      int k = miIdx(bit);
      miClear[k].fetch_add(1, std::memory_order_relaxed);
      if(!(prev & bit)) miClearIdle[k].fetch_add(1, std::memory_order_relaxed);
    }
  }
  static auto spTrace() -> bool { static int t = std::getenv("KESTREL_RSPTRACE")?1:0; return t; }
  auto interruptPending() const -> bool { return (rcp.mi_intr & rcp.mi_mask) != 0; }

  // --- SI: la transaccion de la PIF NO es instantanea -------------------------
  // En hardware, leer el bloque de la PIF (RD64B) hace que el PIF corra el protocolo joybus
  // ANTES de contestar, y el joybus va a 4 us por bit (bit de parada de la consola 3 us).
  // Una lectura de botones de los cuatro conectores son cientos de microsegundos: el juego
  // arranca el DMA, se duerme en la cola de mensajes del SI y el kernel corre otra cosa
  // mientras. Completarlo en la misma instruccion que lo arranca le devuelve el control al
  // hilo del juego ANTES de tiempo y cambia el entrelazado de hilos del invitado.
  bool siBusy   = false;    // SI_STATUS bit 0 (DMA_BUSY) mientras la transaccion va en vuelo
  bool siToPif  = false;    // direccion del DMA pendiente
  u32  siDram   = 0;        // direccion de RDRAM del DMA pendiente
  static constexpr u32 SI_DMA_BUSY = 1u << 0;   // SI_STATUS bit 0
  // Traslado de los 64 B entre RDRAM y PIF RAM, aparte del joybus. No hay medida publicada
  // que se pueda citar, y frente a los ~600 us del joybus es ruido, asi que se deja en un
  // valor pequeno y explicito en vez de fingir precision.
  static constexpr u64 kSiXferUs = 5;
  u64  siDoneAt = 0;        // reloj de invitado (cartNow()) en que termina
  auto siDma(bool toPif) -> void;      // arranca: hace el trabajo y arma el plazo
  auto siFinish() -> void;             // vence el plazo: entrega y levanta MI_SI
  // Instrucciones que faltan para que venza (~0 = no hay nada en vuelo). La usan el
  // interprete (para rematar en la instruccion exacta) y la guarda del JIT (para no
  // saltarse el plazo dentro de un bloque), que es lo que hace que el instante de la
  // interrupcion sea el MISMO en los siete modos.
  auto siDueIn(u64 now) const -> u64 {
    if(!siBusy) return ~0ull;
    return siDoneAt > now ? siDoneAt - now : 0;
  }
  // Microsegundos de joybus -> instrucciones retiradas. Pasa por el unico reloj del
  // emulador (instrucciones por campo x campos por segundo), asi que sigue al factor CPI
  // y no reintroduce un segundo reloj.
  auto usToInsns(u64 us) const -> u64 {
    return us * viFieldInsns * (u64)viFieldHzMilli / 1'000'000'000ull;
  }

  // --- VI tick (drives VI_CURRENT + VI interrupt), called by the run loop --
  // Recibe el contador de instrucciones retiradas (el reloj de tiempo del emulador) y
  // devuelve true cuando ese tramo ha cerrado un campo de video.
  auto viTick(u64 retiredNow) -> bool;
  u64  viLastRetired = 0;   // posicion del VI en el tick anterior
  // --- AI drain tick (paces audio DMA FIFO), called once per field from viTick --
  auto aiTick(u64 retiredNow) -> void;

  // Lectura de un registro del RCP por direccion fisica YA alineada, saltando el prologo
  // de `read32` (cart, dominio de save y el recorrido de regiones de `resolve`). Solo vale
  // para direcciones que se sabe que son MMIO: las bases fijas de SP/DPC que lee el COP0
  // del RSP. Mismo decodificador, mismo valor.
  auto rcpReg32(u32 phys) -> u32;
  // Escritura de un registro del RCP por direccion fisica YA alineada. Gemela de rcpReg32:
  // se salta cart, dominio de save y el recorrido de regiones de `resolve`, que para las
  // bases de SP/DPC no pueden acertar nunca. Tambien se salta la trampa KESTREL_TRAPSPREG,
  // y eso es MEJOR senal, no peor: esa trampa busca una tienda de la CPU que se ha ido a
  // los registros del SP (mira `storePc`, un PC de CPU, contra el rango del driver), y el
  // MTC0 del propio RSP no es eso. El punto de observacion de `watchHit` SI se conserva.
  auto rcpRegWrite32(u32 phys, u32 value) -> void;

private:
  // Resolve a physical address to a backing pointer + remaining bytes, or nullptr.
  auto resolve(u32 addr, u32& remaining) -> u8*;

  // RCP MMIO register access (physical addr already masked to 0x1fffffff).
  auto isMmio(u32 addr) const -> bool;
  auto mmioRead32(u32 addr) -> u32;
  auto mmioWrite32(u32 addr, u32 value) -> void;

  // DMA engines.
  auto piDma(bool toCart) -> void;   // PI: cartridge <-> RDRAM
  auto spDma(bool toRam) -> void;    // SP: DMEM/IMEM <-> RDRAM
  auto dpcAdvance() -> void;         // DPC: consume el FIFO pendiente (END o CLEAR_FREEZE)
  // Corre el bloque de ordenes de la PIF RAM y rellena las respuestas de mandos/EEPROM;
  // devuelve los microsegundos de linea joybus que costo (ver el bloque SI de arriba).
  auto pifProcessJoybus() -> u32;
};

}  // namespace kestrel
