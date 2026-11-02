#pragma once
// Internals shared by the translation units that implement Memory (memory*.cpp, rcp_*.cpp).
// Not an interface: nothing outside src/core/ includes this.
//
// Layout of the Memory implementation:
//   memory.cpp       reset, address map, ROM load, CPU-side reads/writes, PI bus
//   memory_cart.cpp  save types, Controller/Transfer Pak, SummerCart64 + SD
//   memory_mmio.cpp  RCP MMIO register file, VI/AI field ticks
//   memory_dma.cpp   PI/SP/SI DMA, DP FIFO kick
//   memory_pif.cpp   PIF boot, CIC challenge, RTC, joybus
//   rcp_rdp.cpp      RDP job execution and RDP worker thread
//   rcp_sync.cpp     CPU/RSP/RDP pacing, guest-time barriers, rendezvous, RSP worker
#include "types.hpp"
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <vector>
#include <xmmintrin.h>

// Which chip is running right now, so each event is stamped with that chip's own guest clock.
// Thread-local and constant-initialised (constinit lets other TUs skip the TLS init wrapper).
extern constinit thread_local bool tlIsRspThread;
extern constinit thread_local bool tlDpLogApply;
extern constinit thread_local bool tlDpcKick;
extern constinit thread_local kestrel::u64 tlDpLogAt;
extern constinit thread_local bool tlInRetire, tlRetireArmed;

namespace kestrel {

// Physical base addresses of the N64 memory map.
enum : u32 {
  BASE_RDRAM  = 0x0000'0000,
  BASE_DMEM   = 0x0400'0000,
  BASE_IMEM   = 0x0400'1000,
  BASE_CART   = 0x1000'0000,  // PI domain 1 (cartridge ROM)
  BASE_PIFRAM = 0x1fc0'07c0,
  // RCP MMIO register blocks.
  BASE_RDRAMREG = 0x03f0'0000,
  BASE_SP     = 0x0404'0000,
  BASE_SP_PC  = 0x0408'0000,
  BASE_DPC    = 0x0410'0000,
  BASE_DPS    = 0x0420'0000,
  BASE_MI     = 0x0430'0000,
  BASE_VI     = 0x0440'0000,
  BASE_AI     = 0x0450'0000,
  BASE_PI     = 0x0460'0000,
  BASE_RI     = 0x0470'0000,
  BASE_SI     = 0x0480'0000,
};

// SummerCart64 windows in PI space: register file, 8 KiB transfer buffer, SDRAM size.
inline constexpr u32 kScReg = 0x1fff'0000, kScBuf = 0x1ffe'0000, kScBufSz = 0x2000;
inline constexpr u32 kScSdram = 64u << 20;

// Host thread helpers (memory.cpp).
auto selfThreadHandle() -> void*;
auto pinToPhysicalCore(u32 slot) -> void;

// Controller Pak filesystem helpers (memory_cart.cpp), also used by the joybus.
auto mempakAddrCrc(u16 blockAddr) -> u8;
auto mempakDataCrc(const u8* data) -> u8;
auto mempakFormat(std::vector<u8>& p) -> void;

// Diagnostico KESTREL_RDPINLINE: el RDP corre en el hilo de CPU aun en modo threaded. Decide
// tambien que hilo es el dueno de Granite, y por tanto quien puede llamar a vrdp::idle.
inline auto rdpInlineDiag() -> bool {
  static const bool on = std::getenv("KESTREL_RDPINLINE") != nullptr;
  return on;
}

// Host spin-wait tuning (rcp_rdp.cpp / rcp_sync.cpp). None of it changes guest time.
extern const u32 kSpinPauseMask;
extern const bool g_spinPauseOn;
inline void spinPause() { if(g_spinPauseOn) _mm_pause(); }
auto rdpSpinLen() -> u32;

// Bounded wait that reports instead of giving up: every rcpWaitReportMs() it calls
// report(round) and keeps waiting (rcp_rdp.cpp).
auto rcpWaitReportMs() -> unsigned;
template<typename Pred, typename Report>
auto awaitReporting(std::unique_lock<std::mutex>& lk, std::condition_variable& cv,
                    Pred pred, Report report) -> void {
  const unsigned ms = rcpWaitReportMs();
  if(!ms) { cv.wait(lk, pred); return; }
  const auto period = std::chrono::milliseconds(ms);
  for(unsigned round = 1; !cv.wait_for(lk, period, pred); round++) report(round);
}

}  // namespace kestrel
