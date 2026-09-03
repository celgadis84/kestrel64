#pragma once
// kestrel64 — R4300i CPU (MIPS III, 64-bit). M1: interpreter.
//
// GPRs are 64-bit; 32-bit ("word") ops sign-extend their result to 64 bits.
// Branch delay slots are modeled with the pc/nextPc pair (see step()).
// Address translation at M1 is the kernel-segment shortcut (paddr = vaddr &
// 0x1fffffff), which is exact for KSEG0/KSEG1; the TLB lands in M2.

#include "../core/types.hpp"
#include <string>
#include <vector>

namespace kestrel {

struct Memory;
namespace jit { struct CodeCache; }

struct CPU {
  // --- architectural state ---------------------------------------------------
  u64 gpr[32] = {};
  u64 pc = 0;
  u64 nextPc = 0;
  u64 curPc = 0;       // address of the instruction currently executing (for EPC/fault reports)
  u64 hi = 0, lo = 0;
  u64 cop0[32] = {};   // system control (Status/Cause/EPC/Count/Compare/...)
  u64 fpr[32] = {};    // COP1 float registers (raw bits)
  u32 fcr0 = 0, fcr31 = 0;
  u64 cp2latch = 0;    // COP2 has no functional unit on VR4300: MxC2 just latch reads/writes
  u64 cop0Unused = 0;  // shared latch backing the unused COP0 regs {7,21,22,23,31}
  bool llbit = false;

  // --- TLB (32 entries) ------------------------------------------------------
  struct TlbEntry { u64 hi = 0; u64 lo0 = 0; u64 lo1 = 0; u64 mask = 0; bool global = false; };
  TlbEntry tlb[32] = {};
  bool memAbort = false;   // set by translate() when the current access faulted (exception taken)
  // When set, translate()/tlbLookup() are side-effect-free: any path that would vector an
  // exception (AdE / TLB Invalid / Modified / Refill) instead returns the sentinel ~0ull
  // and touches no CPU state (no BadVAddr/EntryHi/memAbort/exception). Success paths are
  // byte-identical to the normal call, so the hot interpreter path is unchanged. Used by
  // the dynarec to translate a TLB-mapped block-entry PC without faulting.
  bool probing = false;
  // Cacheability of the most recent translate(): direct segments set it by segment
  // (KSEG0 cached / KSEG1 uncached); TLB-mapped segments copy the matched entry's C
  // field (C==2 -> uncached). cacheable() consults this for mapped addresses so an
  // uncached TLB mapping (C=2) bypasses the D-cache exactly as on hardware.
  bool xlatCacheable = true;

  // softTLB de una entrada para la traducción de PC del dynarec: cachea la última página
  // guest (vpn) resuelta por tlbProbePhys → phys base + cacheabilidad, evitando el scan
  // lineal de 32 entradas en cada entrada de bloque desde el mismo código. El tag incluye
  // el ASID, así que un cambio de contexto (ASID distinto) es un miss natural que re-prueba.
  // Solo hay que invalidarla cuando cambia el contenido del TLB → tlbWrite pone valid=false.
  u64  jitTlbVpn = 0;
  u32  jitTlbAsid = ~0u;
  u32  jitTlbPhys = 0;        // phys base (bits [31:12]) de la página cacheada
  bool jitTlbCacheable = false;
  bool jitTlbValid = false;

  // Generacion del MAPEO virtual: contenido del TLB + ASID. El dynarec enlaza bloques
  // TLB-mapeados traduciendo el destino EN TIEMPO DE COMPILACION, y esa traduccion solo vale
  // mientras el mapeo no cambie: cada TLBWI/TLBWR y cada cambio real de ASID sube la cuenta y
  // el driver desenlaza todo antes del siguiente despacho. No sirve xlatEpoch, que sube ademas
  // en cada excepcion/ERET (cambio de MODO, que no remapea nada): desenlazar en cada
  // interrupcion dejaria el enlace muerto en la practica.
  u64  tlbGen = 0;

