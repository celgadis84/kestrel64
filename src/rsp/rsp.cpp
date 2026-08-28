// kestrel64 — RSP low-level interpreter implementation. See rsp.hpp.
#include "rsp.hpp"
#include "../core/memory.hpp"
#include "rspjit.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <immintrin.h>   // SSE2..SSE4.2 (Nehalem host: -march=native)
#include <chrono>
#include <vector>
#include <array>
#include <utility>

namespace kestrel {

// El reparto de instrucciones es diagnostico, no produccion: fuera del camino
// caliente salvo build instrumentado (-DKESTREL_VUSTAT=1).
#ifndef KESTREL_VUSTAT
#define KESTREL_VUSTAT 0
#endif

// --- SSE lane helpers (8×s16 = one __m128i; el[n] = lane n) ------------------
// R128.el is u16[8] (16 bytes) but not guaranteed 16-aligned → loadu/storeu.
static inline auto vload(const R128& r) -> __m128i {
  return _mm_load_si128(reinterpret_cast<const __m128i*>(r.el));
}
static inline auto vstore(R128& r, __m128i v) -> void {
  _mm_store_si128(reinterpret_cast<__m128i*>(r.el), v);
}
static inline auto vones() -> __m128i { __m128i z = _mm_setzero_si128(); return _mm_cmpeq_epi16(z, z); }
static inline auto vnot(__m128i x) -> __m128i { return _mm_xor_si128(x, vones()); }
// A flag register stores 0/1 per lane; build a 0x0000/0xffff select mask from it.
static inline auto vmaskFromFlag(const R128& f) -> __m128i {
  return _mm_cmpgt_epi16(vload(f), _mm_setzero_si128());   // 0/1 → 0xffff where ==1
}
// Reduce a 0x0000/0xffff mask back to 0/1 lanes for storing into a flag register.
static inline auto vflagFromMask(__m128i m) -> __m128i {
  return _mm_and_si128(m, _mm_set1_epi16(1));
}
static inline auto vstoreZero(R128& r) -> void { vstore(r, _mm_setzero_si128()); }

// Physical bases of the SP and DPC register blocks; COP0 on the RSP routes
// mfc0/mtc0 to these exactly as the CPU would over the RCP bus.
// --- reparto de instrucciones del RSP (KESTREL_VUSTAT=1) ---------------------
// Para decidir DONDE optimizar hace falta el mix real que ejecuta el microcodigo,
// no una corazonada. Contadores fuera del camino normal: una comprobacion de un
// bool estatico por instruccion cuando esta apagado.
struct VuStat {
  bool on = std::getenv("KESTREL_VUSTAT") != nullptr;
  u64 maj[64]{}, cop2[64]{}, cop2sse[64]{}, lwc2[32]{}, swc2[32]{}, total = 0;
  static auto nameCop2(u32 f) -> const char* {
    static const char* n[64] = {
      "VMULF","VMULU","VRNDP","VMULQ","VMUDL","VMUDM","VMUDN","VMUDH",
      "VMACF","VMACU","VRNDN","VMACQ","VMADL","VMADM","VMADN","VMADH",
      "VADD","VSUB","?12","VABS","VADDC","VSUBC","?16","?17",
      "?18","?19","?1a","?1b","?1c","VSAR","?1e","?1f",
      "VLT","VEQ","VNE","VGE","VCL","VCH","VCR","VMRG",
      "VAND","VNAND","VOR","VNOR","VXOR","VNXOR","?2e","?2f",
      "VRCP","VRCPL","VRCPH","VMOV","VRSQ","VRSQL","VRSQH","VNOP",
      "?38","?39","?3a","?3b","?3c","?3d","?3e","?3f" };
    return n[f & 63];
  }
  static auto nameLd(u32 s) -> const char* {
    static const char* n[32] = { "LBV","LSV","LLV","LDV","LQV","LRV","LPV","LUV",
                                 "LHV","LFV","LWV","LTV","?0c","?0d","?0e","?0f",
                                 "?10","?11","?12","?13","?14","?15","?16","?17",
                                 "?18","?19","?1a","?1b","?1c","?1d","?1e","?1f" };
    return n[s & 31];
  }
  ~VuStat() {
    if(!on || !total) return;
    std::fprintf(stderr, "[vustat] %llu instrucciones de RSP\n", (unsigned long long)total);
    static const char* majName[64] = {0};
    for(int i = 0; i < 64; i++) if(maj[i])
      std::fprintf(stderr, "[vustat] op mayor %02x  %6.2f%%  %llu\n", i,
                   100.0 * maj[i] / total, (unsigned long long)maj[i]);
    for(int i = 0; i < 64; i++) if(cop2[i])
      std::fprintf(stderr, "[vustat]   cop2 %-6s %6.2f%%  %10llu  sse %5.1f%%\n",
                   nameCop2(i), 100.0 * cop2[i] / total, (unsigned long long)cop2[i],
                   100.0 * cop2sse[i] / (double)cop2[i]);
    for(int i = 0; i < 32; i++) if(lwc2[i])
      std::fprintf(stderr, "[vustat]   LWC2 %-4s %6.2f%%  %10llu\n", nameLd(i),
                   100.0 * lwc2[i] / total, (unsigned long long)lwc2[i]);
    for(int i = 0; i < 32; i++) if(swc2[i])
      std::fprintf(stderr, "[vustat]   SWC2 %-4s %6.2f%%  %10llu\n", nameLd(i),
                   100.0 * swc2[i] / total, (unsigned long long)swc2[i]);
    std::fflush(stderr);
  }
};
static VuStat g_vustat;

static constexpr u32 PHYS_SP  = 0x0404'0000;
static constexpr u32 PHYS_DPC = 0x0410'0000;

static inline auto sclamp16(s32 x) -> s16 {
  if(x < -32768) return -32768;
  if(x >  32767) return  32767;
  return (s16)x;
}
static inline auto sclip48(s64 x) -> s64 {
  x &= (((s64)1 << 48) - 1);
  if(x & ((s64)1 << 47)) x |= ~(((s64)1 << 48) - 1);
  return x;
}
static inline auto clz(u32 v) -> u32 { return v ? (u32)__builtin_clz(v) : 32; }

// --- broadcast modifier -----------------------------------------------------
// Tabla de mascaras pshufb del modificador de elemento, como DATO CONSTANTE. Antes era un
// `static` local con constructor, o sea una comprobacion de guarda de inicializacion
// (atomica, thread-safe statics) en CADA operacion de COP2 — y COP2 es la mitad del
// tiempo del RSP. Se sigue derivando de la misma tabla de broadcast del wiki (n64brew):
// la salida de la banda n (bytes 2n,2n+1) toma la banda src[e][n] (bytes 2s,2s+1), asi que
// el resultado es byte a byte el mismo que el `v.el[n] = el[src[e][n]]` escalar.
namespace {
struct BcastMasks { u8 b[16][16]; };
constexpr auto makeBcastMasks() -> BcastMasks {
  const u8 src[16][8] = {
    {0,1,2,3,4,5,6,7}, {0,1,2,3,4,5,6,7},
    {0,0,2,2,4,4,6,6}, {1,1,3,3,5,5,7,7},
    {0,0,0,0,4,4,4,4}, {1,1,1,1,5,5,5,5}, {2,2,2,2,6,6,6,6}, {3,3,3,3,7,7,7,7},
    {0,0,0,0,0,0,0,0}, {1,1,1,1,1,1,1,1}, {2,2,2,2,2,2,2,2}, {3,3,3,3,3,3,3,3},
    {4,4,4,4,4,4,4,4}, {5,5,5,5,5,5,5,5}, {6,6,6,6,6,6,6,6}, {7,7,7,7,7,7,7,7},
  };
  BcastMasks m{};
  for(int e = 0; e < 16; e++)
    for(int n = 0; n < 8; n++) {
      u8 s = src[e][n];
      m.b[e][2 * n] = (u8)(2 * s); m.b[e][2 * n + 1] = (u8)(2 * s + 1);
    }
  return m;
}
constexpr BcastMasks kBcast = makeBcastMasks();

// Intercambio de los dos bytes de cada banda de 16 bits: DMEM guarda la banda en
// big-endian y el[] es un u16 del host. Dato constante a nivel de fichero (un `static`
// local aqui costaria una guarda de inicializacion por cuarteto cargado).
alignas(16) constexpr u8 kLaneSwap[16] = {1,0,3,2,5,4,7,6,9,8,11,10,13,12,15,14};
}  // namespace

auto rspBcastMask(u32 e) -> const void* { return kBcast.b[e & 15]; }

auto R128::bcast(u32 e) const -> __m128i {
  return _mm_shuffle_epi8(_mm_load_si128(reinterpret_cast<const __m128i*>(el)),
                          _mm_loadu_si128(reinterpret_cast<const __m128i*>(kBcast.b[e & 15])));
}
auto R128::operator()(u32 e) const -> R128 {
  R128 v;
  _mm_store_si128(reinterpret_cast<__m128i*>(v.el), bcast(e));
  return v;
}

Rsp::Rsp() {
  // Dynarec del RSP. Por defecto encendido, igual que el de la CPU, y con el interprete
  // como oraculo: KESTREL_RSPJIT=0 lo apaga y el md5 del framebuffer tiene que salir igual.
  {
    const char* v = std::getenv("KESTREL_RSPJIT");
    jitOn = !(v && v[0] == '0');
    if(const char* w = std::getenv("KESTREL_RSPJIT_WAYS")) {
      u32 n = (u32)std::strtoul(w, nullptr, 0);
      kJitWays = n < 1 ? 1 : (n > kJitWaysMax ? kJitWaysMax : n);
    }
    statsOn = std::getenv("KESTREL_RSPJIT_STATS") != nullptr;
    if(jitOn) {
      // Solo la primera imagen al arrancar; las demas ranuras se crean cuando aparece un
      // microcodigo distinto, que en un juego que no cambie de tarea no pasa nunca.
      jc = new rspjit::Cache();
      if(!jc->init(kJitWayBytes)) { std::fprintf(stderr, "[rspjit] RWX alloc fallo: dynarec apagado\n"); delete jc; jc = nullptr; jitOn = false; }
      else { jcWay[0] = jc; jcCur = 0; jcUse[0] = ++jcTick; }
    }
  }
  sse = !std::getenv("KESTREL_NORSPSSE");   // A/B toggle; default ON (proven by --rspfuzz)
  vecfast = !std::getenv("KESTREL_NOVECFAST");   // A/B; probado por --rspldfuzz
  // reciprocal / inverse-sqrt ROMs (generated exactly as the hardware tables).
  reciprocals[0] = (u16)~0;
  for(u32 i = 1; i < 512; i++) {
    u64 a = i + 512;
    u64 b = ((u64)1 << 34) / a;
    reciprocals[i] = (u16)((b + 1) >> 8);
  }
  for(u32 i = 0; i < 512; i++) {
    u64 a = (i + 512) >> (i % 2 == 1);
    u64 b = 1 << 17;
    while(a * (b + 1) * (b + 1) < ((u64)1 << 44)) b++;
    invSqrts[i] = (u16)(b >> 1);
  }
}

Rsp::~Rsp() { if(statsOn) jitStatsDump(); for(auto*& w : jcWay) { delete w; w = nullptr; } jc = nullptr; }

// Elige la tabla de bloques que corresponde al microcodigo que hay AHORA en IMEM.
//
//   1. La imagen activa no ha cambiado -> no hay nada que hacer (caso normal: F3DEX2 vuelve
//      a DMAear sus mismos 4 KB al empezar cada tarea).
//   2. Otra ranura tiene exactamente esta imagen -> cambiar de puntero. Cero invalidaciones y
//      cero recompilaciones: el codigo de esa tarea sigue vivo donde lo dejamos.
//   3. Imagen nueva. Si difiere POCO de la activa es un overlay parcheado sobre el
//      microcodigo vivo: se parchea la misma ranura y se conserva lo que no cambio. Si
//      difiere mucho es otro microcodigo: se ocupa la ranura menos usada y la activa queda
//      intacta para cuando su tarea vuelva.
auto Rsp::jitSelectImage(const u8* imem) -> void {
  if(!jitOn || !jc) return;
  // Se elige la tabla POR PARECIDO, no por igualdad. Medido en SM64: de 2000 cargas de
  // IMEM salen ~1400 imagenes distintas de 4 KB, porque cada microcodigo deja detras un
  // resto distinto del anterior (el de audio es mas corto que el de graficos) y porque el
  // arranque de tarea DMAea por trozos, asi que se ven estados intermedios. Exigir
  // igualdad exacta convierte la cache en un molino: casi todo es fallo. Comparando por
  // trozos de 8 B y quedandose con la tabla que menos difiere, volver a un microcodigo ya
  // visto solo recompila lo que de verdad cambio (el overlay), y nunca se tira la tabla
  // entera.
  u32 best = jcCur, bestDiff = jc->diffChunks(imem);
  for(u32 i = 0; i < kJitWays && bestDiff; i++) {
    if(!jcWay[i] || i == jcCur) continue;
    u32 d = jcWay[i]->diffChunks(imem);
    if(d < bestDiff) { bestDiff = d; best = i; }
  }
  if(!bestDiff) {   // imagen identica: solo cambiar de puntero
    jc = jcWay[best]; jcCur = best; jcUse[best] = ++jcTick; jcHits++;
    return;
  }
  jcMiss++;
  // Diferencia grande y ranura libre: estrenarla en vez de machacar una tabla util. Con
  // todas ocupadas se parchea la mas parecida, que es lo mas barato que hay.
  if(bestDiff >= kJitNewWay) {
    for(u32 i = 0; i < kJitWays; i++) {
      if(jcWay[i]) continue;
      auto* c = new rspjit::Cache();
      if(!c->init(kJitWayBytes)) { delete c; break; }
      jcWay[i] = c; best = i;
      break;
    }
  }
  jc = jcWay[best]; jcCur = best; jcUse[best] = ++jcTick;
  jc->syncImem(imem);   // invalida lo que difiera de SU sombra y la reanota
}

// La escritura de IMEM por DMA es la unica forma de que el microcodigo cambie mientras la
// tarea corre (carga de overlay), y ahi la huella que se comprueba en start() ya no vale:
// hay que tirar la tabla en el acto, antes de volver a entrar en ningun bloque.
// Cobertura del dynarec (KESTREL_RSPJIT_STATS=1). Lo que hay que mirar es el porcentaje de
// instrucciones de microcodigo que salen por codigo compilado: lo que quede en el interprete
// son bloques demasiado cortos o instrucciones que el compilador no absorbe todavia.
auto Rsp::jitStatsDump() -> void {
  if(!jc) return;
  u64 tot = jc->jitOps + jc->interpOps;
  u64 comp = 0, flu = 0, ent = 0, jops = 0, iops = 0; u32 ways = 0;
  for(auto* w : jcWay) if(w) { ways++; comp += w->compiles; flu += w->flushes;
                               ent += w->entries; jops += w->jitOps; iops += w->interpOps; }
  tot = jops + iops;
  std::fprintf(stderr, "[rspjit] ops jit=%llu (%.1f%%) interp=%llu | entradas=%llu bloques=%llu vaciados=%llu | imagenes: ranuras=%u aciertos=%llu fallos=%llu\n",
               (unsigned long long)jops, tot ? 100.0 * (double)jops / (double)tot : 0.0,
               (unsigned long long)iops, (unsigned long long)ent,
               (unsigned long long)comp, (unsigned long long)flu,
               ways, (unsigned long long)jcHits, (unsigned long long)jcMiss);
}

auto Rsp::jitInvalidate(u32 off, u32 bytes, const u8* imem) -> void {
  (void)off; (void)bytes;
  if(!jc) return;
  // OJO: no se puede usar bindMem() aqui. El DMA que carga microcodigo suele llegar
  // ANTES de la primera escritura a SP_STATUS, que es donde Memory le asigna a este Rsp
  // su puntero mem; tocarlo antes lee de un puntero nulo. La memoria del SP la trae
  // quien llama, que ya la tiene delante.
  //
  // Invalidacion POR CONTENIDO (rspjit::Cache::syncImem), no por rango escrito. El rango
  // dice donde ESCRIBIO el DMA, no que haya cambiado algo: F3DEX2 recarga su microcodigo
  // completo al empezar cada tarea y los 4 KB son identicos a los de la tarea anterior.
  // Comparando contra la sombra, esa recarga no invalida ni una ranura y solo los overlays
  // -- que si traen bytes distintos -- pagan recompilacion, y solo de lo que tocan.
  // Con el nucleo PARADO este DMA es la carga de microcodigo de la proxima tarea: no se toca
  // nada, que start() elegira imagen y una imagen ya vista no cuesta ni una recompilacion.
  // Invalidar aqui destruiria la tabla de la tarea anterior justo antes de poder
  // reconocerla. Con el nucleo corriendo es un overlay sobre la imagen viva y hay que
  // invalidar en el acto, antes de volver a entrar en ningun bloque.
  if(!running) return;
  // Corriendo y desde el hilo del nucleo: es el propio microcodigo cargandose (el stub de
  // arranque DMAea el ucode de la tarea) o un overlay. Estamos entre instrucciones
  // interpretadas -- el DMA se pide por COP0, que nunca entra en un bloque -- asi que cambiar
  // de imagen aqui es tan seguro como en start(), y es DONDE de verdad pasa el cambio de
  // tarea: si se invalida a ciegas se tira la tabla de la tarea anterior cada vez.
  if(std::this_thread::get_id() == rspThread) { jitSelectImage(imem); return; }
  jc->syncImem(imem);
  // El codigo emitido de los bloques muertos se queda en el buffer hasta el siguiente
  // reciclado: es un asignador de tope, no hay nada que liberar pieza a pieza.
}

// --- DMEM / IMEM access (12-bit wrapping, big-endian, unaligned OK) ----------
// --- copia rapida DMEM <-> registro vectorial --------------------------------
// El byte k del vector vive en el byte (k^1) de R128::el (u16 del host, DMEM es
// big-endian dentro de la banda). Para un tramo que empieza en un elemento PAR y
// tiene longitud PAR, el conjunto {k^1} es el mismo tramo: la conversion se reduce
// a intercambiar los bytes de cada pareja. Asi LSV/LLV/LDV/LQV (y sus tiendas)
// pasan de 2-16 lecturas de byte con lectura-modificacion-escritura sobre u16 a
// una o dos operaciones de 64 bits. Mismos bytes, mismo orden: no cambia semantica.
static inline auto laneSwap64(u64 x) -> u64 {
  return ((x & 0x00ff00ff00ff00ffull) << 8) | ((x >> 8) & 0x00ff00ff00ff00ffull);
}
static inline auto laneSwap32(u32 x) -> u32 {
  return ((x & 0x00ff00ffu) << 8) | ((x >> 8) & 0x00ff00ffu);
}
static inline auto dmemToVec(const u8* dm, u32 a0, u8* vb, u32 e, u32 n) -> void {
  if(n == 16) {   // registro entero (LQV alineado): un solo pshufb
    __m128i w = _mm_loadu_si128(reinterpret_cast<const __m128i*>(dm + a0));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(vb + e),
                     _mm_shuffle_epi8(w, _mm_load_si128(reinterpret_cast<const __m128i*>(kLaneSwap))));
    return;
  }
  while(n >= 8) { u64 w; std::memcpy(&w, dm + a0, 8); w = laneSwap64(w);
                  std::memcpy(vb + e, &w, 8); a0 += 8; e += 8; n -= 8; }
  if(n >= 4)    { u32 w; std::memcpy(&w, dm + a0, 4); w = laneSwap32(w);
                  std::memcpy(vb + e, &w, 4); a0 += 4; e += 4; n -= 4; }
  if(n >= 2)    { u16 w; std::memcpy(&w, dm + a0, 2); w = (u16)((w >> 8) | (w << 8));
                  std::memcpy(vb + e, &w, 2); }
}
static inline auto vecToDmem(u8* dm, u32 a0, const u8* vb, u32 e, u32 n) -> void {
  if(n == 16) {
    __m128i w = _mm_loadu_si128(reinterpret_cast<const __m128i*>(vb + e));
    _mm_storeu_si128(reinterpret_cast<__m128i*>(dm + a0),
                     _mm_shuffle_epi8(w, _mm_load_si128(reinterpret_cast<const __m128i*>(kLaneSwap))));
    return;
  }
  while(n >= 8) { u64 w; std::memcpy(&w, vb + e, 8); w = laneSwap64(w);
                  std::memcpy(dm + a0, &w, 8); a0 += 8; e += 8; n -= 8; }
  if(n >= 4)    { u32 w; std::memcpy(&w, vb + e, 4); w = laneSwap32(w);
                  std::memcpy(dm + a0, &w, 4); a0 += 4; e += 4; n -= 4; }
  if(n >= 2)    { u16 w; std::memcpy(&w, vb + e, 2); w = (u16)((w >> 8) | (w << 8));
                  std::memcpy(dm + a0, &w, 2); }
}
// Condicion del camino rapido: elemento y longitud pares, sin desbordar el registro
// ni envolver DMEM (los dos casos que el camino byte a byte trata de otra forma).
static inline auto vecFast(u32 a0, u32 e, u32 n) -> bool {
  return ((e | n) & 1) == 0 && e + n <= 16 && a0 + n <= 0x1000;
}

