#include "rspjit.hpp"
#include "rsp.hpp"
#include <cstdio>
#include <cstring>

namespace kestrel::rspjit {

// --- trampolines a los helpers del interprete --------------------------------
// El codigo compilado NO reimplementa ninguna semantica: para todo lo que no sea ALU
// entera pura llama a la MISMA funcion que ejecutaria el interprete. Por eso el JIT no
// puede divergir del oraculo -- comparte el cuerpo con el.
extern "C" {
  void kestrel_rspjit_cop2 (Rsp* r, u32 op) { r->jitCop2(op); }
  void kestrel_rspjit_load (Rsp* r, u32 op) { r->jitLoad(op); }
  void kestrel_rspjit_store(Rsp* r, u32 op) { r->jitStore(op); }
  void kestrel_rspjit_exec (Rsp* r, u32 op) { r->jitExec(op); }
}

auto Cache::syncImem(const u8* imem) -> void {
  // Diferencia por palabras de 64 bits (dos ranuras cada una). Lo normal con diferencia es
  // que no cambie NADA -- una tarea que recarga su propio microcodigo -- y ese caso sale por
  // aqui tras 512 comparaciones, sin tocar la tabla.
  u32 nDirty = 0;
  bool dirty[512];
  for(u32 i = 0; i < 512; i++) {
    u64 a, b;
    std::memcpy(&a, imem + 8 * i, 8);
    std::memcpy(&b, shadow + 8 * i, 8);
    dirty[i] = (a != b);
    nDirty += dirty[i];
  }
  if(!nDirty) return;
  flushes++;
  {
    // Invalidacion EXACTA: una ranura muere solo si el bloque que hay en ella cubre de
    // verdad una palabra cambiada. La version anterior mataba las kMaxOps-1 ranuras
    // anteriores a cada palabra sucia "por si acaso" -- 65 ranuras por chunk de 8 bytes --
    // y con unos pocos chunks dispersos eso barria media tabla: medidos 72 bloques
    // recompilados por evento de invalidacion en SM64. Con la cobertura real (los bloques
    // no envuelven IMEM, asi que ocupan [idx, idx+nOps) sin dar la vuelta) basta con una
    // suma de prefijos sobre el mapa de palabras sucias y una pasada por las 1024 ranuras.
    u16 pre[1025];
    pre[0] = 0;
    for(u32 w = 0; w < 1024; w++) pre[w + 1] = (u16)(pre[w] + (dirty[w >> 1] ? 1 : 0));
    for(u32 idx = 0; idx < 1024; idx++) {
      if(state[idx] == State::Unknown) continue;
      // Un NoComp depende de todo lo que miro el escaneo, que llega hasta kMaxOps palabras.
      u32 n = (state[idx] == State::Compiled) ? blocks[idx].nOps : kMaxOps;
      u32 end = idx + n; if(end > 1024) end = 1024;
      if(pre[end] == pre[idx]) continue;                    // ninguna palabra suya cambio
      state[idx] = State::Unknown;
      blocks[idx] = Block{};
    }
  }
  std::memcpy(shadow, imem, 4096);
}

auto Cache::diffChunks(const u8* imem) const -> u32 {
  u32 n = 0;
  for(u32 i = 0; i < 512; i++) {
    u64 a, b;
    std::memcpy(&a, imem + 8 * i, 8);
    std::memcpy(&b, shadow + 8 * i, 8);
    n += (a != b);
  }
  return n;
}

auto Cache::init(u32 bytes) -> bool {
  // RWX del tamano que pida el llamante: 1024 entradas x hasta 64 instrucciones x ~80 bytes
  // por instruccion es el peor caso teorico (~5 MB, lo marca emitMem), pero el microcodigo
  // real ocupa una fraccion y ahora hay VARIAS tablas vivas (una por imagen de IMEM). Si una
  // se llena, clear() la recicla entera.
  if(!buf.init(bytes)) return false;
  clear();
  ready = true;
  return true;
}

auto Cache::clear() -> void {
  buf.reset();
  for(u32 i = 0; i < 1024; i++) { blocks[i] = Block{}; state[i] = State::Unknown; }
  flushes++;
}

// --- emisor x86-64 minimo ----------------------------------------------------
// Solo lo que hace falta para el subconjunto entero del RSP. Todos los registros que se
// usan estan en 0-7, asi que ninguna instruccion de 32 bits necesita REX; las de 64 bits
// (punteros) lo llevan explicito.
namespace {

enum : u8 { rAX = 0, rCX = 1, rDX = 2, rBX = 3, rSPx = 4, rBP = 5, rSI = 6, rDI = 7 };

struct E {
  jit::CodeBuffer& b;
  explicit E(jit::CodeBuffer& buf) : b(buf) {}

  auto u8_(u8 v) -> void { b.emit(v); }
  auto u32_(u32 v) -> void { for(int i = 0; i < 4; i++) b.emit((u8)(v >> (8 * i))); }
  auto u64_(u64 v) -> void { for(int i = 0; i < 8; i++) b.emit((u8)(v >> (8 * i))); }
  auto modrm(u8 mod, u8 reg, u8 rm) -> void { b.emit((u8)((mod << 6) | ((reg & 7) << 3) | (rm & 7))); }

  // [base+disp]. Aqui base siempre es RBX o RAX, nunca RSP/RBP, asi que no hace falta SIB.
  auto mem(u8 reg, u8 base, s32 disp) -> void {
    if(disp == 0 && base != rBP)          modrm(0, reg, base);
    else if(disp >= -128 && disp <= 127) { modrm(1, reg, base); u8_((u8)(s8)disp); }
    else                                 { modrm(2, reg, base); u32_((u32)disp); }
  }

  // --- 32 bits ---
  auto ld32(u8 dst, u8 base, s32 d)  -> void { u8_(0x8B); mem(dst, base, d); }   // mov r32,[b+d]
  auto st32(u8 src, u8 base, s32 d)  -> void { u8_(0x89); mem(src, base, d); }   // mov [b+d],r32
  auto alu_rm(u8 opc, u8 dst, u8 base, s32 d) -> void { u8_(opc); mem(dst, base, d); }
  auto alu_imm(u8 digit, u8 dst, u32 imm) -> void { u8_(0x81); modrm(3, digit, dst); u32_(imm); }
  auto shift_imm(u8 digit, u8 dst, u8 sa) -> void { u8_(0xC1); modrm(3, digit, dst); u8_(sa); }
  auto shift_cl(u8 digit, u8 dst) -> void { u8_(0xD3); modrm(3, digit, dst); }
  auto not32(u8 dst) -> void { u8_(0xF7); modrm(3, 2, dst); }
  auto neg32(u8 dst) -> void { u8_(0xF7); modrm(3, 3, dst); }
  auto mov_imm32(u8 dst, u32 imm) -> void { u8_((u8)(0xB8 + dst)); u32_(imm); }
  auto setcc(u8 cc, u8 dst) -> void { u8_(0x0F); u8_(cc); modrm(3, 0, dst); }
  auto movzx8(u8 dst, u8 src) -> void { u8_(0x0F); u8_(0xB6); modrm(3, dst, src); }
  auto movsx8_m(u8 dst, u8 base, s32 d) -> void { u8_(0x0F); u8_(0xBE); mem(dst, base, d); }
  auto movzx8_m(u8 dst, u8 base, s32 d) -> void { u8_(0x0F); u8_(0xB6); mem(dst, base, d); }
  auto st8(u8 src, u8 base, s32 d) -> void { u8_(0x88); mem(src, base, d); }
  auto ld16z(u8 dst, u8 base, s32 d) -> void { u8_(0x0F); u8_(0xB7); mem(dst, base, d); }
  auto st16(u8 src, u8 base, s32 d) -> void { u8_(0x66); u8_(0x89); mem(src, base, d); }
  auto rol16(u8 r, u8 n) -> void { u8_(0x66); u8_(0xC1); modrm(3, 0, r); u8_(n); }
  auto movsx16(u8 dst, u8 src) -> void { u8_(0x0F); u8_(0xBF); modrm(3, dst, src); }
  auto bswap(u8 r) -> void { u8_(0x0F); u8_((u8)(0xC8 + r)); }

  // Saltos cortos hacia adelante con hueco a rellenar. Devuelven el origen del rel8 (la
  // posicion siguiente al byte de desplazamiento); patch8 le escribe la distancia hasta el
  // cursor actual. Todo lo que se salta aqui son secuencias de tamano fijo muy por debajo
  // de 127 bytes, pero si alguna vez no cupiera patch8 avisa y el bloque se descarta en
  // vez de emitir un salto a ninguna parte.
  auto jcc8(u8 cc) -> usize { u8_((u8)(0x70 | (cc & 0x0f))); u8_(0); return b.used; }
  auto jmp8() -> usize { u8_(0xEB); u8_(0); return b.used; }
  auto patch8(usize at) -> bool {
    if(!b.base || at == 0 || at - 1 >= b.cap || b.used > b.cap) return false;
    long long d = (long long)b.used - (long long)at;
    if(d < 0 || d > 127) return false;
    b.base[at - 1] = (u8)d;
    return true;
  }

