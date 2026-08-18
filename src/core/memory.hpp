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
  u32 mi_mode = 0, mi_mask = 0;
  std::atomic<u32> mi_intr{0};
  // RDRAM "init/repeat" mode (MI_MODE bit8 arms it, bit7 clears). While armed, the
  // next CPU store to RDRAM is broadcast/repeated across a byte span. Used by the
  // boot code to blank RDRAM; exposed as an observable quirk by n64-systemtest.
  bool mi_repeat_on = false;
  u32  mi_repeat_len = 0;   // repeat span in bytes (init_length + 1)
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
  u32 dpc_start = 0, dpc_end = 0, dpc_current = 0;
  std::atomic<u32> dpc_status{0};
  // DPC performance counters (24-bit, free-running). The RDP worker accumulates
  // them while rasterizing and the CPU reads/clears them, so they are atomic.
  // DPC_STATUS write bits 6..9 clear TMEM/PIPE/BUF/CLOCK respectively.
  std::atomic<u32> dpc_clock{0}, dpc_bufbusy{0}, dpc_pipebusy{0}, dpc_tmem{0};
  // VI
  u32 vi_ctrl = 0, vi_origin = 0, vi_width = 0, vi_intr = 256, vi_current = 0;
  u32 viFlips = 0;    // VI_ORIGIN changed to a different address = displayed buffer swapped
  u32 dpSyncs = 0;    // RDP SYNC_FULL count = display lists completed
  u32 vi_burst = 0, vi_vsync = 0, vi_hsync = 0, vi_leap = 0, vi_hstart = 0;
  u32 vi_vstart = 0, vi_vburst = 0, vi_xscale = 0, vi_yscale = 0;
  // AI — models a 2-deep DMA buffer FIFO so the audio driver blocks (STATUS
  // FIFO_FULL) instead of generating frames forever, and gets MI_AI when a
  // buffer drains. Without this the audio thread spins in the frame builder and
  // starves the gfx thread (no video).
  u32 ai_dram = 0, ai_len = 0, ai_ctrl = 0, ai_status = 0, ai_dacrate = 0, ai_bitrate = 0;
  u32 ai_fifo_addr[2] = {}, ai_fifo_len[2] = {};
  u32 ai_fifo_count = 0;     // buffers queued (0..2)
  u32 ai_play_remaining = 0; // bytes left in the currently playing buffer
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

struct Memory {
  static constexpr u32 RDRAM_SIZE_EXPANDED = 0x0080'0000;  // 8 MB (Expansion Pak)
  static constexpr u32 DMEM_SIZE = 0x1000;                 // 4 KB
  static constexpr u32 IMEM_SIZE = 0x1000;                 // 4 KB
  static constexpr u32 PIFRAM_SIZE = 0x40;                 // 64 B

  std::vector<u8> rdram;
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
  Rsp rsp;                 // low-level RSP interpreter (M3.3), runs microcode

  // --- RCP threading (multihilo rebuild) -------------------------------------
  // Lockstep (default): the RDP rasterizes synchronously inside the DPC_END store,
  // exactly as before — the deterministic path systemtest validates at 0/3721.
  // Threaded (KESTREL_THREADS=1): DPC_END only enqueues a job; a worker thread
  // rasterizes async and raises MI_DP on SYNC_FULL, matching real hardware where
  // the RDP chews the FIFO while the CPU keeps running. Same *results*, async timing.
  enum class RcpMode { Lockstep, Threaded };
  RcpMode rcpMode = RcpMode::Lockstep;

  struct RdpJob { u32 current, end; bool xbus; };
  std::deque<RdpJob>      rdpQueue;
  std::mutex              rdpMx;
  std::condition_variable rdpCv;
  std::thread             rdpWorker;
  bool                    rdpStop = false;      // worker exit flag (guarded by rdpMx)
  std::atomic<bool>       rdpBusy{false};        // true while a job is queued or running

  // RSP worker: in Threaded mode a CPU SP-release runs the whole microcode task to
  // BREAK on this thread (it raises MI_SP itself), instead of the CPU interleaving
  // rsp.step(). The producer/consumer contract is the same as the RDP: the game
  // waits on the SP interrupt before reading the RSP's output, so byte-level
  // coherency between threads isn't needed — the interrupt is the release barrier.
  std::mutex              rspMx;
  std::condition_variable rspCv;
  std::thread             rspWorker;
  bool                    rspStop = false;      // worker exit flag (guarded by rspMx)
  bool                    rspKick = false;      // pending run request (guarded by rspMx)
  std::atomic<bool>       rspBusy{false};        // true from kick until the task breaks

