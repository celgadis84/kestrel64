#pragma once
#include <atomic>
// kestrel64 — Reality Signal Processor, low-level interpreter (M3.3).
//
// The RSP is a MIPS-ish scalar core (no mult/div/HI/LO, 32 GPRs, 12-bit PC over
// a 4 KB IMEM) welded to a proprietary 8-lane×16-bit fixed-point vector unit
// exposed through COP2. It runs a microcode program out of IMEM against DMEM
// until it hits BREAK, at which point it halts and (optionally) raises the SP
// interrupt. Graphics microcode (F3DEX2 & friends) builds RDP command lists and
// kicks the RDP by poking the DPC registers through COP0 — which is exactly why
// the HLE "instant task completion" stub produced zero RDP commands. This core
// replaces that stub so real pixels flow.
//
// Semantics follow the n64brew wiki opcode reference (docs/wiki/n64brew) for the
// decode/encoding and the fixed-point definitions; the exact accumulator /
// clamp / flag corner cases match the hardware-validated ares SISD reference.
// The vector math IS the hardware behaviour, transcribed rather than reinvented.

#include "../core/types.hpp"

namespace kestrel {

struct Memory;

// One 128-bit vector register: 8 lanes of 16 bits, lane 0 = most significant
// (big-endian), matching the wiki's VPR<n> convention. Byte 0 = high byte of
// lane 0. Flag registers reuse this type storing 0/1 per lane.
struct R128 {
  u16 el[8] = {};

  auto u(int n) -> u16& { return el[n & 7]; }
  auto uc(int n) const -> u16 { return el[n & 7]; }
  auto s(int n) const -> s16 { return (s16)el[n & 7]; }

  auto gb(int k) const -> u8 { u16 v = el[(k >> 1) & 7]; return (k & 1) ? (v & 0xff) : (v >> 8); }
  auto sb(int k, u8 b) -> void {
    u16& v = el[(k >> 1) & 7];
    if(k & 1) v = (v & 0xff00) | b; else v = (v & 0x00ff) | ((u16)b << 8);
  }

  // flag-register lane access (stored as 0/1)
  auto get(int n) const -> u32 { return el[n & 7] & 1; }
  auto set(int n, bool b) -> bool { el[n & 7] = b ? 1 : 0; return b; }

  // broadcast modifier: produce vt(e) per the element-field table.
  auto operator()(u32 e) const -> R128;
};

struct Rsp {
  Memory* mem = nullptr;   // owning bus (DMEM/IMEM + COP0 register routing)

  // --- scalar unit -----------------------------------------------------------
  u32 r[32] = {};          // GPRs; r[0] is hardwired zero (enforced on write)
  u32 pc = 0;              // 12-bit program counter into IMEM

  // Per-IMEM-instruction hotpath sampler (opt-in via MCP prof.*). El interruptor vive
  // aqui, junto a los registros que se tocan por instruccion; los 4 KB de contadores
  // viven al final del struct (ver profPc), para no meter una pagina fria entre los
  // GPR escalares y los registros vectoriales, que si son calientes los dos.
  bool profOn = false;

  // --- vector unit -----------------------------------------------------------
  R128 vpr[32];
  R128 acch, accm, accl;               // 48-bit accumulator, split into slices
  R128 vcoh, vcol, vcch, vccl, vce;    // VCO / VCC / VCE flag halves
  u16  divin = 0, divout = 0; bool divdp = false;

  // --- reciprocal ROMs (generated at construction) ---------------------------
  u16 reciprocals[512];
  u16 invSqrts[512];

  // Estos dos los LEE el hilo CPU (el chequeo de reentrada del JIT mira `running` en cada
  // entrada de bloque, ~cada 3 instrucciones guest) y los escribe casi nunca: `running` solo
  // al arrancar la tarea y al BREAK. Todo lo que los rodea en la clase — el estado VU de
  // arriba, el latch de delay-slot y el presupuesto de abajo — lo escribe el hilo RSP en CADA
  // instruccion de microcodigo. Compartiendo linea de cache, esa lectura que deberia ser un
  // hit L1 se convierte en un fallo coherente porque el otro nucleo invalida la linea sin
  // parar: el perfilador de host medía 18% del tiempo TOTAL del emulador en ese unico `cmpb`.
  // Aislarlos en su propia linea (y rellenarla) lo elimina; no cambia ninguna semantica.
  // Contador de instrucciones de microcodigo ejecutadas (telemetria de velocidad). Lo escribe
  // el hilo RSP una vez por step(), lo lee el hilo CPU al refrescar el heartbeat.
  std::atomic<u64> cyclesRun{0};

