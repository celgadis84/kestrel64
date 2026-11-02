#include "jit.hpp"
#include "cpu.hpp"
#include "../core/memory.hpp"
#include "jit_internal.hpp"
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
const int g_xStats = std::getenv("KESTREL_JIT_STATS") ? 1 : 0;

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
using SmokeFn = u64 (KJIT_ABI *)(u64*);

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
extern "C" KJIT_ABI u8 jitTestHelper(u64* ctx, u32 arg) {
  ctx[1] = arg;
  return ctx[0] ? 0u : 1u;   // 0 = faulted (bail), 1 = ok (sigue)
}

using CallBailFn = u32 (KJIT_ABI *)(u64* gpr, u64* ctx);

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
  // Tamano de la cache de codigo. Con 16 MB y 65536 ranuras el codigo de juego de Perfect Dark
  // (pagina por TLB desde 0x7F000000, ~35 k bloques) no cabia: el buffer se desbordaba cada
  // ~1,5 s, clear() tiraba TODO y la CPU recompilaba el conjunto de trabajo entero en bucle
  // (248 k compilaciones en 25 s, ~10 % del hilo de CPU en el emisor). 64 MB y 2^17 ranuras
  // lo dejan en una compilacion por bloque. KESTREL_JIT_BUFMB / KESTREL_JIT_SLOTBITS para medir.
  usize mb = 64; u32 cb = 17;
  if(const char* e = std::getenv("KESTREL_JIT_BUFMB")) mb = (usize)std::strtoul(e, nullptr, 0);
  if(const char* e = std::getenv("KESTREL_JIT_SLOTBITS")) cb = (u32)std::strtoul(e, nullptr, 0);
  if(mb < 1 || mb > 1024) mb = 64;
  if(cb < 12 || cb > 22) cb = 17;
  if(!buf.init(mb * 1024 * 1024)) return false;
  u32 cap = 1u << cb;
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

}  // namespace kestrel::jit
