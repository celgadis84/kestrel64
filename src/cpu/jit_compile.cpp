// Block compiler: guest register residency, instruction emitters and compileBlock.
// Split out of jit.cpp; shared internals live in jit_internal.hpp.
#include "jit.hpp"
#include "cpu.hpp"
#include "../core/memory.hpp"
#include "jit_internal.hpp"
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>

namespace kestrel::jit {

// ========================= Compilador de bloques ============================
//
// Emite un run secuencial de ops "seguras" (ALU/lógica/shift; sin fault, branch,
// memoria, cop, HI/LO) desde `phys`. Para en la primera op no soportada, en el tope,
// o al final de RDRAM. gpr[0] se respeta no emitiendo el store cuando el destino=0.

// ================= residencia de GPR guest en registros del host =================
// Cada op emitia `mov eax,[rbx+8*rs] / <alu> / mov [rbx+8*rd],rax`, asi que dos ops
// dependientes seguidas pagaban un store-to-load forward (~5 ciclos en Nehalem) donde el
// VR4300 solo tiene un bypass de registro. Mantener los GPR calientes del bloque en
// registros del host convierte esa cadena en un mov reg-reg de 1 ciclo.
//
// Invariante de correccion: cpu->gpr tiene que estar coherente en TODO punto donde el
// bloque pueda ceder el control — antes de cualquier CALL a un helper (que lee/escribe gpr
// por el puntero), en cada bail, en cada salida de control y al terminar el bloque. Por eso
// writeback() precede a cada llamada y disable() corre antes de la maquinaria de branch.
// Las ranuras son registros NO-volatiles de Win64, asi que sobreviven al CALL: tras el
// helper solo se olvida lo que el helper haya escrito (gpr[rt] de un load) y el resto sigue
// residente, que es lo que evita recargar el fichero de registros alrededor de cada memoria.
static const Reg kRcRegs[] = { RSI, RDI, R13, R14, R15 };
static constexpr int kRcN = (int)(sizeof(kRcRegs) / sizeof(kRcRegs[0]));

// Instantanea de que ranuras del cache estan sucias en un punto del bloque. Los stubs de
// salida se emiten al final, cuando el estado del cache ya no es el del sitio que salta:
// hay que llevarselo capturado. Con esto el spill de una salida es EXACTO (solo lo sucio
// en ese punto) y deja de hacer falta volcar el cache entero antes de cada llamada.
struct RcSnap { s8 g[kRcN]; };

// Cuenta ESTATICA (KESTREL_JIT_STATS): cuantos accesos a gpr del codigo emitido acaban en un
// registro del host y cuantos siguen yendo a memoria. Dice si la residencia llega a enganchar.
u64 g_rcReg = 0, g_rcMem = 0, g_rcSpill = 0;

struct RegCache {
  Emitter* e = nullptr;
  bool on = false;
  s8   gOf[kRcN];      // gpr guest residente en la ranura k, -1 = libre
  bool dirty[kRcN];    // la ranura tiene un valor aun no escrito a cpu->gpr
  s8   slotOf[32];     // ranura que aloja gpr[g], -1 = ninguna
  int  rr = 0;         // round-robin de desalojo