  alignas(64) bool running = false;    // reentrancy guard (microcode may poke SP_STATUS)

  // Vector unit SSE fast path (8 lanes = 1 XMM). Bit-exact with the scalar reference
  // (proven by the differential fuzz, `--rspfuzz`). Disable with KESTREL_NORSPSSE for A/B.
  bool sse = true;
  // Camino rapido de las cargas/tiendas vectoriales (LSV/LLV/LDV/LQV y sus tiendas):
  // mismos bytes que el camino byte a byte, en operaciones de 64 bits. Apagable para
  // bisecar y para que --rspldfuzz pueda usar el camino lento como oraculo.
  bool vecfast = true;
  char coldPad_[62] = {};   // resto de la linea: nada mas debe caer aqui

  Rsp();

  // Differential VU fuzz: run each SSE-accelerated COP2 op against the scalar reference
  // over `iters` random states, return the number of mismatches (0 = bit-exact).
  auto fuzzVU(u64 iters) -> u64;
  auto fuzzLdSt(u64 iters) -> u64;

  // Throughput A/B of the VU fast path: time `iters` COP2 ops with the scalar loop vs
  // the 8-lane SSE path over a fixed op mix. Prints ns/op and speedup.
  auto benchVU(u64 iters) -> void;

  // Run the loaded microcode from the current SP_PC until it halts (BREAK) or a
  // safety budget is exhausted, in one blocking call. Retained for callers that
  // want the whole task at once; internally it is start() + step(budget).
  auto run() -> void;

  // Interleaved execution: start() arms the core from SP_PC (clearing HALT/BREAK,
  // resetting the delay-slot latch) without executing; step() advances up to
  // `maxInsns` instructions, persisting the branch-delay state across calls so the
  // RSP and CPU make progress together. The system run loop calls step() after each
  // CPU instruction while `running`, so a microcode that spins waiting on a SIGNAL
  // the CPU sets (and vice versa) resolves exactly as on hardware. On BREAK (or a
  // safety-budget hang) step() drops `running` and leaves SP_STATUS updated.
  auto start() -> void;
  auto step(u64 maxInsns) -> void;

private:
  u32  curpc = 0;
  bool branch = false; u32 branchTarget = 0; int branchState = 0;
  bool halt = false;
  bool broke = false;   // BREAK reached; HALT|BROKE published once the PC writeback is done
  bool inDelay = false; u32 pendingTarget = 0;   // persistent branch-delay latch
  u64  budget = 0;                                // remaining safety budget for this task

  // DMEM byte access (12-bit wrap), plus unaligned half/word big-endian helpers.
  auto rb(u32 a) const -> u8;
  auto wb(u32 a, u8 v) -> void;
  auto rHalf(u32 a) const -> u16;
  auto rWord(u32 a) const -> u32;
  auto wHalf(u32 a, u16 v) -> void;
  auto wWord(u32 a, u32 v) -> void;
  auto imword(u32 a) const -> u32;

  auto setR(int n, u32 v) -> void { if(n) r[n] = v; }
  auto take(u32 target) -> void { branch = true; branchTarget = target & 0xffc; }  // RSP PC word-aligned; JR/JALR ignore low 2 bits

  auto exec(u32 op) -> void;
  auto execCop2(u32 op) -> void;
  // 8-lane SSE fast path for the parallelizable COP2 ops. Returns true if it handled
  // `fn` (bit-exact with the scalar switch), false to fall through to scalar.
  auto execVuSse(u32 fn, const R128& vte, R128& S, R128& D) -> bool;
  auto execLoad(u32 op) -> void;
  auto execStore(u32 op) -> void;
  auto mfc0(int rt, int rd) -> void;
  auto mtc0(int rd, u32 v) -> void;

  // vector helpers
  auto accGet(int n) const -> u64;
  auto accSet(int n, u64 v) -> void;
  auto accSat(int n, bool slice, u16 neg, u16 pos) const -> u16;

public:
  // Contadores del muestreador (frios: solo se tocan con profOn). IMEM son 4 KB, o sea
  // un contador u32 por ranura de 4 bytes, lo que senala la rutina de microcodigo exacta.
  u32  profPc[1024] = {};
  u64  profTotal = 0;
  auto profClear() -> void { for(auto& c : profPc) c = 0; profTotal = 0; }
};

}  // namespace kestrel
