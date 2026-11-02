#pragma once
// Internals shared by the SoftRDP translation units. Not an interface: only
// src/rdp/rdp*.cpp include this.
//
// Layout of SoftRDP:
//   rdp.cpp      cost model, rasterizer (triangles, rectangles, fills), texture
//                sampling, combiner, blender, HD texture hooks (all per-pixel code
//                stays in one translation unit so it inlines)
//   rdp_cmd.cpp  TMEM loads (LOAD_BLOCK/LOAD_TILE), GPU-RDP batching and the command
//                loop (run)
#include "rdp.hpp"
#include "../gpurdp/gpurdp.hpp"

namespace kestrel {

// --- raw big-endian RDRAM access (physical addresses) ------------------------
namespace {
template<typename V>
inline auto rd32(const V& m, u32 p) -> u32 {
  if(p + 3 >= m.size()) return 0;
  return (u32(m[p]) << 24) | (u32(m[p+1]) << 16) | (u32(m[p+2]) << 8) | u32(m[p+3]);
}
template<typename V>
inline auto rd64(const V& m, u32 p) -> u64 {
  return (u64(rd32(m, p)) << 32) | rd32(m, p + 4);
}
// Igual, pero sobre un puntero crudo: la instantanea del FIFO que el productor dejo al
// encolar el tramo (mismo tamano y mismas direcciones fisicas que la RDRAM, ver
// Memory::rdpSnapshot). `n` acota como lo hace m.size().
inline auto rd32p(const u8* m, u32 p, u32 n) -> u32 {
  if(p + 3 >= n) return 0;
  return (u32(m[p]) << 24) | (u32(m[p+1]) << 16) | (u32(m[p+2]) << 8) | u32(m[p+3]);
}
inline auto rd64p(const u8* m, u32 p, u32 n) -> u64 {
  return (u64(rd32p(m, p, n)) << 32) | rd32p(m, p + 4, n);
}
}  // namespace

// This span batches to GPU-RDP (checked per command by both halves).
inline auto SoftRdp::gpuOn() const -> bool { return gpuRun && gpurdp::active(); }

}  // namespace kestrel
