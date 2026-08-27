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
  bool ok = true;   // false = algo no se pudo emitir; el bloque se tira sin registrar
  auto RG(u32 n) const -> s32 { return rOff + (s32)(4 * n); }
};

auto emitCall(Ctx& c, void* fn, u32 op) -> void {
  c.e.mov64_rr(rCX, rBX);        // arg0 = Rsp*
  c.e.mov_imm32(rDX, op);        // arg1 = opcode
  c.e.mov_imm64(rAX, (u64)fn);
  c.e.call_r(rAX);
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
  const usize worst = 64 + n * 80;   // emitMem es la secuencia mas larga (~65 bytes)
  if(c.buf.used + worst > c.buf.cap) c.clear();
  if(c.buf.used + worst > c.buf.cap) return;   // no cabe ni en un buffer vacio

  // Que necesita este bloque. Un bloque de solo ALU y un salto -- que es la forma de casi
  // todo bucle de microcodigo -- no toca DMEM ni llama a ningun helper, asi que no tiene
  // por que pagar el puntero a DMEM ni el hueco de sombra de la ABI. El prologo se queda
  // en push rbx / mov rbx,rcx, y el epilogo en pop rbx / ret.
  bool needsDmem = false, needsCall = false;
  for(u32 i = 0; i < n; i++) {
    u32 op = at((pc0 + 4 * i) & 0xffc), maj = op >> 26;
    switch(classify(op)) {
    case Kind::Cop2: case Kind::Lwc2: case Kind::Swc2: case Kind::ExecMem:
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
  const s32 dmpOff = (s32)((const u8*)&rsp.dmp - (const u8*)&rsp);
  E& e = ctx.e;

  // Prologo. RBX = Rsp*, RDI = DMEM (solo si hace falta). Los dos son callee-saved en Win64,
  // de ahi los push. Alineacion: al entrar RSP=8 (mod 16) y un CALL exige RSP=0, asi que el
  // hueco depende de cuantos push hubo -- 40 con dos, 32 con uno; sin CALL no hace falta.
  const u8 frame = needsCall ? (needsDmem ? 40 : 32) : 0;
  e.push(rBX);
  if(needsDmem) e.push(rDI);
  e.mov64_rr(rBX, rCX);
  if(needsDmem) e.ld64(rDI, rBX, dmpOff);
  if(frame) e.sub_rsp(frame);

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
    case Kind::Cop2:    emitCall(ctx, ((op >> 21 & 0x1f) < 0x10) ? (void*)&kestrel_rspjit_cop2
                                                                 : rspCop2Entry(op), op); break;
    case Kind::Lwc2:    emitCall(ctx, rspLwc2Entry(op), op); break;   // ya especializadas
    case Kind::Swc2:    emitCall(ctx, rspSwc2Entry(op), op); break;   // por sub, como COP2
    case Kind::ExecMem: emitCall(ctx, (void*)&kestrel_rspjit_exec,  op); break;
    case Kind::Mem:     emitMem(ctx, op); break;
    case Kind::Stop:    break;                 // no puede pasar: el conteo paro antes
    }
  }

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