  auto reset(Emitter* em, bool enable) -> void {
    e = em; on = enable; rr = 0;
    for(int k = 0; k < kRcN; k++) { gOf[k] = -1; dirty[k] = false; }
    for(int g = 0; g < 32; g++) slotOf[g] = -1;
  }
  auto evict(int k) -> void {
    if(gOf[k] < 0) return;
    if(dirty[k]) { e->st64(kRcRegs[k], (u8)gOf[k]); g_rcSpill++; }
    slotOf[gOf[k]] = -1; gOf[k] = -1; dirty[k] = false;
  }
  auto pick() -> int {                  // libre > limpia > round-robin
    for(int k = 0; k < kRcN; k++) if(gOf[k] < 0) return k;
    for(int k = 0; k < kRcN; k++) { int j = (rr + k) % kRcN; if(!dirty[j]) { rr = (j + 1) % kRcN; return j; } }
    int j = rr; rr = (rr + 1) % kRcN; return j;
  }
  // Ranura para gpr[g], o -1 si hay que ir a memoria. `load`=false cuando el uso es una
  // escritura de 64 bits COMPLETA (todos los st64 del codegen lo son): no hace falta traer
  // el valor previo, solo reservar la ranura.
  auto slot(u32 g, bool load) -> int {
    if(!on || g == 0 || g >= 32) { g_rcMem++; return -1; }
    g_rcReg++;
    if(slotOf[g] >= 0) return slotOf[g];
    int k = pick(); evict(k);
    if(load) e->ld64(kRcRegs[k], (u8)g);
    gOf[k] = (s8)g; slotOf[g] = (s8)k; dirty[k] = !load;
    return k;
  }
  auto ld32(Reg dst, u32 g) -> void { int k = slot(g, true); if(k < 0) e->ld32(dst, (u8)g); else e->mov_r_r32(dst, kRcRegs[k]); }
  auto ld64(Reg dst, u32 g) -> void { int k = slot(g, true); if(k < 0) e->ld64(dst, (u8)g); else e->mov_r_r(dst, kRcRegs[k]); }
  auto st64(Reg src, u32 g) -> void {
    if(!g) return;
    int k = slot(g, false);
    if(k < 0) { e->st64(src, (u8)g); return; }
    e->mov_r_r(kRcRegs[k], src); dirty[k] = true;
  }
  auto alu32(u8 opc, Reg dst, u32 g) -> void { int k = slot(g, true); if(k < 0) e->alu32_rm(opc, dst, (u8)g); else e->alu32_rr(opc, dst, kRcRegs[k]); }
  auto alu64(u8 opc, Reg dst, u32 g) -> void { int k = slot(g, true); if(k < 0) e->alu64_rm(opc, dst, (u8)g); else e->alu64_rr(opc, dst, kRcRegs[k]); }
  auto cmp64(Reg dst, u32 g) -> void { int k = slot(g, true); if(k < 0) e->cmp64_rm(dst, (u8)g); else e->cmp64_rr(dst, kRcRegs[k]); }
  // Deja cpu->gpr coherente sin perder residencia (las ranuras quedan limpias).
  auto writeback() -> void { for(int k = 0; k < kRcN; k++) if(dirty[k]) { e->st64(kRcRegs[k], (u8)gOf[k]); dirty[k] = false; g_rcSpill++; } }
  // Volcado DIRIGIDO: deja cpu->gpr[g] coherente sin tocar el resto ni perder residencia.
  // Un helper solo lee los gpr que su op nombra (base de la direccion, dato de un store),
  // asi que volcar los cinco era pagar hasta cinco stores por cada load, store u op de FPU.
  auto writebackOne(u32 g) -> void {
    if(g >= 32) return;
    int k = slotOf[g];
    if(k >= 0 && dirty[k]) { e->st64(kRcRegs[k], (u8)g); dirty[k] = false; g_rcSpill++; }
  }
  // Lo que queda sucio AQUI. El stub de salida correspondiente lo escribira antes de
  // devolver el control al driver, que lee cpu->gpr.
  auto snap() const -> RcSnap {
    RcSnap s{};
    for(int k = 0; k < kRcN; k++) s.g[k] = dirty[k] ? gOf[k] : (s8)-1;
    return s;
  }
  auto emitSnapSpill(const RcSnap& s) -> void {
    for(int k = 0; k < kRcN; k++) if(s.g[k] >= 0) { e->st64(kRcRegs[k], (u8)s.g[k]); g_rcSpill++; }
  }
  // Olvida SIN escribir: solo para lo que un helper acaba de escribir en memoria.
  auto forget(u32 g) -> void { if(g < 32 && slotOf[g] >= 0) { int k = slotOf[g]; slotOf[g] = -1; gOf[k] = -1; dirty[k] = false; } }
  auto forgetAll() -> void { for(int k = 0; k < kRcN; k++) { if(gOf[k] >= 0) slotOf[gOf[k]] = -1; gOf[k] = -1; dirty[k] = false; } }
  auto disable() -> void { writeback(); forgetAll(); on = false; }
};

// Trampolín C para loads/stores desde el código emitido (Win64: cpu en RCX, op en EDX).
// Ejecuta la op espejando el intérprete; devuelve 1=ok / 0=faultaría (bail).
extern "C" KJIT_ABI u8 jitMemThunk(void* cpu, u32 op) {
  return reinterpret_cast<kestrel::CPU*>(cpu)->jitMem(op);
}

// Trampolines especializados por opcode (definidos en cpu.cpp). El bloque llama al suyo con
// la direccion y el dato YA calculados, en vez de pasar la op cruda para que el helper la
// decodifique y relea cpu->gpr.
extern "C" KJIT_ABI u8 kestrel_jitLB (void*, u64, u32, u64); extern "C" KJIT_ABI u8 kestrel_jitLH (void*, u64, u32, u64);
extern "C" KJIT_ABI u8 kestrel_jitLW (void*, u64, u32, u64); extern "C" KJIT_ABI u8 kestrel_jitLBU(void*, u64, u32, u64);
extern "C" KJIT_ABI u8 kestrel_jitLHU(void*, u64, u32, u64); extern "C" KJIT_ABI u8 kestrel_jitLWU(void*, u64, u32, u64);
extern "C" KJIT_ABI u8 kestrel_jitLD (void*, u64, u32, u64); extern "C" KJIT_ABI u8 kestrel_jitSB (void*, u64, u32, u64);
extern "C" KJIT_ABI u8 kestrel_jitSH (void*, u64, u32, u64); extern "C" KJIT_ABI u8 kestrel_jitSW (void*, u64, u32, u64);
extern "C" KJIT_ABI u8 kestrel_jitSD (void*, u64, u32, u64);
extern "C" KJIT_ABI u8 kestrel_jitLWC1(void*, u64, u32, u64); extern "C" KJIT_ABI u8 kestrel_jitLDC1(void*, u64, u32, u64);
extern "C" KJIT_ABI u8 kestrel_jitSWC1(void*, u64, u32, u64); extern "C" KJIT_ABI u8 kestrel_jitSDC1(void*, u64, u32, u64);
extern "C" KJIT_ABI u8 kestrel_jitMFC1 (void*, u32, u32); extern "C" KJIT_ABI u8 kestrel_jitDMFC1(void*, u32, u32);
extern "C" KJIT_ABI u8 kestrel_jitCTC1(void*, u32, u32);
extern "C" KJIT_ABI u8 kestrel_jitCVTWS(void*, u32, u32);   extern "C" KJIT_ABI u8 kestrel_jitTRUNCWS(void*, u32, u32);
extern "C" KJIT_ABI u8 kestrel_jitCVTWD(void*, u32, u32);   extern "C" KJIT_ABI u8 kestrel_jitTRUNCWD(void*, u32, u32);
extern "C" KJIT_ABI u8 kestrel_jitCVTDS(void*, u32, u32);   extern "C" KJIT_ABI u8 kestrel_jitCVTSD(void*, u32, u32);
extern "C" KJIT_ABI u8 kestrel_jitCMPS (void*, u32, u32);   extern "C" KJIT_ABI u8 kestrel_jitCMPD  (void*, u32, u32);
extern "C" KJIT_ABI u8 kestrel_jitCVTSW(void*, u32, u32);   extern "C" KJIT_ABI u8 kestrel_jitCVTDW(void*, u32, u32);
extern "C" KJIT_ABI u8 kestrel_jitDIVS (void*, u32, u32);   extern "C" KJIT_ABI u8 kestrel_jitDIVD (void*, u32, u32);
extern "C" KJIT_ABI u8 kestrel_jitCFC1 (void*, u32, u32); extern "C" KJIT_ABI u8 kestrel_jitMTC1 (void*, u32, u32);
extern "C" KJIT_ABI u8 kestrel_jitDMTC1(void*, u32, u32);
extern "C" KJIT_ABI u8 kestrel_jitInterpDelay(void*, u32, u32); extern "C" KJIT_ABI u8 kestrel_jitCACHE(void*, u32, u32);
extern "C" KJIT_ABI u8 kestrel_jitMTC0S(void*, u32, u32);
extern "C" KJIT_ABI u8 kestrel_jitADDS(void*, u32, u32); extern "C" KJIT_ABI u8 kestrel_jitSUBS(void*, u32, u32);
extern "C" KJIT_ABI u8 kestrel_jitMULS(void*, u32, u32); extern "C" KJIT_ABI u8 kestrel_jitADDD(void*, u32, u32);
extern "C" KJIT_ABI u8 kestrel_jitSUBD(void*, u32, u32); extern "C" KJIT_ABI u8 kestrel_jitMULD(void*, u32, u32);

// Emite un load/store soportado como call jitMemThunk(cpu,op) + test al,al + je(placeholder).
// Convención del bloque 2b: r12=cpu, rbx=gpr. *bailSite = offset del disp32 del je (a parchear
// al epílogo); *isStore = si muta memoria. Devuelve false si op no es un mem-op soportado.
// `mmioSite` (opcional): sitio del jne que saca del bloque cuando el ayudante contesta 2
// (= el acceso ARMO un plazo nuevo). Ver CPU::jitMemOp y Memory::armEpoch. Se emite solo si
// `mmioExit`; en una ranura de retardo NO se pide, que el bloque ya termina en el salto que
// la precede. Vale para cargas y para stores: leer DPC tambien arma. `mmioSnapOut` recoge la
// instantanea del cache de registros con la que sale el stub (la de DESPUES de `forget`).
// Conocimiento FPU del BLOQUE (KESTREL_JIT_NOFPKNOW=1 lo apaga, A/B). Cada op COP1 en linea
// repetia las mismas guardas: CU1, FR (fuente impar) y la sincronia del MXCSR con el modo de
// redondeo del invitado (con su comprobacion de Enable I). Dentro de un bloque esos hechos
// solo cambian por MTC0 Status (CU1/FR) y CTC1 (modo y Enable I): una vez vistos en el camino
// rapido de una op, las siguientes no los vuelven a mirar.
//   El camino lento (CALL) se reune con el rapido, asi que tambien tiene que dejarlos ciertos:
//   - CU1: si estaba a 0 el ayudante levanta Coprocessor Unusable y el bloque sale.
//   - FR y Enable I: el ayudante no los cambia (CTC1 borra el hecho). Si la op los ESTRENA y
//     en el camino lento no se cumplen, se sale del bloque detras de la op, ya retirada (la
//     misma salida que la de MMIO: el conductor avanza pc).
//   - Modo del MXCSR: los ayudantes lo dejan en el que usaron (TRUNC en RZ, por ejemplo); el
//     camino lento lo repone al del invitado antes de reunirse.
// Solo se estrenan FR=1 y Enable I=0 si el estado al COMPILAR ya es ese: asi un juego con
// FR=0 no se pasa la vida saliendo de bloques.
struct FpKnow { bool cu1 = false, fr1 = false, rcg = false; };
static const bool g_fpKnow = !std::getenv("KESTREL_JIT_NOFPKNOW");
static FpKnow g_fk;                                // cierto en el punto actual de la compilacion
static bool g_fkMayFr = false, g_fkMayRc = false;  // estrenos permitidos (estado al compilar)
static FpKnow g_fkAfter;                           // lo que deja la op en curso (camino rapido)
static bool g_fkNewFr = false, g_fkNewRc = false;  // hechos que estrena la op en curso

// Seccion FRIA (KESTREL_JIT_NOCOLD=1 la apaga, A/B). Una op COP1 (o MTC0 Status) en linea
// lleva detras su camino lento: CALL al ayudante, salida de control y la revalidacion de
// FpKnow, ~160 B que casi nunca corren pero ocupan I-cache entre dos ops calientes. Con esto
// se emiten al final del bloque (como los stubs de I-cache exacta) y el camino rapido sigue
// de corrido sin su `jmp` por encima. Solo en el sitio principal del bucle: la ranura de
// retardo tiene rollback propio y se queda en linea.
struct FpBails { usize at[16]; u32 n = 0; auto add(usize a) -> void { at[n++] = a; } };
struct ColdCall {
  FpBails bails; u32 op, off; void* fn; u32 xk; bool xs;
  bool newFr, newRc, rcg;          // revalidacion FpKnow que tocaba tras el CALL
  RcSnap snap; usize cont; u32 idx;
};
static const bool g_jitCold = !std::getenv("KESTREL_JIT_NOCOLD");
static std::vector<ColdCall>* g_cold = nullptr;    // lo arma compileBlock
// Lo mismo para el camino lento de las mem-ops con camino rapido (KESTREL_JIT_NOCOLDMEM=1 lo
// apaga): ~55 B de ajuste de reloj, CALL, salida por bail y salida por MMIO por cada load o
// store, y las mem-ops son de lejos lo mas frecuente del bloque. Todas las fallas del camino
// rapido dejan RDX (direccion) y R9 (dato del store) intactos, que es lo que lee el CALL.
struct ColdMem {
  usize fails[12]; int nFail; u32 opsBefore; s32 pendOff; u32 rt; void* fn;
  RcSnap snap, mmioSnap; usize cont; u32 idx; bool newKsu;
};
// KSU conocido por bloque (KESTREL_JIT_NOKSUKNOW=1 lo apaga, A/B). El camino rapido de las
// mem-ops exige Status.KSU == 0; dentro de un bloque KSU solo cambia por MTC0 Status (una
// excepcion saca del bloque), asi que tras la primera comprobacion las siguientes sobran.
// La op que la ESTRENA la revalida en su camino lento (seccion fria): si al volver del
// ayudante KSU != 0, sale del bloque detras de la op retirada. Se olvida en cada op COP0
// interpretada (MTC0) y en la ranura de retardo no se estrena.
static const bool g_ksuKnow = !std::getenv("KESTREL_JIT_NOKSUKNOW");
static bool g_ksu0 = false;
// Rango de roundTail por exponente (KESTREL_JIT_NOEXPRANGE=1 vuelve a la comparacion exacta).
static const bool g_jitExpRange = !std::getenv("KESTREL_JIT_NOEXPRANGE");
static const bool g_jitColdMem = !std::getenv("KESTREL_JIT_NOCOLDMEM");
static std::vector<ColdMem>* g_coldMem = nullptr;  // lo arma compileBlock

static auto emitMemOp(Emitter& e, RegCache& rc, u32 op, usize& bailSite, bool& isStore,
                      RcSnap& snap, u32 opsBefore, s32 pendOff,
                      usize* mmioSite = nullptr, bool mmioExit = false,
                      RcSnap* mmioSnapOut = nullptr) -> bool {
  u32 OP = op >> 26;
  void* fn = nullptr;
  bool isFp = false;
  switch(OP) {
    case 0x20: fn = (void*)&kestrel_jitLB;  isStore = false; break;
    case 0x21: fn = (void*)&kestrel_jitLH;  isStore = false; break;
    case 0x23: fn = (void*)&kestrel_jitLW;  isStore = false; break;
    case 0x24: fn = (void*)&kestrel_jitLBU; isStore = false; break;
    case 0x25: fn = (void*)&kestrel_jitLHU; isStore = false; break;
    case 0x27: fn = (void*)&kestrel_jitLWU; isStore = false; break;
    case 0x37: fn = (void*)&kestrel_jitLD;  isStore = false; break;
    case 0x28: fn = (void*)&kestrel_jitSB;  isStore = true;  break;
    case 0x29: fn = (void*)&kestrel_jitSH;  isStore = true;  break;
    case 0x2b: fn = (void*)&kestrel_jitSW;  isStore = true;  break;
    case 0x3f: fn = (void*)&kestrel_jitSD;  isStore = true;  break;
    // COP1: el "rt" indexa fpr, no gpr. El helper lee/escribe el banco FPU por su cuenta,
    // asi que el store no pasa dato y el load no invalida ninguna ranura del cache.
    case 0x31: fn = (void*)&kestrel_jitLWC1; isStore = false; isFp = true; break;
    case 0x35: fn = (void*)&kestrel_jitLDC1; isStore = false; isFp = true; break;
    case 0x39: fn = (void*)&kestrel_jitSWC1; isStore = true;  isFp = true; break;
    case 0x3d: fn = (void*)&kestrel_jitSDC1; isStore = true;  isFp = true; break;
    default: return false;
  }
  // La direccion se calcula AQUI, con gpr[rs] donde ya este (ranura del cache o memoria), y
  // el dato del store sale igual de gpr[rt]. Como el helper ya no lee cpu->gpr, no hay que
  // volcar nada antes del CALL: lo que quede sucio lo escribe el stub de bail.
  u32 rs = (op >> 21) & 31, rt = (op >> 16) & 31;
  s32 simm = (s32)(s16)(op & 0xFFFF);
  rc.ld64(RDX, rs);                       // arg1 = gpr[rs]
  if(simm) e.add_r_imm32(RDX, simm);      //        + sext(imm16)  (add de 64 bits)
  if(isStore && !isFp) rc.ld64(R9, rt);   // arg3 = dato del store (COP1 lo saca de fpr)

  // ------------------------------------------------- camino rapido de memoria dentro del bloque
  // Los helpers de memoria eran casi la mitad del perfil del anfitrion en SM64, y en el caso
  // comun hacen siempre lo mismo: ckseg0, alineada, kernel, dentro de RDRAM, linea de D-cache
  // presente. Ese caso se emite aqui. Las comprobaciones son SUFICIENTES, no necesarias: la
  // que no pase cae al helper, que sigue teniendo la semantica exacta y es el oraculo.
  //
  //   rax = a + 0x80000000; si cabe sin signo en 0x20000000 entonces a era exactamente ckseg0
  //   canonico de 64 bits Y rax ES ya la fisica (a & 0x1FFFFFFF). ckseg0 implica cacheable, y
  //   en kernel reXor() es la identidad, asi que la fisica no lleva swizzle de endianness.
  //
  // Dentro de RDRAM ni storeCart (exige isCart) ni wordStoreQuirk (exige DMEM/SP o PIF_RAM)
  // pueden disparar, asi que los stores de 1/2/8 tampoco necesitan preguntar por ellos. Y la
  // reserva RI de LWU/LD/SD solo muerde fuera de kernel, que la guardia de abajo ya excluye.
  //
  // Mascara de lo que se APAGA (KESTREL_JIT_NOFASTMEM): 1=LW 2=SW 8=LWC1 16=SWC1 32=resto de
  // cargas enteras 64=resto de stores enteros 128=LDC1/SDC1, y 4 = los stores hacen todas las
  // comprobaciones pero se van igual al helper sin escribir (separa "una comprobacion deja
  // pasar algo" de "la escritura esta mal"). =255 lo apaga entero.
  // KESTREL_CACHESTAT cuenta accesos a la D-cache en dcRead/dcWrite, y el camino rapido no
  // pasa por ahi: con el puesto se apaga entero, o la tasa de acierto saldria de una muestra
  // sesgada (solo lo que ya era lento). Mide, no corre.
  static const int g_noFastMem = CPU::cacheStatFromEnv() ? 255
                               : std::getenv("KESTREL_JIT_NOFASTMEM")
                               ? (int)std::strtol(std::getenv("KESTREL_JIT_NOFASTMEM"), nullptr, 0) : 0;
  usize fastDone = 0; bool hasFast = false;
  usize fastFail[12]; int nFail = 0;
  const u32 fsz = (OP == 0x20 || OP == 0x24 || OP == 0x28) ? 1
                : (OP == 0x21 || OP == 0x25 || OP == 0x29) ? 2
                : (OP == 0x23 || OP == 0x27 || OP == 0x2b || OP == 0x31 || OP == 0x39) ? 4 : 8;
  bool fmOk;
  switch(OP) {
    case 0x23: fmOk = !(g_noFastMem &   1); break;                       // LW
    case 0x2b: fmOk = !(g_noFastMem &   2); break;                       // SW
    case 0x31: fmOk = !(g_noFastMem &   8); break;                       // LWC1
    case 0x39: fmOk = !(g_noFastMem &  16); break;                       // SWC1
    case 0x20: case 0x24: case 0x21: case 0x25: case 0x27: case 0x37:
               fmOk = !(g_noFastMem &  32); break;                       // LB/LBU/LH/LHU/LWU/LD
    case 0x28: case 0x29: case 0x3f:
               fmOk = !(g_noFastMem &  64); break;                       // SB/SH/SD
    case 0x35: case 0x3d: fmOk = !(g_noFastMem & 128); break;            // LDC1/SDC1
    default:   fmOk = false; break;
  }
  if(fmOk) {
    const bool st = isStore;
    const s32 stOff = (s32)(offsetof(CPU, cop0) + 8u * (u32)CPU::C0_Status);
    const s32 szOff = (s32)offsetof(CPU, jitRdramSz);
    const s32 dcOff = (s32)offsetof(CPU, dcache);
    const s32 sgOff = (s32)offsetof(CPU, stGuard);
    static_assert(sizeof(CPU::DCacheLine) == 32, "el shl 5 de abajo asume lineas de 32 bytes");
    const s32 lnTag = (s32)offsetof(CPU::DCacheLine, tagv);
    const s32 lnDrt = (s32)offsetof(CPU::DCacheLine, dirty);
    const s32 lnDat = (s32)offsetof(CPU::DCacheLine, data);
    const s32 fpOff = (s32)(offsetof(CPU, fpr) + 8u * rt);
    if(isFp) {
      // CU1 claro = Coprocessor Unusable; la levanta el interprete con su CE exacto.
      if(!g_fk.cu1) {                                 // ya visto en el bloque (ver FpKnow)
        e.test_m8_imm(RBX, stOff + 3, 0x20);          // Status bit29 vive en el byte 3
        fastFail[nFail++] = e.je_rel32_placeholder();
      }
      // FR=1: los 32 registros son independientes y el acceso cae en fpr[rt]. FR=0: los pares
      // se juntan, y para rt PAR el destino sigue siendo fpr[rt] -- solo el IMPAR cambia (la
      // mitad alta del companero en 32 bits, el par entero en 64). Como la paridad se sabe al
      // compilar, la comprobacion de FR solo se emite para registros impares.
      if((rt & 1) && !g_fk.fr1) {
        e.test_m8_imm(RBX, stOff + 3, 0x04);          // Status bit26 (FR)
        fastFail[nFail++] = e.je_rel32_placeholder();
      }
    }
    // Enclavamientos: el primer acceso a D tras un store tiene que pasar por dcbTouch.
    static const bool ilkOn = CPU::ilkFromEnv() != 0;
    if(ilkOn) {
      e.test_m8_imm(RBX, (s32)offsetof(CPU, dcbR), 2);
      fastFail[nFail++] = e.jne_rel32_placeholder();
    }
    e.mov_r_r(RAX, RDX);
    e.alu64_imm(5, RAX, 0x80000000u);                 // sub rax, sext(imm) == rax += 0x80000000
    e.cmp64_imm(RAX, 0x20000000u);
    fastFail[nFail++] = e.jae_rel32_placeholder();    // fuera de ckseg0 (o no canonica)
    if(fsz > 1) {
      e.test_al_imm8((u8)(fsz - 1));                  // desalineada: el interprete vectoriza
      fastFail[nFail++] = e.jne_rel32_placeholder();
    }
    if(!g_ksu0) {                                     // ya visto en el bloque (ver g_ksu0)
      e.test_m8_imm(RBX, stOff, 0x18);                // KSU!=0: traduccion general (KX=1 = ckseg0, mismo AND)
      fastFail[nFail++] = e.jne_rel32_placeholder();
    }
    e.cmp_r32_m(RAX, RBX, szOff);                     // fisica fuera de RDRAM (o sin bus): MMIO
    fastFail[nFail++] = e.jae_rel32_placeholder();
    if(st) {
      // Guardia unica del store (CPU::stGuard): bit1 = modo repeticion de MI_MODE armado (el
      // siguiente store a RDRAM se difunde por la pagina entera), bit0 = punto de vigilancia o
      // write-through de diagnostico armado. Cualquiera de los dos manda la escritura al
      // helper. Antes eran dos comprobaciones y una persecucion de puntero (mem->rcp) por
      // store; el bus mantiene el bit1 al vuelo desde Memory::cpuStGuard.
      e.cmp_m8_imm(RBX, sgOff, 0);
      fastFail[nFail++] = e.jne_rel32_placeholder();
    }
    e.mov_r_r32(RCX, RAX);
    e.shift32_imm(5, RCX, 4);                         // shr ecx,4
    e.mov_r_r32(R8, RCX);
    e.shift32_imm(4, R8, 4);                          // shl r8d,4 = phys & ~0xf
    e.alu32_imm(1, R8, 1u);                           // or r8d,1 = tagv esperado (base + valida)
    e.alu32_imm(4, RCX, 0x1FFu);                      // indice de linea
    e.shift64_imm(4, RCX, 5);                         // idx*32 (linea potencia de dos)
    e.alu64_rr(0x03, RCX, RBX);                       // rcx = &cpu->dcache[idx] - dcOff
    // UNA comparacion resuelve tag y validez: el bit0 del tag ES el bit de valida, asi que un
    // fallo de tag y una linea invalida salen los dos por el mismo salto al helper.
    e.cmp_r32_m(R8, RCX, dcOff + lnTag);
    fastFail[nFail++] = e.jne_rel32_placeholder();
    // El sucio se marca AQUI, con rcx todavia en la base de la linea. Sumarle antes el
    // desplazamiento intra-linea escribiria la bandera dentro de data[], que es corrupcion
    // silenciosa del dato recien escrito. Costo una tarde.
    if(st && !(g_noFastMem & 4)) e.mov_m8_imm(RCX, dcOff + lnDrt, 1);
    // bit0 lo acaba de limpiar la frontera y bit1 esta comprobado a 0 arriba: dcbR = 1.
    if(st && !(g_noFastMem & 4) && ilkOn) e.mov_m8_imm(RBX, (s32)offsetof(CPU, dcbR), 1);
    e.alu32_imm(4, RAX, 0xFu);                        // desplazamiento dentro de la linea
    e.alu64_rr(0x03, RCX, RAX);
    const s32 D = dcOff + lnDat;
    if(!st) {
      // El guest guarda big-endian dentro de la linea, de ahi los bswap. El destino se escribe
      // en MEMORIA, igual que hace el helper, para que los dos caminos converjan con el cache
      // de registros en el mismo estado (el forget de abajo vale para los dos).
      switch(fsz) {
        case 1:
          e.movzx8_r_m(RAX, RCX, D);                  // un byte no tiene orden que arreglar
          if(OP == 0x20) e.movsx64_8(RAX, RAX);       // LB extiende en signo; LBU ya viene en cero
          break;
        case 2:
          e.movzx16_r_m(RAX, RCX, D);
          e.bswap32(RAX); e.shift32_imm(5, RAX, 16);  // los dos bytes, ya en orden, abajo
          if(OP == 0x21) e.movsx64_16(RAX, RAX);      // LH sext; LHU se queda en cero
          break;
        case 4:
          e.mov_r32_m(RAX, RCX, D); e.bswap32(RAX);
          if(OP == 0x23) e.movsxd(RAX, RAX);          // LW sext; LWU se queda en cero
          break;
        default:
          e.mov_r_m(RAX, RCX, D); e.bswap64(RAX);
          break;
      }
      if(isFp) { if(fsz == 4) e.mov_m_r32(RBX, fpOff, RAX); else e.mov_m_r(RBX, fpOff, RAX); }
      else if(rt) e.st64(RAX, (u8)rt);
    } else {
      // Bit 4: hace TODAS las comprobaciones y se va igualmente al helper sin escribir. Separa
      // "las comprobaciones dejan pasar algo que no deberian" de "la escritura esta mal".
      if(g_noFastMem & 4) { fastFail[nFail++] = e.jmp_rel32_placeholder(); }
      else {
        if(isFp) { if(fsz == 8) e.mov_r_m(R8, RBX, fpOff); else e.mov_r32_m(R8, RBX, fpOff); }
        else if(fsz == 8) e.mov_r_r(R8, R9);
        else              e.mov_r_r32(R8, R9);        // dato del store (32 bits bajos)
        switch(fsz) {
          case 1: e.mov_m8_r(RCX, D, R8); break;      // un byte va tal cual
          case 2:
            e.shift32_imm(4, R8, 16); e.bswap32(R8);  // deja el par de bytes ya en orden abajo
            e.mov_m16_r(RCX, D, R8); break;
          case 4: e.bswap32(R8); e.mov_m_r32(RCX, D, R8); break;
          default: e.bswap64(R8); e.mov_m_r(RCX, D, R8); break;
        }
      }
    }
    // Seccion fria (ver ColdMem): solo en el sitio principal, que es el que pide salida MMIO.
    if(g_jitColdMem && g_coldMem && mmioSite && mmioExit) {
      ColdMem cm{};
      for(int i = 0; i < nFail; i++) cm.fails[i] = fastFail[i];
      cm.nFail = nFail; cm.opsBefore = opsBefore; cm.pendOff = pendOff; cm.rt = rt; cm.fn = fn;
      cm.snap = rc.snap();
      if(!isStore && !isFp) rc.forget(rt);
      cm.mmioSnap = rc.snap();
      cm.cont = e.buf.used; cm.idx = 0;         // idx lo pone el llamador
      cm.newKsu = g_ksuKnow && !g_ksu0;
      if(g_ksuKnow) g_ksu0 = true;
      g_coldMem->push_back(cm);
      snap = cm.snap; bailSite = 0; *mmioSite = 0;
      if(isFp && g_fpKnow) g_fk.cu1 = true;
      return true;
    }
    fastDone = e.jmp_rel32_placeholder(); hasFast = true;
  }
  for(int i = 0; i < nFail; i++) e.patchRel32(fastFail[i]);   // todos aterrizan en el CALL
  // Reloj del invitado durante el helper. Las ops de ESTE bloque se cobran en retired al salir,
  // asi que dentro del CALL cartNow() iba `opsBefore` instrucciones por detras del interprete:
  // una escritura MMIO (CLEAR_HALT del RSP, MI, PI...) fechaba su evento antes de tiempo y
  // Lockstep con JIT divergia del interprete en DK64. Solo en la ruta lenta: el camino rapido
  // de RDRAM no mira el reloj.
  if(opsBefore) e.add_m32_imm32(RBX, pendOff, opsBefore);
  e.mov_r_r(RCX, RBX);                    // arg0 = cpu (== &gpr[0] == RBX; R12 no fiable)
  e.mov_r_imm32(R8, rt);                  // arg2 = rt
  e.mov_r_imm64(RAX, (u64)fn);
  e.call_reg(RAX);
  if(opsBefore) e.add_m32_imm32(RBX, pendOff, (u32)-(s32)opsBefore);   // no toca AL
  e.test_al_al();
  bailSite = e.je_rel32_placeholder();    // al==0 (faulted) → salta al stub de bail
  snap = rc.snap();                       // lo sucio aqui lo escribe el stub de bail
  // `forget` va ANTES de la salida por MMIO: la op YA se completo, o sea que el ayudante
  // escribio gpr[rt] el mismo, y el volcado del stub no debe pisarlo con la copia vieja.
  // Es estado de compilacion, no emite nada, asi que adelantarlo no cambia el camino rapido.
  if(!isStore && !isFp) rc.forget(rt);
  // al==2: el acceso SI se hizo, pero ARMO un plazo -> el bloque sale AQUI (ver armEpoch).
  if(mmioSite && mmioExit) { if(mmioSnapOut) *mmioSnapOut = rc.snap();
                             e.cmp_al_imm8(1); *mmioSite = e.jne_rel32_placeholder(); }
  else if(mmioSite) *mmioSite = 0;
  if(hasFast) e.patchRel32(fastDone);     // el camino rapido se reune aqui
  // Con CU1=0 el ayudante falla y el bloque sale por el bail: quien sigue, lo tiene a 1.
  if(isFp && g_fpKnow) g_fk.cu1 = true;
  return true;
}

// ALU con trampa de desbordamiento: ADDI (OP 0x08) y ADD/SUB (SPECIAL 0x20/0x22). Son las
// mismas sumas que ADDIU/ADDU/SUBU salvo que el VR4300 levanta IntegerOverflow cuando la suma
// con signo de 32 bits desborda, y esa excepcion tiene que vectorizarse con el pc exacto de la
// op — por eso estaban fuera del JIT. Pero eso es justo lo que ya sabe hacer el stub de bail
// de las mem-ops: se emite la aritmetica nativa y, si el host marca desbordamiento, se sale
// por el MISMO stub SIN haber escrito el destino, con eax = ops retiradas antes de esta; el
// interprete re-ejecuta la op y levanta la excepcion con su semantica exacta. El flag OF de
// x86 tras un add/sub de 32 bits ES el desbordamiento con signo de MIPS, asi que no hay que
// calcularlo aparte.
//
// Las variantes de 64 bits (DADDI/DADD/DSUB) NO entran, por la misma razon que DADDIU (ver
// nota en emitSafeOp): trapean RI con el modo de 64 bits apagado, y eso es estado de runtime.
// Biseccion: 0 = todo (por defecto), 1 = ninguna, 2 = solo ADDI, 3 = solo ADD/SUB.
// El diff corre el bloque sobre una COPIA de los registros y deja mandar al interprete, asi
// que un desacuerdo no contamina el estado del invitado: parar es solo comodidad. Con esto
// puesto el barrido sigue y saca TODOS los bloques malos de una pasada en vez de uno por run.
const bool g_diffGo = std::getenv("KESTREL_JIT_DIFFGO") != nullptr;
const bool g_pcChk = std::getenv("KESTREL_JIT_PCCHK") != nullptr;
// DIAGNOSTICO (KESTREL_REGCHK): cordura del banco de registros en cada despacho del driver.
// Cuando el guest se descarrila sin excepcion previa, el sintoma final (pc paseando por MMIO)
// llega cientos de miles de ops despues del error real. Pero hay dos registros cuyo valor es
// invariante en un juego sano de 32 bits: el puntero de pila y la direccion de retorno son
// SIEMPRE la extension de signo de un KSEG0/KSEG1 (0x8.../0xA...) -- libultra no pone pilas
// en ningun otro sitio. En cuanto uno de los dos deja de serlo, el error acaba de ocurrir.
const bool g_regChk = std::getenv("KESTREL_REGCHK") != nullptr;
auto regChkBad(u64 v) -> bool {
  if(v == 0) return false;
  if((u64)(s64)(s32)v != v) return true;          // no es una VA de 32 bits extendida
  // PD ejecuta desde kuseg mapeado por TLB en 0x70000000, asi que `ra` vive ahi; las
  // pilas de libultra viven en KSEG0. Por debajo de 0x70000000 no hay ni codigo ni pila.
  return (u32)v < 0x80000000u;                    // las pilas de libultra viven en KSEG0
}
u32 g_diffBad = 0;

// Que ALU-con-trampa absorbe el bloque (KESTREL_JIT_NOTRAPALU):
//   0 ADDI+ADD/SUB (DEFECTO)   1 ninguna   2 solo ADDI   3 solo ADD/SUB
// ADDI estuvo desactivada un tiempo por una "tormenta de excepciones" en systemtest que en
// realidad no era suya: absorberla alarga el bloque hasta cubrir el prologo de un handler, y
// el bloque entraba con `memAbort` aun puesto por la excepcion que acababa de vectorizar.
// Con KX=1 eso hacia que translate() cortocircuitara a fisico 0 y los `sd` del prologo
// machacaran el vector de excepciones. El bloque ahora limpia el pestillo como hace step().

// Bisector A/B: 1 = el bloque nunca se pasa de su pagina de entrada (comportamiento previo).
static const int g_noXPage = std::getenv("KESTREL_JIT_NOXPAGE") ? 1 : 0;

static const int g_trapAlu = std::getenv("KESTREL_JIT_NOTRAPALU")
                           ? (int)std::strtol(std::getenv("KESTREL_JIT_NOTRAPALU"), nullptr, 0) : 0;

static auto emitTrapAlu(Emitter& e, RegCache& rc, u32 op, usize& bailSite, RcSnap& snap) -> bool {
  if(g_trapAlu == 1) return false;
  u32 OP = op >> 26, funct = op & 63;
  u32 rs = (op >> 21) & 31, rt = (op >> 16) & 31, rd = (op >> 11) & 31;
  s32 simm = (s32)(s16)(op & 0xFFFF);
  u32 dst;
  if(OP == 0x08 && g_trapAlu != 3) {                           // ADDI rt, rs, imm
    rc.ld32(RAX, rs);
    e.alu32_imm(0, RAX, (u32)simm);
    dst = rt;
  } else if(OP == 0x00 && (funct == 0x20 || funct == 0x22) && g_trapAlu != 2) {   // ADD / SUB
    rc.ld32(RAX, rs);
    rc.alu32(funct == 0x20 ? 0x03 : 0x2B, RAX, rt);   // ADD / SUB r32, r/m32
    dst = rd;
  } else {
    return false;
  }
  bailSite = e.jo_rel32_placeholder();   // desbordo -> salir sin tocar destino
  snap = rc.snap();                      // lo sucio aqui lo escribe el stub de bail
  // El destino gpr[0] descarta el resultado, pero la trampa se levanta igual (el HW la mira
  // antes que el destino), asi que lo unico condicional es el store.
  if(dst) { e.movsxd(RAX, RAX); rc.st64(RAX, dst); }
  return true;
}

// ALU de 64 bits (DADDU/DSUBU/DADDIU y la familia de desplazamientos dobles). El VR4300 las
// reserva (RI, ExcCode 10) en usuario/supervisor sin UX/SX; en modo KERNEL estan permitidas
// siempre (CPU::reserved64). El modo es estado de runtime, asi que se emite la misma guardia
// que usa el resto del JIT -- KSU==0 en Status[4:3] -- y cualquier otro caso sale por el bail
// SIN haber tocado nada, para que el interprete levante la RI exacta si toca. Antes cortaban
// el bloque en seco y eran las lideres no compilables mas caras de SM64 despues de COP0:
// DSLL32 y DSRA32 sumaban 27k entradas al interprete cada una en 300 fotogramas (el idioma
// del compilador de SGI para extraer la mitad alta de un doble: dsll32 + dsra32).
//
// Los desplazamientos de x86 en 64 bits enmascaran la cuenta a &63, igual que MIPS, asi que
// DSLLV/DSRLV/DSRAV no necesitan mascara explicita. DADD/DSUB (con trampa de desbordamiento)
// NO entran: ademas de la guardia necesitarian el bail de overflow.
static auto emitMulDiv(Emitter& e, RegCache& rc, u32 funct, u32 rs, u32 rt) -> void;
static auto emitAlu64(Emitter& e, RegCache& rc, u32 op, usize& bailSite, RcSnap& snap) -> bool {
  static const int off = std::getenv("KESTREL_JIT_NOALU64") ? 1 : 0;   // bisector A/B
  if(off) return false;
  u32 OP = op >> 26, funct = op & 63;
  u32 rs = (op >> 21) & 31, rt = (op >> 16) & 31, rd = (op >> 11) & 31, sa = (op >> 6) & 31;
  s32 simm = (s32)(s16)(op & 0xFFFF);
  u32 dst;
  bool isImm = (OP == 0x19);                                   // DADDIU
  if(!isImm && OP != 0x00) return false;
  if(isImm) dst = rt;
  else {
    switch(funct) {
      case 0x14: case 0x16: case 0x17:                         // DSLLV / DSRLV / DSRAV
      case 0x2d: case 0x2f:                                    // DADDU / DSUBU
      case 0x38: case 0x3a: case 0x3b:                         // DSLL / DSRL / DSRA
      case 0x3c: case 0x3e: case 0x3f: break;                  // DSLL32 / DSRL32 / DSRA32
      case 0x1c: case 0x1d: case 0x1e: case 0x1f: break;       // DMULT/DMULTU/DDIV/DDIVU -> HI/LO
      default: return false;
    }
    dst = rd;
  }
  const s32 stOff = (s32)(offsetof(CPU, cop0) + 8u * (u32)CPU::C0_Status);
  e.test_m8_imm(RBX, stOff, 0x18);                             // Status[4:3] = KSU
  bailSite = e.jne_rel32_placeholder();                        // no-kernel -> lo resuelve el interprete
  snap = rc.snap();
  if(!isImm && funct >= 0x1c && funct <= 0x1f) { emitMulDiv(e, rc, funct, rs, rt); return true; }
  if(!dst) return true;                                        // destino $0: la op no deja rastro
  if(isImm) { rc.ld64(RAX, rs); e.alu64_imm(0, RAX, (u32)simm); }
  else switch(funct) {
    case 0x2d: rc.ld64(RAX, rs); rc.alu64(0x03, RAX, rt); break;                    // DADDU
    case 0x2f: rc.ld64(RAX, rs); rc.alu64(0x2B, RAX, rt); break;                    // DSUBU
    case 0x14: case 0x16: case 0x17:                                                 // DSLLV/DSRLV/DSRAV
      rc.ld32(RCX, rs); rc.ld64(RAX, rt);
      e.shift64_cl(funct == 0x14 ? 4 : funct == 0x16 ? 5 : 7, RAX); break;
    default: {                                                                       // DSLL/DSRL/DSRA(+32)
      u8 digit = (funct == 0x38 || funct == 0x3c) ? 4 : (funct == 0x3a || funct == 0x3e) ? 5 : 7;
      u8 cnt = (u8)(sa + (funct >= 0x3c ? 32 : 0));
      rc.ld64(RAX, rt); if(cnt) e.shift64_imm(digit, RAX, cnt); break;
    }
  }
  rc.st64(RAX, dst);
  return true;
}

// MFC0: leer un registro COP0 dentro del bloque. Es la mitad del idioma de libultra para
// tocar interrupciones (mfc0 Status / and / mtc0 Status), y hasta ahora cortaba el bloque en
// seco: en SM64 era la op lider de 106k compilaciones fallidas.
//
// Dos condiciones para poder emitirlo aqui:
//  - el registro tiene que valer lo mismo a mitad de bloque que en el borde. Count y Random NO:
//    el JIT los adelanta de golpe al salir (Count += R), asi que dentro del bloque van
//    atrasados. Cause tampoco: sus bits IP2/IP7 se refrescan al ENTRAR al bloque, y una
//    interrupcion que llegue mientras corre no se ve. Esos tres ceden al interprete.
//  - COP0 es privilegiado: en usuario/supervisor sin Status.CU0 levanta Coprocessor Unusable
//    (ExcCode 11). Se emite la guardia de modo kernel (KSU==0, que es donde vive todo el
//    codigo de N64) y cualquier otro caso -- incluido el legitimo de usuario CON CU0 -- sale
//    por el bail, correcto siempre porque aun no se ha escrito nada.
static auto emitCop0(Emitter& e, RegCache& rc, u32 op, usize& bailSite, RcSnap& snap,
                     u32 idx, u32 cpi256) -> bool {
  static const int off = std::getenv("KESTREL_JIT_NOCOP0") ? 1 : 0;   // bisector
  if(off || (op >> 26) != 0x10) return false;
  u32 rs = (op >> 21) & 31, rt = (op >> 16) & 31, rd = (op >> 11) & 31;
  if(rs != 0x00) return false;                       // solo MFC0; MTC0 cambia estado (ver notas)
  if(rd == CPU::C0_Random || rd == CPU::C0_Cause) return false;
  // Count SI se puede leer dentro del bloque, pero no como esta guardado: el bloque lo
  // adelanta de golpe al salir (Count += ops retiradas), asi que a mitad va atrasado
  // exactamente por las ops ya ejecutadas. El interprete lo sube UNA vez al final de cada
  // step, luego durante la op numero `idx` del bloque el valor visible es entrada+idx.
  // Con eso el valor emitido es el mismo bit que daria el interprete, no una aproximacion.
  // La frontera Count==Compare no puede caer dentro del bloque (guarda DR_TIMER), asi que
  // no hay latch de IP7 que se pierda por el camino.
  // readCop0 no devuelve cop0[rd] para estos: PRId es una constante y el resto no tiene
  // almacenamiento (leen el ultimo dato latcheado en el bus). Residuales -> al interprete.
  if(rd == CPU::C0_PRId || rd == 7 || (rd >= 21 && rd <= 25) || rd == 31) return false;

  const s32 stOff = (s32)(offsetof(CPU, cop0) + 8u * (u32)CPU::C0_Status);
  e.test_m8_imm(RBX, stOff, 0x18);                   // Status[4:3] = KSU
  bailSite = e.jne_rel32_placeholder();              // no-kernel -> lo vectoriza el interprete
  snap = rc.snap();
  // $0 descarta el resultado, pero la comprobacion de privilegio se hace igual: la guardia
  // de arriba queda, solo desaparece el store.
  if(rt) {
    if(rd == CPU::C0_Count) {
      // Count guardado va atrasado por TODO lo que aun no se ha commiteado: las idx ops de
      // este bloque y las de los eslabones anteriores de la cadena, que el camino rapido del
      // prologo deja en jitPending sin pasar por el trampolin. El valor que veria el
      // interprete es exactamente lo que sumaria countTicks(jitPending + idx) -- resto
      // (countFrac) y paradas acumuladas (stallCycles) incluidos -- sin tocar el estado.
      // Antes solo se sumaba idx: con la cadena enlazada Count salia 1 tick corto, osSetTimer
      // armaba otro Compare y DK64 Lockstep con JIT sacaba el timer 6 ops antes.
      e.mov_r32_m(RAX, RBX, (s32)offsetof(CPU, jitPending));   // zero-extiende
      if(idx) e.add_r_imm32(RAX, (s32)idx);
      e.mov_r_imm32(RDX, cpi256);
      e.imul64(RAX, RDX);
      e.mov_r32_m(RCX, RBX, (s32)offsetof(CPU, countFrac));
      e.add_r_r(RAX, RCX);
      e.mov_r32_m(RCX, RBX, (s32)offsetof(CPU, stallCycles));
      e.shift64_imm(4, RCX, 7);                               // shl rcx, 7
      e.add_r_r(RAX, RCX);
      e.shift64_imm(5, RAX, 8);                               // shr rax, 8 = ticks
      e.mov_r32_m(RCX, RBX, (s32)(offsetof(CPU, cop0) + 8u * rd));
      e.add_r_r(RAX, RCX);                                    // los 32 bajos = Count
    } else {
      e.mov_r_m(RAX, RBX, (s32)(offsetof(CPU, cop0) + 8u * rd));
    }
    e.movsxd(RAX, RAX);                              // MFC0 = sext32 de la palabra baja
    rc.st64(RAX, rt);
  }
  return true;
}

// Trampolín C para ops NO compilables ejecutadas por el intérprete dentro del bloque.
// `off` = desplazamiento en bytes de la op respecto a la entrada del bloque.
extern "C" KJIT_ABI u8 jitInterpThunk(void* cpu, u32 op, u32 off) {
  return reinterpret_cast<kestrel::CPU*>(cpu)->jitInterpOp(op, off);
}

// Fallback de intérprete en bloque. El compilador declinaba el bloque entero en la PRIMERA
// op no soportada, y en código real (SM64/PD) esa op es casi siempre FPU: LWC1/SWC1/COP1
// aparecen cada pocas instrucciones, así que los bloques quedaban en ~2.4 ops y el coste de
// entrada al bloque se comía la máquina. Ejecutar esas ops llamando al intérprete cuesta un
// CALL, pero deja seguir el bloque: sube la longitud media en vez de partirla.
//
// Conjunto admitido: sólo ops SIN control de flujo y sin estado especulativo, para que el
// contrato del bloque siga siendo "pc/nextPc no cambian salvo salida explícita".
//   - COP1 (0x11) salvo BC1x (rs==8), que ES un branch.
//   - LWC1/LDC1/SWC1/SDC1 — memoria FPU (el thunk incluye el fault → salida).
//   - LWL/LWR/SWL/SWR y LDL/LDR/SDL/SDR — memoria desalineada, que emitMemOp no cubre.
//     Las de 64 bits faltaban y no son un caso raro: GCC (libdragon) resuelve una copia
//     desalineada de 64 bits con LDL/LDR + SDL/SDR, mientras que IDO (libultra) usa las
//     de 32. En junkrunner64 el bucle caliente las lleva cada pocas instrucciones, asi
//     que el bloque se declinaba entero ahi y el juego corria casi entero en el
//     interprete. Misma forma que las de 32: memoria, sin control de flujo.
//   - SPECIAL DIV/DIVU/DMULT/DMULTU/DDIV/DDIVU — HI/LO, sin fault ni salto.
// Excluidos a propósito: COP0 (0x10, cambia TLB/Status → puede vectorizar), CACHE, LL/SC,
// SYSCALL/BREAK/TRAP y todo lo que salte. Si la op falla o vectoriza, el thunk devuelve 0 y
// el bloque sale con la bandera de control (pc/nextPc ya los dejó bien el intérprete).
//
// `cop0Term` abre la puerta a MTC0 (y SOLO a MTC0) con la condicion de que el llamante CIERRE
// el bloque justo despues: escribir Status/Cause puede dejar una interrupcion lista, y el
// bloque no vuelve a mirarlas hasta salir, asi que seguir emitiendo ops detras retrasaria la
// entrega. El llamante excluye ademas Count y Compare (ver compileBlock).
// ---- FPU en linea --------------------------------------------------------------------------
// El caso comun de las ops de formato mas frecuentes se resuelve en el propio bloque, sin CALL:
// ADD/SUB/MUL.S (misma aritmetica que CPU::jitCop1Alu: exacta en doble, UN redondeo a simple,
// Inexact por comparacion), MOV.S/D, ABS/NEG.S/D y C.cond.S/D. Cualquier cosa fuera del caso
// comun salta al CALL de siempre, que hace la op entera -- el camino en linea no escribe NADA
// del invitado hasta haber pasado todas sus comprobaciones, asi que rendirse es seguro en
// cualquier punto. Lo que se exige:
//   - CU1 (Status bit 29), y FR=1 si fs es impar (con FR=0 el campo fuente se alinea a par:
//     caso raro, al CALL).
//   - Aritmetica: redondeo del invitado RN y el del anfitrion tambien (MXCSR.RC == 0; el
//     interprete lo deja en el ultimo modo del invitado, y el CALL lo repone), y el Enable de
//     Inexact apagado. Operandos normales o cero; resultado exacto en rango normal o cero.
//   - ABS/NEG/C.cond: operandos normales o cero (NaN, infinito o subnormal -> CALL).
// Solo con los cobros opcionales apagados (KESTREL_FPU, KESTREL_INTERLOCK), sin oraculo
// (KESTREL_FPORACLE) y sin KESTREL_JIT_NOFPINLINE: los tres dependen de que la op pase por el
// trampolin. Nunca en ranura de retardo (ese camino usa su propio trampolin).
static const bool g_fpInline = !std::getenv("KESTREL_JIT_NOFPINLINE") && !std::getenv("KESTREL_FPORACLE")
                               && CPU::fpuFromEnv() == 0 && CPU::ilkFromEnv() == 0;


// MXCSR.RC = modo del invitado, sin mirar Enable I ni banderas. Pisa RAX/RCX/RDX.
static auto emitRcGuest(Emitter& e) -> void {
  const s32 fcrOff = (s32)offsetof(CPU, fcr31);
  e.mov_r32_m(RCX, RBX, fcrOff);
  e.alu32_imm(4, RCX, 3);
  e.mov_r_r32(RDX, RCX); e.alu32_imm(4, RDX, 1); e.shift32_imm(4, RDX, 1);
  e.alu32_rr(0x33, RCX, RDX);
  e.shift32_imm(4, RCX, 13);
  e.stmxcsr_rsp(40);
  e.ld32_rsp(RAX, 40);
  e.mov_r_r32(RDX, RAX); e.alu32_imm(4, RDX, 0x6000u);
  e.alu32_rr(0x3B, RDX, RCX);
  usize ok = e.jcc_rel32_placeholder(0x84);
  e.alu32_imm(4, RAX, ~0x6000u); e.alu32_rr(0x0B, RAX, RCX);
  e.st32_rsp(RAX, 40);
  e.ldmxcsr_rsp(40);
  e.patchRel32(ok);
}

// Normal o cero en simple (x en r32, destruye t): t = x & 0x7fffffff; t == 0, o bien
// t - 0x800000 < 0x7f000000 (exponente 1..254). Si no, al CALL.
static auto emitPlain32(Emitter& e, Reg x, Reg t, FpBails& b) -> void {
  e.mov_r_r32(t, x);
  e.alu32_imm(4, t, 0x7fff'ffffu);
  usize z = e.jcc_rel32_placeholder(0x84);
  e.alu32_imm(5, t, 0x0080'0000u);
  e.alu32_imm(7, t, 0x7f00'0000u);
  b.add(e.jcc_rel32_placeholder(0x83));         // jae -> CALL
  e.patchRel32(z);
}
// Lo mismo en doble (x en r64, destruye t y u): t = x << 1 (fuera el signo); t == 0, o bien
// t - 2^53 < 0x7fe << 53 (exponente 1..2046).
static auto emitPlain64(Emitter& e, Reg x, Reg t, Reg u, FpBails& b) -> void {
  e.mov_r_r(t, x);
  e.shift64_imm(4, t, 1);
  usize z = e.jcc_rel32_placeholder(0x84);
  e.mov_r_imm64(u, 1ull << 53);
  e.alu64_rr(0x2B, t, u);
  e.mov_r_imm64(u, 0x7feull << 53);
  e.cmp64_rr(t, u);
  b.add(e.jcc_rel32_placeholder(0x83));
  e.patchRel32(z);
}

// Emite el camino en linea de `op` si lo tiene. Devuelve false (sin emitir nada) si no.
static auto emitFpInline(Emitter& e, u32 op, FpBails& b) -> bool {
  u32 fmt = (op >> 21) & 31, fn = op & 63;
  u32 ft = (op >> 16) & 31, fs = (op >> 11) & 31, fd = (op >> 6) & 31;
  if(fmt != 0x10 && fmt != 0x11 && fmt != 0x14) return false;
  bool S = fmt == 0x10, D = fmt == 0x11, W = fmt == 0x14;
  bool arith = !W && fn <= 4;                                              // ADD/SUB/MUL/DIV/SQRT
  bool mov = !W && fn == 6, absneg = !W && (fn == 5 || fn == 7), cmp = !W && fn >= 0x30;
  bool toW = !W && ((fn >= 0x0c && fn <= 0x0f) || fn == 0x24);          // ROUND/TRUNC/CEIL/FLOOR/CVT .W
  bool f2f = (S && fn == 0x21) || (D && fn == 0x20) || (W && (fn == 0x20 || fn == 0x21));
  if(!arith && !mov && !absneg && !cmp && !toW && !f2f) return false;

  const s32 stOff  = (s32)offsetof(CPU, cop0) + 12 * 8 + 3;   // byte alto de Status (32b)
  const s32 fcrOff = (s32)offsetof(CPU, fcr31);
  const s32 fprOff = (s32)offsetof(CPU, fpr);
  auto fpr = [&](u32 i) { return fprOff + (s32)(i * 8); };

  const FpKnow k = g_fk;
  FpKnow& a = g_fkAfter; a = k; a.cu1 = true;
  g_fkNewFr = g_fkNewRc = false;
  if(!k.cu1) { e.test_m8_imm(RBX, stOff, 0x20);    // CU1
               b.add(e.jcc_rel32_placeholder(0x84)); }
  if((fs & 1) && !k.fr1) {
    e.test_m8_imm(RBX, stOff, 0x04);               // FR
    b.add(e.jcc_rel32_placeholder(0x84));
    if(g_fkMayFr) { a.fr1 = true; g_fkNewFr = true; }
  }

  if(mov) {                                        // copia cruda de 64 bits, FCSR intacto
    e.mov_r_m(RAX, RBX, fpr(fs));
    e.mov_m_r(RBX, fpr(fd), RAX);
    return true;
  }

  if(absneg) {
    if(S) {
      e.mov_r32_m(RAX, RBX, fpr(fs));
      emitPlain32(e, RAX, RCX, b);
      if(fn == 5) e.alu32_imm(4, RAX, 0x7fff'ffffu); else e.alu32_imm(6, RAX, 0x8000'0000u);
    } else {
      e.mov_r_m(RAX, RBX, fpr(fs));
      emitPlain64(e, RAX, RCX, RDX, b);
      if(fn == 5) { e.shift64_imm(4, RAX, 1); e.shift64_imm(5, RAX, 1); }
      else        { e.mov_r_imm64(RCX, 1ull << 63); e.xor_r_r(RAX, RCX); }
    }
    e.mov_r32_m(RCX, RBX, fcrOff);                 // Cause a cero, sin excepcion posible
    e.alu32_imm(4, RCX, ~0x0003'F000u);
    e.mov_m_r32(RBX, fcrOff, RCX);
    e.mov_m_r(RBX, fpr(fd), RAX);                  // .S: alto a cero (el load de 32 extiende)
    return true;
  }

  if(cmp) {
    if(S) {
      e.mov_r32_m(RAX, RBX, fpr(fs)); e.mov_r32_m(RDX, RBX, fpr(ft));
      emitPlain32(e, RAX, RCX, b); emitPlain32(e, RDX, RCX, b);
      e.movd_x_r32(0, RAX); e.movd_x_r32(1, RDX); e.ucomiss(0, 1);
    } else {
      e.mov_r_m(RAX, RBX, fpr(fs)); e.mov_r_m(RDX, RBX, fpr(ft));
      emitPlain64(e, RAX, RCX, R8, b); emitPlain64(e, RDX, RCX, R8, b);
      e.movq_x_r64(0, RAX); e.movq_x_r64(1, RDX); e.ucomisd(0, 1);
    }
    // Ordenados: CF = menor, ZF = igual. c = (fn&4 && menor) | (fn&2 && igual).
    // El setcc va JUSTO tras el ucomis (todo lo demas pisa los flags) y luego se extiende.
    u32 m = fn & 6;
    if(m) { e.setcc(m == 4 ? 0x92 : m == 2 ? 0x94 : 0x96, RAX);   // setb / sete / setbe
            e.movzx_r8(RAX, RAX); e.shift32_imm(4, RAX, 23); }
    e.mov_r32_m(RCX, RBX, fcrOff);                 // Cause a cero y C = resultado
    e.alu32_imm(4, RCX, ~(0x0003'F000u | 0x0080'0000u));
    if(m) e.alu32_rr(0x0B, RCX, RAX);              // or ecx, eax
    e.mov_m_r32(RBX, fcrOff, RCX);
    return true;
  }

  // Modo de redondeo del anfitrion. El resultado lo da la unidad SSE con el RC del MXCSR, asi
  // que basta con que sea el que pide la op: `fixed` >= 0 es un RC x86 fijo (ROUND/CEIL/FLOOR),
  // -1 el del invitado. MIPS rm 0/1/2/3 = RN/RZ/RP/RM -> x86 RC 0/3/2/1 = rm ^ ((rm&1)<<1).
  // Los helpers (CVT, TRUNC, jitCop1Alu) dejan el MXCSR en el modo que usaron; se reescribe
  // solo si difiere, como mx::prepRc. Con `clr` se exigen ademas las banderas de excepcion
  // limpias (camino que deduce el Inexact de PE). Con Enable I armado todo Inexact es trampa:
  // se cede antes de mirar nada. Pisa RAX/RCX/RDX.
  auto syncRc = [&](int fixed, bool clr) {
    if(fixed < 0 && k.rcg) {                       // modo e Enable I ya vistos en el bloque
      if(clr) {                                    // solo hacen falta las banderas limpias
        e.stmxcsr_rsp(40);
        e.ld32_rsp(RAX, 40);
        e.test_r32_imm(RAX, 0x3F);
        usize ok = e.jcc_rel32_placeholder(0x84);
        e.alu32_imm(4, RAX, ~0x3Fu);
        e.st32_rsp(RAX, 40);
        e.ldmxcsr_rsp(40);
        e.patchRel32(ok);
      }
      return;
    }
    if(fixed < 0) { if(g_fkMayRc) { a.rcg = true; g_fkNewRc = true; } }
    else a.rcg = false;
    e.mov_r32_m(RCX, RBX, fcrOff);
    e.test_r32_imm(RCX, 0x80);
    b.add(e.jcc_rel32_placeholder(0x85));
    if(fixed < 0) {
      e.alu32_imm(4, RCX, 3);
      e.mov_r_r32(RDX, RCX); e.alu32_imm(4, RDX, 1); e.shift32_imm(4, RDX, 1);
      e.alu32_rr(0x33, RCX, RDX);                  // xor ecx, edx
      e.shift32_imm(4, RCX, 13);                   // RC en los bits 13-14
    } else e.mov_r_imm32(RCX, (u32)fixed << 13);
    u32 m = clr ? 0x603Fu : 0x6000u;
    e.stmxcsr_rsp(40);                             // [rsp+40]: libre (la sombra es 0..31)
    e.ld32_rsp(RAX, 40);
    e.mov_r_r32(RDX, RAX); e.alu32_imm(4, RDX, m);
    e.alu32_rr(0x3B, RDX, RCX);                    // cmp edx, ecx
    usize ok = e.jcc_rel32_placeholder(0x84);
    e.alu32_imm(4, RAX, ~m); e.alu32_rr(0x0B, RAX, RCX);
    e.st32_rsp(RAX, 40);
    e.ldmxcsr_rsp(40);
    e.patchRel32(ok);
  };
  // Cause a cero y, si `inexact` deja ZF=0, Cause I + Flag I pegajosa. Pisa RCX.
  auto causeStore = [&](auto&& inexact) {
    e.mov_r32_m(RCX, RBX, fcrOff);
    e.alu32_imm(4, RCX, ~0x0003'F000u);
    inexact();
    usize ex = e.jcc_rel32_placeholder(0x84);      // ZF=1 -> exacto
    e.alu32_imm(1, RCX, 0x1004u);
    e.patchRel32(ex);
    e.mov_m_r32(RBX, fcrOff, RCX);
  };
  auto clearCause = [&] {
    e.mov_r32_m(RCX, RBX, fcrOff);
    e.alu32_imm(4, RCX, ~0x0003'F000u);
    e.mov_m_r32(RBX, fcrOff, RCX);
  };
  // xmm0 = valor EXACTO en doble -> simple con un unico redondeo (el RC ya es el del
  // invitado). Fuera de [2^-126, maxfloat] (y no cero) no sabemos deducir Overflow/Underflow
  // -- con redondeo hacia cero un desbordamiento da el maximo finito --: al CALL. El Inexact
  // sale de comparar el simple devuelto a doble con el exacto.
  auto roundTail = [&] {
    e.movq_r64_x(RAX, 0);
    e.shift64_imm(4, RAX, 1);
    usize z = e.jcc_rel32_placeholder(0x84);
    if(g_jitExpRange) {
      // Solo el exponente doble: [0x381, 0x47E) = [2^-126, 2^127). La franja [2^127,
      // maxfloat] tambien iria en linea con la comparacion exacta, pero es rarisima y el CALL
      // da el mismo resultado; a cambio, sin los dos movabs (-11 B por op FP aritmetica).
      e.shift64_imm(5, RAX, 53);
      e.alu32_imm(5, RAX, 0x381);
      e.alu32_imm(7, RAX, 0x47E - 0x381 - 1);
    } else {
      e.mov_r_imm64(RCX, 0x3810'0000'0000'0000ull << 1);
      e.alu64_rr(0x2B, RAX, RCX);
      e.mov_r_imm64(RCX, (0x47EF'FFFF'E000'0000ull - 0x3810'0000'0000'0000ull) << 1);
      e.cmp64_rr(RAX, RCX);
    }
    b.add(e.jcc_rel32_placeholder(0x87));
    e.patchRel32(z);
    e.cvtsd2ss(2, 0);
    e.cvtss2sd(3, 2);
    e.movd_r32_x(R9, 2);
    causeStore([&] { e.ucomisd(3, 0); });
    e.mov_m_r(RBX, fpr(fd), R9);                   // 32 bits extendidos con cero
  };
  // Fuente en xmm0 como doble, comprobada normal-o-cero en su formato nativo (un subnormal
  // simple se ensancharia a un doble NORMAL; el VR4300 lo manda a Unimplemented).
  auto loadSrcD = [&] {
    if(S) { e.mov_r32_m(RAX, RBX, fpr(fs)); emitPlain32(e, RAX, RCX, b);
            e.movd_x_r32(0, RAX); e.cvtss2sd(0, 0); }
    else  { e.mov_r_m(RAX, RBX, fpr(fs)); emitPlain64(e, RAX, RCX, RDX, b);
            e.movq_x_r64(0, RAX); }
  };

  if(f2f) {
    if(W) {                                        // CVT.S.W / CVT.D.W: todo int32 es valido
      if(fn == 0x20) syncRc(-1, false);
      e.mov_r32_m(RAX, RBX, fpr(fs));
      e.cvtsi2sd_r32(0, RAX);                      // exacto en doble
      if(fn == 0x20) { roundTail(); return true; }
      e.movq_r64_x(RAX, 0); clearCause(); e.mov_m_r(RBX, fpr(fd), RAX);
      return true;
    }
    if(S) {                                        // CVT.D.S: ensanchar es siempre exacto
      loadSrcD();
      e.movq_r64_x(RAX, 0); clearCause(); e.mov_m_r(RBX, fpr(fd), RAX);
      return true;
    }
    syncRc(-1, false);                             // CVT.S.D: puede redondear
    loadSrcD();
    roundTail();
    return true;
  }

  if(toW) {
    // A entero de 32 bits: cvt(t)sd2si de 64 da el valor exacto redondeado (o el indefinido
    // 2^63 si no cabe); fuera de int32 el VR4300 levanta Unimplemented -> al CALL.
    bool trunc = fn == 0x0d;
    if(trunc) {                                    // cvttsd2si ignora el RC: solo Enable I
      if(!k.rcg) {
        e.mov_r32_m(RCX, RBX, fcrOff);
        e.test_r32_imm(RCX, 0x80);
        b.add(e.jcc_rel32_placeholder(0x85));
      }
    } else syncRc(fn == 0x0c ? 0 : fn == 0x0e ? 2 : fn == 0x0f ? 1 : -1, false);
    loadSrcD();
    e.cvtsd2si64(RAX, 0, trunc);
    e.movsxd(RCX, RAX);
    e.cmp64_rr(RCX, RAX);
    b.add(e.jcc_rel32_placeholder(0x85));
    e.mov_r_r32(R9, RAX);                          // (u64)(u32)(s32)r
    e.cvtsi2sd_r32(1, RAX);
    causeStore([&] { e.ucomisd(1, 0); });
    e.mov_m_r(RBX, fpr(fd), R9);
    return true;
  }

  // Cola del camino con banderas: resultado en R9 (32 o 64 bits segun S), MXCSR con las
  // banderas de la op. Cualquiera salvo PE, o un resultado que no sea normal-o-cero, al CALL.
  auto flagsTail = [&] {
    e.stmxcsr_rsp(40);
    e.test_rsp32_imm(40, 0x1F);
    b.add(e.jcc_rel32_placeholder(0x85));
    if(S) emitPlain32(e, R9, RCX, b); else emitPlain64(e, R9, RCX, RDX, b);
    causeStore([&] { e.test_rsp32_imm(40, 0x20); });
    e.mov_m_r(RBX, fpr(fd), R9);
  };
  usize big = 0;
  if(S && fn <= 2) {
    // ADD/SUB/MUL.S: el resultado exacto cabe en doble (producto siempre; suma/resta con
    // |ea-eb| <= 28 o un operando cero), y redondearlo una vez a simple da el bit del guest.
    syncRc(-1, false);
    e.mov_r32_m(RAX, RBX, fpr(fs)); e.mov_r32_m(RDX, RBX, fpr(ft));
    emitPlain32(e, RAX, R8, b); emitPlain32(e, RDX, R8, b);
    if(fn != 2) {
      e.mov_r_r32(R8, RAX); e.shift32_imm(5, R8, 23); e.alu32_imm(4, R8, 0xff);
      usize z1 = e.jcc_rel32_placeholder(0x84);
      e.mov_r_r32(R9, RDX); e.shift32_imm(5, R9, 23); e.alu32_imm(4, R9, 0xff);
      usize z2 = e.jcc_rel32_placeholder(0x84);
      e.alu32_rr(0x2B, R8, R9);                    // sub r8d, r9d
      e.alu32_imm(0, R8, 28);
      e.alu32_imm(7, R8, 56);
      big = e.jcc_rel32_placeholder(0x87);         // ja -> op nativa con banderas
      e.patchRel32(z1); e.patchRel32(z2);
    }
    e.movd_x_r32(0, RAX); e.cvtss2sd(0, 0);
    e.movd_x_r32(1, RDX); e.cvtss2sd(1, 1);
    if(fn == 0) e.addsd(0, 1); else if(fn == 1) e.subsd(0, 1); else e.mulsd(0, 1);
    roundTail();
    if(fn == 2) return true;
    // Exponentes muy separados: la suma exacta no cabe en doble, pero el RC ya es el del
    // invitado y la op nativa en simple redondea igual. Banderas del MXCSR a cero (salvo el
    // RC), la op, y las mismas reglas que DIV.S: cualquier bandera salvo PE al CALL.
    usize done = e.jmp_rel32_placeholder();
    e.patchRel32(big);
    e.stmxcsr_rsp(40);
    e.ld32_rsp(RCX, 40); e.alu32_imm(4, RCX, ~0x3Fu); e.st32_rsp(RCX, 40);
    e.ldmxcsr_rsp(40);
    e.movd_x_r32(0, RAX); e.movd_x_r32(1, RDX);
    if(fn == 0) e.addss(0, 1); else e.subss(0, 1);
    e.movd_r32_x(R9, 0);
    flagsTail();
    e.patchRel32(done);
    return true;
  }

  // DIV.S, SQRT.S/D y ADD/SUB/MUL/DIV.D: la op en su formato nativo con las banderas del MXCSR
  // limpias. SQRT de un negativo (no cero) levanta IE -> al CALL; sqrt(-0) = -0 sin banderas.
  // Cualquier bandera salvo PE (invalida, cero, overflow, underflow, denormal) o un resultado
  // que no sea normal-o-cero -> al CALL; PE es el Inexact del invitado.
  syncRc(-1, true);
  if(S) {
    e.mov_r32_m(RAX, RBX, fpr(fs)); e.mov_r32_m(RDX, RBX, fpr(ft));
    emitPlain32(e, RAX, R8, b); if(fn != 4) emitPlain32(e, RDX, R8, b);
    e.movd_x_r32(0, RAX); e.movd_x_r32(1, RDX);
    if(fn == 4) e.sqrtss(0, 0); else e.divss(0, 1);
    e.movd_r32_x(R9, 0);
  } else {
    e.mov_r_m(RAX, RBX, fpr(fs)); e.mov_r_m(RDX, RBX, fpr(ft));
    emitPlain64(e, RAX, R8, RCX, b); if(fn != 4) emitPlain64(e, RDX, R8, RCX, b);
    e.movq_x_r64(0, RAX); e.movq_x_r64(1, RDX);
    if(fn == 0) e.addsd(0, 1); else if(fn == 1) e.subsd(0, 1);
    else if(fn == 2) e.mulsd(0, 1); else if(fn == 3) e.divsd(0, 1); else e.sqrtsd(0, 0);
    e.movq_r64_x(R9, 0);
  }
  flagsTail();
  return true;
}

// Movimientos COP1 en linea (MFC1/DMFC1/CFC1/MTC1/DMTC1/CTC1): copia exacta de jitCop1Move
// y jitCTC1w. Con FR=0 un registro impar de 32 bits es la mitad ALTA del par (fpr[i-1]+4) y
// uno de 64 bits es el par entero; con FR=1 es el propio registro. Los gpr ya estan volcados
// (volcado dirigido de emitInterpOp) y el que se escribe se olvida al salir. Rendimientos:
// CU1=0 (Coprocessor Unusable) y una CTC1 que arma trampa -> trampolin.
static auto emitFpMove(Emitter& e, u32 op, FpBails& b) -> bool {
  u32 rs = (op >> 21) & 31, rt = (op >> 16) & 31, rd = (op >> 11) & 31;
  if(rs == 3 || rs > 6) return false;
  const s32 stOff  = (s32)offsetof(CPU, cop0) + 12 * 8 + 3;
  const s32 fcrOff = (s32)offsetof(CPU, fcr31);
  const s32 fprOff = (s32)offsetof(CPU, fpr);
  const s32 gOff   = (s32)(rt * 8);                // RBX == &gpr[0]
  if(!g_fk.cu1) { e.test_m8_imm(RBX, stOff, 0x20); // CU1
                  b.add(e.jcc_rel32_placeholder(0x84)); }
  // Emite `body(disp)` con el desplazamiento del registro FPU segun FR (impar: dos ramas).
  auto fprSel = [&](bool w64, auto&& body) {
    s32 d1 = fprOff + (s32)(rd * 8);               // FR=1, o par
    if(!(rd & 1) || g_fk.fr1) { body(d1); return; }
    s32 d0 = fprOff + (s32)((rd & ~1u) * 8) + (w64 ? 0 : 4);   // FR=0 impar
    e.test_m8_imm(RBX, stOff, 0x04);
    usize fr0 = e.jcc_rel32_placeholder(0x84);
    body(d1);
    usize end = e.jmp_rel32_placeholder();
    e.patchRel32(fr0);
    body(d0);
    e.patchRel32(end);
  };
  switch(rs) {
    case 0:                                        // MFC1
      if(!rt) return true;
      fprSel(false, [&](s32 d) { e.mov_r32_m(RAX, RBX, d); });
      e.movsxd(RAX, RAX); e.mov_m_r(RBX, gOff, RAX);
      return true;
    case 1:                                        // DMFC1
      if(!rt) return true;
      fprSel(true, [&](s32 d) { e.mov_r_m(RAX, RBX, d); });
      e.mov_m_r(RBX, gOff, RAX);
      return true;
    case 2:                                        // CFC1
      if(!rt) return true;
      if(rd == 31 || rd == 0) {
        e.mov_r32_m(RAX, RBX, rd ? fcrOff : (s32)offsetof(CPU, fcr0));
        e.movsxd(RAX, RAX);
      } else e.xor_r_r(RAX, RAX);
      e.mov_m_r(RBX, gOff, RAX);
      return true;
    case 4:                                        // MTC1: 32 bits, la otra mitad intacta
      e.mov_r32_m(RAX, RBX, gOff);
      fprSel(false, [&](s32 d) { e.mov_m_r32(RBX, d, RAX); });
      return true;
    case 5:                                        // DMTC1
      e.mov_r_m(RAX, RBX, gOff);
      fprSel(true, [&](s32 d) { e.mov_m_r(RBX, d, RAX); });
      return true;
    case 6:                                        // CTC1: solo FCR31 es escribible
      if(rd != 31) return true;
      e.mov_r32_m(RAX, RBX, gOff);
      e.alu32_imm(4, RAX, 0x0183'FFFFu);
      e.test_r32_imm(RAX, 1u << 17);               // E: Unimplemented siempre habilitada
      b.add(e.jcc_rel32_placeholder(0x85));
      e.mov_r_r32(RCX, RAX);
      e.shift32_imm(5, RCX, 5);                    // Enable (bits 7-11) frente a Cause (12-16)
      e.alu32_rr(0x23, RCX, RAX);
      e.test_r32_imm(RCX, 0x0F80u);
      b.add(e.jcc_rel32_placeholder(0x85));
      e.mov_m_r32(RBX, fcrOff, RAX);
      return true;
  }
  return false;
}

// MTC0 Status en linea, SIN cerrar el bloque. Es la mitad de __osDisableInt / __osRestoreInt
// de libultra (mfc0 / and ~IE / mtc0), lo que mas cede el bloque en SM64 tras CACHE. Escribir
// Status solo obliga a salir si puede dejar una interrupcion LISTA para tomarse, y eso exige
// IE=1 con EXL=ERL=0 tras la escritura. El camino en linea se limita a:
//  - que solo cambien IE e IM (bits 0 y 8-15): modo, EXL/ERL, CU, FR, BEV... al CALL. Asi no
//    cambia la traduccion (xlatEpoch no se toca), ni el privilegio, ni el rastro de EXL;
//  - modo kernel antes (KSU=0, o EXL/ERL puestos): si no, COP0 inutilizable = excepcion;
//  - interrupciones CERRADAS despues (IE=0, o EXL/ERL puestos), o abiertas sin ninguna
//    pendiente habilitada: Cause & IM == 0. Cause lo refresca el conductor/prologo de cada
//    eslabon (MI + timer), y dentro del bloque no vence nada -- el permiso de la cadena se
//    corta antes de cualquier plazo y del borde de Compare --, asi que ese Cause es el de
//    ESTA instruccion.
// Lo demas va a kestrel_jitMTC0S, que hace la op entera y sale del bloque detras de ella para
// que el conductor re-muestree interrupciones, como hacia el MTC0 terminal. Pisa RAX/RCX/RDX.
static const bool g_c0Inline = !std::getenv("KESTREL_JIT_NOC0INLINE");
static auto isMtc0Status(u32 op) -> bool {
  return (op >> 26) == 0x10 && ((op >> 21) & 31) == 4 && ((op >> 11) & 31) == (u32)CPU::C0_Status;
}
static auto emitMtc0Status(Emitter& e, u32 op, FpBails& b) -> void {
  const s32 stOff = (s32)(offsetof(CPU, cop0) + 8u * (u32)CPU::C0_Status);
  e.mov_r32_m(RAX, RBX, (s32)(8u * ((op >> 16) & 31)));    // nuevo (32 bits bajos de gpr[rt])
  e.mov_r32_m(RCX, RBX, stOff);                            // viejo
  e.mov_r_r32(RDX, RAX);
  e.alu32_rr(0x33, RDX, RCX);                              // xor edx, ecx
  e.test_r32_imm(RDX, ~0x0000'FF01u);
  b.add(e.jcc_rel32_placeholder(0x85));                    // cambia algo mas que IE/IM
  e.test_r32_imm(RCX, 0x6);
  usize kern = e.jcc_rel32_placeholder(0x85);              // EXL/ERL -> kernel
  e.test_r32_imm(RCX, 0x18);
  b.add(e.jcc_rel32_placeholder(0x85));                    // KSU != 0 -> CU0 / excepcion
  e.patchRel32(kern);
  e.test_r32_imm(RAX, 0x1);
  usize shut = e.jcc_rel32_placeholder(0x84);              // IE=0 -> cerradas
  e.test_r32_imm(RAX, 0x6);
  usize shut2 = e.jcc_rel32_placeholder(0x85);             // EXL/ERL -> cerradas
  e.mov_r32_m(RDX, RBX, (s32)(offsetof(CPU, cop0) + 8u * (u32)CPU::C0_Cause));
  e.alu32_rr(0x23, RDX, RAX);                              // and edx, eax
  e.test_r32_imm(RDX, 0xFF00);
  b.add(e.jcc_rel32_placeholder(0x85));                    // abiertas con IP & IM -> al CALL
  e.patchRel32(shut); e.patchRel32(shut2);
  e.movsxd(RAX, RAX);
  e.mov_m_r(RBX, stOff, RAX);
  e.mov_m_r(RBX, (s32)offsetof(CPU, cop0Unused), RAX);    // latch del bus de datos de COP0
}

extern u64 g_helperCnt[8192];
static auto emitInterpOp(Emitter& e, RegCache& rc, u32 op, u32 off, usize& exitSite,
                         RcSnap& snap, bool delay = false, bool cop0Term = false,
                         usize* retSite = nullptr, RcSnap* retSnap = nullptr) -> bool {
  if(retSite) *retSite = 0;
  u32 OP = op >> 26;
  if(OP == 0x10 || delay) g_ksu0 = false;   // MTC0 puede cambiar KSU (ver g_ksu0)
  bool ok = false;
  switch(OP) {
    // MTC0 y las de funcion COP0 (ERET / TLBR / TLBWI / TLBP), todas TERMINALES.
    case 0x10: ok = (cop0Term && (((op >> 21) & 31) == 4 || ((op >> 21) & 31) == 0x10))
                    || (!delay && isMtc0Status(op)); break;
    case 0x11: ok = ((op >> 21) & 31) != 8; break;                   // COP1 salvo BC1x
    case 0x31: case 0x35: case 0x39: case 0x3d: ok = true; break;    // LWC1/LDC1/SWC1/SDC1
    case 0x22: case 0x26: case 0x2a: case 0x2e: ok = true; break;    // LWL/LWR/SWL/SWR
    case 0x1a: case 0x1b: case 0x2c: case 0x2d: ok = true; break;    // LDL/LDR/SDL/SDR
    case 0x2f: ok = true; break;                                     // CACHE (ver kestrel_jitCACHE)
    case 0x00:
      switch(op & 63) {
        case 0x1a: case 0x1b: case 0x1c: case 0x1d: case 0x1e: case 0x1f: ok = true; break;
        default: break;
      }
      break;
    default: break;
  }
  if(!ok) return false;
  // CACHE en ranura de retardo: el trampolin de ranura no distingue I-cache de D-cache, y una
  // de I-cache tiene que cerrar el bloque. Es un caso residual -> no se absorbe el salto.
  if(delay && OP == 0x2f) return false;
  // CTC1 en ranura de retardo: al disparar la FPE, Cause.CE copia el numero de coprocesador
  // de la instruccion SIGUIENTE, y en una ranura esa es el DESTINO del salto -- que aqui aun
  // no esta escrito en pc (la fase de control del salto va despues del delay slot). Residual.
  if(delay && OP == 0x11 && ((op >> 21) & 31) == 6) return false;
  // Volcado DIRIGIDO: solo los gpr que la op nombra. El interprete lee cpu->gpr por el
  // puntero, pero una op de formato de FPU (ADD.S/MUL.S/CVT/C.cond) no nombra ninguno, y esas
  // son la inmensa mayoria de las que caen aqui en SM64. Volcar el cache entero por cada una
  // era pagar hasta cinco stores por op de FPU.
  //   COP1 rs=4/5/6 (MTC1/DMTC1/CTC1) leen gpr[rt]
  //   LWC1/LDC1/SWC1/SDC1 leen gpr[base];  LWL/LWR/SWL/SWR y LDL/LDR/SDL/SDR leen base y
  //   rt (las de carga mezclan con el, las de tienda lo leen entero)
  //   DIV/DIVU/DMULT/DMULTU/DDIV/DDIVU leen gpr[rs] y gpr[rt]
  {
    u32 rsF = (op >> 21) & 31, rtF = (op >> 16) & 31;
    if(OP == 0x11) { if(rsF >= 4 && rsF <= 6) rc.writebackOne(rtF); }
    else if(OP == 0x00) { rc.writebackOne(rsF); rc.writebackOne(rtF); }
    else { rc.writebackOne(rsF); if(OP != 0x31 && OP != 0x35) rc.writebackOne(rtF); }
  }
  // Los movimientos COP1 tienen su propio trampolin: misma firma, pero sin el montaje de
  // contexto de jitInterpOp en el camino normal (ver cpu.cpp). El resto de la mecanica --
  // volcado dirigido, sitio de salida, olvido -- es identica.
  void* fn = (void*)&jitInterpThunk;
  if(delay)           fn = (void*)&kestrel_jitInterpDelay;
  else if(OP == 0x2f) fn = (void*)&kestrel_jitCACHE;
  else if(OP == 0x10 && isMtc0Status(op)) fn = (void*)&kestrel_jitMTC0S;
  else if(OP == 0x11) switch((op >> 21) & 31) {
    case 0x00: fn = (void*)&kestrel_jitMFC1;  break;
    case 0x01: fn = (void*)&kestrel_jitDMFC1; break;
    case 0x02: fn = (void*)&kestrel_jitCFC1;  break;
      case 0x06: fn = (void*)&kestrel_jitCTC1; break;
    case 0x04: fn = (void*)&kestrel_jitMTC1;  break;
    case 0x05: fn = (void*)&kestrel_jitDMTC1; break;
    // ADD/SUB/MUL de formato: dos tercios de las cesiones en SM64. Trampolin con camino
    // rapido; los casos raros los sigue resolviendo el interprete (ver cpu.cpp).
    case 0x10: switch(op & 63) {
      case 0x00: fn = (void*)&kestrel_jitADDS; break;
      case 0x01: fn = (void*)&kestrel_jitSUBS; break;
      case 0x02: fn = (void*)&kestrel_jitMULS; break;
      case 0x03: fn = (void*)&kestrel_jitDIVS; break;
      // Conversiones: lo que mas cede el bloque despues de CTC1 (ver jitCop1Cvt).
      case 0x0d: fn = (void*)&kestrel_jitTRUNCWS; break;
      case 0x21: fn = (void*)&kestrel_jitCVTDS;   break;
      case 0x24: fn = (void*)&kestrel_jitCVTWS;   break;
      // C.cond.fmt: los dieciseis predicados por un solo trampolin (fn se lee dentro).
      default: if((op & 63) >= 0x30) fn = (void*)&kestrel_jitCMPS; break;
    } break;
    case 0x11: switch(op & 63) {
      case 0x00: fn = (void*)&kestrel_jitADDD; break;
      case 0x01: fn = (void*)&kestrel_jitSUBD; break;
      case 0x02: fn = (void*)&kestrel_jitMULD; break;
      case 0x03: fn = (void*)&kestrel_jitDIVD; break;
      case 0x0d: fn = (void*)&kestrel_jitTRUNCWD; break;
      case 0x20: fn = (void*)&kestrel_jitCVTSD;   break;
      case 0x24: fn = (void*)&kestrel_jitCVTWD;   break;
      default: if((op & 63) >= 0x30) fn = (void*)&kestrel_jitCMPD; break;
    } break;
    // W (fuente entera): CVT.S.W / CVT.D.W, lo que emite el compilador al pasar un
    // contador o un indice a coma flotante.
    case 0x14: switch(op & 63) {
      case 0x20: fn = (void*)&kestrel_jitCVTSW; break;
      case 0x21: fn = (void*)&kestrel_jitCVTDW; break;
      default: break;
    } break;
    default: break;
  }
  // FPU en linea: si la op lo admite, el caso comun va aqui y salta por encima del CALL; los
  // rendimientos caen en el CALL de abajo, que hace la op completa.
  usize fpDone = ~(usize)0;
  bool fpFmt = OP == 0x11 && ((op >> 21) & 31) >= 16, fpInl = false;
  // Diferir el camino lento a la seccion fria (ver ColdCall).
  const bool coldOk = g_jitCold && g_cold && !delay && retSite;
  FpBails coldB; bool cold = false;
  // Tambien en ranura de retardo: el camino en linea no levanta excepcion (lo que podria
  // cae al CALL, que ahi es kestrel_jitInterpDelay con EPC/BD bien puestos) y solo pisa
  // registros volatiles, como el propio CALL; la condicion del salto vive en R15.
  if(g_fpInline && OP == 0x11) {
    FpBails bails;
    bool fmtOp = ((op >> 21) & 31) >= 16;
    if(fmtOp ? emitFpInline(e, op, bails) : emitFpMove(e, op, bails)) {
      fpInl = fmtOp;
      if(coldOk) { coldB = bails; cold = true; }
      else {
        fpDone = e.jmp_rel32_placeholder();
        for(u32 i = 0; i < bails.n; i++) e.patchRel32(bails.at[i]);
      }
    }
  }
  if(g_c0Inline && !delay && OP == 0x10 && isMtc0Status(op)) {
    FpBails bails;
    emitMtc0Status(e, op, bails);
    if(coldOk) { coldB = bails; cold = true; }
    else {
      fpDone = e.jmp_rel32_placeholder();
      for(u32 i = 0; i < bails.n; i++) e.patchRel32(bails.at[i]);
    }
  }
  u32 xk = 0;
  {
    u32 rsF = (op >> 21) & 31;
    xk = (OP == 0x11 ? 4096u + rsF * 64u + (rsF >= 16 ? (op & 63) : 0u)
                     : OP * 64u + (OP == 0 ? (op & 63) : OP == 0x10 ? (rsF < 16 ? rsF : 32u + (op & 31)) : 0u)) & 8191;
  }
  if(cold) {
    // El camino lento entero va a la seccion fria; aqui el rapido sigue de corrido.
    ColdCall cc{};
    cc.bails = coldB; cc.op = op; cc.off = off; cc.fn = fn; cc.xk = xk; cc.xs = g_xStats;
    cc.newFr = cc.newRc = cc.rcg = false;
    if(g_fpKnow && fpInl) { cc.newFr = g_fkNewFr; cc.newRc = g_fkNewRc; cc.rcg = g_fkAfter.rcg; }
    else if(g_fpKnow && OP == 0x11 && !fpInl) {
      // Movimiento COP1 sin revalidacion: el hecho de modo se pierde (ver abajo); nada que
      // reponer en el camino lento.
    }
    exitSite = 0; *retSite = 0;
    if(g_fpKnow) {
      if(OP == 0x11) {
        if(fpInl) g_fk = g_fkAfter;
        else { g_fk.cu1 = true; if(fpFmt || ((op >> 21) & 31) == 6) g_fk.rcg = false; }
      } else g_fk = FpKnow{};                  // MTC0 Status
    }
    cc.snap = rc.snap();
    cc.cont = e.buf.used;
    cc.idx = 0;                                // lo pone el llamador
    g_cold->push_back(cc);
    snap = cc.snap;
    u32 wrGprC = 32;
    if(OP == 0x11 && ((op >> 21) & 31) <= 2) wrGprC = (op >> 16) & 31;
    if(wrGprC < 32) rc.forget(wrGprC);
    return true;
  }
  if(g_xStats) {                          // censo dinamico de llamadas al trampolin
    e.mov_r_imm64(RAX, (u64)&g_helperCnt[xk]);
    e.add_m64_imm32(RAX, 0, 1);
  }
  e.mov_r_r(RCX, RBX);                    // arg0 = cpu (== &gpr[0] == RBX)
  e.mov_r_imm32(RDX, op);                 // arg1 = op
  e.mov_r_imm32(R8, off);                 // arg2 = offset de la op en el bloque
  e.mov_r_imm64(RAX, (u64)fn);
  e.call_reg(RAX);
  e.test_al_al();
  exitSite = e.je_rel32_placeholder();    // al==0 → salida de control (la op ya tuvo efecto)
  // Conocimiento FPU tras la op (ver FpKnow). El camino lento lo revalida antes de reunirse.
  if(g_fpKnow) {
    if(OP == 0x11) {
      if(fpInl && !delay && retSite) {
        const s32 stOff3 = (s32)offsetof(CPU, cop0) + 12 * 8 + 3;
        usize fails[2]; int nf = 0;
        if(g_fkNewFr) { e.test_m8_imm(RBX, stOff3, 0x04); fails[nf++] = e.jcc_rel32_placeholder(0x84); }
        if(g_fkNewRc) { e.test_m8_imm(RBX, (s32)offsetof(CPU, fcr31), 0x80);
                        fails[nf++] = e.jcc_rel32_placeholder(0x85); }
        if(g_fkAfter.rcg) emitRcGuest(e);
        if(nf) {
          usize over = e.jmp_rel32_placeholder();
          for(int i = 0; i < nf; i++) e.patchRel32(fails[i]);
          *retSnap = rc.snap();
          *retSite = e.jmp_rel32_placeholder();
          e.patchRel32(over);
        }
        g_fk = g_fkAfter;
      } else {
        // Sin revalidacion: CU1 queda (si no, excepcion), FR no lo toca nadie, y el modo
        // del MXCSR lo pueden cambiar el ayudante (formato) o CTC1.
        g_fk.cu1 = true;
        if(fpFmt || ((op >> 21) & 31) == 6) g_fk.rcg = false;
      }
      if(delay) g_fk = FpKnow{};
    } else if(OP == 0x10) g_fk = FpKnow{};     // MTC0 Status: CU1/FR pueden cambiar
  }
  if(fpDone != ~(usize)0) e.patchRel32(fpDone);
  snap = rc.snap();                       // lo sucio aqui lo escribe el stub de salida
  // Olvidar SOLO el gpr que el helper escribe, no la cache entera. Las ranuras del cache
  // (RSI/RDI/R13/R14/R15) son callee-saved en Win64: sobreviven al CALL intactas, asi que
  // tirarlas era regalar la residencia. Y en SM64 esto pasa cada pocas instrucciones: la op
  // no compilable es casi siempre FPU, y una op de formato (ADD.S/MUL.S/CVT/C.cond) no toca
  // ningun gpr. El writeback de arriba SI hace falta siempre: la op puede vectorizar a una
  // excepcion y el manejador guarda el contexto leyendo cpu->gpr.
  //   COP1 rs=0/1/2 (MFC1/DMFC1/CFC1) escriben gpr[rt]
  //   COP1 rs=4/5/6 (MTC1/DMTC1/CTC1) y rs>=16 (formato) solo tocan FPR/FCR
  //   LWL/LWR y LDL/LDR escriben gpr[rt];  SWL/SWR, SDL/SDR, LWC1/LDC1/SWC1/SDC1 no
  //   DIV/DIVU/DMULT/DMULTU/DDIV/DDIVU solo HI/LO, que no estan en el cache
  u32 wrGpr = 32;                                     // 32 = ninguno
  if(OP == 0x11 && ((op >> 21) & 31) <= 2)   wrGpr = (op >> 16) & 31;
  else if(OP == 0x22 || OP == 0x26 ||
          OP == 0x1a || OP == 0x1b)          wrGpr = (op >> 16) & 31;
  if(wrGpr < 32) rc.forget(wrGpr);
  return true;
}

// Emite UNA op. Devuelve false si no es segura (fin del bloque). op ya validado != code
// que cambie flujo. `c` solo se usa para leer palabras (icFetch) en el llamador.
// El coste de MULT/MULTU se cobra AQUI porque el dynarec las emite en linea (DIV/DIVU y las
// de 64 bits salen por el thunk del interprete, que ya llama a chargeMulDiv). Con la perilla
// apagada no se emite ni un byte, asi que el codigo generado queda igual que siempre.
static const u8 g_mulDivMode = CPU::mulDivFromEnv();

// Traduccion literal de CPU::chargeMulDiv(total) a codigo emitido: RBX es el CPU.
static auto emitMulDivCharge(Emitter& e, u32 total) -> void {
  if(!g_mulDivMode) return;
  e.add_m64_imm32(RBX, (s32)offsetof(CPU, mulDivOps), 1);
  if(g_mulDivMode < 2) return;
  e.add_m32_imm32(RBX, (s32)offsetof(CPU, stallCycles),  total - 1);
  e.add_m64_imm32(RBX, (s32)offsetof(CPU, stallTotal),   total - 1);
  e.add_m64_imm32(RBX, (s32)offsetof(CPU, mulDivStall),  total - 1);
}

static auto emitMulDiv(Emitter& e, RegCache& rc, u32 funct, u32 rs, u32 rt) -> void {
  const s32 hiOff = (s32)offsetof(CPU, hi), loOff = (s32)offsetof(CPU, lo);
  // --- DIV/DIVU/DDIV/DDIVU y DMULT/DMULTU ------------------------------------------------
  // idiv/div x86 lanzan #DE en div-por-0 y en MIN/-1, que en el VR4300 son casos DEFINIDOS
  // (no-trap): div-por-0 da lo = a<0 ? 1 : -1 (DIVU/DDIVU: todo unos) y hi = a; MIN/-1 da
  // lo = MIN, hi = 0. Esos dos van por guardas en el codigo emitido, el resto por la
  // instruccion nativa. Los operandos se cargan ANTES de ramificar: la cache de registros
  // no puede cambiar de estado dentro de una rama. Mismo coste que el interprete.
  if(funct != 0x1c && funct != 0x1d) {           // DIV DIVU DDIV DDIVU
    bool w = funct >= 0x1e, sg = !(funct & 1);
    emitMulDivCharge(e, w ? 69 : 37);
    if(w) { rc.ld64(RAX, rs); rc.ld64(RCX, rt); }
    else  { rc.ld32(RAX, rs); rc.ld32(RCX, rt); }
    if(w) e.cmp64_imm(RCX, 0); else e.alu32_imm(7, RCX, 0);
    usize zero = e.jcc_rel32_placeholder(0x84);
    usize ovf = 0;
    if(sg) {
      if(w) e.cmp64_imm(RCX, 0xFFFF'FFFFu); else e.alu32_imm(7, RCX, 0xFFFF'FFFFu);
      usize norm = e.jcc_rel32_placeholder(0x85);
      if(w) { e.mov_r_imm64(RDX, 1ull << 63); e.cmp64_rr(RAX, RDX); }
      else  e.alu32_imm(7, RAX, 0x8000'0000u);
      ovf = e.jcc_rel32_placeholder(0x84);
      e.patchRel32(norm);
    }
    if(sg) { if(w) e.cqo(); else e.cdq(); }
    else e.alu32_rr(0x33, RDX, RDX);              // xor edx, edx
    e.f7grp(w, sg ? 7 : 6, RCX);
    if(!w) { e.movsxd(RAX, RAX); e.movsxd(RDX, RDX); }
    e.mov_m_r(RBX, loOff, RAX); e.mov_m_r(RBX, hiOff, RDX);
    usize done = e.jmp_rel32_placeholder();
    e.patchRel32(zero);                            // b == 0: hi = a, lo segun signo
    if(!w) e.movsxd(RAX, RAX);
    e.mov_m_r(RBX, hiOff, RAX);
    if(sg) { e.shift64_imm(7, RAX, 63); e.alu64_rr(0x03, RAX, RAX); e.not64(RAX); }  // ~(2*(a>>63))
    else     e.mov_r_imm64(RAX, ~0ull);
    e.mov_m_r(RBX, loOff, RAX);
    if(sg) {
      usize out = e.jmp_rel32_placeholder();
      e.patchRel32(ovf);                           // MIN / -1: lo = MIN, hi = 0
      if(!w) e.movsxd(RAX, RAX);
      e.mov_m_r(RBX, loOff, RAX);
      e.alu32_rr(0x33, RAX, RAX); e.mov_m_r(RBX, hiOff, RAX);
      e.patchRel32(out);
    }
    e.patchRel32(done);
    return; }
  {                                                // DMULT DMULTU: 64x64 -> 128 en RDX:RAX
    emitMulDivCharge(e, 8);
    rc.ld64(RAX, rs); rc.ld64(RCX, rt);
    e.f7grp(true, funct == 0x1c ? 5 : 4, RCX);
    e.mov_m_r(RBX, loOff, RAX); e.mov_m_r(RBX, hiOff, RDX);
  }
}


static auto emitSafeOp(Emitter& e, RegCache& rc, u32 op) -> bool {
  u32 OP = op >> 26;
  u32 rs = (op >> 21) & 31, rt = (op >> 16) & 31, rd = (op >> 11) & 31, sa = (op >> 6) & 31;
  u32 funct = op & 63;
  u32 imm16 = op & 0xFFFF;
  s32 simm  = (s16)imm16;

  auto store32 = [&](u32 dst) {   // rax(32) → sext32 → gpr[dst] (skip si dst==0)
    if(dst) { e.movsxd(RAX, RAX); rc.st64(RAX, dst); }
  };
  auto store64 = [&](u32 dst) { if(dst) rc.st64(RAX, dst); };
  const s32 hiOff = (s32)offsetof(CPU, hi);   // HI/LO son campos u64 del CPU (RBX==&cpu)
  const s32 loOff = (s32)offsetof(CPU, lo);

  static const int only = std::getenv("KESTREL_JIT_ONLY1") ? 1 : 0;
  if(only && !(OP == 0x09)) return false;   // narrowing: solo ADDIU
  switch(OP) {
    case 0x09: /*ADDIU*/ if(!rt) return true; rc.ld32(RAX, rs); e.alu32_imm(0, RAX, (u32)simm); store32(rt); return true;
    // NOTA: DADDIU/DADDU/DSUBU/DSLL*/DSRA*/DSLLV* (64-bit ISA) NO se JITean: en VR4300 trapean
    // RI cuando el modo 64-bit está off (supervisor/user 32-bit, systemtest daddiu_supervisor_32).
    // El chequeo de modo es estado runtime (Status.KX/SX/UX + KSU) y no vale garantizarlo en compile.
    // Terminan el bloque → el intérprete las ejecuta con la semántica de trap correcta.

    case 0x0c: /*ANDI*/  if(!rt) return true; rc.ld64(RAX, rs); e.alu64_imm(4, RAX, imm16); store64(rt); return true;
    case 0x0d: /*ORI*/   if(!rt) return true; rc.ld64(RAX, rs); e.alu64_imm(1, RAX, imm16); store64(rt); return true;
    case 0x0e: /*XORI*/  if(!rt) return true; rc.ld64(RAX, rs); e.alu64_imm(6, RAX, imm16); store64(rt); return true;
    case 0x0a: /*SLTI*/  if(!rt) return true; rc.ld64(RAX, rs); e.cmp64_imm(RAX,(u32)simm); e.setcc(0x9C,RAX); e.movzx_r8(RAX,RAX); store64(rt); return true;
    case 0x0b: /*SLTIU*/ if(!rt) return true; rc.ld64(RAX, rs); e.cmp64_imm(RAX,(u32)simm); e.setcc(0x92,RAX); e.movzx_r8(RAX,RAX); store64(rt); return true;
    case 0x0f: /*LUI*/   if(!rt) return true; e.mov_r_imm32(RAX,(u32)(imm16<<16)); store32(rt); return true;
    case 0x00: /*SPECIAL*/
      switch(funct) {
        case 0x00: /*SLL*/  if(!rd) return true; rc.ld32(RAX, rt); if(sa) e.shift32_imm(4,RAX,(u8)sa); store32(rd); return true;
        case 0x02: /*SRL*/  if(!rd) return true; rc.ld32(RAX, rt); if(sa) e.shift32_imm(5,RAX,(u8)sa); store32(rd); return true;
        case 0x03: /*SRA*/  if(!rd) return true; rc.ld64(RAX, rt); if(sa) e.shift64_imm(7,RAX,(u8)sa); store32(rd); return true;  // VR4300: SRA aritmético de 64b, low32 sign-ext
        case 0x04: /*SLLV*/ if(!rd) return true; rc.ld32(RCX, rs); rc.ld32(RAX, rt); e.shift32_cl(4,RAX); store32(rd); return true;
        case 0x06: /*SRLV*/ if(!rd) return true; rc.ld32(RCX, rs); rc.ld32(RAX, rt); e.shift32_cl(5,RAX); store32(rd); return true;
        case 0x07: /*SRAV*/ if(!rd) return true; rc.ld32(RCX, rs); e.alu32_imm(4,RCX,31); rc.ld64(RAX, rt); e.shift64_cl(7,RAX); store32(rd); return true;  // VR4300: 64b arith shift, cnt=rs&31, low32 sign-ext
        case 0x21: /*ADDU*/ if(!rd) return true; rc.ld32(RAX, rs); rc.alu32(0x03, RAX, rt); store32(rd); return true;
        case 0x23: /*SUBU*/ if(!rd) return true; rc.ld32(RAX, rs); rc.alu32(0x2B, RAX, rt); store32(rd); return true;
        case 0x24: /*AND*/  if(!rd) return true; rc.ld64(RAX, rs); rc.alu64(0x23, RAX, rt); store64(rd); return true;
        case 0x25: /*OR*/   if(!rd) return true; rc.ld64(RAX, rs); rc.alu64(0x0B, RAX, rt); store64(rd); return true;
        case 0x26: /*XOR*/  if(!rd) return true; rc.ld64(RAX, rs); rc.alu64(0x33, RAX, rt); store64(rd); return true;
        case 0x27: /*NOR*/  if(!rd) return true; rc.ld64(RAX, rs); rc.alu64(0x0B, RAX, rt); e.not64(RAX); store64(rd); return true;
        case 0x2a: /*SLT*/  if(!rd) return true; rc.ld64(RAX, rs); rc.cmp64(RAX, rt); e.setcc(0x9C,RAX); e.movzx_r8(RAX,RAX); store64(rd); return true;
        case 0x2b: /*SLTU*/ if(!rd) return true; rc.ld64(RAX, rs); rc.cmp64(RAX, rt); e.setcc(0x92,RAX); e.movzx_r8(RAX,RAX); store64(rd); return true;
        // --- HI/LO move (32-bit base ISA, no gated) ---------------------------------------
        case 0x10: /*MFHI*/ if(!rd) return true; e.mov_r_m(RAX,RBX,hiOff); rc.st64(RAX, rd); return true;
        case 0x12: /*MFLO*/ if(!rd) return true; e.mov_r_m(RAX,RBX,loOff); rc.st64(RAX, rd); return true;
        case 0x11: /*MTHI*/ rc.ld64(RAX, rs); e.mov_m_r(RBX,hiOff,RAX); return true;
        case 0x13: /*MTLO*/ rc.ld64(RAX, rs); e.mov_m_r(RBX,loOff,RAX); return true;
        // --- MULT/MULTU (32×32→64): LO=sext32(low32), HI=sext32(high32). imul64 low64 = producto
        //     exacto (operandos extendidos a 64b; signo por movsxd sí/no). NO escribe gpr → no rd. -
        case 0x18: /*MULT*/ {
          emitMulDivCharge(e, 5);
          rc.ld32(RAX, rs); e.movsxd(RAX,RAX); rc.ld32(RCX, rt); e.movsxd(RCX,RCX);
          e.imul64(RAX,RCX);                                  // rax = (s32)rs * (s32)rt (64b)
          e.mov_r_r(RDX,RAX); e.movsxd(RDX,RDX); e.mov_m_r(RBX,loOff,RDX);   // lo = sext32(low32)
          e.shift64_imm(5,RAX,32); e.movsxd(RAX,RAX); e.mov_m_r(RBX,hiOff,RAX); // hi = sext32(high32)
          return true; }
        case 0x19: /*MULTU*/ {
          emitMulDivCharge(e, 5);
          rc.ld32(RAX, rs); rc.ld32(RCX, rt);             // operandos zero-ext (u32)
          e.imul64(RAX,RCX);                                  // low64 = (u32)rs*(u32)rt (cabe en 64b)
          e.mov_r_r(RDX,RAX); e.movsxd(RDX,RDX); e.mov_m_r(RBX,loOff,RDX);
          e.shift64_imm(5,RAX,32); e.movsxd(RAX,RAX); e.mov_m_r(RBX,hiOff,RAX);
          return true; }
        // DIV/DIVU: ver emitMulDiv (guardas de div-por-0 y MIN/-1). Las de 64 bits van por
        // emitAlu64, que pone la guarda de modo (RI fuera de kernel sin UX/SX).
        case 0x1a: case 0x1b: {
          static const bool noDiv = std::getenv("KESTREL_JIT_NODIV") != nullptr;   // A/B
          if(noDiv) return false;
          emitMulDiv(e, rc, funct, rs, rt); return true; }
        // --- MOVZ/MOVN: mueve rs→rd si rt==0 / rt!=0, si no rd intacto (cmov 64b) ----------
        // No mode-gated (conditional-move ISA, disponibles en todos los modos; systemtest OK).
        case 0x0a: /*MOVZ*/ if(!rd) return true; rc.ld64(RAX, rd); rc.ld64(RCX, rs); rc.ld64(RDX, rt); e.cmp64_imm(RDX,0); e.cmovz(RAX,RCX); store64(rd); return true;
        case 0x0b: /*MOVN*/ if(!rd) return true; rc.ld64(RAX, rd); rc.ld64(RCX, rs); rc.ld64(RDX, rt); e.cmp64_imm(RDX,0); e.cmovnz(RAX,RCX); store64(rd); return true;
        default: return false;   // JR/JALR/MULT/DIV/ADD/SUB(trap)/etc → fin del bloque
      }
    default: return false;       // branches, loads/stores, cop, 64-bit, etc → fin
  }
}

// Diagnóstico: emite una sola op y la ejecuta sobre gpr sembrado con rs/rt.
auto opSelfTest(u32 op, u64 rsVal, u64 rtVal, u32 dst) -> u64 {
  CodeBuffer buf;
  if(!buf.init(4096)) return 0;
  Emitter e(buf);
  u8* entry = buf.cursor();
  e.push_rbx(); e.mov_rbx_rcx();
  RegCache rcOff; rcOff.reset(&e, false);   // diagnostico: gpr siempre por memoria
  bool ok = emitSafeOp(e, rcOff, op);
  e.pop_rbx(); e.ret();
  buf.finalize(entry);
  u32 rs = (op >> 21) & 31, rt = (op >> 16) & 31;
  u64 gpr[32] = {0};
  gpr[rs] = rsVal; gpr[rt] = rtVal;
  reinterpret_cast<BlockFn>(entry)(gpr, nullptr);   // solo-ALU: cpu no se usa
  std::fprintf(stderr, "[jit] opSelfTest op=%08x safe=%d bytes=%zu rs[%u]=%016llx rt[%u]=%016llx -> gpr[%u]=%016llx\n",
               op, ok, buf.used, rs, (unsigned long long)rsVal, rt, (unsigned long long)rtVal,
               dst, (unsigned long long)gpr[dst]);
  return gpr[dst];
}

// Compila desde phys hasta la primera op insegura / tope / fin de RDRAM.
// Diagnóstico: histograma del opcode del LEADER cuando el bloque no compila (nOps==0).
// Dice si el compile-fail steady lo dominan branches vs mult/div/cop/etc → decide el diseño.
u64 g_compFailOp[64] = {0};
u64 g_helperCnt[8192] = {0};   // KESTREL_JIT_STATS: llamadas al trampolin por op
// Cuando el LIDER no compilable es un salto, lo que suele fallar es su RANURA DE RETARDO:
// el salto se absorbe con ella o no se absorbe. Censo por opcode de la ranura (solo stats).
u64 g_compFailDelay[64] = {0};
u64 g_compFailDelaySpec[64] = {0};
// Histograma de la op que TERMINA el bloque (no la líder): dice qué falta por cubrir para
// alargar los bloques, que es la palanca directa sobre avgK.
u64 g_endOp[64] = {0}, g_endSpecial[64] = {0}, g_endRegimm[32] = {0};
u64 g_compFailSpecial[64] = {0};
u64 g_compFailRegimm[32] = {0};
// COP0/COP1 son las dos clases gordas del compile-fail, y el opcode primario no dice
// nada: MFC0 y MTC0 comparten OP10, y bajo OP11 conviven MFC1/CFC1/BC1 con la aritmetica
// que YA se compila. El sub-histograma va por el campo rs, que es quien las separa.
u64 g_compFailCop[2][32] = {{0}};
u64 g_compFailC0Rd[2][32] = {{0}};   // MFC0/MTC0 por registro COP0 destino
const int g_compFailOn = std::getenv("KESTREL_JIT_STATS") ? 1 : 0;

// Block-linking Step 1 (gated KESTREL_JIT_LINK): emite un prólogo re-validable en cada bloque
// que re-muestrea interrupt + borde de timer antes del cuerpo. Neutro sin enlace todavía (el
// driver ya hace el mismo check, así que es redundante pero debe seguir dando 0/3721); habilita
// que un futuro bloque enlazado entre aquí sin volver al driver. El prólogo llama a este
// trampolín (extern "C" → dirección plana, ABI Win64: RCX=cpu, RDX=K).
// Enlace de bloques: ACTIVO por defecto. Con avgK≈3 el viaje de ida y vuelta al driver es
// el coste dominante, así que saltar directo al sucesor es la palanca principal.
// KESTREL_JIT_NOLINK lo apaga para bisecar.
static const int g_jitLink = std::getenv("KESTREL_JIT_NOLINK") ? 0 : 1;
// A/B de la cache de destinos indirectos (JR/JALR). Apagarla deja el resto del enlace intacto,
// asi que mide exactamente lo que aporta el sondeo.
static const int g_noItc = std::getenv("KESTREL_JIT_NOITC") ? 1 : 0;
// Enlace de codigo TLB-mapeado. Ver `linkable` en compileBlock: el destino se traduce al
// compilar y cpu.tlbGen desarma todo cuando el mapeo cambia. KESTREL_JIT_NOTLBLINK lo apaga
// para bisecar (con el apagado, un juego que corre desde el TLB no enlaza nada, que es el
// comportamiento anterior).
const int g_jitTlbLink = std::getenv("KESTREL_JIT_NOTLBLINK") ? 0 : 1;
// Permitir que un enlace TLB apunte FUERA de la pagina de entrada del bloque. Cruzar
// pagina obliga a traducir el destino con una sonda del TLB en tiempo de compilacion, y
// esa traduccion es independiente de la que valida el driver para la entrada. Dentro de
// la pagina, en cambio, la phys del destino sale de la MISMA traduccion que ya trajo aqui.
// Enlace entre bloques cuyo destino cae en OTRO marco de 4 KB mapeado por TLB. Por defecto SI
// desde 2026-09-23. Perfect Dark ejecuta su codigo desde 0x70000000, o sea todo TLB, y la
// restriccion de "mismo marco" dejaba sin enlazar cualquier JAL a otra funcion: 1,28 M de
// salidas lentas directas por corrida, el 41 % de las entradas al driver. Es sano por la
// misma razon que el enlace TLB de mismo marco: la traduccion queda congelada en el sitio,
// y cualquier remapeo (TLBWI/TLBWR/ASID) sube cpu.tlbGen, que desenlaza TODO y re-sondea la
// VA de cada sitio TLB antes del siguiente despacho. Medido en PD (F993->F1082, 3 tandas
// intercaladas): 4590/4656 ms -> 4042/4122 ms, -12 % de pared, mismo [statehash] y mismos
// campos. KESTREL_JIT_NOTLBXPAGE=1 vuelve al comportamiento viejo para bisecar.
static const int g_jitTlbXPage = std::getenv("KESTREL_JIT_NOTLBXPAGE") ? 0 : 1;
// DIAGNOSTICO: deja el ITC por ruta TLB pero desactiva el enlace ESTATICO TLB.
static const int g_noTlbStatic = std::getenv("KESTREL_JIT_NOTLBSTATIC") ? 1 : 0;
// KESTREL_DUEWATCH=0 quita la comprobacion (A/B).
static auto dueWatchOn() -> bool {
  static const bool v = [] { const char* e = std::getenv("KESTREL_DUEWATCH");
                             return !e || !*e || (bool)std::strcmp(e, "0"); }();
  return v;
}
// Camino rápido en línea del prólogo re-validable (ver más abajo). KESTREL_JIT_NOFAST=1 lo
// apaga y deja la llamada al trampolín en cada entrada de bloque (bisección).
static const int g_jitFast = std::getenv("KESTREL_JIT_NOFAST") ? 0 : 1;
// Los modos diff ejecutan el bloque como una unidad aislada y lo comparan contra K pasos del
// intérprete; una cadena enlazada retiraría más ops que K y rompería esa comparación. Son
// modos de diagnóstico, así que ahí simplemente no se emiten enlaces.
static const int g_jitDiffAny = (std::getenv("KESTREL_JIT_DIFF") || std::getenv("KESTREL_JIT_BRDIFF")) ? 1 : 0;

// Formación de trazas: en un branch condicional HACIA ADELANTE la caída se sigue compilando
// dentro del mismo bloque en vez de terminarlo. Motivo medido: el 80% de los finales de bloque
// son branches reales, así que el techo de avgK (~3) es el tamaño del bloque básico de MIPS,
// no una carencia de cobertura — la única forma de subirlo es que un bloque abarque VARIOS
// bloques básicos. Hacia adelante porque ahí la caída es el camino caliente (guardas de `if`);
// hacia atrás el salto es la arista de retorno de un bucle y la caída es la salida, fría.
// Se desactiva con los modos diff, que comparan el bloque contra K pasos SECUENCIALES del
// intérprete y no saben seguir una traza.
// MEDIDO: net NEGATIVO en SM64 (24.8% vs 27.3% de velocidad N64), así que va OPT-IN.
// Causa probable: el bloque trazado declara una K mayor, y el prólogo re-validable
// rechaza el eslabón encadenado cuando K no cabe en la ventana o roza el borde de
// timer — más viajes al driver, más código emitido, peor I-cache del host.
static const int g_jitTrace = (std::getenv("KESTREL_JIT_TRACE") && !g_jitDiffAny) ? 1 : 0;
extern "C" KJIT_ABI u32 kestrel_jitProceedTramp(void* cpu, u32 K);
extern "C" KJIT_ABI u8 kestrel_jitIcRefill(void* cpu, u32 entry, u32 base);

extern const char* g_jitDump;   // KESTREL_JIT_DUMP (definido mas abajo)
auto compileBlock(CPU& c, u32 phys) -> Block {
  // El codegen de branch-en-bloque y de mem-op direccionan el CPU vía RBX, que apunta
  // a &gpr[0]. Eso solo equivale a `cpu` si gpr es el primer miembro (offset 0).
  static_assert(offsetof(CPU, gpr) == 0, "gpr debe ser el primer miembro de CPU (RBX==cpu)");
  Block b;
  const u32 kMaxOps = 64;
  // El camino rapido de memoria compara la fisica contra este campo en vez de perseguir
  // mem->rdram.size(). Se refresca aqui, no en cada despacho: Memory::reset() dimensiona la
  // RDRAM en el arranque, mucho antes de que se compile el primer bloque, y el valor lo lee
  // el codigo emitido en tiempo de ejecucion -- no se hornea en el.
  c.jitRdramSz = c.mem ? (u32)c.mem->rdram.size() : 0;
  Emitter e(c.jitCache->buf);
  u8* entry = c.jitCache->buf.cursor();

  // Prólogo Etapa 2b: rbx=gpr (rcx), r12=cpu (rdx). Ambos callee-saved → push/pop.
  // RSP: entry≡8 (tras el call); 7 push→≡0; sub 48 (48≡0) → ≡0 (16-alin para CALL Win64).
  // RSI/RDI/R13/R14/R15 son las ranuras de residencia de GPR (RegCache): no-volatiles en
  // Win64, asi que sobreviven a los CALL de helper dentro del bloque. Se empujan siempre
  // (con o sin residencia) para que TODOS los bloques compartan marco: un salto enlazado
  // aterriza en el linkEntry del sucesor y reutiliza el marco del predecesor.
  e.push_reg(RBX); e.push_reg(R12);
  e.push_reg(RSI); e.push_reg(RDI); e.push_reg(R13); e.push_reg(R14); e.push_reg(R15);
  e.mov_r_r(RBX, RCX);
  e.mov_r_r(R12, RDX);
  e.sub_rsp_imm8(48);

  static const bool g_jitRegCache = std::getenv("KESTREL_JIT_NOREGCACHE") == nullptr;
  RegCache rc; rc.reset(&e, g_jitRegCache);

  const s32 guardOff   = (s32)((char*)&c.jitGuard     - (char*)&c);
  const s32 timerOff   = (s32)((char*)&c.timerIntr    - (char*)&c);
  const s32 duePendOff = (s32)((char*)&c.jitPendSeen  - (char*)&c);
  // MI: el prólogo lee (mi_intr & mi_mask) directamente. Las dos viven en la misma línea de
  // caché de Rcp, así que el segundo acceso es gratis; el desplazamiento es constante.
  const s32 miMaskDelta = (s32)((char*)&c.mem->rcp.mi_mask - (char*)&c.mem->rcp.mi_intr);
  // Block-linking Step 1: prólogo re-validable. call kestrel_jitProceedTramp(cpu, K); si
  // devuelve 0 (bail: interrupt pendiente o borde de timer) → eax=0, cae al epílogo con pc
  // intacto y el driver re-despacha por la ruta lenta. K aún no se conoce (depende del cuerpo)
  // → placeholder imm32 parcheado con pokeU32 tras compilar. RAX/RCX/RDX son scratch aquí (aún
  // no hay estado guest vivo), RBX/R12 los preserva el trampolín (callee-saved en Win64).
  std::vector<usize> linkBailJmps;   // je (al==0) de cada prólogo → stub de bail
  usize kImmAt = 0, kSubAt = 0;      // imm32 de K: uno en el arg del trampolín, otro en el `sub`
  // Punto de entrada ENLAZADO (Step 3): el marco (push rbx/r12 + mov + sub rsp) ya lo montó el
  // predecesor de la cadena, así que un salto enlazado aterriza AQUÍ, justo en el prólogo
  // re-validable. El epílogo de este bloque desmonta ese marco y retorna al driver que llamó al
  // primer bloque de la cadena — la profundidad de pila no crece con la longitud de la cadena.
  b.linkEntry = c.jitCache->buf.cursor();
  if(g_jitLink) {
    // Camino rápido EN LÍNEA. El trampolín cuesta una llamada Win64 + una veintena de accesos a
    // campos repartidos por el struct, y se pagaba en CADA entrada de bloque — o sea cada ~3
    // instrucciones guest, el 34% del hilo de CPU medido con el perfilador de host. Pero lo que
    // comprueba solo puede cambiar por dos vías mientras la cadena corre: el borde
    // Count==Compare (determinista, y el trampolín ya nos dijo cuántas ops faltan → `jitGuard`)
    // y una interrupción asíncrona del RCP (una lectura de MI, más barata en línea que la
    // llamada). Todo lo demás — Status/Cause, EPC, halted, el modo del RCP — solo cambia en ops
    // interpretadas, y esas terminan el bloque y devuelven el control al driver. Así que si hay
    // permiso y no hay interrupción, se entra al cuerpo sin llamar a nadie.
    const bool fastOk = g_jitFast;
    usize fastToSlow[5] = {0,0,0,0,0}; usize fastToBody = 0; int nSlow = 0;
    if(fastOk) {
      e.mov_r32_m(RAX, RBX, guardOff);            // eax = ops permitidas
      e.alu32_imm_force(5, RAX, 0);               // sub eax, K (placeholder, imm32 fijo)
      kSubAt = c.jitCache->buf.used - 4;
      fastToSlow[nSlow++] = e.jb_rel32_placeholder();   // sin margen → trampolín
      e.mov_r_imm64(RDX, (u64)&c.mem->rcp.mi_intr);
      e.mov_r32_m(RCX, RDX, 0);                   // ecx = MI_INTR
      e.and_r32_m(RCX, RDX, miMaskDelta);         // ecx &= MI_MASK
      fastToSlow[nSlow++] = e.jne_rel32_placeholder();  // interrupción del RCP pendiente
      e.cmp_m8_imm(RBX, timerOff, 0);             // latch Count==Compare ya disparado
      fastToSlow[nSlow++] = e.jne_rel32_placeholder();
      // Plazo ARMADO A MITAD DE CADENA. El permiso de arriba lo calculo el trampolin con los
      // plazos que habia ENTONCES (rcpDueIn); el que arma despues el hilo del RSP -- fin de
      // tarea del SP, fin del RDP, escritura DPC del diario -- no lo ve nadie hasta que el
      // permiso se agota, y ahi MI_SP cae en una instruccion que depende del ANFITRION,
      // porque a ese permiso lo recorta el regulador con el reloj VIVO del RSP. Comparar aqui
      // el mapa de plazos cuesta una lectura de linea compartida por bloque y hace que el
      // corte caiga EXACTAMENTE en el plazo, como en Lockstep.
      if(dueWatchOn() && c.mem && c.mem->rcpMode == Memory::RcpMode::Threaded && Memory::rcpDeadlineOn()) {
        e.mov_r_imm64(RDX, (u64)&c.mem->rcpPend);
        e.mov_r32_m(RCX, RDX, 0);
        e.alu32_imm(4, RCX, kPendWatch);          // and ecx, mascara
        e.cmp_r32_m(RCX, RBX, duePendOff);
        fastToSlow[nSlow++] = e.jne_rel32_placeholder();
      }
      // En LOCKSTEP el hilo CPU interleavea pasos del RSP en cuanto un store lo arranca, así
      // que un bloque de la cadena que lo arranque tiene que devolver el control. Es un byte
      // en una línea de caché propia (rsp.running); comprobarlo aquí cuesta lo mismo que en
      // el trampolín y deja el camino rápido válido en los dos modos del RCP.
      //
      // MEDIDO 2026-08-20, no quitar en THREADED aunque el trampolín no la mire ahí: esta
      // guarda manda al trampolín en cada eslabón mientras el RSP tenga trabajo (376M veces
      // por run contra 25M por permiso agotado) y parece puro desperdicio, pero quitarla
      // EMPEORA el reloj de pared — 2500 campos de SM64 pasan de 119 s a 139 s y el guest
      // ejecuta 20.3 G instrucciones en vez de 8.6 G. Sin ella la CPU emulada corre mucho
      // por delante del RCP y todo el exceso se va en el spin del juego esperandolo, que
      // ademas martillea los registros MMIO que los workers escriben. El emulador no tiene
      // regulador de velocidad, asi que hoy esta guarda hace de freno.
      //
      // RE-MEDIDO con el regulador ya puesto (Memory::rcpPace): sigue siendo catastrofico
      // quitarla — 500 campos de SM64 pasan de 20.3 s a 358 s y 1.2 G instrucciones a
      // 56 G. O sea que la guarda no es solo un freno: sin ella el hilo CPU gira sobre los
      // registros MMIO que los workers escriben y les hunde el subsistema de memoria
      // (ping-pong de lineas entre nucleos), asi que el RSP tarda 18x en la misma tarea.
      // No quitarla. Lo que falta para poder hacerlo no es un regulador, es que el guest
      // no gire: esperar por interrupcion en vez de sondear registros del RCP.
      // TAMBIEN EN THREADED, desde que el enlace de bloques alcanza al codigo TLB-mapeado.
      // Se habia quitado de Threaded confiando en que la regulacion CPU<->RSP viajase dentro
      // del permiso (jitGuard <- Memory::rcpPace): eso evaluaba el freno una vez cada `allow`
      // ops en lugar de una por bloque. Medido en Perfect Dark, que si encadena de verdad, ese
      // grano no regula NADA: de 8,05 M permisos concedidos solo 238 los recorto el regulador
      // (el resto los fijaba el tope de 4096 ops), porque rcpPace no frena hasta que la CPU
      // adelanta kPaceSlack instrucciones al RSP, que entonces era 1 M. Con esa holgura el hilo de
      // CPU corria al ~1200% de la velocidad del N64 y el juego se descarrilaba (pc a datos, TLBL,
      // ~1 de cada 3 arranques). Con la guarda puesta: 8/8 arranques limpios y sigue 3,2x por encima de la
      // linea base sin enlace. La condicion es cualitativa -- hay tarea de RSP en vuelo o no --
      // y lo unico que hace es devolver el control al trampolin, que es donde vive la
      // regulacion; no cambia ni un bit del estado del guest.
      // Revisado 2026-09-03, ya con kPaceSlack en 4096 (= kGuardMaxOps): la guarda SIGUE haciendo
      // falta. Sin ella Perfect Dark se cuelga 1 de cada 16 arranques (con ella, 40 de 40 limpios)
      // y SM64 se desvia del oraculo: 300 intercambios dan 1024 campos VI con guarda y 1804 sin
      // ella (lockstep, el oraculo, da ~4,09 campos por intercambio; 3,41 con guarda, 6,01 sin).
      // La pared "mejora" de 5,50s a 4,51s justo por eso: son campos girados, no trabajo hecho.
      // KESTREL_JIT_NORSPGUARD=1 la quita para poder medir el A/B.
      //
      // Revisado 2026-09-17: en Threaded CON plazos ya sobra. Desde la barrera del SP
      // (spBarrierEff) la CPU no puede pasar del reloj de invitado publicado por el RSP, y ese
      // plazo viaja dentro del permiso (rcpDueIn -> siDue en jitReenterProceed), asi que el
      // adelanto que la guarda frenaba en pared ya no existe en tiempo de invitado. Lo que
      // quedaba era el coste: libdragon deja `rspq` corriendo siempre, y junkrunner64 caia en
      // el trampolin cada ~8 instrucciones (55 M llamadas por 200 intercambios, ~30 % del hilo
      // de CPU). Medido sin ella, mismo md5 y mismas instrucciones retiradas: junkrunner64 200
      // intercambios 17,9 -> 11,9 s, DK64 1.500 M 13,0 -> 11,7 s, SM64 300 intercambios
      // 7,0 -> 6,7 s, Perfect Dark 600 intercambios 16/16 arranques limpios y md5 identico.
      // En Lockstep (o Threaded sin plazos) se queda: ahi no hay barrera que acote.
      // KESTREL_JIT_RSPGUARD=1 la vuelve a poner en Threaded para bisecar.
      static const bool noRspGuard = std::getenv("KESTREL_JIT_NORSPGUARD") != nullptr;
      static const bool forceRspGuard = std::getenv("KESTREL_JIT_RSPGUARD") != nullptr;
      const bool barrierBounds = c.mem->rcpMode == Memory::RcpMode::Threaded && Memory::rcpDeadlineOn() && Memory::spBarrierOn();
      if(!noRspGuard && (forceRspGuard || !barrierBounds)) {
        // `brake`, no `running`: es running MENOS el aparcamiento (ver Rsp::brake). Un RSP
        // aparcado no corre microcodigo ni toca MMIO, y a la CPU la acotan igual la barrera de
        // invitado del SP y rcpPace; devolver el control aqui durante el aparcamiento eran 22 M
        // llamadas al trampolin por corrida de 300 campos de DK64 sin regular nada.
        e.mov_r_imm64(RDX, (u64)&c.mem->rsp.brake);
        e.cmp_m8_imm(RDX, 0, 0);
        fastToSlow[nSlow++] = e.jne_rel32_placeholder();
      }
      e.mov_m_r32(RBX, guardOff, RAX);            // consume el permiso
      fastToBody = e.jmp_rel32_placeholder();
    }
    for(int k = 0; k < nSlow; k++) e.patchRel32(fastToSlow[k]);
    e.mov_r_r(RCX, R12);                                    // arg1 = cpu
    e.mov_r_imm32(RDX, 0);                                  // arg2 = K (placeholder)
    kImmAt = c.jitCache->buf.used - 4;                      // offset del imm32 de K
    e.mov_r_imm64(RAX, (u64)&kestrel_jitProceedTramp);
    e.call_reg(RAX);
    e.test_al_al();                                         // al==0 → bail
    linkBailJmps.push_back(e.je_rel32_placeholder());
    if(fastOk) e.patchRel32(fastToBody);
  }

  std::vector<usize> bailSites;   // offset del disp32 del je de cada mem-op
  // Salidas inmediatas por store a un registro de control del RCP (ver emitMemOp/CPU::jitMemOp).
  std::vector<usize> mmioSites; std::vector<u32> mmioIdx; std::vector<RcSnap> mmioSnap;
  std::vector<u32>   bailIdx;     // ops retiradas antes de esa mem-op (índice)
  std::vector<RcSnap> bailSnap;   // ranuras sucias en cada bail (spill perezoso)
  std::vector<usize> interpSites; // je de cada op interpretada (salida de control)
  std::vector<u32>   interpIdx;   // ops retiradas INCLUYENDO esa op (ya tuvo efecto)
  std::vector<RcSnap> interpSnap; // ranuras sucias en cada salida de op interpretada

  // Enclavamientos de la tuberia (KESTREL_INTERLOCK, ver CPU::ilkStep). El interprete cobra la
  // PAREJA en la frontera de cada instruccion, antes de ejecutarla; aqui se emite la misma
  // frontera delante de cada op. Dentro del bloque la op anterior se conoce al compilar, asi
  // que el cobro es una constante; solo la primera frontera del bloque (o la de un salto lider)
  // mira `ilk` en tiempo de ejecucion. Un bail re-ejecuta la op en el interprete, que vuelve a
  // pasar por su frontera: el stub de bail DESHACE la(s) frontera(s) ya emitidas (IlkUndo).
  // Una salida de op interpretada no se deshace: la op ya corrio, como en el interprete.
  // Apagado (defecto) no se emite nada.
  const bool ilkOn = c.ilkMode != 0, ilkCharge = c.ilkMode >= 2;
  const s32 ilkOff   = (s32)offsetof(CPU, ilk),        dcbOff   = (s32)offsetof(CPU, dcbR);
  const s32 ilkSvOff = (s32)offsetof(CPU, ilkSave),    ilkHsOff = (s32)offsetof(CPU, ilkHitSave);
  const s32 ilkHtOff = (s32)offsetof(CPU, ilkHits),    ilkStOff = (s32)offsetof(CPU, ilkStall);
  const s32 stlOff   = (s32)offsetof(CPU, stallCycles), stlTOff = (s32)offsetof(CPU, stallTotal);
  struct IlkUndo { u8 shifts = 0; bool known = false; u64 prev = 0; u32 hits = 0; };
  std::vector<IlkUndo> coldMemUndo;              // paralelo a coldMems
  bool ilkKnown = false; u64 ilkPrev = 0;          // lo que deja la op anterior, si se sabe
  IlkUndo ilkUndo;                                 // como deshacer las fronteras ya emitidas
  std::vector<IlkUndo> bailUndo;

  // I-cache exacta (ver CPU::jitIcExact): en la primera op del bloque y en cada op que abre
  // linea se emite el mismo `valid && ptag == base` de icFetch. El fallo salta a un stub del
  // final que llama a jitIcRefill y vuelve, o hace bail a `idx` si el codigo cambio. La
  // comprobacion va DESPUES de la frontera de enclavamiento, asi que el bail la deshace como
  // cualquier otro; el relleno no se deshace, y el interprete que re-ejecuta la op acierta.
  const bool icExact = c.jitIcExact();
  auto fetchW = [&](u32 a) -> u32 { return icExact ? c.jitPeekWord(a) : c.jitFetchWord(a); };
  struct IcChk { usize at, j1, j2, cont; u32 base; u32 idx; RcSnap snap; IlkUndo undo; };
  std::vector<ColdCall> coldCalls;
  g_cold = &coldCalls;
  std::vector<ColdMem> coldMems;
  g_coldMem = &coldMems;
  struct ColdOff { ~ColdOff() { g_cold = nullptr; g_coldMem = nullptr; } } coldOff;
  std::vector<IcChk> icChks;
  auto icChk = [&](u32 a, u32 idx) {
    if(!icExact || (a != phys && (a & 0x1f))) return;
    u32 base = a & ~0x1fu;
    const s32 lo = (s32)(offsetof(CPU, icache) + ((base >> 5) & 0x1ff) * sizeof(CPU::ICacheLine));
    IcChk k; k.at = c.jitCache->buf.used; k.base = base; k.idx = idx;
    e.mov_r32_m(RAX, RBX, lo + (s32)offsetof(CPU::ICacheLine, ptag));
    e.alu32_imm(7, RAX, base);                     // cmp eax, base
    k.j1 = e.jne_rel32_placeholder();
    e.movzx8_r_m(RAX, RBX, lo + (s32)offsetof(CPU::ICacheLine, valid));
    e.test_al_al();
    k.j2 = e.je_rel32_placeholder();
    k.cont = c.jitCache->buf.used;
    k.snap = rc.snap(); k.undo = ilkUndo;
    icChks.push_back(k);
  };
  auto icTrim = [&](usize at) { while(!icChks.empty() && icChks.back().at >= at) icChks.pop_back(); };
  // chain = segunda frontera de la pareja salto+ranura: el bail de la ranura apunta al SALTO,
  // asi que hay que deshacer las dos.
  auto ilkPre = [&](u32 op, bool chain) {
    if(!ilkOn) return;
    u64 use; u64 prod = CPU::ilkMasks(op, use);
    e.movzx8_r_m(RAX, RBX, dcbOff);                // dcbR <<= 1
    e.alu32_rr(0x03, RAX, RAX);
    e.mov_m8_r(RBX, dcbOff, RAX);
    if(!chain) ilkUndo = IlkUndo{1, ilkKnown, ilkPrev, 0};
    else       ilkUndo.shifts++;
    if(ilkKnown) {
      if(ilkPrev & use) {
        ilkUndo.hits++;
        e.add_m64_imm32(RBX, ilkHtOff, 1);
        if(ilkCharge) { e.add_m32_imm32(RBX, stlOff, 1); e.add_m64_imm32(RBX, stlTOff, 1);
                        e.add_m64_imm32(RBX, ilkStOff, 1); }
      }
    } else {
      e.mov_r_m(RAX, RBX, ilkOff);
      e.mov_m_r(RBX, ilkSvOff, RAX);               // para deshacer en un bail
      e.mov_r_imm64(RDX, use);
      e.alu64_rr(0x23, RAX, RDX);                  // and rax, rdx
      e.setcc_x(0x95, RAX);                        // setne al
      e.movzx_r8(RAX, RAX);
      e.mov_m_r32(RBX, ilkHsOff, RAX);
      e.add_r_m(RAX, RBX, ilkHtOff); e.mov_m_r(RBX, ilkHtOff, RAX);
      if(ilkCharge) {
        e.mov_r32_m(RAX, RBX, ilkHsOff);
        e.mov_r32_m(RDX, RBX, stlOff); e.alu32_rr(0x03, RDX, RAX); e.mov_m_r32(RBX, stlOff, RDX);
        e.add_r_m(RAX, RBX, stlTOff); e.mov_m_r(RBX, stlTOff, RAX);
        e.mov_r32_m(RAX, RBX, ilkHsOff);
        e.add_r_m(RAX, RBX, ilkStOff); e.mov_m_r(RBX, ilkStOff, RAX);
      }
    }
    e.mov_r_imm64(RAX, prod);
    e.mov_m_r(RBX, ilkOff, RAX);
    ilkKnown = true; ilkPrev = prod;
  };

  // Offsets de los campos de control del CPU (para branch-in-block: el bloque
  // escribe pc/nextPc/inDelay/justBranched directamente y devuelve flag de control).
  const s32 pcOff      = (s32)((char*)&c.pc           - (char*)&c);
  const s32 nextOff    = (s32)((char*)&c.nextPc       - (char*)&c);
  const s32 inDelayOff = (s32)((char*)&c.inDelay      - (char*)&c);
  const s32 justBrOff  = (s32)((char*)&c.justBranched - (char*)&c);
  const s32 pendOff    = (s32)((char*)&c.jitPending   - (char*)&c);
  std::vector<usize> branchExits;  // jmp del terminador de branch → epílogo
  bool endedInBranch = false;

  // Block-linking Step 3: sitios de enlace pendientes de resolver. Se emiten con la guarda
  // desactivada; al final del bloque se les reserva la ranura de 8 bytes y se parchea el
  // disp32 rip-relativo. `entryVA` es la VA con la que se compiló: solo se puede predecir el
  // destino estático de un salto si la entrada está en ckseg0, donde VA→phys es la máscara
  // arquitectónica (segmento NO mapeado) y por tanto independiente del TLB.
  struct Pending { usize dispAt; usize immAt; u64 targetVA; u32 targetPhys; bool tlbTarget; };
  std::vector<Pending> pending;
  const u64 entryVA = c.pc;
  const bool ck0Entry = ((entryVA & 0xFFFF'FFFF'E000'0000ull) == 0xFFFF'FFFF'8000'0000ull)
                        && (((u32)entryVA & 0x1FFF'FFFFu) == phys);
  // ¿Es `va` un destino enlazable? (ckseg0 ⇒ phys arquitectónica y cacheable, dentro de RDRAM)
  // Mismas condiciones que `linkable`, pero sin un destino concreto que comprobar: el sondeo
  // indirecto compara la VA en tiempo de ejecucion contra la que guardo el driver.
  // Enlace desde codigo TLB-mapeado: el destino se traduce con el TLB de AHORA y la traduccion
  // queda congelada en el sitio de enlace. Es sano porque cualquier cosa que remapee (TLBWI/
  // TLBWR, cambio de ASID) sube cpu.tlbGen y el driver desenlaza todo antes del siguiente
  // despacho. Sin esto, un juego que ejecuta desde el TLB no enlazaba NADA: Perfect Dark
  // (codigo en 0x70000000) media 17,5 k salidas enlazadas frente a 2,05 G salidas lentas.
  const bool tlbLink = g_jitTlbLink && !ck0Entry;
  const bool useItc = g_jitLink && !g_jitDiffAny && (ck0Entry || tlbLink) && !g_noItc;
  auto linkable = [&](u64 va, u32& outPhys, bool& outTlb) -> bool {
    if(!g_jitLink || g_jitDiffAny) return false;
    if(va & 3) return false;
    if((va & 0xFFFF'FFFF'E000'0000ull) == 0xFFFF'FFFF'8000'0000ull) {
      // ckseg0: phys arquitectonica y cacheable, sin TLB de por medio; vale desde cualquier
      // bloque, porque la guarda compara la VA de destino y esa VA solo se traduce de un modo.
      u32 p = (u32)va & 0x1FFF'FFFFu;
      if((usize)p + 4 > c.mem->rdram.size()) return false;
      outPhys = p; outTlb = false; return true;
    }
    if(!tlbLink || g_noTlbStatic) return false;
    if(!g_jitTlbXPage) {
      // Mismo marco de 4 KB que la entrada: la phys se deriva de la traduccion con la que se
      // entro al bloque, sin sondear el TLB ni congelar un mapeo ajeno.
      if((va ^ entryVA) & ~0xFFFull) return false;
      u32 p = (phys & ~0xFFFu) | ((u32)va & 0xFFFu);
      if((usize)p + 4 > c.mem->rdram.size()) return false;
      outPhys = p; outTlb = true; return true;
    }
    // Sonda sin efectos secundarios (probing): un destino sin mapear o no cacheable no se
    // enlaza y cae a la salida lenta, que es donde el interprete levanta la excepcion.
    bool sc = c.xlatCacheable;
    c.xlatCacheable = false;
    u64 p = c.tlbProbePhys(va);
    bool cacheable = c.xlatCacheable;
    c.xlatCacheable = sc;
    if(p == ~0ull || !cacheable) return false;
    if((usize)(u32)p + 4 > c.mem->rdram.size()) return false;
    outPhys = (u32)p; outTlb = true; return true;
  };

  // Compila el delay slot (ALU o mem). Si es mem, registra su bail con índice = el del
  // salto (bailIdx=idx): al faultar, el intérprete re-ejecuta desde el salto. Devuelve false
  // si el delay slot no es compilable (→ se descarta la absorción del salto completo).
  auto compileDelay = [&](u32 dop, u32 idx) -> bool {
    usize dBefore0 = c.jitCache->buf.used;
    ilkPre(dop, true);
    icChk(phys + 4 * (idx + 1), idx);             // la linea de la ranura: bail al SALTO
    usize dBefore = c.jitCache->buf.used;         // los reintentos conservan la frontera
    if(emitSafeOp(e, rc, dop)) return true;
    c.jitCache->buf.used = dBefore;
    // ALU-con-trampa / MFC0 en la ranura: los dos emiten su bail ANTES de tocar el destino,
    // asi que el rollback es limpio y el bail puede apuntar al SALTO (idx): el interprete
    // re-ejecuta salto+ranura y aplica la semantica de excepcion en delay slot (EPC=salto,
    // Cause.BD=1). El enlace de JAL/JALR ya emitido se reescribe con el mismo valor, que es
    // idempotente. Sin esto un ADDI/DADDI/ADD/SUB en la ranura tiraba la absorcion del salto
    // entera y, si el salto era el lider, el bloque no compilaba: ~31k lideres BEQ/BNE
    // fallidos medidos en SM64 (ADDI 23k + DADDI 7.8k en la ranura).
    usize tsite; RcSnap tsnap;
    if(emitTrapAlu(e, rc, dop, tsite, tsnap)) {
      bailSites.push_back(tsite); bailIdx.push_back(idx); bailSnap.push_back(tsnap); bailUndo.push_back(ilkUndo);
      b.hasTrap = true;
      return true;
    }
    c.jitCache->buf.used = dBefore;
    usize asite; RcSnap asnap;
    if(emitAlu64(e, rc, dop, asite, asnap)) {
      bailSites.push_back(asite); bailIdx.push_back(idx); bailSnap.push_back(asnap); bailUndo.push_back(ilkUndo);
      return true;
    }
    c.jitCache->buf.used = dBefore;
    usize csite; RcSnap csnap;
    if(emitCop0(e, rc, dop, csite, csnap, idx + 1, c.cpi256)) {   // la ranura va una op detras del salto
      bailSites.push_back(csite); bailIdx.push_back(idx); bailSnap.push_back(csnap); bailUndo.push_back(ilkUndo);
      return true;
    }
    c.jitCache->buf.used = dBefore;
    usize dsite; bool dStore; RcSnap dsnap;
    if(emitMemOp(e, rc, dop, dsite, dStore, dsnap, idx + 1, pendOff)) {
      bailSites.push_back(dsite); bailIdx.push_back(idx); bailSnap.push_back(dsnap); bailUndo.push_back(ilkUndo);
      b.hasMem = true; if(dStore) b.hasStore = true;
      return true;
    }
    c.jitCache->buf.used = dBefore;
    // Interprete en la ranura de retardo. A diferencia del bail de una mem-op, cuando esta
    // devuelve 0 la op YA tuvo efecto, asi que no se puede re-ejecutar el salto: se sale con
    // la bandera de control y pc/nextPc los deja el interprete (que con inDelay puesto pone
    // EPC en el salto y Cause.BD=1, como el VR4300). Retira idx+2 ops: rectas + salto +
    // ranura. Sin esto un salto con LWC1/ADD.S/SWL en la ranura hacia rollback del salto
    // entero, y si el salto era el lider el bloque no compilaba: BEQ/BEQL/JAL/BNE sumaban
    // 1.85M de lideres fallidos medidos en SM64.
    usize isite; RcSnap isnap;
    if(emitInterpOp(e, rc, dop, 4 * (idx + 1), isite, isnap, true)) {
      interpSites.push_back(isite); interpIdx.push_back(idx + 2); interpSnap.push_back(isnap);
      b.hasMem = true; b.hasStore = true;
      return true;
    }
    c.jitCache->buf.used = dBefore0;
    return false;
  };
  // JAL/JALR: link = sext32((u32)nextPc) = sext32(entryVA + 4*(idx+2)) → gpr[reg]. Se emite
  // ANTES del delay slot (en HW el enlace ocurre en la ejecución del salto, y el delay slot
  // ve ya el nuevo $ra). Idempotente si el delay slot faulta y se re-ejecuta el salto.
  auto emitLink = [&](u32 idx, u32 reg) {
    e.mov_r_m(RAX, RBX, pcOff);              // rax = entryVA
    e.add_r_imm32(RAX, (s32)(4 * (idx + 2)));
    e.movsxd(RAX, RAX);                      // sext32(low32)
    e.st64(RAX, (u8)reg);                    // gpr[reg] = link
  };
  // Cola común de salida de control: target ya en RCX. Escribe pc=target, nextPc=target+4,
  // limpia inDelay/justBranched y sale con el flag de control | ops retiradas (idx+2).
  // `cands` = destinos ESTÁTICOS posibles de este salto (0 para JR/JALR, 1 para J/JAL, 2 para
  // los branches condicionales: tomado y caída). Por cada uno se emite una guarda de enlace.
  // `nops` = ops que ESTA salida retira. Casi siempre idx+2 (rectas + salto + delay slot),
  // pero un branch "likely" NO tomado anula su delay slot y retira una menos — el intérprete
  // lo resuelve en un solo paso (pc += 8), así que la cuenta tiene que seguirle.
  auto emitCtrlExit = [&](u32 idx, const u64* cands, int nc, u32 nops) {
    e.mov_m_r(RBX, pcOff, RCX);              // cpu->pc = target
    e.mov_r_r(RDX, RCX); e.add_r_imm8(RDX, 4);
    e.mov_m_r(RBX, nextOff, RDX);            // cpu->nextPc = target+4
    e.mov_m8_imm(RBX, inDelayOff, 0);
    e.mov_m8_imm(RBX, justBrOff, 0);
    // Guardas de enlace. RCX = target de runtime, ya con pc/nextPc escritos (el sucesor entra
    // con el estado de control exactamente como si el driver lo hubiera despachado). La guarda
    // compara contra el VA de compilación: si la VA de entrada del bloque hubiese cambiado
    // (alias por TLB del mismo phys), no casa y se cae a la salida lenta.
    usize prevJne = 0; bool havePrev = false;
    for(int k = 0; k < nc; k++) {
      u32 tp; bool ttlb = false;
      if(!linkable(cands[k], tp, ttlb)) continue;
      if(havePrev) { e.patchRel32(prevJne); havePrev = false; }
      e.mov_r_imm64_force(RDX, kNoLink);                  // guarda (desactivada al nacer)
      usize immAt = c.jitCache->buf.used - 8;
      e.cmp_r_r(RCX, RDX);
      prevJne = e.jne_rel32_placeholder(); havePrev = true;
      // Enlazado: acumula las ops de ESTE bloque en jitPending (el prólogo del sucesor las
      // commitea) y salta a su punto de entrada sin pasar por el driver.
      // RDX ya esta muerto aqui (era la guarda comparada) y add_m32_imm32 no emite REX,
      // asi que la base tiene que ser un registro bajo.
      if(g_xStats) { e.mov_r_imm64(RDX, (u64)(std::uintptr_t)&g_xLink);
                     e.add_m32_imm32(RDX, 0, 1); }
      e.add_m32_imm32(RBX, pendOff, nops);
      usize dispAt = e.jmp_rip_mem_placeholder();
      pending.push_back(Pending{ dispAt, immAt, cands[k], tp, ttlb });
    }
    // Cache de destinos INDIRECTOS. Un JR/JALR no tiene destino estatico (`nc == 0`), asi que
    // hasta ahora TODO retorno de funcion salia al driver por la ruta lenta: con avgK~13 eso
    // es el viaje mas caro que queda en el camino del JIT. El sondeo es una tabla directa
    // VA -> punto de entrada que rellena el driver; si acierta se salta al MISMO `linkEntry`
    // del enlace estatico, que re-chequea interrupciones, borde de timer y presupuesto de
    // cadena, o sea no se salta ninguna comprobacion, solo el viaje.
    if(nc == 0 && useItc) {
      // Un destino NO alineado no es un destino: el VR4300 levanta AdEL en el propio fetch,
      // y eso lo vectoriza el interprete. Aqui ademas era una caida del anfitrion: `kNoLink`
      // vale 1 -- se eligio impar justamente por "imposible" -- y un JR a la VA 1, que
      // junkrunner64 hace a proposito, casaba con la etiqueta de TODA entrada vacia de la
      // ITC; el salto se iba entonces al `code` de una entrada sin rellenar, que es 0.
      // Mandando fuera los desalineados el centinela vuelve a ser inalcanzable por
      // construccion. Solo hace falta en la ruta INDIRECTA: el destino de un J o de un
      // branch sale de la codificacion de la instruccion y siempre esta alineado.
      constexpr s32 kEntSize = (s32)sizeof(CodeCache::ItcEnt);
      e.mov_r_imm64(RDX, (u64)(std::uintptr_t)c.jitCache->itc.data());
      e.mov_r_r(RAX, RCX);
      e.test_al_imm8(3);                               // 2 bits bajos del destino
      const usize mis = e.jne_rel32_placeholder();
      e.shift64_imm(5, RAX, 12);                       // shr rax, 12
      e.xor_r_r(RAX, RCX);                             // rax = va ^ (va>>12)
      e.shift64_imm(5, RAX, 2);                        // shr rax, 2  (ops alineadas)
      e.alu64_imm(4, RAX, c.jitCache->itcMask);        // and rax, mascara
      e.shift64_imm(4, RAX, 4);                        // shl rax, 4  (x sizeof(ItcEnt))
      e.add_r_r(RDX, RAX);                             // rdx = &itc[idx]
      e.mov_r_m(RAX, RDX, 0);                          // rax = va guardada
      e.cmp_r_r(RCX, RAX);                             // �es este el destino?
      usize miss = e.jne_rel32_placeholder();
      // Aqui RDX es el puntero a la entrada (lo usa el jmp de abajo): el scratch es RAX,
      // muerto tras la comparacion.
      if(g_xStats) { e.mov_r_imm64(RAX, (u64)(std::uintptr_t)&g_xItc);
                     e.add_m32_imm32(RAX, 0, 1); }
      e.add_m32_imm32(RBX, pendOff, nops);             // ops de ESTE bloque, al diferido
      e.jmp_m(RDX, 8);                                 // salta al linkEntry guardado
      e.patchRel32(miss);
      e.patchRel32(mis);                               // desalineado: por la salida lenta
      static_assert(kEntSize == 16, "el shl de arriba asume entradas de 16 bytes");
    }
    if(havePrev) e.patchRel32(prevJne);
    if(g_xStats) { e.mov_r_imm64(RDX, (u64)(std::uintptr_t)(nc == 0 ? &g_xSlowInd : &g_xSlowDir));
                   e.add_m32_imm32(RDX, 0, 1); }
    e.mov_r_imm32(RAX, 0x80000000u | nops);
    branchExits.push_back(e.jmp_rel32_placeholder());
  };

  g_fk = FpKnow{};
  g_ksu0 = false;
  g_fkMayFr = ((u32)c.cop0[CPU::C0_Status] >> 26) & 1;
  g_fkMayRc = !(c.fcr31 & 0x80);
  for(u32 i = 0; i < kMaxOps; i++) {
    u32 a = phys + 4 * i;
    if(a + 4 > c.mem->rdram.size()) break;
    // No cruzar la página 4K de entrada: en código TLB-mapeado los VA contiguos NO son
    // phys contiguos al cambiar de página, así que phys+4*i dejaría de corresponder a la
    // instrucción real. El bloque termina en el borde de página (se recompila en la sig.).
    // Cruzar la pagina 4K de entrada solo vale desde ckseg0: ahi VA->phys es un
    // desplazamiento fijo, asi que phys+4*i sigue siendo la instruccion real al pasar de
    // pagina. En codigo TLB-mapeado no lo es y el bloque termina en el borde (se recompila
    // en la siguiente). Cortar SIEMPRE salia caro: un salto en la ultima palabra de la
    // pagina dejaba su ranura de retardo fuera, la absorcion se caia y con el salto de
    // lider el bloque entero no compilaba -- 465k entradas al interprete medidas en SM64.
    if((a & ~0xFFFu) != (phys & ~0xFFFu)) { if(!ck0Entry || g_noXPage) break; b.crossPage = true; }
    u32 op = fetchW(a);
    usize before0 = c.jitCache->buf.used;
    if(g_jitDump) b.opOff.push_back((u32)(before0 - (usize)(entry - c.jitCache->buf.base)));
    const bool ilkKnown0 = ilkKnown; const u64 ilkPrev0 = ilkPrev;
    ilkPre(op, false);
    icChk(a, b.nOps);
    usize before = c.jitCache->buf.used;          // los reintentos conservan la frontera
    if(emitSafeOp(e, rc, op)) { b.src.push_back(op); b.nOps++; continue; }
    c.jitCache->buf.used = before;
    usize tsite; RcSnap tsnap;
    if(emitTrapAlu(e, rc, op, tsite, tsnap)) {
      bailSites.push_back(tsite); bailIdx.push_back(b.nOps); bailSnap.push_back(tsnap); bailUndo.push_back(ilkUndo);
      b.hasTrap = true;
      b.src.push_back(op); b.nOps++;
      continue;
    }
    c.jitCache->buf.used = before;
    usize asite; RcSnap asnap;
    if(emitAlu64(e, rc, op, asite, asnap)) {
      bailSites.push_back(asite); bailIdx.push_back(b.nOps); bailSnap.push_back(asnap); bailUndo.push_back(ilkUndo);
      b.src.push_back(op); b.nOps++; continue;
    }
    c.jitCache->buf.used = before;
    usize csite; RcSnap csnap;
    if(emitCop0(e, rc, op, csite, csnap, i, c.cpi256)) {
      bailSites.push_back(csite); bailIdx.push_back(b.nOps); bailSnap.push_back(csnap); bailUndo.push_back(ilkUndo);
      b.src.push_back(op); b.nOps++; continue;
    }
    c.jitCache->buf.used = before;
    usize site; bool isStore; RcSnap msnap, mmsnap; usize msite = 0;
    if(emitMemOp(e, rc, op, site, isStore, msnap, b.nOps, pendOff, &msite, true, &mmsnap)) {
      if(site) { bailSites.push_back(site); bailIdx.push_back(b.nOps); bailSnap.push_back(msnap); bailUndo.push_back(ilkUndo); }
      else { coldMems.back().idx = b.nOps; coldMemUndo.push_back(ilkUndo); }   // seccion fria
      // Retira ESTA op y sale: el acceso ya tuvo efecto (b.nOps aun no se ha incrementado).
      if(msite) { mmioSites.push_back(msite); mmioIdx.push_back(b.nOps + 1); mmioSnap.push_back(mmsnap); }
      b.hasMem = true; if(isStore) b.hasStore = true;
      b.src.push_back(op); b.nOps++; continue;
    }
    c.jitCache->buf.used = before;
    usize isite; RcSnap isnap; usize rsite; RcSnap rsnap;
    if(emitInterpOp(e, rc, op, 4 * i, isite, isnap, false, false, &rsite, &rsnap)) {
      if(isite) { interpSites.push_back(isite); interpIdx.push_back(b.nOps + 1); interpSnap.push_back(isnap); }
      else coldCalls.back().idx = b.nOps + 1;  // camino lento en la seccion fria
      // Revalidacion FPU fallida en el camino lento: la op ya se hizo, se sale detras.
      if(rsite) { mmioSites.push_back(rsite); mmioIdx.push_back(b.nOps + 1); mmioSnap.push_back(rsnap); }
      // Conservador: la op puede tocar memoria y estado FPU → fuera del modo jitdiff puro.
      b.hasMem = true; b.hasStore = true;
      b.src.push_back(op); b.nOps++; continue;
    }
    c.jitCache->buf.used = before;
    // MTC0 TERMINAL. Escribir un registro COP0 no se puede absorber a media faena (Status o
    // Cause pueden dejar una interrupcion lista y el bloque no las mira hasta salir), pero SI
    // se puede ejecutar como ULTIMA op: el bloque se cierra detras y el conductor vuelve a
    // muestrear interrupciones antes del siguiente. Lo que se gana no es la op en si sino que
    // el MTC0 deje de ser LIDER de bloque: hasta ahora el bloque cortaba ANTES de el, la
    // siguiente entrada aterrizaba justo encima y no compilaba nunca -- 205k entradas al
    // interprete solo con MTC0 Status en 300 fotogramas de SM64.
    //   Fuera quedan los registros que el propio bloque adelanta de golpe al salir, porque ese
    //   sumando machacaria el valor recien escrito: Count (Count += ops retiradas) y Random
    //   (decrementa por op), y con Random su Wired, que lo reinicia. Compare tambien: la guarda
    //   de frontera del timer se calculo con el Compare de la ENTRADA al bloque. Sin excluir
    //   Wired, n64-systemtest cazaba el desfase exacto: "Random, 1 instruction after setting
    //   Wired = 0" esperaba 0x1f y salia 0x1e.
    //   Con la misma regla entran las ops de FUNCION de COP0: ERET (pone pc=EPC y limpia EXL,
    //   asi que el thunk devuelve 0 y el bloque sale por la via de control con el pc ya bueno),
    //   TLBR, TLBWI y TLBP. TLBWR no: indexa por Random, que dentro del bloque va atrasado.
    if((op >> 26) == 0x10 && (((op >> 21) & 31) == 4 || ((op >> 21) & 31) == 0x10)) {
      u32 c0rd = (op >> 11) & 31;
      bool c0fn = ((op >> 21) & 31) == 0x10;
      if((c0fn ? (op & 63) != 0x06
               : (c0rd != CPU::C0_Count && c0rd != CPU::C0_Compare
               && c0rd != CPU::C0_Random && c0rd != CPU::C0_Wired))) {
        usize msite; RcSnap msnap;
        if(emitInterpOp(e, rc, op, 4 * i, msite, msnap, false, true)) {
          interpSites.push_back(msite); interpIdx.push_back(b.nOps + 1); interpSnap.push_back(msnap);
          b.hasMem = true; b.hasStore = true;
          b.src.push_back(op); b.nOps++;
          break;                                  // cierra el bloque: el conductor re-muestrea
        }
        c.jitCache->buf.used = before;
      }
    }
    // Salto/branch-en-bloque: absorber el salto + su delay slot y cerrar el bloque
    // escribiendo el control de flujo (pc/nextPc) directamente. Es la palanca de cobertura:
    // el bucle caliente de PD está dominado por BEQ + llamadas (JAL) + returns (JR $ra), y
    // cortar en cada uno dejaba bloques cortos. cpu == &gpr[0] == RBX (ver static_assert),
    // así el estado de control se direcciona vía RBX (R12 no fiable tras el CALL de mem-op).
    // Ninguna forma recta compilo: fuera la frontera. Si es un salto absorbible la vuelve a
    // emitir su maquinaria (dentro del rango de rollback); si no, el bloque acaba aqui.
    c.jitCache->buf.used = before0;
    icTrim(before0);
    ilkKnown = ilkKnown0; ilkPrev = ilkPrev0;
    u32 LO = op >> 26;
    u32 FN = op & 63;
    u32 rtF = (op >> 16) & 31;
    bool isBeq = (LO == 0x04 || LO == 0x05);                 // BEQ / BNE
    bool isJmp = (LO == 0x02 || LO == 0x03);                 // J / JAL (target estático)
    bool isJr  = (LO == 0x00 && (FN == 0x08 || FN == 0x09)); // JR / JALR (target = gpr[rs])
    // Branches de 1 registro vs 0 (signed 64-bit): comparten fases B/C con BEQ (mismo target
    // VA+4(idx+1)+SIMM*4 / fallthrough VA+4(idx+2)); solo cambia la condición (fase A).
    // Se EXCLUYEN las variantes "likely" (BLEZL/BGTZL 0x16/0x17, REGIMM rt bit1) porque anulan
    // el delay slot cuando NO se toma — semántica distinta a esta maquinaria (que siempre lo
    // ejecuta). Las de enlace SÍ se absorben: BLTZAL/BGEZAL (rt 0x10/0x11) enlazan $31
    // INCONDICIONALMENTE (como JAL) y comparten la fase de condición/target con BLTZ/BGEZ.
    bool isBlez = (LO == 0x06 && rtF == 0);                  // BLEZ  rs<=0
    bool isBgtz = (LO == 0x07 && rtF == 0);                  // BGTZ  rs>0
    bool isRegimmBr = (LO == 0x01 && (rtF == 0x00 || rtF == 0x01 || rtF == 0x10 || rtF == 0x11));
    bool isRegimmAL = (LO == 0x01 && (rtF == 0x10 || rtF == 0x11)); // BLTZAL / BGEZAL (enlazan $31)
    bool isBcondZ = isBlez || isBgtz || isRegimmBr;
    // BC1F/BC1T/BC1FL/BC1TL (COP1 con rs=0x08). Comparten la aritmetica de destino con BEQ
    // (pc + SIMM*4) y la regla de las likely; lo unico distinto es de donde sale la condicion:
    // el bit COND (23) de FCR31 comparado con TF = rt bit0. Eran 2.1M de lideres no
    // compilables en SM64 -- el compile-fail entero de COP1.
    bool isBc1 = (LO == 0x11 && ((op >> 21) & 31) == 0x08);
    bool bc1Tf = (rtF & 1) != 0;                             // TF: con que valor de COND se toma
    bool isBc1L = isBc1 && (rtF & 2) != 0;                   // ND: variante likely
    // Variantes "likely": misma condición y mismo target que las normales, pero ANULAN el
    // delay slot cuando no se toman. Son mayoría en el código que generan los compiladores
    // de SGI (BNEL solo era el 65% de los líderes no compilables medidos en SM64), así que
    // dejarlas fuera cortaba el bloque en cada una y devolvía el control al intérprete.
    bool isBeqL   = (LO == 0x14 || LO == 0x15);              // BEQL / BNEL
    bool isBlezL  = (LO == 0x16 && rtF == 0);                // BLEZL
    bool isBgtzL  = (LO == 0x17 && rtF == 0);                // BGTZL
    bool isRegimmL  = (LO == 0x01 && (rtF == 0x02 || rtF == 0x03 || rtF == 0x12 || rtF == 0x13));
    bool isRegimmALL = (LO == 0x01 && (rtF == 0x12 || rtF == 0x13));  // BLTZALL/BGEZALL: enlazan $31
    bool isLikely = isBeqL || isBlezL || isBgtzL || isRegimmL || isBc1L;
    // Biseccion por clase de salto (KESTREL_JIT_BRSEL, mascara de bits; por defecto todas):
    //   1 BEQ/BNE   2 BLEZ/BGTZ/REGIMM   4 likely   8 J/JAL   16 JR/JALR
    // Apagar una clase la deja fuera de la absorcion: el bloque termina ahi y manda el
    // interprete, que es el oraculo. Sirve para aislar cual de las cinco falla.
    {
      static const u32 kBrSel = std::getenv("KESTREL_JIT_BRSEL")
                              ? (u32)std::strtoul(std::getenv("KESTREL_JIT_BRSEL"), nullptr, 0)
                              : 0xFFFF'FFFFu;
      if(!(kBrSel & 1))  isBeq = false;
      if(!(kBrSel & 2))  isBcondZ = false;
      if(!(kBrSel & 4))  isLikely = false;
      if(!(kBrSel & 8))  isJmp = false;
      if(!(kBrSel & 16)) isJr = false;
      if(!(kBrSel & 32)) { isBc1 = false; if(isBc1L) isLikely = false; }
    }
    // BLTZ(0x00)/BLTZAL(0x10) → rs<0 (setl); BGEZ(0x01)/BGEZAL(0x11) → rs>=0 (setge). bit0 decide.
    // Las likely de REGIMM (0x02/0x03/0x12/0x13) siguen la misma regla de bit0.
    u8 ccz = (isBlez || isBlezL) ? 0x9E /*setle*/ : (isBgtz || isBgtzL) ? 0x9F /*setg*/
             : ((rtF & 1) == 0 ? 0x9C /*setl*/ : 0x9D /*setge*/);
    bool traced = false;   // la caída del branch sigue compilándose en este mismo bloque
    static const int noBranch = std::getenv("KESTREL_JIT_NOBRANCH") ? 1 : 0;
    static const int noJmp = std::getenv("KESTREL_JIT_NOJMP") ? 1 : 0;  // A/B: desactiva SOLO J/JAL/JR/JALR
    if(noJmp && (isJmp || isJr)) break;
    if(!noBranch && (isBeq || isJmp || isJr || isBcondZ || isLikely || isBc1)) {
      u32 ad = a + 4;                              // delay slot
      bool delayOk = (ad + 4 <= c.mem->rdram.size()) &&
                     (((ad & ~0xFFFu) == (phys & ~0xFFFu)) || (ck0Entry && !g_noXPage));
      if(delayOk && (ad & ~0xFFFu) != (phys & ~0xFFFu)) b.crossPage = true;
      if(delayOk) {
        u32 dop = fetchW(ad);
        u32 rs = (op >> 21) & 31, rt = (op >> 16) & 31, rd = (op >> 11) & 31;
        s32 simm = (s32)(s16)(op & 0xFFFF);
        u32 idx = b.nOps;                          // rectas antes del salto (= i)
        // A partir de aqui manda la maquinaria de branch: dos caminos de salida, delay slot
        // que puede emitirse dos veces y rollback del bloque entero si el delay no compila.
        // Volcar y apagar la residencia deja todos esos caminos con cpu->gpr coherente, y el
        // volcado queda FUERA del rango de rollback (antes de beforeBranch) a proposito.
        rc.disable();
        usize beforeBranch = c.jitCache->buf.used; // por si el delay no compila
        ilkPre(op, false);                         // frontera del salto (la de la ranura, en compileDelay)
        icChk(a, idx);
        // Un rollback deja el codigo descartado pero los sitios de bail ya registrados
        // seguirian apuntando dentro de esa zona, que otra op reescribira despues. Se recortan
        // con el buffer. Hoy solo BC1 registra uno en fase A; vale para el que venga.
        usize nBailBefore = bailSites.size();
        auto rollbackBranch = [&]() {
          c.jitCache->buf.used = beforeBranch;
          icTrim(beforeBranch);
          bailSites.resize(nBailBefore); bailIdx.resize(nBailBefore); bailSnap.resize(nBailBefore);
          bailUndo.resize(nBailBefore);
        };
        // Fase A (antes del delay slot): capturar la condición/target/enlace que el delay
        // slot podría pisar (el delay puede escribir gpr[rs]/gpr[rt] o hacer CALL).
        // Condicion CONSTANTE en tiempo de compilacion. `beq $0,$0,L` es como todo ensamblador
        // MIPS escribe el salto incondicional `b L`, y `beq rX,rX` sale igual de las macros;
        // compararlo en tiempo de ejecucion es trabajo puro de mas y ademas deja DOS destinos
        // que enlazar donde solo hay uno. condK: -1 desconocida, 1 siempre tomada, 0 nunca.
        int condK = -1;
        if(isBeq && rs == rt)      condK = (LO == 0x04) ? 1 : 0;   // BEQ rX,rX / BNE rX,rX
        else if(isBcondZ && rs == 0) {
          if(isBlez)      condK = 1;                                // 0 <= 0
          else if(isBgtz) condK = 0;                                // 0 > 0 falso
          else            condK = (int)(rtF & 1);                   // BLTZ falso, BGEZ cierto
        }
        // La condicion vive en R15B, no en la pila. Es no-volatil en Win64 (sobrevive a los CALL
        // de los helpers) y esta libre: rc.disable() acaba de volcar la residencia, unica duena
        // de RSI/RDI/R13-R15. Antes era sete -> [rsp+32] -> recarga, un reenvio almacen-carga
        // por cada bloque con salto.
        if(isBeq || isBeqL) {
          if(condK < 0) { e.ld64(RAX, rs); e.cmp64_rm(RAX, rt);
                          e.setcc_x((LO == 0x04 || LO == 0x14) ? 0x94 : 0x95, R15); }
          else e.mov_r_imm64(R15, (u64)(u32)condK);   // constante: solo la releen las likely
        } else if(isBcondZ || isBlezL || isBgtzL || isRegimmL) {
          if(condK < 0) { e.ld64(RAX, rs); e.cmp64_imm(RAX, 0);     // rs vs 0 (signed 64b)
                          e.setcc_x(ccz, R15); }
          else e.mov_r_imm64(R15, (u64)(u32)condK);
          // BLTZAL/BGEZAL (y sus likely): enlace INCONDICIONAL de $31 tras leer rs (el
          // intérprete lee la condición ANTES de escribir $31; si rs==31 usa el pre-enlace).
          if(isRegimmAL || isRegimmALL) emitLink(idx, 31);
        } else if(isBc1) {
          // CU1 claro = Coprocessor Unusable (ExcCode 11, CE=1). Se bailea con las rectas ya
          // retiradas y pc intacto: el interprete re-ejecuta el BC1 y levanta la excepcion
          // exacta. La residencia ya esta volcada y apagada, asi que el snap va vacio.
          const s32 stOff = (s32)(offsetof(CPU, cop0) + 8u * (u32)CPU::C0_Status);
          e.test_m8_imm(RBX, stOff + 3, 0x20);              // Status bit29 vive en el byte 3
          bailSites.push_back(e.je_rel32_placeholder());
          bailIdx.push_back(idx); bailSnap.push_back(rc.snap()); bailUndo.push_back(ilkUndo);
          // COND es el bit 23 de FCR31 -> byte 2, bit 7. ZF=1 significa COND=0, asi que
          // "tomar" es setne cuando TF=1 y sete cuando TF=0.
          e.test_m8_imm(RBX, (s32)offsetof(CPU, fcr31) + 2, 0x80);
          e.setcc_x(bc1Tf ? 0x95 : 0x94, R15);
        } else if(isJr) {
          e.ld64(R15, rs);                         // target = gpr[rs] (pre-delay) -> r15
          if(FN == 0x09) emitLink(idx, rd ? rd : 31);   // JALR enlaza tras leer rs (rd puede==rs)
        } else if(LO == 0x03) {
          emitLink(idx, 31);                       // JAL enlaza gpr[31]
        }
        // Fase B/C de las "likely": el delay slot se emite DENTRO del camino tomado, porque
        // cuando no se toma queda anulado. Las dos salidas son enlazables y cuentan distinto:
        // tomada retira idx+2 ops (rectas + salto + delay), no tomada idx+1 (pc += 8 de una).
        if(isLikely) {
          e.test_r8_self(R15);
          usize toNot = e.je_rel32_placeholder();
          if(compileDelay(dop, idx)) {
            s32 Ctaken = (s32)(4 * (idx + 1)) + simm * 4;
            s32 Cfall  = (s32)(4 * (idx + 2));
            e.mov_r_m(RCX, RBX, pcOff); e.add_r_imm32(RCX, Ctaken);
            u64 ctaken = entryVA + (u64)(s64)Ctaken;
            emitCtrlExit(idx, &ctaken, 1, idx + 2);
            e.patchRel32(toNot);
            e.mov_r_m(RCX, RBX, pcOff); e.add_r_imm32(RCX, Cfall);
            u64 cfall = entryVA + (u64)(s64)Cfall;
            emitCtrlExit(idx, &cfall, 1, idx + 1);
            b.src.push_back(op); b.src.push_back(dop);
            b.nOps += 2;
            b.hasBranch = true;
            endedInBranch = true;
          } else {
            rollbackBranch();                      // el delay no compila: descartar el salto
          }
        }
        // Fase B: delay slot.
        else if(compileDelay(dop, idx)) {
          // Fase C: computar target → RCX y salir por la cola de control común.
          u64 cands[2]; int nc = 0;   // destinos estáticos, para las guardas de block-linking
          if((isBeq || isBcondZ || isBc1) && g_jitTrace && simm > 0 && i + 2 < kMaxOps) {
            // Traza: salida de control SÓLO por el camino tomado; la caída continúa inline.
            // El delay slot ya está emitido arriba (fase B) y se ejecuta en ambos caminos,
            // que es exactamente la semántica de un branch NO-likely.
            s32 Ctaken = (s32)(4 * (idx + 1)) + simm * 4;
            e.test_r8_self(R15);
            usize toFall = e.je_rel32_placeholder();   // cond==0 → no tomado → seguir compilando
            e.mov_r_m(RCX, RBX, pcOff);
            e.add_r_imm32(RCX, Ctaken);                // rcx = target tomado
            u64 ctaken = entryVA + (u64)(s64)Ctaken;
            emitCtrlExit(idx, &ctaken, 1, idx + 2);
            e.patchRel32(toFall);
            b.src.push_back(op); b.src.push_back(dop);
            b.nOps += 2;
            b.hasBranch = true;
            i++;              // el for avanza otro → salto + delay slot consumidos
            traced = true;
          } else if(isBeq || isBcondZ || isBc1) {
            s32 Ctaken = (s32)(4 * (idx + 1)) + simm * 4;  // VA + 4(idx+1) + SIMM*4
            s32 Cfall  = (s32)(4 * (idx + 2));             // VA + 4(idx+2)
            if(condK >= 0) {
              // Condicion decidida en compilacion: un solo destino, sin cmov y -- lo que mas
              // vale -- un solo candidato de enlace, asi que la guarda del bloque siguiente
              // acierta siempre en vez de tener que elegir entre dos.
              s32 C = condK ? Ctaken : Cfall;
              e.mov_r_m(RCX, RBX, pcOff);
              e.add_r_imm32(RCX, C);
              cands[nc++] = entryVA + (u64)(s64)C;
            } else {
              e.mov_r_m(RDX, RBX, pcOff);     // rdx = entryVA
              e.mov_r_r(RCX, RDX);
              e.add_r_imm32(RDX, Ctaken);     // rdx = target tomado
              e.add_r_imm32(RCX, Cfall);      // rcx = fallthrough
              e.test_r8_self(R15);
              e.cmovnz(RCX, RDX);             // cond!=0 -> rcx = target
              cands[nc++] = entryVA + (u64)(s64)Ctaken;   // tomado (el caliente: bucles)
              cands[nc++] = entryVA + (u64)(s64)Cfall;    // caida
            }
          } else if(isJr) {
            e.mov_r_r(RCX, R15);             // rcx = target de gpr[rs] (pre-delay)
            // JR/JALR: destino dinámico (gpr[rs]) → sin destino estático que enlazar.
          } else {  // J / JAL: target = (entryVA & 0xFFFFFFFF_F0000000) | (TARGET26<<2)
            u32 tgt = (op & 0x03FF'FFFFu) << 2;
            e.mov_r_m(RCX, RBX, pcOff);                 // rcx = entryVA
            e.mov_r_imm64(RDX, 0xFFFF'FFFF'F000'0000ull);
            e.and_r_r(RCX, RDX);                        // rcx = entryVA & mask
            e.mov_r_imm32(RDX, tgt);
            e.or_r_r(RCX, RDX);                         // rcx = target
            cands[nc++] = (entryVA & 0xFFFF'FFFF'F000'0000ull) | (u64)tgt;
          }
          if(!traced) {
            emitCtrlExit(idx, cands, nc, idx + 2);
            b.src.push_back(op); b.src.push_back(dop);
            b.nOps += 2;
            b.hasBranch = true;
            endedInBranch = true;
          }
        } else {
          rollbackBranch();                      // descartar el salto entero
        }
      }
    }
    if(traced) continue;
    if(g_compFailOn) {
      u32 LOe = op >> 26;
      g_endOp[LOe]++;
      if(LOe == 0) g_endSpecial[op & 63]++;
      else if(LOe == 1) g_endRegimm[(op >> 16) & 31]++;
    }
    break;   // op no soportada (mult/div/cop/etc) o salto absorbido → fin
  }

  if(b.nOps == 0) {
    if(g_compFailOn) {
      u32 lop = c.jitFetchWord(phys); u32 LO = lop >> 26;
      g_compFailOp[LO]++;
      if(LO == 0) g_compFailSpecial[lop & 63]++;
      else if(LO == 1) g_compFailRegimm[(lop >> 16) & 31]++;
      else if(LO == 0x10 || LO == 0x11) g_compFailCop[LO - 0x10][(lop >> 21) & 31]++;
      if(LO == 0x10) { u32 rsF = (lop >> 21) & 31; if(rsF == 0 || rsF == 4) g_compFailC0Rd[rsF ? 1 : 0][(lop >> 11) & 31]++; }
      // Diagnostico puntual (KESTREL_BRFAIL): primeros lideres de salto que no absorben.
      { static const bool brf = std::getenv("KESTREL_BRFAIL") != nullptr; static int nbr = 0;
        if(brf && nbr < 200 && (LO == 0x04 || LO == 0x05)) { nbr++;
          u32 dw = c.jitFetchWord(phys + 4);
          std::fprintf(stderr, "[brfail] phys=%08x op=%08x delay=%08x samepage=%d\n",
                       phys, lop, dw, (int)(((phys + 4) & ~0xFFFu) == (phys & ~0xFFFu)));
        } }
      // Lider de salto: el motivo casi seguro es la ranura de retardo, asi que se censa esa.
      if(LO == 0x01 || LO == 0x04 || LO == 0x05 || LO == 0x06 || LO == 0x07
      || (LO >= 0x14 && LO <= 0x17) || (LO == 0x11 && ((lop >> 21) & 31) == 8)) {
        u32 dw = c.jitFetchWord(phys + 4); u32 DO = dw >> 26;
        g_compFailDelay[DO]++;
        if(DO == 0) g_compFailDelaySpec[dw & 63]++;
      }
    }
    c.jitCache->buf.used = (usize)(entry - c.jitCache->buf.base); return b;
  }

  // Salida normal: eax = nOps (todas retiradas); salta al epílogo común.
  // Si el bloque terminó absorbiendo un branch, éste ya emitió su propia salida de
  // control (con pc/nextPc escritos y flag 0x80000000) → no hay caída secuencial.
  usize toDoneMain = 0; bool haveMain = false;
  if(!endedInBranch) {
    // ENLACE SECUENCIAL: PROBADO Y DESCARTADO (2026-09-02). Un bloque que se acaba sin salto
    // -- por el tope de kMaxOps o porque la siguiente op no compila -- devuelve el control al
    // driver para que avance pc y vuelva a entrar. Es el 84 % de los despachos del driver en
    // SM64 (contadores [retorno] salto/completo/corto = 86 k / 2,25 M / 346 k), asi que parecia
    // LA razon de que una cadena solo encadenase una docena de bloques. Se implemento: emitir
    // aqui la misma cola de salida de control que un salto absorbido, con el destino constante
    // entryVA + 4*nOps como unico candidato de enlace.
    //
    // Funciono como se esperaba -- las entradas al driver en SM64 bajaron de 2,88 M a 328 k
    // (8,8x) y las ops por entrada subieron de 580 a 4902 -- y aun asi MIDIO PEOR:
    //   n64-systemtest (unica carga que es de verdad CPU-bound aqui): 13,27 s -> 14,84 s
    //   solo con nOps==kMaxOps (codigo recto largo, sin los finales por op no compilable):
    //                                                                 13,58 s -> 13,80 s
    //   SM64 800 intercambios: 8,536 s -> 8,531 s (empate: ahi el palo largo es el RSP)
    // O sea: el viaje de vuelta al driver es BARATO (revalidar la linea de I-cache es un par
    // de comparaciones) y sale mas caro pagar en CADA final de bloque los cuatro stores de
    // control (pc, nextPc, inDelay, justBranched) mas la guarda de enlace que ahorrarselo.
    // Se deja escrito para que nadie lo vuelva a intentar a ciegas: el cuello del hilo de CPU
    // no es el despacho (jitTryBlock ~5,8 % de las muestras), es el codigo generado.
    rc.writeback();                          // el driver lee cpu->gpr tras el bloque
    e.mov_r_imm32(RAX, b.nOps);
    toDoneMain = e.jmp_rel32_placeholder();
    haveMain = true;
  }

  // Seccion fria: caminos lentos de las ops COP1/MTC0 en linea (ver ColdCall).
  g_cold = nullptr;
  for(const ColdCall& k : coldCalls) {
    for(u32 i = 0; i < k.bails.n; i++) e.patchRel32(k.bails.at[i]);
    if(k.xs) { e.mov_r_imm64(RAX, (u64)&g_helperCnt[k.xk]); e.add_m64_imm32(RAX, 0, 1); }
    e.mov_r_r(RCX, RBX);
    e.mov_r_imm32(RDX, k.op);
    e.mov_r_imm32(R8, k.off);
    e.mov_r_imm64(RAX, (u64)k.fn);
    e.call_reg(RAX);
    e.test_al_al();
    interpSites.push_back(e.je_rel32_placeholder()); interpIdx.push_back(k.idx); interpSnap.push_back(k.snap);
    // Revalidacion FpKnow: si el hecho estrenado no se cumple, salir tras la op retirada.
    if(k.newFr) {
      e.test_m8_imm(RBX, (s32)offsetof(CPU, cop0) + 12 * 8 + 3, 0x04);
      mmioSites.push_back(e.jcc_rel32_placeholder(0x84)); mmioIdx.push_back(k.idx); mmioSnap.push_back(k.snap);
    }
    if(k.newRc) {
      e.test_m8_imm(RBX, (s32)offsetof(CPU, fcr31), 0x80);
      mmioSites.push_back(e.jcc_rel32_placeholder(0x85)); mmioIdx.push_back(k.idx); mmioSnap.push_back(k.snap);
    }
    if(k.rcg) emitRcGuest(e);
    usize back = e.jmp_rel32_placeholder();
    e.patchRel32To(back, k.cont);
  }
  g_coldMem = nullptr;
  for(usize j = 0; j < coldMems.size(); j++) {
    const ColdMem& k = coldMems[j];
    for(int i = 0; i < k.nFail; i++) e.patchRel32(k.fails[i]);
    if(k.opsBefore) e.add_m32_imm32(RBX, k.pendOff, k.opsBefore);
    e.mov_r_r(RCX, RBX);
    e.mov_r_imm32(R8, k.rt);
    e.mov_r_imm64(RAX, (u64)k.fn);
    e.call_reg(RAX);
    if(k.opsBefore) e.add_m32_imm32(RBX, k.pendOff, (u32)-(s32)k.opsBefore);
    e.test_al_al();
    bailSites.push_back(e.je_rel32_placeholder());
    bailIdx.push_back(k.idx); bailSnap.push_back(k.snap); bailUndo.push_back(coldMemUndo[j]);
    e.cmp_al_imm8(1);
    mmioSites.push_back(e.jne_rel32_placeholder()); mmioIdx.push_back(k.idx + 1); mmioSnap.push_back(k.mmioSnap);
    if(k.newKsu) {                             // revalidar el KSU que estrena (ver g_ksu0)
      e.test_m8_imm(RBX, (s32)(offsetof(CPU, cop0) + 8u * (u32)CPU::C0_Status), 0x18);
      mmioSites.push_back(e.jne_rel32_placeholder()); mmioIdx.push_back(k.idx + 1); mmioSnap.push_back(k.mmioSnap);
    }
    usize back = e.jmp_rel32_placeholder();
    e.patchRel32To(back, k.cont);
  }
  // Stubs de I-cache exacta: rellenar y volver, o bail registrado como uno mas.
  for(const IcChk& k : icChks) {
    e.patchRel32(k.j1); e.patchRel32(k.j2);
    e.mov_r_r(RCX, RBX);
    e.mov_r_imm32(RDX, phys);
    e.mov_r_imm32(R8, k.base);
    e.mov_r_imm64(RAX, (u64)&kestrel_jitIcRefill);
    e.call_reg(RAX);
    e.test_al_al();
    bailSites.push_back(e.je_rel32_placeholder());
    bailIdx.push_back(k.idx); bailSnap.push_back(k.snap); bailUndo.push_back(k.undo);
    usize back = e.jmp_rel32_placeholder();
    e.patchRel32To(back, k.cont);
  }
  // Stubs de bail: cada je de mem-op aterriza aquí → eax = índice (ops retiradas) y al epílogo.
  std::vector<usize> toDone;
  for(usize k = 0; k < bailSites.size(); k++) {
    e.patchRel32(bailSites[k]);
    rc.emitSnapSpill(bailSnap[k]);           // spill perezoso: lo sucio en el punto del bail
    if(ilkOn) {
      const IlkUndo& u = bailUndo[k];
      e.movzx8_r_m(RAX, RBX, dcbOff);        // dcbR >>= fronteras deshechas
      e.shift32_imm(5, RAX, u.shifts);
      e.mov_m8_r(RBX, dcbOff, RAX);
      if(u.hits) {
        e.add_m64_imm32(RBX, ilkHtOff, (u32)-(s32)u.hits);
        if(ilkCharge) { e.add_m32_imm32(RBX, stlOff, (u32)-(s32)u.hits);
                        e.add_m64_imm32(RBX, stlTOff, (u32)-(s32)u.hits);
                        e.add_m64_imm32(RBX, ilkStOff, (u32)-(s32)u.hits); }
      }
      if(u.known) { e.mov_r_imm64(RAX, u.prev); e.mov_m_r(RBX, ilkOff, RAX); }
      else {
        e.mov_r_m(RAX, RBX, ilkSvOff); e.mov_m_r(RBX, ilkOff, RAX);
        e.mov_r32_m(RAX, RBX, ilkHsOff);
        e.mov_r_m(RDX, RBX, ilkHtOff); e.alu64_rr(0x2B, RDX, RAX); e.mov_m_r(RBX, ilkHtOff, RDX);
        if(ilkCharge) {
          e.mov_r32_m(RDX, RBX, stlOff); e.alu32_rr(0x2B, RDX, RAX); e.mov_m_r32(RBX, stlOff, RDX);
          e.mov_r_m(RDX, RBX, stlTOff);  e.alu64_rr(0x2B, RDX, RAX); e.mov_m_r(RBX, stlTOff, RDX);
          e.mov_r_m(RDX, RBX, ilkStOff); e.alu64_rr(0x2B, RDX, RAX); e.mov_m_r(RBX, ilkStOff, RDX);
        }
      }
    }
    e.mov_r_imm32(RAX, bailIdx[k]);
    toDone.push_back(e.jmp_rel32_placeholder());
  }
  // Stubs de salida por MMIO que ARMA. A diferencia del bail, el acceso YA se hizo: se sale
  // contando la op como retirada y SIN bandera de control, o sea el conductor avanza pc
  // secuencialmente (pc += 4*R) y acto seguido remata SI/PI/VI, rcpRetire y el muestreo de
  // interrupcion -- en la instruccion exacta, como el interprete. Nada que deshacer del
  // interlock: la op se completo.
  for(usize k = 0; k < mmioSites.size(); k++) {
    e.patchRel32(mmioSites[k]);
    rc.emitSnapSpill(mmioSnap[k]);
    e.mov_r_imm32(RAX, mmioIdx[k]);
    toDone.push_back(e.jmp_rel32_placeholder());
  }
  // Stubs de salida de op interpretada: a diferencia del bail, la op YA tuvo efecto y el
  // intérprete dejó pc/nextPc en el punto de reanudación correcto (excepción, o simplemente
  // un salto que no debería ocurrir en este conjunto). Se sale con la bandera de control
  // para que el driver NO recalcule pc, contando la op como retirada.
  for(usize k = 0; k < interpSites.size(); k++) {
    e.patchRel32(interpSites[k]);
    rc.emitSnapSpill(interpSnap[k]);         // spill perezoso: lo sucio en el punto de salida
    e.mov_r_imm32(RAX, 0x80000000u | interpIdx[k]);
    toDone.push_back(e.jmp_rel32_placeholder());
  }
  // Stub de bail del prólogo re-validable (block-linking Step 1): la je(al==0) aterriza aquí,
  // fija eax=0 (0 ops retiradas) y CAE al epílogo (pc intacto → driver re-despacha ruta lenta).
  // Se emite ANTES de patchear las salidas normales, que apuntan al `add rsp` posterior → no lo
  // atraviesan. Aquí también se parchea el placeholder de K del prólogo con nOps ya conocido.
  if(g_jitLink) {
    for(usize k = 0; k < linkBailJmps.size(); k++) e.patchRel32(linkBailJmps[k]);
    e.mov_r_imm32(RAX, 0);
    e.pokeU32(kImmAt, b.nOps);
    if(kSubAt) e.pokeU32(kSubAt, b.nOps);
  }
  // Epílogo (done): restaura RSP + callee-saved y retorna eax.
  if(haveMain) e.patchRel32(toDoneMain);
  for(usize k = 0; k < branchExits.size(); k++) e.patchRel32(branchExits[k]);
  for(usize k = 0; k < toDone.size(); k++) e.patchRel32(toDone[k]);
  e.add_rsp_imm8(48);
  e.pop_reg(R15); e.pop_reg(R14); e.pop_reg(R13); e.pop_reg(RDI); e.pop_reg(RSI);
  e.pop_reg(R12); e.pop_reg(RBX);
  e.ret();

  // Ranuras de enlace: 8 bytes por sitio, DESPUÉS del `ret` (son datos, nunca se ejecutan) y
  // dentro del mismo buffer, así que el disp32 rip-relativo del `jmp qword [rip+...]` siempre
  // alcanza. Se parchea aquí, cuando ya se conoce el offset de cada ranura.
  for(const Pending& p : pending) {
    usize slotOff = e.reserveSlot();
    e.patchRel32To(p.dispAt, slotOff);
    LinkSite s;
    s.vaImm      = (u64*)(c.jitCache->buf.base + p.immAt);
    s.slot       = (u64*)(c.jitCache->buf.base + slotOff);
    s.targetVA   = p.targetVA;
    s.targetPhys = p.targetPhys;
    s.tlbTarget  = p.tlbTarget;
    b.sites.push_back(s);
  }

  if(c.jitCache->buf.overflowed()) { b.nOps = 0; b.src.clear(); b.sites.clear(); return b; }
  c.jitCache->buf.finalize(entry);
  b.fn = reinterpret_cast<BlockFn>(entry);
  b.codeLen = (u32)(c.jitCache->buf.cursor() - entry);
  b.phys = phys;
  return b;
}

}  // namespace kestrel::jit
