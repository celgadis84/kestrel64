// Standalone unit test for the cartridge save devices (SRAM + FlashRAM). Drives the
// real bus paths (Memory::read32/write32 → saveRead/saveWrite/flashCommand) through
// KSEG1 addresses, exactly as guest code would, and checks round-trips + the FlashRAM
// command state machine. EEPROM is validated end-to-end by SM64 boot, not here.
#include "../src/core/memory.hpp"
#include <cstdio>

using namespace kestrel;

static int failures = 0;
static void chk(const char* what, u32 got, u32 exp) {
  bool ok = got == exp;
  std::printf("  %-28s got=0x%08x exp=0x%08x  %s\n", what, got, exp, ok ? "OK" : "*** FAIL ***");
  if(!ok) failures++;
}

// KSEG1 (uncached) view of PI domain 2. read32/write32 mask off the segment bits, so
// this resolves to physical 0x08000000+.
static constexpr u32 SAVE = 0xA800'0000;
static constexpr u32 FCMD = 0xA801'0000;  // FlashRAM command register

static void useType(Memory& m, Memory::SaveType t) {
  m.saveType = t;
  m.saveRam.assign(m.saveSize(), m.isFlash() ? 0xFF : 0x00);
  m.flashMode = Memory::FlashMode::Status;
}

int main() {
  Memory m;
  m.reset(/*expansionPak=*/true);   // sizes RDRAM etc.; rom stays empty

  std::printf("SRAM 256k linear round-trip:\n");
  useType(m, Memory::SaveType::Sram256k);
  chk("saveSize", m.saveSize(), 32 * 1024);
  m.write32(SAVE + 0x100, 0xDEADBEEF);
  chk("word read-back", m.read32(SAVE + 0x100), 0xDEADBEEF);
  chk("backing byte 0x100", m.saveRam[0x100], 0xDE);
  chk("backing byte 0x103", m.saveRam[0x103], 0xEF);
  m.write8(SAVE + 0x200, 0x5A);
  chk("byte read-back", m.read8(SAVE + 0x200), 0x5A);
  m.write16(SAVE + 0x300, 0x1234);
  chk("half read-back", m.read16(SAVE + 0x300), 0x1234);

  std::printf("SRAM 768k three-bank mapping:\n");
  useType(m, Memory::SaveType::Sram768k);
  chk("saveSize", m.saveSize(), 96 * 1024);
  m.write32(SAVE + 0x0000'0004, 0xAAAA0001);  // bank 0
  m.write32(SAVE + 0x0001'0004, 0xBBBB0002);  // bank 1 (0x08010000)
  m.write32(SAVE + 0x0002'0004, 0xCCCC0003);  // bank 2 (0x08020000)
  chk("bank0 folded @0x00004", m.read32(SAVE + 0x0000'0004), 0xAAAA0001);
  chk("bank1 folded @0x08004", (u32)((m.saveRam[0x8004] << 24) | (m.saveRam[0x8005] << 16) |
                                     (m.saveRam[0x8006] << 8) | m.saveRam[0x8007]), 0xBBBB0002);
  chk("bank2 folded @0x10004", (u32)((m.saveRam[0x10004] << 24) | (m.saveRam[0x10005] << 16) |
                                     (m.saveRam[0x10006] << 8) | m.saveRam[0x10007]), 0xCCCC0003);

  std::printf("FlashRAM status / silicon id:\n");
  useType(m, Memory::SaveType::Flash1m);
  chk("saveSize", m.saveSize(), 128 * 1024);
  m.write32(FCMD, 0xE1000000);                 // status mode
  chk("id hi word", m.read32(SAVE + 0), 0x11118001);
  chk("id lo word", m.read32(SAVE + 4), 0x00C2001E);

  std::printf("FlashRAM page write + read-back:\n");
  m.write32(FCMD, 0xB4000000);                 // write mode (stage page buffer)
  m.write32(SAVE + 0, 0x11223344);             // fill buffer bytes 0..3
  m.write32(SAVE + 4, 0x55667788);             // bytes 4..7
  m.write32(FCMD, 0xA5000002);                 // set write page = 2 (offset 256)
  m.write32(FCMD, 0xD2000000);                 // execute → commit buffer to flash[256]
  chk("committed @256", (u32)((m.saveRam[256] << 24) | (m.saveRam[257] << 16) |
                              (m.saveRam[258] << 8) | m.saveRam[259]), 0x11223344);
  m.write32(FCMD, 0xF0000000);                 // read-array mode
  chk("read-array @256", m.read32(SAVE + 256), 0x11223344);
  chk("read-array @260", m.read32(SAVE + 260), 0x55667788);

  std::printf("FlashRAM sector erase → 0xFF:\n");
  m.saveRam[256] = 0x00;                        // dirty a byte inside the sector
  m.write32(FCMD, 0x78000000);                  // erase mode
  m.write32(FCMD, 0x4B000000);                  // erase page 0 (sector 0 = pages 0..127)
  m.write32(FCMD, 0xD2000000);                  // execute → erase 16 KiB sector
  chk("erased @256", m.saveRam[256], 0xFF);
  chk("erased @0", m.saveRam[0], 0xFF);
  chk("erased @16383", m.saveRam[16383], 0xFF);

  std::printf("Persistence round-trip (.sra load/flush):\n");
  {
    const char* romp = "save_test_tmp.z64";   // → save_test_tmp.sra beside it
    std::remove("save_test_tmp.sra");
    Memory a;
    a.reset(true);
    useType(a, Memory::SaveType::Sram256k);
    a.attachSaveFile(romp);                    // no file yet → backing stays zero
    a.write32(SAVE + 0x40, 0xCAFEF00D);        // guest writes → marks dirty
    chk("pre-flush dirty", a.saveDirty ? 1 : 0, 1);
    a.flushSaveFile();                          // writes save_test_tmp.sra

    Memory b;
    b.reset(true);
    useType(b, Memory::SaveType::Sram256k);
    b.attachSaveFile(romp);                    // loads the .sra written above
    chk("reloaded word @0x40", b.read32(SAVE + 0x40), 0xCAFEF00D);
    chk("untouched byte @0", b.read8(SAVE + 0), 0x00);
    std::remove("save_test_tmp.sra");
  }

  std::printf("\n%s (%d failures)\n", failures ? "FAILED" : "ALL PASS", failures);
  return failures ? 1 : 0;
}
