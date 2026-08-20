// kestrel64 — RSP low-level interpreter implementation. See rsp.hpp.
#include "rsp.hpp"
#include "../core/memory.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <immintrin.h>   // SSE2..SSE4.2 (Nehalem host: -march=native)
#include <chrono>
#include <vector>

namespace kestrel {

// El reparto de instrucciones es diagnostico, no produccion: fuera del camino
// caliente salvo build instrumentado (-DKESTREL_VUSTAT=1).
#ifndef KESTREL_VUSTAT
#define KESTREL_VUSTAT 0
#endif

// --- SSE lane helpers (8×s16 = one __m128i; el[n] = lane n) ------------------
// R128.el is u16[8] (16 bytes) but not guaranteed 16-aligned → loadu/storeu.
static inline auto vload(const R128& r) -> __m128i {
  return _mm_loadu_si128(reinterpret_cast<const __m128i*>(r.el));
}
static inline auto vstore(R128& r, __m128i v) -> void {
  _mm_storeu_si128(reinterpret_cast<__m128i*>(r.el), v);
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

auto R128::operator()(u32 e) const -> R128 {
  R128 v;
  _mm_storeu_si128(reinterpret_cast<__m128i*>(v.el),
                   _mm_shuffle_epi8(_mm_loadu_si128(reinterpret_cast<const __m128i*>(el)),
                                    _mm_loadu_si128(reinterpret_cast<const __m128i*>(kBcast.b[e & 15]))));
  return v;
}

Rsp::Rsp() {
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

auto Rsp::rb(u32 a) const -> u8 { return mem->dmem[a & 0xfff]; }
auto Rsp::wb(u32 a, u8 v) -> void { mem->dmem[a & 0xfff] = v; }
// Accesos escalares a DMEM. El RSP no lanza excepciones de alineacion: una direccion
// impar lee bytes sueltos envolviendo dentro de DMEM, de ahi el camino byte a byte.
// Pero el codigo del microcodigo casi siempre esta alineado, y ahi una carga de 16/32
// bits mas bswap sustituye a 2-4 lecturas de byte con sus mascaras. Mismos bytes.
auto Rsp::rHalf(u32 a) const -> u16 {
  u32 a0 = a & 0xfff;
  if((a0 & 1) == 0) { u16 w; std::memcpy(&w, mem->dmem.data() + a0, 2); return bswap16(w); }
  return (u16)(rb(a) << 8 | rb(a + 1));
}
auto Rsp::rWord(u32 a) const -> u32 {
  u32 a0 = a & 0xfff;
  if((a0 & 3) == 0) { u32 w; std::memcpy(&w, mem->dmem.data() + a0, 4); return bswap32(w); }
  return (u32)rb(a) << 24 | rb(a + 1) << 16 | rb(a + 2) << 8 | rb(a + 3);
}
auto Rsp::wHalf(u32 a, u16 v) -> void {
  u32 a0 = a & 0xfff;
  if((a0 & 1) == 0) { u16 w = bswap16(v); std::memcpy(mem->dmem.data() + a0, &w, 2); return; }
  wb(a, v >> 8); wb(a + 1, v);
}
auto Rsp::wWord(u32 a, u32 v) -> void {
  u32 a0 = a & 0xfff;
  if((a0 & 3) == 0) { u32 w = bswap32(v); std::memcpy(mem->dmem.data() + a0, &w, 4); return; }
  wb(a, v >> 24); wb(a + 1, v >> 16); wb(a + 2, v >> 8); wb(a + 3, v);
}
auto Rsp::imword(u32 a) const -> u32 {
  a &= 0xfff;
  const u8* p = mem->imem.data();
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
auto Rsp::execLoad(u32 op) -> void {
  int base = op >> 21 & 31, vt = op >> 16 & 31;
  u32 sub = op >> 11 & 0x1f, e = op >> 7 & 0xf;
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
    if(vecfast && vecFast(a0, e, lim)) { dmemToVec(mem->dmem.data(), a0, (u8*)V.el, e, lim); break; }
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
      dmemToVec(mem->dmem.data(), a & 0xfff, (u8*)V.el, e, lim + 1); break;
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

// --- vector stores (SWC2) ---------------------------------------------------
auto Rsp::execStore(u32 op) -> void {
  int base = op >> 21 & 31, vt = op >> 16 & 31;
  u32 sub = op >> 11 & 0x1f, e = op >> 7 & 0xf;
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
    if(vecfast && vecFast(a0, e, n)) { vecToDmem(mem->dmem.data(), a0, (const u8*)V.el, e, n); break; }
    for(u32 o = e; o < e + n; o++) wb(a++, V.gb(o & 15));   // aqui el elemento SI envuelve
  } break;
  case 0x04: {  // SQV
    u32 a = rsv + imm * 16;
    u32 n = 16u - (a & 15);
    if(vecfast && vecFast(a & 0xfff, e, n)) { vecToDmem(mem->dmem.data(), a & 0xfff, (const u8*)V.el, e, n); break; }
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

// --- 48-bit accumulator as three 16-bit limbs, 8 lanes wide ------------------
struct V48 { __m128i h, m, l; };   // h=acch, m=accm, l=accl (bits 47:32 / 31:16 / 15:0)

// Pack the low 16 bits of each 32-bit lane from two vectors into one 8×16 vector
// (lanes 0-3 from a, 4-7 from b). Mask first so packus (saturating) is a no-op.
static inline auto vpackLow16(__m128i a32, __m128i b32) -> __m128i {
  const __m128i m16 = _mm_set1_epi32(0xffff);
  return _mm_packus_epi32(_mm_and_si128(a32, m16), _mm_and_si128(b32, m16));
}
static inline auto vzl(__m128i x) -> __m128i { return _mm_cvtepu16_epi32(x); }              // low 4 lanes → u32
static inline auto vzh(__m128i x) -> __m128i { return _mm_cvtepu16_epi32(_mm_srli_si128(x, 8)); }  // high 4 lanes → u32

// 48-bit add with carry across the three 16-bit limbs (unsigned limb add = correct
// two's-complement 48-bit add; the top limb carries the sign). Final carry dropped.
static inline auto vadd48(V48 a, V48 b) -> V48 {
  __m128i sLl = _mm_add_epi32(vzl(a.l), vzl(b.l)), sLh = _mm_add_epi32(vzh(a.l), vzh(b.l));
  __m128i cLl = _mm_srli_epi32(sLl, 16),          cLh = _mm_srli_epi32(sLh, 16);
  __m128i sMl = _mm_add_epi32(_mm_add_epi32(vzl(a.m), vzl(b.m)), cLl);
  __m128i sMh = _mm_add_epi32(_mm_add_epi32(vzh(a.m), vzh(b.m)), cLh);
  __m128i cMl = _mm_srli_epi32(sMl, 16),          cMh = _mm_srli_epi32(sMh, 16);
  __m128i sHl = _mm_add_epi32(_mm_add_epi32(vzl(a.h), vzl(b.h)), cMl);
  __m128i sHh = _mm_add_epi32(_mm_add_epi32(vzh(a.h), vzh(b.h)), cMh);
  return { vpackLow16(sHl, sHh), vpackLow16(sMl, sMh), vpackLow16(sLl, sLh) };
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
auto Rsp::execVuSse(u32 fn, const R128& vte, R128& S, R128& D) -> bool {
  const __m128i s = vload(S), t = vload(vte);

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
    __m128i lo = _mm_mullo_epi16(s, t), hi = _mm_mulhi_epi16(s, t);
    __m128i am = vload(accm), ah = vload(acch);
    __m128i cur_lo = _mm_unpacklo_epi16(am, ah), cur_hi = _mm_unpackhi_epi16(am, ah);   // (acch:accm) as 32-bit
    __m128i p_lo   = _mm_unpacklo_epi16(lo, hi), p_hi   = _mm_unpackhi_epi16(lo, hi);   // S*T as 32-bit
    __m128i sum_lo = _mm_add_epi32(cur_lo, p_lo), sum_hi = _mm_add_epi32(cur_hi, p_hi);
    __m128i nm = vpackLow16(sum_lo, sum_hi);
    __m128i nh = vpackLow16(_mm_srli_epi32(sum_lo, 16), _mm_srli_epi32(sum_hi, 16));
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
  // --- VMRG: select S/vte by VCC.low, clear VCO ------------------------------
  case 0x27: {
    __m128i al = _mm_blendv_epi8(t, s, vmaskFromFlag(vccl));
    vstore(accl, al); vstore(D, al);
    vstoreZero(vcoh); vstoreZero(vcol);
    return true;
  }
  }
  return false;   // not implemented here → scalar fallback
}

// --- COP2 (vector unit) -----------------------------------------------------
auto Rsp::execCop2(u32 op) -> void {
  u32 sub = op >> 21 & 0x1f;
  if(sub < 0x10) {  // register moves
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

  u32 fn = op & 0x3f, e = op >> 21 & 0xf;
  int vt = op >> 16 & 31, vs = op >> 11 & 31, vd = op >> 6 & 31, de = op >> 11 & 7;
  R128 vte = vpr[vt](e);
  R128& S = vpr[vs];
  R128& D = vpr[vd];

#if KESTREL_VUSTAT
  if(g_vustat.on) g_vustat.cop2[fn]++;
#endif
#if KESTREL_VUSTAT
  if(sse && execVuSse(fn, vte, S, D)) { if(g_vustat.on) g_vustat.cop2sse[fn]++; return; }
#else
  if(sse && execVuSse(fn, vte, S, D)) return;
#endif

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
  case 0x1d:  // VSAR
    D = (e == 8) ? acch : (e == 9) ? accm : (e == 10) ? accl : R128{};
    break;
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

// --- differential VU fuzz (scalar reference vs SSE fast path) ----------------
auto Rsp::fuzzVU(u64 iters) -> u64 {
  struct Snap {
    R128 vpr[32], acch, accm, accl, vcoh, vcol, vcch, vccl, vce;
    u16 divin, divout; bool divdp;
  };
  auto save = [&](Snap& z) {
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
    0x10, 0x11, 0x14, 0x15, 0x20, 0x21, 0x22, 0x23, 0x25, 0x26, 0x27,         // add/sub, compares, clip, merge
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

// --- VU throughput A/B (scalar loop vs 8-lane SSE) ---------------------------
auto Rsp::benchVU(u64 iters) -> void {
  // F3DEX2-weighted mix: MAC/multiply chain dominates geometry+lighting transforms.
  static const u32 mix[] = {
    0x07, 0x05, 0x06, 0x0f, 0x0d, 0x0e,   // VMUDH/M/N + VMADH/M/N (matrix T&L)
    0x00, 0x08, 0x04, 0x0c,               // VMULF/VMACF/VMUDL/VMADL
    0x10, 0x11, 0x14, 0x15,               // VADD/VSUB/VADDC/VSUBC
    0x20, 0x23, 0x27, 0x28, 0x2a, 0x2c,   // VLT/VGE/VMRG + logicals
  };
  const u32 nm = (u32)(sizeof mix / sizeof mix[0]);
  for(int v = 0; v < 32; v++) for(int n = 0; n < 8; n++) vpr[v].el[n] = (u16)(0x1234 * v + 0x9e37 * n + 1);
  for(int n = 0; n < 8; n++) { acch.el[n] = 0x0111 * n; accm.el[n] = 0x2222; accl.el[n] = 0x3333; }

  auto runOnce = [&](bool useSse) -> double {
    sse = useSse;
    auto t0 = std::chrono::steady_clock::now();
    volatile u32 sink = 0;
    for(u64 i = 0; i < iters; i++) {
      u32 fn = mix[i % nm];
      u32 op = (0x12u << 26) | (1u << 25) | ((i & 15) << 21) | (2u << 16) | (3u << 11) | (4u << 6) | fn;
      execCop2(op);
      sink ^= vpr[4].el[0];
    }
    (void)sink;
    auto t1 = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(t1 - t0).count();
  };

  runOnce(false); runOnce(true);   // warm up both paths (I$/branch predictor)
  double scal = runOnce(false), simd = runOnce(true);
  std::fprintf(stderr, "[vubench] %llu ops: scalar %.3fs (%.2f ns/op) | sse %.3fs (%.2f ns/op) | speedup %.2fx\n",
               (unsigned long long)iters, scal, scal * 1e9 / iters, simd, simd * 1e9 / iters, scal / simd);
  sse = true;
}

// --- run loop ---------------------------------------------------------------
auto Rsp::start() -> void {
  running = true;
  r[0] = 0;
  pc = mem->rcp.sp_pc & 0xfff;
  halt = false; broke = false;
  inDelay = false; pendingTarget = 0;
  budget = 40'000'000;
}

auto Rsp::step(u64 maxInsns) -> void {
  if(!running) return;
  u64 ran = 0, pub = 0;
  // En Threaded esta llamada es la tarea ENTERA en el worker, y el regulador del hilo CPU
  // (Memory::rcpPace) necesita ver el avance mientras corre, no solo al final. Publicar cada
  // 8K instrucciones (~200 us de RSP emulado) cuesta un fetch_add y un notify por bloque:
  // nada frente a las 8K instrucciones, y evita que el regulador tenga que muestrear.
  const bool pubMid = mem && mem->rcpMode == Memory::RcpMode::Threaded;
  while(!halt && maxInsns && budget) {
    maxInsns--; budget--; ran++;
    if(pubMid && (ran & 0x1FFF) == 0) {
      cyclesRun.fetch_add(ran - pub, std::memory_order_relaxed); pub = ran;
      mem->rspCv.notify_all();
    }
    u32 op = imword(pc);
    curpc = pc;
    if(profOn) { profPc[(pc >> 2) & 1023]++; profTotal++; }   // hotpath sampler (MCP prof.*)
    u32 nextpc = (pc + 4) & 0xfff;
    branch = false;
    exec(op);
    r[0] = 0;
    if(inDelay)      { pc = pendingTarget; inDelay = false; }
    else if(branch)  { pendingTarget = branchTarget; inDelay = true; pc = nextpc; }
    else             { pc = nextpc; }
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
