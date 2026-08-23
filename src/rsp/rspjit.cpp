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

auto imemFingerprint(const u8* imem) -> u64 {
  // FNV-1a sobre las 512 palabras de 64 bits de IMEM. Se recalcula una vez por tarea, no
  // por instruccion: ~1500 ciclos frente a las decenas de miles de instrucciones que dura
  // una tarea de microcodigo.
  u64 h = 0xcbf29ce484222325ull;
  u64 w;
  for(u32 i = 0; i < 4096; i += 8) {
    std::memcpy(&w, imem + i, 8);
    h = (h ^ w) * 0x100000001b3ull;
  }
  return h;
}

auto Cache::init() -> bool {
  // 4 MB de RWX: 1024 entradas x hasta 64 instrucciones x ~24 bytes por instruccion es el
  // peor caso teorico (~1.5 MB); el resto es holgura para no tener que vaciar la tabla en
  // mitad de una tarea. Si aun asi se llena, clear() la recicla entera.
  if(!buf.init(4u << 20)) return false;
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
  auto mov_imm32(u8 dst, u32 imm) -> void { u8_((u8)(0xB8 + dst)); u32_(imm); }
  auto setcc(u8 cc, u8 dst) -> void { u8_(0x0F); u8_(cc); modrm(3, 0, dst); }
  auto movzx8(u8 dst, u8 src) -> void { u8_(0x0F); u8_(0xB6); modrm(3, dst, src); }
  auto movsx8_m(u8 dst, u8 base, s32 d) -> void { u8_(0x0F); u8_(0xBE); mem(dst, base, d); }
  auto movzx8_m(u8 dst, u8 base, s32 d) -> void { u8_(0x0F); u8_(0xB6); mem(dst, base, d); }
  auto st8(u8 src, u8 base, s32 d) -> void { u8_(0x88); mem(src, base, d); }

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
enum : u8 { CC_L = 0x9C, CC_B = 0x92 };

enum class Kind : u8 { Stop, Native, Cop2, Lwc2, Swc2, ExecMem };

// Que hace el compilador con cada instruccion. Stop = la ejecuta el interprete y el bloque
// termina ANTES de ella: saltos (necesitan el pestillo de delay-slot), BREAK y COP0 (pueden
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
    default: return Kind::Stop;      // JR / JALR / BREAK / codificacion invalida
    }
  case 0x08: case 0x09: case 0x0a: case 0x0b:
  case 0x0c: case 0x0d: case 0x0e: case 0x0f:
    return Kind::Native;
  case 0x20: case 0x24: case 0x28:   // LB / LBU / SB: un byte, sin alineacion que mirar
    return Kind::Native;
  case 0x21: case 0x23: case 0x25: case 0x27: case 0x29: case 0x2b:
    return Kind::ExecMem;            // LH/LHU/LW/LWU/SH/SW: al helper del interprete
  case 0x12: return Kind::Cop2;
  case 0x32: return Kind::Lwc2;
  case 0x3a: return Kind::Swc2;
  default:   return Kind::Stop;
  }
}

struct Ctx {
  E   e;
  s32 rOff;      // offset de Rsp::r[0] dentro de Rsp
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

}  // namespace

auto compile(Rsp& rsp, Cache& c, u32 pc0) -> void {
  const u32 idx = (pc0 >> 2) & 1023;
  c.state[idx] = State::NoComp;                // por defecto: si algo falla, no se reintenta
  if(!c.ready) return;

  // Cuantas instrucciones seguidas son compilables desde aqui.
  u32 n = 0;
  while(n < kMaxOps) {
    u32 a = (pc0 + 4 * n) & 0xffc;
    if(n && a < pc0) break;                    // no compilamos bloques que envuelvan IMEM
    u32 w; std::memcpy(&w, rsp.imp + a, 4);
    if(classify(bswap32(w)) == Kind::Stop) break;
    n++;
  }
  if(n < kMinOps) return;                      // el prologo costaria mas que interpretarlas

  // Holgura de buffer: si no cabe el peor caso de este bloque, se recicla la tabla entera.
  // Se puede hacer aqui sin peligro porque el llamante no esta dentro de ningun bloque.
  const usize worst = 64 + n * 32;
  if(c.buf.used + worst > c.buf.cap) c.clear();
  if(c.buf.used + worst > c.buf.cap) return;   // no cabe ni en un buffer vacio

  u8* entry = c.buf.cursor();
  Ctx ctx{ E(c.buf), (s32)((const u8*)&rsp.r[0] - (const u8*)&rsp) };
  const s32 dmpOff = (s32)((const u8*)&rsp.dmp - (const u8*)&rsp);
  E& e = ctx.e;

  // Prologo. RBX = Rsp*, RDI = DMEM. Los dos son callee-saved en Win64, de ahi los push.
  // Alineacion: al entrar RSP=8 (mod 16); dos push -> 8; sub 40 -> 0, que es lo que exige
  // un CALL, y esos 40 bytes cubren de sobra los 32 de shadow space de la ABI.
  e.push(rBX); e.push(rDI);
  e.mov64_rr(rBX, rCX);
  e.ld64(rDI, rBX, dmpOff);
  e.sub_rsp(40);

  for(u32 i = 0; i < n; i++) {
    u32 w; std::memcpy(&w, rsp.imp + ((pc0 + 4 * i) & 0xffc), 4);
    u32 op = bswap32(w);
    switch(classify(op)) {
    case Kind::Native:  emitNative(ctx, op); break;
    case Kind::Cop2:    emitCall(ctx, (void*)&kestrel_rspjit_cop2,  op); break;
    case Kind::Lwc2:    emitCall(ctx, (void*)&kestrel_rspjit_load,  op); break;
    case Kind::Swc2:    emitCall(ctx, (void*)&kestrel_rspjit_store, op); break;
    case Kind::ExecMem: emitCall(ctx, (void*)&kestrel_rspjit_exec,  op); break;
    case Kind::Stop:    break;                 // no puede pasar: el conteo paro antes
    }
  }

  e.add_rsp(40); e.pop(rDI); e.pop(rBX); e.ret();

  if(c.buf.overflowed()) { c.clear(); return; }
  c.buf.finalize(entry);
  c.blocks[idx] = Block{ (BlockFn)entry, (u16)n };
  c.state[idx]  = State::Compiled;
  c.compiles++;
}

}  // namespace kestrel::rspjit
