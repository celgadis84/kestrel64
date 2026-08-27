#include "jit.hpp"
#include "cpu.hpp"
#include "../core/memory.hpp"
#include <cstdio>
#include <cstdlib>

#if defined(_WIN32)
  #include <windows.h>
#else
  #include <sys/mman.h>
  #include <unistd.h>
#endif

namespace kestrel::jit {

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
  if(bi >= 0 && !blocks[bi].dead && blocks[bi].linkEntry) {
    LinkSite& L = links[idx];
    *L.slot = (u64)(std::uintptr_t)blocks[bi].linkEntry;
    *L.vaImm = L.targetVA;
    nLinked++;
    anyLinked = true;
  }
}

auto CodeCache::linkTo(u32 phys, u8* entry) -> void {
  auto it = byTarget.find(phys);
  if(it == byTarget.end() || !entry) return;
  for(u32 i : it->second) {
    LinkSite& L = links[i];
    *L.slot = (u64)(std::uintptr_t)entry;
    *L.vaImm = L.targetVA;
    nLinked++;
    anyLinked = true;
  }
}

auto CodeCache::unlinkTo(u32 phys) -> void {
  auto it = byTarget.find(phys);
  if(it == byTarget.end()) return;
  for(u32 i : it->second) { *links[i].vaImm = kNoLink; nUnlinked++; }
}

