// Standalone unit test for the PIF-NUS side of the SI: the PIF RAM control byte and the
// CIC-NUS-6105 anti-piracy challenge.
//
// PIF RAM byte 0x3F is not data. It is the mailbox the CPU uses to ask the PIF for work,
// and the PIF clears each bit as it takes the job:
//
//   bit 0  run the joybus command block (libultra's CONT_CMD_EXE, written into `pifstatus`
//          before every 64-byte write); consumed when the block ARRIVES, i.e. on the
//          RDRAM -> PIF DMA.
//   bit 1  CIC challenge; served on the PIF -> RDRAM DMA, and it is NOT a joybus
//          transaction -- the PIF talks to the cart's CIC over its own serial line, so the
//          controllers are not polled during that read.
//
// Everything here drives the real entry point (the SI registers), not the internals, so a
// regression in the register decode shows up too. The challenge is checked against a golden
// vector: the same 30 nibbles pushed through an independent transcription of the published
// 6105 algorithm, so a typo in either the table or the state machine fails the test rather
// than silently producing a plausible-looking answer.
#include "../src/core/memory.hpp"
#include <cstdio>
#include <cstring>
#include <memory>

using namespace kestrel;

static int failures = 0;

static auto check(const char* name, bool ok, const char* detail = nullptr) -> void {
  if(ok) { std::printf("  ok   %s\n", name); return; }
  ++failures;
  std::printf("  FAIL %s%s%s\n", name, detail ? " -- " : "", detail ? detail : "");
}

// --- SI register addresses (physical) ---------------------------------------------
static constexpr u32 SI_DRAM_ADDR      = 0x0480'0000;
static constexpr u32 SI_PIF_ADDR_RD64B = 0x0480'0004;   // PIF RAM -> RDRAM (PIF serves first)
static constexpr u32 SI_PIF_ADDR_WR64B = 0x0480'0010;   // RDRAM -> PIF RAM
static constexpr u32 DRAM_BLOCK        = 0x0000'2000;   // scratch for the 64-byte block

static auto makeMem() -> std::unique_ptr<Memory> {
  auto m = std::make_unique<Memory>();
  m->reset();
  return m;
}

// Push the 64 bytes currently in `blk` through the real RDRAM -> PIF path.
static auto writeBlock(Memory& m, const u8 blk[64]) -> void {
  for(int i = 0; i < 64; i++) m.rdram[DRAM_BLOCK + i] = blk[i];
  m.write32(SI_DRAM_ADDR, DRAM_BLOCK);
  m.write32(SI_PIF_ADDR_WR64B, 0);
}

// Ask the PIF to serve the block. The copy back to RDRAM happens when the SI deadline
// expires, which this test does not need: what it checks is what the PIF left in PIF RAM.
static auto readBlock(Memory& m) -> void {
  m.write32(SI_DRAM_ADDR, DRAM_BLOCK);
  m.write32(SI_PIF_ADDR_RD64B, 0);
}

// Golden vector for the CIC-NUS-6105 challenge: 30 nibbles in, 30 nibbles out.
static const char* kChallengeIn  = "3a18f6d4b2907e5c3a18f6d4b2907e";
static const char* kChallengeOut = "aadba6fda271aef54091c4fda271ae";

static auto nibblesToPif(Memory& m, const char* hex) -> void {
  auto val = [](char c) -> u8 { return (u8)(c <= '9' ? c - '0' : c - 'a' + 10); };
  for(int i = 0; i < 15; i++) m.pifram[0x30 + i] = (u8)(val(hex[i * 2]) << 4 | val(hex[i * 2 + 1]));
}

static auto pifToNibbles(Memory& m, char out[31]) -> void {
  static const char* d = "0123456789abcdef";
  for(int i = 0; i < 15; i++) {
    out[i * 2]     = d[m.pifram[0x30 + i] >> 4];
    out[i * 2 + 1] = d[m.pifram[0x30 + i] & 0xf];
  }
  out[30] = 0;
}

