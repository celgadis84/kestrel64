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
#include <chrono>
#include <cstdint>
#include <cstdio>
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
  // Por defecto se traga TODO: parallel-rdp escupe mucho ruido informativo. Con
  // KESTREL_VRDPLOG deja pasar sus mensajes, que es la unica forma de ver un
  // LOGE("Unimplemented!") suyo cuando un pase entero desaparece.
  auto log(const char* tag, const char* fmt, va_list va) -> bool override {
    static const bool on = std::getenv("KESTREL_VRDPLOG") != nullptr;
    if(!on) return true;
    std::fprintf(stderr, "[prdp %s] ", tag ? tag : "?");
    std::vfprintf(stderr, fmt, va);
    std::fflush(stderr);
    return true;
  }
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

  // Scanout en vuelo: se pide al empezar el campo y se recoge al empezar el siguiente,
  // asi la GPU trabaja mientras el emulador sigue. Esperar el fence justo despues de
  // pedirlo (lo que haciamos antes) serializa CPU y GPU dos veces por campo.
  ::RDP::VIScanoutBuffer pending;
  bool              havePending = false;

  // Latest scanned-out picture, produced on the RDP thread, read by present.
  std::mutex        frameMutex;
  std::vector<u8>   frameRGBA;              // width*height*4, RGBA8888
  u32               frameW = 0, frameH = 0;
  bool              haveFrame = false;

  // Reparto de tiempo del hilo RDP (KESTREL_PRDP_STATS=1). Sin esto no se sabe si el coste
  // esta en trocear la FIFO, en esperar a la GPU o en bajarse los pixeles por PCIe.
  bool  stats = false;
  u64   nsEnq = 0, nsWait = 0, nsScan = 0, nEnq = 0, nSync = 0, nSkip = 0;

  // Comandos encolados desde el ultimo SYNC_FULL que pueden ESCRIBIR en RDRAM. Un
  // SYNC_FULL solo obliga a esperar a la GPU si hay pixeles nuevos que el CPU podria
  // leer; si el display list solo toco estado (Set_*, Load_*, Sync_*) no hay nada que
  // sincronizar y el fence de la GPU cuesta ~1.6 ms de reloj de host por nada.
  u32   drawsSinceSync = 0;
};

inline auto nowNs() -> u64 {
  return (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch()).count();
}

Backend* g = nullptr;

// Read a big-endian 32-bit word from a host byte buffer at physical offset `off`.
inline auto rd32(const u8* p, u32 off) -> u32 {
  return (u32)p[off] << 24 | (u32)p[off + 1] << 16 | (u32)p[off + 2] << 8 | p[off + 3];
}

auto harvestScanout() -> void;
auto issueScanout() -> void;

// Apply any pending VI register writes + frame-context rotation. RDP thread only.
auto pumpViState() -> void {
  u32 dirty = g->viDirty.exchange(0, std::memory_order_acquire);
  for(u32 i = 0; i < 14; i++)
    if(dirty & (1u << i))
      g->proc->set_vi_register(::RDP::VIRegister(i), g->viReg[i].load(std::memory_order_relaxed));
  if(g->frameReq.exchange(false, std::memory_order_acq_rel)) {
    // Frontera de campo: el VI escupe lo que haya en el framebuffer, pase lo que pase con
    // el DP. Recoger el campo anterior (ya terminado en la GPU) y pedir el siguiente.
    harvestScanout();
    g->proc->begin_frame_context();
    issueScanout();
  }
}

// Pedir el scanout del campo actual. No bloquea: deja el fence en vuelo. RDP thread only.
auto issueScanout() -> void {
  ::RDP::ScanoutOptions opts;
  opts.persist_frame_on_invalid_input = true;
  g->proc->scanout_async_buffer(g->pending, opts);
  g->havePending = g->pending.fence && g->pending.width && g->pending.height;
}

// Recoger el scanout pedido en el campo anterior. RDP thread only (Granite call).
auto harvestScanout() -> void {
  if(!g->havePending) return;
  g->havePending = false;
  u64 t0 = g->stats ? nowNs() : 0;
  ::RDP::VIScanoutBuffer& sb = g->pending;
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
  if(g->stats) g->nsScan += nowNs() - t0;
}

}  // namespace

