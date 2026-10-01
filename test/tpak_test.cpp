// Test unitario del Transfer Pak (accesorio 3). Conduce el joybus por el camino REAL, igual
// que __osContRamRead/__osContRamWrite del SDK: bloque de ordenes en RDRAM -> SI DMA a PIF
// RAM -> pifProcessJoybus -> SI DMA de vuelta. La secuencia es la de osGbpakInit /
// osGbpakGetStatus / osGbpakReadWrite (libultra gbpak), sobre una ROM de GB sintetica MBC5
// con RAM y bateria escrita a un fichero temporal.
#include "../src/core/memory.hpp"
#include "../src/core/runtime.hpp"
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>

using namespace kestrel;

static int failures = 0;
static void chk(const char* what, u32 got, u32 exp) {
  bool ok = got == exp;
  std::printf("  %-34s got=0x%08x exp=0x%08x  %s\n", what, got, exp, ok ? "OK" : "*** FAIL ***");
  if(!ok) failures++;
}

static u8 addrCrc(u16 a) {                           // __osContAddressCrc
  u8 t = 0;
  for(int i = 0; i < 16; i++) {
    u8 t2 = (t & 0x10) ? 21 : 0;
    t = (u8)(t << 1); t |= (u8)((a & 0x400) ? 1 : 0); a = (u16)(a << 1); t ^= t2;
  }
  return (u8)(t & 0x1f);
}
static u8 dataCrc(const u8* d) {                     // __osContDataCrc
  u8 t = 0;
  for(int i = 0; i <= 32; i++)
    for(int j = 7; j >= 0; j--) {
      u8 t2 = (t & 0x80) ? 133 : 0;
      t = (u8)(t << 1);
      if(i != 32) t |= (u8)((d[i] & (1 << j)) ? 1 : 0);
      t ^= t2;
    }
  return t;
}

static constexpr u32 CMDBUF = 0x1000;

