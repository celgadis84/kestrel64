#pragma once
#include <atomic>
// kestrel64 — software RDP (M3.2). Consumes the RDP command FIFO
// (DPC_START..DPC_END, 64-bit commands living in RDRAM) and rasterizes into the
// RDRAM color/z images the game set up. This is the "first light" renderer: no
// Vulkan, output goes straight into RDRAM where the VI presenter reads it, so it
// is directly inspectable via a BMP dump. parallel-rdp (GPU) is the later
// accuracy/perf path; this exists to get real pixels on screen and to be
// debuggable command-by-command.
//
// Scope grows incrementally. Implemented: SET_COLOR_IMAGE/Z_IMAGE/SCISSOR/
// OTHER_MODES/FILL_COLOR/FILL_RECTANGLE, the SET_*_COLOR registers, SYNC_FULL,
// and shaded/textured triangles + TEXTURE_RECTANGLE (added stepwise).

#include "../core/types.hpp"

namespace kestrel {

struct Memory;

struct SoftRdp {
  // Execute the command list in RDRAM spanning [start, end) physical addresses.
  // Returns the number of commands executed. Writes pixels into RDRAM directly.
  // Rasterize the RDP command FIFO from `start` to `end`. When `xbus` is set the
  // command words are fetched from DMEM (12-bit wrapping) instead of RDRAM — the
  // hardware's "run from DMEM" mode (DP_STATUS_XBUS). Pixel output always lands in
  // RDRAM at the color image. Returns the command count processed.
  auto run(Memory& mem, u32 start, u32 end, bool xbus = false) -> u32;

  // True iff the last run() processed a SYNC_FULL command. The DP interrupt must
  // be raised only on SYNC_FULL — NOT on every DPC_END write. PD streams the RDP
  // FIFO with many DPC_END bumps per frame; raising DP on each one is an interrupt
  // storm that starves the game thread. The caller gates raiseIntr(MI_DP) on this.
  bool sawSyncFull = false;

  // read-only introspection for telemetry / tracing
  // Donde publicar el puntero de lectura del FIFO (DPC_CURRENT). Opcional: en las
  // pruebas unitarias del RDP no hay registros que actualizar.
  std::atomic<u32>* curOut = nullptr;
  // Direccion donde se paro el consumo. Igual a `end` salvo cuando el ultimo comando del
  // span esta partido: el command processor no ejecuta comandos a medias, se para delante
  // de el y lo reanuda cuando END avanza. El llamante reanuda ahi el span siguiente.
  u32 stopAt = 0;
  auto colorImage() const -> u32 { return ci_addr; }
  auto colorImageSize() const -> u32 { return ci_size; }

  // --- DPC performance counters (accounting only, never gates execution) -------
  // The RDP owns four 24-bit counters the CPU reads at DPC_CLOCK/BUFBUSY/PIPEBUSY/
  // TMEM. Games (and Thar0's RDP-Timing-Tests) use them to time rasterization, so
  // they must advance with a cost model, not stay pinned at zero. See rdp.cpp for
  // the model and scripts/rdptiming.py for its calibration against hardware.
  auto accountPixels(Memory& mem, u64 npx, u64 nWrite, u64 nZWrite) -> void;
  auto accountTmem(Memory& mem, u64 bytes) -> void;

private:
  // --- pipeline state (persists across command lists, like the real RDP) ------
  u32 ci_addr = 0;     // color image base (physical)
  u32 ci_width = 320;  // color image width in pixels
  u32 ci_size = 2;     // 2 = 16bpp RGBA5551, 3 = 32bpp RGBA8888
  u32 zi_addr = 0;     // z image base (physical)

  u32 ti_addr = 0;     // texture image base (physical)
  u32 ti_width = 0;    // texture image width in texels
  u32 ti_size = 2;     // texel size code
  u32 ti_fmt  = 0;     // texel format code

