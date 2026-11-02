#pragma once
// Internals shared by the translation units of the VR4300 interpreter. Not an interface:
// only src/cpu/cpu*.cpp include this.
//
// Layout of the CPU:
//   cpu.cpp         env knobs, reset, memory access, caches, step, exceptions, TLB,
//                   execute/special/regimm/COP0/COP2, memory helpers for the dynarec
//   cpu_boot.cpp    CIC detection, fastBoot (PIF-less boot) and warmBoot (NMI)
//   cpu_fpu.cpp     COP1 interpreter and the COP1 helpers the dynarec calls
//   cpu_disasm.cpp  disassembler
#include "cpu.hpp"

namespace kestrel {

// Sign/zero helpers.
inline auto sext32(u32 v) -> u64 { return (u64)(s64)(s32)v; }
inline auto sext16(u16 v) -> u64 { return (u64)(s64)(s16)v; }
inline auto sext8 (u8  v) -> u64 { return (u64)(s64)(s8)v; }

// Instruction field extractors.
#define OP    (op >> 26 & 0x3f)
#define RS    (op >> 21 & 0x1f)
#define RT    (op >> 16 & 0x1f)
#define RD    (op >> 11 & 0x1f)
#define SA    (op >>  6 & 0x1f)
#define FUNCT (op & 0x3f)
#define IMM16 (op & 0xffff)
#define SIMM  sext16(op & 0xffff)
#define TARGET26 (op & 0x03ff'ffff)

// KESTREL_DCWT: D-cache in write-through mode (cpu.cpp, read at boot by cpu_boot.cpp).
extern const bool g_dcWriteThrough;
// KESTREL_SEENADDR: physical word whose transitions are logged; 0 = off (set at boot).
extern u32 g_seenAddr;

// Branch commit (interpreter branches and COP1 BC1x).
inline auto CPU::branch(bool taken, u64 target) -> void {
  justBranched = true;   // a delay slot follows whether or not the branch is taken
  if(taken) nextPc = target;
}

// FPR access by Status.FR (used by the interpreter loads/stores and by COP1).
inline auto CPU::fprGet64(u32 i) -> u64 {
  if((u32)cop0[C0_Status] & (1u<<26)) return fpr[i];   // FR=1
  return fpr[i & ~1u];                                 // FR=0: even reg holds the pair
}
inline auto CPU::fprSet64(u32 i, u64 v) -> void {
  if((u32)cop0[C0_Status] & (1u<<26)) { fpr[i] = v; return; }
  fpr[i & ~1u] = v;
}
inline auto CPU::fprGet32(u32 i) -> u32 {
  if((u32)cop0[C0_Status] & (1u<<26)) return (u32)fpr[i];  // FR=1: low 32
  if(i & 1) return (u32)(fpr[i & ~1u] >> 32);              // FR=0 odd: high half of partner
  return (u32)fpr[i];                                      // FR=0 even: low half
}
inline auto CPU::fprSet32(u32 i, u32 v) -> void {
  if((u32)cop0[C0_Status] & (1u<<26)) { fpr[i] = (fpr[i] & 0xffff'ffff'0000'0000ull) | v; return; }
  if(i & 1) { u32 e=i&~1u; fpr[e] = (fpr[e] & 0x0000'0000'ffff'ffffull) | ((u64)v << 32); return; }
  fpr[i] = (fpr[i] & 0xffff'ffff'0000'0000ull) | v;
}

}  // namespace kestrel