auto Rsp::bindMem() -> void { dmp = mem->dmem.data(); imp = mem->imem.data(); }
auto Rsp::rb(u32 a) const -> u8 { return dmp[a & 0xfff]; }
auto Rsp::wb(u32 a, u8 v) -> void { dmp[a & 0xfff] = v; }
// Accesos escalares a DMEM. El RSP no lanza excepciones de alineacion: una direccion
// impar lee bytes sueltos envolviendo dentro de DMEM, de ahi el camino byte a byte.
// Pero el codigo del microcodigo casi siempre esta alineado, y ahi una carga de 16/32
// bits mas bswap sustituye a 2-4 lecturas de byte con sus mascaras. Mismos bytes.
auto Rsp::rHalf(u32 a) const -> u16 {
  u32 a0 = a & 0xfff;
  if((a0 & 1) == 0) { u16 w; std::memcpy(&w, dmp + a0, 2); return bswap16(w); }
  return (u16)(rb(a) << 8 | rb(a + 1));
}
auto Rsp::rWord(u32 a) const -> u32 {
  u32 a0 = a & 0xfff;
  if((a0 & 3) == 0) { u32 w; std::memcpy(&w, dmp + a0, 4); return bswap32(w); }
  return (u32)rb(a) << 24 | rb(a + 1) << 16 | rb(a + 2) << 8 | rb(a + 3);
}
auto Rsp::wHalf(u32 a, u16 v) -> void {
  u32 a0 = a & 0xfff;
  if((a0 & 1) == 0) { u16 w = bswap16(v); std::memcpy(dmp + a0, &w, 2); return; }
  wb(a, v >> 8); wb(a + 1, v);
}
auto Rsp::wWord(u32 a, u32 v) -> void {
  u32 a0 = a & 0xfff;
  if((a0 & 3) == 0) { u32 w = bswap32(v); std::memcpy(dmp + a0, &w, 4); return; }
  wb(a, v >> 24); wb(a + 1, v >> 16); wb(a + 2, v >> 8); wb(a + 3, v);
}
auto Rsp::imword(u32 a) const -> u32 {
  a &= 0xfff;
  const u8* p = imp;
  // Camino normal: el PC del RSP siempre esta alineado a palabra (pc avanza de 4 en 4 y
  // take() enmascara el destino con 0xffc), asi que una carga de 32 bits + bswap sustituye
  // a cuatro cargas de byte con sus desplazamientos. El camino byte a byte se queda para
  // las lecturas no alineadas (solo llegan de utilidades de depuracion) porque ahi si
  // puede cruzar el final de IMEM y hay que envolver a 0.
  if((a & 3) == 0) { u32 w; std::memcpy(&w, p + a, 4); return bswap32(w); }
  return (u32)p[a] << 24 | p[(a + 1) & 0xfff] << 16 | p[(a + 2) & 0xfff] << 8 | p[(a + 3) & 0xfff];
}

// --- accumulator ------------------------------------------------------------
auto Rsp::accGet(int n) const -> u64 {
  return (u64)acch.uc(n) << 32 | (u64)accm.uc(n) << 16 | (u64)accl.uc(n);
}
auto Rsp::accSet(int n, u64 v) -> void {
  acch.u(n) = (u16)(v >> 32); accm.u(n) = (u16)(v >> 16); accl.u(n) = (u16)v;
}
auto Rsp::accSat(int n, bool slice, u16 neg, u16 pos) const -> u16 {
  if(acch.s(n) < 0) {
    if(acch.uc(n) != 0xffff) return neg;
    if(accm.s(n) >= 0)       return neg;
  } else {
    if(acch.uc(n) != 0x0000) return pos;
    if(accm.s(n) < 0)        return pos;
  }
  return !slice ? accl.uc(n) : accm.uc(n);
}

// --- COP0 register access (SP + DPC) ----------------------------------------
auto Rsp::mfc0(int rt, int rd) -> void {
  if((rd & 0xf) == 10) mem->rcp.dpcCurReads.fetch_add(1, std::memory_order_relaxed);  // DPC_CURRENT
  u32 data = (rd & 8) ? mem->read32(PHYS_DPC + ((rd & 7) << 2))
                      : mem->read32(PHYS_SP  + ((rd & 7) << 2));
  setR(rt, data);
}
auto Rsp::mtc0(int rd, u32 v) -> void {
  if(rd & 8) { mem->write32(PHYS_DPC + ((rd & 7) << 2), v); return; }
  mem->write32(PHYS_SP + ((rd & 7) << 2), v);
  // Writing SET_HALT to SP_STATUS from within the RSP halts the core immediately,
  // without a BREAK — so Status.broke is NOT set (unlike the BREAK instruction).
  if((rd & 7) == 4 && (mem->rcp.sp_status.load(std::memory_order_acquire) & 1u)) halt = true;
}

// --- scalar dispatch --------------------------------------------------------
auto Rsp::exec(u32 op) -> void {
  u32 maj = op >> 26;
#if KESTREL_VUSTAT
  if(g_vustat.on) { g_vustat.maj[maj]++; g_vustat.total++; }
#endif
  int rs = op >> 21 & 31, rt = op >> 16 & 31, rd = op >> 11 & 31;
  u32 imm = op & 0xffff; s32 simm = (s16)imm;
  switch(maj) {
  case 0x00:  // SPECIAL
    switch(op & 0x3f) {
    case 0x00: setR(rd, r[rt] << (op >> 6 & 31)); break;           // SLL
    case 0x02: setR(rd, r[rt] >> (op >> 6 & 31)); break;           // SRL
    case 0x03: setR(rd, (s32)r[rt] >> (op >> 6 & 31)); break;      // SRA
    case 0x04: setR(rd, r[rt] << (r[rs] & 31)); break;             // SLLV
    case 0x06: setR(rd, r[rt] >> (r[rs] & 31)); break;             // SRLV
    case 0x07: setR(rd, (s32)r[rt] >> (r[rs] & 31)); break;        // SRAV
    case 0x08: take(r[rs]); break;                                 // JR
    case 0x09: { u32 tgt = r[rs]; setR(rd, (curpc + 8) & 0xfff); take(tgt); } break;  // JALR (read rs before linking rd)
    case 0x0d:                                                     // BREAK
      // Halt now, but publish HALT|BROKE only once step() has written the final PC
      // back (see below) — the CPU treats HALT as "task over" and immediately writes
      // the next task's SP_PC, so a status that lands first lets our own stale PC
      // writeback clobber it and the next task starts on this very BREAK.
      halt = true; broke = true;
      break;
    case 0x20: case 0x21: setR(rd, r[rs] + r[rt]); break;          // ADD/ADDU
    case 0x22: case 0x23: setR(rd, r[rs] - r[rt]); break;          // SUB/SUBU
    case 0x24: setR(rd, r[rs] & r[rt]); break;                     // AND
    case 0x25: setR(rd, r[rs] | r[rt]); break;                     // OR
    case 0x26: setR(rd, r[rs] ^ r[rt]); break;                     // XOR
    case 0x27: setR(rd, ~(r[rs] | r[rt])); break;                  // NOR
    case 0x2a: setR(rd, (s32)r[rs] < (s32)r[rt]); break;           // SLT
    case 0x2b: setR(rd, r[rs] < r[rt]); break;                     // SLTU
    default:   setR(rd, r[rs] >> (r[rs] & 31)); break;             // invalid -> SRLV(rd,rs,rs)
    }
    break;
  case 0x01:  // REGIMM
    switch(rt) {
    case 0x00: if((s32)r[rs] <  0) take(curpc + 4 + (simm << 2)); break;  // BLTZ
    case 0x01: if((s32)r[rs] >= 0) take(curpc + 4 + (simm << 2)); break;  // BGEZ
    case 0x10: if((s32)r[rs] <  0) take(curpc + 4 + (simm << 2)); setR(31, (curpc + 8) & 0xfff); break;  // BLTZAL
    case 0x11: if((s32)r[rs] >= 0) take(curpc + 4 + (simm << 2)); setR(31, (curpc + 8) & 0xfff); break;  // BGEZAL
    }
    break;
  case 0x02: take((op & 0x3ffffff) << 2); break;                             // J
  case 0x03: setR(31, (curpc + 8) & 0xfff); take((op & 0x3ffffff) << 2); break;  // JAL
  case 0x04: if(r[rs] == r[rt]) take(curpc + 4 + (simm << 2)); break;        // BEQ
  case 0x05: if(r[rs] != r[rt]) take(curpc + 4 + (simm << 2)); break;        // BNE
  case 0x06: if((s32)r[rs] <= 0) take(curpc + 4 + (simm << 2)); break;       // BLEZ
  case 0x07: if((s32)r[rs] >  0) take(curpc + 4 + (simm << 2)); break;       // BGTZ
  case 0x08: case 0x09: setR(rt, r[rs] + simm); break;                       // ADDI/ADDIU
  case 0x0a: setR(rt, (s32)r[rs] < simm); break;                             // SLTI
  case 0x0b: setR(rt, r[rs] < (u32)simm); break;                             // SLTIU
  case 0x0c: setR(rt, r[rs] & imm); break;                                   // ANDI
  case 0x0d: setR(rt, r[rs] | imm); break;                                   // ORI
  case 0x0e: setR(rt, r[rs] ^ imm); break;                                   // XORI
  case 0x0f: setR(rt, imm << 16); break;                                     // LUI
  case 0x10:  // COP0
    if((op >> 21 & 0x1f) == 0x00) mfc0(rt, rd);
    else if((op >> 21 & 0x1f) == 0x04) mtc0(rd, r[rt]);
    break;
  case 0x12: execCop2(op); break;                                            // COP2
  case 0x20: setR(rt, (s8)rb(r[rs] + simm)); break;                          // LB
  case 0x21: setR(rt, (s16)rHalf(r[rs] + simm)); break;                      // LH
  case 0x23: setR(rt, rWord(r[rs] + simm)); break;                           // LW
  case 0x24: setR(rt, rb(r[rs] + simm)); break;                              // LBU
  case 0x25: setR(rt, rHalf(r[rs] + simm)); break;                           // LHU
  case 0x27: setR(rt, rWord(r[rs] + simm)); break;                           // LWU
  case 0x28: wb(r[rs] + simm, (u8)r[rt]); break;                             // SB
  case 0x29: wHalf(r[rs] + simm, (u16)r[rt]); break;                         // SH
  case 0x2b: wWord(r[rs] + simm, r[rt]); break;                              // SW
  case 0x32: execLoad(op); break;                                           // LWC2
  case 0x3a: execStore(op); break;                                          // SWC2
  }
}

