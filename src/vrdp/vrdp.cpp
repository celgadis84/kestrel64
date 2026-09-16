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
// Campo VI actual, solo para armar la traza de comandos (KESTREL_VRDPLOGFROM). El log
// completo cuesta lo bastante como para que el juego avance a otro ritmo y la escena que se
// busca no llegue a salir; armandolo tarde, los campos previos corren a velocidad normal.
u64 logField = 0;

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
  bool direct = false;                   // procesado de comandos sin anillo (ver init)

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
    // El tamano del scanout se dice UNA vez y cada vez que cambia: es la unica prueba de
    // que el escalado interno esta puesto (el volcado de framebuffer lee la RDRAM del
    // invitado, que sigue siendo 1x por definicion).
    if(sb.width != g->frameW || sb.height != g->frameH)
      std::fprintf(stderr, "[vrdp] scanout %ux%u\n", sb.width, sb.height);
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
  // Por defecto SI, cuando el backend esta compilado: la GPU es el camino del emulador.
  // KESTREL_PRDP=0 fuerza SoftRDP (lo que usan los gates deterministas y el A/B de perf).
  // Si Vulkan no arranca esto devuelve false mas abajo y el RDP software toma el relevo
  // solo -- no hay configuracion que dejar puesta para que una maquina sin GPU funcione.
  const char* e = std::getenv("KESTREL_PRDP");
  if(e && e[0] == '0') return false;

  // This thread (the RDP worker) is the sole Granite driver — register it as index 0 so
  // Granite's per-thread lookups resolve instead of spamming "thread does not exist".
  Util::register_thread_index(0);

  Util::set_thread_logging_interface(&gLog);
  if(!::Vulkan::Context::init_loader(nullptr)) return false;

  g = new Backend();
  // Extensiones de presentacion: el contexto lo comparte el presentador (ver sharedVk), asi
  // que la instancia necesita las de superficie y el dispositivo la de swapchain aunque
  // parallel-rdp por si solo no presente nada.
  static const char* kInstExt[] = { "VK_KHR_surface",
#ifdef _WIN32
                                    "VK_KHR_win32_surface"
#else
                                    "VK_KHR_xlib_surface"
#endif
                                  };
  static const char* kDevExt[]  = { "VK_KHR_swapchain" };
  if(!g->context.init_instance_and_device(kInstExt, 2, kDevExt, 1, 0)) { shutdown(); return false; }
  g->device.set_context(g->context);
  // El presentador comparte esta cola grafica (ver sharedVk) y submite desde el hilo de la
  // ventana mientras Granite submite desde el hilo del RDP. vkQueueSubmit NO es seguro entre
  // hilos sobre la misma cola: sin esto el driver pierde el dispositivo (VK_ERROR_DEVICE_LOST)
  // al primer solape. Granite toma este candado alrededor de cada submit suyo; el presentador
  // toma el mismo por vrdp::queueLock/queueUnlock.
  g->device.set_queue_lock([] { queueLock(); }, [] { queueUnlock(); });
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

  // Escalado interno. parallel-rdp puede rasterizar a 2x/4x/8x la resolucion del N64
  // manteniendo la semantica del RDP (el dominio ampliado es una RDRAM paralela; lo que el
  // juego lee de la RDRAM de verdad sigue siendo 1x, asi que un juego que se lea el
  // framebuffer no nota nada). Es una bandera del CommandProcessor y no del scanout porque
  // el factor decide el tamano de los buffers que se crean al construirlo: cambiarlo exige
  // relanzar, y por eso vive en el entorno y no en runtime.hpp.
  //
  // KESTREL_SSAA=1 cambia como se RESUELVE el dominio ampliado cuando hay que volcarlo a la
  // RDRAM de 1x (que es lo que ve el juego si se relee su propio framebuffer, y lo que sale
  // por el camino sin escalar): en vez de coger una muestra, promedia las NxN -- o sea
  // antialiasing por supermuestreo dentro del framebuffer del invitado, con tramado opcional
  // que aqui va emparejado. No encoge la imagen del scanout, que sigue saliendo grande.
  // parallel-rdp rechaza la combinacion con factor 1, de ahi la guarda. Por defecto apagado:
  // cuesta una pasada de resolucion extra por render pass.
  u32 cpFlags = 0;
  int upscale = 1;
  if(const char* u = std::getenv("KESTREL_UPSCALE")) {
    upscale = std::atoi(u);
    if(upscale != 1 && upscale != 2 && upscale != 4 && upscale != 8) {
      std::fprintf(stderr, "[vrdp] KESTREL_UPSCALE=%s no vale (1, 2, 4 u 8); se queda en 1\n", u);
      upscale = 1;
    }
  }
  if(upscale == 2) cpFlags |= ::RDP::COMMAND_PROCESSOR_FLAG_UPSCALING_2X_BIT;
  if(upscale == 4) cpFlags |= ::RDP::COMMAND_PROCESSOR_FLAG_UPSCALING_4X_BIT;
  if(upscale == 8) cpFlags |= ::RDP::COMMAND_PROCESSOR_FLAG_UPSCALING_8X_BIT;
  if(upscale > 1 && envFlag("KESTREL_SSAA", false))
    cpFlags |= ::RDP::COMMAND_PROCESSOR_FLAG_SUPER_SAMPLED_READ_BACK_BIT
             | ::RDP::COMMAND_PROCESSOR_FLAG_SUPER_SAMPLED_DITHER_BIT;

  // Procesado de comandos EN ESTE HILO, sin el anillo ni el hilo propio de parallel-rdp.
  // Todo Granite ya vive en el worker del RDP (ver arriba), asi que el anillo solo anadia un
  // salto entre hilos por comando y un hilo de anfitrion mas compitiendo por nucleos con CPU y
  // RSP. Mismos comandos, mismo orden: la imagen no cambia. Medido SM64 prdp-jit min de 5,
  // 5 rondas: anillo 3,44/3,45/3,47/3,44/3,42 s -> directo 3,42/3,37/3,39/3,37/3,38 (-1,7 %).
  // Lo unico que el anillo hacia por su cuenta es el aviso de ocio (Op::MetaIdle tras 500 us
  // sin comandos: manda a la GPU lo acumulado). Sin el, una lista que no acaba en SYNC_FULL
  // (krom HelloWorldRDP) no se pinta nunca. Ese aviso lo da ahora quien es dueno de Granite:
  // el worker del RDP al irse a dormir, o el cierre de campo en lockstep (ver vrdp::idle).
  // Bench con el aviso, 4 rondas: anillo 3,41-3,45 s -> directo 3,38-3,39.
  // parallel-rdp lo lee de su variable de entorno en el constructor; si el usuario la ha puesto
  // (PARALLEL_RDP_SINGLE_THREADED_COMMAND=0 vuelve al anillo) se respeta.
  if(!std::getenv("PARALLEL_RDP_SINGLE_THREADED_COMMAND"))
    _putenv_s("PARALLEL_RDP_SINGLE_THREADED_COMMAND", "1");
  if(const char* st = std::getenv("PARALLEL_RDP_SINGLE_THREADED_COMMAND")) g->direct = std::strtol(st, nullptr, 0) > 0;
  // hidden RDRAM (coverage/AA aux bits) is sized at rdram/2 on hardware.
  g->proc = new ::RDP::CommandProcessor(g->device, rdram, 0, size, size / 2, cpFlags);
  if(!g->proc->device_is_supported()) { shutdown(); return false; }
  if(upscale > 1)
    std::fprintf(stderr, "[vrdp] escalado interno %dx%s\n", upscale,
                 (cpFlags & ::RDP::COMMAND_PROCESSOR_FLAG_SUPER_SAMPLED_READ_BACK_BIT)
                   ? " con supermuestreo a resolucion nativa" : "");

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

