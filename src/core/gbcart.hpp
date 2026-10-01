#pragma once
// Cartucho de Game Boy / Game Boy Color metido en el Transfer Pak.
//
// El Transfer Pak no emula nada: es un adaptador que pone el bus del cartucho GB (16 bits de
// direccion, 8 de datos) al alcance del joybus. Lo que hay detras de ese bus es el
// controlador de bancos (MBC) del propio cartucho, y eso es lo que modela esta clase: una
// lectura o escritura en el espacio GB 0x0000-0xBFFF tal como la veria la consola portatil.
//
//   0x0000-0x3FFF  ROM banco 0 (MBC1 en modo 1 puede mapear otro)
//   0x4000-0x7FFF  ROM banco conmutable
//   0xA000-0xBFFF  RAM externa (con bateria en los que guardan) o registros del RTC (MBC3)
//   escrituras a 0x0000-0x7FFF = registros del MBC (no hay ROM que escribir)
//
// Tipos (cabecera 0x147, Pan Docs): ROM sola, MBC1, MBC2, MBC3 (+RTC), MBC5. Son los de los
// juegos que hablan con el Transfer Pak (Pokemon R/A/V = MBC3/MBC5, Oro/Plata = MBC3+RTC,
// Perfect Dark GBC = MBC5). Un tipo desconocido se trata como ROM sola.
//
// La RAM con bateria se guarda en un .sav junto a la ROM de GB (convencion de todos los
// emuladores de GB); el RTC del MBC3 se anade al final en el formato de 48 bytes de
// VBA-M/BGB (cinco registros vivos, cinco enclavados, marca de tiempo Unix de 64 bits), asi
// que las partidas son intercambiables. El RTC sigue el reloj del anfitrion, como el cristal
// de 32 kHz del cartucho sigue el tiempo real aunque la consola este apagada.
#include "types.hpp"
#include <vector>
#include <string>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace kestrel {

struct GbCart {
  enum class Mbc : u8 { None, Mbc1, Mbc2, Mbc3, Mbc5 };

  std::vector<u8> rom, ram;
  std::string path, savPath;       // ROM de GB y su .sav
  Mbc  mbc = Mbc::None;
  bool battery = false, hasRtc = false;
  bool ramDirty = false;

  // Registros del MBC (estado de invitado: van en el savestate).
  struct Regs {
    bool ramOn = false;
    u16  romBank = 1;              // MBC1: 5 bits bajos; MBC3: 7; MBC5: 9
    u8   hiBank = 0;               // MBC1: registro de 2 bits; MBC3/5: banco de RAM o RTC
    u8   mode = 0;                 // MBC1: modo de bancos; MBC3: ultimo byte del enclavado
    u8   rtc[5] = {}, latched[5] = {};   // S M H DL DH
    s64  rtcBase = 0;              // segundos Unix a los que corresponde `rtc`
  } r;

  auto loaded() const -> bool { return !rom.empty(); }

  auto clear() -> void { *this = GbCart{}; }

  // Carga la ROM y, si el cartucho la tiene con bateria, su .sav. false si no se pudo leer
  // o no parece una ROM de GB (cabecera mas corta que 0x150 bytes).
  auto load(const std::string& p) -> bool {
    clear();
    std::FILE* f = std::fopen(p.c_str(), "rb");
    if(!f) return false;
    std::vector<u8> d;
    u8 buf[65536]; usize n;
    while((n = std::fread(buf, 1, sizeof buf, f)) > 0) d.insert(d.end(), buf, buf + n);
    std::fclose(f);
    if(d.size() < 0x150) return false;
    rom = std::move(d);
    path = p;
    u8 t = rom[0x147];
    switch(t) {
    case 0x01: mbc = Mbc::Mbc1; break;
    case 0x02: mbc = Mbc::Mbc1; break;
    case 0x03: mbc = Mbc::Mbc1; battery = true; break;
    case 0x05: mbc = Mbc::Mbc2; break;
    case 0x06: mbc = Mbc::Mbc2; battery = true; break;
    case 0x09: battery = true; break;                          // ROM+RAM+BATERIA
    case 0x0F: mbc = Mbc::Mbc3; battery = true; hasRtc = true; break;
    case 0x10: mbc = Mbc::Mbc3; battery = true; hasRtc = true; break;
    case 0x11: case 0x12: mbc = Mbc::Mbc3; break;
    case 0x13: mbc = Mbc::Mbc3; battery = true; break;
    case 0x19: case 0x1A: case 0x1C: case 0x1D: mbc = Mbc::Mbc5; break;
    case 0x1B: case 0x1E: mbc = Mbc::Mbc5; battery = true; break;
    default: break;
    }
    static const u32 kRam[6] = {0, 2048, 8192, 32768, 131072, 65536};
    u32 rs = rom[0x149] < 6 ? kRam[rom[0x149]] : 0;
    if(mbc == Mbc::Mbc2) rs = 512;                             // 512 x 4 bits en el chip
    ram.assign(rs, 0x00);
    r = Regs{};
    r.rtcBase = (s64)std::time(nullptr);
    savPath = p;
    usize dot = savPath.find_last_of('.');
    usize sep = savPath.find_last_of("/\\");
    if(dot != std::string::npos && (sep == std::string::npos || dot > sep)) savPath.resize(dot);
    savPath += ".sav";
    if(battery) loadSav();
    return true;
  }