// --- vector loads (LWC2) ----------------------------------------------------
// SUB es parametro de plantilla, no argumento. Mismo motivo que en vuOpT: con los dieciseis
// casos vivos dentro de una sola funcion, el asignador dimensiona el prologo con el peor
// caso -- LFV/LTV, que gastan un R128 en pila y varios xmm -- y ESO lo paga tambien un LQV,
// que es una carga de 128 bits y un pshufb. Instanciado por sub, cada entrada tiene el
// prologo que le toca y el llamante que conoce el opcode (el dynarec) entra sin salto
// indirecto. Las reglas son las mismas de siempre; no hay una segunda copia.
template<u32 SUB>
auto Rsp::execLoadT(u32 op) -> void {
  constexpr u32 sub = SUB;
  int base = op >> 21 & 31, vt = op >> 16 & 31;
  u32 e = op >> 7 & 0xf;
#if KESTREL_VUSTAT
  if(g_vustat.on) g_vustat.lwc2[sub]++;
#endif
  s32 imm = (op & 0x7f); if(imm & 0x40) imm -= 0x80;
  u32 rsv = r[base];
  R128& V = vpr[vt];
  switch(sub) {
  case 0x00: V.sb(e, rb(rsv + imm)); break;                                  // LBV
  case 0x01: case 0x02: case 0x03: {   // LSV / LLV / LDV
    u32 n = 2u << (sub - 1);                     // 2, 4 u 8 bytes
    u32 a = rsv + imm * (s32)n, a0 = a & 0xfff;
    u32 lim = e + n > 16u ? 16u - e : n;         // el registro no envuelve en estas cargas
    if(vecfast && vecFast(a0, e, lim)) { dmemToVec(dmp, a0, (u8*)V.el, e, lim); break; }
    for(u32 o = e; o < e + n && o < 16; o++) V.sb(o & 15, rb(a++));
  } break;
  case 0x04: {  // LQV
    u32 a = rsv + imm * 16;
    // Camino rapido: cuarteto alineado y sin desplazamiento de elemento — el caso que usan
    // los microcodigos de graficos para traerse un vertice entero. Son los mismos 16 bytes
    // en el mismo orden, solo que en una carga de 128 bits con un pshufb que intercambia
    // los bytes de cada banda de 16 bits (DMEM es big-endian dentro de la banda, el[] no),
    // en vez de dieciseis lecturas de byte con su enmascarado. Alineado a 16 no puede
    // cruzar el final de DMEM, asi que no hay envoltura que respetar.
    u32 end = (a | 15); u32 lim = end - a; if(lim > 15u - e) lim = 15u - e;
    if(vecfast && vecFast(a & 0xfff, e, lim + 1)) {
      dmemToVec(dmp, a & 0xfff, (u8*)V.el, e, lim + 1); break;
    }
    for(u32 o = 0; o <= lim; o++) V.sb((e + o) & 15, rb(a + o));
  } break;
  case 0x05: {  // LRV
    u32 a = rsv + imm * 16; u32 index = e; u32 start = 16 - ((a & 15) - index); a &= ~15u;
    for(u32 o = start; o < 16; o++) V.sb(o & 15, rb(a++));
  } break;
  case 0x06: {  // LPV
    u32 a = rsv + imm * 8; s32 index = (s32)(a & 7) - (s32)e; a &= ~7u;
    for(u32 o = 0; o < 8; o++) V.u(o) = (u16)(rb(a + ((index + (s32)o) & 15)) << 8);
  } break;
  case 0x07: {  // LUV
    u32 a = rsv + imm * 8; s32 index = (s32)(a & 7) - (s32)e; a &= ~7u;
    for(u32 o = 0; o < 8; o++) V.u(o) = (u16)(rb(a + ((index + (s32)o) & 15)) << 7);
  } break;
  case 0x08: {  // LHV
    u32 a = rsv + imm * 16; s32 index = (s32)(a & 7) - (s32)e; a &= ~7u;
    for(u32 o = 0; o < 8; o++) V.u(o) = (u16)(rb(a + ((index + (s32)o * 2) & 15)) << 7);
  } break;
  case 0x09: {  // LFV
    u32 a = rsv + imm * 16; s32 index = (s32)(a & 7) - (s32)e; a &= ~7u;
    R128 tmp;
    for(u32 o = 0; o < 4; o++) {
      tmp.u(o + 0) = (u16)(rb(a + ((index + (s32)o * 4 + 0) & 15)) << 7);
      tmp.u(o + 4) = (u16)(rb(a + ((index + (s32)o * 4 + 8) & 15)) << 7);
    }
    for(u32 o = e; o < e + 8u && o < 16; o++) V.sb(o, tmp.gb(o));
  } break;
  case 0x0b: {  // LTV
    u32 a = rsv + imm * 16; u32 begin = a & ~7u; a = begin + ((e + (a & 8)) & 15);
    u32 vtbase = vt & ~7u; u32 vtoff = e >> 1;
    for(u32 i = 0; i < 8; i++) {
      vpr[vtbase + vtoff].sb(i * 2 + 0, rb(a++)); if(a == begin + 16) a = begin;
      vpr[vtbase + vtoff].sb(i * 2 + 1, rb(a++)); if(a == begin + 16) a = begin;
      vtoff = (vtoff + 1) & 7;
    }
  } break;
  }
}

using Lwc2Fn = void (*)(Rsp*, u32);
namespace {
template<u32 S> auto lwc2Thunk(Rsp* r, u32 op) -> void { r->execLoadT<S>(op); }
template<u32... I> constexpr auto makeLwc2Tab(std::integer_sequence<u32, I...>) {
  return std::array<Lwc2Fn, 32>{ &lwc2Thunk<I>... };
}
constexpr std::array<Lwc2Fn, 32> kLwc2Tab = makeLwc2Tab(std::make_integer_sequence<u32, 32>{});
}  // namespace

auto rspLwc2Entry(u32 op) -> void* { return (void*)kLwc2Tab[op >> 11 & 0x1f]; }
auto Rsp::execLoad(u32 op) -> void { kLwc2Tab[op >> 11 & 0x1f](this, op); }

// --- vector stores (SWC2) ---------------------------------------------------
// Instanciado por sub por la misma razon que las cargas: SFV/STV son los que marcan el
// prologo y no tiene por que pagarlo un SQV.
template<u32 SUB>
auto Rsp::execStoreT(u32 op) -> void {
  constexpr u32 sub = SUB;
  int base = op >> 21 & 31, vt = op >> 16 & 31;
  u32 e = op >> 7 & 0xf;
#if KESTREL_VUSTAT
  if(g_vustat.on) g_vustat.swc2[sub]++;
#endif
  s32 imm = (op & 0x7f); if(imm & 0x40) imm -= 0x80;
  u32 rsv = r[base];
  R128& V = vpr[vt];
  switch(sub) {
  case 0x00: wb(rsv + imm, V.gb(e)); break;                                  // SBV
  case 0x01: case 0x02: case 0x03: {   // SSV / SLV / SDV
    u32 n = 2u << (sub - 1);                     // 2, 4 u 8 bytes
    u32 a = rsv + imm * (s32)n, a0 = a & 0xfff;
    if(vecfast && vecFast(a0, e, n)) { vecToDmem(dmp, a0, (const u8*)V.el, e, n); break; }
    for(u32 o = e; o < e + n; o++) wb(a++, V.gb(o & 15));   // aqui el elemento SI envuelve
  } break;
  case 0x04: {  // SQV
    u32 a = rsv + imm * 16;
    u32 n = 16u - (a & 15);
    if(vecfast && vecFast(a & 0xfff, e, n)) { vecToDmem(dmp, a & 0xfff, (const u8*)V.el, e, n); break; }
    u32 end = e + n; for(u32 o = e; o < end; o++) wb(a++, V.gb(o & 15));
  } break;
  case 0x05: {  // SRV
    u32 a = rsv + imm * 16; u32 end = e + (a & 15); u32 bse = 16 - (a & 15); a &= ~15u;
    for(u32 o = e; o < end; o++) wb(a++, V.gb((o + bse) & 15));
  } break;
  case 0x06: {  // SPV
    u32 a = rsv + imm * 8; for(u32 o = e; o < e + 8u; o++) {
      if((o & 15) < 8) wb(a++, V.gb((o & 7) << 1)); else wb(a++, (u8)(V.uc(o & 7) >> 7));
    }
  } break;
  case 0x07: {  // SUV
    u32 a = rsv + imm * 8; for(u32 o = e; o < e + 8u; o++) {
      if((o & 15) < 8) wb(a++, (u8)(V.uc(o & 7) >> 7)); else wb(a++, V.gb((o & 7) << 1));
    }
  } break;
  case 0x08: {  // SHV
    u32 a = rsv + imm * 16; u32 index = a & 7; a &= ~7u;
    for(u32 o = 0; o < 8; o++) {
      u32 byte = e + o * 2; u8 val = (u8)(V.gb((byte + 0) & 15) << 1 | V.gb((byte + 1) & 15) >> 7);
      wb(a + ((index + o * 2) & 15), val);
    }
  } break;
  case 0x09: {  // SFV
    u32 a = rsv + imm * 16; u32 bse = a & 7; a &= ~7u;
    auto put = [&](u32 off, u16 lane){ wb(a + ((bse + off) & 15), (u8)(lane >> 7)); };
    switch(e) {
    case 0: case 15: put(0, V.uc(0)); put(4, V.uc(1)); put(8, V.uc(2)); put(12, V.uc(3)); break;
    case 1:  put(0, V.uc(6)); put(4, V.uc(7)); put(8, V.uc(4)); put(12, V.uc(5)); break;
    case 4:  put(0, V.uc(1)); put(4, V.uc(2)); put(8, V.uc(3)); put(12, V.uc(0)); break;
    case 5:  put(0, V.uc(7)); put(4, V.uc(4)); put(8, V.uc(5)); put(12, V.uc(6)); break;
    case 8:  put(0, V.uc(4)); put(4, V.uc(5)); put(8, V.uc(6)); put(12, V.uc(7)); break;
    case 11: put(0, V.uc(3)); put(4, V.uc(0)); put(8, V.uc(1)); put(12, V.uc(2)); break;
    case 12: put(0, V.uc(5)); put(4, V.uc(6)); put(8, V.uc(7)); put(12, V.uc(4)); break;
    default: put(0, 0); put(4, 0); put(8, 0); put(12, 0); break;
    }
  } break;
  case 0x0a: {  // SWV
    u32 a = rsv + imm * 16; u32 bse = a & 7; a &= ~7u;
    for(u32 o = e; o < e + 16u; o++) wb(a + ((bse + (o - e)) & 15), V.gb(o & 15));
  } break;
  case 0x0b: {  // STV
    u32 a = rsv + imm * 16; u32 start = vt & ~7u; u32 element = 16 - (e & ~1u);
    s32 bse = (s32)(a & 7) - (s32)(e & ~1u); a &= ~7u;
    for(u32 o = start; o < start + 8; o++) {
      wb(a + ((bse++) & 15), vpr[o].gb(element++ & 15));
      wb(a + ((bse++) & 15), vpr[o].gb(element++ & 15));
    }
  } break;
  }
}

using Swc2Fn = void (*)(Rsp*, u32);
namespace {
template<u32 S> auto swc2Thunk(Rsp* r, u32 op) -> void { r->execStoreT<S>(op); }
template<u32... I> constexpr auto makeSwc2Tab(std::integer_sequence<u32, I...>) {
  return std::array<Swc2Fn, 32>{ &swc2Thunk<I>... };
}
constexpr std::array<Swc2Fn, 32> kSwc2Tab = makeSwc2Tab(std::make_integer_sequence<u32, 32>{});
}  // namespace

auto rspSwc2Entry(u32 op) -> void* { return (void*)kSwc2Tab[op >> 11 & 0x1f]; }
auto Rsp::execStore(u32 op) -> void { kSwc2Tab[op >> 11 & 0x1f](this, op); }

// --- 48-bit accumulator as three 16-bit limbs, 8 lanes wide ------------------
struct V48 { __m128i h, m, l; };   // h=acch, m=accm, l=accl (bits 47:32 / 31:16 / 15:0)

// Acarreo de salida (0/1 por banda de 16 bits) de la suma s = a + b. Formula clasica de
// sumador: el acarreo sale del bit alto de (a&b) | ((a|b) & ~s), y un desplazamiento de 15
// lo baja a 0/1. Sin comparaciones y sin ensanchar a 32 bits.
static inline auto vcarry16(__m128i a, __m128i b, __m128i s) -> __m128i {
  __m128i c = _mm_or_si128(_mm_and_si128(a, b), _mm_andnot_si128(s, _mm_or_si128(a, b)));
  return _mm_srli_epi16(c, 15);
}

// 48-bit add with carry across the three 16-bit limbs (unsigned limb add = correct
// two's-complement 48-bit add; the top limb carries the sign). Final carry dropped.
//
// Se hace EN 16 BITS, propagando el acarreo con vcarry16. La version anterior ensanchaba
// cada rebanada a 32 bits (seis cvtepu16 + seis sumas + cuatro desplazamientos) y volvia a
// empaquetar con mascara (tres packus): treinta y tantas instrucciones para una suma de 48
// bits. Esta hace nueve. Es la MISMA suma -- banco de pruebas de 2 M de casos aleatorios con
// sesgo a los limites (0xffff en cada rebanada): resultado identico bit a bit, y el fuzz
// diferencial (--rspfuzz, oraculo escalar) lo vuelve a cubrir sobre las instrucciones reales.
// Cadena dependiente medida en el host: 3.80 -> 1.57 ns por suma.
//
// El acarreo de la rebanada media sale de dos sitios (la suma a.m+b.m y el +1 del acarreo
// de abajo) y nunca de los dos a la vez -- si a.m+b.m desborda, la suma es <= 0xfffe y el +1
// no puede desbordar -- asi que un OR basta.
static inline auto vadd48(V48 a, V48 b) -> V48 {
  __m128i l  = _mm_add_epi16(a.l, b.l);
  __m128i c  = vcarry16(a.l, b.l, l);
  __m128i t  = _mm_add_epi16(a.m, b.m);
  __m128i c1 = vcarry16(a.m, b.m, t);
  __m128i m  = _mm_add_epi16(t, c);
  __m128i c2 = vcarry16(t, c, m);
  __m128i h  = _mm_add_epi16(_mm_add_epi16(a.h, b.h), _mm_or_si128(c1, c2));
  return { h, m, l };
}

