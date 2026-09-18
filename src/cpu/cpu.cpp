#include "../core/wrtag.hpp"
#include "cpu.hpp"
#include "jit.hpp"          // CodeCache completo: cacheOp desenlaza las cadenas del dynarec
#include "../core/memory.hpp"
#include "../video/vifilter.hpp"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <bit>
#include <cmath>
#include <cfenv>
#include <xmmintrin.h>
#pragma STDC FENV_ACCESS ON

namespace kestrel {

// --- Entorno de coma flotante del host, sin <cfenv> --------------------------
// Cada op COP1 fija el modo de redondeo y limpia el estado IEEE antes de calcular. En
// mingw eso entra en __mingw_setfp, que sincroniza la palabra de control x87 Y MXCSR:
// el perfilador de host lo medía en ~32% del tiempo total del emulador con SM64.
// En x86-64 todo el cálculo de float/double va por SSE, así que el único registro que
// gobierna redondeo y banderas es MXCSR, y leerlo/escribirlo son dos instrucciones.
// Los bits son los mismos que expone <cfenv>, sólo que sin la capa de la CRT.
namespace mx {
constexpr u32 RC = 0x6000;                                       // bits 13-14: redondeo
constexpr u32 RN = 0x0000, RM = 0x2000, RP = 0x4000, RZ = 0x6000; // near/-inf/+inf/cero
constexpr u32 EX = 0x003f;                                       // IE DE ZE OE UE PE
constexpr u32 INVALID = 0x01, DIVZERO = 0x04, OVERFLOW_ = 0x08,
              UNDERFLOW_ = 0x10, INEXACT = 0x20;
// Redondeo pedido + banderas a cero. Escribe sólo si algo cambia: el modo casi nunca
// cambia entre ops, así que el caso normal es una lectura y una comparación.
inline auto prep(u32 rc) -> void {
  u32 v = _mm_getcsr(), w = (v & ~(RC | EX)) | rc;
  if(w != v) _mm_setcsr(w);
}
inline auto flags() -> u32 { return _mm_getcsr() & EX; }
// Solo el modo de redondeo, sin tocar las banderas. Para el camino de ADD/SUB/MUL en
// simple, que deduce el Inexact de la aritmetica y por tanto no necesita el MXCSR limpio:
// asi el `ldmxcsr` -- que serializa el pipe -- solo se paga cuando el guest cambia de modo,
// no en cada operacion por culpa de una bandera pegajosa que se quedo puesta.
inline auto prepRc(u32 rc) -> void {
  u32 v = _mm_getcsr();
  if((v & RC) != rc) _mm_setcsr((v & ~RC) | rc);
}
}  // namespace mx

// Interruptores de entorno leídos UNA vez al arranque. Antes vivían como `static` locales
// dentro de step()/dcWrite(): en C++ eso obliga a comprobar la variable-guarda de
// inicialización en CADA ejecución de la sentencia, es decir por instrucción emulada.
// A escala de decenas de millones de instrucciones por segundo eso es coste puro; en
// ámbito de namespace la inicialización dinámica ocurre una sola vez antes de main().
static const bool g_fastFetch       = !std::getenv("KESTREL_NOFETCHFAST");
static const bool g_intLog          = std::getenv("KESTREL_INTLOG")      != nullptr;
static const bool g_jlogNoSpin      = std::getenv("KESTREL_JLOG_NOSPIN") != nullptr;
static const bool g_dcWriteThrough  = std::getenv("KESTREL_DCWT")        != nullptr;

// Depuracion: anillo de transiciones de UNA palabra de RDRAM que elige el usuario
// (`KESTREL_SEENADDR=<fisica>`), volcado junto al veredicto de un xlog con
// KESTREL_LEAKDUMP. Apagado de fabrica: sin la variable no se mira ni una escritura, y
// ninguna direccion de ninguna ROM concreta vive en el nucleo.
static u32 g_seenPrev = 0;
static u32 g_seenAddr = 0;   // 0 = apagado; se fija una vez desde KESTREL_SEENADDR
static constexpr int kSeenRing = 24;
static u64 g_seenRet[kSeenRing] = {};
static u32 g_seenOld[kSeenRing] = {}, g_seenNew[kSeenRing] = {};
static int g_seenIdx = 0;

// Headless framebuffer dump (debug): reads the current VI framebuffer straight
// out of RDRAM and writes a 24-bit BMP. Independent of the Vulkan presenter so a
// batch/headless run can be eyeballed. Honors 16bpp (RGBA5551) and 32bpp.
static auto dumpFramebufferBmp(Memory* mem, const char* path) -> void {
  if(!mem) return;
  u32 origin = mem->rcp.vi_origin & 0x00ff'ffff;
  u32 type   = mem->rcp.vi_ctrl & 3;                 // 2=16bpp, 3=32bpp
  u32 srcW = mem->rcp.vi_width ? mem->rcp.vi_width : 320;   // framebuffer line stride (source pixels)
  if(srcW == 0 || srcW > 640) srcW = 320;
  // This dump is the SOURCE framebuffer exactly as the RDP wrote it: w = VI_WIDTH,
  // one output pixel per stored pixel, no X_SCALE resample.
  //
  // It used to derive the width from the H_VIDEO active window ((hend-hstart)/2),
  // which is the VI's *display* width, and then resample through X_SCALE. That is
  // the presenter's job, not the oracle's: it pinned every dump at 320 columns
  // (standard H_VIDEO $6C02EC = 640 active / 2) regardless of what the ROM
  // actually rendered, so a 640-wide framebuffer came out half as wide and a
  // 160-wide one came out doubled. 181 of the 371 PeterLemon references
  // disagreed on size with the dump for that reason alone, which makes every
  // accuracy number computed from them meaningless. The references are captures
  // of the framebuffer, so the dump has to be the framebuffer.
  u32 w = srcW;
  // Framebuffer height is NOT fixed at 240 — the VI Y_SCALE register (2.10 fixed,
  // source lines per display line) sets it. Krom's low-res demos use YSCALE 0x200
  // (half → 120 source lines) etc. The native source height the RDP renders into is
  // baseH * y_scale, where baseH is the field's active line count (NTSC 240, PAL 288,
  // picked from the VI_V_SYNC total). Hardcoding 240 left the bottom half black and
  // mis-sized the dump vs the reference for every non-240 test.
  u32 ysc = mem->rcp.vi_yscale & 0xfff;
  u32 baseH = (mem->rcp.viHalflines() >= 550) ? 288 : 240;   // PAL (625) vs NTSC (525)
  u32 h = ysc ? ((baseH * ysc) >> 10) : baseH;
  if(h == 0 || h > 576) h = baseH;
  const auto& ram = mem->rdram;
  auto exp5 = [](u32 v){ return (v << 3) | (v >> 2); };
  // Filtros del VI (AA de cobertura, divot, de-dither) SOLO bajo peticion. Este volcado es
  // el framebuffer tal cual lo escribio el RDP, que es lo que son las referencias de
  // PeterLemon: pasarles el filtro del VI las empeora (medido: RSPGradient 100 -> 95,4,
  // FillRectangle 98,6 -> 97,5), porque el barrido filtrado es otra cosa que el contenido
  // de la RDRAM. El filtro de verdad vive en el presentador, que es lo que se ve. Aqui
  // queda como A/B con KESTREL_VIFILTER=1.
  const u32 viCtrl = mem->rcp.vi_ctrl;
  static const bool viFilt = std::getenv("KESTREL_VIFILTER") != nullptr;
  std::vector<u32> filt;
  if(viFilt && vi::active(viCtrl)) {
    filt.resize((usize)w * h);
    vi::fetchFiltered(ram.data(), ram.size(), mem->rdramHidden.data(), mem->rdramHidden.size(),
                      origin, srcW, w, h, viCtrl, filt.data());
  }
  std::vector<u8> rgb((usize)w * h * 3, 0);
  for(u32 y = 0; y < h; y++) for(u32 x = 0; x < w; x++) {
    u32 R=0,G=0,B=0;
    u32 sx = x;
    if(!filt.empty()) { u32 c = filt[(usize)y*w+x]; R=c>>16; G=(c>>8)&0xff; B=c&0xff; }
    else if(type == 2) { u32 p = origin + (y*srcW+sx)*2; if(p+1 < ram.size()) { u32 px=((u32)ram[p]<<8)|ram[p+1]; R=exp5((px>>11)&0x1f); G=exp5((px>>6)&0x1f); B=exp5((px>>1)&0x1f); } }
    else if(type == 3) { u32 p = origin + (y*srcW+sx)*4; if(p+3 < ram.size()) { R=ram[p]; G=ram[p+1]; B=ram[p+2]; } }
    usize o = ((usize)(h-1-y)*w + x)*3;             // BMP is bottom-up; store BGR
    rgb[o]=(u8)B; rgb[o+1]=(u8)G; rgb[o+2]=(u8)R;
  }
  u32 rowSize = ((w*3 + 3) & ~3u), imgSize = rowSize*h, fileSize = 54 + imgSize;
  FILE* f = std::fopen(path, "wb"); if(!f) return;
  u8 hdr[54] = {}; hdr[0]='B'; hdr[1]='M';
  auto put32=[&](int o,u32 v){ hdr[o]=v; hdr[o+1]=v>>8; hdr[o+2]=v>>16; hdr[o+3]=v>>24; };
  auto put16=[&](int o,u16 v){ hdr[o]=v; hdr[o+1]=v>>8; };
  put32(2,fileSize); put32(10,54); put32(14,40); put32(18,w); put32(22,h);
  put16(26,1); put16(28,24); put32(34,imgSize);
  std::fwrite(hdr,1,54,f);
  std::vector<u8> row(rowSize,0);
  for(u32 y=0;y<h;y++){ for(u32 x=0;x<w*3;x++) row[x]=rgb[(usize)y*w*3+x]; std::fwrite(row.data(),1,rowSize,f); }
  std::fclose(f);
  std::fprintf(stderr, "[fbdump] wrote %s (%ux%u type=%u origin=%06x)\n", path, w, h, type, origin);
  std::fflush(stderr);
}

// Sign/zero helpers.
static inline auto sext32(u32 v) -> u64 { return (u64)(s64)(s32)v; }
static inline auto sext16(u16 v) -> u64 { return (u64)(s64)(s16)v; }
static inline auto sext8 (u8  v) -> u64 { return (u64)(s64)(s8)v; }

// --- CIC boot-chip detection --------------------------------------------------
// The cart carries its own CIC-signed IPL3 in ROM bytes 0x40..0xFFF. Which CIC
// variant matters at hand-off: each leaves a different seed in s6 (and games
// keyed to it check osCicId). We identify the chip by CRC32 of the IPL3 image
// (0xFC0 bytes) against the well-known community table, then pick the seed.
// Every field below is a property of the IPL3 image itself, so the CRC identifies
// all of them at once:
//   seed       the byte the CIC feeds the PIF as the IPL2/IPL3 checksum seed. IPL3
//              leaves it in s6, and games keyed to their chip check it.
//   entryDelta what this IPL3 subtracts from the boot address in the ROM header
//              before using it. 6103/7103 and 5101 take off 1 MB, 6106/7106 take off
//              2 MB — obfuscation, per n64brew's CIC-NUS table. The subtraction hits
//              the DMA destination as well as the jump: IPL3 keeps one boot address
//              in one register, and loading the megabyte anywhere else would leave
//              the game unloaded at the address it was linked for.
//   entryFixed non-zero when this IPL3 ignores the header outright (only 7102 does,
//              hardwired to 0x80000480).
//   idNtsc/idPal  the chip's part number. Most NTSC/PAL pairs SHARE one IPL3 image
//              (6102/7101, 6103/7103, 6105/7105, 6106/7106), so the CRC alone cannot
//              tell them apart — the cart's region byte does. 6101 and 7102 are the
//              exception: different images, and 7102 is PAL-only.
struct CicInfo { u32 crc; int idNtsc, idPal; u8 seed; u32 entryDelta; u32 entryFixed; };
static const CicInfo kCicTable[] = {
  { 0x6170A4A1, 6101, 6101, 0x3f,        0, 0 },           // 6101 (Star Fox 64), NTSC only
  { 0x90BB6CB5, 6102, 7101, 0x3f,        0, 0 },           // 6102 / 7101, ~88% of the library
  { 0x009E9EA3, 7102, 7102, 0x3f,        0, 0x8000'0480 }, // 7102 (Lylat Wars), PAL only
  { 0x0B050EE0, 6103, 7103, 0x78, 0x10'0000, 0 },          // 6103 / 7103 (Banjo-Kazooie, DKR)
  { 0x98BC2C86, 6105, 7105, 0x91,        0, 0 },           // 6105 / 7105 (Perfect Dark, DK64, OoT/MM)
  { 0xACC8580A, 6106, 7106, 0x85, 0x20'0000, 0 },          // 6106 / 7106 (F-Zero X, Yoshi's Story)
  { 0x0E018159, 5101, 5101, 0xac, 0x10'0000, 0 },          // 5101 (Aleck 64 arcade)
};
static constexpr u32 kCrcCic6105 = 0x98BC2C86;  // the one IPL3 with an RSP boot stage

static auto crc32(const u8* p, usize n) -> u32 {
  u32 c = 0xffff'ffffu;
  for(usize i = 0; i < n; i++) {
    c ^= p[i];
    for(int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB8'8320u & (~(c & 1) + 1));
  }
  return ~c;
}

static auto detectCic(const std::vector<u8>& rom) -> CicInfo {
  if(rom.size() >= 0x1000) {
    u32 c = crc32(rom.data() + 0x40, 0x1000 - 0x40);
    for(auto& e : kCicTable) if(e.crc == c) return e;
  }
  return { 0, 6102, 7101, 0x3f, 0, 0 };  // unknown → assume the common 6102/7101
}

// Instruction field extractors.
#define OP    (op >> 26 & 0x3f)
#define RS    (op >> 21 & 0x1f)
#define RT    (op >> 16 & 0x1f)
#define RD    (op >> 11 & 0x1f)
#define SA    (op >>  6 & 0x1f)
#define FUNCT (op & 0x3f)
#define IMM16 (op & 0xffff)
#define SIMM  sext16(op & 0xffff)
#define TARGET26 (op & 0x03ff'ffff)

// KESTREL_CPI = ciclos de CPU por instruccion (1..2). Se lee UNA vez por proceso: es
// configuracion de maquina, no estado de invitado.
//
// De fabrica vale 1,4 (kCpiDefault256 = 179 = 128 x 1,4), NO 2. El 2 historico no salia
// de ninguna medida: salia de atar "instruccion retirada" a "tick de Count", y como
// Count corre a medio reloj eso equivale a decir que cada instruccion cuesta dos ciclos.
// El VR4300 real gasta 1,2-1,4 en codigo de juego, asi que con 2 el emulador le daba al
// invitado LA MITAD del presupuesto que tenia en la consola y las escenas pesadas perdian
// el cuadro (la liana de la intro de DK64). Por que 1,4 y no menos: es la parte alta del
// rango real, y las dos medidas propias (DK64 1,19 y Perfect Dark 1,45, docs/GAPS.md) son
// COTAS SUPERIORES de agresividad porque solo restan el giro ocioso de libultra; ademas
// por debajo de 1,4 el rendimiento es decreciente (SM64 arranque: 3,41 campos por cuadro
// con CPI 2, 2,76 con 1,4, 2,69 con 1,25). Sigue siendo una aproximacion de un solo
// numero: el modelo fino es coste por instruccion (fallos de cache, multiciclo), que es
// otra fase. `KESTREL_CPI=2` recupera el comportamiento historico bit a bit.
auto CPU::cpiFromEnv() -> u32 {
  static const u32 v = [] {
    const char* e = std::getenv("KESTREL_CPI");
    if(!e || !*e) return kCpiDefault256;
    double d = std::strtod(e, nullptr);
    if(!(d > 0.0)) return kCpiDefault256;
    double t = 128.0 * d;                  // ticks x256 por op = 256 * (CPI/2)
    if(t > 256.0) t = 256.0;               // >1 tick/op invalidaria las guardas del JIT
    if(t < 1.0)   t = 1.0;
    return (u32)(t + 0.5);
  }();
  return v;
}

// KESTREL_CACHECOST: ciclos de CPU que cuesta rellenar UNA linea de cache primaria desde
// RDRAM. n64brew mide ~640 ns de latencia aleatoria en el RDRAM del N64, que a 93,75 MHz son
// ~60 ciclos; ese es el valor que documenta CLAUDE.md y el que sale al pedir "1" o "on".
//
// Por que sigue apagado de fabrica: el CPI de 1,4 que hay hoy (kCpiDefault256) NO es el CPI de
// canalizacion del VR4300, es un CPI *efectivo* medido sobre juegos reales -- o sea, ya lleva
// dentro el coste medio de los fallos, promediado. Encender esto sin bajar antes el CPI base a
// la canalizacion pura contaria la penalizacion DOS veces y el juego iria al ralenti. La
// recalibracion (CPI base + presupuesto de campo) va aparte y se mide, no se adivina.
auto CPU::missFromEnv(const char* own) -> u32 {
  // Sin static: se llama dos veces (I y D) con perillas distintas. Solo corre al construir.
  {
    const char* e = std::getenv(own);
    if(!e || !*e) e = std::getenv("KESTREL_CACHECOST");
    if(!e || !*e) return 0u;
    if(!std::strcmp(e, "0") || !std::strcmp(e, "off")) return 0u;
    if(!std::strcmp(e, "1") || !std::strcmp(e, "on"))  return 60u;   // ~640 ns @ 93,75 MHz
    unsigned long n = std::strtoul(e, nullptr, 0);
    if(n > 1024) n = 1024;                 // cota de cordura: countTicksMax multiplica por esto
    return (u32)n;
  }
}

// Ciclos que cuesta UNA lectura no cacheada. Misma latencia de RDRAM que un fallo de cache
// (~640 ns = ~60 ciclos a 93,75 MHz) pero perilla separada: el fallo de cache y el acceso
// KSEG1 son fenomenos distintos y hay que poder medirlos por separado. Apagado de fabrica
// por el mismo motivo que KESTREL_CACHECOST -- el CPI por defecto ya lleva dentro la media.
auto CPU::uncachedFromEnv() -> u32 {
  static const u32 v = [] {
    const char* e = std::getenv("KESTREL_UNCACHEDCOST");
    if(!e || !*e) return 0u;
    if(!std::strcmp(e, "0") || !std::strcmp(e, "off")) return 0u;
    if(!std::strcmp(e, "1") || !std::strcmp(e, "on"))  return 60u;   // ~640 ns @ 93,75 MHz
    unsigned long n = std::strtoul(e, nullptr, 0);
    if(n > 1024) n = 1024;                 // misma cota de cordura: countTicksMax multiplica
    return (u32)n;
  }();
  return v;
}

// Perilla del coste de multiplicar/dividir enteros. Ver el comentario largo de chargeMulDiv()
// en cpu.hpp para el porque de cada numero (manual NEC VR4300, tabla 3-12).
auto CPU::stallClockCart() -> bool {
  static const bool v = [] {
    const char* e = std::getenv("KESTREL_STALLCLOCK");
    if(!e || !*e) return true;
    return (bool)(std::strcmp(e, "0") && std::strcmp(e, "off") && std::strcmp(e, "vi"));
  }();
  return v;
}
auto CPU::stallClockOn() -> bool {
  static const bool v = [] {
    const char* e = std::getenv("KESTREL_STALLCLOCK");
    return !e || !*e || (std::strcmp(e, "0") && std::strcmp(e, "off") && std::strcmp(e, "cart"));
  }();
  return v;
}
auto CPU::fpuFromEnv() -> u8 {
  static const u8 v = []() -> u8 {
    const char* e = std::getenv("KESTREL_FPUCOST");
    if(!e || !*e) return 0;
    if(!std::strcmp(e, "stat")) return 1;                        // contar sin cobrar
    if(!std::strcmp(e, "0") || !std::strcmp(e, "off")) return 0;
    return 2;                                                     // block / 1 / on
  }();
  return v;
}
auto CPU::ilkFromEnv() -> u8 {
  static const u8 v = []() -> u8 {
    const char* e = std::getenv("KESTREL_INTERLOCK");
    if(!e || !*e) return 0;
    if(!std::strcmp(e, "stat")) return 1;                        // contar sin cobrar
    if(!std::strcmp(e, "0") || !std::strcmp(e, "off")) return 0;
    return 2;                                                     // 1 / on
  }();
  return v;
}
auto CPU::mulDivFromEnv() -> u8 {
  static const u8 v = []() -> u8 {
    const char* e = std::getenv("KESTREL_MULDIVCOST");
    if(!e || !*e) return 0;
    if(!std::strcmp(e, "stat")) return 1;                       // contar sin cobrar
    if(!std::strcmp(e, "0") || !std::strcmp(e, "off")) return 0;
    return 2;                                                    // 1 / on / cualquier otra cosa
  }();
  return v;
}

auto CPU::reset() -> void {
  for(auto& r : gpr) r = 0;
  hi = lo = 0;
  for(auto& c : cop0) c = 0;
  cop0[C0_Status] = 0x3400'0000;   // CU1|CU0? standard post-reset: FR=0, kernel mode
  cop0[C0_Config] = 0x7006'e463;   // VR4300 post-reset Config (matches n64-systemtest StartupTest)
  cop0[C0_PRId]   = 0x0000'0b22;   // R4300i revision
  cop0[C0_Count]  = 0;
  cop0[C0_Compare]= 0;
  countFrac = 0;
  stallCycles = 0;          // resto de paradas: estado de invitado, como countFrac
  stallOps = 0; stallOpsRem = 0;
  uncachedReads = 0; ramCpuBytes = 0; mulDivOps = 0; mulDivStall = 0;
  fpuOps = 0; fpuStall = 0; fpuNested = 0;
  cop0[C0_Random] = 31;            // Random resets to the top TLB index
  fcr0  = 0x0000'0a00;             // FCR0: VR4300 FPU implementation/revision (CFC1 $0)
  fcr31 = 0;
  for(auto& e : tlb) e = TlbEntry{};
  pc = nextPc = 0;
  halted = false; haltReason.clear();
  retired = 0; lastUnimplemented = 0;
  llbit = false;
  if(mem) mem->cartClock = &retired;   // PI write-latch decay clock (retired-instr count)
  if(mem) mem->cartStall = stallClockCart() ? &stallOps : nullptr;  // + las ops equivalentes a las paradas de cache
  if(mem) { mem->cartStallRem = &stallOpsRem; mem->cartStallCyc = &stallCycles; mem->cartCpi256 = &cpi256; }
  if(mem) mem->jitGuardPtr = &jitGuard;
  if(mem) mem->cartClockPend = &jitPending;   // + lo que la cadena del JIT aun no ha commiteado
}

auto CPU::fastBoot(u32 entryPoint) -> void {
  reset();
  // Identify the cart's CIC boot chip from its IPL3 image — sets the correct
  // hand-off seed (s6) and osCicId. PD NTSC is 6105 (seed 0x91), not 6102.
  CicInfo cic = mem ? detectCic(mem->rom) : CicInfo{ 0, 6102, 7101, 0x3f, 0, 0 };
  bool pal = (bootTvType == 0);                     // 0 PAL / 1 NTSC / 2 MPAL (MPAL carts use NTSC chips)
  int  cicId = pal ? cic.idPal : cic.idNtsc;
  bool is6105 = (cic.crc == kCrcCic6105);
  // El PIF necesita saberlo: el desafio anti-pirateria solo lo contesta este chip.
  if(mem) mem->cic6105 = is6105;
  // Arrancar por IPL3 real o emular su resultado. El camino HLE no es generico: sabe
  // reproducir el efecto de los IPL3 que CONOCE (direccion de arranque propia de cada CIC,
  // la etapa RSP del 6105, la copia del primer megabyte desde ROM+0x1000). Un cartucho con
  // un IPL3 que no esta en la tabla -- homebrew con bootcode propio, y en particular el IPL3
  // libre de libdragon, que es un trampolin firmado en ROM 0x40 que carga el IPL3 de verdad
  // desde ROM 0x1040 y luego un cargador ELF -- no tiene nada que emular en alto nivel: lo
  // unico fiel es EJECUTARLO, que es lo que hace la consola. Con HLE ese cartucho se comia
  // como codigo de juego el IPL3 que hay en ROM+0x1000 y descarrilaba.
  // KESTREL_HLE_IPL3=1 fuerza el camino viejo (bisecar); KESTREL_LLE_IPL3=1 lo fuerza al reves.
  bool lle = std::getenv("KESTREL_LLE_IPL3") != nullptr;
  if(!lle && cic.crc == 0 && !std::getenv("KESTREL_HLE_IPL3")) lle = true;   // IPL3 desconocido
  // Where this IPL3 actually boots the cart. Only 7102, 6103/7103, 6106/7106 and 5101
  // differ from the header word; for everyone else this is the header word verbatim.
  u32 bootAddr = cic.entryFixed ? cic.entryFixed : (entryPoint - cic.entryDelta);
  std::fprintf(stderr, "[boot] CIC detected: %d (seed 0x%02x)%s\n",
               cicId, cic.seed, lle ? " [LLE IPL3]" : "");
  if(bootAddr != entryPoint)
    std::fprintf(stderr, "[boot] CIC-%d boot address: header 0x%08x -> 0x%08x\n",
                 cicId, entryPoint, bootAddr);
  std::fflush(stderr);

  // --- CIC-6105 IPL3 low-RDRAM image + boot microcode ------------------------
  // Carts signed with CIC-NUS-6105 (Perfect Dark, Donkey Kong 64, Majora's Mask,
  // Banjo-Tooie...) all ship the same IPL3, and that IPL3 does two things no other
  // CIC's does before handing control to the game. Games check the result and spin
  // forever if it is missing, so both have to happen on every 6105 boot.
  //
  // Stage 1 — the IPL3 copies a slice of its OWN image out of DMEM into low RDRAM
  // (`lw`/`sw` loop at DMEM 0x524..0x538: DMEM 0x554..0x888 -> RDRAM 0x004..0x338, a
  // fixed -0x550 skew) and carries on executing from that copy. The image is the very
  // bytes the cart holds at ROM 0x554..0x888. Perfect Dark's bootloader spins unless
  // *(0xA00002E8) == 0xC86E2000 — that is just IPL3 code word ROM 0x838 landing at
  // RDRAM 0x2E8.
  //
  // Stage 2 — the IPL3 then starts the RSP (SP_STATUS = 0xAD, DMEM 0x550) on a
  // microcode it XOR-decrypts into IMEM from the IPL2 code the PIF ROM left there.
  // That microcode DMAs RDRAM 0x1E8 (0x1F0 bytes) into IMEM 0x120 and issues ONE
  // strided SP write DMA: SP_DRAM_ADDR = 0x2FB1F0, SP_WR_LEN = 0xFE817000 (length 8,
  // count 24, skip 0xFE8), scattering 24 8-byte rows 0xFF0 apart. Donkey Kong 64
  // checks row 3: *(0xA02FE1C0) == 0xAD170014, i.e. RDRAM 0x200 (the IPL3's
  // `sw s7,0x14(t0)`) at 0x2FB1F0 + 3*0xFF0. The microcode itself cannot be run
  // without the PIF ROM's IMEM residue, and its memory effect is entirely fixed, so
  // reproduce the effect. Same bytes for every 6105 cart; nothing game-specific.
  auto cic6105Ipl3 = [&]() {
    if(!mem || !is6105) return;
    if(mem->rom.size() < 0x888 || mem->rdram.size() < 0x313000 || mem->imem.size() < 0x310) return;
    for(u32 i = 0x554; i < 0x888; i++) mem->rdram[i - 0x550] = mem->rom[i];
    // The first instruction word of that microcode stays behind at IMEM 0x004 once the
    // RSP halts, and 6105 games do read it back. It differs by console region because
    // the microcode is the XOR of the IPL3 key table with the PIF ROM's IPL2 residue,
    // and the PAL PIF ROM is a different image.
    { u32 w = pal ? 0xbda8'07fcu : 0x8da8'07fcu;
      mem->imem[4]=w>>24; mem->imem[5]=w>>16; mem->imem[6]=w>>8; mem->imem[7]=w; }
    for(u32 i = 0; i < 0x1f0; i++)     mem->imem[0x120 + i]  = mem->rdram[0x1e8 + i];
    for(u32 row = 0; row < 24; row++) {
      u32 dst = 0x2fb1f0 + row * 0xff0, src = 0x120 + row * 8;
      for(u32 i = 0; i < 8; i++) mem->rdram[dst + i] = mem->imem[src + i];
    }
  };

  if(lle && mem && mem->rom.size() >= 0x1000) {
    // LLE boot: run the cart's OWN CIC-signed IPL3 instead of faking its result.
    // The PIF copies the 0xFC0-byte IPL3 image (ROM 0x40..0xFFF) into SP DMEM at
    // 0x04000040 and starts the CPU there; the IPL3 then inits RDRAM, copies the
    // boot segment ROM->RDRAM, verifies its checksum against the CIC seed, and
    // jumps to the game entry. We reproduce that hand-off exactly.
    for(u32 i = 0; i < 0xFC0 && i < mem->dmem.size() - 0x40; i++)
      mem->dmem[0x40 + i] = mem->rom[0x40 + i];
    // IPL2 -> IPL3 register hand-off (what the PIF/IPL2 leave for IPL3).
    gpr[19] = 0;                              // s3 = osRomType (cart)
    gpr[20] = (u64)bootTvType;                // s4 = osTvType (0 PAL / 1 NTSC / 2 MPAL)
    gpr[21] = 0;                              // s5 = osResetType (cold)
    gpr[22] = (u64)cic.seed;                 // s6 = CIC seed
    gpr[23] = pal ? 6u : 0u;                  // s7 = osVersion (PAL PIF ROM reports 6)
    // t3 apunta al PROPIO IPL3 ya copiado en DMEM. No es decorativo: el IPL3 de los
    // cartuchos CIC-6105 (Perfect Dark, Zelda, Banjo) arranca con un descifrador que lee
    // su tabla con `lw t2, 0x44(t3)`, es decir DMEM+0x84, justo detras del stub. Sin ese
    // registro la primera lectura va a la direccion 0x44 y el IPL3 muere en un TLBL.
    gpr[11] = 0xffff'ffff'a400'0040ull;      // t3 = base del IPL3 en DMEM
    gpr[31] = 0xffff'ffff'a400'1550ull;      // ra (el IPL3 no vuelve, pero es el valor real)
    gpr[29] = 0xffff'ffff'a400'1ff0ull;      // sp in SP DMEM
    pc = sext32(0xa400'0040);                // execute IPL3 from DMEM (uncached)
    nextPc = pc + 4;
    // osMemSize/osTvType/etc. the real IPL3 leaves at 0x300; write the few the
    // game reads even though a full IPL3 would compute them, so post-boot matches.
    if(mem->rdram.size() >= 0x400) {
      auto putw = [&](u32 p, u32 v) {
        mem->rdram[p]=v>>24; mem->rdram[p+1]=v>>16; mem->rdram[p+2]=v>>8; mem->rdram[p+3]=v; };
      putw(0x300, bootTvType);               // osTvType (region del cartucho)
      putw(0x318, (u32)mem->rdram.size());   // osMemSize (IPL3 probes RDRAM for this)
      // The PIF's RDRAM self test leaves the size here too; the 6105 IPL3 reads it back
      // (`lw t1,0xf0(t0)`, t0 = 0xA0000300) and is what actually fills osMemSize at 0x318.
      putw(0x3f0, (u32)mem->rdram.size());
    }
    // The real IPL3 redoes stage 1 itself (identical bytes), but stage 2 needs the RSP
    // microcode that only exists once the PIF ROM's IPL2 residue is in IMEM — which no
    // HLE PIF leaves behind. Seed both here so LLE lands on the same memory state.
    cic6105Ipl3();
    goto envflags;
  }
  // HLE IPL3: copy the boot segment (up to 1 MB from ROM+0x1000) to RDRAM at the
  // entry's physical address, then jump to the entry point.
  if(mem && !mem->rom.empty()) {
    u32 phys = bootAddr & 0x1fff'ffff;
    usize count = mem->rom.size() > 0x1000 ? mem->rom.size() - 0x1000 : 0;
    if(count > 0x0010'0000) count = 0x0010'0000;
    for(usize i = 0; i < count && phys + i < mem->rdram.size(); i++) {
      mem->rdram[phys + i] = mem->rom[0x1000 + i];
    }
  }
  cic6105Ipl3();               // 6105 low-RDRAM image + boot-microcode scatter (see above)
  // PIF boot globals the OS/game spin-wait on (physical 0x300..0x3FF in RDRAM).
  // Real IPL3/PIF populates these; the HLE boot must too or osInitialize hangs.
  if(mem && mem->rdram.size() >= 0x400) {
    auto putw = [&](u32 phys, u32 v) {
      mem->rdram[phys+0]=v>>24; mem->rdram[phys+1]=v>>16;
      mem->rdram[phys+2]=v>>8;  mem->rdram[phys+3]=v;
    };
    putw(0x300, bootTvType);   // osTvType (0 PAL / 1 NTSC / 2 MPAL), segun la region del cart
    putw(0x304, 0);            // osRomType (0 = cart)
    putw(0x308, 0xb000'0000);  // osRomBase (cart domain-1, KSEG1)
    putw(0x30c, 0);            // osResetType: 0 = cold boot
    putw(0x310, (u32)cicId);   // osCicId (6101/6102/6103/6105/6106, PAL 71xx)
    putw(0x314, 0);            // osVersion
    putw(0x318, (u32)mem->rdram.size());  // osMemSize (RDRAM bytes)
    putw(0x31c, 0);            // osAppNMIBuffer[0]
    // The PIF's RDRAM power-on self test leaves the measured size at 0x3F0 as well;
    // the 6105 IPL3 reads it back (`lw t1,0xf0(t0)` with t0 = 0xA0000300) and forwards
    // it to osMemSize. Under HLE nothing reads it, but the word is part of the boot
    // state a game may inspect, and the LLE IPL3 path needs it to compute 0x318.
    putw(0x3f0, (u32)mem->rdram.size());
  }
  // --- register hand-off, exactly as the real IPL3 leaves it ------------------
  // Most of these are not "initialisation": they are the leftovers of the checksum
  // work IPL3 does over the first megabyte of the cart, so they depend on BOTH the
  // CIC variant (each one uses a different seed and magic) and the console region
  // (the PIF ROM the checksum runs against differs). v0/v1/a0 and, on PAL, a1 are
  // literally pieces of the IPL2 checksum for that chip — for 6102 the checksum is
  // 0xA536C0F1D859 and the boot leaves a0 = 0xA536 with PAL a1 = 0xC0F1D859.
  // A game that reads them (a few anti-piracy checks do) sees the same values a
  // console would. Table cross-checked against Project64's post-IPL3 state.
  gpr[ 6] = 0xffff'ffff'a400'1f0cull;      // a2
  gpr[ 7] = 0xffff'ffff'a400'1f08ull;      // a3
  gpr[ 8] = 0x0000'0000'0000'00c0ull;      // t0
  gpr[10] = 0x0000'0000'0000'0040ull;      // t2
  gpr[11] = 0xffff'ffff'a400'0040ull;      // t3 = the IPL3 image still in DMEM
  gpr[20] = (u64)bootTvType;               // s4 = osTvType
  gpr[22] = (u64)cic.seed;                 // s6 = CIC seed
  gpr[23] = pal ? 6u : 0u;                 // s7 = osVersion
  gpr[24] = pal ? 0u : 3u;                 // t8 (6105/6106 override it below on PAL)
  gpr[29] = 0xffff'ffff'a400'1ff0ull;      // sp
  gpr[31] = pal ? 0xffff'ffff'a400'1554ull // ra: the PAL IPL3 tail is one instruction later
                : 0xffff'ffff'a400'1550ull;
  {
    auto set = [&](u64 v0, u64 a0, u64 t4, u64 t5, u64 t7, u64 t9, u64 at) {
      gpr[2] = gpr[3] = v0; gpr[4] = a0; gpr[12] = t4; gpr[13] = t5;
      gpr[15] = t7; gpr[25] = t9; gpr[1] = at;
    };
    switch(cic.crc) {
      case 0x90BB6CB5:  // 6102 / 7101
        set(0x0000'0000'0ebd'a536ull, 0xa536, 0xffff'ffff'ed10'd0b3ull, 0x0000'0000'1402'a4ccull,
            0x0000'0000'3103'e121ull, 0xffff'ffff'9deb'b54full, 1);
        gpr[5]  = pal ? 0xffff'ffff'c0f1'd859ull : 0xffff'ffff'c959'73d5ull;   // a1
        gpr[14] = pal ? 0x0000'0000'2de1'08eaull : 0x0000'0000'2449'a366ull;   // t6
        break;
      case 0x0B050EE0:  // 6103 / 7103
        set(0x0000'0000'49a5'ee96ull, 0xee96, 0xffff'ffff'ce9d'fbf7ull, 0xffff'ffff'ce9d'fbf7ull,
            0x0000'0000'18b6'3d28ull, 0xffff'ffff'825b'21c9ull, 1);
        gpr[5]  = pal ? 0xffff'ffff'd464'6273ull : 0xffff'ffff'9531'5a28ull;
        gpr[14] = pal ? 0x0000'0000'1af9'9984ull : 0x0000'0000'5bac'a1dfull;
        break;
      case kCrcCic6105:  // 6105 / 7105
        set(0xffff'ffff'f58b'0fbfull, 0x0fbf, 0xffff'ffff'9651'f81eull, 0x0000'0000'2d42'aac5ull,
            0x0000'0000'5658'4d60ull, 0xffff'ffff'cdce'565full, 0);
        gpr[5]  = pal ? 0xffff'ffff'deca'aad1ull : 0x0000'0000'5493'fb9aull;
        gpr[14] = pal ? 0x0000'0000'0cf8'5c13ull : 0xffff'ffff'c2c2'0384ull;
        if(pal) gpr[24] = 2;
        break;
      case 0xACC8580A:  // 6106 / 7106
        set(0xffff'ffff'a959'30a4ull, 0x30a4, 0xffff'ffff'bcb5'9510ull, 0xffff'ffff'bcb5'9510ull,
            0x0000'0000'7a3c'07f4ull, 0x0000'0000'465e'3f72ull, 0);
        gpr[5]  = pal ? 0xffff'ffff'b04d'c903ull : 0xffff'ffff'e067'221full;
        gpr[14] = pal ? 0x0000'0000'1af9'9984ull : 0x0000'0000'5cd2'b70full;
        if(pal) gpr[24] = 2;
        break;
      default: break;   // 6101, 7102, 5101 and unknown images: only the seed is known
    }
  }
  // COP0 state the real IPL3 leaves at game entry (CIC-6102 hand-off, matches krom
  // COP0Register + n64brew "initial register state"): Status has CU1|FR|SR and 64-bit
  // addressing enabled in all modes (KX|SX|UX); EPC and ErrorEPC keep their power-on
  // all-ones (the boot chain never writes them). Kernel mode → CU0 is unnecessary.
  cop0[C0_Status]   = 0x2410'00e0;
  cop0[C0_EPC]      = sext32(0xffff'ffff);
  cop0[C0_ErrorEPC] = sext32(0xffff'ffff);
  pc = sext32(bootAddr);
  nextPc = pc + 4;
envflags:
  if(const char* b = std::getenv("KESTREL_BP")) bpAddr = sext32((u32)std::strtoul(b, nullptr, 0));
  if(std::getenv("KESTREL_BPTRACE")) bpTrace = true;
  if(std::getenv("KESTREL_TRAPWILD")) trapWild = true;
  if(mem && std::getenv("KESTREL_TRAPSPREG")) mem->trapSpRegStore = true;
  if(std::getenv("KESTREL_HUFT")) huftTrap = true;
  if(const char* a = std::getenv("KESTREL_AUDIOHOOK")) audioHook = (u32)std::strtoul(a, nullptr, 0);
  if(const char* m = std::getenv("KESTREL_MAXINSN")) maxInsn = std::strtoull(m, nullptr, 0);
  if(std::getenv("KESTREL_EXCTRACE")) excTrace = true;
  if(std::getenv("KESTREL_EXCTAIL")) excTail = true;
  if(std::getenv("KESTREL_FPDBG")) fpDbg = true;
  if(std::getenv("KESTREL_FPTRACE")) fpTrace = true;
  if(std::getenv("KESTREL_PCRING")) pcRingOn = true;
  if(const char* e = std::getenv("KESTREL_SEENADDR"))
    g_seenAddr = (u32)std::strtoul(e, nullptr, 0) & 0x1fff'fffcu;
  if(const char* w = std::getenv("KESTREL_WATCHP")) wPhys = (u32)std::strtoul(w, nullptr, 0) & 0x1fff'ffffu;
  if(std::getenv("KESTREL_HALT_UNIMPL")) haltUnimpl = true;
  dcDbgOn = (wPhys != 0) || g_dcWriteThrough;
  if(dcDbgOn) stGuard |= StGuardDbg; else stGuard = (u8)(stGuard & ~StGuardDbg);
  refreshDebugArmed();
}

// Recompute the single hot-path debug guard from the individual trap flags plus
// the pc-window/pc-sample env vars (which are read lazily inside the prologue).
auto CPU::refreshDebugArmed() -> void {
  debugArmed = bpAddr || bpTrace || trapWild || excTail || huftTrap ||
               audioHook || maxInsn ||
               std::getenv("KESTREL_PCLO") || std::getenv("KESTREL_PCSAMPLE");
}

// --- memory (segment rules + TLB translation via translate()) ----------------
// Cached data accesses route through the write-back D-cache; uncached (KSEG1) and
// non-RDRAM targets go straight to the bus. `pe` is the reverse-endian-adjusted phys.
auto CPU::read8 (u64 v) -> u8  { u64 p=xlat(v,AccRead); if(memAbort||!mem) return 0; if(__builtin_expect(watchArmed,0) && watchTrip(p,AccRead)) return 0; u32 pe=(u32)reXor(p,1); if(cacheable(v)&&pe<mem->rdram.size()) return (u8)dcRead(pe,1); return uncachedRead(pe, 1, [&]{ return mem->read8(pe); }); }
auto CPU::read16(u64 v) -> u16 { if(alignBad(v,2,AccRead)) return 0; u64 p=xlat(v,AccRead); if(memAbort||!mem) return 0; if(__builtin_expect(watchArmed,0) && watchTrip(p,AccRead)) return 0; u32 pe=(u32)reXor(p,2); if(cacheable(v)&&pe<mem->rdram.size()) return (u16)dcRead(pe,2); return uncachedRead(pe, 2, [&]{ return mem->read16(pe); }); }
auto CPU::read32(u64 v) -> u32 { if(alignBad(v,4,AccRead)) return 0; u64 p=xlat(v,AccRead); if(memAbort||!mem) return 0; if(__builtin_expect(watchArmed,0) && watchTrip(p,AccRead)) return 0; u32 pe=(u32)reXor(p,4); if(cacheable(v)&&pe<mem->rdram.size()) return (u32)dcRead(pe,4); return uncachedRead(pe, 4, [&]{ return mem->read32(pe); }); }
auto CPU::read64(u64 v) -> u64 { if(alignBad(v,8,AccRead)) return 0; u64 p=xlat(v,AccRead); if(memAbort||!mem) return 0; if(__builtin_expect(watchArmed,0) && watchTrip(p,AccRead)) return 0; u32 pe=(u32)p; if(cacheable(v)&&pe<mem->rdram.size()) return dcRead(pe,8); return uncachedRead(pe, 8, [&]{ return mem->read64(pe); }); }
auto CPU::write8 (u64 v, u8  x) -> void { u64 p=xlat(v,AccWrite); if(memAbort||!mem) return; if(__builtin_expect(watchArmed,0) && watchTrip(p,AccWrite)) return; u32 pe=(u32)reXor(p,1); if(cacheable(v)&&pe<mem->rdram.size()){ dcWrite(pe,x,1); return; } ramUncached(pe,1); mem->write8 (pe, x); }
auto CPU::write16(u64 v, u16 x) -> void { if(alignBad(v,2,AccWrite)) return; u64 p=xlat(v,AccWrite); if(memAbort||!mem) return; if(__builtin_expect(watchArmed,0) && watchTrip(p,AccWrite)) return; u32 pe=(u32)reXor(p,2); if(cacheable(v)&&pe<mem->rdram.size()){ dcWrite(pe,x,2); return; } ramUncached(pe,2); mem->write16(pe, x); }
auto CPU::seenWatch(u64 p, u32 size) -> void {
  if(!g_seenAddr || !mem || !(p <= g_seenAddr && p + size > g_seenAddr)) return;
  u32 cur = mem->read32(g_seenAddr);
  if(cur != g_seenPrev) { int i=g_seenIdx%kSeenRing; g_seenRet[i]=retired; g_seenOld[i]=g_seenPrev; g_seenNew[i]=cur; g_seenIdx++; g_seenPrev=cur; }
}
auto CPU::write32(u64 v, u32 x) -> void { if(alignBad(v,4,AccWrite)) return; u64 p=xlat(v,AccWrite); if(memAbort||!mem) return; if(__builtin_expect(watchArmed,0) && watchTrip(p,AccWrite)) return; u32 pe=(u32)reXor(p,4); if(cacheable(v)&&pe<mem->rdram.size()) dcWrite(pe,x,4); else { ramUncached(pe,4); mem->write32(pe, x); } seenWatch(p&0x1fffffff,4); }
auto CPU::write64(u64 v, u64 x) -> void { if(alignBad(v,8,AccWrite)) return; u64 p=xlat(v,AccWrite); if(memAbort||!mem) return; if(__builtin_expect(watchArmed,0) && watchTrip(p,AccWrite)) return; u32 pe=(u32)p; if(cacheable(v)&&pe<mem->rdram.size()) dcWrite(pe,x,8); else { ramUncached(pe,8); mem->write64(pe, x); } seenWatch(p&0x1fffffff,8); }

auto CPU::connect(Memory* m) -> void {
  mem = m;
  // El bus arma/desarma el modo repeticion de MI_MODE; el camino rapido de store del dynarec
  // lo mira en stGuard, asi que le damos al bus la direccion del byte en vez de que el codigo
  // emitido tenga que perseguir mem->rcp en cada escritura.
  if(m) m->cpuStGuard = &stGuard;
}

auto CPU::storeRepeat(u32 phys, u64 reg, u32 sz) -> bool {
  if(!mem || !mem->rcp.mi_repeat_on) return false;
  if(phys >= mem->rdram.size()) return false;   // only RDRAM is broadcast; MMIO/cart write normally
  mem->rcp.mi_repeat_on = false;                 // the arm fires exactly once
  stGuard = (u8)(stGuard & ~StGuardRepeat);
  mem->miRepeatStore(phys, reg, sz);
  return true;
}

auto CPU::storeCart(u32 phys, u64 reg, u32 width) -> bool {
  if(!mem || !mem->isCart(phys)) return false;
  // The 16-bit PI bus carries a 2-byte unit selected by addr bit0; the register's bytes
  // sit on that lane so the latched upper halfword is reg shifted to land the stored byte,
  // spilling the neighbouring register byte above it (e.g. SB 0x..56BA at offset 1 -> 0x56BA).
  u32 base  = phys & 1u;
  u32 shift = (2 - width - base) * 8;
  u16 hw    = (u16)(reg << shift);
  mem->cartWrite(phys, hw, 2);                 // latch the upper halfword of the 32-bit bus word
  return true;
}

// --- primary caches ----------------------------------------------------------
// (cacheable() vive ahora en linea en cpu.hpp: se llama una vez por acceso a memoria.)

auto CPU::ramUncached(u32 pe, u32 size) -> void {
  if(mem && pe < (u32)mem->rdram.size()) ramCpuBytes += size;
}

auto CPU::dcFill(u32 idx, u32 base) -> void {
  DCacheLine& l = dcache[idx];
  ramCpuBytes += 16;
  l.tagv = base | 1u; l.dirty = 0;
  // Camino normal: la línea entera cae dentro de RDRAM → una copia de 16 B en vez de 16
  // lecturas con comprobación de rango. El borde (línea a caballo del final) mantiene la
  // semántica byte a byte con relleno a 0.
  if(base + 16 <= mem->rdram.size()) std::memcpy(l.data, &mem->rdram[base], 16);
  else for(u32 i = 0; i < 16; i++) l.data[i] = (base + i < mem->rdram.size()) ? mem->rdram[base + i] : 0;
}

auto CPU::dcFlush(u32 idx) -> void {
  DCacheLine& l = dcache[idx];
  if(!l.valid() || !l.dirty) return;
  ramCpuBytes += 16;
  u32 tag = l.ptag();
  // El volcado de una linea sucia es la unica forma en que un store CACHEADO de la CPU
  // llega a RDRAM, asi que sin esto KESTREL_WATCH no ve el 99% de lo que escribe el juego.
  wrtag::mark(tag, wrtag::kDcache, (u32)pc);
  if(mem->watchAddr) mem->watchHit(tag, 16, 0, false);
  if(tag + 16 <= mem->rdram.size()) std::memcpy(&mem->rdram[tag], l.data, 16);
  else for(u32 i = 0; i < 16; i++) if(tag + i < mem->rdram.size()) mem->rdram[tag + i] = l.data[i];
  l.dirty = 0;
}

// Fallo de linea: volcado de la vieja + relleno de la nueva en una sola llamada. Es
// exactamente dcFlush(idx) seguido de dcFill(idx, base) -- misma semantica, mismo orden --
// pero resolviendo la linea, el puntero de RDRAM y su tamano una sola vez. La rama de
// volcado es la unica forma en que un store CACHEADO llega a RDRAM, asi que conserva el
// tag de ultimo escritor y el punto de vigilancia.
auto CPU::dcMiss(u32 idx, u32 base) -> void {
  dcMisses++;
  chargeDcMiss();   // el relleno de linea paga la latencia de RDRAM (ver countTicks)
  DCacheLine& l = dcache[idx];
  u8* const ram = mem->rdram.data();
  const u32  sz = (u32)mem->rdram.size();
  ramCpuBytes += 16;                 // relleno de la linea nueva
  if(l.dirty && l.valid()) {
    ramCpuBytes += 16;               // ... mas el volcado de la vieja
    u32 tag = l.ptag();
    wrtag::mark(tag, wrtag::kDcache, (u32)pc);
    if(mem->watchAddr) mem->watchHit(tag, 16, 0, false);
    if(tag + 16 <= sz) std::memcpy(ram + tag, l.data, 16);
    else for(u32 i = 0; i < 16; i++) if(tag + i < sz) ram[tag + i] = l.data[i];
  }
  l.tagv = base | 1u; l.dirty = 0;
  if(base + 16 <= sz) std::memcpy(l.data, ram + base, 16);
  else for(u32 i = 0; i < 16; i++) l.data[i] = (base + i < sz) ? ram[base + i] : 0;
}

// (dcRead/dcWrite viven ahora en linea en cpu.hpp.) Cola de depuracion del store: punto de
// vigilancia (KESTREL_WATCHP) y write-through de diagnostico (KESTREL_DCWT). Fuera de linea
// porque solo corre con una de las dos armadas.
auto CPU::dcWriteDbg(u32 phys, u64 val, u32 size) -> void {
  u32 idx = (phys >> 4) & 0x1ff, off = phys & 0xf;
  DCacheLine& l = dcache[idx];
  if(wPhys != 0 && (phys & ~0xfu) == (wPhys & ~0xfu)) wNote((u32)val);
  if(g_dcWriteThrough)
    for(u32 i = 0; i < size; i++) if(phys + i < mem->rdram.size()) mem->rdram[phys + i] = l.data[off + i];
}

auto CPU::peekPhysCoherent(u32 phys) -> u8 {
  if(!mem) return 0;
  u32 idx  = (phys >> 4) & 0x1ff;
  u32 base = phys & ~0xfu;
  const DCacheLine& l = dcache[idx];
  if(l.dirty && l.tagv == (base | 1u)) return l.data[phys & 0xf];   // dirty line shadows RAM
  return phys < mem->rdram.size() ? mem->rdram[phys] : 0;
}

auto CPU::peekPhysCoherent(u32 phys, u32 size) -> u64 {
  u64 v = 0;
  for(u32 i = 0; i < size; i++) v = (v << 8) | peekPhysCoherent(phys + i);   // big-endian
  return v;
}

auto CPU::pokePhysCoherent(u32 phys, u32 size, u64 val) -> void {
  if(!mem || phys + size > mem->rdram.size()) return;
  dcWrite(phys, val, size);
}

auto CPU::icFill(u32 idx, u32 base) -> void {
  icMisses++;
  ramCpuBytes += 32;
  chargeIcMiss();   // idem, con su propio coste: la linea de I son 8 palabras
  ICacheLine& l = icache[idx];
  l.ptag = base; l.valid = true; l.seq = ++icSeq;
  if(base + 32 <= mem->rdram.size()) std::memcpy(l.data, &mem->rdram[base], 32);
  else for(u32 i = 0; i < 32; i++) l.data[i] = (base + i < mem->rdram.size()) ? mem->rdram[base + i] : 0;
}

auto CPU::icFetch(u32 phys) -> u32 {
  u32 idx  = (phys >> 5) & 0x1ff;
  u32 base = phys & ~0x1fu;
  ICacheLine& l = icache[idx];
  if(!l.valid || l.ptag != base) icFill(idx, base);
  u32 off = phys & 0x1c;
  u32 w; std::memcpy(&w, &l.data[off], 4);
  return __builtin_bswap32(w);   // la línea guarda bytes big-endian; el host es little-endian
}

// The CACHE instruction. op = the 5-bit rt field: bit0 selects cache (0=I,1=D),
// bits[4:2] the operation. Index ops address a line by its virtual index; Hit ops
// only act when the addressed line's tag matches. TagLo is COP0 reg 28 (PState in
// bits[7:6], PFN in bits[27:8]); D-cache valid PState = 3, I-cache valid PState = 2.
auto CPU::cacheOp(u32 op, u64 vaddr) -> void {
  u32 phys = (u32)vaddr & 0x1fff'ffff;
  bool dCache = op & 1;
  u32  fn     = (op >> 2) & 7;
  if(dCache) {
    u32 idx  = (phys >> 4) & 0x1ff;
    u32 base = phys & ~0xfu;
    DCacheLine& l = dcache[idx];
    switch(fn) {
      case 0: /*Index_Writeback_Invalidate*/ dcFlush(idx); l.tagv &= ~1u; l.dirty = 0; break;
      case 1: /*Index_Load_Tag*/ {
        u32 pstate = l.valid() ? 3u : 0u;
        cop0[28] = (pstate << 6) | ((l.ptag() >> 12) << 8);
      } break;
      case 2: /*Index_Store_Tag*/ {
        u32 pstate = ((u32)cop0[28] >> 6) & 3;
        l.dirty = 0;   // el bit de valida viaja DENTRO del tag (bit0), como el PState del HW
        l.tagv = ((((u32)cop0[28] >> 8) & 0x000f'ffff) << 12) | (pstate != 0 ? 1u : 0u);
      } break;
      case 3: /*Create_Dirty_Exclusive*/
        if(l.valid() && l.dirty && l.ptag() != base) dcFlush(idx);
        l.tagv = base | 1u; l.dirty = 1; break;
      case 4: /*Hit_Invalidate*/  if(l.tagv == (base | 1u)) { l.tagv &= ~1u; l.dirty = 0; } break;
      case 5: /*Hit_Writeback_Invalidate*/ if(l.tagv == (base | 1u)) { dcFlush(idx); l.tagv &= ~1u; } break;
      case 6: /*Hit_Writeback*/   if(l.tagv == (base | 1u)) dcFlush(idx); break;
      default: break;
    }
  } else {
    u32 idx  = (phys >> 5) & 0x1ff;
    u32 base = phys & ~0x1fu;
    ICacheLine& l = icache[idx];
    // Block-linking: invalidar la I-cache es la ÚNICA vía por la que el HW puede pasar a
    // ejecutar código nuevo bajo una dirección ya ejecutada (el RCP/DMA no espía las cachés,
    // así que el software está obligado a invalidar). Un bloque alcanzado por un salto enlazado
    // se salta la revalidación del driver, luego aquí hay que romper TODAS las cadenas; cada
    // bloque se re-enlaza en su próxima entrada validada por el driver.
    if(jitCache && (fn == 0 || fn == 2 || fn == 4)) jitCache->unlinkAll();
    switch(fn) {
      case 0: /*Index_Invalidate*/ l.valid = false; break;
      case 1: /*Index_Load_Tag*/ {
        u32 pstate = l.valid ? 2u : 0u;
        cop0[28] = (pstate << 6) | ((l.ptag >> 12) << 8);
      } break;
      case 2: /*Index_Store_Tag*/ {
        u32 pstate = ((u32)cop0[28] >> 6) & 3;
        l.valid = pstate != 0;
        l.ptag = (((u32)cop0[28] >> 8) & 0x000f'ffff) << 12;
      } break;
      case 4: /*Hit_Invalidate*/ if(l.valid && l.ptag == base) l.valid = false; break;
      case 5: /*Fill*/ icFill(idx, base); break;
      case 6: /*Hit_Writeback*/
        if(l.valid && l.ptag == base)
          for(u32 i = 0; i < 32; i++) if(l.ptag + i < mem->rdram.size()) mem->rdram[l.ptag + i] = l.data[i];
        break;
      default: break;
    }
  }
}

auto CPU::branch(bool taken, u64 target) -> void {
  justBranched = true;   // a delay slot follows whether or not the branch is taken
  if(taken) nextPc = target;
}

auto CPU::unimplemented(u32 op) -> void {
  lastUnimplemented = op;
  // An unrecognized encoding is, on the real VR4300, a Reserved Instruction
  // exception (ExcCode 10) — not a machine halt. Default to that so a test ROM
  // (n64-systemtest) that deliberately probes reserved encodings keeps running.
  // KESTREL_HALT_UNIMPL forces the old halt+dump for hunting genuine op gaps.
  if(!haltUnimpl) {
    if(unimplCount < 40) {
      std::fprintf(stderr, "[RI] op %08x (%s) @ pc=%08x -> Reserved Instruction\n",
                   op, disasm(op, curPc).c_str(), (u32)curPc);
      std::fflush(stderr);
    }
    unimplCount++;
    takeException(10);
    return;
  }
  char buf[96];
  std::snprintf(buf, sizeof buf, "unimplemented op %08x (%s) @ pc=%08x",
                op, disasm(op, curPc).c_str(), (u32)curPc);
  // Direct rdram dump around the faulting fetch — bypasses read/telemetry paths.
  if(mem) {
    u32 phys = (u32)curPc & 0x1fff'ffff;
    std::fprintf(stderr, "[halt] rdram bytes @0x%08x:", phys);
    for(int i = -8; i < 12; i++) {
      u32 a = phys + i;
      if(a < mem->rdram.size()) std::fprintf(stderr, " %02x", mem->rdram[a]);
    }
    std::fprintf(stderr, "\n[halt] fetch read32(curPc)=%08x  read32 via mem=%08x\n",
                 op, mem->read32((u32)curPc));
    std::fflush(stderr);
  }
  halt(buf);
}

// Cold, out-of-line trap/debug prologue. Kept OUT of step() so the hot
// fetch/decode/execute body stays compact for the host I-cache. Runs only when
// debugArmed. Returns true iff step() should return immediately (halt/redirect).
[[gnu::noinline, gnu::cold]] auto CPU::stepTraps() -> bool {
  if(mem) mem->storePc = curPc;   // for the debug store watchpoint
  { static const char* lo=std::getenv("KESTREL_PCLO"); static const char* hi=std::getenv("KESTREL_PCHI");
    if(lo && hi){ static u64 L=strtoull(lo,0,10), H=strtoull(hi,0,10);
      if(retired>=L && retired<=H){ memAbort=false; u32 w=read32(pc); memAbort=false;
        std::fprintf(stderr,"[pc] r=%llu pc=0x%08x %08x %s",(unsigned long long)retired,(u32)pc,w,disasm(w,pc).c_str());
        u32 t6=(u32)gpr[14]; memAbort=false; u32 m0=read32(t6),m1=read32(t6+4),m2=read32(t6+8),m3=read32(t6+12); memAbort=false;
        std::fprintf(stderr,"  at=%08x t6=%08x [t6]=%08x,%08x,%08x,%08x\n",(u32)gpr[1],t6,m0,m1,m2,m3); std::fflush(stderr);} } }
  if(trapWild) { u32 p = (u32)pc;
    if(p >= 0xa400'0000 && p < 0xa490'0000) {
      std::fprintf(stderr, "[wildpc] fetch @0x%08x (from prev) retired=%llu sp=%08x ra=%08x epc=%08x — trail:\n",
                   p, (unsigned long long)retired, (u32)gpr[29], (u32)gpr[31], (u32)cop0[C0_EPC]);
      for(int k=0;k<kJumpLog;k++){ int i=(jlogIdx+k)%kJumpLog;
        if(jlogSrc[i]||jlogDst[i]) std::fprintf(stderr,"  jl 0x%08x -> 0x%08x (op %08x)\n",(u32)jlogSrc[i],(u32)jlogDst[i],jlogOp[i]); }
      std::fflush(stderr); halt("wild pc in MMIO"); return true;
    } }
  if(excTail && ((u32)pc == 0x80000400 || (u32)pc == 0x80000460) && retired > 120000000) {
    std::fprintf(stderr, "[memcpy] ret=%llu pc=0x%08x a0=0x%016llx a1=0x%016llx a2=0x%016llx ra=0x%08x\n",
                 (unsigned long long)retired, (u32)pc,
                 (unsigned long long)gpr[4], (unsigned long long)gpr[5],
                 (unsigned long long)gpr[6], (u32)gpr[31]);
  }
  // Audio-off debug hook (KESTREL_AUDIOHOOK=<vaddr of n_alAudioFrame>): short-circuit
  // the audio-synth frame builder to an immediate empty return. The PD sequence player
  // otherwise wedges the single CPU inside a zero-delta MIDI event loop, starving the
  // gfx thread. Emulates `*cmdLen = 0; return cmdList;` — a0=cmdList, a1=&cmdLen, v0=ret.
  if(audioHook && (u32)pc == audioHook) {
    static u64 fires = 0;
    if(fires++ < 20) std::fprintf(stderr, "[audiohook] #%llu ra=0x%08x a0=0x%08x a1=0x%08x\n",
                                  (unsigned long long)fires, (u32)gpr[31], (u32)gpr[4], (u32)gpr[5]);
    if((u32)gpr[5]) mem->write32((u32)gpr[5], 0);   // *cmdLen = 0
    gpr[2] = gpr[4];                                 // v0 = cmdList (a0)
    pc = (u32)gpr[31]; nextPc = pc + 4;              // return to $ra
    return true;
  }
  // Trap the wild control-transfer: reaching buildHufts' inner store with the stack
  // pointer inside SP mem means execution jumped here off the rails.
  if(trapWild) {
    u32 spp = (u32)gpr[29] & 0x1fff'ffff;
    if((u32)pc >= 0x8000'6054 && (u32)pc <= 0x8000'65fc && spp >= 0x0400'0000 && spp < 0x0404'0000) {
      halted = true;
      char b[80]; std::snprintf(b, sizeof b, "wild-sp @0x%08x sp=0x%08x", (u32)pc, (u32)gpr[29]);
      haltReason = b;
      return true;
    }
  }
  static bool pcSample = std::getenv("KESTREL_PCSAMPLE") != nullptr;
  // Pagina que ademas se desglosa instruccion a instruccion, 0 = ninguna.
  static const u32 pcSampFineBase = [] {
    const char* e = std::getenv("KESTREL_PCSAMPLE");
    if(!e) return 0u;
    u32 v = (u32)std::strtoull(e, nullptr, 0);
    return v >= 0x8000'0000u ? (v & ~0xfffu) : 0u;
  }();
  if(pcSample) {
    u32 p = (u32)pc;
    if(p >= 0x8000'0000 && p < 0x8100'0000) { sampCount[(p >> 12) & (kSampPages-1)]++; sampTotal++; }
    if(pcSampFineBase && (p & ~0xfffu) == pcSampFineBase) { sampFine[(p >> 2) & (kSampFine-1)]++; sampFineTotal++; }
  }
  if(maxInsn && retired >= maxInsn) {
    if(pcSample) {
      std::fprintf(stderr, "[pcsample] total=%llu top pages:\n", (unsigned long long)sampTotal);
      for(int rank = 0; rank < 20; rank++) {
        int best = -1; u32 bv = 0;
        for(int i = 0; i < kSampPages; i++) if(sampCount[i] > bv) { bv = sampCount[i]; best = i; }
        if(best < 0 || bv == 0) break;
        std::fprintf(stderr, "  0x%08x  %8u  %.1f%%\n", 0x8000'0000u + (best << 12), bv, 100.0 * bv / sampTotal);
        sampCount[best] = 0;
      }
      if(pcSampFineBase && sampFineTotal) {
        std::fprintf(stderr, "[pcsample] fine 0x%08x total=%llu (%.1f%% of all):\n",
                     pcSampFineBase, (unsigned long long)sampFineTotal,
                     100.0 * (double)sampFineTotal / (double)sampTotal);
        for(int rank = 0; rank < 24; rank++) {
          int best = -1; u32 bv = 0;
          for(int i = 0; i < kSampFine; i++) if(sampFine[i] > bv) { bv = sampFine[i]; best = i; }
          if(best < 0 || bv == 0) break;
          u32 a = pcSampFineBase + ((u32)best << 2);
          u32 w = mem ? mem->read32(a & 0x1fff'ffff) : 0u;
          std::fprintf(stderr, "  0x%08x  %8u  %5.1f%%  op %08x\n",
                       a, bv, 100.0 * bv / sampTotal, w);
          sampFine[best] = 0;
        }
      }
      std::fflush(stderr);
    }
    // Huella del estado arquitectónico al llegar al tope. Comparada entre dos modos
    // (interp vs JIT, lockstep vs threaded) dice en una línea si ambos llegaron al MISMO
    // sitio, sin tener que diffear volcados enteros.
    {
      u64 h = 1469598103934665603ull;
      auto mix = [&](u64 v){ h ^= v; h *= 1099511628211ull; };
      for(int r = 0; r < 32; r++) mix((u64)gpr[r]);
      for(int r = 0; r < 32; r++) mix((u64)cop0[r]);
      for(int r = 0; r < 32; r++) mix(fpr[r]);
      mix(pc); mix(nextPc); mix(hi); mix(lo);
      std::fprintf(stderr, "[statehash] %016llx\n", (unsigned long long)h);
    }
    std::fprintf(stderr, "[maxinsn] cap %llu reached, pc=0x%08x sp=0x%08x ra=0x%08x\n",
                 (unsigned long long)maxInsn, (u32)pc, (u32)gpr[29], (u32)gpr[31]);
    { u32 st=(u32)cop0[C0_Status], ca=(u32)cop0[C0_Cause];
      std::fprintf(stderr, "[cp0] Status=%08x (IE=%u EXL=%u ERL=%u IM=%02x) Cause=%08x (IP=%02x Exc=%u) EPC=%08x badv=%08x\n",
        st, st&1, (st>>1)&1, (st>>2)&1, (st>>8)&0xff, ca, (ca>>8)&0xff, (ca>>2)&0x1f,
        (u32)cop0[C0_EPC], (u32)cop0[C0_BadVAddr]); }
    std::fprintf(stderr, "[exl] set@ret=%llu pc=0x%08x src=%s | cleared@ret=%llu | now=%llu\n",
                 (unsigned long long)exlSetRet, exlSetPc, exlSetSrc==1?"exc":(exlSetSrc==2?"mtc0":"-"),
                 (unsigned long long)exlClrRet, (unsigned long long)retired);
    if(mem)
      std::fprintf(stderr, "[vi] ctrl=%08x origin=%06x width=%u xscale=%08x yscale=%08x intr=%u\n",
                   mem->rcp.vi_ctrl, mem->rcp.vi_origin, mem->rcp.vi_width,
                   mem->rcp.vi_xscale, mem->rcp.vi_yscale, mem->rcp.vi_intr);
    if(mem)
      std::fprintf(stderr, "[rcp] mi_intr=%02x mi_mask=%02x sp_status=%08x sp_pc=%03x dpc_status=%08x rspRun=%u\n",
                   mem->rcp.mi_intr.load(), mem->rcp.mi_mask, mem->rcp.sp_status.load(), mem->rcp.sp_pc,
                   mem->rcp.dpc_status.load(), (unsigned)mem->rsp.running);
    if(mem)
      std::fprintf(stderr, "[det] rspCycles=%llu rdpGclk=%llu spArm=%u/%u tarde dpArm=%u/%u tarde"
                           " dpcRd=%u/%u rsp=%u open=%u busy=%u endv=%u await=%u dmaW=%llu lateMax=%llu waiv=%u/%u wv=%u/%u/%u ooo=%u(C%u/R%u) stale=%u(C%u/R%u) rdv=%llu/%u idle=%llu/%llu(sig%llu/drn%llu/room%llu) park=%u/%u/%u\n",
                   (unsigned long long)mem->rsp.cyclesRun.load(),
                   (unsigned long long)mem->rcp.rdpGclk.load(),
                   mem->spArms.load(), mem->spLate.load(),
                   mem->dpArms.load(), mem->dpLate.load(),
                   mem->dpcRdCur[0].load() + mem->dpcRdCur[1].load(),
                   mem->dpcRdSt[0].load() + mem->dpcRdSt[1].load(),
                   mem->dpcRdRsp.load(), mem->dpcRdOpen[0].load() + mem->dpcRdOpen[1].load(),
                   mem->dpcRdBusy[0].load() + mem->dpcRdBusy[1].load(),
                   mem->dpcRdEndV[0].load() + mem->dpcRdEndV[1].load(), mem->dpAwaits.load(),
                   (unsigned long long)mem->rspDmaRdpWaits.load(),
                   (unsigned long long)mem->dpLateMax.load(),
                   mem->dpBarWaives.load(), mem->spBarWaives.load(),
                  mem->dpWvBar.load(), mem->dpWvAwait.load(), mem->dpWvSched.load(),
                  mem->dpOoo.load(), mem->dpOooC.load(), mem->dpOooR.load(),
                  mem->dpStale.load(), mem->dpStaleC.load(), mem->dpStaleR.load(),
                  (unsigned long long)mem->dpRdv.load(), mem->dpRdvWaives.load(),
            (unsigned long long)mem->rsp.idleSkips.load(),
            (unsigned long long)mem->rsp.idleIters.load(),
            (unsigned long long)mem->rsp.idleNoSig,
            (unsigned long long)mem->rsp.idleNoDrain,
            (unsigned long long)mem->rsp.idleNoRoom,
            mem->rspParks.load(), mem->rspParkWv.load(), mem->rspParkMiss.load());
    // Antes de mirar el framebuffer hay que dejar quieto al RCP. En modo threaded el
    // hilo del RDP puede tener la lista de comandos todavia sin consumir cuando la CPU
    // llega al tope de instrucciones: el volcado saldria de un frame a medio pintar, o
    // directamente vacio. Con el dynarec la CPU quema el tope tan rapido que el RDP no
    // ha empezado siquiera, y con parallel-rdp (que ademas paga arranque de GPU) la
    // carrera se pierde SIEMPRE: la imagen salia entera a cero. rdpDrain() espera a que
    // la cola quede vacia, y el SYNC_FULL de dentro del trabajo ya espera a la GPU.
    if(mem && std::getenv("KESTREL_FBDUMP")) mem->rdpDrain();
    if(const char* fb = std::getenv("KESTREL_FBDUMP")) dumpFramebufferBmp(mem, fb);
    if(const char* md = std::getenv("KESTREL_MEMDUMP")) {   // dump real guest words direct from RDRAM (KSEG0/1 phys)
      const auto& ram = mem->rdram;
      auto rd = [&](u32 va)->u32{ u32 p=va&0x1fff'ffff; if((usize)p+3>=ram.size()) return 0;
        return ((u32)ram[p]<<24)|((u32)ram[p+1]<<16)|((u32)ram[p+2]<<8)|ram[p+3]; };
      // La RDRAM cruda ensena estado RANCIO: el guest escribe sus globales por KSEG0 y una
      // linea sucia del D-cache puede no haber bajado nunca (bss escrito una vez al arrancar).
      // Volcar tambien la vista coherente es la diferencia entre leer un puntero valido y leer
      // un cero que no existe: sin ella este volcado miente en justo los campos que importan.
      auto rdc = [&](u32 va)->u32{ u32 p=va&0x1fff'ffff; if((usize)p+3>=ram.size()) return 0;
        return ((u32)peekPhysCoherent(p)<<24)|((u32)peekPhysCoherent(p+1)<<16)
             | ((u32)peekPhysCoherent(p+2)<<8)|(u32)peekPhysCoherent(p+3); };
      const char* p = md;
      while(*p) {
        char* end=nullptr; unsigned long a = std::strtoul(p, &end, 0);
        if(end==p) break;
        u32 base = (u32)a & ~3u;
        std::fprintf(stderr, "[memdump @%08x]\n", base);
        for(int i=-4;i<12;i++){ u32 va=base+(u32)(i*4); u32 r=rd(va), c=rdc(va);
          if(r==c) std::fprintf(stderr,"  %08x: %08x\n", va, r);
          else     std::fprintf(stderr,"  %08x: %08x  (D$ %08x)\n", va, r, c); }
        p = end; while(*p==',' || *p==' ') p++;
      }
      std::fflush(stderr);
    }
    if(mem && envFlag("KESTREL_THREADS", true)) {   // walk libultra all-threads list (tlnext)
      // Lectura de depuracion: NUNCA por read32(). read32 lanza la excepcion de verdad
      // -- takeException ya ha escrito EPC/Cause/BadVAddr cuando volvemos a poner
      // memAbort=false -- asi que sondear una base que no existe, justo lo que hace el
      // bucle de las dos bases de abajo, corrompia el estado del guest y ensuciaba
      // [exchist] con AdEL fantasma que parecian un fallo del juego. Con probing=true
      // translate() no tiene efectos secundarios y devuelve ~0 si no traduce; la lectura
      // va por la D-cache para no ensenar RDRAM rancia detras de una linea sucia.
      auto rd = [&](u32 va) -> u32 {
        bool sv = probing; probing = true;
        u64 ph = translate(sext32(va), AccRead);
        probing = sv;
        if(ph == ~0ull) return 0;
        u32 p = (u32)ph;
        if((usize)p + 3 >= mem->rdram.size()) return 0;
        return ((u32)peekPhysCoherent(p)<<24)   | ((u32)peekPhysCoherent(p+1)<<16)
             | ((u32)peekPhysCoherent(p+2)<<8)  |  (u32)peekPhysCoherent(p+3);
      };
      const char* sn[]={"?","STOPPED","RUNNABLE","RUNNING","4","WAITING","6","7","8"};
      // .lib globals have KSEG0 VMA 0x8004xxxx but the game runs it TLB-mapped at 0x7000xxxx;
      // probe both bases and use whichever yields a sane tail pointer.
      for(u32 base : {0x8000'0000u, 0x7000'0000u}) {
        u32 run = rd(base + 0x463e4), tail = rd(base + 0x463f0);
        std::fprintf(stderr, "[threads base=%08x] __osRunningThread=%08x tail=%08x\n", base, run, tail);
        u32 t = tail; int g=0;
        while(t && (t>>28)>=7 && g++<16) {   // sane KSEG0/TLB pointer
          u32 id=rd(t+0x14), state=rd(t+0x10)>>16, pri=rd(t+0x04);
          u32 tpc=rd(t+0x118), tra=rd(t+0xfc), tsp=rd(t+0xec);
          std::fprintf(stderr, "  thr id=%u pri=%u state=%s(%u) pc=%08x ra=%08x sp=%08x %s\n",
            id, pri, state<9?sn[state]:"?", state, tpc, tra, tsp, t==run?"<-RUNNING":"");
          t = rd(t+0x0c);   // tlnext
        }
      }
      // El walker de arriba depende de la direccion de __osRunningThread, que cambia
      // con el juego. Este escaneo no depende de simbolos: una OSThread se reconoce por
      // su forma (prioridad 0-255, estado en {1,2,4,8}, id pequeno, punteros a RDRAM y
      // un PC guardado que apunta a codigo). Con eso se ve quien espera y en que cola,
      // que es lo unico que importa cuando la maquina se queda sin hilo ejecutable.
      if(std::getenv("KESTREL_THREADSCAN")) {
        const auto& ram = mem->rdram;
        // Coherente a proposito: libultra escribe las OSThread por KSEG0, asi que el
        // estado recien cambiado puede vivir todavia en una linea sucia del D-cache.
        // Leer la RDRAM cruda ensenaba el estado de hace varios cambios de contexto.
        auto p32 = [&](u32 p) -> u32 { if((usize)p + 3 >= ram.size()) return 0;
          return ((u32)peekPhysCoherent(p)<<24)   | ((u32)peekPhysCoherent(p+1)<<16)
               | ((u32)peekPhysCoherent(p+2)<<8)  |  (u32)peekPhysCoherent(p+3); };
        // Los punteros de una OSThread apuntan o bien a KSEG0 (0x8xxxxxxx) o bien al
        // segmento que el juego mapea por TLB (Perfect Dark corre su codigo y guarda sus
        // contextos en 0x70000000). Aceptar solo KSEG0 dejaba el escaneo ciego justo en
        // los juegos con TLB, que son los que mas falta hacen depurar.
        auto ptrOk = [&](u32 v) {
          if(v == 0) return true;
          if((v >> 24) == 0x80) return (v & 0x1fffffff) + 0x1b0 < ram.size();
          return (v >> 28) == 7;   // segmento mapeado por TLB
        };
        auto codeOk = [&](u32 v) { return !(v & 3) && ((v >> 24) == 0x80 || (v >> 28) == 7); };
        int found = 0;
        for(u32 p = 0; p + 0x200 < (u32)ram.size() && found < 24; p += 8) {
          u32 pri = p32(p + 0x04), st = p32(p + 0x10) >> 16, id = p32(p + 0x14);
          // id==0 es legitimo: los hilos que crea la propia libultra (vimgr, pimgr)
          // se registran con id 0, y son justo los que hay que ver cuando el juego se
          // queda esperando un retrace que no llega.
          if(pri > 255 || id > 64) continue;
          if(st != 1 && st != 2 && st != 4 && st != 8) continue;
          if(!ptrOk(p32(p + 0x00)) || !ptrOk(p32(p + 0x0c)) || !ptrOk(p32(p + 0x08))) continue;
          // El desplazamiento exacto de context.pc depende de como quede alineado el
          // contexto (u64 por registro), asi que se aceptan las dos posiciones vistas.
          u32 pcA = p32(p + 0x118), pcB = p32(p + 0x11c);
          u32 tpc = codeOk(pcA) ? pcA : pcB;
          if(!codeOk(tpc)) continue;
          const char* sn = st==1?"STOPPED":st==2?"RUNNABLE":st==4?"RUNNING":"WAITING";
          u32 q = p32(p + 0x08);
          std::fprintf(stderr, "  [scan] thr@%08x id=%u pri=%u %s pc=%08x ra=%08x sp=%08x mq=%08x",
                       0x80000000u + p, id, pri, sn, tpc, p32(p + 0x104), p32(p + 0xf4), q);
          if(q && (q >> 24) == 0x80) {          // OSMesgQueue: validCount +8, msgCount +0x10
            u32 qp = q & 0x1fffffff;
            std::fprintf(stderr, " (mensajes %d de %d)", (s32)p32(qp + 0x08), (s32)p32(qp + 0x10));
          }
          std::fprintf(stderr, "\n");
          found++;
        }
        std::fprintf(stderr, "  [scan] %d hilos plausibles\n", found);
      }
      std::fflush(stderr);
    }
    { std::fprintf(stderr, "[exchist]");
      const char* nm[] = {"Int","Mod","TLBL","TLBS","AdEL","AdES","IBE","DBE","Sys","Bp","RI","CpU","Ov","Tr","","FPE"};
      for(int i=0;i<16;i++) if(excCodeHist[i]) std::fprintf(stderr, " %s(%d)=%llu", nm[i], i, (unsigned long long)excCodeHist[i]);
      std::fprintf(stderr, "\n"); }
    for(int k = 0; k < kJumpLog; k++) {
      int i = (jlogIdx + k) % kJumpLog;
      if(jlogSrc[i] || jlogDst[i])
        std::fprintf(stderr, "  jl 0x%08x -> 0x%08x (op %08x)\n",
                     (u32)jlogSrc[i], (u32)jlogDst[i], jlogOp[i]);
    }
    std::fflush(stderr);
    halted = true; haltReason = "maxinsn cap";
    return true;
  }
  if(bpAddr && pc == bpAddr) {
    if(bpTrace) {
      std::fprintf(stderr, "[bp] @0x%08x #%llu sp=0x%08x a0=0x%08x a1=%lld a2=%lld ra=0x%08x\n",
                   (u32)pc, (unsigned long long)++bpHits, (u32)gpr[29], (u32)gpr[4],
                   (long long)(s64)gpr[5], (long long)(s64)gpr[6], (u32)gpr[31]);
      std::fflush(stderr);
    } else {
      std::fprintf(stderr, "[bp] HIT @0x%08x sp=0x%08x ra=0x%08x — jump trail:\n", (u32)pc, (u32)gpr[29], (u32)gpr[31]);
      for(int k = 0; k < kJumpLog; k++) {
        int i = (jlogIdx + k) % kJumpLog;
        if(jlogSrc[i] || jlogDst[i])
          std::fprintf(stderr, "  jl 0x%08x -> 0x%08x (op %08x)\n", (u32)jlogSrc[i], (u32)jlogDst[i], jlogOp[i]);
      }
      auto disHere = [&](u64 base, int n){
        std::fprintf(stderr, "  --- disasm @0x%08x ---\n", (u32)base);
        for(int j = 0; j < n; j++){ u64 a = base + j*4; memAbort=false; u32 w = read32(a);
          if(memAbort){ std::fprintf(stderr, "    0x%08x <unmapped>\n", (u32)a); break; }
          std::fprintf(stderr, "    0x%08x: %08x  %s\n", (u32)a, w, disasm(w, a).c_str()); } };
      disHere(pc - 0x40, 40);
      std::fprintf(stderr, "  a0=%08x a1=%08x a2=%08x v0=%08x v1=%08x k0=%08x k1=%08x cause=%08x epc=%08x badv=%08x (caller ra=0x%08x)\n",
                   (u32)gpr[4],(u32)gpr[5],(u32)gpr[6],(u32)gpr[2],(u32)gpr[3],(u32)gpr[26],(u32)gpr[27],
                   (u32)cop0[C0_Cause],(u32)cop0[C0_EPC],(u32)cop0[C0_BadVAddr],(u32)gpr[31]);
      disHere((gpr[31] & 0xffffffff) - 0x30, 16);
      // Boot block the IPL3/PIF leaves at physical 0x300 (osTvType..osAppNMIBuffer) plus
      // the words just below it, which the CIC-6105 IPL3 fills with its own image: a game
      // stuck early is nearly always spinning on one of these.
      { memAbort=false;
        for(u64 a=0xa00002e0; a<0xa0000320; a+=4){ u32 w=read32(a); std::fprintf(stderr, "    [phys %03x] = %08x\n", (u32)(a&0x1fffffff), w); } }
      memAbort = false;
      std::fflush(stderr);
      halted = true;
      char b[64]; std::snprintf(b, sizeof b, "breakpoint @0x%08x", (u32)pc);
      haltReason = b;
      return true;
    }
  }
  // buildHufts sanity: on entry (0x80006054) scan the code-length array b[a0]
  // for the first `a1` word entries. A value > 16 (BMAX) is an out-of-range code
  // length that overruns the local count[] on the stack -> runaway offset loop.
  if(huftTrap && (u32)pc == 0x8000'6054) {
    u32 b = (u32)gpr[4], n = (u32)gpr[5];
    if(n && n < 4096) {
      u32 mx = 0, mxi = 0;
      for(u32 i = 0; i < n; i++) { u32 v = read32(b + i * 4); if(v > mx) { mx = v; mxi = i; } }
      if(mx > 16) {
        std::fprintf(stderr, "[huft] bad code-length: b=0x%08x n=%u max=%u at idx=%u ra=0x%08x retired=%llu\n",
                     b, n, mx, mxi, (u32)gpr[31], (unsigned long long)retired);
        for(u32 i = 0; i < n && i < 24; i++) std::fprintf(stderr, "  b[%u]=%u\n", i, read32(b + i * 4));
        std::fflush(stderr);
        halted = true; haltReason = "huft bad code-length"; return true;
      }
    }
  }
  return false;
}

auto CPU::profEnable(bool on) -> void {
  if(on && profBuckets.empty()) profBuckets.assign(kProfBuckets, 0);
  if(on) { std::fill(profBuckets.begin(), profBuckets.end(), 0u); profTotal = 0; }
  profOn = on;
}

auto CPU::profClear() -> void {
  if(!profBuckets.empty()) std::fill(profBuckets.begin(), profBuckets.end(), 0u);
  profTotal = 0;
}

auto CPU::step() -> void {
  if(halted) return;
  curPc = pc;   // stable faulting-instruction address for EPC / unimplemented reports
  if(debugArmed && stepTraps()) return;
  // Interrupt sample on the instruction boundary. Inlined hot path: refresh the
  // Cause IP2/IP7 pin bits from the current MI state, and only branch to the cold
  // delivery path when an enabled interrupt is actually pending. Keeping this
  // inline (vs a per-instruction call into checkInterrupts) is worth ~5%.
  {
    u32 cause = (u32)cop0[C0_Cause];
    if(mem && (mem->rcp.mi_intr & mem->rcp.mi_mask)) cause |= (1u << 10); else cause &= ~(1u << 10);
    if(timerIntr)                                    cause |= (1u << 15); else cause &= ~(1u << 15);
    cop0[C0_Cause] = sext32(cause);
    u32 status = (u32)cop0[C0_Status];
    // ie=1, exl=0, erl=0  ⇔  (status & 0b111) == 0b001, plus any unmasked pending IP.
    if((status & 0x7) == 0x1 && (cause & status & 0xff00)) deliverInterrupt();
    if(g_intLog) {
      static u64 tick = 0;
      if((++tick & 0x3fffff) == 0) {   // ~every 4M steps
        u32 mi = mem ? (u32)mem->rcp.mi_intr : 0, mk = mem ? mem->rcp.mi_mask : 0;
        std::fprintf(stderr, "[intlog] pc=%08x mi_intr=%02x mi_mask=%02x pend=%u cause=%08x status=%08x\n",
                     (u32)pc, mi, mk, (mi & mk) ? 1u : 0u, (u32)cop0[C0_Cause], status);
        std::fflush(stderr);
      }
    }
  }
  if(halted) return;
  memAbort = false;
  // Instruction-fetch address error: PC must be word-aligned (AdEL, ExcCode 4).
  if(pc & 3) { setBadVAddr(pc); takeException(4); return; }
  // Fetch translation via the per-I-cache-line fast-path. `fpe` is the reverse-endian
  // adjusted physical fetch address; the bytes still come through icFetch() so the
  // I-cache / SMC snapshot semantics are unchanged — only translate()+cacheable() are
  // memoized while the PC stays in the same 32-byte line and xlatEpoch is unchanged.
  u64 vbase = pc & ~0x1full;   // línea de 32 B, clave 64-bit completa (ver fetchLineVBase)
  u32 fpe; bool fcacheable;
  if(g_fastFetch && vbase == fetchLineVBase && fetchLineEpoch == xlatEpoch) {
    fpe        = fetchLinePhys | (((u32)pc & 0x1cu) ^ fetchLineReXor);
    fcacheable = fetchLineCache;
  } else {
    // Slow path: translate through the TLB/segment rules (may vector a TLB miss).
    u64 fetchPhys = translate(pc, AccFetch);
    if(memAbort) return;
    fpe        = (u32)reXor(fetchPhys, 4);
    fcacheable = cacheable(pc);
    fetchLineVBase = vbase;
    fetchLinePhys  = fpe & ~0x1fu;                       // 32-byte line base (reXor bit2 stays in-line)
    fetchLineReXor = (u32)(fpe ^ (u32)fetchPhys) & 0x1cu;// captures the User+RE word-swap, else 0
    fetchLineCache = fcacheable;
    fetchLineEpoch = xlatEpoch;
  }
  if(profOn) {   // physical-PC hotpath sampler (MCP prof.*)
    u32 pp = fpe & 0x1fff'ffff;
    if(pp < (8u << 20)) { profBuckets[pp >> kProfShift]++; profTotal++; }
  }
  // pc/nextPc branch-delay model: fetch pc, advance, then default nextPc = pc+4.
  // While execute() runs, `pc` is the delay-slot address, so relative branches
  // use `pc + (simm<<2)` as their target base; branch()/jumps override nextPc.
  u32 op = !mem ? 0 : (fcacheable && fpe < mem->rdram.size()) ? icFetch(fpe) : mem->read32(fpe);
  pc = nextPc;
  nextPc = pc + 4;
  justBranched = false;
  if(ilkMode) ilkStep(op);
  execute(op);
  // Log taken control transfers (target differs from the sequential fall-through).
  // The jump ring-buffer + wild-jump traps are debug-only, so skip the whole block
  // on normal runs — it otherwise runs on every taken branch (~1 in 6 instructions).
  if(debugArmed && justBranched && nextPc != pc + 4 &&
     !(g_jlogNoSpin && (u32)nextPc == (u32)curPc)) {  // optionally skip b. self-loops (idle)
    jlogSrc[jlogIdx] = curPc; jlogDst[jlogIdx] = nextPc; jlogOp[jlogIdx] = op;
    jlogIdx = (jlogIdx + 1) % kJumpLog;
    // Trap wild control transfer into RCP MMIO space (0xA4000000..0xA4900000):
    // no code lives there, so a jump/return targeting it is a corrupted pointer.
    u32 d = (u32)nextPc;
    static int trapZero = std::getenv("KESTREL_TRAPZERO") ? 1 : 0;
    static int zdumped = 0;
    if(trapZero && d < 0x1000 && !zdumped) {
      zdumped = 1;
      std::fprintf(stderr, "[jump<0x1000] %08x -> %08x (op %08x) retired=%llu sp=%08x ra=%08x — trail:\n",
                   (u32)curPc, d, op, (unsigned long long)retired, (u32)gpr[29], (u32)gpr[31]);
      for(int k = 0; k < kJumpLog; k++){ int i=(jlogIdx+k)%kJumpLog;
        if(jlogSrc[i]||jlogDst[i]) std::fprintf(stderr, "  jl 0x%08x -> 0x%08x (op %08x)\n",(u32)jlogSrc[i],(u32)jlogDst[i],jlogOp[i]); }
      std::fflush(stderr);
    }
    if(trapWild && d >= 0xa400'0000 && d < 0xa490'0000) {
      std::fprintf(stderr, "[wildjump] %08x -> %08x (op %08x) retired=%llu sp=%08x ra=%08x — trail:\n",
                   (u32)curPc, d, op, (unsigned long long)retired, (u32)gpr[29], (u32)gpr[31]);
      for(int k = 0; k < kJumpLog; k++){ int i=(jlogIdx+k)%kJumpLog;
        if(jlogSrc[i]||jlogDst[i]) std::fprintf(stderr, "  jl 0x%08x -> 0x%08x (op %08x)\n",(u32)jlogSrc[i],(u32)jlogDst[i],jlogOp[i]); }
      std::fflush(stderr); halted=true; haltReason="wild jump into MMIO"; return;
    }
  }
  inDelay = justBranched;   // next instruction is a delay slot iff this was a branch
  gpr[0] = 0;               // r0 stays hardwired
  retired++;
  if(mem && mem->pendingTrap) {
    mem->pendingTrap = false;
    std::fprintf(stderr, "[trap] %s  retired=%llu sp=0x%08x ra=0x%08x\n",
                 mem->trapMsg.c_str(), (unsigned long long)retired, (u32)gpr[29], (u32)gpr[31]);
    for(int k = 0; k < kJumpLog; k++) {
      int i = (jlogIdx + k) % kJumpLog;
      if(jlogSrc[i] || jlogDst[i])
        std::fprintf(stderr, "  jl 0x%08x -> 0x%08x (op %08x)\n", (u32)jlogSrc[i], (u32)jlogDst[i], jlogOp[i]);
    }
    std::fflush(stderr);
    halted = true; haltReason = mem->trapMsg;
    return;
  }
  // Count runs at ~half CPU clock; Compare match latches the timer interrupt (IP7).
  // countTicks(1) es 1 con el factor de fabrica; con CPI<2 hay pasos que no mueven Count, y con
  // el coste de fallos de cache encendido puede saltar decenas de ticks de golpe -- por eso el
  // latch lo hace countAdd() por CRUCE (Compare dentro del tramo) y no por igualdad: con un
  // salto, Count pasaria POR ENCIMA de Compare y la interrupcion del timer se perderia.
  if(countAdd(countTicks(1))) timerIntr = true;
  // Random counts down each cycle, snapping back to 31 only when it exactly equals
  // Wired (HW model). With Wired>31 this makes Random sweep the full [0..63] range,
  // since it decrements past 0 to 63 before ever meeting Wired again.
  //
  // A write to Wired reloads Random=31, but the COP0 write hazard delays it by one
  // instruction: the mtc0 step itself and the very next step still decrement the old
  // value; the reload lands at the end of that next step (and replaces its decrement).
  if(randomReload && --randomReload == 0) {
    cop0[C0_Random] = 31;                 // delayed Wired reload arrives — no decrement this step
  } else {
    u32 r = (u32)cop0[C0_Random] & 0x3f;
    u32 w = (u32)cop0[C0_Wired] & 0x3f;
    cop0[C0_Random] = (r == w) ? 31 : ((r - 1) & 0x3f);
  }
}

// Full interrupt check (the inline hot path lives in step(); this remains the
// authoritative implementation for any out-of-loop caller).
auto CPU::checkInterrupts() -> void {
  u32 cause = (u32)cop0[C0_Cause];
  // External RCP interrupt → IP2 (Cause bit 10); timer → IP7 (Cause bit 15).
  if(mem && mem->interruptPending()) cause |= (1u << 10); else cause &= ~(1u << 10);
  if(timerIntr)                      cause |= (1u << 15); else cause &= ~(1u << 15);
  cop0[C0_Cause] = sext32(cause);

  u32 status = (u32)cop0[C0_Status];
  if((status & 0x7) == 0x1 && (cause & status & 0xff00)) deliverInterrupt();
}

// Cold delivery: only reached when an enabled interrupt is actually pending.
[[gnu::cold, gnu::noinline]] auto CPU::deliverInterrupt() -> void {
  static int noint = std::getenv("KESTREL_NOINT") ? 1 : 0;
  if(noint) return;   // debug: suppress interrupt delivery to isolate inflate corruption
  // KESTREL_INTLOG=2: TODAS, con instrucciones retiradas y MI, para comparar modos linea a linea.
  static int intlog = std::getenv("KESTREL_INTLOG") ? std::atoi(std::getenv("KESTREL_INTLOG")) : 0;
  if(intlog >= 2)
    std::fprintf(stderr, "[deliver] cnt=%08x cmp=%08x fr=%u ret=%llu pc=%08x cause=%08x mi=%02x\n", (u32)cop0[C0_Count], (u32)cop0[C0_Compare], countFrac, (unsigned long long)retired,
                 (u32)pc, (u32)cop0[C0_Cause], mem ? (u32)mem->rcp.mi_intr.load() : 0u);
  else if(intlog) {
    static u64 n = 0;
    if((++n & 0x3f) == 0)
      std::fprintf(stderr, "[deliver] #%llu at pc=%08x cause=%08x\n",
                   (unsigned long long)n, (u32)pc, (u32)cop0[C0_Cause]);
  }
  takeException(0 /*Int*/);
}

auto CPU::setBadVAddr(u64 vaddr) -> void {
  cop0[C0_BadVAddr] = vaddr;   // full 64-bit faulting address (32-bit mode addrs are already sign-extended)
  // Context (reg 4): BadVPN2 = vaddr[31:13] at bits [22:4]; PTEBase [63:23] untouched.
  u64 ctx = cop0[4] & ~0x7FFFF0ull;
  ctx |= ((vaddr >> 13) & 0x7FFFF) << 4;
  cop0[4] = ctx;
  // XContext (reg 20): BadVPN2 = vaddr[39:13] at [30:4], R = vaddr[63:62] at [33:31].
  u64 xctx = cop0[20] & ~0x1FFFFFFF0ull;
  xctx |= ((vaddr >> 13) & 0x7FFFFFF) << 4;
  xctx |= ((vaddr >> 62) & 0x3) << 31;
  cop0[20] = xctx;
  // EntryHi VPN2 [39:13] and R [63:62] also latch the faulting address (ASID kept).
  // The TLB fault paths overwrite this with their PageMask-masked fill right after,
  // so this only surfaces for AddressError, which the VR4300 updates the same way.
  cop0[C0_EntryHi] = (cop0[C0_EntryHi] & 0xFF) | (vaddr & 0xC00000FF'FFFFE000ull);
}

auto CPU::takeException(u32 excCode, bool tlbRefill, bool xtlb) -> void {
  bumpXlat();   // EXL/modo cambian → invalida el fetch fast-path del intérprete
  ilk = 0; dcbR = 0;   // la excepcion vacia la tuberia: no queda pareja
  u32 status = (u32)cop0[C0_Status];
  bool bd = inDelay;
  bool exl = status & 0x2;
  u64 epc = bd ? (curPc - 4) : curPc;   // branch instruction if we're in its delay slot
  if(!exl) cop0[C0_EPC] = sext32((u32)epc);   // EPC frozen while EXL already set (nested)
  // Tope de la traza. 80 basta para un arranque, pero para bisecar una divergencia tardia
  // (systemtest lleva miles de excepciones a proposito antes de llegar al test que falla)
  // hace falta el rastro entero: KESTREL_EXCTRACE=<n> lo sube.
  static const u64 kExcTraceMax = []{
    const char* v = std::getenv("KESTREL_EXCTRACE");
    u64 n = v ? std::strtoull(v, nullptr, 0) : 0;
    return n > 1 ? n : 80ull;
  }();
  if(excTrace && exceptions < kExcTraceMax) {
    std::fprintf(stderr, "[exc] #%llu code=%u epc=0x%016llx badv=0x%016llx status=0x%08x cause=0x%08x mi_intr=0x%02x mi_mask=0x%02x retired=%llu\n",
                 (unsigned long long)exceptions, excCode, (unsigned long long)epc, (unsigned long long)cop0[C0_BadVAddr], status, (u32)cop0[C0_Cause],
                 mem ? mem->rcp.mi_intr.load() : 0u, mem ? mem->rcp.mi_mask : 0,
                 (unsigned long long)retired);
    std::fflush(stderr);
  }

  // DIAGNOSTICO (KESTREL_EXCODD=<n>): un juego sano toma interrupciones, fallos de TLB y
  // syscalls a millones, pero NUNCA una instruccion reservada, un coprocesador no usable
  // ni un error de direccion. Cuando el guest se descarrila, la primera de esas dice
  // DONDE empezo -- mucho antes de que el sintoma (pc paseando por memoria en blanco)
  // sea visible. Se filtran los codigos normales y se imprime el contexto de llamada.
  static const u64 kOddMax = []{
    const char* v = std::getenv("KESTREL_EXCODD");
    u64 n = v ? std::strtoull(v, nullptr, 0) : 0;
    return (v && n < 1) ? 20ull : n;
  }();
  // TLBL(2)/TLBS(3) son pan de cada dia en un juego que use el TLB, pero si el invitado no
  // tiene NI UNA entrada valida instalada el TLB no puede acertar jamas: ese fallo no es
  // paginacion bajo demanda sino un puntero que se fue a kuseg por accidente, y entonces
  // interesa tanto como una instruccion reservada. Se comprueba el estado real del TLB, no
  // el nombre del juego.
  bool oddCode = (excCode > 3 && excCode != 8)
              || ((excCode == 2 || excCode == 3) && !tlbAnyValid());
  if(oddCode && kOddMax && oddExc < kOddMax) {
    oddExc++;
    std::fprintf(stderr, "[excodd] #%llu code=%u epc=0x%llx badv=0x%llx curPc=0x%llx bd=%u "
                         "ra=0x%llx sp=0x%llx status=0x%08x retired=%llu\n",
                 (unsigned long long)oddExc, excCode, (unsigned long long)epc,
                 (unsigned long long)cop0[C0_BadVAddr], (unsigned long long)curPc, (unsigned)bd,
                 (unsigned long long)gpr[31], (unsigned long long)gpr[29], status,
                 (unsigned long long)retired);
    // Que hay REALMENTE en la direccion que fallo: si son instrucciones plausibles el
    // fallo esta en la decodificacion; si son datos, el guest salto donde no debia o la
    // pagina ya no contiene ese codigo.
    u64 pp = tlbProbePhys(epc & ~0xFull);
    if(pp != ~0ull && mem && (usize)(u32)pp + 32 <= mem->rdram.size()) {
      std::fprintf(stderr, "[excodd]   phys=0x%08x:", (u32)pp);
      for(u32 k = 0; k < 8; k++) {
        u32 o = (u32)pp + 4 * k; const u8* q = mem->rdram.data() + o;
        std::fprintf(stderr, " %08x", ((u32)q[0]<<24)|((u32)q[1]<<16)|((u32)q[2]<<8)|q[3]);
      }
      std::fprintf(stderr, "\n");
    } else std::fprintf(stderr, "[excodd]   phys=SIN MAPEO\n");
    // El banco entero: el registro que contiene badv identifica la instruccion exacta que
    // fallo, y de donde salio el puntero podrido.
    for(int r = 1; r < 32; r++)
      std::fprintf(stderr, "   $%-2d=%016llx%s", r, (unsigned long long)gpr[r], (r % 4) ? "" : "\n");
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
  }

  u32 cause = (u32)cop0[C0_Cause];
  cause = (cause & ~0x7cu) | ((excCode & 0x1f) << 2);   // ExcCode (bits 2-6)
  cause &= ~0x3000'0000u;   // clear CE; the CU-unusable path re-sets it to the cop number
  if(!exl) { if(bd) cause |= 0x8000'0000u; else cause &= ~0x8000'0000u; }  // BD frozen if nested
  cop0[C0_Cause] = sext32(cause);

  if(excTail) {
    u32 i = excRingIdx % kExcRing;
    excRingCode[i] = excCode; excRingEpc[i] = epc;
    excRingBad[i] = cop0[C0_BadVAddr]; excRingRet[i] = retired;
    excRingIdx++;
  }
  excCodeHist[excCode & 0x1f]++;
  { static int faultN = std::getenv("KESTREL_FAULTTRACE") ? 0 : -1;
    u32 ec = excCode & 0x1f;
    if(faultN >= 0 && faultN < 24 && ec != 0 && ec != 11) {
      faultN++;
      const char* nm[] = {"Int","Mod","TLBL","TLBS","AdEL","AdES","IBE","DBE","Sys","Bp","RI","CpU","Ov","Tr","","FPE"};
      std::fprintf(stderr, "[fault] %s(%u) epc=0x%016llx badv=0x%016llx cause=0x%08x ra=0x%016llx sp=0x%016llx retired=%llu\n",
        ec<16?nm[ec]:"?", ec, (unsigned long long)epc, (unsigned long long)cop0[C0_BadVAddr],
        (u32)cop0[C0_Cause], (unsigned long long)gpr[31], (unsigned long long)gpr[29],
        (unsigned long long)retired);
      std::fflush(stderr);
      // Los primeros fallos son la ventana del bug: el banco entero identifica de que
      // registro salio el puntero podrido, y el codigo en EPC dice si el guest salto a
      // tierra de nadie o fallo un load concreto. Traducir por TLB: PD ejecuta en
      // 0x70000000 y enmascarar a 0x1fffffff daria ceros que parecen NOPs.
      if(faultN <= 4) {
        // translate() cortocircuita a 0 con memAbort puesto -- y aqui SIEMPRE lo esta,
        // porque el fallo que se vectoriza acaba de ponerlo. Sin limpiarlo la sonda
        // decia "phys 0" para cualquier VA y el volcado de codigo salia basura.
        bool ab = memAbort; memAbort = false;
        u64 pp = tlbProbePhys((epc - 48) & ~0x3ull);
        if(pp != ~0ull && mem && (usize)(u32)pp + 64 <= mem->rdram.size()) {
          std::fprintf(stderr, "[fault]   epc-48 phys=0x%08x:", (u32)pp);
          for(u32 k = 0; k < 16; k++) {
            const u8* q = mem->rdram.data() + (u32)pp + 4 * k;
            std::fprintf(stderr, k == 12 ? " >%08x" : " %08x", ((u32)q[0]<<24)|((u32)q[1]<<16)|((u32)q[2]<<8)|q[3]);
          }
          std::fprintf(stderr, "\n");
        } else std::fprintf(stderr, "[fault]   phys=SIN MAPEO\n");
        u64 bp = tlbProbePhys(cop0[C0_BadVAddr] & ~0x3ull);
        if(bp != ~0ull && mem && (usize)(u32)bp + 4 <= mem->rdram.size()) {
          const u8* q = mem->rdram.data() + (u32)bp;
          std::fprintf(stderr, "[fault]   badvPhys=0x%08x w=%08x\n", (u32)bp,
                       ((u32)q[0]<<24)|((u32)q[1]<<16)|((u32)q[2]<<8)|q[3]);
        }
        // Volcado de la TLB. Perfect Dark ejecuta desde kuseg 0x70000000, o sea que
        // TODO su codigo llega por una traduccion de la TLB: si la entrada que cubre el
        // EPC apunta a una pagina fisica equivocada, la CPU ejecuta datos y el fallo que
        // se ve aqui es una consecuencia, no la causa. Solo se imprimen las entradas
        // validas, y se marca con '*' la que cubre el EPC.
        {
          u64 evpn = (u64)(u32)epc;
          for(int t = 0; t < 32; t++) {
            const TlbEntry& e = tlb[t];
            if(!e.hi && !e.lo0 && !e.lo1) continue;
            u64 msk = e.mask | 0x1fffull;
            bool cov = ((evpn & ~msk) == (e.hi & ~msk & 0xffffffffull));
            std::fprintf(stderr, "[fault]   %ctlb%02d hi=%016llx lo0=%016llx lo1=%016llx mask=%08x g=%d\n",
                         cov ? '*' : ' ', t, (unsigned long long)e.hi,
                         (unsigned long long)e.lo0, (unsigned long long)e.lo1,
                         (u32)e.mask, (int)e.global);
          }
        }
        // Quien escribio por ultima vez cada bloque de 16 B del codigo que se estaba ejecutando.
      // Si el codigo esta pisado, esto dice el autor (KESTREL_WRTAG=1).
      if(wrtag::tag) {
        u32 base = (u32)pp & ~15u;
        for(int b = 0; b < 5 && pp != ~0ull; b++) {
          u32 blk = (base + (u32)b * 16) >> 4;
          if(blk >= wrtag::blocks) continue;
          std::fprintf(stderr, "[fault]   wrtag phys=0x%06x %-12s pc=0x%08x\n",
                       base + (u32)b * 16, wrtag::name(wrtag::tag[blk]), wrtag::pcOf[blk]);
        }
      }
      memAbort = ab;
        for(int r = 1; r < 32; r++)
          std::fprintf(stderr, "   $%-2d=%016llx%s", r, (unsigned long long)gpr[r], (r % 4) ? "" : "\n");
        std::fprintf(stderr, "\n");
        std::fflush(stderr);
      }
      // KESTREL_FAULTSTOP=1: un fallo del guest (deref nulo, direccion mala) es la ventana
      // exacta del bug; parar AQUI conserva el anillo de eventos del RCP intacto.
      static const bool faultStop = std::getenv("KESTREL_FAULTSTOP") != nullptr;
      if(faultStop) {
        if(mem) mem->evDump(std::getenv("KESTREL_EVDUMP")
                            ? (u32)std::strtoul(std::getenv("KESTREL_EVDUMP"), nullptr, 0) : 120);
        pcRingDump(120);
        halted = true;
      }
    } }
  if(fpTrace && excCode==15 && fpDbgN==0 && std::getenv("KESTREL_HDUMP")) {
    memAbort=false;
    std::fprintf(stderr, "== handler prologue @0x80000180 ==\n");
    for(u32 a=0x80170b68; a<0x80170f80; a+=4) {
      u32 w=read32(a); std::fprintf(stderr, "%08x: %08x %s\n", a, w, disasm(w,a).c_str());
    }
    memAbort=false; std::fflush(stderr); fpDbgN=1000000;  // one-shot
  }
  if(fpTrace && excCode==15 && (u32)gpr[31]==0x80010cb4 && std::getenv("KESTREL_DIS")) {
    static int done=0; if(done<12){ done++;
      memAbort=false;
      u32 skipDisc=read32(0x801acf64), skipVal=read32(0x801acf6c), seenDisc=read32(0x801acfa4);
      memAbort=false;
      std::fprintf(stderr,"[L666] epc=0x%08x insn=%08x f0=%016llx f2=%016llx fcr31=0x%08x skipDisc=%u skipVal=%u seenDisc=%u ret=%llu\n",
        (u32)epc, (u32)((mem?mem->read32((u32)epc&0x1fffffff):0)), (unsigned long long)fpr[0],(unsigned long long)fpr[2], fcr31, skipDisc, skipVal, seenDisc, (unsigned long long)retired);
      static int disC=0; if(!disC){ disC=1;
        std::fprintf(stderr,"  -- caller code [ra-0x60 .. ra+0x8] --\n");
        for(u32 a=0x8001037c; a<=0x800103d4; a+=4){ memAbort=false; u32 w=read32(a); memAbort=false;
          std::fprintf(stderr,"    0x%08x: %08x %s%s\n",a,w,disasm(w,a).c_str(), a==(u32)gpr[31]?"  <- ra":""); }
      }
      static int dis1=0; if(!dis1 && pcRingOn){ dis1=1;
        std::fprintf(stderr,"  -- last %d executed insns before leak (oldest first) --\n", kPcRing);
        u32 nn = pcRingIdx < (u32)kPcRing ? pcRingIdx : (u32)kPcRing;
        u32 st = pcRingIdx >= (u32)kPcRing ? pcRingIdx - kPcRing : 0;
        for(u32 k=0;k<nn;k++){ u32 i=(st+k)%kPcRing;
          std::fprintf(stderr,"    0x%08x: %08x %s\n",(u32)pcRing[i],opRing[i],disasm(opRing[i],pcRing[i]).c_str()); }
      }
      std::fflush(stderr);
    }
  }
  if(fpTrace && (excCode==15 || excCode==11)) {
    std::fprintf(stderr, "[E%u] epc=0x%08x bd=%d exl=%d fcr31=0x%08x ra=0x%08x sp=0x%08x ret=%llu\n",
                 excCode, (u32)epc, bd?1:0, exl?1:0, fcr31, (u32)gpr[31], (u32)gpr[29], (unsigned long long)retired);
    std::fflush(stderr);
    fpTraceEret = (excCode==15) ? 2 : 1;
  }
  if(excCode == 15 && fpDbg && std::getenv("KESTREL_UNARMED")) {
    memAbort=false; u32 disc = read32(0x801acf64); memAbort=false;
    if(disc == 0) {   // skip NOT armed => fire outside expect_exception window => leaks into SEEN
      memAbort=false; u32 fop = read32(epc); memAbort=false;
      static int lk=0;
      if(lk++ < 30) {
        std::fprintf(stderr, "[UNARMED] epc=0x%08x insn=%08x %s fcr31=0x%08x ret=%llu\n",
                     (u32)epc, fop, disasm(fop,epc).c_str(), fcr31, (unsigned long long)retired);
        std::fflush(stderr);
      }
    }
  }
  static const bool trapRi = std::getenv("KESTREL_TRAPRI") != nullptr;
  if((excCode == 10 || excCode == 2 || excCode == 3 || excCode == 11) && trapRi) {
    memAbort=false; u32 fop = read32(epc); memAbort=false;
    std::fprintf(stderr, "[exc %u] epc=0x%08x badv=0x%08x insn=%08x %s ra=0x%08x retired=%llu\n",
                 excCode, (u32)epc, (u32)cop0[C0_BadVAddr], fop, disasm(fop, epc).c_str(),
                 (u32)gpr[31], (unsigned long long)retired);
    std::fflush(stderr);
  }

  cop0[C0_Status] = sext32(status | 0x2);   // set EXL
  if(!exl) { exlSetRet = retired; exlSetPc = (u32)epc; exlSetSrc = 1; }
  // Vector selection. The TLB-refill special vector (offset 0x000, or XTLB 0x080) is
  // only used on the *first* miss (EXL=0); a nested miss uses the general 0x180 vector.
  bool bev = status & 0x0040'0000u;
  u64 base = bev ? 0xffff'ffff'bfc0'0200ull : 0xffff'ffff'8000'0000ull;
  u64 off  = 0x180;
  if(tlbRefill && !exl) off = xtlb ? 0x080 : 0x000;
  u64 vec = base + off;
  pc = vec;
  nextPc = vec + 4;
  inDelay = false;
  exceptions++;
}

// Current operating mode from Status: 0 kernel, 1 supervisor, 2 user.
// EXL or ERL force kernel mode regardless of KSU.
auto CPU::cpuMode() -> u32 {
  u32 s = (u32)cop0[C0_Status];
  if(s & 0x6) return 0;            // EXL(0x2) or ERL(0x4) -> kernel
  return (s >> 3) & 0x3;           // KSU
}

// 64-bit doubleword ops (ALU, loads/stores, LWU) are illegal in User/Supervisor
// mode unless that mode's 64-bit-addressing bit (UX/SX) is set; kernel mode always
// permits them. When illegal the VR4300 raises Reserved Instruction (ExcCode 10)
// before the op executes. Returns true if it raised — the decoder must then abort.
auto CPU::reserved64() -> bool {
  u32 mode = cpuMode();
  if(mode == 0) return false;                                     // kernel: always allowed
  u32 status = (u32)cop0[C0_Status];
  bool allowed = (mode == 2) ? (status & 0x20) : (status & 0x40); // UX (user) : SX (supervisor)
  if(allowed) return false;
  takeException(10);                                              // Reserved Instruction
  return true;
}

// Virtual->physical translation with VR4300 segment rules and 32-entry TLB.
// Handles both 32-bit (compatibility) and 64-bit addressing, selected per current
// mode by the Status UX/SX/KX bits. On fault, sets memAbort and vectors the
// appropriate exception (AdEL/AdES, TLB refill/invalid, TLB modified).
auto CPU::translate(u64 vaddr, Access acc) -> u64 {
  if(memAbort) return 0;
  u32 status = (u32)cop0[C0_Status];
  u32 mode = cpuMode();
  // 64-bit addressing active for the current mode?  UX(0x20)/SX(0x40)/KX(0x80).
  bool bit64 = (mode == 2) ? (status & 0x20) : (mode == 1) ? (status & 0x40) : (status & 0x80);

  if(!bit64) {
    // --- 32-bit (compatibility) addressing --------------------------------
    // A compatibility address must be sign-extended from bit 31 (bits 63:32 all
    // equal bit 31). A register value like 0x00000000_80xxxxxx (upper bits zeroed)
    // is not a valid 32-bit address -> AddressError, even though its low 32 bits
    // would otherwise land in kseg0.
    if((s64)vaddr != (s32)(u32)vaddr) goto ade;
    u32 va = (u32)vaddr;
    u32 seg = va >> 29;   // top 3 bits pick the segment
    bool mapped, direct = false;
    // seg: 0-3 = useg(kuseg), 4 = kseg0, 5 = kseg1, 6 = ksseg, 7 = kseg3.
    if(seg <= 3)      { mapped = true;  }                         // useg: all modes
    else if(seg == 6) { mapped = true;  if(mode == 2) goto ade; } // ksseg: kernel+super
    else if(seg == 7) { mapped = true;  if(mode != 0) goto ade; } // kseg3: kernel only
    else              { mapped = false; direct = true; if(mode != 0) goto ade; } // kseg0/1
    if(direct) return va & 0x1FFF'FFFF;
    if(mapped) return tlbLookup(vaddr, acc, false);
  } else {
    // --- 64-bit addressing ------------------------------------------------
    bool lo40 = (vaddr >> 40) == 0;                 // vaddr < 2^40 (xkuseg/xsuseg/xuseg)
    if(mode == 2) {                    // user: only xuseg
      if(lo40) return tlbLookup(vaddr, acc, true);
      goto ade;
    }
    if(mode == 1) {                    // supervisor
      if(lo40) return tlbLookup(vaddr, acc, true);                       // xsuseg
      if(vaddr >= 0x4000'0000'0000'0000ull && vaddr <= 0x4000'00FF'FFFF'FFFFull)
        return tlbLookup(vaddr, acc, true);                              // xsseg
      if(vaddr >= 0xFFFF'FFFF'C000'0000ull && vaddr <= 0xFFFF'FFFF'DFFF'FFFFull)
        return tlbLookup(vaddr, acc, true);                              // csseg
      goto ade;
    }
    // kernel (mode 0)
    if(lo40) return tlbLookup(vaddr, acc, true);                         // xkuseg
    if(vaddr >= 0x4000'0000'0000'0000ull && vaddr <= 0x4000'00FF'FFFF'FFFFull)
      return tlbLookup(vaddr, acc, true);                                // xksseg
    if((vaddr >> 62) == 0x2) {         // xkphys: 0x8000.. direct-mapped physical
      if(vaddr & 0x07FF'FFFF'0000'0000ull) goto ade;   // bits [58:32] must be zero
      xlatCacheable = (((vaddr >> 59) & 0x7) != 2);     // cache-attr in bits [61:59]; 2 = uncached
      return vaddr & 0xFFFF'FFFF;      // full 32-bit physical (LLAddr latches all of it)
    }
    if(vaddr >= 0xC000'0000'0000'0000ull && vaddr <= 0xC000'00FF'7FFF'FFFFull)
      return tlbLookup(vaddr, acc, true);                                // xkseg (top 2 GiB reserved)
    if(vaddr >= 0xFFFF'FFFF'8000'0000ull && vaddr <= 0xFFFF'FFFF'9FFF'FFFFull)
      return (u32)vaddr & 0x1FFF'FFFF;                                   // ckseg0 (cached)
    if(vaddr >= 0xFFFF'FFFF'A000'0000ull && vaddr <= 0xFFFF'FFFF'BFFF'FFFFull)
      return (u32)vaddr & 0x1FFF'FFFF;                                   // ckseg1 (uncached)
    if(vaddr >= 0xFFFF'FFFF'C000'0000ull && vaddr <= 0xFFFF'FFFF'DFFF'FFFFull)
      return tlbLookup(vaddr, acc, true);                                // cksseg
    if(vaddr >= 0xFFFF'FFFF'E000'0000ull)
      return tlbLookup(vaddr, acc, true);                                // ckseg3
    goto ade;
  }
ade:
  if(probing) return ~0ull;   // probe: no efectos, señala no-mapeado
  setBadVAddr(vaddr);
  memAbort = true;
  takeException(acc == AccWrite ? 5 : 4, false);   // AdES / AdEL
  return 0;
}

// Search the 32-entry TLB for `vaddr`; return the physical address, or vector a
// TLB Invalid / Modified / Refill exception (xtlb selects the XTLB refill vector).
auto CPU::tlbLookup(u64 vaddr, Access acc, bool xtlb) -> u64 {
  u32 va = (u32)vaddr;
  u32 asid = (u32)cop0[C0_EntryHi] & 0xFF;
  // EntryHi VPN2 fill on a TLB exception. In 64-bit addressing the field spans the
  // full VPN2 [39:13] plus the R region select [63:62]; in 32-bit mode only the low
  // VPN2 [31:13] is written (R reads 0).
  u64 ehFill = xtlb ? (vaddr & 0xC00000FF'FFFFE000ull) : (u64)(va & 0xFFFF'E000u);
  for(int i = 0; i < 32; i++) {
    TlbEntry& e = tlb[i];
    u32 m = (u32)e.mask & 0x01FF'E000;         // PageMask bits [24:13]
    u32 twoPage = (m | 0x1FFF) + 1;             // size of the VPN2 region (two pages)
    u32 cmpMask = ~(m | 0x1FFF) & 0xFFFF'E000;  // VPN2 bits [31:13] that must match
    if(((va ^ (u32)e.hi) & cmpMask) != 0) continue;
    // In 64-bit addressing the VPN2 comparison also spans the R region select
    // [63:62] and the high VPN2 bits [39:32]; a 32-bit-only match here is a miss.
    if(xtlb && (((vaddr ^ e.hi) & 0xC000'00FF'0000'0000ull) != 0)) continue;
    if(!e.global && ((u32)e.hi & 0xFF) != asid) continue;
    u32 sel = twoPage >> 1;                     // even/odd page selector bit
    u32 offMask = sel - 1;                       // in-page offset mask
    u64 lo = (va & sel) ? e.lo1 : e.lo0;
    if(!(lo & 0x2)) {                            // V (valid) clear -> TLB Invalid
      if(probing) return ~0ull;
      setBadVAddr(vaddr);
      cop0[C0_EntryHi] = ((u64)cop0[C0_EntryHi] & 0xFF) | ehFill;
      memAbort = true;
      takeException(acc == AccWrite ? 3 : 2, false);
      return 0;
    }
    if(acc == AccWrite && !(lo & 0x4)) {         // D (dirty) clear on store -> TLB Modified
      if(probing) return ~0ull;
      setBadVAddr(vaddr);
      cop0[C0_EntryHi] = ((u64)cop0[C0_EntryHi] & 0xFF) | ehFill;
      memAbort = true;
      takeException(1, false);
      return 0;
    }
    xlatCacheable = (((lo >> 3) & 0x7) != 2);   // C field: 2 = uncached, else cached
    u64 pfn = (lo >> 6) & 0xFF'FFFF;
    return ((pfn << 12) & ~(u64)offMask) | (va & offMask);
  }
  // No match -> TLB Refill (special/XTLB vector when EXL=0).
  if(probing) return ~0ull;
  setBadVAddr(vaddr);
  cop0[C0_EntryHi] = ((u64)cop0[C0_EntryHi] & 0xFF) | ehFill;
  memAbort = true;
  takeException(acc == AccWrite ? 3 : 2, true, xtlb);
  return 0;
}

// VR4300 normalizes PageMask when latched into a TLB entry: the MASK field [24:13]
// is six bit-pairs; only the higher bit of each pair counts, and when set it forces
// BOTH bits of that pair on (a lone lower bit is dropped).
static auto normPageMask(u64 pm) -> u64 {
  u32 f = (u32)((pm >> 13) & 0xFFF);   // 12-bit MASK field
  u32 out = 0;
  for(int k = 0; k < 6; k++) if(f & (1u << (2 * k + 1))) out |= (0x3u << (2 * k));
  return (u64)out << 13;
}

auto CPU::tlbAnyValid() const -> bool {
  for(const TlbEntry& e : tlb) if((e.lo0 | e.lo1) & 0x2) return true;
  return false;
}

auto CPU::tlbWrite(u32 index) -> void {
  index &= 0x3f;
  if(index >= 32) return;
  TlbEntry& e = tlb[index];
  e.mask = normPageMask(cop0[C0_PageMask]);
  // EntryHi VPN2 is masked by ~PageMask when written into the TLB.
  e.hi  = cop0[C0_EntryHi] & ~((u64)e.mask);
  e.lo0 = cop0[C0_EntryLo0] & 0x03FF'FFFE;   // drop G bit (bit0), keep PFN/C/D/V
  e.lo1 = cop0[C0_EntryLo1] & 0x03FF'FFFE;
  e.global = (cop0[C0_EntryLo0] & cop0[C0_EntryLo1] & 1) != 0;
  jitTlbValid = false;   // el mapeo cambió → invalida el softTLB de entrada del dynarec
  ++tlbGen;              // ...y los enlaces del dynarec que congelaron una traduccion TLB
  bumpXlat();            // ...y el fetch fast-path del intérprete
}

auto CPU::tlbRead(u32 index) -> void {
  index &= 0x3f;
  if(index >= 32) return;
  TlbEntry& e = tlb[index];
  cop0[C0_PageMask] = e.mask;
  cop0[C0_EntryHi]  = e.hi & ~((u64)e.mask);
  cop0[C0_EntryLo0] = (e.lo0 & ~1ull) | (e.global ? 1 : 0);
  cop0[C0_EntryLo1] = (e.lo1 & ~1ull) | (e.global ? 1 : 0);
}

auto CPU::tlbProbe() -> void {
  u32 asid = (u32)cop0[C0_EntryHi] & 0xFF;
  for(int i = 0; i < 32; i++) {
    TlbEntry& e = tlb[i];
    u32 cmpMask = ~((u32)e.mask | 0x1FFF) & 0xFFFF'E000;
    if(((u32)cop0[C0_EntryHi] ^ (u32)e.hi) & cmpMask) continue;
    // The R region select (EntryHi [63:62]) is part of the tag: a mismatch there is
    // a probe miss even when the VPN2 and ASID agree.
    if(((u64)cop0[C0_EntryHi] ^ e.hi) & 0xC000'0000'0000'0000ull) continue;
    if(!e.global && ((u32)e.hi & 0xFF) != asid) continue;
    cop0[C0_Index] = i;
    return;
  }
  cop0[C0_Index] = 0x8000'0000u;   // probe failure
}

auto CPU::wDump(u32 n) -> void {
  if(!wPhys) { std::fprintf(stderr, "[watch] apagado (KESTREL_WATCHP=<fis>)\n"); return; }
  u32 have = wIdx < 128u ? wIdx : 128u;
  if(n > have) n = have;
  std::fprintf(stderr, "[watch] ultimas %u escrituras CPU a la linea de %08x\n", n, wPhys);
  for(u32 k = 0; k < n; k++) {
    u32 i = (wIdx - n + k) % 128u;
    std::fprintf(stderr, "  ret=%llu pc=%08x val=%08x\n", (unsigned long long)wRing[i].ret, wRing[i].pc, wRing[i].val);
  }
  std::fflush(stderr);
}

auto CPU::pcRingDump(u32 n) -> void {
  if(!pcRingOn) { std::fprintf(stderr, "[pcring] apagado (KESTREL_PCRING=1)\n"); return; }
  u32 have = pcRingIdx < (u32)kPcRing ? pcRingIdx : (u32)kPcRing;
  if(n > have) n = have;
  std::fprintf(stderr, "[pcring] ultimas %u PCs distintas (antigua->reciente)\n", n);
  for(u32 k = 0; k < n; k++) {
    u32 i = (pcRingIdx - n + k) % kPcRing;
    std::fprintf(stderr, "  %08x: %08x  %s\n", (u32)pcRing[i], opRing[i], disasm(opRing[i], pcRing[i]).c_str());
  }
  std::fflush(stderr);
}

auto CPU::execute(u32 op) -> void {
  if(__builtin_expect(pcRingOn, 0)) {
   // Un bucle de espera (b .) llenaria el anillo entero y borraria justo lo que interesa:
   // las instrucciones que llevaron hasta el. Se colapsa el giro sobre la misma PC.
   if(pcRingIdx == 0 || pcRing[(pcRingIdx - 1) % kPcRing] != curPc) {
     pcRing[pcRingIdx % kPcRing] = curPc; opRing[pcRingIdx % kPcRing] = op; pcRingIdx++; }
  if(retired>=8195000 && retired<=8225000){
    if((u32)curPc==0x8001aa64){ u32 v0=(u32)gpr[2]; memAbort=false;
      u32 c=mem?mem->read32((v0+12)&0x1fffffff):0,d=mem?mem->read32((v0+16)&0x1fffffff):0; memAbort=false;
      std::fprintf(stderr,"[box] v0=0x%08x [v0+12]=0x%08x [v0+16]=0x%08x ret=%llu\n",v0,c,d,(unsigned long long)retired); }
    if((u32)curPc==0x8001aaac){ std::fprintf(stderr,"[preSD LDLR] at64=0x%016llx ret=%llu\n",(unsigned long long)gpr[1],(unsigned long long)retired); }
    if((u32)curPc==0x8001aab0){ std::fprintf(stderr,"[postSD at] at64=0x%016llx ret=%llu\n",(unsigned long long)gpr[1],(unsigned long long)retired); }
    if((u32)curPc==0x8001aabc){ u32 t6=(u32)gpr[14]; memAbort=false; u32 m0=mem?mem->read32((t6+0)&0x1fffffff):0,m4=mem?mem->read32((t6+4)&0x1fffffff):0; memAbort=false;
      std::fprintf(stderr,"[preJAL] t6=0x%08x [t6+0]=0x%08x [t6+4]=0x%08x at64=0x%016llx ret=%llu\n",t6,m0,m4,(unsigned long long)gpr[1],(unsigned long long)retired); }
  }
   if((u32)curPc==0x8001037c || (u32)curPc==0x800103cc) {   // disc-load / disc-branch probe
    if(retired>=8195000 && retired<=8225000 && (u32)curPc==0x8001037c){
      u32 t6=(u32)gpr[14]; memAbort=false;
      u32 m0=mem?mem->read32((t6+0)&0x1fffffff):0, m4=mem?mem->read32((t6+4)&0x1fffffff):0;
      u32 m8=mem?mem->read32((t6+8)&0x1fffffff):0, mc=mem?mem->read32((t6+12)&0x1fffffff):0; memAbort=false;
      std::fprintf(stderr,"[disc@%08x] t6=0x%08x expected[0..16]=%08x %08x %08x %08x ret=%llu\n",(u32)curPc,t6,m0,m4,m8,mc,(unsigned long long)retired);
    }
   }
  }  // end pcRingOn debug block
  // 64-bit doubleword ops + LWU raise Reserved Instruction in 32-bit non-kernel mode.
  // Bitmask test over the primary opcode replaces a second full switch-dispatch per
  // instruction: OP is 6-bit (op>>26), and the common case (kernel mode / 32-bit op)
  // is a single predictable not-taken branch instead of a jump-table indirect.
  {
    constexpr u64 k64Ops =
      (1ull<<0x18)|(1ull<<0x19)|(1ull<<0x1a)|(1ull<<0x1b)|(1ull<<0x27)|
      (1ull<<0x2c)|(1ull<<0x2d)|(1ull<<0x34)|(1ull<<0x37)|(1ull<<0x3c)|(1ull<<0x3f);
    if((k64Ops >> OP) & 1) { if(reserved64()) return; }
  }
  switch(OP) {
  case 0x00: special(op); break;
  case 0x01: regimm(op);  break;
  case 0x02: /*J*/   justBranched = true; nextPc = ((pc) & 0xffff'ffff'f000'0000ull) | (u64)(TARGET26 << 2); break;
  case 0x03: /*JAL*/ justBranched = true; set(31, sext32((u32)nextPc)); nextPc = (pc & 0xffff'ffff'f000'0000ull) | (u64)(TARGET26 << 2); break;
  case 0x04: /*BEQ*/  branch(gpr[RS] == gpr[RT], pc + (SIMM << 2)); break;
  case 0x05: /*BNE*/  branch(gpr[RS] != gpr[RT], pc + (SIMM << 2)); break;
  case 0x06: /*BLEZ*/ branch((s64)gpr[RS] <= 0, pc + (SIMM << 2)); break;
  case 0x07: /*BGTZ*/ branch((s64)gpr[RS] >  0, pc + (SIMM << 2)); break;
  case 0x08: /*ADDI*/ { s32 a=(s32)gpr[RS], b=(s32)SIMM, r=(s32)((u32)a+(u32)b); if(((a^r)&(b^r))<0){ takeException(12); break; } set(RT, sext32((u32)r)); break; }
  case 0x09: /*ADDIU*/set(RT, sext32((u32)(gpr[RS] + SIMM))); break;
  case 0x0a: /*SLTI*/ set(RT, (s64)gpr[RS] < (s64)SIMM ? 1 : 0); break;
  case 0x0b: /*SLTIU*/set(RT, gpr[RS] < (u64)SIMM ? 1 : 0); break;
  case 0x0c: /*ANDI*/ set(RT, gpr[RS] & IMM16); break;
  case 0x0d: /*ORI*/  set(RT, gpr[RS] | IMM16); break;
  case 0x0e: /*XORI*/ set(RT, gpr[RS] ^ IMM16); break;
  case 0x0f: /*LUI*/  set(RT, sext32((u32)(IMM16 << 16))); break;
  case 0x10: cop0op(op); break;
  case 0x11: cop1op(op); break;
  case 0x12: cop2op(op); break;
  case 0x14: /*BEQL*/  if(gpr[RS] == gpr[RT]) { justBranched = true; nextPc = pc + (SIMM<<2); } else { pc = nextPc; nextPc = pc + 4; } break;
  case 0x15: /*BNEL*/  if(gpr[RS] != gpr[RT]) { justBranched = true; nextPc = pc + (SIMM<<2); } else { pc = nextPc; nextPc = pc + 4; } break;
  case 0x16: /*BLEZL*/ if((s64)gpr[RS] <= 0)  { justBranched = true; nextPc = pc + (SIMM<<2); } else { pc = nextPc; nextPc = pc + 4; } break;
  case 0x17: /*BGTZL*/ if((s64)gpr[RS] >  0)  { justBranched = true; nextPc = pc + (SIMM<<2); } else { pc = nextPc; nextPc = pc + 4; } break;
  case 0x18: /*DADDI*/ { s64 a=(s64)gpr[RS], b=(s64)SIMM, r=(s64)((u64)a+(u64)b); if(((a^r)&(b^r))<0){ takeException(12); break; } set(RT, (u64)r); break; }
  case 0x19: /*DADDIU*/set(RT, gpr[RS] + SIMM); break;
  case 0x1a: /*LDL*/ { u64 a=gpr[RS]+SIMM; u32 s=(u32)(reOn()?(7-(a&7)):(a&7))*8; u64 d=read64(a&~7ull); u64 keep = s? (~0ull>>(64-s)):0ull; set(RT,(gpr[RT]&keep)|(d<<s)); break; }
  case 0x1b: /*LDR*/ { u64 a=gpr[RS]+SIMM; u32 s=(u32)(reOn()?(a&7):(7-(a&7)))*8; u64 d=read64(a&~7ull); u64 keep = s? (~0ull<<(64-s)):0ull; set(RT,(gpr[RT]&keep)|(d>>s)); break; }
  case 0x20: /*LB*/  set(RT, sext8 (read8 (gpr[RS]+SIMM))); break;
  case 0x21: /*LH*/  set(RT, sext16(read16(gpr[RS]+SIMM))); break;
  case 0x22: /*LWL*/ { u64 a=gpr[RS]+SIMM; u32 s=(u32)(reOn()?(3-(a&3)):(a&3))*8; u32 d=read32(a&~3ull); u32 cur=(u32)gpr[RT]; u32 keep = s? (~0u>>(32-s)):0u; set(RT, sext32((cur & keep) | (d<<s))); break; }
  case 0x23: /*LW*/  set(RT, sext32(read32(gpr[RS]+SIMM))); break;
  case 0x24: /*LBU*/ set(RT, (u64)read8 (gpr[RS]+SIMM)); break;
  case 0x25: /*LHU*/ set(RT, (u64)read16(gpr[RS]+SIMM)); break;
  case 0x26: /*LWR*/ { u64 a=gpr[RS]+SIMM; u32 s=(u32)(reOn()?(a&3):(3-(a&3)))*8; u32 d=read32(a&~3ull); u32 cur=(u32)gpr[RT]; u32 keep = s? (~0u<<(32-s)):0u; u32 lo=(cur & keep)|(d>>s);
      // LWR sign-extends into bits 63:32 only when it loaded the MSByte (s==0, the
      // full-word case). Otherwise the upper 32 bits of the register are untouched.
      set(RT, s ? ((gpr[RT] & 0xffff'ffff'0000'0000ull) | lo) : sext32(lo)); break; }
  case 0x27: /*LWU*/ set(RT, (u64)read32(gpr[RS]+SIMM)); break;
  case 0x28: /*SB*/ { u64 a=gpr[RS]+SIMM; u64 p=xlat(a,AccWrite); if(memAbort||!mem) break;
      if(__builtin_expect(watchArmed,0) && watchTrip(p,AccWrite)) break;
      if(storeRepeat((u32)p&0x1fff'ffff, gpr[RT], 1)) break;
      if(storeCart((u32)p&0x1fff'ffff, gpr[RT], 1)) break;
      if(mem->wordStoreQuirk((u32)p&0x1fff'ffff, gpr[RT], 1)) break;
      { u32 pe=(u32)reXor(p,1); if(cacheable(a)&&pe<mem->rdram.size()){ dcWrite(pe,gpr[RT],1); break; } mem->write8(pe,(u8)gpr[RT]); } } break;
  case 0x29: /*SH*/ { u64 a=gpr[RS]+SIMM; if(alignBad(a,2,AccWrite)) break; u64 p=xlat(a,AccWrite); if(memAbort||!mem) break;
      if(__builtin_expect(watchArmed,0) && watchTrip(p,AccWrite)) break;
      if(storeRepeat((u32)p&0x1fff'ffff, gpr[RT], 2)) break;
      if(storeCart((u32)p&0x1fff'ffff, gpr[RT], 2)) break;
      if(mem->wordStoreQuirk((u32)p&0x1fff'ffff, gpr[RT], 2)) break;
      { u32 pe=(u32)reXor(p,2); if(cacheable(a)&&pe<mem->rdram.size()){ dcWrite(pe,gpr[RT],2); break; } mem->write16(pe,(u16)gpr[RT]); } } break;
  case 0x2a: /*SWL*/ { u64 a=gpr[RS]+SIMM; xlat(a,AccWrite); if(memAbort) break;   // store: a TLB/addr fault here is TLBS/AdES, not the load flavor
      u32 s=(u32)(reOn()?(3-(a&3)):(a&3))*8; u32 d=readNoWatch32(a&~3ull); u32 m = s? (~0u>>s):~0u; write32(a&~3ull, (d & ~(m>>0)) | ((u32)gpr[RT]>>s)); break; }
  case 0x2b: /*SW*/ { u64 a=gpr[RS]+SIMM; if(alignBad(a,4,AccWrite)) break; u64 p=xlat(a,AccWrite); if(memAbort||!mem) break;
      if(__builtin_expect(watchArmed,0) && watchTrip(p,AccWrite)) break;
      if(storeRepeat((u32)p&0x1fff'ffff, gpr[RT], 4)) break;
      { u32 pe=(u32)reXor(p,4); if(cacheable(a)&&pe<mem->rdram.size()){ dcWrite(pe,gpr[RT],4); break; } mem->write32(pe, (u32)gpr[RT]); } } break;
  case 0x2c: /*SDL*/ { u64 a=gpr[RS]+SIMM; xlat(a,AccWrite); if(memAbort) break;
      u32 s=(u32)(reOn()?(7-(a&7)):(a&7))*8; u64 d=readNoWatch64(a&~7ull); u64 m = s? (~0ull>>s):~0ull; write64(a&~7ull, (d & ~(m>>0)) | (gpr[RT]>>s)); break; }
  case 0x2d: /*SDR*/ { u64 a=gpr[RS]+SIMM; xlat(a,AccWrite); if(memAbort) break;
      u32 s=(u32)(reOn()?(a&7):(7-(a&7)))*8; u64 d=readNoWatch64(a&~7ull); u64 m = s? (~0ull<<s):~0ull; write64(a&~7ull, (d & ~(m<<0)) | (gpr[RT]<<s)); break; }
  case 0x2e: /*SWR*/ { u64 a=gpr[RS]+SIMM; xlat(a,AccWrite); if(memAbort) break;
      u32 s=(u32)(reOn()?(a&3):(3-(a&3)))*8; u32 d=readNoWatch32(a&~3ull); u32 m = s? (~0u<<s):~0u; write32(a&~3ull, (d & ~(m<<0)) | ((u32)gpr[RT]<<s)); break; }
  case 0x2f: /*CACHE*/
    // Privileged: kernel mode always, else requires Status.CU0 (Coprocessor Unusable, CE=0).
    if(cpuMode() != 0 && !((u32)cop0[C0_Status] & 0x1000'0000u)) { takeException(11); break; }
    { u64 a=gpr[RS]+SIMM; translate(a,AccRead); if(memAbort) break; cacheOp((op >> 16) & 0x1f, a); } break;
  case 0x30: { /*LL*/  u64 va=gpr[RS]+SIMM; u64 pa=xlat(va,AccRead); if(memAbort||!mem) break;
              if(__builtin_expect(watchArmed,0) && watchTrip(pa,AccRead)) break;
              u32 pe=(u32)pa; u32 val = (cacheable(va)&&pe<mem->rdram.size()) ? (u32)dcRead(pe,4) : uncachedRead(pe, 4, [&]{ return mem->read32(pe); });
              set(RT, sext32(val)); cop0[17]=(u32)(pa>>4); llbit=true; } break;  // LLAddr = phys>>4
  case 0x31: /*LWC1*/ if(!((u32)cop0[C0_Status]&0x2000'0000u)){copUnusable(1);break;} fprSet32(RT, read32(gpr[RS]+SIMM)); break;
  case 0x34: { /*LLD*/ u64 va=gpr[RS]+SIMM; u64 pa=xlat(va,AccRead); if(memAbort||!mem) break;
              if(__builtin_expect(watchArmed,0) && watchTrip(pa,AccRead)) break;
              u32 pe=(u32)pa; u64 val = (cacheable(va)&&pe<mem->rdram.size()) ? dcRead(pe,8) : uncachedRead(pe, 8, [&]{ return mem->read64(pe); });
              set(RT, val); cop0[17]=(u32)(pa>>4); llbit=true; } break;  // LLAddr = phys>>4
  case 0x35: /*LDC1*/ if(!((u32)cop0[C0_Status]&0x2000'0000u)){copUnusable(1);break;} fprSet64(RT, read64(gpr[RS]+SIMM)); break;
  case 0x37: /*LD*/  set(RT, read64(gpr[RS]+SIMM)); break;
  case 0x38: /*SC*/  if(llbit) write32(gpr[RS]+SIMM, (u32)gpr[RT]); set(RT, llbit?1:0); break;
  case 0x39: /*SWC1*/ if(!((u32)cop0[C0_Status]&0x2000'0000u)){copUnusable(1);break;} write32(gpr[RS]+SIMM, fprGet32(RT)); break;
  case 0x3c: /*SCD*/ if(llbit) write64(gpr[RS]+SIMM, gpr[RT]); set(RT, llbit?1:0); break;
  case 0x3d: /*SDC1*/ if(!((u32)cop0[C0_Status]&0x2000'0000u)){copUnusable(1);break;} write64(gpr[RS]+SIMM, fprGet64(RT)); break;
  case 0x3f: /*SD*/ { u64 a=gpr[RS]+SIMM; if(alignBad(a,8,AccWrite)) break; u64 p=xlat(a,AccWrite); if(memAbort||!mem) break;
      if(__builtin_expect(watchArmed,0) && watchTrip(p,AccWrite)) break;
      if(storeRepeat((u32)p&0x1fff'ffff, gpr[RT], 8)) break;
      if(mem->wordStoreQuirk((u32)p&0x1fff'ffff, gpr[RT], 8)) break;
      { u32 pe=(u32)p; if(cacheable(a)&&pe<mem->rdram.size()){ dcWrite(pe,gpr[RT],8); break; } mem->write64(pe, gpr[RT]); } } break;
  default: unimplemented(op);
  }
}

// Dynarec Etapa 2b: ejecuta un load/store simple-alineado exactamente como el intérprete.
// Solo se llama desde bloques JIT (mem!=null garantizado por jitTryBlock). Devuelve 0 si
// la op faultaría (misalign/TLB/ADE): NO vectoriza (probe sin efectos vía `probing`), el
// bloque hace bail y el intérprete re-ejecuta esa op para levantar la excepción exacta.
// Ejecuta con el intérprete una op que el compilador de bloques no sabe emitir (COP1,
// loads/stores no alineados, LWC1/SWC1...), SIN cerrar el bloque. Es la palanca de longitud
// de bloque: cortar en cada op de coma flotante dejaba bloques de ~2 instrucciones en juegos
// con FPU, y el coste de entrada al bloque dominaba sobre el trabajo emulado.
//
// Convención de pc del intérprete durante execute(): curPc = dirección de la op, pc = op+4
// (la ranura de retardo), nextPc = op+8. El bloque mantiene `pc` fijo en su VA de entrada,
// así que la VA de esta op es pc+off.
//
// Postcondición al devolver 0: pc/nextPc describen exactamente por dónde seguir — el vector
// de excepción si execute() la levantó, o la op siguiente si simplemente hay que parar. El
// llamador sale del bloque con la bandera de "control ya escrito" contando esta op como
// retirada, así que nunca se re-ejecuta.
// ADD/SUB/MUL de FPU con camino rapido, emitidos por el JIT como CALL directo. Son dos
// tercios de todo lo que el bloque cede al interprete en SM64. El caso comun -- operandos
// normales o cero, resultado normal o cero, sin mas bandera IEEE que Inexact y sin el Enable
// de Inexact armado -- no puede desviar el control ni necesita nada del contexto de
// instruccion, asi que se resuelve aqui y se vuelve al bloque. CUALQUIER otra cosa (CU1=0,
// NaN, infinito o subnormal en una entrada o en el resultado, Overflow/Underflow/Invalid,
// un Enable que atrape) delega en jitInterpOp, que ejecuta la op entera por el interprete
// con el contexto exacto que la excepcion necesita. Delegar es seguro en cualquier punto de
// aqui: lo unico que se ha tocado hasta entonces es el MXCSR del anfitrion, que el
// interprete vuelve a fijar en su propio mx::prep.
template<u32 FN, u32 FMT>
auto CPU::jitCop1Alu(u32 op, u32 off) -> u8 {
  if(__builtin_expect(!((u32)cop0[C0_Status] & 0x2000'0000u), 0)) return jitInterpOp(op, off);
  u32 fs = (op >> 11) & 31, ft = (op >> 16) & 31, fd = (op >> 6) & 31;
  // Mismo emparejamiento de registro que el interprete: con FR=0 el campo fuente se alinea
  // a par, y solo el campo fuente (ft y fd conservan su indice crudo).
  if(!((u32)cop0[C0_Status] & (1u << 26))) fs &= ~1u;
  u32 rm = fcr31 & 3;
  u32 rc = (rm == 1) ? mx::RZ : (rm == 2) ? mx::RP : (rm == 3) ? mx::RM : mx::RN;
  if constexpr(FMT == 0x10) {
    u32 ab = (u32)fpr[fs], bb = (u32)fpr[ft];
    // Normal o cero: exponente ni todo-ceros-con-mantisa (subnormal) ni todo-unos (inf/NaN).
    auto plain = [](u32 x) { u32 e = x & 0x7f80'0000u;
                             return (e != 0 || (x & 0x007f'ffffu) == 0) && e != 0x7f80'0000u; };
    if(__builtin_expect(!(plain(ab) && plain(bb)), 0)) return jitInterpOp(op, off);
    float a = std::bit_cast<float>(ab), b = std::bit_cast<float>(bb);
    u32 rb, ex;
    if constexpr(FN == 2) {
      // El producto de dos `float` NORMALES O CERO es EXACTO en `double`: la mantisa cabe
      // (24+24 = 48 bits contra 53) y el exponente tambien (2^-252 .. 2^256 dentro del rango
      // normal del doble). Asi que multiplicar en doble y redondear UNA sola vez a simple da
      // exactamente el mismo bit que multiplicar en simple -- no hay doble redondeo posible --
      // y el Inexact sale de comparar: si el `float` redondeado vuelto a doble no es el
      // producto exacto, es que se perdio algo.
      //
      // Lo que se gana es no tocar el MXCSR. Antes cada operacion pagaba getcsr+setcsr+getcsr,
      // y el setcsr NO se ahorraba nunca porque la bandera PE es pegajosa: en cuanto habia una
      // operacion inexacta todas las siguientes escribian el registro para limpiarla, y
      // `ldmxcsr` serializa el pipeline del anfitrion.
      //
      // Suma y resta NO entran: el resultado exacto de a+b necesita hasta ~277 bits de rango
      // (exponentes muy separados), el doble tambien redondea y entonces la comparacion diria
      // "exacto" cuando en simple no lo es. Medido: pasa en ~1 de cada 3 pares al azar.
      // La division tampoco: a/b en doble y luego a simple si sufre doble redondeo.
      mx::prepRc(rc);
      double d = (double)a * (double)b;
      float r = (float)d;                      // unico redondeo, con el modo del guest puesto
      rb = std::bit_cast<u32>(r);
      // Fuera del rango normal del simple hay que rendirse: por arriba, con redondeo hacia cero
      // o al infinito contrario, un producto que desborda NO da inf sino el maximo finito -- que
      // a ojos de `plain` es un numero cualquiera -- y lleva Overflow ademas de Inexact; por
      // abajo pasa lo mismo con Underflow y el cero. Y de aqui solo sabemos deducir el Inexact.
      // Se mira el producto EXACTO en doble (que no desborda nunca) contra [2^-126, maxfloat].
      u64 db = std::bit_cast<u64>(d) & 0x7fff'ffff'ffff'ffffull;
      if(__builtin_expect(db != 0 && (db < 0x3810'0000'0000'0000ull
                                   || db > 0x47EF'FFFF'E000'0000ull), 0))
        return jitInterpOp(op, off);
      ex = ((double)r != d) ? mx::INEXACT : 0u;
    } else if constexpr(FN != 3) {
      // Suma y resta: el mismo truco que el producto, pero condicionado. a+b en doble es
      // EXACTO cuando los dos exponentes estan cerca -- el resultado exacto ocupa
      // (ea-eb)+25 bits de mantisa, asi que con |ea-eb| <= 28 cabe en los 53 del doble --
      // y entonces redondear UNA vez a simple da el bit del guest y el Inexact sale de
      // comparar. Un cero cuenta como "cerca" de cualquier cosa: sumar cero es exacto
      // siempre (y `plain` ya garantiza que un exponente nulo es un cero, no un subnormal).
      // Cuando los exponentes se separan mucho el doble TAMBIEN redondea, la comparacion
      // diria "exacto" cuando no lo es, y hay que volver al MXCSR. Es el caso raro: el
      // codigo de juego suma magnitudes parecidas.
      u32 ea = (ab >> 23) & 0xffu, eb = (bb >> 23) & 0xffu;
      s32 de = (s32)ea - (s32)eb;
      if(__builtin_expect(ea != 0 && eb != 0 && (de > 28 || de < -28), 0)) {
        mx::prep(rc);
        float r = (FN == 0) ? a + b : a - b;
        rb = std::bit_cast<u32>(r);
        ex = mx::flags();
      } else {
        mx::prepRc(rc);
        double d = (FN == 0) ? (double)a + (double)b : (double)a - (double)b;
        float r = (float)d;                    // unico redondeo, con el modo del guest puesto
        rb = std::bit_cast<u32>(r);
        // Mismo cierre que el producto: fuera del rango normal del simple no sabemos deducir
        // Overflow/Underflow (y con redondeo hacia cero un desbordamiento da el maximo finito,
        // que `plain` no distingue de un numero cualquiera). El doble no desborda nunca aqui.
        u64 db = std::bit_cast<u64>(d) & 0x7fff'ffff'ffff'ffffull;
        if(__builtin_expect(db != 0 && (db < 0x3810'0000'0000'0000ull
                                     || db > 0x47EF'FFFF'E000'0000ull), 0))
          return jitInterpOp(op, off);
        ex = ((double)r != d) ? mx::INEXACT : 0u;
      }
    } else {
      mx::prep(rc);
      float r = a / b;
      rb = std::bit_cast<u32>(r);
      ex = mx::flags();
    }
    if(__builtin_expect(!plain(rb) || (ex & ~mx::INEXACT), 0)) return jitInterpOp(op, off);
    u32 cause = (ex & mx::INEXACT) ? (1u << 12) : 0;
    if(__builtin_expect(cause != 0 && ((fcr31 >> 7) & 1) != 0, 0)) return jitInterpOp(op, off);
    fcr31 = (fcr31 & ~0x0003'F000u) | cause | (cause >> 10);   // Cause I + Flag I pegajosa
    fpr[fd] = (u64)rb;                                          // resultado de 32 bits: limpia el alto
    return 1;
  } else {
    u64 ab = fpr[fs], bb = fpr[ft];
    auto plain = [](u64 x) { u64 e = x & 0x7ff0'0000'0000'0000ull;
                             return (e != 0 || (x & 0x000f'ffff'ffff'ffffull) == 0)
                                    && e != 0x7ff0'0000'0000'0000ull; };
    if(__builtin_expect(!(plain(ab) && plain(bb)), 0)) return jitInterpOp(op, off);
    mx::prep(rc);
    double a = std::bit_cast<double>(ab), b = std::bit_cast<double>(bb);
    double r = (FN == 0) ? a + b : (FN == 1) ? a - b : (FN == 2) ? a * b : a / b;
    u64 rb = std::bit_cast<u64>(r);
    u32 ex = mx::flags();
    if(__builtin_expect(!plain(rb) || (ex & ~mx::INEXACT), 0)) return jitInterpOp(op, off);
    u32 cause = (ex & mx::INEXACT) ? (1u << 12) : 0;
    if(__builtin_expect(cause != 0 && ((fcr31 >> 7) & 1) != 0, 0)) return jitInterpOp(op, off);
    fcr31 = (fcr31 & ~0x0003'F000u) | cause | (cause >> 10);
    fpr[fd] = rb;
    return 1;
  }
}
// CTC1 es un tercio de todo lo que el JIT cedia al interprete en SM64: el compilador de SGI
// reprograma el modo de redondeo de FCSR antes de cada conversion a entero. El caso comun es
// una escritura que NO arma ninguna trampa; solo eso se atiende aqui.
auto CPU::jitCTC1w(u32 op, u32 off) -> u8 {
  if(__builtin_expect(!((u32)cop0[C0_Status] & 0x2000'0000u), 0)) return jitInterpOp(op, off);
  u32 rd = (op >> 11) & 31;
  if(rd != 31) return 1;                     // FCR0 es de solo lectura; el resto no existe
  u32 v = (u32)gpr[(op >> 16) & 31] & 0x0183'FFFFu;
  // Una CTC1 que deja armado E (bit 17) o un Cause con su Enable puesto dispara la excepcion
  // FP en el acto, y eso arrastra el apa�o de Cause.CE: al interprete.
  if(__builtin_expect(((v >> 17) & 1) || (((v >> 12) & 0x1f) & ((v >> 7) & 0x1f)), 0))
    return jitInterpOp(op, off);
  fcr31 = v;
  return 1;
}
extern "C" u8 kestrel_jitCTC1(void* c, u32 op, u32 off) {
  return reinterpret_cast<kestrel::CPU*>(c)->jitCTC1w(op, off); }

// Conversiones COP1 emitidas por el JIT como CALL directo. Tras CTC1 son lo siguiente en la
// cuenta de cesiones al interprete en SM64: el compilador de SGI convierte a entero cada vez
// que un float se usa como indice, coordenada o contador (CVT.W.S ~16% de las cesiones,
// TRUNC.W.S ~14%), y cambia de precision al pasar por rutinas de doble (CVT.D.S ~9%,
// CVT.S.D ~5%). Aqui se resuelve SOLO la conversion normal: operando normal-o-cero,
// resultado dentro de rango y ninguna trampa armada. Todo lo demas -- CU1=0, NaN, infinito,
// subnormal, magnitud fuera del entero de destino (que en el VR4300 es Unimplemented, no un
// saturado) y el Inexact con su Enable puesto -- delega en jitInterpOp, que ejecuta la op
// entera por el interprete con el contexto que la excepcion necesita. Delegar es seguro en
// cualquier punto: lo unico tocado hasta entonces es el MXCSR del anfitrion, que el
// interprete vuelve a fijar en su propio mx::prep.
// KIND: 0=CVT.W.S 1=TRUNC.W.S 2=CVT.W.D 3=TRUNC.W.D 4=CVT.D.S 5=CVT.S.D
template<u32 KIND>
auto CPU::jitCop1Cvt(u32 op, u32 off) -> u8 {
  if(__builtin_expect(!((u32)cop0[C0_Status] & 0x2000'0000u), 0)) return jitInterpOp(op, off);
  u32 fs = (op >> 11) & 31, fd = (op >> 6) & 31;
  // Mismo emparejamiento que el interprete: con FR=0 solo el campo FUENTE se alinea a par.
  if(!((u32)cop0[C0_Status] & (1u << 26))) fs &= ~1u;
  // "Plain" = normal o cero: ni subnormal ni infinito ni NaN. El VR4300 no implementa
  // subnormales y manda NaN/inf de una conversion por el camino de excepcion.
  auto plain32 = [](u32 x) { u32 e = x & 0x7f80'0000u;
                             return (e != 0 || (x & 0x007f'ffffu) == 0) && e != 0x7f80'0000u; };
  auto plain64 = [](u64 x) { u64 e = x & 0x7ff0'0000'0000'0000ull;
                             return (e != 0 || (x & 0x000f'ffff'ffff'ffffull) == 0)
                                    && e != 0x7ff0'0000'0000'0000ull; };
  constexpr bool srcD = (KIND == 2 || KIND == 3 || KIND == 5);
  double src;
  if constexpr(srcD) {
    u64 a = fpr[fs];
    if(__builtin_expect(!plain64(a), 0)) return jitInterpOp(op, off);
    src = std::bit_cast<double>(a);
  } else {
    u32 a = (u32)fpr[fs];
    if(__builtin_expect(!plain32(a), 0)) return jitInterpOp(op, off);
    src = (double)std::bit_cast<float>(a);
  }
  u32 rm = fcr31 & 3;
  u32 rc = (rm == 1) ? mx::RZ : (rm == 2) ? mx::RP : (rm == 3) ? mx::RM : mx::RN;
  if constexpr(KIND == 4) {                            // CVT.D.S: ensanchar es siempre exacto
    fcr31 &= ~0x0003'F000u;
    fpr[fd] = std::bit_cast<u64>(src);
    return 1;
  } else if constexpr(KIND == 5) {                     // CVT.S.D: puede redondear
    mx::prep(rc);
    float r = (float)src;
    u32 rb = std::bit_cast<u32>(r);
    u32 ex = mx::flags();
    // Un resultado no-plain es overflow (inf) o underflow (subnormal/cero): camino lento.
    if(__builtin_expect(!plain32(rb) || (ex & ~mx::INEXACT), 0)) return jitInterpOp(op, off);
    u32 cause = (ex & mx::INEXACT) ? (1u << 12) : 0;
    if(__builtin_expect(cause != 0 && ((fcr31 >> 7) & 1) != 0, 0)) return jitInterpOp(op, off);
    fcr31 = (fcr31 & ~0x0003'F000u) | cause | (cause >> 10);   // Cause I + Flag I pegajosa
    fpr[fd] = (u64)rb;
    return 1;
  } else {                                             // .W: redondeo a entero de 32 bits
    constexpr bool trunc = (KIND == 1 || KIND == 3);
    mx::prep(trunc ? mx::RZ : rc);
    double r = std::rint(src);
    u32 ex = mx::flags();
    // Fuera del rango de un entero con signo de 32 bits el VR4300 levanta Unimplemented:
    // no satura. Eso es excepcion, asi que va por el interprete.
    if(__builtin_expect(!(r >= -2147483648.0 && r < 2147483648.0), 0))
      return jitInterpOp(op, off);
    u32 cause = (ex & mx::INEXACT) ? (1u << 12) : 0;
    if(__builtin_expect(cause != 0 && ((fcr31 >> 7) & 1) != 0, 0)) return jitInterpOp(op, off);
    fcr31 = (fcr31 & ~0x0003'F000u) | cause | (cause >> 10);
    fpr[fd] = (u64)(u32)(s32)r;                        // resultado de 32 bits: limpia el alto
    return 1;
  }
}
// CVT.S.W / CVT.D.W: entero de 32 bits a coma flotante. Aqui la fuente no tiene casos raros --
// cualquier patron de 32 bits es un entero con signo valido -- asi que lo unico que puede
// levantarse es Inexact, y solo en .S (un doble representa cualquier int32 de forma exacta).
// Con el Enable de Inexact armado delega en el interprete, igual que el resto de conversiones.
template<u32 KIND>   // 0 = CVT.S.W, 1 = CVT.D.W
auto CPU::jitCop1CvtW(u32 op, u32 off) -> u8 {
  if(__builtin_expect(!((u32)cop0[C0_Status] & 0x2000'0000u), 0)) return jitInterpOp(op, off);
  u32 fs = (op >> 11) & 31, fd = (op >> 6) & 31;
  if(!((u32)cop0[C0_Status] & (1u << 26))) fs &= ~1u;
  s32 v = (s32)(u32)fpr[fs];
  if constexpr(KIND == 1) {                            // .D: exacto siempre, sin banderas
    fcr31 &= ~0x0003'F000u;
    fpr[fd] = std::bit_cast<u64>((double)v);
    return 1;
  } else {                                             // .S: 24 bits de mantisa -> puede redondear
    u32 rm = fcr31 & 3;
    u32 rc = (rm == 1) ? mx::RZ : (rm == 2) ? mx::RP : (rm == 3) ? mx::RM : mx::RN;
    mx::prep(rc);
    float r = (float)v;
    u32 ex = mx::flags();
    u32 cause = (ex & mx::INEXACT) ? (1u << 12) : 0;
    if(__builtin_expect(cause != 0 && ((fcr31 >> 7) & 1) != 0, 0)) return jitInterpOp(op, off);
    fcr31 = (fcr31 & ~0x0003'F000u) | cause | (cause >> 10);
    fpr[fd] = (u64)std::bit_cast<u32>(r);
    return 1;
  }
}
// Oraculo temporal (KESTREL_FPORACLE): el camino rapido calcula, se restaura el estado y el
// interprete ejecuta la MISMA op; destino y fcr31 tienen que salir identicos. Es el patron con
// que se validaron ADD/SUB/MUL de COP1 (ver docs/PERF-RCP-SYNC.md). Sin la variable de entorno
// no cuesta nada: una lectura de bool estatico. Las conversiones son idempotentes -- el
// resultado solo depende de fs y del modo de redondeo -- asi que repetirlas no altera nada mas.
template<u32 KIND>
auto CPU::jitCop1CvtChk(u32 op, u32 off) -> u8 {
  static const bool on = std::getenv("KESTREL_FPORACLE") != nullptr;
  auto fast1 = [&]() -> u8 { if constexpr(KIND >= 6) return jitCop1CvtW<KIND - 6>(op, off);
                             else                   return jitCop1Cvt<KIND>(op, off); };
  if(__builtin_expect(!on, 1)) return fast1();
  u32 fd = (op >> 6) & 31;
  u64 fdPre = fpr[fd]; u32 fcrPre = fcr31;
  static u64 seen = 0;
  if((++seen & 0xFFFFF) == 0)
    std::fprintf(stderr, "[fporacle] comprobadas %llu conversiones sin discrepancia\n", (unsigned long long)seen);

  u8 fast = fast1();
  u64 fdFast = fpr[fd]; u32 fcrFast = fcr31;
  fpr[fd] = fdPre; fcr31 = fcrPre;                    // rebobinar y repetir por el interprete
  u8 slow = jitInterpOp(op, off);
  if(fast != slow || fpr[fd] != fdFast || fcr31 != fcrFast) {
    static u32 n = 0;
    if(n++ < 40)
      std::fprintf(stderr, "[fporacle] kind=%u op=%08x  fast fd=%016llx fcr=%08x r=%u"
                           " | interp fd=%016llx fcr=%08x r=%u\n",
                   KIND, op, (unsigned long long)fdFast, fcrFast, fast,
                   (unsigned long long)fpr[fd], fcr31, slow);
  }
  return slow;
}

#define KC1C(name, kind) \
  extern "C" u8 name(void* c, u32 op, u32 off) { \
    auto* p = reinterpret_cast<kestrel::CPU*>(c); \
    kestrel::CPU::FpuCharge g(p, op); \
    return p->jitCop1CvtChk<kind>(op, off); }
KC1C(kestrel_jitCVTWS, 0) KC1C(kestrel_jitTRUNCWS, 1)
KC1C(kestrel_jitCVTWD, 2) KC1C(kestrel_jitTRUNCWD, 3)
KC1C(kestrel_jitCVTDS, 4) KC1C(kestrel_jitCVTSD, 5)
KC1C(kestrel_jitCVTSW, 6) KC1C(kestrel_jitCVTDW, 7)
#undef KC1C

// C.cond.fmt emitida por el JIT como CALL directo. Tras las conversiones es lo siguiente en la
// cuenta de cesiones (~15%): cada comparacion en coma flotante del codigo de SGI acaba en una
// C.LT/C.LE/C.EQ seguida de BC1T/BC1F. Aqui se resuelve el caso ORDENADO: si ningun operando
// es NaN, ninguno de los dieciseis predicados puede levantar Invalid, ni por la via del
// predicado senalizador ni por la rareza del VR4300 con el MSB de la mantisa invertido. Con un
// NaN en cualquiera de los dos (o CU1=0) delega en jitInterpOp. Los subnormales SI valen: el
// interprete tampoco los filtra en la comparacion -- solo lo hacen las ops computacionales -- y
// el `<`/`==` del anfitrion da exactamente el mismo orden. `fn` se queda dinamico: un unico
// trampolin por formato cubre los dieciseis predicados.
template<u32 FMT>
auto CPU::jitCop1Cmp(u32 op, u32 off) -> u8 {
  if(__builtin_expect(!((u32)cop0[C0_Status] & 0x2000'0000u), 0)) return jitInterpOp(op, off);
  u32 fs = (op >> 11) & 31, ft = (op >> 16) & 31, fn = op & 63;
  // Mismo emparejamiento que el interprete: con FR=0 solo el campo FUENTE se alinea a par.
  if(!((u32)cop0[C0_Status] & (1u << 26))) fs &= ~1u;
  bool less, equal;
  if constexpr(FMT == 0x10) {
    u32 ab = (u32)fpr[fs], bb = (u32)fpr[ft];
    if(__builtin_expect((ab & 0x7fff'ffffu) > 0x7f80'0000u
                     || (bb & 0x7fff'ffffu) > 0x7f80'0000u, 0)) return jitInterpOp(op, off);
    float a = std::bit_cast<float>(ab), b = std::bit_cast<float>(bb);
    less = a < b; equal = a == b;
  } else {
    u64 ab = fpr[fs], bb = fpr[ft];
    if(__builtin_expect((ab & 0x7fff'ffff'ffff'ffffull) > 0x7ff0'0000'0000'0000ull
                     || (bb & 0x7fff'ffff'ffff'ffffull) > 0x7ff0'0000'0000'0000ull, 0))
      return jitInterpOp(op, off);
    double a = std::bit_cast<double>(ab), b = std::bit_cast<double>(bb);
    less = a < b; equal = a == b;
  }
  // Ordenado: Cause sale limpia y no hay Flag pegajosa que acumular. El bit `unordered` del
  // predicado (fn & 1) nunca se cumple aqui, asi que no entra en la condicion.
  bool c = (((fn & 0x4) != 0) && less) || (((fn & 0x2) != 0) && equal);
  fcr31 &= ~0x0003'F000u;
  if(c) fcr31 |= (1u << 23); else fcr31 &= ~(1u << 23);
  return 1;
}
// Mismo oraculo que las conversiones (KESTREL_FPORACLE): la comparacion solo escribe fcr31 y
// depende unicamente de fs/ft, asi que rebobinando fcr31 se puede repetir por el interprete.
template<u32 FMT>
auto CPU::jitCop1CmpChk(u32 op, u32 off) -> u8 {
  static const bool on = std::getenv("KESTREL_FPORACLE") != nullptr;
  if(__builtin_expect(!on, 1)) return jitCop1Cmp<FMT>(op, off);
  u32 fcrPre = fcr31;
  static u64 seen = 0;
  if((++seen & 0xFFFFF) == 0)
    std::fprintf(stderr, "[fporacle] comprobadas %llu comparaciones sin discrepancia\n", (unsigned long long)seen);

  u8 fast = jitCop1Cmp<FMT>(op, off);
  u32 fcrFast = fcr31;
  fcr31 = fcrPre;                                     // rebobinar y repetir por el interprete
  u8 slow = jitInterpOp(op, off);
  if(fast != slow || fcr31 != fcrFast) {
    static u32 n = 0;
    if(n++ < 40)
      std::fprintf(stderr, "[fporacle] cmp fmt=%02x op=%08x  fast fcr=%08x r=%u"
                           " | interp fcr=%08x r=%u\n",
                   FMT, op, fcrFast, fast, fcr31, slow);
  }
  return slow;
}
extern "C" u8 kestrel_jitCMPS(void* c, u32 op, u32 off) {
  auto* p = reinterpret_cast<kestrel::CPU*>(c);
  kestrel::CPU::FpuCharge g(p, op);
  return p->jitCop1CmpChk<0x10>(op, off); }
extern "C" u8 kestrel_jitCMPD(void* c, u32 op, u32 off) {
  auto* p = reinterpret_cast<kestrel::CPU*>(c);
  kestrel::CPU::FpuCharge g(p, op);
  return p->jitCop1CmpChk<0x11>(op, off); }

// Oraculo diferencial (KESTREL_FPORACLE), igual que conversiones y comparaciones: el camino
// rapido calcula, se rebobina fd y fcr31, y el interprete ejecuta la MISMA op. La aritmetica
// es idempotente -- el resultado solo depende de fs, ft y el modo de redondeo, y fd es lo
// unico que escribe, asi que rebobinarlo basta aunque fd aliasee a fs o a ft.
template<u32 FN, u32 FMT>
auto CPU::jitCop1AluChk(u32 op, u32 off) -> u8 {
  static const bool on = std::getenv("KESTREL_FPORACLE") != nullptr;
  if(__builtin_expect(!on, 1)) return jitCop1Alu<FN, FMT>(op, off);
  u32 fd = (op >> 6) & 31;
  u64 fdPre = fpr[fd]; u32 fcrPre = fcr31;
  static u64 seen = 0;
  if((++seen & 0xFFFFF) == 0)
    std::fprintf(stderr, "[fporacle] comprobadas %llu operaciones aritmeticas sin discrepancia\n",
                 (unsigned long long)seen);

  u8 fast = jitCop1Alu<FN, FMT>(op, off);
  u64 fdFast = fpr[fd]; u32 fcrFast = fcr31;
  fpr[fd] = fdPre; fcr31 = fcrPre;                    // rebobinar y repetir por el interprete
  u8 slow = jitInterpOp(op, off);
  if(fast != slow || fpr[fd] != fdFast || fcr31 != fcrFast) {
    static u32 n = 0;
    if(n++ < 40)
      std::fprintf(stderr, "[fporacle] alu fn=%u fmt=%02x op=%08x  fast fd=%016llx fcr=%08x r=%u"
                           " | interp fd=%016llx fcr=%08x r=%u\n",
                   FN, FMT, op, (unsigned long long)fdFast, fcrFast, fast,
                   (unsigned long long)fpr[fd], fcr31, slow);
  }
  return slow;
}

#define KC1A(name, fn, fmt) \
  extern "C" u8 name(void* c, u32 op, u32 off) { \
    auto* p = reinterpret_cast<kestrel::CPU*>(c); \
    kestrel::CPU::FpuCharge g(p, op); \
    return p->jitCop1AluChk<fn, fmt>(op, off); }
KC1A(kestrel_jitADDS, 0, 0x10) KC1A(kestrel_jitSUBS, 1, 0x10) KC1A(kestrel_jitMULS, 2, 0x10)
KC1A(kestrel_jitADDD, 0, 0x11) KC1A(kestrel_jitSUBD, 1, 0x11) KC1A(kestrel_jitMULD, 2, 0x11)
KC1A(kestrel_jitDIVS, 3, 0x10) KC1A(kestrel_jitDIVD, 3, 0x11)
#undef KC1A

// Movimientos COP1 (MFC1/DMFC1/CFC1/MTC1/DMTC1) emitidos por el JIT como CALL directo.
// Son una cuarta parte de todo lo que el bloque cede al interprete en SM64 y no pueden
// desviar el control: mueven 32/64 bits entre un gpr y el banco FPU y nada mas. El camino
// normal se salta entero el montaje de contexto de jitInterpOp (guardar pc/nextPc/curPc,
// fabricar la VA de la op, y las seis comprobaciones de desviacion al volver) y el switch
// de cop1op. El unico caso que SI puede desviar -- CU1=0, que vectoriza Coprocessor
// Unusable -- delega en jitInterpOp, que monta el contexto exacto que necesita la excepcion.
template<u32 RSc>
auto CPU::jitCop1Move(u32 op, u32 off) -> u8 {
  if(__builtin_expect(!((u32)cop0[C0_Status] & 0x2000'0000u), 0)) return jitInterpOp(op, off);
  u32 rt = (op >> 16) & 31, rd = (op >> 11) & 31;
  if      constexpr(RSc == 0) set(rt, sext32(fprGet32(rd)));                       // MFC1
  else if constexpr(RSc == 1) set(rt, fprGet64(rd));                               // DMFC1
  else if constexpr(RSc == 2) set(rt, sext32(rd == 31 ? fcr31 : rd == 0 ? fcr0 : 0));  // CFC1
  else if constexpr(RSc == 4) fprSet32(rd, (u32)gpr[rt]);                          // MTC1
  else                        fprSet64(rd, gpr[rt]);                               // DMTC1
  return 1;
}
#define KC1(name, rs)   extern "C" u8 name(void* c, u32 op, u32 off) {     return reinterpret_cast<kestrel::CPU*>(c)->jitCop1Move<rs>(op, off); }
KC1(kestrel_jitMFC1, 0) KC1(kestrel_jitDMFC1, 1) KC1(kestrel_jitCFC1, 2)
KC1(kestrel_jitMTC1, 4) KC1(kestrel_jitDMTC1, 5)
#undef KC1

// Ranura de retardo. Mismo interprete, pero con inDelay puesto para que takeException
// congele EPC = direccion del SALTO y Cause.BD = 1, como el VR4300. Sin esto un LWC1 o un
// ADD.S en la ranura que faltara dejaria EPC en la ranura y el ERET del kernel volveria
// SALTANDOSE el salto.
extern "C" u8 kestrel_jitInterpDelay(void* cpu, u32 op, u32 off) {
  auto* c = reinterpret_cast<kestrel::CPU*>(cpu);
  c->jitDelaySlot = true;
  u8 r = c->jitInterpOp(op, off);
  c->jitDelaySlot = false;
  return r;
}

// CACHE dentro del bloque. Las de D-cache (bit0 del selector rt) no tocan el codigo: el
// bloque sigue. Las de I-cache SI: el bloque valido sus palabras contra las lineas de
// I-cache al entrar, y tras invalidarlas el HW volveria a buscar en RDRAM, que puede tener
// otro codigo -- esa es justo la razon de que el software invalide. Por eso una op de
// I-cache CIERRA el bloque y el driver revalida en la siguiente entrada. Las masivas son
// las de D-cache: osWritebackDCache / osInvalDCache barren la cache en bucles de tres ops,
// y con CACHE fuera del conjunto compilable esos bucles caian enteros al interprete.
extern "C" u8 kestrel_jitCACHE(void* cpu, u32 op, u32 off) {
  auto* c = reinterpret_cast<kestrel::CPU*>(cpu);
  u8 r = c->jitInterpOp(op, off);
  if(!r) return 0;                 // vectorizo (TLB sobre la direccion): estado ya correcto
  if((op >> 16) & 1) return 1;     // D-cache: sin efecto sobre el codigo, el bloque sigue
  u64 va = c->pc + off;            // I-cache: jitInterpOp restauro pc a la entrada del bloque
  c->pc = va + 4; c->nextPc = va + 8;
  c->inDelay = false; c->justBranched = false;
  return 0;                        // salida de control, con la op contada como retirada
}

auto CPU::jitInterpOp(u32 op, u32 off) -> u8 {
  u64 va      = pc + off;
  u64 savedPc = pc, savedNext = nextPc, savedCur = curPc;
  // Monta el contexto EXACTO que vería el intérprete en step() para esta VA: curPc = la op,
  // pc = la ranura de retardo (base de los branches relativos), nextPc = pc+4. Un bloque JIT
  // nunca entra en ranura de retardo (jitTryBlock declina si inDelay), así que inDelay=false.
  curPc  = va;
  pc     = va + 4;
  nextPc = va + 8;
  memAbort     = false;
  justBranched = false;
  inDelay      = jitDelaySlot;   // ranura de retardo: EPC = el salto, Cause.BD = 1
  // Despacho DIRECTO de COP1. La tabla de saltos de execute() es un indirecto de ~60
  // destinos, y aqui cae casi siempre la misma familia: el fallback en bloque del JIT existe
  // sobre todo para FPU. Es exactamente la rama que execute() elegiria (0x11 no esta en la
  // mascara de ops de 64 bits, asi que no hay comprobacion previa que saltarse), solo sin
  // pagar el indirecto. Con el anillo de depuracion armado se vuelve a execute() para no
  // perder la traza de instrucciones.
  // Mismo reloj que en el helper de memoria (emitMemOp): las ops previas del bloque aun no
  // estan en retired, y una LWL/SWL/LWC1... sobre MMIO fecharia su evento antes de tiempo.
  if((op >> 26) == 0x11 && !pcRingOn) cop1op(op);
  else { jitPending += off >> 2; execute(op); jitPending -= off >> 2; }
  gpr[0] = 0;                 // r0 cableado: el bloque puede leerlo como fuente después
  inDelay = justBranched;     // misma actualización que hace step() tras execute()
  // Cualquier desviación del avance secuencial (excepción vectorizada, halt, salto) significa
  // que el estado de control YA es el punto de reanudación correcto → el bloque sale con la
  // bandera de control y el driver no vuelve a tocar pc.
  if(halted || memAbort || justBranched || pc != va + 4 || nextPc != va + 8) return 0;
  pc = savedPc; nextPc = savedNext; curPc = savedCur;
  return 1;
}

auto CPU::jitMem(u32 op) -> u8 {
  if(!mem) return 0;
  u32 OPc = op >> 26 & 0x3f;
  u64 a = gpr[RS] + SIMM;
  u32 sz; bool store;
  switch(OPc) {
    case 0x20: case 0x24: sz = 1; store = false; break;   // LB / LBU
    case 0x21: case 0x25: sz = 2; store = false; break;   // LH / LHU
    case 0x23: case 0x27: sz = 4; store = false; break;   // LW / LWU
    case 0x37:            sz = 8; store = false; break;   // LD
    case 0x28:            sz = 1; store = true;  break;   // SB
    case 0x29:            sz = 2; store = true;  break;   // SH
    case 0x2b:            sz = 4; store = true;  break;   // SW
    case 0x3f:            sz = 8; store = true;  break;   // SD
    default: return 0;
  }
  // LWU/LD/SD son ops de 64 bits: reservadas (RI) en modo no-kernel sin UX/SX. Mismo criterio
  // que reserved64() pero sin vectorizar → bail para que el intérprete levante la RI exacta.
  if(OPc == 0x27 || OPc == 0x37 || OPc == 0x3f) {
    u32 mode = cpuMode();
    if(mode != 0) { u32 st = (u32)cop0[C0_Status]; bool allowed = (mode == 2) ? (st & 0x20) : (st & 0x40); if(!allowed) return 0; }
  }
  // Alineación (AdEL/AdES) comprobada sin efectos. Traducción UNA sola vez vía probe (sin
  // vectorizar): captura la phys y reúsala para el acceso, evitando el 2º translate que hacían
  // read*/write* (el intérprete traduce una vez por op; igualamos ese coste).
  if(sz > 1 && (a & (u64)(sz - 1))) return 0;             // misalign → intérprete vectoriza AdE
  u64 p;
  { bool s = probing; probing = true;
    p = translate(a, store ? AccWrite : AccRead); probing = s; }
  if(p == ~0ull) return 0;                                // TLB/ADE fault → intérprete vectoriza
  // reXor: byte-swap de sub-palabra por endianness (LD/SD de 8B no lo aplican). inRdram decide
  // ruta D-cache vs MMIO/cart. Mismas rutas dcRead/dcWrite/storeRepeat/storeCart/wordStoreQuirk
  // que el intérprete → output byte-idéntico.
  u32 pe = (sz == 8) ? (u32)p : (u32)reXor(p, sz);
  bool inRdram = cacheable(a) && pe < mem->rdram.size();
  if(!store) {
    u64 raw = inRdram ? dcRead(pe, sz)
            : uncachedRead(pe, sz, [&]{ return sz == 1 ? (u64)mem->read8(pe) : sz == 2 ? (u64)mem->read16(pe)
                                             : sz == 4 ? (u64)mem->read32(pe) : mem->read64(pe); });
    switch(OPc) {
      case 0x20: set(RT, sext8 ((u8) raw)); break;
      case 0x21: set(RT, sext16((u16)raw)); break;
      case 0x23: set(RT, sext32((u32)raw)); break;
      case 0x24: set(RT, (u64)(u8) raw); break;
      case 0x25: set(RT, (u64)(u16)raw); break;
      case 0x27: set(RT, (u64)(u32)raw); break;
      case 0x37: set(RT, raw); break;
    }
    return 1;
  }
  // Stores: rutas especiales EXACTAS por tamaño (SW no llama storeCart ni wordStoreQuirk;
  // SD no llama storeCart; SB/SH llaman ambos) — igual que los case del intérprete.
  u32 pm = (u32)p & 0x1fff'ffff;
  u64 rt = gpr[RT];
  if(storeRepeat(pm, rt, sz)) return 1;
  if(sz == 1 || sz == 2) { if(storeCart(pm, rt, sz)) return 1; }
  if(sz != 4)            { if(mem->wordStoreQuirk(pm, rt, sz)) return 1; }
  if(inRdram) { dcWrite(pe, rt, sz); return 1; }
  ramUncached(pe, sz);
  switch(sz) {
    case 1: mem->write8 (pe, (u8) rt); break;
    case 2: mem->write16(pe, (u16)rt); break;
    case 4: mem->write32(pe, (u32)rt); break;
    case 8: mem->write64(pe, rt);      break;
  }
  return 1;
}

// Helper de memoria del JIT especializado por opcode (ver declaracion en cpu.hpp). Todo lo
// que el generico decidia en caliente -- switch de 11 casos sobre el opcode, tamano, signo,
// si es store, la comprobacion de op de 64 bits -- aqui es constante de plantilla, y la
// direccion (a) y el dato del store (rtVal) llegan en registros: el bloque ya los tiene
// residentes en el cache de registros, asi que no hace falta ni volcarlos a cpu->gpr ni
// releerlos. El orden y las rutas de acceso son EXACTAMENTE las del generico.
template<u32 OPc>
auto CPU::jitMemOp(u64 a, u32 rt, u64 rtVal) -> u8 {
  constexpr u32 sz = (OPc == 0x20 || OPc == 0x24 || OPc == 0x28) ? 1
                   : (OPc == 0x21 || OPc == 0x25 || OPc == 0x29) ? 2
                   : (OPc == 0x23 || OPc == 0x27 || OPc == 0x2b || OPc == 0x31 || OPc == 0x39) ? 4
                   : 8;
  constexpr bool store = (OPc == 0x28 || OPc == 0x29 || OPc == 0x2b || OPc == 0x3f
                       || OPc == 0x39 || OPc == 0x3d);
  // LWC1/LDC1/SWC1/SDC1: el "rt" indexa el banco FPU, no los gpr, y el dato del store sale
  // de fpr (el bloque no lo pasa). Con CU1=0 se bailea para que el interprete levante la
  // Coprocessor Unusable exacta (ExcCode 11, CE=1).
  constexpr bool fp = (OPc == 0x31 || OPc == 0x35 || OPc == 0x39 || OPc == 0x3d);
  if(!mem) return 0;
  if constexpr(fp) { if(!((u32)cop0[C0_Status] & 0x2000'0000u)) return 0; }
  // LWU/LD/SD: reservadas (RI) en modo no-kernel sin UX/SX. Bail para que el interprete
  // levante la RI exacta.
  if constexpr(OPc == 0x27 || OPc == 0x37 || OPc == 0x3f) {
    u32 mode = cpuMode();
    if(mode != 0) {
      u32 st = (u32)cop0[C0_Status];
      bool allowed = (mode == 2) ? (st & 0x20) : (st & 0x40);
      if(!allowed) return 0;
    }
  }
  if constexpr(sz > 1) { if(a & (u64)(sz - 1)) return 0; }   // misalign -> interprete vectoriza
  u64 p;
  // Traduccion: primero el camino directo en linea (kseg0/kseg1 en kernel de 32 bits), que es
  // donde vive practicamente todo el trabajo de un juego. Solo si no aplica se paga la llamada
  // a translate(), que ademas hay que hacer en modo `probing` para que un fallo de TLB no
  // vectorice desde dentro del bloque: eso lo hace el interprete al reejecutar la op.
  if(__builtin_expect(!xlatDirect(a, p), 0)) {
    bool s = probing; probing = true;
    p = translate(a, store ? AccWrite : AccRead); probing = s;
    if(p == ~0ull || memAbort) return 0;                    // TLB/ADE/abort -> interprete vectoriza
  }
  u32 pe = (sz == 8) ? (u32)p : (u32)reXor(p, sz);
  bool inRdram = cacheable(a) && pe < mem->rdram.size();
  if constexpr(!store) {
    u64 raw = inRdram ? dcRead(pe, sz)
            : uncachedRead(pe, sz, [&]{ return sz == 1 ? (u64)mem->read8(pe) : sz == 2 ? (u64)mem->read16(pe)
                                             : sz == 4 ? (u64)mem->read32(pe) : mem->read64(pe); });
    if      constexpr(OPc == 0x20) set(rt, sext8 ((u8) raw));
    else if constexpr(OPc == 0x21) set(rt, sext16((u16)raw));
    else if constexpr(OPc == 0x23) set(rt, sext32((u32)raw));
    else if constexpr(OPc == 0x24) set(rt, (u64)(u8) raw);
    else if constexpr(OPc == 0x25) set(rt, (u64)(u16)raw);
    else if constexpr(OPc == 0x27) set(rt, (u64)(u32)raw);
    else if constexpr(OPc == 0x31) fprSet32(rt, (u32)raw);   // LWC1
    else if constexpr(OPc == 0x35) fprSet64(rt, raw);        // LDC1
    else                           set(rt, raw);            // LD
    return 1;
  } else {
    u32 pm = (u32)p & 0x1fff'ffff;
    if constexpr(fp) rtVal = (sz == 4) ? (u64)fprGet32(rt) : fprGet64(rt);
    if(storeRepeat(pm, rtVal, sz)) return 1;
    if constexpr(sz == 1 || sz == 2) { if(storeCart(pm, rtVal, sz)) return 1; }
    if constexpr(sz != 4)            { if(mem->wordStoreQuirk(pm, rtVal, sz)) return 1; }
    if(inRdram) { dcWrite(pe, rtVal, sz); return 1; }
    ramUncached(pe, sz);
    if      constexpr(sz == 1) mem->write8 (pe, (u8) rtVal);
    else if constexpr(sz == 2) mem->write16(pe, (u16)rtVal);
    else if constexpr(sz == 4) mem->write32(pe, (u32)rtVal);
    else                       mem->write64(pe, rtVal);
    return 1;
  }
}

// Trampolines C, uno por opcode: el bloque emite un CALL directo al suyo. Win64 pasa
// (cpu, direccion, rt, dato) en RCX/RDX/R8/R9.
#define KJM(name, opc) \
  extern "C" u8 name(void* c, u64 a, u32 rt, u64 v) { \
    return reinterpret_cast<kestrel::CPU*>(c)->jitMemOp<opc>(a, rt, v); }
KJM(kestrel_jitLB,  0x20) KJM(kestrel_jitLH,  0x21) KJM(kestrel_jitLW,  0x23)
KJM(kestrel_jitLBU, 0x24) KJM(kestrel_jitLHU, 0x25) KJM(kestrel_jitLWU, 0x27)
KJM(kestrel_jitLD,  0x37) KJM(kestrel_jitSB,  0x28) KJM(kestrel_jitSH,  0x29)
KJM(kestrel_jitSW,  0x2b) KJM(kestrel_jitSD,  0x3f)
KJM(kestrel_jitLWC1, 0x31) KJM(kestrel_jitLDC1, 0x35)
KJM(kestrel_jitSWC1, 0x39) KJM(kestrel_jitSDC1, 0x3d)
#undef KJM

auto CPU::special(u32 op) -> void {
  // Doubleword SPECIAL functs (DADD/DSUB/DMULT/DDIV/DSxx*) are reserved in 32-bit
  // non-kernel mode.
  switch(FUNCT) {
  case 0x14: case 0x16: case 0x17: case 0x1c: case 0x1d: case 0x1e: case 0x1f:
  case 0x2c: case 0x2d: case 0x2e: case 0x2f:
  case 0x38: case 0x3a: case 0x3b: case 0x3c: case 0x3e: case 0x3f:
    if(reserved64()) return; break;
  default: break;
  }
  switch(FUNCT) {
  case 0x00: /*SLL*/  set(RD, sext32((u32)gpr[RT] << SA)); break;
  case 0x02: /*SRL*/  set(RD, sext32((u32)gpr[RT] >> SA)); break;
  case 0x03: /*SRA*/  set(RD, sext32((u32)((s64)gpr[RT] >> SA))); break;  // VR4300: 64-bit arith shift, low32 sign-extended
  case 0x04: /*SLLV*/ set(RD, sext32((u32)gpr[RT] << (gpr[RS] & 31))); break;
  case 0x06: /*SRLV*/ set(RD, sext32((u32)gpr[RT] >> (gpr[RS] & 31))); break;
  case 0x07: /*SRAV*/ set(RD, sext32((u32)((s64)gpr[RT] >> (gpr[RS] & 31)))); break;  // VR4300 64-bit arith shift quirk
  case 0x08: /*JR*/   justBranched = true; nextPc = gpr[RS]; break;
  case 0x09: /*JALR*/ { u64 t = gpr[RS]; justBranched = true; set(RD ? RD : 31, sext32((u32)nextPc)); nextPc = t; } break;  // read target before linking (rd may == rs)
  case 0x0c: /*SYSCALL*/ takeException(8); break;
  case 0x0d: /*BREAK*/   takeException(9); break;
  case 0x0f: /*SYNC*/ break;
  case 0x10: /*MFHI*/ set(RD, hi); break;
  case 0x11: /*MTHI*/ hi = gpr[RS]; break;
  case 0x12: /*MFLO*/ set(RD, lo); break;
  case 0x13: /*MTLO*/ lo = gpr[RS]; break;
  case 0x14: /*DSLLV*/ set(RD, gpr[RT] << (gpr[RS] & 63)); break;
  case 0x16: /*DSRLV*/ set(RD, gpr[RT] >> (gpr[RS] & 63)); break;
  case 0x17: /*DSRAV*/ set(RD, (u64)((s64)gpr[RT] >> (gpr[RS] & 63))); break;
  case 0x18: /*MULT*/ { chargeMulDiv(5); s64 r=(s64)(s32)gpr[RS]*(s64)(s32)gpr[RT]; lo=sext32((u32)r); hi=sext32((u32)(r>>32)); break; }
  case 0x19: /*MULTU*/{ chargeMulDiv(5); u64 r=(u64)(u32)gpr[RS]*(u64)(u32)gpr[RT]; lo=sext32((u32)r); hi=sext32((u32)(r>>32)); break; }
  // MIPS div never traps: divide-by-zero and INT_MIN/-1 overflow produce defined
  // R4300i results (host idiv WOULD trap on both — must guard). n64-systemtest checks these.
  case 0x1a: /*DIV*/  { chargeMulDiv(37); s32 a=(s32)gpr[RS], b=(s32)gpr[RT];
      if(b==0){ lo=sext32(a<0?1u:0xffffffffu); hi=sext32((u32)a); }
      else if(a==(s32)0x80000000 && b==-1){ lo=sext32(0x80000000u); hi=0; }
      else { lo=sext32((u32)(a/b)); hi=sext32((u32)(a%b)); } break; }
  case 0x1b: /*DIVU*/ { chargeMulDiv(37); u32 a=(u32)gpr[RS], b=(u32)gpr[RT];
      if(b==0){ lo=sext32(0xffffffffu); hi=sext32(a); }
      else { lo=sext32(a/b); hi=sext32(a%b); } break; }
  case 0x1c: /*DMULT*/ { chargeMulDiv(8); __int128 r=(__int128)(s64)gpr[RS]*(s64)gpr[RT]; lo=(u64)r; hi=(u64)(r>>64); break; }
  case 0x1d: /*DMULTU*/{ chargeMulDiv(8); unsigned __int128 r=(unsigned __int128)gpr[RS]*gpr[RT]; lo=(u64)r; hi=(u64)(r>>64); break; }
  case 0x1e: /*DDIV*/ { chargeMulDiv(69); s64 a=(s64)gpr[RS], b=(s64)gpr[RT];
      if(b==0){ lo=(u64)(a<0?1:-1); hi=(u64)a; }
      else if(a==(s64)0x8000000000000000ull && b==-1){ lo=0x8000000000000000ull; hi=0; }
      else { lo=(u64)(a/b); hi=(u64)(a%b); } break; }
  case 0x1f: /*DDIVU*/{ chargeMulDiv(69); u64 a=gpr[RS], b=gpr[RT];
      if(b==0){ lo=~0ull; hi=a; }
      else { lo=a/b; hi=a%b; } break; }
  case 0x20: /*ADD*/  { s32 a=(s32)gpr[RS], b=(s32)gpr[RT], r=(s32)((u32)a+(u32)b); if(((a^r)&(b^r))<0){ takeException(12); break; } set(RD, sext32((u32)r)); break; }
  case 0x21: /*ADDU*/ set(RD, sext32((u32)(gpr[RS] + gpr[RT]))); break;
  case 0x22: /*SUB*/  { s32 a=(s32)gpr[RS], b=(s32)gpr[RT], r=(s32)((u32)a-(u32)b); if(((a^b)&(a^r))<0){ takeException(12); break; } set(RD, sext32((u32)r)); break; }
  case 0x23: /*SUBU*/ set(RD, sext32((u32)(gpr[RS] - gpr[RT]))); break;
  case 0x24: /*AND*/  set(RD, gpr[RS] & gpr[RT]); break;
  case 0x25: /*OR*/   set(RD, gpr[RS] | gpr[RT]); break;
  case 0x26: /*XOR*/  set(RD, gpr[RS] ^ gpr[RT]); break;
  case 0x27: /*NOR*/  set(RD, ~(gpr[RS] | gpr[RT])); break;
  case 0x2a: /*SLT*/  set(RD, (s64)gpr[RS] < (s64)gpr[RT] ? 1 : 0); break;
  case 0x2b: /*SLTU*/ set(RD, gpr[RS] < gpr[RT] ? 1 : 0); break;
  case 0x2c: /*DADD*/ { s64 a=(s64)gpr[RS], b=(s64)gpr[RT], r=(s64)((u64)a+(u64)b); if(((a^r)&(b^r))<0){ takeException(12); break; } set(RD, (u64)r); break; }
  case 0x2d: /*DADDU*/set(RD, gpr[RS] + gpr[RT]); break;
  case 0x2e: /*DSUB*/ { s64 a=(s64)gpr[RS], b=(s64)gpr[RT], r=(s64)((u64)a-(u64)b); if(((a^b)&(a^r))<0){ takeException(12); break; } set(RD, (u64)r); break; }
  case 0x2f: /*DSUBU*/set(RD, gpr[RS] - gpr[RT]); break;
  // Register-form traps: condition true → Trap exception (ExcCode 13). No result reg.
  case 0x30: /*TGE*/  if((s64)gpr[RS] >= (s64)gpr[RT]) takeException(13); break;
  case 0x31: /*TGEU*/ if(gpr[RS] >= gpr[RT])           takeException(13); break;
  case 0x32: /*TLT*/  if((s64)gpr[RS] <  (s64)gpr[RT]) takeException(13); break;
  case 0x33: /*TLTU*/ if(gpr[RS] <  gpr[RT])           takeException(13); break;
  case 0x34: /*TEQ*/  if(gpr[RS] == gpr[RT])           takeException(13); break;
  case 0x36: /*TNE*/  if(gpr[RS] != gpr[RT])           takeException(13); break;
  case 0x38: /*DSLL*/ set(RD, gpr[RT] << SA); break;
  case 0x3a: /*DSRL*/ set(RD, gpr[RT] >> SA); break;
  case 0x3b: /*DSRA*/ set(RD, (u64)((s64)gpr[RT] >> SA)); break;
  case 0x3c: /*DSLL32*/ set(RD, gpr[RT] << (SA + 32)); break;
  case 0x3e: /*DSRL32*/ set(RD, gpr[RT] >> (SA + 32)); break;
  case 0x3f: /*DSRA32*/ set(RD, (u64)((s64)gpr[RT] >> (SA + 32))); break;
  default: unimplemented(op);
  }
}

auto CPU::regimm(u32 op) -> void {
  switch(RT) {
  case 0x00: /*BLTZ*/  branch((s64)gpr[RS] <  0, pc + (SIMM << 2)); break;
  case 0x01: /*BGEZ*/  branch((s64)gpr[RS] >= 0, pc + (SIMM << 2)); break;
  case 0x02: /*BLTZL*/ if((s64)gpr[RS] <  0) { justBranched = true; nextPc = pc + (SIMM<<2); } else { pc = nextPc; nextPc = pc + 4; } break;
  case 0x03: /*BGEZL*/ if((s64)gpr[RS] >= 0) { justBranched = true; nextPc = pc + (SIMM<<2); } else { pc = nextPc; nextPc = pc + 4; } break;
  // AL variants read the branch condition from rs BEFORE linking $31 — rs may be $31
  // (a branch that overwrites its own condition input); the test uses the pre-link value.
  case 0x10: /*BLTZAL*/ { bool c=(s64)gpr[RS] <  0; set(31, sext32((u32)nextPc)); branch(c, pc + (SIMM << 2)); } break;
  case 0x11: /*BGEZAL*/ { bool c=(s64)gpr[RS] >= 0; set(31, sext32((u32)nextPc)); branch(c, pc + (SIMM << 2)); } break;
  // Branch-likely-and-link: link $31 unconditionally, then likely-nullify the delay slot when not taken.
  case 0x12: /*BLTZALL*/ { bool c=(s64)gpr[RS] <  0; set(31, sext32((u32)nextPc)); if(c) { justBranched = true; nextPc = pc + (SIMM<<2); } else { pc = nextPc; nextPc = pc + 4; } } break;
  case 0x13: /*BGEZALL*/ { bool c=(s64)gpr[RS] >= 0; set(31, sext32((u32)nextPc)); if(c) { justBranched = true; nextPc = pc + (SIMM<<2); } else { pc = nextPc; nextPc = pc + 4; } } break;
  // Immediate-form traps: compare rs against sign-extended immediate → Trap (ExcCode 13).
  case 0x08: /*TGEI*/  if((s64)gpr[RS] >= (s64)SIMM)          takeException(13); break;
  case 0x09: /*TGEIU*/ if(gpr[RS] >= (u64)(s64)SIMM)          takeException(13); break;
  case 0x0a: /*TLTI*/  if((s64)gpr[RS] <  (s64)SIMM)          takeException(13); break;
  case 0x0b: /*TLTIU*/ if(gpr[RS] <  (u64)(s64)SIMM)          takeException(13); break;
  case 0x0c: /*TEQI*/  if((s64)gpr[RS] == (s64)SIMM)          takeException(13); break;
  case 0x0e: /*TNEI*/  if((s64)gpr[RS] != (s64)SIMM)          takeException(13); break;
  default: unimplemented(op);
  }
}

// VR4300 COP0 read/write masking. The unused registers {7,21,22,23,31} share a
// single hidden latch (writing any writes the latch, reading any returns it); the
// documented registers apply per-field write masks. n64-systemtest's cop0 suite
// pins every one of these behaviors.
auto CPU::readCop0(u32 reg) -> u64 {
  switch(reg) {
    case 7: case 21: case 22: case 23: case 24: case 25: case 31: return cop0Unused;
    case C0_PRId:   return 0x0000'0b22;   // constant R4300i revision
    // Count lleva dentro las paradas ya ocurridas aunque aun no se hayan volcado: el fallo de
    // I-cache del fetch de ESTA instruccion (o su enclavamiento) para la tuberia antes de que
    // llegue a ejecutarse, y el reloj sigue corriendo mientras. El interprete vuelca
    // stallCycles tras ejecutar (countAdd al final del paso); sin esto un osGetCount que abre
    // linea leia Count 24 ticks corto y el dynarec (que suma lo pendiente, ver jit.cpp MFC0)
    // armaba otro Compare. Sin paradas pendientes, la suma vale 0 y es el registro de siempre.
    case C0_Count:  return (u32)((u32)cop0[reg] + (u32)(((u64)countFrac + ((u64)stallCycles << 7)) >> 8));
    default: return cop0[reg];
  }
}

auto CPU::writeCop0(u32 reg, u64 v) -> void {
  cop0Unused = v;   // every COP0 write latches the data bus; unused regs read it back
  switch(reg) {
    case C0_Index:   cop0[reg] = sext32((u32)v & 0x8000'003Fu); return;
    case C0_Random:  return;                       // read-only (free-running counter)
    case C0_Wired:   cop0[reg] = (u32)v & 0x3F; randomReload = 2; return;  // reloads Random=31 one instr later
    case C0_EntryLo0:
    case C0_EntryLo1: cop0[reg] = v & 0x3FFF'FFFFull; return;   // PFN+C+D+V+G, bits [29:0]; upper read 0
    case C0_EntryHi: { u64 nv = v & 0xC00000FF'FFFFE0FFull;
      // El ASID entra en el match del TLB: cambiarlo remapea el espacio virtual entero,
      // asi que invalida los enlaces del dynarec igual que un TLBWI. El resto de EntryHi
      // (VPN2) solo alimenta al proximo TLBWI/TLBP y por si mismo no remapea nada.
      if((nv & 0xFF) != (cop0[reg] & 0xFF)) { ++tlbGen; jitTlbValid = false; }
      cop0[reg] = nv; bumpXlat(); return; }  // R+VPN2+ASID; bits [12:8] read 0 (ASID cambia el match TLB → invalida fetch fast-path)
    case C0_PageMask: cop0[reg] = v & 0x01FFE000ull; return;    // MASK field, bits [24:13]
    case C0_BadVAddr: return;                      // read-only
    case C0_PRId:    return;                        // read-only constant
    case 26:/*ParityError*/  cop0[reg] = (u32)v & 0xFF; return;
    case 27:/*CacheError*/   return;               // read-only, reads 0
    case C0_Context:  cop0[reg] = (v & ~0x7F'FFFFull) | (cop0[reg] & 0x7F'FFFFull); return;   // [63:23] writable
    case C0_XContext: cop0[reg] = (v & ~0x1'FFFF'FFFFull) | (cop0[reg] & 0x1'FFFF'FFFFull); return; // [63:33] writable
    case 17:/*LLAddr*/ cop0[reg] = (u32)v; return; // 32-bit, zero-extended
    // WatchLo: PAddr0 en [31:3], R en el bit 1, W en el bit 0; el bit 2 no existe y lee 0.
    // WatchHi: PAddr1, los bits [35:32] de la fisica, o sea [3:0] del registro. Armar
    // cualquiera de las dos condiciones saca al dynarec: la excepcion Watch es precisa y el
    // codigo emitido no la mira, asi que mientras este armada manda el interprete.
    case 18:/*WatchLo*/ cop0[reg] = sext32((u32)v & 0xFFFF'FFFBu);
                        watchArmed = ((u32)v & 3u) != 0; ++tlbGen; jitTlbValid = false; return;
    case 19:/*WatchHi*/ cop0[reg] = sext32((u32)v & 0xFu); return;
    case C0_Config:  cop0[reg] = sext32(((u32)v & 0x0F00'800Fu) | 0x7006'6460u); return;  // writable: 0-3,15,24-27; rest fixed
    case C0_Status: {
      u32 ov = (u32)cop0[reg], nv = (u32)v & ~(1u<<19);
      if(!(ov & 0x2) && (nv & 0x2)) { exlSetRet = retired; exlSetPc = (u32)curPc; exlSetSrc = 2; }
      if((ov & 0x2) && !(nv & 0x2)) exlClrRet = retired;
      cop0[reg] = sext32(nv); bumpXlat(); return; }  // bit19 not writable (modo/RE/bit64 cambian la traducción → invalida fetch fast-path)
    case C0_Compare: cop0[reg] = sext32((u32)v); timerIntr = false; return;  // writing acks timer
    case 7: case 21: case 22: case 23: case 24: case 25: case 31: return;    // no storage; latch only
    default: cop0[reg] = v; return;
  }
}

auto CPU::cop0op(u32 op) -> void {
  // COP0 is privileged: usable in kernel mode always, or in user/supervisor only
  // when Status.CU0 (bit 28) is set. Otherwise Coprocessor Unusable (ExcCode 11),
  // CE=0. cpuMode() already collapses EXL/ERL to kernel.
  if(cpuMode() != 0 && !((u32)cop0[C0_Status] & 0x1000'0000u)) {
    takeException(11);   // takeException clears Cause.CE -> CE=0 for cop 0
    return;
  }
  u32 rs = RS;
  switch(rs) {
  case 0x00: /*MFC0*/ set(RT, sext32((u32)readCop0(RD))); break;
  case 0x04: /*MTC0*/
    // Cacheado: esto corre en CADA MTC0 a Status y getenv recorre el entorno entero bajo
    // candado. El perfilador de host lo veia, junto al de ERET, como el 7% del hilo de CPU.
    static const bool statTrace = std::getenv("KESTREL_STATTRACE") != nullptr;
    if(RD == C0_Status && statTrace) {
      u32 ov=(u32)cop0[C0_Status], nv=(u32)gpr[RT];
      if((ov^nv)&0x2000'0000u) std::fprintf(stderr,"[stat] CU1 %s @pc=0x%08x %08x->%08x ra=0x%08x ret=%llu\n",
        (nv&0x2000'0000u)?"ON ":"OFF",(u32)curPc,ov,nv,(u32)gpr[31],(unsigned long long)retired);
    }
    writeCop0(RD, sext32((u32)gpr[RT]));   // 32-bit write: sign-extend before masking
    break;
  case 0x01: /*DMFC0*/ set(RT, readCop0(RD)); break;
  case 0x05: /*DMTC0*/
    writeCop0(RD, gpr[RT]);
    break;
  default:
    if(op & 0x02000000) { /*CO: ERET / TLB ops / emux extensions*/
      if(FUNCT == 0x18) {  /*ERET*/
        bumpXlat();   // limpiar EXL/ERL cambia el modo → invalida el fetch fast-path
        if(cop0[C0_Status] & 0x4) { nextPc = cop0[30/*ErrorEPC*/]; cop0[C0_Status] &= ~0x4u; }
        else                      { nextPc = cop0[C0_EPC];         cop0[C0_Status] &= ~0x2u; }
        exlClrRet = retired;
        if(excTrace && exceptions < 200)
          std::fprintf(stderr, "[eret] -> 0x%08x status=%08x retired=%llu\n",
                       (u32)nextPc, (u32)cop0[C0_Status], (unsigned long long)retired);
        int eretKind = fpTraceEret;
        if(fpTrace && fpTraceEret) {
          std::fprintf(stderr, "[R] -> 0x%08x status=%08x ret=%llu\n",
                       (u32)nextPc, (u32)cop0[C0_Status], (unsigned long long)retired);
          std::fflush(stderr); fpTraceEret = 0;
        }
        static bool seen15done=false;
        static const bool seenFind = std::getenv("KESTREL_SEENFIND") != nullptr;
        if(seenFind && eretKind==2 && !seen15done) {
          memAbort=false;
          std::fprintf(stderr, "[SEENDUMP15@eret ret=%llu] (epc was code-15)\n",(unsigned long long)retired);
          for(u32 a=0x801acf50;a<0x801ad010;a+=4){u32 w=read32(a); if(w) std::fprintf(stderr,"  0x%08x=0x%08x\n",a,w);}
          memAbort=false; std::fflush(stderr); seen15done=true;  // one-shot
        }
        pc = nextPc; nextPc = pc + 4;   // ERET has no delay slot
        inDelay = false; llbit = false;
      } else if(FUNCT == 0x01) { tlbRead((u32)cop0[C0_Index]); }
        else if(FUNCT == 0x02) { tlbWrite((u32)cop0[C0_Index]); }
        else if(FUNCT == 0x06) { tlbWrite((u32)cop0[C0_Random]); }
        else if(FUNCT == 0x08) { tlbProbe(); }
        else if(FUNCT == 0x20 || FUNCT == 0x25 || FUNCT == 0x2c) {
        emuxOp(op);   // n64-systemtest headless log/exit channel
      }
      break;
    }
    unimplemented(op);
  }
}

// n64-systemtest "emux" emulator-extension protocol (see src/emux.rs in that repo).
// Encoded in COP0 CO space with a non-standard field layout:
//   arg_rd = (op>>20)&0x1f, arg_rt = (op>>15)&0x1f, code = (op>>6)&0x1ff, funct = op&0x3f.
// funct 0x20 XDETECT: report supported extensions (0x20..0x3f) as a bitmask in arg_rd.
// funct 0x25 XLOG:    write arg_rt bytes of the string at arg_rd to stdout (headless log).
// funct 0x2c XIOCTL:  code 1 = EXIT (stop), code 2 = FAST (no-op).
auto CPU::emuxOp(u32 op) -> void {
  u32 argRd = (op >> 20) & 0x1f;
  u32 argRt = (op >> 15) & 0x1f;
  u32 code  = (op >>  6) & 0x1ff;
  switch(FUNCT) {
  case 0x20: {  // XDETECT
    u32 mask = 0;
    if(code == 1) mask = (1u << (0x25 - 0x20)) | (1u << (0x2c - 0x20));  // XLOG | XIOCTL
    set(argRd, sext32(mask));
    break;
  }
  case 0x25: {  // XLOG
    u64 ptr = gpr[argRd];
    u64 len = gpr[argRt];
    if(len > 0x10000) len = 0x10000;   // sanity cap
    static std::string acc;
    for(u64 i = 0; i < len; i++) { u8 c = read8(ptr + i); std::fputc(c, stdout); acc.push_back((char)c); }
    std::fflush(stdout);
    if(std::getenv("KESTREL_LEAKDUMP") && acc.find("with exception") != std::string::npos) {
      std::fprintf(stderr, "\n[LEAKDUMP] verdict='%s'\n  SEEN-disc transitions (retired: old->new):\n", acc.c_str());
      int n = g_seenIdx < kSeenRing ? g_seenIdx : kSeenRing;
      int start = g_seenIdx < kSeenRing ? 0 : g_seenIdx % kSeenRing;
      for(int k=0;k<n;k++){ int i=(start+k)%kSeenRing; std::fprintf(stderr,"   ret=%llu: %u->%u\n",(unsigned long long)g_seenRet[i],g_seenOld[i],g_seenNew[i]); }
      std::fprintf(stderr, "  last control transfers (oldest first):\n");
      for(u32 k=0;k<(u32)kJumpLog;k++){ u32 i=(jlogIdx+k)%kJumpLog; if(!jlogSrc[i]&&!jlogDst[i])continue;
        std::fprintf(stderr,"    %-8s 0x%08x -> 0x%08x\n", disasm(jlogOp[i],jlogSrc[i]).c_str(),(u32)jlogSrc[i],(u32)jlogDst[i]); }
      std::fflush(stderr);
    }
    if(acc.size() > 400 || acc.find('\n') != std::string::npos) acc.clear();
    break;
  }
  case 0x2c:    // XIOCTL
    if(code == 1) {
      if(excTail) {
        u32 n = excRingIdx < (u32)kExcRing ? excRingIdx : (u32)kExcRing;
        u32 start = excRingIdx >= (u32)kExcRing ? excRingIdx - kExcRing : 0;
        std::fprintf(stderr, "[exctail] last %u exceptions (of %u):\n", n, excRingIdx);
        for(u32 k = 0; k < n; k++) {
          u32 i = (start + k) % kExcRing;
          std::fprintf(stderr, "  code=%2u epc=0x%016llx bad=0x%016llx ret=%llu\n",
                       excRingCode[i], (unsigned long long)excRingEpc[i],
                       (unsigned long long)excRingBad[i], (unsigned long long)excRingRet[i]);
        }
        std::fprintf(stderr, "[jtail] last control transfers (oldest first):\n");
        for(u32 k = 0; k < (u32)kJumpLog; k++) {
          u32 i = (jlogIdx + k) % kJumpLog;
          if(!jlogSrc[i] && !jlogDst[i]) continue;
          std::fprintf(stderr, "  op=0x%08x %-10s 0x%016llx -> 0x%016llx\n",
                       jlogOp[i], disasm(jlogOp[i], jlogSrc[i]).c_str(),
                       (unsigned long long)jlogSrc[i], (unsigned long long)jlogDst[i]);
        }
        std::fprintf(stderr, "[memdump] guest 0x801841a0..0x80184250:\n");
        for(u32 a = 0x1841a0; a < 0x184250; a += 4) {
          u32 w = mem ? mem->read32(a) : 0;
          std::fprintf(stderr, "  0x80%06x: 0x%08x  %s\n", a, w, disasm(w, 0x80000000 + a).c_str());
        }
        std::fflush(stderr);
      }
      halt("emux xioctl exit");   // XIOCTL_EXIT
    }
    break;      // code 2 = XIOCTL_FAST: run without frame throttling; no-op here
  }
}

auto CPU::fprGet64(u32 i) -> u64 {
  if((u32)cop0[C0_Status] & (1u<<26)) return fpr[i];   // FR=1
  return fpr[i & ~1u];                                 // FR=0: even reg holds the pair
}
auto CPU::fprSet64(u32 i, u64 v) -> void {
  if((u32)cop0[C0_Status] & (1u<<26)) { fpr[i] = v; return; }
  fpr[i & ~1u] = v;
}
auto CPU::fprGet32(u32 i) -> u32 {
  if((u32)cop0[C0_Status] & (1u<<26)) return (u32)fpr[i];  // FR=1: low 32
  if(i & 1) return (u32)(fpr[i & ~1u] >> 32);              // FR=0 odd: high half of partner
  return (u32)fpr[i];                                      // FR=0 even: low half
}
auto CPU::fprSet32(u32 i, u32 v) -> void {
  if((u32)cop0[C0_Status] & (1u<<26)) { fpr[i] = (fpr[i] & 0xffff'ffff'0000'0000ull) | v; return; }
  if(i & 1) { u32 e=i&~1u; fpr[e] = (fpr[e] & 0x0000'0000'ffff'ffffull) | ((u64)v << 32); return; }
  fpr[i] = (fpr[i] & 0xffff'ffff'0000'0000ull) | v;
}

auto CPU::cop1op(u32 op) -> void {
  // Coprocessor-1 usability: FP ops require Status.CU1 (bit 29). A thread with FP
  // disabled traps here (ExcCode 11, Coprocessor Unusable, CE=1); the OS is what
  // enables FP / saves-restores FP context per thread. Skipping this check lets a
  // CU1=0 thread scribble FP registers the OS never preserves across interrupts.
  if(!((u32)cop0[C0_Status] & 0x2000'0000u)) {
    takeException(11);
    cop0[C0_Cause] = sext32(((u32)cop0[C0_Cause] & ~0x3000'0000u) | (1u << 28));  // CE = 1
    return;
  }
  u32 rs = RS;
  switch(rs) {
  case 0x00: /*MFC1*/ set(RT, sext32(fprGet32(RD))); return;
  case 0x01: /*DMFC1*/ set(RT, fprGet64(RD)); return;
  case 0x02: /*CFC1*/ set(RT, sext32(RD == 31 ? fcr31 : RD == 0 ? fcr0 : 0)); return;
  case 0x04: /*MTC1*/ fprSet32(RD, (u32)gpr[RT]); return;
  case 0x05: /*DMTC1*/ fprSet64(RD, gpr[RT]); return;
  case 0x06: /*CTC1*/ if(RD == 31) {  // FCSR write mask
      fcr31 = (u32)gpr[RT] & 0x0183'FFFFu;
      // A CTC1 that leaves an armed Cause bit fires the FP exception immediately:
      // E (bit17, Unimplemented) always traps; V/Z/O/U/I (cause [16:12]) trap when
      // the matching Enable bit ([11:7]) is set.
      u32 c5 = (fcr31 >> 12) & 0x1f, en = (fcr31 >> 7) & 0x1f;
      if(((fcr31 >> 17) & 1) || (c5 & en)) {
        // Pipeline quirk: the FPE is recognized on CTC1 (EPC=CTC1) but Cause.CE
        // reflects the coprocessor number of the NEXT instruction (the one already
        // fetched into the pipe). MFC1(COP1)->1, MFC2(COP2)->2, NOP/other->0.
        // In the delay-slot model `pc` already points at that next instruction.
        u32 nxt = mem ? mem->read32((u32)pc) : 0;
        u32 nop = nxt >> 26, ce = (nop >= 0x10 && nop <= 0x13) ? (nop - 0x10) : 0;
        takeException(15);
        cop0[C0_Cause] = sext32(((u32)cop0[C0_Cause] & ~0x3000'0000u) | (ce << 28));
      }
    }
    return;
  case 0x08: /*BC1*/ {  // BC1F/BC1T (+ likely variants nd=bit1, tf=bit0 of rt)
    bool cond = (fcr31 >> 23) & 1;
    bool tf = RT & 1;
    bool likely = RT & 2;
    bool take = (tf == cond);
    if(likely) { if(take) { justBranched = true; nextPc = pc + (SIMM<<2); } else { pc = nextPc; nextPc = pc + 4; } }
    else branch(take, pc + (SIMM<<2));
    return;
  }
  default: break;  // fall through to the format (arithmetic/convert/compare) ops
  }

  // Latencia de la tabla 7-14. Solo si ningun trampolin COP1 del JIT la cobro ya (ver
  // CPU::FpuCharge): ese camino puede acabar aqui via jitInterpOp y se contaria dos veces.
  if(!fpuNested) chargeFpu(op);

  // fmt in rs: 0x10 S(single), 0x11 D(double), 0x14 W(int32), 0x15 L(int64).
  // Fields: ft=RT, fs=RD, fd=SA, funct.
  u32 fmt = rs, ft = RT, fs = RD, fd = SA, fn = FUNCT;
  // Half mode (Status FR=0): computational/convert/compare ops drop the LOW BIT of
  // the source field fs only. ft and fd keep their raw index, and every operand is
  // the low 32 (.S) / full 64 (.D) of that raw register — no odd->high-half pairing
  // (that pairing is a MFC1/MTC1/LWC1 quirk, handled above). See cop1/full_vs_half_mode.
  if(!((u32)cop0[C0_Status] & (1u<<26))) fs &= ~1u;
  auto getS = [&](u32 i){ return std::bit_cast<float>((u32)fpr[i]); };
  auto getD = [&](u32 i){ return std::bit_cast<double>(fpr[i]); };
  // A 32-bit arithmetic/convert result CLEARS the upper 32 bits of the destination
  // register (unlike MTC1/LWC1, which preserve them), in both FR modes.
  auto set32c = [&](u32 i, u32 v){ fpr[i] = (u64)v; };
  auto setFlag = [&](bool c){ if(c) fcr31 |= (1u<<23); else fcr31 &= ~(1u<<23); };

  // --- FCSR exception model --------------------------------------------------
  // Every computational COP1 op rewrites the Cause field [17:12] (I=12,U=13,O=14,
  // Z=15,V=16,E=17). If any raised exception has its Enable bit [11:7] set the op
  // instead traps (FPE, ExcCode 15) and the sticky Flags [6:2] are left untouched;
  // otherwise the raised bits accumulate into Flags. Host <cfenv> supplies the IEEE
  // status for arithmetic; conversions to integer detect inexact/overflow directly.
  auto rmHost = [&]() -> u32 {
    switch(fcr31 & 3) { case 1: return mx::RZ; case 2: return mx::RP; case 3: return mx::RM; default: return mx::RN; }
  };
  bool flushDenorm = (fcr31 >> 24) & 1;                 // FCSR FS bit: flush denormals to zero
  bool ueEn = (fcr31 >> 8) & 1, ieEn = (fcr31 >> 7) & 1; // underflow / inexact enables
  auto unimpl = [&](){ fcr31 = (fcr31 & ~0x0003'F000u) | (1u << 17); takeException(15); };  // Unimplemented-Operation (E)
  // Fold host IEEE flags into FCSR after an arithmetic op; returns true if it trapped.
  auto harvest = [&]() -> bool {
    u32 ex = mx::flags();
    // VR4300: Underflow is never a normal trappable cause. A tiny result that rounds
    // all the way to zero still underflows: with FS=0, or with the U/I enable set, the
    // VR4300 raises Unimplemented-Operation; otherwise it silently sets U+I and writes
    // the (signed-zero) result. Subnormal results proper are caught earlier in setS/setD.
    if(ex & mx::UNDERFLOW_) {
      if(!flushDenorm || ueEn || ieEn) { unimpl(); return true; }
      fcr31 = (fcr31 & ~0x0003'F000u) | (1u << 13) | (1u << 12);  // Cause U+I
      fcr31 |= (1u << 3) | (1u << 2);                             // Flag  U+I
      return false;
    }
    u32 cause = 0;
    if(ex & mx::INEXACT)   cause |= 1u << 12;
    if(ex & mx::OVERFLOW_)  cause |= 1u << 14;
    if(ex & mx::DIVZERO)   cause |= 1u << 15;
    if(ex & mx::INVALID)   cause |= 1u << 16;
    fcr31 = (fcr31 & ~0x0003'F000u) | cause;
    u32 c5 = (cause >> 12) & 0x1f, en = (fcr31 >> 7) & 0x1f;
    if(c5 & en) { takeException(15); return true; }
    fcr31 |= (c5 << 2);            // accumulate sticky Flags [6:2]
    return false;
  };
  auto snan32 = [](u32 x){ return (x & 0x7fff'ffffu) > 0x7f80'0000u && !((x >> 22) & 1); };
  auto snan64 = [](u64 x){ return (x & 0x7fff'ffff'ffff'ffffull) > 0x7ff0'0000'0000'0000ull && !((x >> 51) & 1); };
  // When FS=1 flushes a subnormal result, the flushed magnitude depends on the
  // rounding mode: a directed rounding *away from zero* (toward +inf for a positive
  // value, toward -inf for a negative one) rounds the subnormal up to the smallest
  // normal instead of down to a signed zero. RM: 0 nearest, 1 toward-zero, 2 +inf,
  // 3 -inf. minNorm carries the sign bit already present in `sign`.
  auto flushMin32 = [&](u32 sign) -> u32 {
    u32 rm = fcr31 & 3;
    if((rm == 2 && sign == 0) || (rm == 3 && sign != 0)) return sign | 0x0080'0000u;
    return sign;   // -> signed zero
  };
  auto flushMin64 = [&](u64 sign) -> u64 {
    u32 rm = fcr31 & 3;
    if((rm == 2 && sign == 0) || (rm == 3 && sign != 0)) return sign | 0x0010'0000'0000'0000ull;
    return sign;
  };
  // Result setters. The VR4300 substitutes its canonical qNaN (0x7fbfffff /
  // 0x7ff7ffff…) for any NaN an arithmetic op produces. A subnormal result is not
  // representable by the VR4300 FPU: with FS=1 (and no U/I enable) it is flushed
  // (to a signed zero or the smallest normal, see flushMin) with Underflow+Inexact
  // raised; otherwise it raises Unimplemented.
  auto setS = [&](u32 i, float v){
    u32 b = std::bit_cast<u32>(v);
    if((b & 0x7f80'0000u) == 0 && (b & 0x007f'ffffu) != 0) {      // subnormal
      if(!flushDenorm || ueEn || ieEn) { unimpl(); return; }
      set32c(i, flushMin32(b & 0x8000'0000u));
      fcr31 = (fcr31 & ~0x0003'F000u) | (1u << 13) | (1u << 12);  // Cause U+I
      fcr31 |= (1u << 3) | (1u << 2);                             // Flag  U+I
      return;
    }
    if((b & 0x7fff'ffffu) > 0x7f80'0000u) b = 0x7fbf'ffffu;
    if(!harvest()) set32c(i, b);   // on a trapped exception the destination is left untouched
  };
  auto setD = [&](u32 i, double v){
    u64 b = std::bit_cast<u64>(v);
    if((b & 0x7ff0'0000'0000'0000ull) == 0 && (b & 0x000f'ffff'ffff'ffffull) != 0) {  // subnormal
      if(!flushDenorm || ueEn || ieEn) { unimpl(); return; }
      fpr[i] = flushMin64(b & 0x8000'0000'0000'0000ull);
      fcr31 = (fcr31 & ~0x0003'F000u) | (1u << 13) | (1u << 12);
      fcr31 |= (1u << 3) | (1u << 2);
      return;
    }
    if((b & 0x7fff'ffff'ffff'ffffull) > 0x7ff0'0000'0000'0000ull) b = 0x7ff7'ffff'ffff'ffffull;
    if(!harvest()) fpr[i] = b;   // on a trapped exception the destination is left untouched
  };
  auto setL = [&](u32 i, u64 v){ fpr[i] = v; };   // raw .L / MOV result (no exceptions), dest index raw
  // Convert a float source to a W(32)/L(64) integer. NaN, ±inf, subnormal inputs, or
  // an out-of-range result raise the Unimplemented-Operation exception (E) on the
  // VR4300 instead of producing a value; an inexact conversion sets I.
  auto cvtInt = [&](double src, u32 hostRnd, bool isL) {
    u32 save = _mm_getcsr();
    _mm_setcsr((save & ~(mx::RC | mx::EX)) | hostRnd);
    double r = std::rint(src);
    u32 ex = _mm_getcsr() & mx::INEXACT;
    // Restaura SÓLO el redondeo, igual que hacía fesetround(save): las banderas que
    // acabe de levantar esta conversión se quedan, y la siguiente op las limpia.
    _mm_setcsr((_mm_getcsr() & ~mx::RC) | (save & mx::RC));
    double lim = isL ? 9223372036854775808.0 : 2147483648.0;   // 2^63 / 2^31
    // .L conversions only resolve results that fit in 53 significant bits; a magnitude
    // >= 2^53 needs a 54th bit the VR4300 conversion unit lacks, so it raises Unimplemented.
    bool bad = !(src == src) || std::isinf(src) || r >= lim || r < -lim
             || (isL && std::fabs(r) >= 9007199254740992.0)
             || (src != 0.0 && std::fpclassify(src) == FP_SUBNORMAL);
    if(bad) { fcr31 = (fcr31 & ~0x0003'F000u) | (1u << 17); takeException(15); return; }
    u32 cause = ex ? (1u << 12) : 0;
    fcr31 = (fcr31 & ~0x0003'F000u) | cause;
    if(cause && ((fcr31 >> 7) & 1)) { takeException(15); return; }  // inexact enabled -> trap
    if(cause) fcr31 |= (1u << 2);
    if(isL) setL(fd, (u64)(s64)r); else set32c(fd, (u32)(s32)r);
  };

  // Prepare host rounding + a clean IEEE status for the arithmetic ops below.
  mx::prep(rmHost());

  if(fn >= 0x30) {  // C.cond.fmt — compare, set fcr31 C bit. bit0=less, bit1=equal, bit2 unord.
    bool isD = (fmt == 0x11);
    u64 ab = isD ? fpr[fs] : (u32)fpr[fs];
    u64 bb = isD ? fpr[ft] : (u32)fpr[ft];
    auto isNan  = [&](u64 x){ return isD ? (x & 0x7fff'ffff'ffff'ffffull) > 0x7ff0'0000'0000'0000ull
                                         : (x & 0x7fff'ffffu) > 0x7f80'0000u; };
    // VR4300 quirk: the mantissa MSB is inverted from IEEE-2008. A NaN with the MSB
    // SET (0x7ff8.. — what the systemtest calls QUIET) is treated as *signalling* and
    // raises Invalid on any compare. A NaN with the MSB CLEAR (0x7ff0..0x7ff7 — the
    // systemtest's SIGNALLING, and the VR4300's own canonical output NaN) raises
    // nothing on the non-signalling predicates and only fires on the signalling ones.
    auto msbSet = [&](u64 x){ if(!isNan(x)) return false; return isD ? ((x >> 51) & 1) != 0 : ((x >> 22) & 1) != 0; };
    bool unordered = isNan(ab) || isNan(bb);
    bool less = false, equal = false;
    if(!unordered) {
      if(isD) { double a = getD(fs), b = getD(ft); less = a < b; equal = a == b; }
      else    { float  a = getS(fs), b = getS(ft); less = a < b; equal = a == b; }
    }
    bool sig = (fn & 0x08) != 0;                       // signaling predicate (C.SF..C.NGT)
    bool invalid = sig ? unordered : (msbSet(ab) || msbSet(bb));
    fcr31 &= ~0x0003'F000u;
    if(invalid) {
      fcr31 |= (1u << 16);                             // Cause V
      if((fcr31 >> 11) & 1) { takeException(15); return; }
      fcr31 |= (1u << 6);                              // Flag V
    }
    bool c = false;
    if(fn & 0x4) c |= less;
    if(fn & 0x2) c |= equal;
    if(fn & 0x1) c |= unordered;
    setFlag(c);
    return;
  }

  // MOV.S / MOV.D copy the raw 64-bit register and, uniquely among FPU ops, leave
  // FCSR (including the Cause field) completely untouched — so this must run before
  // the Cause-clear below.
  if(fn == 0x06 && (fmt == 0x10 || fmt == 0x11)) { setL(fd, fpr[fs]); return; }

  fcr31 &= ~0x0003'F000u;   // computational op: start with a clear Cause field

  // NaN / subnormal operand handling for add/sub/mul/div/sqrt/abs/neg. The VR4300 FPU
  // does not implement subnormals or signalling NaNs: either raises Unimplemented-Op.
  // A quiet-NaN operand is treated as signalling too — it raises Invalid-Operation and
  // yields the canonical qNaN (trapping only if Invalid is enabled). ABS/NEG are not
  // pure sign-flips: they detect NaN/subnormal operands exactly like arithmetic.
  // CVT.D.S (single->double, fn 0x21) and CVT.S.D (double->single, fn 0x20) also read a
  // float operand and fault the same way on a subnormal/NaN source. They are handled here
  // too — but their RESULT is in the *destination* format, not the source. (The integer
  // source CVTs in fmt 0x14/0x15 have no subnormal/NaN inputs and are excluded.)
  bool f2fCvt = (fmt == 0x10 && fn == 0x21) || (fmt == 0x11 && fn == 0x20);
  // Float->int conversions (ROUND/TRUNC/CEIL/FLOOR/CVT to .W/.L) screen their source
  // in its native format: a signalling OR quiet NaN, or a subnormal, all raise
  // Unimplemented (unlike arithmetic, where a qNaN raises Invalid). Detecting this on
  // the native bits matters — a single subnormal widens to a *normal* double.
  bool intCvt = (fn >= 0x08 && fn <= 0x0f) || fn == 0x24 || fn == 0x25;
  if((fn <= 0x07 && fn != 0x06) || f2fCvt || intCvt) {
    bool binary = (fn <= 0x03);                       // only add/sub/mul/div read operand b
    bool sNan = false, subn = false, qNan = false;
    if(fmt == 0x10) {
      u32 a = (u32)fpr[fs], b = (u32)fpr[ft];
      auto sub = [](u32 x){ return (x & 0x7f80'0000u) == 0 && (x & 0x007f'ffffu) != 0; };
      auto nan = [](u32 x){ return (x & 0x7fff'ffffu) > 0x7f80'0000u; };
      sNan = snan32(a) || (binary && snan32(b));
      subn = sub(a)    || (binary && sub(b));
      qNan = (nan(a) && !snan32(a)) || (binary && nan(b) && !snan32(b));
    } else if(fmt == 0x11) {
      u64 a = fpr[fs], b = fpr[ft];
      auto sub = [](u64 x){ return (x & 0x7ff0'0000'0000'0000ull) == 0 && (x & 0x000f'ffff'ffff'ffffull) != 0; };
      auto nan = [](u64 x){ return (x & 0x7fff'ffff'ffff'ffffull) > 0x7ff0'0000'0000'0000ull; };
      sNan = snan64(a) || (binary && snan64(b));
      subn = sub(a)    || (binary && sub(b));
      qNan = (nan(a) && !snan64(a)) || (binary && nan(b) && !snan64(b));
    }
    if(fpDbg && std::getenv("KESTREL_ARGDBG")) {
      std::fprintf(stderr, "[arg] fn=%u fmt=%u fs=%u ft=%u a=%08x b=%08x fcr31=%08x FS=%d en=0x%x sNan=%d subn=%d qNan=%d retired=%llu\n",
        fn, fmt, fs, ft, fprGet32(fs), fprGet32(ft), fcr31, (fcr31>>24)&1, (fcr31>>7)&0x1f, sNan, subn, qNan, (unsigned long long)retired);
      std::fflush(stderr);
    }
    if(sNan || subn) { unimpl(); return; }
    if(qNan && intCvt) { unimpl(); return; }            // qNaN -> int is Unimplemented, not Invalid
    if(qNan) {
      fcr31 |= (1u << 16);                              // Cause V (invalid)
      if((fcr31 >> 11) & 1) { takeException(15); return; }
      fcr31 |= (1u << 6);                               // Flag V
      // canonical qNaN in the *destination* format (CVT changes format; everything else keeps it)
      bool dstSingle = f2fCvt ? (fmt == 0x11) : (fmt == 0x10);
      if(dstSingle) set32c(fd, 0x7fbf'ffffu); else fprSet64(fd, 0x7ff7'ffff'ffff'ffffull);
      return;
    }
  }

  if(fmt == 0x10) {  // single
    switch(fn) {
    case 0x00: setS(fd, getS(fs) + getS(ft)); return;
    case 0x01: setS(fd, getS(fs) - getS(ft)); return;
    case 0x02: setS(fd, getS(fs) * getS(ft)); return;
    case 0x03: setS(fd, getS(fs) / getS(ft)); return;
    case 0x04: setS(fd, std::sqrt(getS(fs))); return;
    case 0x05: set32c(fd, (u32)fpr[fs] & 0x7fff'ffffu); return;   // ABS.S (clear sign, no exceptions)
    case 0x06: setL(fd, fpr[fs]); return;                 // MOV.S (copies the full 64 bits, upper included)
    case 0x07: set32c(fd, (u32)fpr[fs] ^ 0x8000'0000u); return;   // NEG.S (flip sign, no exceptions)
    case 0x08: cvtInt(getS(fs), mx::RN, true);  return; // ROUND.L.S
    case 0x09: cvtInt(getS(fs), mx::RZ, true); return; // TRUNC.L.S
    case 0x0a: cvtInt(getS(fs), mx::RP, true);     return; // CEIL.L.S
    case 0x0b: cvtInt(getS(fs), mx::RM, true);   return; // FLOOR.L.S
    case 0x0c: cvtInt(getS(fs), mx::RN, false); return; // ROUND.W.S
    case 0x0d: cvtInt(getS(fs), mx::RZ, false);return; // TRUNC.W.S
    case 0x0e: cvtInt(getS(fs), mx::RP, false);    return; // CEIL.W.S
    case 0x0f: cvtInt(getS(fs), mx::RM, false);  return; // FLOOR.W.S
    case 0x21: setD(fd, (double)getS(fs)); return;        // CVT.D.S
    case 0x24: cvtInt(getS(fs), rmHost(), false); return;      // CVT.W.S
    case 0x25: cvtInt(getS(fs), rmHost(), true);  return;      // CVT.L.S
    }
  } else if(fmt == 0x11) {  // double
    switch(fn) {
    case 0x00: setD(fd, getD(fs) + getD(ft)); return;
    case 0x01: setD(fd, getD(fs) - getD(ft)); return;
    case 0x02: setD(fd, getD(fs) * getD(ft)); return;
    case 0x03: setD(fd, getD(fs) / getD(ft)); return;
    case 0x04: setD(fd, std::sqrt(getD(fs))); return;
    case 0x05: setL(fd, fpr[fs] & 0x7fff'ffff'ffff'ffffull); return;   // ABS.D
    case 0x06: setL(fd, fpr[fs]); return;                 // MOV.D
    case 0x07: setL(fd, fpr[fs] ^ 0x8000'0000'0000'0000ull); return;   // NEG.D
    case 0x08: cvtInt(getD(fs), mx::RN, true);  return; // ROUND.L.D
    case 0x09: cvtInt(getD(fs), mx::RZ, true); return; // TRUNC.L.D
    case 0x0a: cvtInt(getD(fs), mx::RP, true);     return; // CEIL.L.D
    case 0x0b: cvtInt(getD(fs), mx::RM, true);   return; // FLOOR.L.D
    case 0x0c: cvtInt(getD(fs), mx::RN, false); return; // ROUND.W.D
    case 0x0d: cvtInt(getD(fs), mx::RZ, false);return; // TRUNC.W.D
    case 0x0e: cvtInt(getD(fs), mx::RP, false);    return; // CEIL.W.D
    case 0x0f: cvtInt(getD(fs), mx::RM, false);  return; // FLOOR.W.D
    case 0x20: setS(fd, (float)getD(fs)); return;         // CVT.S.D
    case 0x24: cvtInt(getD(fs), rmHost(), false); return;      // CVT.W.D
    case 0x25: cvtInt(getD(fs), rmHost(), true);  return;      // CVT.L.D
    }
  } else if(fmt == 0x14) {  // W (int32 source)
    s32 v = (s32)(u32)fpr[fs];
    switch(fn) {
    case 0x20: setS(fd, (float)v); return;                // CVT.S.W
    case 0x21: setD(fd, (double)v); return;               // CVT.D.W
    }
  } else if(fmt == 0x15) {  // L (int64 source)
    s64 v = (s64)fpr[fs];
    // CVT.S.L / CVT.D.L only accept a 64-bit integer that fits in a signed 56-bit field
    // (i.e. bits [63:55] all equal the sign bit). Anything larger raises Unimplemented on
    // the VR4300 — the conversion hardware simply does not handle it. (W source is int32,
    // always in range, so it is not checked.)
    if((fn == 0x20 || fn == 0x21)) {
      s64 top = v >> 55;
      if(top != 0 && top != -1) { unimpl(); return; }
    }
    switch(fn) {
    case 0x20: setS(fd, (float)v); return;                // CVT.S.L
    case 0x21: setD(fd, (double)v); return;               // CVT.D.L
    }
  }
  // Invalid/reserved COP1 encoding (e.g. CVT.S.S, bad fmt): the VR4300 raises the
  // FP Unimplemented-Operation exception (FCSR Cause bit E=17), not a host halt.
  fcr31 = (fcr31 & ~0x0003'F000u) | (1u << 17);  // clear all Cause[17:12], set only E
  takeException(15);  // FPE
}

auto CPU::cop2op(u32 op) -> void {
  // COP2 has no functional unit on the VR4300 — it exists only as a set of move
  // targets backed by a single 64-bit latch. Accessing it requires Status.CU2
  // (bit 30); otherwise Coprocessor Unusable (ExcCode 11, CE=2). n64-systemtest
  // checks both the trap and the latch read-back behavior.
  if(!((u32)cop0[C0_Status] & 0x4000'0000u)) {
    takeException(11);
    cop0[C0_Cause] = sext32(((u32)cop0[C0_Cause] & ~0x3000'0000u) | (2u << 28));  // CE = 2
    return;
  }
  switch(RS) {
  case 0x00: /*MFC2*/  set(RT, sext32((u32)cp2latch)); return;
  case 0x01: /*DMFC2*/ set(RT, cp2latch); return;
  case 0x02: /*CFC2*/  set(RT, 0); return;   // CP2 control regs read 0
  case 0x04: /*MTC2*/  cp2latch = gpr[RT]; return;   // latches all 64 bits (MFC2 reads low 32 sext, DMFC2 reads full)
  case 0x05: /*DMTC2*/ cp2latch = gpr[RT]; return;
  case 0x06: /*CTC2*/  return;               // CP2 control regs are read-only latches
  default: break;
  }
  // Reserved COP2 sub-op (e.g. DCFC2/DCTC2): Reserved Instruction, with the Cause
  // CE field set to 2 (the coprocessor the faulting instruction targeted).
  takeException(10);
  cop0[C0_Cause] = sext32(((u32)cop0[C0_Cause] & ~0x3000'0000u) | (2u << 28));
}

// --- disassembler (compact, telemetry-only) ----------------------------------
static const char* gprName[32] = {
  "zero","at","v0","v1","a0","a1","a2","a3","t0","t1","t2","t3","t4","t5","t6","t7",
  "s0","s1","s2","s3","s4","s5","s6","s7","t8","t9","k0","k1","gp","sp","fp","ra"};

auto CPU::disasm(u32 op, u64 pc) -> std::string {
  // Telemetria, no una herramienta de matching: aqui solo importa que quien mira un cuelgue
  // LEA lo que hay. La tabla estaba a medias y un `sb` salia como "op.28", que es justo el
  // agujero que hace perder una hora en un diagnostico.
  char b[80];
  u32 o = op >> 26 & 0x3f, rs = op>>21&0x1f, rt = op>>16&0x1f, rd = op>>11&0x1f, sa = op>>6&0x1f, fn = op&0x3f;
  s16 imm = (s16)(op & 0xffff);
  auto R = [](u32 i){ return gprName[i]; };
  auto F = [](u32 i){ static char f[8]; std::snprintf(f,sizeof f,"f%u",i); return (const char*)f; };
  auto target = [&]{ return (u32)((pc & 0xf0000000) | ((op & 0x3ffffff) << 2)); };
  auto branch = [&]{ return (u32)(pc + 4 + ((s32)imm << 2)); };
  auto mem  = [&](const char* n){ std::snprintf(b,sizeof b,"%s %s,%d(%s)",n,R(rt),imm,R(rs)); return b; };
  auto memf = [&](const char* n){ std::snprintf(b,sizeof b,"%s %s,%d(%s)",n,F(rt),imm,R(rs)); return b; };
  if(op == 0) return "nop";
  switch(o) {
  case 0x00:
    switch(fn) {
    case 0x00: std::snprintf(b,sizeof b,"sll %s,%s,%u",R(rd),R(rt),sa); return b;
    case 0x02: std::snprintf(b,sizeof b,"srl %s,%s,%u",R(rd),R(rt),sa); return b;
    case 0x03: std::snprintf(b,sizeof b,"sra %s,%s,%u",R(rd),R(rt),sa); return b;
    case 0x04: std::snprintf(b,sizeof b,"sllv %s,%s,%s",R(rd),R(rt),R(rs)); return b;
    case 0x06: std::snprintf(b,sizeof b,"srlv %s,%s,%s",R(rd),R(rt),R(rs)); return b;
    case 0x07: std::snprintf(b,sizeof b,"srav %s,%s,%s",R(rd),R(rt),R(rs)); return b;
    case 0x08: std::snprintf(b,sizeof b,"jr %s",R(rs)); return b;
    case 0x09: std::snprintf(b,sizeof b,"jalr %s,%s",R(rd),R(rs)); return b;
    case 0x0c: return "syscall";
    case 0x0d: return "break";
    case 0x0f: return "sync";
    case 0x10: std::snprintf(b,sizeof b,"mfhi %s",R(rd)); return b;
    case 0x11: std::snprintf(b,sizeof b,"mthi %s",R(rs)); return b;
    case 0x12: std::snprintf(b,sizeof b,"mflo %s",R(rd)); return b;
    case 0x13: std::snprintf(b,sizeof b,"mtlo %s",R(rs)); return b;
    case 0x14: std::snprintf(b,sizeof b,"dsllv %s,%s,%s",R(rd),R(rt),R(rs)); return b;
    case 0x16: std::snprintf(b,sizeof b,"dsrlv %s,%s,%s",R(rd),R(rt),R(rs)); return b;
    case 0x17: std::snprintf(b,sizeof b,"dsrav %s,%s,%s",R(rd),R(rt),R(rs)); return b;
    case 0x18: case 0x19: std::snprintf(b,sizeof b,"mult%s %s,%s",fn==0x19?"u":"",R(rs),R(rt)); return b;
    case 0x1a: case 0x1b: std::snprintf(b,sizeof b,"div%s %s,%s",fn==0x1b?"u":"",R(rs),R(rt)); return b;
    case 0x1c: case 0x1d: std::snprintf(b,sizeof b,"dmult%s %s,%s",fn==0x1d?"u":"",R(rs),R(rt)); return b;
    case 0x1e: case 0x1f: std::snprintf(b,sizeof b,"ddiv%s %s,%s",fn==0x1f?"u":"",R(rs),R(rt)); return b;
    case 0x20: case 0x21: std::snprintf(b,sizeof b,"add%s %s,%s,%s",fn==0x21?"u":"",R(rd),R(rs),R(rt)); return b;
    case 0x22: case 0x23: std::snprintf(b,sizeof b,"sub%s %s,%s,%s",fn==0x23?"u":"",R(rd),R(rs),R(rt)); return b;
    case 0x24: std::snprintf(b,sizeof b,"and %s,%s,%s",R(rd),R(rs),R(rt)); return b;
    case 0x25: std::snprintf(b,sizeof b,"or %s,%s,%s",R(rd),R(rs),R(rt)); return b;
    case 0x26: std::snprintf(b,sizeof b,"xor %s,%s,%s",R(rd),R(rs),R(rt)); return b;
    case 0x27: std::snprintf(b,sizeof b,"nor %s,%s,%s",R(rd),R(rs),R(rt)); return b;
    case 0x2a: std::snprintf(b,sizeof b,"slt %s,%s,%s",R(rd),R(rs),R(rt)); return b;
    case 0x2b: std::snprintf(b,sizeof b,"sltu %s,%s,%s",R(rd),R(rs),R(rt)); return b;
    case 0x2c: case 0x2d: std::snprintf(b,sizeof b,"dadd%s %s,%s,%s",fn==0x2d?"u":"",R(rd),R(rs),R(rt)); return b;
    case 0x2e: case 0x2f: std::snprintf(b,sizeof b,"dsub%s %s,%s,%s",fn==0x2f?"u":"",R(rd),R(rs),R(rt)); return b;
    case 0x38: std::snprintf(b,sizeof b,"dsll %s,%s,%u",R(rd),R(rt),sa); return b;
    case 0x3a: std::snprintf(b,sizeof b,"dsrl %s,%s,%u",R(rd),R(rt),sa); return b;
    case 0x3b: std::snprintf(b,sizeof b,"dsra %s,%s,%u",R(rd),R(rt),sa); return b;
    case 0x3c: std::snprintf(b,sizeof b,"dsll32 %s,%s,%u",R(rd),R(rt),sa); return b;
    case 0x3e: std::snprintf(b,sizeof b,"dsrl32 %s,%s,%u",R(rd),R(rt),sa); return b;
    case 0x3f: std::snprintf(b,sizeof b,"dsra32 %s,%s,%u",R(rd),R(rt),sa); return b;
    default: std::snprintf(b,sizeof b,"special.%02x",fn); return b;
    }
  case 0x01: {  // REGIMM: el sub-opcode va en el campo rt
    static const char* rn[32] = {
      "bltz","bgez","bltzl","bgezl","?","?","?","?","tgei","tgeiu","tlti","tltiu","teqi","?","tnei","?",
      "bltzal","bgezal","bltzall","bgezall","?","?","?","?","?","?","?","?","?","?","?","?"};
    std::snprintf(b,sizeof b,"%s %s,%08x",rn[rt],R(rs),branch()); return b;
  }
  case 0x02: std::snprintf(b,sizeof b,"j %08x",target()); return b;
  case 0x03: std::snprintf(b,sizeof b,"jal %08x",target()); return b;
  case 0x04: std::snprintf(b,sizeof b,"beq %s,%s,%08x",R(rs),R(rt),branch()); return b;
  case 0x05: std::snprintf(b,sizeof b,"bne %s,%s,%08x",R(rs),R(rt),branch()); return b;
  case 0x06: std::snprintf(b,sizeof b,"blez %s,%08x",R(rs),branch()); return b;
  case 0x07: std::snprintf(b,sizeof b,"bgtz %s,%08x",R(rs),branch()); return b;
  case 0x08: case 0x09: std::snprintf(b,sizeof b,"addi%s %s,%s,%d",o==0x09?"u":"",R(rt),R(rs),imm); return b;
  case 0x0a: std::snprintf(b,sizeof b,"slti %s,%s,%d",R(rt),R(rs),imm); return b;
  case 0x0b: std::snprintf(b,sizeof b,"sltiu %s,%s,%d",R(rt),R(rs),imm); return b;
  case 0x0c: std::snprintf(b,sizeof b,"andi %s,%s,0x%x",R(rt),R(rs),(u16)imm); return b;
  case 0x0d: std::snprintf(b,sizeof b,"ori %s,%s,0x%x",R(rt),R(rs),(u16)imm); return b;
  case 0x0e: std::snprintf(b,sizeof b,"xori %s,%s,0x%x",R(rt),R(rs),(u16)imm); return b;
  case 0x0f: std::snprintf(b,sizeof b,"lui %s,0x%x",R(rt),(u16)imm); return b;
  case 0x10:   // COP0
    switch(rs) {
    case 0x00: std::snprintf(b,sizeof b,"mfc0 %s,r%u",R(rt),rd); return b;
    case 0x01: std::snprintf(b,sizeof b,"dmfc0 %s,r%u",R(rt),rd); return b;
    case 0x04: std::snprintf(b,sizeof b,"mtc0 %s,r%u",R(rt),rd); return b;
    case 0x05: std::snprintf(b,sizeof b,"dmtc0 %s,r%u",R(rt),rd); return b;
    case 0x10:
      switch(fn) {
      case 0x01: return "tlbr";
      case 0x02: return "tlbwi";
      case 0x06: return "tlbwr";
      case 0x08: return "tlbp";
      case 0x18: return "eret";
      default:   std::snprintf(b,sizeof b,"cop0.co.%02x",fn); return b;
      }
    default: std::snprintf(b,sizeof b,"cop0.%02x",rs); return b;
    }
  case 0x11:   // COP1
    switch(rs) {
    case 0x00: std::snprintf(b,sizeof b,"mfc1 %s,%s",R(rt),F(rd)); return b;
    case 0x01: std::snprintf(b,sizeof b,"dmfc1 %s,%s",R(rt),F(rd)); return b;
    case 0x02: std::snprintf(b,sizeof b,"cfc1 %s,%u",R(rt),rd); return b;
    case 0x04: std::snprintf(b,sizeof b,"mtc1 %s,%s",R(rt),F(rd)); return b;
    case 0x05: std::snprintf(b,sizeof b,"dmtc1 %s,%s",R(rt),F(rd)); return b;
    case 0x06: std::snprintf(b,sizeof b,"ctc1 %s,%u",R(rt),rd); return b;
    case 0x08: std::snprintf(b,sizeof b,"bc1%s%s %08x",(rt&1)?"t":"f",(rt&2)?"l":"",branch()); return b;
    default: {
      static const char* fmt[32] = {"s","d","?","?","w","l","?","?","?","?","?","?","?","?","?","?",
                                    "?","?","?","?","?","?","?","?","?","?","?","?","?","?","?","?"};
      static const char* fop[64] = {
        "add","sub","mul","div","sqrt","abs","mov","neg","round.l","trunc.l","ceil.l","floor.l",
        "round.w","trunc.w","ceil.w","floor.w","?","?","?","?","?","?","?","?","?","?","?","?","?","?","?","?",
        "cvt.s","cvt.d","?","?","cvt.w","cvt.l","?","?","?","?","?","?","?","?","?","?",
        "c.f","c.un","c.eq","c.ueq","c.olt","c.ult","c.ole","c.ule",
        "c.sf","c.ngle","c.seq","c.ngl","c.lt","c.nge","c.le","c.ngt"};
      std::snprintf(b,sizeof b,"%s.%s %s,%s,%s",fop[fn],fmt[rs&0x1f],F(sa),F(rd),F(rt)); return b;
    }
    }
  case 0x14: std::snprintf(b,sizeof b,"beql %s,%s,%08x",R(rs),R(rt),branch()); return b;
  case 0x15: std::snprintf(b,sizeof b,"bnel %s,%s,%08x",R(rs),R(rt),branch()); return b;
  case 0x16: std::snprintf(b,sizeof b,"blezl %s,%08x",R(rs),branch()); return b;
  case 0x17: std::snprintf(b,sizeof b,"bgtzl %s,%08x",R(rs),branch()); return b;
  case 0x18: case 0x19: std::snprintf(b,sizeof b,"daddi%s %s,%s,%d",o==0x19?"u":"",R(rt),R(rs),imm); return b;
  case 0x1a: return mem("ldl");
  case 0x1b: return mem("ldr");
  case 0x20: return mem("lb");
  case 0x21: return mem("lh");
  case 0x22: return mem("lwl");
  case 0x23: return mem("lw");
  case 0x24: return mem("lbu");
  case 0x25: return mem("lhu");
  case 0x26: return mem("lwr");
  case 0x27: return mem("lwu");
  case 0x28: return mem("sb");
  case 0x29: return mem("sh");
  case 0x2a: return mem("swl");
  case 0x2b: return mem("sw");
  case 0x2c: return mem("sdl");
  case 0x2d: return mem("sdr");
  case 0x2e: return mem("swr");
  case 0x2f: std::snprintf(b,sizeof b,"cache 0x%x,%d(%s)",rt,imm,R(rs)); return b;
  case 0x30: return mem("ll");
  case 0x31: return memf("lwc1");
  case 0x34: return mem("lld");
  case 0x35: return memf("ldc1");
  case 0x37: return mem("ld");
  case 0x38: return mem("sc");
  case 0x39: return memf("swc1");
  case 0x3c: return mem("scd");
  case 0x3d: return memf("sdc1");
  case 0x3f: return mem("sd");
  default: std::snprintf(b,sizeof b,"op.%02x",o); return b;
  }
}

}  // namespace kestrel
