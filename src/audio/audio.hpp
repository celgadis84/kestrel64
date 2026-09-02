#pragma once
// kestrel64 — host audio sink. The AI register model in memory.cpp already paces
// the game correctly (FIFO of 2 buffers, MI_AI on drain, dacrate timing); this is
// the missing last mile: it takes the 16-bit big-endian stereo PCM the RSP audio
// microcode leaves in RDRAM and actually pushes it to the speakers.
//
// Design notes:
//  - Windows-only backend (waveOut / winmm). On other platforms the calls compile
//    to no-ops, so the emulator core stays portable.
//  - Fully decoupled from the deterministic core: init/push/shutdown never touch
//    emulated state and never write RDRAM, so systemtest and the lockstep/threaded
//    framebuffer md5 are unaffected. It only *reads* RDRAM sample bytes.
//  - N64 stores samples as big-endian s16; the pull-from-RDRAM helper byteswaps.

#include "../core/types.hpp"

namespace kestrel::audio {

// Open (or reopen, if the sample rate changed) the host device as 16-bit stereo
// at `sampleRate` Hz. No-op when audio is disabled (env KESTREL_AUDIO=0) or the
// platform has no backend.
auto init(u32 sampleRate) -> void;

// Queue `nframes` interleaved host-endian s16 stereo frames.
auto push(const s16* frames, u32 nframes) -> void;

// Pull `len` bytes of big-endian s16 stereo PCM from a RDRAM span, byteswap to
// host order, and queue for playback. `base`/`size` bound the RDRAM buffer;
// `addr` is the physical start, `len` the byte count. Opens the device lazily at
// `sampleRate` on the first call. Bounds-checked; a partial/oob span is clipped.
auto pushRdram(const u8* base, u32 size, u32 addr, u32 len, u32 sampleRate) -> void;

// Stop playback and release the device. Safe to call when never initialised.
auto shutdown() -> void;

// True once a backend device is open. Lets callers skip the byteswap work when
// audio is disabled.
auto enabled() -> bool;

// Instantanea de hambre para el latido (KESTREL_HEARTBEAT). `silence`/`pulled` son
// acumulados desde el arranque; `lowSince` es el minimo del anillo DESDE LA ULTIMA
// llamada -- se rearma aqui -- porque lo que delata un corte es el colchon que hubo en
// esa ventana, no el minimo historico (que casi siempre es 0 por el cebado inicial).
// Devuelve false si no hay dispositivo abierto.
auto statSnapshot(u64& pulled, u64& silence, u64& dropped,
                  u32& level, u32& lowSince, u32& cap) -> bool;

}  // namespace kestrel::audio