  // --- 64 bits ---
  auto mov64_rr(u8 dst, u8 src) -> void { u8_(0x48); u8_(0x89); modrm(3, src, dst); }
  auto ld64(u8 dst, u8 base, s32 d) -> void { u8_(0x48); u8_(0x8B); mem(dst, base, d); }
  auto add64_rr(u8 dst, u8 src) -> void { u8_(0x48); u8_(0x03); modrm(3, dst, src); }
  auto mov_imm64(u8 dst, u64 imm) -> void { u8_(0x48); u8_((u8)(0xB8 + dst)); u64_(imm); }
  auto call_r(u8 r) -> void { u8_(0xFF); modrm(3, 2, r); }
  auto push(u8 r) -> void { u8_((u8)(0x50 + r)); }
  auto pop(u8 r)  -> void { u8_((u8)(0x58 + r)); }
  auto sub_rsp(u8 n) -> void { u8_(0x48); u8_(0x83); modrm(3, 5, rSPx); u8_(n); }
  auto add_rsp(u8 n) -> void { u8_(0x48); u8_(0x83); modrm(3, 0, rSPx); u8_(n); }
  auto ret() -> void { u8_(0xC3); }

  // --- SSE (128 bits) ---------------------------------------------------------
  // Solo se usan xmm0..xmm5, que son volatiles en Win64: por eso no hay ni un solo
  // guardado de registro en el codigo emitido, que es justo el coste que se venia
  // pagando por instruccion vectorial en el prologo del thunk de COP2. Todos los
  // registros caben en 3 bits, asi que ninguna forma necesita REX.
  auto sse_m(u8 p0, u8 opc, u8 reg, u8 base, s32 d) -> void {
    u8_(p0);
    if((reg | base) & 8) u8_((u8)(0x40 | ((reg & 8) >> 1) | ((base & 8) >> 3)));
    u8_(0x0F); u8_(opc); mem((u8)(reg & 7), base, d);
  }
  // REX solo cuando hace falta: xmm8..15 en el campo reg (REX.R) o en el r/m (REX.B).
  auto sse_rr(u8 opc, u8 dst, u8 src) -> void {
    u8_(0x66);
    if((dst | src) & 8) u8_((u8)(0x40 | ((dst & 8) >> 1) | ((src & 8) >> 3)));
    u8_(0x0F); u8_(opc); modrm(3, (u8)(dst & 7), (u8)(src & 7));
  }
  // movdqa entre xmm y [rsp+disp8]. RSP como base exige SIB, que el resto del emisor
  // no necesita: aqui es solo para salvar/restaurar xmm6..xmm8 en el prologo.
  auto xmmSpill(u8 reg, u8 disp, bool store) -> void {
    u8_(0x66);
    if(reg & 8) u8_(0x44);
    u8_(0x0F); u8_(store ? 0x7F : 0x6F);
    modrm(1, (u8)(reg & 7), rSPx); u8_(0x24); u8_(disp);
  }
  auto sse38(u8 opc, u8 dst, u8 src) -> void {
    u8_(0x66); u8_(0x0F); u8_(0x38); u8_(opc); modrm(3, dst, src);
  }
  auto ldx (u8 dst, u8 base, s32 d) -> void { sse_m(0x66, 0x6F, dst, base, d); }   // movdqa x,[b+d]
  auto stx (u8 src, u8 base, s32 d) -> void { sse_m(0x66, 0x7F, src, base, d); }   // movdqa [b+d],x
  auto ldxu(u8 dst, u8 base, s32 d) -> void { sse_m(0xF3, 0x6F, dst, base, d); }   // movdqu x,[b+d]
  // pextrw r32, xmm, imm8 -- saca una banda de 16 bits a un registro entero (SSE2).
  auto pextrw(u8 dst, u8 src, u8 lane) -> void {
    u8_(0x66);
    if((dst | src) & 8) u8_((u8)(0x40 | ((dst & 8) >> 1) | ((src & 8) >> 3)));
    u8_(0x0F); u8_(0xC5); modrm(3, (u8)(dst & 7), (u8)(src & 7)); u8_(lane);
  }
  auto movx(u8 dst, u8 src) -> void { sse_rr(0x6F, dst, src); }                    // movdqa x,x
  auto pshufb_x(u8 dst, u8 src) -> void { sse38(0x00, dst, src); }
  auto pmovsxwd(u8 dst, u8 src) -> void { sse38(0x23, dst, src); }
  auto pmovzxwd(u8 dst, u8 src) -> void { sse38(0x33, dst, src); }
  auto psrld_i (u8 dst, u8 n) -> void { u8_(0x66); u8_(0x0F); u8_(0x72); modrm(3, 2, dst); u8_(n); }
  auto psrldq_i(u8 dst, u8 n) -> void { u8_(0x66); u8_(0x0F); u8_(0x73); modrm(3, 3, dst); u8_(n); }
  auto psraw_i(u8 dst, u8 n) -> void { u8_(0x66); u8_(0x0F); u8_(0x71); modrm(3, 4, dst); u8_(n); }
  auto psrlw_i(u8 dst, u8 n) -> void { u8_(0x66); u8_(0x0F); u8_(0x71); modrm(3, 2, dst); u8_(n); }
  auto psllw_i(u8 dst, u8 n) -> void { u8_(0x66); u8_(0x0F); u8_(0x71); modrm(3, 6, dst); u8_(n); }
  auto pslld_i(u8 dst, u8 n) -> void { u8_(0x66); u8_(0x0F); u8_(0x72); modrm(3, 6, dst); u8_(n); }
  auto zerox(u8 r) -> void { sse_rr(0xEF, r, r); }            // pxor x,x
  auto onesx(u8 r) -> void { sse_rr(0x76, r, r); }            // pcmpeqd x,x  -> todo unos
};

// opcodes de la forma "r32, r/m32" de las ALU que se usan
enum : u8 { OP_ADD = 0x03, OP_SUB = 0x2B, OP_AND = 0x23, OP_OR = 0x0B, OP_XOR = 0x33, OP_CMP = 0x3B };
// digitos /d para la forma con inmediato y para los desplazamientos
enum : u8 { D_ADD = 0, D_OR = 1, D_AND = 4, D_SUB = 5, D_XOR = 6, D_CMP = 7 };
enum : u8 { D_SHL = 4, D_SHR = 5, D_SAR = 7 };
enum : u8 { CC_L = 0x9C, CC_B = 0x92, CC_E = 0x94, CC_NE = 0x95,
           CC_LE = 0x9E, CC_G = 0x9F, CC_GE = 0x9D, CC_A = 0x97 };

enum class Kind : u8 { Stop, Native, Cop2, Lwc2, Swc2, ExecMem, Mem, Branch };

// Que hace el compilador con cada instruccion. Stop = la ejecuta el interprete y el bloque
// termina ANTES de ella: saltos (necesitan el pestillo de delay-slot), BREAK y MTC0 (puede
// parar el nucleo o lanzar un DMA que reescriba IMEM bajo nuestros pies), y todo lo que no
// este reconocido, que asi cae en el mismo camino de siempre.
auto classify(u32 op) -> Kind {
  u32 maj = op >> 26;
  switch(maj) {
  case 0x00:
    switch(op & 0x3f) {
    case 0x00: case 0x02: case 0x03: case 0x04: case 0x06: case 0x07:
    case 0x20: case 0x21: case 0x22: case 0x23: case 0x24: case 0x25:
    case 0x26: case 0x27: case 0x2a: case 0x2b:
      return Kind::Native;
    case 0x08: case 0x09: return Kind::Branch;   // JR / JALR
    default: return Kind::Stop;      // BREAK / codificacion invalida
    }
  case 0x01:   // REGIMM: solo los cuatro branches definidos; el resto no se toca
    switch(op >> 16 & 31) {
    case 0x00: case 0x01: case 0x10: case 0x11: return Kind::Branch;
    default: return Kind::Stop;
    }
  case 0x02: case 0x03:               // J / JAL
  case 0x04: case 0x05: case 0x06: case 0x07:   // BEQ / BNE / BLEZ / BGTZ
    return Kind::Branch;
  case 0x08: case 0x09: case 0x0a: case 0x0b:
  case 0x0c: case 0x0d: case 0x0e: case 0x0f:
    return Kind::Native;
  case 0x20: case 0x24: case 0x28:   // LB / LBU / SB: un byte, sin alineacion que mirar
    return Kind::Native;
  case 0x21: case 0x23: case 0x25: case 0x27: case 0x29: case 0x2b:
    return Kind::Mem;                // LH/LHU/LW/LWU/SH/SW: nativos, envoltura al helper
  case 0x10:
    // MFC0 solo lee un registro de SP/DPC: no puede parar el nucleo ni reescribir IMEM,
    // asi que no hay razon para cortar el bloque -- basta llamar al mismo interprete. MTC0
    // si puede las dos cosas, y ese se queda en Stop.
    return (op >> 21 & 0x1f) == 0x00 ? Kind::ExecMem : Kind::Stop;
  case 0x12: return Kind::Cop2;
  case 0x32: return Kind::Lwc2;
  case 0x3a: return Kind::Swc2;
  default:   return Kind::Stop;
  }
}

struct Ctx {
  E   e;
  s32 rOff;      // offset de Rsp::r[0] dentro de Rsp
  s32 pcOff;     // offset de Rsp::pc dentro de Rsp
  s32 vOff = 0;        // offset de Rsp::vpr[0]
  s32 aOff[3] = {};    // acch / accm / accl
  s32 coOff[2] = {};   // vcoh / vcol
  s32 ccOff[2] = {};   // vcch / vccl
  s32 ceOff = 0;       // vce
  s32 divInOff = 0, divOutOff = 0, divDpOff = 0;   // estado de la familia del reciproco
  // Acumulador de 48 bits residente en xmm6/7/8 (acch/accm/accl) mientras dure el bloque.
  bool accReg = false;      // el prologo los salvo: se puede cachear
  u8   accValid = 0;        // bit i: xmm(6+i) tiene la rebanada i
  u8   accDirty = 0;        // bit i: xmm(6+i) es mas nueva que la memoria
  bool ok = true;   // false = algo no se pudo emitir; el bloque se tira sin registrar
  auto RG(u32 n) const -> s32 { return rOff + (s32)(4 * n); }
  auto VR(u32 n) const -> s32 { return vOff + (s32)(16 * n); }
};

// --- el acumulador de la VU, residente en registro -----------------------------
// Una racha de operaciones MAC recarga y reescribe las mismas tres rebanadas de 16 bytes
// una y otra vez. xmm6..xmm8 son callee-saved en Win64: el bloque los salva una vez en el
// prologo y a partir de ahi el acumulador vive en registro. La memoria se pone al dia solo
// al salir del bloque, o antes de un CALL -- el helper lee y escribe la copia de memoria,
// asi que ahi hay que volcar lo sucio y olvidar lo cacheado.
auto accGet(Ctx& c, u8 dst, u32 i) -> void {
  if(!c.accReg) { c.e.ldx(dst, rBX, c.aOff[i]); return; }
  if(!(c.accValid & (1u << i))) { c.e.ldx((u8)(6 + i), rBX, c.aOff[i]); c.accValid |= (u8)(1u << i); }
  c.e.movx(dst, (u8)(6 + i));
}
auto accPut(Ctx& c, u8 src, u32 i) -> void {
  if(!c.accReg) { c.e.stx(src, rBX, c.aOff[i]); return; }
  c.e.movx((u8)(6 + i), src);
  c.accValid |= (u8)(1u << i); c.accDirty |= (u8)(1u << i);
}
auto accFlush(Ctx& c) -> void {
  if(!c.accReg) return;
  for(u32 i = 0; i < 3; i++) if(c.accDirty & (1u << i)) c.e.stx((u8)(6 + i), rBX, c.aOff[i]);
  c.accDirty = 0;
}
auto accSpill(Ctx& c) -> void { accFlush(c); c.accValid = 0; }

auto emitCall(Ctx& c, void* fn, u32 op) -> void {
  accSpill(c);                   // el helper trabaja sobre la copia de memoria
  c.e.mov64_rr(rCX, rBX);        // arg0 = Rsp*
  c.e.mov_imm32(rDX, op);        // arg1 = opcode
  c.e.mov_imm64(rAX, (u64)fn);
  c.e.call_r(rAX);
}

// --- unidad vectorial en linea ----------------------------------------------
// Una COP2 aritmetica cuesta, por la ABI de Win64, mucho mas que la operacion en si: el
// thunk especializado abre marco, derrama xmm6..xmm10 (diez accesos a memoria antes de
// tocar un dato) y vuelve a decodificar el opcode que el compilador YA conoce. Para las
// operaciones cuyo cuerpo SSE es corto se emite ese mismo cuerpo dentro del bloque: los
// mismos intrinsecos de vuOpT, en el mismo orden, con vs/vt/vd y el modificador de
// elemento resueltos como constantes. No hay una segunda semantica; si la hubiera, el
// oraculo (KESTREL_RSPJIT=0 y el modo rspinterp) daria otro md5.
//
// Solo xmm0..xmm5, volatiles en Win64: cero derrames. Reparto fijo: xmm0 = S, xmm1 = T ya
// barajado, xmm2..xmm5 temporales.
enum : u8 { X_PMULLW = 0xD5, X_PMULHW = 0xE5, X_PCMPGTW = 0x65,
            X_PUNPCKLWD = 0x61, X_PUNPCKHWD = 0x69,
            X_PAND = 0xDB, X_PANDN = 0xDF, X_POR = 0xEB, X_PXOR = 0xEF,
            X_PADDW = 0xFD, X_PSUBW = 0xF9, X_PADDD = 0xFE, X_PSUBD = 0xFA,
            X_PCMPEQD = 0x76, X_PMULHUW = 0xE4, X_PACKSSDW = 0x6B,
            X_PCMPGTD = 0x66, X_PCMPEQW = 0x75, X_PSUBSW = 0xE9 };

auto vuInline(const Rsp& rsp, u32 op) -> bool {
#if KESTREL_VUSTAT
  (void)rsp; (void)op;
  return false;   // con el censo encendido todo pasa por el interprete, que es quien cuenta
#else
  if(!rsp.sse) return false;             // KESTREL_NORSPSSE: manda el respaldo escalar
  if((op >> 21 & 0x1f) < 0x10) return false;   // movimientos escalar<->vector: no
  switch(op & 0x3f) {
  case 0x00: case 0x01:                          // VMULF / VMULU
  case 0x04:                                     // VMUDL
  case 0x05: case 0x06: case 0x07:               // VMUDM / VMUDN / VMUDH
  case 0x08: case 0x09:                          // VMACF / VMACU
  case 0x0c: case 0x0d: case 0x0e:               // VMADL / VMADM / VMADN
  case 0x0f:                                     // VMADH
  case 0x10: case 0x11: case 0x14: case 0x15:    // VADD / VSUB / VADDC / VSUBC
  case 0x13:                                     // VABS
  case 0x1d:                                     // VSAR
  case 0x20: case 0x21: case 0x22: case 0x23:    // VLT / VEQ / VNE / VGE
  case 0x27:                                     // VMRG
  case 0x28: case 0x29: case 0x2a: case 0x2b:    // VAND / VNAND / VOR / VNOR
  case 0x2c: case 0x2d:                          // VXOR / VNXOR
  case 0x32: case 0x33: case 0x36:               // VRCPH / VMOV / VRSQH
    return true;
  default: return false;
  }
#endif
}

// Acarreo de salida (0/1 por banda de 16 bits) de sum = a + b: la misma formula que
// vcarry16 del interprete, (a&b) | (~sum & (a|b)) desplazado 15. DESTRUYE a.
auto emitCarry16(E& e, u8 a, u8 b, u8 sum, u8 out, u8 tmp) -> void {
  e.movx(out, a); e.sse_rr(X_PAND, out, b);      // a & b
  e.sse_rr(X_POR, a, b);                         // a | b
  e.movx(tmp, sum); e.sse_rr(X_PANDN, tmp, a);   // ~sum & (a|b)
  e.sse_rr(X_POR, out, tmp); e.psrlw_i(out, 15);
}

// D = saturacion con signo de (acch:accm), igual que vsatSigned: rearmar los pares de 16
// bits como enteros de 32 con signo y bajarlos con packssdw. No toca h ni m; los dos
// temporales se eligen fuera de ellos.
auto emitSatSigned(E& e, Ctx& c, u8 h, u8 m, u32 vd) -> void {
  u8 t0 = 0, t1 = 1;
  while(t0 == h || t0 == m) t0++;
  while(t1 == h || t1 == m || t1 == t0) t1++;
  e.movx(t0, m); e.sse_rr(X_PUNPCKLWD, t0, h);
  e.movx(t1, m); e.sse_rr(X_PUNPCKHWD, t1, h);
  e.sse_rr(X_PACKSSDW, t0, t1);
  e.stx(t0, rBX, c.VR(vd));
}

// D = saturacion sin signo de accl guiada por (acch:accm), igual que vsatUnsignedN: 0 si el
// entero de 32 bits queda por debajo de -0x8000, 0xffff si pasa de 0x7fff, y accl si cabe.
// ENTRA con acch en xmm0 y accm en xmm5 (lo que dejan emitAccAdd48 y el doblado), el resto
// libre. La mezcla del interprete (_mm_blendv_epi8 con todo-unos y con cero) es, con
// mascaras de todo-unos, exactamente (accl | desborde) & ~subdesborde -- sin pblendvb, que
// ademas usa xmm0 como operando implicito.
auto emitSatUnsignedN(E& e, Ctx& c, u32 vd) -> void {
  e.movx(1, 5); e.sse_rr(X_PUNPCKLWD, 1, 0);      // (acch:accm) como 4 enteros de 32, bajos
  e.movx(2, 5); e.sse_rr(X_PUNPCKHWD, 2, 0);      // ... y altos
  e.onesx(3); e.psrld_i(3, 17);                   // 0x00007fff por banda de 32
  e.movx(4, 1); e.sse_rr(X_PCMPGTD, 4, 3);
  e.movx(5, 2); e.sse_rr(X_PCMPGTD, 5, 3);
  e.sse_rr(X_PACKSSDW, 4, 5);                     // desborde por arriba
  e.onesx(3); e.pslld_i(3, 15);                   // 0xffff8000 = -0x8000 por banda de 32
  e.movx(5, 3); e.sse_rr(X_PCMPGTD, 5, 1);
  e.sse_rr(X_PCMPGTD, 3, 2);
  e.sse_rr(X_PACKSSDW, 5, 3);                     // desborde por abajo
  accGet(c, 0, 2);
  e.sse_rr(X_POR, 0, 4);
  e.sse_rr(X_PANDN, 5, 0);                        // ~subdesborde & (accl | desborde)
  e.stx(5, rBX, c.VR(vd));
}

// acc(48 bits, en memoria) += el triple que traen xmm2/xmm3/xmm4 (bajo/medio/alto). Es la
// cuenta de vadd48 rebanada a rebanada, con el mismo acarreo por banda de 16. Entra con
// xmm0/xmm1/xmm5 libres y sale con el nuevo acch en xmm0 y el nuevo accm en xmm5, ya
// escritas las tres rebanadas -- que es justo lo que piden las dos saturaciones.
auto emitAccAdd48(E& e, Ctx& c) -> void {
  accGet(c, 5, 2);
  e.movx(0, 5); e.sse_rr(X_PADDW, 0, 2);          // accl + bajo
  accPut(c, 0, 2);
  emitCarry16(e, 5, 2, 0, 1, 2);                  // acarreo de la baja -> xmm1
  accGet(c, 5, 1);
  e.movx(0, 5); e.sse_rr(X_PADDW, 0, 3);          // accm + medio
  emitCarry16(e, 5, 3, 0, 2, 3);                  // primer acarreo de la media -> xmm2
  e.movx(5, 0); e.sse_rr(X_PADDW, 5, 1);          // ... + el acarreo de abajo
  emitCarry16(e, 0, 1, 5, 3, 1);                  // segundo acarreo -> xmm3
  accPut(c, 5, 1);
  e.sse_rr(X_POR, 2, 3);                          // los dos nunca son 1 a la vez: basta un OR
  accGet(c, 0, 0);
  e.sse_rr(X_PADDW, 0, 4); e.sse_rr(X_PADDW, 0, 2);
  accPut(c, 0, 0);
}

// Duplica el producto de 48 bits que traen xmm2 (bajo) y xmm3 (medio): desplaza uno a la
// izquierda arrastrando el bit alto de cada rebanada, y deja el alto en xmm4. Sale lo mismo
// que vadd48(p,p) del interprete: el acarreo de cada banda ES su bit alto, y el segundo
// acarreo de la media no puede darse porque el desplazado tiene el bit 0 a cero. Usa xmm5.
auto emitDouble48(E& e) -> void {
  e.movx(4, 3); e.psraw_i(4, 15);                 // alto = extension de signo del medio
  e.psllw_i(4, 1);
  e.movx(5, 3); e.psrlw_i(5, 15); e.sse_rr(X_POR, 4, 5);
  e.psllw_i(3, 1);
  e.movx(5, 2); e.psrlw_i(5, 15); e.sse_rr(X_POR, 3, 5);
  e.psllw_i(2, 1);
}

// blendv(T, S, cm) sin blendv: con mascaras de todo-unos es (S & cm) | (T & ~cm), sin
// perder un bit. Se evita `pblendvb` a proposito: usa xmm0 como operando implicito y xmm0
// es justamente donde vive S. Entra S=xmm0, T=xmm1, la mascara en `cm`; sale en `dst`.
auto emitSelectST(E& e, u8 dst, u8 cm, u8 tmp) -> void {
  e.movx(dst, 0);  e.sse_rr(X_PAND,  dst, cm);
  e.movx(tmp, cm); e.sse_rr(X_PANDN, tmp, 1);
  e.sse_rr(X_POR, dst, tmp);
}

// Una bandera del RSP guarda 0/1 por banda; la mascara de seleccion es cmpgt contra cero.
auto emitFlagMask(E& e, u8 dst, u8 tmpZero, s32 off) -> void {
  e.ldx(dst, rBX, off);
  e.zerox(tmpZero);
  e.sse_rr(X_PCMPGTW, dst, tmpZero);
}

// ...y al reves: 0xffff -> 1. `pcmpeqd` + `psrlw 15` da el vector de unos de 16 bits.
auto emitFlagStore(E& e, u8 mask, u8 tmp, s32 off) -> void {
  e.onesx(tmp); e.psrlw_i(tmp, 15);
  e.sse_rr(X_PAND, tmp, mask);
  e.stx(tmp, rBX, off);
}

auto emitVu(Ctx& c, u32 op) -> void {
  E& e = c.e;
  const u32 fn = op & 0x3f, el = op >> 21 & 0xf;
  const u32 vt = op >> 16 & 31, vs = op >> 11 & 31, vd = op >> 6 & 31;

  // VSAR no mira operandos: copia la rebanada del acumulador que nombra el elemento.
  if(fn == 0x1d) {
    if(el >= 8 && el <= 10) accGet(c, 0, el - 8);
    else                    e.zerox(0);
    e.stx(0, rBX, c.VR(vd));
    return;
  }

  e.ldx(1, rBX, c.VR(vt));
  if(el >= 2) {                     // e=0 y e=1 son la identidad en la tabla de broadcast
    e.mov_imm64(rAX, (u64)rspBcastMask(el));
    e.ldxu(2, rAX, 0);              // la fila de mascaras no esta alineada: movdqu
    e.pshufb_x(1, 2);
  }
  // --- VMOV / VRCPH / VRSQH: no miran S, y su unica salida vectorial es UNA banda ---
  // El acumulador bajo se lleva T entero (barajado); del resultado solo cambia la banda
  // `de` de vd, asi que se escribe con un store de 16 bits en vez de leer, mezclar y
  // reescribir el registro. VRCPH y VRSQH son el mismo codigo en el interprete: arman la
  // mitad alta del dividendo (divin), marcan doble precision y entregan la mitad alta del
  // resultado anterior (divout).
  if(fn == 0x33 || fn == 0x32 || fn == 0x36) {
    const u32 de = op >> 11 & 7;
    accPut(c, 1, 2);                                   // accl = T barajado
    if(fn == 0x33) {
      e.pextrw(rAX, 1, (u8)de);                        // VMOV: vd[de] = T[de]
    } else {
      e.mov_imm32(rAX, 1); e.st8(rAX, rBX, c.divDpOff);
      e.pextrw(rAX, 1, (u8)(el & 7)); e.st16(rAX, rBX, c.divInOff);
      e.ld16z(rAX, rBX, c.divOutOff);
    }
    e.st16(rAX, rBX, c.VR(vd) + (s32)(2 * de));
    return;
  }

  e.ldx(0, rBX, c.VR(vs));

  switch(fn) {
  // logicas: accl = op; D = accl
  case 0x28: case 0x29: case 0x2a: case 0x2b: case 0x2c: case 0x2d: {
    e.sse_rr(fn <= 0x29 ? X_PAND : fn <= 0x2b ? X_POR : X_PXOR, 0, 1);
    if(fn & 1) { e.onesx(2); e.sse_rr(X_PXOR, 0, 2); }   // las negadas (NAND/NOR/NXOR)
    accPut(c, 0, 2);
    e.stx(0, rBX, c.VR(vd));
  } break;

  // --- VLT / VEQ / VNE / VGE: comparan, seleccionan y dejan la decision en VCC.low ---
  // cm = "gana S". accl = D = seleccion; VCC.high y VCO enteros a cero. Es la formula del
  // interprete tal cual, con la mascara de VCO.low/high reconstruida desde la bandera.
  case 0x20: case 0x21: case 0x22: case 0x23: {
    e.movx(2, 0); e.sse_rr(X_PCMPEQW, 2, 1);              // xmm2 = S==T
    emitFlagMask(e, 3, 5, c.coOff[1]);                    // xmm3 = VCO.low
    emitFlagMask(e, 4, 5, c.coOff[0]);                    // xmm4 = VCO.high
    if(fn == 0x20) {                                      // VLT: T>S || (S==T && low && high)
      e.sse_rr(X_PAND, 3, 4); e.sse_rr(X_PAND, 3, 2);
      e.movx(2, 1); e.sse_rr(X_PCMPGTW, 2, 0);
      e.sse_rr(X_POR, 2, 3);
    } else if(fn == 0x21) {                               // VEQ: !high && S==T
      e.sse_rr(X_PANDN, 4, 2); e.movx(2, 4);
    } else if(fn == 0x22) {                               // VNE: S!=T || high
      e.onesx(5); e.sse_rr(X_PXOR, 2, 5); e.sse_rr(X_POR, 2, 4);
    } else {                                              // VGE: S>T || (S==T && !(low && high))
      e.sse_rr(X_PAND, 3, 4); e.onesx(5); e.sse_rr(X_PXOR, 3, 5);
      e.sse_rr(X_PAND, 3, 2);
      e.movx(2, 0); e.sse_rr(X_PCMPGTW, 2, 1);
      e.sse_rr(X_POR, 2, 3);
    }
    emitSelectST(e, 3, 2, 4);
    accPut(c, 3, 2); e.stx(3, rBX, c.VR(vd));
    emitFlagStore(e, 2, 4, c.ccOff[1]);
    e.zerox(5);
    e.stx(5, rBX, c.ccOff[0]); e.stx(5, rBX, c.coOff[0]); e.stx(5, rBX, c.coOff[1]);
  } break;

  // --- VMRG: selecciona por VCC.low sin tocarla; solo borra VCO --------------
  case 0x27: {
    emitFlagMask(e, 2, 5, c.ccOff[1]);
    emitSelectST(e, 3, 2, 4);
    accPut(c, 3, 2); e.stx(3, rBX, c.VR(vd));
    e.zerox(5); e.stx(5, rBX, c.coOff[0]); e.stx(5, rBX, c.coOff[1]);
  } break;

  // --- VABS: el signo de S aplicado a T. No toca banderas --------------------
  // El unico caso que no es una negacion normal es T=-32768: el acumulador se queda con
  // el patron de negar en 16 bits (0x8000) y el destino con el saturado (0x7fff). Es la
  // misma resta hecha con dos saturaciones distintas: psubw y psubsw.
  case 0x13: {
    e.zerox(5);
    e.movx(2, 5); e.sse_rr(X_PCMPGTW, 2, 0);     // xmm2 = S<0
    e.movx(3, 0); e.sse_rr(X_PCMPEQW, 3, 5);     // xmm3 = S==0
    e.movx(4, 5); e.sse_rr(X_PSUBW, 4, 1);       // -T (envuelve)
    e.sse_rr(X_PAND, 4, 2);
    e.movx(5, 2); e.sse_rr(X_PANDN, 5, 1);       // T donde S>=0
    e.sse_rr(X_POR, 4, 5);
    e.movx(5, 3); e.sse_rr(X_PANDN, 5, 4);       // S==0 -> 0
    accPut(c, 5, 2);
    e.zerox(5); e.sse_rr(X_PSUBSW, 5, 1);        // -T saturado
    e.sse_rr(X_PAND, 5, 2);
    e.movx(4, 2); e.sse_rr(X_PANDN, 4, 1);
    e.sse_rr(X_POR, 5, 4);
    e.sse_rr(X_PANDN, 3, 5);
    e.stx(3, rBX, c.VR(vd));
  } break;

  // VMUDL: acc = zeroext(mulhi sin signo); D = accl
  case 0x04: {
    e.sse_rr(X_PMULHUW, 0, 1);
    e.zerox(2);
    accPut(c, 2, 0); accPut(c, 2, 1);
    accPut(c, 0, 2); e.stx(0, rBX, c.VR(vd));
  } break;

  // VADD / VSUB: envuelve a 16 bits en accl, satura con signo en D, borra VCO
  case 0x10: case 0x11: {
    const u8 w = (fn == 0x11) ? X_PSUBW : X_PADDW;
    const u8 d = (fn == 0x11) ? X_PSUBD : X_PADDD;
    e.ldx(2, rBX, c.coOff[1]);                       // acarreo/prestamo de entrada (0/1)
    e.movx(3, 0); e.sse_rr(w, 3, 1); e.sse_rr(w, 3, 2);
    accPut(c, 3, 2);
    // la misma cuenta exacta en 32 bits: al reempaquetar con signo sale sclamp16
    e.pmovsxwd(3, 0); e.pmovsxwd(4, 1); e.sse_rr(d, 3, 4);
    e.pmovsxwd(4, 2); e.sse_rr(d, 3, 4);
    e.movx(4, 0); e.psrldq_i(4, 8); e.pmovsxwd(4, 4);
    e.movx(5, 1); e.psrldq_i(5, 8); e.pmovsxwd(5, 5); e.sse_rr(d, 4, 5);
    e.movx(5, 2); e.psrldq_i(5, 8); e.pmovsxwd(5, 5); e.sse_rr(d, 4, 5);
    e.sse_rr(X_PACKSSDW, 3, 4);
    e.stx(3, rBX, c.VR(vd));
    e.zerox(0); e.stx(0, rBX, c.coOff[0]); e.stx(0, rBX, c.coOff[1]);
  } break;

  // VADDC: suma sin signo, acarreo de salida a VCO.low, VCO.high a cero
  case 0x14: {
    e.movx(3, 0); e.sse_rr(X_PADDW, 3, 1);
    accPut(c, 3, 2); e.stx(3, rBX, c.VR(vd));
    e.pmovzxwd(3, 0); e.pmovzxwd(4, 1); e.sse_rr(X_PADDD, 3, 4); e.psrld_i(3, 16);
    e.movx(4, 0); e.psrldq_i(4, 8); e.pmovzxwd(4, 4);
    e.movx(5, 1); e.psrldq_i(5, 8); e.pmovzxwd(5, 5); e.sse_rr(X_PADDD, 4, 5); e.psrld_i(4, 16);
    e.sse_rr(X_PACKSSDW, 3, 4);
    e.stx(3, rBX, c.coOff[1]);
    e.zerox(3); e.stx(3, rBX, c.coOff[0]);
  } break;

  // VSUBC: resta sin signo; prestamo a VCO.low, "distinto de cero" a VCO.high
  case 0x15: {
    e.movx(3, 0); e.sse_rr(X_PSUBW, 3, 1);
    accPut(c, 3, 2); e.stx(3, rBX, c.VR(vd));
    e.pmovzxwd(3, 0); e.pmovzxwd(4, 1); e.sse_rr(X_PSUBD, 3, 4);            // dlo
    e.movx(4, 0); e.psrldq_i(4, 8); e.pmovzxwd(4, 4);
    e.movx(5, 1); e.psrldq_i(5, 8); e.pmovzxwd(5, 5); e.sse_rr(X_PSUBD, 4, 5);   // dhi
    e.onesx(2); e.psrld_i(2, 31);                                           // 1 por banda
    e.movx(0, 3); e.psrld_i(0, 16); e.sse_rr(X_PAND, 0, 2);
    e.movx(1, 4); e.psrld_i(1, 16); e.sse_rr(X_PAND, 1, 2);
    e.sse_rr(X_PACKSSDW, 0, 1); e.stx(0, rBX, c.coOff[1]);
    e.zerox(0);
    e.sse_rr(X_PCMPEQD, 3, 0); e.sse_rr(X_PANDN, 3, 2);   // (~(d==0)) & 1
    e.sse_rr(X_PCMPEQD, 4, 0); e.sse_rr(X_PANDN, 4, 2);
    e.sse_rr(X_PACKSSDW, 3, 4); e.stx(3, rBX, c.coOff[0]);
  } break;

  // --- familia MAC ---------------------------------------------------------
  // El producto de 48 bits se arma igual que vprodSS/SU/US del interprete: mullo da el limbo
  // bajo, mulhi el medio y el alto es la extension de signo del medio. La correccion de signo
  // de SU/US es la misma resta condicional (-= el otro operando donde este es negativo).

  // VMUDM: acc = signext32(S.s * T.u); D = accm
  // VMUDN: acc = signext32(S.u * T.s); D = accl
  case 0x05: case 0x06: {
    const u8 sgn = (fn == 0x05) ? 0 : 1;         // el operando que va con signo
    const u8 oth = (fn == 0x05) ? 1 : 0;
    e.movx(2, 0); e.sse_rr(X_PMULLW, 2, 1);      // limbo bajo
    e.movx(3, 0); e.sse_rr(X_PMULHUW, 3, 1);
    e.zerox(4); e.sse_rr(X_PCMPGTW, 4, sgn);     // mascara: operando con signo < 0
    e.sse_rr(X_PAND, 4, oth);
    e.sse_rr(X_PSUBW, 3, 4);                     // limbo medio corregido
    e.movx(4, 3); e.psraw_i(4, 15);              // limbo alto = signo del medio
    accPut(c, 2, 2); accPut(c, 3, 1); accPut(c, 4, 0);
    e.stx((fn == 0x05) ? 3 : 2, rBX, c.VR(vd));
  } break;

  // VMULF / VMULU: acc = S.s*T.s*2 + 0x8000; D = satSigned / satMulU
  case 0x00: case 0x01: {
    e.movx(2, 0); e.sse_rr(X_PMULLW, 2, 1);      // limbo bajo
    e.movx(3, 0); e.sse_rr(X_PMULHW, 3, 1);      // limbo medio
    emitDouble48(e);                             // x2:x3:x4 = producto por dos
    e.onesx(5); e.psllw_i(5, 15);                // 0x8000 por banda
    e.movx(0, 2); e.sse_rr(X_PADDW, 0, 5);
    accPut(c, 0, 2);
    emitCarry16(e, 2, 5, 0, 1, 5);
    e.movx(5, 3); e.sse_rr(X_PADDW, 5, 1);
    emitCarry16(e, 3, 1, 5, 2, 1);
    accPut(c, 5, 1);
    e.movx(0, 4); e.sse_rr(X_PADDW, 0, 2);
    accPut(c, 0, 0);
    if(fn == 0x00) { emitSatSigned(e, c, 0, 5, vd); break; }
    // VMULU: acch<0 -> 0; (acch^accm)<0 -> 0xffff; si no, accm. Con mascaras de todo-unos
    // la doble mezcla del interprete es (accm | signo-cruzado) & ~negativo.
    e.zerox(1); e.movx(2, 1);
    e.sse_rr(X_PCMPGTW, 1, 0);
    e.movx(3, 0); e.sse_rr(X_PXOR, 3, 5);
    e.sse_rr(X_PCMPGTW, 2, 3);
    e.movx(4, 5); e.sse_rr(X_POR, 4, 2);
    e.sse_rr(X_PANDN, 1, 4);
    e.stx(1, rBX, c.VR(vd));
  } break;

  // VMACF / VMACU: acc += S.s*T.s*2; D = satSigned / satMacU
  case 0x08: case 0x09: {
    e.movx(2, 0); e.sse_rr(X_PMULLW, 2, 1);
    e.movx(3, 0); e.sse_rr(X_PMULHW, 3, 1);
    emitDouble48(e);
    emitAccAdd48(e, c);
    if(fn == 0x08) { emitSatSigned(e, c, 0, 5, vd); break; }
    // VMACU: acch<0 -> 0; acch!=0 o accm<0 -> 0xffff; si no, accm.
    e.zerox(1); e.movx(2, 1); e.movx(3, 1);
    e.sse_rr(X_PCMPGTW, 1, 0);                   // negativo
    e.sse_rr(X_PCMPEQW, 2, 0); e.onesx(4); e.sse_rr(X_PXOR, 2, 4);   // acch != 0
    e.sse_rr(X_PCMPGTW, 3, 5);                   // accm < 0
    e.sse_rr(X_POR, 2, 3);
    e.movx(4, 5); e.sse_rr(X_POR, 4, 2);
    e.sse_rr(X_PANDN, 1, 4);
    e.stx(1, rBX, c.VR(vd));
  } break;

  // VMADL: acc += zeroext(mulhi sin signo), sin extension de signo; D = satUnsignedN
  case 0x0c: {
    e.movx(2, 0); e.sse_rr(X_PMULHUW, 2, 1);
    e.zerox(3); e.zerox(4);
    emitAccAdd48(e, c);
    emitSatUnsignedN(e, c, vd);
  } break;

  // VMADM: acc += signext32(S.s * T.u); D = satSigned
  // VMADN: acc += signext32(S.u * T.s); D = satUnsignedN
  case 0x0d: case 0x0e: {
    const u8 sgn = (fn == 0x0d) ? 0 : 1;
    const u8 oth = (fn == 0x0d) ? 1 : 0;
    e.movx(2, 0); e.sse_rr(X_PMULLW, 2, 1);
    e.movx(3, 0); e.sse_rr(X_PMULHUW, 3, 1);
    e.zerox(4); e.sse_rr(X_PCMPGTW, 4, sgn);
    e.sse_rr(X_PAND, 4, oth);
    e.sse_rr(X_PSUBW, 3, 4);                     // limbo medio corregido
    e.movx(4, 3); e.psraw_i(4, 15);              // limbo alto = signo del medio
    emitAccAdd48(e, c);
    if(fn == 0x0d) emitSatSigned(e, c, 0, 5, vd);
    else           emitSatUnsignedN(e, c, vd);
  } break;

  // VMUDH: acc = (S.s * T.s) << 16, accl = 0; D = satSigned
  case 0x07: {
    e.movx(2, 0); e.sse_rr(X_PMULLW, 2, 1);      // -> accm
    e.movx(3, 0); e.sse_rr(X_PMULHW, 3, 1);      // -> acch
    e.zerox(4); accPut(c, 4, 2);
    accPut(c, 2, 1); accPut(c, 3, 0);
    emitSatSigned(e, c, 3, 2, vd);
  } break;

  // VMADH: (acch:accm) += S.s*T.s como suma de 32 bits; accl intacto; D = satSigned
  case 0x0f: {
    e.movx(2, 0); e.sse_rr(X_PMULLW, 2, 1);      // lo
    e.movx(3, 0); e.sse_rr(X_PMULHW, 3, 1);      // hi
    accGet(c, 4, 1);                    // accm
    e.movx(5, 4); e.sse_rr(X_PADDW, 5, 2);       // nm = accm + lo
    emitCarry16(e, 4, 2, 5, 0, 1);               // S y T ya no hacen falta: se usan de temporal
    accGet(c, 4, 0);
    e.sse_rr(X_PADDW, 4, 3); e.sse_rr(X_PADDW, 4, 0);   // nh = acch + hi + acarreo
    accPut(c, 5, 1); accPut(c, 4, 0);
    emitSatSigned(e, c, 4, 5, vd);
  } break;

  default: c.ok = false; break;     // no puede pasar: vuInline decide lo mismo
  }
}

auto emitNative(Ctx& c, u32 op) -> void {
  E& e = c.e;
  u32 maj = op >> 26;
  u32 rs = op >> 21 & 31, rt = op >> 16 & 31, rd = op >> 11 & 31, sa = op >> 6 & 31;
  u32 imm = op & 0xffff; s32 simm = (s16)imm;

  if(maj == 0x00) {
    u32 fn = op & 0x3f;
    if(rd == 0) return;                       // r0 es cableado a cero: la op no tiene efecto
    switch(fn) {
    case 0x00: case 0x02: case 0x03: {        // SLL / SRL / SRA  rd = rt op sa
      e.ld32(rAX, rBX, c.RG(rt));
      if(sa) e.shift_imm(fn == 0x00 ? D_SHL : fn == 0x02 ? D_SHR : D_SAR, rAX, (u8)sa);
      e.st32(rAX, rBX, c.RG(rd));
    } break;
    case 0x04: case 0x06: case 0x07: {        // SLLV / SRLV / SRAV  rd = rt op (rs & 31)
      // x86 ya enmascara el contador a 5 bits, que es exactamente el "& 31" del RSP.
      e.ld32(rCX, rBX, c.RG(rs));
      e.ld32(rAX, rBX, c.RG(rt));
      e.shift_cl(fn == 0x04 ? D_SHL : fn == 0x06 ? D_SHR : D_SAR, rAX);
      e.st32(rAX, rBX, c.RG(rd));
    } break;
    case 0x20: case 0x21:                     // ADD / ADDU (el RSP no lanza overflow)
      e.ld32(rAX, rBX, c.RG(rs)); e.alu_rm(OP_ADD, rAX, rBX, c.RG(rt));
      e.st32(rAX, rBX, c.RG(rd)); break;
    case 0x22: case 0x23:                     // SUB / SUBU
      e.ld32(rAX, rBX, c.RG(rs)); e.alu_rm(OP_SUB, rAX, rBX, c.RG(rt));
      e.st32(rAX, rBX, c.RG(rd)); break;
    case 0x24:                                // AND
      e.ld32(rAX, rBX, c.RG(rs)); e.alu_rm(OP_AND, rAX, rBX, c.RG(rt));
      e.st32(rAX, rBX, c.RG(rd)); break;
    case 0x25:                                // OR
      e.ld32(rAX, rBX, c.RG(rs)); e.alu_rm(OP_OR, rAX, rBX, c.RG(rt));
      e.st32(rAX, rBX, c.RG(rd)); break;
    case 0x26:                                // XOR
      e.ld32(rAX, rBX, c.RG(rs)); e.alu_rm(OP_XOR, rAX, rBX, c.RG(rt));
      e.st32(rAX, rBX, c.RG(rd)); break;
    case 0x27:                                // NOR
      e.ld32(rAX, rBX, c.RG(rs)); e.alu_rm(OP_OR, rAX, rBX, c.RG(rt));
      e.not32(rAX); e.st32(rAX, rBX, c.RG(rd)); break;
    case 0x2a: case 0x2b:                     // SLT / SLTU
      e.ld32(rAX, rBX, c.RG(rs)); e.alu_rm(OP_CMP, rAX, rBX, c.RG(rt));
      e.setcc(fn == 0x2a ? CC_L : CC_B, rAX); e.movzx8(rAX, rAX);
      e.st32(rAX, rBX, c.RG(rd)); break;
    }
    return;
  }

  if(maj >= 0x08 && maj <= 0x0f) {
    if(rt == 0) return;
    switch(maj) {
    case 0x08: case 0x09:                     // ADDI / ADDIU
      e.ld32(rAX, rBX, c.RG(rs));
      if(simm) e.alu_imm(D_ADD, rAX, (u32)simm);
      e.st32(rAX, rBX, c.RG(rt)); break;
    case 0x0a: case 0x0b:                     // SLTI / SLTIU: el inmediato va extendido en
      e.ld32(rAX, rBX, c.RG(rs));             // signo en los dos; lo que cambia es la compar.
      e.alu_imm(D_CMP, rAX, (u32)simm);
      e.setcc(maj == 0x0a ? CC_L : CC_B, rAX); e.movzx8(rAX, rAX);
      e.st32(rAX, rBX, c.RG(rt)); break;
    case 0x0c: case 0x0d: case 0x0e:          // ANDI / ORI / XORI (inmediato con ceros arriba)
      e.ld32(rAX, rBX, c.RG(rs));
      e.alu_imm(maj == 0x0c ? D_AND : maj == 0x0d ? D_OR : D_XOR, rAX, imm);
      e.st32(rAX, rBX, c.RG(rt)); break;
    case 0x0f:                                // LUI
      e.mov_imm32(rAX, imm << 16);
      e.st32(rAX, rBX, c.RG(rt)); break;
    }
    return;
  }

  // LB / LBU / SB. DMEM son 4 KB con envoltura, y un acceso de UN byte nunca la cruza:
  // basta enmascarar la direccion con 0xfff y sumarla al puntero de DMEM que el prologo
  // dejo en RDI. El "and" de 32 bits ademas pone a cero la mitad alta de RAX, asi que la
  // suma de 64 bits siguiente no arrastra basura.
  if(maj == 0x20 || maj == 0x24) {            // LB / LBU
    if(rt == 0) return;                        // una carga a r0 no tiene efecto observable
    e.ld32(rAX, rBX, c.RG(rs));
    if(simm) e.alu_imm(D_ADD, rAX, (u32)simm);
    e.alu_imm(D_AND, rAX, 0xfff);
    e.add64_rr(rAX, rDI);
    if(maj == 0x20) e.movsx8_m(rCX, rAX, 0); else e.movzx8_m(rCX, rAX, 0);
    e.st32(rCX, rBX, c.RG(rt));
    return;
  }
  // SB: r0 SI se lee (guarda un cero), asi que aqui no se puede saltar la instruccion.
  e.ld32(rAX, rBX, c.RG(rs));
  if(simm) e.alu_imm(D_ADD, rAX, (u32)simm);
  e.alu_imm(D_AND, rAX, 0xfff);
  e.add64_rr(rAX, rDI);
  e.ld32(rCX, rBX, c.RG(rt));
  e.st8(rCX, rAX, 0);
}

// LH / LHU / LW / LWU / SH / SW nativos.
//
// La clave es que el ensamblado byte a byte del interprete (rWord/rHalf) y una lectura
// del host mas bswap dan EXACTAMENTE los mismos bytes mientras el acceso no cruce el
// final de DMEM: rb(a)<<24|rb(a+1)<<16|rb(a+2)<<8|rb(a+3) es, por definicion, el bswap32
// de la palabra little-endian que hay en dmp+a. No hay una segunda semantica aqui, es la
// misma reordenada -- y por eso vale igual para direcciones desalineadas, que el RSP
// tampoco trata como excepcion.
//
// El unico caso que se aparta es la envoltura de 4 KB (a > 0xffc a 32 bits, a > 0xffe a
// 16), donde el byte de mas alto se pliega al principio de DMEM. Ese se manda al MISMO
// helper del interprete, asi que el oraculo sigue siendo el de siempre y el camino raro
// no tiene copia propia de las reglas. En microcodigo real no se da nunca, asi que el
// salto sale siempre no-tomado y no cuesta prediccion.
auto emitMem(Ctx& c, u32 op) -> void {
  E& e = c.e;
  const u32 maj = op >> 26, rs = op >> 21 & 31, rt = op >> 16 & 31;
  const s32 simm = (s16)(op & 0xffff);
  const bool wide  = (maj == 0x23 || maj == 0x27 || maj == 0x2b);   // LW / LWU / SW
  const bool store = (maj == 0x29 || maj == 0x2b);                  // SH / SW
  const u32  lim   = wide ? 0xffcu : 0xffeu;    // ultima direccion que no envuelve

  // El volcado del acumulador cacheado va AQUI, antes de la bifurcacion: el emitCall de la
  // rama lenta solo emitiria los stx dentro de esa rama, pero apagaria accDirty en tiempo
  // de compilacion, y entonces el camino rapido (el habitual) saldria del bloque con el
  // acumulador vivo solo en xmm6..xmm8 y la copia de memoria vieja.
  accFlush(c);   // solo volcar: una carga/almacenamiento escalar de la RSP no toca el acumulador

  e.ld32(rAX, rBX, c.RG(rs));
  if(simm) e.alu_imm(D_ADD, rAX, (u32)simm);
  e.alu_imm(D_AND, rAX, 0xfff);
  e.alu_imm(D_CMP, rAX, lim);
  const usize slow = e.jcc8(CC_A);
  e.add64_rr(rAX, rDI);              // el AND de 32 bits ya dejo limpia la mitad alta
  if(store) {
    e.ld32(rCX, rBX, c.RG(rt));
    if(wide) { e.bswap(rCX); e.st32(rCX, rAX, 0); }
    else     { e.rol16(rCX, 8); e.st16(rCX, rAX, 0); }   // rol de 16 bits = bswap16
  } else if(rt) {                    // una carga a r0 no tiene efecto observable
    if(wide) { e.ld32(rCX, rAX, 0); e.bswap(rCX); }
    else {
      e.ld16z(rCX, rAX, 0); e.rol16(rCX, 8);
      if(maj == 0x21) e.movsx16(rCX, rCX);               // LH extiende signo, LHU no
    }
    e.st32(rCX, rBX, c.RG(rt));
  }
  const usize done = e.jmp8();
  if(!e.patch8(slow)) { c.ok = false; return; }
  emitCall(c, (void*)&kestrel_rspjit_exec, op);
  if(!e.patch8(done)) c.ok = false;
}

// Salto (condicional o no) con su delay-slot ABSORBIDO en el bloque. Es lo ultimo que
// emite un bloque: escribe Rsp::pc con el PC que toca DESPUES del delay-slot y el llamante
// no vuelve a tocarlo (Block::setsPc). El delay-slot se emite justo detras con el camino
// normal, que es exactamente el orden del hardware: la condicion y el enlace se resuelven
// con los registros de ANTES del delay-slot, y el delay-slot corre igual salte o no.
//
// La condicion se resuelve SIN salto de host: setcc -> mascara 0/-1 -> select entre los dos
// PC constantes. Un bloque de microcodigo se ejecuta millones de veces con la misma
// direccion pero condicion alterna (bucles de vertices), asi que un jcc mal predicho ahi
// costaria mas que las cuatro ALU de esta forma sin ramas.
auto emitBranch(Ctx& c, u32 op, u32 bpc) -> void {
  E& e = c.e;
  const u32 maj = op >> 26, rs = op >> 21 & 31, rt = op >> 16 & 31, rd = op >> 11 & 31;
  const s32 simm = (s16)(op & 0xffff);
  const u32 fall = (bpc + 8) & 0xfff;                    // no tomado: tras el delay-slot
  const u32 tgt  = (bpc + 4 + (simm << 2)) & 0xffc;      // take() alinea a palabra

  // rAX tiene el resultado de setcc; deja en Rsp::pc  cond ? tgt : fall.
  auto selPc = [&](u8 cc) {
    e.setcc(cc, rAX); e.movzx8(rAX, rAX); e.neg32(rAX);  // 0 -> 0, 1 -> 0xffffffff
    e.alu_imm(D_AND, rAX, fall ^ tgt);
    e.alu_imm(D_XOR, rAX, fall);
    e.st32(rAX, rBX, c.pcOff);
  };
  // setR(n, fall): el enlace de JAL/JALR/B*AL. r0 es cableado a cero, no se escribe.
  auto link = [&](u32 n) {
    if(!n) return;
    e.mov_imm32(rCX, fall);
    e.st32(rCX, rBX, c.RG(n));
  };

  if(maj == 0x00) {                       // JR / JALR: destino en un registro
    e.ld32(rAX, rBX, c.RG(rs));           // se LEE rs antes de escribir rd (pueden coincidir)
    e.alu_imm(D_AND, rAX, 0xffc);
    e.st32(rAX, rBX, c.pcOff);
    if((op & 0x3f) == 0x09) link(rd);
    return;
  }

  if(maj == 0x02 || maj == 0x03) {        // J / JAL: destino inmediato
    if(maj == 0x03) link(31);             // el interprete enlaza antes del take; da igual,
    e.mov_imm32(rAX, ((op & 0x3ffffff) << 2) & 0xffc);   // ninguno de los dos lee el otro
    e.st32(rAX, rBX, c.pcOff);
    return;
  }

  if(maj == 0x01) {                       // REGIMM: BLTZ / BGEZ / BLTZAL / BGEZAL
    e.ld32(rAX, rBX, c.RG(rs));
    e.alu_imm(D_CMP, rAX, 0);
    selPc((rt & 1) ? CC_GE : CC_L);
    if(rt & 0x10) link(31);               // el enlace de B*AL es INCONDICIONAL
    return;
  }

  if(maj == 0x04 || maj == 0x05) {        // BEQ / BNE
    e.ld32(rAX, rBX, c.RG(rs));
    e.alu_rm(OP_CMP, rAX, rBX, c.RG(rt));
    selPc(maj == 0x04 ? CC_E : CC_NE);
    return;
  }

  // BLEZ / BGTZ: contra cero, con signo
  e.ld32(rAX, rBX, c.RG(rs));
  e.alu_imm(D_CMP, rAX, 0);
  selPc(maj == 0x06 ? CC_LE : CC_G);
}

}  // namespace

auto compile(Rsp& rsp, Cache& c, u32 pc0) -> void {
  const u32 idx = (pc0 >> 2) & 1023;
  c.state[idx] = State::NoComp;                // por defecto: si algo falla, no se reintenta
  if(!c.ready) return;

  // Cuantas instrucciones seguidas son compilables desde aqui. Un salto reconocido cierra
  // el bloque LLEVANDOSE su delay-slot: los dos entran, y el bloque deja Rsp::pc puesto.
  u32 n = 0; bool endsBranch = false;
  auto at = [&](u32 a) -> u32 { u32 w; std::memcpy(&w, rsp.imp + a, 4); return bswap32(w); };
  while(n < kMaxOps) {
    u32 a = (pc0 + 4 * n) & 0xffc;
    if(n && a < pc0) break;                    // no compilamos bloques que envuelvan IMEM
    Kind k = classify(at(a));
    if(k == Kind::Stop) break;
    if(k == Kind::Branch) {
      if(n + 2 > kMaxOps) break;               // no cabe el par salto+delay
      u32 ad = (a + 4) & 0xffc;
      if(ad != a + 4) break;                   // el delay-slot envolveria IMEM
      Kind dk = classify(at(ad));
      // Un salto en el delay-slot de otro salto no esta definido en el R4000 y el
      // interprete lo resuelve con su pestillo; el bloque no sabe hacerlo, asi que corta.
      if(dk == Kind::Stop || dk == Kind::Branch) break;
      n += 2; endsBranch = true;
      break;
    }
    n++;
  }
  if(n < kMinOps) return;                      // el prologo costaria mas que interpretarlas

  // Holgura de buffer: si no cabe el peor caso de este bloque, se recicla la tabla entera.
  // Se puede hacer aqui sin peligro porque el llamante no esta dentro de ningun bloque.
  // La VU en linea es la secuencia mas larga con diferencia: VMACU son el doblado del
  // producto, la suma de 48 bits con sus tres acarreos y la saturacion, ~290 bytes. Quedarse
  // corto no corrompe nada -- el emisor detecta el desbordamiento y tira el bloque -- pero
  // lo tira DESPUES de compilarlo, y eso es trabajo perdido cada vez.
  const usize worst = 96 + n * 400;
  if(c.buf.used + worst > c.buf.cap) c.clear();
  if(c.buf.used + worst > c.buf.cap) return;   // no cabe ni en un buffer vacio

  // Que necesita este bloque. Un bloque de solo ALU y un salto -- que es la forma de casi
  // todo bucle de microcodigo -- no toca DMEM ni llama a ningun helper, asi que no tiene
  // por que pagar el puntero a DMEM ni el hueco de sombra de la ABI. El prologo se queda
  // en push rbx / mov rbx,rcx, y el epilogo en pop rbx / ret.
  bool needsDmem = false, needsCall = false;
  u32 vuOps = 0;                         // COP2 en linea: deciden si vale cachear el acumulador
  for(u32 i = 0; i < n; i++) {
    u32 op = at((pc0 + 4 * i) & 0xffc), maj = op >> 26;
    switch(classify(op)) {
    case Kind::Cop2:
      if(vuInline(rsp, op)) vuOps++; else needsCall = true; break;   // la VU en linea no llama a nadie
    case Kind::Lwc2: case Kind::Swc2: case Kind::ExecMem:
      needsCall = true; break;
    case Kind::Mem:                        // camino rapido en DMEM + envoltura al helper
      needsCall = true; needsDmem = true; break;
    case Kind::Native:
      if(maj == 0x20 || maj == 0x24 || maj == 0x28) needsDmem = true;  // LB / LBU / SB
      break;
    default: break;
    }
  }

  u8* entry = c.buf.cursor();
  Ctx ctx{ E(c.buf), (s32)((const u8*)&rsp.r[0] - (const u8*)&rsp),
                     (s32)((const u8*)&rsp.pc   - (const u8*)&rsp) };
  ctx.vOff     = (s32)((const u8*)&rsp.vpr[0] - (const u8*)&rsp);
  ctx.aOff[0]  = (s32)((const u8*)&rsp.acch   - (const u8*)&rsp);
  ctx.aOff[1]  = (s32)((const u8*)&rsp.accm   - (const u8*)&rsp);
  ctx.aOff[2]  = (s32)((const u8*)&rsp.accl   - (const u8*)&rsp);
  ctx.coOff[0] = (s32)((const u8*)&rsp.vcoh   - (const u8*)&rsp);
  ctx.coOff[1] = (s32)((const u8*)&rsp.vcol   - (const u8*)&rsp);
  ctx.ccOff[0] = (s32)((const u8*)&rsp.vcch   - (const u8*)&rsp);
  ctx.ccOff[1] = (s32)((const u8*)&rsp.vccl   - (const u8*)&rsp);
  ctx.ceOff    = (s32)((const u8*)&rsp.vce    - (const u8*)&rsp);
  ctx.divInOff  = (s32)((const u8*)&rsp.divin  - (const u8*)&rsp);
  ctx.divOutOff = (s32)((const u8*)&rsp.divout - (const u8*)&rsp);
  ctx.divDpOff  = (s32)((const u8*)&rsp.divdp  - (const u8*)&rsp);
  const s32 dmpOff = (s32)((const u8*)&rsp.dmp - (const u8*)&rsp);
  E& e = ctx.e;

  // Prologo. RBX = Rsp*, RDI = DMEM (solo si hace falta). Los dos son callee-saved en Win64,
  // de ahi los push. Alineacion: al entrar RSP=8 (mod 16) y un CALL exige RSP=0, asi que el
  // hueco depende de cuantos push hubo -- 40 con dos, 32 con uno; sin CALL no hace falta.
  // Con dos o mas COP2 en linea el acumulador se queda en xmm6/7/8, que son callee-saved:
  // hay que salvarlos, y su hueco va DETRAS del de sombra y alineado a 16.
  const bool accReg = vuOps >= 2;
  const u8 pushes = (u8)(1 + (needsDmem ? 1 : 0));
  u8 frame = 0, accBase = 0;
  if(needsCall || accReg) {
    u8 base = needsCall ? 32 : 0;
    if(accReg) { accBase = base; base = (u8)(base + 48); }
    frame = (u8)(base + ((pushes & 1) ? 0 : 8));
  }
  ctx.accReg = accReg;
  e.push(rBX);
  if(needsDmem) e.push(rDI);
  e.mov64_rr(rBX, rCX);
  if(needsDmem) e.ld64(rDI, rBX, dmpOff);
  if(frame) e.sub_rsp(frame);
  if(accReg) for(u8 k = 0; k < 3; k++) e.xmmSpill((u8)(6 + k), (u8)(accBase + 16 * k), true);

  for(u32 i = 0; i < n; i++) {
    const u32 a = (pc0 + 4 * i) & 0xffc;
    u32 w; std::memcpy(&w, rsp.imp + a, 4);
    u32 op = bswap32(w);
    switch(classify(op)) {
    case Kind::Branch:  emitBranch(ctx, op, a); break;
    case Kind::Native:  emitNative(ctx, op); break;
    // Para la aritmetica vectorial se llama a la entrada ya especializada para ESE fn:
    // el destino es constante en tiempo de compilacion del bloque, asi que sale un CALL
    // directo y dentro no queda ningun switch que predecir. Los movimientos escalar<->vector
    // (sub<0x10) siguen por el puente generico, que es donde se decide.
    case Kind::Cop2:
      if(vuInline(rsp, op)) { emitVu(ctx, op); break; }
      emitCall(ctx, ((op >> 21 & 0x1f) < 0x10) ? (void*)&kestrel_rspjit_cop2
                                               : rspCop2Entry(op), op); break;
    case Kind::Lwc2:    emitCall(ctx, rspLwc2Entry(op), op); break;   // ya especializadas
    case Kind::Swc2:    emitCall(ctx, rspSwc2Entry(op), op); break;   // por sub, como COP2
    case Kind::ExecMem: emitCall(ctx, (void*)&kestrel_rspjit_exec,  op); break;
    case Kind::Mem:     emitMem(ctx, op); break;
    case Kind::Stop:    break;                 // no puede pasar: el conteo paro antes
    }
  }

  accFlush(ctx);
  if(accReg) for(u8 k = 0; k < 3; k++) e.xmmSpill((u8)(6 + k), (u8)(accBase + 16 * k), false);
  if(frame) e.add_rsp(frame);
  if(needsDmem) e.pop(rDI);
  e.pop(rBX); e.ret();

  if(c.buf.overflowed()) { c.clear(); return; }
  if(!ctx.ok) return;                          // emision incompleta: no se registra
  c.buf.finalize(entry);
  c.blocks[idx] = Block{ (BlockFn)entry, (u16)n, endsBranch };
  c.state[idx]  = State::Compiled;
  c.compiles++;
}

}  // namespace kestrel::rspjit
