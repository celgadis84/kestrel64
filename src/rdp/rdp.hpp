#pragma once
#include <atomic>
#include <vector>
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
#include "texpack.hpp"

namespace kestrel {

struct Memory;

struct StateVisitor;

// Estadisticas del RDP para optimizar juegos (telemetria `rdp.stats`, PD64_pending P3). Las
// escribe solo el paseo que COBRA un tramo (charge), asi que cada comando cuenta una vez, y
// solo con `on`: apagadas cuestan un test por comando. Contadores atomicos relajados porque
// el lector es el hilo de telemetria; el seguimiento de redundancia (prim*) lo toca solo el
// hilo que cobra, en orden de FIFO.
struct RdpStats {
  std::atomic<bool> on{false};
  std::atomic<u64> op[64]{};
  // GCLK cobrados, repartidos: pixeles en 1/2 ciclos y COPY, pixeles en FILL, cargas de TMEM,
  // paradas fijas de SYNC_LOAD/PIPE/TILE.
  std::atomic<u64> gclkPixel{0}, gclkFill{0}, gclkTmem{0}, gclkSync{0};
  std::atomic<u64> px[4]{};              // pixeles que entran al pipeline por modo de ciclo
  std::atomic<u64> pxWritten{0}, pxImRd{0}, pxZCmp{0}, pxZUpd{0};
  // Redundancia. Un SYNC_* es redundante si no hubo NINGUNA primitiva (triangulo, rect de
  // textura, fill rect) desde el anterior del mismo tipo o desde el ultimo SYNC_FULL: no
  // habia nada en vuelo que esperar, y la parada se paga igual (n64brew).
  std::atomic<u64> syncRedundant[3]{};   // load, pipe, tile
  // Mas estricto: ni primitiva NI carga de TMEM (LOAD_BLOCK/TILE/TLUT) desde el anterior.
  // El SYNC_PIPE tras un LOAD_BLOCK (macro gDPLoadTextureBlock) cae en syncRedundant pero
  // no aqui: esto es lo que se puede quitar sin discutir (peticion PD-opt 2026-09-30).
  std::atomic<u64> syncPure[3]{};
  std::atomic<u64> loads{0}, loadBytes{0}, loadRedundant{0}, loadRedundantBytes{0};
  std::atomic<u64> otherModesSame{0}, combineSame{0};
  // GPU-RDP: tramos de run() que fueron a la GPU / que el reparto por area mando al CPU, y
  // primitivas que la GPU no admite y pinto SoftRDP (triangulos, rects de textura).
  std::atomic<u64> gpuTramos{0}, cpuTramos{0}, gpuFallTri{0}, gpuFallRect{0};
  bool primSince[3] = {true, true, true};
  bool workSince[3] = {true, true, true};  // primitiva o carga de TMEM
  u32 viFlips0 = 0, viFields0 = 0;
  // Reloj de INVITADO al reset (PD64_pending P4): con ellos rdp.stats da que fraccion del
  // tiempo de invitado de la ventana tuvo ocupado al RDP (GCLK), corriendo al RSP y a la CPU
  // en el bucle ocioso del kernel.
  u64 guestOps0 = 0, retired0 = 0, idleOps0 = 0, rspCyc0 = 0, rspPoll0 = 0;
};

struct SoftRdp {
  friend struct StateVisitor;   // savestate: lee/escribe el estado de pipeline privado
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
  // De donde se leen los COMANDOS (no los pixeles ni las texturas, que siguen yendo a la
  // RDRAM viva). Normalmente es la instantanea que el productor dejo al encolar el tramo:
  // ver Memory::rdpSnapshot. Nulo = leer la RDRAM directamente (lockstep, pruebas).
  const u8* cmdSrc = nullptr;
  // Modo SOLO-COSTE: decodifica el FIFO y cobra los contadores del RDP (DPC_CLOCK y el
  // reloj de coste que frena a la CPU) SIN escribir un solo pixel. Existe porque con
  // parallel-RDP quien rasteriza es la GPU, y la GPU no puede decirle al invitado lo que
  // costo: sin esto el RDP le sale GRATIS al juego -- DPC_CLOCK clavado a cero y ninguna
  // espera -- y el emulador corre mas suelto de lo que jamas corrio la consola. En este
  // modo el bucle por pixel no se ejecuta: cada tramo aporta su ANCHURA de una vez, que es
  // la misma cuenta de pixeles que entrarian al pipeline, asi que el coste es O(altura)
  // por primitiva en vez de O(area). Los pixeles de TRIANGULO se cobran como escritos: saber
  // cuales moririan en el test pediria interpolar la z pixel a pixel, O(area). Un FILL_RECT
  // en 1/2 ciclos si se evalua (alpha constante, z de primitiva constante contra la z de
  // RDRAM, solo lectura): Thar0 mide justo eso y sin ello se iba de 0.13 a 0.99 cyc/px.
  bool costOnly = false;
  // Cobrar o no los contadores del RDP (DPC_CLOCK/PIPEBUSY/BUFBUSY/TMEM y el reloj de coste
  // rdpGclk). El coste de un tramo se paga UNA vez: si delante ha ido un paseo solo-coste,
  // el paseo que pinta ya no cobra. Ver Memory::rdpRunJob.
  bool charge = true;
  // Direccion donde se paro el consumo. Igual a `end` salvo cuando el ultimo comando del
  // span esta partido: el command processor no ejecuta comandos a medias, se para delante
  // de el y lo reanuda cuando END avanza. El llamante reanuda ahi el span siguiente.
  u32 stopAt = 0;
  // GPU-RDP (ver src/gpurdp/gpurdp.hpp): hay primitivas encoladas en la GPU que aun no estan
  // en la RDRAM. gpuFlush las baja; run() lo llama antes de cualquier comando que toque la
  // RDRAM por su cuenta y al terminar el tramo.
  bool gpuQueued = false;
  // GPU-RDP para ESTE tramo (lo decide run() al empezar, por area estimada). Fuera de run(), si.
  bool gpuRun = true;
  auto gpuOn() const -> bool;
  auto gpuFlush(Memory& mem) -> void;
  // Bytes de RDRAM que lo encolado va a escribir, en hasta 8 intervalos [lo, hi). Una carga
  // de TMEM que lee fuera de ellos no necesita vaciar la cola (lo que lee ya esta al dia).
  u32 gpuDirtyLo[8] = {}, gpuDirtyHi[8] = {};
  int gpuNDirty = 0;
  auto gpuMark(u32 lo, u32 hi) -> void;
  static auto gpuFlushStat(int why) -> void;
  auto gpuDirtyHit(u64 lo, u64 hi) const -> bool;
  auto colorImage() const -> u32 { return ci_addr; }
  // Zona de RDRAM que cubren las primitivas cobradas desde el ultimo reinicio: color image y
  // z image hasta la esquina del scissor, en hasta kWrSlots intervalos [lo, hi) fisicos (lo >
  // hi = vacio). Varios y no uno: un juego que borra el z-buffer usandolo como color image
  // (SM64, z en 0x400) y pinta luego en un framebuffer alto dejaba un unico intervalo que
  // cubria casi toda la RDRAM. Cota por arriba de lo que el motor que pinta de verdad puede
  // escribir. Ver Memory::rspDmaRdpWait.
  static constexpr u32 kWrSlots = 4;
  u32 wrLo[kWrSlots] = {~0u, ~0u, ~0u, ~0u}, wrHi[kWrSlots] = {};
  auto wrAdd(u32 a, u32 b) -> void;
  auto wrClear() -> void { for(u32 i = 0; i < kWrSlots; i++) { wrLo[i] = ~0u; wrHi[i] = 0; } }
  auto colorImageSize() const -> u32 { return ci_size; }