  // Block-linking Step 3: contabilidad de la CADENA de bloques enlazados. Un bloque enlazado
  // salta directo al sucesor sin volver al driver, así que la contabilidad que el driver hace
  // al retornar (retired/Count/Random) la difiere al prólogo del sucesor:
  //  - jitPending = ops retiradas por los bloques ya ejecutados de la cadena y AÚN sin commitear.
  //    La salida de control enlazada lo incrementa; el prólogo del sucesor lo commitea y lo pone
  //    a 0 (así, en cualquier retorno al driver, pending==0 y el valor devuelto es exacto).
  //  - jitChain = enlaces consumidos desde la última entrada por el driver. Presupuesto duro:
  //    sin él, un bucle auto-enlazado no devolvería el control hasta el borde del timer.
  u32  jitPending = 0;
  u32  jitChain = 0;
  //  - jitChainOps = ops ya commiteadas por la cadena en ESTA entrada del driver. jitTryBlock
  //    las suma a las del último bloque: quien llama (stepCpu) tiene que ver el total real, o
  //    la ventana de 750k ops/campo se estiraría y el VI llegaría tarde.
  //  - jitOpsBudget = ops que aún caben en la ventana del bucle del sistema. Un eslabón
  //    enlazado no arranca si no cabe entero, así que la cadena NO desborda el límite de
  //    campo más de lo que ya lo hace un bloque suelto.
  u32  jitChainOps = 0;
  u32  jitOpsBudget = 0;
  //  - jitGuard = ops que la cadena puede encadenar SIN volver a llamar al trampolin de
  //    re-validacion. Lo fija el trampolin: es el minimo entre las ops que faltan para el
  //    borde Count==Compare, lo que queda de ventana del bucle del sistema y un tope duro.
  //    Solo dos cosas pueden invalidar el permiso sin pasar por el trampolin — ese borde de
  //    timer (determinista, contado en ops) y una interrupcion asincrona del RCP (que el
  //    prologo mira en linea) — asi que el resto de eslabones se saltan la llamada entera.
  //    El driver lo pone a 0 en cada entrada: un permiso nunca cruza dos entradas.
  u32  jitGuard = 0;

  // --- VR4300 primary caches (direct-mapped, write-back) ---------------------
  // Only RDRAM is cacheable; MMIO/cart accesses (KSEG1 / uncached) bypass. The N64
  // has no hardware cache coherence, so RAM and cache diverge exactly as on silicon:
  // a cached store lands in the D-cache dirty (RAM stale until writeback), and an
  // uncached store to a cached line leaves the cache stale until it is invalidated.
  // D-cache: 8 KB, 16-byte lines, 512 lines. I-cache: 16 KB, 32-byte lines, 512 lines.
  // El tag lleva DENTRO el estado de la linea, igual que la VR4300 (PTagLo y PState comparten
  // la palabra de tag): tagv = (phys & ~0xf) | 1 con la linea valida, bit0 a 0 sin ella. Asi
  // "es la linea que busco Y esta valida" es UNA comparacion de 32 bits en el camino rapido
  // del dynarec, en vez de un cmp de tag mas un cmp de bandera con dos saltos. El relleno a
  // 32 B (potencia de dos) ademas convierte idx*24 (lea x3 + shl 3) en un solo shl 5, y deja
  // cada linea dentro de una sola linea de cache del anfitrion en lugar de a caballo.
  struct alignas(32) DCacheLine {
    u32 tagv = 0; u32 dirty = 0; u8 data[16] = {}; u8 pad[8] = {};
    auto ptag()  const -> u32  { return tagv & ~0xfu; }
    auto valid() const -> bool { return (tagv & 1u) != 0; }
  };
  // seq = numero de relleno. Sube en CADA icFill; con (valid && ptag igual && seq igual)
  // los 32 bytes son BIT A BIT los mismos que la ultima vez que se miraron: el unico
  // camino que cambia data[] es icFill. Deja validar un bloque JIT por linea y no por op.
  struct ICacheLine { u32 ptag = 0; bool valid = false; u32 seq = 0; u8 data[32] = {}; };
  DCacheLine dcache[512] = {};
  // Tamano de RDRAM copiado aqui para que el camino rapido de memoria del dynarec lo compruebe
  // con un cmp contra un campo del propio CPU, en vez de perseguir mem->rdram.size() (puntero
  // + vector). Vale 0 mientras no haya bus atado, y ese 0 hace fallar la comprobacion de rango,
  // que es justo el "if(!mem) return 0" del helper. Lo refresca la ruta de compilacion de
  // bloques; Memory::reset() solo corre en el arranque, antes de compilar nada.
  u32 jitRdramSz = 0;
  // Punto de vigilancia o write-through de diagnostico armado. Vive aqui arriba, en la parte
  // publica, porque el camino rapido de memoria del dynarec lo consulta desde el codigo
  // emitido: con la bandera puesta, un store se va al helper para que pase por dcWriteDbg.
  bool dcDbgOn = false;
  // Guardia UNICA del camino rapido de store: bit0 = depuracion armada (punto de vigilancia o
  // write-through), bit1 = MI_MODE con el modo repeticion armado (el siguiente store a RDRAM se
  // difunde por la pagina). Juntarlas en un byte del propio CPU le ahorra al codigo emitido
  // perseguir mem (una carga de puntero) y un segundo cmp/jne en CADA store.
  u8 stGuard = 0;
  enum : u8 { StGuardDbg = 1, StGuardRepeat = 2 };
  ICacheLine icache[512] = {};
  u32 icSeq = 0;                 // sello de relleno de I-cache (0 = nunca rellenada)

