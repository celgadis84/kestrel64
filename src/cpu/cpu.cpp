#include "../core/wrtag.hpp"
#include "cpu.hpp"
#include "jit.hpp"          // CodeCache completo: cacheOp desenlaza las cadenas del dynarec
#include "../core/memory.hpp"
#include "cpu_internal.hpp"
#include "../core/cic.hpp"
#include "../video/vifilter.hpp"
#include "../video/capture.hpp"
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
namespace jit { extern u64 g_idleWhy[8]; extern u64 g_idleLim[5], g_idleLimOps[5]; }   // motivos del bucle ocioso, ver jitIdleSkip


// Interruptores de entorno leídos UNA vez al arranque. Antes vivían como `static` locales
// dentro de step()/dcWrite(): en C++ eso obliga a comprobar la variable-guarda de
// inicialización en CADA ejecución de la sentencia, es decir por instrucción emulada.
// A escala de decenas de millones de instrucciones por segundo eso es coste puro; en
// ámbito de namespace la inicialización dinámica ocurre una sola vez antes de main().
static const bool g_fastFetch       = !std::getenv("KESTREL_NOFETCHFAST");
static const bool g_intLog          = std::getenv("KESTREL_INTLOG")      != nullptr;
static const bool g_jlogNoSpin      = std::getenv("KESTREL_JLOG_NOSPIN") != nullptr;
const bool g_dcWriteThrough  = std::getenv("KESTREL_DCWT")        != nullptr;
// Barrera de escritura: ver Memory::cpuRamWrBarrier. Apagada = una comparacion contra un bool.
static const bool g_wrBarrier       = [] { const char* e = std::getenv("KESTREL_WRBARRIER");
                                          if(!e || !*e) return true;
                                          return (bool)(std::strcmp(e, "0") && std::strcmp(e, "off")); }();

// Depuracion: anillo de transiciones de UNA palabra de RDRAM que elige el usuario
// (`KESTREL_SEENADDR=<fisica>`), volcado junto al veredicto de un xlog con
// KESTREL_LEAKDUMP. Apagado de fabrica: sin la variable no se mira ni una escritura, y
// ninguna direccion de ninguna ROM concreta vive en el nucleo.
static u32 g_seenPrev = 0;
u32 g_seenAddr = 0;   // 0 = apagado; se fija una vez desde KESTREL_SEENADDR
static constexpr int kSeenRing = 24;
static u64 g_seenRet[kSeenRing] = {};
static u32 g_seenOld[kSeenRing] = {}, g_seenNew[kSeenRing] = {};
static int g_seenIdx = 0;

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
  if(wrtag::tag) {
    static CPU* self; self = this;
    wrtag::dirtyProbe = [](uint32_t phys) -> bool {
      const DCacheLine& l = self->dcache[(phys >> 4) & 0x1ff];
      return l.tagv == ((phys & ~0xfu) | 1u) && l.dirty;
    };
  }
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

// Recompute the single hot-path debug guard from the individual trap flags plus
// the pc-window/pc-sample env vars (which are read lazily inside the prologue).
auto CPU::refreshDebugArmed() -> void {
  debugArmed = bpAddr || bpTrace || trapWild || excTail ||
               audioHook || maxInsn || pcRingOn || profOn || g_intLog ||
               (mem && mem->trapSpRegStore) ||
               std::getenv("KESTREL_PCLO") || std::getenv("KESTREL_PCSAMPLE");
}

