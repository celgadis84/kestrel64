#pragma once
// kestrel64 -- cabecera avanzada de homebrew y metadatos embebidos.
//
// n64brew, "ROM Header": con el codigo de juego "ED" en 0x3C-0x3D los bytes de 0x34 a 0x3F
// dejan de ser reservados y pasan a decir que necesita el juego:
//   0x34-0x37  que va enchufado en cada puerto (0 = sin dato, 1 Rumble Pak, 2 Controller Pak,
//              3 Transfer Pak, 0x80 raton, 0x81 VRU, 0x82-0x84 GameCube/teclados, 0xFF nada)
//   0x38       bit 0: lleva al final un zip con metadatos (n64brew, "ROM Metadata")
//   0x3E       region; 0 = region libre en homebrew
//   0x3F       bit 0 RTC, bit 1 region libre, bits 4-7 tipo de partida
// El tipo de partida y el RTC ya los aplica memory.cpp (resolveSaveType); los mandos tambien,
// en Memory::loadRom. Aqui va lo que usan la ficha, la biblioteca y el MCP.
//
// Metadatos: zip SIN comprimir pegado al final de la ROM, o un <rom>.meta al lado (manda el de
// fuera). Dentro, metadata.ini (UTF-8) con [meta] (name, author, release-date, osi-license,
// website, age-rating, num-players, short-desc, long-desc = .txt del zip, screenshots), su
// version por idioma [meta.<iso639-1>], y [boxart]/[cartart] con imagenes PNG o JPG.

#include "types.hpp"
#include <string>
#include <utility>
#include <vector>

namespace kestrel::hb {

struct Header {
  bool ed = false;          // codigo de juego "ED": cabecera avanzada
  u8   ctrl[4] = {};        // 0x34-0x37
  u8   saveType = 0;        // 0x3F >> 4
  bool rtc = false;         // 0x3F bit 0
  bool regionFree = false;  // 0x3F bit 1 (o region 0x00)
  bool hasMeta = false;     // 0x38 bit 0
};

// `h`: los 0x40 primeros bytes de la ROM ya en big-endian.
inline auto parseHeader(const u8* h) -> Header {
  Header r;
  r.ed = h[0x3C] == 'E' && h[0x3D] == 'D';
  if(!r.ed) return r;
  for(int i = 0; i < 4; i++) r.ctrl[i] = h[0x34 + i];
  r.saveType = h[0x3F] >> 4;
  r.rtc = h[0x3F] & 1;
  r.regionFree = (h[0x3F] & 2) || h[0x3E] == 0;
  r.hasMeta = h[0x38] & 1;
  return r;
}

inline auto saveTypeName(u8 t) -> const char* {
  static const char* const k[] = {"ninguna", "EEPROM 4K", "EEPROM 16K", "SRAM 256K",
                                  "SRAM 768K (bancos)", "FlashRAM", "SRAM 1M"};
  return t < 7 ? k[t] : "desconocida";
}

inline auto ctrlName(u8 v) -> const char* {
  switch(v) {
    case 0x00: return "sin dato";
    case 0x01: return "mando + Rumble Pak";
    case 0x02: return "mando + Controller Pak";
    case 0x03: return "mando + Transfer Pak";
    case 0x80: return "raton";
    case 0x81: return "VRU (reconocimiento de voz)";
    case 0x82: return "mando de GameCube";
    case 0x83: return "teclado Randnet";
    case 0x84: return "teclado de GameCube";
    case 0xFF: return "nada";
    default:   return v < 0x80 ? "mando" : "otro dispositivo";
  }
}

struct Meta {
  bool ok = false;
  std::string source;                                     // "fichero .meta" o "embebido en la ROM"
  std::vector<std::pair<std::string, std::string>> kv;    // [meta] ya con el idioma aplicado
  std::string longDesc;                                   // texto del .txt de long-desc
  std::vector<std::string> screenshots;                   // nombres dentro del zip
  std::vector<std::pair<std::string, std::string>> boxart, cartart;   // cara -> nombre
  std::vector<u8> zip;                                    // el zip entero, para sacar imagenes
  auto get(const char* k) const -> std::string;
  auto file(const std::string& name, std::vector<u8>& out) const -> bool;
};

// Zip en `p` (puede estar pegado tras otros datos: las posiciones se corrigen con el
// directorio central). Saca la entrada `name` (guardada o deflate).
auto zipEntry(const u8* p, usize n, const std::string& name, std::vector<u8>& out) -> bool;
// metadata.ini del zip, con `lang` (dos letras) por encima de [meta].
auto parseMeta(std::vector<u8> zip, const std::string& lang, Meta& out) -> bool;
// Cabecera de la ROM en disco (desenvuelve .zip/.gz y el orden de bytes). false = no es ROM.
auto probe(const std::string& romPath, Header& out) -> bool;
// Metadatos de la ROM en disco: <rom>.meta si existe, si no el zip del final cuando la
// cabecera lo anuncia. `lang` vacio = idioma del usuario.
auto load(const std::string& romPath, const std::string& lang = std::string()) -> Meta;
auto userLang() -> std::string;

}  // namespace kestrel::hb