  // --- interpreter fetch fast-path -------------------------------------------
  // Memoiza SOLO la traducción de la línea de I-cache (32 B) que se está ejecutando:
  // mientras el PC no salga de la línea y el contexto de traducción no cambie, se
  // salta translate()+cacheable() — el coste per-fetch dominante (un scan lineal del
  // TLB de 32 entradas en código mapeado, p.ej. PD). NO cachea los bytes: el fetch
  // sigue pasando por icFetch(phys), así que la semántica de I-cache y de código
  // automodificable (snapshot stale hasta invalidar) queda EXACTAMENTE igual.
  //
  // Seguridad: una línea de 32 B nunca cruza una página TLB (mínimo 4 KB, alineada),
  // así que las 8 instrucciones comparten mapeo — si la primera tradujo sin fallo, el
  // resto también. xlatEpoch se incrementa en cada evento que puede alterar una
  // traducción (TLBWI/R, mtc0/dmtc0 a Status o EntryHi, entrada de excepción, ERET);
  // un epoch distinto fuerza miss y re-traduce. El oráculo es el propio intérprete
  // vía systemtest (casos TLB/exc/icache) — cualquier divergencia se caza ahí.
  u64  fetchLineVBase = 1;      // vaddr base (pc & ~31) COMPLETO 64-bit; 1 = imposible → miss.
                                // Debe ser 64-bit: en modo 64-bit dos PC con igual low-32 pero
                                // distinta región alta (xkphys/xkuseg/ckseg…) traducen distinto.
  u32  fetchLinePhys  = 0;      // phys base (post-reXor) de esa línea
  u32  fetchLineReXor = 0;      // flip de offset intra-línea por reverse-endian (0 salvo User+RE)
  u32  fetchLineEpoch = ~0u;    // valor de xlatEpoch con el que se llenó
  bool fetchLineCache = false;  // ¿la línea va por I-cache (true) o bus directo (false)?
  u32  xlatEpoch      = 0;      // generación de traducción; ++ invalida el fetch fast-path
  auto bumpXlat() -> void { ++xlatEpoch; }   // llamar en todo cambio de mapeo/modo

  // --- exception / interrupt state -------------------------------------------
  bool inDelay = false;      // the instruction at pc sits in a branch delay slot
  bool justBranched = false; // the instruction just executed was a branch/jump
  bool jitDelaySlot = false; // la op interpretada del JIT ocupa una ranura de retardo
  bool timerIntr = false;    // Count==Compare latch (Cause IP7)
  u32  randomReload = 0;      // COP0 write hazard: a Wired write reloads Random=31 one
                             // instruction late (2 = armed this step, 1 = reload lands next end)

  // --- run control -----------------------------------------------------------
  bool  halted = false;
  std::string haltReason;
  u64   retired = 0;       // instructions retired
  u32   lastUnimplemented = 0;
  u64   exceptions = 0;    // exceptions/interrupts taken

  // Control-transfer ring buffer (debug): last taken branches/jumps.
  static constexpr int kJumpLog = 48;
  u64 jlogSrc[kJumpLog] = {};
  u64 jlogDst[kJumpLog] = {};
  u32 jlogOp [kJumpLog] = {};
  u32 jlogIdx = 0;

  // Coarse PC-page sampler (debug KESTREL_PCSAMPLE): counts visits per 4 KB page.
  static constexpr int kSampPages = 4096;   // covers 0x80000000..0x81000000
  u32 sampCount[kSampPages] = {};
  u64 sampTotal = 0;
  u64 excCodeHist[32] = {};   // total exceptions per ExcCode