// --- memory (segment rules + TLB translation via translate()) ----------------
// Cached data accesses route through the write-back D-cache; uncached (KSEG1) and
// non-RDRAM targets go straight to the bus. `pe` is the reverse-endian-adjusted phys.
auto CPU::read8 (u64 v) -> u8  { u64 p=xlat(v,AccRead); if(memAbort||!mem) return 0; if(__builtin_expect(watchArmed,0) && watchTrip(p,AccRead)) return 0; u32 pe=(u32)reXor(p,1); if(cacheable(v)&&pe<mem->rdram.size()) return (u8)dcRead(pe,1); return uncachedRead(pe, 1, [&]{ return mem->read8(pe); }); }
auto CPU::read16(u64 v) -> u16 { if(alignBad(v,2,AccRead)) return 0; u64 p=xlat(v,AccRead); if(memAbort||!mem) return 0; if(__builtin_expect(watchArmed,0) && watchTrip(p,AccRead)) return 0; u32 pe=(u32)reXor(p,2); if(cacheable(v)&&pe<mem->rdram.size()) return (u16)dcRead(pe,2); return uncachedRead(pe, 2, [&]{ return mem->read16(pe); }); }
auto CPU::read32(u64 v) -> u32 { if(alignBad(v,4,AccRead)) return 0; u64 p=xlat(v,AccRead); if(memAbort||!mem) return 0; if(__builtin_expect(watchArmed,0) && watchTrip(p,AccRead)) return 0; u32 pe=(u32)reXor(p,4); if(cacheable(v)&&pe<mem->rdram.size()) return (u32)dcRead(pe,4); return uncachedRead(pe, 4, [&]{ return mem->read32(pe); }); }
auto CPU::read64(u64 v) -> u64 { if(alignBad(v,8,AccRead)) return 0; u64 p=xlat(v,AccRead); if(memAbort||!mem) return 0; if(__builtin_expect(watchArmed,0) && watchTrip(p,AccRead)) return 0; u32 pe=(u32)p; if(cacheable(v)&&pe<mem->rdram.size()) return dcRead(pe,8); return uncachedRead(pe, 8, [&]{ return mem->read64(pe); }); }
auto CPU::write8 (u64 v, u8  x) -> void { u64 p=xlat(v,AccWrite); if(memAbort||!mem) return; if(__builtin_expect(watchArmed,0) && watchTrip(p,AccWrite)) return; u32 pe=(u32)reXor(p,1); if(cacheable(v)&&pe<mem->rdram.size()){ dcWrite(pe,x,1); return; } ramSettle(pe,1); ramUncached(pe,1); mem->write8 (pe, x); }
auto CPU::write16(u64 v, u16 x) -> void { if(alignBad(v,2,AccWrite)) return; u64 p=xlat(v,AccWrite); if(memAbort||!mem) return; if(__builtin_expect(watchArmed,0) && watchTrip(p,AccWrite)) return; u32 pe=(u32)reXor(p,2); if(cacheable(v)&&pe<mem->rdram.size()){ dcWrite(pe,x,2); return; } ramSettle(pe,2); ramUncached(pe,2); mem->write16(pe, x); }
auto CPU::seenWatch(u64 p, u32 size) -> void {
  if(!g_seenAddr || !mem || !(p <= g_seenAddr && p + size > g_seenAddr)) return;
  u32 cur = mem->read32(g_seenAddr);
  if(cur != g_seenPrev) { int i=g_seenIdx%kSeenRing; g_seenRet[i]=retired; g_seenOld[i]=g_seenPrev; g_seenNew[i]=cur; g_seenIdx++; g_seenPrev=cur; }
}
auto CPU::write32(u64 v, u32 x) -> void { if(alignBad(v,4,AccWrite)) return; u64 p=xlat(v,AccWrite); if(memAbort||!mem) return; if(__builtin_expect(watchArmed,0) && watchTrip(p,AccWrite)) return; u32 pe=(u32)reXor(p,4); if(cacheable(v)&&pe<mem->rdram.size()) dcWrite(pe,x,4); else { ramSettle(pe,4); ramUncached(pe,4); mem->write32(pe, x); } seenWatch(p&0x1fffffff,4); }
auto CPU::write64(u64 v, u64 x) -> void { if(alignBad(v,8,AccWrite)) return; u64 p=xlat(v,AccWrite); if(memAbort||!mem) return; if(__builtin_expect(watchArmed,0) && watchTrip(p,AccWrite)) return; u32 pe=(u32)p; if(cacheable(v)&&pe<mem->rdram.size()) dcWrite(pe,x,8); else { ramSettle(pe,8); ramUncached(pe,8); mem->write64(pe, x); } seenWatch(p&0x1fffffff,8); }

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
  if(!mem) return;
  if(pe < (u32)mem->rdram.size()) {
    ramCpuBytes += size;
    // Escritura no cacheada: llega a RDRAM ya, asi que no puede adelantar al RSP.
    if(g_wrBarrier) mem->cpuRamWrBarrier(mem->cartNow());
    return;
  }
  // DMEM/IMEM: el otro sitio por el que una escritura de la CPU es visible al RSP EN EL ACTO.
  // No pasa por RDRAM ni por diario -- Memory::resolve la manda directa al vector -- asi que
  // necesita la misma cita que un volcado de linea sucia. Ver Memory::cpuRamWrBarrier.
  if(g_wrBarrier && pe >= 0x0400'0000u && pe < 0x0404'0000u) mem->cpuRamWrBarrier(mem->cartNow());
}

