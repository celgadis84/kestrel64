// kestrel64 — host audio sink implementation (waveOut backend on Windows).
//
// A single ring buffer holds host-endian s16 stereo samples produced by the
// emulator thread (via push/pushRdram). A dedicated feeder thread owns a small
// pool of waveOut buffers: whenever one finishes playing it refills it from the
// ring — padding with silence on underrun so the device never stalls — and
// re-queues it. waveOut clocks playback at the opened sample rate, so this thread
// only ever has to keep the ring drained; it does no timing of its own.
//
// The emulator side is lock-light: push() copies under a short mutex and returns.
// Nothing here writes emulated state, so it cannot perturb determinism.

#include "audio.hpp"
#include "../core/runtime.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  include <windows.h>
#  include <mmsystem.h>
#endif

namespace kestrel::audio {

namespace {

#ifdef _WIN32

constexpr int  kChannels   = 2;
constexpr int  kBufFrames  = 512;                 // ~11 ms @ 44.1 kHz per waveOut buffer
constexpr int  kNumBufs    = 4;                   // total ~46 ms of device-side latency
constexpr int  kBufSamples = kBufFrames * kChannels;
// Cap the software ring so a fast (faster-than-realtime) emulator can't grow it
// without bound; when it fills we drop the oldest, keeping latency in check.
constexpr u32  kRingFrames = 44100 / 4;           // ~250 ms of slack
// Colchon minimo antes de empezar (o de volver) a tocar. Tiene que ser lo que el alimentador
// va a encolar DE GOLPE en cuanto se ceba, que son los kNumBufs bufers del dispositivo: con
// dos, la primera vuelta encolaba cuatro y los dos ultimos salian medio vacios -- ringPull
// rellena de ceros lo que no hay. Medido en DK64: ~9600 muestras de silencio por arranque,
// siempre las mismas corriera lo que corriera, o sea un hipo fijo al empezar a sonar.
constexpr u32  kPrimeSamples = (u32)(kBufSamples * kNumBufs);

struct Backend {
  std::mutex          mtx;                          // guards the ring
  std::vector<s16>    ring;                         // interleaved stereo, size = kRingFrames*2
  u32                 rdPos = 0, wrPos = 0, count = 0;  // ring cursors, in samples

  HWAVEOUT            dev = nullptr;
  HANDLE             doneEvt = nullptr;
  WAVEHDR            hdr[kNumBufs] = {};
  std::vector<s16>   buf[kNumBufs];                 // backing store per waveOut buffer
  std::thread        feeder;
  std::atomic<bool>  running{false};
  u32                rate = 0;
  bool               disabled = false;
  // Cebado. El primer bufer no se manda hasta que el anillo tiene colchon: si se empieza a
  // tocar con el anillo vacio, el dispositivo se come el arranque en silencio y ya nunca
  // recupera ventaja, porque el hueco se rellena con ceros en vez de esperar. Tambien se
  // vuelve a cebar despues de un hueco, para reconstruir el colchon en vez de ir dando
  // hipidos seguidos.
  bool               primed = false;