// 16×16 products, sign-extended into a 48-bit limb triple.
static inline auto vprodSS(__m128i s, __m128i t) -> V48 {           // signed × signed
  __m128i lo = _mm_mullo_epi16(s, t), hi = _mm_mulhi_epi16(s, t);
  return { _mm_srai_epi16(hi, 15), hi, lo };
}
static inline auto vprodSU(__m128i s, __m128i t) -> V48 {           // signed S × unsigned T
  __m128i lo = _mm_mullo_epi16(s, t);
  __m128i hi = _mm_sub_epi16(_mm_mulhi_epu16(s, t),
                             _mm_and_si128(_mm_cmpgt_epi16(_mm_setzero_si128(), s), t));  // -= T where S<0
  return { _mm_srai_epi16(hi, 15), hi, lo };
}
static inline auto vprodUS(__m128i s, __m128i t) -> V48 {           // unsigned S × signed T
  __m128i lo = _mm_mullo_epi16(s, t);
  __m128i hi = _mm_sub_epi16(_mm_mulhi_epu16(s, t),
                             _mm_and_si128(_mm_cmpgt_epi16(_mm_setzero_si128(), t), s));  // -= S where T<0
  return { _mm_srai_epi16(hi, 15), hi, lo };
}

// accSat(slice=1): signed-saturate the 32-bit (acch:accm) to s16 (returns accm in range).
static inline auto vsatSigned(const V48& a) -> __m128i {
  return _mm_packs_epi32(_mm_unpacklo_epi16(a.m, a.h), _mm_unpackhi_epi16(a.m, a.h));
}
// accSat(slice=0, neg=0, pos=0xffff): 0 if (acch:accm)<-0x8000, 0xffff if >0x7fff, else accl.
static inline auto vsatUnsignedN(const V48& a) -> __m128i {
  __m128i lo = _mm_unpacklo_epi16(a.m, a.h), hi = _mm_unpackhi_epi16(a.m, a.h);   // V32=(acch:accm)
  const __m128i posMax = _mm_set1_epi32(0x7fff), negMin = _mm_set1_epi32(-0x8000);
  __m128i ov = _mm_packs_epi32(_mm_cmpgt_epi32(lo, posMax), _mm_cmpgt_epi32(hi, posMax));
  __m128i un = _mm_packs_epi32(_mm_cmpgt_epi32(negMin, lo), _mm_cmpgt_epi32(negMin, hi));
  __m128i r = _mm_blendv_epi8(a.l, _mm_set1_epi16(-1), ov);
  return _mm_blendv_epi8(r, _mm_setzero_si128(), un);
}
// VMULU D: acch<0 ? 0 : ((acch^accm)<0 ? 0xffff : accm).
static inline auto vsatMulU(const V48& a) -> __m128i {
  __m128i zero = _mm_setzero_si128();
  __m128i neg  = _mm_cmpgt_epi16(zero, a.h);                          // acch < 0
  __m128i xorn = _mm_cmpgt_epi16(zero, _mm_xor_si128(a.h, a.m));      // (acch^accm) < 0
  __m128i r = _mm_blendv_epi8(a.m, _mm_set1_epi16(-1), xorn);
  return _mm_blendv_epi8(r, zero, neg);
}
// VMACU D: acch<0 ? 0 : ((acch!=0 || accm<0) ? 0xffff : accm).
static inline auto vsatMacU(const V48& a) -> __m128i {
  __m128i zero = _mm_setzero_si128();
  __m128i neg  = _mm_cmpgt_epi16(zero, a.h);
  __m128i ov   = _mm_or_si128(vnot(_mm_cmpeq_epi16(a.h, zero)), _mm_cmpgt_epi16(zero, a.m));  // acch!=0 || accm<0
  __m128i r = _mm_blendv_epi8(a.m, _mm_set1_epi16(-1), ov);
  return _mm_blendv_epi8(r, zero, neg);
}

// --- COP2 SSE fast path (8 lanes at once) ------------------------------------
// Bit-exact with the scalar switch below. `--rspfuzz` proves scalar==sse over random
// states for every fn handled here. Returns false for ops it doesn't implement.
// FN es parametro de plantilla, no argumento: el switch de abajo se pliega a un solo caso en
// cada instanciacion. Es la misma tabla de semanticas de siempre -- no hay una segunda copia
// de las reglas del VU -- pero el llamante que conoce el opcode (el dynarec, y el
// despachador de execCop2) entra directo en el caso, sin el salto indirecto que con la
// mezcla real de microcodigo falla la prediccion casi siempre.
template<u32 FN>
auto Rsp::vuOpT(__m128i t, R128& S, R128& D) -> bool {
  constexpr u32 fn = FN;
  const __m128i s = vload(S);

  switch(fn) {
  // --- MAC / multiply-accumulate chain (48-bit acc via limb helpers) ---------
  case 0x00: case 0x01: {  // VMULF / VMULU: acc = S*T*2 + 0x8000 (set)
    V48 p = vprodSS(s, t);
    V48 a = vadd48(vadd48(p, p), V48{ _mm_setzero_si128(), _mm_setzero_si128(), _mm_set1_epi16((short)0x8000) });
    vstore(acch, a.h); vstore(accm, a.m); vstore(accl, a.l);
    vstore(D, fn == 0x00 ? vsatSigned(a) : vsatMulU(a));
    return true;
  }
  case 0x08: case 0x09: {  // VMACF / VMACU: acc += S*T*2
    V48 cur{ vload(acch), vload(accm), vload(accl) };
    V48 p = vprodSS(s, t);
    V48 a = vadd48(cur, vadd48(p, p));
    vstore(acch, a.h); vstore(accm, a.m); vstore(accl, a.l);
    vstore(D, fn == 0x08 ? vsatSigned(a) : vsatMacU(a));
    return true;
  }
  case 0x04: {  // VMUDL: acc = zeroext(mulhi_epu16); D = accl
    __m128i mh = _mm_mulhi_epu16(s, t);
    vstoreZero(acch); vstoreZero(accm); vstore(accl, mh);
    vstore(D, mh);
    return true;
  }
  case 0x05: {  // VMUDM: acc = signext32(S.s * T.u); D = accm
    V48 p = vprodSU(s, t);
    vstore(acch, p.h); vstore(accm, p.m); vstore(accl, p.l);
    vstore(D, p.m);
    return true;
  }
  case 0x06: {  // VMUDN: acc = signext32(S.u * T.s); D = accl
    V48 p = vprodUS(s, t);
    vstore(acch, p.h); vstore(accm, p.m); vstore(accl, p.l);
    vstore(D, p.l);
    return true;
  }
  case 0x07: {  // VMUDH: acc = (S.s * T.s) << 16; D = satSigned
    __m128i lo = _mm_mullo_epi16(s, t), hi = _mm_mulhi_epi16(s, t);
    V48 a{ hi, lo, _mm_setzero_si128() };
    vstore(acch, a.h); vstore(accm, a.m); vstore(accl, a.l);
    vstore(D, vsatSigned(a));
    return true;
  }
  case 0x0c: {  // VMADL: acc += zeroext(mulhi_epu16) (unsigned partial, no sign-ext); D = satUnsignedN
    __m128i mh = _mm_mulhi_epu16(s, t);
    V48 add{ _mm_setzero_si128(), _mm_setzero_si128(), mh };
    V48 a = vadd48(V48{ vload(acch), vload(accm), vload(accl) }, add);
    vstore(acch, a.h); vstore(accm, a.m); vstore(accl, a.l);
    vstore(D, vsatUnsignedN(a));
    return true;
  }
  case 0x0d: {  // VMADM: acc += signext32(S.s * T.u); D = satSigned
    V48 a = vadd48(V48{ vload(acch), vload(accm), vload(accl) }, vprodSU(s, t));
    vstore(acch, a.h); vstore(accm, a.m); vstore(accl, a.l);
    vstore(D, vsatSigned(a));
    return true;
  }
  case 0x0e: {  // VMADN: acc += signext32(S.u * T.s); D = satUnsignedN
    V48 a = vadd48(V48{ vload(acch), vload(accm), vload(accl) }, vprodUS(s, t));
    vstore(acch, a.h); vstore(accm, a.m); vstore(accl, a.l);
    vstore(D, vsatUnsignedN(a));
    return true;
  }
  case 0x0f: {  // VMADH: (acch:accm) += S.s*T.s (32-bit), accl untouched; D = satSigned
    // Suma de 32 bits hecha por rebanadas de 16 con acarreo, igual que vadd48: ahorra los
    // cuatro unpack, los dos packus y el enmascarado de la version ensanchada.
    __m128i lo = _mm_mullo_epi16(s, t), hi = _mm_mulhi_epi16(s, t);
    __m128i am = vload(accm), ah = vload(acch);
    __m128i nm = _mm_add_epi16(am, lo);
    __m128i nh = _mm_add_epi16(_mm_add_epi16(ah, hi), vcarry16(am, lo, nm));
    vstore(accm, nm); vstore(acch, nh);
    vstore(D, vsatSigned(V48{ nh, nm, vload(accl) }));
    return true;
  }


  // --- bitwise logicals: accl = op; D = accl ---------------------------------
  case 0x28: { __m128i r = _mm_and_si128(s, t);        vstore(accl, r); vstore(D, r); return true; }  // VAND
  case 0x29: { __m128i r = vnot(_mm_and_si128(s, t));  vstore(accl, r); vstore(D, r); return true; }  // VNAND
  case 0x2a: { __m128i r = _mm_or_si128(s, t);         vstore(accl, r); vstore(D, r); return true; }  // VOR
  case 0x2b: { __m128i r = vnot(_mm_or_si128(s, t));   vstore(accl, r); vstore(D, r); return true; }  // VNOR
  case 0x2c: { __m128i r = _mm_xor_si128(s, t);        vstore(accl, r); vstore(D, r); return true; }  // VXOR
  case 0x2d: { __m128i r = vnot(_mm_xor_si128(s, t));  vstore(accl, r); vstore(D, r); return true; }  // VNXOR

  // --- VADD / VSUB: 16-bit wrap into accl, signed-saturate into D, clear VCO --
  case 0x10: case 0x11: {
    const bool sub = (fn == 0x11);
    __m128i c = vload(vcol);                                  // carry/borrow-in (0/1 per lane)
    __m128i al = sub ? _mm_sub_epi16(_mm_sub_epi16(s, t), c)
                     : _mm_add_epi16(_mm_add_epi16(s, t), c);
    vstore(accl, al);
    // exact 32-bit lane math → signed saturate on repack = sclamp16(rr)
    __m128i slo = _mm_cvtepi16_epi32(s), shi = _mm_cvtepi16_epi32(_mm_srli_si128(s, 8));
    __m128i tlo = _mm_cvtepi16_epi32(t), thi = _mm_cvtepi16_epi32(_mm_srli_si128(t, 8));
    __m128i clo = _mm_cvtepi16_epi32(c), chi = _mm_cvtepi16_epi32(_mm_srli_si128(c, 8));
    __m128i lo, hi;
    if(sub) { lo = _mm_sub_epi32(_mm_sub_epi32(slo, tlo), clo); hi = _mm_sub_epi32(_mm_sub_epi32(shi, thi), chi); }
    else    { lo = _mm_add_epi32(_mm_add_epi32(slo, tlo), clo); hi = _mm_add_epi32(_mm_add_epi32(shi, thi), chi); }
    vstore(D, _mm_packs_epi32(lo, hi));
    vstoreZero(vcol); vstoreZero(vcoh);
    return true;
  }

  // --- VADDC: unsigned add, carry-out → VCO.low, clear VCO.high ---------------
  case 0x14: {
    __m128i al = _mm_add_epi16(s, t); vstore(accl, al); vstore(D, al);
    __m128i slo = _mm_cvtepu16_epi32(s), shi = _mm_cvtepu16_epi32(_mm_srli_si128(s, 8));
    __m128i tlo = _mm_cvtepu16_epi32(t), thi = _mm_cvtepu16_epi32(_mm_srli_si128(t, 8));
    __m128i clo = _mm_srli_epi32(_mm_add_epi32(slo, tlo), 16);
    __m128i chi = _mm_srli_epi32(_mm_add_epi32(shi, thi), 16);
    vstore(vcol, _mm_packs_epi32(clo, chi));                  // 0/1 per lane
    vstoreZero(vcoh);
    return true;
  }
  // --- VSUBC: unsigned sub, borrow → VCO.low, nonzero → VCO.high --------------
  case 0x15: {
    __m128i al = _mm_sub_epi16(s, t); vstore(accl, al); vstore(D, al);
    __m128i slo = _mm_cvtepu16_epi32(s), shi = _mm_cvtepu16_epi32(_mm_srli_si128(s, 8));
    __m128i tlo = _mm_cvtepu16_epi32(t), thi = _mm_cvtepu16_epi32(_mm_srli_si128(t, 8));
    __m128i dlo = _mm_sub_epi32(slo, tlo), dhi = _mm_sub_epi32(shi, thi);
    __m128i one = _mm_set1_epi32(1), zero = _mm_setzero_si128();
    __m128i blo = _mm_and_si128(_mm_srli_epi32(dlo, 16), one);
    __m128i bhi = _mm_and_si128(_mm_srli_epi32(dhi, 16), one);
    __m128i nlo = _mm_and_si128(vnot(_mm_cmpeq_epi32(dlo, zero)), one);
    __m128i nhi = _mm_and_si128(vnot(_mm_cmpeq_epi32(dhi, zero)), one);
    vstore(vcol, _mm_packs_epi32(blo, bhi));
    vstore(vcoh, _mm_packs_epi32(nlo, nhi));
    return true;
  }

  // --- VLT / VEQ / VNE / VGE: compare → VCC.low, select S/vte, clear VCC.hi+VCO
  case 0x20: case 0x21: case 0x22: case 0x23: {
    __m128i eq  = _mm_cmpeq_epi16(s, t);
    __m128i col = vmaskFromFlag(vcol), coh = vmaskFromFlag(vcoh);
    __m128i cm;
    if(fn == 0x20)      cm = _mm_or_si128(_mm_cmpgt_epi16(t, s), _mm_and_si128(eq, _mm_and_si128(col, coh)));  // VLT
    else if(fn == 0x21) cm = _mm_andnot_si128(coh, eq);                                                        // VEQ
    else if(fn == 0x22) cm = _mm_or_si128(vnot(eq), coh);                                                      // VNE
    else                cm = _mm_or_si128(_mm_cmpgt_epi16(s, t), _mm_and_si128(eq, vnot(_mm_and_si128(col, coh))));  // VGE
    __m128i al = _mm_blendv_epi8(t, s, cm);
    vstore(accl, al); vstore(D, al);
    vstore(vccl, vflagFromMask(cm));
    vstoreZero(vcch); vstoreZero(vcol); vstoreZero(vcoh);
    return true;
  }
  // --- VMRG: select S/vte by VCC.low, clear VCO ------------------------------
  case 0x27: {
    __m128i al = _mm_blendv_epi8(t, s, vmaskFromFlag(vccl));
    vstore(accl, al); vstore(D, al);
    vstoreZero(vcoh); vstoreZero(vcol);
    return true;
  }
  // --- VABS: sign of S applied to T. No toca banderas -------------------------
  // El unico caso que no es una negacion normal es T=-32768: el acumulador se queda con
  // el patron que sale de negar en 16 bits (0x8000) y el destino con el valor saturado
  // (0x7fff). _mm_sub_epi16 da lo primero y _mm_subs_epi16 lo segundo, asi que las dos
  // salidas son la misma resta hecha con dos saturaciones distintas.
  case 0x13: {
    __m128i zero = _mm_setzero_si128();
    __m128i sNeg  = _mm_cmpgt_epi16(zero, s);       // S<0
    __m128i sZero = _mm_cmpeq_epi16(s, zero);       // S==0
    __m128i al = _mm_blendv_epi8(t, _mm_sub_epi16 (zero, t), sNeg);
    __m128i dv = _mm_blendv_epi8(t, _mm_subs_epi16(zero, t), sNeg);
    vstore(accl, _mm_andnot_si128(sZero, al));      // S==0 -> 0 en las dos
    vstore(D,    _mm_andnot_si128(sZero, dv));
    return true;
  }


  // --- VCH: clip compare, sets all of VCC/VCO/VCE from S,T (element-wise) -----
  case 0x25: {
    __m128i zero = _mm_setzero_si128();
    __m128i diff = _mm_cmpgt_epi16(zero, _mm_xor_si128(s, t));   // (S^T)<0 → opposite signs
    __m128i tNeg = _mm_cmpgt_epi16(zero, t);                     // T<0
    __m128i resA = _mm_add_epi16(s, t);                         // opposite-sign branch result
    __m128i resB = _mm_sub_epi16(s, t);                         // same-sign branch result
    __m128i result = _mm_blendv_epi8(resB, resA, diff);
    __m128i negT = _mm_sub_epi16(zero, t);
    __m128i leA = vnot(_mm_cmpgt_epi16(result, zero));           // result<=0
    __m128i geB = vnot(_mm_cmpgt_epi16(zero, result));           // result>=0
    // accl: A → (result<=0 ? -T : S); B → (result>=0 ? T : S)
    __m128i acclA = _mm_blendv_epi8(s, negT, leA);
    __m128i acclB = _mm_blendv_epi8(s, t, geB);
    __m128i al = _mm_blendv_epi8(acclB, acclA, diff);
    vstore(accl, al); vstore(D, al);
    // flags
    vstore(vccl, vflagFromMask(_mm_blendv_epi8(tNeg, leA, diff)));   // A:result<=0  B:T<0
    vstore(vcch, vflagFromMask(_mm_blendv_epi8(geB, tNeg, diff)));   // A:T<0        B:result>=0
    vstore(vcol, vflagFromMask(diff));                              // A:1 B:0
    __m128i vohM = _mm_and_si128(vnot(_mm_cmpeq_epi16(result, zero)),
                                 vnot(_mm_cmpeq_epi16(s, vnot(t)))); // result!=0 && S!=~T (both branches)
    vstore(vcoh, vflagFromMask(vohM));
    vstore(vce, vflagFromMask(_mm_and_si128(diff, _mm_cmpeq_epi16(result, vones()))));  // A:result==-1 B:0
    return true;
  }
  // --- VCR: clip compare (no VCE), sets VCC, clears VCO/VCE -------------------
  case 0x26: {
    __m128i zero = _mm_setzero_si128();
    __m128i diff = _mm_cmpgt_epi16(zero, _mm_xor_si128(s, t));   // opposite signs
    __m128i tNeg = _mm_cmpgt_epi16(zero, t);
    // opposite-sign: cclA = (S+T)<0 (true 32-bit sum); accl = cclA ? ~T : S
    __m128i slo = _mm_cvtepi16_epi32(s), shi = _mm_cvtepi16_epi32(_mm_srli_si128(s, 8));
    __m128i tlo = _mm_cvtepi16_epi32(t), thi = _mm_cvtepi16_epi32(_mm_srli_si128(t, 8));
    __m128i cclA = _mm_packs_epi32(_mm_cmpgt_epi32(zero, _mm_add_epi32(slo, tlo)),
                                   _mm_cmpgt_epi32(zero, _mm_add_epi32(shi, thi)));
    // same-sign: ccB = S>=T (signed); accl = ccB ? T : S
    __m128i ccB = vnot(_mm_cmpgt_epi16(t, s));
    __m128i acclA = _mm_blendv_epi8(s, vnot(t), cclA);
    __m128i acclB = _mm_blendv_epi8(s, t, ccB);
    __m128i al = _mm_blendv_epi8(acclB, acclA, diff);
    vstore(accl, al); vstore(D, al);
    vstore(vccl, vflagFromMask(_mm_blendv_epi8(tNeg, cclA, diff)));  // A:cclA B:T<0
    vstore(vcch, vflagFromMask(_mm_blendv_epi8(ccB, tNeg, diff)));   // A:T<0  B:ccB
    vstoreZero(vcol); vstoreZero(vcoh); vstoreZero(vce);
    return true;
  }
  // --- VCL: cierre del clip que VCH empezo. Cuatro ramas por carril ----------
  // El escalar decide con VCO.low (carry) y VCO.high, y cada rama escribe una bandera
  // distinta: solo (carry && !high) toca VCC.low y solo (!carry && !high) toca VCC.high;
  // las otras dos leen la que ya habia. Aqui se calculan las cuatro y se mezclan, y las
  // banderas se actualizan con blendv usando la mascara de la rama que las escribe, que
  // es lo que conserva las que no se tocan.
  case 0x24: {
    __m128i zero = _mm_setzero_si128();
    __m128i col = vmaskFromFlag(vcol), coh = vmaskFromFlag(vcoh), ce = vmaskFromFlag(vce);
    __m128i cclOld = vmaskFromFlag(vccl), cchOld = vmaskFromFlag(vcch);
    __m128i negT = _mm_sub_epi16(zero, t);

    // rama carry && !high: suma sin signo de 16 bits; hubo acarreo si el resultado
    // envuelto queda por debajo de S (comparacion sin signo, de ahi el max_epu16).
    __m128i sum      = _mm_add_epi16(s, t);
    __m128i carry    = vnot(_mm_cmpeq_epi16(_mm_max_epu16(sum, s), sum));   // sum <u S
    __m128i notCarry = vnot(carry);
    __m128i sumZero  = _mm_cmpeq_epi16(sum, zero);
    __m128i cclNew   = _mm_blendv_epi8(_mm_and_si128(sumZero, notCarry),    // !VCE: !sum && !carry
                                       _mm_or_si128 (sumZero, notCarry),    //  VCE: !sum || !carry
                                       ce);
    // rama !carry && !high: S >= T sin signo
    __m128i cchNew = _mm_cmpeq_epi16(_mm_max_epu16(s, t), s);

    __m128i acclA = _mm_blendv_epi8(s, negT, cclOld);   // carry &&  high
    __m128i acclB = _mm_blendv_epi8(s, negT, cclNew);   // carry && !high
    __m128i acclC = _mm_blendv_epi8(s, t,    cchOld);   // !carry &&  high
    __m128i acclD = _mm_blendv_epi8(s, t,    cchNew);   // !carry && !high
    __m128i al = _mm_blendv_epi8(_mm_blendv_epi8(acclD, acclC, coh),
                                 _mm_blendv_epi8(acclB, acclA, coh), col);
    vstore(accl, al); vstore(D, al);

    __m128i notCoh = vnot(coh);
    vstore(vccl, vflagFromMask(_mm_blendv_epi8(cclOld, cclNew, _mm_and_si128(col, notCoh))));
    vstore(vcch, vflagFromMask(_mm_blendv_epi8(cchOld, cchNew, _mm_andnot_si128(col, notCoh))));
    vstoreZero(vcol); vstoreZero(vcoh); vstoreZero(vce);
    return true;
  }
  }
  return false;   // not implemented here → scalar fallback
}