auto CodeCache::unlinkAll() -> void {
  if(!anyLinked) return;      // ya está todo desenlazado: nada que recorrer (ver jit.hpp)
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
                      RcSnap& snap) -> bool {
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
    const s32 dbOff = (s32)offsetof(CPU, dcDbgOn);
    const s32 mmOff = (s32)offsetof(CPU, mem);
    const s32 rpOff = (s32)(offsetof(Memory, rcp) + offsetof(Rcp, mi_repeat_on));
    static_assert(sizeof(CPU::DCacheLine) == 24, "el x3<<3 de abajo asume lineas de 24 bytes");
    const s32 lnTag = (s32)offsetof(CPU::DCacheLine, ptag);
    const s32 lnVal = (s32)offsetof(CPU::DCacheLine, valid);
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
    e.mov_r_r(RAX, RDX);
    e.alu64_imm(5, RAX, 0x80000000u);                 // sub rax, sext(imm) == rax += 0x80000000
    e.cmp64_imm(RAX, 0x20000000u);
    fastFail[nFail++] = e.jae_rel32_placeholder();    // fuera de ckseg0 (o no canonica)
    if(fsz > 1) {
      e.test_al_imm8((u8)(fsz - 1));                  // desalineada: el interprete vectoriza
      fastFail[nFail++] = e.jne_rel32_placeholder();
    }
    e.test_m8_imm(RBX, stOff, 0x98);                  // KSU!=0 o KX: traduccion general
    fastFail[nFail++] = e.jne_rel32_placeholder();
    e.cmp_r32_m(RAX, RBX, szOff);                     // fisica fuera de RDRAM (o sin bus): MMIO
    fastFail[nFail++] = e.jae_rel32_placeholder();
    if(st) {
      // storeRepeat: con MI_MODE bit8 armado el siguiente store a RDRAM se difunde por la
      // pagina entera. mem no puede ser nulo aqui: con mem nulo jitRdramSz vale 0 y la
      // comprobacion de rango ya salio.
      e.mov_r_m(RCX, RBX, mmOff);
      e.cmp_m8_imm(RCX, rpOff, 0);
      fastFail[nFail++] = e.jne_rel32_placeholder();
      // Punto de vigilancia / write-through de diagnostico armado: al helper, que llama a
      // dcWriteDbg. El camino rapido no puede tragarse una escritura que el depurador espera.
      e.cmp_m8_imm(RBX, dbOff, 0);
      fastFail[nFail++] = e.jne_rel32_placeholder();
    }
    e.mov_r_r32(RCX, RAX);
    e.shift32_imm(5, RCX, 4);                         // shr ecx,4
    e.mov_r_r32(R8, RCX);
    e.shift32_imm(4, R8, 4);                          // shl r8d,4 = tag esperado (phys & ~0xf)
    e.alu32_imm(4, RCX, 0x1FFu);                      // indice de linea
    e.lea_x3(RCX, RCX);
    e.shift64_imm(4, RCX, 3);                         // idx*24
    e.alu64_rr(0x03, RCX, RBX);                       // rcx = &cpu->dcache[idx] - dcOff
    e.cmp_r32_m(R8, RCX, dcOff + lnTag);
    fastFail[nFail++] = e.jne_rel32_placeholder();
    e.cmp_m8_imm(RCX, dcOff + lnVal, 0);
    fastFail[nFail++] = e.je_rel32_placeholder();     // linea invalida: el helper la rellena
    // El sucio se marca AQUI, con rcx todavia en la base de la linea. Sumarle antes el
    // desplazamiento intra-linea escribiria la bandera dentro de data[], que es corrupcion
    // silenciosa del dato recien escrito. Costo una tarde.
    if(st && !(g_noFastMem & 4)) e.mov_m8_imm(RCX, dcOff + lnDrt, 1);
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
  e.mov_r_r(RCX, RBX);                    // arg0 = cpu (== &gpr[0] == RBX; R12 no fiable)
  e.mov_r_imm32(R8, rt);                  // arg2 = rt
  e.mov_r_imm64(RAX, (u64)fn);
  e.call_reg(RAX);
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
static u32 g_diffBad = 0;

// Que ALU-con-trampa absorbe el bloque (KESTREL_JIT_NOTRAPALU):
//   0 ADDI+ADD/SUB   1 ninguna   2 solo ADDI   3 solo ADD/SUB (DEFECTO)
//   4 ADDI+ADD/SUB sin trampa    6 solo ADDI sin trampa    7 solo ADDI y corta el bloque
// Los modos 4/6/7 son de diagnostico: emiten la aritmetica sin levantar el desbordamiento
// (semanticamente INCORRECTOS) para separar "la trampa esta mal" de "el bloque mas largo
// destapa otra cosa". El 7 ademas deja un bloque de una sola op, que es lo unico que el
// jitdiff sabe comparar contra el interprete.
//
// El defecto es 3 y no 0 a proposito: absorber ADDI destapa una corrupcion del contexto de
// excepcion en systemtest ("Privilege: memory accesses" -> tormenta) que solo aparece con
// J/JAL tambien absorbidos (KESTREL_JIT_BRSEL=8). Medido: la ADDI en si es correcta (el
// jitdiff sobre un bloque de una sola op no la pilla), la trampa es inocente (modo 6 falla
// igual) y el bail no llega a saltar nunca. El registro corrupto sale con dos palabras de 32
// bits pegadas ([sp|ra], [dir|dir]), que es la firma de un contexto guardado y restaurado con
// desfase. Sin resolver; ADD/SUB si esta verificado verde. Ver docs/baselines/jit-notes.md.
static const int g_trapAlu = std::getenv("KESTREL_JIT_NOTRAPALU")
                           ? (int)std::strtol(std::getenv("KESTREL_JIT_NOTRAPALU"), nullptr, 0) : 3;

// Biseccion por conteo (KESTREL_TRAPALU_LIM=N): solo las N primeras ALU-con-trampa que
// aparecen se absorben; a partir de ahi el bloque termina ahi como antes. La compilacion es
// determinista, asi que buscar N por biseccion senala exactamente cual rompe.
static const long g_trapAluLim = std::getenv("KESTREL_TRAPALU_LIM")
                               ? std::strtol(std::getenv("KESTREL_TRAPALU_LIM"), nullptr, 0) : -1;
static long g_trapAluN = 0;

static auto emitTrapAlu(Emitter& e, RegCache& rc, u32 op, usize& bailSite, RcSnap& snap) -> bool {
  if(g_trapAlu == 1) return false;
  if(g_trapAluLim >= 0 && g_trapAluN >= g_trapAluLim) return false;
  u32 OP = op >> 26, funct = op & 63;
  u32 rs = (op >> 21) & 31, rt = (op >> 16) & 31, rd = (op >> 11) & 31;
  s32 simm = (s32)(s16)(op & 0xFFFF);
  u32 dst;
  if(OP == 0x08 && g_trapAlu != 3) {                           // ADDI rt, rs, imm
    rc.ld32(RAX, rs);
    e.alu32_imm(0, RAX, (u32)simm);
    dst = rt;
  } else if(OP == 0x00 && (funct == 0x20 || funct == 0x22) && g_trapAlu != 2 && g_trapAlu != 6 && g_trapAlu != 7) {   // ADD / SUB
    rc.ld32(RAX, rs);
    rc.alu32(funct == 0x20 ? 0x03 : 0x2B, RAX, rt);   // ADD / SUB r32, r/m32
    dst = rd;
  } else {
    return false;
  }
  // g_trapAlu==4: DIAGNOSTICO. Emite la aritmetica sin la trampa (o sea, como si fuera la
  // variante U). Semanticamente INCORRECTO; solo sirve para separar "el bail esta mal" de
  // "el bloque mas largo destapa otra cosa".
  // modo 5: DIAGNOSTICO de tamano. Mismo jo real que el modo 2, pero con 3 bytes de relleno
  // delante para que el bloque mida exactamente lo mismo que en modo 4. Si el modo 5 pasa,
  // lo que separa 2 de 4 no es la trampa sino donde cae el codigo.
  // modo 6: MISMO conjunto que el 2 (solo ADDI) pero SIN trampa -> aisla si lo que rompe
  // es la trampa o el simple hecho de que el bloque empiece una instruccion antes.
  if(g_trapAlu == 4 || g_trapAlu == 6) { e.cmp_r_r(RAX, RAX); bailSite = e.jne_rel32_placeholder(); }  // nunca salta
  else                bailSite = e.jo_rel32_placeholder();   // desbordo → salir sin tocar destino
  snap = rc.snap();                      // lo sucio aqui lo escribe el stub de bail
  // El destino gpr[0] descarta el resultado, pero la trampa se levanta igual (el HW la mira
  // antes que el destino), asi que lo unico condicional es el store.
  if(dst) { e.movsxd(RAX, RAX); rc.st64(RAX, dst); }
  g_trapAluN++;
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
//   - LWL/LWR/SWL/SWR — memoria desalineada, que emitMemOp no cubre.
//   - SPECIAL DIV/DIVU/DMULT/DMULTU/DDIV/DDIVU — HI/LO, sin fault ni salto.
// Excluidos a propósito: COP0 (0x10, cambia TLB/Status → puede vectorizar), CACHE, LL/SC,
// SYSCALL/BREAK/TRAP y todo lo que salte. Si la op falla o vectoriza, el thunk devuelve 0 y
// el bloque sale con la bandera de control (pc/nextPc ya los dejó bien el intérprete).
static auto emitInterpOp(Emitter& e, RegCache& rc, u32 op, u32 off, usize& exitSite,
                         RcSnap& snap, bool delay = false) -> bool {
  u32 OP = op >> 26;
  bool ok = false;
  switch(OP) {
    case 0x11: ok = ((op >> 21) & 31) != 8; break;                   // COP1 salvo BC1x
    case 0x31: case 0x35: case 0x39: case 0x3d: ok = true; break;    // LWC1/LDC1/SWC1/SDC1
    case 0x22: case 0x26: case 0x2a: case 0x2e: ok = true; break;    // LWL/LWR/SWL/SWR
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
  //   LWC1/LDC1/SWC1/SDC1 leen gpr[base];  LWL/LWR/SWL/SWR leen base y rt (mezclan con el)
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
  //   LWL/LWR escriben gpr[rt];  SWL/SWR, LWC1/LDC1/SWC1/SDC1 no
  //   DIV/DIVU/DMULT/DMULTU/DDIV/DDIVU solo HI/LO, que no estan en el cache
  u32 wrGpr = 32;                                     // 32 = ninguno
  if(OP == 0x11 && ((op >> 21) & 31) <= 2)   wrGpr = (op >> 16) & 31;
  else if(OP == 0x22 || OP == 0x26)          wrGpr = (op >> 16) & 31;
  if(wrGpr < 32) rc.forget(wrGpr);
  return true;
}

// Emite UNA op. Devuelve false si no es segura (fin del bloque). op ya validado != code
// que cambie flujo. `c` solo se usa para leer palabras (icFetch) en el llamador.
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
          rc.ld32(RAX, rs); e.movsxd(RAX,RAX); rc.ld32(RCX, rt); e.movsxd(RCX,RCX);
          e.imul64(RAX,RCX);                                  // rax = (s32)rs * (s32)rt (64b)
          e.mov_r_r(RDX,RAX); e.movsxd(RDX,RDX); e.mov_m_r(RBX,loOff,RDX);   // lo = sext32(low32)
          e.shift64_imm(5,RAX,32); e.movsxd(RAX,RAX); e.mov_m_r(RBX,hiOff,RAX); // hi = sext32(high32)
          return true; }
        case 0x19: /*MULTU*/ {
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
// Histograma de la op que TERMINA el bloque (no la líder): dice qué falta por cubrir para
// alargar los bloques, que es la palanca directa sobre avgK.
u64 g_endOp[64] = {0}, g_endSpecial[64] = {0}, g_endRegimm[32] = {0};
u64 g_compFailSpecial[64] = {0};
u64 g_compFailRegimm[32] = {0};
// COP0/COP1 son las dos clases gordas del compile-fail, y el opcode primario no dice
// nada: MFC0 y MTC0 comparten OP10, y bajo OP11 conviven MFC1/CFC1/BC1 con la aritmetica
// que YA se compila. El sub-histograma va por el campo rs, que es quien las separa.
u64 g_compFailCop[2][32] = {{0}};
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
      // En THREADED la guarda no hace falta: el trampolin no la mira (corta por rcpMode) y
      // la regulacion CPU<->RSP viaja ahora dentro del propio permiso (jitGuard, ver
      // jitReenterProceed), que el camino rapido ya descuenta. Asi se paga una llamada cada
      // `allow` ops en vez de una por bloque mientras el RSP tenga trabajo.
      // KESTREL_JIT_RSPGUARD=1 la fuerza en los dos modos para poder medir el A/B.
      static const bool forceRspGuard = std::getenv("KESTREL_JIT_RSPGUARD") != nullptr;
      if(forceRspGuard || (c.mem && c.mem->rcpMode == Memory::RcpMode::Lockstep)) {
        e.mov_r_imm64(RDX, (u64)&c.mem->rsp.running);
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
  struct Pending { usize dispAt; usize immAt; u64 targetVA; u32 targetPhys; };
  std::vector<Pending> pending;
  const u64 entryVA = c.pc;
  const bool ck0Entry = ((entryVA & 0xFFFF'FFFF'E000'0000ull) == 0xFFFF'FFFF'8000'0000ull)
                        && (((u32)entryVA & 0x1FFF'FFFFu) == phys);
  // ¿Es `va` un destino enlazable? (ckseg0 ⇒ phys arquitectónica y cacheable, dentro de RDRAM)
  auto linkable = [&](u64 va, u32& outPhys) -> bool {
    if(!g_jitLink || g_jitDiffAny || !ck0Entry) return false;
    if((va & 0xFFFF'FFFF'E000'0000ull) != 0xFFFF'FFFF'8000'0000ull) return false;
    if(va & 3) return false;
    u32 p = (u32)va & 0x1FFF'FFFFu;
    if((usize)p + 4 > c.mem->rdram.size()) return false;
    outPhys = p; return true;
  };

  // Compila el delay slot (ALU o mem). Si es mem, registra su bail con índice = el del
  // salto (bailIdx=idx): al faultar, el intérprete re-ejecuta desde el salto. Devuelve false
  // si el delay slot no es compilable (→ se descarta la absorción del salto completo).
  auto compileDelay = [&](u32 dop, u32 idx) -> bool {
    usize dBefore = c.jitCache->buf.used;
    if(emitSafeOp(e, rc, dop)) return true;
    c.jitCache->buf.used = dBefore;
    usize dsite; bool dStore; RcSnap dsnap;
    if(emitMemOp(e, rc, dop, dsite, dStore, dsnap)) {
      bailSites.push_back(dsite); bailIdx.push_back(idx); bailSnap.push_back(dsnap);
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
    c.jitCache->buf.used = dBefore;
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
      u32 tp;
      if(!linkable(cands[k], tp)) continue;
      if(havePrev) { e.patchRel32(prevJne); havePrev = false; }
      e.mov_r_imm64(RDX, kNoLink);                        // guarda (desactivada al nacer)
      usize immAt = c.jitCache->buf.used - 8;
      e.cmp_r_r(RCX, RDX);
      prevJne = e.jne_rel32_placeholder(); havePrev = true;
      // Enlazado: acumula las ops de ESTE bloque en jitPending (el prólogo del sucesor las
      // commitea) y salta a su punto de entrada sin pasar por el driver.
      e.add_m32_imm32(RBX, pendOff, nops);
      usize dispAt = e.jmp_rip_mem_placeholder();
      pending.push_back(Pending{ dispAt, immAt, cands[k], tp });
    }
    if(havePrev) e.patchRel32(prevJne);
    e.mov_r_imm32(RAX, 0x80000000u | nops);
    branchExits.push_back(e.jmp_rel32_placeholder());
  };

  for(u32 i = 0; i < kMaxOps; i++) {
    u32 a = phys + 4 * i;
    if(a + 4 > c.mem->rdram.size()) break;
    // No cruzar la página 4K de entrada: en código TLB-mapeado los VA contiguos NO son
    // phys contiguos al cambiar de página, así que phys+4*i dejaría de corresponder a la
    // instrucción real. El bloque termina en el borde de página (se recompila en la sig.).
    if((a & ~0xFFFu) != (phys & ~0xFFFu)) break;
    u32 op = c.jitFetchWord(a);
    usize before = c.jitCache->buf.used;
    if(emitSafeOp(e, rc, op)) { b.src.push_back(op); b.nOps++; continue; }
    c.jitCache->buf.used = before;
    usize tsite; RcSnap tsnap;
    if(emitTrapAlu(e, rc, op, tsite, tsnap)) {
      bailSites.push_back(tsite); bailIdx.push_back(b.nOps); bailSnap.push_back(tsnap);
      if(g_trapAlu != 4 && g_trapAlu != 6 && g_trapAlu != 7) b.hasTrap = true;   // modo 4 no trapea -> comparable con el diff
      b.src.push_back(op); b.nOps++;
      // modo 7: cortar el bloque justo tras la ALU absorbida. Deja un bloque de nOps cortas
      // sin memoria ni salto, que es lo unico que el jitdiff sabe comparar contra el interprete.
      if(g_trapAlu == 7) break;
      continue;
    }
    c.jitCache->buf.used = before;
    usize site; bool isStore; RcSnap msnap;
    if(emitMemOp(e, rc, op, site, isStore, msnap)) {
      bailSites.push_back(site); bailIdx.push_back(b.nOps); bailSnap.push_back(msnap);
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
    // Salto/branch-en-bloque: absorber el salto + su delay slot y cerrar el bloque
    // escribiendo el control de flujo (pc/nextPc) directamente. Es la palanca de cobertura:
    // el bucle caliente de PD está dominado por BEQ + llamadas (JAL) + returns (JR $ra), y
    // cortar en cada uno dejaba bloques cortos. cpu == &gpr[0] == RBX (ver static_assert),
    // así el estado de control se direcciona vía RBX (R12 no fiable tras el CALL de mem-op).
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
                     ((ad & ~0xFFFu) == (phys & ~0xFFFu));
      if(delayOk) {
        u32 dop = c.jitFetchWord(ad);
        u32 rs = (op >> 21) & 31, rt = (op >> 16) & 31, rd = (op >> 11) & 31;
        s32 simm = (s32)(s16)(op & 0xFFFF);
        u32 idx = b.nOps;                          // rectas antes del salto (= i)
        // A partir de aqui manda la maquinaria de branch: dos caminos de salida, delay slot
        // que puede emitirse dos veces y rollback del bloque entero si el delay no compila.
        // Volcar y apagar la residencia deja todos esos caminos con cpu->gpr coherente, y el
        // volcado queda FUERA del rango de rollback (antes de beforeBranch) a proposito.
        rc.disable();
        usize beforeBranch = c.jitCache->buf.used; // por si el delay no compila
        // Un rollback deja el codigo descartado pero los sitios de bail ya registrados
        // seguirian apuntando dentro de esa zona, que otra op reescribira despues. Se recortan
        // con el buffer. Hoy solo BC1 registra uno en fase A; vale para el que venga.
        usize nBailBefore = bailSites.size();
        auto rollbackBranch = [&]() {
          c.jitCache->buf.used = beforeBranch;
          bailSites.resize(nBailBefore); bailIdx.resize(nBailBefore); bailSnap.resize(nBailBefore);
        };
        // Fase A (antes del delay slot): capturar la condición/target/enlace que el delay
        // slot podría pisar (el delay puede escribir gpr[rs]/gpr[rt] o hacer CALL).
        if(isBeq || isBeqL) {
          e.ld64(RAX, rs); e.cmp64_rm(RAX, rt);
          e.setcc((LO == 0x04 || LO == 0x14) ? 0x94 : 0x95, RAX);  // sete/setne al → [rsp+32]
          e.st8_rsp(32);
        } else if(isBcondZ || isBlezL || isBgtzL || isRegimmL) {
          e.ld64(RAX, rs); e.cmp64_imm(RAX, 0);    // rs vs 0 (signed 64b; OF=0 → setl/ge/le/g ok)
          e.setcc(ccz, RAX);                       // condición → al → [rsp+32]
          e.st8_rsp(32);
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
          bailIdx.push_back(idx); bailSnap.push_back(rc.snap());
          // COND es el bit 23 de FCR31 -> byte 2, bit 7. ZF=1 significa COND=0, asi que
          // "tomar" es setne cuando TF=1 y sete cuando TF=0.
          e.test_m8_imm(RBX, (s32)offsetof(CPU, fcr31) + 2, 0x80);
          e.setcc(bc1Tf ? 0x95 : 0x94, RAX);
          e.st8_rsp(32);
        } else if(isJr) {
          e.ld64(RAX, rs); e.st64_rsp(RAX, 32);    // target = gpr[rs] (pre-delay) → [rsp+32]
          if(FN == 0x09) emitLink(idx, rd ? rd : 31);   // JALR enlaza tras leer rs (rd puede==rs)
        } else if(LO == 0x03) {
          emitLink(idx, 31);                       // JAL enlaza gpr[31]
        }
        // Fase B/C de las "likely": el delay slot se emite DENTRO del camino tomado, porque
        // cuando no se toma queda anulado. Las dos salidas son enlazables y cuentan distinto:
        // tomada retira idx+2 ops (rectas + salto + delay), no tomada idx+1 (pc += 8 de una).
        if(isLikely) {
          e.ld8_rsp(32); e.test_al_al();
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
            e.ld8_rsp(32); e.test_al_al();
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
            e.mov_r_m(RDX, RBX, pcOff);     // rdx = entryVA
            e.mov_r_r(RCX, RDX);
            e.add_r_imm32(RDX, Ctaken);     // rdx = target tomado
            e.add_r_imm32(RCX, Cfall);      // rcx = fallthrough
            e.ld8_rsp(32); e.test_al_al();
            e.cmovnz(RCX, RDX);             // cond!=0 → rcx = target
            cands[nc++] = entryVA + (u64)(s64)Ctaken;   // tomado (el caliente: bucles)
            cands[nc++] = entryVA + (u64)(s64)Cfall;    // caída
          } else if(isJr) {
            e.ld64_rsp(RCX, 32);            // rcx = target de gpr[rs] (pre-delay)
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
    }
    c.jitCache->buf.used = (usize)(entry - c.jitCache->buf.base); return b;
  }

  // Salida normal: eax = nOps (todas retiradas); salta al epílogo común.
  // Si el bloque terminó absorbiendo un branch, éste ya emitió su propia salida de
  // control (con pc/nextPc escritos y flag 0x80000000) → no hay caída secuencial.
  usize toDoneMain = 0; bool haveMain = false;
  if(!endedInBranch) {
    rc.writeback();                          // el driver lee cpu->gpr tras el bloque
    e.mov_r_imm32(RAX, b.nOps);
    toDoneMain = e.jmp_rel32_placeholder();
    haveMain = true;
  }
  // Stubs de bail: cada je de mem-op aterriza aquí → eax = índice (ops retiradas) y al epílogo.
  std::vector<usize> toDone;
  for(usize k = 0; k < bailSites.size(); k++) {
    e.patchRel32(bailSites[k]);
    rc.emitSnapSpill(bailSnap[k]);           // spill perezoso: lo sucio en el punto del bail
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
    b.sites.push_back(s);
  }

  if(c.jitCache->buf.overflowed()) { b.nOps = 0; b.src.clear(); b.sites.clear(); return b; }
  c.jitCache->buf.finalize(entry);
  b.fn = reinterpret_cast<BlockFn>(entry);
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
extern u64 g_trampWhy[5];
namespace jit { extern u64 g_compFailOp[64], g_compFailSpecial[64], g_compFailRegimm[32];
                extern u64 g_endOp[64], g_endSpecial[64], g_endRegimm[32];
                extern u64 g_compFailCop[2][32]; }
namespace jit { extern u64 g_rcReg, g_rcMem, g_rcSpill; }
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
    cop0[C0_Count] = (u32)((u32)cop0[C0_Count] + p);
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
  if((u32)(cmp - cnt) <= K) return 0;                                // cruzaría borde de timer
  // Permiso para el camino rápido del prólogo: cuántas ops MÁS puede encadenar la cadena sin
  // volver a preguntar. Lo acota lo mismo que acaba de comprobarse aquí — el borde de timer
  // (determinista: Count avanza 1 por op) y lo que queda de ventana del bucle del sistema —
  // menos las K de ESTE bloque, que aún no están commiteadas. Lo demás que mira el trampolín o
  // no puede cambiar dentro de una cadena (Status/Cause por mtc0, halted: terminan bloque) o lo
  // re-comprueba el propio prólogo en línea (MI, latch de timer).
  {
    u32 slack = (u32)(cmp - cnt) - K - 1;                 // > 0 garantizado por la línea de arriba
    u32 budg  = (K >= jitOpsBudget) ? 0 : (jitOpsBudget - K);
    u32 g     = slack < budg ? slack : budg;
    // Regulador Threaded: duerme si la CPU emulada adelanta al RSP en vuelo y mete lo que
    // le queda de adelanto en el permiso. Asi el camino rapido del prologo lo descuenta solo
    // y vuelve aqui justo cuando toca frenar otra vez — misma regulacion que la guarda
    // rsp.running, pero sin una llamada por bloque.
    if(mem && mem->rcpMode == Memory::RcpMode::Threaded) {
      mem->rcpPace(retired);
      u32 pa = mem->paceAllowance(retired);
      if(pa < g) g = pa;
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
extern "C" u32 kestrel_jitProceedTramp(void* cpu, u32 K) {
  CPU* c = reinterpret_cast<CPU*>(cpu);
  if(g_jitStats) {
    if(c->jitGuard < K)                                          g_trampWhy[0]++;
    else if(c->mem && (c->mem->rcp.mi_intr & c->mem->rcp.mi_mask)) g_trampWhy[1]++;
    else if(c->timerIntr)                                        g_trampWhy[2]++;
    else if(c->mem && c->mem->rsp.running)                       g_trampWhy[3]++;
    else                                                         g_trampWhy[4]++;
  }
  return c->jitReenterProceed(K);
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
  if(g_jitStats) {
    g_jitCalls++;
    if((g_jitCalls & 0x3FFFFF) == 0) {
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
      std::fprintf(stderr, "[tramp] guard=%lluM mi=%lluM timer=%lluM rsp=%lluM otro=%lluM\n",
                   (unsigned long long)(g_trampWhy[0]/1000000), (unsigned long long)(g_trampWhy[1]/1000000),
                   (unsigned long long)(g_trampWhy[2]/1000000), (unsigned long long)(g_trampWhy[3]/1000000),
                   (unsigned long long)(g_trampWhy[4]/1000000));
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
      for(int f = 0; f < 64; f++) if(jit::g_compFailSpecial[f] > 10000)
        std::fprintf(stderr, " SP%02x=%llu", f, (unsigned long long)jit::g_compFailSpecial[f]);
      for(int r = 0; r < 32; r++) if(jit::g_compFailRegimm[r] > 10000)
        std::fprintf(stderr, " RI%02x=%llu", r, (unsigned long long)jit::g_compFailRegimm[r]);
      std::fprintf(stderr, "\n[compfailcop]");
      for(int k = 0; k < 2; k++) for(int r = 0; r < 32; r++) if(jit::g_compFailCop[k][r] > 10000)
        std::fprintf(stderr, " COP%d.rs%02x=%llu", k, r, (unsigned long long)jit::g_compFailCop[k][r]);
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
  if((pc & 0xFFFF'FFFF'E000'0000ull) == 0xFFFF'FFFF'8000'0000ull) {
    // ckseg0 solo existe en modo kernel. En usuario o supervisor esa direccion no esta
    // traducida: el VR4300 levanta AdEL en el propio fetch. Atajar aqui a phys = pc & 0x1FFFFFFF
    // se saltaba esa comprobacion y el bloque se ejecutaba igual, asi que el kernel nunca veia
    // la excepcion que el programa esperaba. Modo kernel = KSU==0 (Status[4:3]) o EXL o ERL.
    u32 st = (u32)cop0[C0_Status];
    if(((st >> 3) & 3) != 0 && !(st & 0x6)) { JDECL(DR_MISC); return 0; }  // interprete vectoriza el AdEL
    phys = (u32)pc & 0x1FFF'FFFF;
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

  if(!jitCache) {
    auto* cc = new jit::CodeCache();
    if(!cc->init()) { delete cc; return 0; }
    jitCache = cc;
  }
  jit::CodeCache* cc = jitCache;

  // Cache negativa: este PC ya falló al compilar y su op líder no ha cambiado → intérprete
  // directo, sin volver a emitir. Un fallo de compilación depende SOLO de la palabra líder
  // (compileBlock corta ahí y devuelve nOps==0), así que validarla basta y cuesta un icFetch
  // frente a una compilación entera.
  jit::CodeCache::NoComp& nc = cc->noComp[(phys >> 2) & (jit::CodeCache::kNoCompSlots - 1)];
  if(nc.phys == phys) {
    if(jitFetchWord(phys) == nc.word) {
      if(jit::g_compFailOn) { u32 LO = nc.word >> 26; jit::g_compFailOp[LO]++;
        if(LO == 0) jit::g_compFailSpecial[nc.word & 63]++;
        else if(LO == 1) jit::g_compFailRegimm[(nc.word >> 16) & 31]++;
        else if(LO == 0x10 || LO == 0x11) jit::g_compFailCop[LO - 0x10][(nc.word >> 21) & 31]++; }
      JDECL(DR_COMPILE); return 0; }
    nc.phys = ~0u;                              // el código cambió bajo el PC → reintentar
  }

  s32 bi = cc->find(phys);
  if(bi >= 0 && cc->blocks[bi].dead) bi = -1;   // Step2: bloque invalidado por SMC → recompila (insert lo sobrescribe in-place)
  if(bi < 0) {
    // Reclamo de buffer: si el buf ejecutable desbordó (fugas por dead-mark en SMC pesado),
    // clear global recupera memoria antes de recompilar. Sin esto el JIT quedaría muerto.
    if(cc->buf.overflowed()) cc->clear();
    jit::Block b = jit::compileBlock(*this, phys);
    if(b.nOps == 0) { nc.phys = phys; nc.word = jitFetchWord(phys); JDECL(DR_COMPILE); return 0; }
    bi = cc->insert(phys, std::move(b));
    if(bi < 0) { cc->clear(); jit::Block b2 = jit::compileBlock(*this, phys); if(b2.nOps==0) return 0; bi = cc->insert(phys, std::move(b2)); if(bi < 0) return 0; }
    // Block-linking Step 3: registra los sitios de enlace que este bloque emitió (se resuelven
    // solos si el destino ya está compilado) y publica el bloque como destino, activando los
    // sitios que ya lo esperaban — incluidos los suyos propios, que es el caso de un bucle
    // auto-enlazado. Sólo aquí, tras insert(), es encontrable por find().
    {
      jit::Block& nb = cc->blocks[bi];
      for(const jit::LinkSite& s : nb.sites) cc->addLink(s);
      cc->linkTo(phys, nb.linkEntry);
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
    u32 d = cmp - cnt;                 // pasos hasta Count==Compare (mod 2^32)
    if(d <= K) { JDECL(DR_TIMER); return 0; }  // el intérprete maneja el borde del timer exacto
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
  if(!noSmc)
  for(u32 base = phys & ~0x1fu, li = 0; base < endPa; base += 32, li++) {
    u32 idx = (base >> 5) & 0x1ff;
    ICacheLine& l = icache[idx];
    if(!l.valid || l.ptag != base) icFill(idx, base);
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
    cc->linkTo(phys, blk.linkEntry);
    blk.linkedEpoch = cc->linkEpoch;
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
    u32 sCnt = (u32)cop0[C0_Count], sRnd = (u32)cop0[C0_Random];
    u32 Rd = blk.fn(gpr, this) & 0x7FFF'FFFFu; gpr[0] = 0;
    u64 tmp[32]; for(int r = 0; r < 32; r++) tmp[r] = gpr[r];
    // restaurar: a partir de aqui el estado del invitado es como si el bloque no hubiera corrido
    for(int r = 0; r < 32; r++) gpr[r] = pre[r];
    hi = sHi; lo = sLo; pc = sPc; nextPc = sNext; inDelay = sIn; justBranched = sJb;
    cop0[C0_Count] = sCnt; cop0[C0_Random] = sRnd;
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
  if(brdiff && blk.hasBranch && !blk.hasMem && !blk.hasTrap) {
    u64 sg[32]; for(int r = 0; r < 32; r++) sg[r] = gpr[r];
    u64 sPc = pc, sNext = nextPc; bool sIn = inDelay, sJb = justBranched;
    u32 sCnt = (u32)cop0[C0_Count], sRnd = (u32)cop0[C0_Random];
    // intérprete K pasos → referencia
    for(u32 s = 0; s < K; s++) step();
    u64 iG[32]; for(int r = 0; r < 32; r++) iG[r] = gpr[r];
    u64 iPc = pc, iNext = nextPc;
    bool iIn = inDelay, iJb = justBranched;
    u32 iCnt = (u32)cop0[C0_Count], iRnd = (u32)cop0[C0_Random];
    // restaura y corre el bloque
    for(int r = 0; r < 32; r++) gpr[r] = sg[r];
    pc = sPc; nextPc = sNext; inDelay = sIn; justBranched = sJb;
    cop0[C0_Count] = sCnt; cop0[C0_Random] = sRnd;
    u32 Rr = blk.fn(gpr, this); gpr[0] = 0;
    u32 Rops = Rr & 0x7FFF'FFFFu; bool isCtrl = (Rr & 0x8000'0000u) != 0;
    if(!isCtrl) { pc = sPc + 4 * Rops; nextPc = pc + 4; }  // bail: avance secuencial
    // Mismo caso que arriba: guardia no pasada -> el bloque no ha corrido, no hay que juzgarlo.
    if(!isCtrl && Rops != K) {
      for(int r = 0; r < 32; r++) gpr[r] = iG[r];
      pc = iPc; nextPc = iNext; inDelay = iIn; justBranched = iJb;
      cop0[C0_Count] = iCnt; cop0[C0_Random] = iRnd;
      return K;                                  // manda el interprete, ya avanzado arriba
    }
    bool bad = (pc != iPc) || (nextPc != iNext);
    for(int r = 1; r < 32; r++) if(gpr[r] != iG[r]) bad = true;
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
    retired += Rops; cop0[C0_Count] = (u32)(sCnt + Rops);
    cc->hits += Rops; return Rops;
  }

  // Ejecuta el bloque. Devuelve R = ops REALMENTE retiradas: R==K en éxito total, o el índice
  // de la primera mem-op que faultaría (bail limpio, sin efectos). El intérprete re-ejecuta la
  // op R para vectorizar la excepción exacta, así que aquí solo avanzamos el estado por R.
  jitChain = 0;                       // presupuesto de cadena fresco por entrada del driver
  jitChainOps = 0;                    // ops que la cadena commitee por su cuenta (las sumamos al salir)
  jitGuard = 0;                       // el primer bloque siempre pasa por el trampolín (chequeo completo)
  const u64 entryVAdbg = pc;          // solo para el chequeo de pc canonico de abajo
  u32 Rraw = blk.fn(gpr, this);
  gpr[0] = 0;
  // Bit alto = el bloque terminó en un branch absorbido: ya escribió pc/nextPc/inDelay/
  // justBranched por sí mismo. Sólo avanzamos contadores; NO tocamos el control de flujo.
  bool ctrl = (Rraw & 0x8000'0000u) != 0;
  u32 R = Rraw & 0x7FFF'FFFFu;
  // DIAGNOSTICO (KESTREL_JIT_PCCHK): en modo de 32 bits toda direccion virtual valida es la
  // extension de signo de sus 32 bits bajos. Un pc con basura arriba solo puede venir de una
  // salida de control que compuso mal el destino, y saltarlo hace que la excepcion aparezca
  // muy lejos del bloque culpable. Aqui se caza en el acto, con el bloque delante.
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
  cop0[C0_Count] = (u32)((u32)cop0[C0_Count] + R);   // sin latch (garantizado: d>K≥R arriba)

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
    cop0[C0_Count]  = (u32)((u32)cop0[C0_Count] + p);
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