  // Physical-PC execution profiler (opt-in via MCP prof.*). Buckets by physical
  // address so KSEG0 and TLB-mapped code that alias the same RDRAM (e.g. PD runs at
  // VA 0x70xxxxxx) fall in the same bucket — a virtual-PC sampler would miss it.
  // Backing is allocated on first enable; the hot loop pays a single bool test when off.
  static constexpr u32 kProfShift   = 4;                        // 16-byte (4-instr) resolution
  static constexpr u32 kProfBuckets = (8u << 20) >> kProfShift; // 8 MB RDRAM / 16 = 512 K
  bool  profOn = false;
  std::vector<u32> profBuckets;                                 // sized kProfBuckets on enable
  u64   profTotal = 0;
  auto  profEnable(bool on) -> void;                            // alloc + clear when enabling
  auto  profClear() -> void;

  // Exception ring buffer (debug KESTREL_EXCTAIL): last N exceptions, dumped on exit.
  static constexpr int kExcRing = 40;
  u32 excRingCode[kExcRing] = {};
  u64 excRingEpc [kExcRing] = {};
  u64 excRingBad [kExcRing] = {};
  u64 excRingRet [kExcRing] = {};
  u32 excRingIdx = 0;
  bool excTail = false;

  // Debug breakpoint (env KESTREL_BP): halt when pc first reaches bpAddr.
  // With KESTREL_BPTRACE set, log regs on every hit instead of halting.
  u64 bpAddr = 0;
  bool bpTrace = false;
  u64 bpHits = 0;
  bool trapWild = false;
  // Debug (KESTREL_PCRING): ring of last executed (pc,op) dumped on the line-666 leak fire.
  static constexpr int kPcRing = 4096;
  u64 pcRing[kPcRing] = {};
  u32 opRing[kPcRing] = {};
  u32 pcRingIdx = 0;
  bool pcRingOn = false;
  auto pcRingDump(u32 n) -> void;
  // Vigilancia de una palabra fisica concreta (KESTREL_WATCHP): apunta quien la escribe
  // desde la CPU. Si la palabra cambia sin aparecer aqui, el escritor fue un DMA/RDP.
  u32 wPhys = 0;
  struct WEnt { u64 ret; u32 pc; u32 val; };
  WEnt wRing[128] = {};
  u32  wIdx = 0;
  auto wNote(u32 val) -> void { wRing[wIdx % 128] = WEnt{ retired, (u32)curPc, val }; wIdx++; }
  auto wDump(u32 n) -> void;
  bool excTrace = false;
  // Cuenta de excepciones "imposibles" ya volcadas por KESTREL_EXCODD (ver takeException).
  u64  oddExc = 0;
  bool fpDbg = false;
  bool fpTrace = false;
  int  fpTraceEret = 0;
  int  fpWatchN = 0;
  int  fpDbgN = 0;
  bool huftTrap = false;
  u32  audioHook = 0;        // KESTREL_AUDIOHOOK: vaddr of n_alAudioFrame, stubbed to empty return
  bool haltUnimpl = false;   // KESTREL_HALT_UNIMPL: halt+dump on unknown op instead of RI
  u64 unimplCount = 0;       // reserved/unknown ops turned into RI exceptions
  u64 maxInsn = 0;

  // OR of every per-instruction debug/trap that lives in the step() prologue
  // (breakpoints, wild-pc traps, pc-window logging, pc-sampling, audio hook,
  // huft trap, exc-tail, maxinsn cap). Normal runs leave this false so the whole
  // debug prologue collapses to a single not-taken branch instead of ~12. Set by
  // refreshDebugArmed() after the env flags are parsed and whenever a trap field
  // is changed at runtime (e.g. telemetry sets a breakpoint).
  bool debugArmed = false;
  auto refreshDebugArmed() -> void;
  auto stepTraps() -> bool;   // cold, out-of-line debug/trap prologue (see cpu.cpp)

  Memory* mem = nullptr;

  // COP0 register indices we name.
  enum Cop0 { C0_Index=0, C0_Random=1, C0_EntryLo0=2, C0_EntryLo1=3, C0_Context=4,
              C0_PageMask=5, C0_Wired=6, C0_BadVAddr=8, C0_Count=9, C0_EntryHi=10,
              C0_Compare=11, C0_Status=12, C0_Cause=13, C0_EPC=14, C0_PRId=15, C0_Config=16,
              C0_XContext=20, C0_ErrorEPC=30 };
  enum Access { AccRead=0, AccWrite=1, AccFetch=2 };