  auto loadSav() -> void {
    std::FILE* f = std::fopen(savPath.c_str(), "rb");
    if(!f) return;
    if(!ram.empty()) (void)std::fread(ram.data(), 1, ram.size(), f);
    if(hasRtc) {
      u8 t[48];
      if(std::fread(t, 1, 48, f) == 48) {
        for(int i = 0; i < 5; i++) { r.rtc[i] = t[i * 4]; r.latched[i] = t[20 + i * 4]; }
        s64 ts = 0;
        for(int i = 7; i >= 0; i--) ts = (ts << 8) | t[40 + i];
        r.rtcBase = ts;
      }
    }
    std::fclose(f);
  }

  auto flush() -> void {
    if(!battery || !ramDirty || savPath.empty()) return;
    std::FILE* f = std::fopen(savPath.c_str(), "wb");
    if(!f) return;
    if(!ram.empty()) std::fwrite(ram.data(), 1, ram.size(), f);
    if(hasRtc) {
      rtcSync();
      u8 t[48] = {};
      for(int i = 0; i < 5; i++) { t[i * 4] = r.rtc[i]; t[20 + i * 4] = r.latched[i]; }
      u64 ts = (u64)r.rtcBase;
      for(int i = 0; i < 8; i++) t[40 + i] = (u8)(ts >> (8 * i));
      std::fwrite(t, 1, 48, f);
    }
    std::fclose(f);
    ramDirty = false;
  }

  // --- RTC del MBC3 -----------------------------------------------------------------
  // `rtc` es el valor en el instante `rtcBase`; avanzar = sumar los segundos pasados desde
  // entonces (salvo con el bit de parada DH.6). Dias en 9 bits (DL + DH.0), y al pasar de
  // 511 se pone el acarreo DH.7, que solo borra el juego.
  auto rtcSync() -> void {
    s64 now = (s64)std::time(nullptr);
    s64 dt = now - r.rtcBase;
    r.rtcBase = now;
    if(dt <= 0 || (r.rtc[4] & 0x40)) return;
    s64 s = r.rtc[0] + dt;
    s64 m = r.rtc[1] + s / 60; s %= 60;
    s64 h = r.rtc[2] + m / 60; m %= 60;
    s64 d = (s64)(r.rtc[3] | ((r.rtc[4] & 1) << 8)) + h / 24; h %= 24;
    u8 dh = r.rtc[4] & 0xC0;
    if(d > 511) { dh |= 0x80; d %= 512; }
    r.rtc[0] = (u8)s; r.rtc[1] = (u8)m; r.rtc[2] = (u8)h;
    r.rtc[3] = (u8)d; r.rtc[4] = (u8)(dh | ((d >> 8) & 1));
  }

  auto romAt(u32 off) const -> u8 { return rom.empty() ? 0xFF : rom[off % rom.size()]; }
  auto ramIdx(u16 a, u32 bank) const -> u32 { return (u32)(bank * 0x2000u + (a - 0xA000u)) % (u32)ram.size(); }