// --- COP2 (vector unit) -----------------------------------------------------
// Los movimientos entre banco escalar y vectorial (MFC2/CFC2/MTC2/CTC2) viven aparte del
// camino aritmetico por la misma razon que el switch de execVuSse esta partido: el prologo
// lo dimensiona el peor caso de la funcion entera. Ver execCop2 mas abajo.
auto Rsp::execCop2Move(u32 op) -> void {
  u32 sub = op >> 21 & 0x1f;
  {
    int rt = op >> 16 & 31, vs = op >> 11 & 31, velem = op >> 7 & 0xf;
    switch(sub) {
    case 0x00: {  // MFC2
      u8 hi = vpr[vs].gb(velem & 15), lo = vpr[vs].gb((velem + 1) & 15);
      setR(rt, (s32)(s16)(hi << 8 | lo));
    } break;
    case 0x02: {  // CFC2
      int cr = op >> 11 & 3; R128 hi, lo;
      if(cr == 0) { hi = vcoh; lo = vcol; } else if(cr == 1) { hi = vcch; lo = vccl; } else { lo = vce; }
      u32 v = 0; for(int n = 0; n < 8; n++) { v |= lo.get(n) << n; v |= hi.get(n) << (8 + n); }
      setR(rt, (s32)(s16)v);
    } break;
    case 0x04: {  // MTC2
      vpr[vs].sb(velem & 15, r[rt] >> 8);
      if(velem != 15) vpr[vs].sb((velem + 1) & 15, r[rt] & 0xff);
    } break;
    case 0x06: {  // CTC2
      int cr = op >> 11 & 3; u32 v = r[rt];
      R128 *hi = nullptr, *lo = nullptr;
      if(cr == 0) { hi = &vcoh; lo = &vcol; } else if(cr == 1) { hi = &vcch; lo = &vccl; } else { lo = &vce; }
      for(int n = 0; n < 8; n++) { lo->set(n, v & (1 << n)); if(hi) hi->set(n, v & (1 << (8 + n))); }
    } break;
    }
    return;
  }
}

// Camino rapido de COP2. Aqui SOLO va lo que se ejecuta siempre: decodificar, construir
// vt(e) y llamar al SSE. Todo lo demas (los movimientos de arriba y el respaldo escalar de
// abajo) esta en funciones propias, y no por limpieza: con el switch escalar de 64 casos
// dentro, el asignador de registros dimensionaba el prologo de ESTA funcion con el peor caso
// de todo el fichero -- 8 push de GPR, 264 bytes de pila y 10 volcados de xmm6..xmm15 a la
// entrada, mas sus 10 recargas a la salida. Eran ~36 accesos a memoria pagados por CADA
// instruccion vectorial, incluido un VXOR que solo toca dos registros, y COP2 es ~37% del
// microcodigo. Partido, el prologo del camino caliente es el que le corresponde.
template<u32 FN>
auto Rsp::execCop2T(u32 op) -> void {
  constexpr u32 fn = FN;
  u32 e = op >> 21 & 0xf;
  int vt = op >> 16 & 31, vs = op >> 11 & 31, vd = op >> 6 & 31;
  // El valor de vt con su modificador de elemento se queda en registro para el camino
  // rapido; solo el respaldo escalar lo materializa en memoria (ver mas abajo).
  const __m128i tv = vpr[vt].bcast(e);
  R128& S = vpr[vs];
  R128& D = vpr[vd];

#if KESTREL_VUSTAT
  if(g_vustat.on) g_vustat.cop2[fn]++;
#endif
  // Sin camino SSE posible: ir DIRECTO a la funcion pequena, sin tocar el respaldo gordo.
  if constexpr(FN == 0x1d || (FN >= 0x30 && FN <= 0x36)) { execCop2Div(op, tv); return; }

#if KESTREL_VUSTAT
  if(sse && vuOpT<FN>(tv, S, D)) { if(g_vustat.on) g_vustat.cop2sse[fn]++; return; }
#else
  if(sse && vuOpT<FN>(tv, S, D)) return;
#endif

  execCop2Scalar(op, tv);
}

// Tabla de entradas especializadas, una por fn. La rellena el desplegado de abajo, y de ella
// salen tanto el despachador del interprete como los destinos que emite el dynarec: los dos
// ejecutan LA MISMA funcion, asi que no pueden divergir.
using Cop2Fn = void (*)(Rsp*, u32);
namespace {
template<u32 FN> auto cop2Thunk(Rsp* r, u32 op) -> void { r->execCop2T<FN>(op); }
template<u32... I> constexpr auto makeCop2Tab(std::integer_sequence<u32, I...>) {
  return std::array<Cop2Fn, 64>{ &cop2Thunk<I>... };
}
constexpr std::array<Cop2Fn, 64> kCop2Tab = makeCop2Tab(std::make_integer_sequence<u32, 64>{});
}  // namespace

// Punto de entrada del dynarec: le da la direccion ya resuelta para un opcode concreto, de
// modo que el codigo compilado emite un CALL directo en vez de decodificar y saltar.
auto rspCop2Entry(u32 op) -> void* { return (void*)kCop2Tab[op & 0x3f]; }

auto Rsp::execCop2(u32 op) -> void {
  u32 sub = op >> 21 & 0x1f;
  if(sub < 0x10) { execCop2Move(op); return; }
  kCop2Tab[op & 0x3f](this, op);
}