  // Contabilidad del hambre. `KESTREL_AUDIOSTAT` la imprime al cerrar. Sin numeros, un
  // "se oye entrecortado" no distingue entre el emulador que no produce a tiempo (hueco
  // en el anillo) y el que produce de mas (se tira lo viejo), y el arreglo es distinto.
  u64                statPulled = 0, statSilence = 0, statDropped = 0, statPushed = 0;
  u64                statRefills = 0, statStarved = 0;   // recargas y cuantas salieron cortas
  u32                statLowMark = 0xffffffffu, statHighMark = 0;
  u32                statLowLife = 0xffffffffu;   // minimo de por vida (statLowMark se rearma por ventana)
};

Backend g;

// Pop up to `n` samples from the ring into `dst`; zero-fill the remainder.
auto ringPull(s16* dst, u32 n) -> void {
  std::lock_guard<std::mutex> lk(g.mtx);
  u32 got = n < g.count ? n : g.count;
  // En dos tramos con memcpy en vez de muestra a muestra con modulo: esto corre con el
  // mutex cogido y el que espera detras es el hilo de emulacion.
  const u32 cap = (u32)g.ring.size();
  u32 first = got < cap - g.rdPos ? got : cap - g.rdPos;
  std::memcpy(dst, &g.ring[g.rdPos], first * sizeof(s16));
  if(got > first) std::memcpy(dst + first, &g.ring[0], (got - first) * sizeof(s16));
  g.rdPos = (g.rdPos + got) % cap;
  g.count -= got;
  for(u32 i = got; i < n; i++) dst[i] = 0;         // underrun → silence, no stall
  g.statPulled += got; g.statSilence += (n - got);
  g.statRefills++; if(got < n) g.statStarved++;
  if(g.count < g.statLowMark) g.statLowMark = g.count;
  if(g.count < g.statLowLife) g.statLowLife = g.count;
  if(g.count > g.statHighMark) g.statHighMark = g.count;
}

auto ringPush(const s16* src, u32 n) -> void {
  std::lock_guard<std::mutex> lk(g.mtx);
  u32 cap = (u32)g.ring.size();
  g.statPushed += n;
  if(n >= cap) { src += n - cap; g.statDropped += n - cap; n = cap; }   // no cabe ni entero
  if(g.count + n > cap) {                           // lleno: se tira lo mas viejo
    u32 drop = g.count + n - cap;
    g.rdPos = (g.rdPos + drop) % cap;
    g.count -= drop;
    g.statDropped += drop;
  }
  u32 first = n < cap - g.wrPos ? n : cap - g.wrPos;
  std::memcpy(&g.ring[g.wrPos], src, first * sizeof(s16));
  if(n > first) std::memcpy(&g.ring[0], src + first, (n - first) * sizeof(s16));
  g.wrPos = (g.wrPos + n) % cap;
  g.count += n;
}

// Feeder thread: keep every finished waveOut buffer refilled and requeued.
auto feederLoop() -> void {
  while(g.running.load(std::memory_order_acquire)) {
    if(!g.primed) {                                  // esperando colchon: no se toca nada
      u32 have;
      { std::lock_guard<std::mutex> lk(g.mtx); have = g.count; }
      if(have < kPrimeSamples) { Sleep(1); continue; }
      g.primed = true;
    }
    bool anyIdle = false;
    for(int i = 0; i < kNumBufs; i++) {
      if(g.hdr[i].dwFlags & WHDR_INQUEUE) continue;  // still playing
      // Un bufer a medias es un hueco audible: ringPull rellena de ceros lo que falta. Antes
      // de sacar nada se comprueba que hay bufer ENTERO; si no lo hay se descebra y se vuelve
      // a esperar colchon, que es lo que ya hacia el codigo pero DESPUES de haber servido el
      // hueco. Lo que ya esta encolado sigue sonando mientras tanto.
      { std::lock_guard<std::mutex> lk(g.mtx); if(g.count < (u32)kBufSamples) g.primed = false; }
      if(!g.primed) break;
      anyIdle = true;
      if(g.hdr[i].dwFlags & WHDR_PREPARED)
        waveOutUnprepareHeader(g.dev, &g.hdr[i], sizeof(WAVEHDR));
      ringPull(g.buf[i].data(), kBufSamples);
      g.hdr[i] = WAVEHDR{};
      g.hdr[i].lpData = reinterpret_cast<LPSTR>(g.buf[i].data());
      g.hdr[i].dwBufferLength = kBufSamples * sizeof(s16);
      waveOutPrepareHeader(g.dev, &g.hdr[i], sizeof(WAVEHDR));
      waveOutWrite(g.dev, &g.hdr[i], sizeof(WAVEHDR));
    }
    if(!anyIdle) WaitForSingleObject(g.doneEvt, 10);  // all queued: wait for one to finish
  }
}

auto openDevice(u32 sampleRate) -> void {
  WAVEFORMATEX wf = {};
  wf.wFormatTag      = WAVE_FORMAT_PCM;
  wf.nChannels       = kChannels;
  wf.nSamplesPerSec  = sampleRate;
  wf.wBitsPerSample  = 16;
  wf.nBlockAlign     = wf.nChannels * wf.wBitsPerSample / 8;
  wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;

  g.doneEvt = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  if(waveOutOpen(&g.dev, WAVE_MAPPER, &wf, (DWORD_PTR)g.doneEvt, 0,
                 CALLBACK_EVENT) != MMSYSERR_NOERROR) {
    g.dev = nullptr;
    if(g.doneEvt) { CloseHandle(g.doneEvt); g.doneEvt = nullptr; }
    g.disabled = true;                              // no device: quietly give up
    return;
  }
  for(int i = 0; i < kNumBufs; i++) g.buf[i].assign(kBufSamples, 0);
  g.ring.assign((size_t)kRingFrames * kChannels, 0);
  g.rdPos = g.wrPos = g.count = 0;
  g.primed = false;
  g.rate = sampleRate;
  g.running.store(true, std::memory_order_release);
  g.feeder = std::thread(feederLoop);
}

auto closeDevice() -> void {
  if(!g.dev) return;
  if(std::getenv("KESTREL_AUDIOSTAT")) {
    // Silencio y descartes son las dos caras: hueco = el emulador llego tarde, descarte =
    // llego de sobra y la latencia se estaba yendo. El minimo del anillo dice cuanto colchon
    // hubo de verdad; si roza cero, el siguiente hipo ya se oye.
    double sil = g.statPulled + g.statSilence ? 100.0 * (double)g.statSilence
                                              / (double)(g.statPulled + g.statSilence) : 0.0;
    std::fprintf(stderr,
      "[audio] rate=%u empujadas=%llu servidas=%llu silencio=%llu (%.2f%%) descartadas=%llu\n"
      "[audio] recargas=%llu cortas=%llu anillo min=%u max=%u de %u muestras\n",
      g.rate, (unsigned long long)g.statPushed, (unsigned long long)g.statPulled,
      (unsigned long long)g.statSilence, sil, (unsigned long long)g.statDropped,
      (unsigned long long)g.statRefills, (unsigned long long)g.statStarved,
      g.statLowLife == 0xffffffffu ? 0u : g.statLowLife, g.statHighMark,
      (u32)g.ring.size());
  }
  g.running.store(false, std::memory_order_release);
  if(g.doneEvt) SetEvent(g.doneEvt);
  if(g.feeder.joinable()) g.feeder.join();
  waveOutReset(g.dev);
  for(int i = 0; i < kNumBufs; i++)
    if(g.hdr[i].dwFlags & WHDR_PREPARED)
      waveOutUnprepareHeader(g.dev, &g.hdr[i], sizeof(WAVEHDR));
  waveOutClose(g.dev);
  g.dev = nullptr;
  if(g.doneEvt) { CloseHandle(g.doneEvt); g.doneEvt = nullptr; }
}

#endif  // _WIN32

}  // namespace

auto init(u32 sampleRate) -> void {
#ifdef _WIN32
  if(sampleRate < 8000 || sampleRate > 96000) return;   // implausible; ignore
  if(const char* e = std::getenv("KESTREL_AUDIO"); e && e[0] == '0') { g.disabled = true; return; }
  if(g.disabled) return;
  if(g.dev && g.rate == sampleRate) return;              // already open at this rate
  // Reabrir el dispositivo corta el sonido, asi que un cambio de dacrate de dos duros no lo
  // justifica: los juegos ajustan la tasa del AI en pasos minusculos (redondeo del divisor)
  // y a menos del 1% la diferencia de tono no se oye, mientras que el corte SI.
  if(g.dev) {
    u32 lo = g.rate < sampleRate ? g.rate : sampleRate, hi = g.rate ^ sampleRate ^ lo;
    if((hi - lo) * 100u <= hi) return;   // se sigue tocando a la tasa ya abierta
  }
  if(g.dev) closeDevice();                               // rate changed → reopen
  openDevice(sampleRate);
#else
  (void)sampleRate;
#endif
}

auto push(const s16* frames, u32 nframes) -> void {
#ifdef _WIN32
  if(!g.dev) return;
  ringPush(frames, nframes * kChannels);
#else
  (void)frames; (void)nframes;
#endif
}

auto pushRdram(const u8* base, u32 size, u32 addr, u32 len, u32 sampleRate) -> void {
#ifdef _WIN32
  if(g.disabled) return;
  init(sampleRate);
  if(!g.dev) return;
  // Clip the span to the RDRAM bounds and to whole stereo frames (4 bytes each).
  if(addr >= size) return;
  u32 avail = size - addr;
  if(len > avail) len = avail;
  u32 nsamp = (len / 2);                                 // 16-bit samples
  nsamp &= ~1u;                                          // keep whole L/R pairs
  if(nsamp == 0) return;
  if(static bool tr = std::getenv("KESTREL_AUDIO_TRACE") != nullptr; tr) {
    static u32 n = 0;
    if(n++ < 12)
      std::fprintf(stderr, "[audio] push #%u addr=%06x len=%u rate=%u dev=%d\n",
                   n, addr, len, sampleRate, g.dev != nullptr);
  }
  static thread_local std::vector<s16> tmp;
  tmp.resize(nsamp);
  const u8* p = base + addr;
  for(u32 i = 0; i < nsamp; i++) {                       // big-endian s16 → host
    tmp[i] = (s16)((p[i * 2] << 8) | p[i * 2 + 1]);
  }
  // Silenciar y volumen se aplican AQUI, no cerrando el dispositivo: el menu puede quitar
  // el sonido a mitad de partida desde otro hilo, y cerrar waveOut por debajo del hilo que
  // empuja seria una carrera. Ademas se siguen metiendo muestras (mudas) en el anillo, con
  // lo que el colchon y las estadisticas de hambre siguen midiendo lo mismo.
  if(!rt::audioOn.load(std::memory_order_relaxed)) {
    std::memset(tmp.data(), 0, nsamp * sizeof(s16));
  } else if(int vol = rt::volume.load(std::memory_order_relaxed); vol != 100) {
    if(vol < 0) vol = 0; else if(vol > 100) vol = 100;
    for(u32 i = 0; i < nsamp; i++) tmp[i] = (s16)((int)tmp[i] * vol / 100);
  }
  ringPush(tmp.data(), nsamp);
#else
  (void)base; (void)size; (void)addr; (void)len; (void)sampleRate;
#endif
}

auto statSnapshot(u64& pulled, u64& silence, u64& dropped,
                  u32& level, u32& lowSince, u32& cap) -> bool {
#ifdef _WIN32
  if(!g.dev) return false;
  std::lock_guard<std::mutex> lk(g.mtx);
  pulled = g.statPulled; silence = g.statSilence; dropped = g.statDropped;
  level = g.count; cap = (u32)g.ring.size();
  lowSince = g.statLowMark == 0xffffffffu ? g.count : g.statLowMark;
  g.statLowMark = g.count;                 // rearmado: el minimo es POR VENTANA
  return true;
#else
  (void)pulled; (void)silence; (void)dropped; (void)level; (void)lowSince; (void)cap;
  return false;
#endif
}

auto shutdown() -> void {
#ifdef _WIN32
  closeDevice();
#endif
}

auto enabled() -> bool {
#ifdef _WIN32
  return g.dev != nullptr;
#else
  return false;
#endif
}

}  // namespace kestrel::audio
