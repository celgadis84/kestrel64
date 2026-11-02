// CPU-side JIT dispatch: jitTryBlock, chain re-entry, idle-loop skip and JIT statistics.
// Split out of jit.cpp; shared internals live in jit_internal.hpp.
#include "jit.hpp"
#include "cpu.hpp"
#include "../core/memory.hpp"
#include "jit_internal.hpp"
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>

// ===================== Gancho del intérprete (miembro de CPU) ================
namespace kestrel {

// Estadística opcional (KESTREL_JIT_STATS): mide cobertura y longitud media de bloque
// para decidir si la Etapa 2b (memoria/branches en bloque) merece la pena.
static u64 g_jitCalls = 0, g_jitBlocks = 0, g_jitOps = 0;
// Cadena: eslabones enlazados y ops que se comieron por su cuenta. avgK solo mide el ULTIMO
// bloque de la cadena, asi que por si solo no dice si el enlace esta funcionando.
static u64 g_chainOps = 0, g_chainLinks = 0;
static const int g_jitStats = std::getenv("KESTREL_JIT_STATS") ? 1 : 0;
// Periodo del volcado, en despachos. Por defecto 4 M, que era el unico valor posible y
// dejo de imprimir NADA en cuanto el enlace de bloques + la ITC bajaron los despachos:
// una tanda entera de SM64 no llega a 4 M. `KESTREL_JIT_STATS=<n>` fija el periodo.
static const u64 g_jitStatsMask = [] {
  const char* e = std::getenv("KESTREL_JIT_STATS");
  u64 n = (e && e[0] >= '1' && e[0] <= '9') ? std::strtoull(e, nullptr, 0) : 0;
  if(n < 1024) return (u64)0x3FFFFF;
  u64 m = 1; while(m < n) m <<= 1;                 // a la potencia de dos mas cercana
  return m - 1;
}();
// KESTREL_JIT_DUMP=<fichero>: al terminar el proceso, vuelca los bloques mas ejecutados con
// sus opcodes MIPS y los bytes x86-64 que emitio el compilador. Sin esto, la mitad del tiempo
// de pared del hilo de CPU (medido: 48.6% en jit/anon) es una caja negra y cualquier idea de
// mejora del codegen es a ciegas. Se desensambla luego con:
//   llvm-objdump -D -b binary -m x86-64 --start-address=0 <bytes.bin>
namespace jit {
const char* g_jitDump = std::getenv("KESTREL_JIT_DUMP");
CodeCache*  g_dumpCache = nullptr;
const char* g_jitDumpSel = std::getenv("KESTREL_JIT_DUMPSEL");
static auto dumpBlocks() -> void {
  CodeCache* cc = g_dumpCache;
  if(!cc || !g_jitDump) return;
  std::FILE* f = std::fopen(g_jitDump, "w");
  if(!f) return;
  // Ordena por ops-de-guest ejecutadas (runs*nOps): es el peso real en tiempo, no el numero
  // de entradas -- un bloque de 40 ops que corre mil veces pesa mas que uno de 2 que corre
  // diez mil, y lo que se quiere leer es donde se van los ciclos.
  std::vector<u32> ord;
  for(u32 i = 0; i < cc->blocks.size(); i++) if(cc->blocks[i].runs) ord.push_back(i);
  std::sort(ord.begin(), ord.end(), [&](u32 a, u32 b) {
    return cc->blocks[a].runs * cc->blocks[a].nOps > cc->blocks[b].runs * cc->blocks[b].nOps;
  });
  u64 totOps = 0, totRuns = 0, totCode = 0;
  for(u32 i : ord) { totOps += cc->blocks[i].runs * cc->blocks[i].nOps; totRuns += cc->blocks[i].runs; totCode += cc->blocks[i].codeLen; }
  std::fprintf(f, "# bloques=%zu vivos=%zu runs=%llu guestOps=%llu codeBytes=%llu\n",
               cc->blocks.size(), ord.size(), (unsigned long long)totRuns,
               (unsigned long long)totOps, (unsigned long long)totCode);
  u32 n = 0;
  for(u32 i : ord) {
    const Block& b = cc->blocks[i];
    // Cabecera de TODOS los bloques vivos (hace falta para atribuir muestras de hostprof a
    // un bloque); los bytes emitidos solo de los 64 mas pesados, que es lo que se lee a mano.
    // `runs` solo cuenta entradas DESDE EL DRIVER: un bloque al que se llega por block-linking
    // no pasa por aqui, asi que los bloques mas calientes suelen tener runs bajo. Por eso la
    // seleccion de que bloques volcar con bytes se puede forzar por PC fisica con
    // KESTREL_JIT_DUMPSEL=<phys>[,<phys>...], que es lo que dice el perfil de hostprof.
    bool full = (n++ < 64);
    if(g_jitDumpSel) {
      char pat[16]; std::snprintf(pat, sizeof pat, "%08x", b.phys);
      full = std::strstr(g_jitDumpSel, pat) != nullptr;
    }
    double share = totOps ? 100.0 * (double)(b.runs * b.nOps) / (double)totOps : 0.0;
    std::fprintf(f, "\n=== phys=%08x host=%p nOps=%u runs=%llu guestOps=%llu (%.2f%%) codeLen=%u"
                    " bytes/op=%.1f ctrl=%d mem=%d store=%d\n",
                 b.phys, (void*)b.fn, b.nOps, (unsigned long long)b.runs,
                 (unsigned long long)(b.runs * b.nOps), share, b.codeLen,
                 b.nOps ? (double)b.codeLen / b.nOps : 0.0,
                 (int)b.hasBranch, (int)b.hasMem, (int)b.hasStore);
    std::fprintf(f, "  off:");
    for(u32 k = 0; k < b.opOff.size(); k++) std::fprintf(f, " %x", b.opOff[k]);
    std::fprintf(f, "\n");
    for(u32 k = 0; k < b.nOps && k < b.src.size(); k++)
      std::fprintf(f, "  mips[%2u] +%05x %08x  %s\n", k, k < b.opOff.size() ? b.opOff[k] : 0u,
                   b.src[k], CPU::disasm(b.src[k], b.phys + 4 * k).c_str());
    if(!full) continue;
    std::fprintf(f, "  x86:");
    const u8* p = reinterpret_cast<const u8*>(b.fn);
    for(u32 k = 0; k < b.codeLen; k++) std::fprintf(f, "%s%02x", (k % 32) ? "" : "\n   ", p[k]);
    std::fprintf(f, "\n");
  }
  std::fclose(f);
}
struct DumpAtExit { ~DumpAtExit() { dumpBlocks(); } };
static DumpAtExit g_dumpAtExit;
}  // namespace jit
extern u64 g_trampWhy[5];
extern u64 g_trampBail;
// Diagnostico: QUIEN acota el permiso de cadena. Los tres candidatos se arreglan de forma
// distinta (el borde de timer no se toca, la ventana del bucle de sistema es politica de
// campo, el regulador es calibracion), asi que saber cual manda es lo unico accionable.
static u64 g_dueWhy[6][2] = {};
static u64 g_compiles = 0, g_compClears = 0, g_compDead = 0, g_deadProceed = 0;   // bloques compilados / vaciados de buffer (solo con STATS)
static u64 g_guardWhy[4] = {};
static u64 g_guardOps[4] = {};   // ops concedidas, para ver el GRANO del permiso
static u64 g_guardHist[16] = {};  // histograma log2 del permiso concedido   // 0=borde de timer 1=ventana de campo 2=regulador 3=tope
namespace jit { extern u64 g_compFailDelay[64], g_compFailDelaySpec[64]; extern u64 g_helperCnt[8192]; }
namespace jit { extern u64 g_compFailOp[64], g_compFailSpecial[64], g_compFailRegimm[32];
                extern u64 g_endOp[64], g_endSpecial[64], g_endRegimm[32];
                extern u64 g_compFailCop[2][32]; extern u64 g_compFailC0Rd[2][32]; }
namespace jit { extern u64 g_rcReg, g_rcMem, g_rcSpill; }
namespace jit { extern u64 g_retBranch, g_retFull, g_retShort; }
// Diagnóstico: razón de decline (por qué jitTryBlock devuelve 0). Solo bajo stats.
enum { DR_RSP=0, DR_CTRL, DR_INT, DR_UNCACHED, DR_COMPILE, DR_TIMER, DR_SMC, DR_MISC, DR_N };
static u64 g_decl[DR_N] = {0};
#define JDECL(r) do{ if(g_jitStats) g_decl[r]++; }while(0)

// Re-chequeo de reentrada (block-linking Step 1). ESPEJA EXACTO el muestreo del driver: (1)
// refresca Cause IP2/IP7 desde interruptPending()/timerIntr, (2) si hay entrega habilitada
// pendiente → bail, (3) si correr K ops cruzaría Count==Compare → bail. Idempotente (no altera
// estado de interrupt). Devuelve 1=proceder, 0=bail. Idéntico a jitTryBlock líneas del sample.
// Presupuesto de cadena (Step 3): tope de bloques enlazados por entrada del driver. Sin él, un
// bucle auto-enlazado no devolvería el control hasta el borde del timer (miles de millones de
// ops): el bucle del sistema tiene que poder mirar maxinsn/vídeo/apagado. Las interrupciones NO
// dependen de esto — se re-muestrean en CADA eslabón, aquí abajo.
// KESTREL_JIT_CHAIN lo baja para diagnóstico: chain=1 deja toda la maquinaria de enlace en pie
// (guardas, ranuras, contabilidad diferida) pero sin ningún salto encadenado, así que aísla
// "el enlace rompe algo" de "la cadena corre demasiado sin volver al driver".
static const u32 kChainMax = []{
  if(const char* s = std::getenv("KESTREL_JIT_CHAIN")) { u32 v = (u32)std::strtoul(s, nullptr, 0); if(v) return v; }
  return 256u;
}();

// Avanza Random `p` pasos en O(1). El HW lo decrementa por ciclo y recarga 31 solo cuando
// coincide EXACTAMENTE con Wired, asi que la secuencia es un ciclo: bajando desde r hasta
// tocar wired (d = (r-wired) mod 64 pasos) y de ahi un ciclo de n = ((31-wired) mod 64)+1
// valores. Con Wired<=31 eso es el rango [wired..31] de siempre; con Wired>31 es el barrido
// completo que ya modelaba el bucle. Se hacia iterando p veces en CADA enlace de bloque —
// con avgK~3 eran ~30 instrucciones de host por bloque para un registro que el juego casi
// nunca lee. Misma funcion, sin bucle.
static inline auto randomAdvance(u32 rnd, u32 wired, u32 p) -> u32 {
  u32 wi = wired & 0x3f, r = rnd & 0x3f;
  u32 d  = (r - wi) & 0x3f;                       // pasos hasta tocar Wired
  if(p <= d) return (r - p) & 0x3f;               // aun no ha recargado
  u32 n  = ((31u - wi) & 0x3f) + 1;               // longitud del ciclo tras la recarga
  return (wi + (n - ((p - d) % n)) % n) & 0x3f;
}

// Tope del permiso del camino rápido. No es una condición de corrección (el borde de timer y
// la ventana del sistema ya acotan), sino una correa: garantiza que el trampolín — y con él el
// commit de jitPending y el presupuesto de cadena — se ejecute con regularidad aunque el guest
// esté en un bucle enlazado con Compare muy lejos.
static constexpr u32 kGuardMaxOps = 4096;

auto CPU::jitReenterProceed(u32 K) -> u32 {
  // (0) Commit diferido de la cadena: las ops de los bloques ya ejecutados y aún sin contabilizar.
  // Va PRIMERO para que el chequeo de borde de timer de más abajo vea el Count real de ESTE punto.
  if(u32 p = jitPending) {
    jitPending = 0;
    retired += p;
    // prof.* bajo JIT: los eslabones commiteados aqui van al bloque cabeza de la cadena
    // (profCurPhys, puesto al entrar en jitTryBlock); si no, el driver ya no los ve
    if(__builtin_expect(profOn, 0)) { profBuckets[profCurPhys >> kProfShift] += p; profTotal += p; }
    if(countAdd(countTicks(p))) timerIntr = true;   // cruce, no igualdad (ver CPU::countAdd)
    cop0[C0_Random] = randomAdvance((u32)cop0[C0_Random], (u32)cop0[C0_Wired], p);
    if(jitCache) jitCache->hits += p;
    // Estas ops tienen que llegar a quien llamó al driver: el bucle del sistema mide la
    // ventana de campo en ops retiradas. Si la cadena se las queda, el VI llega tarde.
    jitChainOps += p;
    jitOpsBudget = (p >= jitOpsBudget) ? 0 : (jitOpsBudget - p);
  }
  if(++jitChain > kChainMax) return 0;          // presupuesto agotado → devuelve el control
  // El eslabón enlazado sólo arranca si cabe entero en lo que queda de ventana del bucle del
  // sistema. Así la cadena no desborda el límite de campo más que un bloque suelto (el primer
  // bloque lo despacha el driver y conserva el comportamiento previo: jitChain==1).
  if(jitChain > 1 && K > jitOpsBudget) return 0;
  if(halted) return 0;
  // Estado que un store del bloque anterior pudo cambiar a mitad de cadena: en LOCKSTEP el hilo
  // CPU debe interleavear pasos del RSP, así que arrancarlo obliga a salir (espeja el driver).
  // rcpMode PRIMERO: en modo Threaded corta aqui y no llega a leer rsp.running, que vive en
  // memoria que el hilo RSP escribe. El orden inverso pagaba esa lectura en cada bloque.
  if(mem && mem->rcpMode == Memory::RcpMode::Lockstep && mem->rsp.running) return 0;
  u32 cause = (u32)cop0[C0_Cause];
  if(mem->interruptPending()) cause |= (1u << 10); else cause &= ~(1u << 10);
  if(timerIntr)               cause |= (1u << 15); else cause &= ~(1u << 15);
  cop0[C0_Cause] = (s64)(s32)cause;
  u32 status = (u32)cop0[C0_Status];
  if((status & 0x7) == 0x1 && (cause & status & 0xff00)) return 0;   // interrupt pendiente
  u32 cnt = (u32)cop0[C0_Count], cmp = (u32)cop0[C0_Compare];
  // La distancia a Compare se mide en TICKS y el bloque en OPS. Con el coste de fallos de
  // cache apagado la relacion es <=1 tick/op y comparar K basta; con el encendido un bloque
  // puede costar mucho mas de K ticks, asi que se compara contra la COTA SUPERIOR de lo que
  // puede costar (countTicksMax, que devuelve exactamente K cuando esta apagado).
  u64 kTicks = countTicksMax(K);
  if((u64)(u32)(cmp - cnt) <= kTicks) return 0;
  // Mismo trato que el borde de timer para el plazo del SI (transaccion de la PIF en
  // vuelo): el bloque no puede tragarselo, o la interrupcion del mando llegaria en un
  // punto distinto al del interprete y los modos dejarian de ser el mismo emulador.
  u64 siDue = ~0ull;
  if(mem) {
    // ANTES de mirar los plazos: si el otro hilo arma uno entre esta lectura y rcpDueIn,
    // el mapa visto se queda VIEJO y el prologo en linea corta la cadena en el siguiente
    // bloque. Al reves -- leerlo despues -- el plazo nuevo quedaria dentro del permiso sin
    // estar contado, que es justo lo que se quiere evitar.
    jitPendSeen = mem->rcpPend.load(std::memory_order_acquire) & jit::kPendWatch;
    u64 now = mem->cartNow();
    siDue = mem->eventDueIn(now);
    // El fin de tarea del RCP en Threaded es otro plazo del mismo reloj: si el bloque se lo
    // traga, MI_SP / MI_DP caen en una instruccion distinta a la del interprete. Se mete en
    // el mismo cupo, que ya esta en unidades de cartNow().
    u64 rcpDue = mem->rcpDueIn(now);
    if(g_jitStats && rcpDue < 4096) {
      // DIAG: que termino del plazo del RCP ata el permiso (0 spDone 1 dpDone 2 dpBar 3 spBar 4 dpLog)
      u32 pend = mem->rcpPend.load(std::memory_order_relaxed); int w = 5;
      auto chk = [&](int i, u64 at){ if((at > now ? at - now : 0) == rcpDue) w = i; };
      if(pend & 16u) { u64 at = mem->dpLogDueAt(); if(at != ~0ull) chk(4, at); }
      if(pend & 8u) chk(3, mem->spBarrierEff());
      if(pend & 4u) chk(2, mem->dpBarrierAt());
      if(pend & 2u) chk(1, mem->dpDoneAt);
      if(pend & 1u) chk(0, mem->spDoneAt);
      g_dueWhy[w][rcpDue < 64 ? 0 : 1]++;
    }
    if(rcpDue < siDue) siDue = rcpDue;
  }
  // El plazo esta en el reloj de invitado (cartNow = retiradas + pendientes + paradas), no en
  // ops, asi que se compara contra la MISMA cota superior que el borde de timer: con el coste
  // de cache encendido un bloque de K ops adelanta el reloj mas de K y se tragaria el plazo.
  // countTicksMax(K) == K con todos los costes apagados, luego esto queda igual byte a byte.
  const u64 kGuest = guestOpsMax(K);                           // == K con los costes apagados
  if(siDue <= kGuest) return 0;                                // cruzaria el plazo del SI
  // Permiso para el camino rápido del prólogo: cuántas ops MÁS puede encadenar la cadena sin
  // volver a preguntar. Lo acota lo mismo que acaba de comprobarse aquí — el borde de timer
  // (determinista: Count avanza 1 por op) y lo que queda de ventana del bucle del sistema —
  // menos las K de ESTE bloque, que aún no están commiteadas. Lo demás que mira el trampolín o
  // no puede cambiar dentro de una cadena (Status/Cause por mtc0, halted: terminan bloque) o lo
  // re-comprueba el propio prólogo en línea (MI, latch de timer).
  {
    // El margen hasta el borde de timer esta en TICKS; el permiso se descuenta en OPS, asi
    // que se convierte con la inversa conservadora (opsForTicks == identidad con el coste
    // de cache apagado, luego esta linea queda byte a byte como estaba).
    u32 slack = opsForTicks((u64)(u32)(cmp - cnt) - kTicks - 1ull);
    // El permiso de la cadena tambien lo acota el plazo del SI, por lo mismo.
    if(siDue != ~0ull) { u32 sl = opsForGuest(siDue - kGuest - 1); if(sl < slack) slack = sl; }                 // > 0 garantizado por la línea de arriba
    u32 budg  = (K >= jitOpsBudget) ? 0 : (jitOpsBudget - K);
    u32 g     = slack < budg ? slack : budg;
    // Regulador Threaded: duerme si la CPU emulada adelanta al RSP en vuelo y mete lo que
    // le queda de adelanto en el permiso. Asi el camino rapido del prologo lo descuenta solo
    // y vuelve aqui justo cuando toca frenar otra vez — misma regulacion que la guarda
    // rsp.running, pero sin una llamada por bloque.
    if(mem && mem->rcpMode == Memory::RcpMode::Threaded) {
      u32 pa = mem->rcpPace(guestOps());   // mismo reloj que el interleave de Lockstep
      if(pa < g) g = pa;
      if(g_jitStats && pa <= g) g_guardWhy[2]++;
    }
    if(g_jitStats) {
      if(g >= kGuardMaxOps)   { g_guardWhy[3]++; g_guardOps[3] += g; }
      else if(g == slack)     { g_guardWhy[0]++; g_guardOps[0] += g; }
      else if(g == budg)      { g_guardWhy[1]++; g_guardOps[1] += g; }
      else                    { g_guardOps[2] += g; }
    }
    if(g_jitStats) { u32 b = 0, v = g; while(v >>= 1) b++; if(b > 15) b = 15; g_guardHist[b]++; }
    jitGuard  = g < kGuardMaxOps ? g : kGuardMaxOps;
  }
  return 1;
}
// Trampolín extern "C" (dirección plana, ABI Win64 RCX/RDX) que llama el prólogo emitido.
// Diagnostico (KESTREL_JIT_STATS): POR QUE se llega al trampolin. El camino rapido en linea
// solo cae aqui por una de sus guardas, y cada una se arregla de forma distinta, asi que el
// numero de llamadas por si solo no dice nada accionable. Se reconstruyen los mismos
// predicados que evaluo el prologo (jitGuard aun conserva el valor no consumido).
u64 g_trampWhy[5] = {0};   // 0=permiso agotado 1=MI 2=latch timer 3=rsp corriendo 4=otro
// Cuantas veces el trampolin dice NO y por tanto ROMPE la cadena (vuelta al driver). Es la
// mitad de la respuesta a "por que solo se encadenan 12 bloques"; la otra mitad son las
// salidas que no resuelven destino (ni enlace estatico ni acierto en la ITC).
u64 g_trampBail = 0;
auto CPU::jitPeekWord(u32 phys) -> u32 {
  const ICacheLine& l = icache[(phys >> 5) & 0x1ff];
  if(!l.valid || l.ptag != (phys & ~0x1fu)) {
    if((usize)phys + 4 > mem->rdram.size()) return 0;
    mem->dmaSettle(phys, (u64)phys + 4);   // el compilador mira RDRAM: ver Memory::dmaSettle
    const u8* b = &mem->rdram[phys];
    return ((u32)b[0] << 24) | ((u32)b[1] << 16) | ((u32)b[2] << 8) | b[3];
  }
  u32 w; std::memcpy(&w, &l.data[phys & 0x1c], 4);
  return __builtin_bswap32(w);
}
auto CPU::jitIcRefill(u32 entry, u32 base) -> u8 {
  u32 idx = (base >> 5) & 0x1ff;
  icFill(idx, base);
  if(!jitCache) return 0;
  s32 bi = jitCache->find(entry);
  if(bi < 0) return 0;
  jit::Block& b = jitCache->blocks[bi];
  if(b.dead) return 0;
  const ICacheLine& l = icache[idx];
  u32 endPa = entry + 4 * b.nOps;
  u32 lo = base > entry ? base : entry, hi = base + 32 < endPa ? base + 32 : endPa;
  for(u32 pa = lo; pa < hi; pa += 4) {
    u32 off = pa & 0x1c;
    u32 w = ((u32)l.data[off] << 24) | ((u32)l.data[off + 1] << 16) | ((u32)l.data[off + 2] << 8) | l.data[off + 3];
    if(w != b.src[(pa - entry) >> 2]) { b.dead = true; if(g_jitStats) g_deadProceed++; jitCache->unlinkTo(entry); return 0; }
  }
  u32 li = ((base - (entry & ~0x1fu)) >> 5);
  if(li < sizeof(b.lineSeq) / sizeof(b.lineSeq[0])) b.lineSeq[li] = l.seq;
  return 1;
}
extern "C" KJIT_ABI u8 kestrel_jitIcRefill(void* cpu, u32 entry, u32 base) {
  return reinterpret_cast<CPU*>(cpu)->jitIcRefill(entry, base);
}

extern "C" KJIT_ABI u32 kestrel_jitProceedTramp(void* cpu, u32 K) {
  CPU* c = reinterpret_cast<CPU*>(cpu);
  if(g_jitStats) {
    if(c->jitGuard < K)                                          g_trampWhy[0]++;
    else if(c->mem && (c->mem->rcp.mi_intr & c->mem->rcp.mi_mask)) g_trampWhy[1]++;
    else if(c->timerIntr)                                        g_trampWhy[2]++;
    else if(c->mem && c->mem->rsp.brake)                         g_trampWhy[3]++;
    else                                                         g_trampWhy[4]++;
  }
  u32 r = c->jitReenterProceed(K);
  if(g_jitStats && r == 0) g_trampBail++;
  return r;
}


// --- Bucle ocioso de la CPU --------------------------------------------------------------
// Una rama SIEMPRE tomada a si misma con la ranura de retardo vacia (`b .`, que el ensamblador
// monta como `beq $0,$0,-1`, o `bgez $0,-1`) es un bucle del que el VR4300 no puede salir mas
// que por una excepcion: no escribe ningun registro, no toca memoria y lo unico que produce es
// Count. Es el hilo ocioso de libultra y en DK64 se lleva el 62,5% de las instrucciones de
// invitado de los primeros 300 campos (251,5 M de 402,3 M, medido con KESTREL_PCSAMPLE=
// 0x80000000: 0x80000a08 op 1000ffff mas su NOP).
//
// Emular esas iteraciones una a una no produce nada observable, asi que se cobran de golpe. La
// clave para que esto NO sea un atajo que cambie el emulador es el limite: es EXACTAMENTE el
// mismo que jitReenterProceed le concede a una cadena enlazada -- borde de Compare, plazo del
// SI, plazo del RCP, ventana de campo del bucle del sistema, regulador Threaded y el tope de
// kGuardMaxOps. O sea que el instante en que se vuelve a mirar cada evento es el que ya era:
// no se cambia CUANDO se mira, solo se deja de emular lo que hay entre dos miradas. El cobro
// (retired, Count, Random) es el mismo que hace el commit del trampolin.
//
// KESTREL_CPUIDLE=0 lo apaga. Es un atajo de anfitrion: con el apagado tiene que salir todo
// identico, trazas por campo y md5 de framebuffer incluidos.
static const bool g_idleCpuOn = [] {
  const char* e = std::getenv("KESTREL_CPUIDLE");
  return !e || !*e || std::strcmp(e, "0") != 0;
}();
static u64 g_idleSkips = 0, g_idleOps = 0;
// Por que NO se salta el bucle ocioso: 0 cadena, 1 icache, 2 no es b., 3 ranura,
// 4 IE/EXL, 5 sin plazos, 6 permiso 0, 7 saltado. Ver jitIdleSkip.
namespace jit { u64 g_idleWhy[8] = {}; }
// Quien fija el permiso del salto ocioso: 0 Compare, 1 evento/RCP, 2 presupuesto,
// 3 regulador, 4 tope kGuardMaxOps.
namespace jit { u64 g_idleLim[5] = {}, g_idleLimOps[5] = {}; }
// Reparto del tiempo OCIOSO del invitado segun quien estuviera trabajando en ese instante:
// 0 = nadie, 1 = solo RSP, 2 = solo RDP, 3 = los dos. Con esto se separa "el juego espera al
// RCP" de "el juego espera a un plazo" (VI, timer), que son problemas distintos.
static u64 g_idleWho[4] = {0,0,0,0};

static auto idlePaceOn() -> bool {
  static const bool v = [] { const char* e = std::getenv("KESTREL_IDLEPACE");
                             return !e || !*e || (bool)std::strcmp(e, "0"); }();
  return v;
}

auto CPU::jitIdleSkip(u32 phys) -> u32 {
  if(jitPending) { jit::g_idleWhy[0]++; return 0; }   // el driver siempre entra con la cadena vacia
  if((usize)phys + 8 > mem->rdram.size()) return 0;
  // I-cache exacta: el salto no pasa por el fetch, asi que solo vale con las dos lineas ya
  // cargadas (todas las vueltas aciertan); si no, la primera vuelta la da el camino normal.
  if(jitIcExact()) {
    for(u32 pa : {phys, phys + 4}) { const ICacheLine& l = icache[(pa >> 5) & 0x1ff];
      if(!l.valid || l.ptag != (pa & ~0x1fu)) { jit::g_idleWhy[1]++; return 0; } }
  }
  u32 w0 = jitFetchWord(phys), op = w0 >> 26;
  bool self = false;
  if(op == 0x04)                                 // BEQ rX,rX,-1
    self = ((w0 >> 21) & 31) == ((w0 >> 16) & 31) && (w0 & 0xffff) == 0xffff;
  else if(op == 0x01)                            // REGIMM BGEZ $0,-1
    self = ((w0 >> 21) & 31) == 0 && ((w0 >> 16) & 31) == 0x01 && (w0 & 0xffff) == 0xffff;
  if(!self) { jit::g_idleWhy[2]++; return 0; }
  if(jitFetchWord(phys + 4) != 0u) { jit::g_idleWhy[3]++; return 0; }   // ranura != NOP: el bucle hace algo
  // Con IE=0 o dentro de una excepcion ninguna interrupcion puede sacar de aqui: el invitado
  // esta colgado de verdad y saltarle el reloj lo esconderia en vez de arreglarlo. Que lo
  // ejecute el camino normal, que es donde estan los vigilantes.
  if(((u32)cop0[C0_Status] & 0x7) != 0x1) { jit::g_idleWhy[4]++; return 0; }
  // Threaded SIN plazos (KESTREL_RCPDEADLINE=0) es el unico modo en el que un worker publica
  // MI_SP/MI_DP por su cuenta en tiempo de pared: ahi no hay plazo que acote el salto y la
  // interrupcion se veria hasta 4096 ops tarde. Se queda fuera.
  if(mem->rcpMode == Memory::RcpMode::Threaded && !Memory::rcpDeadlineOn()) { jit::g_idleWhy[5]++; return 0; }

  // Limite = el permiso de una cadena, calculado igual que en jitReenterProceed.
  u32 cnt = (u32)cop0[C0_Count], cmp = (u32)cop0[C0_Compare];
  u64 lim = opsForTicks((u64)(u32)(cmp - cnt));
  u32 who = 0;
  u64 now = mem->cartNow();
  u64 due = mem->eventDueIn(now);
  { u64 r = mem->rcpDueIn(now); if(r < due) due = r; }
  if(due != ~0ull) { u64 d = opsForGuest(due); if(d < lim) { lim = d; who = 1; } }
  if((u64)jitOpsBudget < lim) { lim = jitOpsBudget; who = 2; }
  // PRUEBA (KESTREL_IDLEPACE=0): el regulador es la UNICA entrada host-dependiente de este
  // limite -- sale del reloj VIVO del RSP -- y con el puesto, cuantas vueltas del bucle
  // ocioso se saltan cambia entre corridas del mismo binario. Como el salto entrega la
  // interrupcion en el SALTO y la ejecucion normal puede entregarla en la ranura de
  // retardo, eso mueve Cause.BD.
  if(mem->rcpMode == Memory::RcpMode::Threaded && idlePaceOn()) {
    u32 pa = mem->rcpPace(guestOps());
    if((u64)pa < lim) { lim = pa; who = 3; }
  }
  if(lim > kGuardMaxOps) { lim = kGuardMaxOps; who = 4; }
  jit::g_idleLim[who]++; jit::g_idleLimOps[who] += lim;
  u32 k = (u32)lim & ~1u;                        // iteraciones enteras: el pc no se mueve
  if(!k) { jit::g_idleWhy[6]++; return 0; }
  jit::g_idleWhy[7] += k;

  retired += k;
  // prof.* bajo JIT: el bucle ocioso saltado cuenta como ejecutado en su PC (el interprete
  // lo recorre de verdad y ahi lo cuenta)
  if(__builtin_expect(profOn, 0)) { profBuckets[phys >> kProfShift] += k; profTotal += k; profCurPhys = phys; }
  if(countAdd(countTicks(k))) timerIntr = true;
  cop0[C0_Random] = randomAdvance((u32)cop0[C0_Random], (u32)cop0[C0_Wired], k);
  if(jitCache) jitCache->hits += k;
  idleSkipOps += k;
  if(g_jitStats) { g_idleSkips++; g_idleOps += k;
    u32 who = (mem->rspBusy.load(std::memory_order_relaxed) ? 1u : 0u)
            | (mem->rdpBusy.load(std::memory_order_relaxed) ? 2u : 0u);
    g_idleWho[who] += k; }
  return k;
}

// Perfil de PC de invitado bajo dynarec (KESTREL_GPCPROF=<topN>, 0=apagado). Cuenta
// visitas por PC de ENTRADA al driver del JIT, que es donde el guest pasa el control.
// Tabla abierta de 64 k ranuras, sondeo lineal; al cerrar saca el top N. Es diagnostico
// puro: no toca el codigo generado ni el estado del invitado.
namespace {
struct GpcProf {
  static constexpr u32 kSlots = 1u << 16;
  u64* key = nullptr; u64* cnt = nullptr; u32 top = 0; u64 total = 0;
  GpcProf() {
    const char* e = std::getenv("KESTREL_GPCPROF");
    if(!e) return;
    top = (u32)std::strtoul(e, nullptr, 0);
    if(!top) return;
    key = (u64*)std::calloc(kSlots, sizeof(u64));
    cnt = (u64*)std::calloc(kSlots, sizeof(u64));
    phy = (u32*)std::calloc(kSlots, sizeof(u32));
    wrd = (u32*)std::calloc((usize)kSlots * 12, sizeof(u32));
    if(!key || !cnt || !phy || !wrd) top = 0;
  }
  u32* phy = nullptr; u32* wrd = nullptr;   // wrd: 12 palabras copiadas del sitio
  inline auto slot(u64 pc) -> u32 {
    u64 h = pc * 0x9E3779B97F4A7C15ull; u32 i = (u32)(h >> 48) & (kSlots - 1);
    for(u32 n = 0; n < 64; n++, i = (i + 1) & (kSlots - 1)) {
      if(cnt[i] && key[i] != pc) continue;
      return i;
    }
    return ~0u;   // tabla saturada en esa cadena
  }
  u64 lastBkt = 0;
  // Muestreo PROPORCIONAL AL TIEMPO DE INVITADO: una muestra por cada 1024 instrucciones
  // retiradas, no por entrada al driver. Contar entradas sesga hacia los bloques cortos.
  inline auto hit(u64 pc, u64 ret) -> void {
    // Una muestra por CADA bucket cruzado, no una por entrada que cruce alguno: un bloque que
    // retira 20 k ops de una vez pesa 20 veces mas que uno que retira 1 k, que es justo lo que
    // significa "proporcional al tiempo de invitado". Contando una sola, el perfil se iba
    // detras de los sitios que se visitan mucho y retiran poco -- el bucle ocioso salia al
    // 37% siendo el 1,9% de las instrucciones.
    u64 b = ret >> 10; if(b == lastBkt) return;
    if(!lastBkt) { lastBkt = b; return; }   // primera: `retired` ya viene de un savestate
    u64 n = b - lastBkt; lastBkt = b;
    total += n;
    u32 i = slot(pc); if(i == ~0u) return;
    key[i] = pc; cnt[i] += n;
  }
  inline auto note(u64 pc, u32 ph, const u8* r, usize sz) -> void {
    u32 i = slot(pc); if(i == ~0u || phy[i]) return;
    phy[i] = ph;
    if((usize)ph + 48 > sz) return;
    for(u32 k = 0; k < 12; k++) {
      u32 a = ph + k * 4;
      wrd[(usize)i * 12 + k] = ((u32)r[a] << 24) | ((u32)r[a+1] << 16) | ((u32)r[a+2] << 8) | r[a+3];
    }
  }
  ~GpcProf() {
    if(!top || !cnt) return;
    std::vector<u32> idx;
    for(u32 i = 0; i < kSlots; i++) if(cnt[i]) idx.push_back(i);
    std::sort(idx.begin(), idx.end(), [&](u32 a, u32 b){ return cnt[a] > cnt[b]; });
    std::fprintf(stderr, "[gpcprof] muestras=%llu sitios=%zu\n",
                 (unsigned long long)total, idx.size());
    for(u32 r = 0; r < top && r < idx.size(); r++) {
      u32 i = idx[r];
      std::fprintf(stderr, "[gpcprof] %2u va=%016llx ph=%08x %10llu %6.2f%%\n", r,
                   (unsigned long long)key[i], phy[i], (unsigned long long)cnt[i],
                   100.0 * (double)cnt[i] / (double)total);
      if(r < 8 && phy[i])
        for(u32 k = 0; k < 12; k++) {
          u32 w = wrd[(usize)i * 12 + k];
          std::fprintf(stderr, "[gpcprof]      %016llx  %08x  %s\n",
                       (unsigned long long)(key[i] + k * 4), w,
                       CPU::disasm(w, key[i] + k * 4).c_str());
        }
    }
    std::fflush(stderr);
  }
};
GpcProf g_gpcProf;
}

auto CPU::jitTryBlock() -> u32 {
  if(g_gpcProf.top) g_gpcProf.hit(pc, retired);
  // DIAGNOSTICO (KESTREL_JIT_PCCHK): en modo de 32 bits toda direccion virtual valida es la
  // extension de signo de sus 32 bits bajos. Mirarlo en CADA despacho caza el primer momento
  // en que el pc se descarrila, venga de donde venga (salida de control, JR con registro
  // corrupto o el propio interprete), y no muchas excepciones despues.
  if(jit::g_pcChk && (u64)(s64)(s32)pc != pc) {
    std::fprintf(stderr, "[pcchk] despacho con pc=%016llx nextPc=%016llx inDelay=%d justBr=%d retired=%llu\n",
                 (unsigned long long)pc, (unsigned long long)nextPc, (int)inDelay, (int)justBranched,
                 (unsigned long long)retired);
    for(int r = 1; r < 32; r++) if((u64)(s64)(s32)gpr[r] != gpr[r])
      std::fprintf(stderr, "   $%d=%016llx\n", r, (unsigned long long)gpr[r]);
    std::fflush(stderr);
    halt("pcchk");
  }
  // Ver jit::regChkBad: sp/ra fuera de KSEG0/KSEG1 = el banco de registros ya esta podrido, y
  // esto ocurre MUCHISIMO antes de que el pc lo note. Se vuelca todo el contexto y se para.
  // Solo `sp`: `ra` no sirve de canario porque el compilador de Nintendo lo reutiliza como
  // temporal en funciones hoja (visto con $31=1 en un arranque sano).
  if(jit::g_regChk && jit::regChkBad(gpr[29])) {
    std::fprintf(stderr, "[regchk] pc=%016llx sp=%016llx ra=%016llx retired=%llu status=%08x\n",
                 (unsigned long long)pc, (unsigned long long)gpr[29], (unsigned long long)gpr[31],
                 (unsigned long long)retired, (u32)cop0[C0_Status]);
    for(int r = 1; r < 32; r++)
      std::fprintf(stderr, "   $%-2d=%016llx%s", r, (unsigned long long)gpr[r], (r % 4) ? "" : "\n");
    std::fprintf(stderr, "\n");
    std::fflush(stderr);
    halt("regchk");
  }
  if(g_jitStats) {
    g_jitCalls++;
    if((g_jitCalls & g_jitStatsMask) == 0) {
      std::fprintf(stderr, "[jitstats] calls=%llu blocksRun=%llu opsJIT=%llu cover=%.1f%% avgK=%.2f\n",
                   (unsigned long long)g_jitCalls, (unsigned long long)g_jitBlocks,
                   (unsigned long long)g_jitOps,
                   100.0 * g_jitOps / (double)g_jitCalls,
                   g_jitBlocks ? (double)g_jitOps / g_jitBlocks : 0.0);
      std::fprintf(stderr, "[jitdecl] rsp=%llu ctrl=%llu int=%llu uncached=%llu compile=%llu timer=%llu smc=%llu misc=%llu\n",
                   (unsigned long long)g_decl[DR_RSP], (unsigned long long)g_decl[DR_CTRL],
                   (unsigned long long)g_decl[DR_INT], (unsigned long long)g_decl[DR_UNCACHED],
                   (unsigned long long)g_decl[DR_COMPILE], (unsigned long long)g_decl[DR_TIMER],
                   (unsigned long long)g_decl[DR_SMC], (unsigned long long)g_decl[DR_MISC]);
      std::fprintf(stderr, "[plazo] spDone=%llu/%llu dpDone=%llu/%llu dpBar=%llu/%llu spBar=%llu/%llu dpLog=%llu/%llu otro=%llu/%llu\n", (unsigned long long)g_dueWhy[0][0], (unsigned long long)g_dueWhy[0][1], (unsigned long long)g_dueWhy[1][0], (unsigned long long)g_dueWhy[1][1], (unsigned long long)g_dueWhy[2][0], (unsigned long long)g_dueWhy[2][1], (unsigned long long)g_dueWhy[3][0], (unsigned long long)g_dueWhy[3][1], (unsigned long long)g_dueWhy[4][0], (unsigned long long)g_dueWhy[4][1], (unsigned long long)g_dueWhy[5][0], (unsigned long long)g_dueWhy[5][1]);
      std::fprintf(stderr, "[compila] bloques=%llu vaciados=%llu sobreMuerto=%llu muertosProceed=%llu\n", (unsigned long long)g_compiles, (unsigned long long)g_compClears, (unsigned long long)g_compDead, (unsigned long long)g_deadProceed);
      std::fprintf(stderr, "[regcache] reg=%llu mem=%llu spill=%llu hit=%.1f%%\n",
                   (unsigned long long)jit::g_rcReg, (unsigned long long)jit::g_rcMem,
                   (unsigned long long)jit::g_rcSpill,
                   100.0 * jit::g_rcReg / (double)(jit::g_rcReg + jit::g_rcMem + 1));
      std::fprintf(stderr, "[permiso] timer=%llu(%.0f) campo=%llu(%.0f) pace=%llu(%.0f) tope=%llu(%.0f)\n",
                   (unsigned long long)g_guardWhy[0], (double)g_guardOps[0]/(g_guardWhy[0]+1.0),
                   (unsigned long long)g_guardWhy[1], (double)g_guardOps[1]/(g_guardWhy[1]+1.0),
                   (unsigned long long)g_guardWhy[2], (double)g_guardOps[2]/(g_guardWhy[2]+1.0),
                   (unsigned long long)g_guardWhy[3], (double)g_guardOps[3]/(g_guardWhy[3]+1.0));
      { std::fprintf(stderr, "[permisohist]");
        for(int i = 0; i < 16; i++) std::fprintf(stderr, " %d:%llu", 1 << i, (unsigned long long)g_guardHist[i]);
        std::fprintf(stderr, "\n"); }
      std::fprintf(stderr, "[tramp] guard=%lluM mi=%lluM timer=%lluM rsp=%lluM otro=%lluM\n",
                   (unsigned long long)(g_trampWhy[0]/1000000), (unsigned long long)(g_trampWhy[1]/1000000),
                   (unsigned long long)(g_trampWhy[2]/1000000), (unsigned long long)(g_trampWhy[3]/1000000),
                   (unsigned long long)(g_trampWhy[4]/1000000));
      std::fprintf(stderr, "[retorno] salto=%llu completo=%llu corto=%llu\n",
                   (unsigned long long)jit::g_retBranch, (unsigned long long)jit::g_retFull,
                   (unsigned long long)jit::g_retShort);
      std::fprintf(stderr, "[salidas] enlace=%u itc=%u lentaDirecta=%u lentaIndirecta=%u\n",
                   jit::g_xLink, jit::g_xItc, jit::g_xSlowDir, jit::g_xSlowInd);
      { u64 it = g_idleWho[0]+g_idleWho[1]+g_idleWho[2]+g_idleWho[3]; if(!it) it = 1;
        std::fprintf(stderr, "[ociosoquien] nadie=%.1f%% soloRSP=%.1f%% soloRDP=%.1f%% ambos=%.1f%%\n",
                     100.0*g_idleWho[0]/it, 100.0*g_idleWho[1]/it, 100.0*g_idleWho[2]/it, 100.0*g_idleWho[3]/it); }
      std::fprintf(stderr, "[ocioso] saltos=%llu ops=%lluM (%.1f%% de las retiradas)\n",
                   (unsigned long long)g_idleSkips, (unsigned long long)(g_idleOps / 1000000),
                   retired ? 100.0 * (double)g_idleOps / (double)retired : 0.0);
      std::fprintf(stderr, "[cadena] rotasPorTramp=%llu (%.2f por entrada al driver)\n",
                   (unsigned long long)g_trampBail, (double)g_trampBail / (double)g_jitCalls);
      if(jitCache) std::fprintf(stderr,
                   "[icache] barridos=%llu efectivos=%llu vaciadosITC=%llu desenlaceTLB=%llu\n",
                   (unsigned long long)jitCache->nInvalAll,
                   (unsigned long long)jitCache->nInvalAllEff,
                   (unsigned long long)jitCache->nItcClear,
                   (unsigned long long)jitCache->nTlbUnlink);
      if(jitCache) std::fprintf(stderr,
                   "[link] sitios=%zu armados=%llu desarmados=%llu | eslabones/entrada=%.2f ops/entrada=%.2f\n",
                   jitCache->links.size(), (unsigned long long)jitCache->nLinked,
                   (unsigned long long)jitCache->nUnlinked,
                   (double)g_chainLinks / (double)g_jitCalls,
                   (double)(g_jitOps + g_chainOps) / (double)g_jitCalls);
      // Top opcodes que causan compile-fail (leader no compilable).
      std::fprintf(stderr, "[compfail]");
      for(int o = 0; o < 64; o++) if(jit::g_compFailOp[o] > 10000)
        std::fprintf(stderr, " OP%02x=%llu", o, (unsigned long long)jit::g_compFailOp[o]);
      std::fprintf(stderr, "\n[blockend]");
      for(int o = 0; o < 64; o++) if(jit::g_endOp[o] > 50)
        std::fprintf(stderr, " OP%02x=%llu", o, (unsigned long long)jit::g_endOp[o]);
      for(int o = 0; o < 64; o++) if(jit::g_endSpecial[o] > 50)
        std::fprintf(stderr, " SPEC%02x=%llu", o, (unsigned long long)jit::g_endSpecial[o]);
      for(int o = 0; o < 32; o++) if(jit::g_endRegimm[o] > 50)
        std::fprintf(stderr, " RI%02x=%llu", o, (unsigned long long)jit::g_endRegimm[o]);
      std::fprintf(stderr, " |");
      for(int f = 0; f < 64; f++) if(jit::g_compFailSpecial[f] > 1000)
        std::fprintf(stderr, " SP%02x=%llu", f, (unsigned long long)jit::g_compFailSpecial[f]);
      for(int r = 0; r < 32; r++) if(jit::g_compFailRegimm[r] > 10000)
        std::fprintf(stderr, " RI%02x=%llu", r, (unsigned long long)jit::g_compFailRegimm[r]);
      std::fprintf(stderr, "\n[compfaildelay]");
      for(int o = 0; o < 64; o++) if(jit::g_compFailDelay[o] > 5000)
        std::fprintf(stderr, " OP%02x=%llu", o, (unsigned long long)jit::g_compFailDelay[o]);
      for(int o = 0; o < 64; o++) if(jit::g_compFailDelaySpec[o] > 5000)
        std::fprintf(stderr, " SPEC%02x=%llu", o, (unsigned long long)jit::g_compFailDelaySpec[o]);
      { // top 16 de llamadas dinamicas al trampolin (k: OP*64+funct, COP0 fn=rs o 32+funct CO, COP1 4096+rs*64+fn)
        std::fprintf(stderr, "\n[helper]");
        static u64 cp[8192]; for(int i = 0; i < 8192; i++) cp[i] = jit::g_helperCnt[i];
        for(int t = 0; t < 16; t++) { int bi = 0; for(int i = 1; i < 8192; i++) if(cp[i] > cp[bi]) bi = i;
          if(!cp[bi]) break;
          if(bi >= 4096) std::fprintf(stderr, " C1.rs%02x.fn%02x=%llu", (bi - 4096) / 64, bi & 63, (unsigned long long)cp[bi]);
          else std::fprintf(stderr, " OP%02x.fn%02x=%llu", bi / 64, bi & 63, (unsigned long long)cp[bi]);
          cp[bi] = 0; } }
      std::fprintf(stderr, "\n[compfailcop]");
      for(int k = 0; k < 2; k++) for(int r = 0; r < 32; r++) if(jit::g_compFailCop[k][r] > 10000)
        std::fprintf(stderr, " COP%d.rs%02x=%llu", k, r, (unsigned long long)jit::g_compFailCop[k][r]);
      for(int k = 0; k < 2; k++) for(int r = 0; r < 32; r++) if(jit::g_compFailC0Rd[k][r] > 10000)
        std::fprintf(stderr, " %s.rd%02u=%llu", k ? "MTC0" : "MFC0", r, (unsigned long long)jit::g_compFailC0Rd[k][r]);
      std::fprintf(stderr, "\n");
    }
  }
  if(halted || !mem) return 0;
  // RSP corriendo: en LOCKSTEP el hilo CPU interleavea pasos RSP 2:3 (system.cpp), y un bloque
  // JIT de K ops los saltaría → divergencia. En THREADED el RSP va en su propio worker y el hilo
  // CPU NO lo pisa, así que JIT es tan válido como el intérprete (mismo thunk de memoria, misma
  // concurrencia ya existente). Solo declinamos en LOCKSTEP.
  if(mem->rcpMode == Memory::RcpMode::Lockstep && mem->rsp.running) { JDECL(DR_RSP); return 0; }
  if(randomReload) { JDECL(DR_MISC); return 0; }        // hazard COP0 Wired (raro)
  if((u32)cop0[C0_Status] & (1u << 25)) { JDECL(DR_MISC); return 0; } // RE → no JIT
  if(pc & 3) { JDECL(DR_MISC); return 0; }              // fetch address error → intérprete
  // Redirección de control pendiente: si estamos en un delay slot (nextPc apunta al
  // destino del branch, no a pc+4), el bloque NO puede correr secuencial — saltaría
  // el branch. Igual para inDelay/justBranched. Declina → el intérprete resuelve el salto.
  if(nextPc != pc + 4 || inDelay || justBranched) { JDECL(DR_CTRL); return 0; }

  // Muestreo de interrupt idéntico al intérprete (idempotente sobre el bloque, que no
  // cambia el estado de interrupt): refresca Cause IP2/IP7 y, si hay una entrega
  // habilitada pendiente, deja que el intérprete la vectore exactamente.
  {
    u32 cause = (u32)cop0[C0_Cause];
    if(mem->interruptPending()) cause |= (1u << 10); else cause &= ~(1u << 10);
    if(timerIntr)               cause |= (1u << 15); else cause &= ~(1u << 15);
    cop0[C0_Cause] = (s64)(s32)cause;
    u32 status = (u32)cop0[C0_Status];
    if((status & 0x7) == 0x1 && (cause & status & 0xff00)) { JDECL(DR_INT); return 0; }
  }

  // Traducción de la PC de entrada del bloque. Dos rutas, ambas exigen RDRAM cacheable:
  //  - ckseg0 directo (0xFFFFFFFF_80000000..9FFFFFFF): phys = pc & 0x1FFFFFFF, cacheable.
  //  - resto → TLB-mapeado (p.ej. PD corre desde 0x00000000_70xxxxxx): probe sin efectos.
  //    Pre-limpio xlatCacheable: si translate toma un segmento directo-uncached (kseg1/
  //    ckseg1) no lo pone → declinamos; los segmentos mapeados/xkphys sí lo fijan.
  u32 phys;
  bool ck0Route = false;   // ruta directa: VA->phys contiguo (permite bloques que cruzan pagina)
  if((pc & 0xFFFF'FFFF'E000'0000ull) == 0xFFFF'FFFF'8000'0000ull) {
    // ckseg0 solo existe en modo kernel. En usuario o supervisor esa direccion no esta
    // traducida: el VR4300 levanta AdEL en el propio fetch. Atajar aqui a phys = pc & 0x1FFFFFFF
    // se saltaba esa comprobacion y el bloque se ejecutaba igual, asi que el kernel nunca veia
    // la excepcion que el programa esperaba. Modo kernel = KSU==0 (Status[4:3]) o EXL o ERL.
    u32 st = (u32)cop0[C0_Status];
    if(((st >> 3) & 3) != 0 && !(st & 0x6)) { JDECL(DR_MISC); return 0; }  // interprete vectoriza el AdEL
    phys = (u32)pc & 0x1FFF'FFFF;
    ck0Route = true;
  } else {
    // softTLB: la traducción TLB de la PC es cara (scan lineal de 32 entradas). Cachea la
    // última página resuelta; un hit del mismo (vpn,asid) salta el scan por completo.
    u64 vpn  = pc & ~0xFFFull;
    u32 asid = (u32)cop0[C0_EntryHi] & 0xFF;
    if(jitTlbValid && vpn == jitTlbVpn && asid == jitTlbAsid) {
      if(!jitTlbCacheable) { JDECL(DR_UNCACHED); return 0; } // no-mapeado/uncached (cacheado)
      phys = jitTlbPhys | ((u32)pc & 0xFFF);
    } else {
      xlatCacheable = false;
      u64 p = tlbProbePhys(pc);
      jitTlbVpn = vpn; jitTlbAsid = asid; jitTlbValid = true;
      if(p == ~0ull || !xlatCacheable) {          // no-mapeado o uncached → intérprete (y cachea el veredicto)
        jitTlbCacheable = false; jitTlbPhys = 0; JDECL(DR_UNCACHED); return 0;
      }
      jitTlbCacheable = true;
      jitTlbPhys = (u32)p & ~0xFFFu;
      phys = (u32)p;
    }
  }
  if((usize)phys + 4 > mem->rdram.size()) { JDECL(DR_MISC); return 0; }
  if(g_gpcProf.top) g_gpcProf.note(pc, phys, mem->rdram.data(), mem->rdram.size());

  // Hilo ocioso del invitado: se cobra entero sin emitir ni ejecutar nada. Ver jitIdleSkip.
  if(g_idleCpuOn) { if(u32 kIdle = jitIdleSkip(phys)) return kIdle; }

  if(!jitCache) {
    auto* cc = new jit::CodeCache();
    if(!cc->init()) { delete cc; return 0; }
    jitCache = cc;
  }
  jit::CodeCache* cc = jitCache;

  // El mapeo virtual ha cambiado (TLBWI/TLBWR o ASID): los sitios de enlace y las entradas de
  // la ITC guardan traducciones VA->bloque congeladas al compilar/despachar, y ya no valen.
  // Desenlace global. Es barato: unlinkAll() se guarda tras `anyLinked`/`itcAny`, asi que una
  // rafaga de TLBWI (un osMapTLB toca varias entradas seguidas) solo paga el primero.
  if(cc->tlbGen != tlbGen) {
    if(cc->anyLinked || cc->itcAny) cc->nTlbUnlink++;
    cc->unlinkAll();
    cc->tlbGen = tlbGen;
    // Desarmar no basta. Un sitio con destino TLB guarda el par (VA, phys) que se tradujo al
    // COMPILARLO, y linkTo() lo vuelve a armar buscando por PHYS, sin mirar el TLB: tras un
    // remapeo ese par puede ser mentira y el salto enlazado entraria en el bloque equivocado
    // -- medido en Perfect Dark, que acababa ejecutando datos en 0x70003a94. Se re-sondea la
    // VA de cada sitio TLB una vez por cambio de mapeo; el que sigue casando vuelve a ser
    // elegible y el que no se queda fuera hasta que el mapeo vuelva.
    for(jit::LinkSite& L : cc->links) {
      if(!L.tlbTarget) continue;
      bool sc = xlatCacheable; xlatCacheable = false;
      u64 tp = tlbProbePhys(L.targetVA);
      bool ca = xlatCacheable; xlatCacheable = sc;
      L.tlbOk = (tp != ~0ull) && ca && ((u32)tp == L.targetPhys);
    }
  }

  // Cache negativa: este PC ya falló al compilar y su op líder no ha cambiado → intérprete
  // directo, sin volver a emitir. Un fallo de compilación depende SOLO de la palabra líder
  // (compileBlock corta ahí y devuelve nOps==0), así que validarla basta y cuesta un icFetch
  // frente a una compilación entera.
  jit::CodeCache::NoComp& nc = cc->noComp[(phys >> 2) & (jit::CodeCache::kNoCompSlots - 1)];
  if(nc.phys == phys && nc.ck0 == (u8)ck0Route) {
    if((jitIcExact() ? jitPeekWord(phys) : jitFetchWord(phys)) == nc.word) {
      if(jit::g_compFailOn) { u32 LO = nc.word >> 26; jit::g_compFailOp[LO]++;
        if(LO == 0) jit::g_compFailSpecial[nc.word & 63]++;
        else if(LO == 1) jit::g_compFailRegimm[(nc.word >> 16) & 31]++;
        else if(LO == 0x10 || LO == 0x11) jit::g_compFailCop[LO - 0x10][(nc.word >> 21) & 31]++;
        if(LO == 0x10) { u32 rsF = (nc.word >> 21) & 31; if(rsF == 0 || rsF == 4) jit::g_compFailC0Rd[rsF ? 1 : 0][(nc.word >> 11) & 31]++; }
        if(LO == 0x01 || LO == 0x04 || LO == 0x05 || LO == 0x06 || LO == 0x07
        || (LO >= 0x14 && LO <= 0x17) || (LO == 0x11 && ((nc.word >> 21) & 31) == 8)) {
          u32 dw = jitFetchWord(phys + 4); u32 DO = dw >> 26;
          jit::g_compFailDelay[DO]++;
          if(DO == 0) jit::g_compFailDelaySpec[dw & 63]++;
        } }
      JDECL(DR_COMPILE); return 0; }
    nc.phys = ~0u;                              // el código cambió bajo el PC → reintentar
  }

  s32 bi = cc->find(phys);
  if(bi >= 0 && cc->blocks[bi].dead) { bi = -1; if(g_jitStats) g_compDead++; }
  // Un bloque que se pasa de su pagina de entrada asume contiguidad VA->phys: solo es
  // valido por la ruta directa con la que se compilo. Alcanzado el mismo phys por TLB,
  // la pagina siguiente puede mapear a otro sitio -> lo ejecuta el interprete.
  if(bi >= 0 && cc->blocks[bi].crossPage && !ck0Route) { JDECL(DR_MISC); return 0; }   // Step2: bloque invalidado por SMC → recompila (insert lo sobrescribe in-place)
  if(bi < 0) {
    // Plazo cerca: no compilar un bloque que probablemente no se va a poder ejecutar. Con la
    // CPU a pocas ops de un plazo (barrera del SP, fin de tarea, SI, borde de timer) cada op la
    // da el interprete y el despacho siguiente cae en pc+4, un lider NUEVO: se compilaba un
    // bloque entero por op para declinarlo acto seguido (plazo < K). Medido en DK64 Threaded
    // con la cita de DMA (KESTREL_DMARDV): ~25 us por op, la CPU tardaba cientos de us en
    // cubrir las decenas de ops que la separaban de la cita. Solo es coste de anfitrion: el
    // bloque se compila la proxima vez que se llegue a este lider con margen. Un bloque tiene
    // como mucho kMaxOps ops mas la ranura de retardo.
    {
      u32 cnt = (u32)cop0[C0_Count], cmp = (u32)cop0[C0_Compare];
      const u64 kFit = countTicksMax(65);
      u64 due = mem->eventDueIn(mem->cartNow());
      { u64 r = mem->rcpDueIn(mem->cartNow()); if(r < due) due = r; }
      if((u64)(u32)(cmp - cnt) <= kFit || due <= guestOpsMax(65))
        { JDECL(DR_TIMER); return 0; }
    }
    // Reclamo de buffer: si el buf ejecutable desbordó (fugas por dead-mark en SMC pesado),
    // clear global recupera memoria antes de recompilar. Sin esto el JIT quedaría muerto.
    if(cc->buf.overflowed()) { cc->clear(); if(g_jitStats) g_compClears++; }
    if(g_jitStats) g_compiles++;
    jit::Block b = jit::compileBlock(*this, phys);
    if(b.nOps == 0) { nc.phys = phys; nc.word = jitIcExact() ? jitPeekWord(phys) : jitFetchWord(phys); nc.ck0 = (u8)ck0Route; JDECL(DR_COMPILE); return 0; }
    bi = cc->insert(phys, std::move(b));
    if(bi < 0) { cc->clear(); jit::Block b2 = jit::compileBlock(*this, phys); if(b2.nOps==0) return 0; bi = cc->insert(phys, std::move(b2)); if(bi < 0) return 0; }
    // Block-linking Step 3: registra los sitios de enlace que este bloque emitió (se resuelven
    // solos si el destino ya está compilado) y publica el bloque como destino, activando los
    // sitios que ya lo esperaban — incluidos los suyos propios, que es el caso de un bucle
    // auto-enlazado. Sólo aquí, tras insert(), es encontrable por find().
    {
      jit::Block& nb = cc->blocks[bi];
      for(const jit::LinkSite& s : nb.sites) cc->addLink(s);
      cc->linkTo(phys, nb.linkEntry, nb.crossPage);
      nb.linkedEpoch = cc->linkEpoch;
    }
  }
  jit::Block& blk = cc->blocks[bi];
  u32 K = blk.nOps;

  // Cap de depuración KESTREL_MAXINSN: el intérprete lo comprueba op a op, así que un bloque
  // (o una cadena enlazada) que lo cruzase pararía más tarde y en OTRO punto del programa —
  // el banco de pruebas dejaría de comparar el mismo trabajo entre intérprete y JIT. Recorta
  // el presupuesto de cadena al resto y cede las últimas ops al intérprete.
  if(maxInsn) {
    u64 rem = (retired >= maxInsn) ? 0 : (maxInsn - retired);
    if(rem < (u64)jitOpsBudget) jitOpsBudget = (u32)rem;
    if((u64)K > rem) { JDECL(DR_MISC); return 0; }
  }

  // Ventana del llamante: un bloque de K ops que se pase de las que quedan haría que el
  // bucle del sistema tickease el VI DESPUÉS de la cuenta pedida, y el borde de campo caería
  // en una instrucción distinta segun el modo (interp no se pasa nunca, JIT si). Eso desplaza
  // la fase de fotograma entre intérprete/JIT/enlace aunque la semántica sea identica. El
  // bloque solo se toma si cabe entero; el resto de la ventana lo termina el intérprete.
  if(jitOpsBudget && K > jitOpsBudget) { JDECL(DR_MISC); return 0; }

  // Seguridad de timer: no atravesar una frontera Count==Compare dentro del bloque.
  {
    u32 cnt = (u32)cop0[C0_Count], cmp = (u32)cop0[C0_Compare];
    u32 d = cmp - cnt;                 // ticks hasta Count==Compare (mod 2^32)
    // Contra la COTA SUPERIOR de ticks del bloque, no contra K: con el coste de fallos de
    // cache encendido un bloque cuesta mas ticks que ops (ver countTicksMax).
    if((u64)d <= countTicksMax(K)) { JDECL(DR_TIMER); return 0; }  // el intérprete maneja el borde del timer exacto
  }

  // Fetch+validación por op: mantiene el estado de I-cache exacto (fetch cuesta ~0, exp-B)
  // y detecta SMC/DMA que reescriba el código bajo el bloque. icFetch inlineado aquí: es
  // byte-idéntico (misma fill en miss, misma extracción big-endian de la línea) pero evita
  // K llamadas cross-TU — medido ~15-20% del path JIT en PD. La comparación se hace contra
  // la LÍNEA I-cache (no rdram directo): en HW la CPU ejecuta código stale de I-cache si un
  // DMA reescribe rdram sin invalidar, así que validar contra rdram sobre-invalidaría.
  static const int noSmc = std::getenv("KESTREL_JIT_NOSMC") ? 1 : 0;  // DIAGNÓSTICO: mide techo del loop SMC
  // Recorrido por LINEA, no por op: la palabra solo puede cambiar si la linea se relleno de
  // nuevo, y eso lo dice el sello (icFill lo sube siempre). Con el sello y el tag intactos no
  // hace falta mirar ni un byte; solo cuando el sello cambia (evicion + refill, que casi
  // siempre trae los MISMOS bytes) se comparan las palabras del bloque en esa linea. Un bloque
  // de 16 ops pasa de 16 extracciones big-endian a 3 comparaciones de u32.
  u32 endPa = phys + 4 * K;
  const bool icExact = jitIcExact();
  if(!noSmc)
  for(u32 base = phys & ~0x1fu, li = 0; base < endPa; base += 32, li++) {
    u32 idx = (base >> 5) & 0x1ff;
    ICacheLine& l = icache[idx];
    if(!l.valid || l.ptag != base) { if(icExact) continue; icFill(idx, base); }   // exacta: la rellena el bloque
    if(l.seq == blk.lineSeq[li]) continue;                 // linea intacta desde la ultima mirada
    u32 lo = (base > phys) ? base : phys;                  // primer byte del bloque en esta linea
    u32 hi = (base + 32 < endPa) ? base + 32 : endPa;
    for(u32 pa = lo; pa < hi; pa += 4) {
      u32 off = pa & 0x1c;
      u32 w = ((u32)l.data[off] << 24) | ((u32)l.data[off + 1] << 16) | ((u32)l.data[off + 2] << 8) | l.data[off + 3];
      // Step2: solo ESTE bloque -> recompila in-place (no clear global; imprescindible para
      // block-linking). Step3: ademas hay que DESENLAZARLO ya: su codigo va a re-emitirse en
      // otra direccion y cualquier sitio que apunte al viejo saltaria a bytes reciclados.
      if(w != blk.src[(pa - phys) >> 2]) { blk.dead = true; cc->unlinkTo(phys); JDECL(DR_SMC); return 0; }
    }
    blk.lineSeq[li] = l.seq;                               // mismos bytes tras el refill: revalida
  }

  // Re-enlace tras un desenlace global (invalidación de I-cache): este bloque acaba de pasar la
  // validación contra la línea de I-cache, así que vuelve a ser un destino legítimo.
  if(blk.linkedEpoch != cc->linkEpoch) {
    cc->linkTo(phys, blk.linkEntry, blk.crossPage);
    blk.linkedEpoch = cc->linkEpoch;
  }

  // Cache de destinos INDIRECTOS: este bloque acaba de pasar TODA la validacion del driver
  // (SMC contra la linea de I-cache, borde de timer, ventana del bucle) para ESTA VA, asi que
  // es el destino legitimo de cualquier JR/JALR que apunte aqui. Se guarda la VA completa, no
  // el phys: el sitio del JR compara contra el registro, y solo ckseg0 -- donde VA->phys es un
  // desplazamiento fijo, sin TLB de por medio -- puede prometer que esa VA sigue siendo este
  // codigo. Se reescribe en cada despacho: la entrada mas reciente es la que mas vale.
  // Por la ruta TLB tambien: la traduccion que respalda esta VA la vigila cpu.tlbGen (arriba),
  // y un bloque crossPage ya lo ha rechazado el driver antes de llegar aqui cuando !ck0Route.
  if((ck0Route || jit::g_jitTlbLink) && blk.linkEntry && !cc->itc.empty()) {
    jit::CodeCache::ItcEnt& ie = cc->itc[cc->itcIndex(pc)];
    ie.va = pc; ie.code = (u64)(std::uintptr_t)blk.linkEntry;
    cc->itcAny = true;
  }

  // Modo diff (solo bloques SIN memoria): ejecuta el bloque sobre una copia y el intérprete
  // real K pasos; compara. Los bloques con loads/stores se validan con el oráculo systemtest
  // (correr un load 2 veces duplicaría efectos MMIO/store; el intérprete es la verdad ahí).
  static const int diff = std::getenv("KESTREL_JIT_DIFF") ? 1 : 0;
  if(diff && !blk.hasMem && !blk.hasBranch && !blk.hasTrap) {
    // El bloque se ejecuta sobre los registros REALES, no sobre una copia: el codigo emitido
    // recibe en rbx la direccion que se le pasa y direcciona HI/LO/pc como campos del propio
    // CPU a partir de ahi. Con un u64[32] local, un MFLO leia 280 bytes mas alla del array,
    // o sea pila del anfitrion -- de ahi el "jit=ffffffff807ffc58" constante que acusaba al
    // JIT de un fallo del arnes. Se guarda el estado, se corre el bloque, se restaura, y solo
    // entonces manda el interprete.
    u64 pre[32]; for(int r = 0; r < 32; r++) pre[r] = gpr[r];
    u64 sHi = hi, sLo = lo, sPc = pc, sNext = nextPc;
    bool sIn = inDelay, sJb = justBranched;
    u32 sCnt = (u32)cop0[C0_Count], sRnd = (u32)cop0[C0_Random], sFrc = countFrac, sStl = stallCycles;
    u64 sUnc = uncachedReads, sMdo = mulDivOps, sMds = mulDivStall, sFpo = fpuOps, sFps = fpuStall;
    u64 sIlk = ilk, sIlkH = ilkHits, sDcbH = dcbHits, sIlkS = ilkStall; u8 sDcb = dcbR; u64 sStlT = stallTotal;
    u32 Rd = blk.fn(gpr, this) & 0x7FFF'FFFFu; gpr[0] = 0;
    ilk = sIlk; dcbR = sDcb; ilkHits = sIlkH; dcbHits = sDcbH; ilkStall = sIlkS; stallTotal = sStlT;
    u64 tmp[32]; for(int r = 0; r < 32; r++) tmp[r] = gpr[r];
    // restaurar: a partir de aqui el estado del invitado es como si el bloque no hubiera corrido
    for(int r = 0; r < 32; r++) gpr[r] = pre[r];
    hi = sHi; lo = sLo; pc = sPc; nextPc = sNext; inDelay = sIn; justBranched = sJb;
    cop0[C0_Count] = sCnt; cop0[C0_Random] = sRnd; countFrac = sFrc; stallCycles = sStl; uncachedReads = sUnc; mulDivOps = sMdo; mulDivStall = sMds; fpuOps = sFpo; fpuStall = sFps;
    // El bloque lleva su propia guardia (presupuesto, MI, temporizador): cuando no pasa
    // vuelve con 0 ops SIN haber ejecutado nada. Compararlo entonces contra K pasos del
    // interprete acusa al JIT de un fallo que no ha cometido -- es el harness el que no ha
    // corrido nada. Se deja pasar al interprete y ya se comparara en la siguiente vuelta.
    if(Rd != K) { for(u32 s = 0; s < K; s++) step(); return K; }
    for(u32 s = 0; s < K; s++) step();
    for(int r = 1; r < 32; r++) {
      if(gpr[r] != tmp[r]) {
        std::fprintf(stderr, "[jitdiff] MISMATCH phys=%08x K=%u reg $%d interp=%016llx jit=%016llx\n",
                     phys, K, r, (unsigned long long)gpr[r], (unsigned long long)tmp[r]);
        for(u32 i = 0; i < K; i++) {
          u32 o = blk.src[i];
          u32 rs = (o>>21)&31, rt = (o>>16)&31;
          std::fprintf(stderr, "   op[%u] = %08x  pre $rs%u=%016llx $rt%u=%016llx\n", i, o,
                       rs, (unsigned long long)pre[rs], rt, (unsigned long long)pre[rt]);
        }
        if(!jit::g_diffGo || ++jit::g_diffBad > 40) halt("jitdiff mismatch");
        break;
      }
    }
    return K;
  }

  // Diff de branch (KESTREL_JIT_BRDIFF): valida bloques hasBranch pure-ALU (sin mem) contra
  // el intérprete K pasos, comparando gpr + pc + nextPc. Aísla bugs de control/condición.
  // Solo bloques sin memoria: correr un load 2 veces tras el store del intérprete (que aliasa
  // la misma dirección) daría un falso positivo — esos se validan con el oráculo systemtest.
  static const int brdiff = std::getenv("KESTREL_JIT_BRDIFF") ? 1 : 0;
  // DIAGNOSTICO (KESTREL_JIT_BRDIFF_PHYS=0x...): fuerza el diff sobre UN bloque concreto
  // aunque tenga memoria o trampa. Solo vale cuando sus accesos son idempotentes (p.ej. una
  // tanda de stores del mismo valor a la misma direccion, como el prologo de un handler):
  // el interprete corre primero y el bloque repite las mismas escrituras.
  static const u32 brdiffPhys = std::getenv("KESTREL_JIT_BRDIFF_PHYS")
                              ? (u32)std::strtoul(std::getenv("KESTREL_JIT_BRDIFF_PHYS"), nullptr, 0) : 0;
  if((brdiff && blk.hasBranch && !blk.hasMem && !blk.hasTrap) || (brdiffPhys && phys == brdiffPhys)) {
    u64 sg[32]; for(int r = 0; r < 32; r++) sg[r] = gpr[r];
    u64 sPc = pc, sNext = nextPc; bool sIn = inDelay, sJb = justBranched;
    u32 sCnt = (u32)cop0[C0_Count], sRnd = (u32)cop0[C0_Random], sFrc = countFrac, sStl = stallCycles;
    u64 sUnc = uncachedReads, sMdo = mulDivOps, sMds = mulDivStall, sFpo = fpuOps, sFps = fpuStall;
    // Con BRDIFF_PHYS tambien se compara la RDRAM: un bloque puede dejar los gpr identicos y
    // escribir mal (dato o direccion), que es justo lo que hace un prologo de handler.
    static std::vector<u8> memPre, memPost;   // copias planas: GuestBytes usa otro allocador
    const bool memCmp = brdiffPhys && phys == brdiffPhys;
    u64 sIlk = ilk, sIlkH = ilkHits, sDcbH = dcbHits, sIlkS = ilkStall; u8 sDcb = dcbR; u64 sStlT = stallTotal;
    static DCacheLine dcPre[512], dcPost[512];
    if(memCmp) { memPre.assign(mem->rdram.begin(), mem->rdram.end());
                 std::memcpy(dcPre, dcache, sizeof(dcache)); }
    // intérprete K pasos → referencia
    for(u32 s = 0; s < K; s++) step();
    if(memCmp) { memPost.assign(mem->rdram.begin(), mem->rdram.end());
                 std::memcpy(dcPost, dcache, sizeof(dcache));
                 std::memcpy(mem->rdram.data(), memPre.data(), memPre.size());
                 std::memcpy(dcache, dcPre, sizeof(dcache)); }
    u64 iG[32]; for(int r = 0; r < 32; r++) iG[r] = gpr[r];
    u64 iPc = pc, iNext = nextPc;
    bool iIn = inDelay, iJb = justBranched;
    u32 iCnt = (u32)cop0[C0_Count], iRnd = (u32)cop0[C0_Random], iFrc = countFrac, iStl = stallCycles;
    u64 iUnc = uncachedReads, iMdo = mulDivOps, iMds = mulDivStall, iFpo = fpuOps, iFps = fpuStall;
    u64 iIlk = ilk, iIlkH = ilkHits, iDcbH = dcbHits, iIlkS = ilkStall; u8 iDcb = dcbR; u64 iStlT = stallTotal;
    // restaura y corre el bloque
    ilk = sIlk; dcbR = sDcb; ilkHits = sIlkH; dcbHits = sDcbH; ilkStall = sIlkS; stallTotal = sStlT;
    for(int r = 0; r < 32; r++) gpr[r] = sg[r];
    pc = sPc; nextPc = sNext; inDelay = sIn; justBranched = sJb;
    cop0[C0_Count] = sCnt; cop0[C0_Random] = sRnd; countFrac = sFrc; stallCycles = sStl; uncachedReads = sUnc; mulDivOps = sMdo; mulDivStall = sMds; fpuOps = sFpo; fpuStall = sFps;
    u32 Rr = blk.fn(gpr, this); gpr[0] = 0;
    u32 Rops = Rr & 0x7FFF'FFFFu; bool isCtrl = (Rr & 0x8000'0000u) != 0;
    if(!isCtrl) { pc = sPc + 4 * Rops; nextPc = pc + 4; }  // bail: avance secuencial
    // Mismo caso que arriba: guardia no pasada -> el bloque no ha corrido, no hay que juzgarlo.
    if(!isCtrl && Rops != K) {
      for(int r = 0; r < 32; r++) gpr[r] = iG[r];
      pc = iPc; nextPc = iNext; inDelay = iIn; justBranched = iJb;
      cop0[C0_Count] = iCnt; cop0[C0_Random] = iRnd; countFrac = iFrc; stallCycles = iStl; uncachedReads = iUnc; mulDivOps = iMdo; mulDivStall = iMds; fpuOps = iFpo; fpuStall = iFps;
      ilk = iIlk; dcbR = iDcb; ilkHits = iIlkH; dcbHits = iDcbH; ilkStall = iIlkS; stallTotal = iStlT;
      return K;                                  // manda el interprete, ya avanzado arriba
    }
    bool bad = (pc != iPc) || (nextPc != iNext) || ilk != iIlk || dcbR != iDcb;
    for(int r = 1; r < 32; r++) if(gpr[r] != iG[r]) bad = true;
    if(memCmp && (std::memcmp(mem->rdram.data(), memPost.data(), memPost.size()) != 0 ||
                  std::memcmp(dcache, dcPost, sizeof(dcache)) != 0)) {
      for(int q = 0; q < 512; q++)
        if(std::memcmp(&dcache[q], &dcPost[q], sizeof(DCacheLine)) != 0)
          std::fprintf(stderr, "[dcdiff] phys=%08x linea=%d interp(tag=%08x v=%d d=%d) jit(tag=%08x v=%d d=%d)\n",
                       phys, q, dcPost[q].ptag(), (int)dcPost[q].valid(), (int)dcPost[q].dirty,
                       dcache[q].ptag(), (int)dcache[q].valid(), (int)dcache[q].dirty);
      for(usize q = 0; q + 8 <= memPost.size(); q += 8)
        if(std::memcmp(&mem->rdram[q], &memPost[q], 8) != 0) {
          u64 vi = 0, vj = 0;
          for(int t = 0; t < 8; t++) { vi = (vi << 8) | memPost[q + t]; vj = (vj << 8) | mem->rdram[q + t]; }
          std::fprintf(stderr, "[memdiff] phys=%08x K=%u @%08x interp=%016llx jit=%016llx\n",
                       phys, K, (u32)q, (unsigned long long)vi, (unsigned long long)vj);
        }
      bad = true;
    }
    if(bad) {
      std::fprintf(stderr, "[brdiff] MISMATCH phys=%08x va=%016llx K=%u ctrl=%d Rops=%u\n"
                   "  pc: interp=%016llx jit=%016llx   nextPc: interp=%016llx jit=%016llx\n",
                   phys, (unsigned long long)sPc, K, isCtrl, Rops,
                   (unsigned long long)iPc, (unsigned long long)pc,
                   (unsigned long long)iNext, (unsigned long long)nextPc);
      for(u32 i = 0; i < K; i++) std::fprintf(stderr, "   op[%u]=%08x\n", i, blk.src[i]);
      for(int r = 1; r < 32; r++) if(gpr[r] != iG[r])
        std::fprintf(stderr, "   $%d interp=%016llx jit=%016llx\n", r,
                     (unsigned long long)iG[r], (unsigned long long)gpr[r]);
      halt("brdiff mismatch");
    }
    retired += Rops; countFrac = sFrc; stallCycles = sStl; uncachedReads = sUnc; mulDivOps = sMdo; mulDivStall = sMds; fpuOps = sFpo; fpuStall = sFps; cop0[C0_Count] = sCnt;
    if(countAdd(countTicks(Rops))) timerIntr = true;
    cc->hits += Rops; return Rops;
  }

  // Ejecuta el bloque. Devuelve R = ops REALMENTE retiradas: R==K en éxito total, o el índice
  // de la primera mem-op que faultaría (bail limpio, sin efectos). El intérprete re-ejecuta la
  // op R para vectorizar la excepción exacta, así que aquí solo avanzamos el estado por R.
  jitChain = 0;                       // presupuesto de cadena fresco por entrada del driver
  jitChainOps = 0;                    // ops que la cadena commitee por su cuenta (las sumamos al salir)
  // El primer bloque de la cadena necesita el chequeo completo, pero NO hace falta llegar a el
  // por el trampolin: el camino rapido en linea veia jitGuard==0, saltaba a
  // kestrel_jitProceedTramp y este llamaba a jitReenterProceed. Eso era una llamada Win64 y una
  // rama fallada por CADA entrada al driver (3,1 M por corrida de PD, 3,85 % del hilo de CPU en
  // el perfil de anfitrion). Se llama aqui directamente: es la MISMA funcion con la MISMA K y
  // con jitChain recien puesto a 0, o sea el mismo jitChain==1 que veia el trampolin, asi que
  // no cambia ni un bit del estado del invitado. Si dice que no, se devuelve el control igual
  // que hacia el bail del prologo, con pc intacto.
  // KESTREL_JIT_NOPREARM=1 vuelve al viaje por el trampolin, para bisecar.
  static const bool noPreArm = std::getenv("KESTREL_JIT_NOPREARM") != nullptr;
  jitGuard = 0;
  if(!noPreArm && !jitReenterProceed(K)) { jitChain = 0; return 0; }
  const u64 entryVAdbg = pc;          // solo para el chequeo de pc canonico de abajo
  // `memAbort` es un pestillo POR INSTRUCCION: lo pone translate() cuando el acceso falla y
  // significa "esta instruccion abort�". step() lo limpia al empezar cada instruccion; el
  // bloque compilado tambien tiene que hacerlo, porque puede entrar justo despues de que una
  // excepcion lo dejara puesto (p.ej. el prologo de un handler, al que se vectoriza con el
  // pestillo aun a 1). Sin esto, translate() cortocircuita con `return 0` y cualquier op del
  // bloque que no coja el camino directo (kernel de 64 bits: KX=1) traduce a fisico 0.
  memAbort = false;
  if(jit::g_jitDump) { blk.runs++; jit::g_dumpCache = cc; }
  // prof.* dmiss/imiss bajo JIT: los fallos del bloque van a su PC fisica de entrada (sin
  // esto caian en la ultima op interpretada, a veces muy lejos)
  if(__builtin_expect(profOn, 0) && phys < (8u << 20)) profCurPhys = phys;
  u32 Rraw = blk.fn(gpr, this);
  if(g_jitStats) {
    if(Rraw & 0x80000000u)        jit::g_retBranch++;   // salio por un terminador de salto
    else if((Rraw & 0x7fffffff) >= K) jit::g_retFull++;  // agoto el bloque sin salto
    else                          jit::g_retShort++;    // aborto/cesion a media altura
  }
  gpr[0] = 0;
  // Bit alto = el bloque terminó en un branch absorbido: ya escribió pc/nextPc/inDelay/
  // justBranched por sí mismo. Sólo avanzamos contadores; NO tocamos el control de flujo.
  bool ctrl = (Rraw & 0x8000'0000u) != 0;
  u32 R = Rraw & 0x7FFF'FFFFu;
  // DIAGNOSTICO (KESTREL_JIT_PCCHK): en modo de 32 bits toda direccion virtual valida es la
  // extension de signo de sus 32 bits bajos. Un pc con basura arriba solo puede venir de una
  // salida de control que compuso mal el destino, y saltarlo hace que la excepcion aparezca
  // muy lejos del bloque culpable. Aqui se caza en el acto, con el bloque delante.
  // DIAGNOSTICO (KESTREL_JIT_REGCHK=<n>): igual que pcchk pero sobre un gpr concreto. Sirve
  // para cazar el bloque que ensucia la parte alta de un registro que deberia ir extendido.
  {
    static const int rchk = std::getenv("KESTREL_JIT_REGCHK")
                          ? (int)std::strtol(std::getenv("KESTREL_JIT_REGCHK"), nullptr, 0) : -1;
    static const u64 rchkHalt = std::getenv("KESTREL_JIT_REGCHK_FROM")
                              ? std::strtoull(std::getenv("KESTREL_JIT_REGCHK_FROM"), nullptr, 0) : 0;
    static const u64 rchkVal = std::getenv("KESTREL_JIT_REGCHK_VAL")
                             ? std::strtoull(std::getenv("KESTREL_JIT_REGCHK_VAL"), nullptr, 0) : 0;
    if(rchk >= 0 && retired >= rchkHalt &&
       (rchkVal ? gpr[rchk] == rchkVal : (u64)(s64)(s32)gpr[rchk] != gpr[rchk])) {
      std::fprintf(stderr, "[regchk] $%d=%016llx tras bloque phys=%08x entryVA=%016llx nOps=%u R=%u ctrl=%d\n",
                   rchk, (unsigned long long)gpr[rchk], phys, (unsigned long long)entryVAdbg,
                   blk.nOps, R, ctrl ? 1 : 0);
      for(u32 i = 0; i < blk.nOps && i < blk.src.size(); i++)
        std::fprintf(stderr, "   op[%u]=%08x  %s\n", i, blk.src[i],
                     disasm(blk.src[i], entryVAdbg + 4 * i).c_str());
      std::fflush(stderr);
      halt("regchk");
    }
  }
  if(jit::g_pcChk && ctrl && (u64)(s64)(s32)pc != pc) {
    std::fprintf(stderr, "[pcchk] bloque phys=%08x entryVA=%016llx nOps=%u -> pc=%016llx nextPc=%016llx\n",
                 phys, (unsigned long long)entryVAdbg, blk.nOps,
                 (unsigned long long)pc, (unsigned long long)nextPc);
    for(u32 i = 0; i < blk.nOps && i < blk.src.size(); i++)
      std::fprintf(stderr, "   op[%u]=%08x  %s\n", i, blk.src[i],
                   disasm(blk.src[i], entryVAdbg + 4 * i).c_str());
    std::fflush(stderr);
    halt("pcchk");
  }
  if(g_jitStats) { g_jitBlocks++; g_jitOps += R; }

  // R==0 = el bloque salio SIN retirar nada (guardia del prologo no pasada, o bail limpio en
  // la op 0: mem-op que faultaria, ALU que desborda). Entonces NO ha pasado nada y no hay
  // estado de flujo que avanzar. Pisar nextPc/inDelay/justBranched aqui destruye una ranura
  // de retardo en curso: si se entro al bloque con inDelay puesto, nextPc guarda el destino
  // del salto y sustituirlo por pc+4 pierde el salto entero. El int�rprete re-ejecuta la op
  // 0 con el estado intacto, que es justo lo que el bail promete.
  if(!ctrl && R) {
    // Avanza el estado exactamente R instrucciones secuenciales no-branch.
    pc += 4 * R;
    nextPc = pc + 4;
    inDelay = false; justBranched = false;
  }
  retired += R;
  // prof.* bajo JIT: el bloque entero va al cubo de su PC fisica de entrada (las cadenas
  // enlazadas, al bloque cabeza: KESTREL_JIT_NOLINK=1 para el reparto exacto por bloque)
  if(__builtin_expect(profOn, 0) && phys < (8u << 20)) {
    u32 nR = R + jitPending; profBuckets[phys >> kProfShift] += nR; profTotal += nR;
  }
  // La guarda de arriba garantiza que no se cruza Compare aqui, pero el latch se hace igual:
  // countAdd lo comprueba gratis y asi no hay un solo camino que pueda perderse el borde.
  if(countAdd(countTicks(R))) timerIntr = true;   // antes: sin latch (garantizado: d>K≥R arriba)

  // Random cuenta atrás R pasos (randomReload==0 garantizado arriba).
  {
    cop0[C0_Random] = randomAdvance((u32)cop0[C0_Random], (u32)cop0[C0_Wired], R);
  }
  cc->hits += R;
  // Eslabones que entraron por el camino rápido: sus ops quedaron en jitPending porque ningún
  // trampolín llegó a commitearlas. Se commitean aquí, al salir de la cadena, exactamente como
  // haría el trampolín (el borde de timer estaba cubierto por el permiso, ver kGuardMaxOps).
  if(u32 p = jitPending) {
    jitPending = 0;
    retired += p;
    if(countAdd(countTicks(p))) timerIntr = true;
    cop0[C0_Random] = randomAdvance((u32)cop0[C0_Random], (u32)cop0[C0_Wired], p);
    cc->hits += p;
    jitChainOps += p;
  }
  if(g_jitStats) { g_chainOps += jitChainOps; g_chainLinks += jitChain; }
  // R = ops del ÚLTIMO bloque; jitChainOps = las de los eslabones anteriores, ya contabilizadas
  // en retired/Count/hits por el prólogo del sucesor. El total es lo que avanzó el guest.
  return R + jitChainOps;
}

}  // namespace kestrel