auto main() -> int {
  std::printf("pif_test\n");

  // --- 1. The challenge on a 6105 cart matches the published algorithm --------------
  {
    auto m = makeMem();
    m->cic6105 = true;
    nibblesToPif(*m, kChallengeIn);
    m->pifram[63] = 0x02;
    readBlock(*m);
    char got[31]; pifToNibbles(*m, got);
    check("challenge 6105 == vector de oro", std::strcmp(got, kChallengeOut) == 0, got);
    check("challenge apaga el bit 1 del byte de control", (m->pifram[63] & 0x02) == 0);
  }

  // --- 2. A cart that is not 6105 gets no answer invented for it ---------------------
  {
    auto m = makeMem();
    m->cic6105 = false;
    nibblesToPif(*m, kChallengeIn);
    m->pifram[63] = 0x02;
    readBlock(*m);
    char got[31]; pifToNibbles(*m, got);
    check("challenge sin 6105 deja el bloque intacto", std::strcmp(got, kChallengeIn) == 0, got);
  }

  // --- 3. The challenge is not a joybus transaction ----------------------------------
  // A status command for controller 1 sits in the block. Serving the challenge must leave
  // it untouched: the PIF is talking to the CIC, not to the port.
  {
    auto m = makeMem();
    m->cic6105 = true;
    m->padPort[0].connected = true;
    u8 blk[64]; std::memset(blk, 0xFF, sizeof blk);
    blk[0] = 0x01; blk[1] = 0x03; blk[2] = 0x00;        // tx=1 rx=3, cmd 0x00 (status)
    blk[3] = blk[4] = blk[5] = 0xAA;                     // reply area, poisoned
    blk[6] = 0xFE;                                       // end of commands
    blk[63] = 0x02;                                      // challenge, NOT execute
    writeBlock(*m, blk);
    readBlock(*m);
    check("el desafio no sondea el joybus",
          m->pifram[3] == 0xAA && m->pifram[4] == 0xAA && m->pifram[5] == 0xAA);
  }

  // --- 4. A normal read does run the joybus ------------------------------------------
  {
    auto m = makeMem();
    m->padPort[0].connected = true;
    m->padPort[0].accessory = 0;
    u8 blk[64]; std::memset(blk, 0xFF, sizeof blk);
    blk[0] = 0x01; blk[1] = 0x03; blk[2] = 0x00;
    blk[3] = blk[4] = blk[5] = 0xAA;
    blk[6] = 0xFE;
    blk[63] = 0x01;                                      // execute
    writeBlock(*m, blk);
    check("el bit 0 se consume al recibir el bloque", (m->pifram[63] & 0x01) == 0);
    readBlock(*m);
    // Controller type 0x0005, and no accessory in the slot.
    check("status del mando 1", m->pifram[3] == 0x05 && m->pifram[4] == 0x00 && m->pifram[5] == 0x00);
  }

  // --- 5. The control byte's other bits are left alone --------------------------------
  // Only the two the PIF serves here may change; anything else the CPU wrote has to survive
  // the round trip, or a game that packs flags in there reads back something it never wrote.
  // In NORMAL mode: during boot (reset mode) 0x10/0x20 are PIF commands, see section 6.
  {
    auto m = makeMem();
    m->pifBootMode = false;
    u8 blk[64]; std::memset(blk, 0xFF, sizeof blk);
    blk[0] = 0xFE;
    blk[63] = 0x31;                                      // 0x30 (bits ajenos) + 0x01 (ejecutar)
    writeBlock(*m, blk);
    check("solo se apaga el bit 0", m->pifram[63] == 0x30);
  }

  // --- 6. Boot (reset) mode: the IPL2/IPL3 command sequence ----------------------------
  // n64brew PIF-NUS: 0x10 locks the PIF ROM, 0x20 makes the PIF take the 6 checksum bytes at
  // 0x32 (it wipes them and answers 0x80), 0x40 compares them with the CIC's, 0x08 ends the
  // boot. Driven through a CPU SW to the control word, which is how libultra sends 0x08.
  static constexpr u32 PIF_CTRL_WORD = 0x1fc0'07fc;
  static constexpr u64 kSum6102 = 0xA536'C0F1'D859ull;
  auto putSum = [](Memory& m, u64 sum) { for(int i = 0; i < 6; i++) m.pifram[0x32 + i] = (u8)(sum >> (8 * (5 - i))); };
  {
    auto m = makeMem();
    m->cicSum = kSum6102;
    check("encendido en modo reset", m->pifBootMode && !m->pifRomLocked && !m->pifFrozen);
    m->write32(PIF_CTRL_WORD, 0x10);
    check("0x10 bloquea la PIF ROM y se consume", m->pifRomLocked && m->pifram[63] == 0x00);
    putSum(*m, kSum6102);
    m->write32(PIF_CTRL_WORD, 0x20);
    bool wiped = true; for(int i = 0; i < 6; i++) wiped &= m->pifram[0x32 + i] == 0;
    check("0x20 recoge el checksum, lo borra y contesta 0x80", m->pifSumGot && wiped && m->pifram[63] == 0x80);
    m->write32(PIF_CTRL_WORD, 0x40);
    check("0x40 con checksum bueno no congela", !m->pifFrozen && m->pifram[63] == 0x00);
    m->write32(PIF_CTRL_WORD, 0x08);
    check("0x08 sale del modo reset y se consume", !m->pifBootMode && m->pifram[63] == 0x00);
    m->write32(PIF_CTRL_WORD, 0x08);
    check("en modo normal 0x08 es bandera del joybus y se queda", m->pifram[63] == 0x08 && !m->pifFrozen);
  }

  // --- 7. A wrong checksum freezes the console ---------------------------------------
  {
    auto m = makeMem();
    m->cicSum = kSum6102;
    putSum(*m, kSum6102 ^ 1);
    m->write32(PIF_CTRL_WORD, 0x20);
    m->write32(PIF_CTRL_WORD, 0x40);
    check("0x40 con checksum malo congela", m->pifFrozen);
  }

  // --- 8. HLE boot leaves the PIF where IPL2 leaves it --------------------------------
  {
    auto m = makeMem();
    m->pifBootHle(kSum6102);
    check("tras IPL2 HLE: ROM bloqueada, checksum entregado, aun modo reset",
          m->pifRomLocked && m->pifSumGot && m->pifBootMode);
    m->write32(PIF_CTRL_WORD, 0x40);
    check("re-verificar tras IPL2 HLE no congela", !m->pifFrozen);
  }

  // --- 9. Unknown CIC (homebrew IPL3): no verdict invented -----------------------------
  {
    auto m = makeMem();
    m->pifBootHle(0);
    putSum(*m, 0x1234'5678'9abcull);
    m->write32(PIF_CTRL_WORD, 0x20);
    m->write32(PIF_CTRL_WORD, 0x40);
    check("CIC desconocido: 0x40 no congela", !m->pifFrozen);
  }

  // --- 10. The 0x08 deadline: ~5 s of guest time from power-on ---------------------
  // Driven by the VI field clock, the same one the run loop uses.
  {
    auto m = makeMem();
    const u64 dl = m->usToInsns(Memory::kPifBootUs);
    u64 t = 0, step = dl / 4000 + 1;
    bool frozeEarly = false;
    while(t < dl * 2 && !m->pifFrozen) {
      t += step; m->viTick(t);
      if(m->pifFrozen && t < dl) frozeEarly = true;
    }
    check("sin 0x08 la consola se congela al vencer el plazo", m->pifFrozen && !frozeEarly);
    auto m2 = makeMem();
    m2->write32(PIF_CTRL_WORD, 0x08);
    for(u64 u = 0; u < dl * 2; u += step) m2->viTick(u);
    check("con 0x08 a tiempo no se congela", !m2->pifFrozen);
  }

  std::printf(failures ? "FAILURES: %d\n" : "ALL PASS\n", failures);
  return failures ? 1 : 0;
}
