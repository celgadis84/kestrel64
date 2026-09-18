// Standalone unit test for the COP0 Watch exception (ExcCode 23).
//
// The VR4300 has a pair of registers, WatchLo (18) and WatchHi (19), that fire a precise
// exception when a data access touches the doubleword they name. WatchLo holds PAddr0 in
// bits [31:3] plus R (bit 1) and W (bit 0); bit 2 does not exist and reads back as zero.
// WatchHi holds PAddr1, the bits [35:32] of a physical address, which on the N64 can only
// ever be zero: nothing above 4 GB exists, so a non-zero PAddr1 can never match.
//
// Rules checked here, all taken from the VR4300 manual (U10504EJ7V0UM, 6.4.9):
//   - the compare is on the PHYSICAL address and at doubleword granularity;
//   - R traps loads, W traps stores, and each traps only its own direction;
//   - the exception is PRECISE: the access must not take effect;
//   - BadVAddr is NOT written (Watch is not an addressing error);
//   - with Status.EXL or Status.ERL set the watch is deferred, never taken, because the
//     exception vector itself would otherwise loop forever on its own stack.
//
// The interpreter is the oracle. While the watch is armed the driver (src/core/system.cpp)
// keeps the dynarec out of the loop, because the emitted code does not compare each address
// against WatchLo; the last case here re-creates the driver's rule and asserts the two
// engines only have to agree when the watch is disarmed.
#include "../src/core/memory.hpp"
#include "../src/cpu/cpu.hpp"
#include <cstdio>
#include <memory>
#include <vector>

using namespace kestrel;

static int failures = 0;
static void fail(const char* name, const char* what) {
  std::printf("    *** FAIL *** %s: %s\n", name, what);
  failures++;
}

