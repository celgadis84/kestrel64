#include "rom.hpp"
#include <cstdio>

namespace kestrel {

auto Rom::loadFile(const std::string& path, std::string& error) -> bool {
  FILE* f = std::fopen(path.c_str(), "rb");
  if(!f) { error = "cannot open ROM: " + path; return false; }
  std::fseek(f, 0, SEEK_END);
  long len = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if(len < 0x1000) { std::fclose(f); error = "ROM too small"; return false; }
  data.resize((usize)len);
  usize got = std::fread(data.data(), 1, (usize)len, f);
  std::fclose(f);
  if(got != (usize)len) { error = "short read on ROM"; data.clear(); return false; }

  // Detect byte order from the first word, normalize to big-endian (.z64).
  u32 magic = (u32)data[0] << 24 | (u32)data[1] << 16 | (u32)data[2] << 8 | data[3];
  if(magic == 0x8037'1240) {
    originalOrder = Order::Z64;                       // already big-endian
  } else if(magic == 0x4012'3780) {
    originalOrder = Order::N64;                        // little-endian: swap each 32-bit word
    for(usize i = 0; i + 3 < data.size(); i += 4) {
      std::swap(data[i + 0], data[i + 3]);
      std::swap(data[i + 1], data[i + 2]);
    }
  } else if(magic == 0x3780'4012) {
    originalOrder = Order::V64;                        // byteswapped halfwords: swap each pair
    for(usize i = 0; i + 1 < data.size(); i += 2) {
      std::swap(data[i + 0], data[i + 1]);
    }
  } else {
    originalOrder = Order::Unknown;
    error = "unrecognized ROM magic";
    data.clear();
    return false;
  }

  parseHeader();
  return true;
}

auto Rom::parseHeader() -> void {
  auto be32 = [&](usize o) -> u32 {
    return (u32)data[o] << 24 | (u32)data[o+1] << 16 | (u32)data[o+2] << 8 | data[o+3];
  };
  for(int i = 0; i < 4; i++) header.pi_bsd_dom1[i] = data[i];
  header.clockRate  = be32(0x04);
  header.entryPoint = be32(0x08);
  header.release    = be32(0x0c);
  header.crc1       = be32(0x10);
  header.crc2       = be32(0x14);
  header.name.assign((const char*)&data[0x20], 20);
  // trim trailing spaces/nulls
  while(!header.name.empty() && (header.name.back() == ' ' || header.name.back() == '\0'))
    header.name.pop_back();
  header.cartId[0] = (char)data[0x3c];
  header.cartId[1] = (char)data[0x3d];
  header.countryCode = (char)data[0x3e];
  header.version = data[0x3f];
}

}  // namespace kestrel