  auto read(u16 a) -> u8 {
    if(rom.empty()) return 0xFF;
    if(a < 0x4000) {
      u32 bank = (mbc == Mbc::Mbc1 && r.mode) ? (u32)(r.hiBank << 5) : 0u;
      return romAt(bank * 0x4000u + a);
    }
    if(a < 0x8000) {
      u32 bank = r.romBank;
      if(mbc == Mbc::Mbc1) bank = (u32)(r.hiBank << 5) | (r.romBank & 0x1F);
      return romAt(bank * 0x4000u + (a - 0x4000u));
    }
    if(a >= 0xA000 && a < 0xC000) {
      switch(mbc) {
      case Mbc::None:
        return ram.empty() ? 0xFF : ram[ramIdx(a, 0)];
      case Mbc::Mbc1:
        if(!r.ramOn || ram.empty()) return 0xFF;
        return ram[ramIdx(a, r.mode ? r.hiBank : 0)];
      case Mbc::Mbc2:
        if(!r.ramOn) return 0xFF;
        return (u8)(0xF0 | (ram[(a - 0xA000u) & 0x1FF] & 0x0F));   // nibble alto flotando a 1
      case Mbc::Mbc3:
        if(!r.ramOn) return 0xFF;
        if(r.hiBank >= 0x08 && r.hiBank <= 0x0C) return hasRtc ? r.latched[r.hiBank - 8] : 0xFF;
        if(ram.empty()) return 0xFF;
        return ram[ramIdx(a, r.hiBank & 3)];
      case Mbc::Mbc5:
        if(!r.ramOn || ram.empty()) return 0xFF;
        return ram[ramIdx(a, r.hiBank & 0x0F)];
      }
    }
    return 0xFF;                                               // 0x8000-0x9FFF: no hay nada
  }

  auto write(u16 a, u8 v) -> void {
    if(rom.empty()) return;
    if(a < 0x8000) {
      switch(mbc) {
      case Mbc::None: break;
      case Mbc::Mbc1:
        if(a < 0x2000)      r.ramOn = (v & 0x0F) == 0x0A;
        else if(a < 0x4000) { r.romBank = v & 0x1F; if(!r.romBank) r.romBank = 1; }
        else if(a < 0x6000) r.hiBank = v & 3;
        else                r.mode = v & 1;
        break;
      case Mbc::Mbc2:
        if(a < 0x4000) {                                       // A8 elige el registro
          if(a & 0x100) { r.romBank = v & 0x0F; if(!r.romBank) r.romBank = 1; }
          else r.ramOn = (v & 0x0F) == 0x0A;
        }
        break;
      case Mbc::Mbc3:
        if(a < 0x2000)      r.ramOn = (v & 0x0F) == 0x0A;
        else if(a < 0x4000) { r.romBank = v & 0x7F; if(!r.romBank) r.romBank = 1; }
        else if(a < 0x6000) r.hiBank = v;
        else {                                                 // 0 y luego 1 = enclavar
          if(r.mode == 0 && v == 1 && hasRtc) { rtcSync(); std::memcpy(r.latched, r.rtc, 5); }
          r.mode = v;
        }
        break;
      case Mbc::Mbc5:
        if(a < 0x2000)      r.ramOn = (v & 0x0F) == 0x0A;
        else if(a < 0x3000) r.romBank = (u16)((r.romBank & 0x100) | v);
        else if(a < 0x4000) r.romBank = (u16)((r.romBank & 0xFF) | ((v & 1) << 8));
        else if(a < 0x6000) r.hiBank = v & 0x0F;
        break;
      }
      return;
    }
    if(a >= 0xA000 && a < 0xC000) {
      switch(mbc) {
      case Mbc::None:
        if(!ram.empty()) { ram[ramIdx(a, 0)] = v; ramDirty = true; }
        break;
      case Mbc::Mbc1:
        if(r.ramOn && !ram.empty()) { ram[ramIdx(a, r.mode ? r.hiBank : 0)] = v; ramDirty = true; }
        break;
      case Mbc::Mbc2:
        if(r.ramOn) { ram[(a - 0xA000u) & 0x1FF] = v & 0x0F; ramDirty = true; }
        break;
      case Mbc::Mbc3:
        if(!r.ramOn) break;
        if(r.hiBank >= 0x08 && r.hiBank <= 0x0C) {
          if(hasRtc) {
            rtcSync();
            static const u8 kMask[5] = {0x3F, 0x3F, 0x1F, 0xFF, 0xC1};
            r.rtc[r.hiBank - 8] = v & kMask[r.hiBank - 8];
            ramDirty = true;
          }
        } else if(!ram.empty()) { ram[ramIdx(a, r.hiBank & 3)] = v; ramDirty = true; }
        break;
      case Mbc::Mbc5:
        if(r.ramOn && !ram.empty()) { ram[ramIdx(a, r.hiBank & 0x0F)] = v; ramDirty = true; }
        break;
      }
    }
  }
};

}  // namespace kestrel
