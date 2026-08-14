// kestrel64 — paraLLEl-RDP backend glue (implementation). See vrdp.hpp.
//
// Modeled on ares' n64/vulkan/vulkan.cpp, which is the canonical CommandProcessor driver.
//
// THREADING MODEL (critical): Granite / ::RDP::CommandProcessor is single-thread only
// (we build parallel-rdp WITHOUT GRANITE_VULKAN_MT). Every Granite call must run on ONE
// thread. In kestrel the natural owner is the RDP worker thread (it calls init + runFifo).
// The other producers run on different threads:
//   - viWrite()   ← CPU thread (VI register store)
//   - frameBegin()← CPU/viTick thread (field boundary)
//   - scanout()   ← main/present thread (wants host pixels)
// So those NEVER touch Granite directly: they only stash state in atomics/buffers. The RDP
// worker applies the pending VI regs, rotates the frame context, and produces the scanout
// RGBA into a mutex-guarded buffer inside runFifo (right after SYNC_FULL). present just copies
// that buffer out. This keeps Granite on a single registered thread — no cross-thread races.

#include "vrdp.hpp"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include "rdp_device.hpp"     // ::RDP::CommandProcessor  (parallel-rdp)
#include "context.hpp"        // ::Vulkan::Context
#include "device.hpp"         // ::Vulkan::Device
#include "logging.hpp"        // Util::LoggingInterface
#include "thread_id.hpp"      // Util::register_thread_index

namespace kestrel::vrdp {

namespace {

// RDP command length (in 64-bit words) indexed by op = word0>>24 & 63. Same table ares uses.
constexpr u32 kCmdLen[64] = {
  1, 1, 1, 1, 1, 1, 1, 1, 4, 6,12,14,12,14,20,22,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 2, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
  1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
};

struct SilentLog : Util::LoggingInterface {
  auto log(const char*, const char*, va_list) -> bool override { return true; }
} gLog;

struct Backend {
  ::Vulkan::Context context;
  ::Vulkan::Device  device;
  ::RDP::CommandProcessor* proc = nullptr;
  bool ok = false;

  // VI register shadow written by the CPU thread, applied on the RDP thread.
  std::atomic<u32> viReg[14] = {};
  std::atomic<u32> viDirty{0};              // bitmask of regs pending apply
  std::atomic<bool> frameReq{false};        // begin_frame_context() pending

