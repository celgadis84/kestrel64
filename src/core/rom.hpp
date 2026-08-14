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
