// Standalone unit test for wild guest addresses.
//
// Guest code that walks off the end of RDRAM, misaligns a load, or dereferences an
// unmapped page must raise a *guest* exception (or read defined garbage) — never a host
// access violation and never a silent divergence between the interpreter and the dynarec.
// The JIT fastmem path (src/cpu/jit.cpp) short-circuits ckseg0 accesses with an inline
// bounds/alignment/kernel-mode guard, so every one of those guards needs an address that
// actually trips it, checked against the interpreter, which is the oracle.
//
// Each case runs the same little program twice on two fresh CPUs: once stepped by the
// interpreter, once dispatched through jitTryBlock(). We compare the architectural
// outcome (ExcCode, EPC, BadVAddr, the destination register) and, where the VR4300
// semantics are unambiguous (misalignment -> AdEL/AdES, unmapped KUSEG -> TLBL/TLBS),
// also assert the absolute value. Cases whose result is open-bus-ish (a read past the
// end of RDRAM) are only cross-checked: the point there is "no host crash, both engines
// agree", not a made-up number.
#include "../src/core/memory.hpp"
#include "../src/cpu/cpu.hpp"
#include <cstdio>
#include <memory>
#include <vector>

using namespace kestrel;

static int failures = 0;

// --- MIPS encoders ----------------------------------------------------------------
static u32 luiOp(int rt, u16 imm) { return 0x3C00'0000u | (u32)(rt & 31) << 16 | imm; }
static u32 oriOp(int rt, int rs, u16 imm) {
  return 0x3400'0000u | (u32)(rs & 31) << 21 | (u32)(rt & 31) << 16 | imm;
}
static u32 memOp(u32 op, int rt, int rs, s16 off) {
  return (op & 63) << 26 | (u32)(rs & 31) << 21 | (u32)(rt & 31) << 16 | (u16)off;
}
static u32 specOp(int rs, int rt, int rd, int sa, u32 funct) {
  return (u32)(rs & 31) << 21 | (u32)(rt & 31) << 16 | (u32)(rd & 31) << 11 |
         (u32)(sa & 31) << 6 | (funct & 63);
}
static constexpr u32 OP_LB = 0x20, OP_LH = 0x21, OP_LW = 0x23, OP_LWU = 0x27;
static constexpr u32 OP_SW = 0x2B, OP_LD = 0x37, OP_SD = 0x3F;
static constexpr u32 OP_LWC1 = 0x31, OP_SWC1 = 0x39, OP_LDC1 = 0x35;
static constexpr u32 FN_DSLL = 0x38, FN_DSLL32 = 0x3C, FN_DADDU = 0x2D;
static constexpr u32 NOP = 0;
static u32 selfLoop() { return 0x1000'FFFFu; }   // b . (never reached; keeps blocks terminated)

// Load a 32-bit-sign-extended constant into $rt (the canonical shape of every N64 pointer).
static void emitAddr32(std::vector<u32>& c, int rt, u32 a) {
  c.push_back(luiOp(rt, (u16)(a >> 16)));
  c.push_back(oriOp(rt, rt, (u16)a));
}
// Load a genuinely 64-bit (non-sign-extended) constant: hi32 shifted up, then the low half
// added in. Nothing on the N64 produces one of these on purpose; a corrupt register does.
static void emitAddr64(std::vector<u32>& c, int rt, int tmp, u32 hi, u32 lo) {
  c.push_back(luiOp(rt, (u16)(hi >> 16)));
  c.push_back(oriOp(rt, rt, (u16)hi));
  c.push_back(specOp(0, rt, rt, 0, FN_DSLL32));            // rt <<= 32
  c.push_back(oriOp(tmp, 0, (u16)(lo >> 16)));
  c.push_back(specOp(0, tmp, tmp, 16, FN_DSLL));           // tmp = lo_hi16 << 16
  c.push_back(specOp(rt, tmp, rt, 0, FN_DADDU));
  c.push_back(oriOp(rt, rt, (u16)lo));
}

// --- one run of a program ---------------------------------------------------------
// ckseg0, well clear of the exception vectors. Sign-extended on purpose: in 32-bit kernel
// mode every valid VA is the sign extension of its low 32 bits, and a bare 0x80001000
// would fault on the *fetch* (AdEL) before any of these programs got to run.
static constexpr u64 PROG_VA = 0xFFFF'FFFF'8000'1000ull;
static constexpr int RT = 9;                     // destination / source register under test
static constexpr u64 POISON = 0x0BADC0DE'DEADBEEFull;

struct Outcome {
  bool  excTaken = false;
  u32   excCode  = 0;
  u64   epc = 0, badVAddr = 0, pc = 0, rt = 0;
  bool  jitRan = false;    // the dynarec actually compiled/ran something
  bool  halted = false;
};

// Runs `code` from PROG_VA. `useJit` picks the engine. Stops on the first exception
// (Status.EXL set) or when the program's instructions are exhausted.
static Outcome run(Memory& mem, const std::vector<u32>& code, bool useJit) {
  // Program into RDRAM through the physical view; ckseg0 aliases it 1:1.
  u32 phys = (u32)PROG_VA & 0x1FFF'FFFF;
  for(usize i = 0; i < code.size(); i++) {
    u32 w = code[i];
    mem.rdram[phys + 4 * i + 0] = (u8)(w >> 24);
    mem.rdram[phys + 4 * i + 1] = (u8)(w >> 16);
    mem.rdram[phys + 4 * i + 2] = (u8)(w >> 8);
    mem.rdram[phys + 4 * i + 3] = (u8)w;
  }
  auto cp = std::make_unique<CPU>();
  CPU& cpu = *cp;
  cpu.connect(&mem);
  cpu.reset();
  cpu.pc = PROG_VA;
  cpu.nextPc = PROG_VA + 4;
  cpu.gpr[RT] = POISON;

  Outcome o;
  const u32 ops = (u32)code.size();
  u32 done = 0;
  for(u32 guard = 0; guard < ops + 8 && done < ops; guard++) {
    if((u32)cpu.cop0[CPU::C0_Status] & 0x2) break;   // EXL: exception already vectored
    if(cpu.halted) break;
    if(useJit) {
      cpu.jitOpsBudget = ops - done;
      u32 k = cpu.jitTryBlock();
      if(k) { done += k; o.jitRan = true; continue; }
    }
    cpu.step();
    done++;
  }
  u32 status = (u32)cpu.cop0[CPU::C0_Status];
  u32 cause  = (u32)cpu.cop0[CPU::C0_Cause];
  o.excTaken = (status & 0x2) != 0;
  o.excCode  = (cause >> 2) & 0x1F;
  o.epc      = cpu.cop0[CPU::C0_EPC];
  o.badVAddr = cpu.cop0[CPU::C0_BadVAddr];
  o.pc       = cpu.pc;
  o.rt       = cpu.gpr[RT];
  o.halted   = cpu.halted;
  return o;
}

// --- case driver ------------------------------------------------------------------
enum ExcExp { EXC_NONE = -1, EXC_ANY = -2 };   // EXC_ANY = only cross-check the two engines

struct Case {
  const char*       name;
  std::vector<u32>  code;
  int               expExc;      // ExcCode, EXC_NONE or EXC_ANY
  u64               expBad;      // expected BadVAddr when expExc >= 0 (0 = don't check)
  u64               faultAt;     // VA of the faulting instruction, for EPC (0 = don't check)
};

static void fail(const char* name, const char* what) {
  std::printf("    *** FAIL *** %s: %s\n", name, what);
  failures++;
}

static void runCase(Memory& mem, const Case& c) {
  Outcome i = run(mem, c.code, /*useJit=*/false);
  Outcome j = run(mem, c.code, /*useJit=*/true);

  std::printf("  %-42s int exc=%s(%u) epc=%08llx bad=%016llx rt=%016llx | jit %s exc=%s(%u)\n",
              c.name,
              i.excTaken ? "si" : "no", i.excCode,
              (unsigned long long)i.epc, (unsigned long long)i.badVAddr,
              (unsigned long long)i.rt,
              j.jitRan ? "corrio" : "declino",
              j.excTaken ? "si" : "no", j.excCode);

  if(i.halted || j.halted) fail(c.name, "la CPU se detuvo (halt) — no deberia");

  // Cross-check: the dynarec must reproduce the interpreter exactly.
  if(i.excTaken != j.excTaken) fail(c.name, "interp y jit no coinciden en si hay excepcion");
  else if(i.excTaken) {
    if(i.excCode != j.excCode)   fail(c.name, "ExcCode distinto entre interp y jit");
    if(i.epc != j.epc)           fail(c.name, "EPC distinto entre interp y jit");
    if(i.badVAddr != j.badVAddr) fail(c.name, "BadVAddr distinto entre interp y jit");
  }
  if(i.rt != j.rt) fail(c.name, "registro destino distinto entre interp y jit");

  // Absolute expectations, only where the VR4300 semantics are unambiguous.
  if(c.expExc == EXC_NONE) {
    if(i.excTaken) fail(c.name, "excepcion inesperada");
  } else if(c.expExc >= 0) {
    if(!i.excTaken) fail(c.name, "se esperaba excepcion y no la hubo");
    else {
      if(i.excCode != (u32)c.expExc) fail(c.name, "ExcCode no es el esperado");
      if(c.expBad && i.badVAddr != c.expBad) fail(c.name, "BadVAddr no es la direccion salvaje");
      if(c.faultAt && i.epc != c.faultAt) fail(c.name, "EPC no apunta a la instruccion que fallo");
    }
  }
}

// Program helper: base address in $8, then one memory op on it, then a self-loop trailer.
static std::vector<u32> prog32(u32 base, u32 op, s16 off) {
  std::vector<u32> c;
  emitAddr32(c, 8, base);
  c.push_back(memOp(op, RT, 8, off));
  c.push_back(selfLoop());
  c.push_back(NOP);
  return c;
}
static u64 faultPc(const std::vector<u32>& c) { return PROG_VA + 4 * (c.size() - 3); }

int main() {
  std::printf("wildmem: direcciones salvajes -> excepcion de invitado, nunca fallo del host\n");

  auto mp = std::make_unique<Memory>();
  Memory& mem = *mp;
  mem.reset(/*expansionPak=*/true);            // 8 MB
  const u32 rd8 = (u32)mem.rdram.size();
  std::printf("RDRAM = %u MB\n\n", rd8 >> 20);

  std::printf("Desalineacion (AdEL/AdES son ExcCode 4 y 5):\n");
  {
    auto c = prog32(0x8000'2002, OP_LW, 0);
    runCase(mem, { "lw a ckseg0+2 (desalineado)", c, 4, 0xFFFF'FFFF'8000'2002ull, faultPc(c) });
  }
  {
    auto c = prog32(0x8000'2001, OP_LH, 0);
    runCase(mem, { "lh a ckseg0+1 (desalineado)", c, 4, 0xFFFF'FFFF'8000'2001ull, faultPc(c) });
  }
  {
    auto c = prog32(0x8000'2000, OP_SW, 1);
    runCase(mem, { "sw a ckseg0+1 (desalineado)", c, 5, 0xFFFF'FFFF'8000'2001ull, faultPc(c) });
  }
  {
    auto c = prog32(0x8000'2004, OP_LD, 0);
    runCase(mem, { "ld a doubleword+4 (desalineado)", c, 4, 0xFFFF'FFFF'8000'2004ull, faultPc(c) });
  }
  {
    auto c = prog32(0x8000'2000, OP_SD, 4);
    runCase(mem, { "sd a doubleword+4 (desalineado)", c, 5, 0xFFFF'FFFF'8000'2004ull, faultPc(c) });
  }
  {
    auto c = prog32(0x8000'2002, OP_LWC1, 0);
    runCase(mem, { "lwc1 desalineado (fastmem COP1)", c, 4, 0xFFFF'FFFF'8000'2002ull, faultPc(c) });
  }
  {
    auto c = prog32(0x8000'2002, OP_SWC1, 0);
    runCase(mem, { "swc1 desalineado (fastmem COP1)", c, 5, 0xFFFF'FFFF'8000'2002ull, faultPc(c) });
  }

  std::printf("\nFuera del final de RDRAM (ckseg0 valido, fisico inexistente):\n");
  {
    auto c = prog32(0x8000'0000 + rd8, OP_LW, 0);
    runCase(mem, { "lw justo pasado el final de RDRAM", c, EXC_NONE, 0, 0 });
  }
  {
    auto c = prog32(0x8000'0000 + rd8, OP_SW, 0);
    runCase(mem, { "sw justo pasado el final de RDRAM", c, EXC_NONE, 0, 0 });
  }
  {
    auto c = prog32(0x8000'0000 + rd8 - 8, OP_LD, 0);
    runCase(mem, { "ld en el ultimo doubleword valido", c, EXC_NONE, 0, 0 });
  }
  {
    auto c = prog32(0x8000'0000 + rd8 - 4, OP_LD, 0);   // aligned to 4, not 8 -> AdEL first
    runCase(mem, { "ld a caballo del final (desalineado)", c, 4, 0, faultPc(c) });
  }
  {
    auto c = prog32(0x8000'0000 + rd8, OP_LDC1, 0);
    runCase(mem, { "ldc1 pasado el final de RDRAM", c, EXC_NONE, 0, 0 });
  }
  {
    auto c = prog32(0x9FFF'FFF0, OP_LW, 0);            // top of ckseg0: phys 0x1FFFFFF0
    runCase(mem, { "lw en la cima de ckseg0", c, EXC_NONE, 0, 0 });
  }
  {
    auto c = prog32(0x8000'0000 + rd8, OP_LB, 3);
    runCase(mem, { "lb pasado el final (sin alineacion que valga)", c, EXC_NONE, 0, 0 });
  }

  std::printf("\nKUSEG sin TLB (TLBL=2 / TLBS=3, refill):\n");
  {
    auto c = prog32(0x0000'1000, OP_LW, 0);
    runCase(mem, { "lw a kuseg no mapeado", c, 2, 0x0000'0000'0000'1000ull, faultPc(c) });
  }
  {
    auto c = prog32(0x0000'1000, OP_SW, 0);
    runCase(mem, { "sw a kuseg no mapeado", c, 3, 0x0000'0000'0000'1000ull, faultPc(c) });
  }
  {
    auto c = prog32(0xC000'0000, OP_LW, 0);            // ksseg/sseg, tambien mapeado por TLB
    runCase(mem, { "lw a ksseg no mapeado", c, 2, 0xFFFF'FFFF'C000'0000ull, faultPc(c) });
  }

  std::printf("\nHuecos de MMIO en KSEG1 (sin cache, sin dispositivo detras):\n");
  {
    auto c = prog32(0xA470'0000, OP_LW, 0);            // RI regs: existen, deben leer algo
    runCase(mem, { "lw a RI (kseg1, dispositivo real)", c, EXC_NONE, 0, 0 });
  }
  {
    auto c = prog32(0xA4C0'0000, OP_LW, 0);            // hueco entre bloques del RCP
    runCase(mem, { "lw a hueco del RCP (kseg1)", c, EXC_NONE, 0, 0 });
  }
  {
    auto c = prog32(0xBFF0'0000, OP_LW, 0);            // pasado el PIF, sin dispositivo
    runCase(mem, { "lw pasado el PIF (kseg1)", c, EXC_NONE, 0, 0 });
  }
  {
    auto c = prog32(0xA400'0000, OP_SW, 0);            // SP DMEM via kseg1: escritura legal
    runCase(mem, { "sw a SP DMEM (kseg1)", c, EXC_NONE, 0, 0 });
  }

  std::printf("\nPunteros de 64 bits no canonicos (registro corrompido):\n");
  {
    std::vector<u32> c;
    emitAddr64(c, 8, 10, 0x1234'5678, 0x8000'0000);
    c.push_back(memOp(OP_LW, RT, 8, 0));
    c.push_back(selfLoop());
    c.push_back(NOP);
    runCase(mem, { "lw con base 0x12345678_80000000", c, EXC_ANY, 0, 0 });
  }
  {
    std::vector<u32> c;
    emitAddr64(c, 8, 10, 0x1234'5678, 0x8000'0000);
    c.push_back(memOp(OP_SW, RT, 8, 0));
    c.push_back(selfLoop());
    c.push_back(NOP);
    runCase(mem, { "sw con base 0x12345678_80000000", c, EXC_ANY, 0, 0 });
  }
  {
    std::vector<u32> c;
    emitAddr64(c, 8, 10, 0x0000'0000, 0x8000'0000);   // 0x00000000_80000000: ckseg0 sin sext
    c.push_back(memOp(OP_LWU, RT, 8, 0));
    c.push_back(selfLoop());
    c.push_back(NOP);
    runCase(mem, { "lwu con ckseg0 sin extender signo", c, EXC_ANY, 0, 0 });
  }

  // Modo 4 MB genuino: la mitad alta del mapa de 8 MB deja de existir, y el guardia de
  // fastmem del JIT usa jitRdramSz, que debe seguir al tamaño real, no a un 8 MB fijo.
  std::printf("\nSin Expansion Pak (RDRAM de 4 MB):\n");
  auto mp4 = std::make_unique<Memory>();
  Memory& m4 = *mp4;
  m4.reset(/*expansionPak=*/false);
  const u32 rd4 = (u32)m4.rdram.size();
  std::printf("RDRAM = %u MB\n", rd4 >> 20);
  if(rd4 != 4u << 20) { std::printf("    *** FAIL *** reset(false) no da 4 MB\n"); failures++; }
  {
    auto c = prog32(0x8000'0000 + rd4 - 4, OP_LW, 0);
    runCase(m4, { "lw en la ultima palabra de los 4 MB", c, EXC_NONE, 0, 0 });
  }
  {
    auto c = prog32(0x8000'0000 + rd4, OP_LW, 0);
    runCase(m4, { "lw en el primer byte inexistente", c, EXC_NONE, 0, 0 });
  }
  {
    auto c = prog32(0x8000'0000 + rd4, OP_SW, 0);
    runCase(m4, { "sw en el primer byte inexistente", c, EXC_NONE, 0, 0 });
  }
  {
    auto c = prog32(0x8060'0000, OP_LD, 0);            // dentro del mapa de 8 MB, fuera del real
    runCase(m4, { "ld a 6 MB (solo existe con Expansion Pak)", c, EXC_NONE, 0, 0 });
  }

  std::printf("\n%s (%d fallos)\n", failures ? "*** HAY FALLOS ***" : "ALL PASS", failures);
  return failures ? 1 : 0;
}