  u32 fill_color = 0;  // SET_FILL_COLOR (raw 32-bit; two 16bpp pixels or one 32bpp)
  u32 blend_color = 0, fog_color = 0, prim_color = 0, env_color = 0;
  u8  prim_lod_frac = 0;            // SET_PRIM_COLOR bits 39:32 — combiner mul input
  // LOD fraction feeding the combiner's LOD_FRAC mux. We do not mipmap: max_level is 0,
  // so the LOD unit always reports "distant" (parallel-rdp compute_lod_2cycle: the
  // magnify branch takes `distant = max_level == 0` and the mip branch takes
  // `distant = mip_base >= max_level`; both then pin lod_frac to 0xff when neither
  // SHARPEN nor DETAIL is on). It is 0xff, NOT 0x100: the mul port is 9 bits and 0xff is
  // an ordinary positive value there, so x*LOD_FRAC == (x*0xff + 0x80) >> 8 == x - 1 for
  // x = 0xff. krom's GRB decoders are the witness — they route TEXEL0_ALPHA through
  // LOD_FRAC into COMBINED_ALPHA and their hardware captures pin the resulting scale at
  // 254/256, which only 0xff produces (0x100 would pass the texel through untouched).
  static constexpr auto lodFrac() -> int { return 0xff; }
  u32 prim_z = 0;      // SET_PRIM_DEPTH primitive Z (used when Z_SOURCE_SEL is set)

  int k0 = 0, k1 = 0, k2 = 0, k3 = 0, k4 = 0, k5 = 0;  // SET_CONVERT (YUV→RGB coeffs, 9-bit signed)

  u32 other_lo = 0, other_hi = 0;   // SET_OTHER_MODES (cycle type etc.)
  u32 combine_lo = 0, combine_hi = 0;
  // Decoded color combiner. Two sets: comb[0]=cycle-0 equation, comb[1]=cycle-1.
  // Each channel computes out = ((A-B)*C + (D<<8) + 0x80) >> 8 (HW rounding). RGB and
  // alpha carry independent A/B/C/D source selectors (the SET_COMBINE mux indices).
  struct CombSet { int aR = 0, bR = 0, cR = 0, dR = 0, aA = 0, bA = 0, cA = 0, dA = 0; };
  CombSet comb[2];
  // COMBINED feedback register. This is real pipeline state, not a per-pixel temporary:
  // the mux index for COMBINED/COMBINED_ALPHA reads whatever the combiner last wrote, so
  // in 1-cycle mode (and in cycle 0 of 2-cycle mode) it holds the PREVIOUS pixel's result.
  // Held CLAMPED (the 8-bit value that left the pipeline), unlike the cycle0→cycle1 path
  // inside one pixel, which forwards the raw 9-bit result. Storing the raw value here
  // would feed a negative COMBINED_ALPHA into the next pixel and turn every
  // TEXEL0*COMBINED_ALPHA decoder into saturated garbage.
  int combined[4] = {0, 0, 0, 0};

  int sx0 = 0, sy0 = 0, sx1 = 320, sy1 = 240;   // scissor box (pixels)

  // Pixels that actually reached the color / z images since boot. Only feeds the
  // DPC counters: a pixel killed by alpha or depth compare performs its reads but
  // neither write, and that difference is worth ~1 cycle/pixel on hardware.
  u64 pxWrites = 0, pxZWrites = 0;

  // --- tiles / TMEM -----------------------------------------------------------
  struct Tile {
    u32 fmt = 0, size = 0, line = 0, tmem = 0, palette = 0;
    u32 cmS = 0, maskS = 0, shiftS = 0, cmT = 0, maskT = 0, shiftT = 0;
    u32 sl = 0, tl = 0, sh = 0, th = 0;   // SET_TILE_SIZE (10.2 fixed)
  } tiles[8];
  u8  tmem[0x1000] = {};   // 4 KB texture memory
  u16 tlut[256] = {};      // palette RAM (LOAD_TLUT); CI4/CI8 index into this
  u32 tlutMode() const { return (other_hi >> 14) & 3; }  // TEXTLUT: 0 none,2 RGBA16,3 IA16