auto dumpStats() -> void;

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

  // parallel-rdp maps guest RDRAM straight into the GPU via VK_EXT_external_memory_host
  // (zero copy). The import demands a page-aligned host pointer; if it fails, parallel-rdp
  // silently drops to a device-side mirror that must be re-uploaded around every sync.
  // Say out loud which path we got — the two differ by an 8 MB PCIe transfer per frame.
  {
    const auto& feat = g->device.get_device_features();
    usize align = feat.supports_external_memory_host
                ? (usize)feat.host_memory_properties.minImportedHostPointerAlignment : 0;
    std::fprintf(stderr, "[vrdp] gpu=\"%s\" ext_mem_host=%d align=%zu rdram=%p%s\n",
                 g->device.get_gpu_properties().deviceName,
                 (int)feat.supports_external_memory_host, align, (void*)rdram,
                 (align && ((uintptr_t)rdram & (align - 1))) ? "  MISALIGNED -> slow copy path" : "");
  }

  // hidden RDRAM (coverage/AA aux bits) is sized at rdram/2 on hardware.
  g->proc = new ::RDP::CommandProcessor(g->device, rdram, 0, size, size / 2, 0);
  if(!g->proc->device_is_supported()) { shutdown(); return false; }

  g->stats = std::getenv("KESTREL_PRDP_STATS") != nullptr;
  if(g->stats) std::atexit([]{ dumpStats(); });
  g->ok = true;
  return true;
}

// Reparto de tiempo del hilo RDP. Se engancha a atexit cuando KESTREL_PRDP_STATS esta
// puesto, porque nadie llama a shutdown() en la salida normal.
auto dumpStats() -> void {
  if(!g || !g->stats) return;
  std::fprintf(stderr,
      "[vrdp] fifo=%llu (%.2f ms) gpuwait=%llu (%.2f ms) syncskip=%llu scanout=%.2f ms\n",
      (unsigned long long)g->nEnq, g->nsEnq / 1e6,
      (unsigned long long)g->nSync, g->nsWait / 1e6,
      (unsigned long long)g->nSkip, g->nsScan / 1e6);
}

auto shutdown() -> void {
  if(!g) return;
  dumpStats();
  if(g->proc) { delete g->proc; g->proc = nullptr; }
  delete g;
  g = nullptr;
}

auto active() -> bool { return g && g->ok; }

auto runFifo(const u8* rdram, u32 rdramSize, const u8* dmem, u32 start, u32 end, bool xbus)
    -> bool {
  if(!active()) return false;
  pumpViState();                                    // apply CPU-side VI writes / frame rotate

  u64 tEnq = g->stats ? nowNs() : 0;
  u64 subEnq = g->stats ? g->nsWait + g->nsScan : 0;   // el bloqueo/lectura se cobra aparte
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

    static const bool cmdLog = std::getenv("KESTREL_VRDPLOG") != nullptr;
    if(cmdLog) {
      std::fprintf(stderr, "[vrdp] %06x op=%02x len=%u w0=%08x w1=%08x\n",
                   cur, op, len, words[0], words[1]);
    }
    if(op >= 8) g->proc->enqueue_command(len * 2, words);

    // Solo estos escriben en el color/z image; el resto es estado o carga de TMEM.
    if((op >= 0x08 && op <= 0x0f) || op == 0x24 || op == 0x25 || op == 0x36)
      g->drawsSinceSync++;

    if(::RDP::Op(op) == ::RDP::Op::SyncFull) {
      static const bool sfLog = std::getenv("KESTREL_DPSYNCLOG") != nullptr;
      if(sfLog) { std::fprintf(stderr, "[dpsync] at=%06x span=%06x..%06x xbus=%u\n", cur, start, end, (unsigned)xbus); std::fflush(stderr); }
      // El fence NO se difiere: SYNC_FULL significa "pipe drenado" y la interrupcion DP
      // que sigue autoriza a la CPU a reescribir el buffer. Diferirlo dejaria a la GPU
      // leyendo RDRAM que la CPU ya puede pisar. Medido ademas que no compensa: con el
      // fence saltado (experimento) SM64 no acelera -- el hilo RDP esta ocioso ~89% y el
      // fence cae dentro de ese hueco, no en el camino critico. Ver docs.
      if(g->drawsSinceSync) {
        u64 tw = g->stats ? nowNs() : 0;
        g->proc->wait_for_timeline(g->proc->signal_timeline());
        if(g->stats) { g->nsWait += nowNs() - tw; g->nSync++; }
        g->drawsSinceSync = 0;
      } else if(g->stats) {
        g->nSkip++;
      }
      sawSyncFull = true;
    }
    cur += len * 8;
  }
  if(g->stats) { g->nsEnq += nowNs() - tEnq - (g->nsWait + g->nsScan - subEnq); g->nEnq++; }
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
