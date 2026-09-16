#pragma once
// Loom core — cartridge ROM loading and header parsing.
//
// N64 ROMs ship in three byte orders; loadFile() detects the magic and normalizes
// the whole image to big-endian (.z64) so the rest of the core sees one format.

#include "types.hpp"
#include <string>
#include <vector>

namespace kestrel {

struct RomHeader {
  u8   pi_bsd_dom1[4] = {};  // 0x00: PI BSD domain 1 config
  u32  clockRate = 0;        // 0x04
  u32  entryPoint = 0;       // 0x08: initial CPU PC
  u32  release = 0;          // 0x0C
  u32  crc1 = 0;             // 0x10
  u32  crc2 = 0;             // 0x14
  std::string name;          // 0x20: 20-byte internal name
  char cartId[2] = {};       // 0x3C
  char countryCode = 0;      // 0x3E
  u8   version = 0;          // 0x3F
};

// Norma de television que espera el cartucho. En la consola de verdad la region del
// cartucho y la de la maquina coinciden siempre (un cartucho PAL se juega en una consola
// PAL), y libultra publica la norma de la maquina como `osTvType` en 0x80000300. Los
// juegos actuan sobre ese valor: el SDK de Nintendo pedia expresamente negarse a
// funcionar con la norma equivocada, y Perfect Dark (PAL) se queda en un bucle infinito
// dentro de mainInit() cuando lee NTSC ahi. Fijarlo a NTSC pase lo que pase deja negros
// todos los cartuchos PAL.
enum class TvType : u32 { Pal = 0, Ntsc = 1, Mpal = 2 };   // valores OS_TV_* de libultra

// Norma que corresponde al codigo de pais del encabezado (offset 0x3E).
auto tvTypeForCountry(char code) -> TvType;
// Campos de video por segundo de esa norma.
auto tvFieldHz(TvType t) -> double;
// Reloj de video del RCP de esa norma, en hercios (de el cuelga el DAC de audio).
auto tvVidClock(TvType t) -> u32;
// Nombre corto para los mensajes.
auto tvName(TvType t) -> const char*;

struct Rom {
  std::vector<u8> data;      // normalized big-endian image
  RomHeader header;
  enum class Order { Z64, N64, V64, Unknown };
  Order originalOrder = Order::Unknown;

  // Load a ROM file from disk, normalize to big-endian, parse the header.
  // Returns false (with `error` set) on failure.
  auto loadFile(const std::string& path, std::string& error) -> bool;

  auto valid() const -> bool { return !data.empty(); }

private:
  auto parseHeader() -> void;
};

}  // namespace kestrel