  auto connect(Memory* m) -> void;   // ata el bus y le pasa la direccion de stGuard
  auto reset() -> void;
  auto fastBoot(u32 entryPoint) -> void;  // HLE IPL3 hand-off state

  // Execute one instruction (including its effect on pc/nextPc). No-op if halted.
  auto step() -> void;
  // Cache-coherent read of a physical RDRAM byte: returns the value the CPU would
  // see (dirty D-cache line shadows RAM). Read-only — no fill, no side effects.
  // Lets external observers (telemetry) inspect kernel structs the CPU wrote through
  // the write-back cache but has not yet flushed to the RDRAM backing store.
  auto peekPhysCoherent(u32 phys) -> u8;

  // Decode a word into a short mnemonic string (for telemetry disasm).
  static auto disasm(u32 op, u64 pc) -> std::string;

private:
  // r0 is hardwired to zero: writes through set() are dropped for index 0.
  inline auto set(u32 i, u64 v) -> void { if(i) gpr[i] = v; }

  auto read8 (u64 vaddr) -> u8;
  auto read16(u64 vaddr) -> u16;
  auto read32(u64 vaddr) -> u32;
  auto read64(u64 vaddr) -> u64;
  auto write8 (u64 vaddr, u8  v) -> void;
  auto write16(u64 vaddr, u16 v) -> void;
  auto write32(u64 vaddr, u32 v) -> void;
  auto write64(u64 vaddr, u64 v) -> void;
  auto seenWatch(u64 paddr, u32 size) -> void;   // debug: track SEEN_EXCEPTION discriminant

