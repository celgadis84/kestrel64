#pragma once
// kestrel64 dynarec foundation — Etapa 1 (infra de code-cache + emisor x86-64).
//
// Esto NO está cableado todavía al bucle de ejecución: es la maquinaria verificable
// en aislado (buffer ejecutable + emisor + self-test). El intérprete sigue siendo la
// única ruta activa, así que 0/3721 queda intacto por construcción. La Etapa 2 usará
// este emisor para compilar bloques de guest a x86-64.
#include "../core/types.hpp"
#include <cstddef>
#include <vector>
#include <unordered_map>

namespace kestrel::jit {

// x86-64 general-purpose registers (encoding order).
enum Reg : u8 {
  RAX = 0, RCX = 1, RDX = 2, RBX = 3, RSP = 4, RBP = 5, RSI = 6, RDI = 7,
  R8  = 8, R9  = 9, R10 = 10, R11 = 11, R12 = 12, R13 = 13, R14 = 14, R15 = 15,
};

// RWX executable memory: bump-allocated code cache. Windows VirtualAlloc /
// POSIX mmap; FlushInstructionCache after emission so the CPU sees fresh bytes.
class CodeBuffer {
public:
  CodeBuffer() = default;
  ~CodeBuffer();
  auto init(usize bytes) -> bool;   // reserve+commit RWX; false on failure
  auto reset() -> void { used = 0; }
  auto cursor() -> u8* { return base + used; }
  auto emit(u8 b) -> void { if(base && used < cap) base[used] = b; used++; }
  auto emit(const u8* p, usize n) -> void { for(usize i = 0; i < n; i++) emit(p[i]); }
  auto finalize(u8* from) -> void;  // flush icache for [from, cursor)
  auto overflowed() const -> bool { return used > cap; }
  u8*   base = nullptr;
  usize cap  = 0;
  usize used = 0;
};

// Minimal x86-64 emitter. Enough for the Etapa 2 hot subset (ALU imm/reg, loads/
// stores through a base register, moves, ret). Grows as codegen needs it.
//
// Limitación conocida: los helpers memory-operand usan disp8/disp32 con mod!=00, y
// no emiten SIB, así que el registro base no puede ser RSP/R12 (rm==100). El guest
// state se direcciona vía un base fijo (p.ej. RCX apuntando al CPU), que se elige
// fuera de esa clase de registros.
class Emitter {
public:
  explicit Emitter(CodeBuffer& b) : buf(b) {}

  // --- prefijos / codificación ---
  auto rex(bool w, u8 reg, u8 index, u8 rm) -> void {
    u8 v = 0x40 | (w ? 8 : 0) | ((reg >> 3) << 2) | ((index >> 3) << 1) | (rm >> 3);
    // REX siempre necesario si W=1 o si algún reg es R8-R15; con W=1 aquí siempre.
    buf.emit(v);
  }
  auto modrm(u8 mod, u8 reg, u8 rm) -> void {
    buf.emit((u8)((mod << 6) | ((reg & 7) << 3) | (rm & 7)));
  }
  auto imm32(u32 v) -> void { for(int i = 0; i < 4; i++) buf.emit((u8)(v >> (8 * i))); }
  auto imm64(u64 v) -> void { for(int i = 0; i < 8; i++) buf.emit((u8)(v >> (8 * i))); }
  auto disp8(s8 d) -> void  { buf.emit((u8)d); }

  // mem-operand ModRM+disp para [base+disp], base ∉ {RSP,R12}. disp8 si cabe.
  auto memOperand(u8 reg, u8 base, s32 disp) -> void {
    if(disp >= -128 && disp <= 127) { modrm(1, reg, base); disp8((s8)disp); }
    else { modrm(2, reg, base); imm32((u32)disp); }
  }

