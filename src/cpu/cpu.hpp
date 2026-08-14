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

  // --- VR4300 primary caches (direct-mapped, write-back) ---------------------
  // Only RDRAM is cacheable; MMIO/cart accesses (KSEG1 / uncached) bypass. The N64
  // has no hardware cache coherence, so RAM and cache diverge exactly as on silicon:
  // a cached store lands in the D-cache dirty (RAM stale until writeback), and an
  // uncached store to a cached line leaves the cache stale until it is invalidated.
  // D-cache: 8 KB, 16-byte lines, 512 lines. I-cache: 16 KB, 32-byte lines, 512 lines.
  struct DCacheLine { u32 ptag = 0; bool valid = false; bool dirty = false; u8 data[16] = {}; };
  struct ICacheLine { u32 ptag = 0; bool valid = false; u8 data[32] = {}; };
  DCacheLine dcache[512] = {};
  ICacheLine icache[512] = {};

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
  static constexpr int kPcRing = 80;
  u64 pcRing[kPcRing] = {};
  u32 opRing[kPcRing] = {};
  u32 pcRingIdx = 0;
  bool pcRingOn = false;
  bool excTrace = false;
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

  auto connect(Memory* m) -> void { mem = m; }
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
  auto cacheable(u64 vaddr) -> bool;
  // Write-back D-cache byte-addressed access (phys already reverse-endian adjusted).
  // Aligned CPU accesses never straddle a 16-byte line, so a single line suffices.
  auto dcRead(u32 phys, u32 size) -> u64;
  auto dcWrite(u32 phys, u64 val, u32 size) -> void;
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
