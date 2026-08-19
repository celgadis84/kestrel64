#pragma once
// Loom core — fundamental fixed-width types and small helpers.

#include <cstdint>
#include <cstdlib>
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


// Toggle de entorno con VALOR, no por presencia. Los conmutadores caros (hilos RCP,
// dynarec) van en ON por defecto: medido en SM64, threaded-jit corre al 99% de tiempo
// real y el modo interp/lockstep al 12.8%, asi que arrancar apagado por defecto era
// regalar 7x. `KESTREL_X=0|off|false|no` apaga; cualquier otro valor enciende; ausente
// = `def`. Hace falta el valor explicito para que las puertas de validacion puedan
// forzar el modo oraculo (interp puro) sin depender del default del build.
inline auto envFlag(const char* name, bool def) -> bool {
  const char* v = std::getenv(name);
  if(!v || !*v) return def;
  return !(v[0] == '0' || v[0] == 'n' || v[0] == 'N' || v[0] == 'f' || v[0] == 'F'
           || ((v[0] == 'o' || v[0] == 'O') && (v[1] == 'f' || v[1] == 'F')));
}

}  // namespace kestrel