  // --- instrucciones ---
  auto mov_r_imm64(Reg dst, u64 imm) -> void {         // movabs dst, imm64
    rex(true, 0, 0, dst); buf.emit((u8)(0xB8 + (dst & 7))); imm64(imm);
  }
  auto mov_r_m(Reg dst, Reg base, s32 disp) -> void {  // mov dst, [base+disp]
    rex(true, dst, 0, base); buf.emit(0x8B); memOperand(dst, base, disp);
  }
  auto mov_m_r(Reg base, s32 disp, Reg src) -> void {  // mov [base+disp], src
    rex(true, src, 0, base); buf.emit(0x89); memOperand(src, base, disp);
  }
  auto mov_r_r(Reg dst, Reg src) -> void {             // mov dst, src
    rex(true, src, 0, dst); buf.emit(0x89); modrm(3, src, dst);
  }
  auto add_r_m(Reg dst, Reg base, s32 disp) -> void {  // add dst, [base+disp]
    rex(true, dst, 0, base); buf.emit(0x03); memOperand(dst, base, disp);
  }
  auto add_r_r(Reg dst, Reg src) -> void {             // add dst, src
    rex(true, src, 0, dst); buf.emit(0x01); modrm(3, src, dst);
  }
  auto add_r_imm32(Reg dst, s32 imm) -> void {         // add dst, imm32 (sign-ext)
    rex(true, 0, 0, dst); buf.emit(0x81); modrm(3, 0, dst); imm32((u32)imm);
  }
  auto ret() -> void { buf.emit(0xC3); }

  // ---- subconjunto ALU para el codegen de bloques (Etapa 2a) -----------------
  // Convención del bloque emitido: base = RBX apunta a gpr[0]; gpr[i] es [rbx+8*i].
  // Scratch = RAX/RCX/RDX (volátiles, sin salvar). Prólogo push rbx / mov rbx,rcx;
  // epílogo pop rbx / ret. Regs 0-3 → nunca hace falta REX.R/B; REX solo por W.
  static constexpr s32 goff(u8 r) { return (s32)r * 8; }

  auto push_rbx() -> void { buf.emit(0x53); }
  auto pop_rbx()  -> void { buf.emit(0x5B); }
  auto mov_rbx_rcx() -> void { rex(true,RCX,0,RBX); buf.emit(0x89); modrm(3,RCX,RBX); }

  // 32-bit load into a scratch (zero-extends into the 64-bit reg): mov r32,[rbx+off]
  auto ld32(Reg dst, u8 gi) -> void { buf.emit(0x8B); memOperand(dst, RBX, goff(gi)); }
  // 64-bit load: mov r64,[rbx+off]
  auto ld64(Reg dst, u8 gi) -> void { rex(true,dst,0,RBX); buf.emit(0x8B); memOperand(dst, RBX, goff(gi)); }
  // 64-bit store: mov [rbx+off],r64
  auto st64(Reg src, u8 gi) -> void { rex(true,src,0,RBX); buf.emit(0x89); memOperand(src, RBX, goff(gi)); }
  // movsxd r64, r32  (sign-extend 32→64)
  auto movsxd(Reg dst, Reg src) -> void { rex(true,dst,0,src); buf.emit(0x63); modrm(3,dst,src); }
  // imul dst, src (64): REX.W 0F AF /r  → dst = low64(dst*src). Para MULT/MULTU (operandos ya
  // extendidos a 64b, producto de dos valores de 32b cabe en 64b, low64 = producto exacto).
  auto imul64(Reg dst, Reg src) -> void { rex(true,dst,0,src); buf.emit(0x0F); buf.emit(0xAF); modrm(3,dst,src); }
  auto movzx_r8(Reg dst, Reg src) -> void { buf.emit(0x0F); buf.emit(0xB6); modrm(3,dst,src); }