// Respaldo escalar: la referencia bit a bit de la que sale el camino SSE. Se llega aqui solo
// con los fn que execVuSse no cubre (y con KESTREL_NORSPSSE). Funcion propia: ver execCop2.
auto Rsp::execCop2Scalar(u32 op, __m128i tv) -> void {
  u32 fn = op & 0x3f, e = op >> 21 & 0xf;
  int vt = op >> 16 & 31, vs = op >> 11 & 31, vd = op >> 6 & 31, de = op >> 11 & 7;
  (void)vt; (void)e; (void)de;
  R128& S = vpr[vs];
  R128& D = vpr[vd];

  R128 vte; _mm_store_si128(reinterpret_cast<__m128i*>(vte.el), tv);
  switch(fn) {
  case 0x00: case 0x01: {  // VMULF / VMULU
    bool U = fn & 1;
    for(int n = 0; n < 8; n++) {
      accSet(n, (s64)S.s(n) * (s64)vte.s(n) * 2 + 0x8000);
      if(!U) D.u(n) = accSat(n, 1, 0x8000, 0x7fff);
      else   D.u(n) = acch.s(n) < 0 ? 0x0000 : ((acch.s(n) ^ accm.s(n)) < 0 ? 0xffff : accm.uc(n));
    }
  } break;
  case 0x08: case 0x09: {  // VMACF / VMACU
    bool U = fn & 1;
    for(int n = 0; n < 8; n++) {
      accSet(n, accGet(n) + (s64)S.s(n) * (s64)vte.s(n) * 2);
      if(!U) D.u(n) = accSat(n, 1, 0x8000, 0x7fff);
      else   D.u(n) = acch.s(n) < 0 ? 0x0000 : ((acch.s(n) || accm.s(n) < 0) ? 0xffff : accm.uc(n));
    }
  } break;
  case 0x04: for(int n = 0; n < 8; n++) accSet(n, (u16)((u32)(S.uc(n) * vte.uc(n)) >> 16));   D = accl; break;  // VMUDL
  case 0x05: for(int n = 0; n < 8; n++) accSet(n, (u64)(s64)(s32)(S.s(n) * vte.uc(n)));        D = accm; break;  // VMUDM
  case 0x06: for(int n = 0; n < 8; n++) accSet(n, (u64)(s64)(s32)(S.uc(n) * vte.s(n)));        D = accl; break;  // VMUDN
  case 0x07: for(int n = 0; n < 8; n++) { accSet(n, (s64)(S.s(n) * vte.s(n)) << 16); D.u(n) = accSat(n, 1, 0x8000, 0x7fff); } break;  // VMUDH
  case 0x0c: for(int n = 0; n < 8; n++) { accSet(n, accGet(n) + (u32)(S.uc(n) * vte.uc(n) >> 16)); D.u(n) = accSat(n, 0, 0x0000, 0xffff); } break;  // VMADL
  case 0x0d: for(int n = 0; n < 8; n++) { accSet(n, accGet(n) + (u64)(s64)(s32)(S.s(n) * vte.uc(n))); D.u(n) = accSat(n, 1, 0x8000, 0x7fff); } break;  // VMADM
  case 0x0e: for(int n = 0; n < 8; n++) { accSet(n, accGet(n) + (u64)(s64)(s32)(S.uc(n) * vte.s(n))); D.u(n) = accSat(n, 0, 0x0000, 0xffff); } break;  // VMADN
  case 0x0f: for(int n = 0; n < 8; n++) {  // VMADH
    s32 result = (s32)(accGet(n) >> 16) + S.s(n) * vte.s(n);
    acch.u(n) = result >> 16; accm.u(n) = (u16)result; D.u(n) = accSat(n, 1, 0x8000, 0x7fff);
  } break;
  case 0x10: for(int n = 0; n < 8; n++) { s32 rr = S.s(n) + vte.s(n) + (s32)vcol.get(n); accl.u(n) = (u16)rr; D.u(n) = sclamp16(rr); } vcol = R128{}; vcoh = R128{}; break;  // VADD
  case 0x11: for(int n = 0; n < 8; n++) { s32 rr = S.s(n) - vte.s(n) - (s32)vcol.get(n); accl.u(n) = (u16)rr; D.u(n) = sclamp16(rr); } vcol = R128{}; vcoh = R128{}; break;  // VSUB
  case 0x13: for(int n = 0; n < 8; n++) {  // VABS
    if(S.s(n) < 0) { if(vte.s(n) == -32768) { accl.u(n) = 0x8000; D.u(n) = 0x7fff; } else { accl.u(n) = (u16)(-vte.s(n)); D.u(n) = (u16)(-vte.s(n)); } }
    else if(S.s(n) > 0) { accl.u(n) = (u16)vte.s(n); D.u(n) = (u16)vte.s(n); }
    else { accl.u(n) = 0; D.u(n) = 0; }
  } break;
  case 0x14: for(int n = 0; n < 8; n++) { u32 rr = S.uc(n) + vte.uc(n); accl.u(n) = (u16)rr; vcol.set(n, rr >> 16); } vcoh = R128{}; D = accl; break;  // VADDC
  case 0x15: for(int n = 0; n < 8; n++) { u32 rr = (u32)(S.uc(n) - vte.uc(n)); accl.u(n) = (u16)rr; vcol.set(n, (rr >> 16) & 1); vcoh.set(n, rr != 0); } D = accl; break;  // VSUBC
  // VSAR se queda aqui a proposito: es copiar una de las tres mitades del acumulador
  // al destino, 16 bytes, que el compilador ya emite como un movdqa. No hay bucle por
  // carril que vectorizar, meterlo en execVuSse solo cambiaria de sitio la misma copia.
  case 0x1d: execCop2Div(op, tv); break;   // VSAR
  case 0x20: for(int n = 0; n < 8; n++) {  // VLT
    bool c = S.s(n) < vte.s(n) || (S.s(n) == vte.s(n) && vcol.get(n) && vcoh.get(n));
    accl.u(n) = vccl.set(n, c) ? S.uc(n) : vte.uc(n);
  } vcch = R128{}; vcol = R128{}; vcoh = R128{}; D = accl; break;
  case 0x21: for(int n = 0; n < 8; n++) {  // VEQ
    bool c = !vcoh.get(n) && S.uc(n) == vte.uc(n);
    accl.u(n) = vccl.set(n, c) ? S.uc(n) : vte.uc(n);
  } vcch = R128{}; vcol = R128{}; vcoh = R128{}; D = accl; break;
  case 0x22: for(int n = 0; n < 8; n++) {  // VNE
    bool c = S.uc(n) != vte.uc(n) || vcoh.get(n);
    accl.u(n) = vccl.set(n, c) ? S.uc(n) : vte.uc(n);
  } vcch = R128{}; vcol = R128{}; vcoh = R128{}; D = accl; break;
  case 0x23: for(int n = 0; n < 8; n++) {  // VGE
    bool c = S.s(n) > vte.s(n) || (S.s(n) == vte.s(n) && (!vcol.get(n) || !vcoh.get(n)));
    accl.u(n) = vccl.set(n, c) ? S.uc(n) : vte.uc(n);
  } vcch = R128{}; vcol = R128{}; vcoh = R128{}; D = accl; break;
  case 0x24: for(int n = 0; n < 8; n++) {  // VCL
    if(vcol.get(n)) {
      if(vcoh.get(n)) { accl.u(n) = vccl.get(n) ? (u16)(-vte.uc(n)) : S.uc(n); }
      else {
        u16 sum = (u16)(S.uc(n) + vte.uc(n)); bool carry = (u32)(S.uc(n) + vte.uc(n)) != sum;
        if(vce.get(n)) accl.u(n) = vccl.set(n, (!sum || !carry)) ? (u16)(-vte.uc(n)) : S.uc(n);
        else           accl.u(n) = vccl.set(n, (!sum && !carry)) ? (u16)(-vte.uc(n)) : S.uc(n);
      }
    } else {
      if(vcoh.get(n)) { accl.u(n) = vcch.get(n) ? vte.uc(n) : S.uc(n); }
      else { accl.u(n) = vcch.set(n, (s32)S.uc(n) - (s32)vte.uc(n) >= 0) ? vte.uc(n) : S.uc(n); }
    }
  } vcol = R128{}; vcoh = R128{}; vce = R128{}; D = accl; break;
  case 0x25: for(int n = 0; n < 8; n++) {  // VCH
    if((S.s(n) ^ vte.s(n)) < 0) {
      s16 result = (s16)(S.s(n) + vte.s(n));
      accl.u(n) = (result <= 0 ? (u16)(-vte.s(n)) : S.uc(n));
      vccl.set(n, result <= 0); vcch.set(n, vte.s(n) < 0); vcol.set(n, 1);
      vcoh.set(n, result != 0 && S.uc(n) != (u16)(vte.uc(n) ^ 0xffff)); vce.set(n, result == -1);
    } else {
      s16 result = (s16)(S.s(n) - vte.s(n));
      accl.u(n) = (result >= 0 ? vte.uc(n) : S.uc(n));
      vccl.set(n, vte.s(n) < 0); vcch.set(n, result >= 0); vcol.set(n, 0);
      vcoh.set(n, result != 0 && S.uc(n) != (u16)(vte.uc(n) ^ 0xffff)); vce.set(n, 0);
    }
  } D = accl; break;
  case 0x26: for(int n = 0; n < 8; n++) {  // VCR
    if((S.s(n) ^ vte.s(n)) < 0) {
      vcch.set(n, vte.s(n) < 0);
      accl.u(n) = vccl.set(n, S.s(n) + vte.s(n) + 1 <= 0) ? (u16)(~vte.uc(n)) : S.uc(n);
    } else {
      vccl.set(n, vte.s(n) < 0);
      accl.u(n) = vcch.set(n, S.s(n) - vte.s(n) >= 0) ? vte.uc(n) : S.uc(n);
    }
  } vcol = R128{}; vcoh = R128{}; vce = R128{}; D = accl; break;
  case 0x27: for(int n = 0; n < 8; n++) accl.u(n) = vccl.get(n) ? S.uc(n) : vte.uc(n); vcoh = R128{}; vcol = R128{}; D = accl; break;  // VMRG
  case 0x28: for(int n = 0; n < 8; n++) accl.u(n) = S.uc(n) & vte.uc(n);    D = accl; break;  // VAND
  case 0x29: for(int n = 0; n < 8; n++) accl.u(n) = ~(S.uc(n) & vte.uc(n)); D = accl; break;  // VNAND
  case 0x2a: for(int n = 0; n < 8; n++) accl.u(n) = S.uc(n) | vte.uc(n);    D = accl; break;  // VOR
  case 0x2b: for(int n = 0; n < 8; n++) accl.u(n) = ~(S.uc(n) | vte.uc(n)); D = accl; break;  // VNOR
  case 0x2c: for(int n = 0; n < 8; n++) accl.u(n) = S.uc(n) ^ vte.uc(n);    D = accl; break;  // VXOR
  case 0x2d: for(int n = 0; n < 8; n++) accl.u(n) = ~(S.uc(n) ^ vte.uc(n)); D = accl; break;  // VNXOR
  case 0x30: case 0x31: case 0x32: case 0x33: case 0x34: case 0x35: case 0x36:
    execCop2Div(op, tv); break;   // VRCP*/VMOV/VRSQ*: fuera del switch gordo, ver rsp.hpp
  case 0x02: case 0x0a: {  // VRNDP (D=1) / VRNDN (D=0)
    bool Dn = (fn == 0x02);
    for(int n = 0; n < 8; n++) {
      s32 product = (s16)vte.uc(n); if(vs & 1) product <<= 16;
      s64 acc = ((s64)acch.uc(n) << 32 | (s64)accm.uc(n) << 16 | accl.uc(n));
      acc = (acc << 16) >> 16;   // sign-extend 48-bit
      if(!Dn && acc <  0) acc = sclip48(acc + product);
      if(Dn  && acc >= 0) acc = sclip48(acc + product);
      acch.u(n) = acc >> 32; accm.u(n) = acc >> 16; accl.u(n) = acc; D.u(n) = sclamp16((s32)(acc >> 16));
    }
  } break;
  case 0x03: for(int n = 0; n < 8; n++) {  // VMULQ
    s32 product = (s16)S.uc(n) * (s16)vte.uc(n); if(product < 0) product += 31;
    acch.u(n) = product >> 16; accm.u(n) = (u16)product; accl.u(n) = 0; D.u(n) = (u16)(sclamp16(product >> 1) & ~15);
  } break;
  case 0x0b: for(int n = 0; n < 8; n++) {  // VMACQ
    s32 product = acch.uc(n) << 16 | accm.uc(n);
    if(product < 0 && !(product & (1 << 5))) product += 32;
    else if(product >= 32 && !(product & (1 << 5))) product -= 32;
    acch.u(n) = product >> 16; accm.u(n) = (u16)product; D.u(n) = (u16)(sclamp16(product >> 1) & ~15);
  } break;
  case 0x37: case 0x3f: break;  // VNOP / VNULL
  default: for(int n = 0; n < 8; n++) { accl.u(n) = (u16)(S.s(n) + vte.s(n)); D.u(n) = 0; } break;  // VZERO-class
  }
}


// VSAR y la familia del reciproco. Mismo codigo que tenia el switch escalar, en una funcion
// propia: el dynarec y el interprete entran aqui directamente (execCop2T), sin pasar por el
// prologo de peor caso de execCop2Scalar. Ver rsp.hpp.
auto Rsp::execCop2Div(u32 op, __m128i tv) -> void {
  u32 fn = op & 0x3f, e = op >> 21 & 0xf;
  int vt = op >> 16 & 31, vd = op >> 6 & 31, de = op >> 11 & 7;
  R128& D = vpr[vd];
  R128 vte; _mm_store_si128(reinterpret_cast<__m128i*>(vte.el), tv);
  switch(fn) {
  case 0x1d:  // VSAR: copiar una de las tres mitades del acumulador
    D = (e == 8) ? acch : (e == 9) ? accm : (e == 10) ? accl : R128{};
    break;
  case 0x30: case 0x31: {  // VRCP / VRCPL
    bool L = (fn == 0x31);
    s32 input = (L && divdp) ? (s32)((divin << 16) | vte.uc(e & 7)) : (s32)(s16)vte.uc(e & 7);
    s32 mask = input >> 31; s32 data = input ^ mask; if(input > -32768) data -= mask;
    s32 result;
    if(data == 0) result = 0x7fffffff;
    else if(input == -32768) result = (s32)0xffff0000;
    else { u32 shift = clz(data); u32 index = ((u64)data << shift & 0x7fc00000) >> 22;
           result = reciprocals[index]; result = (0x10000 | result) << 14; result = (result >> (31 - shift)) ^ mask; }
    divdp = false; divout = (u16)(result >> 16); accl = vpr[vt](e); D.u(de) = (u16)result;
  } break;
  case 0x32: accl = vpr[vt](e); divdp = true; divin = vte.uc(e & 7); D.u(de) = divout; break;  // VRCPH
  case 0x33: accl = vte; D.u(de) = vte.uc(de); break;                                            // VMOV
  case 0x34: case 0x35: {  // VRSQ / VRSQL
    bool L = (fn == 0x35);
    s32 input = (L && divdp) ? (s32)((divin << 16) | vte.uc(e & 7)) : (s32)(s16)vte.uc(e & 7);
    s32 mask = input >> 31; s32 data = input ^ mask; if(input > -32768) data -= mask;
    s32 result;
    if(data == 0) result = 0x7fffffff;
    else if(input == -32768) result = (s32)0xffff0000;
    else { u32 shift = clz(data); u32 index = ((u64)data << shift & 0x7fc00000) >> 22;
           result = invSqrts[(index & 0x1fe) | (shift & 1)]; result = (0x10000 | result) << 14;
           result = (result >> ((31 - shift) >> 1)) ^ mask; }
    divdp = false; divout = (u16)(result >> 16); accl = vpr[vt](e); D.u(de) = (u16)result;
  } break;
  case 0x36: accl = vpr[vt](e); divdp = true; divin = vte.uc(e & 7); D.u(de) = divout; break;  // VRSQH
  default: break;
  }
}

// --- differential VU fuzz (scalar reference vs SSE fast path) ----------------
auto Rsp::fuzzVU(u64 iters) -> u64 {
  struct Snap {
    R128 vpr[32], acch, accm, accl, vcoh, vcol, vcch, vccl, vce;
    u16 divin, divout; bool divdp;
  };
  auto save = [&](Snap& z) {
    // El memcmp de abajo compara la instantanea entera, relleno incluido: R128 va
    // alineado a 16 y Snap acaba con hueco tras divdp. Sin borrarlo, dos instantaneas
    // distintas traen basura de pila distinta y TODA comparacion sale desigual.
    std::memset(&z, 0, sizeof z);
    std::memcpy(z.vpr, vpr, sizeof vpr);
    z.acch = acch; z.accm = accm; z.accl = accl;
    z.vcoh = vcoh; z.vcol = vcol; z.vcch = vcch; z.vccl = vccl; z.vce = vce;
    z.divin = divin; z.divout = divout; z.divdp = divdp;
  };
  auto load = [&](const Snap& z) {
    std::memcpy(vpr, z.vpr, sizeof vpr);
    acch = z.acch; accm = z.accm; accl = z.accl;
    vcoh = z.vcoh; vcol = z.vcol; vcch = z.vcch; vccl = z.vccl; vce = z.vce;
    divin = z.divin; divout = z.divout; divdp = z.divdp;
  };
  auto eq = [](const Snap& a, const Snap& b) { return std::memcmp(&a, &b, sizeof(Snap)) == 0; };

  u32 st = 0x9e3779b9u;
  auto rnd = [&]() -> u32 { st ^= st << 13; st ^= st >> 17; st ^= st << 5; return st; };

  // fns handled by execVuSse (kept in sync with that switch).
  static const u32 fns[] = {
    0x00, 0x01, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0c, 0x0d, 0x0e, 0x0f,   // VMUL*/VMAC*/VMUD*
    0x10, 0x11, 0x13, 0x14, 0x15, 0x20, 0x21, 0x22, 0x23,                     // add/sub, abs, compares
    0x24, 0x25, 0x26, 0x27,                                                   // clip, merge
    0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d,                                       // logicals
  };
  const u32 nf = (u32)(sizeof fns / sizeof fns[0]);

  Snap in, outScalar, outSse;
  u64 fails = 0, checked = 0;
  for(u64 it = 0; it < iters; it++) {
    // random valid state: vpr/acc arbitrary 16-bit; flag regs strictly 0/1 per lane.
    for(int v = 0; v < 32; v++) for(int n = 0; n < 8; n++) vpr[v].el[n] = (u16)rnd();
    for(int n = 0; n < 8; n++) { acch.el[n] = (u16)rnd(); accm.el[n] = (u16)rnd(); accl.el[n] = (u16)rnd(); }
    for(int n = 0; n < 8; n++) {
      vcoh.el[n] = rnd() & 1; vcol.el[n] = rnd() & 1;
      vcch.el[n] = rnd() & 1; vccl.el[n] = rnd() & 1; vce.el[n] = rnd() & 1;
    }
    u32 fn = fns[rnd() % nf];
    u32 e  = rnd() & 15;
    u32 vt = rnd() & 31, vs = rnd() & 31, vd = rnd() & 31;
    u32 op = (0x12u << 26) | (1u << 25) | (e << 21) | (vt << 16) | (vs << 11) | (vd << 6) | fn;

    save(in);
    sse = false; execCop2(op); save(outScalar);
    load(in);
    sse = true;  execCop2(op); save(outSse);

    checked++;
    if(!eq(outScalar, outSse)) {
      if(fails < 12)
        std::fprintf(stderr, "[rspfuzz] MISMATCH fn=0x%02x e=%u vs=%u vt=%u vd=%u\n", fn, e, vs, vt, vd);
      fails++;
    }
  }
  std::fprintf(stderr, "[rspfuzz] %llu checked, %llu mismatches\n",
               (unsigned long long)checked, (unsigned long long)fails);
  sse = true;
  return fails;
}