// --- MIPS encoders ----------------------------------------------------------------
static u32 luiOp(int rt, u16 imm) { return 0x3C00'0000u | (u32)(rt & 31) << 16 | imm; }
static u32 oriOp(int rt, int rs, u16 imm) {
  return 0x3400'0000u | (u32)(rs & 31) << 21 | (u32)(rt & 31) << 16 | imm;
}
static u32 memOp(u32 op, int rt, int rs, s16 off) {
  return (op & 63) << 26 | (u32)(rs & 31) << 21 | (u32)(rt & 31) << 16 | (u16)off;
}
static u32 mtc0Op(int rt, int rd) { return 0x4080'0000u | (u32)(rt & 31) << 16 | (u32)(rd & 31) << 11; }
static constexpr u32 OP_LW = 0x23, OP_SW = 0x2B, OP_LB = 0x20, OP_SB = 0x28;
static constexpr u32 NOP = 0;

static void emitAddr32(std::vector<u32>& c, int rt, u32 a) {
  c.push_back(luiOp(rt, (u16)(a >> 16)));
  c.push_back(oriOp(rt, rt, (u16)a));
}
static void emitMtc0(std::vector<u32>& c, int rd, u32 v) {
  emitAddr32(c, 10, v);
  c.push_back(mtc0Op(10, rd));
}

// --- the fixture ------------------------------------------------------------------
static constexpr u64 PROG_VA  = 0xFFFF'FFFF'8000'1000ull;   // ckseg0, aliases phys 0x1000
static constexpr u32 DATA_PHYS = 0x0000'3000;               // the watched doubleword
// Sin cachear a proposito (kseg1): un store cacheado se queda en la D-cache y no llega a
// RDRAM, asi que no se podria comprobar "el acceso NO ha ocurrido" mirando la memoria. La
// fisica es la misma, que es lo unico que mira WatchLo. El caso cacheado se prueba aparte.
static constexpr u64 DATA_VA   = 0xFFFF'FFFF'A000'3000ull;
static constexpr u64 DATA_VA_C = 0xFFFF'FFFF'8000'3000ull;   // misma fisica, por la cache
static constexpr int RT = 9;                                // register under test
static constexpr int RB = 8;                                // base register
static constexpr u64 POISON  = 0x0BADC0DE'DEADBEEFull;      // pre-load value of $9
static constexpr u32 PRESET  = 0x1234'5678u;                // pre-store value in memory
static constexpr u64 EXC_VEC = 0xFFFF'FFFF'8000'0180ull;    // general vector, EXL clear

// WatchLo as the guest writes it: physical address in [31:3], R in bit 1, W in bit 0.
static u32 watchLo(u32 phys, bool r, bool w) {
  return (phys & 0xFFFF'FFF8u) | (r ? 2u : 0u) | (w ? 1u : 0u);
}

struct Outcome {
  u64  pc = 0, epc = 0, badVAddr = 0, rt = 0;
  u32  cause = 0, status = 0, memWord = 0;
  bool jitRan = false;
  bool trapped = false;     // vectored to the general exception entry point
};

// Runs `code` from PROG_VA and stops right after the last instruction of the program
// proper (the caller puts the access last). `preStatus` ORs bits into Status before the
// first step, which is how the EXL/ERL deferral cases are set up.
static Outcome run(Memory& mem, const std::vector<u32>& code, bool useJit, u32 preStatus = 0) {
  u32 phys = (u32)PROG_VA & 0x1FFF'FFFF;
  for(usize i = 0; i < code.size(); i++) {
    u32 w = code[i];
    mem.rdram[phys + 4 * i + 0] = (u8)(w >> 24);
    mem.rdram[phys + 4 * i + 1] = (u8)(w >> 16);
    mem.rdram[phys + 4 * i + 2] = (u8)(w >> 8);
    mem.rdram[phys + 4 * i + 3] = (u8)w;
  }
  // Known value in the watched doubleword, so a store that should not have happened is
  // visible as a change and a load that should not have happened is visible in $9.
  mem.rdram[DATA_PHYS + 0] = (u8)(PRESET >> 24);
  mem.rdram[DATA_PHYS + 1] = (u8)(PRESET >> 16);
  mem.rdram[DATA_PHYS + 2] = (u8)(PRESET >> 8);
  mem.rdram[DATA_PHYS + 3] = (u8)PRESET;

  auto cp = std::make_unique<CPU>();
  CPU& cpu = *cp;
  cpu.connect(&mem);
  cpu.reset();
  cpu.pc = PROG_VA;
  cpu.nextPc = PROG_VA + 4;
  cpu.gpr[RT] = POISON;
  if(preStatus) cpu.cop0[CPU::C0_Status] = (s64)(s32)((u32)cpu.cop0[CPU::C0_Status] | preStatus);

  Outcome o;
  const u32 ops = (u32)code.size();
  u32 done = 0;
  while(done < ops) {
    if(cpu.halted) break;
    // The driver's rule: with the watch armed the interpreter is the only engine that
    // implements it, so the dynarec is not allowed to run.
    if(useJit && !cpu.watchArmed) {
      cpu.jitOpsBudget = ops - done;
      u32 k = cpu.jitTryBlock();
      if(k) { done += k; o.jitRan = true; continue; }
    }
    cpu.step();
    done++;
  }

  o.pc       = cpu.pc;
  o.epc      = cpu.cop0[CPU::C0_EPC];
  o.badVAddr = cpu.cop0[CPU::C0_BadVAddr];
  o.rt       = cpu.gpr[RT];
  o.cause    = (u32)cpu.cop0[CPU::C0_Cause];
  o.status   = (u32)cpu.cop0[CPU::C0_Status];
  o.trapped  = (o.pc & 0xFFFF'FFFFull) == (EXC_VEC & 0xFFFF'FFFFull);
  o.memWord  = (u32)mem.rdram[DATA_PHYS] << 24 | (u32)mem.rdram[DATA_PHYS + 1] << 16 |
               (u32)mem.rdram[DATA_PHYS + 2] << 8 | (u32)mem.rdram[DATA_PHYS + 3];
  return o;
}

static u32 excCode(const Outcome& o) { return (o.cause >> 2) & 0x1F; }

// Program: arm WatchLo/WatchHi, put the data base in $8, then one memory op. The trailer
// is not executed: run() stops with the access.
static std::vector<u32> prog(u32 lo, u32 hi, u32 op, s16 off, u64 base = DATA_VA) {
  std::vector<u32> c;
  emitMtc0(c, 19, hi);
  emitMtc0(c, 18, lo);
  emitAddr32(c, RB, (u32)base);
  c.push_back(memOp(op, RT, RB, off));
  return c;
}
static u64 faultPc(const std::vector<u32>& c) { return PROG_VA + 4 * (c.size() - 1); }

// --- expectations -----------------------------------------------------------------
// `expTrap` = the Watch exception must fire; then the access must NOT have taken effect.
static void check(Memory& mem, const char* name, const std::vector<u32>& code, bool expTrap,
                  bool isStore, u32 preStatus = 0) {
  Outcome o = run(mem, code, /*useJit=*/false, preStatus);
  std::printf("  %-46s trap=%s exc=%2u epc=%08llx bad=%016llx rt=%016llx mem=%08x\n",
              name, o.trapped ? "si" : "no", excCode(o),
              (unsigned long long)o.epc, (unsigned long long)o.badVAddr,
              (unsigned long long)o.rt, o.memWord);

  if(expTrap) {
    if(!o.trapped) { fail(name, "no salto al vector de excepcion"); return; }
    if(excCode(o) != 23) fail(name, "ExcCode no es 23 (Watch)");
    if(!preStatus && o.epc != faultPc(code)) fail(name, "EPC no apunta a la instruccion vigilada");
    if(o.badVAddr != 0) fail(name, "Watch no debe escribir BadVAddr");
    // Precisa: la excepcion se toma ANTES de completar el acceso.
    if(isStore && o.memWord != PRESET) fail(name, "el store se completo pese a la excepcion");
    if(!isStore && o.rt != POISON)     fail(name, "el load se completo pese a la excepcion");
  } else {
    if(o.trapped) { fail(name, "excepcion inesperada"); return; }
    if(isStore && o.memWord == PRESET) fail(name, "el store no llego a memoria");
    if(!isStore && o.rt == POISON)     fail(name, "el load no llego al registro");
  }
}

int main() {
  std::printf("watch: excepcion Watch de COP0 (ExcCode 23), el interprete es el oraculo\n\n");

  auto mp = std::make_unique<Memory>();
  Memory& mem = *mp;
  mem.reset(/*expansionPak=*/true);

  std::printf("Direccion vigilada = fisica %08x, R y W por separado:\n", DATA_PHYS);
  check(mem, "sw con W armado", prog(watchLo(DATA_PHYS, false, true), 0, OP_SW, 0), true, true);
  check(mem, "lw con R armado", prog(watchLo(DATA_PHYS, true, false), 0, OP_LW, 0), true, false);
  check(mem, "sw con solo R armado (no vigila stores)",
        prog(watchLo(DATA_PHYS, true, false), 0, OP_SW, 0), false, true);
  check(mem, "lw con solo W armado (no vigila loads)",
        prog(watchLo(DATA_PHYS, false, true), 0, OP_LW, 0), false, false);
  check(mem, "sw con R y W armados", prog(watchLo(DATA_PHYS, true, true), 0, OP_SW, 0), true, true);
  check(mem, "sw con nada armado", prog(0, 0, OP_SW, 0), false, true);

  std::printf("\nGranularidad: la comparacion es por DOBLEPALABRA, bits [31:3]:\n");
  check(mem, "sb al byte +3 del mismo doblepalabra",
        prog(watchLo(DATA_PHYS, false, true), 0, OP_SB, 3), true, true);
  // +4 sigue dentro del mismo doblepalabra alineado a 8: tambien dispara.
  check(mem, "sw a +4 (mismo doblepalabra)",
        prog(watchLo(DATA_PHYS, false, true), 0, OP_SW, 4), true, true);
  // El doblepalabra siguiente ya no coincide; el store debe llegar... a otra direccion,
  // asi que se comprueba solo que NO hay excepcion.
  {
    auto c = prog(watchLo(DATA_PHYS + 8, false, true), 0, OP_SW, 0);
    Outcome o = run(mem, c, false);
    std::printf("  %-46s trap=%s exc=%2u\n", "sw con el vigia en otro doblepalabra",
                o.trapped ? "si" : "no", excCode(o));
    if(o.trapped) fail("sw con el vigia en otro doblepalabra", "excepcion sin coincidencia de direccion");
  }

  // El store cacheado se queda en la D-cache, asi que aqui solo se comprueba que dispare:
  // la fisica es la misma y el vigia compara fisicas, haya cache o no.
  {
    auto c = prog(watchLo(DATA_PHYS, false, true), 0, OP_SW, 0, DATA_VA_C);
    Outcome o = run(mem, c, false);
    std::printf("  %-46s trap=%s exc=%2u epc=%08llx\n", "sw cacheado (ckseg0, misma fisica)",
                o.trapped ? "si" : "no", excCode(o), (unsigned long long)o.epc);
    if(!o.trapped || excCode(o) != 23) fail("sw cacheado", "el camino cacheado se salta el vigia");
  }

  std::printf("\nWatchHi: PAddr1 son los bits [35:32] de la fisica, imposibles en N64:\n");
  check(mem, "sw con WatchHi=1 (fisica > 4 GB, nunca coincide)",
        prog(watchLo(DATA_PHYS, false, true), 1, OP_SW, 0), false, true);

  std::printf("\nDiferida con EXL o ERL: la excepcion NO se toma (manual 6.4.9):\n");
  // Status.EXL = bit 1, Status.ERL = bit 2, puestos antes del primer paso: escribirlos
  // con MTC0 desde el propio programa borraria CU0 y ensuciaria el caso.
  check(mem, "sw con EXL puesto", prog(watchLo(DATA_PHYS, false, true), 0, OP_SW, 0),
        false, true, 0x2u);
  check(mem, "lw con ERL puesto", prog(watchLo(DATA_PHYS, true, false), 0, OP_LW, 0),
        false, false, 0x4u);

  std::printf("\nRegistros: WatchLo no tiene bit 2 y WatchHi solo tiene [3:0]:\n");
  {
    // mtc0 de todo unos: WatchLo debe leer 0xFFFFFFFB y WatchHi 0xF.
    std::vector<u32> c;
    emitMtc0(c, 19, 0xFFFF'FFFFu);
    emitMtc0(c, 18, 0xFFFF'FFFFu);
    c.push_back(NOP);
    auto cp = std::make_unique<CPU>();
    CPU& cpu = *cp;
    cpu.connect(&mem);
    cpu.reset();
    cpu.pc = PROG_VA; cpu.nextPc = PROG_VA + 4;
    u32 phys = (u32)PROG_VA & 0x1FFF'FFFF;
    for(usize i = 0; i < c.size(); i++) {
      u32 w = c[i];
      mem.rdram[phys + 4*i + 0] = (u8)(w >> 24); mem.rdram[phys + 4*i + 1] = (u8)(w >> 16);
      mem.rdram[phys + 4*i + 2] = (u8)(w >> 8);  mem.rdram[phys + 4*i + 3] = (u8)w;
    }
    for(usize i = 0; i < c.size(); i++) cpu.step();
    u32 lo = (u32)cpu.cop0[18], hi = (u32)cpu.cop0[19];
    std::printf("  %-46s WatchLo=%08x WatchHi=%08x armado=%d\n",
                "mtc0 de todo unos", lo, hi, (int)cpu.watchArmed);
    if(lo != 0xFFFF'FFFBu) fail("mtc0 de todo unos", "WatchLo deberia leer 0xFFFFFFFB (bit 2 no existe)");
    if(hi != 0xFu)         fail("mtc0 de todo unos", "WatchHi deberia leer 0xF (solo PAddr1)");
    if(!cpu.watchArmed)    fail("mtc0 de todo unos", "R o W puestos y el driver no se entera");
  }

  std::printf("\nDynarec: sin vigilancia armada los dos motores deben coincidir:\n");
  {
    auto c = prog(0, 0, OP_SW, 0);
    Outcome i = run(mem, c, /*useJit=*/false);
    Outcome j = run(mem, c, /*useJit=*/true);
    std::printf("  %-46s int mem=%08x | jit %s mem=%08x\n", "sw sin vigilancia",
                i.memWord, j.jitRan ? "corrio" : "declino", j.memWord);
    if(i.memWord != j.memWord || i.trapped != j.trapped)
      fail("sw sin vigilancia", "interprete y dynarec no coinciden");
  }
  {
    // Con la vigilancia armada el driver manda al interprete; run() reproduce esa regla,
    // asi que el resultado debe ser identico al del interprete puro (la excepcion se toma).
    auto c = prog(watchLo(DATA_PHYS, false, true), 0, OP_SW, 0);
    Outcome j = run(mem, c, /*useJit=*/true);
    std::printf("  %-46s trap=%s exc=%2u jit=%s\n", "sw vigilado con el jit disponible",
                j.trapped ? "si" : "no", excCode(j), j.jitRan ? "corrio" : "declino");
    if(!j.trapped || excCode(j) != 23)
      fail("sw vigilado con el jit disponible", "el dynarec se salto la excepcion Watch");
  }

  std::printf("\n%s (%d fallos)\n", failures ? "*** HAY FALLOS ***" : "ALL PASS", failures);
  return failures ? 1 : 0;
}