  // --- DPC performance counters (accounting only, never gates execution) -------
  // The RDP owns four 24-bit counters the CPU reads at DPC_CLOCK/BUFBUSY/PIPEBUSY/
  // TMEM. Games (and Thar0's RDP-Timing-Tests) use them to time rasterization, so
  // they must advance with a cost model, not stay pinned at zero. See rdp.cpp for
  // the model and scripts/rdptiming.py for its calibration against hardware.
  auto accountPixels(Memory& mem, u64 npx, u64 nWrite, u64 nZWrite) -> void;
  // accountPixels en dos mitades: la foto del estado al rasterizar y el cobro, que necesita
  // saber cuantos pixeles se escribieron (con el GPU-RDP llega al vaciar la cola).
  struct AcctSnap {
    u64 npx = 0, lines = 0, units = 0;
    u32 ci = 0, zi = 0, ciW = 0, ciSize = 0, olo = 0, cyc = 0;
    int sx1 = 0, sy1 = 0;
    bool viOn = false, fbviSame = false, charge = true;
  };
  auto acctSnap(Memory& mem, u64 npx) -> AcctSnap;
  auto acctFinish(Memory& mem, const AcctSnap& a, u64 nWrite, u64 nZWrite) -> void;
  // Triangulos encolados en la GPU, en orden: su foto de cobro y si dejan COMBINED.
  struct GpuTri { AcctSnap acct; bool comb; };
  std::vector<GpuTri> gpuTris;
  // Tramos (una linea de una primitiva) de lo que se va a cobrar: el llamante declara cada
  // linea con su primer pixel y su anchura y accountPixels los consume. Sin tramos declarados
  // cae al reparto plano npx/8 de antes.
  auto addSpan(int x0, u64 npx) -> void;
  u64 spanLines = 0, spanUnits = 0;
  auto accountTmem(Memory& mem, u64 bytes, u64 gclk) -> void;
  auto accountLoad(Memory& mem, u64 rows, u64 bytesPerRow) -> void;   // fijo por rafaga + 8 B/GCLK
  // Coste fijo de un comando que para el pipeline sin tocar RDRAM ni TMEM (SYNC_LOAD/PIPE/TILE).
  auto accountStall(Memory& mem, u32 gclk) -> void;

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
  u8  prim_min_level = 0;           // SET_PRIM_COLOR bits 44:40 — suelo de LOD_FRAC (sharpen/detail)
  // LOD_FRAC del pixel en curso: registro real del TX. Lo escribe la unidad de LOD
  // (lodSelect) cuando corre, que es con TEX_LOD_EN o en un 2-cycle cuyo combinador lo lee;
  // si no corre se queda saturado en 0xff (parallel-rdp shading.h). Es 0xff, NO 0x100: el
  // puerto mul es de 9 bits y 0xff ahi es un positivo normal, asi que x*LOD_FRAC ==
  // (x*0xff + 0x80) >> 8 == x - 1 para x = 0xff. Testigo: los decodificadores GRB de krom
  // pasan TEXEL0_ALPHA por LOD_FRAC a COMBINED_ALPHA y sus capturas de HW fijan la escala
  // en 254/256, que solo da 0xff (0x100 dejaria pasar el texel intacto).
  int lodFracV = 0xff;
  auto lodFrac() const -> int { return lodFracV; }
  u32 prim_z = 0;      // SET_PRIM_DEPTH primitive Z (used when Z_SOURCE_SEL is set)
  u32 prim_dz = 0;     // SET_PRIM_DEPTH delta Z (bits 15:0), the dz paired with prim_z

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
  auto statsCmd(Memory& mem, u32 op, u64 cmd) -> void;     // rdp.stats: antes de ejecutar
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
  auto depthPasses(Memory& mem, int x, int y, s32 d) const -> bool;   // Z_CMP only, read-only
  // Escritura al color image SIN el test de scissor: la usan los llamadores que ya lo han
  // hecho (blendPixel entra por su propio test y luego escribe hasta tres veces). El test
  // seguia repitiendose en cada eslabon de coverPixel -> blendPixel -> putPixel.
  [[gnu::always_inline]] auto storePixel(Memory& mem, int x, int y, u32 rgba32) -> void;
  auto putPixel(Memory& mem, int x, int y, u32 rgba32) -> void;
  // Blender: fold `src` (pipeline RGBA, alpha = combined alpha) against the framebuffer
  // per SET_OTHER_MODES render-mode word. Only engages when IM_RD (read-enable, bit 0x40)
  // is set; opaque modes with no framebuffer read write straight through. out = P*a + M*b
  // with P/M/a/b picked by the blend mux (m1a,m1b,m2a,m2b) of the final blender cycle.
  auto readFb(Memory& mem, int x, int y) -> u32;             // framebuffer colour → RGBA32
  auto blendColor(u32 src, u32 memc, bool blendEn, bool cvgWrap, int shA = 0, int shB = 0) -> u32;   // pure blend-mux math
  // `cvg` = subpixeles cubiertos por el primitivo, 0..8 (8 = pixel entero). Es el valor que
  // recorre TODA la etapa de escritura del RDP: modula el alfa (CVG_TIMES_ALPHA /
  // ALPHA_CVG_SELECT), decide si el blender se enciende (AA_EN sin desbordar coverage),
  // mata el pixel si se queda en cero, y acaba guardado en el framebuffer segun CVG_DEST.
  // `z` = profundidad del pixel cuando la primitiva hace Z_CMP/Z_UPD (nullptr si no): la
  // etapa de profundidad va DENTRO, despues de que el combinador fije la cobertura.
  auto blendPixel(Memory& mem, int x, int y, u32 src, int cvg = 8, const s32* z = nullptr) -> void;
  // Color images que sabe pintar la GPU: 8, 16 (direccion par) y 32 bits (multiplo de 4).
  auto gpuCiOk() const -> bool {
    return ci_size == 1 || (ci_size == 2 && !(ci_addr & 1)) || (ci_size == 3 && !(ci_addr & 3));
  }
  auto gpuCiBpp() const -> u32 { return ci_size == 3 ? 4u : ci_size == 2 ? 2u : 1u; }
  auto gpuTexRect(Memory& mem, const u64* w, bool flip, int X0, int X1, int Y0, int Y1,
                  bool usesTex1) -> bool;
  auto ditherRgb(int x, int y, u32 c) const -> u32;   // RGB_DITHER_SEL, framebuffer write path
  auto alphaDither(int x, int y) const -> int;        // ALPHA_DITHER_SEL, 0..7
  // Alfa de referencia del alpha compare: el alfa combinado expandido (0xff -> 0x100) mas
  // el dither de alfa, salvo con ALPHA_CVG_SELECT, saturado a 0xff.
  auto alphaRef(int x, int y, u32 c) const -> int {
    int a = (int)(c & 0xff);
    if(other_lo & 0x2000) return a;
    a += ((a + 1) >> 8) + alphaDither(x, y);
    return a > 255 ? 255 : a;
  }
  // Bits ocultos de RDRAM: los 2 bits bajos de la cobertura de cada pixel de 16bpp. El bit
  // alto vive en el bit 0 del propio pixel RGBA5551 (lo que el GBI llama "alfa"); los otros
  // dos estan en la RAM oculta de 9 bits de los chips RDRAM, invisible para la CPU.
  auto hiddenBits(Memory& mem) -> u8*;
  auto fillRect(Memory& mem, int x0, int y0, int x1, int y1) -> void;
  auto drawTriangle(Memory& mem, const u64* w, int words, u32 op) -> void;
  auto gpuTriangle(Memory& mem, const u64* w, bool leftMajor, bool fillMode, bool gouraud,
                   bool combProg, u32 flat, u32 flatTexel, bool zActive, bool zSrc,
                   const s32* cC, const s32* cDx, const s32* cDe, const s32* cDy,
                   const s32* tC, const s32* tDx, const s32* tDe, const s32* tDy,
                   bool textured, bool persp, bool usesLod, bool usesTex1, u32 texTile,
                   u32 maxLevel) -> bool;
  auto texRect(Memory& mem, const u64* w, bool flip) -> void;
  // RDRAM -> TMEM recortado al primer limite (0x1000 en TMEM, fin de RDRAM): copia el
  // MISMO prefijo que el bucle byte a byte que sustituye, porque ambos limites son
  // monotonos y la copia va en orden ascendente.
  template<typename V> auto copyRun(const V& m, u32 src, u32 dst, u32 n) -> void;
  auto loadTile(Memory& mem, u32 tile, bool block, u64 cmd) -> void;
  // Constantes derivadas de un `Tile` que NO dependen del texel: el filtro de 3 puntos
  // muestrea 3-4 veces el MISMO tile por pixel, y recalcular esto en cada toma era el grueso
  // de `sampleTexel` (43 % del hilo del RDP en el perfil). Se arma una vez por pixel y las
  // tomas solo hacen desplazamiento + pliegue + decodificacion. `kind` colapsa el par
  // (size, fmt) a un unico salto de tabla en vez de la cadena de ifs anidados.
  struct TexFold {
    u32 shiftS, shiftT, maskS, maskT, cmS, cmT;
    int sMax, tMax;
    u32 rowBytes, base, palette, kind;
    // Caso comun del pliegue: sin SHIFT, con mask y sin clamp ni mirror. Ahi las dos
    // etapas de `foldCoord` se reducen a un AND, y el tile decide el camino una vez.
    bool fastS, fastT;
    int  andS, andT;
  };
  enum : u32 { TK_CI4, TK_IA4, TK_I4, TK_YUV, TK_IA16, TK_RGBA16, TK_RGBA32,
               TK_CI8, TK_IA8, TK_I8, TK_BAD };
  auto foldOf(u32 tile) const -> TexFold;
  // Unidad de LOD (mipmap): vecinos +x/+y en 1/32 de texel -> LOD_FRAC, y con TEX_LOD_EN
  // elige tile0/tile1 (entrada: tile base y base+1).
  auto lodSelect(s32 s, s32 t, s32 sdx, s32 tdx, s32 sdy, s32 tdy, bool ovf, u32 maxLevel,
                 u32& tile0, u32& tile1) const -> int;
  // Que pide la primitiva al TX: unidad de LOD y segundo muestreo (TEXEL1 de tile1).
  auto texNeeds(bool& lod, bool& tex1) const -> void;
  auto tlutEntry(u32 idx) const -> u32;          // entrada de paleta -> RGBA8888
  // Muestreo y filtro ESPECIALIZADOS por formato. El despacho (size, fmt) se hace una sola
  // vez por pixel en `sampleTexFiltered`; a partir de ahi las 3-4 tomas del filtro son codigo
  // sin ramas de formato y se alinean dentro del filtro, en vez de tres llamadas a una
  // funcion generica que vuelve a decidir el formato en cada toma.
  // always_inline: clang la dejaba fuera de linea y el filtro pagaba 3 llamadas por pixel
  // (35 % del hilo del RDP en el perfil); especializada es corta y merece alinearse.
  // Pliegue de una coordenada (SHIFT + clamp/mirror/mask) separado de la lectura: el
  // filtro de 3 puntos comparte cuatro pliegues entre sus tres tomas.
  [[gnu::always_inline]] static inline auto foldCoord(int c, u32 sh, u32 mask, u32 cm, int lim) -> int;
  template<u32 K> [[gnu::always_inline]] inline auto fetchK(const TexFold& f, int s, int t) const -> u32;
  template<u32 K> [[gnu::always_inline]] inline auto texelK(const TexFold& f, int s, int t) const -> u32;
  template<u32 K> auto filterK(const TexFold& f, double s, double t) const -> u32;
  auto texelAt(const TexFold& f, int s, int t) const -> u32;   // version generica (despacha)
  // Filtro con el pliegue YA armado: el tile es constante dentro de una primitiva, asi que
  // el rasterizador lo arma una vez por triangulo en vez de una vez por pixel.
  auto sampleTexFold(const TexFold& f, double s, double t) const -> u32;
  auto sampleTexel(u32 tile, int s, int t) -> u32;
  auto sampleRawIndex(u32 tile, int s, int t) -> int;   // raw CI index (pre-TLUT) for 8bpp CI blits
  auto sampleTexFiltered(u32 tile, double s, double t) -> u32;  // point or N64 3-point
  // --- texturas HD / realce (texpack.hpp, docs/TEXTURAS-HD.md). Inerte sin variables. ---
  // Lo que GLideN64 apunta de cada carga para calcular el hash Rice de la textura en RDRAM
  // al dibujarla: indexado por la direccion de TMEM del tile de carga, como alli.
  struct HdLoad {
    u32 addr = 0, uls = 0, ult = 0, width = 0, height = 0, texWidth = 0, size = 0, dxt = 0;
    bool block = false, valid = false;
  };
  HdLoad hdLoads[512];
  u8  hdPal[512] = {};   // paletas cargadas, en bytes "del anfitrion" (ver texpack.hpp)
  u32 hdGen = 1;         // sube con cada carga a TMEM: invalida hdMemo
  struct HdBind {
    const texpack::Tex* tex = nullptr;   // nullptr = sin sustituto, muestreo normal
    double scX = 1, scY = 1;             // texeles HD por texel original
    TexFold f{};
  };
  struct HdMemo { u32 gen = 0, mode = ~0u; Tile tile{}; HdBind b; };
  HdMemo hdMemo[8];
  HdBind gpuHd;          // la del primitivo en curso, para gpuTriangle / gpuTexRect
  auto hdNoteLoad(u32 tile, bool block, u64 cmd) -> void;
  auto hdNoteTlut(Memory& mem, u32 tile, u64 cmd) -> void;
  auto hdBind(Memory& mem, u32 tile) -> HdBind;
  // `fp` = texeles ORIGINALES que cubre un pixel (elige nivel de la piramide).
  auto hdSample(const HdBind& b, double s, double t, double fp) const -> u32;
  // Run the color combiner: texel0/texel1 sampled, shade (Gouraud) → final RGBA.
  // Honours 1-cycle (comb[1]) and 2-cycle (comb[0] feeds COMBINED into comb[1]).
  // --- plan del combinador -----------------------------------------------------------
  // Los 8 selectores de cada ciclo son constantes hasta el siguiente SET_COMBINE, y el
  // numero de ciclos hasta el siguiente SET_OTHER_MODES. Resolverlos con un `switch` por
  // canal y por pixel salia el 13 % del hilo del RDP; el plan los traduce UNA vez a un
  // indice de fila y el camino por pixel queda en tabla. La llave es el par de palabras
  // SET_COMBINE mas el tipo de ciclo, asi que cualquier via que los cambie (comando,
  // savestate, MCP) invalida el plan sola, sin bandera que se pueda quedar rancia.
  enum : u32 { CR_CIN, CR_TEX0, CR_TEX1, CR_PRIM, CR_SHADE, CR_ENV,
               CR_CINA, CR_TEX0A, CR_TEX1A, CR_PRIMA, CR_SHADEA, CR_ENVA,
               CR_ONE, CR_ZERO, CR_LOD, CR_PLOD, CR_ROWS, CR_NOISE = 0xff };
  struct CombPlan {
    u8   sel[2][8] = {};   // [ciclo][aR,bR,cR,dR,aA,bA,cA,dA] -> fila
    u32  need = 0;         // filas a materializar (bitmask)
    bool fast = false;     // false: algun selector es NOISE -> camino generico
    bool two  = false;     // 2-cycle
    u32  keyHi = ~0u, keyLo = ~0u, keyCyc = ~0u;
  } combPlan;
  // --- plan del blender --------------------------------------------------------------
  // Los muxes P/A/M/B, el bit de lectura de framebuffer, FORCE_BLEND, AA_EN y el modo de
  // dither salen todos de SET_OTHER_MODES, o sea que son constantes de primitiva; se
  // estaban redescodificando por pixel (y `blendPixel` volvia a llamar a `cycleType()`
  // tres veces). Misma disciplina que el plan del combinador: la llave es el par de
  // palabras de modo, no una bandera de suciedad, asi que savestate/MCP no lo dejan rancio.
  struct BlendPlan {
    u32  keyHi = ~0u, keyLo = ~0u;
    u8   psel = 0, asel = 0, msel = 0, bsel = 0;
    // 2-cycle: el ciclo 0 del blender (GBL_c1) corre SIEMPRE y su RGB es el IN del ciclo 1.
    bool two = false;
    u8   psel0 = 0, asel0 = 0, msel0 = 0, bsel0 = 0;
    u8   dither = 3;      // RGB_DITHER_SEL: 0 magic, 1 bayer, 2 ruido, 3 off
    bool usesMem = false; // el mux referencia CLR_MEM o MEM_alpha
    bool imRd = false;    // IM_RD: lecturas de framebuffer habilitadas
    bool force = false;   // FORCE_BLEND
    bool aaEn = false;    // ANTIALIAS_EN
    bool passthru = true; // FILL/COPY: sin blender ni dither
    // Etapa de coverage de SET_OTHER_MODES. El RDP no guarda un alfa en el framebuffer:
    // guarda la COBERTURA del pixel (3 bits) en el sitio del alfa, y esos bits son los que
    // luego alimentan el filtro AA del VI y la propia mezcla del siguiente primitivo.
    u8   cvgDst = 0;          // CVG_DEST (bits 9:8): 0 clamp, 1 wrap, 2 zap, 3 save
    bool colorOnCvg = false;  // COLOR_ON_CVG (bit 7)
    bool cvgXAlpha  = false;  // CVG_TIMES_ALPHA (bit 12)
    bool alphaCvgSel= false;  // ALPHA_CVG_SELECT (bit 13)
    bool needMem = false;     // el pixel necesita leer el framebuffer (color y/o coverage)
  } blendPlan;
  // Alfa de shade del pixel en curso (mux A del blender = SHADE_ALPHA): alfa de shade
  // interpolado y acotado; 0 en primitivas sin shade. Lo pone drawTriangle por pixel.
  u8 pxShadeA = 0;
  // Delta-z de la primitiva en curso (potencia de dos) y su forma comprimida de 4 bits
  // (log2), que es la que se guarda junto al z y la que mueve los desplazamientos del
  // blender. Sin pendiente de z valen 1 y 0.
  int pxDz = 1;
  u8 pxDzC = 0;
  auto setPrimDz(bool fromPrim, s32 dzdx, s32 dzdy) -> void;
  auto buildBlendPlan() -> void;
  auto buildCombPlan() -> void;
  auto combineColorSlow(u32 tex0, u32 tex1, u32 shade) -> u32;   // referencia (y camino NOISE)
  auto combineColor(u32 tex0, u32 tex1, u32 shade) -> u32;
};

}  // namespace kestrel