  // 32-bit ALU: op r32, [rbx+off].  opc: ADD=0x03 SUB=0x2B AND=0x23 OR=0x0B XOR=0x33
  auto alu32_rm(u8 opc, Reg dst, u8 gi) -> void { buf.emit(opc); memOperand(dst, RBX, goff(gi)); }
  // 64-bit ALU: op r64, [rbx+off]
  auto alu64_rm(u8 opc, Reg dst, u8 gi) -> void { rex(true,dst,0,RBX); buf.emit(opc); memOperand(dst, RBX, goff(gi)); }
  // 32-bit ALU imm: op r32, imm32.  /digit: ADD=0 OR=1 AND=4 SUB=5 XOR=6
  auto alu32_imm(u8 digit, Reg dst, u32 imm) -> void { buf.emit(0x81); modrm(3,digit,dst); imm32(imm); }
  // 64-bit ALU imm: op r64, imm32 (sign-extended to 64)
  auto alu64_imm(u8 digit, Reg dst, u32 imm) -> void { rex(true,0,0,dst); buf.emit(0x81); modrm(3,digit,dst); imm32(imm); }
  // 32-bit shift by imm8:  /digit SHL=4 SHR=5 SAR=7
  auto shift32_imm(u8 digit, Reg dst, u8 sa) -> void { buf.emit(0xC1); modrm(3,digit,dst); buf.emit(sa); }
  // 32-bit shift by CL
  auto shift32_cl(u8 digit, Reg dst) -> void { buf.emit(0xD3); modrm(3,digit,dst); }
  // 64-bit shift by imm8:  REX.W /digit SHL=4 SHR=5 SAR=7  (SRA/SRAV usan la variante 64b)
  auto shift64_imm(u8 digit, Reg dst, u8 sa) -> void { rex(true,0,0,dst); buf.emit(0xC1); modrm(3,digit,dst); buf.emit(sa); }
  // 64-bit shift by CL
  auto shift64_cl(u8 digit, Reg dst) -> void { rex(true,0,0,dst); buf.emit(0xD3); modrm(3,digit,dst); }
  // not r64
  auto not64(Reg dst) -> void { rex(true,0,0,dst); buf.emit(0xF7); modrm(3,2,dst); }
  // cmp r64, [rbx+off]
  auto cmp64_rm(Reg dst, u8 gi) -> void { rex(true,dst,0,RBX); buf.emit(0x3B); memOperand(dst, RBX, goff(gi)); }
  // cmp r64, imm32 (sign-extended)
  auto cmp64_imm(Reg dst, u32 imm) -> void { rex(true,0,0,dst); buf.emit(0x81); modrm(3,7,dst); imm32(imm); }
  // setcc r8:  setl=0x9C setb=0x92 (into AL etc.)
  auto setcc(u8 cc, Reg r8) -> void { buf.emit(0x0F); buf.emit(cc); modrm(3,0,r8); }
  // mov r32, imm32 (zero-extends to 64)
  auto mov_r_imm32(Reg dst, u32 imm) -> void { buf.emit((u8)(0xB8 + (dst & 7))); imm32(imm); }

  // ---- soporte de llamada C (Etapa 2b: loads/stores como helper) -------------
  // Win64: args en RCX,RDX,R8,R9; retorno en (E)AX; shadow-space 32B; RSP 16-alineado
  // en el punto de CALL. RAX/RCX/RDX/R8-R11 son volátiles; el bloque recarga gpr de RAM
  // cada op → no depende de regs entre llamadas.
  auto push_reg(Reg r) -> void { if(r & 8) buf.emit(0x41); buf.emit((u8)(0x50 + (r & 7))); }
  auto pop_reg(Reg r)  -> void { if(r & 8) buf.emit(0x41); buf.emit((u8)(0x58 + (r & 7))); }
  // sub/add rsp, imm8 (para shadow space; imm 0..127)
  auto sub_rsp_imm8(u8 v) -> void { rex(true,0,0,RSP); buf.emit(0x83); modrm(3,5,RSP); buf.emit(v); }
  auto add_rsp_imm8(u8 v) -> void { rex(true,0,0,RSP); buf.emit(0x83); modrm(3,0,RSP); buf.emit(v); }
  auto call_reg(Reg r) -> void { if(r & 8) buf.emit(0x41); buf.emit(0xFF); modrm(3,2,r); }
  // mov dst, src (32-bit, para pasar índices/imm a regs de arg)
  auto mov_r_r32(Reg dst, Reg src) -> void { if((dst&8)||(src&8)) rex(false,src,0,dst); buf.emit(0x89); modrm(3,src,dst); }
  // test al, al  → ZF=1 si al==0 (helper devolvió false = faulted)
  auto test_al_al() -> void { buf.emit(0x84); modrm(3,RAX,RAX); }
  // je rel32 (salta si ZF): emite el opcode + placeholder; devuelve el offset del disp32
  // en el buffer para parchear luego con patchRel32(). Salto tomado cuando al==0.
  auto je_rel32_placeholder() -> usize { buf.emit(0x0F); buf.emit(0x84); usize at = buf.used; imm32(0); return at; }
  auto jmp_rel32_placeholder() -> usize { buf.emit(0xE9); usize at = buf.used; imm32(0); return at; }
  // Parchea un disp32 emitido en `at` para que apunte al cursor actual (destino = aquí).
  auto patchRel32(usize at) -> void {
    if(!buf.base || at + 4 > buf.cap) return;
    s32 rel = (s32)((s64)buf.used - (s64)(at + 4));
    for(int i = 0; i < 4; i++) buf.base[at + i] = (u8)((u32)rel >> (8 * i));
  }
  // Sobrescribe un imm32 absoluto ya emitido en `at` (p.ej. rellenar K en un placeholder).
  auto pokeU32(usize at, u32 v) -> void {
    if(!buf.base || at + 4 > buf.cap) return;
    for(int i = 0; i < 4; i++) buf.base[at + i] = (u8)(v >> (8 * i));
  }