  // Latest scanned-out picture, produced on the RDP thread, read by present.
  std::mutex        frameMutex;
  std::vector<u8>   frameRGBA;              // width*height*4, RGBA8888
  u32               frameW = 0, frameH = 0;
  bool              haveFrame = false;
};

Backend* g = nullptr;

// Read a big-endian 32-bit word from a host byte buffer at physical offset `off`.
inline auto rd32(const u8* p, u32 off) -> u32 {
  return (u32)p[off] << 24 | (u32)p[off + 1] << 16 | (u32)p[off + 2] << 8 | p[off + 3];
}

// Apply any pending VI register writes + frame-context rotation. RDP thread only.
auto pumpViState() -> void {
  u32 dirty = g->viDirty.exchange(0, std::memory_order_acquire);
  for(u32 i = 0; i < 14; i++)
    if(dirty & (1u << i))
      g->proc->set_vi_register(::RDP::VIRegister(i), g->viReg[i].load(std::memory_order_relaxed));
  if(g->frameReq.exchange(false, std::memory_order_acq_rel))
    g->proc->begin_frame_context();
}

// Produce the current scanout into frameRGBA. RDP thread only (Granite call).
auto produceScanout() -> void {
  ::RDP::VIScanoutBuffer sb;
  ::RDP::ScanoutOptions opts;
  opts.persist_frame_on_invalid_input = true;
  g->proc->scanout_async_buffer(sb, opts);
  if(!sb.fence || !sb.width || !sb.height) return;
  sb.fence->wait();
  const u8* rgba = (const u8*)g->device.map_host_buffer(*sb.buffer,
                                                        ::Vulkan::MEMORY_ACCESS_READ_BIT);
  if(rgba) {
    std::lock_guard<std::mutex> lk(g->frameMutex);
    g->frameW = sb.width; g->frameH = sb.height;
    g->frameRGBA.resize((usize)sb.width * sb.height * 4);
    std::memcpy(g->frameRGBA.data(), rgba, g->frameRGBA.size());
    g->haveFrame = true;
  }
  g->device.unmap_host_buffer(*sb.buffer, ::Vulkan::MEMORY_ACCESS_READ_BIT);
}

}  // namespace

auto init(u8* rdram, u32 size) -> bool {
  if(g) return g->ok;
  const char* e = std::getenv("KESTREL_PRDP");
  if(!e || e[0] == '0') return false;               // opt-in only

  // This thread (the RDP worker) is the sole Granite driver — register it as index 0 so
  // Granite's per-thread lookups resolve instead of spamming "thread does not exist".
  Util::register_thread_index(0);

  Util::set_thread_logging_interface(&gLog);
  if(!::Vulkan::Context::init_loader(nullptr)) return false;

  g = new Backend();
  if(!g->context.init_instance_and_device(nullptr, 0, nullptr, 0, 0)) { shutdown(); return false; }
  g->device.set_context(g->context);
  g->device.init_frame_contexts(3);

  // hidden RDRAM (coverage/AA aux bits) is sized at rdram/2 on hardware.
  g->proc = new ::RDP::CommandProcessor(g->device, rdram, 0, size, size / 2, 0);
  if(!g->proc->device_is_supported()) { shutdown(); return false; }

  g->ok = true;
  return true;
}

auto shutdown() -> void {
  if(!g) return;
  if(g->proc) { delete g->proc; g->proc = nullptr; }
  delete g;
  g = nullptr;
}

auto active() -> bool { return g && g->ok; }

auto runFifo(const u8* rdram, u32 rdramSize, const u8* dmem, u32 start, u32 end, bool xbus)
    -> bool {
  if(!active()) return false;
  pumpViState();                                    // apply CPU-side VI writes / frame rotate

  bool sawSyncFull = false;
  u32 cur = start & ~7u, fin = end & ~7u;
  // Enqueue command-by-command, splitting on the length table (like ares render()).
  while(cur + 8 <= fin) {
    u32 hi;
    if(xbus && dmem) hi = rd32(dmem, cur & 0xfff);
    else { if(cur + 8 > rdramSize) break; hi = rd32(rdram, cur); }
    u32 op  = (hi >> 24) & 63;
    u32 len = kCmdLen[op];                          // in 64-bit words
    if(len > 32) len = 32;                          // clamp defensively

    // Gather the whole command into a swapped u32 pair buffer for enqueue_command.
    static thread_local u32 words[64];
    for(u32 i = 0; i < len; i++) {
      u32 a = cur + i * 8;
      if(xbus && dmem) {
        words[i * 2 + 0] = rd32(dmem, a & 0xfff);
        words[i * 2 + 1] = rd32(dmem, (a + 4) & 0xfff);
      } else {
        words[i * 2 + 0] = (a + 8 <= rdramSize) ? rd32(rdram, a) : 0;
        words[i * 2 + 1] = (a + 8 <= rdramSize) ? rd32(rdram, a + 4) : 0;
      }
    }

    if(op >= 8) g->proc->enqueue_command(len * 2, words);

    if(::RDP::Op(op) == ::RDP::Op::SyncFull) {
      g->proc->wait_for_timeline(g->proc->signal_timeline());
      produceScanout();                             // grab the finished frame's pixels
      sawSyncFull = true;
    }
    cur += len * 8;
  }
  return sawSyncFull;
}

auto viWrite(u32 index, u32 value) -> void {
  if(!g || index >= 14) return;                     // pure state stash; any thread
  g->viReg[index].store(value, std::memory_order_relaxed);
  g->viDirty.fetch_or(1u << index, std::memory_order_release);
}

auto frameBegin() -> void {
  if(g) g->frameReq.store(true, std::memory_order_release);
}

auto scanout(u32& width, u32& height) -> const u8* {
  width = height = 0;
  if(!active()) return nullptr;
  // Hold the frame mutex across scanout()/scanoutDone() so the RDP thread can't overwrite
  // the buffer mid-copy. present ALWAYS calls scanoutDone() afterwards, so the lock is taken
  // unconditionally here (even when no frame is ready) and released there — symmetric, no
  // double-unlock. Single caller (present/main thread).
  g->frameMutex.lock();
  if(!g->haveFrame || g->frameRGBA.empty()) return nullptr;   // still locked; scanoutDone unlocks
  width  = g->frameW;
  height = g->frameH;
  return g->frameRGBA.data();
}

auto scanoutDone() -> void {
  if(g) g->frameMutex.unlock();
}

}  // namespace kestrel::vrdp