  auto startRcpThreads() -> void;   // spawn workers if rcpMode==Threaded (idempotent)
  auto stopRcpThreads()  -> void;   // join workers on shutdown
  auto rdpSubmit(u32 current, u32 end, bool xbus) -> void;  // enqueue (threaded)
  auto rdpDrain() -> void;          // block until the RDP queue is fully consumed
  auto rdpRunJob(u32 current, u32 end, bool xbus) -> void;  // rasterize + DP bookkeeping
  auto rspAwaitIdle() -> void;      // block until the RSP worker has published its task result
  auto rspSubmitKick() -> void;     // wake the RSP worker to run the armed task (threaded)
private:
  auto rdpWorkerLoop() -> void;
  auto rspWorkerLoop() -> void;
public:

  // --- debug store watchpoint ------------------------------------------------
  u32  watchAddr = 0;      // physical byte address to watch (0 = disabled)
  u32  watchLen  = 4;      // watched window length in bytes
  u64  storePc   = 0;      // CPU sets this to its curPc before each store
  auto watchHit(u32 physAddr, u32 nbytes, u64 value, bool dma) -> void;

  // Player-1 controller state, published by the video/input layer (or KESTREL_BUTTONS
  // when headless) and read back by the joybus (0x01 read-buttons command). Button
  // word is byte0<<8|byte1 (A=0x8000 … C-Right=0x0001, START=0x1000). Stick is the
  // signed ±80 analog range the N64 pad reports in bytes 2/3.
  u32  padButtons = 0;
  s8   padStickX = 0, padStickY = 0;

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
  static constexpr u64 CART_LATCH_TTL = 200;
  auto isCart(u32 phys) const -> bool { return !rom.empty() && phys >= 0x1000'0000 && phys < 0x1fc0'0000; }
  auto cartRom32(u32 phys) -> u32;               // aligned 32-bit ROM word (0 if past image)
  auto cartRead(u32 phys, u32 nbytes) -> u32;    // CPU read from cart space (latch + 16-bit mux)
  auto cartWrite(u32 phys, u64 value, u32 nbytes) -> void;  // CPU write to cart space (latch)

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

  // --- interrupt aggregation (MI) --------------------------------------------
  auto raiseIntr(u32 bit) -> void {
    static int irqt = std::getenv("KESTREL_IRQTRACE") ? 1 : 0;
    if(irqt) { const char* nm = bit==MI_SP?"SP":bit==MI_SI?"SI":bit==MI_AI?"AI":bit==MI_VI?"VI":bit==MI_PI?"PI":bit==MI_DP?"DP":"?";
      static u32 cnt[6]={}; int idx = bit==MI_SP?0:bit==MI_SI?1:bit==MI_AI?2:bit==MI_VI?3:bit==MI_PI?4:5;
      if(cnt[idx]++ < 12) std::fprintf(stderr,"[irq] %s #%u mask=%02x intr=%02x\n",nm,cnt[idx],rcp.mi_mask,rcp.mi_intr|bit); }
    rcp.mi_intr |= bit;
    if((bit & MI_SP) && spTrace()) { static u32 n=0; if(n++<40) std::fprintf(stderr,"[mi] raise SP #%u mask=%02x intr=%02x\n",n,rcp.mi_mask,rcp.mi_intr.load()); }
    if((bit & MI_DP) && spTrace()) { static u32 n=0; if(n++<40) std::fprintf(stderr,"[mi] raise DP #%u mask=%02x intr=%02x\n",n,rcp.mi_mask,rcp.mi_intr.load()); }
  }
  auto clearIntr(u32 bit) -> void {
    if((bit & MI_SP) && spTrace()) { static u32 n=0; if(n++<40) std::fprintf(stderr,"[mi] clear SP #%u\n",n); }
    if((bit & MI_DP) && spTrace()) { static u32 n=0; if(n++<40) std::fprintf(stderr,"[mi] clear DP #%u\n",n); }
    rcp.mi_intr &= ~bit;
  }
  static auto spTrace() -> bool { static int t = std::getenv("KESTREL_RSPTRACE")?1:0; return t; }
  auto interruptPending() const -> bool { return (rcp.mi_intr & rcp.mi_mask) != 0; }

  // --- VI field tick (drives VI_CURRENT + VI interrupt), called by the run loop --
  auto viTick() -> void;
  // --- AI drain tick (paces audio DMA FIFO), called once per field from viTick --
  auto aiTick() -> void;

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
  auto siDma(bool toPif) -> void;    // SI: PIF RAM <-> RDRAM (64 bytes)
  auto pifProcessJoybus() -> void;   // parse PIF RAM command block, fill controller/EEPROM responses
};

}  // namespace kestrel
