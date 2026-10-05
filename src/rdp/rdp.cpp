#include "../core/wrtag.hpp"
#include "rdp.hpp"
#include "../gpurdp/gpurdp.hpp"
#include <cstdlib>
#include "../core/memory.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#if defined(__SSE4_1__)
#include <smmintrin.h>
// Tabla del divisor del blender medida en hardware (parallel-rdp luts.hpp, vendorizado).
#include "../../third_party/parallel-rdp/parallel-rdp/luts.hpp"
#endif
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
#if defined(__SSE4_1__)
// RGBA de 32 bits a cuatro carriles de 16 bits y vuelta, para el filtro de textura. El pixel
// vive como R<<24|G<<16|B<<8|A, o sea que en memoria little-endian el byte 0 es el alpha: un
// solo `pshufb` reparte los bytes a carriles al desempaquetar y los junta al empaquetar, sin
// pasar por memoria ni por cuatro desplazamientos. Los rangos del filtro caben
// de sobra en int16 -- diferencia de texeles +-255, peso 0..32, producto +-8160, suma de dos
// +0x10 = +-16336 -- asi que el resultado entero es EL MISMO que en 32 bits, pero `pmullw`
// es una micro-op en este anfitrion (Nehalem) contra las seis de `pmulld`.
static inline auto unpackRgba16(u32 c) -> __m128i {
  const __m128i m = _mm_setr_epi8(3,-1, 2,-1, 1,-1, 0,-1, -1,-1,-1,-1, -1,-1,-1,-1);
  return _mm_shuffle_epi8(_mm_cvtsi32_si128((int)c), m);
}
static inline auto packRgba16Clamped(__m128i v) -> u32 {
  v = _mm_min_epi16(_mm_max_epi16(v, _mm_setzero_si128()), _mm_set1_epi16(255));
  const __m128i m = _mm_setr_epi8(6,4,2,0, -1,-1,-1,-1, -1,-1,-1,-1, -1,-1,-1,-1);
  return (u32)_mm_cvtsi128_si32(_mm_shuffle_epi8(v, m));
}

// Las dos coordenadas a punto fijo 10.5 de una vez. `trunc(v + copysign(0.5, v))` es
// exactamente el `floor(v + 0.5)` / `ceil(v - 0.5)` del escalar: para v >= 0 el argumento
// es > 0 (trunc == floor) y para v < 0 es < 0 (trunc == ceil). Mismo valor, sin ramas.
static inline auto stFixed(double s, double t) -> __m128i {
  const __m128d v = _mm_mul_pd(_mm_set_pd(t, s), _mm_set1_pd(32.0));
  const __m128d sgn = _mm_and_pd(v, _mm_castsi128_pd(_mm_set_epi32((int)0x80000000, 0,
                                                                  (int)0x80000000, 0)));
  return _mm_cvttpd_epi32(_mm_add_pd(v, _mm_or_pd(_mm_set1_pd(0.5), sgn)));
}
#endif

static const bool g_triDbg   = std::getenv("KESTREL_TRIDBG")   != nullptr;