static auto queueMutex() -> std::mutex& { static std::mutex m; return m; }
auto queueLock()   -> void { queueMutex().lock(); }
auto queueUnlock() -> void { queueMutex().unlock(); }

auto sharedVk() -> const SharedVk* {
  if(!g || !g->ok) return nullptr;
  static SharedVk sh{};
  sh.instance    = (void*)g->context.get_instance();
  sh.gpu         = (void*)g->context.get_gpu();
  sh.device      = (void*)g->context.get_device();
  sh.queueFamily = g->context.get_queue_info().family_indices[::Vulkan::QUEUE_INDEX_GRAPHICS];
  sh.queue       = (void*)g->context.get_queue_info().queues[::Vulkan::QUEUE_INDEX_GRAPHICS];
  return &sh;
}

auto shutdown() -> void {
  if(!g) return;
  dumpStats();
  if(g->proc) { delete g->proc; g->proc = nullptr; }
  delete g;
  g = nullptr;
}

auto active() -> bool { return g && g->ok; }

auto runFifo(const u8* rdram, u32 rdramSize, const u8* dmem, u32 start, u32 end, bool xbus,
             u32* stop) -> bool {
  if(stop) *stop = start;
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
    // El command processor no ejecuta comandos a medias. Si el ultimo del span no cabe
    // entero, se para DELANTE de el y espera a que END avance; el resto del comando llega
    // con el siguiente span. Leer mas alla de `fin` toma bytes que aun no son de este span
    // (en un FIFO circular, restos del frame anterior): un TEXRECT asi salia con s/t/dsdx
    // basura, que es lo que convertia los glifos de texto en barras verticales.
    if(cur + len * 8 > fin) break;

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
    // KESTREL_VRDPLOGMASK=<hex de 64 bits>: bit N = loguea el opcode N. Sin el, se loguea
    // todo, que a millones de comandos por segundo cambia la velocidad del emulador lo
    // bastante para que el juego avance a otro ritmo y la escena buscada no salga.
    static const u64 logMask = []{ const char* m = std::getenv("KESTREL_VRDPLOGMASK");
                                   return m ? std::strtoull(m, nullptr, 16) : ~0ull; }();
    static const u64 logFrom = []{ const char* f = std::getenv("KESTREL_VRDPLOGFROM");
                                   return f ? std::strtoull(f, nullptr, 0) : 0ull; }();
    if(cmdLog && logField >= logFrom && (logMask >> op) & 1) {
      std::fprintf(stderr, "[vrdp] %06x/%06x op=%02x len=%u w0=%08x w1=%08x w2=%08x w3=%08x\n",
                   cur, fin, op, len, words[0], words[1],
                   len > 1 ? words[2] : 0u, len > 1 ? words[3] : 0u);
    }
    if(op >= 8) g->proc->enqueue_command(len * 2, words);

    // Solo estos escriben en el color/z image; el resto es estado o carga de TMEM.
    if((op >= 0x08 && op <= 0x0f) || op == 0x24 || op == 0x25 || op == 0x36) {
      g->drawsSinceSync++;
      // KESTREL_PRDP_SYNCALL=1: esperar a la GPU tras CADA primitiva. No es fiel al
      // hardware (el RDP real dibuja en paralelo con la CPU) y cuesta un fence de ~1.6 ms
      // por triangulo, pero deja la escritura de RDRAM por parte de la GPU completamente
      // ordenada respecto al hilo de RDP. Solo para bisecar: si con esto desaparece una
      // corrupcion, la corrupcion venia de escrituras de la GPU en vuelo.
      static const bool syncAll = std::getenv("KESTREL_PRDP_SYNCALL") != nullptr;
      if(syncAll) { g->proc->wait_for_timeline(g->proc->signal_timeline()); g->drawsSinceSync = 0; }
    }

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
  if(stop) *stop = cur;
  return sawSyncFull;
}

auto viWrite(u32 index, u32 value) -> void {
  if(!g || index >= 14) return;                     // pure state stash; any thread
  g->viReg[index].store(value, std::memory_order_relaxed);
  g->viDirty.fetch_or(1u << index, std::memory_order_release);
}

auto idle() -> void {
  if(!g || !g->ok || !g->direct) return;   // con anillo, su hilo ya lo hace
  // Mismo umbral que el anillo (maintain_queues_idle: >= 32 primitivas o >= 2 render passes
  // pendientes); si no llega, no manda nada.
  const u32 w = u32(::RDP::Op::MetaIdle) << 24;
  g->proc->enqueue_command(1, &w);
}

auto frameBegin() -> void {
  logField++;
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