  // ---- soporte de branch-en-bloque (Etapa 2c) --------------------------------
  // store byte AL → [rsp+disp8]  (88 /r con SIB base=RSP): preserva la condición del
  // branch a través del delay slot, que pisa RAX/RCX/RDX.
  auto st8_rsp(u8 disp) -> void { buf.emit(0x88); buf.emit(0x44); buf.emit(0x24); buf.emit(disp); }
  // load byte [rsp+disp8] → AL   (8A /r)
  auto ld8_rsp(u8 disp) -> void { buf.emit(0x8A); buf.emit(0x44); buf.emit(0x24); buf.emit(disp); }
  // store r64 → [rsp+disp8]  (REX.W 89 /r, SIB base=RSP): preserva un valor de 64b (p.ej.
  // el target de JR/JALR leído de gpr[rs]) a través del delay slot, que puede hacer CALL.
  auto st64_rsp(Reg src, u8 disp) -> void { rex(true,src,0,RSP); buf.emit(0x89); modrm(1,src,RSP); buf.emit(0x24); buf.emit(disp); }
  // load [rsp+disp8] → r64  (REX.W 8B /r, SIB base=RSP)
  auto ld64_rsp(Reg dst, u8 disp) -> void { rex(true,dst,0,RSP); buf.emit(0x8B); modrm(1,dst,RSP); buf.emit(0x24); buf.emit(disp); }
  // cmovnz dst,src (64): REX.W 0F 45 /r  — si ZF=0, dst=src.
  auto cmovnz(Reg dst, Reg src) -> void { rex(true,dst,0,src); buf.emit(0x0F); buf.emit(0x45); modrm(3,dst,src); }
  // cmovz dst,src (64): REX.W 0F 44 /r  — si ZF=1, dst=src (para MOVZ).
  auto cmovz(Reg dst, Reg src) -> void { rex(true,dst,0,src); buf.emit(0x0F); buf.emit(0x44); modrm(3,dst,src); }
  // mov byte [base+disp], imm8:  C6 /0 ib  (base ∉ {RSP,R12})
  auto mov_m8_imm(Reg base, s32 disp, u8 imm) -> void { buf.emit(0xC6); memOperand(0, base, disp); buf.emit(imm); }
  // add r64, imm8 (sign-ext):  REX.W 83 /0 ib  — para nextPc = pc+4
  auto add_r_imm8(Reg dst, s8 imm) -> void { rex(true,0,0,dst); buf.emit(0x83); modrm(3,0,dst); buf.emit((u8)imm); }
  // and r64, src (reg-reg):  REX.W 21 /r  — para J/JAL: pc & mask de segmento
  auto and_r_r(Reg dst, Reg src) -> void { rex(true,src,0,dst); buf.emit(0x21); modrm(3,src,dst); }
  // or  r64, src (reg-reg):  REX.W 09 /r  — para J/JAL: (pc&mask) | (target<<2)
  auto or_r_r(Reg dst, Reg src) -> void { rex(true,src,0,dst); buf.emit(0x09); modrm(3,src,dst); }