static void run(Memory& m, const u8* blk, u32 n) {
  for(u32 i = 0; i < 64; i++) m.rdram[CMDBUF + i] = i < n ? blk[i] : 0x00;
  m.write32(0xA480'0000, CMDBUF);                   // SI_DRAM_ADDR
  m.write32(0xA480'0010, 0);                        // RDRAM -> PIF (ejecuta)
  m.write32(0xA480'0004, 0);                        // PIF -> RDRAM (respuesta)
  m.siFinish();                                     // no hay CPU: el arnes pasa el tiempo
}

// Escribe 32 bytes en la direccion `addr` del pak (canal 0). Devuelve el CRC de respuesta.
static u8 pakWrite(Memory& m, u16 addr, const u8* d) {
  u8 blk[64] = {0};
  u16 block = (u16)(addr >> 5);
  u16 wire = (u16)((block << 5) | addrCrc(block));
  blk[0] = 35; blk[1] = 1; blk[2] = 0x03;
  blk[3] = (u8)(wire >> 8); blk[4] = (u8)wire;
  for(int k = 0; k < 32; k++) blk[5 + k] = d[k];
  blk[38] = 0xfe;
  run(m, blk, 39);
  return m.rdram[CMDBUF + 37];
}
static void pakFill(Memory& m, u16 addr, u8 v) { u8 d[32]; std::memset(d, v, 32); pakWrite(m, addr, d); }
// Lee 32 bytes; devuelve el CRC recibido y copia los datos en `out`.
static u8 pakRead(Memory& m, u16 addr, u8* out) {
  u8 blk[64] = {0};
  u16 block = (u16)(addr >> 5);
  u16 wire = (u16)((block << 5) | addrCrc(block));
  blk[0] = 3; blk[1] = 33; blk[2] = 0x02;
  blk[3] = (u8)(wire >> 8); blk[4] = (u8)wire;
  blk[38] = 0xfe;
  run(m, blk, 39);
  std::memcpy(out, &m.rdram[CMDBUF + 5], 32);
  return m.rdram[CMDBUF + 37];
}
// osGbpakGetStatus: OR de los 32 bytes & (RSTB_DETECTION|GBCART_PULL), mas el ultimo byte.
static u8 gbStatus(Memory& m) {
  u8 d[32];
  pakRead(m, 0xB000, d);
  u8 o = 0;
  for(int k = 0; k < 32; k++) o |= d[k];
  return (u8)((o & 0x44) | d[31]);
}

static void setCart(int port, const std::string& path) {
  std::lock_guard<std::mutex> lk(rt::tpakMx);
  rt::tpakRom[port] = path;
  rt::tpakGen[port].fetch_add(1, std::memory_order_relaxed);
}

int main() {
  // ROM MBC5+RAM+BATERIA (0x1B), 4 bancos de 16 KiB, 32 KiB de RAM (0x149 = 3). Cada banco
  // lleva su numero en todos sus bytes, salvo la cabecera del banco 0.
  const char* romPath = "tpak_test_gb.gbc";
  const char* savPath = "tpak_test_gb.sav";
  std::remove(savPath);
  {
    std::vector<u8> rom(4 * 0x4000);
    for(usize i = 0; i < rom.size(); i++) rom[i] = (u8)(i / 0x4000);
    const char title[] = "KESTRELTPAK";
    std::memcpy(&rom[0x134], title, sizeof title - 1);
    rom[0x147] = 0x1B; rom[0x148] = 0x01; rom[0x149] = 0x03;
    std::FILE* f = std::fopen(romPath, "wb");
    if(!f) { std::printf("no se pudo crear la ROM de GB\n"); return 1; }
    std::fwrite(rom.data(), 1, rom.size(), f);
    std::fclose(f);
  }

  auto mp = std::make_unique<Memory>();
  Memory& m = *mp;
  m.reset(true);
  m.padPort[0].accessory = 3;
  u8 d[32];

  std::printf("Transfer Pak sin cartucho:\n");
  {
    u8 blk[8] = { 1, 3, 0x00, 0xff, 0xff, 0xff, 0xfe, 0 };
    run(m, blk, 8);
    chk("estado: CONT_CARD_ON", m.rdram[CMDBUF + 5], 0x01);
    pakRead(m, 0x8000, d);
    chk("apagado al arrancar: 0x8000", d[0], 0x00);
    pakFill(m, 0x8000, 0xFE);                         // osGbpakInit: apaga y comprueba
    pakRead(m, 0x8000, d);
    chk("tras 0xFE: 0x8000 no es 0xFE", d[31], 0x00);
    pakFill(m, 0x8000, 0x84);                         // y enciende
    u8 crc = pakRead(m, 0x8000, d);
    chk("tras 0x84: 0x8000", d[31], 0x84);
    chk("CRC de datos no invertido", crc, dataCrc(d));
    chk("sin cartucho: estado", gbStatus(m), 0x40);
  }

  std::printf("\nRumble/mempak sobre Transfer Pak (sondeo del SDK):\n");
  {
    pakFill(m, 0x8000, 0xFE);
    pakFill(m, 0x8000, 0x80);                         // osMotorInit escribe 0x80...
    pakRead(m, 0x8000, d);
    chk("0x80 no se queda (no es Rumble)", d[31], 0x00);
    pakRead(m, 0x0000, d);
    chk("apagado: ventana baja a cero", d[0], 0x00);
  }

  std::printf("\nCartucho MBC5 puesto:\n");
  setCart(0, romPath);
  pakFill(m, 0x8000, 0x84);
  chk("cartucho cargado", m.padPort[0].gb.loaded() ? 1 : 0, 1);
  chk("mbc = MBC5", (u32)m.padPort[0].gb.mbc, (u32)GbCart::Mbc::Mbc5);
  chk("RAM 32 KiB", (u32)m.padPort[0].gb.ram.size(), 0x8000);
  {
    pakRead(m, 0xB000, d);
    chk("estado sin alimentar", d[31], 0x80);
    pakFill(m, 0xB000, 0x01);                         // alimentar el cartucho GB
    pakRead(m, 0xB000, d);
    chk("alimentado: byte 0 (+RSTB_DET)", d[0], 0x8D);
    chk("alimentado: byte 31", d[31], 0x89);
    chk("RSTB_DETECTION se entrega una vez", gbStatus(m), 0x89);

    pakFill(m, 0xA000, 0x00);                         // banco 0 del bus GB
    pakRead(m, 0xC000 + 0x120, d);                    // GB 0x0120..0x013F
    chk("cabecera: titulo", (u32)std::memcmp(&d[0x14], "KESTRELTPAK", 11), 0);
    pakFill(m, 0xA000, 0x01);                         // GB 0x4000-0x7FFF
    pakRead(m, 0xC000, d);
    chk("banco ROM 1 por defecto", d[0], 0x01);
    pakFill(m, 0xA000, 0x00);
    pakFill(m, 0xC000 + 0x2000, 0x03);                // MBC5: banco ROM = 3
    pakFill(m, 0xA000, 0x01);
    pakRead(m, 0xC000 + 0x3FE0, d);
    chk("banco ROM 3", d[31], 0x03);
  }
  {
    pakFill(m, 0xA000, 0x00);
    pakFill(m, 0xC000 + 0x0000, 0x0A);                // habilitar RAM
    pakFill(m, 0xA000, 0x01);
    pakFill(m, 0xC000, 0x02);                         // GB 0x4000: banco RAM 2 (0x4000-0x5FFF)
    // GB 0xA000 cae en el banco 2 del pak (0x8000-0xBFFF): ventana 0xC000 + 0x2000.
    pakFill(m, 0xA000, 0x02);
    u8 pay[32];
    for(int k = 0; k < 32; k++) pay[k] = (u8)(0x40 + k);
    chk("escritura RAM: CRC", pakWrite(m, 0xC000 + 0x2000, pay), dataCrc(pay));
    pakRead(m, 0xC000 + 0x2000, d);
    chk("RAM: vuelve lo escrito", (u32)std::memcmp(d, pay, 32), 0);
    chk("RAM: en el banco 2", (u32)std::memcmp(&m.padPort[0].gb.ram[2 * 0x2000], pay, 32), 0);
    chk("RAM: sucia", m.padPort[0].gb.ramDirty ? 1 : 0, 1);
    m.flushSaveFile();
    GbCart g;
    g.load(romPath);
    chk(".sav: RAM recargada", (u32)std::memcmp(&g.ram[2 * 0x2000], pay, 32), 0);
  }

  std::printf("\nApagado y cambio de cartucho:\n");
  {
    pakFill(m, 0x8000, 0xFE);
    pakRead(m, 0xC000, d);
    chk("apagado: ventana GB a cero", d[0], 0x00);
    pakFill(m, 0x8000, 0x84);
    setCart(0, romPath);                              // mismo juego, sacado y vuelto a meter
    chk("cambio: GBCART_PULL", gbStatus(m) & 0x40, 0x40);
    chk("PULL se entrega una vez", gbStatus(m) & 0x40, 0x00);
    setCart(0, "");                                   // sacado
    chk("sacado: sin GBCART_ON", gbStatus(m) & 0x80, 0x00);
  }

  std::printf("\nMBC1 / MBC3 directos:\n");
  {
    const char* p1 = "tpak_test_mbc1.gb";
    std::vector<u8> rom(64 * 0x4000);                 // 1 MiB: 64 bancos
    for(usize i = 0; i < rom.size(); i++) rom[i] = (u8)(i / 0x4000);
    rom[0x147] = 0x01;
    std::FILE* f = std::fopen(p1, "wb");
    std::fwrite(rom.data(), 1, rom.size(), f); std::fclose(f);
    GbCart g;
    g.load(p1);
    g.write(0x2000, 0x00);
    chk("MBC1: banco 0 -> 1", g.read(0x4000), 0x01);
    g.write(0x2000, 0x05); g.write(0x4000, 0x01);
    chk("MBC1: banco 0x25", g.read(0x4000), 0x25);
    g.write(0x6000, 0x01);
    chk("MBC1 modo 1: 0x0000 = banco 0x20", g.read(0x0000), 0x20);
    std::remove(p1);

    const char* p3 = "tpak_test_mbc3.gb";
    rom.assign(8 * 0x4000, 0);
    rom[0x147] = 0x10; rom[0x149] = 0x03;             // MBC3+TIMER+RAM+BATERIA
    f = std::fopen(p3, "wb");
    std::fwrite(rom.data(), 1, rom.size(), f); std::fclose(f);
    GbCart h;
    h.load(p3);
    h.write(0x0000, 0x0A);
    h.write(0x4000, 0x0A);                            // RTC: horas
    h.write(0xA000, 0x17);
    h.write(0x6000, 0x00); h.write(0x6000, 0x01);     // enclavar
    chk("MBC3 RTC: horas enclavadas", h.read(0xA000), 0x17);
    h.write(0x4000, 0x0C);
    h.write(0xA000, 0x40);                            // parar el reloj
    h.write(0x6000, 0x00); h.write(0x6000, 0x01);
    chk("MBC3 RTC: DH parado", h.read(0xA000), 0x40);
    std::remove(p3);
    std::remove("tpak_test_mbc3.sav");
  }

  std::remove(romPath);
  std::remove(savPath);
  if(failures == 0) std::printf("\nALL PASS\n");
  else std::printf("\n%d FAILURES\n", failures);
  return failures ? 1 : 0;
}
