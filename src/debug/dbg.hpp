#pragma once
// Debug -- per-frame recorder of RDP commands, RSP tasks and guest timeline.
//
// Responsibility: when enabled, the core hooks (rdpJob, rspStart/rspEnd, flip) record what the
// RCP did in the current frame; the `dbg.*` telemetry commands and the debugger window read the
// last closed frame through snapshot(). Also holds the RDP command decoder (names, fields).
//
// Invariants:
// - Off by default; every hook first reads the `on` atomic, so the cost when disabled is one load.
// - A frame closes when the game flips VI_ORIGIN (flip()); freeze stops the closed frame from
//   being replaced. At most kMaxCmds commands per frame are stored, the rest are only counted.
// - Hooks must be called with the guest clock `now` (Memory::cartNow()), not host time.
//
// Threads: the RDP worker records RDP jobs, the RSP records task start (CPU thread) and end (RSP
// worker), the CPU thread closes frames. All writes and snapshot() take `mx`; `on` and `freeze`
// are atomics. Readers (telemetry, UI) always get a copy.
// kestrel64 — capa de depuracion (docs/DEBUGGER.md).
//
// Graba, por CUADRO (el cuadro se cierra cuando el juego cambia VI_ORIGIN a otro buffer),
// lo que paso en el RCP: cada comando del RDP con su direccion en el FIFO, cada tarea del
// RSP con su tipo de OSTask y sus ciclos, y una linea de tiempo en reloj de invitado. La
// leen las ordenes `dbg.*` de la telemetria (MCP) y la ventana del depurador, por el mismo
// camino. Apagada de fabrica: el coste en el camino caliente es leer un atomico.
//
// Hilos: el RDP graba desde su worker, el RSP desde el hilo de CPU (lanzamiento) y desde su
// worker (fin), el cierre de cuadro desde el hilo de CPU. Todo va bajo `mx`; la frecuencia
// es por TRAMO de FIFO / por tarea, no por instruccion, asi que el cerrojo no se nota.

#include "../core/types.hpp"
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

namespace kestrel::dbg {

// Un comando del RDP tal y como salio del FIFO. Las palabras van aparte (`words`) porque
// un triangulo con todo lleva 22 y un SET_* solo una.
struct RdpCmd {
  u32 addr = 0;        // direccion del comando (RDRAM fisica, o DMEM si xbus)
  u32 word0 = 0;       // indice en Frame::words
  u8  op = 0;          // opcode de 6 bits
  u8  nwords = 1;      // palabras de 64 bits
  u8  xbus = 0;
  u32 job = 0;         // tramo (kick del DPC) al que pertenece
};

struct RspTask {
  u32 type = 0, flags = 0;      // OSTask.type / .flags (DMEM 0xFC0 / 0xFC4)
  u32 ucode = 0, ucodeData = 0; // OSTask.ucode / .ucode_data (0xFD0 / 0xFD8)
  u32 dataPtr = 0, dataSize = 0;// OSTask.data_ptr / .data_size (0xFF0 / 0xFF4)
  u32 pc = 0;                   // SP_PC del lanzamiento
  u64 t0 = 0, t1 = 0;           // reloj de invitado (instrucciones de CPU) inicio / fin
  u64 cycles = 0;               // ciclos del RSP de la tarea
  bool done = false;
};

// Tramo del RDP (un kick del DPC: DPC_START..DPC_END).
struct RdpJob {
  u32 start = 0, end = 0;
  u32 cmd0 = 0, ncmds = 0;      // rango en Frame::cmds
  u64 t0 = 0, t1 = 0;           // reloj de invitado aprox. (ver docs/DEBUGGER.md)
  u8  xbus = 0;
};

struct Frame {
  u32 number = 0;               // viFlips al abrir el cuadro
  u64 t0 = 0, t1 = 0;           // reloj de invitado del cuadro
  std::vector<RdpCmd>  cmds;
  std::vector<u64>     words;
  std::vector<RdpJob>  jobs;
  std::vector<RspTask> tasks;
  u32 truncated = 0;            // comandos que no cupieron (tope kMaxCmds)
  auto clear() -> void { cmds.clear(); words.clear(); jobs.clear(); tasks.clear(); truncated = 0; }
};

struct State {
  static constexpr u32 kMaxCmds = 400000;   // por cuadro; mas alla se cuenta y no se guarda

  std::atomic<bool> on{false};
  std::mutex mx;
  Frame cur, last;              // cuadro en curso / ultimo cuadro cerrado
  u32   framesClosed = 0;
  // Congelar: el cuadro cerrado deja de sustituirse (para mirarlo con calma en la ventana).
  std::atomic<bool> freeze{false};

  // Ganchos (llamados por el nucleo). Todos miran `on` antes de nada.
  auto rdpJob(const u8* src, u32 srcSize, const u8* dmem, u32 start, u32 end, bool xbus, u64 now) -> void;
  auto rdpJobEnd(u64 now) -> void;
  auto rspStart(const u8* dmem, u32 pc, u64 now) -> void;
  auto rspEnd(u64 cycles, u64 now) -> void;
  auto flip(u32 flips, u64 now) -> void;
  auto enable(bool v) -> void;

  // Copia del cuadro pedido (0 = ultimo cerrado, 1 = en curso) bajo el cerrojo.
  auto snapshot(int which) -> Frame;
};

extern State g;

// --- decodificador de comandos del RDP -----------------------------------------------------
// Escrito desde la documentacion publica del RDP (n64brew "Reality Display Processor/Commands"):
// nombre, numero de palabras y campos legibles. Nada de codigo ajeno.
auto rdpCmdWords(u32 op) -> u32;                     // palabras de 64 bits del comando
auto rdpCmdName(u32 op) -> const char*;
auto rdpDecode(const u64* w, u32 n) -> std::string;  // "SET_COLOR_IMAGE fmt=RGBA siz=16b w=320 addr=0x..."
auto combinerText(u64 w) -> std::string;             // (A-B)*C+D por ciclo, con nombres
auto otherModesText(u64 w) -> std::string;
// Comando "interesante" para saltar con siguiente/anterior: primitivas, SYNC_FULL, SET_*_IMAGE.
auto rdpIsPrimitive(u32 op) -> bool;

// --- puntos de observacion del anfitrion (no tocan el Watch de COP0 del invitado) ---------
struct HostWatch { u32 lo = 0, hi = 0; bool r = false, w = true; };
struct WatchHit  { u32 pc = 0, addr = 0; bool write = false; bool valid = false; };

}  // namespace kestrel::dbg