  // ---- soporte de block-linking (Etapa 3) ------------------------------------
  // cmp r64, r64:  REX.W 39 /r  — guarda de enlace (target runtime vs VA de compilación).
  auto cmp_r_r(Reg a, Reg b) -> void { rex(true,b,0,a); buf.emit(0x39); modrm(3,b,a); }
  // jne rel32 (ZF=0): placeholder; devuelve el offset del disp32.
  auto jne_rel32_placeholder() -> usize { buf.emit(0x0F); buf.emit(0x85); usize at = buf.used; imm32(0); return at; }
  // add dword [base+disp], imm32:  81 /0 id  (32-bit, sin REX) — acumula ops en jitPending.
  auto add_m32_imm32(Reg base, s32 disp, u32 imm) -> void {
    buf.emit(0x81); memOperand(0, base, disp); imm32(imm);
  }
  // jmp qword [rip+disp32]:  FF /4 con mod=00 rm=101 → FF 25 disp32. Salto indirecto a través
  // de una ranura de datos en el propio buffer: enlazar/desenlazar = un store de 8 bytes, sin
  // reescribir código (nada de SMC sobre el buffer ejecutable). Devuelve el offset del disp32.
  auto jmp_rip_mem_placeholder() -> usize {
    buf.emit(0xFF); buf.emit(0x25); usize at = buf.used; imm32(0); return at;
  }
  // Reserva 8 bytes alineados para una ranura de enlace; devuelve su offset en el buffer.
  auto reserveSlot() -> usize {
    while(buf.used & 7) buf.emit(0x90);
    usize at = buf.used; imm64(0); return at;
  }
  // Parchea un disp32 rip-relativo para que apunte a un offset ARBITRARIO del buffer (no al
  // cursor, como patchRel32): el destino puede estar por delante (ranura al final del bloque).
  auto patchRel32To(usize at, usize targetOff) -> void {
    if(!buf.base || at + 4 > buf.cap) return;
    s32 rel = (s32)((s64)targetOff - (s64)(at + 4));
    for(int i = 0; i < 4; i++) buf.base[at + i] = (u8)((u32)rel >> (8 * i));
  }