// `std::lround` no se puede alinear: devuelve `long` y arrastra errno/dominio, asi que clang
// la deja como llamada a la CRT. En el perfil del hilo del RDP eso salia 6,7% (una llamada por
// pixel con Z interpolada, dos mas por pixel filtrado). Esta version da el MISMO bit para todo
// el rango que se usa aqui (coordenadas y Z, muy por debajo de 2^52, donde v+-0.5 es exacto):
// lround redondea el empate ALEJANDOSE del cero, que es justo floor(v+0.5) / ceil(v-0.5). Y
// floor/ceil si se alinean a `roundsd` con la linea base SSE4.1 del build.
[[maybe_unused]] static inline auto lroundExact(double v) -> int {
  return (int)(v < 0.0 ? std::ceil(v - 0.5) : std::floor(v + 0.5));
}


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
// KESTREL_RDPGUARD=<lo>:<hi>: chiva cualquier escritura del RDP a RDRAM dentro de ese
// rango fisico (hasta 40 veces). Sirve para probar si el rasterizador esta pisando codigo
// o heap del guest por un color/z image mal programado. Coste apagado = una comparacion
// contra un global que siempre esta en cache.
struct RdpGuardRange { u32 lo = 0, hi = 0; };
inline auto rdpGuardRange() -> RdpGuardRange {
  RdpGuardRange r;
  if(const char* e = std::getenv("KESTREL_RDPGUARD")) {
    char* q = nullptr; r.lo = (u32)std::strtoul(e, &q, 0);
    if(q && *q == ':') r.hi = (u32)std::strtoul(q + 1, nullptr, 0);
  }
  return r;
}
inline const RdpGuardRange g_rg = rdpGuardRange();
inline u32 g_rgLo = g_rg.lo, g_rgHi = g_rg.hi;
inline int g_rgN = 0;
inline auto rdpGuard(u32 p, u32 v, int nb) -> void {
  if(p < g_rgLo || p >= g_rgHi) return;
  if(++g_rgN > 40) return;
  std::fprintf(stderr, "[rdpguard] write%d phys=0x%06x v=0x%08x\n", nb, p, v);
  std::fflush(stderr);
}
template<typename V>
inline auto wr8(V& m, u32 p, u8 v) -> void {
  if(p >= m.size()) return;
  if(g_rgHi) rdpGuard(p, v, 8);
  wrtag::mark(p, wrtag::kRdp, 0);
  m[p] = v;
}
template<typename V>
inline auto wr16(V& m, u32 p, u16 v) -> void {
  if(p + 1 >= m.size()) return;
  if(g_rgHi) rdpGuard(p, v, 16);
  wrtag::markRange(p, 2, wrtag::kRdp, 0);
  m[p] = u8(v >> 8); m[p+1] = u8(v);
}
template<typename V>
inline auto wr32(V& m, u32 p, u32 v) -> void {
  if(p + 3 >= m.size()) return;
  if(g_rgHi) rdpGuard(p, v, 32);
  wrtag::markRange(p, 4, wrtag::kRdp, 0);
  m[p] = u8(v >> 24); m[p+1] = u8(v >> 16); m[p+2] = u8(v >> 8); m[p+3] = u8(v);
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
  // El bit 0 de un pixel RGBA5551 no es un alfa: es el BIT ALTO de la cobertura de 3 bits
  // que el RDP guarda por pixel (los otros dos viven en la RAM oculta de RDRAM). El byte
  // de alfa que llega aqui es esa cobertura desplazada a 7:5 (parallel-rdp
  // `write_color(u8x4(rgb, new_coverage << 5))` y la lectura inversa
  // `a = (hidden << 5) | ((word & 1) << 7)`), asi que el bit que va al framebuffer es el 7.
  return u16(((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | ((a >> 7) & 1));
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
// Anade [a, b) a la zona escrita: se funde con el intervalo que ya lo toca; si no, ocupa uno
// libre; sin libres, se funde con el que menos hueco anade. Solo crece: nunca deja fuera nada.
auto SoftRdp::wrAdd(u32 a, u32 b) -> void {
  u32 best = 0; u64 bestGrow = ~0ull;
  for(u32 i = 0; i < kWrSlots; i++) {
    if(wrLo[i] > wrHi[i]) { wrLo[i] = a; wrHi[i] = b; return; }
    if(a <= wrHi[i] && wrLo[i] <= b) { wrLo[i] = std::min(wrLo[i], a); wrHi[i] = std::max(wrHi[i], b); return; }
    const u64 grow = (u64)std::max(wrHi[i], b) - std::min(wrLo[i], a) - (wrHi[i] - wrLo[i]);
    if(grow < bestGrow) { bestGrow = grow; best = i; }
  }
  wrLo[best] = std::min(wrLo[best], a); wrHi[best] = std::max(wrHi[best], b);
}

// Una linea de primitiva. Lo que cuenta depende del tipo de ciclo:
//  * FILL/COPY no pasan por el span buffer: escriben directo a RDRAM en palabras de 64 bits
//    alineadas, una por GCLK (n64brew "Reality Display Processor/Pipeline": "Writes are
//    committed straight to RDRAM without passing through the span buffers", "Pixels are
//    written out 64-bits ... at a time"; manual de programacion: 4 px de 16 bits por ciclo).
//    La unidad es la palabra de 64 bits que toca la linea, contando la alineacion.
//  * 1/2 ciclos: la unidad es el chunk del span buffer (T_CHUNK pixeles). Un chunk no
//    cruza de una linea a la siguiente, asi que una linea de 2 pixeles paga un chunk entero.
// En los dos casos cada linea paga ademas 1 GCLK de pipeline muerto (n64brew: "1 dead cycle
// at the end of every line in a primitive where the pipeline is cycled but no pixel is
// output").
auto SoftRdp::addSpan(int x0, u64 npx) -> void {
  if(!npx) return;
  spanLines++;
  if(cycleType() >= 2) {
    const u64 bpp = ci_size == 3 ? 4u : ci_size == 2 ? 2u : 1u;
    const u64 a = u64(std::max(x0, 0)) * bpp, b = a + npx * bpp;
    spanUnits += (b + 7) / 8 - a / 8;
  } else {
    spanUnits += (npx + T_CHUNK - 1) / T_CHUNK;
  }
}

auto SoftRdp::accountPixels(Memory& mem, u64 npx, u64 nWrite, u64 nZWrite) -> void {
  acctFinish(mem, acctSnap(mem, npx), nWrite, nZWrite);
}

// Foto del estado que decide el coste de una primitiva, tomada al rasterizarla. La parte que
// depende de cuantos pixeles escribio (acctFinish) puede llegar mas tarde: con el GPU-RDP esos
// numeros los devuelve la GPU al vaciar la cola, y para entonces el estado ya puede ser otro.
auto SoftRdp::acctSnap(Memory& mem, u64 npx) -> AcctSnap {
  AcctSnap a;
  a.npx = npx; a.lines = spanLines; a.units = spanUnits;
  spanLines = spanUnits = 0;
  a.ci = ci_addr; a.zi = zi_addr; a.ciW = ci_width; a.ciSize = ci_size;
  a.sx1 = sx1; a.sy1 = sy1; a.olo = other_lo; a.cyc = cycleType();
  a.viOn = (mem.rcp.vi_ctrl & 3) != 0 && mem.rcp.vi_origin != 0;
  a.fbviSame = a.viOn && (ci_addr >> 20) == ((mem.rcp.vi_origin & 0x00ff'ffff) >> 20);
  a.charge = charge;
  return a;
}

auto SoftRdp::acctFinish(Memory& mem, const AcctSnap& a, u64 nWrite, u64 nZWrite) -> void {
  const u64 npx = a.npx, lines = a.lines, units = a.units;
  const u32 ci_addr = a.ci, zi_addr = a.zi, ci_width = a.ciW, ci_size = a.ciSize, other_lo = a.olo;
  const int sx1 = a.sx1, sy1 = a.sy1;
  const u32 cyc = a.cyc;
  if(!npx) return;
  {
    // Zona escrita (ver wrLo): 4 bytes por pixel sea cual sea el formato; pasarse solo cuesta
    // esperas de mas.
    const u32 w = (u32)std::max<int>((int)ci_width, sx1) + 1, h = (u32)std::max(sy1, 0) + 1;
    const u32 bytes = w * h * 4;
    if(nWrite) wrAdd(ci_addr, ci_addr + bytes);
    if(nZWrite && zi_addr) wrAdd(zi_addr, zi_addr + bytes);
  }
  bool fbRead = (other_lo & 0x40) != 0;                  // IM_RD
  bool zRead  = (other_lo & 0x10) != 0 && zi_addr != 0;  // Z_CMP
  bool zWrite = nZWrite != 0;
  bool fbzbSame = zi_addr != 0 && (ci_addr >> 20) == (zi_addr >> 20);
  const bool viOn = a.viOn, fbviSame = a.fbviSame;

  double cycles;
  if(cyc >= 2) {
    // FILL/COPY: una palabra de 64 bits por GCLK mas el ciclo muerto de cada linea; el VI
    // sigue robando bus igual que a los chunks (mismo T_VI calibrado). Sin tramos
    // declarados, 64 bits = 8 bytes de la anchura de pixel del color image.
    const u64 bpp = ci_size == 3 ? 4u : ci_size == 2 ? 2u : 1u;
    double gclk = lines ? double(units + lines) : double(npx * bpp) / 8.0;
    cycles = gclk * (viOn ? 1.0 + T_VI : 1.0);
  } else {
  // Chunks: los declarados por linea, o el reparto plano. El pipeline de un chunk es el de
  // sus pixeles medios mas el sobrecoste fijo y su parte del ciclo muerto de fin de linea.
  const double chunks = lines ? double(units) : double(npx) / T_CHUNK;
  double pipeline = double(cyc + 1) * (double(npx) / chunks) + T_CHUNKOVH
                  + double(lines) / chunks;
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
  cycles = (frac * wrote + (1.0 - frac) * killed) * chunks;
  }
  u32 c = (u32)(u64)cycles;
  if(!a.charge) return;   // el paseo solo-coste de este tramo ya lo pago
  if(auto& st = mem.rdpStats; st.on.load(std::memory_order_relaxed)) {
    const auto r = std::memory_order_relaxed;
    (cyc == 3 ? st.gclkFill : st.gclkPixel).fetch_add(c, r);
    st.px[cyc].fetch_add(npx, r);
    st.pxWritten.fetch_add(nWrite, r);
    if(fbRead) st.pxImRd.fetch_add(npx, r);
    if(zRead)  st.pxZCmp.fetch_add(npx, r);
    st.pxZUpd.fetch_add(nZWrite, r);
  }
  // Ocupacion del bus de RDRAM. Se deriva de la MISMA lista de transacciones que acaba de
  // usar el modelo de coste, asi que no hay ninguna constante nueva que calibrar: cada
  // transaccion mueve un chunk entero del buffer al que apunta (T_CHUNK pixeles del ancho
  // que toque). Un pixel matado hace las lecturas pero ninguna escritura, igual que arriba.
  {
    const u64 ciBpp = ci_size == 3 ? 4u : ci_size == 2 ? 2u : 1u;
    const u64 rdB = (fbRead ? ciBpp : 0) + (zRead ? 2u : 0);
    const u64 wrB = ciBpp + (zWrite ? 2u : 0);
    double bytes = (double(nWrite) * double(rdB + wrB) + double(npx - nWrite) * double(rdB));
    mem.ramBytesRdp.fetch_add((u64)bytes, std::memory_order_relaxed);
  }
  mem.rcp.dpc_pipebusy.fetch_add(c, std::memory_order_relaxed);
  mem.rcp.dpc_bufbusy.fetch_add(c, std::memory_order_relaxed);
  // Publicacion para el regulador (ver Memory::rdpPace). release: el freno de la CPU lee
  // este contador para decidir cuanto puede avanzar, y tiene que ver los pixeles ya escritos.
  mem.rcp.rdpGclk.fetch_add(c, std::memory_order_release);
}

// Cargas de TMEM: puerto de texturas de 64 bits, 8 bytes por GCLK, y un coste FIJO por
// rafaga de RDRAM (arranque del comando + latencia de fila). LOAD_BLOCK y LOAD_TLUT son una
// sola rafaga lineal; LOAD_TILE abre una por FILA, porque el origen salta `ti_width` entre
// filas y la rafaga se rompe: por eso las filas cortas le salen tan caras.
//   LOAD_BLOCK  ~ 14 + ceil(bytes/8)
//   LOAD_TILE   ~ filas * (14 + ceil(bytes_fila/8))
//   LOAD_TLUT   ~ 14 + ceil(entradas*2/8)   (CI4 16 entradas ~18, CI8 256 ~78)
// Las cifras son la especificacion que dio el usuario (2026-10-04), NO una medida en consola:
// n64brew solo da ciclos de los SYNC y Thar0 solo mide rellenos (ver PD64_pending P2). El fijo
// se puede mover con KESTREL_TMEMSETUP=<gclk>; =0 vuelve al modelo anterior (solo
// transferencia) para el A/B.
static u32 tmemSetup() {
  static const u32 v = []{ const char* e = std::getenv("KESTREL_TMEMSETUP");
                           return (e && *e) ? (u32)std::strtoul(e, nullptr, 10) : 14u; }();
  return v;
}

auto SoftRdp::accountLoad(Memory& mem, u64 rows, u64 bytesPerRow) -> void {
  accountTmem(mem, rows * bytesPerRow, rows * (tmemSetup() + (bytesPerRow + 7) / 8));
}

auto SoftRdp::accountTmem(Memory& mem, u64 bytes, u64 gclk) -> void {
  u32 c = (u32)gclk;
  if(!charge) return;   // el paseo solo-coste de este tramo ya lo pago
  if(mem.rdpStats.on.load(std::memory_order_relaxed)) {
    mem.rdpStats.gclkTmem.fetch_add(c, std::memory_order_relaxed);
    mem.rdpStats.loadBytes.fetch_add(bytes, std::memory_order_relaxed);
  }
  mem.ramBytesRdp.fetch_add(bytes, std::memory_order_relaxed);
  mem.rcp.dpc_tmem.fetch_add(c, std::memory_order_relaxed);
  mem.rcp.dpc_bufbusy.fetch_add(c, std::memory_order_relaxed);
  mem.rcp.rdpGclk.fetch_add(c, std::memory_order_release);
}

// SYNC_LOAD/PIPE/TILE paran el pipeline un numero FIJO de GCLK (n64brew, RDP Commands:
// "stalls the RDP pipeline for exactly 25/50/33 GCLK cycles", "does not wait on any
// particular internal signal(s)": uno redundante paga el coste entero). Durante la parada el
// comando sigue en el FIFO (CMD_BUSY -> dpc_bufbusy) y el pipeline esta activo hasta el
// SYNC_FULL (PIPE_BUSY), pero no hay carga de TMEM ni trafico de RDRAM.
auto SoftRdp::accountStall(Memory& mem, u32 gclk) -> void {
  if(!charge) return;   // el paseo solo-coste de este tramo ya lo pago
  if(mem.rdpStats.on.load(std::memory_order_relaxed))
    mem.rdpStats.gclkSync.fetch_add(gclk, std::memory_order_relaxed);
  mem.rcp.dpc_pipebusy.fetch_add(gclk, std::memory_order_relaxed);
  mem.rcp.dpc_bufbusy.fetch_add(gclk, std::memory_order_relaxed);
  mem.rcp.rdpGclk.fetch_add(gclk, std::memory_order_release);
}

// rdp.stats (ver RdpStats): cuenta el comando y mira la redundancia contra el estado de
// ANTES de ejecutarlo. Solo lo llama el paseo que cobra, con las estadisticas encendidas.
auto SoftRdp::statsCmd(Memory& mem, u32 op, u64 cmd) -> void {
  auto& st = mem.rdpStats;
  const auto r = std::memory_order_relaxed;
  st.op[op].fetch_add(1, r);
  if((op >= 0x08 && op <= 0x0f) || op == 0x24 || op == 0x25 || op == 0x36) {
    st.primSince[0] = st.primSince[1] = st.primSince[2] = true;
    st.workSince[0] = st.workSince[1] = st.workSince[2] = true;
  } else if(op == 0x30 || op == 0x33 || op == 0x34) {   // LOAD_TLUT / BLOCK / TILE
    st.workSince[0] = st.workSince[1] = st.workSince[2] = true;
  } else if(op >= 0x26 && op <= 0x28) {          // SYNC_LOAD / PIPE / TILE
    const u32 k = op == 0x26 ? 0 : op == 0x27 ? 1 : 2;
    if(!st.primSince[k]) st.syncRedundant[k].fetch_add(1, r);
    if(!st.workSince[k]) st.syncPure[k].fetch_add(1, r);
    st.primSince[k] = st.workSince[k] = false;
  } else if(op == 0x29) {                        // SYNC_FULL drena todo
    st.primSince[0] = st.primSince[1] = st.primSince[2] = false;
    st.workSince[0] = st.workSince[1] = st.workSince[2] = false;
  } else if(op == 0x2f) {
    if(((u32)(cmd >> 32) & 0x00ff'ffff) == other_hi && (u32)cmd == other_lo) st.otherModesSame.fetch_add(1, r);
  } else if(op == 0x3c) {
    if(((u32)(cmd >> 32) & 0x00ff'ffff) == combine_hi && (u32)cmd == combine_lo) st.combineSame.fetch_add(1, r);
  }
}

// Per-pixel depth test against the 16-bit z image. Opaque z-mode: the pixel wins
// when its depth is nearer (strictly less) than the stored depth. On a pass with
// Z_UPDATE the new depth is written back. Returns whether the colour is drawn.
// Solo el Z_CMP, sin escribir (lo usa el paseo solo-coste).
auto SoftRdp::depthPasses(Memory& mem, int x, int y, s32 d) const -> bool {
  const auto& m = mem.rdram;
  if(d < 0) d = 0; else if(d > 0x3ffff) d = 0x3ffff;
  u32 zoff = zi_addr + (u32(y) * ci_width + u32(x)) * 2;
  if(zoff + 1 >= m.size()) return false;
  u32 old = zDecode(((u16)m[zoff] << 8) | m[zoff + 1]);
  return !((other_lo & 0x10) && (u32)d >= old);
}

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

// RAM oculta de RDRAM. Cada chip RDRAM es de 9 bits: 8 de datos y 1 que la CPU no puede
// direccionar. El RCP usa esa novena linea para guardar, por cada pixel de 16 bits, los 2
// bits bajos de su cobertura (y por cada palabra del z-buffer, el delta-z comprimido). Sin
// ella la cobertura solo tendria el bit que cabe en el pixel y el filtro AA del VI no
// podria distinguir un borde a 1/8 de uno a 7/8. Se dimensiona a la primera y sigue a
// RDRAM: una entrada por palabra de 16 bits.
auto SoftRdp::hiddenBits(Memory& mem) -> u8* {
  const size_t want = mem.rdram.size() >> 1;
  if(mem.rdramHidden.size() != want) mem.rdramHidden.assign(want, 0);
  return mem.rdramHidden.data();
}

auto SoftRdp::putPixel(Memory& mem, int x, int y, u32 rgba32) -> void {
  if(x < sx0 || x >= sx1 || y < sy0 || y >= sy1) return;
  if(x < 0 || y < 0) return;
  storePixel(mem, x, y, rgba32);
}

auto SoftRdp::storePixel(Memory& mem, int x, int y, u32 rgba32) -> void {
  pxWrites++;         // DPC counters: this pixel reaches the color image
  auto& m = mem.rdram;
  if(ci_size == 3) {  // 32bpp RGBA8888
    wr32(m, ci_addr + (u32(y) * ci_width + u32(x)) * 4, rgba32);
  } else if(ci_size == 1) {   // 8bpp colour-index framebuffer: store the low byte
    wr8(m, ci_addr + (u32(y) * ci_width + u32(x)), (u8)(rgba32 & 0xff));
  } else {            // 16bpp RGBA5551
    const u32 a = ci_addr + (u32(y) * ci_width + u32(x)) * 2;
    wr16(m, a, to5551(rgba32));
    // ...y los dos bits bajos de la cobertura a la RAM oculta, en la misma palabra.
    if(a + 1 < m.size()) hiddenBits(mem)[a >> 1] = (u8)((rgba32 >> 5) & 3);
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
  u32 b = ((px >> 1) & 0x1f) << 3;
  // El "alfa" del color de memoria es la cobertura guardada, no una transparencia: bit alto
  // en el bit 0 del pixel, dos bits bajos en la RAM oculta, y el conjunto colocado en 7:5
  // (parallel-rdp `decode_memory_color`). Es lo que consume el mux B = MEM_ALPHA del
  // blender y lo que decide el desbordamiento de cobertura del pixel entrante.
  u32 al = ((((px & 1) << 2) | (hiddenBits(mem)[a >> 1] & 3)) << 5);
  return (r << 24) | (g << 16) | (b << 8) | al;
}

auto SoftRdp::buildBlendPlan() -> void {
  // 1-cycle evalua la config del PRIMER ciclo del blender (GBL_c1: m1a<<30, m1b<<26,
  // m2a<<22, m2b<<18); en 2-cycle la escritura final usa el SEGUNDO (<<28/24/20/16).
  BlendPlan p;
  const int sh = (cycleType() == 1) ? 0 : 2;
  p.psel = (u8)((other_lo >> (28 + sh)) & 3);
  p.asel = (u8)((other_lo >> (24 + sh)) & 3);
  p.msel = (u8)((other_lo >> (20 + sh)) & 3);
  p.bsel = (u8)((other_lo >> (16 + sh)) & 3);
  p.two   = cycleType() == 1;
  p.psel0 = (u8)((other_lo >> 30) & 3);
  p.asel0 = (u8)((other_lo >> 26) & 3);
  p.msel0 = (u8)((other_lo >> 22) & 3);
  p.bsel0 = (u8)((other_lo >> 18) & 3);
  p.usesMem  = (p.msel == 1) || (p.bsel == 1) ||
               (p.two && (p.psel0 == 1 || p.msel0 == 1 || p.bsel0 == 1));
  p.imRd     = (other_lo & 0x40) != 0;
  p.force    = ((other_lo >> 14) & 1) != 0;
  p.aaEn     = (other_lo & 0x08) != 0 && !g_noAA;
  p.passthru = cycleType() >= 2;
  p.dither   = (u8)((other_hi >> 6) & 3);
  p.cvgDst      = (u8)((other_lo >> 8) & 3);
  p.colorOnCvg  = (other_lo & 0x80) != 0;
  p.cvgXAlpha   = ((other_lo >> 12) & 1) != 0;
  p.alphaCvgSel = ((other_lo >> 13) & 1) != 0;
  // Leer el framebuffer cuesta una lectura de RDRAM por pixel, asi que solo se hace cuando
  // algo la consume: el mux (CLR_MEM / MEM_alpha), COLOR_ON_CVG, o la cobertura de memoria
  // -- que hace falta para el desbordamiento (AA_EN) y para todos los CVG_DEST menos ZAP.
  // Con Z_CMP tambien: la prueba de profundidad decide "misma superficie" con el desborde.
  p.needMem  = p.usesMem || p.colorOnCvg || p.aaEn || p.cvgDst != 2 || (other_lo & 0x10);
  p.keyHi = other_hi; p.keyLo = other_lo;
  blendPlan = p;
}

auto SoftRdp::blendColor(u32 src, u32 memc, bool blendEn, bool cvgWrap, int shA, int shB) -> u32 {
  // Blend mux from the render-mode word (other_lo). 1-cycle mode evaluates the FIRST
  // blender cycle's config (GBL_c1: m1a<<30, m1b<<26, m2a<<22, m2b<<18); 2-cycle mode's
  // final write uses the SECOND cycle (GBL_c2: <<28/24/20/16). P/M pick a colour
  // (IN/MEM/BLEND/FOG), A picks a coefficient, B picks the second coefficient.
  if(blendPlan.keyHi != other_hi || blendPlan.keyLo != other_lo) buildBlendPlan();
  const BlendPlan& bp = blendPlan;
  const int Psel = bp.psel, Asel = bp.asel, Msel = bp.msel, Bsel = bp.bsel;
  auto pick = [&](int sel) -> u32 {   // P/M colour mux: IN / MEM / BLEND / FOG
    switch(sel) { case 0: return src; case 1: return memc; case 2: return blend_color; default: return fog_color; }
  };
  // 2-cycle: el ciclo 0 corre siempre, sin FORCE_BLEND ni blend_en ni atajos, con >>5 directo
  // (sin divisor); su RGB pasa a ser el IN del ciclo 1 y el alfa del pixel no cambia
  // (parallel-rdp `blender(..., final_cycle=false)`; angrylion blender_equation_cycle0_2).
  // Es la fase de niebla de G_RM_FOG_SHADE_A: (FOG, SHADE_A, IN, 1MA).
  if(bp.two) {
    const u32 P0 = pick(bp.psel0), M0 = pick(bp.msel0);
    int b0;
    switch(bp.asel0) { case 0: b0 = src & 0xff; break; case 1: b0 = fog_color & 0xff; break;
                       case 2: b0 = pxShadeA; break; default: b0 = 0; }
    int b1;
    switch(bp.bsel0) { case 0: b1 = (~b0) & 0xff; break; case 1: b1 = memc & 0xff; break;
                       case 2: b1 = 0xff; break; default: b1 = 0; }
    b0 >>= 3; b1 >>= 3;
    if(bp.bsel0 == 1) { b0 = (b0 >> shA) & 0x3c; b1 = (b1 >> shB) | 3; }
    u32 c0 = src & 0xff;
    for(int i = 0; i < 3; i++) {
      const int sh = 24 - i * 8;
      const int v = ((int)((P0 >> sh) & 0xff) * b0 + (int)((M0 >> sh) & 0xff) * (b1 + 1)) >> 5;
      c0 |= (u32)(v & 0xff) << sh;
    }
    src = c0;
  }
  u32 P = pick(Psel), M = pick(Msel);
  // COLOR_ON_CVG: cuando la cobertura del pixel NO desborda, el ciclo final del blender
  // devuelve el color M tal cual, sin mirar coeficientes ni siquiera si el blender esta
  // encendido (parallel-rdp `blender()`: el retorno va ANTES de la prueba de blend_en).
  // Es como los juegos pintan "solo donde el borde no esta lleno".
  if(bp.colorOnCvg && !cvgWrap) return (M & ~0xffu) | (src & 0xff);
  int a0;                             // A mux: IN alpha / FOG alpha / SHADE alpha / 0
  switch(Asel) { case 0: a0 = src & 0xff; break; case 1: a0 = fog_color & 0xff; break;
                 case 2: a0 = pxShadeA; break; default: a0 = 0; }
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
  // B = MEM_ALPHA: los coeficientes pasan por los desplazadores que fija la etapa de
  // profundidad (diferencia de delta-z entre pixel y memoria, 0..4), y el de memoria nunca
  // baja de 3 (parallel-rdp `blender`: `a0 = (a0 >> shift.x) & 0x3c; a1 = (a1 >> shift.y) | 3`).
  if(Bsel == 1) { a0 = (a0 >> shA) & 0x3c; a1 = (a1 >> shB) | 3; }
  const bool force = bp.force;
  // FORCE_BLEND takes the plain >>5; otherwise the RDP runs the sum through its divider,
  // normalising by the actual coefficient weight (a0 + a1 + 1) rather than by a fixed 32.
  int sum = (a0 >> 2) + (a1 >> 2) + 1;
  // El divisor NO es una division entera: con `sum` en 1..15 y el numerador enmascarado a
  // 11 bits, el hardware da n/d solo en el caso normal (pesos que suman <= 32). Cuando los
  // desplazadores de MEM_ALPHA desequilibran los pesos el resultado desborda o sale
  // "raro", y la unica descripcion fiel es la tabla medida (parallel-rdp
  // `uBlenderDividerLUT`, indice (sum << 11) | n). FORCE_BLEND toma >>5 en 8 bits, que
  // tambien da la vuelta en vez de saturar.
  auto ch = [](u32 c, int i) -> int { return (int)((c >> (24 - i * 8)) & 0xff); };
  u32 out = 0;
  for(int i = 0; i < 3; i++) {
    int blended = ch(P, i) * a0 + ch(M, i) * (a1 + 1);
    int v = force ? ((blended >> 5) & 0xff) : (int)RDP::blender_lut[(sum << 11) | ((blended >> 2) & 0x7ff)];
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
static const u8 kDitherMatrix[2][16] = {
  { 0, 6, 1, 7, 4, 2, 5, 3, 3, 5, 2, 4, 7, 1, 6, 0 },   // magic square
  { 0, 4, 1, 5, 4, 0, 5, 1, 3, 7, 2, 6, 7, 3, 6, 2 },   // standard Bayer
};

auto SoftRdp::ditherRgb(int x, int y, u32 c) const -> u32 {
  const auto& kMatrix = kDitherMatrix;
  const u32 mode = blendPlan.dither;   // el llamador ya valido el plan (blendPixel)
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

// Dither de alfa (SET_OTHER_MODES ALPHA_DITHER_SEL, bits 37:36 -> other_hi bits 5:4):
//   0 = patron, 1 = patron invertido (~d & 7), 2 = ruido, 3 = apagado.
// El patron es la matriz del modo RGB (magic square o Bayer; con RGB en ruido/apagado,
// la del bit bajo del modo RGB). Se SUMA (0..7) al alfa expandido de la salida del
// combinador cuando no hay ALPHA_CVG_SELECT, al alfa de referencia del alpha compare y al
// alfa de shade que ve el blender (parallel-rdp `dither_coefficients`, `combiner_cycle1`,
// `shading.h shade_alpha`).
auto SoftRdp::alphaDither(int x, int y) const -> int {
  const u32 am = (u32)(other_hi >> 4) & 3, rm = (u32)(other_hi >> 6) & 3;
  if(am == 3) return 0;
  if(am == 2) {
    // Ruido: mismo hash por pixel que el dither RGB de ruido (canal 3), reproducible.
    u32 h = (u32)x * 0x9e3779b1u ^ (u32)y * 0x85ebca6bu ^ 3u * 0xc2b2ae35u;
    h ^= h >> 15; h *= 0x2545f491u; h ^= h >> 13;
    return (int)(h & 7);
  }
  int d = kDitherMatrix[rm & 1][(y & 3) * 4 + (x & 3)];
  return am == 1 ? (~d & 7) : d;
}

auto SoftRdp::setPrimDz(bool fromPrim, s32 dzdx, s32 dzdy) -> void {
  // Delta-z por pixel de la primitiva (parallel-rdp `build_derived_attributes`): con
  // Z_SOURCE_SEL el de SET_PRIM_DEPTH tal cual; si no, |DzDx| + |DzDy| en enteros (el
  // negativo en complemento a uno, 15 bits) llevado a la potencia de dos SIGUIENTE, con
  // tope 0x8000. La forma comprimida es su log2.
  int dz;
  if(fromPrim) dz = (int)prim_dz;
  else {
    const s32 dx = dzdx >> 16, dy = dzdy >> 16;
    dz = (dx < 0 ? (~dx & 0x7fff) : dx) + (dy < 0 ? (~dy & 0x7fff) : dy);
    dz = dz >= 0x8000 ? 0x8000 : dz == 0 ? 1 : 1 << (32 - __builtin_clz((u32)dz));
  }
  pxDz = dz;
  pxDzC = (u8)(dz > 0 ? 31 - __builtin_clz((u32)dz) : 0);
}

auto SoftRdp::blendPixel(Memory& mem, int x, int y, u32 src, int cvg, const s32* z) -> void {
  if(x < sx0 || x >= sx1 || y < sy0 || y >= sy1 || x < 0 || y < 0) return;
  // The blender runs in every 1-/2-cycle primitive — it is not gated on IM_RD. IM_RD
  // (bit 0x40) only enables READS of the framebuffer, i.e. it matters solely when the mux
  // selects CLR_MEM (M) or MEM_alpha (B). When the blender references memory but reads are
  // disabled, hardware writes the pipeline colour straight through; otherwise the blender
  // evaluates with memc=0 (its memory inputs are never consulted). COPY/FILL bypass it.
  if(blendPlan.keyHi != other_hi || blendPlan.keyLo != other_lo) buildBlendPlan();
  const BlendPlan& bp = blendPlan;
  if(g_noBlend || bp.passthru) {   // FILL/COPY: no blender, no dither
    if(z && !depthTest(mem, x, y, *z)) return;
    storePixel(mem, x, y, src); return;
  }
  if(bp.usesMem && !bp.imRd) {
    if(z && !depthTest(mem, x, y, *z)) return;
    storePixel(mem, x, y, ditherRgb(x, y, src)); return;
  }

  // --- color y cobertura de memoria -------------------------------------------------
  // IM_RD deshabilitado no significa "cobertura cero": el hardware entrega 7 (pixel lleno),
  // que es lo que hace que un primitivo sin lecturas se comporte como opaco
  // (parallel-rdp `decode_memory_color`: `image_read_en ? current.a : 0xe0`).
  u32 memc = 0; int memCvg = 7;
  if(bp.imRd && bp.needMem) { memc = readFb(mem, x, y); memCvg = (int)((memc & 0xff) >> 5); }

  // --- salida de alfa del combinador: CVG_TIMES_ALPHA / ALPHA_CVG_SELECT ------------
  // El RDP expande 0xff a 0x100 para poder dividir por potencias de dos, multiplica la
  // cobertura por el alfa cuando CVG_TIMES_ALPHA (y ESA es la cobertura que se guarda), y
  // con ALPHA_CVG_SELECT sustituye el alfa del pixel por la cobertura modulada.
  {
    int a = (int)(src & 0xff);
    int expanded = a + ((a + 1) >> 8);
    int modulated;
    if(bp.cvgXAlpha) { modulated = (expanded * cvg + 4) >> 3; cvg = modulated >> 5; }
    else             { modulated = cvg << 5; }
    if(bp.alphaCvgSel) expanded = modulated;
    else               expanded += alphaDither(x, y);
    src = (src & ~0xffu) | (u32)(expanded < 0 ? 0 : expanded > 255 ? 255 : expanded);
  }
  // Un pixel sin cobertura no existe. Solo con antialias encendido: con AA apagado el
  // hardware ya decidio la vida del pixel con una sola toma en el recorte del tramo.
  if(bp.aaEn && cvg == 0) return;

  // --- etapa de profundidad (parallel-rdp `depth_test`) -----------------------------
  // Desbordar (cobertura entrante + la que ya hay >= 8) significa que el pixel es de OTRA
  // superficie, no del mismo borde. Con Z_CMP el z guardado lleva su delta-z (2 bits en
  // la palabra + 2 en la RAM oculta), y "misma superficie" es estar dentro de la suma de
  // las dos pendientes: asi el borde compartido de dos triangulos vecinos pasa la prueba y
  // se mezcla en vez de perderse por un "menor estricto".
  auto& m = mem.rdram;
  const bool zCmp = z && (other_lo & 0x10), zUpd = z && (other_lo & 0x20);
  u32 zoff = 0;
  s32 zz = 0;
  if(z) {
    zoff = zi_addr + (u32(y) * ci_width + u32(x)) * 2;
    if(zoff + 1 >= m.size()) return;
    zz = std::clamp(*z, 0, 0x3ffff);
  }
  const bool overflow = (cvg + memCvg) >= 8;
  bool blendEn;
  int shA = 0, shB;
  if(zCmp) {
    const u16 zw = (u16)(((u16)m[zoff] << 8) | m[zoff + 1]);
    const s32 memZ = (s32)zDecode(zw);
    const int memDzC = (int)(((zw & 3) << 2) | (hiddenBits(mem)[zoff >> 1] & 3));
    int memDz = 1 << memDzC;
    const int prec = (zw >> 13) & 7;   // exponente del z comprimido
    shA = std::clamp(pxDzC - memDzC, 0, 4);
    shB = std::clamp(memDzC - pxDzC, 0, 4);
    // Con poca precision guardada (exponente < 3) el delta-z de memoria se ensancha; el
    // maximo (0x8000) marca "coplanar" y pasa siempre.
    bool coplanar = false;
    if(prec < 3) {
      if(memDz != 0x8000) memDz = std::max(memDz << 1, 16 >> prec);
      else { coplanar = true; memDz = 0xffff; }
    }
    int cdz = pxDz | memDz;
    cdz = cdz ? 1 << (31 - __builtin_clz((u32)cdz)) : 0;
    const int cdzI = cdz;
    cdz <<= 3;
    const bool farther = coplanar || zz + cdz >= memZ;
    blendEn = bp.force || (!overflow && bp.aaEn && farther);
    const bool maxZ = memZ == 0x3ffff, front = zz < memZ;
    const bool nearer = coplanar || zz - cdz <= memZ;
    bool pass;
    switch((other_lo >> 10) & 3) {
      case 0:   // OPAQUE: misma superficie si esta cerca; si desborda, delante estricto
        pass = maxZ || (overflow ? front : nearer); break;
      case 1:   // INTERPENETRATING: en el cruce de dos superficies recorta la cobertura
        if(!front || !farther || !overflow) pass = maxZ || (overflow ? front : nearer);
        else {
          const int c = (cdzI & 0xffff) ? 31 - __builtin_clz((u32)(cdzI & 0xffff)) : 0;
          const int coeff = ((memZ >> c) - (zz >> c)) & 0xf;
          cvg = std::min((coeff * cvg) >> 3, 8);
          pass = true;
        }
        break;
      case 2:   // TRANSPARENT: delante estricto
        pass = front || maxZ; break;
      default:  // DECAL: dentro de la tolerancia por los dos lados
        pass = farther && nearer && !maxZ; break;
    }
    if(!pass || (bp.aaEn && cvg == 0)) return;
  } else {
    shB = std::min(0xf - (int)pxDzC, 4);
    blendEn = bp.force || (!overflow && bp.aaEn);
  }

  // --- blender ----------------------------------------------------------------------
  u32 out = blendColor(src, memc, blendEn, overflow, shA, shB);
  out = ditherRgb(x, y, out);

  // --- cobertura de salida: CVG_DEST -------------------------------------------------
  // CLAMP suma coberturas cuando el blender esta encendido (mismo borde) y si no guarda
  // cvg-1; WRAP suma en modulo 8; ZAP fuerza lleno; SAVE conserva la que hubiera.
  int newCvg;
  switch(bp.cvgDst) {
    case 1:  newCvg = (cvg + memCvg) & 7; break;                       // WRAP
    case 2:  newCvg = 7; break;                                        // ZAP
    case 3:  newCvg = memCvg; break;                                   // SAVE
    default: newCvg = blendEn ? std::min(7, memCvg + cvg) : ((cvg - 1) & 7); break;  // CLAMP
  }
  storePixel(mem, x, y, (out & ~0xffu) | (u32)(newCvg << 5));
  // Z_UPD: el z comprimido en 15:2, los 2 bits altos del delta-z en 1:0 y los 2 bajos en
  // la RAM oculta de la misma palabra.
  if(zUpd) {
    wr16(m, zoff, (u16)(zEncode((u32)zz) | ((pxDzC >> 2) & 3)));
    hiddenBits(mem)[zoff >> 1] = pxDzC & 3;
    pxZWrites++;
  }
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
  setPrimDz(other_lo & 4, 0, 0);   // un rect no tiene pendiente de z
  u64 npx = u64(std::max(0, x1 - x0)) * u64(std::max(0, y1 - y0));
  u64 w0 = pxWrites, z0 = pxZWrites;
  // Solo-coste (ver SoftRdp::costOnly): el area ya esta recortada al scissor, que es
  // exactamente lo que entra al pipeline. Un relleno solo toca el z si corre en 1/2 ciclos
  // con Z_UPDATE; en FILL/COPY el z ni se mira.
  if(costOnly) {
    // Con Z de primitiva (Z_SOURCE_SEL) y Z_CMP el relleno se come pixeles: los que fallan
    // no escriben ni color ni z. La z es constante, asi que el test se hace de verdad y solo
    // leyendo el z-buffer (el paseo solo-coste va antes que el que pinta; ningun pixel del
    // rect se toca dos veces). Sin esto un relleno con Z que falla se cobraba como si pasara
    // (Thar0 "Z Fail": +2,7 cyc/px).
    // Lo mismo con el alpha compare: el color del rect es constante, se evalua una vez igual
    // que en el camino que pinta (Thar0 "Alpha Compare": +0,74 cyc/px).
    u64 pass = npx;
    if(pipeMode && (other_lo & 1)) {
      bool combProg = (combine_hi | combine_lo) != 0;
      u32 flatTexel = combProg ? 0xffffffff : 0;
      u32 c = combProg ? combineColor(flatTexel, flatTexel, 0) : blend_color;
      if((c & 0xff) < (blend_color & 0xff)) pass = 0;
    }
    if(pass && pipeMode && (other_lo & 4) && (other_lo & 0x10) && zi_addr) {
      pass = 0;
      for(int y = y0; y < y1; y++)
        for(int x = x0; x < x1; x++) pass += depthPasses(mem, x, y, (s32)prim_z);
    }
    for(int y = y0; y < y1 && x1 > x0; y++) addSpan(x0, u64(x1 - x0));
    accountPixels(mem, npx, pass, (pipeMode && (other_lo & 0x20) && zi_addr) ? pass : 0);
    return;
  }
  // GPU-RDP (KESTREL_GPURDP=1): en FILL/COPY el relleno es escritura pura y se encola en la
  // GPU. Solo cuando el resultado es el mismo que el bucle de abajo sin casos raros: todo
  // dentro de la RDRAM (los wrN de abajo descartan pixeles sueltos), x1 dentro del ancho (si
  // no, el pixel x >= ancho cae en la fila siguiente), base alineada al pixel, y sin las
  // herramientas de depuracion que miran cada escritura (rdpGuard, wrtag).
  if(!pipeMode && x1 > x0 && y1 > y0 && gpurdp::active() && !g_rgHi && !wrtag::tag) {
    const u32 bpp = ci_size == 3 ? 4 : ci_size == 1 ? 1 : 2;
    const u64 last = u64(ci_addr) + (u64(y1 - 1) * ci_width + u64(x1)) * bpp;   // un byte detras
    if(u32(x1) <= ci_width && ci_addr % bpp == 0 && last <= m.size()) {
      gpurdp::queueFill({ci_addr, ci_width, bpp, u32(x0), u32(y0), u32(x1), u32(y1), fill_color});
      gpuQueued = true;
      pxWrites += npx;
      for(int y = y0; y < y1; y++) addSpan(x0, u64(x1 - x0));
      accountPixels(mem, npx, pxWrites - w0, pxZWrites - z0);
      return;
    }
  }
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
        if((other_lo & 1) && alphaRef(x, y, c) < (int)(blend_color & 0xff)) continue;
        // Depth: a fill rect carries no z slope, so its only defined depth source is
        // SET_PRIM_DEPTH (Z_SOURCE_SEL, other_lo bit 2). Without that bit the span z
        // the rect never programs is undefined, so leave the z image alone.
        const s32 pz = (s32)prim_z;
        blendPixel(mem, x, y, c, 8, ((other_lo & 4) && zi_addr && (other_lo & 0x30)) ? &pz : nullptr);
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
        u32 a  = ci_addr + (u32(y) * ci_width + u32(x)) * 2;
        wr16(m, a, px);
        // Un relleno tambien deja cobertura: el hardware toma el bit 0 del color de relleno
        // como pixel lleno o vacio (parallel-rdp `fill_color`: `a = (col & 1) * 0xe0`), o
        // sea 7 o 0. Sin esto el borrado de pantalla dejaria la cobertura del frame anterior
        // y el filtro AA del VI trabajaria sobre bordes fantasma.
        if(a + 1 < m.size()) hiddenBits(mem)[a >> 1] = (px & 1) ? 3 : 0;
        pxWrites++;
      }
    }
  }
  for(int y = y0; y < y1 && x1 > x0; y++) addSpan(x0, u64(x1 - x0));
  accountPixels(mem, npx, pxWrites - w0, pxZWrites - z0);
}

// Flat/Gouraud triangle from RDP edge coefficients. First light: correct geometry,
// solid shade-base color (Gouraud/texture refined later).
// Division perspectiva del RDP: s16 entre W s1.15 con reciproco por tabla de 64 entradas
// + pendiente, producto y desplazamiento; saturacion fuera de rango y W<=0 -> 0x7fff.
// Copia bit a bit de parallel-rdp shaders/perspective.h (= tcdiv_persp de angrylion).
// `ovf` (opcional) se pone a true si algun cociente satura o W <= 0: la unidad de LOD lo
// toma como "distante" (perspective_overflow de parallel-rdp).
static auto perspDivide(s32 s, s32 t, s32 w, s32& os, s32& ot, bool* ovf = nullptr) -> void {
  static constexpr s16 tab[64][2] = {
    {0x4000,-252*4},{0x3f04,-244*4},{0x3e10,-238*4},{0x3d22,-230*4},{0x3c3c,-223*4},{0x3b5d,-218*4},
    {0x3a83,-210*4},{0x39b1,-205*4},{0x38e4,-200*4},{0x381c,-194*4},{0x375a,-189*4},{0x369d,-184*4},
    {0x35e5,-179*4},{0x3532,-175*4},{0x3483,-170*4},{0x33d9,-166*4},{0x3333,-162*4},{0x3291,-157*4},
    {0x31f4,-155*4},{0x3159,-150*4},{0x30c3,-147*4},{0x3030,-143*4},{0x2fa1,-140*4},{0x2f15,-137*4},
    {0x2e8c,-134*4},{0x2e06,-131*4},{0x2d83,-128*4},{0x2d03,-125*4},{0x2c86,-123*4},{0x2c0b,-120*4},
    {0x2b93,-117*4},{0x2b1e,-115*4},{0x2aab,-113*4},{0x2a3a,-110*4},{0x29cc,-108*4},{0x2960,-106*4},
    {0x28f6,-104*4},{0x288e,-102*4},{0x2828,-100*4},{0x27c4,-98*4},{0x2762,-96*4},{0x2702,-94*4},
    {0x26a4,-92*4},{0x2648,-91*4},{0x25ed,-89*4},{0x2594,-87*4},{0x253d,-86*4},{0x24e7,-85*4},
    {0x2492,-83*4},{0x243f,-81*4},{0x23ee,-80*4},{0x239e,-79*4},{0x234f,-77*4},{0x2302,-76*4},
    {0x22b6,-74*4},{0x226c,-74*4},{0x2222,-72*4},{0x21da,-71*4},{0x2193,-70*4},{0x214d,-69*4},
    {0x2108,-67*4},{0x20c5,-67*4},{0x2082,-65*4},{0x2041,-65*4}};
  const bool wCarry = w <= 0;
  w &= 0x7fff;
  const int msb = w ? 31 - __builtin_clz((u32)w) : -1;   // findMSB(0) = -1
  const int shift = std::min(14 - msb, 14);
  const int normout = (s32)((u32)w << shift) & 0x3fff;
  const int rcp = ((tab[normout >> 8][1] * (normout & 0xff)) >> 10) + tab[normout >> 8][0];
  s32 prod[2] = {(s32)((u32)s * (u32)rcp), (s32)((u32)t * (u32)rcp)}, out[2];
  const s32 mask = ((1 << 30) - 1) & -((1 << 29) >> shift);
  for(int i = 0; i < 2; i++) {
    const s32 oob = prod[i] & mask;
    const s32 p = shift != 14 ? (prod[i] >> (13 - shift)) : prod[i];
    out[i] = shift != 14 ? p : (s32)((u32)prod[i] << 1);
    if(oob != mask && oob != 0) { out[i] = (p & (1 << 29)) == 0 ? 0x7fff : -0x8000; if(ovf) *ovf = true; }
    if(wCarry) { out[i] = 0x7fff; if(ovf) *ovf = true; }
    out[i] = std::clamp(out[i], -0x10000, 0xffff);
  }
  os = out[0]; ot = out[1];
}

// GPU-RDP fase 1: encola el triangulo en la GPU si sale identico a pintarlo aqui (ver
// gpurdp/shaders/tri.comp, que es el mismo recorrido, combinador y blender que este fichero).
// Hace aqui el recorrido de cobertura, que es barato, para cobrar como el camino del CPU y para
// darle a la GPU la caja exacta de pixeles. false = no se puede, el llamante lo pinta.
auto SoftRdp::gpuTriangle(Memory& mem, const u64* w, bool leftMajor, bool fillMode, bool gouraud,
                          bool combProg, u32 flat, u32 flatTexel, bool zActive, bool zSrc,
                          const s32* cC, const s32* cDx, const s32* cDe, const s32* cDy,
                          s32 zC, s32 zDx, s32 zDe, s32 zDy) -> bool {
  if(g_rgHi || wrtag::tag || ci_size != 2 || (ci_addr & 1) || (zi_addr & 1)) return false;
  const u32 cyc = cycleType();
  if(cyc == 2) return false;                          // COPY sin textura: no merece la pena
  if(sx1 > (int)ci_width) return false;               // x >= ancho pisaria la fila siguiente
  if(!fillMode && combProg) {
    if(combPlan.keyHi != combine_hi || combPlan.keyLo != combine_lo || combPlan.keyCyc != cyc)
      buildCombPlan();
    // NOISE va por std::rand() en orden de pixel; COMBINED en el primer ciclo lee el pixel
    // anterior: los dos atan cada pixel al de antes y en la GPU no hay "antes".
    if(!combPlan.fast) return false;
    const u8* s0 = combPlan.sel[combPlan.two ? 0 : 1];
    for(int k = 0; k < 8; k++) if(s0[k] == CR_CIN || s0[k] == CR_CINA) return false;
  }
  auto sext = [](u32 v, int bits) -> s32 { return (s32)(v << (32 - bits)) >> (32 - bits); };
  const u64 w0 = w[0];
  const s32 iYl = sext((u32)(w0 >> 32) & 0x3fff, 14), iYm = sext((u32)(w0 >> 16) & 0x3fff, 14),
            iYh = sext((u32)w0 & 0x3fff, 14);
  const s32 iXl = sext((u32)(w[1] >> 32), 28) >> 1, iDxl = sext((u32)w[1] >> 2, 28) >> 1;
  const s32 iXh = sext((u32)(w[2] >> 32), 28) >> 1, iDxh = sext((u32)w[2] >> 2, 28) >> 1;
  const s32 iXm = sext((u32)(w[3] >> 32), 28) >> 1, iDxm = sext((u32)w[3] >> 2, 28) >> 1;
  const s32 yhBase = iYh & ~3;
  const s32 subLo = std::max(iYh, sy0 * 4), subHi = std::min(iYl, sy1 * 4);
  const s32 scLo = sx0 * 8, scHi = sx1 * 8;
  const bool aaOn = (other_lo & 0x08) != 0 && !g_noAA;
  auto quant = [&](s32 x) -> s32 { x = sext((u32)x, 27); return (x >> 12) | ((x & 0xfff) != 0); };
  const int yFirst = std::max(subLo, 0) >> 2, yLast = (subHi - 1) >> 2;
  // Mismo recorrido que el paseo solo-coste de drawTriangle: misma cuenta, mismos tramos.
  u64 rasterPx = 0;
  int bx0 = INT32_MAX, bx1 = INT32_MIN, by0 = -1, by1 = -1, lastX = -1, lastY = -1;
  for(int y = yFirst; y <= yLast && subHi > subLo; y++) {
    s32 qL[4], qR[4];
    bool anyValid = false;
    for(int k = 0; k < 4; k++) {
      const s32 ys = y * 4 + k;
      const s32 eh = iXh + (ys - yhBase) * iDxh;
      const s32 el = ys < iYm ? iXm + (ys - yhBase) * iDxm : iXl + (ys - iYm) * iDxl;
      s32 L = quant(leftMajor ? eh : el), R = quant(leftMajor ? el : eh);
      bool bad = (L >> 1) > (R >> 1) || ys < subLo || ys >= subHi;
      L = std::min(std::max(L, scLo), scHi); R = std::min(std::max(R, scLo), scHi);
      if(bad) { L = 0xffff; R = 0; } else anyValid = true;
      qL[k] = L; qR[k] = R;
    }
    if(!anyValid) continue;
    const int xs = std::max(std::min(std::min(qL[0], qL[1]), std::min(qL[2], qL[3])) >> 3, 0);
    const int xe = (std::max(std::max(qR[0], qR[1]), std::max(qR[2], qR[3])) >> 3) + 1;
    const u64 rowPx0 = rasterPx;
    for(int x = xs; x < xe; x++) {
      u32 c = 0;
      for(int k = 0; k < 4; k++) {
        const s32 a = x * 8 + ((k & 1) ? 2 : 0), b = a + 4;
        if(a >= qL[k] && a < qR[k]) c |= 1u << k;
        if(b >= qL[k] && b < qR[k]) c |= 16u << k;
      }
      if(aaOn ? c == 0 : (c & 1) == 0) continue;
      rasterPx++;
      bx0 = std::min(bx0, x); bx1 = std::max(bx1, x);
      if(by0 < 0) by0 = y;
      by1 = y; lastX = x; lastY = y;
    }
    addSpan(xs, rasterPx - rowPx0);
  }
  // Zonas de RDRAM: filas [by0, by1] enteras del color image y del z image. Todo dentro de la
  // RDRAM (el CPU descarta escrituras sueltas fuera; la GPU no) y sin solaparse entre si (un
  // pixel leeria lo que escribe otro del mismo triangulo).
  const u64 size = mem.rdram.size();
  u32 lo[2] = {0, 0}, hi[2] = {0, 0};
  if(rasterPx) {
    const u64 rowB = u64(ci_width) * 2;
    const u64 cLo = ci_addr + u64(by0) * rowB, cHi = ci_addr + u64(by1 + 1) * rowB;
    if(cHi > size) { spanLines = spanUnits = 0; return false; }
    lo[0] = (u32)cLo; hi[0] = (u32)cHi;
    if(zActive && !fillMode) {
      const u64 zLo = zi_addr + u64(by0) * rowB, zHi = zi_addr + u64(by1 + 1) * rowB;
      if(zHi > size || (zLo < cHi && cLo < zHi)) { spanLines = spanUnits = 0; return false; }
      lo[1] = (u32)zLo; hi[1] = (u32)zHi;
    }
  }
  hiddenBits(mem);   // dimensionada antes de que el flush la use
  const AcctSnap acct = acctSnap(mem, rasterPx);
  if(!rasterPx) return true;   // nada cubierto: ni escribe ni toca COMBINED

  gpurdp::TriRec t{};
  using namespace gpurdp;
  s32* r = t.w;
  r[T_YL] = iYl; r[T_YM] = iYm; r[T_YH] = iYh;
  r[T_XL] = iXl; r[T_DXL] = iDxl; r[T_XH] = iXh; r[T_DXH] = iDxh; r[T_XM] = iXm; r[T_DXM] = iDxm;
  int fl = 0;
  if(leftMajor) fl |= TF_LEFT;
  if(leftMajor == ((s32)(u32)w[2] < 0)) fl |= TF_DOOFF;
  if(fillMode) fl |= TF_FILL;
  if(gouraud) fl |= TF_SHADE;
  if(zActive) fl |= TF_ZACT;
  if(zSrc) fl |= TF_ZSRC;
  if(aaOn) fl |= TF_AA;
  if(combProg) fl |= TF_COMB;
  if(g_noBlend) fl |= TF_NOBLEND;
  if(cyc == 1) fl |= TF_TWO;
  if(g_noAA) fl |= TF_NOAA;
  r[T_FLAGS] = fl;
  r[T_SX0] = sx0; r[T_SY0] = sy0; r[T_SX1] = sx1; r[T_SY1] = sy1;
  r[T_BX] = bx0; r[T_BY] = by0; r[T_BW] = bx1 - bx0 + 1; r[T_BH] = by1 - by0 + 1;
  for(int c = 0; c < 4; c++) {
    r[T_CC + c] = cC[c]; r[T_CDX + c] = cDx[c]; r[T_CDE + c] = cDe[c]; r[T_CDY + c] = cDy[c];
  }
  r[T_Z] = zC; r[T_ZDX] = zDx; r[T_ZDE] = zDe; r[T_ZDY] = zDy;
  r[T_CI] = (s32)ci_addr; r[T_CIW] = (s32)ci_width; r[T_ZI] = (s32)zi_addr;
  r[T_OLO] = (s32)other_lo; r[T_OHI] = (s32)other_hi;
  r[T_FILL] = (s32)fill_color; r[T_PRIM] = (s32)prim_color; r[T_ENV] = (s32)env_color;
  r[T_BLEND] = (s32)blend_color; r[T_FOG] = (s32)fog_color; r[T_FLAT] = (s32)flat;
  r[T_FTEX] = (s32)flatTexel;
  if(combProg) std::memcpy(&r[T_SEL], combPlan.sel, 16);
  r[T_LOD] = lodFracV; r[T_PLOD] = prim_lod_frac; r[T_PZ] = (s32)prim_z;
  r[T_PDZ] = pxDz; r[T_PDZC] = pxDzC; r[T_LASTX] = lastX; r[T_LASTY] = lastY;
  t.lo[0] = lo[0]; t.hi[0] = hi[0]; t.lo[1] = lo[1]; t.hi[1] = hi[1];
  if(!gpurdp::queueTri(t)) { gpuFlush(mem); gpurdp::queueTri(t); }
  gpuTris.push_back({acct, combProg && !fillMode});
  gpuQueued = true;
  return true;
}

auto SoftRdp::drawTriangle(Memory& mem, const u64* w, int words, u32 op) -> void {
  if(g_noRaster) return;
  bool hasShade = op & 4, hasTex = op & 2, hasZ = op & 1;
  u64 rasterPx = 0, accW0 = pxWrites, accZ0 = pxZWrites;   // DPC counter accounting
  u64 w0 = w[0];
  bool leftMajor = (w0 >> 55) & 1;

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
  // Atributos en ENTERO, crudos s15.16 tal como llegan (parallel-rdp rdp_device.cpp
  // decode_rgba/tex/z_setup): parte entera en una palabra y fraccion en otra, mismo
  // carril. c* = R,G,B,A (bloque de shade); t* = S,T,Z,W (texture + z). Por cada uno:
  // valor inicial, paso por +x (Dx), por +y sobre el borde mayor (De) y por +y (Dy).
  s32 cC[4] = {}, cDx[4] = {}, cDe[4] = {}, cDy[4] = {};
  s32 tC[4] = {}, tDx[4] = {}, tDe[4] = {}, tDy[4] = {};
  auto attrRaw = [](u64 hi, u64 lo, int c) -> s32 {
    const int sh = 48 - c * 16;
    return (s32)((((u32)(hi >> sh) & 0xffff) << 16) | ((u32)(lo >> sh) & 0xffff));
  };
  if(gouraud) for(int c = 0; c < 4; c++) {
    cC[c]  = attrRaw(w[4], w[6], c);  cDx[c] = attrRaw(w[5], w[7], c);
    cDe[c] = attrRaw(w[8], w[10], c); cDy[c] = attrRaw(w[9], w[11], c);
  }
  // Texture: S [63:48], T [47:32], W [31:16]; entero en j0/j1/j4/j5, fraccion en j2/j3/j6/j7.
  if(textured) {
    const int tb = 4 + (hasShade ? 8 : 0);
    static const int kSlot[3] = { 0, 1, 3 };
    for(int c = 0; c < 3; c++) {
      const int d = kSlot[c];
      tC[d]  = attrRaw(w[tb + 0], w[tb + 2], c); tDx[d] = attrRaw(w[tb + 1], w[tb + 3], c);
      tDe[d] = attrRaw(w[tb + 4], w[tb + 6], c); tDy[d] = attrRaw(w[tb + 5], w[tb + 7], c);
    }
  }
  // Z: word0[63:32]=Z, word0[31:0]=DzDx, word1[63:32]=DzDe, word1[31:0]=DzDy.
  if(hasZ) {
    const int zb = 4 + (hasShade ? 8 : 0) + (hasTex ? 8 : 0);
    if(zb + 1 < words) {
      tC[2]  = (s32)(u32)(w[zb] >> 32);     tDx[2] = (s32)(u32)w[zb];
      tDe[2] = (s32)(u32)(w[zb + 1] >> 32); tDy[2] = (s32)(u32)w[zb + 1];
    }
  }
  setPrimDz(zSrc, tDx[2], tDy[2]);
  // TEX_PERSP (other_hi bit 19): el RSP manda S/W, T/W y 1/W, y el RDP divide por pixel.
  const bool persp = textured && ((other_hi >> 19) & 1);

  // Flat (non-shade, non-texture) triangle colour. Without a full combiner/blender
  // model the constant source is a guess; use the register a demo most likely put it
  // in — prim, else blend, else env — before the neutral grey fallback. (Real games
  // route this through the combiner; that lands with combiner emulation later.)
  u32 flat = prim_color ? prim_color : blend_color ? blend_color : env_color ? env_color : 0xa0a0a0ff;
  bool combProg = (combine_hi | combine_lo) != 0;   // combiner programmed? else old heuristics
  // Texel the combiner sees on a flat (no-tex-coord) primitive. The RDP texel bus for a
  // primitive with no texture block presents all-ones (0xFFFFFFFF, opaque white): the Krom
  // "Fill Triangle" demos route this through the combiner alpha (TEX0_A * LOD_FRAC = 1) so
  // the blender's coverage weight is 1 and blend_color shows solid. If the combiner ignores
  // texel this is harmless.
  u32 flatTexel = (combProg && !textured && !gouraud && !fillMode) ? 0xffffffff : 0;
  if(g_triDbg) {
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

  // Invariantes de la primitiva sacados del bucle por pixel: el tile no cambia dentro de un
  // triangulo, ni el tipo de ciclo, ni el bit de alpha-compare.
  const TexFold texF   = foldOf(texTile);
  const bool    copyCy = cycleType() == 2;
  const bool    alphaCmpEn = (other_lo & 1) != 0;
  // Mipmap / TEXEL1. La unidad de LOD mira el pixel y sus vecinos +x (en el sentido del
  // recorrido: +1 con borde mayor a la izquierda, -1 si no) y +y, con los pasos tal como
  // los ve el HW (parallel-rdp interpolate_stz: DxDx sin sus 5 bits bajos, DxDy sin los 15
  // bajos), divididos por W si hay perspectiva. Nivel maximo = bits 53:51 del comando.
  bool usesLod = false, usesTex1 = false;
  if(!fillMode && !copyCy) texNeeds(usesLod, usesTex1);
  const u32 maxLevel = (u32)(w0 >> 51) & 7;
  TexFold folds[8];
  if(textured && (usesLod || usesTex1)) for(u32 i = 0; i < 8; i++) folds[i] = foldOf(i);
  const u32 ldir = leftMajor ? 1u : ~0u;   // +1 / -1 en aritmetica modular
  // Textura HD / realce (texpack): sustituye el texel de tile0 cuando hay reemplazo. No con
  // mipmap del propio juego (los niveles serian de otra textura) ni en solo-coste.
  HdBind hdB;
  if(texpack::active() && textured && !usesLod && !costOnly && !fillMode) hdB = hdBind(mem, texTile);
  lodFracV = 0xff;
  if(usesLod && !textured) {   // sin coordenadas: S=T=W=0 en todo el primitivo -> magnify
    u32 t0 = texTile, t1 = (texTile + 1) & 7;
    lodFracV = lodSelect(0, 0, 0, 0, 0, 0, false, maxLevel, t0, t1);
  }
  // GPU-RDP fase 1 (KESTREL_GPURDP=1): triangulo sin textura en 16 bpp a la GPU.
  if(gpurdp::active() && !costOnly) {
    if(!textured && gpuTriangle(mem, w, leftMajor, fillMode, gouraud, combProg, flat, flatTexel,
                                zActive, zSrc, cC, cDx, cDe, cDy, tC[2], tDx[2], tDe[2], tDy[2])) {
      pxShadeA = 0; lodFracV = 0xff;
      return;
    }
    if(gpuQueued) gpuFlush(mem);   // va por el CPU: antes, lo encolado a la RDRAM
  }
  // Recorrido de bordes en ENTERO, como el rasterizador del RDP. Oraculo: parallel-rdp
  // `span_setup.comp` (decodificacion en rdp_device.cpp) y `compute_coverage()` de
  // coverage.h. Antes iba en double con los bordes anclados en el `yh` FRACCIONARIO y la
  // primera fila en ceil(yh); el HW ancla XH/XM en la scanline entera (yh & ~3, en cuartos)
  // y XL en ym, evalua cada borde en las 4 sub-scanlines de la fila, recorta cada
  // sub-scanline contra [yh, yl) y cuantiza x a 1/8 de pixel con un bit "sticky". Con el
  // ancla desplazada, dos triangulos que comparten arista calculaban x distintas para ella
  // y quedaban pixeles que no pintaba ninguno (costuras de fondo en los cubos de
  // kestrel64-sdk 05_kgfx_cube, que parallel-rdp pinta limpios).
  //   x: s15.16 crudo -> sext28 >> 1 = 16.15; dxdy: (crudo >> 2) = por sub-scanline.
  auto sext = [](u32 v, int bits) -> s32 { return (s32)(v << (32 - bits)) >> (32 - bits); };
  const s32 iYl = sext((u32)(w0 >> 32) & 0x3fff, 14), iYm = sext((u32)(w0 >> 16) & 0x3fff, 14),
            iYh = sext((u32)w0 & 0x3fff, 14);
  const s32 iXl = sext((u32)(w[1] >> 32), 28) >> 1, iDxl = sext((u32)w[1] >> 2, 28) >> 1;
  const s32 iXh = sext((u32)(w[2] >> 32), 28) >> 1, iDxh = sext((u32)w[2] >> 2, 28) >> 1;
  const s32 iXm = sext((u32)(w[3] >> 32), 28) >> 1, iDxm = sext((u32)w[3] >> 2, 28) >> 1;
  const s32 yhBase = iYh & ~3;
  const s32 subLo = std::max(iYh, sy0 * 4), subHi = std::min(iYl, sy1 * 4);
  const s32 scLo = sx0 * 8, scHi = sx1 * 8;   // tijera en octavos de pixel
  const bool aaOn = (other_lo & 0x08) != 0 && !g_noAA;   // mismo criterio que blendParams
  auto quant = [&](s32 x) -> s32 { x = sext((u32)x, 27); return (x >> 12) | ((x & 0xfff) != 0); };
  const int yFirst = std::max(subLo, 0) >> 2, yLast = (subHi - 1) >> 2;
  for(int y = yFirst; y <= yLast && subHi > subLo; y++) {
    s32 qL[4], qR[4];
    bool anyValid = false;
    for(int k = 0; k < 4; k++) {
      const s32 ys = y * 4 + k;
      const s32 eh = iXh + (ys - yhBase) * iDxh;
      const s32 el = ys < iYm ? iXm + (ys - yhBase) * iDxm : iXl + (ys - iYm) * iDxl;
      s32 L = quant(leftMajor ? eh : el), R = quant(leftMajor ? el : eh);
      bool bad = (L >> 1) > (R >> 1) || ys < subLo || ys >= subHi;
      L = std::min(std::max(L, scLo), scHi); R = std::min(std::max(R, scLo), scHi);
      if(bad) { L = 0xffff; R = 0; } else anyValid = true;
      qL[k] = L; qR[k] = R;
    }
    if(!anyValid) continue;
    // Cobertura de 8 tomas: fila k en (x*8 + {0,4}) para k par y (x*8 + {2,6}) para k impar.
    // Bit 0 = sub-scanline 0, columna 0: con AA apagado el pixel vive o muere con ella
    // (shading.h: `if (!aa_enable && (coverage & 1) == 0) return false`).
    auto coverage = [&](int x) -> u32 {
      u32 c = 0;
      for(int k = 0; k < 4; k++) {
        const s32 a = x * 8 + ((k & 1) ? 2 : 0), b = a + 4;
        if(a >= qL[k] && a < qR[k]) c |= 1u << k;
        if(b >= qL[k] && b < qR[k]) c |= 16u << k;
      }
      return c;
    };
    const int xs = std::max(std::min(std::min(qL[0], qL[1]), std::min(qL[2], qL[3])) >> 3, 0);
    const int xe = (std::max(std::max(qR[0], qR[1]), std::max(qR[2], qR[3])) >> 3) + 1;
    // Solo-coste: misma decision de pixel que el bucle largo (la de la cobertura), asi que
    // la cuenta es IDENTICA; se salta el trabajo por pixel (textura, combinador, mezcla).
    const u64 rowPx0 = rasterPx;
    if(costOnly) {
      for(int x = xs; x < xe; x++) { u32 c = coverage(x); if(aaOn ? c != 0 : (c & 1) != 0) rasterPx++; }
      addSpan(xs, rasterPx - rowPx0);
      continue;
    }
    // --- atributos de la fila, en ENTERO (parallel-rdp span_setup.comp) ----------------
    // El HW ancla los atributos en el borde mayor de la fila: x entera base_x = XH >> 15
    // (XH en 16.15) y su fraccion de 8 bits se DESCUENTA del arranque con DxDx >> 8, asi
    // que el valor del pixel x es arranque + DxDx * (x - base_x). Con do_offset (flip ==
    // signo de DxHDy) el HW latchea en la ULTIMA sub-scanline: XH avanza 3 pasos y el
    // arranque se corrige con 3/4 de (DxDe - DxDy). Todo modular a 32 bits, como el HW.
    const s32 dyRow = y - (iYh >> 2);
    u32 xhRow = (u32)iXh + (u32)dyRow * ((u32)iDxh << 2);
    const bool doOffset = leftMajor == ((s32)(u32)w[2] < 0);
    if(doOffset) xhRow += 3u * (u32)iDxh;
    const s32 baseX = (s32)xhRow >> 15;
    const u32 xfrac = copyCy ? 0u : ((xhRow >> 7) & 0xff);
    auto rowAttr = [&](s32 c0, s32 ddx, s32 dde, s32 ddy) -> s32 {
      u32 diff = 0;
      if(doOffset) {
        const s32 deh = dde & ~0x1ff, dyh = ddy & ~0x1ff;
        diff = (u32)deh - (u32)(deh >> 2) - (u32)dyh + (u32)(dyh >> 2);
      }
      const u32 v = (u32)c0 + (u32)dde * (u32)dyRow;
      return (s32)(((v & ~0x1ffu) + diff - xfrac * (u32)((ddx >> 8) & ~1)) & ~0x3ffu);
    };
    s32 rC[4], rT[4];
    for(int c = 0; c < 4; c++) {
      rC[c] = rowAttr(cC[c], cDx[c], cDe[c], cDy[c]);
      rT[c] = rowAttr(tC[c], tDx[c], tDe[c], tDy[c]);
    }
    // Centroide: el HW evalua shade y z en la PRIMERA muestra cubierta en orden de
    // scanline (fila k, columna izquierda antes que derecha), con el desfase en cuartos
    // de pixel xo (0..3) / yo (0..3) aplicado a DxDx y DxDy (interpolation.h).
    auto centroid = [](u32 cv, int& xo, int& yo) {
      int idx = 0;
      for(int k = 0; k < 4; k++) {
        if(cv & (1u << k))  { idx = 2 * k; break; }
        if(cv & (16u << k)) { idx = 2 * k + 1; break; }
      }
      yo = idx >> 1; xo = ((idx & 1) << 1) + (yo & 1);
    };
    // Shade: 9 bits con signo de guarda (>>14), centroide en i16, sujeto a 0..255 con la
    // regla de 9 bits (un poco por debajo de 0 -> 0, por encima de 0xff -> 0xff).
    auto shadeAt = [&](s32 dxI, int xo, int yo) -> u32 {
      u32 out = 0;
      for(int c = 0; c < 4; c++) {
        const s32 v = (s32)((u32)rC[c] + (u32)(cDx[c] & ~0x1f) * (u32)dxI);
        int t = (s16)((((int)(s16)(v >> 14)) << 2) + xo * (s16)(cDx[c] >> 14) + yo * (s16)(cDy[c] >> 14));
        t >>= 4;
        t = ((s32)((u32)(t - 0x80) << 23) >> 23) + 0x80;
        t = t < 0 ? 0 : t > 255 ? 255 : t;
        out |= (u32)t << (24 - 8 * c);
      }
      return out;
    };
    // Z: 18 bits sin signo (15.3) con un bit de guarda, mismo centroide (clamp_z).
    auto zAt = [&](s32 dxI, int xo, int yo) -> s32 {
      const s32 z = (s32)((u32)rT[2] + (u32)tDx[2] * (u32)dxI);
      s32 sz = (s32)(((u32)(z >> 10) << 2) + (u32)(xo * (tDx[2] >> 10)) + (u32)(yo * (tDy[2] >> 10)));
      sz >>= 5;
      sz = ((s32)((u32)(sz - (1 << 17)) << 13) >> 13) + (1 << 17);
      return sz < 0 ? 0 : sz > 0x3ffff ? 0x3ffff : sz;
    };
    // Profundidad del pixel para la etapa de z de blendPixel (Z_SOURCE_SEL = la de
    // SET_PRIM_DEPTH); nullptr si la primitiva ni compara ni actualiza.
    s32 zCur = 0;
    auto zIn = [&](s32 dxI, int xo, int yo) -> const s32* {
      if(!zActive) return nullptr;
      zCur = zSrc ? (s32)prim_z : zAt(dxI, xo, yo);
      return &zCur;
    };
    for(int x = xs; x < xe; x++) {
      const u32 cov = coverage(x);
      if(aaOn ? cov == 0 : (cov & 1) == 0) continue;
      rasterPx++;      // DPC counters: pixel entered the pipeline (may still be killed)
      const int hits = __builtin_popcount(cov);
      if(fillMode) {   // raw packed fill color straight to the color image (no AA)
        if(ci_size == 3) wr32(m, ci_addr + (u32(y) * ci_width + u32(x)) * 4, fill_color);
        else { u16 px = (x & 1) ? u16(fill_color & 0xffff) : u16(fill_color >> 16);
               u32 a = ci_addr + (u32(y) * ci_width + u32(x)) * 2;
               wr16(m, a, px);
               if(a + 1 < m.size()) hiddenBits(mem)[a >> 1] = (px & 1) ? 3 : 0; }
        pxWrites++;
      } else if(textured) {
        const s32 dxI = x - baseX;
        int xo, yo; centroid(cov, xo, yo);
        // S/T/W del pixel: DxDx sin sus 5 bits bajos, parte entera (>>16) = 1/32 de texel.
        auto stwAt = [&](int c, u32 add) -> s32 {
          return (s32)((u32)rT[c] + (u32)(tDx[c] & ~0x1f) * (u32)dxI + add) >> 16;
        };
        s32 ps = stwAt(0, 0), pt = stwAt(1, 0);
        bool ovf = false;
        if(persp) perspDivide(ps, pt, stwAt(3, 0), ps, pt, &ovf);
        const double su = ps / 32.0, tu = pt / 32.0;
        u32 tile0 = texTile, tile1 = (texTile + 1) & 7;
        if(usesLod) {
          // Vecinos +x (en el sentido del recorrido) y +y (DxDy sin sus 15 bits bajos).
          const u32 xs0 = ldir * (u32)(tDx[0] & ~0x1f), xt0 = ldir * (u32)(tDx[1] & ~0x1f),
                    xw0 = ldir * (u32)(tDx[3] & ~0x1f);
          const u32 ys0 = (u32)(tDy[0] & ~0x7fff), yt0 = (u32)(tDy[1] & ~0x7fff),
                    yw0 = (u32)(tDy[3] & ~0x7fff);
          s32 s1 = stwAt(0, xs0), t1 = stwAt(1, xt0), s2 = stwAt(0, ys0), t2 = stwAt(1, yt0);
          if(persp) {
            perspDivide(s1, t1, stwAt(3, xw0), s1, t1, &ovf);
            perspDivide(s2, t2, stwAt(3, yw0), s2, t2, &ovf);
          }
          lodFracV = lodSelect(ps, pt, s1, t1, s2, t2, ovf, maxLevel, tile0, tile1);
        }
        u32 tex;
        if(hdB.tex) {
          // Huella del pixel en texeles originales (mismos vecinos que la unidad de LOD):
          // elige el nivel de la piramide HD.
          const u32 xs0 = ldir * (u32)(tDx[0] & ~0x1f), xt0 = ldir * (u32)(tDx[1] & ~0x1f),
                    xw0 = ldir * (u32)(tDx[3] & ~0x1f);
          const u32 ys0 = (u32)(tDy[0] & ~0x7fff), yt0 = (u32)(tDy[1] & ~0x7fff),
                    yw0 = (u32)(tDy[3] & ~0x7fff);
          s32 s1 = stwAt(0, xs0), t1 = stwAt(1, xt0), s2 = stwAt(0, ys0), t2 = stwAt(1, yt0);
          if(persp) {
            bool o2 = false;
            perspDivide(s1, t1, stwAt(3, xw0), s1, t1, &o2);
            perspDivide(s2, t2, stwAt(3, yw0), s2, t2, &o2);
          }
          const double fp = std::max({std::abs((double)s1 - ps), std::abs((double)t1 - pt),
                                      std::abs((double)s2 - ps), std::abs((double)t2 - pt)}) / 32.0;
          tex = hdSample(hdB, su, tu, fp);
        } else {
          tex = sampleTexFold(usesLod ? folds[tile0] : texF, su, tu);
        }
        // 2-cycle: TEXEL1 es un segundo muestreo, de tile1, en el mismo S/T.
        u32 tex1 = usesTex1 ? sampleTexFold(folds[tile1], su, tu) : tex;
        // Shade (if the triangle carries a shade block) feeds the combiner alongside
        // the texel — this is how MODULATE (texel*shade) textures get their lighting.
        u32 shd = gouraud ? shadeAt(dxI, xo, yo) : 0;
        u32 c = (combProg && !copyCy) ? combineColor(tex, tex1, shd) : tex;
        pxShadeA = (u8)std::min<u32>((shd & 0xff) + (u32)alphaDither(x, y), 0xff);
        // Alpha compare (see texRect): COPY mode keys on the 1-bit texel alpha (drop
        // alpha==0); 1-/2-cycle compares COMBINED alpha (+ alpha dither) against the
        // blend_color threshold. Disabled → texel drawn regardless of its 5551 bit.
        bool apass = !alphaCmpEn ||
                     (copyCy ? (c & 0xff) != 0 : (alphaRef(x, y, c) >= (int)(blend_color & 0xff)));
        if(apass) blendPixel(mem, x, y, c, hits, zIn(dxI, xo, yo));
      } else if(gouraud) {
        const s32 dxI = x - baseX;
        int xo, yo; centroid(cov, xo, yo);
        u32 shd = shadeAt(dxI, xo, yo);
        u32 c = combProg ? combineColor(0, 0, shd) : shd;
        pxShadeA = (u8)std::min<u32>((shd & 0xff) + (u32)alphaDither(x, y), 0xff);
        blendPixel(mem, x, y, c, hits, zIn(dxI, xo, yo));
      } else { const s32 dxI = x - baseX; int xo, yo; centroid(cov, xo, yo);
        // Flat (no shade/tex coords) but the combiner may still select TEXEL0/1. The RDP
        // has no per-vertex S/T here, so it samples the current tile at its origin (0,0) —
        // a solid-fill triangle that routes a loaded texel through the combiner (common in
        // the Krom fill tests) picks up that texel. If the combiner ignores texel this is
        // harmless. Sampled once (constant across the primitive).
        u32 c = combProg ? combineColor(flatTexel, flatTexel, 0) : flat;
        pxShadeA = (u8)alphaDither(x, y);   // shade 0 + dither de alfa
        blendPixel(mem, x, y, c, hits, zIn(dxI, xo, yo)); }
    }
    addSpan(xs, rasterPx - rowPx0);
  }
  pxShadeA = 0;   // rects y demas primitivas sin shade ven 0
  lodFracV = 0xff;
  if(costOnly) accountPixels(mem, rasterPx, rasterPx, zUpd ? rasterPx : 0);
  else         accountPixels(mem, rasterPx, pxWrites - accW0, pxZWrites - accZ0);
}

// Unidad de LOD del TX. Oraculo: parallel-rdp texture.h compute_lod_2cycle. La distancia
// es el mayor salto |vecino - pixel| en S o T (con el "abs" del HW, uno de menos en
// negativos); su log2 sobre 1 texel elige el nivel y la mantisa es LOD_FRAC. Con
// TEX_LOD_EN el nivel desplaza el tile base; SHARPEN y DETAIL cambian el reparto en
// magnificacion. Bits de SET_OTHER_MODES: 48 TEX_LOD_EN, 49 SHARPEN, 50 DETAIL.
auto SoftRdp::lodSelect(s32 s, s32 t, s32 sdx, s32 tdx, s32 sdy, s32 tdy, bool ovf, u32 maxLevel,
                        u32& tile0, u32& tile1) const -> int {
  const bool lodEn = (other_hi >> 16) & 1, sharpen = (other_hi >> 17) & 1, detail = (other_hi >> 18) & 1;
  bool magnify = false, distant = false;
  u32 off = 0;
  int frac = 0xff;
  if(ovf) distant = true;
  else {
    auto ab = [](s32 v) { return v ^ (v >> 31); };
    const s32 maxd = std::max(std::max(ab(sdx - s), ab(tdx - t)), std::max(ab(sdy - s), ab(tdy - t)));
    if(maxd >= 0x4000) { distant = true; off = maxLevel; }
    else if(maxd < 32) {   // LOD < 0
      distant = maxLevel == 0; magnify = true;
      frac = (!sharpen && !detail) ? (distant ? 0xff : 0)
           : (std::max((s32)prim_min_level, maxd) << 3) + (sharpen ? -0x100 : 0);
    } else {
      const int mipBase = 31 - __builtin_clz((u32)(maxd >> 5));
      distant = (u32)mipBase >= maxLevel;
      if(!distant || sharpen || detail) { frac = ((maxd << 3) >> mipBase) & 0xff; off = (u32)mipBase; }
    }
  }
  if(lodEn) {
    if(distant) off = maxLevel;
    if(!detail) {
      tile0 = (tile0 + off) & 7;
      tile1 = (distant || (!sharpen && magnify)) ? tile0 : (tile0 + 1) & 7;
    } else {
      tile1 = (tile0 + off + ((distant || magnify) ? 1 : 2)) & 7;
      tile0 = (tile0 + off + (magnify ? 0 : 1)) & 7;
    }
  }
  return frac;
}

// Que necesita la primitiva del TX (parallel-rdp deduce_static_texture_state): la unidad de
// LOD corre con TEX_LOD_EN o si un 2-cycle lee LOD_FRAC; el segundo muestreo (tile1) solo
// existe en 2-cycle y hace falta si el ciclo 0 lee TEXEL1 o el ciclo 1 lee TEXEL0 (que alli
// es el TEXEL1 del pixel). En 1-cycle TEXEL1 es el texel del pixel siguiente: se aproxima
// con el propio TEXEL0.
auto SoftRdp::texNeeds(bool& lod, bool& tex1) const -> void {
  auto rd = [](const CombSet& c, int tx) {   // tx: 1 = TEXEL0, 2 = TEXEL1
    return c.aR == tx || c.bR == tx || c.cR == tx || c.cR == tx + 7 || c.dR == tx
        || c.aA == tx || c.bA == tx || c.cA == tx || c.dA == tx;
  };
  auto rl = [](const CombSet& c) { return c.cR == 13 || c.cA == 0; };
  const bool two = cycleType() == 1;
  lod  = ((other_hi >> 16) & 1) || (two && (rl(comb[0]) || rl(comb[1])));
  tex1 = two && (rd(comb[0], 2) || rd(comb[1], 1));
}

auto SoftRdp::foldOf(u32 tileIdx) const -> TexFold {
  // Todo lo que un tile aporta al muestreo y NO depende del texel concreto. El filtro de 3
  // puntos toma 3-4 texeles del MISMO tile por pixel: sacando esto del bucle de tomas se
  // recalcula una vez en vez de cuatro, y la decodificacion pasa a ser un salto de tabla.
  const Tile& tl = tiles[tileIdx & 7];
  TexFold f;
  f.shiftS = tl.shiftS;  f.shiftT = tl.shiftT;
  f.maskS  = tl.maskS;   f.maskT  = tl.maskT;
  f.cmS    = tl.cmS;     f.cmT    = tl.cmT;
  f.sMax   = (int)(tl.sh >> 2) - (int)(tl.sl >> 2);
  f.tMax   = (int)(tl.th >> 2) - (int)(tl.tl >> 2);
  f.rowBytes = tl.line * 8;
  f.base     = tl.tmem * 8;
  f.palette  = tl.palette;
  // Camino rapido del pliegue: SHIFT nulo + mask activa + ni clamp ni mirror => la etapa
  // de clamp no se ejecuta (cm & 2 == 0 y mask != 0) y la de mask se queda en el AND.
  f.fastS = tl.shiftS == 0 && tl.maskS != 0 && (tl.cmS & 3) == 0;
  f.fastT = tl.shiftT == 0 && tl.maskT != 0 && (tl.cmT & 3) == 0;
  f.andS  = (1 << tl.maskS) - 1;
  f.andT  = (1 << tl.maskT) - 1;
  switch(tl.size) {                       // (size, fmt) -> un solo `kind`
    case 0:  f.kind = tl.fmt == 2 ? TK_CI4 : tl.fmt == 3 ? TK_IA4  : TK_I4;     break;
    case 1:  f.kind = tl.fmt == 2 ? TK_CI8 : tl.fmt == 3 ? TK_IA8  : TK_I8;     break;
    case 2:  f.kind = tl.fmt == 1 ? TK_YUV : tl.fmt == 3 ? TK_IA16 : TK_RGBA16; break;
    case 3:  f.kind = TK_RGBA32; break;
    default: f.kind = TK_BAD;    break;   // inalcanzable: `size` son 2 bits
  }
  return f;
}

auto SoftRdp::tlutEntry(u32 idx) const -> u32 {
  // Decode one palette entry (CI formats). TEXTLUT mode picks RGBA5551 vs IA16.
  u16 e = tlut[idx & 0xff];
  if(tlutMode() == 3) { u32 i = (e >> 8) & 0xff, a = e & 0xff;   // IA16 palette
                        return (i << 24) | (i << 16) | (i << 8) | a; }
  // A palette entry expands exactly like a texel: 5 bits replicated into 8 (v<<3 | v>>2), as
  // parallel-rdp does on both read paths. krom GRB decoders pin this down: their palettes hold
  // the odd 5-bit values 1,3,..,31 and the hardware captures only reproduce once the entry is
  // replicated and then scaled by the LOD_FRAC(0xff)->COMBINED_ALPHA and 31/32 blender paths -
  // zero-filling the entry cannot reach those levels for any choice of the two scales.
  u32 r = exp5((e >> 11) & 0x1f), g = exp5((e >> 6) & 0x1f);
  u32 b = exp5((e >> 1)  & 0x1f), a = (e & 1) ? 255 : 0;   // RGBA5551 palette
  return (r << 24) | (g << 16) | (b << 8) | a;
}

auto SoftRdp::foldCoord(int c, u32 sh, u32 mask, u32 cm, int lim) -> int {
  // Pliegue de UNA coordenada de textura: SHIFT del tile y luego clamp/mirror/mask. Va
  // aparte de la lectura porque el filtro de 3 puntos toca cuatro coordenadas distintas
  // (s0, s0+1, t0, t0+1) repartidas en tres tomas: plegandolas por separado salen cuatro
  // pliegues por pixel en vez de seis.
  // Tile SHIFT: scales the incoming texel coordinate (shift 1..10 -> >>, 11..15 -> <<).
  if(sh) c = sh <= 10 ? (c >> sh) : (c << (16 - sh));
  // Texel address folding - matches angrylion tcclamp->tcmask order (the two stages run in
  // sequence, NOT mutually exclusive). Stage 1 (clamp): when the clamp bit is set OR there is
  // no mask, pin the coordinate to the tile-size box [0,lim]; this is the ONLY place negatives
  // are pinned. Stage 2 (mask): when maskN != 0, mirror on odd 2^mask periods then fold into
  // 2^mask. With clamp+mask both active and 2^mask > lim the mask is idempotent (clamp stays
  // visible); with 2^mask <= lim it re-wraps - exactly as hardware does. Both stages are
  // two complement so negative coords (roms start S/T at -14) fold correctly instead of
  // collapsing to texel 0. Note: no premature `c<0 -> 0` before masking.
  if((cm & 2) || mask == 0) {                     // clamp stage
    if(c < 0) c = 0;
    else if(lim >= 0 && c > lim) c = lim;
  }
  if(mask) {                                       // mask stage (wrap + mirror)
    if((cm & 1) && ((c >> (int)mask) & 1)) c = ~c; // mirror: flip on odd period
    c &= (1 << mask) - 1;
  }
  return c;
}

template<u32 K>
auto SoftRdp::fetchK(const TexFold& f, int s, int t) const -> u32 {
  // Lee UN texel de TMEM con las coordenadas YA plegadas (ver `foldCoord`). `K` fija el
  // formato en tiempo de compilacion: la decodificacion sale sin ramas.
  const u32 rowBytes = f.rowBytes, base = f.base;
  // Filas impares de TMEM van con las dos mitades de 32 bits de cada palabra de 64
  // intercambiadas (las cargas las escriben asi, ver `loadTile`); el muestreador las deshace
  // con la paridad de la fila T ya plegada. 32 bpp: el intercambio es de pares de texeles.
  const u32 ox = (t & 1) ? (K == TK_RGBA32 ? 8u : 4u) : 0u;
  if constexpr(K == TK_CI4 || K == TK_IA4 || K == TK_I4) {     // 4-bit texels
    u32 off = (base + (u32)t * rowBytes + (u32)s / 2) ^ ox;
    if(off >= 0x1000) return 0;
    u8 nib = (s & 1) ? (tmem[off] & 0xf) : (tmem[off] >> 4);
    if constexpr(K == TK_CI4) return tlutEntry(f.palette * 16 + nib);  // 16-entry sub-palette
    else if constexpr(K == TK_IA4) {
      u32 i = (((nib >> 1) & 7) * 255) / 7, a = (nib & 1) ? 255 : 0;   // IA4 (3I/1A)
      return (i << 24) | (i << 16) | (i << 8) | a;
    } else {
      u32 i = nib * 17;                      // I4 -> intensity replicated to R,G,B AND alpha
      return (i << 24) | (i << 16) | (i << 8) | i;   // HW: I formats set alpha = intensity
    }
  } else if constexpr(K == TK_YUV) {              // YUV 4:2:2 (UYVY pairs) via SET_CONVERT
    // TMEM layout per 32-bit texel pair [U, Y0, V, Y1]: luma at the odd byte of each texel,
    // chroma at the even bytes and shared across the pair (4:2:2). Convert with the
    // SET_CONVERT coefficients: R=Y+K0*V, G=Y+K1*U+K2*V, B=Y+K3*U (V,U signed about 128,
    // K/128 scale, HW rounds the >>7).
    u32 off = (base + (u32)t * rowBytes + (u32)s * 2) ^ ox;
    u32 pairBase = (base + (u32)t * rowBytes + (s & ~1u) * 2) ^ ox;
    if(pairBase + 2 >= 0x1000 || off + 1 >= 0x1000) return 0;
    int Y = tmem[off + 1], dU = (int)tmem[pairBase] - 128, dV = (int)tmem[pairBase + 2] - 128;
    auto cl = [](int v) -> u32 { return (u32)(v < 0 ? 0 : v > 255 ? 255 : v); };
    u32 r = cl(Y + ((k0 * dV + 0x40) >> 7));
    u32 g = cl(Y + ((k1 * dU + k2 * dV + 0x40) >> 7));
    u32 b = cl(Y + ((k3 * dU + 0x40) >> 7));
    return (r << 24) | (g << 16) | (b << 8) | 0xff;   // opaque
  } else if constexpr(K == TK_IA16 || K == TK_RGBA16) {        // 16-bit texels
    u32 off = (base + (u32)t * rowBytes + (u32)s * 2) ^ ox;
    if(off + 1 >= 0x1000) return 0;
    u16 px = ((u16)tmem[off] << 8) | tmem[off + 1];
    if constexpr(K == TK_IA16) {
      u32 i = (px >> 8) & 0xff, a = px & 0xff;                 // IA16
      return (i << 24) | (i << 16) | (i << 8) | a;
    } else {
      u32 r = exp5((px >> 11) & 0x1f);        // RGBA5551 (default)
      u32 g = exp5((px >> 6)  & 0x1f);
      u32 b = exp5((px >> 1)  & 0x1f);
      u32 a = (px & 1) ? 255 : 0;
      return (r << 24) | (g << 16) | (b << 8) | a;
    }
  } else if constexpr(K == TK_RGBA32) {                        // 32-bit RGBA8888
    u32 off = (base + (u32)t * rowBytes + (u32)s * 4) ^ ox;
    if(off + 3 >= 0x1000) return 0;
    return ((u32)tmem[off] << 24) | ((u32)tmem[off+1] << 16) | ((u32)tmem[off+2] << 8) | tmem[off+3];
  } else if constexpr(K == TK_CI8 || K == TK_IA8 || K == TK_I8) {   // 8-bit texels
    u32 off = (base + (u32)t * rowBytes + (u32)s) ^ ox;
    if(off >= 0x1000) return 0;
    u8 v = tmem[off];
    if constexpr(K == TK_CI8) return tlutEntry(v);              // full 256-entry palette
    else if constexpr(K == TK_IA8) {
      u32 i = (v >> 4) * 17, a = (v & 0xf) * 17;                // IA8 (4/4)
      return (i << 24) | (i << 16) | (i << 8) | a;
    } else {
      return ((u32)v << 24) | ((u32)v << 16) | ((u32)v << 8) | v;   // I8 -> grey opaque
    }
  } else {
    return 0xffffffff;
  }
}

template<u32 K>
auto SoftRdp::texelK(const TexFold& f, int s, int t) const -> u32 {
  return fetchK<K>(f, foldCoord(s, f.shiftS, f.maskS, f.cmS, f.sMax),
                      foldCoord(t, f.shiftT, f.maskT, f.cmT, f.tMax));
}

// Despacho de formato -> especializacion. `KEST_TEXKINDS(X)` lista los formatos una sola vez
// para que el muestreo suelto y el filtro compartan exactamente el mismo juego.
#define KEST_TEXKINDS(X) X(TK_CI4) X(TK_IA4) X(TK_I4) X(TK_YUV) X(TK_IA16) \
                         X(TK_RGBA16) X(TK_RGBA32) X(TK_CI8) X(TK_IA8) X(TK_I8)

auto SoftRdp::texelAt(const TexFold& f, int s, int t) const -> u32 {
  switch(f.kind) {
#define KEST_CASE(K) case K: return texelK<K>(f, s, t);
    KEST_TEXKINDS(KEST_CASE)
#undef KEST_CASE
    default: return 0xffffffff;
  }
}

auto SoftRdp::sampleTexel(u32 tileIdx, int s, int t) -> u32 {
  return texelAt(foldOf(tileIdx), s, t);
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
  u32 rowBytes = tl.line * 8, base = tl.tmem * 8, ox = (t & 1) ? 4u : 0u;   // fila impar: ver fetchK
  if(tl.size == 0) {                                    // CI4 (4-bit index)
    u32 off = (base + (u32)t * rowBytes + (u32)s / 2) ^ ox;
    if(off >= 0x1000) return 0;
    u8 nib = (s & 1) ? (tmem[off] & 0xf) : (tmem[off] >> 4);
    return tl.palette * 16 + nib;
  }
  u32 off = (base + (u32)t * rowBytes + (u32)s) ^ ox;   // CI8 (8-bit index)
  return off < 0x1000 ? tmem[off] : 0;
}

template<u32 K>
auto SoftRdp::filterK(const TexFold& f, double s, double t) const -> u32 {
  // Point sample unless SAMPLE_TYPE (other_hi bit 13 = full mode bit 45) selects the N64's
  // 3-point ("bilinear") filter. The RDP is not a 4-tap bilinear: it picks the triangle of
  // texels around the sample and lerps by the fractional coords. No -0.5 GL-style bias: the
  // RDP addresses texel origins directly, sfrac/tfrac are the low fractional bits of S/T.
  if(g_noFilter || !((other_hi >> 13) & 1))
    return texelK<K>(f, (int)std::floor(s), (int)std::floor(t));
  // Hardware works in 10.5 fixed point and lerps in integers, so do the same: a double lerp
  // rounded at the end lands on a different level whenever the exact result sits on a .5
  // boundary, and the 5-bit framebuffer then quantizes that difference into a visible band.
  // Weights are the 5-bit S/T fractions, the rounding is +0x10 before an arithmetic >>5 (so a
  // negative slope truncates toward -inf, as on HW).
#if defined(__SSE4_1__)
  const __m128i sti = stFixed(s, t);
  const int si = _mm_cvtsi128_si32(sti), ti = _mm_extract_epi32(sti, 1);
#else
  int si = lroundExact(s * 32.0), ti = lroundExact(t * 32.0);
#endif
  int fx = si & 31, fy = ti & 31;
  int s0 = si >> 5, t0 = ti >> 5;
  auto ch = [](u32 c, int i) -> int { return (int)((c >> (24 - i * 8)) & 0xff); };
  // Cuatro pliegues, no seis: las tres tomas se reparten s0/s0+1 y t0/t0+1.
  const int sw0 = f.fastS ? (s0 & f.andS)     : foldCoord(s0,     f.shiftS, f.maskS, f.cmS, f.sMax),
            sw1 = f.fastS ? ((s0 + 1) & f.andS) : foldCoord(s0 + 1, f.shiftS, f.maskS, f.cmS, f.sMax),
            tw0 = f.fastT ? (t0 & f.andT)     : foldCoord(t0,     f.shiftT, f.maskT, f.cmT, f.tMax),
            tw1 = f.fastT ? ((t0 + 1) & f.andT) : foldCoord(t0 + 1, f.shiftT, f.maskT, f.cmT, f.tMax);
  u32 c10 = fetchK<K>(f, sw1, tw0), c01 = fetchK<K>(f, sw0, tw1);
  const bool midTexel = ((other_hi >> 12) & 1) && fx == 16 && fy == 16;
#if defined(__SSE4_1__)
  // Los cuatro canales hacen la misma cuenta entera: un vector de 4x int32. Mismos
  // operandos, mismos desplazamientos aritmeticos, mismo clamp final => mismos valores.
  {
    const __m128i v10 = unpackRgba16(c10), v01 = unpackRgba16(c01);
    if(midTexel) {
      const __m128i v00 = unpackRgba16(fetchK<K>(f, sw0, tw0)),
                    v11 = unpackRgba16(fetchK<K>(f, sw1, tw1));
      __m128i sum = _mm_add_epi16(_mm_add_epi16(v00, v10), _mm_add_epi16(v01, v11));
      return packRgba16Clamped(_mm_srai_epi16(_mm_add_epi16(sum, _mm_set1_epi16(2)), 2));
    }
    const bool upper = fx + fy >= 32;
    const __m128i b = unpackRgba16(upper ? fetchK<K>(f, sw1, tw1) : fetchK<K>(f, sw0, tw0));
    const int wx = upper ? 32 - fy : fx, wy = upper ? 32 - fx : fy;
    __m128i acc = _mm_add_epi16(_mm_mullo_epi16(_mm_sub_epi16(v10, b), _mm_set1_epi16((short)wx)),
                                _mm_mullo_epi16(_mm_sub_epi16(v01, b), _mm_set1_epi16((short)wy)));
    acc = _mm_add_epi16(_mm_srai_epi16(_mm_add_epi16(acc, _mm_set1_epi16(0x10)), 5), b);
    return packRgba16Clamped(acc);
  }
#endif
  int o[4];
  if(midTexel) {
    // MID_TEXEL: at the exact centre of the quad the filter degenerates to the average of all
    // four texels (MPEG half-pel motion compensation).
    u32 c00 = fetchK<K>(f, sw0, tw0), c11 = fetchK<K>(f, sw1, tw1);
    for(int i = 0; i < 4; i++)
      o[i] = (ch(c00, i) + ch(c10, i) + ch(c01, i) + ch(c11, i) + 2) >> 2;
  } else {
    // The RDP is a 3-tap filter, not a 4-tap bilinear: it takes the triangle half the sample
    // falls in. Past the diagonal the base flips to the opposite corner and the weights flip
    // with it (and swap axes).
    bool upper = fx + fy >= 32;
    u32 base = upper ? fetchK<K>(f, sw1, tw1) : fetchK<K>(f, sw0, tw0);
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

auto SoftRdp::sampleTexFold(const TexFold& f, double s, double t) const -> u32 {
  // Despacho de formato: una vez por pixel (o por primitiva si el llamador ya lo saco del
  // bucle). A partir de aqui el filtro entero va especializado, con las 3-4 tomas alineadas.
  switch(f.kind) {
#define KEST_CASE(K) case K: return filterK<K>(f, s, t);
    KEST_TEXKINDS(KEST_CASE)
#undef KEST_CASE
    default: return 0xffffffff;
  }
}

auto SoftRdp::sampleTexFiltered(u32 tile, double s, double t) -> u32 {
  return sampleTexFold(foldOf(tile), s, t);
}

// --- texturas HD / realce (texpack) ---------------------------------------------------
// Semantica de los packs de Rice/GLideN64: el hash se toma de la textura en RDRAM con los
// parametros de la ULTIMA carga a esa direccion de TMEM y el tile con que se dibuja. Ver
// texpack.hpp y docs/TEXTURAS-HD.md. Nada de esto toca TMEM ni el muestreo normal: solo
// sustituye el texel cuando hay textura de reemplazo.

auto SoftRdp::hdNoteLoad(u32 t, bool block, u64 cmd) -> void {
  hdGen++;
  const Tile& tl = tiles[t & 7];
  HdLoad& li = hdLoads[tl.tmem & 0x1ff];
  const u32 sl = (u32)((cmd >> 44) & 0xfff) >> 2, tlo = (u32)((cmd >> 32) & 0xfff) >> 2;
  const u32 sh = (u32)((cmd >> 12) & 0xfff) >> 2, th = (u32)(cmd & 0xfff) >> 2;
  li = HdLoad{};
  li.valid = true;
  li.addr = ti_addr; li.texWidth = ti_width; li.size = ti_size;
  li.block = block;
  if(block) {
    li.dxt = (u32)(cmd & 0xfff);
  } else {
    li.uls = sl; li.ult = tlo;
    u32 w = (sh - sl + 1) & 0x3ff, h = (th - tlo + 1) & 0x3ff;
    if(tl.maskS) w = std::min(w, 1u << tl.maskS);
    if(tl.maskT) h = std::min(h, 1u << tl.maskT);
    li.width = w; li.height = h;
  }
}

auto SoftRdp::hdNoteTlut(Memory& mem, u32 t, u64 cmd) -> void {
  // GLideN64 copia la paleta tal cual esta en su RDRAM (palabras de 32 bits del anfitrion)
  // desde la direccion de la imagen, sin desplazar por SL/TL, a partir de la entrada
  // tmem-256. Se reproduce byte a byte: byte b del anfitrion = byte b^3 del invitado.
  hdGen++;
  const Tile& tl = tiles[t & 7];
  if(tl.tmem < 256) return;
  const u32 sl = (u32)((cmd >> 44) & 0xfff) >> 2, tlo = (u32)((cmd >> 32) & 0xfff) >> 2;
  const u32 sh = (u32)((cmd >> 12) & 0xfff) >> 2, th = (u32)(cmd & 0xfff) >> 2;
  const u32 count = ((sh - sl + 1) * (th - tlo + 1)) & 0xffff;
  const u32 start = (tl.tmem - 256) * 2;
  const auto& m = mem.rdram;
  for(u32 b = 0; b < count * 2 && start + b < sizeof hdPal; b++) {
    const usize a = (usize)((ti_addr + b) ^ 3);
    hdPal[start + b] = a < m.size() ? m[a] : 0;
  }
}

namespace {
// Rice: deduce las palabras por fila de un LOAD_BLOCK a partir de su DXT (la misma cuenta
// que ReverseDXT de Rice Video / GLideN64; implementacion propia).
auto txl2Words(u32 width, u32 size) -> u32 {
  static const u32 kBytes[4] = {0, 1, 2, 4};
  return size == 0 ? std::max(1u, width / 16) : std::max(1u, width * kBytes[size] / 8);
}
auto reverseDxt(u32 val, u32 width, u32 size) -> u32 {
  if(val == 0x800) return 1;
  auto calc = [](u32 w) -> u32 { return w == 0 ? 1 : (2048 + w - 1) / w; };
  int low = 2047 / (int)val;
  if(calc((u32)low) > val) low++;
  const int high = 2047 / (int)(val - 1);
  if(low == high) return (u32)low;
  for(int i = low; i <= high; i++)
    if(txl2Words(width, size) == (u32)i) return (u32)i;
  return (u32)((low + high) / 2);
}
}  // namespace

auto SoftRdp::hdBind(Memory& mem, u32 t) -> HdBind {
  const Tile& tl = tiles[t & 7];
  HdMemo& mm = hdMemo[t & 7];
  const u32 mode = tlutMode();
  if(mm.gen == hdGen && mm.mode == mode && !std::memcmp(&mm.tile, &tl, sizeof tl)) return mm.b;
  mm.gen = hdGen; mm.mode = mode; mm.tile = tl; mm.b = HdBind{};
  if(tl.fmt == 1) return mm.b;                       // YUV: no hay nada que sustituir
  const HdLoad& li = hdLoads[tl.tmem & 0x1ff];
  if(!li.valid || !li.addr) return mm.b;
  int bpl = 0, w = 0, h = 0;
  u32 addr = li.addr;
  if(!li.block) {
    bpl = (int)((li.texWidth << li.size) >> 1);
    addr += li.ult * (u32)bpl + (((li.uls << li.size) + 1) >> 1);
    w = (int)std::min(li.width, li.texWidth);
    if(li.size > tl.size) w <<= li.size - tl.size;
    h = (int)li.height;
  } else {
    const int tw = (int)((((tl.sh >> 2) - (tl.sl >> 2)) & 0x3ff) + 1);
    const int th = (int)((((tl.th >> 2) - (tl.tl >> 2)) & 0x3ff) + 1);
    const int mw = tl.maskS ? 1 << tl.maskS : tw, mh = tl.maskT ? 1 << tl.maskT : th;
    w = ((tl.cmS & 2) && tw <= 256) ? std::min(mw, tw) : mw;
    h = (((tl.cmT & 2) && th <= 256) || mh > 256) ? std::min(mh, th) : mh;
    if(tl.size == 3)      bpl = (int)(tl.line << 4);
    else if(li.dxt == 0)  bpl = (int)(tl.line << 3);
    else                  bpl = (int)(reverseDxt(li.dxt, (u32)w, tl.size) << 3);
  }
  if(w <= 0 || h <= 0 || w > 1024 || h > 1024 || bpl <= 0) return mm.b;
  const auto& m = mem.rdram;
  auto hb = [&](s64 i) -> u32 {
    const s64 a = ((s64)addr + i) ^ 3;
    return a >= 0 && (usize)a < m.size() ? m[(usize)a] : 0;
  };
  const bool usePal = tl.size < 2 && (mode != 0 || tl.fmt == 2);
  u64 crc = 0;
  if(usePal) {
    const u32 tcrc = texpack::riceCrc32(hb, 0, w, h, (int)tl.size, bpl);
    u32 cimax = 0;
    for(int y = 0; y < h && cimax < (tl.size ? 0xffu : 0xfu); y++) {
      if(tl.size == 1) for(int x = 0; x < w; x++) cimax = std::max(cimax, hb((s64)y * bpl + x));
      else for(int x = 0; x < w / 2; x++) {
        const u32 v = hb((s64)y * bpl + x);
        cimax = std::max({cimax, v >> 4, v & 15u});
      }
    }
    const s64 off = tl.size == 1 ? 0 : (s64)tl.palette * 32;
    auto pb = [&](s64 i) -> u32 { const s64 a = off + i; return a >= 0 && a < (s64)sizeof hdPal ? hdPal[a] : 0; };
    const u32 pcrc = texpack::riceCrc32(pb, 0, (int)cimax + 1, 1, 2, tl.size == 1 ? 512 : 32);
    crc = ((u64)pcrc << 32) | tcrc;
  }
  if(!crc) crc = texpack::riceCrc32(hb, 0, w, h, (int)tl.size, bpl);
  const texpack::Tex* tex = texpack::g_load ? texpack::find(crc, tl.fmt, tl.size) : nullptr;
  if(!tex && texpack::g_fx != texpack::FxNone) tex = texpack::enhanced(crc, tl.fmt, tl.size);
  const bool wantDump = texpack::g_dump && !texpack::dumped(crc, tl.fmt, tl.size);
  const bool wantFx = !tex && texpack::g_fx != texpack::FxNone;
  if(wantDump || wantFx) {
    // La textura tal como la ve el muestreador, leida de TMEM sin pliegue (sin mask,
    // clamp ni shift): texel (s, t) de la imagen cargada.
    TexFold f = foldOf(t);
    f.shiftS = f.shiftT = f.maskS = f.maskT = f.cmS = f.cmT = 0;
    f.sMax = f.tMax = 4095;
    f.fastS = f.fastT = false;
    std::vector<u32> px((usize)w * h);
    for(int y = 0; y < h; y++)
      for(int x = 0; x < w; x++) px[(usize)y * w + x] = texelAt(f, x, y);
    if(wantDump) texpack::dump(crc, tl.fmt, tl.size, usePal, w, h, px.data());
    if(wantFx) tex = texpack::enhance(crc, tl.fmt, tl.size, w, h, px.data());
  }
  if(!tex || tex->lv.empty()) return mm.b;
  mm.b.tex = tex;
  mm.b.scX = (double)tex->w / w;
  mm.b.scY = (double)tex->h / h;
  mm.b.f = foldOf(t);
  return mm.b;
}

auto SoftRdp::hdSample(const HdBind& b, double s, double t, double fp) const -> u32 {
  // Mismo pliegue que `foldCoord` (shift, clamp, mirror, mask) pero en coordenada continua:
  // el texel entero se pliega igual que en el original y la fraccion va con el (invertida
  // en el tramo espejo). Luego se escala a texeles HD.
  auto fold = [](double c, u32 sh, u32 mask, u32 cm, s64 lim) -> double {
    if(sh) c = sh <= 10 ? c / (double)(1 << sh) : c * (double)(1 << (16 - sh));
    s64 i = (s64)std::floor(c);
    double fr = c - (double)i;
    if((cm & 2) || mask == 0) {
      if(c < 0) { i = 0; fr = 0; }
      else if(i > lim) { i = lim; fr = 0.999; }
    }
    if(mask) {
      if((cm & 1) && ((i >> mask) & 1)) { i = ~i; fr = 1.0 - fr; }
      i &= ((s64)1 << mask) - 1;
    }
    return (double)i + fr;
  };
  const TexFold& f = b.f;
  const texpack::Tex& tx = *b.tex;
  double u = fold(s, f.shiftS, f.maskS, f.cmS, (s64)f.sMax) * b.scX;
  double v = fold(t, f.shiftT, f.maskT, f.cmT, (s64)f.tMax) * b.scY;
  // Nivel: texeles HD por pixel = fp * escala. log2 entero hacia abajo, sin mezclar niveles.
  int L = 0;
  for(double k = fp * std::max(b.scX, b.scY); k >= 2.0 && L + 1 < (int)tx.lv.size(); k *= 0.5) L++;
  const int lw = tx.lw[L], lh = tx.lh[L];
  u *= (double)lw / tx.w; v *= (double)lh / tx.h;
  const u32* p = tx.lv[L].data();
  auto ix = [](s64 i, int n) -> int {
    i %= n;
    return (int)(i < 0 ? i + n : i);
  };
  if(!((other_hi >> 13) & 1))
    return p[(usize)ix((s64)std::floor(v), lh) * lw + ix((s64)std::floor(u), lw)];
  u -= 0.5; v -= 0.5;
  const s64 x0 = (s64)std::floor(u), y0 = (s64)std::floor(v);
  const int fx = (int)((u - (double)x0) * 256), fy = (int)((v - (double)y0) * 256);
  const int xa = ix(x0, lw), xb = ix(x0 + 1, lw);
  const int ya = ix(y0, lh), yb = ix(y0 + 1, lh);
  const u32 c00 = p[(usize)ya * lw + xa], c10 = p[(usize)ya * lw + xb];
  const u32 c01 = p[(usize)yb * lw + xa], c11 = p[(usize)yb * lw + xb];
  u32 r = 0;
  for(int sft = 0; sft < 32; sft += 8) {
    const int a = (int)((c00 >> sft) & 255), bb = (int)((c10 >> sft) & 255);
    const int c = (int)((c01 >> sft) & 255), d = (int)((c11 >> sft) & 255);
    const int top = a * 256 + (bb - a) * fx, bot = c * 256 + (d - c) * fx;
    r |= (u32)((top * 256 + (bot - top) * fy + 32768) >> 16) << sft;
  }
  return r;
}

auto SoftRdp::buildCombPlan() -> void {
  // Traduce cada selector a una fila de la tabla de fuentes. Las tablas siguen una a una a
  // los `switch` de `combineColorSlow`, que es la referencia: mismo caso, misma fuente.
  auto mapA = [](int i) -> u32 {   // RGB sub_a (0..15); 6 = ONE (0x100), 7 = NOISE
    switch(i) { case 0: return CR_CIN; case 1: return CR_TEX0; case 2: return CR_TEX1;
      case 3: return CR_PRIM; case 4: return CR_SHADE; case 5: return CR_ENV;
      case 6: return CR_ONE; case 7: return CR_NOISE; default: return CR_ZERO; }
  };
  auto mapB = [](int i) -> u32 {   // RGB sub_b (0..15); 6 = key center, 7 = K4 -> 0
    switch(i) { case 0: return CR_CIN; case 1: return CR_TEX0; case 2: return CR_TEX1;
      case 3: return CR_PRIM; case 4: return CR_SHADE; case 5: return CR_ENV;
      default: return CR_ZERO; }
  };
  auto mapC = [](int i) -> u32 {   // RGB mul (0..31); 6 = key scale, 15 = K5 -> 0
    switch(i) { case 0: return CR_CIN; case 1: return CR_TEX0; case 2: return CR_TEX1;
      case 3: return CR_PRIM; case 4: return CR_SHADE; case 5: return CR_ENV;
      case 7: return CR_CINA; case 8: return CR_TEX0A; case 9: return CR_TEX1A;
      case 10: return CR_PRIMA; case 11: return CR_SHADEA; case 12: return CR_ENVA;
      case 13: return CR_LOD; case 14: return CR_PLOD; default: return CR_ZERO; }
  };
  auto mapD = [](int i) -> u32 {   // RGB add (0..7); 6 = ONE
    switch(i) { case 0: return CR_CIN; case 1: return CR_TEX0; case 2: return CR_TEX1;
      case 3: return CR_PRIM; case 4: return CR_SHADE; case 5: return CR_ENV;
      case 6: return CR_ONE; default: return CR_ZERO; }
  };
  auto mapAABD = [](int i) -> u32 {   // alpha sub_a / sub_b / add (0..7); 6 = ONE
    switch(i) { case 0: return CR_CINA; case 1: return CR_TEX0A; case 2: return CR_TEX1A;
      case 3: return CR_PRIMA; case 4: return CR_SHADEA; case 5: return CR_ENVA;
      case 6: return CR_ONE; default: return CR_ZERO; }
  };
  auto mapAC = [](int i) -> u32 {     // alpha mul (0..7); 0 = LOD_FRAC, 6 = PRIM_LOD_FRAC
    switch(i) { case 0: return CR_LOD; case 1: return CR_TEX0A; case 2: return CR_TEX1A;
      case 3: return CR_PRIMA; case 4: return CR_SHADEA; case 5: return CR_ENVA;
      case 6: return CR_PLOD; default: return CR_ZERO; }
  };
  CombPlan p;
  p.two = cycleType() == 1;
  p.fast = true;
  p.need = 0;
  for(int c = 0; c < 2; c++) {
    const CombSet& cs = comb[c];
    u32 r[8] = { mapA(cs.aR), mapB(cs.bR), mapC(cs.cR), mapD(cs.dR),
                 mapAABD(cs.aA), mapAABD(cs.bA), mapAC(cs.cA), mapAABD(cs.dA) };
    // 2-cycle: por el desfase de la tuberia el ciclo 1 ve los texels cruzados (su TEXEL0
    // es el TEXEL1 del pixel y viceversa; parallel-rdp shading.h).
    if(p.two && c == 1)
      for(u32& v : r) v = v == CR_TEX0 ? CR_TEX1 : v == CR_TEX1 ? CR_TEX0
                        : v == CR_TEX0A ? CR_TEX1A : v == CR_TEX1A ? CR_TEX0A : v;
    for(int k = 0; k < 8; k++) p.sel[c][k] = (u8)r[k];
    // Solo cuentan (para NOISE y para la mascara) los ciclos que de verdad se ejecutan:
    // en 1-cycle el hardware evalua la ecuacion del segundo ciclo y el primero no existe.
    if(!p.two && c == 0) continue;
    for(int k = 0; k < 8; k++) {
      if(r[k] == CR_NOISE) { p.fast = false; continue; }
      p.need |= 1u << r[k];
    }
  }
  p.keyHi = combine_hi; p.keyLo = combine_lo; p.keyCyc = cycleType();
  combPlan = p;
}

auto SoftRdp::combineColor(u32 tex0, u32 tex1, u32 shade) -> u32 {
  if(combPlan.keyHi != combine_hi || combPlan.keyLo != combine_lo
     || combPlan.keyCyc != cycleType()) buildCombPlan();
  const CombPlan& p = combPlan;
  if(!p.fast) return combineColorSlow(tex0, tex1, shade);   // NOISE: orden de std::rand() intacto
#if defined(__SSE4_1__)
  // Los cuatro canales corren la MISMA ecuacion entera con operandos distintos, asi que
  // van en un solo vector de 4x int32 (carriles R,G,B,A). Los selectores de alpha son
  // otros que los de RGB: el carril 3 se trae de su fila con un blend (mascara 0xC0 de
  // `_mm_blend_epi16` = los bytes 12..15 = el int de indice 3). Aritmetica entera pura,
  // mismos valores exactos que el camino escalar de abajo, que sigue como referencia.
  {
    const __m128i k80 = _mm_set1_epi32(0x80), k100 = _mm_set1_epi32(0x100),
                  k1ff = _mm_set1_epi32(0x1ff);
    __m128i rowv[CR_ROWS];
    auto put4 = [&](u32 r, u32 c) {
      rowv[r] = _mm_set_epi32((int)(c & 0xff), (int)((c >> 8) & 0xff),
                              (int)((c >> 16) & 0xff), (int)((c >> 24) & 0xff));
    };
    const u32 need = p.need;
    if(need & (1u << CR_CIN))   rowv[CR_CIN] = _mm_loadu_si128((const __m128i*)combined);
    if(need & (1u << CR_CINA))  rowv[CR_CINA]  = _mm_set1_epi32(combined[3]);
    if(need & (1u << CR_TEX0))  put4(CR_TEX0, tex0);
    if(need & (1u << CR_TEX1))  put4(CR_TEX1, tex1);
    if(need & (1u << CR_PRIM))  put4(CR_PRIM, prim_color);
    if(need & (1u << CR_SHADE)) put4(CR_SHADE, shade);
    if(need & (1u << CR_ENV))   put4(CR_ENV, env_color);
    if(need & (1u << CR_TEX0A)) rowv[CR_TEX0A] = _mm_set1_epi32((int)(tex0 & 0xff));
    if(need & (1u << CR_TEX1A)) rowv[CR_TEX1A] = _mm_set1_epi32((int)(tex1 & 0xff));
    if(need & (1u << CR_PRIMA)) rowv[CR_PRIMA] = _mm_set1_epi32((int)(prim_color & 0xff));
    if(need & (1u << CR_SHADEA))rowv[CR_SHADEA]= _mm_set1_epi32((int)(shade & 0xff));
    if(need & (1u << CR_ENVA))  rowv[CR_ENVA]  = _mm_set1_epi32((int)(env_color & 0xff));
    if(need & (1u << CR_ONE))   rowv[CR_ONE]   = _mm_set1_epi32(0x100);
    if(need & (1u << CR_ZERO))  rowv[CR_ZERO]  = _mm_setzero_si128();
    if(need & (1u << CR_LOD))   rowv[CR_LOD]   = _mm_set1_epi32(lodFrac());
    if(need & (1u << CR_PLOD))  rowv[CR_PLOD]  = _mm_set1_epi32((int)prim_lod_frac);
    // special_expand vectorizado: (v-0x80) & 0x1ff, extension de signo desde el bit 8, +0x80.
    auto sexpv = [&](__m128i v) -> __m128i {
      __m128i x = _mm_and_si128(_mm_sub_epi32(v, k80), k1ff);
      x = _mm_sub_epi32(_mm_xor_si128(x, k100), k100);
      return _mm_add_epi32(x, k80);
    };
    auto runCycle = [&](int c) -> __m128i {
      const u8* s = p.sel[c];
      auto pick = [&](int i) { return _mm_blend_epi16(rowv[s[i]], rowv[s[i + 4]], 0xC0); };
      __m128i A = sexpv(pick(0)), B = sexpv(pick(1)), C = pick(2), D = sexpv(pick(3));
      __m128i t = _mm_mullo_epi32(_mm_sub_epi32(A, B), C);
      return _mm_add_epi32(_mm_srai_epi32(_mm_add_epi32(t, k80), 8), D);
    };
    __m128i fin;
    if(p.two) {
      __m128i mid = runCycle(0);
      // COMBINED del segundo ciclo = resultado CRUDO del primero (sin clamp).
      if(need & (1u << CR_CIN))  rowv[CR_CIN]  = mid;
      if(need & (1u << CR_CINA)) rowv[CR_CINA] = _mm_shuffle_epi32(mid, 0xff);
      fin = runCycle(1);
    } else {
      fin = runCycle(1);
    }
    __m128i cl = sexpv(fin);
    cl = _mm_min_epi32(_mm_max_epi32(cl, _mm_setzero_si128()), _mm_set1_epi32(255));
    _mm_storeu_si128((__m128i*)combined, cl);
    u32 rgba = ((u32)combined[0] << 24) | ((u32)combined[1] << 16)
             | ((u32)combined[2] << 8)  |  (u32)combined[3];
    combined[3] += (combined[3] + 1) >> 8;   // alpha latcheado = EXPANDIDO (0xff -> 0x100)
    return rgba;
  }
#endif
  // Filas de fuentes. Las escalares (alpha, ONE, LOD...) se replican en los 4 carriles para
  // que el camino RGB indexe por canal y el de alpha por el carril 3 sin ramas extra.
  int rows[CR_ROWS][4];
  auto put4 = [&](u32 r, u32 c) {
    rows[r][0] = (int)((c >> 24) & 0xff); rows[r][1] = (int)((c >> 16) & 0xff);
    rows[r][2] = (int)((c >> 8)  & 0xff); rows[r][3] = (int)( c        & 0xff);
  };
  auto bc = [&](u32 r, int v) { rows[r][0] = rows[r][1] = rows[r][2] = rows[r][3] = v; };
  const u32 need = p.need;
  if(need & (1u << CR_CIN))   { for(int i = 0; i < 4; i++) rows[CR_CIN][i] = combined[i]; }
  if(need & (1u << CR_CINA))  bc(CR_CINA, combined[3]);
  if(need & (1u << CR_TEX0))  put4(CR_TEX0, tex0);
  if(need & (1u << CR_TEX1))  put4(CR_TEX1, tex1);
  if(need & (1u << CR_PRIM))  put4(CR_PRIM, prim_color);
  if(need & (1u << CR_SHADE)) put4(CR_SHADE, shade);
  if(need & (1u << CR_ENV))   put4(CR_ENV, env_color);
  if(need & (1u << CR_TEX0A)) bc(CR_TEX0A, (int)(tex0 & 0xff));
  if(need & (1u << CR_TEX1A)) bc(CR_TEX1A, (int)(tex1 & 0xff));
  if(need & (1u << CR_PRIMA)) bc(CR_PRIMA, (int)(prim_color & 0xff));
  if(need & (1u << CR_SHADEA))bc(CR_SHADEA,(int)(shade & 0xff));
  if(need & (1u << CR_ENVA))  bc(CR_ENVA,  (int)(env_color & 0xff));
  if(need & (1u << CR_ONE))   bc(CR_ONE,  0x100);
  if(need & (1u << CR_ZERO))  bc(CR_ZERO, 0);
  if(need & (1u << CR_LOD))   bc(CR_LOD,  lodFrac());
  if(need & (1u << CR_PLOD))  bc(CR_PLOD, (int)prim_lod_frac);
  // special_expand + ecuacion de 9 bits, identicas a la version generica.
  auto sexp = [](int v) -> int { int x = (v - 0x80) & 0x1ff; if(x & 0x100) x |= ~0x1ff; return x + 0x80; };
  auto eq = [&](int a, int b, int c, int dd) -> int {
    a = sexp(a); b = sexp(b); dd = sexp(dd);
    return (((a - b) * c + 0x80) >> 8) + dd;
  };
  auto runCycle = [&](int c, int* out) {
    const u8* s = p.sel[c];
    for(int i = 0; i < 3; i++)
      out[i] = eq(rows[s[0]][i], rows[s[1]][i], rows[s[2]][i], rows[s[3]][i]);
    out[3] = eq(rows[s[4]][3], rows[s[5]][3], rows[s[6]][3], rows[s[7]][3]);
  };
  int mid[4], fin[4];
  if(p.two) {                       // 2-cycle: cycle0 (raw) -> COMBINED -> cycle1
    runCycle(0, mid);
    // COMBINED del segundo ciclo es el resultado CRUDO del primero, no el clampado.
    if(need & (1u << CR_CIN))  { for(int i = 0; i < 4; i++) rows[CR_CIN][i] = mid[i]; }
    if(need & (1u << CR_CINA)) bc(CR_CINA, mid[3]);
    runCycle(1, fin);
  } else {                          // 1-cycle usa la ecuacion del segundo ciclo
    runCycle(1, fin);
  }
  auto clampN = [&](int v) -> int { int x = sexp(v); return x < 0 ? 0 : x > 255 ? 255 : x; };
  for(int i = 0; i < 4; i++) combined[i] = clampN(fin[i]);
  u32 rgba = ((u32)combined[0] << 24) | ((u32)combined[1] << 16)
           | ((u32)combined[2] << 8)  |  (u32)combined[3];
  // El alpha latcheado es el EXPANDIDO: 0xff pasa a 0x100 (ver version generica).
  combined[3] += (combined[3] + 1) >> 8;
  return rgba;
}

auto SoftRdp::combineColorSlow(u32 tex0, u32 tex1, u32 shade) -> u32 {
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
    std::swap(tex0, tex1);               // el ciclo 1 ve los texels cruzados (ver buildCombPlan)
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
  setPrimDz(other_lo & 4, 0, 0);   // un rect no tiene pendiente de z
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
  // Mipmap / TEXEL1 como en los triangulos (parallel-rdp op_texture_rectangle: el rect es
  // un primitivo con DsDx<<11 por +x y DtDy<<11 por +y, ejes cruzados con FLIP, nivel
  // maximo 0 y recorrido siempre +x). En unidades de 1/32 de texel.
  bool usesLod = false, usesTex1 = false;
  if(cycleType() < 2) texNeeds(usesLod, usesTex1);
  TexFold folds[8];
  if(usesLod || usesTex1) for(u32 i = 0; i < 8; i++) folds[i] = foldOf(i);
  const s32 rdsdx = (s16)((c1 >> 16) & 0xffff), rdtdy = (s16)(c1 & 0xffff);
  auto yCut = [](s32 r) -> double { return (double)(s32)(((u32)r << 11) & ~0x7fffu) / 65536.0; };
  const double lxS = flip ? 0 : rdsdx / 32.0, lxT = flip ? rdtdy / 32.0 : 0;
  const double lyS = flip ? yCut(rdsdx) : 0,  lyT = flip ? 0 : yCut(rdtdy);
  int X0 = std::max((int)std::ceil(xh), sx0), X1 = std::min((int)std::ceil(xl), sx1);
  int Y0 = std::max((int)std::ceil(yh), sy0), Y1 = std::min((int)std::ceil(yl), sy1);
  u64 rasterPx = 0, accW0 = pxWrites, accZ0 = pxZWrites;   // DPC counter accounting
  // Solo-coste: un texrect es un rectangulo recortado al scissor y NUNCA escribe el z
  // (no lleva pendiente de profundidad), asi que su coste sale del area sin rasterizar.
  if(costOnly) {
    int bx = X0 < 0 ? 0 : X0, by = Y0 < 0 ? 0 : Y0;
    u64 npx = u64(std::max(0, X1 - bx)) * u64(std::max(0, Y1 - by));
    for(int y = by; y < Y1 && X1 > bx; y++) addSpan(bx, u64(X1 - bx));
    accountPixels(mem, npx, npx, 0);
    return;
  }
  // Textura HD / realce (texpack). No en COPY a framebuffer de 8 bits (escribe indices).
  HdBind hdB;
  if(texpack::active() && !usesLod && !(cycleType() == 2 && ci_size == 1)) hdB = hdBind(mem, tile);
  const double hdFp = std::max(std::abs(dsdx), std::abs(dtdy));
  for(int y = Y0; y < Y1; y++) {
    if(y < 0) continue;
    if(X1 > std::max(X0, 0)) addSpan(std::max(X0, 0), u64(X1 - std::max(X0, 0)));
    for(int x = X0; x < X1; x++) {
      if(x < 0) continue;
      rasterPx++;
      double fx = x - xh, fy = y - yh;
      double s = flip ? s0 + dsdx * fy : s0 + dsdx * fx;
      double t = flip ? t0 + dtdy * fx : t0 + dtdy * fy;
      u32 tile0 = tile, tile1 = (tile + 1) & 7;
      if(usesLod) {
        const double cs = s * 32.0, ct = t * 32.0;
        auto fl = [](double v) { return (s32)std::floor(v); };
        lodFracV = lodSelect(fl(cs), fl(ct), fl(cs + lxS), fl(ct + lxT), fl(cs + lyS), fl(ct + lyT),
                             false, 0, tile0, tile1);
      }
      // point or N64 3-point per SAMPLE_TYPE
      u32 tex = hdB.tex ? hdSample(hdB, s, t, hdFp)
              : usesLod ? sampleTexFold(folds[tile0], s, t) : sampleTexFiltered(tile, s, t);
      u32 tex1 = usesTex1 ? sampleTexFold(folds[tile1], s, t) : tex;
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
      u32 c = (combProg && !copy) ? combineColor(tex, tex1, 0) : tex;
      // Alpha compare (ALPHA_COMPARE_EN, other_lo bit 0) has two HW forms:
      //  - COPY mode: 1-bit transparency — the texel is discarded when its alpha is 0
      //    (e.g. TLUT index → $0000). This is the classic copy-mode sprite key.
      //  - 1-/2-cycle: the COMBINED alpha is compared against the blend_color threshold
      //    (dither approximated by the same compare); fail when alpha < threshold.
      // Disabled → texel written regardless of its 5551 transparency bit (opaque mode).
      // (The old `tex & 0xff` gate wrongly dropped alpha-0 texels in every mode.)
      if(other_lo & 1) {
        if(copy ? (c & 0xff) == 0 : alphaRef(x, y, c) < (int)(blend_color & 0xff)) continue;
      }
      if(other_lo & 0x40) blendPixel(mem, x, y, c);      // blend against framebuffer
      else putPixel(mem, x, y, c);                        // opaque write
    }
  }
  lodFracV = 0xff;
  accountPixels(mem, rasterPx, pxWrites - accW0, pxZWrites - accZ0);
}

// Copia hasta `n` bytes de RDRAM a TMEM parando donde paraba el bucle byte a byte: los
// dos limites (0x1000 en TMEM, tamano de RDRAM) son monotonos y la copia va en orden
// ascendente, asi que recortar la cuenta una vez copia EXACTAMENTE el mismo prefijo.
template<typename V>
auto SoftRdp::copyRun(const V& m, u32 src, u32 dst, u32 n) -> void {
  if(dst >= 0x1000 || src >= m.size()) return;
  u32 lim = std::min(0x1000u - dst, (u32)(m.size() - src));
  if(n > lim) n = lim;
  if(n) std::memcpy(&tmem[dst], &m[src], n);
}

auto SoftRdp::loadTile(Memory& mem, u32 t, bool block, u64 cmd) -> void {
  if(texpack::active()) hdNoteLoad(t, block, cmd);
  // Copy texels from the texture image (RDRAM, ti_addr/ti_width/ti_size) into TMEM
  // at the tile's base. LOAD_TILE walks a [SL,TL]..[SH,TH] rectangle (fields in
  // 10.2); LOAD_BLOCK copies a contiguous run (SL..SH linear texels).
  //
  // Filas impares: el hardware guarda cada fila impar de TMEM con las dos mitades de 32 bits
  // de cada palabra de 64 intercambiadas, y el muestreador las deshace (fetchK). LOAD_TILE
  // intercambia las filas impares de la carga. LOAD_BLOCK no sabe de filas: lleva un contador
  // T que sube DXT (1.11) por palabra escrita, T = (i*DXT) >> 11, intercambia la palabra i
  // cuando T es impar y la coloca en i + T*line (el LINE del tile de carga, casi siempre 0).
  // Con DXT bien puesto la textura queda igual que con LOAD_TILE; con DXT = 0 no intercambia
  // nada, y quien carga asi tiene que traer las filas impares ya intercambiadas en RDRAM
  // (port nativo de PD). Ref: parallel-rdp tmem_update.comp. En 32 bpp el intercambio es de
  // pares de texeles: en este TMEM lineal de 4 B/texel, XOR 8 en vez de 4.
  Tile& tl = tiles[t & 7];
  const auto& m = mem.rdram;
  u32 bpt = ti_size == 3 ? 4 : ti_size == 2 ? 2 : ti_size == 1 ? 1 : 0;   // bytes/texel (0 = 4-bit)
  const u32 oxb = bpt == 4 ? 8u : 4u;
  // LOAD_BLOCK: nb bytes de RDRAM desde src, palabra a palabra con el contador DXT.
  auto blockCopy = [&](u32 src, u32 nb, u32 dxt) {
    const u32 dst = tl.tmem * 8;
    if(!dxt && !tl.line) { copyRun(m, src, dst, nb); return; }   // T = 0 siempre: copia recta
    for(u32 i = 0, done = 0; done < nb; i++, done += 8) {
      const u32 tt = (u32)(((u64)i * dxt) >> 11);
      const u32 w = dst + (i + tt * tl.line) * 8, x = (tt & 1) ? oxb : 0u;
      const u32 n = std::min(8u, nb - done);
      for(u32 b = 0; b < n; b++) {
        const u32 d = (w + b) ^ x, sa = src + done + b;
        if(d < 0x1000 && sa < m.size()) tmem[d] = m[sa];
      }
    }
  };
  u32 sl = (u32)((cmd >> 44) & 0xfff), tlo = (u32)((cmd >> 32) & 0xfff);
  u32 sh = (u32)((cmd >> 12) & 0xfff), th = (u32)((cmd >> 0) & 0xfff);
  // 4-bit texels (CI4/IA4/I4) pack 2/byte. LOAD_BLOCK is a linear byte copy of
  // ceil(texels/2) bytes; the SL/SH texel indices halve to byte offsets.
  if(!bpt) {
    tl.sl = sl; tl.tl = tlo; tl.sh = sh; tl.th = th;
    if(block) {
      u32 count = (sh >= sl) ? (sh - sl + 1) : 1, nb = (count + 1) / 2;
      blockCopy(ti_addr + (sl >> 1), nb, th);
      accountLoad(mem, 1, nb);
    } else {
      u32 s0 = sl >> 2, t0 = tlo >> 2, s1 = sh >> 2, t1 = th >> 2, rowBytes = tl.line * 8;
      accountLoad(mem, t1 - t0 + 1, (s1 - s0 + 2) / 2);
      for(u32 ty = t0; ty <= t1; ty++)
        for(u32 tx = s0; tx <= s1; tx++) {         // nibble-granular copy
          u32 src = ti_addr + (ty * ti_width + tx) / 2;   // ti_width in texels
          u32 dstByte = (tl.tmem * 8 + (ty - t0) * rowBytes + (tx - s0) / 2) ^ (((ty - t0) & 1) ? 4u : 0u);
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
    u32 nb = count * bpt;
    blockCopy(ti_addr + sl * bpt, nb, th);
    accountLoad(mem, 1, nb);
    return;
  }
  u32 s0 = sl >> 2, t0 = tlo >> 2, s1 = sh >> 2, t1 = th >> 2;
  u32 rowBytes = tl.line * 8;
  accountLoad(mem, t1 - t0 + 1, u64(s1 - s0 + 1) * bpt);
  // Los texeles de una fila son contiguos en el texture image (paso `bpt`) y tambien en
  // TMEM (paso `bpt` desde el inicio de la fila), asi que la fila entera es un solo run.
  // Copiar byte a byte con dos comprobaciones de limite POR BYTE salia el 7 % del hilo.
  const u32 rowRun = (s1 - s0 + 1) * bpt;
  for(u32 ty = t0; ty <= t1; ty++) {
    u32 src = ti_addr + (ty * ti_width + s0) * bpt;
    u32 dst = tl.tmem * 8 + (ty - t0) * rowBytes;
    if(!((ty - t0) & 1)) { copyRun(m, src, dst, rowRun); continue; }
    for(u32 b = 0; b < rowRun; b++) {                 // fila impar: mitades intercambiadas
      const u32 d = (dst + b) ^ oxb;
      if(d < 0x1000 && src + b < m.size()) tmem[d] = m[src + b];
    }
  }
}

auto SoftRdp::gpuFlush(Memory& mem) -> void {
  gpuQueued = false;
  static thread_local std::vector<gpurdp::TriOut> outs;
  gpurdp::flush(mem.rdram.data(), (u32)mem.rdram.size(), hiddenBits(mem), &outs);
  // Lo que dependia del resultado de cada triangulo, en el orden del FIFO: cobro (pixeles
  // escritos en color y z) y el registro COMBINED que dejo su ultimo pixel.
  for(size_t i = 0; i < gpuTris.size() && i < outs.size(); i++) {
    const GpuTri& t = gpuTris[i];
    const gpurdp::TriOut& o = outs[i];
    pxWrites += (u64)o.nWrite; pxZWrites += (u64)o.nZWrite;
    acctFinish(mem, t.acct, (u64)o.nWrite, (u64)o.nZWrite);
    if(t.comb) for(int k = 0; k < 4; k++) combined[k] = o.comb[k];
  }
  gpuTris.clear();
}

// Comandos que no leen ni escriben RDRAM: con primitivas en la GPU pendientes se pueden
// ejecutar sin bajarlas (cada primitiva encolada lleva ya su propio estado). Un FILL_RECTANGLE
// en FILL/COPY tambien, porque se encola detras. Todo lo demas (texrect, cargas de TMEM y TLUT,
// rellenos de 1/2 ciclos que mezclan con el framebuffer) necesita la RDRAM al dia.
static auto gpuNoFlush(u32 op, u32 cycle) -> bool {
  switch(op) {
  case 0x00: case 0x26: case 0x27: case 0x28: case 0x29: case 0x2a: case 0x2b: case 0x2c:
  case 0x2d: case 0x2e: case 0x2f: case 0x32: case 0x35: case 0x37: case 0x38: case 0x39:
  case 0x3a: case 0x3b: case 0x3c: case 0x3d: case 0x3e: case 0x3f:
    return true;
  case 0x36: return cycle >= 2;
  // Triangulos: drawTriangle decide; si el triangulo no va a la GPU vacia la cola el mismo.
  case 0x08: case 0x09: case 0x0a: case 0x0b: case 0x0c: case 0x0d: case 0x0e: case 0x0f:
    return true;
  default: return false;
  }
}

auto SoftRdp::run(Memory& mem, u32 start, u32 end, bool xbus) -> u32 {
  const auto& m = mem.rdram;
  const auto& dm = mem.dmem;
  // Command fetch source: RDRAM by physical address, or DMEM (12-bit wrap) in xbus
  // mode. Big-endian 64-bit word either way. Pixel writes still target RDRAM.
  auto fetch = [&](u32 a) -> u64 {
    if(!xbus) return cmdSrc ? rd64p(cmdSrc, a, (u32)m.size()) : rd64(m, a);
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
  bool split = false;                 // ultimo comando del span partido: no se ejecuta
  sawSyncFull = false;
  stopAt = cur;
  static bool lowCi = false; static u64 lowMark = 0;
  static const bool citrace = std::getenv("KESTREL_CITRACE") != nullptr;
  static bool ops = std::getenv("KESTREL_RDPOPS") != nullptr;
  static u32 hist[64] = {};
  while(cur < end && guard++ < 200000) {
    // Publicar el puntero de lectura ANTES de consumir el comando: el productor debe
    // ver como ocupado todo lo que aun no se ha leido.
    if(curOut) curOut->store(cur, std::memory_order_release);
    if(wrtag::tag) wrtag::moveWinLo(0, cur);
    u64 cmd = fetch(cur);
    u32 op = (cmd >> 56) & 0x3f;
    if(gpuQueued && !gpuNoFlush(op, cycleType())) gpuFlush(mem);
    const bool st = charge && mem.rdpStats.on.load(std::memory_order_relaxed);
    if(st) statsCmd(mem, op, cmd);
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
      // Comando partido por el borde del span: parar delante de el (ver stopAt).
      if(cur + (u32)n * 8 > end) break;
      for(int i = 0; i < n; i++) words[i] = fetch(cur + i * 8);
      drawTriangle(mem, words, n, op);
      cur += n * 8; executed++; continue;
    }

    // Carga TMEM redundante (rdp.stats): TMEM+TLUT identicas antes y despues de la carga.
    u8 snap[sizeof tmem + sizeof tlut];
    const bool stLoad = st && (op == 0x30 || op == 0x33 || op == 0x34);
    u64 stBytes0 = 0;
    if(stLoad) {
      std::memcpy(snap, tmem, sizeof tmem); std::memcpy(snap + sizeof tmem, tlut, sizeof tlut);
      stBytes0 = mem.rdpStats.loadBytes.load(std::memory_order_relaxed);
    }
    switch(op) {
    case 0x00: break;                                   // no-op
    case 0x26: accountStall(mem, 25); break;            // SYNC_LOAD: 25 GCLK fijos
    case 0x27: accountStall(mem, 50); break;            // SYNC_PIPE: 50 GCLK fijos
    case 0x28: accountStall(mem, 33); break;            // SYNC_TILE: 33 GCLK fijos
    case 0x29: {
      static const bool sfLog = std::getenv("KESTREL_DPSYNCLOG") != nullptr;
      if(sfLog) { std::fprintf(stderr, "[dpsync] at=%06x span=%06x..%06x xbus=%u\n", cur, start & 0x00ffffffu, end, (unsigned)xbus); std::fflush(stderr); }
      sawSyncFull = true; break; }               // SYNC_FULL → raises DP (see caller)
    case 0x24: case 0x25: {                             // TEXTURE_RECTANGLE (+flip)
      if(cur + 16 > end) { split = true; break; }       // partido: parar delante (ver stopAt)
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
    case 0x2e: prim_z = ((u32)(cmd >> 16) & 0x7fff) << 3; prim_dz = (u32)cmd & 0xffff; break;
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
      if(texpack::active()) hdNoteTlut(mem, t, cmd);
      //
      // La fuente es la de un LOAD_TILE de una sola fila: texel (SL..SH, TL) de la imagen,
      // o sea ti_addr + (TL * ti_width + s) * 2. Con TL = 0 sale lo de siempre, pero quien
      // guarda la paleta DETRAS de los texeles en el mismo bloque la direcciona con TL != 0
      // (ancho 1, TL = fila de la paleta); ignorar TL leia los texeles como paleta.
      u32 sl = ((u32)(cmd >> 44) & 0xfff) >> 2, sh = ((u32)(cmd >> 12) & 0xfff) >> 2;
      u32 row = ti_addr + (((u32)(cmd >> 32) & 0xfff) >> 2) * ti_width * 2;
      for(u32 i = sl; i <= sh; i++) {
        u32 src = row + i * 2;
        if(src + 1 < m.size()) tlut[(dst + (i - sl)) & 0xff] = ((u16)m[src] << 8) | m[src + 1];
      }
      if(sh >= sl) accountLoad(mem, 1, u64(sh - sl + 1) * 2);   // antes no cobraba nada
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
      prim_min_level = (u8)((cmd >> 40) & 31);
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
      // El suelo por defecto son los vectores de excepcion; KESTREL_CIFLOOR=<phys> lo
      // sube para cazar un framebuffer que aterriza sobre el codigo del juego.
      static const u32 ciFloor = []{ const char* e = std::getenv("KESTREL_CIFLOOR");
                                     return e ? (u32)std::strtoul(e, nullptr, 0) : 0x400u; }();
      if(ci_addr < ciFloor || citrace) {
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
    if(stLoad) {
      auto& rs = mem.rdpStats;
      rs.loads.fetch_add(1, std::memory_order_relaxed);
      if(!std::memcmp(snap, tmem, sizeof tmem) && !std::memcmp(snap + sizeof tmem, tlut, sizeof tlut)) {
        rs.loadRedundant.fetch_add(1, std::memory_order_relaxed);
        rs.loadRedundantBytes.fetch_add(rs.loadBytes.load(std::memory_order_relaxed) - stBytes0,
                                        std::memory_order_relaxed);
      }
    }
    if(split) break;                  // el switch solo pudo salir de si mismo
    cur += 8; executed++;
  }
  stopAt = cur;
  if(gpuQueued) gpuFlush(mem);   // fuera de run() la RDRAM siempre esta al dia
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
