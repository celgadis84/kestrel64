// MIPS III disassembler for telemetry and the debugger (CPU::disasm).
// Split out of cpu.cpp.
#include "cpu.hpp"
#include <cstdio>
#include <string>

namespace kestrel {

// --- disassembler (compact, telemetry-only) ----------------------------------
static const char* gprName[32] = {
  "zero","at","v0","v1","a0","a1","a2","a3","t0","t1","t2","t3","t4","t5","t6","t7",
  "s0","s1","s2","s3","s4","s5","s6","s7","t8","t9","k0","k1","gp","sp","fp","ra"};

auto CPU::disasm(u32 op, u64 pc) -> std::string {
  // Telemetria, no una herramienta de matching: aqui solo importa que quien mira un cuelgue
  // LEA lo que hay. La tabla estaba a medias y un `sb` salia como "op.28", que es justo el
  // agujero que hace perder una hora en un diagnostico.
  char b[80];
  u32 o = op >> 26 & 0x3f, rs = op>>21&0x1f, rt = op>>16&0x1f, rd = op>>11&0x1f, sa = op>>6&0x1f, fn = op&0x3f;
  s16 imm = (s16)(op & 0xffff);
  auto R = [](u32 i){ return gprName[i]; };
  auto F = [](u32 i){ static char f[8]; std::snprintf(f,sizeof f,"f%u",i); return (const char*)f; };
  auto target = [&]{ return (u32)((pc & 0xf0000000) | ((op & 0x3ffffff) << 2)); };
  auto branch = [&]{ return (u32)(pc + 4 + ((s32)imm << 2)); };
  auto mem  = [&](const char* n){ std::snprintf(b,sizeof b,"%s %s,%d(%s)",n,R(rt),imm,R(rs)); return b; };
  auto memf = [&](const char* n){ std::snprintf(b,sizeof b,"%s %s,%d(%s)",n,F(rt),imm,R(rs)); return b; };
  if(op == 0) return "nop";
  switch(o) {
  case 0x00:
    switch(fn) {
    case 0x00: std::snprintf(b,sizeof b,"sll %s,%s,%u",R(rd),R(rt),sa); return b;
    case 0x02: std::snprintf(b,sizeof b,"srl %s,%s,%u",R(rd),R(rt),sa); return b;
    case 0x03: std::snprintf(b,sizeof b,"sra %s,%s,%u",R(rd),R(rt),sa); return b;
    case 0x04: std::snprintf(b,sizeof b,"sllv %s,%s,%s",R(rd),R(rt),R(rs)); return b;
    case 0x06: std::snprintf(b,sizeof b,"srlv %s,%s,%s",R(rd),R(rt),R(rs)); return b;
    case 0x07: std::snprintf(b,sizeof b,"srav %s,%s,%s",R(rd),R(rt),R(rs)); return b;
    case 0x08: std::snprintf(b,sizeof b,"jr %s",R(rs)); return b;
    case 0x09: std::snprintf(b,sizeof b,"jalr %s,%s",R(rd),R(rs)); return b;
    case 0x0c: return "syscall";
    case 0x0d: return "break";
    case 0x0f: return "sync";
    case 0x10: std::snprintf(b,sizeof b,"mfhi %s",R(rd)); return b;
    case 0x11: std::snprintf(b,sizeof b,"mthi %s",R(rs)); return b;
    case 0x12: std::snprintf(b,sizeof b,"mflo %s",R(rd)); return b;
    case 0x13: std::snprintf(b,sizeof b,"mtlo %s",R(rs)); return b;
    case 0x14: std::snprintf(b,sizeof b,"dsllv %s,%s,%s",R(rd),R(rt),R(rs)); return b;
    case 0x16: std::snprintf(b,sizeof b,"dsrlv %s,%s,%s",R(rd),R(rt),R(rs)); return b;
    case 0x17: std::snprintf(b,sizeof b,"dsrav %s,%s,%s",R(rd),R(rt),R(rs)); return b;
    case 0x18: case 0x19: std::snprintf(b,sizeof b,"mult%s %s,%s",fn==0x19?"u":"",R(rs),R(rt)); return b;
    case 0x1a: case 0x1b: std::snprintf(b,sizeof b,"div%s %s,%s",fn==0x1b?"u":"",R(rs),R(rt)); return b;
    case 0x1c: case 0x1d: std::snprintf(b,sizeof b,"dmult%s %s,%s",fn==0x1d?"u":"",R(rs),R(rt)); return b;
    case 0x1e: case 0x1f: std::snprintf(b,sizeof b,"ddiv%s %s,%s",fn==0x1f?"u":"",R(rs),R(rt)); return b;
    case 0x20: case 0x21: std::snprintf(b,sizeof b,"add%s %s,%s,%s",fn==0x21?"u":"",R(rd),R(rs),R(rt)); return b;
    case 0x22: case 0x23: std::snprintf(b,sizeof b,"sub%s %s,%s,%s",fn==0x23?"u":"",R(rd),R(rs),R(rt)); return b;
    case 0x24: std::snprintf(b,sizeof b,"and %s,%s,%s",R(rd),R(rs),R(rt)); return b;
    case 0x25: std::snprintf(b,sizeof b,"or %s,%s,%s",R(rd),R(rs),R(rt)); return b;
    case 0x26: std::snprintf(b,sizeof b,"xor %s,%s,%s",R(rd),R(rs),R(rt)); return b;
    case 0x27: std::snprintf(b,sizeof b,"nor %s,%s,%s",R(rd),R(rs),R(rt)); return b;
    case 0x2a: std::snprintf(b,sizeof b,"slt %s,%s,%s",R(rd),R(rs),R(rt)); return b;
    case 0x2b: std::snprintf(b,sizeof b,"sltu %s,%s,%s",R(rd),R(rs),R(rt)); return b;
    case 0x2c: case 0x2d: std::snprintf(b,sizeof b,"dadd%s %s,%s,%s",fn==0x2d?"u":"",R(rd),R(rs),R(rt)); return b;
    case 0x2e: case 0x2f: std::snprintf(b,sizeof b,"dsub%s %s,%s,%s",fn==0x2f?"u":"",R(rd),R(rs),R(rt)); return b;
    case 0x38: std::snprintf(b,sizeof b,"dsll %s,%s,%u",R(rd),R(rt),sa); return b;
    case 0x3a: std::snprintf(b,sizeof b,"dsrl %s,%s,%u",R(rd),R(rt),sa); return b;
    case 0x3b: std::snprintf(b,sizeof b,"dsra %s,%s,%u",R(rd),R(rt),sa); return b;
    case 0x3c: std::snprintf(b,sizeof b,"dsll32 %s,%s,%u",R(rd),R(rt),sa); return b;
    case 0x3e: std::snprintf(b,sizeof b,"dsrl32 %s,%s,%u",R(rd),R(rt),sa); return b;
    case 0x3f: std::snprintf(b,sizeof b,"dsra32 %s,%s,%u",R(rd),R(rt),sa); return b;
    default: std::snprintf(b,sizeof b,"special.%02x",fn); return b;
    }
  case 0x01: {  // REGIMM: el sub-opcode va en el campo rt
    static const char* rn[32] = {
      "bltz","bgez","bltzl","bgezl","?","?","?","?","tgei","tgeiu","tlti","tltiu","teqi","?","tnei","?",
      "bltzal","bgezal","bltzall","bgezall","?","?","?","?","?","?","?","?","?","?","?","?"};
    std::snprintf(b,sizeof b,"%s %s,%08x",rn[rt],R(rs),branch()); return b;
  }
  case 0x02: std::snprintf(b,sizeof b,"j %08x",target()); return b;
  case 0x03: std::snprintf(b,sizeof b,"jal %08x",target()); return b;
  case 0x04: std::snprintf(b,sizeof b,"beq %s,%s,%08x",R(rs),R(rt),branch()); return b;
  case 0x05: std::snprintf(b,sizeof b,"bne %s,%s,%08x",R(rs),R(rt),branch()); return b;
  case 0x06: std::snprintf(b,sizeof b,"blez %s,%08x",R(rs),branch()); return b;
  case 0x07: std::snprintf(b,sizeof b,"bgtz %s,%08x",R(rs),branch()); return b;
  case 0x08: case 0x09: std::snprintf(b,sizeof b,"addi%s %s,%s,%d",o==0x09?"u":"",R(rt),R(rs),imm); return b;
  case 0x0a: std::snprintf(b,sizeof b,"slti %s,%s,%d",R(rt),R(rs),imm); return b;
  case 0x0b: std::snprintf(b,sizeof b,"sltiu %s,%s,%d",R(rt),R(rs),imm); return b;
  case 0x0c: std::snprintf(b,sizeof b,"andi %s,%s,0x%x",R(rt),R(rs),(u16)imm); return b;
  case 0x0d: std::snprintf(b,sizeof b,"ori %s,%s,0x%x",R(rt),R(rs),(u16)imm); return b;
  case 0x0e: std::snprintf(b,sizeof b,"xori %s,%s,0x%x",R(rt),R(rs),(u16)imm); return b;
  case 0x0f: std::snprintf(b,sizeof b,"lui %s,0x%x",R(rt),(u16)imm); return b;
  case 0x10:   // COP0
    switch(rs) {
    case 0x00: std::snprintf(b,sizeof b,"mfc0 %s,r%u",R(rt),rd); return b;
    case 0x01: std::snprintf(b,sizeof b,"dmfc0 %s,r%u",R(rt),rd); return b;
    case 0x04: std::snprintf(b,sizeof b,"mtc0 %s,r%u",R(rt),rd); return b;
    case 0x05: std::snprintf(b,sizeof b,"dmtc0 %s,r%u",R(rt),rd); return b;
    case 0x10:
      switch(fn) {
      case 0x01: return "tlbr";
      case 0x02: return "tlbwi";
      case 0x06: return "tlbwr";
      case 0x08: return "tlbp";
      case 0x18: return "eret";
      default:   std::snprintf(b,sizeof b,"cop0.co.%02x",fn); return b;
      }
    default: std::snprintf(b,sizeof b,"cop0.%02x",rs); return b;
    }
  case 0x11:   // COP1
    switch(rs) {
    case 0x00: std::snprintf(b,sizeof b,"mfc1 %s,%s",R(rt),F(rd)); return b;
    case 0x01: std::snprintf(b,sizeof b,"dmfc1 %s,%s",R(rt),F(rd)); return b;
    case 0x02: std::snprintf(b,sizeof b,"cfc1 %s,%u",R(rt),rd); return b;
    case 0x04: std::snprintf(b,sizeof b,"mtc1 %s,%s",R(rt),F(rd)); return b;
    case 0x05: std::snprintf(b,sizeof b,"dmtc1 %s,%s",R(rt),F(rd)); return b;
    case 0x06: std::snprintf(b,sizeof b,"ctc1 %s,%u",R(rt),rd); return b;
    case 0x08: std::snprintf(b,sizeof b,"bc1%s%s %08x",(rt&1)?"t":"f",(rt&2)?"l":"",branch()); return b;
    default: {
      static const char* fmt[32] = {"s","d","?","?","w","l","?","?","?","?","?","?","?","?","?","?",
                                    "?","?","?","?","?","?","?","?","?","?","?","?","?","?","?","?"};
      static const char* fop[64] = {
        "add","sub","mul","div","sqrt","abs","mov","neg","round.l","trunc.l","ceil.l","floor.l",
        "round.w","trunc.w","ceil.w","floor.w","?","?","?","?","?","?","?","?","?","?","?","?","?","?","?","?",
        "cvt.s","cvt.d","?","?","cvt.w","cvt.l","?","?","?","?","?","?","?","?","?","?",
        "c.f","c.un","c.eq","c.ueq","c.olt","c.ult","c.ole","c.ule",
        "c.sf","c.ngle","c.seq","c.ngl","c.lt","c.nge","c.le","c.ngt"};
      std::snprintf(b,sizeof b,"%s.%s %s,%s,%s",fop[fn],fmt[rs&0x1f],F(sa),F(rd),F(rt)); return b;
    }
    }
  case 0x14: std::snprintf(b,sizeof b,"beql %s,%s,%08x",R(rs),R(rt),branch()); return b;
  case 0x15: std::snprintf(b,sizeof b,"bnel %s,%s,%08x",R(rs),R(rt),branch()); return b;
  case 0x16: std::snprintf(b,sizeof b,"blezl %s,%08x",R(rs),branch()); return b;
  case 0x17: std::snprintf(b,sizeof b,"bgtzl %s,%08x",R(rs),branch()); return b;
  case 0x18: case 0x19: std::snprintf(b,sizeof b,"daddi%s %s,%s,%d",o==0x19?"u":"",R(rt),R(rs),imm); return b;
  case 0x1a: return mem("ldl");
  case 0x1b: return mem("ldr");
  case 0x20: return mem("lb");
  case 0x21: return mem("lh");
  case 0x22: return mem("lwl");
  case 0x23: return mem("lw");
  case 0x24: return mem("lbu");
  case 0x25: return mem("lhu");
  case 0x26: return mem("lwr");
  case 0x27: return mem("lwu");
  case 0x28: return mem("sb");
  case 0x29: return mem("sh");
  case 0x2a: return mem("swl");
  case 0x2b: return mem("sw");
  case 0x2c: return mem("sdl");
  case 0x2d: return mem("sdr");
  case 0x2e: return mem("swr");
  case 0x2f: std::snprintf(b,sizeof b,"cache 0x%x,%d(%s)",rt,imm,R(rs)); return b;
  case 0x30: return mem("ll");
  case 0x31: return memf("lwc1");
  case 0x34: return mem("lld");
  case 0x35: return memf("ldc1");
  case 0x37: return mem("ld");
  case 0x38: return mem("sc");
  case 0x39: return memf("swc1");
  case 0x3c: return mem("scd");
  case 0x3d: return memf("sdc1");
  case 0x3f: return mem("sd");
  default: std::snprintf(b,sizeof b,"op.%02x",o); return b;
  }
}


}  // namespace kestrel