  CodeBuffer& buf;
};

// Self-test: emite en runtime una función que computa slot[0]+slot[1] → slot[2] y la
// ejecuta, comparando con la suma en C. Prueba el emisor+buffer antes del hot loop.
// Devuelve true si el código emitido produjo el resultado correcto.
auto smokeTest() -> bool;

// Self-test dirigido de una op ALU (p.ej. SRA) end-to-end: emite emitSafeOp para el
// opcode dado, la ejecuta sobre un gpr[] sembrado y devuelve gpr[dst]. Diagnóstico.
auto opSelfTest(u32 op, u64 rsVal, u64 rtVal, u32 dst) -> u64;

// Self-test de la maquinaria Etapa-2b (call C + bail condicional con patch de rel32):
// emite fn(gpr,ctx) = [ALU op0][call helper; si al==0 bail→return 1][ALU op2] return 3.
// Corre 2 veces (helper ok / helper faulted) y verifica: sin fault ejecuta op0+op2 y
// devuelve 3; con fault ejecuta solo op0 y devuelve 1 (op2 saltada). Prueba ABI+patch
// antes de cablear loads/stores. Devuelve true si ambos casos cuadran.
auto callBailSelfTest() -> bool;

// Firma del código emitido por bloque: recibe puntero a gpr[0] y el CPU* (como void*,
// para no acoplar jit.hpp a cpu.hpp). Devuelve el número de ops REALMENTE retiradas: ==
// nOps en éxito total, o el índice de la op de memoria que haría fault (bail limpio, sin
// vectorizar). El driver avanza pc/Count/retired por ese valor; el intérprete re-ejecuta
// la op faultante para vectorizar la excepción exacta. Los bloques solo-ALU devuelven nOps.
using BlockFn = u32 (*)(u64* gpr, void* cpu);

// Un bloque compilado = run secuencial de ops ALU/lógica/shift + loads/stores alineados
// simples (LB/LH/LW/LBU/LHU/LWU/LD/SB/SH/SW/SD) desde una PC física. Termina en la primera
// op no soportada (branch/cop/HI-LO/unaligned/LL-SC). `src` guarda las palabras originales
// para revalidar contra SMC/DMA en cada entrada.
// Un sitio de enlace emitido en la salida de control de un bloque. La guarda compara el target
// calculado en runtime contra `targetVA`; el enlace se activa/desactiva escribiendo SOLO datos:
//  - *vaImm  = targetVA  → guarda casa  → se toma el salto indirecto (ENLAZADO)
//  - *vaImm  = kNoLink   → guarda falla → cae a la salida lenta (DESENLAZADO)
//  - *slot   = destino del `jmp qword [rip+slot]`
// Nunca se reescribe código ejecutable, solo estas dos palabras de datos.
struct LinkSite {
  u64* vaImm = nullptr;     // imm64 del `mov rdx, targetVA` de la guarda
  u64* slot = nullptr;      // ranura de 8 bytes del salto indirecto
  u64  targetVA = 0;        // VA de destino en tiempo de compilación
  u32  targetPhys = 0;      // phys de destino (clave para (des)enlazar)
};
// VA imposible (impar: toda PC de N64 está alineada a 4) → la guarda nunca casa.
static constexpr u64 kNoLink = 1;

struct Block {
  BlockFn fn = nullptr;
  u32 nOps = 0;
  bool hasMem = false;    // contiene al menos un load/store (afecta modo diff)
  bool hasStore = false;  // contiene al menos un store (muta memoria)
  bool hasBranch = false; // termina en un branch absorbido (escribe pc/nextPc; salida de control)
  bool dead = false;      // SMC invalidó este bloque: find() lo trata como miss → recompila in-place
  std::vector<u32> src;   // opcodes originales, para validación
  // --- block-linking (Etapa 3) ---
  u32 phys = 0;             // PC física de entrada (clave del cache; necesaria para (des)enlazar)
  std::vector<LinkSite> sites;  // sitios de enlace emitidos en las salidas de este bloque
  u64 linkedEpoch = ~0ull;  // época de enlace con la que este bloque fue publicado como destino;
                            // si != cache.linkEpoch hay que re-enlazarlo (tras un desenlace global)
  u8* linkEntry = nullptr;  // punto de entrada para un salto ENLAZADO: justo tras el prólogo de
                            // marco (push rbx/r12; mov; sub rsp), donde el predecesor ya dejó
                            // RBX/R12/RSP válidos → el sucesor reutiliza el marco del predecesor
                            // y su epílogo retorna al driver que llamó al PRIMER bloque.
};

// Cache de bloques + buffer ejecutable. Propiedad del CPU (uno por núcleo emulado).
struct CodeCache {
  CodeBuffer buf;
  // Mapa disperso: clave = dirección física de inicio (fpe). value índice en `blocks`.
  std::vector<Block> blocks;
  // hash simple phys→idx; -1 = vacío. Tabla de tamaño potencia de 2.
  std::vector<u32> index;   // phys de la ranura
  std::vector<s32> slot;    // idx de bloque en la ranura
  u32 mask = 0;
  bool ready = false;
  u64 hits = 0, misses = 0, compiles = 0;

  // --- block-linking (Etapa 3) ---
  std::vector<LinkSite> links;                              // todos los sitios emitidos
  std::unordered_map<u32, std::vector<u32>> byTarget;       // targetPhys → índices en `links`
  u64 linkEpoch = 0;      // sube en cada desenlace global (invalidación de I-cache)
  u64 nLinked = 0, nUnlinked = 0;   // estadística

  auto init() -> bool;
  auto find(u32 phys) -> s32;               // idx o -1
  auto insert(u32 phys, Block&& b) -> s32;  // devuelve idx
  auto clear() -> void;                      // vacía todo (buffer lleno / invalidación global)

  // Registra un sitio de enlace (desenlazado). Lo resuelve al vuelo si el destino ya existe.
  auto addLink(const LinkSite& s) -> void;
  // Activa todos los sitios que apuntan a `phys` para que salten a `entry`.
  auto linkTo(u32 phys, u8* entry) -> void;
  // Desactiva todos los sitios que apuntan a `phys` (bloque muerto o recompilado).
  auto unlinkTo(u32 phys) -> void;
  // Desactiva TODOS los sitios: lo exige una invalidación de I-cache, que es la única vía por
  // la que el HW puede pasar a ejecutar código nuevo bajo una dirección ya ejecutada. Sube
  // linkEpoch para que cada bloque se re-enlace en su próxima entrada VALIDADA por el driver.
  auto unlinkAll() -> void;
};

}  // namespace kestrel::jit
