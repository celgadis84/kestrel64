#include "jit.hpp"
#include "cpu.hpp"
#include "../core/memory.hpp"
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>

#if defined(_WIN32)
  #include <windows.h>
#else
  #include <sys/mman.h>
  #include <unistd.h>
#endif

namespace kestrel::jit {

// DIAGNOSTICO (KESTREL_JIT_STATS): por donde sale cada terminador de bloque. El incremento se
// emite en linea SOLO cuando las estadisticas estan puestas, asi que el codigo de produccion
// no lo paga. Contestan a por que una cadena solo encadena ~14 bloques: enlace estatico
// acertado / ITC acertada / salida lenta al driver (con destino estatico o sin el).
u32 g_xLink = 0, g_xItc = 0, g_xSlowDir = 0, g_xSlowInd = 0;
// Y por el otro lado: como TERMINA el bloque que devuelve el control al driver. Un bloque
// puede salir por su terminador de salto (los cuatro contadores de arriba) o por el
// epilogo: agotar sus ops sin salto (secuencial), abortar en una mem-op o ceder en una op
// interpretada que cambio el control.
u64 g_retBranch = 0, g_retFull = 0, g_retShort = 0;
static const int g_xStats = std::getenv("KESTREL_JIT_STATS") ? 1 : 0;

CodeBuffer::~CodeBuffer() {
  if(!base) return;
#if defined(_WIN32)
  VirtualFree(base, 0, MEM_RELEASE);
#else
  munmap(base, cap);
#endif
  base = nullptr;
}

auto CodeBuffer::init(usize bytes) -> bool {
#if defined(_WIN32)
  base = (u8*)VirtualAlloc(nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
#else
  base = (u8*)mmap(nullptr, bytes, PROT_READ | PROT_WRITE | PROT_EXEC,
                   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if(base == MAP_FAILED) base = nullptr;
#endif
  cap = base ? bytes : 0;
  used = 0;
  return base != nullptr;
}

auto CodeBuffer::finalize(u8* from) -> void {
  if(!base) return;
#if defined(_WIN32)
  FlushInstructionCache(GetCurrentProcess(), from, (usize)(cursor() - from));
#else
  __builtin___clear_cache((char*)from, (char*)cursor());
#endif
}

// Signature of the emitted self-test thunk: takes a pointer to a u64[3] slot array.
using SmokeFn = u64 (*)(u64*);

auto smokeTest() -> bool {
  CodeBuffer buf;
  if(!buf.init(4096)) { std::fprintf(stderr, "[jit] smokeTest: RWX alloc failed\n"); return false; }

  u8* entry = buf.cursor();
  Emitter e(buf);
  // Win64 ABI: primer argumento (u64* slots) llega en RCX; retorno en RAX.
  //   rax = slots[0]
  //   rax += slots[1]
  //   slots[2] = rax
  //   return rax
  e.mov_r_m(RAX, RCX, 0);    // mov rax, [rcx+0]
  e.add_r_m(RAX, RCX, 8);    // add rax, [rcx+8]
  e.mov_m_r(RCX, 16, RAX);   // mov [rcx+16], rax
  e.ret();

  if(buf.overflowed()) { std::fprintf(stderr, "[jit] smokeTest: code buffer overflow\n"); return false; }
  buf.finalize(entry);

  u64 slots[3] = { 0x0000'0001'2345'6789ull, 0x0000'0000'1111'1111ull, 0 };
  SmokeFn fn = reinterpret_cast<SmokeFn>(entry);
  u64 got = fn(slots);
  u64 want = slots[0] + slots[1];
  bool ok = (got == want) && (slots[2] == want);
  std::fprintf(stderr, "[jit] smokeTest: emitted %zu bytes, fn()=0x%016llx slot[2]=0x%016llx want=0x%016llx -> %s\n",
               buf.used, (unsigned long long)got, (unsigned long long)slots[2],
               (unsigned long long)want, ok ? "PASS" : "FAIL");
  return ok;
}

// Helper de prueba para callBailSelfTest: registra el arg y devuelve fault/ok según ctx.
// ctx[0] = flag "queremos fault" (in), ctx[1] = arg recibido (out).
extern "C" u8 jitTestHelper(u64* ctx, u32 arg) {
  ctx[1] = arg;
  return ctx[0] ? 0u : 1u;   // 0 = faulted (bail), 1 = ok (sigue)
}

using CallBailFn = u32 (*)(u64* gpr, u64* ctx);

static auto buildCallBail(CodeBuffer& buf) -> CallBailFn {
  Emitter e(buf);
  u8* entry = buf.cursor();
  // Prólogo: rbx=gpr (rcx), r12=ctx (rdx). RSP: entry≡8; push,push→≡8; sub 40→≡0 (16-alin).
  e.push_reg(RBX); e.push_reg(R12);
  e.mov_r_r(RBX, RCX);        // rbx = gpr
  e.mov_r_r(R12, RDX);        // r12 = ctx
  e.sub_rsp_imm8(40);
  // op0 (ALU): gpr[1] += 1
  e.ld64(RAX, 1); e.alu64_imm(0, RAX, 1); e.st64(RAX, 1);
  // op1 (mem-like): al = jitTestHelper(ctx, 0xAA); si al==0 → bail (return 1)
  e.mov_r_r(RCX, R12);                 // arg0 = ctx
  e.mov_r_imm32(RDX, 0xAA);            // arg1 = 0xAA
  e.mov_r_imm64(RAX, (u64)&jitTestHelper);
  e.call_reg(RAX);
  e.test_al_al();
  usize bail = e.je_rel32_placeholder();   // je bail (si faulted)
  // op2 (ALU): gpr[2] += 1   (solo si no hubo bail).  alu64_imm(/digit=0=ADD, dst, imm=1)
  e.ld64(RAX, 2); e.alu64_imm(0, RAX, 1); e.st64(RAX, 2);
  // fin normal: eax = 3 (K); jmp epi
  e.mov_r_imm32(RAX, 3);
  usize toEpi = e.jmp_rel32_placeholder();
  // bail: eax = 1 (índice de la op que falló)
  e.patchRel32(bail);
  e.mov_r_imm32(RAX, 1);
  // epi:
  e.patchRel32(toEpi);
  e.add_rsp_imm8(40);
  e.pop_reg(R12); e.pop_reg(RBX);
  e.ret();
  buf.finalize(entry);
  return reinterpret_cast<CallBailFn>(entry);
}

auto callBailSelfTest() -> bool {
  CodeBuffer buf;
  if(!buf.init(4096)) { std::fprintf(stderr, "[jit] callBail: RWX alloc failed\n"); return false; }
  CallBailFn fn = buildCallBail(buf);
  if(buf.overflowed()) { std::fprintf(stderr, "[jit] callBail: buffer overflow\n"); return false; }
  bool allOk = true;
  for(int wantFault = 0; wantFault <= 1; wantFault++) {
    u64 gpr[3] = { 0, 100, 200 };
    u64 ctx[2] = { (u64)wantFault, 0 };
    u32 k = fn(gpr, ctx);
    u32 wantK = wantFault ? 1u : 3u;
    u64 wantG1 = 101;                       // op0 siempre corre
    u64 wantG2 = wantFault ? 200u : 201u;   // op2 solo si no bail
    bool ok = (k == wantK) && (gpr[1] == wantG1) && (gpr[2] == wantG2) && (ctx[1] == 0xAA);
    allOk &= ok;
    std::fprintf(stderr, "[jit] callBail wantFault=%d -> K=%u gpr1=%llu gpr2=%llu argSeen=%llx : %s\n",
                 wantFault, k, (unsigned long long)gpr[1], (unsigned long long)gpr[2],
                 (unsigned long long)ctx[1], ok ? "PASS" : "FAIL");
  }
  return allOk;
}

// ============================ CodeCache =====================================

auto CodeCache::init() -> bool {
  if(!buf.init(16 * 1024 * 1024)) return false;   // 16 MB de código emitido
  u32 cap = 1u << 16;                              // 65536 ranuras
  index.assign(cap, 0xFFFF'FFFFu);
  slot.assign(cap, -1);
  mask = cap - 1;
  blocks.reserve(8192);
  noComp.assign(kNoCompSlots, NoComp{});
  {
    const char* e = std::getenv("KESTREL_JIT_ITCBITS");
    u32 bits = e ? (u32)std::strtoul(e, nullptr, 0) : kItcBitsDefault;
    if(bits < 8 || bits > 20) bits = kItcBitsDefault;
    itcMask = (1u << bits) - 1;
    itc.assign((usize)itcMask + 1, ItcEnt{});
  }
  ready = true;
  return true;
}

auto CodeCache::find(u32 phys) -> s32 {
  u32 h = (phys >> 2) & mask;
  for(u32 n = 0; n <= mask; n++) {
    u32 s = (h + n) & mask;
    if(slot[s] < 0) return -1;              // ranura vacía → no está
    if(index[s] == phys) return slot[s];
  }
  return -1;
}

auto CodeCache::insert(u32 phys, Block&& b) -> s32 {
  u32 h = (phys >> 2) & mask;
  for(u32 n = 0; n <= mask; n++) {
    u32 s = (h + n) & mask;
    if(slot[s] < 0) {
      blocks.push_back(std::move(b));
      s32 idx = (s32)blocks.size() - 1;
      index[s] = phys; slot[s] = idx; compiles++;
      return idx;
    }
    if(index[s] == phys) { blocks[slot[s]] = std::move(b); return slot[s]; }
  }
  return -1;   // tabla llena
}

auto CodeCache::clear() -> void {
  buf.reset();
  blocks.clear();
  std::fill(index.begin(), index.end(), 0xFFFF'FFFFu);
  std::fill(slot.begin(),  slot.end(),  -1);
  // Todo el código emitido (y con él las guardas/ranuras de enlace) deja de existir: los
  // punteros de `links` apuntarían a bytes reciclados por el bump-allocator.
  links.clear();
  byTarget.clear();
  itcClear();     // sus puntos de entrada son bytes reciclados igual que los de `links`
}

// ---- block-linking: (des)enlace, siempre por escritura de DATOS ---------------
// Un sitio se activa poniendo el VA de destino en la guarda y el punto de entrada en la
// ranura; se desactiva devolviendo la guarda a kNoLink (VA imposible). El código emitido
// nunca se reescribe, así que no hay coherencia de I-cache de host que gestionar.
auto CodeCache::addLink(const LinkSite& s) -> void {
  u32 idx = (u32)links.size();
  links.push_back(s);
  *s.vaImm = kNoLink;          // nace desenlazado
  *s.slot  = 0;
  byTarget[s.targetPhys].push_back(idx);
  s32 bi = find(s.targetPhys);
  // Un sitio con destino TLB no puede saltar a un bloque que se pasa de su pagina de entrada:
  // ese bloque solo es correcto por la ruta directa con la que se compilo (ver Block::crossPage).
  if(bi >= 0 && !blocks[bi].dead && blocks[bi].linkEntry
     && !(s.tlbTarget && (blocks[bi].crossPage || !s.tlbOk))) {
    LinkSite& L = links[idx];
    *L.slot = (u64)(std::uintptr_t)blocks[bi].linkEntry;
    *L.vaImm = L.targetVA;
    nLinked++;
    anyLinked = true;
  }
}

auto CodeCache::linkTo(u32 phys, u8* entry, bool xpage) -> void {
  auto it = byTarget.find(phys);
  if(it == byTarget.end() || !entry) return;
  for(u32 i : it->second) {
    LinkSite& L = links[i];
    // Destino TLB: ni a un bloque crossPage (ver addLink) ni con la traduccion caducada.
    if(L.tlbTarget && (xpage || !L.tlbOk)) continue;
    *L.slot = (u64)(std::uintptr_t)entry;
    *L.vaImm = L.targetVA;
    nLinked++;
    anyLinked = true;
  }
}

auto CodeCache::unlinkTo(u32 phys) -> void {
  // La cache de destinos indirectos se indexa por VA y no sabe a que bloque pertenece cada
  // entrada, asi que un bloque muerto obliga a vaciarla entera. Solo pasa en SMC/recompilacion,
  // que es raro; guardar el phys en cada entrada para barrer selectivamente costaria en el
  // sondeo, que es lo caliente.
  itcClear();
  auto it = byTarget.find(phys);
  if(it == byTarget.end()) return;
  for(u32 i : it->second) { *links[i].vaImm = kNoLink; nUnlinked++; }
}

auto CodeCache::unlinkAll() -> void {
  // La invalidacion de I-cache mata tambien la cache indirecta: sus entradas apuntan a codigo
  // compilado de bytes que el guest acaba de declarar obsoletos. Va fuera de la guarda
  // `anyLinked`, que solo habla de los sitios de enlace estatico.
  nInvalAll++;
  if(itcAny) nItcClear++;
  itcClear();
  if(!anyLinked) return;      // ya está todo desenlazado: nada que recorrer (ver jit.hpp)
  nInvalAllEff++;
  anyLinked = false;
  for(LinkSite& L : links) *L.vaImm = kNoLink;
  nUnlinked += links.size();
  linkEpoch++;
  for(Block& b : blocks) b.linkedEpoch = ~0ull;   // fuerza re-enlace tras revalidar
}

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
extern "C" u8 jitMemThunk(void* cpu, u32 op) {
  return reinterpret_cast<kestrel::CPU*>(cpu)->jitMem(op);
}

// Trampolines especializados por opcode (definidos en cpu.cpp). El bloque llama al suyo con
// la direccion y el dato YA calculados, en vez de pasar la op cruda para que el helper la
// decodifique y relea cpu->gpr.
extern "C" u8 kestrel_jitLB (void*, u64, u32, u64); extern "C" u8 kestrel_jitLH (void*, u64, u32, u64);
extern "C" u8 kestrel_jitLW (void*, u64, u32, u64); extern "C" u8 kestrel_jitLBU(void*, u64, u32, u64);
extern "C" u8 kestrel_jitLHU(void*, u64, u32, u64); extern "C" u8 kestrel_jitLWU(void*, u64, u32, u64);
extern "C" u8 kestrel_jitLD (void*, u64, u32, u64); extern "C" u8 kestrel_jitSB (void*, u64, u32, u64);
extern "C" u8 kestrel_jitSH (void*, u64, u32, u64); extern "C" u8 kestrel_jitSW (void*, u64, u32, u64);
extern "C" u8 kestrel_jitSD (void*, u64, u32, u64);
extern "C" u8 kestrel_jitLWC1(void*, u64, u32, u64); extern "C" u8 kestrel_jitLDC1(void*, u64, u32, u64);
extern "C" u8 kestrel_jitSWC1(void*, u64, u32, u64); extern "C" u8 kestrel_jitSDC1(void*, u64, u32, u64);
extern "C" u8 kestrel_jitMFC1 (void*, u32, u32); extern "C" u8 kestrel_jitDMFC1(void*, u32, u32);
extern "C" u8 kestrel_jitCTC1(void*, u32, u32);
extern "C" u8 kestrel_jitCVTWS(void*, u32, u32);   extern "C" u8 kestrel_jitTRUNCWS(void*, u32, u32);
extern "C" u8 kestrel_jitCVTWD(void*, u32, u32);   extern "C" u8 kestrel_jitTRUNCWD(void*, u32, u32);
extern "C" u8 kestrel_jitCVTDS(void*, u32, u32);   extern "C" u8 kestrel_jitCVTSD(void*, u32, u32);
extern "C" u8 kestrel_jitCMPS (void*, u32, u32);   extern "C" u8 kestrel_jitCMPD  (void*, u32, u32);
extern "C" u8 kestrel_jitCVTSW(void*, u32, u32);   extern "C" u8 kestrel_jitCVTDW(void*, u32, u32);
extern "C" u8 kestrel_jitDIVS (void*, u32, u32);   extern "C" u8 kestrel_jitDIVD (void*, u32, u32);
extern "C" u8 kestrel_jitCFC1 (void*, u32, u32); extern "C" u8 kestrel_jitMTC1 (void*, u32, u32);
extern "C" u8 kestrel_jitDMTC1(void*, u32, u32);
extern "C" u8 kestrel_jitInterpDelay(void*, u32, u32); extern "C" u8 kestrel_jitCACHE(void*, u32, u32);
extern "C" u8 kestrel_jitADDS(void*, u32, u32); extern "C" u8 kestrel_jitSUBS(void*, u32, u32);
extern "C" u8 kestrel_jitMULS(void*, u32, u32); extern "C" u8 kestrel_jitADDD(void*, u32, u32);
extern "C" u8 kestrel_jitSUBD(void*, u32, u32); extern "C" u8 kestrel_jitMULD(void*, u32, u32);

// Emite un load/store soportado como call jitMemThunk(cpu,op) + test al,al + je(placeholder).
// Convención del bloque 2b: r12=cpu, rbx=gpr. *bailSite = offset del disp32 del je (a parchear
// al epílogo); *isStore = si muta memoria. Devuelve false si op no es un mem-op soportado.
static auto emitMemOp(Emitter& e, RegCache& rc, u32 op, usize& bailSite, bool& isStore,
                      RcSnap& snap, u32 opsBefore, s32 pendOff) -> bool {
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
  static const int g_noFastMem = std::getenv("KESTREL_JIT_NOFASTMEM")
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
      e.test_m8_imm(RBX, stOff + 3, 0x20);            // Status bit29 vive en el byte 3
      fastFail[nFail++] = e.je_rel32_placeholder();
      // FR=1: los 32 registros son independientes y el acceso cae en fpr[rt]. FR=0: los pares
      // se juntan, y para rt PAR el destino sigue siendo fpr[rt] -- solo el IMPAR cambia (la
      // mitad alta del companero en 32 bits, el par entero en 64). Como la paridad se sabe al
      // compilar, la comprobacion de FR solo se emite para registros impares.
      if(rt & 1) {
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
    e.test_m8_imm(RBX, stOff, 0x18);                  // KSU!=0: traduccion general (KX=1 = ckseg0, mismo AND)
    fastFail[nFail++] = e.jne_rel32_placeholder();
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
  if(hasFast) e.patchRel32(fastDone);     // el camino rapido se reune aqui
  if(!isStore && !isFp) rc.forget(rt);    // el helper acaba de escribir gpr[rt] en memoria
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
static const bool g_diffGo = std::getenv("KESTREL_JIT_DIFFGO") != nullptr;
static const bool g_pcChk = std::getenv("KESTREL_JIT_PCCHK") != nullptr;
// DIAGNOSTICO (KESTREL_REGCHK): cordura del banco de registros en cada despacho del driver.
// Cuando el guest se descarrila sin excepcion previa, el sintoma final (pc paseando por MMIO)
// llega cientos de miles de ops despues del error real. Pero hay dos registros cuyo valor es
// invariante en un juego sano de 32 bits: el puntero de pila y la direccion de retorno son
// SIEMPRE la extension de signo de un KSEG0/KSEG1 (0x8.../0xA...) -- libultra no pone pilas
// en ningun otro sitio. En cuanto uno de los dos deja de serlo, el error acaba de ocurrir.
static const bool g_regChk = std::getenv("KESTREL_REGCHK") != nullptr;
static auto regChkBad(u64 v) -> bool {
  if(v == 0) return false;
  if((u64)(s64)(s32)v != v) return true;          // no es una VA de 32 bits extendida
  // PD ejecuta desde kuseg mapeado por TLB en 0x70000000, asi que `ra` vive ahi; las
  // pilas de libultra viven en KSEG0. Por debajo de 0x70000000 no hay ni codigo ni pila.
  return (u32)v < 0x80000000u;                    // las pilas de libultra viven en KSEG0
}
static u32 g_diffBad = 0;

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
      default: return false;
    }
    dst = rd;
  }
  const s32 stOff = (s32)(offsetof(CPU, cop0) + 8u * (u32)CPU::C0_Status);
  e.test_m8_imm(RBX, stOff, 0x18);                             // Status[4:3] = KSU
  bailSite = e.jne_rel32_placeholder();                        // no-kernel -> lo resuelve el interprete
  snap = rc.snap();
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
extern "C" u8 jitInterpThunk(void* cpu, u32 op, u32 off) {
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
static auto emitInterpOp(Emitter& e, RegCache& rc, u32 op, u32 off, usize& exitSite,
                         RcSnap& snap, bool delay = false, bool cop0Term = false) -> bool {
  u32 OP = op >> 26;
  bool ok = false;
  switch(OP) {
    // MTC0 y las de funcion COP0 (ERET / TLBR / TLBWI / TLBP), todas TERMINALES.
    case 0x10: ok = cop0Term && (((op >> 21) & 31) == 4 || ((op >> 21) & 31) == 0x10); break;
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
  e.mov_r_r(RCX, RBX);                    // arg0 = cpu (== &gpr[0] == RBX)
  e.mov_r_imm32(RDX, op);                 // arg1 = op
  e.mov_r_imm32(R8, off);                 // arg2 = offset de la op en el bloque
  e.mov_r_imm64(RAX, (u64)fn);
  e.call_reg(RAX);
  e.test_al_al();
  exitSite = e.je_rel32_placeholder();    // al==0 → salida de control (la op ya tuvo efecto)
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
        // NOTA: DIV/DIVU NO se JITean: idiv x86 lanza #DE en div-por-0 y en 0x80000000/-1, que en
        // VR4300 son casos DEFINIDOS (no-trap). Emularlos exigiría saltos-guardia en el código
        // generado; terminan el bloque → el intérprete los ejecuta con la semántica correcta.
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
static const int g_compFailOn = std::getenv("KESTREL_JIT_STATS") ? 1 : 0;

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
static const int g_jitTlbLink = std::getenv("KESTREL_JIT_NOTLBLINK") ? 0 : 1;
// Permitir que un enlace TLB apunte FUERA de la pagina de entrada del bloque. Cruzar
// pagina obliga a traducir el destino con una sonda del TLB en tiempo de compilacion, y
// esa traduccion es independiente de la que valida el driver para la entrada. Dentro de
// la pagina, en cambio, la phys del destino sale de la MISMA traduccion que ya trajo aqui.
static const int g_jitTlbXPage = std::getenv("KESTREL_JIT_TLBXPAGE") ? 1 : 0;
// DIAGNOSTICO: deja el ITC por ruta TLB pero desactiva el enlace ESTATICO TLB.
static const int g_noTlbStatic = std::getenv("KESTREL_JIT_NOTLBSTATIC") ? 1 : 0;
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
extern "C" u32 kestrel_jitProceedTramp(void* cpu, u32 K);
extern "C" u8 kestrel_jitIcRefill(void* cpu, u32 entry, u32 base);

extern const char* g_jitDump;   // KESTREL_JIT_DUMP (definido mas abajo)
static auto compileBlock(CPU& c, u32 phys) -> Block {
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
    usize fastToSlow[4] = {0,0,0,0}; usize fastToBody = 0; int nSlow = 0;
    if(fastOk) {
      e.mov_r32_m(RAX, RBX, guardOff);            // eax = ops permitidas
      e.alu32_imm(5, RAX, 0);                     // sub eax, K (placeholder)
      kSubAt = c.jitCache->buf.used - 4;
      fastToSlow[nSlow++] = e.jb_rel32_placeholder();   // sin margen → trampolín
      e.mov_r_imm64(RDX, (u64)&c.mem->rcp.mi_intr);
      e.mov_r32_m(RCX, RDX, 0);                   // ecx = MI_INTR
      e.and_r32_m(RCX, RDX, miMaskDelta);         // ecx &= MI_MASK
      fastToSlow[nSlow++] = e.jne_rel32_placeholder();  // interrupción del RCP pendiente
      e.cmp_m8_imm(RBX, timerOff, 0);             // latch Count==Compare ya disparado
      fastToSlow[nSlow++] = e.jne_rel32_placeholder();
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
      e.mov_r_imm64(RDX, kNoLink);                        // guarda (desactivada al nacer)
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
      constexpr s32 kEntSize = (s32)sizeof(CodeCache::ItcEnt);
      e.mov_r_imm64(RDX, (u64)(std::uintptr_t)c.jitCache->itc.data());
      e.mov_r_r(RAX, RCX);
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
      static_assert(kEntSize == 16, "el shl de arriba asume entradas de 16 bytes");
    }
    if(havePrev) e.patchRel32(prevJne);
    if(g_xStats) { e.mov_r_imm64(RDX, (u64)(std::uintptr_t)(nc == 0 ? &g_xSlowInd : &g_xSlowDir));
                   e.add_m32_imm32(RDX, 0, 1); }
    e.mov_r_imm32(RAX, 0x80000000u | nops);
    branchExits.push_back(e.jmp_rel32_placeholder());
  };

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
    usize site; bool isStore; RcSnap msnap;
    if(emitMemOp(e, rc, op, site, isStore, msnap, b.nOps, pendOff)) {
      bailSites.push_back(site); bailIdx.push_back(b.nOps); bailSnap.push_back(msnap); bailUndo.push_back(ilkUndo);
      b.hasMem = true; if(isStore) b.hasStore = true;
      b.src.push_back(op); b.nOps++; continue;
    }
    c.jitCache->buf.used = before;
    usize isite; RcSnap isnap;
    if(emitInterpOp(e, rc, op, 4 * i, isite, isnap)) {
      interpSites.push_back(isite); interpIdx.push_back(b.nOps + 1); interpSnap.push_back(isnap);
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
static u64 g_guardWhy[4] = {};   // 0=borde de timer 1=ventana de campo 2=regulador 3=tope
namespace jit { extern u64 g_compFailDelay[64], g_compFailDelaySpec[64]; }
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
    u64 now = mem->cartNow();
    siDue = mem->eventDueIn(now);
    // El fin de tarea del RCP en Threaded es otro plazo del mismo reloj: si el bloque se lo
    // traga, MI_SP / MI_DP caen en una instruccion distinta a la del interprete. Se mete en
    // el mismo cupo, que ya esta en unidades de cartNow().
    u64 rcpDue = mem->rcpDueIn(now);
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
      if(g >= kGuardMaxOps)   g_guardWhy[3]++;
      else if(g == slack)     g_guardWhy[0]++;
      else if(g == budg)      g_guardWhy[1]++;
    }
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
    if(w != b.src[(pa - entry) >> 2]) { b.dead = true; jitCache->unlinkTo(entry); return 0; }
  }
  u32 li = ((base - (entry & ~0x1fu)) >> 5);
  if(li < sizeof(b.lineSeq) / sizeof(b.lineSeq[0])) b.lineSeq[li] = l.seq;
  return 1;
}
extern "C" u8 kestrel_jitIcRefill(void* cpu, u32 entry, u32 base) {
  return reinterpret_cast<CPU*>(cpu)->jitIcRefill(entry, base);
}

extern "C" u32 kestrel_jitProceedTramp(void* cpu, u32 K) {
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

auto CPU::jitIdleSkip(u32 phys) -> u32 {
  if(jitPending) return 0;                       // el driver siempre entra con la cadena vacia
  if((usize)phys + 8 > mem->rdram.size()) return 0;
  // I-cache exacta: el salto no pasa por el fetch, asi que solo vale con las dos lineas ya
  // cargadas (todas las vueltas aciertan); si no, la primera vuelta la da el camino normal.
  if(jitIcExact()) {
    for(u32 pa : {phys, phys + 4}) { const ICacheLine& l = icache[(pa >> 5) & 0x1ff];
      if(!l.valid || l.ptag != (pa & ~0x1fu)) return 0; }
  }
  u32 w0 = jitFetchWord(phys), op = w0 >> 26;
  bool self = false;
  if(op == 0x04)                                 // BEQ rX,rX,-1
    self = ((w0 >> 21) & 31) == ((w0 >> 16) & 31) && (w0 & 0xffff) == 0xffff;
  else if(op == 0x01)                            // REGIMM BGEZ $0,-1
    self = ((w0 >> 21) & 31) == 0 && ((w0 >> 16) & 31) == 0x01 && (w0 & 0xffff) == 0xffff;
  if(!self) return 0;
  if(jitFetchWord(phys + 4) != 0u) return 0;     // ranura de retardo != NOP: el bucle hace algo
  // Con IE=0 o dentro de una excepcion ninguna interrupcion puede sacar de aqui: el invitado
  // esta colgado de verdad y saltarle el reloj lo esconderia en vez de arreglarlo. Que lo
  // ejecute el camino normal, que es donde estan los vigilantes.
  if(((u32)cop0[C0_Status] & 0x7) != 0x1) return 0;
  // Threaded SIN plazos (KESTREL_RCPDEADLINE=0) es el unico modo en el que un worker publica
  // MI_SP/MI_DP por su cuenta en tiempo de pared: ahi no hay plazo que acote el salto y la
  // interrupcion se veria hasta 4096 ops tarde. Se queda fuera.
  if(mem->rcpMode == Memory::RcpMode::Threaded && !Memory::rcpDeadlineOn()) return 0;

  // Limite = el permiso de una cadena, calculado igual que en jitReenterProceed.
  u32 cnt = (u32)cop0[C0_Count], cmp = (u32)cop0[C0_Compare];
  u64 lim = opsForTicks((u64)(u32)(cmp - cnt));
  u64 now = mem->cartNow();
  u64 due = mem->eventDueIn(now);
  { u64 r = mem->rcpDueIn(now); if(r < due) due = r; }
  if(due != ~0ull) { u64 d = opsForGuest(due); if(d < lim) lim = d; }
  if((u64)jitOpsBudget < lim) lim = jitOpsBudget;
  if(mem->rcpMode == Memory::RcpMode::Threaded) {
    u32 pa = mem->rcpPace(guestOps());
    if((u64)pa < lim) lim = pa;
  }
  if(lim > kGuardMaxOps) lim = kGuardMaxOps;
  u32 k = (u32)lim & ~1u;                        // iteraciones enteras: el pc no se mueve
  if(!k) return 0;

  retired += k;
  if(countAdd(countTicks(k))) timerIntr = true;
  cop0[C0_Random] = randomAdvance((u32)cop0[C0_Random], (u32)cop0[C0_Wired], k);
  if(jitCache) jitCache->hits += k;
  if(g_jitStats) { g_idleSkips++; g_idleOps += k; }
  return k;
}

auto CPU::jitTryBlock() -> u32 {
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
      std::fprintf(stderr, "[regcache] reg=%llu mem=%llu spill=%llu hit=%.1f%%\n",
                   (unsigned long long)jit::g_rcReg, (unsigned long long)jit::g_rcMem,
                   (unsigned long long)jit::g_rcSpill,
                   100.0 * jit::g_rcReg / (double)(jit::g_rcReg + jit::g_rcMem + 1));
      std::fprintf(stderr, "[permiso] timer=%llu campo=%llu pace=%llu tope=%llu\n",
                   (unsigned long long)g_guardWhy[0], (unsigned long long)g_guardWhy[1],
                   (unsigned long long)g_guardWhy[2], (unsigned long long)g_guardWhy[3]);
      std::fprintf(stderr, "[tramp] guard=%lluM mi=%lluM timer=%lluM rsp=%lluM otro=%lluM\n",
                   (unsigned long long)(g_trampWhy[0]/1000000), (unsigned long long)(g_trampWhy[1]/1000000),
                   (unsigned long long)(g_trampWhy[2]/1000000), (unsigned long long)(g_trampWhy[3]/1000000),
                   (unsigned long long)(g_trampWhy[4]/1000000));
      std::fprintf(stderr, "[retorno] salto=%llu completo=%llu corto=%llu\n",
                   (unsigned long long)jit::g_retBranch, (unsigned long long)jit::g_retFull,
                   (unsigned long long)jit::g_retShort);
      std::fprintf(stderr, "[salidas] enlace=%u itc=%u lentaDirecta=%u lentaIndirecta=%u\n",
                   jit::g_xLink, jit::g_xItc, jit::g_xSlowDir, jit::g_xSlowInd);
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
  if(bi >= 0 && cc->blocks[bi].dead) bi = -1;
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
    if(cc->buf.overflowed()) cc->clear();
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
  jitGuard = 0;                       // el primer bloque siempre pasa por el trampolín (chequeo completo)
  const u64 entryVAdbg = pc;          // solo para el chequeo de pc canonico de abajo
  // `memAbort` es un pestillo POR INSTRUCCION: lo pone translate() cuando el acceso falla y
  // significa "esta instruccion abort�". step() lo limpia al empezar cada instruccion; el
  // bloque compilado tambien tiene que hacerlo, porque puede entrar justo despues de que una
  // excepcion lo dejara puesto (p.ej. el prologo de un handler, al que se vectoriza con el
  // pestillo aun a 1). Sin esto, translate() cortocircuita con `return 0` y cualquier op del
  // bloque que no coja el camino directo (kernel de 64 bits: KX=1) traduce a fisico 0.
  memAbort = false;
  if(jit::g_jitDump) { blk.runs++; jit::g_dumpCache = cc; }
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