// --- fuzz diferencial de cargas/tiendas vectoriales --------------------------
// Oraculo = el camino byte a byte (el que ya validaban systemtest y los microcodigos
// reales). Se ejecuta la MISMA instruccion con vecfast apagado y encendido sobre el
// mismo estado inicial y se comparan los 32 registros vectoriales y los 4 KB de DMEM.
auto Rsp::fuzzLdSt(u64 iters) -> u64 {
  bindMem();
  u32 st = 0x12345678u;
  auto rnd = [&]() -> u32 { st ^= st << 13; st ^= st >> 17; st ^= st << 5; return st; };
  if(mem == nullptr) { std::fprintf(stderr, "[rspldfuzz] sin bus\n"); return 1; }

  std::vector<u8> dm0(4096), dmSlow(4096), dmFast(4096);
  R128 v0[32], vSlow[32], vFast[32];
  u64 fails = 0, checked = 0;
  for(u64 it = 0; it < iters; it++) {
    for(u32 i = 0; i < 4096; i++) dm0[i] = (u8)rnd();
    for(int v = 0; v < 32; v++) for(int n = 0; n < 8; n++) v0[v].el[n] = (u16)rnd();
    u32 r0 = rnd();  // registro base: cualquier direccion, incluida no alineada

    // sub 0x00-0x05 = B/S/L/D/Q/R, que son los que tocan los caminos rapidos.
    u32 sub = rnd() % 6, e = rnd() & 15, imm7 = rnd() & 0x7f;
    bool store = (rnd() & 1) != 0;
    u32 op = ((store ? 0x3au : 0x32u) << 26) | (1u << 21) | (2u << 16) | (sub << 11) | (e << 7) | imm7;

    for(int pass = 0; pass < 2; pass++) {
      vecfast = pass != 0;
      std::memcpy(mem->dmem.data(), dm0.data(), 4096);
      std::memcpy(vpr, v0, sizeof vpr);
      r[1] = r0;
      if(store) execStore(op); else execLoad(op);
      if(pass == 0) { std::memcpy(dmSlow.data(), mem->dmem.data(), 4096); std::memcpy(vSlow, vpr, sizeof vpr); }
      else          { std::memcpy(dmFast.data(), mem->dmem.data(), 4096); std::memcpy(vFast, vpr, sizeof vpr); }
    }
    checked++;
    if(std::memcmp(dmSlow.data(), dmFast.data(), 4096) != 0 ||
       std::memcmp(vSlow, vFast, sizeof vpr) != 0) {
      if(fails < 12)
        std::fprintf(stderr, "[rspldfuzz] MISMATCH %s sub=0x%02x e=%u base=0x%08x imm=0x%02x\n",
                     store ? "SWC2" : "LWC2", sub, e, r0, imm7);
      fails++;
    }
  }
  vecfast = true;
  std::fprintf(stderr, "[rspldfuzz] %llu comprobadas, %llu diferencias\n",
               (unsigned long long)checked, (unsigned long long)fails);
  return fails;
}

// --- fuzz diferencial de la VU EN LINEA del dynarec --------------------------
// Oraculo = el interprete (execCop2, o sea vuOpT). Se monta un bloque real en IMEM con
// cuatro operaciones vectoriales de las que el compilador emite en linea, se compila, y se
// compara el estado vectorial completo tras ejecutar el bloque contra el de interpretar
// las mismas cuatro instrucciones desde el mismo estado inicial. Cubre los 16 modificadores
// de elemento y los solapes vd==vs/vt, que es donde un emisor se rompe.
auto Rsp::fuzzVuJit(u64 iters) -> u64 {
  if(mem == nullptr) { std::fprintf(stderr, "[rspjitfuzz] sin bus\n"); return 1; }
  bindMem();
  // Las que el dynarec emite en linea (vuInline). Mezclarlas de verdad importa: la familia
  // MAC deja el acumulador escrito y la siguiente lo lee, asi que un fallo de acarreo solo
  // aparece con varias seguidas.
  // La rotacion cubre TODO lo que el dynarec emite en linea: si entra una operacion nueva
  // en vuInline y no entra aqui, el oraculo deja de mirarla.
  static const u32 fns[] = { 0x00, 0x01, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
                             0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x13, 0x14, 0x15, 0x1d,
                             0x20, 0x21, 0x22, 0x23, 0x27,
                             0x28, 0x29, 0x2a, 0x2b, 0x2c, 0x2d };
  const u32 nf = (u32)(sizeof fns / sizeof fns[0]);
  const u32 nOps = 4;

  auto* cache = new rspjit::Cache();
  if(!cache->init(1u << 20)) { std::fprintf(stderr, "[rspjitfuzz] sin buffer\n"); delete cache; return 1; }

  u32 st = 0xc0ffee11u;
  auto rnd = [&]() -> u32 { st ^= st << 13; st ^= st >> 17; st ^= st << 5; return st; };

  struct VState { R128 vpr[32], acch, accm, accl, vcoh, vcol, vcch, vccl, vce; };
  VState in{}, outInterp{}, outJit{};
  auto save = [&](VState& z) {
    std::memcpy(z.vpr, vpr, sizeof vpr);
    z.acch = acch; z.accm = accm; z.accl = accl;
    z.vcoh = vcoh; z.vcol = vcol; z.vcch = vcch; z.vccl = vccl; z.vce = vce;
  };
  auto load = [&](const VState& z) {
    std::memcpy(vpr, z.vpr, sizeof vpr);
    acch = z.acch; accm = z.accm; accl = z.accl;
    vcoh = z.vcoh; vcol = z.vcol; vcch = z.vcch; vccl = z.vccl; vce = z.vce;
  };

  u64 fails = 0, checked = 0;
  for(u64 it = 0; it < iters; it++) {
    for(int v = 0; v < 32; v++) for(int n = 0; n < 8; n++) vpr[v].el[n] = (u16)rnd();
    for(int n = 0; n < 8; n++) { acch.el[n] = (u16)rnd(); accm.el[n] = (u16)rnd(); accl.el[n] = (u16)rnd(); }
    for(int n = 0; n < 8; n++) {
      vcoh.el[n] = rnd() & 1; vcol.el[n] = rnd() & 1;
      vcch.el[n] = rnd() & 1; vccl.el[n] = rnd() & 1; vce.el[n] = rnd() & 1;
    }
    save(in);

    u32 ops[8];
    for(u32 i = 0; i < nOps; i++) {
      u32 fn = fns[rnd() % nf], e = rnd() & 15;
      u32 vt = rnd() & 7, vs = rnd() & 7, vd = rnd() & 7;   // pocos registros: fuerza solapes
      ops[i] = (0x12u << 26) | (1u << 25) | (e << 21) | (vt << 16) | (vs << 11) | (vd << 6) | fn;
    }
    // El bloque se pone en una direccion distinta cada vez para no reusar compilaciones.
    u32 pc0 = (u32)((it * 4 * (nOps + 1)) % (4096 - 4 * (nOps + 1))) & 0xffc;
    for(u32 i = 0; i < nOps; i++) {
      u32 w = bswap32(ops[i]); std::memcpy(imp + ((pc0 + 4 * i) & 0xffc), &w, 4);
    }
    u32 brk = bswap32(0x0000000du);   // BREAK: corta el bloque justo detras
    std::memcpy(imp + ((pc0 + 4 * nOps) & 0xffc), &brk, 4);

    for(u32 i = 0; i < nOps; i++) execCop2(ops[i]);
    save(outInterp);

    load(in);
    cache->syncImem(imp);
    cache->state[(pc0 >> 2) & 1023] = rspjit::State::Unknown;
    rspjit::compile(*this, *cache, pc0);
    const rspjit::Block& b = cache->blocks[(pc0 >> 2) & 1023];
    if(cache->state[(pc0 >> 2) & 1023] != rspjit::State::Compiled || b.nOps != nOps) {
      std::fprintf(stderr, "[rspjitfuzz] bloque no compilado en pc=0x%03x (nOps=%u)\n", pc0, b.nOps);
      fails++; continue;
    }
    b.fn(this);
    save(outJit);

    checked++;
    if(std::memcmp(&outInterp, &outJit, sizeof(VState)) != 0) {
      if(fails < 12) {
        std::fprintf(stderr, "[rspjitfuzz] MISMATCH pc=0x%03x ops:", pc0);
        for(u32 i = 0; i < nOps; i++)
          std::fprintf(stderr, " fn=0x%02x e=%u vs=%u vt=%u vd=%u |", ops[i] & 0x3f, ops[i] >> 21 & 0xf,
                       ops[i] >> 11 & 31, ops[i] >> 16 & 31, ops[i] >> 6 & 31);
        std::fprintf(stderr, "\n");
      }
      fails++;
    }
  }
  delete cache;
  std::fprintf(stderr, "[rspjitfuzz] %llu comprobadas, %llu diferencias\n",
               (unsigned long long)checked, (unsigned long long)fails);
  return fails;
}

// --- VU throughput A/B (scalar loop vs 8-lane SSE) ---------------------------
auto Rsp::benchVU(u64 iters) -> void {
  // F3DEX2-weighted mix: MAC/multiply chain dominates geometry+lighting transforms.
  static const u32 mix[] = {
    0x07, 0x05, 0x06, 0x0f, 0x0d, 0x0e,   // VMUDH/M/N + VMADH/M/N (matrix T&L)
    0x00, 0x08, 0x04, 0x0c,               // VMULF/VMACF/VMUDL/VMADL
    0x10, 0x11, 0x14, 0x15,               // VADD/VSUB/VADDC/VSUBC
    0x20, 0x23, 0x27, 0x28, 0x2a, 0x2c,   // VLT/VGE/VMRG + logicals
  };
  u32 nm = (u32)(sizeof mix / sizeof mix[0]);
  static u32 one[1];
  if(const char* q = std::getenv("KESTREL_VUOP")) { one[0] = (u32)strtoul(q, nullptr, 16); nm = 1; }
  const u32* mixp = nm == 1 ? one : mix;
  for(int v = 0; v < 32; v++) for(int n = 0; n < 8; n++) vpr[v].el[n] = (u16)(0x1234 * v + 0x9e37 * n + 1);
  for(int n = 0; n < 8; n++) { acch.el[n] = 0x0111 * n; accm.el[n] = 0x2222; accl.el[n] = 0x3333; }

  auto runOnce = [&](bool useSse) -> double {
    sse = useSse;
    // Las palabras se precalculan: un `i % nm` dentro del bucle mete una division entera
    // (~20 ciclos en este host) por cada op medida y el numero que sale es el de la
    // division, no el de la VU. Con una tabla de 1024 y mascara, el bucle solo tiene la
    // carga y la llamada.
    static u32 ops[1024];
    for(u32 k = 0; k < 1024; k++)
      ops[k] = (0x12u << 26) | (1u << 25) | ((k & 15) << 21) | (2u << 16) | (3u << 11) | (4u << 6) | mixp[k % nm];
    auto t0 = std::chrono::steady_clock::now();
    // Sin sumidero: execCop2 no esta en linea y escribe estado del objeto, asi que el
    // compilador no puede quitar la llamada. El `sink ^= vpr[4].el[0]` que habia aqui leia
    // 16 bits de un registro que la propia op acababa de escribir entero (16 bytes): eso es
    // un store-forward fallido en CADA iteracion (~12 ciclos en Nehalem) y lo que medias
    // era ese tropiezo, no la VU.
    for(u64 i = 0; i < iters; i++) execCop2(ops[i & 1023]);
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(t1 - t0).count();
  };

  runOnce(false); runOnce(true);   // warm up both paths (I$/branch predictor)
  double scal = runOnce(false), simd = runOnce(true);
  std::fprintf(stderr, "[vubench] %llu ops: scalar %.3fs (%.2f ns/op) | sse %.3fs (%.2f ns/op) | speedup %.2fx\n",
               (unsigned long long)iters, scal, scal * 1e9 / iters, simd, simd * 1e9 / iters, scal / simd);
  sse = true;
}

