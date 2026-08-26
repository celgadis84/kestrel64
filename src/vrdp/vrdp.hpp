#pragma once
// kestrel64 — paraLLEl-RDP (GPU) backend glue.
//
// Thin wrapper over Themaister's ::RDP::CommandProcessor + Granite Vulkan. Hides the
// whole Granite stack behind plain kestrel types (pimpl), so the emulator core only ever
// sees these free functions. Built only when KESTREL_PRDP is defined; every entry point
// is a cheap no-op when the backend is inactive, so callers can invoke unconditionally.
//
// Determinism: this is a display/raster path only. It reads guest RDRAM and produces host
// RGBA pixels; it never writes back into the deterministic core state. Keep systemtest and
// the lockstep/threaded md5 on the SoftRDP path (PRDP off).

#include "../core/types.hpp"

namespace kestrel::vrdp {

#ifndef KESTREL_PRDP
// Backend not built: every entry point is an inline no-op so core code can call
// vrdp::* unconditionally without link errors or #ifdef clutter at the call sites.
inline auto init(u8*, u32) -> bool { return false; }
inline auto shutdown() -> void {}
inline auto active() -> bool { return false; }
inline auto runFifo(const u8*, u32, const u8*, u32, u32, bool) -> bool { return false; }
inline auto viWrite(u32, u32) -> void {}
inline auto frameBegin() -> void {}
inline auto scanout(u32& w, u32& h) -> const u8* { w = h = 0; return nullptr; }
inline auto scanoutDone() -> void {}
struct SharedVk { void* instance; void* gpu; void* device; u32 queueFamily; void* queue; };
inline auto sharedVk() -> const SharedVk* { return nullptr; }
inline auto queueLock()   -> void {}
inline auto queueUnlock() -> void {}
#else

// Bring up the GPU backend over the guest RDRAM block. Returns false (and stays inactive)
// if KESTREL_PRDP=1 is not set, if the build lacks the backend, or if no usable Vulkan
// device/GPU is present — callers then fall back to SoftRDP. Safe to call once at boot.
auto init(u8* rdram, u32 size) -> bool;
auto shutdown() -> void;

// True once init() succeeded and the backend is live. All other calls are no-ops otherwise.
auto active() -> bool;

// Feed one DP command FIFO span. Words are fetched from RDRAM (or DMEM when xbus), split by
// the RDP command-length table, and enqueued. Returns true iff a SYNC_FULL was seen (the
// caller raises MI_DP on that, exactly as with SoftRdp::sawSyncFull). `dmem` may be null
// when xbus is false.
auto runFifo(const u8* rdram, u32 rdramSize, const u8* dmem, u32 start, u32 end, bool xbus)
    -> bool;

// Forward a VI register write (index = VI register byte offset >> 2). Mirrors the guest's
// VI programming so the GPU scanout matches.
auto viWrite(u32 index, u32 value) -> void;

// Per-emulated-field frame boundary (rotates Granite frame contexts).
auto frameBegin() -> void;

// Produce the current scanned-out image as host RGBA8888. Returns nullptr if none is ready.
// Call scanoutDone() when finished reading the returned pointer.
auto scanout(u32& width, u32& height) -> const u8*;
auto scanoutDone() -> void;

// Handles del contexto Vulkan de parallel-rdp. El presentador los reutiliza en vez de crear
// un segundo dispositivo: volk resuelve los punteros de funcion en UNA tabla global, asi que
// dos contextos vivos se pisan las entradas y la siguiente llamada salta a un puntero nulo.
// Se dan como void* para no arrastrar vulkan.h hasta el nucleo; present.cpp los recastea.
// Devuelve nullptr si el backend no esta activo.
struct SharedVk { void* instance; void* gpu; void* device; u32 queueFamily; void* queue; };
auto sharedVk() -> const SharedVk*;

// Candado de la cola grafica compartida. vkQueueSubmit/vkQueuePresentKHR sobre una misma cola
// no son seguros entre hilos, y aqui submiten dos: Granite desde el hilo del RDP y el
// presentador desde el de la ventana. Ambos pasan por este mismo mutex.
auto queueLock()   -> void;
auto queueUnlock() -> void;

#endif  // KESTREL_PRDP

}  // namespace kestrel::vrdp
