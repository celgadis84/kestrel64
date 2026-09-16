// Standalone unit test for the RSP LLE core. Hand-assembles a tiny microcode
// into IMEM, seeds DMEM, kicks the RSP via SP_STATUS exactly as the CPU would,
// and checks the vector-unit results against values computed by hand. Runs in
// milliseconds — decoupled from full-system boot speed.
#include "../src/core/memory.hpp"
#include <cstdio>
#include <cstdint>
#include <vector>

using namespace kestrel;

static int failures = 0;
static void chk(const char* what, u32 got, u32 exp) {
  bool ok = got == exp;
  std::printf("  %-22s got=0x%08x exp=0x%08x  %s\n", what, got, exp, ok ? "OK" : "*** FAIL ***");
  if(!ok) failures++;
}

static void putImem(Memory& m, u32 idx, u32 word) {
  u32 a = idx * 4;
  m.imem[a + 0] = word >> 24; m.imem[a + 1] = word >> 16;
  m.imem[a + 2] = word >> 8;  m.imem[a + 3] = word;
}
static void putLaneS16(Memory& m, u32 byteoff, const int16_t* lanes) {
  for(int n = 0; n < 8; n++) {
    u16 v = (u16)lanes[n];
    m.dmem[byteoff + n * 2 + 0] = v >> 8;
    m.dmem[byteoff + n * 2 + 1] = v & 0xff;
  }
}
static u16 getLane(Memory& m, u32 byteoff, int n) {
  return (u16)(m.dmem[byteoff + n * 2] << 8 | m.dmem[byteoff + n * 2 + 1]);
}

int main() {
  Memory mem;
  mem.reset();

  // --- microcode ------------------------------------------------------------
  u32 p[] = {
    0xC8012000,  // 0  LQV  v1, dmem[0]
    0xC8022001,  // 1  LQV  v2, dmem[16]
    0x4A0208D0,  // 2  VADD v3, v1, v2
    0xE8032002,  // 3  SQV  v3, dmem[32]
    0x4A020907,  // 4  VMUDH v4, v1, v2
    0xE8042003,  // 5  SQV  v4, dmem[48]
    0x4B20015D,  // 6  VSAR v5, accm (e=9)
    0xE8052004,  // 7  SQV  v5, dmem[64]
    0x4A0201B0,  // 8  VRCP v6, v2[0]     (reciprocal of 10 -> ~0.1)
    0xE8062006,  // 9  SQV  v6, dmem[96]
    0x24060000,  // 10 addiu r6, r0, 0
    0x24070005,  // 11 addiu r7, r0, 5
    0x24C60001,  // 12 loop: addiu r6, r6, 1
    0x14C7FFFE,  // 13 bne r6, r7, loop
    0xAC060050,  // 14 sw r6, dmem[80]   (delay slot)
    0x0000000D,  // 15 BREAK
  };
  for(u32 i = 0; i < sizeof(p) / 4; i++) putImem(mem, i, p[i]);

  int16_t v1[8] = { 1, 2, 3, 4, -1, -2, 100, 30000 };
  int16_t v2[8] = { 10, 20, 30, 40, -5, 32767, 5, 30000 };
  putLaneS16(mem, 0,  v1);
  putLaneS16(mem, 16, v2);

  // --- kick the RSP (CPU clears HALT via SP_STATUS write) -------------------
  mem.rcp.sp_pc = 0;
  mem.rcp.sp_status = 1;                 // halted
  mem.rsp.mem = &mem;
  mem.write32(0x0404'0010, 1);           // SP_STATUS: clear HALT -> arma el nucleo
  // En modo Lockstep (el de este test, sin System) la escritura solo ARMA el RSP: quien
  // lo hace avanzar es System::run intercalando pasos. Aqui no hay System, asi que se le
  // da un paso sin tope, que es lo que hace KESTREL_RSPINLINE: corre hasta su BREAK.
  mem.rsp.step(~0ull);

  std::printf("sp_status after run = 0x%08x (expect HALT|BROKE = 0x3)\n", mem.rcp.sp_status.load());
  chk("halt+broke", mem.rcp.sp_status & 3, 3);

  // --- expected results -----------------------------------------------------
  auto clamps16 = [](long x) -> u16 { if(x < -32768) x = -32768; if(x > 32767) x = 32767; return (u16)(int16_t)x; };
  std::printf("VADD (dmem[32]):\n");
  for(int n = 0; n < 8; n++) {
    u16 exp = clamps16((long)v1[n] + v2[n]);
    char nm[24]; std::snprintf(nm, sizeof nm, "vadd lane%d", n);
    chk(nm, getLane(mem, 32, n), exp);
  }
  std::printf("VMUDH (dmem[48]):\n");
  for(int n = 0; n < 8; n++) {
    u16 exp = clamps16((long)v1[n] * v2[n]);
    char nm[24]; std::snprintf(nm, sizeof nm, "vmudh lane%d", n);
    chk(nm, getLane(mem, 48, n), exp);
  }
  std::printf("VSAR accm (dmem[64]):\n");
  for(int n = 0; n < 8; n++) {
    u16 exp = (u16)(((long)v1[n] * v2[n]) & 0xffff);
    char nm[24]; std::snprintf(nm, sizeof nm, "vsar lane%d", n);
    chk(nm, getLane(mem, 64, n), exp);
  }
  std::printf("VRCP (dmem[96]): rcp(10) ~ 0.1 -> 0x0CCC in high, low16 0xC800\n");
  chk("vrcp low16", getLane(mem, 96, 0), 0xC800);
  std::printf("scalar loop (dmem[80]):\n");
  u32 loopres = (u32)mem.dmem[80] << 24 | mem.dmem[81] << 16 | mem.dmem[82] << 8 | mem.dmem[83];
  chk("loop count", loopres, 5);

  std::printf("\n%s (%d failures)\n", failures ? "FAILED" : "ALL PASS", failures);
  return failures ? 1 : 0;
}
