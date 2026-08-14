#pragma once
// Loom core — fundamental fixed-width types and small helpers.

#include <cstdint>
#include <cstddef>

namespace kestrel {

using u8  = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8  = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;
using usize = std::size_t;

// Byteswap helpers. N64 ROMs and the RDRAM bus are big-endian; the host x86-64 is
// little-endian, so every 16/32/64-bit access through the bus swaps.
inline auto bswap16(u16 v) -> u16 { return (u16)(v >> 8 | v << 8); }
inline auto bswap32(u32 v) -> u32 {
  return  v >> 24
       | (v >> 8  & 0x0000'ff00u)
       | (v << 8  & 0x00ff'0000u)
       |  v << 24;
}
inline auto bswap64(u64 v) -> u64 {
  return  (u64)bswap32((u32)v) << 32 | bswap32((u32)(v >> 32));
}

}  // namespace kestrel
