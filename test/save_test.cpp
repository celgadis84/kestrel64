// Standalone unit test for the cartridge save devices (SRAM + FlashRAM). Drives the
// real bus paths (Memory::read32/write32 → saveRead/saveWrite/flashCommand) through
// KSEG1 addresses, exactly as guest code would, and checks round-trips + the FlashRAM
// command state machine. EEPROM is validated end-to-end by SM64 boot, not here.
#include "../src/core/memory.hpp"
#include <cstdio>
#include <cstring>
#include <memory>

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
  // Memory ya no cabe varias veces en la pila de Windows (1 MB): todas al monton.
  auto mp = std::make_unique<Memory>();
  Memory& m = *mp;
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
    auto ap = std::make_unique<Memory>();
    Memory& a = *ap;
    a.reset(true);
    useType(a, Memory::SaveType::Sram256k);
    a.attachSaveFile(romp);                    // no file yet → backing stays zero
    a.write32(SAVE + 0x40, 0xCAFEF00D);        // guest writes → marks dirty
    chk("pre-flush dirty", a.saveDirty ? 1 : 0, 1);
    a.flushSaveFile();                          // writes save_test_tmp.sra

    auto bp = std::make_unique<Memory>();
    Memory& b = *bp;
    b.reset(true);
    useType(b, Memory::SaveType::Sram256k);
    b.attachSaveFile(romp);                    // loads the .sra written above
    chk("reloaded word @0x40", b.read32(SAVE + 0x40), 0xCAFEF00D);
    chk("untouched byte @0", b.read8(SAVE + 0), 0x00);
    std::remove("save_test_tmp.sra");
  }


  // --- Controller Pak (joybus, canal 0) --------------------------------------
  // Se conduce por el camino REAL: bloque de ordenes en RDRAM -> SI DMA a PIF RAM ->
  // pifProcessJoybus -> SI DMA de vuelta, igual que hace __osContRamRead. Los dos CRC se
  // recalculan aqui a partir del codigo del SDK, no se reusa el del emulador: si ambos
  // estuvieran mal a la vez el test no valdria nada.
  std::printf("\nController Pak:\n");
  {
    auto addrCrc = [](u16 a) {                       // __osContAddressCrc
      u8 t = 0;
      for(int i = 0; i < 16; i++) {
        u8 t2 = (t & 0x10) ? 21 : 0;
        t = (u8)(t << 1); t |= (u8)((a & 0x400) ? 1 : 0); a = (u16)(a << 1); t ^= t2;
      }
      return (u8)(t & 0x1f);
    };
    auto dataCrc = [](const u8* d) {                 // __osContDataCrc
      u8 t = 0;
      for(int i = 0; i <= 32; i++) {
        for(int j = 7; j >= 0; j--) {
          u8 t2 = (t & 0x80) ? 133 : 0;
          t = (u8)(t << 1);
          if(i != 32) t |= (u8)((d[i] & (1 << j)) ? 1 : 0);
          t ^= t2;
        }
      }
      return t;
    };

    auto pp = std::make_unique<Memory>();
    Memory& m = *pp;
    m.reset(true);
    // El Controller Pak es del MANDO: desde que hay cuatro puertos vive en padPort[i],
    // y `reset` lo formatea para todo puerto cuyo accesorio sea 1 (el valor por defecto).
    chk("pak presente", m.padPort[0].accessory == 1 ? 1 : 0, 1);
    chk("tamano del pak", (u32)m.padPort[0].mempak.size(), 32 * 1024);

    // El pak sale formateado: bloque de ID (1/3/4/6) con las dos sumas buenas y deviceid
    // impar, que es lo que mira __osGetId antes de dar el pak por utilizable.
    for(int blk : {1, 3, 4, 6}) {
      const u8* id = &m.padPort[0].mempak[blk * 32];
      u16 sum = 0, isum = 0;
      for(int j = 0; j < 28; j += 2) {
        u16 d = (u16)((id[j] << 8) | id[j + 1]);
        sum = (u16)(sum + d); isum = (u16)(isum + (u16)~d);
      }
      char name[32];
      std::snprintf(name, sizeof name, "ID blq %d suma", blk);
      chk(name, (u32)((id[0x1C] << 8) | id[0x1D]), sum);
      std::snprintf(name, sizeof name, "ID blq %d suma inv", blk);
      chk(name, (u32)((id[0x1E] << 8) | id[0x1F]), isum);
      std::snprintf(name, sizeof name, "ID blq %d deviceid impar", blk);
      chk(name, (u32)(((id[0x18] << 8) | id[0x19]) & 1), 1);
    }
    {                                                 // suma de la tabla de inodos
      const u8* n = &m.padPort[0].mempak[8 * 32];
      u32 s = 0;
      for(int j = 10; j < 256; j++) s = (s + n[j]) & 0xffff;
      chk("inodos: suma", (u32)((n[0] << 8) | n[1]), s);
      chk("inodos: pagina 5 libre", (u32)((n[10] << 8) | n[11]), 3);
      chk("inodos: copia igual", (u32)std::memcmp(&m.padPort[0].mempak[8 * 32], &m.padPort[0].mempak[16 * 32], 256), 0);
    }

    // Ejecuta un bloque de ordenes joybus y devuelve la RDRAM ya releida.
    const u32 CMDBUF = 0x1000;
    auto run = [&](const u8* blockIn, u32 n) {
      for(u32 i = 0; i < 64; i++) m.rdram[CMDBUF + i] = i < n ? blockIn[i] : 0x00;
      m.write32(0xA480'0000, CMDBUF);                 // SI_DRAM_ADDR
      m.write32(0xA480'0010, 0);                      // RDRAM -> PIF (ejecuta)
      m.write32(0xA480'0004, 0);                      // PIF -> RDRAM (respuesta)
      // La transaccion del SI ya no termina en la instruccion que la arranca (el joybus va
      // a 4 us por bit): en el emulador la remata el bucle de CPU al vencer el plazo, y
      // aqui no hay CPU, asi que el arnes hace pasar el tiempo a mano.
      m.siFinish();
    };

    {                                                 // estado: tipo 0x0005 y pak dentro
      u8 blk[8] = { 1, 3, 0x00, 0xff, 0xff, 0xff, 0xfe, 0 };
      run(blk, 8);
      chk("estado: tipo alto", m.rdram[CMDBUF + 3], 0x05);
      chk("estado: tipo bajo", m.rdram[CMDBUF + 4], 0x00);
      chk("estado: CONT_CARD_ON", m.rdram[CMDBUF + 5], 0x01);
    }

    const u16 block = 40;                             // primera pagina de datos
    u8 payload[32];
    for(int k = 0; k < 32; k++) payload[k] = (u8)(0xA0 + k);
    {                                                 // escritura: la respuesta es el CRC
      u8 blk[64] = {0};
      u16 wire = (u16)((block << 5) | addrCrc(block));
      blk[0] = 35; blk[1] = 1; blk[2] = 0x03;
      blk[3] = (u8)(wire >> 8); blk[4] = (u8)wire;
      for(int k = 0; k < 32; k++) blk[5 + k] = payload[k];
      blk[38] = 0xfe;
      run(blk, 39);
      chk("escritura: CRC de datos", m.rdram[CMDBUF + 37], dataCrc(payload));
      chk("escritura: sin bit de ausente", (u32)(m.rdram[CMDBUF + 1] & 0xc0), 0);
      chk("escritura: llega al pak", (u32)std::memcmp(&m.padPort[0].mempak[block * 32], payload, 32), 0);
      chk("escritura: marca sucio", m.padPort[0].mempakDirty ? 1 : 0, 1);
    }
    {                                                 // lectura: 32 bytes + CRC
      u8 blk[64] = {0};
      u16 wire = (u16)((block << 5) | addrCrc(block));
      blk[0] = 3; blk[1] = 33; blk[2] = 0x02;
      blk[3] = (u8)(wire >> 8); blk[4] = (u8)wire;
      blk[38] = 0xfe;
      run(blk, 39);
      chk("lectura: datos", (u32)std::memcmp(&m.rdram[CMDBUF + 5], payload, 32), 0);
      chk("lectura: CRC de datos", m.rdram[CMDBUF + 37], dataCrc(payload));
      chk("lectura: sin bit de ausente", (u32)(m.rdram[CMDBUF + 1] & 0xc0), 0);
    }
    {                                                 // ventana del Rumble: fuera del pak
      u16 rb = 0x400;                                 // byte 0x8000
      u8 blk[64] = {0};
      u16 wire = (u16)((rb << 5) | addrCrc(rb));
      blk[0] = 3; blk[1] = 33; blk[2] = 0x02;
      blk[3] = (u8)(wire >> 8); blk[4] = (u8)wire;
      blk[38] = 0xfe;
      run(blk, 39);
      u32 nz = 0;
      for(int k = 0; k < 32; k++) nz += m.rdram[CMDBUF + 5 + k];
      chk("fuera de rango: ceros", nz, 0);
    }

    // Sin pak enchufado el mando responde igual pero con el CRC INVERTIDO: asi es como
    // __osContRamRead se entera y pasa a preguntar el estado del canal.
    {
      auto np = std::make_unique<Memory>();
      Memory& n = *np;
      n.reset(true);
      n.padPort[0].accessory = 0;          // ranura vacia
      n.padPort[0].mempak.clear();
      chk("sin pak: memoria vacia", (u32)n.padPort[0].mempak.size(), 0);
      u8 blk[64] = {0};
      u16 wire = (u16)((block << 5) | addrCrc(block));
      blk[0] = 3; blk[1] = 33; blk[2] = 0x02;
      blk[3] = (u8)(wire >> 8); blk[4] = (u8)wire;
      blk[38] = 0xfe;
      for(u32 i = 0; i < 64; i++) n.rdram[CMDBUF + i] = i < 39 ? blk[i] : 0x00;
      n.write32(0xA480'0000, CMDBUF);
      n.write32(0xA480'0010, 0);
      n.write32(0xA480'0004, 0);
      n.siFinish();   // hacer pasar el tiempo del joybus (no hay CPU en el arnes)
      u8 zeros[32] = {0};
      chk("sin pak: CRC invertido", n.rdram[CMDBUF + 37], (u8)~dataCrc(zeros));
      u8 st[8] = { 1, 3, 0x00, 0xff, 0xff, 0xff, 0xfe, 0 };
      for(u32 i = 0; i < 64; i++) n.rdram[CMDBUF + i] = i < 8 ? st[i] : 0x00;
      n.write32(0xA480'0000, CMDBUF);
      n.write32(0xA480'0010, 0);
      n.write32(0xA480'0004, 0);
      n.siFinish();   // hacer pasar el tiempo del joybus (no hay CPU en el arnes)
      chk("sin pak: CONT_CARD_ON a cero", n.rdram[CMDBUF + 5], 0x00);
    }

    // Persistencia: el .mpk va aparte del save de la cartuchera y sobrevive al apagado.
    {
      const char* romp = "save_test_tmp.z64";
      std::remove("save_test_tmp.mpk");
      m.attachSaveFile(romp);      // sin fichero: el pak formateado se queda como esta
      chk("escritura: sigue en el pak", (u32)std::memcmp(&m.padPort[0].mempak[block * 32], payload, 32), 0);
      m.padPort[0].mempakDirty = true;
      m.flushSaveFile();

      auto qp = std::make_unique<Memory>();
      Memory& q = *qp;
      q.reset(true);
      q.attachSaveFile(romp);
      chk("recargado del .mpk", (u32)std::memcmp(&q.padPort[0].mempak[block * 32], payload, 32), 0);
      std::remove("save_test_tmp.mpk");
      std::remove("save_test_tmp.sra");
    }
  }

  std::printf("\n%s (%d failures)\n", failures ? "FAILED" : "ALL PASS", failures);
  return failures ? 1 : 0;
}