// Antes de un acceso no cacheado (lectura o store): un DMA del diario sobre el tramo con instante
// <= el nuestro cae ANTES. En un store, aplicado despues se comeria la escritura. Va antes de
// cobrar la parada (ver uncachedRead), por eso no lo hace ramUncached.
auto CPU::ramSettle(u32 pe, u32 size) -> void {
  if(mem && pe < (u32)mem->rdram.size()) mem->dmaSettle(pe, (u64)pe + size);
}

auto CPU::dcFill(u32 idx, u32 base) -> void {
  DCacheLine& l = dcache[idx];
  mem->dmaSettle(base, (u64)base + 16);
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
  // Unica via por la que un store CACHEADO llega a RDRAM, o sea el unico punto en el que la
  // CPU puede ensenarle al RSP un byte de su futuro. Ver Memory::cpuRamWrBarrier.
  if(g_wrBarrier) mem->cpuRamWrBarrier(mem->cartNow());
  mem->dmaSettle(tag, (u64)tag + 16);
  // El volcado de una linea sucia es la unica forma en que un store CACHEADO de la CPU
  // llega a RDRAM, asi que sin esto KESTREL_WATCH no ve el 99% de lo que escribe el juego.
  wrtag::mark(tag, wrtag::kDcache, (u32)pc, false);
  if(tag + 16 <= mem->rdram.size()) wrtag::fifoCheckLine(tag, &mem->rdram[tag], l.data, wrtag::kDcache, (u32)pc);
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
  if(__builtin_expect(profOn, 0) && !profDMiss.empty()) {   // P8: fallos de D$ por PC
    profDMiss[profCurPhys >> kProfShift]++;
    if(base < (8u << 20)) profDLine[base >> 4]++;
    profDMissTotal++;
  }
  chargeDcMiss();   // el relleno de linea paga la latencia de RDRAM (ver countTicks)
  DCacheLine& l = dcache[idx];
  u8* const ram = mem->rdram.data();
  const u32  sz = (u32)mem->rdram.size();
  ramCpuBytes += 16;                 // relleno de la linea nueva
  mem->dmaSettle(base, (u64)base + 16);
  if(l.dirty && l.valid()) {
    ramCpuBytes += 16;               // ... mas el volcado de la vieja
    u32 tag = l.ptag();
    mem->dmaSettle(tag, (u64)tag + 16);
    wrtag::mark(tag, wrtag::kDcache, (u32)pc, false);
    if(tag + 16 <= sz) wrtag::fifoCheckLine(tag, ram + tag, l.data, wrtag::kDcache, (u32)pc);
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
auto CPU::hostPoke(u32 phys, const u8* src, u32 n, bool code) -> u32 {
  if(!mem) return 0;
  const u32 sz = (u32)mem->rdram.size();
  phys &= 0x1fff'ffffu;
  if(phys >= sz) return 0;
  if(n > sz - phys) n = sz - phys;
  std::memcpy(mem->rdram.data() + phys, src, n);
  for(u32 a = phys; a < phys + n; ) {
    // D$: linea de 16 B, tag fisico; I$: linea de 32 B. Se copian los bytes del tramo que
    // caigan en la linea residente; una linea que no esta no hace falta tocarla.
    DCacheLine& d = dcache[(a >> 4) & 0x1ff];
    if(d.tagv == ((a & ~0xfu) | 1u)) d.data[a & 0xf] = mem->rdram[a];
    ICacheLine& ic = icache[(a >> 5) & 0x1ff];
    if(ic.valid && ic.ptag == (a & ~0x1fu)) ic.data[a & 0x1f] = mem->rdram[a];
    a++;
  }
  if(code) {
    if(jitCache) jitCache->clear();
    jitTlbValid = false;
    fetchLineVBase = 1;
    bumpXlat();
  }
  return n;
}

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
  if(__builtin_expect(profOn, 0) && !profIMiss.empty()) {   // P8: fallos de I$ por PC
    profIMiss[profCurPhys >> kProfShift]++;
    profIMissTotal++;
  }
  mem->dmaSettle(base, (u64)base + 32);
  ramCpuBytes += 32;
  chargeIcMiss();   // idem, con su propio coste: la linea de I son 8 palabras
  ICacheLine& l = icache[idx];
  l.ptag = base; l.valid = true; l.seq = ++icSeq;
  if(base + 32 <= mem->rdram.size()) std::memcpy(l.data, &mem->rdram[base], 32);
  else for(u32 i = 0; i < 32; i++) l.data[i] = (base + i < mem->rdram.size()) ? mem->rdram[base + i] : 0;
}

auto CPU::icFetch(u32 phys) -> u32 {
  if(__builtin_expect(cacheStat, 0)) icAccess++;
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
        if(l.valid && l.ptag == base) {
          mem->dmaSettle(l.ptag, (u64)l.ptag + 32);
          for(u32 i = 0; i < 32; i++) if(l.ptag + i < mem->rdram.size()) mem->rdram[l.ptag + i] = l.data[i];
        }
        break;
      default: break;
    }
  }
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
      // KESTREL_STATEDUMP=1: el estado arquitectonico entero, una linea por registro. El
      // statehash dice SI dos modos llegaron al mismo sitio; esto dice EN QUE se separaron,
      // que es lo unico util cuando Lockstep y Threaded no cuadran.
      if(std::getenv("KESTREL_STATEDUMP")) {
        for(int r = 0; r < 32; r++)
          std::fprintf(stderr, "[sd] gpr%-2d %016llx\n", r, (unsigned long long)gpr[r]);
        for(int r = 0; r < 32; r++)
          std::fprintf(stderr, "[sd] cp0%-2d %016llx\n", r, (unsigned long long)cop0[r]);
        for(int r = 0; r < 32; r++)
          std::fprintf(stderr, "[sd] fpr%-2d %016llx\n", r, (unsigned long long)fpr[r]);
        std::fprintf(stderr, "[sd] cnt ops=%llu ret=%llu frac=%u stall=%llu\n",
                     (unsigned long long)countOpsTot, (unsigned long long)retired,
                     countFrac, (unsigned long long)stallTotal);
        // Historial de las ultimas excepciones. Con el statehash solo se ve QUE campo cambio;
        // esto dice en que retiro cambio la ENTREGA, que es lo unico que separa dos corridas
        // del mismo binario cuando la trayectoria es identica. Pide KESTREL_EXCTAIL.
        if(excTail) {
          u32 n = excRingIdx < (u32)kExcRing ? excRingIdx : (u32)kExcRing;
          u32 start = excRingIdx >= (u32)kExcRing ? excRingIdx - kExcRing : 0;
          for(u32 k = 0; k < n; k++) {
            u32 i = (start + k) % kExcRing;
            std::fprintf(stderr, "[sdexc] code=%u epc=%016llx ret=%llu\n", excRingCode[i],
                         (unsigned long long)excRingEpc[i], (unsigned long long)excRingRet[i]);
          }
        }
        std::fprintf(stderr, "[sd] pc %016llx nextPc %016llx hi %016llx lo %016llx\n",
                     (unsigned long long)pc, (unsigned long long)nextPc,
                     (unsigned long long)hi, (unsigned long long)lo);
      }
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
                           " dpcRd=%u/%u rsp=%u open=%u busy=%u endv=%u await=%u dmaW=%llu lateMax=%llu waiv=%u/%u wv=%u/%u/%u ooo=%u(C%u/R%u) stale=%u(C%u/R%u) rdv=%llu/%u idle=%llu/%llu ciclos=%lluM(rdp%lluM/cpu%lluM sig%llu/drn%llu/room%llu) park=%u/%u/%u ocioso=%llu/%llu/%llu/%llu/%llu/%llu/%lluM limite=%llu:%llu/%llu:%llu/%llu:%llu/%llu:%llu/%llu:%llu\n",
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
            (unsigned long long)(mem->rsp.idleCyc.load() / 1000000),
            (unsigned long long)(mem->rsp.idleCycRdp / 1000000),
            (unsigned long long)(mem->rsp.idleCycCpu / 1000000),
            (unsigned long long)mem->rsp.idleNoSig,
            (unsigned long long)mem->rsp.idleNoDrain,
            (unsigned long long)mem->rsp.idleNoRoom,
            mem->rspParks.load(), mem->rspParkWv.load(), mem->rspParkMiss.load(),
            (unsigned long long)jit::g_idleWhy[0], (unsigned long long)jit::g_idleWhy[1],
            (unsigned long long)jit::g_idleWhy[2], (unsigned long long)jit::g_idleWhy[3],
            (unsigned long long)jit::g_idleWhy[4], (unsigned long long)jit::g_idleWhy[6],
            (unsigned long long)(jit::g_idleWhy[7] / 1000000),
            (unsigned long long)jit::g_idleLim[0], (unsigned long long)(jit::g_idleLimOps[0]/(jit::g_idleLim[0]?jit::g_idleLim[0]:1)),
            (unsigned long long)jit::g_idleLim[1], (unsigned long long)(jit::g_idleLimOps[1]/(jit::g_idleLim[1]?jit::g_idleLim[1]:1)),
            (unsigned long long)jit::g_idleLim[2], (unsigned long long)(jit::g_idleLimOps[2]/(jit::g_idleLim[2]?jit::g_idleLim[2]:1)),
            (unsigned long long)jit::g_idleLim[3], (unsigned long long)(jit::g_idleLimOps[3]/(jit::g_idleLim[3]?jit::g_idleLim[3]:1)),
            (unsigned long long)jit::g_idleLim[4], (unsigned long long)(jit::g_idleLimOps[4]/(jit::g_idleLim[4]?jit::g_idleLim[4]:1)));
    // Antes de mirar el framebuffer hay que dejar quieto al RCP. En modo threaded el
    // hilo del RDP puede tener la lista de comandos todavia sin consumir cuando la CPU
    // llega al tope de instrucciones: el volcado saldria de un frame a medio pintar, o
    // directamente vacio. Con el dynarec la CPU quema el tope tan rapido que el RDP no
    // ha empezado siquiera, y con GPU-RDP (que ademas paga arranque de GPU) la
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
  return false;
}

