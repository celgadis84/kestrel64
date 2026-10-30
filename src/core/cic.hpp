#pragma once
// CIC-NUS boot checksum: the 48-bit value IPL2 computes over the cart's IPL3 (ROM 0x40..0xFFF)
// and hands to the PIF with command 0x20; the PIF compares it (0x40) with the one burnt into
// the cart's CIC and halts the CPU on a mismatch. The CIC's value is never sent to the CPU, so
// the only way a homebrew IPL3 boots is by being brute-force "signed" to produce a known one.
//
// Algorithm reverse-engineered from the PIF ROM (n64brew "PIF-NUS", jago85/PifChecksum);
// this transcription follows ares (mia/medium/nintendo-64.cpp, ipl2checksum), ISC license,
// Copyright (c) 2004-2025 ares team, Near et al.
#include "types.hpp"

namespace kestrel::cic {

inline constexpr u32 kIpl3Off   = 0x40;    // IPL3 image in ROM
inline constexpr u32 kIpl3Words = 1008;    // 0x40..0xFFF, big-endian words

inline auto ipl2Checksum(u8 seed, const u8* ipl3) -> u64 {
  auto rotl = [](u32 v, u32 s) -> u32 { s &= 31; return s ? (v << s) | (v >> (32 - s)) : v; };
  auto rotr = [](u32 v, u32 s) -> u32 { s &= 31; return s ? (v >> s) | (v << (32 - s)) : v; };
  auto csum = [](u32 a0, u32 a1, u32 a2) -> u32 {
    if(a1 == 0) a1 = a2;
    u64 prod = (u64)a0 * a1;
    u32 hi = (u32)(prod >> 32), lo = (u32)prod;
    u32 diff = hi - lo;
    return diff ? diff : a0;
  };
  u32 pos = 0;
  auto read = [&]() -> u32 {
    const u8* p = ipl3 + 4 * pos++;
    return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3];
  };

  u32 state[16];
  u32 init = 0x6c07'8965u * seed + 1;
  u32 data = read();
  init ^= data;
  for(auto& s : state) s = init;

  u32 dataNext = data, dataLast;
  u32 loop = 0;
  while(true) {
    loop++;
    dataLast = data;
    data = dataNext;
    state[0] += csum(1007 - loop, data, loop);
    state[1]  = csum(state[1], data, loop);
    state[2] ^= data;
    state[3] += csum(data + 5, 0x6c07'8965u, loop);
    state[9]  = dataLast < data ? csum(state[9], data, loop) : state[9] + data;
    state[4] += rotr(data, dataLast & 0x1f);
    state[7]  = csum(state[7], rotl(data, dataLast & 0x1f), loop);
    state[6]  = data < state[6] ? (state[3] + state[6]) ^ (data + loop) : (state[4] + data) ^ state[6];
    state[5] += rotl(data, dataLast >> 27);
    state[8]  = csum(state[8], rotr(data, dataLast >> 27), loop);
    if(loop == kIpl3Words) break;
    dataNext = read();
    state[15] = csum(csum(state[15], rotl(data, dataLast >> 27), loop), rotl(dataNext, data >> 27), loop);
    state[14] = csum(csum(state[14], rotr(data, dataLast & 0x1f), loop), rotr(dataNext, data & 0x1f), loop);
    state[13] += rotr(data, data & 0x1f) + rotr(dataNext, dataNext & 0x1f);
    state[10] = csum(state[10] + data, dataNext, loop);
    state[11] = csum(state[11] ^ data, dataNext, loop);
    state[12] += state[8] ^ data;
  }

  u32 buf[4] = { state[0], state[0], state[0], state[0] };
  for(loop = 0; loop < 16; loop++) {
    data = state[loop];
    u32 tmp = buf[0] + rotr(data, data & 0x1f);
    buf[0] = tmp;
    buf[1] = data < tmp ? buf[1] + data : csum(buf[1], data, loop);
    u32 b1 = (data & 2) >> 1, b0 = data & 1;
    buf[2] = b1 == b0 ? buf[2] + data : csum(buf[2], data, loop);
    buf[3] = b0 == 1 ? buf[3] ^ data : csum(buf[3], data, loop);
  }
  u64 sum = (u64)csum(buf[0], buf[1], 16) << 32 | (buf[3] ^ buf[2]);
  return sum & 0xffff'ffff'ffffull;
}

// Cart CIC chips: seed (handed to IPL2/IPL3 and left in s6) and the checksum the chip holds.
// NTSC/PAL pairs share seed and checksum; 6101 and 7102 share the seed but not the IPL3.
struct Chip { int idNtsc, idPal; u8 seed; u64 sum; };
inline constexpr Chip kChips[] = {
  { 6102, 7101, 0x3f, 0xA536'C0F1'D859ull },   // first: by far the most common
  { 6101, 6101, 0x3f, 0x45CC'73EE'317Aull },
  { 7102, 7102, 0x3f, 0x4416'0EC5'D9AFull },
  { 6103, 7103, 0x78, 0x586F'D470'9867ull },
  { 6105, 7105, 0x91, 0x8618'A45B'C2D3ull },
  { 6106, 7106, 0x85, 0x2BBA'D4E6'EB74ull },
  { 5101, 5101, 0xac, 0x93E9'83A8'F152ull },   // Aleck 64
};

// Which chip an IPL3 was signed for: the one whose seed makes IPL2 produce its checksum.
// Returns nullptr for an IPL3 no known chip accepts (real hardware would halt at 0x40).
inline auto chipForIpl3(const u8* ipl3) -> const Chip* {
  for(auto& c : kChips)
    if(ipl2Checksum(c.seed, ipl3) == c.sum) return &c;
  return nullptr;
}

}  // namespace kestrel::cic
