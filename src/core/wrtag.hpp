#pragma once
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

inline auto fifoCheck(uint32_t phys, uint8_t who) -> void {
  if(who == kRdp) return;
  for(int i = 0; i < 2; i++) {
    uint64_t w = win[i].load(std::memory_order_acquire);
    uint32_t hi = (uint32_t)(w >> 32), lo = (uint32_t)w;
    if(!hi || phys < lo || phys >= hi) continue;
    if(fifoN.fetch_add(1, std::memory_order_relaxed) >= 40) return;
    std::fprintf(stderr, "[fifo!] %s piso phys=0x%06x dentro de FIFO sin consumir %s=[%06x,%06x)\n",
                 name(who), phys, i ? "encolado" : "en curso", lo, hi);
    std::fflush(stderr);
    return;
  }
}

inline auto mark(uint32_t phys, uint8_t who, uint32_t pc) -> void {
  if(!tag) return;
  uint32_t b = phys >> 4;
  if(b >= blocks) return;
  tag[b] = who;
  pcOf[b] = pc;
  fifoCheck(phys, who);
}

inline auto markRange(uint32_t phys, uint32_t nbytes, uint8_t who, uint32_t pc) -> void {
  if(!tag || !nbytes) return;
  for(uint32_t b = phys >> 4, e = (phys + nbytes - 1) >> 4; b <= e && b < blocks; b++) {
    tag[b] = who;
    pcOf[b] = pc;
  }
}

}  // namespace wrtag