auto CPU::profEnable(bool on) -> void {
  if(on && profBuckets.empty()) {
    profBuckets.assign(kProfBuckets, 0);
    profDMiss.assign(kProfBuckets, 0);
    profIMiss.assign(kProfBuckets, 0);
    profDLine.assign(kProfBuckets, 0);
  }
  if(on) profClear();
  profOn = on;
  refreshDebugArmed();   // el muestreador cuelga de la guarda unica del prologo
}

auto CPU::profClear() -> void {
  for(auto* v : {&profBuckets, &profDMiss, &profIMiss, &profDLine})
    if(!v->empty()) std::fill(v->begin(), v->end(), 0u);
  profTotal = profDMissTotal = profIMissTotal = 0;
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
    if(debugArmed && g_intLog) {
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
  if(debugArmed && profOn) {   // physical-PC hotpath sampler (MCP prof.*)
    u32 pp = fpe & 0x1fff'ffff;
    if(pp < (8u << 20)) { profBuckets[pp >> kProfShift]++; profTotal++; profCurPhys = pp; }
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
  if(debugArmed && mem && mem->pendingTrap) {
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
  if(fpTrace && (excCode==15 || excCode==11)) {
    std::fprintf(stderr, "[E%u] epc=0x%08x bd=%d exl=%d fcr31=0x%08x ra=0x%08x sp=0x%08x ret=%llu\n",
                 excCode, (u32)epc, bd?1:0, exl?1:0, fcr31, (u32)gpr[31], (u32)gpr[29], (unsigned long long)retired);
    std::fflush(stderr);
    fpTraceEret = (excCode==15) ? 2 : 1;
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
    // Status.ERL=1 (kernel, tras un Reset/NMI/error de cache): kuseg deja de pasar por la TLB
    // y queda como 2 GiB directos y sin cache, virtual = fisica (VR4300 UM 5.2.4).
    if(seg <= 3 && (status & 0x4)) { xlatCacheable = false; return va; }
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
    if((status & 0x4) && vaddr < 0x8000'0000ull) { xlatCacheable = false; return vaddr; } // ERL: xkuseg bajo directo
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
      { u32 pe=(u32)reXor(p,1); if(cacheable(a)&&pe<mem->rdram.size()){ dcWrite(pe,gpr[RT],1); break; } ramSettle(pe,1); ramUncached(pe,1); mem->write8(pe,(u8)gpr[RT]); } } break;
  case 0x29: /*SH*/ { u64 a=gpr[RS]+SIMM; if(alignBad(a,2,AccWrite)) break; u64 p=xlat(a,AccWrite); if(memAbort||!mem) break;
      if(__builtin_expect(watchArmed,0) && watchTrip(p,AccWrite)) break;
      if(storeRepeat((u32)p&0x1fff'ffff, gpr[RT], 2)) break;
      if(storeCart((u32)p&0x1fff'ffff, gpr[RT], 2)) break;
      if(mem->wordStoreQuirk((u32)p&0x1fff'ffff, gpr[RT], 2)) break;
      { u32 pe=(u32)reXor(p,2); if(cacheable(a)&&pe<mem->rdram.size()){ dcWrite(pe,gpr[RT],2); break; } ramSettle(pe,2); ramUncached(pe,2); mem->write16(pe,(u16)gpr[RT]); } } break;
  case 0x2a: /*SWL*/ { u64 a=gpr[RS]+SIMM; xlat(a,AccWrite); if(memAbort) break;   // store: a TLB/addr fault here is TLBS/AdES, not the load flavor
      u32 s=(u32)(reOn()?(3-(a&3)):(a&3))*8; u32 d=readNoWatch32(a&~3ull); u32 m = s? (~0u>>s):~0u; write32(a&~3ull, (d & ~(m>>0)) | ((u32)gpr[RT]>>s)); break; }
  case 0x2b: /*SW*/ { u64 a=gpr[RS]+SIMM; if(alignBad(a,4,AccWrite)) break; u64 p=xlat(a,AccWrite); if(memAbort||!mem) break;
      if(__builtin_expect(watchArmed,0) && watchTrip(p,AccWrite)) break;
      if(storeRepeat((u32)p&0x1fff'ffff, gpr[RT], 4)) break;
      { u32 pe=(u32)reXor(p,4); if(cacheable(a)&&pe<mem->rdram.size()){ dcWrite(pe,gpr[RT],4); break; } ramSettle(pe,4); ramUncached(pe,4); mem->write32(pe, (u32)gpr[RT]); } } break;
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
  case 0x30: { /*LL*/  u64 va=gpr[RS]+SIMM; if(alignBad(va,4,AccRead)) break; u64 pa=xlat(va,AccRead); if(memAbort||!mem) break;
              if(__builtin_expect(watchArmed,0) && watchTrip(pa,AccRead)) break;
              u32 pe=(u32)reXor(pa,4); u32 val = (cacheable(va)&&pe<mem->rdram.size()) ? (u32)dcRead(pe,4) : uncachedRead(pe, 4, [&]{ return mem->read32(pe); });
              set(RT, sext32(val)); cop0[17]=(u32)(pa>>4); llbit=true; } break;  // LLAddr = phys>>4
  case 0x31: /*LWC1*/ if(!((u32)cop0[C0_Status]&0x2000'0000u)){copUnusable(1);break;} fprSet32(RT, read32(gpr[RS]+SIMM)); break;
  case 0x34: { /*LLD*/ u64 va=gpr[RS]+SIMM; if(alignBad(va,8,AccRead)) break; u64 pa=xlat(va,AccRead); if(memAbort||!mem) break;
              if(__builtin_expect(watchArmed,0) && watchTrip(pa,AccRead)) break;
              u32 pe=(u32)pa; u64 val = (cacheable(va)&&pe<mem->rdram.size()) ? dcRead(pe,8) : uncachedRead(pe, 8, [&]{ return mem->read64(pe); });
              set(RT, val); cop0[17]=(u32)(pa>>4); llbit=true; } break;  // LLAddr = phys>>4
  case 0x35: /*LDC1*/ if(!((u32)cop0[C0_Status]&0x2000'0000u)){copUnusable(1);break;} fprSet64(RT, read64(gpr[RS]+SIMM)); break;
  case 0x37: /*LD*/  set(RT, read64(gpr[RS]+SIMM)); break;
  case 0x38: /*SC*/  if(alignBad(gpr[RS]+SIMM,4,AccWrite)) break; if(llbit) write32(gpr[RS]+SIMM, (u32)gpr[RT]); set(RT, llbit?1:0); break;
  case 0x39: /*SWC1*/ if(!((u32)cop0[C0_Status]&0x2000'0000u)){copUnusable(1);break;} write32(gpr[RS]+SIMM, fprGet32(RT)); break;
  case 0x3c: /*SCD*/ if(alignBad(gpr[RS]+SIMM,8,AccWrite)) break; if(llbit) write64(gpr[RS]+SIMM, gpr[RT]); set(RT, llbit?1:0); break;
  case 0x3d: /*SDC1*/ if(!((u32)cop0[C0_Status]&0x2000'0000u)){copUnusable(1);break;} write64(gpr[RS]+SIMM, fprGet64(RT)); break;
  case 0x3f: /*SD*/ { u64 a=gpr[RS]+SIMM; if(alignBad(a,8,AccWrite)) break; u64 p=xlat(a,AccWrite); if(memAbort||!mem) break;
      if(__builtin_expect(watchArmed,0) && watchTrip(p,AccWrite)) break;
      if(storeRepeat((u32)p&0x1fff'ffff, gpr[RT], 8)) break;
      if(mem->wordStoreQuirk((u32)p&0x1fff'ffff, gpr[RT], 8)) break;
      { u32 pe=(u32)p; if(cacheable(a)&&pe<mem->rdram.size()){ dcWrite(pe,gpr[RT],8); break; } mem->write64(pe, gpr[RT]); } } break;
  default: unimplemented(op);
  }
}

// Ranura de retardo. Mismo interprete, pero con inDelay puesto para que takeException
// congele EPC = direccion del SALTO y Cause.BD = 1, como el VR4300. Sin esto un LWC1 o un
// ADD.S en la ranura que faltara dejaria EPC en la ranura y el ERET del kernel volveria
// SALTANDOSE el salto.
extern "C" KJIT_ABI u8 kestrel_jitInterpDelay(void* cpu, u32 op, u32 off) {
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
// CACHE sobre KSEG0/KSEG1 en modo kernel: el caso de osWritebackDCache/osInvalDCache, que
// recorren el buffer linea a linea (en SM64 es lo que mas cede el bloque tras la FPU). Ahi no
// hay excepcion posible -- ni privilegio (kernel), ni TLB (segmento sin mapear) -- y
// translate() no aporta nada: cacheOp solo mira los 29 bits bajos. Se salta el montaje de
// contexto de jitInterpOp y el despacho de execute(), pero con el mismo reloj (jitPending),
// porque una I-cache Hit_Writeback asienta DMA. Con la traza de pc armada, al camino lento.
auto CPU::jitCacheFast(u32 op, u32 off) -> bool {
  if(pcRingOn || cpuMode() != 0) return false;
  u64 a = gpr[(op >> 21) & 31] + (u64)(s64)(s16)(op & 0xFFFF);
  u64 seg = a >> 29;
  if(seg != 0x7'FFFF'FFFCull && seg != 0x7'FFFF'FFFDull) return false;
  jitPending += off >> 2;
  cacheOp((op >> 16) & 0x1f, a);
  jitPending -= off >> 2;
  return true;
}
extern "C" KJIT_ABI u8 kestrel_jitCACHE(void* cpu, u32 op, u32 off) {
  auto* c = reinterpret_cast<kestrel::CPU*>(cpu);
  u8 r = c->jitCacheFast(op, off) ? 1 : c->jitInterpOp(op, off);
  if(!r) return 0;                 // vectorizo (TLB sobre la direccion): estado ya correcto
  if((op >> 16) & 1) return 1;     // D-cache: sin efecto sobre el codigo, el bloque sigue
  u64 va = c->pc + off;            // I-cache: jitInterpOp restauro pc a la entrada del bloque
  c->pc = va + 4; c->nextPc = va + 8;
  c->inDelay = false; c->justBranched = false;
  return 0;                        // salida de control, con la op contada como retirada
}

// MTC0 Status que el dynarec no pudo hacer en linea (puede dejar una interrupcion lista, o
// cambia modo/EXL/CU/FR): la op entera por el interprete y salida de control detras de ella,
// para que el conductor re-muestree interrupciones antes de la siguiente.
extern "C" KJIT_ABI u8 kestrel_jitMTC0S(void* cpu, u32 op, u32 off) {
  auto* c = reinterpret_cast<kestrel::CPU*>(cpu);
  if(!c->jitInterpOp(op, off)) return 0;   // vectorizo (COP0 inutilizable): estado ya correcto
  u64 va = c->pc + off;
  c->pc = va + 4; c->nextPc = va + 8;
  c->inDelay = false; c->justBranched = false;
  return 0;
}

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

// Dynarec Etapa 2b: ejecuta un load/store simple-alineado exactamente como el intérprete.
// Solo se llama desde bloques JIT (mem!=null garantizado por jitTryBlock). Devuelve 0 si
// la op faultaría (misalign/TLB/ADE): NO vectoriza (probe sin efectos vía `probing`), el
// bloque hace bail y el intérprete re-ejecuta esa op para levantar la excepción exacta.
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
  ramSettle(pe, sz); ramUncached(pe, sz);
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
    // Sello de armado ANTES del acceso: una LECTURA de MMIO tambien puede armar un plazo --
    // leer un registro DPC aplica el diario del RSP (dpLogApply) y eso arma el fin de tramo
    // del RDP. Si el sello cambia, el bloque sale en esta instruccion (ver abajo y armEpoch).
    const u32 ep0 = inRdram ? 0u : mem->armEpoch;
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
    return (!inRdram && mem->armEpoch != ep0) ? (u8)2 : (u8)1;
  } else {
    u32 pm = (u32)p & 0x1fff'ffff;
    if constexpr(fp) rtVal = (sz == 4) ? (u64)fprGet32(rt) : fprGet64(rt);
    if(storeRepeat(pm, rtVal, sz)) return 1;
    if constexpr(sz == 1 || sz == 2) { if(storeCart(pm, rtVal, sz)) return 1; }
    if constexpr(sz != 4)            { if(mem->wordStoreQuirk(pm, rtVal, sz)) return 1; }
    if(inRdram) { dcWrite(pe, rtVal, sz); return 1; }
    ramSettle(pe, sz); ramUncached(pe, sz);
    const u32 ep0 = mem->armEpoch;
    if      constexpr(sz == 1) mem->write8 (pe, (u8) rtVal);
    else if constexpr(sz == 2) mem->write16(pe, (u16)rtVal);
    else if constexpr(sz == 4) mem->write32(pe, (u32)rtVal);
    else                       mem->write64(pe, rtVal);
    // SALIDA INMEDIATA DEL BLOQUE si el acceso ARMO algo. El store ya esta fechado bien (el
    // prologo del ayudante suma las ops del bloque a jitPending), pero lo que arma -- plazo
    // del VI/AI, tramo del RDP, cesion del SP, DMA del SI/PI, mascara del MI -- puede vencer
    // DENTRO de las ops que le quedan al bloque, y ahi no mira nadie: el conductor no vuelve a
    // muestrear plazos ni interrupciones hasta el final del bloque, y `*jitGuardPtr = 0` corta
    // la CADENA (lo mira el prologo del bloque siguiente), no el bloque en curso.
    // Devolviendo 2 el bloque sale en esta op y el conductor remata SI/PI/VI, rcpRetire y el
    // muestreo de interrupcion en la instruccion exacta, igual que el interprete.
    // El disparo es el sello `Memory::armEpoch`, que suben exactamente los sitios que arman
    // (Memory::jitCancelChain): ni un MMIO inocuo paga la salida, ni se escapa uno que arme.
    return (mem->armEpoch != ep0) ? (u8)2 : (u8)1;
  }
}

// Trampolines C, uno por opcode: el bloque emite un CALL directo al suyo. Win64 pasa
// (cpu, direccion, rt, dato) en RCX/RDX/R8/R9.
#define KJM(name, opc) \
  extern "C" KJIT_ABI u8 name(void* c, u64 a, u32 rt, u64 v) { \
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
    // linea leia Count 24 ticks corto y el dynarec (que suma lo pendiente, ver jit_compile.cpp MFC0)
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
                        watchArmed = ((u32)v & 3u) != 0 || hostWatchAcc != 0; ++tlbGen; jitTlbValid = false; return;
    case 19:/*WatchHi*/ cop0[reg] = sext32((u32)v & 0xFu); return;
    case C0_Config:  cop0[reg] = sext32(((u32)v & 0x0F00'800Fu) | 0x7006'6460u); return;  // writable: 0-3,15,24-27; rest fixed
    case C0_Status: {
      u32 ov = (u32)cop0[reg], nv = (u32)v & ~(1u<<19);
      if(!(ov & 0x2) && (nv & 0x2)) { exlSetRet = retired; exlSetPc = (u32)curPc; exlSetSrc = 2; }
      if((ov & 0x2) && !(nv & 0x2)) exlClrRet = retired;
      cop0[reg] = sext32(nv); bumpXlat(); return; }  // bit19 not writable (modo/RE/bit64 cambian la traducción → invalida fetch fast-path)
    case C0_Compare: cop0[reg] = sext32((u32)v); timerIntr = false; return;  // writing acks timer
    // Cause: por MTC0 solo se escriben IP1:0 (bits 9:8, interrupciones software). ExcCode,
    // BD, CE e IP7:2 los pone el hardware; dejarlos escribibles clavaba IP3-6 para siempre
    // (step() solo refresca IP2/IP7) y daba una tormenta de interrupciones fantasma.
    case C0_Cause: cop0[reg] = sext32(((u32)cop0[reg] & ~0x300u) | ((u32)v & 0x300u)); return;
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
        if(fpTrace && fpTraceEret) {
          std::fprintf(stderr, "[R] -> 0x%08x status=%08x ret=%llu\n",
                       (u32)nextPc, (u32)cop0[C0_Status], (unsigned long long)retired);
          std::fflush(stderr); fpTraceEret = 0;
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

}  // namespace kestrel