  // --- helpers ----------------------------------------------------------------
  auto cycleType() const -> u32 { return (other_hi >> 20) & 3; }  // 0 1cyc,1 2cyc,2 copy,3 fill

  // N64 z-buffer compression. The pipeline depth is an 18-bit integer (0..0x3FFFF);
  // it is stored in 16 bits as a piecewise-linear float: 3-bit exponent + 11-bit
  // mantissa packed into bits[15:2] of the memory word (the low 2 bits carry dz,
  // unmodeled here — irrelevant to the fill/compare tests). Encode/decode are exact
  // inverses to the segment base, so ordering is preserved as on hardware; the clear
  // value 0xFFFF decodes to the farthest depth (0x3FFFF).
  static constexpr u32 zBase[8]  = {0,0x20000,0x30000,0x38000,0x3c000,0x3e000,0x3f000,0x3f800};
  static constexpr u32 zShift[8] = {6,5,4,3,2,1,0,0};
  static auto zEncode(u32 z) -> u16 {                     // 18-bit z → 16-bit mem word
    if(z > 0x3ffff) z = 0x3ffff;
    int e = 0;
    while(e < 7 && z >= zBase[e + 1]) e++;
    u32 mant = (z - zBase[e]) >> zShift[e];               // 11-bit
    return u16((((u32)e << 11) | (mant & 0x7ff)) << 2);   // 14-bit z_com in [15:2]
  }
  static auto zDecode(u16 mem) -> u32 {                    // 16-bit mem word → 18-bit z
    u32 com = (mem >> 2) & 0x3fff;
    int e = (com >> 11) & 7;
    return ((com & 0x7ff) << zShift[e]) + zBase[e];
  }

  auto depthTest(Memory& mem, int x, int y, s32 d) -> bool;   // Z_CMP / Z_UPD on one pixel
  auto putPixel(Memory& mem, int x, int y, u32 rgba32) -> void;
  // Blender: fold `src` (pipeline RGBA, alpha = combined alpha) against the framebuffer
  // per SET_OTHER_MODES render-mode word. Only engages when IM_RD (read-enable, bit 0x40)
  // is set; opaque modes with no framebuffer read write straight through. out = P*a + M*b
  // with P/M/a/b picked by the blend mux (m1a,m1b,m2a,m2b) of the final blender cycle.
  auto readFb(Memory& mem, int x, int y) -> u32;             // framebuffer colour → RGBA32
  auto blendColor(u32 src, u32 memc, bool blendEn) -> u32;   // pure blend-mux math
  // aaEdge = this pixel is only partially covered, which is what ANTIALIAS_EN turns the
  // blender on for. Fully covered pixels (the default) blend only under FORCE_BLEND.
  auto blendPixel(Memory& mem, int x, int y, u32 src, bool aaEdge = false) -> void;
  auto ditherRgb(int x, int y, u32 c) const -> u32;   // RGB_DITHER_SEL, framebuffer write path
  // Coverage-based edge AA: cvg<1 folds `src` (after blend) against the framebuffer.
  auto coverPixel(Memory& mem, int x, int y, u32 src, double cvg) -> void;
  auto fillRect(Memory& mem, int x0, int y0, int x1, int y1) -> void;
  auto drawTriangle(Memory& mem, const u64* w, int words, u32 op) -> void;
  auto texRect(Memory& mem, const u64* w, bool flip) -> void;
  auto loadTile(Memory& mem, u32 tile, bool block, u64 cmd) -> void;
  auto sampleTexel(u32 tile, int s, int t) -> u32;
  auto sampleRawIndex(u32 tile, int s, int t) -> int;   // raw CI index (pre-TLUT) for 8bpp CI blits
  auto sampleTexFiltered(u32 tile, double s, double t) -> u32;  // point or N64 3-point
  // Run the color combiner: texel0/texel1 sampled, shade (Gouraud) → final RGBA.
  // Honours 1-cycle (comb[1]) and 2-cycle (comb[0] feeds COMBINED into comb[1]).
  auto combineColor(u32 tex0, u32 tex1, u32 shade) -> u32;
};

}  // namespace kestrel
