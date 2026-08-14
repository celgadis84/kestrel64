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
};

Backend g;

// Pop up to `n` samples from the ring into `dst`; zero-fill the remainder.
auto ringPull(s16* dst, u32 n) -> void {
  std::lock_guard<std::mutex> lk(g.mtx);
  u32 got = n < g.count ? n : g.count;
  for(u32 i = 0; i < got; i++) {
    dst[i] = g.ring[g.rdPos];
    g.rdPos = (g.rdPos + 1) % g.ring.size();
  }
  g.count -= got;
  for(u32 i = got; i < n; i++) dst[i] = 0;         // underrun → silence, no stall
}

auto ringPush(const s16* src, u32 n) -> void {
  std::lock_guard<std::mutex> lk(g.mtx);
  u32 cap = (u32)g.ring.size();
  for(u32 i = 0; i < n; i++) {
    if(g.count == cap) {                            // full: drop oldest sample
      g.rdPos = (g.rdPos + 1) % cap;
      g.count--;
    }
    g.ring[g.wrPos] = src[i];
    g.wrPos = (g.wrPos + 1) % cap;
    g.count++;
  }
}

// Feeder thread: keep every finished waveOut buffer refilled and requeued.
auto feederLoop() -> void {
  while(g.running.load(std::memory_order_acquire)) {
    bool anyIdle = false;
    for(int i = 0; i < kNumBufs; i++) {
      if(g.hdr[i].dwFlags & WHDR_INQUEUE) continue;  // still playing
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
  g.rate = sampleRate;
  g.running.store(true, std::memory_order_release);
  g.feeder = std::thread(feederLoop);
}

auto closeDevice() -> void {
  if(!g.dev) return;
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
  ringPush(tmp.data(), nsamp);
#else
  (void)base; (void)size; (void)addr; (void)len; (void)sampleRate;
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