// --- rendimiento del interprete de RSP completo ------------------------------
// `--vubench` solo mide la VU, y la VU resulto ser un tercio del coste: el resto son
// cargas/tiendas vectoriales y ALU escalar, cada una con su propio despacho. Este banco
// llena IMEM con la mezcla REAL medida sobre SM64 (KESTREL_VUSTAT) y cronometra step(),
// que es exactamente el bucle que corre el worker, pero sin los otros dos hilos delante:
// un numero de baja varianza para comparar builds del interprete.
auto Rsp::benchStep(u64 iters) -> void {
  bindMem();
  // Reparto por opcode mayor, en milesimas, tal y como lo midio vustat sobre el arranque
  // de SM64. Se omiten COP0 (toca MMIO) y los saltos (romperian el flujo lineal); su peso
  // se reparte sobre ADDI, que es el relleno barato y no altera nada observable.
  struct Slot { u32 op; u32 per1000; };
  // Las subfamilias tambien van pesadas: un reparto redondo daria a VMULF el mismo peso que
  // a VMADN (4x mas frecuente) y a LPV el mismo que a LDV (9x), y el numero medido seria el
  // de un microcodigo que no existe. Las tablas repiten cada opcode tantas veces como su
  // proporcion real dentro de su familia.
  static const u8 cop2fn[100] = {
    0x0e,0x0e,0x0e,0x0e,0x0e,0x0e,0x0e,0x0e,0x0e,0x0e,0x0e,0x0e,0x0e,0x0e,0x0e,0x0e,0x0e,  // VMADN
    0x0f,0x0f,0x0f,0x0f,0x0f,0x0f,0x0f,0x0f,0x0f,0x0f,0x0f,0x0f,0x0f,                      // VMADH
    0x0d,0x0d,0x0d,0x0d,0x0d,0x0d,0x0d,0x0d,                                               // VMADM
    0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,                                               // VMUDL
    0x10,0x10,0x10,0x10,0x10,0x10,                                                         // VADD
    0x06,0x06,0x06,0x06,0x06,                                                              // VMUDN
    0x11,0x11,0x11,0x11,  0x00,0x00,0x00,0x00,                                             // VSUB / VMULF
    0x15,0x15,0x15,  0x2a,0x2a,0x2a,  0x05,0x05,0x05,                                      // VSUBC / VRCPH / VMUDM
    0x1d,0x1d,0x1d,  0x33,0x33,0x33,  0x23,0x23,0x23,                                      // VSAR / VMOV / VGE
    0x27,0x27,  0x20,0x20,  0x07,0x07,  0x29,0x29,                                         // VMRG / VLT / VMUDH / VRCPL
    0x2c,0x08,0x13,0x24,0x2a,0x25,0x28,0x14,0x2b };                                        // cola: VXOR VMACF VABS VCL ...
  static const u8 lwc2sub[25] = { 3,3,3,3,3,3,3,3,3, 1,1,1,1,1,1, 2,2,2,2, 4,4,4, 7,7, 6 };
  static const u8 swc2sub[20] = { 1,1,1,1,1,1,1,1,1, 3,3,3,3,3,3,3,3, 4,4, 2 };

  auto vuOp   = [&](u32 fn, u32 e, u32 vt, u32 vs, u32 vd) {
    return (0x12u << 26) | (1u << 25) | (e << 21) | (vt << 16) | (vs << 11) | (vd << 6) | fn; };
  auto ldOp   = [&](u32 maj, u32 sub, u32 base, u32 vt, u32 e, u32 off) {
    return (maj << 26) | (base << 21) | (vt << 16) | (sub << 11) | (e << 7) | (off & 0x7f); };
  auto immOp  = [&](u32 maj, u32 rs, u32 rt, u32 imm) {
    return (maj << 26) | (rs << 21) | (rt << 16) | (imm & 0xffff); };

  // 1024 ranuras = IMEM entera; el PC del RSP envuelve a 0 al llegar al final, asi que el
  // flujo se repite solo sin necesidad de un salto (que sesgaria la medida).
  u32 prog[1024];
  u32 k = 0;
  auto fill = [&](u32 n, auto gen) { for(u32 j = 0; j < n && k < 1024; j++) prog[k++] = gen(j); };
  // KESTREL_RSPMIX aisla una familia de opcodes para saber cual pesa. Sin el, la mezcla real.
  const char* mixSel = std::getenv("KESTREL_RSPMIX");
  auto only = [&](const char* w) { return mixSel && std::strcmp(mixSel, w) == 0; };
  if(only("cop2"))  fill(1024, [&](u32 j){ return vuOp(cop2fn[j % 100], j & 15, 2 + (j % 6), 8 + (j % 7), 16 + (j % 8)); });
  // Una sola operacion vectorial repetida (VMADN, la mas frecuente del reparto real). La
  // diferencia contra "cop2" aisla lo que cuesta el DESPACHO por opcode -- el switch de
  // execVuSse es un salto indirecto que con la mezcla real falla la prediccion casi siempre,
  // y con una sola operacion la acierta siempre.
  // KESTREL_RSPMIXFN elige que func vectorial se repite (por defecto VMADN).
  const u32 oneFn = std::getenv("KESTREL_RSPMIXFN") ? (u32)std::strtoul(std::getenv("KESTREL_RSPMIXFN"), nullptr, 0) : 0x0eu;
  if(only("cop2one")) fill(1024, [&](u32 j){ return vuOp(oneFn, j & 15, 2 + (j % 6), 8 + (j % 7), 16 + (j % 8)); });
  // Igual pero ademas con el modificador de elemento fijo a 0 (sin barajado): aisla lo que
  // cuesta construir vt(e).
  if(only("cop2one0")) fill(1024, [&](u32 j){ return vuOp(0x0e, 0, 2 + (j % 6), 8 + (j % 7), 16 + (j % 8)); });
  if(only("vecld")) fill(1024, [&](u32 j){ return (j & 1) ? ldOp(0x32, lwc2sub[j % 25], 1, 2 + (j % 6), (j * 2) & 14, j & 7)
                                                          : ldOp(0x3a, swc2sub[j % 20], 1, 2 + (j % 6), (j * 2) & 14, j & 7); });
  // Dos variantes de ALU a proposito: `alu1` encadena ADDI sobre el mismo registro (cada
  // instruccion depende de la anterior a traves de r[], o sea de un reenvio tienda->carga)
  // y `alu` rota el destino sobre cuatro registros, que es lo que mide el rendimiento real
  // del bucle. La diferencia entre las dos = coste de la cadena de dependencia.
  if(only("alu1"))  fill(1024, [&](u32 j){ return immOp(0x08, 4, 4, 1 + j); });
  if(only("alu"))   fill(1024, [&](u32 j){ return immOp(0x08, 4 + (j & 3), 4 + (j & 3), 1 + j); });
  // `nop` = SLL r0,r0,0 mil veces: no toca ni un registro, asi que lo que mida es el coste
  // del bucle de step() mas el reparto por opcode, sin nada de trabajo util debajo. Es la
  // linea base contra la que hay que restar cualquier otra mezcla.
  if(only("nop"))   fill(1024, [&](u32 j){ (void)j; return 0u; });
  if(only("scald")) fill(1024, [&](u32 j){ return (j & 1) ? immOp(0x23, 1, 11 + (j % 4), (j * 4) & 0x7fc)
                                                          : immOp(0x2b, 1, 11 + (j % 4), (j * 4) & 0x7fc); });
  fill(386, [&](u32 j){ return vuOp(cop2fn[j % 100], j & 15, 2 + (j % 6), 8 + (j % 7), 16 + (j % 8)); });
  fill(123, [&](u32 j){ return (j & 1) ? immOp(0x00, 3 + (j % 4), 5, 0) | (j % 8) * 0x40 | 0x00u
                                       : (0x00u << 26) | ((5 + (j % 3)) << 21) | (6u << 16) | ((7u + (j % 4)) << 11) | 0x21u; });
  fill(103, [&](u32 j){ return ldOp(0x32, lwc2sub[j % 25], 1, 2 + (j % 6), (j * 2) & 14, j & 7); });
  fill( 83, [&](u32 j){ return ldOp(0x3a, swc2sub[j % 20], 1, 2 + (j % 6), (j * 2) & 14, j & 7); });
  fill(122, [&](u32 j){ return immOp(0x08, 4, 4, 1 + j); });                    // ADDI (mas el relleno)
  fill( 38, [&](u32 j){ return immOp(0x21, 1, 9 + (j % 4), (j * 2) & 0x7fe); }); // LH
  fill( 37, [&](u32 j){ return immOp(0x0c, 9, 10, 0x0fff); });                   // ANDI
  fill( 24, [&](u32 j){ return immOp(0x23, 1, 11 + (j % 4), (j * 4) & 0x7fc); }); // LW
  fill( 17, [&](u32 j){ return immOp(0x2b, 1, 11 + (j % 4), (j * 4) & 0x7fc); }); // SW
  fill( 91, [&](u32 j){ return immOp(0x08, 4, 4, 3 + j); });                     // resto -> ADDI
  while(k < 1024) prog[k++] = immOp(0x08, 4, 4, 1);
  for(u32 j = 0; j < 1024; j++) {
    u32 w = prog[j];
    mem->imem[j * 4 + 0] = (u8)(w >> 24); mem->imem[j * 4 + 1] = (u8)(w >> 16);
    mem->imem[j * 4 + 2] = (u8)(w >> 8);  mem->imem[j * 4 + 3] = (u8)w;
  }
  for(u32 j = 0; j < 4096; j++) mem->dmem[j] = (u8)(j * 7 + 3);
  for(u32 v = 0; v < 32; v++) for(u32 n = 0; n < 8; n++) vpr[v].el[n] = (u16)(0x1234 * v + 0x9e37 * n + 1);
  for(u32 g = 1; g < 32; g++) r[g] = g * 0x40;

  auto runOnce = [&]() -> double {
    pc = 0; halt = false; running = true; branch = false; inDelay = false;
    budget = iters + 16;
    auto t0 = std::chrono::steady_clock::now();
    step(iters);
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(t1 - t0).count();
  };
  runOnce();                       // calentar I$/predictor
  double dt = runOnce();
  std::fprintf(stderr, "[rspbench] %llu instrucciones: %.3fs (%.2f ns/op, %.1f Mips) � el N64 pide 62.5\n",
               (unsigned long long)iters, dt, dt * 1e9 / iters, iters / dt / 1e6);
  std::fflush(stderr);
}

// --- run loop ---------------------------------------------------------------
auto Rsp::start() -> void {
  bindMem();
  // Una tarea nueva puede traer microcodigo nuevo. Comparar los 4 KB de IMEM contra la
  // sombra cuesta como calcular una huella y ademas dice QUE ha cambiado, asi que una tarea
  // que recarga su propio microcodigo no invalida nada. Cubre a CUALQUIER escritor (DMA,
  // tienda de la CPU, escritura por MCP) sin poner un gancho en ningun camino caliente.
  jitSelectImage(imp);
  running = true;
  r[0] = 0;
  pc = mem->rcp.sp_pc & 0xfff;
  halt = false; broke = false;
  inDelay = false; pendingTarget = 0;
  budget = 40'000'000;
}

__attribute__((flatten))
auto Rsp::step(u64 maxInsns) -> void {
  if(!running) return;
  bindMem();
  rspThread = std::this_thread::get_id();
  u64 ran = 0, pub = 0;
  // En Threaded esta llamada es la tarea ENTERA en el worker, y el regulador del hilo CPU
  // (Memory::rcpPace) necesita ver el avance mientras corre, no solo al final. Publicar cada
  // 8K instrucciones (~200 us de RSP emulado) cuesta un fetch_add y un notify por bloque:
  // nada frente a las 8K instrucciones, y evita que el regulador tenga que muestrear.
  const bool pubMid = mem && mem->rcpMode == Memory::RcpMode::Threaded;
  // El bucle va por tandas. Este anfitrion (Nehalem) despacha UNA carga por ciclo, asi que
  // en un interprete el numero de accesos a memoria por instruccion es el que manda: cada
  // campo del objeto leido dentro del cuerpo cuesta un ciclo entero del unico puerto de
  // carga. Sacando fuera lo que no cambia durante la tanda -- el puntero a IMEM, el
  // interruptor del muestreador, los tres contadores (maxInsns, budget, ran) reducidos a
  // uno solo -- el cuerpo se queda con las cargas que de verdad hacen falta.
  // La tanda es la misma que ya usaba la publicacion al regulador (8K instrucciones), asi
  // que tampoco se retrasa nada de lo que el hilo CPU necesita ver.
  while(!halt && maxInsns && budget) {
    u64 chunk = maxInsns < budget ? maxInsns : budget;
    if(chunk > 8192) chunk = 8192;
    const bool prof = profOn;
    const bool useJit = jitOn && jc && !prof;
    const bool jitStats = statsOn;
    const u8* const limp = imp;
    u64 c = chunk;
    while(c && !halt) {
      // Dynarec: si en este PC hay un bloque compilado y cabe en lo que queda de tanda, se
      // ejecuta entero y el bucle se salta sus instrucciones. Nunca dentro de un delay-slot
      // (ahi el destino ya esta decidido y el bloque no lo sabe) ni con el muestreador
      // puesto (contaria por bloque en vez de por instruccion).
      if(useJit && !inDelay) {
        const u32 bi = (pc >> 2) & 1023;
        if(jitStats) jc->entries++;
        if(jc->state[bi] == rspjit::State::Unknown) { rspjit::compile(*this, *jc, pc & 0xffc); continue; }
        // Copia, no referencia: el hilo del CPU puede invalidar esta ranura por un DMA a
        // IMEM mientras se mira, y leer fn y nOps por separado de la tabla viva daria un
        // par incoherente (fn valido con nOps=0 = avance de pc nulo = bucle infinito).
        const rspjit::Block blk = jc->blocks[bi];
        if(blk.fn && blk.nOps && blk.nOps <= c) {
          blk.fn(this);
          // Si el bloque cerraba en un salto ya se llevo el delay-slot dentro y dejo el PC
          // final escrito; avanzarlo aqui lo tiraria. Y no queda pestillo de delay pendiente.
          if(!blk.setsPc) pc = (pc + 4u * blk.nOps) & 0xfff;
          c -= blk.nOps;
          if(jitStats) jc->jitOps += blk.nOps;
          continue;
        }
      }
      c--;
      if(jitStats && jc) jc->interpOps++;
      // Fetch: dentro del bucle el PC SIEMPRE esta alineado a palabra (avanza de 4 en 4 y
      // take() enmascara con 0xffc), asi que aqui no hace falta la comprobacion de
      // alineacion de imword() - ese camino byte a byte solo existe para las utilidades de
      // depuracion, que si pueden pedir una direccion impar.
      u32 w; std::memcpy(&w, limp + (pc & 0xffc), 4);
      u32 op = bswap32(w);
      curpc = pc;
      if(prof) { profPc[(pc >> 2) & 1023]++; profTotal++; }   // hotpath sampler (MCP prof.*)
      u32 nextpc = (pc + 4) & 0xfff;
      branch = false;
      exec(op);
      // El `r[0] = 0` que habia aqui era una tienda por instruccion sin efecto: setR() es el
      // unico camino que escribe en r[] y ya ignora el registro 0, que es cableado a cero.
      if(inDelay)      { pc = pendingTarget; inDelay = false; }
      else if(branch)  { pendingTarget = branchTarget; inDelay = true; pc = nextpc; }
      else             { pc = nextpc; }
    }
    u64 done = chunk - c;
    maxInsns -= done; budget -= done; ran += done;
    if(pubMid) {
      // En Threaded esta llamada es la tarea ENTERA en el worker, y el regulador del hilo
      // CPU (Memory::rcpPace) necesita ver el avance mientras corre, no solo al final.
      cyclesRun.fetch_add(ran - pub, std::memory_order_relaxed); pub = ran;
      // Notificar solo si hay alguien dormido. Sin esperador, notify_all sigue siendo
      // una llamada a la CRT y un candado; con esperador, una llamada al kernel. El
      // fetch_add anterior publica cyclesRun ANTES de leer el contador (ver rspWaiters).
      if(mem->rspWaiters.load()) mem->rspCv.notify_all();
    }
  }
  // Ciclos de RSP ejecutados. En modo Lockstep los contaba el bucle del sistema, pero en
  // Threaded el worker llama a step() con la tarea entera y nadie los contaba: el heartbeat
  // decia "RSP 0.0%" justo cuando el RSP es el palo largo. Se publica una vez por llamada,
  // no por instruccion, asi que no toca la linea de cache en el bucle caliente.
  cyclesRun.fetch_add(ran - pub, std::memory_order_relaxed);
  mem->rcp.sp_pc = pc & 0xffc;
  if(halt) {
    // PC is final; now let the CPU see the task end. The release in the fetch_or
    // publishes everything the microcode wrote (DMEM output, PC) to the poller.
    if(broke) {
      broke = false;
      mem->rcp.sp_status.fetch_or(1u | 2u, std::memory_order_acq_rel);   // HALT | BROKE
      if(mem->rcp.sp_intr_on_break) mem->raiseIntr(MI_SP);
    }
    running = false; return;
  }
  if(budget == 0) {                            // microcode hang: force a break
    std::fprintf(stderr, "[rsp] WARNING: budget exhausted at pc=0x%03x (microcode hang?)\n", curpc);
    mem->rcp.sp_status.fetch_or(1u | 2u, std::memory_order_acq_rel);
    if(mem->rcp.sp_intr_on_break) mem->raiseIntr(MI_SP);
    running = false;
  }
}

auto Rsp::run() -> void {
  start();
  step(~0ull);
}

}  // namespace kestrel