  // Cacheability of a data/fetch access: KSEG1 (0xA0000000..0xBFFFFFFF) and the
  // uncached XKPHYS windows bypass; KSEG0 and cached-mapped regions go through the
  // primary cache. Only accesses that also land in RDRAM are actually cached here.
  inline auto cacheable(u64 vaddr) -> bool {
    u32 seg = (u32)vaddr & 0xE000'0000u;
    if(seg == 0x8000'0000u) return true;    // KSEG0 / ckseg0 (cached)
    if(seg == 0xA000'0000u) return false;   // KSEG1 / ckseg1 (uncached)
    return xlatCacheable;                   // TLB-mapped: per-entry C field
  }
  // Camino rapido de traduccion, en linea en el llamador. Cubre lo unico que hace un juego
  // de N64 en la practica: direccion de compatibilidad (los 32 bits altos son la extension de
  // signo del bit 31) dentro de kseg0/kseg1, con la CPU en modo kernel y direccionamiento de
  // 32 bits (Status.KX=0). Esos dos segmentos son DIRECTOS -- no pasan por la TLB, no pueden
  // fallar y no miran el ASID --, asi que la traduccion entera es un AND, exactamente el
  // `return va & 0x1FFF'FFFF` de translate(). Tampoco tocan xlatCacheable, igual que alli:
  // la cacheabilidad de esos segmentos la decide el segmento (ver cacheable()).
  // Devuelve false cuando NO aplica; entonces el llamador llama a translate(), que resuelve
  // el caso general (TLB, 64 bits, usuario/supervisor, xkphys, AdE).
  inline auto xlatDirect(u64 v, u64& pa) const -> bool {
    u32 st = (u32)cop0[C0_Status];
    // kernel = EXL/ERL puestos o KSU==0; ademas KX (bit 7) claro para quedarse en 32 bits.
    bool kernel32 = ((st & 0x6u) != 0 || (st & 0x18u) == 0) && (st & 0x80u) == 0;
    if(__builtin_expect(!kernel32, 0)) return false;
    if(__builtin_expect((s64)v != (s32)(u32)v, 0)) return false;
    if(__builtin_expect(((u32)v & 0xC000'0000u) != 0x8000'0000u, 0)) return false;
    pa = (u32)v & 0x1FFF'FFFFu;
    return true;
  }
  // Envoltorio: camino directo en linea y, si no aplica, la llamada de siempre.
  inline auto xlat(u64 v, Access acc) -> u64 {
    u64 p;
    if(__builtin_expect(xlatDirect(v, p), 1)) return p;
    return translate(v, acc);
  }
  // Write-back D-cache byte-addressed access (phys already reverse-endian adjusted).
  // Aligned CPU accesses never straddle a 16-byte line, so a single line suffices.
  // dcRead/dcWrite son EN LINEA: el llamador del JIT conoce el tamano como constante, asi que
  // el switch desaparece y con el la llamada. El camino de fallo (flush+fill) sigue fuera de
  // linea, que es donde debe estar el codigo frio.
  inline auto dcRead(u32 phys, u32 size) -> u64 {
    u32 idx  = (phys >> 4) & 0x1ff;
    u32 base = phys & ~0xfu;
    DCacheLine& l = dcache[idx];
    if(__builtin_expect(l.tagv != (base | 1u), 0)) { dcFlush(idx); dcFill(idx, base); }
    u32 off = phys & 0xf;
    // El valor guest es big-endian dentro de la linea; el anfitrion es little-endian. Un
    // memcpy del ancho exacto + bswap da el MISMO resultado que el bucle byte a byte.
    switch(size) {
      case 1: return l.data[off];
      case 2: { u16 v; __builtin_memcpy(&v, &l.data[off], 2); return __builtin_bswap16(v); }
      case 4: { u32 v; __builtin_memcpy(&v, &l.data[off], 4); return __builtin_bswap32(v); }
      case 8: { u64 v; __builtin_memcpy(&v, &l.data[off], 8); return __builtin_bswap64(v); }
      default: break;
    }
    u64 v = 0;
    for(u32 i = 0; i < size; i++) v = (v << 8) | l.data[off + i];   // big-endian
    return v;
  }
  inline auto dcWrite(u32 phys, u64 val, u32 size) -> void {
    u32 idx  = (phys >> 4) & 0x1ff;
    u32 base = phys & ~0xfu;
    DCacheLine& l = dcache[idx];
    if(__builtin_expect(l.tagv != (base | 1u), 0)) { dcFlush(idx); dcFill(idx, base); }
    u32 off = phys & 0xf;
    switch(size) {
      case 1: l.data[off] = (u8)val; break;
      case 2: { u16 v = __builtin_bswap16((u16)val); __builtin_memcpy(&l.data[off], &v, 2); break; }
      case 4: { u32 v = __builtin_bswap32((u32)val); __builtin_memcpy(&l.data[off], &v, 4); break; }
      case 8: { u64 v = __builtin_bswap64(val);      __builtin_memcpy(&l.data[off], &v, 8); break; }
      default: for(u32 i = 0; i < size; i++) l.data[off + i] = (u8)(val >> (8 * (size - 1 - i))); break;
    }
    l.dirty = true;
    // Cola de depuracion (punto de vigilancia y write-through de diagnostico): una sola
    // bandera armada de antemano, y el trabajo fuera de linea.
    if(__builtin_expect(dcDbgOn, 0)) dcWriteDbg(phys, val, size);
  }
  auto dcWriteDbg(u32 phys, u64 val, u32 size) -> void;   // solo con depuracion armada
  auto dcFlush(u32 idx) -> void;              // push a dirty line to RDRAM, clear dirty
  auto dcFill(u32 idx, u32 base) -> void;     // load 16 bytes RDRAM -> line
  auto icFetch(u32 phys) -> u32;              // instruction fetch through the I-cache
  auto icFill(u32 idx, u32 base) -> void;     // load 32 bytes RDRAM -> line
  auto cacheOp(u32 op, u64 vaddr) -> void;    // the CACHE instruction (index/hit ops)
  // Unaligned data access raises AdEL/AdES (checked before translation). Returns
  // true (and vectors the exception) when vaddr is not naturally aligned to size.
  inline auto alignBad(u64 v, u32 size, Access acc) -> bool {
    if(v & (u64)(size - 1)) { setBadVAddr(v); memAbort = true; takeException(acc == AccWrite ? 5 : 4); return true; }
    return false;
  }

  // RDRAM init/repeat broadcast: while MI_MODE armed it, the next store to RDRAM is
  // replicated across a byte span (the whole source register drives the datapath, so
  // this needs the full 64-bit reg, not the size-truncated store value). Consumes the
  // arm; returns true if the store was handled here and the normal path must be skipped.
  auto storeRepeat(u32 phys, u64 reg, u32 sz) -> bool;

  // Sub-word store to cartridge space latches onto the 16-bit PI bus. The stored byte
  // rides the 2-byte bus lane (addr bit0) alongside its register neighbour, so the full
  // 64-bit source register is needed — not the size-truncated store value. Returns true
  // if `phys` is cart space and the latch was written (skip the normal path).
  auto storeCart(u32 phys, u64 reg, u32 width) -> bool;

  auto execute(u32 op) -> void;
  // Rastro de EXL: quien lo puso a 1 y cuando se limpio. Con EXL=1 el nucleo no
  // acepta interrupciones, asi que un EXL pegado deja la maquina viva pero sorda.
  u64 exlSetRet = 0, exlClrRet = 0;  u32 exlSetPc = 0;  u8 exlSetSrc = 0;  // 1=excepcion 2=mtc0
  auto writeCop0(u32 reg, u64 v) -> void;  // MTC0/DMTC0 with VR4300 per-register write masks
  auto readCop0(u32 reg) -> u64;           // MFC0/DMFC0 with unused-latch + read-only regs
  // FP register access honoring the FR bit (Status bit26). FR=1: 32 independent
  // 64-bit regs. FR=0 (half mode): 16 pairs — the even reg holds the full 64-bit,
  // an odd 32-bit access hits the high half of its even partner.
  auto fprGet32(u32 i) -> u32;
  auto fprSet32(u32 i, u32 v) -> void;
  auto fprGet64(u32 i) -> u64;
  auto fprSet64(u32 i, u64 v) -> void;
  auto special(u32 op) -> void;   // SPECIAL (opcode 0)
  auto regimm(u32 op) -> void;    // REGIMM (opcode 1)
  auto cop0op(u32 op) -> void;
  auto emuxOp(u32 op) -> void;   // n64-systemtest emulator extensions (xdetect/xlog/xioctl)
  auto cop1op(u32 op) -> void;
  auto cop2op(u32 op) -> void;

  auto branch(bool taken, u64 target) -> void;   // sets nextPc if taken
  auto halt(const std::string& why) -> void { halted = true; haltReason = why; }
  auto unimplemented(u32 op) -> void;

  // Exception/interrupt delivery.
  auto checkInterrupts() -> void;         // full check (out-of-loop callers)
  auto deliverInterrupt() -> void;        // cold: vector an enabled pending interrupt

public:
  // --- dynarec (Etapa 2a) ----------------------------------------------------
  // Cache de bloques compilados (x86-64) para runs secuenciales de ops ALU seguras.
  // Gated por KESTREL_JIT en stepCpu; el intérprete es el fallback para todo lo demás.
  jit::CodeCache* jitCache = nullptr;
  auto jitTryBlock() -> u32;              // ejecuta un bloque; devuelve nº ops (0 = declina)
  // Re-chequeo de reentrada de bloque (block-linking Step 1): muestrea interrupt + borde de
  // timer EXACTAMENTE como el driver. Devuelve 1 = seguro correr K ops; 0 = bail a ruta lenta
  // (entrega de interrupt pendiente o cruzaría Count==Compare). Lo llama el prólogo emitido.
  auto jitReenterProceed(u32 K) -> u32;
  // Lectura de una palabra de instrucción por el compilador de bloques (mismo valor
  // que icFetch, pero expuesto para el codegen en jit.cpp sin abrir toda la I-cache).
  auto jitFetchWord(u32 phys) -> u32 { return icFetch(phys); }
  // Ejecuta un load/store simple-alineado (Etapa 2b) espejando EXACTO el intérprete.
  // Devuelve 1 = hecho limpio; 0 = faultaría (misalign/TLB/ADE) → el bloque hace bail y
  // el intérprete re-ejecuta la op para vectorizar la excepción. Nunca vectoriza aquí.
  auto jitMem(u32 op) -> u8;
  // Version ESPECIALIZADA por opcode del helper de memoria del JIT. El opcode es constante
  // de plantilla (tamano, signo, store) y la direccion y el dato llegan ya calculados en
  // registros desde el bloque, que los tiene residentes: el helper deja de decodificar la op
  // y de leer cpu->gpr por el puntero. El camino de acceso es el MISMO (translate/dcRead/
  // dcWrite/storeRepeat/storeCart/wordStoreQuirk), byte a byte.
  template<u32 OPc> auto jitMemOp(u64 a, u32 rt, u64 rtVal) -> u8;
  template<u32 RSc> auto jitCop1Move(u32 op, u32 off) -> u8;
  template<u32 FN, u32 FMT> auto jitCop1Alu(u32 op, u32 off) -> u8;
  template<u32 FN, u32 FMT> auto jitCop1AluChk(u32 op, u32 off) -> u8;
  template<u32 KIND> auto jitCop1Cvt(u32 op, u32 off) -> u8;
  template<u32 KIND> auto jitCop1CvtW(u32 op, u32 off) -> u8;
  template<u32 KIND> auto jitCop1CvtChk(u32 op, u32 off) -> u8;
  template<u32 FMT> auto jitCop1Cmp(u32 op, u32 off) -> u8;
  template<u32 FMT> auto jitCop1CmpChk(u32 op, u32 off) -> u8;
  // Ejecuta UNA op no compilable con el intérprete desde dentro de un bloque JIT.
  // `off` = desplazamiento en bytes de la op respecto a la entrada del bloque (pc).
  // Devuelve 1 si la op terminó normal (el bloque sigue), 0 si hubo excepción/parada:
  // en ese caso pc/nextPc ya describen el punto de reanudación correcto.
  auto jitInterpOp(u32 op, u32 off) -> u8;
  auto jitCTC1w(u32 op, u32 off) -> u8;
private:
  auto takeException(u32 excCode, bool tlbRefill = false, bool xtlb = false) -> void;
  // Coprocessor Unusable (ExcCode 11) with the Cause CE field set to the cop number.
  auto copUnusable(u32 cop) -> void { takeException(11); cop0[C0_Cause] = (s64)(s32)(((u32)cop0[C0_Cause] & ~0x3000'0000u) | ((cop & 3) << 28)); }
  // Set BadVAddr and the BadVPN2 fields of Context/XContext (updated by the VR4300
  // on address-error and TLB exceptions).
  auto setBadVAddr(u64 vaddr) -> void;

  // Translate a virtual address to physical for the given access (read/write/fetch).
  // On a TLB/address fault it sets memAbort, records the fault state, and vectors
  // the exception; the caller then aborts the access.
  auto translate(u64 vaddr, Access acc) -> u64;
  auto tlbLookup(u64 vaddr, Access acc, bool xtlb) -> u64;   // TLB search + fault vectoring
public:
  // Side-effect-free translate (probing=true). Returns physical address (32-bit in the
  // low bits) on success, or ~0ull if the address would fault / is unmapped. Also updates
  // xlatCacheable for mapped/xkphys hits. For the dynarec block-entry path.
  auto tlbProbePhys(u64 vaddr) -> u64 { bool s = probing; probing = true; u64 p = translate(vaddr, AccFetch); probing = s; return p; }
private:
  auto tlbWrite(u32 index) -> void;      // TLBWI/TLBWR: cop0 EntryHi/Lo/PageMask -> tlb[index]
  auto tlbRead(u32 index) -> void;       // TLBR: tlb[index] -> cop0 registers
  auto tlbProbe() -> void;               // TLBP: set cop0 Index from EntryHi match
  auto cpuMode() -> u32;                 // 0 kernel, 1 supervisor, 2 user (EXL/ERL force kernel)
  // 64-bit ops (doubleword ALU/loads/stores, LWU) are reserved in User/Supervisor
  // mode when that mode's addressing bit (UX/SX) is clear. Raises RI and returns
  // true if the current mode forbids them; the decoder must then abort the op.
  auto reserved64() -> bool;
  // Reverse-Endian (Status bit25): in User mode it flips the byte lane within the
  // aligned doubleword for both instruction fetch and data access — byte^7, half^6,
  // word^4, doubleword unchanged. Kernel/supervisor and RE=0 are pass-through. The
  // xor stays inside the doubleword, so page/translation are unaffected and a faulting
  // access still reports the raw (un-xored) vaddr in BadVAddr.
  inline auto reXor(u64 addr, u32 size) -> u64 {
    if(!((u32)cop0[C0_Status] & (1u<<25))) return addr;   // RE clear: common case
    if(cpuMode() != 2) return addr;                        // only User mode reverses
    return addr ^ (u64)(8 - size);
  }
  // Reverse-endian active (Status.RE set and running in User mode). Partial
  // load/store (LWL/LWR/LDL/LDR/SWL/SWR/SDL/SDR) flip their in-word byte index
  // when this holds; the aligned word/dword itself is already swapped by reXor.
  inline auto reOn() -> bool {
    return ((u32)cop0[C0_Status] & (1u<<25)) && cpuMode() == 2;
  }
};

}  // namespace kestrel
