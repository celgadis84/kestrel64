#include "rdp.hpp"
#include <cstdlib>
#include "../core/memory.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace kestrel {

// Conmutadores de depuracion del rasterizador. Estaban como `static` LOCALES dentro de
// blendPixel/aaPixel/el filtrado de textura, o sea funciones por-pixel: cada pixel pagaba
// la comprobacion de la guarda de inicializacion del static. Al subirlos a ambito de
// fichero se inicializan una vez al arrancar el proceso y el punto caliente lee una
// constante ya materializada.
static const bool g_noBlend  = std::getenv("KESTREL_NOBLEND")  != nullptr;
static const bool g_noAA     = std::getenv("KESTREL_NOAA")     != nullptr;
static const bool g_noFilter = std::getenv("KESTREL_NOFILTER") != nullptr;
static const bool g_noRaster = std::getenv("KESTREL_NORASTER") != nullptr;  // DIAG: salta el rasterizado


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
template<typename V>
inline auto wr8(V& m, u32 p, u8 v) -> void {
  if(p >= m.size()) return;
  m[p] = v;
}
template<typename V>
inline auto wr16(V& m, u32 p, u16 v) -> void {
  if(p + 1 >= m.size()) return;
  m[p] = u8(v >> 8); m[p+1] = u8(v);
}
template<typename V>
inline auto wr32(V& m, u32 p, u32 v) -> void {
  if(p + 3 >= m.size()) return;
  m[p] = u8(v >> 24); m[p+1] = u8(v >> 16); m[p+2] = u8(v >> 8); m[p+3] = u8(v);
}
// sign-extend an n-bit field
inline auto sx(u32 v, int bits) -> s32 {
  u32 m = 1u << (bits - 1);
  return (s32)((v ^ m) - m);
}
// 5-bit channel -> 8-bit, by bit-replication: (v<<3)|(v>>2). This is the exact
// N64 hardware expansion (angrylion replicated_rgba[i]=(i<<3)|(i>>2), tmem.c),
// NOT a linear *255/31 scale — the two differ by 1 in the mid-range (v=16: 132 vs
// 131). Used for framebuffer readback (blender IMAGE_READ) and RGBA16 texels/palette.
// TMEM texels and TLUT entries use the same expansion (parallel-rdp convert_rgba16).
// Truncation (v<<3) was tried and is wrong: krom's RDP/TextureCoordinates is built to
// expose exactly this, and it settles it without any combiner/blender in the way. At
// (164,69) the sample sits between texel $0000 (R=0) and $F800 (R=31) with tfrac=4/32
// and the combiner is a bare TEXEL0 pass-through, so the written 5-bit level is
// ((R01-R00)*4 + 0x10) >> 5. Truncation gives (248*4+16)>>5 = 31 -> level 3; replication
// gives (255*4+16)>>5 = 32 -> level 4, and level 4 is what the hardware capture holds.
inline auto exp5(u32 v) -> u32 { return (v << 3) | (v >> 2); }
// R8G8B8A8 -> RGBA5551 (N64 16bpp)
inline auto to5551(u32 c) -> u16 {
  u32 r = (c >> 24) & 0xff, g = (c >> 16) & 0xff, b = (c >> 8) & 0xff, a = c & 0xff;
  return u16(((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | (a ? 1 : 0));
}
}  // namespace

// --- DPC performance counters ------------------------------------------------
// The RDP rasterizes into a span buffer that holds ~8 RGBA16 pixels and then runs
// that chunk's RDRAM transactions in bulk, in the order color read, depth read,
// color write, depth write. A chunk costs max(pipeline, memory) GCLK:
//
//  * pipeline = 1 GCLK/pixel in 1-cycle mode, 2 in 2-cycle mode (FILL/COPY blast
//    64 bits per cycle = 4 RGBA16 pixels), plus a fixed per-chunk overhead.
//  * memory = one bus occupancy (XFER) per transaction, plus one RDRAM latency
//    stall (LAT) per chunk if the chunk reads at all — writes are posted, so a
//    lone color write disappears under the pipeline, which is why enabling the
//    framebuffer write costs almost nothing but enabling IM_RD nearly doubles the
//    fill time on hardware.
//  * RDRAM keeps ONE open row (0x800 bytes) per 1 MB bank. With the framebuffer
//    and the z-buffer in the same bank, every alternation between them (including
//    the wrap from the previous chunk) closes and reopens a row: ROW each.
//  * The VI reads the framebuffer continuously and outranks the RDP on the bus,
//    scaling every RDP transaction; sharing its bank also costs the open row.
//
// Constants are in GCLK and were calibrated by scripts/rdptiming.py against the
// 100 hardware configurations of Thar0's RDP-Timing-Tests (rmse 0.133, worst
// 0.30 cycles/pixel over fills spanning 1.01 .. 4.69 cycles/pixel).
namespace {
constexpr double T_XFER = 6.677, T_LAT = 3.583, T_ROW = 1.905;
constexpr double T_VI = 0.088, T_VIROW = 0.595, T_CHUNKOVH = 1.606;
constexpr int    T_CHUNK = 8;   // pixels buffered per span-buffer flush

// Cost in GCLK of one span chunk, given which buffers it touches. `seq` lists the
// transactions in hardware order as buffer ids (0 = color image, 1 = z image).
auto chunkCost(double pipeline, const int* seq, int n, bool reads, bool fbzbSame,
               bool viOn, bool fbviSame) -> double {
  if(n == 0) return pipeline;
  double mem = T_XFER * n + (reads ? T_LAT : 0.0);
  if(fbzbSame) {
    int changes = 0;
    for(int i = 0; i < n; i++) changes += seq[i] != seq[(i + n - 1) % n];
    mem += T_ROW * changes;
  }
  if(viOn) {
    mem *= 1.0 + T_VI;
    if(fbviSame) {
      int fb = 0;
      for(int i = 0; i < n; i++) fb += seq[i] == 0;
      mem += T_VIROW * fb;
    }
  }
  return mem > pipeline ? mem : pipeline;
}
}  // namespace

// Charge `npx` rasterized pixels to the DPC counters. `nWrite` of them wrote the
// color image and `nZWrite` wrote the z image (the rest were killed by alpha or
// depth compare, which on hardware suppresses both writes).
auto SoftRdp::accountPixels(Memory& mem, u64 npx, u64 nWrite, u64 nZWrite) -> void {
  if(!npx) return;
  bool fbRead = (other_lo & 0x40) != 0;                  // IM_RD
  bool zRead  = (other_lo & 0x10) != 0 && zi_addr != 0;  // Z_CMP
  bool zWrite = nZWrite != 0;
  bool fbzbSame = zi_addr != 0 && (ci_addr >> 20) == (zi_addr >> 20);
  bool viOn = (mem.rcp.vi_ctrl & 3) != 0 && mem.rcp.vi_origin != 0;
  bool fbviSame = viOn && (ci_addr >> 20) == ((mem.rcp.vi_origin & 0x00ff'ffff) >> 20);

  double pipeline = (cycleType() < 2 ? double(cycleType() + 1) : 0.25) * T_CHUNK + T_CHUNKOVH;
  int seq[4], n = 0;
  if(fbRead) seq[n++] = 0;
  if(zRead)  seq[n++] = 1;
  int nRead = n;
  // A killed pixel performs the reads but neither write, so the two outcomes have
  // different chunk costs; blend them by how many pixels actually wrote.
  double killed = chunkCost(pipeline, seq, n, nRead > 0, fbzbSame, viOn, fbviSame);
  seq[n++] = 0;
  if(zWrite) seq[n++] = 1;
  double wrote = chunkCost(pipeline, seq, n, nRead > 0, fbzbSame, viOn, fbviSame);

  double frac = double(nWrite) / double(npx);
  double cycles = (frac * wrote + (1.0 - frac) * killed) * double(npx) / T_CHUNK;
  u32 c = (u32)(u64)cycles;
  mem.rcp.dpc_clock.fetch_add(c, std::memory_order_relaxed);
  mem.rcp.dpc_pipebusy.fetch_add(c, std::memory_order_relaxed);
  mem.rcp.dpc_bufbusy.fetch_add(c, std::memory_order_relaxed);
}

// TMEM loads run on the RDP's 64-bit texture port: one GCLK per 8 bytes.
auto SoftRdp::accountTmem(Memory& mem, u64 bytes) -> void {
  u32 c = (u32)((bytes + 7) / 8);
  mem.rcp.dpc_tmem.fetch_add(c, std::memory_order_relaxed);
  mem.rcp.dpc_clock.fetch_add(c, std::memory_order_relaxed);
  mem.rcp.dpc_bufbusy.fetch_add(c, std::memory_order_relaxed);
}

// Per-pixel depth test against the 16-bit z image. Opaque z-mode: the pixel wins
// when its depth is nearer (strictly less) than the stored depth. On a pass with
// Z_UPDATE the new depth is written back. Returns whether the colour is drawn.
auto SoftRdp::depthTest(Memory& mem, int x, int y, s32 d) -> bool {
  auto& m = mem.rdram;
  if(d < 0) d = 0; else if(d > 0x3ffff) d = 0x3ffff;
  u32 zoff = zi_addr + (u32(y) * ci_width + u32(x)) * 2;
  if(zoff + 1 >= m.size()) return false;
  u32 old = zDecode(((u16)m[zoff] << 8) | m[zoff + 1]);
  if((other_lo & 0x10) && (u32)d >= old) return false;            // Z_CMP
  if(other_lo & 0x20) { wr16(m, zoff, zEncode((u32)d)); pxZWrites++; }  // Z_UPD
  return true;
}

auto SoftRdp::putPixel(Memory& mem, int x, int y, u32 rgba32) -> void {
  if(x < sx0 || x >= sx1 || y < sy0 || y >= sy1) return;
  if(x < 0 || y < 0) return;
  pxWrites++;         // DPC counters: this pixel reaches the color image
  auto& m = mem.rdram;
  if(ci_size == 3) {  // 32bpp RGBA8888
    wr32(m, ci_addr + (u32(y) * ci_width + u32(x)) * 4, rgba32);
  } else if(ci_size == 1) {   // 8bpp colour-index framebuffer: store the low byte
    wr8(m, ci_addr + (u32(y) * ci_width + u32(x)), (u8)(rgba32 & 0xff));
  } else {            // 16bpp RGBA5551
    wr16(m, ci_addr + (u32(y) * ci_width + u32(x)) * 2, to5551(rgba32));
  }
}

auto SoftRdp::readFb(Memory& mem, int x, int y) -> u32 {
  // Read the current framebuffer colour at (x,y) as RGBA32.
  const auto& m = mem.rdram;
  if(ci_size == 3) return rd32(m, ci_addr + (u32(y) * ci_width + u32(x)) * 4);
  if(ci_size == 1) {   // 8bpp CI: no readback semantics for blend; expose the raw index byte
    u32 a8 = ci_addr + (u32(y) * ci_width + u32(x));
    u8 v = a8 < m.size() ? m[a8] : 0;
    return ((u32)v << 24) | ((u32)v << 16) | ((u32)v << 8) | v;
  }
  u32 a = ci_addr + (u32(y) * ci_width + u32(x)) * 2;
  if(a + 1 >= m.size()) return 0;
  u16 px = ((u16)m[a] << 8) | m[a + 1];
  // The blender's memory colour is NOT expanded the way a texel is: the RDP feeds the
  // stored 5-bit channel into the 8-bit blend path with the low 3 bits ZERO, it does not
  // replicate (parallel-rdp decode_memory_color: FB_FMT_RGBA5551 -> `rgb & 0xf8`).
  // The difference is invisible without dither -- the writeback truncates back to 5 bits --
  // but dither rounds a channel UP whenever its low 3 bits beat the matrix threshold, so a
  // replicated readback (low bits 111) would bump every blended-through pixel one level.
  // krom's texture-rectangle suites are the witness: their transparent texels blend the
  // background straight through, and the hardware capture keeps the background level exactly.
  u32 r = ((px >> 11) & 0x1f) << 3, g = ((px >> 6) & 0x1f) << 3;
  u32 b = ((px >> 1) & 0x1f) << 3,  al = (px & 1) ? 255 : 0;
  return (r << 24) | (g << 16) | (b << 8) | al;
}

auto SoftRdp::blendColor(u32 src, u32 memc, bool blendEn) -> u32 {
  // Blend mux from the render-mode word (other_lo). 1-cycle mode evaluates the FIRST
  // blender cycle's config (GBL_c1: m1a<<30, m1b<<26, m2a<<22, m2b<<18); 2-cycle mode's
  // final write uses the SECOND cycle (GBL_c2: <<28/24/20/16). P/M pick a colour
  // (IN/MEM/BLEND/FOG), A picks a coefficient, B picks the second coefficient.
  int sh = (cycleType() == 1) ? 0 : 2;   // 2-cycle → shift down 2 to hit the cyc1 fields
  int Psel = (other_lo >> (28 + sh)) & 3, Asel = (other_lo >> (24 + sh)) & 3;
  int Msel = (other_lo >> (20 + sh)) & 3, Bsel = (other_lo >> (16 + sh)) & 3;
  auto pick = [&](int sel) -> u32 {   // P/M colour mux: IN / MEM / BLEND / FOG
    switch(sel) { case 0: return src; case 1: return memc; case 2: return blend_color; default: return fog_color; }
  };
  u32 P = pick(Psel), M = pick(Msel);
  int a0;                             // A mux: IN alpha / FOG alpha / SHADE alpha / 0
  switch(Asel) { case 0: a0 = src & 0xff; break; case 1: a0 = fog_color & 0xff; break;
                 case 2: a0 = src & 0xff; break; default: a0 = 0; }
  // Two hardware shortcuts that write the P colour untouched. Without them a "solid"
  // primitive picks up a 1/32 smear of M, because the coefficient path below is NOT an
  // exact lerp (see the 5-bit truncation).
  //  - blender disabled (no FORCE_BLEND and the pixel is not an AA edge): the blender is
  //    bypassed entirely, whatever the mux says;
  //  - the classic opaque case A=IN alpha, B=1-A, alpha==0xff.
  if(!blendEn || (Asel == 0 && Bsel == 0 && (src & 0xff) == 0xff)) return (P & ~0xffu) | (src & 0xff);
  int a1;                             // B mux: 1-A / MEM alpha / 1.0 / 0
  switch(Bsel) { case 0: a1 = (~a0) & 0xff; break; case 1: a1 = memc & 0xff; break;
                 case 2: a1 = 0xff; break; default: a1 = 0; }
  // The blender's coefficients are 5-bit, not 8: the RDP drops the low 3 bits of each
  // alpha and computes P*a0 + M*(a1+1) in that space. That truncation is visible — an
  // alpha of 0xf8..0xff all weigh the same — so scaling by /255 instead is wrong, and
  // there is no rounding-up of a0 either: 0xff weighs 31/32, not 32/32. Only the M term
  // gets the +1 (parallel-rdp blender(): `rgb0*a0 + rgb1*(a1+1)`), which is what makes
  // an additive pass with B = ONE carry the framebuffer through untouched while the
  // incoming colour still loses its 1/32.
  a0 >>= 3; a1 >>= 3;
  bool force = (other_lo >> 14) & 1;
  // FORCE_BLEND takes the plain >>5; otherwise the RDP runs the sum through its divider,
  // normalising by the actual coefficient weight (a0 + a1 + 1) rather than by a fixed 32.
  int sum = (a0 >> 2) + (a1 >> 2) + 1;
  auto ch = [](u32 c, int i) -> int { return (int)((c >> (24 - i * 8)) & 0xff); };
  u32 out = 0;
  for(int i = 0; i < 3; i++) {
    int blended = ch(P, i) * a0 + ch(M, i) * (a1 + 1);
    int v = force ? (blended >> 5) : (((blended >> 2) & 0x7ff) / sum);
    v = v < 0 ? 0 : v > 255 ? 255 : v;
    out |= (u32)v << (24 - i * 8);
  }
  return out | (src & 0xff);         // carry pipeline alpha (coverage) into the stored pixel
}

// RGB dither, applied to the blender output on its way to the colour image
// (SET_OTHER_MODES RGB_DITHER_SEL, bits 39:38 -> other_hi bits 7:6):
//   0 = magic square, 1 = standard Bayer, 2 = noise, 3 = off.
// The RDP does not add a signed offset: it rounds the channel UP to the next multiple
// of 8 when its low 3 bits exceed the matrix threshold, and leaves it alone otherwise
// (247 and above saturate to 255). That is why a dithered flat colour shows up in a
// hardware capture as two adjacent 5-bit levels in a 4x4 pattern rather than as noise.
// It runs regardless of the colour image's depth — on a 32bpp image the +8 survives
// verbatim, on a 16bpp one it decides which way the >>3 truncation goes.
auto SoftRdp::ditherRgb(int x, int y, u32 c) const -> u32 {
  static const u8 kMatrix[2][16] = {
    { 0, 6, 1, 7, 4, 2, 5, 3, 3, 5, 2, 4, 7, 1, 6, 0 },   // magic square
    { 0, 4, 1, 5, 4, 0, 5, 1, 3, 7, 2, 6, 7, 3, 6, 2 },   // standard Bayer
  };
  u32 mode = (other_hi >> 6) & 3;
  if(mode == 3) return c;
  u32 out = c & 0xff;                      // alpha/coverage untouched by RGB dither
  for(int i = 0; i < 3; i++) {
    int v = (int)((c >> (24 - i * 8)) & 0xff);
    int d;
    if(mode < 2) d = kMatrix[mode][(y & 3) * 4 + (x & 3)];
    else {
      // Noise dither. Hardware clocks an LFSR that no capture can be aligned to, so the
      // only reproducible choice is a per-pixel hash: same pixel, same value in every
      // run and in every thread configuration (the lockstep==threaded md5 gate depends
      // on the RDP being a pure function of the command stream).
      u32 h = (u32)x * 0x9e3779b1u ^ (u32)y * 0x85ebca6bu ^ (u32)i * 0xc2b2ae35u;
      h ^= h >> 15; h *= 0x2545f491u; h ^= h >> 13;
      d = (int)(h & 7);
    }
    if((v & 7) > d) v = v > 247 ? 255 : (v & 0xf8) + 8;
    out |= (u32)v << (24 - i * 8);
  }
  return out;
}

auto SoftRdp::blendPixel(Memory& mem, int x, int y, u32 src, bool aaEdge) -> void {
  if(x < sx0 || x >= sx1 || y < sy0 || y >= sy1 || x < 0 || y < 0) return;
  // The blender runs in every 1-/2-cycle primitive — it is not gated on IM_RD. IM_RD
  // (bit 0x40) only enables READS of the framebuffer, i.e. it matters solely when the mux
  // selects CLR_MEM (M) or MEM_alpha (B). When the blender references memory but reads are
  // disabled, hardware writes the pipeline colour straight through; otherwise the blender
  // evaluates with memc=0 (its memory inputs are never consulted). COPY/FILL bypass it.
  if(g_noBlend || cycleType() >= 2) { putPixel(mem, x, y, src); return; }   // FILL/COPY: no blender, no dither
  int sh = (cycleType() == 1) ? 0 : 2;
  int Msel = (other_lo >> (20 + sh)) & 3, Bsel = (other_lo >> (16 + sh)) & 3;
  bool usesMem = (Msel == 1) || (Bsel == 1);
  if(usesMem && !(other_lo & 0x40)) { putPixel(mem, x, y, ditherRgb(x, y, src)); return; }
  // blend_en = FORCE_BLEND || (ANTIALIAS_EN && the pixel is not fully covered). That is
  // the hardware rule verbatim ("if not force blend, allow blend enable - use CVG bits"):
  // with neither bit set the blender is bypassed and the P colour is written as-is.
  bool blendEn = ((other_lo >> 14) & 1) || (aaEdge && (other_lo & 0x08));
  u32 memc = usesMem ? readFb(mem, x, y) : 0;
  putPixel(mem, x, y, ditherRgb(x, y, blendColor(src, memc, blendEn)));
}

// Edge anti-aliasing. When AA_EN (other_lo bit 0x08) is set and a pixel is only
// partially covered by the primitive (cvg < 1), the RDP folds the pipeline colour
// against the framebuffer weighted by coverage: out = pipe*cvg + fb*(1-cvg). This is
// the coverage-based silhouette AA — the soft one-pixel edge on N64 polygons. Interior
// pixels (cvg>=1) take the normal blend/write path. `src` is the pipeline RGBA.
auto SoftRdp::coverPixel(Memory& mem, int x, int y, u32 src, double cvg) -> void {
  if(x < sx0 || x >= sx1 || y < sy0 || y >= sy1 || x < 0 || y < 0) return;
  if(g_noAA || !(other_lo & 0x08) || cvg >= 0.999) { blendPixel(mem, x, y, src); return; }
  if(cvg < 0.0) cvg = 0.0;
  u32 fb = readFb(mem, x, y);
  // Pipeline colour first through the blender (if IM_RD), then coverage-fold vs the
  // original framebuffer. On an edge the two references coincide closely enough. A
  // partially covered pixel is exactly the case ANTIALIAS_EN enables the blender for.
  u32 base = (other_lo & 0x40) ? blendColor(src, fb, true) : src;
  auto ch = [](u32 c, int i) -> int { return (int)((c >> (24 - i * 8)) & 0xff); };
  u32 out = 0;
  for(int i = 0; i < 3; i++) {
    int v = (int)(ch(base, i) * cvg + ch(fb, i) * (1.0 - cvg) + 0.5);
    v = v < 0 ? 0 : v > 255 ? 255 : v;
    out |= (u32)v << (24 - i * 8);
  }
  putPixel(mem, x, y, out | (src & 0xff));
}

auto SoftRdp::fillRect(Memory& mem, int x0, int y0, int x1, int y1) -> void {
  x0 = std::max(x0, sx0); y0 = std::max(y0, sy0);
  x1 = std::min(x1, sx1); y1 = std::min(y1, sy1);
  auto& m = mem.rdram;
  // FILL_RECTANGLE behaves per cycle type: in FILL (3) / COPY (2) modes it blasts the
  // packed fill colour straight to memory. In 1-/2-cycle modes the rect is a shadeless
  // primitive — each pixel runs the colour combiner (no texel/shade; constants like PRIM/
  // ENV/BLEND come through the mux) and then the blender against the framebuffer.
  bool pipeMode = cycleType() < 2;
  u64 npx = u64(std::max(0, x1 - x0)) * u64(std::max(0, y1 - y0));
  u64 w0 = pxWrites, z0 = pxZWrites;
  for(int y = y0; y < y1; y++) {
    for(int x = x0; x < x1; x++) {
      if(pipeMode) {
        // 1-/2-cycle FILL_RECTANGLE is a shadeless primitive: run the combiner (texel bus
        // all-ones for the missing texture, like a flat triangle) then the blender, which is
        // how the Krom fill demos paint solid blend_color rects. Never a raw fill_color here.
        bool combProg = (combine_hi | combine_lo) != 0;
        u32 flatTexel = combProg ? 0xffffffff : 0;
        u32 c = combProg ? combineColor(flatTexel, flatTexel, 0) : blend_color;
        // Alpha compare (1-/2-cycle form): COMBINED alpha against the blend_color
        // threshold. A fill rect is a primitive like any other here.
        if((other_lo & 1) && (c & 0xff) < (blend_color & 0xff)) continue;
        // Depth: a fill rect carries no z slope, so its only defined depth source is
        // SET_PRIM_DEPTH (Z_SOURCE_SEL, other_lo bit 2). Without that bit the span z
        // the rect never programs is undefined, so leave the z image alone.
        if((other_lo & 4) && zi_addr && (other_lo & 0x30) && !depthTest(mem, x, y, (s32)prim_z))
          continue;
        blendPixel(mem, x, y, c);
      } else if(ci_size == 3) {
        wr32(m, ci_addr + (u32(y) * ci_width + u32(x)) * 4, fill_color);
        pxWrites++;
      } else if(ci_size == 1) {
        // FILL cycle, 8bpp CI: the 32-bit fill color packs four 8bpp bytes; pick by x&3
        // (MSB-first byte order, matching the packed 16bpp pair layout above).
        u8 px = (u8)(fill_color >> (24 - (x & 3) * 8));
        wr8(m, ci_addr + (u32(y) * ci_width + u32(x)), px);
        pxWrites++;
      } else {
        // FILL cycle: the 32-bit fill color packs two 16bpp pixels; pick by x parity.
        u16 px = (x & 1) ? u16(fill_color & 0xffff) : u16(fill_color >> 16);
        wr16(m, ci_addr + (u32(y) * ci_width + u32(x)) * 2, px);
        pxWrites++;
      }
    }
  }
  accountPixels(mem, npx, pxWrites - w0, pxZWrites - z0);
}

// Flat/Gouraud triangle from RDP edge coefficients. First light: correct geometry,
// solid shade-base color (Gouraud/texture refined later).
auto SoftRdp::drawTriangle(Memory& mem, const u64* w, int words, u32 op) -> void {
  if(g_noRaster) return;
  bool hasShade = op & 4, hasTex = op & 2, hasZ = op & 1;
  u64 rasterPx = 0, accW0 = pxWrites, accZ0 = pxZWrites;   // DPC counter accounting
  u64 w0 = w[0];
  bool leftMajor = (w0 >> 55) & 1;
  double yl = sx((w0 >> 32) & 0x3fff, 14) / 4.0;   // bottom
  double ym = sx((w0 >> 16) & 0x3fff, 14) / 4.0;   // middle
  double yh = sx((w0 >> 0)  & 0x3fff, 14) / 4.0;   // top
  auto fx = [](u64 v) { return (s32)(u32)v / 65536.0; };
  double xl = fx(w[1] >> 32), dxldy = fx(w[1]);
  double xh = fx(w[2] >> 32), dxhdy = fx(w[2]);
  double xm = fx(w[3] >> 32), dxmdy = fx(w[3]);

  // Rendering mode. In FILL cycle a triangle is painted with the packed FILL_COLOR
  // pixel (the bare-metal "Fill_Triangle" path). Otherwise, if the command carries a
  // shade block, Gouraud-interpolate the per-vertex RGBA; else fall back to a flat
  // prim color. Texture triangles land with the textured pass (still flat here).
  bool fillMode = (cycleType() == 3);
  bool gouraud  = hasShade && words >= 12 && !fillMode;
  bool textured = hasTex && !fillMode;
  u32  texTile  = (u32)(w[0] >> 48) & 7;   // tile index lives in the first edge word
  auto& m = mem.rdram;

  // Z-buffer state (SET_OTHER_MODES low word). Depth compare/update gate on
  // Z_COMPARE_EN(0x10)/Z_UPDATE_EN(0x20); Z_SOURCE_SEL(0x04) takes the constant
  // primitive depth (SET_PRIM_DEPTH) instead of the interpolated per-pixel z.
  bool zCmp = (other_lo & 0x10) && zi_addr;
  bool zUpd = (other_lo & 0x20) && zi_addr;
  bool zSrc = other_lo & 0x04;
  bool zActive = (zCmp || zUpd);
  // Z coefficients (s16.16): block after edge(4)+shade(8)+tex(8). word0[63:32]=Z,
  // word0[31:0]=DzDx, word1[63:32]=DzDe (per +y along major edge), word1[31:0]=DzDy.
  double Zs = 0, dZdx = 0, dZde = 0;
  if(hasZ && !zSrc) {
    int zb = 4 + (hasShade ? 8 : 0) + (hasTex ? 8 : 0);
    if(zb + 1 < words) {
      // >>13, not >>16: the depth unit consumes bits 31:13 of the s15.16 attribute,
      // so the value it compares and stores is 18-bit with 3 fractional bits. Dropping
      // to whole units would throw away precision the z-buffer's float format keeps
      // (its far segments step by 1 in this domain) — see SET_PRIM_DEPTH below.
      Zs   = (double)(s32)(u32)(w[zb] >> 32)     / 8192.0;
      dZdx = (double)(s32)(u32) w[zb]            / 8192.0;
      dZde = (double)(s32)(u32)(w[zb + 1] >> 32) / 8192.0;
    }
  }

  // Texture coefficients (s10.5 → 1/32-texel units, split int-hi / frac-lo). Block
  // starts after edge(4) + shade(8 if present). Layout per +x/+e/+y like shade:
  //   start = j0(int)/j2(frac)   DsDx = j1/j3   DsDe = j4/j6 (per +y along edge).
  // S occupies bits [63:48], T [47:32], W [31:16] (perspective W ignored for now).
  double S = 0, T = 0, dSdx = 0, dTdx = 0, dSde = 0, dTde = 0;
  if(textured) {
    int tb = 4 + (hasShade ? 8 : 0);
    auto tc = [&](int ii, int fi, int comp) -> double {
      int sh = 48 - comp * 16;
      u32 i = (u32)(w[tb + ii] >> sh) & 0xffff, f = (u32)(w[tb + fi] >> sh) & 0xffff;
      return (double)(s32)((i << 16) | f) / 65536.0;   // value in 1/32-texel units
    };
    S = tc(0, 2, 0); T = tc(0, 2, 1);
    dSdx = tc(1, 3, 0); dTdx = tc(1, 3, 1);
    dSde = tc(4, 6, 0); dTde = tc(4, 6, 1);
  }

  // Shade coefficients (s16.16, split int-hi / frac-lo across two 64-bit words):
  //   start C  = w4(int) / w6(frac)      DcDx = w5 / w7  (per +x)
  //   DcDe     = w8(int) / w10(frac)     DcDy = w9 / w11 (per +y)
  // Component c: 0=R 1=G 2=B 3=A occupy bits [63:48],[47:32],[31:16],[15:0].
  double R = 0, G = 0, B = 0, A = 0, dRdx = 0, dGdx = 0, dBdx = 0, dAdx = 0;
  double dRde = 0, dGde = 0, dBde = 0, dAde = 0;
  // Flat (non-shade, non-texture) triangle colour. Without a full combiner/blender
  // model the constant source is a guess; use the register a demo most likely put it
  // in — prim, else blend, else env — before the neutral grey fallback. (Real games
  // route this through the combiner; that lands with combiner emulation later.)
  u32 flat = prim_color ? prim_color : blend_color ? blend_color : env_color ? env_color : 0xa0a0a0ff;
  bool combProg = (combine_hi | combine_lo) != 0;   // combiner programmed? else old heuristics
  if(gouraud) {
    auto comp = [](u64 hi, u64 lo, int c) -> double {
      int sh = 48 - c * 16;
      u32 i = (u32)(hi >> sh) & 0xffff, f = (u32)(lo >> sh) & 0xffff;
      return (double)(s32)((i << 16) | f) / 65536.0;   // s16.16 → color in 0..255
    };
    R = comp(w[4], w[6], 0); G = comp(w[4], w[6], 1); B = comp(w[4], w[6], 2); A = comp(w[4], w[6], 3);
    dRdx = comp(w[5], w[7], 0); dGdx = comp(w[5], w[7], 1); dBdx = comp(w[5], w[7], 2); dAdx = comp(w[5], w[7], 3);
    dRde = comp(w[8], w[10], 0); dGde = comp(w[8], w[10], 1); dBde = comp(w[8], w[10], 2); dAde = comp(w[8], w[10], 3);
  }
  auto clamp8 = [](double v) -> u32 { int i = (int)(v + 0.5); return (u32)(i < 0 ? 0 : i > 255 ? 255 : i); };
  // Texel the combiner sees on a flat (no-tex-coord) primitive. The RDP texel bus for a
  // primitive with no texture block presents all-ones (0xFFFFFFFF, opaque white): the Krom
  // "Fill Triangle" demos route this through the combiner alpha (TEX0_A * LOD_FRAC = 1) so
  // the blender's coverage weight is 1 and blend_color shows solid. If the combiner ignores
  // texel this is harmless.
  u32 flatTexel = (combProg && !textured && !gouraud && !fillMode) ? 0xffffffff : 0;
  if(std::getenv("KESTREL_TRIDBG")) {
    static int n = 0;
    if(n++ < 4) std::fprintf(stderr, "[tri] op=%02x cyc=%u shade=%d tex=%d comb=%06x/%08x\n"
                              "      c0R[a=%d b=%d c=%d d=%d] c0A[a=%d b=%d c=%d d=%d]\n"
                              "      c1R[a=%d b=%d c=%d d=%d] c1A[a=%d b=%d c=%d d=%d]\n"
                              "      prim=%08x env=%08x blend=%08x fog=%08x fill=%08x flatTexel=%08x olo=%08x ohi=%08x\n",
                              op, cycleType(), (int)hasShade, (int)hasTex, combine_hi, combine_lo,
                              comb[0].aR, comb[0].bR, comb[0].cR, comb[0].dR, comb[0].aA, comb[0].bA, comb[0].cA, comb[0].dA,
                              comb[1].aR, comb[1].bR, comb[1].cR, comb[1].dR, comb[1].aA, comb[1].bA, comb[1].cA, comb[1].dA,
                              prim_color, env_color, blend_color, fog_color, fill_color, flatTexel, other_lo, other_hi);
  }

  int yTop = (int)std::ceil(yh), yBot = (int)std::ceil(yl);
  yTop = std::max(yTop, sy0); yBot = std::min(yBot, sy1);
  for(int y = yTop; y < yBot; y++) {
    if(y < 0) continue;
    double xA = xh + dxhdy * (y - yh);                                   // major (H) edge
    double xB = (y < ym) ? xm + dxmdy * (y - yh) : xl + dxldy * (y - ym); // minor edge
    double xLeft  = leftMajor ? xA : xB;
    double xRight = leftMajor ? xB : xA;
    if(xLeft > xRight) std::swap(xLeft, xRight);
    int xs = std::max((int)std::ceil(xLeft), sx0);
    int xe = std::min((int)std::ceil(xRight), sx1);
    // Values at the major edge for this scanline (start advances by Dc/De per +y).
    double eR = R + dRde * (y - yh), eG = G + dGde * (y - yh);
    double eB = B + dBde * (y - yh), eA = A + dAde * (y - yh);
    double eS = S + dSde * (y - yh), eT = T + dTde * (y - yh);
    double eZ = Zs + dZde * (y - yh);
    // Per-pixel depth test against the 16-bit z image, honouring Z_SOURCE_SEL and
    // Z_UPDATE. Opaque z-mode: the pixel wins when its depth is nearer (strictly
    // less) than the stored depth. On a pass with Z_UPDATE the new depth is written
    // back. Returns whether the colour should be drawn.
    auto zPass = [&](int x, double dx) -> bool {
      if(!zActive) return true;
      return depthTest(mem, x, y, zSrc ? (s32)prim_z : (s32)std::lround(eZ + dZdx * dx));
    };
    // Sub-pixel coverage the way the RDP raster does it: instead of a single horizontal
    // box fraction, the primitive edges are evaluated at several sub-scanlines and
    // sub-columns inside each pixel and the covered subsamples are counted. That gives
    // graded coverage on NEAR-HORIZONTAL edges too (a pure horizontal box misses those)
    // and quantises to 3-bit steps like the hardware. 4 sub-scanlines × 2 sub-columns = 8.
    auto edgesAt = [&](double yy, double& L, double& R) {
      double a = xh + dxhdy * (yy - yh);
      double b = (yy < ym) ? xm + dxmdy * (yy - yh) : xl + dxldy * (yy - ym);
      L = leftMajor ? a : b; R = leftMajor ? b : a;
      if(L > R) std::swap(L, R);
    };
    static const double subY[4] = {0.125, 0.375, 0.625, 0.875};
    static const double subX[2] = {0.25, 0.75};
    // Las cuatro sub-scanlines no dependen de x: se evaluan UNA vez por linea, no por
    // pixel. Y con el mayor de los bordes izquierdos y el menor de los derechos se
    // reconoce el pixel enteramente dentro (cobertura 8/8) sin tocar los 8 subsamples,
    // que es el caso comun en el interior del triangulo. Mismos comparadores => mismo
    // recuento exacto que el bucle largo.
    double sL[4], sR[4], Lmax = -1e30, Rmin = 1e30;
    for(int sy = 0; sy < 4; sy++) {
      edgesAt((double)y + subY[sy], sL[sy], sR[sy]);
      if(sL[sy] > Lmax) Lmax = sL[sy];
      if(sR[sy] < Rmin) Rmin = sR[sy];
    }
    for(int x = xs; x < xe; x++) {
      if(x < 0) continue;
      rasterPx++;      // DPC counters: pixel entered the pipeline (may still be killed)
      int hits;
      if((double)x + subX[0] >= Lmax && (double)x + subX[1] < Rmin) hits = 8;
      else {
        hits = 0;
        for(int sy = 0; sy < 4; sy++)
          for(int sx = 0; sx < 2; sx++) {
            double px = (double)x + subX[sx];
            if(px >= sL[sy] && px < sR[sy]) hits++;
          }
      }
      double cvg = hits / 8.0;
      if(fillMode) {   // raw packed fill color straight to the color image (no AA)
        if(ci_size == 3) wr32(m, ci_addr + (u32(y) * ci_width + u32(x)) * 4, fill_color);
        else { u16 px = (x & 1) ? u16(fill_color & 0xffff) : u16(fill_color >> 16);
               wr16(m, ci_addr + (u32(y) * ci_width + u32(x)) * 2, px); }
        pxWrites++;
      } else if(textured) {
        double dx = x - xA;
        double su = (eS + dSdx * dx) / 32.0, tu = (eT + dTdx * dx) / 32.0;  // 1/32 → texel
        u32 tex = sampleTexFiltered(texTile, su, tu);
        // Shade (if the triangle carries a shade block) feeds the combiner alongside
        // the texel — this is how MODULATE (texel*shade) textures get their lighting.
        u32 shd = gouraud ? ((clamp8(eR + dRdx * dx) << 24) | (clamp8(eG + dGdx * dx) << 16)
                            | (clamp8(eB + dBdx * dx) << 8) | clamp8(eA + dAdx * dx)) : 0;
        bool copy = cycleType() == 2;
        u32 c = (combProg && !copy) ? combineColor(tex, tex, shd) : tex;
        // Alpha compare (see texRect): COPY mode keys on the 1-bit texel alpha (drop
        // alpha==0); 1-/2-cycle compares COMBINED alpha against the blend_color
        // threshold. Disabled → texel drawn regardless of its 5551 transparency bit.
        bool apass = !(other_lo & 1) ||
                     (copy ? (c & 0xff) != 0 : ((c & 0xff) >= (blend_color & 0xff)));
        if(apass && zPass(x, dx)) coverPixel(mem, x, y, c, cvg);
      } else if(gouraud) {
        double dx = x - xA;
        u32 shd = (clamp8(eR + dRdx * dx) << 24) | (clamp8(eG + dGdx * dx) << 16)
                | (clamp8(eB + dBdx * dx) << 8)  |  clamp8(eA + dAdx * dx);
        u32 c = combProg ? combineColor(0, 0, shd) : shd;
        if(zPass(x, dx)) coverPixel(mem, x, y, c, cvg);
      } else { double dx = x - xA;
        // Flat (no shade/tex coords) but the combiner may still select TEXEL0/1. The RDP
        // has no per-vertex S/T here, so it samples the current tile at its origin (0,0) —
        // a solid-fill triangle that routes a loaded texel through the combiner (common in
        // the Krom fill tests) picks up that texel. If the combiner ignores texel this is
        // harmless. Sampled once (constant across the primitive).
        u32 c = combProg ? combineColor(flatTexel, flatTexel, 0) : flat;
        if(zPass(x, dx)) coverPixel(mem, x, y, c, cvg); }
    }
  }
  accountPixels(mem, rasterPx, pxWrites - accW0, pxZWrites - accZ0);
}

auto SoftRdp::sampleTexel(u32 tileIdx, int s, int t) -> u32 {
  // Point-sample one texel out of TMEM for `tileIdx`. Coordinates are integer
  // texels (the caller already did S>>5 etc). Wrap by mask when the tile carries
  // one, else clamp to the SET_TILE_SIZE box. Decodes the common RGBA16/IA16/
  // I8/IA8/RGBA32 formats; unknowns fall back to opaque white.
  const Tile& tl = tiles[tileIdx & 7];
  // Tile SHIFT: scales the incoming texel coordinate (shift 1..10 → >>, 11..15 → <<).
  auto applyShift = [](int c, u32 sh) -> int {
    if(!sh) return c;
    return sh <= 10 ? (c >> sh) : (c << (16 - sh));
  };
  s = applyShift(s, tl.shiftS); t = applyShift(t, tl.shiftT);
  int sMax = (int)(tl.sh >> 2) - (int)(tl.sl >> 2);
  int tMax = (int)(tl.th >> 2) - (int)(tl.tl >> 2);
  // Wrap/mirror/clamp per axis (HW-accurate, two stages like the RDP sampler).
  // cmN bit0 = mirror, bit1 = clamp. Stage 1 (clamp): when the clamp bit is set OR
  // there is no mask, the coordinate is clamped to the SET_TILE_SIZE box [0,lim];
  // this is the ONLY place negatives are pinned. Stage 2 (mask): when maskN != 0,
  // fold the coordinate into a 2^mask period — mirror flips it (bitwise ~c) on odd
  // periods. Both stages run in two's complement so negative coords (the roms start
  // S/T below zero, e.g. -14) mirror/wrap exactly as on hardware instead of collapsing
  // to texel 0. Note: no premature `c<0 → 0` before masking.
  // Texel address folding — matches angrylion's tcclamp→tcmask order (the two stages
  // run in sequence, NOT mutually exclusive). Stage 1 (clamp): when the clamp bit is
  // set OR there is no mask, pin the coordinate to the tile-size box [0,lim]. Stage 2
  // (mask): when maskN != 0, mirror on odd 2^mask periods then fold into 2^mask. With
  // clamp+mask both active and 2^mask > lim the mask is idempotent (clamp stays
  // visible); with 2^mask <= lim it re-wraps — exactly as hardware does. Both stages
  // are two's-complement so negative coords (roms start S/T at -14) fold correctly.
  auto wrap = [](int c, u32 mask, u32 cm, int lim) -> int {
    if((cm & 2) || mask == 0) {                     // clamp stage
      if(c < 0) c = 0;
      else if(lim >= 0 && c > lim) c = lim;
    }
    if(mask) {                                       // mask stage (wrap + mirror)
      if((cm & 1) && ((c >> (int)mask) & 1)) c = ~c; // mirror: flip on odd period
      c &= (1 << mask) - 1;
    }
    return c;
  };
  s = wrap(s, tl.maskS, tl.cmS, sMax);
  t = wrap(t, tl.maskT, tl.cmT, tMax);
  u32 rowBytes = tl.line * 8;
  u32 base = tl.tmem * 8;
  // Decode one palette entry (CI formats). TEXTLUT mode picks RGBA5551 vs IA16.
  auto tlutLookup = [&](u32 idx) -> u32 {
    u16 e = tlut[idx & 0xff];
    if(tlutMode() == 3) { u32 i = (e >> 8) & 0xff, a = e & 0xff;   // IA16 palette
                          return (i << 24) | (i << 16) | (i << 8) | a; }
    // A palette entry expands exactly like a texel: 5 bits replicated into 8
    // (v<<3 | v>>2), as parallel-rdp does on both read paths. krom's GRB decoders pin
    // this down: their palettes hold the odd 5-bit values 1,3,..,31 and the hardware
    // captures only reproduce once the entry is replicated and then scaled by the
    // LOD_FRAC(0xff)→COMBINED_ALPHA and 31/32 blender paths — zero-filling the entry
    // cannot reach those levels for any choice of the two scales.
    u32 r = exp5((e >> 11) & 0x1f), g = exp5((e >> 6) & 0x1f);
    u32 b = exp5((e >> 1)  & 0x1f), a = (e & 1) ? 255 : 0;   // RGBA5551 palette
    return (r << 24) | (g << 16) | (b << 8) | a;
  };
  if(tl.size == 0) {                                   // 4-bit texels (CI4 / IA4 / I4)
    u32 off = base + (u32)t * rowBytes + (u32)s / 2;
    if(off >= 0x1000) return 0;
    u8 nib = (s & 1) ? (tmem[off] & 0xf) : (tmem[off] >> 4);
    if(tl.fmt == 2) return tlutLookup(tl.palette * 16 + nib);        // CI4 → 16-entry sub-palette
    if(tl.fmt == 3) { u32 i = (((nib >> 1) & 7) * 255) / 7, a = (nib & 1) ? 255 : 0;  // IA4 (3I/1A)
                      return (i << 24) | (i << 16) | (i << 8) | a; }
    u32 i = nib * 17;                        // I4 → intensity replicated to R,G,B AND alpha
    return (i << 24) | (i << 16) | (i << 8) | i;   // HW: I formats set alpha = intensity
  }
  if(tl.size == 2) {                                   // 16-bit texels
    u32 off = base + (u32)t * rowBytes + (u32)s * 2;
    if(off + 1 >= 0x1000) return 0;
    if(tl.fmt == 1) {                                  // YUV 4:2:2 (UYVY pairs) → RGB via SET_CONVERT
      // TMEM layout per 32-bit texel pair [U, Y0, V, Y1]: luma at the odd byte of each
      // texel, chroma at the even bytes and shared across the pair (4:2:2). Convert with
      // the SET_CONVERT coefficients: R=Y+K0*V, G=Y+K1*U+K2*V, B=Y+K3*U (V,U signed about
      // 128, K/128 scale, HW rounds the >>7).
      u32 pairBase = base + (u32)t * rowBytes + (s & ~1u) * 2;
      if(pairBase + 2 >= 0x1000 || off + 1 >= 0x1000) return 0;
      int Y = tmem[off + 1], dU = (int)tmem[pairBase] - 128, dV = (int)tmem[pairBase + 2] - 128;
      auto cl = [](int v) -> u32 { return (u32)(v < 0 ? 0 : v > 255 ? 255 : v); };
      u32 r = cl(Y + ((k0 * dV + 0x40) >> 7));
      u32 g = cl(Y + ((k1 * dU + k2 * dV + 0x40) >> 7));
      u32 b = cl(Y + ((k3 * dU + 0x40) >> 7));
      return (r << 24) | (g << 16) | (b << 8) | 0xff;   // opaque
    }
    u16 px = ((u16)tmem[off] << 8) | tmem[off + 1];
    if(tl.fmt == 3) { u32 i = (px >> 8) & 0xff, a = px & 0xff;   // IA16
                      return (i << 24) | (i << 16) | (i << 8) | a; }
    u32 r = exp5((px >> 11) & 0x1f);          // RGBA5551 (default)
    u32 g = exp5((px >> 6)  & 0x1f);
    u32 b = exp5((px >> 1)  & 0x1f);
    u32 a = (px & 1) ? 255 : 0;
    return (r << 24) | (g << 16) | (b << 8) | a;
  }
  if(tl.size == 3) {                                   // 32-bit RGBA8888
    u32 off = base + (u32)t * rowBytes + (u32)s * 4;
    if(off + 3 >= 0x1000) return 0;
    return ((u32)tmem[off] << 24) | ((u32)tmem[off+1] << 16) | ((u32)tmem[off+2] << 8) | tmem[off+3];
  }
  if(tl.size == 1) {                                   // 8-bit I8 / IA8
    u32 off = base + (u32)t * rowBytes + (u32)s;
    if(off >= 0x1000) return 0;
    u8 v = tmem[off];
    if(tl.fmt == 2) return tlutLookup(v);                        // CI8 → full 256-entry palette
    if(tl.fmt == 3) { u32 i = (v >> 4) * 17, a = (v & 0xf) * 17;  // IA8 (4/4)
                      return (i << 24) | (i << 16) | (i << 8) | a; }
    return ((u32)v << 24) | ((u32)v << 16) | ((u32)v << 8) | v;   // I8 → grey opaque
  }
  return 0xffffffff;
}

auto SoftRdp::sampleRawIndex(u32 tileIdx, int s, int t) -> int {
  // Return the RAW colour-index texel (pre-TLUT) for CI formats, applying the same
  // SHIFT + clamp/mask/mirror addressing as sampleTexel. Used by copy-mode blits into
  // an 8bpp colour-index framebuffer, where hardware stores the index byte verbatim
  // (the "internal palette": the framebuffer holds indices, VI displays them raw).
  const Tile& tl = tiles[tileIdx & 7];
  auto applyShift = [](int c, u32 sh) -> int {
    if(!sh) return c;
    return sh <= 10 ? (c >> sh) : (c << (16 - sh));
  };
  s = applyShift(s, tl.shiftS); t = applyShift(t, tl.shiftT);
  int sMax = (int)(tl.sh >> 2) - (int)(tl.sl >> 2);
  int tMax = (int)(tl.th >> 2) - (int)(tl.tl >> 2);
  auto wrap = [](int c, u32 mask, u32 cm, int lim) -> int {   // clamp→mask sequential (see sampleTexel)
    if((cm & 2) || mask == 0) { if(c < 0) c = 0; else if(lim >= 0 && c > lim) c = lim; }
    if(mask) { if((cm & 1) && ((c >> (int)mask) & 1)) c = ~c; c &= (1 << mask) - 1; }
    return c;
  };
  s = wrap(s, tl.maskS, tl.cmS, sMax);
  t = wrap(t, tl.maskT, tl.cmT, tMax);
  u32 rowBytes = tl.line * 8, base = tl.tmem * 8;
  if(tl.size == 0) {                                    // CI4 (4-bit index)
    u32 off = base + (u32)t * rowBytes + (u32)s / 2;
    if(off >= 0x1000) return 0;
    u8 nib = (s & 1) ? (tmem[off] & 0xf) : (tmem[off] >> 4);
    return tl.palette * 16 + nib;
  }
  u32 off = base + (u32)t * rowBytes + (u32)s;          // CI8 (8-bit index)
  return off < 0x1000 ? tmem[off] : 0;
}

auto SoftRdp::sampleTexFiltered(u32 tile, double s, double t) -> u32 {
  // Point sample unless SAMPLE_TYPE (other_hi bit 13 = full mode bit 45) selects the
  // N64's 3-point ("bilinear") filter. The RDP is not a 4-tap bilinear: it picks the
  // triangle of texels around the sample and lerps by the fractional coords. No -0.5
  // GL-style bias: the RDP addresses texel origins directly, sfrac/tfrac are the low
  // fractional bits of the S/T coordinate.
  if(g_noFilter || !((other_hi >> 13) & 1))
    return sampleTexel(tile, (int)std::floor(s), (int)std::floor(t));
  // Hardware works in 10.5 fixed point and lerps in integers, so do the same: a double
  // lerp rounded at the end lands on a different level whenever the exact result sits on
  // a .5 boundary, and the 5-bit framebuffer then quantizes that difference into a
  // visible band. Weights are the 5-bit S/T fractions, the rounding is +0x10 before an
  // arithmetic >>5 (so a negative slope truncates toward -inf, as on HW).
  int si = (int)std::lround(s * 32.0), ti = (int)std::lround(t * 32.0);
  int fx = si & 31, fy = ti & 31;
  int s0 = si >> 5, t0 = ti >> 5;
  auto ch = [](u32 c, int i) -> int { return (int)((c >> (24 - i * 8)) & 0xff); };
  u32 c10 = sampleTexel(tile, s0 + 1, t0), c01 = sampleTexel(tile, s0, t0 + 1);
  int o[4];
  if(((other_hi >> 12) & 1) && fx == 16 && fy == 16) {
    // MID_TEXEL: at the exact centre of the quad the filter degenerates to the average
    // of all four texels (MPEG half-pel motion compensation).
    u32 c00 = sampleTexel(tile, s0, t0), c11 = sampleTexel(tile, s0 + 1, t0 + 1);
    for(int i = 0; i < 4; i++)
      o[i] = (ch(c00, i) + ch(c10, i) + ch(c01, i) + ch(c11, i) + 2) >> 2;
  } else {
    // The RDP is a 3-tap filter, not a 4-tap bilinear: it takes the triangle half the
    // sample falls in. Past the diagonal the base flips to the opposite corner and the
    // weights flip with it (and swap axes).
    bool upper = fx + fy >= 32;
    u32 base = upper ? sampleTexel(tile, s0 + 1, t0 + 1) : sampleTexel(tile, s0, t0);
    int wx = upper ? 32 - fy : fx, wy = upper ? 32 - fx : fy;
    for(int i = 0; i < 4; i++) {
      int b = ch(base, i);
      o[i] = (((ch(c10, i) - b) * wx + (ch(c01, i) - b) * wy + 0x10) >> 5) + b;
    }
  }
  u32 r = 0;
  for(int i = 0; i < 4; i++) {
    int v = o[i] < 0 ? 0 : o[i] > 255 ? 255 : o[i];
    r |= (u32)v << (24 - i * 8);
  }
  return r;
}

auto SoftRdp::combineColor(u32 tex0, u32 tex1, u32 shade) -> u32 {
  // N64 color combiner — modelled on parallel-rdp (HW-exact). The RDP works in a 9-bit
  // signed fixed-point space where 0x100 == 1.0, NOT 255. Per channel the equation is
  //   out = (((A - B) * C + 0x80) >> 8) + D
  // with A/B/D pushed through `special_expand` (a 9-bit sign-extend that is the identity
  // for ordinary 0..255 inputs but bites on out-of-range COMBINED feedback) and C
  // sign-extended to 9 bits. Crucially there is NO per-cycle [0,255] clamp: the cycle-0
  // result is fed to cycle 1 RAW (may be negative or >255), and only the FINAL cycle is
  // folded into [0,255] by clamp_9bit_notrunc — that fold is where the HW overflow quirk
  // lives (a sum that wraps past +256 comes back negative → 0 instead of saturating to
  // white; the [-129,-256] band clamps to 255). 1-cycle mode evaluates comb[1].
  auto ch = [](u32 c, int i) -> int { return (int)((c >> (24 - i * 8)) & 0xff); };  // i: 0=R 1=G 2=B 3=A
  // special_expand: (v-0x80) kept as signed 9-bit, then +0x80. Identity on 0..255.
  auto sexp = [](int v) -> int { int x = (v - 0x80) & 0x1ff; if(x & 0x100) x |= ~0x1ff; return x + 0x80; };
  // The multiplier port is 9 bits wide and its sources are already in that domain:
  // 0..0xff colours, 0x100 for the "one" sources (LOD_FRAC with no mipmap, an expanded
  // 0xff alpha), and the raw signed result of cycle 0. Sign-extending the field would
  // read 0x100 as -256 and negate every product that a "one" multiplies — the reference
  // images say x*ONE == x, so the port is not re-interpreted here.
  // clamp_9bit_notrunc: fold a 9-bit-wrapped value into [0,255].
  auto clampN = [&](int v) -> int { int x = sexp(v); return x < 0 ? 0 : x > 255 ? 255 : x; };
  // RGB source resolvers. `cin` is the previous cycle's raw RGBA (all 0 on the first cycle).
  auto srcA = [&](int idx, int i, const int* cin) -> int {   // sub_a (0..15); 6=ONE(0x100),7=NOISE
    switch(idx) { case 0: return cin[i]; case 1: return ch(tex0, i); case 2: return ch(tex1, i);
      case 3: return ch(prim_color, i); case 4: return ch(shade, i); case 5: return ch(env_color, i);
      case 6: return 0x100; case 7: return std::rand() & 0xff; default: return 0; }
  };
  auto srcB = [&](int idx, int i, const int* cin) -> int {   // sub_b (0..15); 6=key center,7=K4 → 0
    switch(idx) { case 0: return cin[i]; case 1: return ch(tex0, i); case 2: return ch(tex1, i);
      case 3: return ch(prim_color, i); case 4: return ch(shade, i); case 5: return ch(env_color, i);
      default: return 0; }
  };
  auto srcC = [&](int idx, int i, const int* cin) -> int {   // mul (0..31)
    switch(idx) { case 0: return cin[i]; case 1: return ch(tex0, i); case 2: return ch(tex1, i);
      case 3: return ch(prim_color, i); case 4: return ch(shade, i); case 5: return ch(env_color, i);
      case 7: return cin[3]; case 8: return ch(tex0, 3); case 9: return ch(tex1, 3);
      case 10: return ch(prim_color, 3); case 11: return ch(shade, 3); case 12: return ch(env_color, 3);
      case 13: return lodFrac(); case 14: return (int)prim_lod_frac;
      default: return 0; }   // 6 key-scale, 15 K5 → 0
  };
  auto srcD = [&](int idx, int i, const int* cin) -> int {   // add (0..7); 6=ONE(0x100)
    switch(idx) { case 0: return cin[i]; case 1: return ch(tex0, i); case 2: return ch(tex1, i);
      case 3: return ch(prim_color, i); case 4: return ch(shade, i); case 5: return ch(env_color, i);
      case 6: return 0x100; default: return 0; }
  };
  // Alpha source resolvers (scalar). sub_a/b/d share a table; mul has its own.
  auto aABD = [&](int idx, const int* cin) -> int {          // alpha sub_a / sub_b / add (0..7); 6=ONE
    switch(idx) { case 0: return cin[3]; case 1: return ch(tex0, 3); case 2: return ch(tex1, 3);
      case 3: return ch(prim_color, 3); case 4: return ch(shade, 3); case 5: return ch(env_color, 3);
      case 6: return 0x100; default: return 0; }
  };
  auto aC = [&](int idx, const int* cin) -> int {            // alpha mul (0..7)
    switch(idx) { case 0: return lodFrac();
      case 1: return ch(tex0, 3); case 2: return ch(tex1, 3); case 3: return ch(prim_color, 3);
      case 4: return ch(shade, 3); case 5: return ch(env_color, 3);
      case 6: return (int)prim_lod_frac; default: return 0; }   // 7=0
  };
  auto eq = [&](int a, int b, int c, int d) -> int {        // 9-bit combiner equation (raw, unclamped)
    a = sexp(a); b = sexp(b); d = sexp(d);
    return (((a - b) * c + 0x80) >> 8) + d;
  };
  // Run one combiner cycle, producing a RAW (unclamped) RGBA in `out`.
  auto oneCycle = [&](const CombSet& cs, const int* cin, int* out) -> void {
    out[0] = eq(srcA(cs.aR, 0, cin), srcB(cs.bR, 0, cin), srcC(cs.cR, 0, cin), srcD(cs.dR, 0, cin));
    out[1] = eq(srcA(cs.aR, 1, cin), srcB(cs.bR, 1, cin), srcC(cs.cR, 1, cin), srcD(cs.dR, 1, cin));
    out[2] = eq(srcA(cs.aR, 2, cin), srcB(cs.bR, 2, cin), srcC(cs.cR, 2, cin), srcD(cs.dR, 2, cin));
    out[3] = eq(aABD(cs.aA, cin), aABD(cs.bA, cin), aC(cs.cA, cin), aABD(cs.dA, cin));
  };
  // COMBINED is a pipeline register, not a per-pixel zero: on the first cycle it still
  // holds the previous pixel's result. That is not a corner case, it is how a 1-cycle
  // combiner reads COMBINED_ALPHA at all (krom's video decoders multiply TEXEL0 by it,
  // which is 1.0 in steady state and would be black if COMBINED were forced to zero).
  int mid[4], fin[4];
  if(cycleType() == 1) {                 // 2-cycle: cycle0 (raw) → COMBINED → cycle1
    oneCycle(comb[0], combined, mid);
    oneCycle(comb[1], mid, fin);
  } else {                               // 1-cycle uses the second-cycle equation
    oneCycle(comb[1], combined, fin);
  }
  for(int i = 0; i < 4; i++) combined[i] = clampN(fin[i]);
  u32 rgba = ((u32)combined[0] << 24) | ((u32)combined[1] << 16)
           | ((u32)combined[2] << 8)  |  (u32)combined[3];
  // The latched alpha is the EXPANDED one: 0xff becomes 0x100 so that a downstream
  // multiply by it is an exact identity instead of x*255/256, which would shave a level
  // off every pass. Same expansion the blender applies to its coefficient.
  combined[3] += (combined[3] + 1) >> 8;
  return rgba;
}

auto SoftRdp::texRect(Memory& mem, const u64* w, bool flip) -> void {
  if(g_noRaster) return;
  // TEXTURE_RECTANGLE: sample `tile` across a screen rect. word0 = XL,YL(10.2),
  // tile, XH,YH(10.2) [XH/YH top-left, XL/YL bottom-right]. word1 = S,T (s10.5,
  // 1/32-texel) and DsDx,DtDy (s5.10, 1/1024 texel-per-pixel). S starts at XH and
  // steps by DsDx per pixel-x; T at YH steps DtDy per pixel-y. FLIP swaps the S/T
  // axes (S follows y, T follows x). COPY-cycle mode blits a texel per pixel.
  u64 c0 = w[0], c1 = w[1];
  double xl = ((c0 >> 44) & 0xfff) / 4.0, yl = ((c0 >> 32) & 0xfff) / 4.0;
  u32    tile = (u32)(c0 >> 24) & 7;
  double xh = ((c0 >> 12) & 0xfff) / 4.0, yh = (c0 & 0xfff) / 4.0;
  double s0 = (s16)((c1 >> 48) & 0xffff) / 32.0;
  double t0 = (s16)((c1 >> 32) & 0xffff) / 32.0;
  double dsdx = (s16)((c1 >> 16) & 0xffff) / 1024.0;
  double dtdy = (s16)(c1 & 0xffff) / 1024.0;
  // COPY cycle blits 4 pixels per RDP clock, so the horizontal S step is expressed at 4×
  // scale: DsDx = 4<<10 (4.0) means a 1:1 texel-per-pixel copy. Divide by 4 to recover the
  // true per-pixel texel advance. (Vertical DtDy is one line per pass — unscaled.)
  if(cycleType() == 2) dsdx /= 4.0;
  int X0 = std::max((int)std::ceil(xh), sx0), X1 = std::min((int)std::ceil(xl), sx1);
  int Y0 = std::max((int)std::ceil(yh), sy0), Y1 = std::min((int)std::ceil(yl), sy1);
  u64 rasterPx = 0, accW0 = pxWrites, accZ0 = pxZWrites;   // DPC counter accounting
  for(int y = Y0; y < Y1; y++) {
    if(y < 0) continue;
    for(int x = X0; x < X1; x++) {
      if(x < 0) continue;
      rasterPx++;
      double fx = x - xh, fy = y - yh;
      double s = flip ? s0 + dsdx * fy : s0 + dsdx * fx;
      double t = flip ? t0 + dtdy * fx : t0 + dtdy * fy;
      u32 tex = sampleTexFiltered(tile, s, t);   // point or N64 3-point per SAMPLE_TYPE
      // COPY cycle writes the raw texel; 1-/2-cycle route it through the combiner
      // (texrect carries no shade → SHADE input is 0).
      bool combProg = (combine_hi | combine_lo) != 0;
      bool copy = cycleType() == 2;
      // COPY into an 8bpp colour-index framebuffer: the RDP blits the raw texel INDEX
      // (not the TLUT colour) — the framebuffer stores indices, VI shows them ("internal
      // palette"). EN_TLUT + ALPHA_COMPARE still keys out transparent entries: the TLUT
      // alpha of the index (carried in `tex`) drives the 1-bit copy-mode key.
      if(copy && ci_size == 1) {
        // COPY into an 8bpp colour-index framebuffer. The RDP does NOT run the fetched
        // index through the TLUT for the stored value — the framebuffer keeps raw indices
        // ("internal palette", VI shows them). When ALPHA_COMPARE_EN is set the copy-mode
        // 1-bit key uses the INDEX itself as coverage (index 0 = transparent), NOT the TLUT
        // alpha: this demo's TLUT entries (idx<<8) all have LSB 0, yet hardware draws every
        // non-zero index — so the key is index==0, the coverage carried by the CI value.
        int idx = sampleRawIndex(tile, (int)std::floor(s), (int)std::floor(t));
        if((other_lo & 1) && idx == 0) continue;   // transparent index → skip
        putPixel(mem, x, y, (u32)idx);             // 8bpp path stores low byte = index
        continue;
      }
      u32 c = (combProg && !copy) ? combineColor(tex, tex, 0) : tex;
      // Alpha compare (ALPHA_COMPARE_EN, other_lo bit 0) has two HW forms:
      //  - COPY mode: 1-bit transparency — the texel is discarded when its alpha is 0
      //    (e.g. TLUT index → $0000). This is the classic copy-mode sprite key.
      //  - 1-/2-cycle: the COMBINED alpha is compared against the blend_color threshold
      //    (dither approximated by the same compare); fail when alpha < threshold.
      // Disabled → texel written regardless of its 5551 transparency bit (opaque mode).
      // (The old `tex & 0xff` gate wrongly dropped alpha-0 texels in every mode.)
      if(other_lo & 1) {
        if(copy ? (c & 0xff) == 0 : (c & 0xff) < (blend_color & 0xff)) continue;
      }
      if(other_lo & 0x40) blendPixel(mem, x, y, c);      // blend against framebuffer
      else putPixel(mem, x, y, c);                        // opaque write
    }
  }
  accountPixels(mem, rasterPx, pxWrites - accW0, pxZWrites - accZ0);
}

auto SoftRdp::loadTile(Memory& mem, u32 t, bool block, u64 cmd) -> void {
  // Copy texels from the texture image (RDRAM, ti_addr/ti_width/ti_size) into TMEM
  // at the tile's base. LOAD_TILE walks a [SL,TL]..[SH,TH] rectangle (fields in
  // 10.2); LOAD_BLOCK copies a contiguous run (SL..SH linear texels — no DxT
  // deswizzle yet, fine for the load-once test roms). 4-bit texels not handled.
  Tile& tl = tiles[t & 7];
  const auto& m = mem.rdram;
  u32 bpt = ti_size == 3 ? 4 : ti_size == 2 ? 2 : ti_size == 1 ? 1 : 0;   // bytes/texel (0 = 4-bit)
  u32 sl = (u32)((cmd >> 44) & 0xfff), tlo = (u32)((cmd >> 32) & 0xfff);
  u32 sh = (u32)((cmd >> 12) & 0xfff), th = (u32)((cmd >> 0) & 0xfff);
  // 4-bit texels (CI4/IA4/I4) pack 2/byte. LOAD_BLOCK is a linear byte copy of
  // ceil(texels/2) bytes; the SL/SH texel indices halve to byte offsets.
  if(!bpt) {
    tl.sl = sl; tl.tl = tlo; tl.sh = sh; tl.th = th;
    if(block) {
      u32 count = (sh >= sl) ? (sh - sl + 1) : 1, nb = (count + 1) / 2;
      u32 dst = tl.tmem * 8, src = ti_addr + (sl >> 1);
      for(u32 i = 0; i < nb && dst + i < 0x1000 && src + i < m.size(); i++) tmem[dst + i] = m[src + i];
      accountTmem(mem, nb);
    } else {
      u32 s0 = sl >> 2, t0 = tlo >> 2, s1 = sh >> 2, t1 = th >> 2, rowBytes = tl.line * 8;
      accountTmem(mem, u64(t1 - t0 + 1) * (s1 - s0 + 1) / 2);
      for(u32 ty = t0; ty <= t1; ty++)
        for(u32 tx = s0; tx <= s1; tx++) {         // nibble-granular copy
          u32 src = ti_addr + (ty * ti_width + tx) / 2;   // ti_width in texels
          u32 dstByte = tl.tmem * 8 + (ty - t0) * rowBytes + (tx - s0) / 2;
          if(dstByte < 0x1000 && src < m.size()) {
            u8 nib = (tx & 1) ? (m[src] & 0xf) : (m[src] >> 4);
            if((tx - s0) & 1) tmem[dstByte] = (tmem[dstByte] & 0xf0) | nib;
            else              tmem[dstByte] = (tmem[dstByte] & 0x0f) | (nib << 4);
          }
        }
    }
    return;
  }
  // On real HW LOAD_TILE/LOAD_BLOCK also latch the tile's clamp box (same SL/TL/
  // SH/TH registers SET_TILE_SIZE writes). Roms that skip SET_TILE_SIZE rely on
  // this — without it the sampler clamps every texel to (0,0).
  tl.sl = sl; tl.tl = tlo; tl.sh = sh; tl.th = th;
  if(block) {
    u32 count = (sh >= sl) ? (sh - sl + 1) : 1;        // linear texel count
    u32 dst = tl.tmem * 8, src = ti_addr + sl * bpt, nb = count * bpt;
    for(u32 i = 0; i < nb && dst + i < 0x1000 && src + i < m.size(); i++) tmem[dst + i] = m[src + i];
    accountTmem(mem, nb);
    return;
  }
  u32 s0 = sl >> 2, t0 = tlo >> 2, s1 = sh >> 2, t1 = th >> 2;
  u32 rowBytes = tl.line * 8;
  accountTmem(mem, u64(t1 - t0 + 1) * (s1 - s0 + 1) * bpt);
  for(u32 ty = t0; ty <= t1; ty++)
    for(u32 tx = s0; tx <= s1; tx++) {
      u32 src = ti_addr + (ty * ti_width + tx) * bpt;
      u32 dst = tl.tmem * 8 + (ty - t0) * rowBytes + (tx - s0) * bpt;
      for(u32 b = 0; b < bpt && dst + b < 0x1000 && src + b < m.size(); b++) tmem[dst + b] = m[src + b];
    }
}

auto SoftRdp::run(Memory& mem, u32 start, u32 end, bool xbus) -> u32 {
  const auto& m = mem.rdram;
  const auto& dm = mem.dmem;
  // Command fetch source: RDRAM by physical address, or DMEM (12-bit wrap) in xbus
  // mode. Big-endian 64-bit word either way. Pixel writes still target RDRAM.
  auto fetch = [&](u32 a) -> u64 {
    if(!xbus) return rd64(m, a);
    u64 v = 0;
    for(int i = 0; i < 8; i++) v = (v << 8) | dm[(a + i) & 0xfff];
    return v;
  };
  u32 cur, end2;
  if(xbus) { cur = start & 0xfff'ffff; end2 = end & 0xfff'ffff; }  // DMEM offsets: keep the
  else     { cur = start & 0x00ff'ffff; end2 = end & 0x00ff'ffff; }  // overflow range unmasked
  end = end2;
  u32 executed = 0;
  u64 words[24];
  int guard = 0;
  sawSyncFull = false;
  static bool lowCi = false; static u64 lowMark = 0;
  static const bool citrace = std::getenv("KESTREL_CITRACE") != nullptr;
  static bool ops = std::getenv("KESTREL_RDPOPS") != nullptr;
  static u32 hist[64] = {};
  while(cur < end && guard++ < 200000) {
    // Publicar el puntero de lectura ANTES de consumir el comando: el productor debe
    // ver como ocupado todo lo que aun no se ha leido.
    if(curOut) curOut->store(cur, std::memory_order_release);
    u64 cmd = fetch(cur);
    u32 op = (cmd >> 56) & 0x3f;
    if(ops) {
      hist[op]++;
      // alert on the first time we ever see a color-image set or a triangle
      static bool sawCimg = false, sawTri = false, sawFill = false;
      if(op == 0x3f && !sawCimg) { sawCimg = true; std::fprintf(stderr, "[rdp!] first SET_COLOR_IMAGE %016llx\n",(unsigned long long)cmd); std::fflush(stderr); }
      if(op >= 0x08 && op <= 0x0f && !sawTri) { sawTri = true; std::fprintf(stderr, "[rdp!] first TRIANGLE op=%02x\n", op); std::fflush(stderr); }
      if(op == 0x36 && !sawFill) { sawFill = true; std::fprintf(stderr, "[rdp!] first FILL_RECT %016llx\n",(unsigned long long)cmd); std::fflush(stderr); }
    }

    // triangles have a variable length; everything else is one 64-bit word.
    // Coefficient blocks (64-bit words): edge 4, +shade 8, +texture 8, +zbuffer 2
    // (op bit2=shade, bit1=texture, bit0=zbuffer). Undercounting these desyncs the
    // FIFO — the next command lands mid-block and decodes as garbage.
    if(op >= 0x08 && op <= 0x0f) {
      int n = 4 + ((op & 4) ? 8 : 0) + ((op & 2) ? 8 : 0) + ((op & 1) ? 2 : 0);
      if(n > 24) n = 24;
      for(int i = 0; i < n; i++) words[i] = fetch(cur + i * 8);
      drawTriangle(mem, words, n, op);
      cur += n * 8; executed++; continue;
    }

    switch(op) {
    case 0x00: break;                                   // no-op
    case 0x26: case 0x27: case 0x28: break;             // SYNC_LOAD/PIPE/TILE → no-op here
    case 0x29: {
      static const bool sfLog = std::getenv("KESTREL_DPSYNCLOG") != nullptr;
      if(sfLog) { std::fprintf(stderr, "[dpsync] at=%06x span=%06x..%06x xbus=%u\n", cur, start & 0x00ffffffu, end, (unsigned)xbus); std::fflush(stderr); }
      sawSyncFull = true; break; }               // SYNC_FULL → raises DP (see caller)
    case 0x24: case 0x25: {                             // TEXTURE_RECTANGLE (+flip)
      words[0] = cmd; words[1] = fetch(cur + 8);        // 2 words
      texRect(mem, words, op == 0x25);
      cur += 16; executed++; continue;
    }
    case 0x2d:                                          // SET_SCISSOR
      sx0 = (int)(((cmd >> 44) & 0xfff) >> 2);
      sy0 = (int)(((cmd >> 32) & 0xfff) >> 2);
      sx1 = (int)(((cmd >> 12) & 0xfff) >> 2);
      sy1 = (int)(((cmd >> 0)  & 0xfff) >> 2);
      break;
    case 0x2c: {                                        // SET_CONVERT (YUV→RGB coeffs)
      auto s9 = [](u32 v) -> int { v &= 0x1ff; return (int)(v ^ 0x100) - 0x100; };  // 9-bit sign-extend
      k0 = s9((u32)(cmd >> 45)); k1 = s9((u32)(cmd >> 36)); k2 = s9((u32)(cmd >> 27));
      k3 = s9((u32)(cmd >> 18)); k4 = s9((u32)(cmd >> 9));  k5 = s9((u32)cmd);
      break;
    }
    // SET_PRIM_DEPTH (Z<<16 | dZ). The Z field is a 15-bit integer depth that the RDP
    // loads into the same s15.16 attribute the triangle z interpolator produces, and the
    // depth unit then takes bits 31:13 of that — an 18-bit value, 3 bits of it fractional.
    // Storing the bare 15-bit number instead costs those 3 bits, and they matter: near
    // z=0x7FFF the z-buffer's floating format has 1-unit steps in the 18-bit domain but
    // 64-unit steps in the 15-bit one, so successive prim depths 1 apart all quantize to
    // the same stored value and every depth compare after the first fails.
    case 0x2e: prim_z = ((u32)(cmd >> 16) & 0x7fff) << 3; break;
    case 0x2f:                                          // SET_OTHER_MODES
      other_hi = (u32)(cmd >> 32) & 0x00ff'ffff;
      other_lo = (u32)cmd;
      break;
    case 0x35: {                                        // SET_TILE
      u32 t = (cmd >> 24) & 7; Tile& tl = tiles[t];
      tl.fmt = (cmd >> 53) & 7; tl.size = (cmd >> 51) & 3;
      tl.line = (cmd >> 41) & 0x1ff; tl.tmem = (cmd >> 32) & 0x1ff;
      tl.palette = (cmd >> 20) & 0xf;
      tl.cmT = (cmd >> 18) & 3; tl.maskT = (cmd >> 14) & 0xf; tl.shiftT = (cmd >> 10) & 0xf;
      tl.cmS = (cmd >> 8)  & 3; tl.maskS = (cmd >> 4)  & 0xf; tl.shiftS = (cmd >> 0)  & 0xf;
      break;
    }
    case 0x32: {                                        // SET_TILE_SIZE
      u32 t = (cmd >> 24) & 7; Tile& tl = tiles[t];
      tl.sl = (cmd >> 44) & 0xfff; tl.tl = (cmd >> 32) & 0xfff;
      tl.sh = (cmd >> 12) & 0xfff; tl.th = (cmd >> 0) & 0xfff;
      break;
    }
    case 0x30: {                                        // LOAD_TLUT (palette load)
      // Copy 16-bit palette entries from the texture image (ti_addr) into `tlut`.
      // SL/SH (10.2) give the first/last source index; each entry is 2 bytes in RDRAM.
      //
      // The destination is NOT index SL: on HW the palette lives in the high 2 KB of TMEM
      // (64-bit words 0x100..0x1ff, one word per entry, the 16-bit value replicated 4x), and
      // LOAD_TLUT writes starting at the *destination tile's* TMEM address. `tlut[]` models
      // that region flat, so entry n of the load lands at (tile.tmem & 0xff) + n -- which is
      // exactly the sub-palette a CI4 draw then selects with its PALETTE field
      // (palette p covers flat entries p*16 .. p*16+15).
      u32 t  = (cmd >> 24) & 7;
      u32 dst = tiles[t].tmem & 0xff;
      u32 sl = ((u32)(cmd >> 44) & 0xfff) >> 2, sh = ((u32)(cmd >> 12) & 0xfff) >> 2;
      for(u32 i = sl; i <= sh; i++) {
        u32 src = ti_addr + i * 2;
        if(src + 1 < m.size()) tlut[(dst + (i - sl)) & 0xff] = ((u16)m[src] << 8) | m[src + 1];
      }
      break;
    }
    case 0x33: loadTile(mem, (cmd >> 24) & 7, true, cmd); break;   // LOAD_BLOCK
    case 0x34: loadTile(mem, (cmd >> 24) & 7, false, cmd); break;  // LOAD_TILE
    case 0x36:                                          // FILL_RECTANGLE
      fillRect(mem,
               (int)(((cmd >> 12) & 0xfff) >> 2), (int)(((cmd >> 0) & 0xfff) >> 2),
               (int)(((cmd >> 44) & 0xfff) >> 2) + 1, (int)(((cmd >> 32) & 0xfff) >> 2) + 1);
      break;
    case 0x37: fill_color  = (u32)cmd; break;           // SET_FILL_COLOR
    case 0x38: fog_color   = (u32)cmd; break;           // SET_FOG_COLOR
    case 0x39: blend_color = (u32)cmd; break;           // SET_BLEND_COLOR
    case 0x3a:                                          // SET_PRIM_COLOR
      // Bits 44:40 = min_level (LOD clamp, unused while we do not mipmap), 39:32 =
      // prim_lod_frac (a real combiner input), 31:0 = the RGBA colour.
      prim_color    = (u32)cmd;
      prim_lod_frac = (u8)(cmd >> 32);
      break;
    case 0x3b: env_color   = (u32)cmd; break;           // SET_ENV_COLOR
    case 0x3c: {                                        // SET_COMBINE
      combine_hi = (u32)(cmd >> 32) & 0x00ff'ffff; combine_lo = (u32)cmd;
      u32 hi = combine_hi, lo = combine_lo;
      // Field packing per the GBI SET_COMBINE word (see combineColor for the mux).
      comb[0] = { (int)((hi >> 20) & 0xf), (int)((lo >> 28) & 0xf), (int)((hi >> 15) & 0x1f), (int)((lo >> 15) & 0x7),
                  (int)((hi >> 12) & 0x7), (int)((lo >> 12) & 0x7), (int)((hi >> 9)  & 0x7),  (int)((lo >> 9)  & 0x7) };
      comb[1] = { (int)((hi >> 5)  & 0xf), (int)((lo >> 24) & 0xf), (int)((hi >> 0)  & 0x1f), (int)((lo >> 6)  & 0x7),
                  (int)((lo >> 21) & 0x7), (int)((lo >> 3)  & 0x7), (int)((lo >> 18) & 0x7),  (int)((lo >> 0)  & 0x7) };
      break;
    }
    case 0x3d:                                          // SET_TEXTURE_IMAGE
      ti_fmt = (cmd >> 53) & 7; ti_size = (cmd >> 51) & 3;
      ti_width = ((cmd >> 32) & 0x3ff) + 1; ti_addr = (u32)cmd & 0x00ff'ffff;
      break;
    case 0x3e: zi_addr = (u32)cmd & 0x00ff'ffff; break; // SET_Z_IMAGE
    case 0x3f:                                          // SET_COLOR_IMAGE
      ci_size = (cmd >> 51) & 3;
      ci_width = ((cmd >> 32) & 0x3ff) + 1;
      ci_addr = (u32)cmd & 0x00ff'ffff;
      // Diagnostico: ningun juego pone su framebuffer encima de los vectores de
      // excepcion de libultra (0x0-0x400) ni del area del OS. Si aparece aqui es que
      // el FIFO se ha desincronizado o el puntero llego corrupto: avisar con el
      // comando crudo y la posicion del FIFO para poder rastrear el origen.
      if(ci_addr < 0x400u || citrace) {
        lowCi = true; lowMark = pxWrites;
        std::fprintf(stderr, "[rdp!] SET_COLOR_IMAGE bajo: addr=%06x cmd=%016llx fifo=%08x\n",
                     ci_addr, (unsigned long long)cmd, cur);
        // Para distinguir "el escritor todavia no habia puesto el comando" (ceros o
        // basura alrededor) de "el puntero del FIFO apunta mal" (el vecindario si
        // tiene comandos validos), se vuelca el FIFO tal cual esta en RDRAM ahora.
        std::fprintf(stderr, "[rdp!]   dpc start=%06x end=%06x current=%06x  ucode leyo CURRENT %llu veces\n",
                     mem.rcp.dpc_start, mem.rcp.dpc_end, mem.rcp.dpc_current.load(),
                     (unsigned long long)mem.rcp.dpcCurReads.load());
        std::fprintf(stderr, "[rdp!]   span %06x..%06x  vecindario:\n", start & 0xffffff, end);
        for(int k = -3; k <= 3; k++) {
          u32 a = cur + (u32)(k * 8);
          std::fprintf(stderr, "[rdp!]     %06x %016llx%s\n", a,
                       (unsigned long long)fetch(a), k == 0 ? "  <- aqui" : "");
        }
      } else if(lowCi) {
        lowCi = false;
        std::fprintf(stderr, "[rdp!]   ...se escribieron %llu pixeles con el CI bajo\n",
                     (unsigned long long)(pxWrites - lowMark));
      }
      break;
    default: break;                                     // sync/tlut/other → no-op for now
    }
    cur += 8; executed++;
  }
  if(ops) {
    // KESTREL_RDPOPS is the print interval in DP runs (default 512). Demos that
    // submit a single command buffer and then spin need =1, or the histogram
    // never prints and the trace looks like "the RDP did nothing".
    static const u32 every = []{ const char* s = std::getenv("KESTREL_RDPOPS");
                                 u32 v = s ? (u32)std::strtoul(s, nullptr, 0) : 0; return v ? v : 512u; }();
    static u32 calls = 0;
    if(++calls % every == 0) {
      std::fprintf(stderr, "[rdpops] ci=%06x sz=%u w=%u | ", ci_addr, ci_size, ci_width);
      for(int i = 0; i < 64; i++) if(hist[i]) std::fprintf(stderr, "%02x:%u ", i, hist[i]);
      std::fprintf(stderr, "\n"); std::fflush(stderr);
    }
  }
  return executed;
}

}  // namespace kestrel
