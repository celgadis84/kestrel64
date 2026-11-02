#pragma once
// Internals shared by the translation units of the CPU JIT. Not an interface: only
// src/cpu/jit*.cpp include this.
//
// Layout of the JIT:
//   jit.cpp          code buffer, code cache, block linking, emitter self-tests
//   jit_compile.cpp  guest register residency, instruction emitters, compileBlock
//   jit_run.cpp      CPU-side dispatch (jitTryBlock), chain re-entry, idle skip, stats
#include "jit.hpp"

namespace kestrel { struct CPU; }

namespace kestrel::jit {

// KESTREL_JIT_STATS: emit inline exit counters into compiled blocks (jit.cpp).
extern const int g_xStats;
extern u32 g_xLink, g_xItc, g_xSlowDir, g_xSlowInd;   // terminator exit counters
extern u64 g_retBranch, g_retFull, g_retShort;        // how blocks return to the driver

// Diagnostics read by the dispatcher (jit_compile.cpp).
extern const bool g_diffGo, g_pcChk, g_regChk;
extern u32 g_diffBad;
auto regChkBad(u64 v) -> bool;
extern const int g_compFailOn;
extern const int g_jitTlbLink;

// RCP deadlines another thread can arm while the CPU runs a linked chain: bit0 SP task end,
// bit1 RDP end, bit4 DPC write logged in the journal. The other two rcpPend bits (barriers,
// the CPU's own DPC mailbox) are written by the CPU thread and cannot change mid-chain.
inline constexpr u32 kPendWatch = 1u | 2u | 16u;

// Compile the block that starts at physical address `phys` (jit_compile.cpp).
auto compileBlock(CPU& c, u32 phys) -> Block;

}  // namespace kestrel::jit
