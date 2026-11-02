#pragma once
// Last-writer map -- debug tag per 16-byte RDRAM block saying who wrote it last.
//
// Responsibility: find who corrupted code or data (CPU uncached, CPU D-cache write-back, SP/PI/SI
// DMA, RDP) and report writes that land inside RDP FIFO spans not yet consumed. Armed only by
// KESTREL_WRTAG=1 via init(); disarmed, every hook is one compare against the null `tag`.
//
// Invariants:
// - init() runs from Memory::reset(), right after RDRAM is sized; tag/pcOf/blocks keep that
//   size until the next reset.
// - win[] holds the unconsumed FIFO windows as (hi << 32) | lo in ONE atomic, so a reader never
//   mixes bounds of two spans; setWin/moveWinLo publish with release, readers acquire.
// - dirtyProbe is installed by the CPU model (line valid and dirty in its D-cache).
//
// Threads: markers are called from whichever thread performs the write (CPU thread; SP DMA may
// also run on the RSP worker). The tag and pc arrays are plain unsynchronised bytes -- a
// debugging aid where a stale tag is acceptable -- while the windows and counters are atomics.
// Mapa de "ultimo escritor" de RDRAM, un tag por bloque de 16 bytes (= linea de D-cache).
// Cuando el guest se despena ejecutando codigo pisado, lo unico que hace falta saber es QUIEN
// lo piso: la CPU (cacheada o no), un DMA del SP/PI/SI, o el rasterizador. Sin esto solo se ve
// el destrozo, no el autor. Se arma con KESTREL_WRTAG=1; apagado cuesta una comparacion contra
// un puntero global que siempre esta en cache.
#include <cstdint>
#include <cstdlib>
#include <atomic>
#include <cstdio>
#include <vector>

namespace wrtag {

enum Who : uint8_t { kNone = 0, kCpu = 1, kDcache = 2, kSpDma = 3, kPiDma = 4, kSiDma = 5, kRdp = 6 };

inline const char* name(uint8_t w) {
  switch(w) {
    case kCpu:    return "CPU-uncached";
    case kDcache: return "CPU-dcache";
    case kSpDma:  return "SP-DMA";
    case kPiDma:  return "PI-DMA";
    case kSiDma:  return "SI-DMA";
    case kRdp:    return "RDP";
    default:      return "-";
  }
}

inline std::vector<uint8_t> tagBuf;
inline std::vector<uint32_t> pcBuf;
inline uint8_t* tag = nullptr;
inline uint32_t* pcOf = nullptr;
inline uint32_t blocks = 0;

inline auto init(uint32_t ramBytes) -> void {
  if(!std::getenv("KESTREL_WRTAG")) return;
  blocks = ramBytes >> 4;
  tagBuf.assign(blocks, 0);
  pcBuf.assign(blocks, 0);
  tag = tagBuf.data();
  pcOf = pcBuf.data();
}

// Ventanas de FIFO del RDP todavia SIN CONSUMIR. win[0] = el tramo que el rasterizador esta
// leyendo ahora mismo (desde su puntero de lectura hasta el final del tramo); win[1] = la union
// de los tramos encolados que aun no ha empezado. Cualquier escritura de la CPU o de un DMA ahi
// dentro es el productor pisando comandos que el RDP no ha leido: la corrupcion que buscamos.
inline std::atomic<uint64_t> win[2] = {};   // (hi << 32) | lo, en un solo atomico para que
inline std::atomic<int> fifoN{0};           // el lector nunca vea un lo de un tramo y un hi de otro

inline auto setWin(int i, uint32_t lo, uint32_t hi) -> void {
  if(!tag) return;
  win[i].store(((uint64_t)hi << 32) | lo, std::memory_order_release);
}

inline auto moveWinLo(int i, uint32_t lo) -> void {
  uint64_t w = win[i].load(std::memory_order_relaxed);
  win[i].store((w & 0xffffffff00000000ull) | lo, std::memory_order_release);
}

inline auto fifoCheck(uint32_t phys, uint8_t who, uint32_t pc = 0) -> void {
  if(who == kRdp) return;
  for(int i = 0; i < 2; i++) {
    uint64_t w = win[i].load(std::memory_order_acquire);
    uint32_t hi = (uint32_t)(w >> 32), lo = (uint32_t)w;
    if(!hi || phys < lo || phys >= hi) continue;
    if(fifoN.fetch_add(1, std::memory_order_relaxed) >= 40) return;
    std::fprintf(stderr, "[fifo!] %s pc=%08x piso phys=0x%06x dentro de FIFO sin consumir %s=[%06x,%06x)\n",
                 name(who), pc, phys, i ? "encolado" : "en curso", lo, hi);
    std::fflush(stderr);
    return;
  }
}

inline auto inWin(uint32_t phys) -> bool {
  for(int i = 0; i < 2; i++) {
    uint64_t w = win[i].load(std::memory_order_acquire);
    uint32_t hi = (uint32_t)(w >> 32), lo = (uint32_t)w;
    if(hi && phys >= lo && phys < hi) return true;
  }
  return false;
}

// Volcado de una linea de D$: solo corrompe si algun byte que CAMBIA cae en el FIFO sin
// consumir (los bytes de la linea fuera de la ventana son comandos nuevos, legitimos).
inline auto fifoCheckLine(uint32_t phys, const uint8_t* oldb, const uint8_t* newb, uint8_t who, uint32_t pc) -> void {
  if(!tag) return;
  for(uint32_t i = 0; i < 16; i++)
    if(oldb[i] != newb[i] && inWin(phys + i)) { fifoCheck(phys + i, who, pc); return; }
}

// changed = false: la escritura deja los bytes como estaban (volcado de una linea de D$ que
// coincide con RDRAM) y no corrompe nada aunque caiga en el FIFO: no se chiva.
inline auto mark(uint32_t phys, uint8_t who, uint32_t pc, bool changed = true) -> void {
  if(!tag) return;
  uint32_t b = phys >> 4;
  if(b >= blocks) return;
  tag[b] = who;
  pcOf[b] = pc;
  if(changed) fifoCheck(phys, who, pc);
}

// Coherencia: un DMA que escribe RDRAM bajo una linea que la D$ de la CPU tiene SUCIA se
// pierde (o corrompe) cuando esa linea se expulse, sea cual sea el ritmo del RDP. Lo instala
// la CPU (dirtyProbe(phys) = la linea de phys esta valida y sucia en su D$).
inline bool (*dirtyProbe)(uint32_t phys) = nullptr;
inline std::atomic<int> dirtyN{0};

inline auto markRange(uint32_t phys, uint32_t nbytes, uint8_t who, uint32_t pc) -> void {
  if(!tag || !nbytes) return;
  if(dirtyProbe && who == kSpDma)
    for(uint32_t a = phys & ~15u; a < phys + nbytes; a += 16)
      if(dirtyProbe(a) && dirtyN.fetch_add(1, std::memory_order_relaxed) < 40) {
        std::fprintf(stderr, "[dirty!] SP-DMA pc=%08x escribe phys=0x%06x con la linea SUCIA en la D$ de la CPU (ultimo escritor CPU pc=%08x)\n",
                     pc, a, pcOf[a >> 4]);
        std::fflush(stderr);
      }
  for(uint32_t b = phys >> 4, e = (phys + nbytes - 1) >> 4; b <= e && b < blocks; b++) {
    tag[b] = who;
    pcOf[b] = pc;
  }
}

}  // namespace wrtag
