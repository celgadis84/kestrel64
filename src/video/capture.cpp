// kestrel64 -- captura de pantalla (PNG) y de video (AVI MJPEG + PCM). Ver capture.hpp.
//
// Sin dependencias: PNG con deflate propio (Huffman fijo + LZ77 con cadena de hash), JPEG
// base (4:2:0, tablas del anexo K) y el contenedor AVI 1.0 con idx1. Un AVI 1.0 no pasa de
// 2 GB y muchos reproductores se lian pasado 1 GB, asi que la grabacion se parte en trozos
// de 1 GB (<nombre>_002.avi, ...), cada uno valido por si solo.

#include "capture.hpp"
#include "../core/memory.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>

#ifdef _WIN32
#include <windows.h>
#endif

namespace kestrel::capture {
namespace {

// ============================================================ utilidades

auto le16(std::vector<u8>& v, u32 x) -> void { v.push_back((u8)x); v.push_back((u8)(x >> 8)); }
auto le32(std::vector<u8>& v, u32 x) -> void { for(int i = 0; i < 4; i++) v.push_back((u8)(x >> (8 * i))); }
auto be32(std::vector<u8>& v, u32 x) -> void { for(int i = 3; i >= 0; i--) v.push_back((u8)(x >> (8 * i))); }
auto put32(u8* p, u32 x) -> void { p[0] = (u8)x; p[1] = (u8)(x >> 8); p[2] = (u8)(x >> 16); p[3] = (u8)(x >> 24); }
auto fourcc(std::vector<u8>& v, const char* s) -> void { v.insert(v.end(), s, s + 4); }

auto crc32(const u8* p, usize n, u32 c = 0) -> u32 {
  static u32 t[256];
  static const bool init = [] {
    for(u32 i = 0; i < 256; i++) { u32 x = i; for(int k = 0; k < 8; k++) x = (x & 1) ? 0xedb88320u ^ (x >> 1) : x >> 1; t[i] = x; }
    return true; }();
  (void)init;
  c = ~c;
  for(usize i = 0; i < n; i++) c = t[(c ^ p[i]) & 0xff] ^ (c >> 8);
  return ~c;
}

auto mkdirs(const std::string& path) -> void {
#ifdef _WIN32
  for(usize i = 1; i <= path.size(); i++)
    if(i == path.size() || path[i] == '/' || path[i] == '\\') {
      std::string d = path.substr(0, i);
      if(d.size() > 2 || (d.size() == 2 && d[1] != ':')) CreateDirectoryA(d.c_str(), nullptr);
    }
#else
  (void)path;
#endif
}

auto dirOf(const std::string& p) -> std::string {
  usize s = p.find_last_of("/\\");
  return s == std::string::npos ? std::string() : p.substr(0, s);
}

std::mutex g_nameMu;
std::string g_rom = "kestrel64";

// Carpeta por defecto: KESTREL_CAPTURE_DIR o, si no, Imagenes\kestrel64 (fotos) y
// Videos\kestrel64 (video) del usuario. Ni la carpeta del exe (instalado, Program Files no
// se puede escribir) ni la de trabajo (cambia segun quien lance el emulador).
auto defaultDir(bool video) -> std::string {
  if(const char* d = std::getenv("KESTREL_CAPTURE_DIR")) if(*d) return d;
#ifdef _WIN32
  if(const char* u = std::getenv("USERPROFILE")) if(*u)
    return std::string(u) + (video ? "\\Videos\\kestrel64" : "\\Pictures\\kestrel64");
#endif
  return "captures";
}

auto defaultPath(bool video) -> std::string {
  std::string rom;
  { std::lock_guard<std::mutex> lk(g_nameMu); rom = g_rom; }
  std::string clean;
  for(char c : rom) clean += (std::isalnum((unsigned char)c) || c == '-' || c == '_') ? c : (c == ' ' ? '_' : '\0');
  clean.erase(std::remove(clean.begin(), clean.end(), '\0'), clean.end());
  while(!clean.empty() && clean.back() == '_') clean.pop_back();
  if(clean.empty()) clean = "kestrel64";
  std::time_t t = std::time(nullptr);
  std::tm tmv{};
#ifdef _WIN32
  localtime_s(&tmv, &t);
#else
  localtime_r(&t, &tmv);
#endif
  char stamp[32];
  std::strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", &tmv);
  std::string base = defaultDir(video) + "/" + clean + "_" + stamp;
  const char* ext = video ? ".avi" : ".png";
  std::string p = base + ext;
  for(int n = 2; n < 1000; n++) {               // dos fotos en el mismo segundo
    if(FILE* f = std::fopen(p.c_str(), "rb")) { std::fclose(f); p = base + "_" + std::to_string(n) + ext; }
    else break;
  }
  return p;
}

// ============================================================ deflate (Huffman fijo)

struct BitOut {
  std::vector<u8>& o;
  u32 acc = 0; int n = 0;
  explicit BitOut(std::vector<u8>& out) : o(out) {}
  auto bits(u32 v, int c) -> void { acc |= v << n; n += c; while(n >= 8) { o.push_back((u8)acc); acc >>= 8; n -= 8; } }
  auto huff(u32 code, int len) -> void {   // los codigos de Huffman van del bit alto al bajo
    u32 r = 0; for(int i = 0; i < len; i++) r |= ((code >> i) & 1) << (len - 1 - i);
    bits(r, len);
  }
  auto flush() -> void { if(n) { o.push_back((u8)acc); acc = 0; n = 0; } }
};

auto litCode(BitOut& b, u32 v) -> void {
  if(v < 144)      b.huff(0x30 + v, 8);
  else if(v < 256) b.huff(0x190 + (v - 144), 9);
  else if(v < 280) b.huff(v - 256, 7);
  else             b.huff(0xc0 + (v - 280), 8);
}

constexpr u16 kLenBase[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
constexpr u8  kLenExtra[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
constexpr u16 kDistBase[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
constexpr u8  kDistExtra[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

auto deflateFixed(const u8* in, usize n, std::vector<u8>& out) -> void {
  BitOut b(out);
  b.bits(1, 1);   // BFINAL
  b.bits(1, 2);   // BTYPE = 01, Huffman fijo
  constexpr u32 kHashBits = 15, kWin = 32768, kMaxChain = 32;
  std::vector<s32> head(1u << kHashBits, -1), prev(n ? n : 1, -1);
  auto hash = [&](usize i) -> u32 { return ((u32)in[i] << 10 ^ (u32)in[i + 1] << 5 ^ in[i + 2]) & ((1u << kHashBits) - 1); };
  usize i = 0;
  while(i < n) {
    u32 bestLen = 0, bestDist = 0;
    if(i + 2 < n) {
      u32 h = hash(i);
      s32 c = head[h];
      for(u32 chain = 0; c >= 0 && i - (usize)c <= kWin && chain < kMaxChain; chain++, c = prev[c]) {
        u32 len = 0, maxLen = (u32)std::min<usize>(258, n - i);
        while(len < maxLen && in[c + len] == in[i + len]) len++;
        if(len > bestLen) { bestLen = len; bestDist = (u32)(i - c); if(len == maxLen) break; }
      }
    }
    usize adv = 1;
    if(bestLen >= 3) {
      int lc = 28; while(kLenBase[lc] > bestLen) lc--;
      litCode(b, 257 + lc);
      if(kLenExtra[lc]) b.bits(bestLen - kLenBase[lc], kLenExtra[lc]);
      int dc = 29; while(kDistBase[dc] > bestDist) dc--;
      b.huff(dc, 5);
      if(kDistExtra[dc]) b.bits(bestDist - kDistBase[dc], kDistExtra[dc]);
      adv = bestLen;
    } else {
      litCode(b, in[i]);
    }
    for(usize k = 0; k < adv; k++, i++)
      if(i + 2 < n) { u32 h = hash(i); prev[i] = head[h]; head[h] = (s32)i; }
  }
  litCode(b, 256);   // fin de bloque
  b.flush();
}

// ============================================================ JPEG base 4:2:0

constexpr u8 kZig[64] = {0,1,8,16,9,2,3,10,17,24,32,25,18,11,4,5,12,19,26,33,40,48,41,34,27,20,13,6,7,14,21,28,
                         35,42,49,56,57,50,43,36,29,22,15,23,30,37,44,51,58,59,52,45,38,31,39,46,53,60,61,54,47,55,62,63};
constexpr u8 kQLum[64] = {16,11,10,16,24,40,51,61, 12,12,14,19,26,58,60,55, 14,13,16,24,40,57,69,56, 14,17,22,29,51,87,80,62,
                          18,22,37,56,68,109,103,77, 24,35,55,64,81,104,113,92, 49,64,78,87,103,121,120,101, 72,92,95,98,112,100,103,99};
constexpr u8 kQChr[64] = {17,18,24,47,99,99,99,99, 18,21,26,66,99,99,99,99, 24,26,56,99,99,99,99,99, 47,66,99,99,99,99,99,99,
                          99,99,99,99,99,99,99,99, 99,99,99,99,99,99,99,99, 99,99,99,99,99,99,99,99, 99,99,99,99,99,99,99,99};
constexpr u8 kDcLumBits[16] = {0,1,5,1,1,1,1,1,1,0,0,0,0,0,0,0};
constexpr u8 kDcChrBits[16] = {0,3,1,1,1,1,1,1,1,1,1,0,0,0,0,0};
constexpr u8 kDcVals[12] = {0,1,2,3,4,5,6,7,8,9,10,11};
constexpr u8 kAcLumBits[16] = {0,2,1,3,3,2,4,3,5,5,4,4,0,0,1,0x7d};
constexpr u8 kAcLumVals[162] = {
  0x01,0x02,0x03,0x00,0x04,0x11,0x05,0x12,0x21,0x31,0x41,0x06,0x13,0x51,0x61,0x07,0x22,0x71,0x14,0x32,0x81,0x91,0xa1,0x08,
  0x23,0x42,0xb1,0xc1,0x15,0x52,0xd1,0xf0,0x24,0x33,0x62,0x72,0x82,0x09,0x0a,0x16,0x17,0x18,0x19,0x1a,0x25,0x26,0x27,0x28,
  0x29,0x2a,0x34,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,0x59,
  0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x83,0x84,0x85,0x86,0x87,0x88,0x89,
  0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,0xb5,0xb6,
  0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,0xe1,0xe2,
  0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf1,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,0xfa};
constexpr u8 kAcChrBits[16] = {0,2,1,2,4,4,3,4,7,5,4,4,0,1,2,0x77};
constexpr u8 kAcChrVals[162] = {
  0x00,0x01,0x02,0x03,0x11,0x04,0x05,0x21,0x31,0x06,0x12,0x41,0x51,0x07,0x61,0x71,0x13,0x22,0x32,0x81,0x08,0x14,0x42,0x91,
  0xa1,0xb1,0xc1,0x09,0x23,0x33,0x52,0xf0,0x15,0x62,0x72,0xd1,0x0a,0x16,0x24,0x34,0xe1,0x25,0xf1,0x17,0x18,0x19,0x1a,0x26,
  0x27,0x28,0x29,0x2a,0x35,0x36,0x37,0x38,0x39,0x3a,0x43,0x44,0x45,0x46,0x47,0x48,0x49,0x4a,0x53,0x54,0x55,0x56,0x57,0x58,
  0x59,0x5a,0x63,0x64,0x65,0x66,0x67,0x68,0x69,0x6a,0x73,0x74,0x75,0x76,0x77,0x78,0x79,0x7a,0x82,0x83,0x84,0x85,0x86,0x87,
  0x88,0x89,0x8a,0x92,0x93,0x94,0x95,0x96,0x97,0x98,0x99,0x9a,0xa2,0xa3,0xa4,0xa5,0xa6,0xa7,0xa8,0xa9,0xaa,0xb2,0xb3,0xb4,
  0xb5,0xb6,0xb7,0xb8,0xb9,0xba,0xc2,0xc3,0xc4,0xc5,0xc6,0xc7,0xc8,0xc9,0xca,0xd2,0xd3,0xd4,0xd5,0xd6,0xd7,0xd8,0xd9,0xda,
  0xe2,0xe3,0xe4,0xe5,0xe6,0xe7,0xe8,0xe9,0xea,0xf2,0xf3,0xf4,0xf5,0xf6,0xf7,0xf8,0xf9,0xfa};

struct HuffTab { u16 code[256] = {}; u8 len[256] = {}; };

auto buildHuff(const u8* bits, const u8* vals) -> HuffTab {
  HuffTab t;
  u32 code = 0, k = 0;
  for(int l = 1; l <= 16; l++) {
    for(int i = 0; i < bits[l - 1]; i++) { t.code[vals[k]] = (u16)code; t.len[vals[k]] = (u8)l; code++; k++; }
    code <<= 1;
  }
  return t;
}

struct JpegBits {
  std::vector<u8>& o;
  u32 acc = 0; int n = 0;
  explicit JpegBits(std::vector<u8>& out) : o(out) {}
  auto put(u32 v, int c) -> void {
    acc = (acc << c) | (v & ((1u << c) - 1)); n += c;
    while(n >= 8) { u8 b = (u8)(acc >> (n - 8)); o.push_back(b); if(b == 0xff) o.push_back(0); n -= 8; }
  }
  auto flush() -> void { if(n) put(0x7f, 8 - n); }   // relleno con unos
};

struct JpegEnc {
  float fq[2][64];          // 1/(q*escala DCT), en orden natural
  u8 qz[2][64];             // tablas para DQT, en orden zigzag
  HuffTab dcL, acL, dcC, acC;
  float cosT[8][8];

  explicit JpegEnc(u32 quality) {
    quality = std::clamp<u32>(quality, 1, 100);
    u32 sc = quality < 50 ? 5000 / quality : 200 - quality * 2;
    for(int t = 0; t < 2; t++) for(int i = 0; i < 64; i++) {
      u32 q = ((t ? kQChr : kQLum)[i] * sc + 50) / 100;
      q = q < 1 ? 1 : q > 255 ? 255 : q;
      fq[t][i] = 1.0f / (float)q;
    }
    for(int t = 0; t < 2; t++) for(int k = 0; k < 64; k++) qz[t][k] = (u8)std::lround(1.0f / fq[t][kZig[k]]);
    dcL = buildHuff(kDcLumBits, kDcVals); acL = buildHuff(kAcLumBits, kAcLumVals);
    dcC = buildHuff(kDcChrBits, kDcVals); acC = buildHuff(kAcChrBits, kAcChrVals);
    for(int x = 0; x < 8; x++) for(int u = 0; u < 8; u++)
      cosT[x][u] = std::cos((2 * x + 1) * u * 3.14159265358979f / 16.0f) * (u ? 0.5f : 0.35355339f);
  }

  // DCT 2D separable (C(u)/2 ya metido en cosT) y cuantizacion, salida en zigzag.
  auto block(const float* in, int t, int* out) const -> void {
    float tmp[64];
    for(int y = 0; y < 8; y++) for(int u = 0; u < 8; u++) {
      float s = 0; for(int x = 0; x < 8; x++) s += in[y * 8 + x] * cosT[x][u]; tmp[y * 8 + u] = s;
    }
    for(int k = 0; k < 64; k++) {   // salida directamente en orden zigzag
      int idx = kZig[k], v = idx >> 3, u = idx & 7;
      float s = 0; for(int y = 0; y < 8; y++) s += tmp[y * 8 + u] * cosT[y][v];
      out[k] = (int)std::lround(s * fq[t][idx]);
    }
  }

  static auto category(int v) -> int { int a = v < 0 ? -v : v, c = 0; while(a) { c++; a >>= 1; } return c; }

  auto code(JpegBits& b, const int* z, int& dcPrev, const HuffTab& dc, const HuffTab& ac) const -> void {
    int d = z[0] - dcPrev; dcPrev = z[0];
    int c = category(d);
    b.put(dc.code[c], dc.len[c]);
    if(c) b.put(d < 0 ? d - 1 : d, c);
    int run = 0;
    for(int k = 1; k < 64; k++) {
      if(z[k] == 0) { run++; continue; }
      while(run >= 16) { b.put(ac.code[0xf0], ac.len[0xf0]); run -= 16; }
      int cc = category(z[k]);
      int sym = (run << 4) | cc;
      b.put(ac.code[sym], ac.len[sym]);
      b.put(z[k] < 0 ? z[k] - 1 : z[k], cc);
      run = 0;
    }
    if(run) b.put(ac.code[0], ac.len[0]);
  }
};

auto putDht(std::vector<u8>& o, u8 cls, const u8* bits, const u8* vals, int nv) -> void {
  o.push_back(0xff); o.push_back(0xc4);
  u32 len = 2 + 1 + 16 + nv;
  o.push_back((u8)(len >> 8)); o.push_back((u8)len);
  o.push_back(cls);
  o.insert(o.end(), bits, bits + 16);
  o.insert(o.end(), vals, vals + nv);
}

// ============================================================ AVI

struct AviWriter {
  FILE* f = nullptr;
  std::string path;
  u32 w = 0, h = 0, rate = 0, scale = 1;   // video: rate/scale cuadros por segundo
  u32 aRate = 0;
  bool audio = true;
  u64 frames = 0, aFrames = 0, aBytes = 0;
  u64 moviPos = 0;   // posicion del fourcc 'movi' (base de los desplazamientos de idx1)
  u32 maxChunk = 0, maxAChunk = 0;
  std::vector<u8> idx;

  auto open(const std::string& p) -> bool {
    path = p;
    f = std::fopen(p.c_str(), "wb+");
    if(!f) return false;
    std::vector<u8> hdr;
    header(hdr);
    std::fwrite(hdr.data(), 1, hdr.size(), f);
    moviPos = hdr.size() - 4;
    return true;
  }

  // Cabecera completa; se escribe al abrir con los contadores a 0 y se reescribe al cerrar
  // (mismo tamano: todo son campos fijos).
  auto header(std::vector<u8>& o) const -> void {
    const u32 nStreams = audio ? 2 : 1;
    const u32 hdrlSize = 4 + (8 + 56) + (12 + (8 + 56) + (8 + 40)) + (audio ? 12 + (8 + 56) + (8 + 18) : 0);
    fourcc(o, "RIFF"); le32(o, 0); fourcc(o, "AVI ");
    fourcc(o, "LIST"); le32(o, hdrlSize); fourcc(o, "hdrl");
    fourcc(o, "avih"); le32(o, 56);
    le32(o, rate ? (u32)((u64)1000000 * scale / rate) : 16683);
    le32(o, 0); le32(o, 0); le32(o, 0x10);           // AVIF_HASINDEX
    le32(o, (u32)frames); le32(o, 0); le32(o, nStreams);
    le32(o, std::max(maxChunk, maxAChunk) + 8); le32(o, w); le32(o, h);
    for(int i = 0; i < 4; i++) le32(o, 0);
    // video
    fourcc(o, "LIST"); le32(o, 4 + (8 + 56) + (8 + 40)); fourcc(o, "strl");
    fourcc(o, "strh"); le32(o, 56); fourcc(o, "vids"); fourcc(o, "MJPG");
    le32(o, 0); le32(o, 0); le32(o, 0);
    le32(o, scale); le32(o, rate); le32(o, 0); le32(o, (u32)frames);
    le32(o, maxChunk); le32(o, 0xffffffffu); le32(o, 0);
    le16(o, 0); le16(o, 0); le16(o, w); le16(o, h);
    fourcc(o, "strf"); le32(o, 40);
    le32(o, 40); le32(o, w); le32(o, h); le16(o, 1); le16(o, 24); fourcc(o, "MJPG");
    le32(o, w * h * 3); le32(o, 0); le32(o, 0); le32(o, 0); le32(o, 0);
    if(audio) {
      const u32 r = aRate ? aRate : 44100;
      fourcc(o, "LIST"); le32(o, 4 + (8 + 56) + (8 + 18)); fourcc(o, "strl");
      fourcc(o, "strh"); le32(o, 56); fourcc(o, "auds"); le32(o, 0);
      le32(o, 0); le32(o, 0); le32(o, 0);
      le32(o, 1); le32(o, r); le32(o, 0); le32(o, (u32)aFrames);   // escala 1 / tasa = muestras por segundo
      le32(o, maxAChunk); le32(o, 0xffffffffu); le32(o, 4);
      le16(o, 0); le16(o, 0); le16(o, 0); le16(o, 0);
      fourcc(o, "strf"); le32(o, 18);
      le16(o, 1); le16(o, 2); le32(o, r); le32(o, r * 4); le16(o, 4); le16(o, 16); le16(o, 0);
    }
    fourcc(o, "LIST"); le32(o, 0); fourcc(o, "movi");
  }

  auto bytes() const -> u64 { return f ? (u64)std::ftell(f) : 0; }

  auto chunk(const char* id, const u8* d, u32 n, bool key) -> void {
    u64 pos = (u64)std::ftell(f);
    u8 h8[8]; std::memcpy(h8, id, 4); put32(h8 + 4, n);
    std::fwrite(h8, 1, 8, f);
    std::fwrite(d, 1, n, f);
    if(n & 1) std::fputc(0, f);
    fourcc(idx, id); le32(idx, key ? 0x10 : 0); le32(idx, (u32)(pos - moviPos)); le32(idx, n);
  }
  auto video(const std::vector<u8>& jpg) -> void {
    chunk("00dc", jpg.data(), (u32)jpg.size(), true);
    maxChunk = std::max(maxChunk, (u32)jpg.size());
    frames++;
  }
  auto pcm(const std::vector<u8>& d) -> void {
    if(!audio || d.empty()) return;
    chunk("01wb", d.data(), (u32)d.size(), true);
    maxAChunk = std::max(maxAChunk, (u32)d.size());
    aBytes += d.size(); aFrames += d.size() / 4;
  }

  auto close() -> void {
    if(!f) return;
    u64 moviEnd = (u64)std::ftell(f);
    u8 h8[8]; std::memcpy(h8, "idx1", 4); put32(h8 + 4, (u32)idx.size());
    std::fwrite(h8, 1, 8, f);
    std::fwrite(idx.data(), 1, idx.size(), f);
    u64 end = (u64)std::ftell(f);
    std::vector<u8> hdr;
    header(hdr);
    put32(hdr.data() + 4, (u32)(end - 8));
    put32(hdr.data() + hdr.size() - 8, (u32)(moviEnd - moviPos));
    std::fseek(f, 0, SEEK_SET);
    std::fwrite(hdr.data(), 1, hdr.size(), f);
    std::fclose(f);
    f = nullptr;
  }
};

// ============================================================ grabacion

struct Item {
  bool isVideo = true;
  u32 w = 0, h = 0;
  std::vector<u8> data;   // RGB24 (video) o PCM s16le estereo (audio)
};

constexpr u64 kSegmentBytes = 1ull << 30;
constexpr usize kMaxQueue = 16;

struct Recorder {
  VideoOpts opt;
  std::string basePath;
  u32 outW = 0, outH = 0, rate = 0, scale = 1;
  std::atomic<u32> aRate{0};   // lo fija el nucleo con el primer buffer; lo lee el codificador
  double aPhase = 0;
  u64 aSamples = 0;            // muestras estereo metidas en la cola (hilo del nucleo)
  u32 fieldN = 0;

  std::mutex mu;
  std::condition_variable cv, cvSpace;
  std::deque<Item> q;
  bool quit = false;
  std::thread th;

  // del hilo del codificador
  AviWriter avi;
  u32 segments = 0;
  u64 frames = 0, aFrames = 0, closedBytes = 0;
  std::atomic<u64> statFrames{0}, statAFrames{0}, statBytes{0};
  std::string err;

  auto segPath(u32 n) const -> std::string {
    if(n <= 1) return basePath;
    usize dot = basePath.find_last_of('.');
    char suf[16]; std::snprintf(suf, sizeof suf, "_%03u", n);
    return dot == std::string::npos ? basePath + suf : basePath.substr(0, dot) + suf + basePath.substr(dot);
  }

  auto openSeg() -> bool {
    segments++;
    avi = AviWriter{};
    avi.w = outW; avi.h = outH; avi.rate = rate; avi.scale = scale;
    avi.audio = opt.audio; avi.aRate = aRate;
    if(!avi.open(segPath(segments))) { err = "no se puede crear " + segPath(segments); return false; }
    return true;
  }

  auto run() -> void {
    std::vector<u8> scaled, jpg;
    bool ok = openSeg();
    for(;;) {
      Item it;
      {
        std::unique_lock<std::mutex> lk(mu);
        cv.wait(lk, [&] { return quit || !q.empty(); });
        if(q.empty()) break;
        it = std::move(q.front()); q.pop_front();
      }
      cvSpace.notify_all();
      if(!ok) continue;
      if(it.isVideo) {
        const u8* src = it.data.data();
        if(it.w != outW || it.h != outH) {   // cambio de resolucion a media grabacion: al tamano del primero
          scaled.resize((usize)outW * outH * 3);
          for(u32 y = 0; y < outH; y++) {
            u32 sy = (u32)((u64)y * it.h / outH);
            for(u32 x = 0; x < outW; x++) {
              u32 sx = (u32)((u64)x * it.w / outW);
              std::memcpy(&scaled[((usize)y * outW + x) * 3], &it.data[((usize)sy * it.w + sx) * 3], 3);
            }
          }
          src = scaled.data();
        }
        jpg.clear();
        encodeJpeg(src, outW, outH, opt.quality, jpg);
        if(avi.bytes() + jpg.size() + 64 > kSegmentBytes) {
          closedBytes += avi.bytes(); avi.close();
          if(!openSeg()) { ok = false; continue; }
        }
        avi.video(jpg);
        statFrames.fetch_add(1, std::memory_order_relaxed);
      } else {
        if(avi.aRate == 0) avi.aRate = aRate;
        avi.pcm(it.data);
        statAFrames.fetch_add(it.data.size() / 4, std::memory_order_relaxed);
      }
      statBytes.store(closedBytes + avi.bytes(), std::memory_order_relaxed);
    }
    if(avi.f) { avi.aRate = aRate; avi.close(); }
  }

  auto push(Item&& it) -> void {
    std::unique_lock<std::mutex> lk(mu);
    cvSpace.wait(lk, [&] { return q.size() < kMaxQueue || quit; });
    q.push_back(std::move(it));
    lk.unlock();
    cv.notify_one();
  }

  auto queued() -> u32 { std::lock_guard<std::mutex> lk(mu); return (u32)q.size(); }

  auto finish() -> void {
    { std::lock_guard<std::mutex> lk(mu); quit = true; }
    cv.notify_all();
    if(th.joinable()) th.join();
  }
};

std::mutex g_recMu;                    // protege g_rec / g_armed (no la cola: esa tiene la suya)
std::unique_ptr<Recorder> g_rec;
bool g_armed = false;
VideoOpts g_armOpts;
std::atomic<bool> g_recording{false};
VideoStats g_last;

// ============================================================ fotos

struct ShotReq { u64 id; std::string path; };
struct ShotRes { u64 id; bool ok; std::string path, err; u32 w, h; };
std::mutex g_shotMu;
std::condition_variable g_shotCv;
std::vector<ShotReq> g_shotQ;
std::vector<ShotRes> g_shotDone;
std::atomic<u64> g_shotNext{1};
std::atomic<bool> g_shotPending{false};

auto serviceShots(Memory* mem) -> void {
  if(!g_shotPending.load(std::memory_order_acquire)) return;
  std::vector<ShotReq> reqs;
  { std::lock_guard<std::mutex> lk(g_shotMu); reqs.swap(g_shotQ); g_shotPending.store(false); }
  if(reqs.empty()) return;
  mem->rdpDrain();
  FbGrab g;
  const bool grabbed = grabFramebuffer(mem, g);
  std::vector<u8> png;
  if(grabbed) encodePng(g.rgb.data(), g.w, g.h, png);
  for(auto& r : reqs) {
    ShotRes res{r.id, false, r.path.empty() ? defaultPath(false) : r.path, {}, g.w, g.h};
    if(!grabbed) res.err = "sin framebuffer";
    else {
      mkdirs(dirOf(res.path));
      if(writeFile(res.path, png)) res.ok = true; else res.err = "no se puede escribir " + res.path;
    }
    std::printf("[captura] %s %s (%ux%u)\n", res.ok ? "foto" : "foto FALLIDA:", res.ok ? res.path.c_str() : res.err.c_str(), g.w, g.h);
    std::fflush(stdout);
    std::lock_guard<std::mutex> lk(g_shotMu);
    g_shotDone.push_back(std::move(res));
    if(g_shotDone.size() > 64) g_shotDone.erase(g_shotDone.begin());
  }
  g_shotCv.notify_all();
}

auto statsOf(Recorder& r) -> VideoStats {
  VideoStats s;
  s.recording = true;
  s.path = r.basePath;
  s.width = r.outW; s.height = r.outH;
  s.frames = r.statFrames.load();
  s.audioFrames = r.statAFrames.load();
  s.audioRate = r.aRate;
  s.fps = r.rate ? (double)r.rate / r.scale : 0;
  s.seconds = s.fps > 0 ? s.frames / s.fps : 0;
  s.bytes = r.statBytes.load();
  s.segments = r.segments;
  s.queued = r.queued();
  return s;
}

}  // namespace

// ============================================================ API

auto setRomName(const std::string& name) -> void { std::lock_guard<std::mutex> lk(g_nameMu); g_rom = name; }

auto requestScreenshot(const std::string& path) -> u64 {
  u64 id = g_shotNext.fetch_add(1);
  std::lock_guard<std::mutex> lk(g_shotMu);
  g_shotQ.push_back({id, path});
  g_shotPending.store(true, std::memory_order_release);
  return id;
}

auto waitScreenshot(u64 id, u32 timeoutMs, std::string& path, u32& w, u32& h, std::string& err) -> bool {
  std::unique_lock<std::mutex> lk(g_shotMu);
  ShotRes* found = nullptr;
  g_shotCv.wait_for(lk, std::chrono::milliseconds(timeoutMs), [&] {
    for(auto& r : g_shotDone) if(r.id == id) { found = &r; return true; }
    return false; });
  if(!found) { err = "la foto no se atendio a tiempo (nucleo ocupado o parado fuera del bucle)"; return false; }
  path = found->path; w = found->w; h = found->h; err = found->err;
  return found->ok;
}

auto startVideo(const VideoOpts& o, std::string& err) -> bool {
  std::lock_guard<std::mutex> lk(g_recMu);
  if(g_rec || g_armed) { err = "ya hay una grabacion en marcha"; return false; }
  g_armOpts = o;
  if(g_armOpts.every < 1) g_armOpts.every = 1;
  if(g_armOpts.scale < 1) g_armOpts.scale = 1;
  if(g_armOpts.scale > 4) g_armOpts.scale = 4;
  if(g_armOpts.quality < 1 || g_armOpts.quality > 100) g_armOpts.quality = 90;
  if(g_armOpts.path.empty()) g_armOpts.path = defaultPath(true);
  mkdirs(dirOf(g_armOpts.path));
  g_armed = true;
  g_recording.store(true);
  std::printf("[captura] grabando video en %s\n", g_armOpts.path.c_str());
  std::fflush(stdout);
  return true;
}

auto stopVideo() -> VideoStats {
  std::unique_ptr<Recorder> r;
  {
    std::lock_guard<std::mutex> lk(g_recMu);
    if(g_armed && !g_rec) { g_armed = false; g_recording.store(false); return VideoStats{}; }
    r = std::move(g_rec);
    g_armed = false;
  }
  if(!r) return g_last;
  r->finish();
  VideoStats s = statsOf(*r);
  s.recording = false;
  s.queued = 0;
  s.audioRate = r->aRate;
  if(r->avi.f == nullptr) s.bytes = r->closedBytes;   // (el ultimo trozo ya cerrado)
  { FILE* f = std::fopen(r->segPath(r->segments).c_str(), "rb");
    if(f) { std::fseek(f, 0, SEEK_END); s.bytes = r->closedBytes + (u64)std::ftell(f); std::fclose(f); } }
  g_recording.store(false);
  {
    std::lock_guard<std::mutex> lk(g_recMu);
    g_last = s;
  }
  if(!r->err.empty()) std::printf("[captura] video: %s\n", r->err.c_str());
  std::printf("[captura] video cerrado: %s (%llu cuadros, %.1f s, %ux%u, %.1f MB, %u trozo%s)\n",
              s.path.c_str(), (unsigned long long)s.frames, s.seconds, s.width, s.height,
              s.bytes / 1048576.0, s.segments, s.segments == 1 ? "" : "s");
  std::fflush(stdout);
  return s;
}

auto videoStats() -> VideoStats {
  std::lock_guard<std::mutex> lk(g_recMu);
  if(g_rec) return statsOf(*g_rec);
  if(g_armed) { VideoStats s; s.recording = true; s.path = g_armOpts.path; return s; }
  return g_last;
}

auto recording() -> bool { return g_recording.load(std::memory_order_relaxed); }

auto toggleVideo() -> void {
  if(recording()) { stopVideo(); return; }
  std::string err;
  if(!startVideo(VideoOpts{}, err)) std::printf("[captura] %s\n", err.c_str());
}

auto onIdle(Memory* mem) -> void { serviceShots(mem); }

auto onField(Memory* mem) -> void {
  serviceShots(mem);
  if(!g_recording.load(std::memory_order_relaxed)) return;
  std::lock_guard<std::mutex> lk(g_recMu);
  if(g_armed && !g_rec) {
    auto r = std::make_unique<Recorder>();
    r->opt = g_armOpts;
    r->basePath = g_armOpts.path;
    r->rate = mem->viFieldHzMilli;           // mismo reloj de invitado que el drenaje del AI
    r->scale = 1000 * g_armOpts.every;
    g_rec = std::move(r);
    g_armed = false;
  }
  Recorder* r = g_rec.get();
  if(!r) return;
  // El PCM del AVI es continuo: si el AI se calla (cargas, juego parado), el audio que
  // viene despues saldria adelantado. Con mas de 3 campos de retraso se rellena con
  // silencio hasta 1 campo de retraso (el desfase normal entre buffer del AI y campo).
  if(r->aRate && r->opt.audio && r->th.joinable() && r->rate) {
    const u64 fieldSmp = (u64)1000 * r->aRate / r->rate;
    const u64 want = (u64)r->fieldN * fieldSmp;
    if(r->aSamples + 3 * fieldSmp < want) {
      Item pad;
      pad.isVideo = false;
      pad.data.assign((usize)(want - fieldSmp - r->aSamples) * 4, 0);
      r->aSamples += pad.data.size() / 4;
      r->push(std::move(pad));
    }
  }
  if(r->fieldN++ % r->opt.every) return;
  mem->rdpDrain();
  FbGrab g;
  if(!grabFramebuffer(mem, g) || !g.w || !g.h) return;
  Item it;
  it.isVideo = true;
  const u32 s = r->opt.scale;
  if(s > 1) {
    it.w = g.w * s; it.h = g.h * s;
    it.data.resize((usize)it.w * it.h * 3);
    for(u32 y = 0; y < it.h; y++) for(u32 x = 0; x < it.w; x++)
      std::memcpy(&it.data[((usize)y * it.w + x) * 3], &g.rgb[((usize)(y / s) * g.w + x / s) * 3], 3);
  } else {
    it.w = g.w; it.h = g.h; it.data = std::move(g.rgb);
  }
  if(!r->th.joinable()) {                    // primer cuadro: fija tamano y arranca el codificador
    r->outW = it.w; r->outH = it.h;
    Recorder* rp = r;
    r->th = std::thread([rp] { rp->run(); });
  }
  r->push(std::move(it));
}

auto onAudio(const u8* base, u32 size, u32 addr, u32 len, u32 sampleRate) -> void {
  if(!g_recording.load(std::memory_order_relaxed)) return;
  std::lock_guard<std::mutex> lk(g_recMu);
  Recorder* r = g_rec.get();
  if(!r || !r->opt.audio || !r->th.joinable() || addr >= size || !sampleRate) return;
  if(len > size - addr) len = size - addr;
  len &= ~3u;
  if(!len) return;
  Item it;
  it.isVideo = false;
  if(!r->aRate) {
    r->aRate = sampleRate;
    // El AVI arranca las dos pistas en t=0: si el juego empieza a sonar despues de abrir la
    // grabacion (lo normal al grabar desde el arranque), el hueco va en silencio o todo el
    // audio saldria adelantado. El cuadro k es el campo k desde que se abrio.
    const u64 fields = r->fieldN ? r->fieldN - 1 : 0;
    const u64 pad = r->rate ? fields * 1000 * sampleRate / r->rate : 0;
    it.data.assign((usize)pad * 4, 0);
  }
  const usize head = it.data.size();
  it.data.resize(head + len);
  for(u32 i = 0; i < len; i += 2) { it.data[head + i] = base[addr + i + 1]; it.data[head + i + 1] = base[addr + i]; }   // BE -> LE
  // Otra tasa que la de la pista (el juego cambio el DAC): remuestreo lineal a la primera.
  // (head solo es distinto de 0 en el primer buffer, que fija la tasa y no pasa por aqui.)
  if(sampleRate * 100 < r->aRate * 99 || sampleRate * 100 > r->aRate * 101) {
    const s16* in = (const s16*)it.data.data();
    u32 nin = len / 4;
    std::vector<s16> out;
    double step = (double)sampleRate / r->aRate;
    for(; r->aPhase < nin; r->aPhase += step) {
      u32 i = (u32)r->aPhase; double fr = r->aPhase - i;
      u32 j = i + 1 < nin ? i + 1 : i;
      for(int c = 0; c < 2; c++) out.push_back((s16)(in[2 * i + c] * (1 - fr) + in[2 * j + c] * fr));
    }
    r->aPhase -= nin;
    it.data.assign((const u8*)out.data(), (const u8*)(out.data() + out.size()));
  }
  r->aSamples += it.data.size() / 4;
  r->push(std::move(it));
}

auto shutdown() -> void { if(recording()) stopVideo(); }

auto writeFile(const std::string& path, const std::vector<u8>& data) -> bool {
  FILE* f = std::fopen(path.c_str(), "wb");
  if(!f) return false;
  bool ok = std::fwrite(data.data(), 1, data.size(), f) == data.size();
  return std::fclose(f) == 0 && ok;
}

auto encodePng(const u8* rgb, u32 w, u32 h, std::vector<u8>& out) -> void {
  // Filtro por fila: el de menor suma de |residuo| (heuristica estandar de libpng).
  const usize stride = (usize)w * 3;
  std::vector<u8> raw;
  raw.reserve((stride + 1) * h);
  std::vector<u8> cand[5];
  for(auto& c : cand) c.resize(stride);
  for(u32 y = 0; y < h; y++) {
    const u8* cur = rgb + y * stride;
    const u8* up = y ? cur - stride : nullptr;
    u64 best = ~0ull; int bf = 0;
    for(int ft = 0; ft < 5; ft++) {
      u64 sum = 0;
      for(usize i = 0; i < stride; i++) {
        int a = i >= 3 ? cur[i - 3] : 0, b = up ? up[i] : 0, c = (up && i >= 3) ? up[i - 3] : 0;
        int pr = 0;
        switch(ft) {
          case 1: pr = a; break;
          case 2: pr = b; break;
          case 3: pr = (a + b) >> 1; break;
          case 4: { int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
                    pr = (pa <= pb && pa <= pc) ? a : pb <= pc ? b : c; break; }
        }
        u8 v = (u8)(cur[i] - pr);
        cand[ft][i] = v;
        sum += v < 128 ? v : 256 - v;
      }
      if(sum < best) { best = sum; bf = ft; }
    }
    raw.push_back((u8)bf);
    raw.insert(raw.end(), cand[bf].begin(), cand[bf].end());
  }
  std::vector<u8> z = {0x78, 0x01};
  deflateFixed(raw.data(), raw.size(), z);
  u32 a = 1, b = 0;
  for(u8 c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
  be32(z, (b << 16) | a);

  out.clear();
  static const u8 sig[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};
  out.insert(out.end(), sig, sig + 8);
  auto chunk = [&](const char* tag, const std::vector<u8>& d) {
    be32(out, (u32)d.size());
    usize start = out.size();
    out.insert(out.end(), tag, tag + 4);
    out.insert(out.end(), d.begin(), d.end());
    be32(out, crc32(&out[start], out.size() - start));
  };
  std::vector<u8> ihdr;
  be32(ihdr, w); be32(ihdr, h);
  ihdr.push_back(8); ihdr.push_back(2); ihdr.push_back(0); ihdr.push_back(0); ihdr.push_back(0);   // RGB8
  chunk("IHDR", ihdr);
  chunk("IDAT", z);
  chunk("IEND", {});
}

auto encodeJpeg(const u8* rgb, u32 w, u32 h, u32 quality, std::vector<u8>& out) -> void {
  static thread_local std::unique_ptr<JpegEnc> enc;
  static thread_local u32 encQ = 0;
  if(!enc || encQ != quality) { enc = std::make_unique<JpegEnc>(quality); encQ = quality; }
  const JpegEnc& E = *enc;

  out.push_back(0xff); out.push_back(0xd8);
  static const u8 app0[] = {0xff,0xe0,0,16,'J','F','I','F',0,1,1,0,0,1,0,1,0,0};
  out.insert(out.end(), app0, app0 + sizeof app0);
  for(int t = 0; t < 2; t++) {
    out.push_back(0xff); out.push_back(0xdb); out.push_back(0); out.push_back(67); out.push_back((u8)t);
    out.insert(out.end(), E.qz[t], E.qz[t] + 64);
  }
  static const u8 sof[] = {0xff,0xc0,0,17,8};
  out.insert(out.end(), sof, sof + 5);
  out.push_back((u8)(h >> 8)); out.push_back((u8)h); out.push_back((u8)(w >> 8)); out.push_back((u8)w);
  static const u8 comps[] = {3, 1,0x22,0, 2,0x11,1, 3,0x11,1};
  out.insert(out.end(), comps, comps + sizeof comps);
  putDht(out, 0x00, kDcLumBits, kDcVals, 12);
  putDht(out, 0x10, kAcLumBits, kAcLumVals, 162);
  putDht(out, 0x01, kDcChrBits, kDcVals, 12);
  putDht(out, 0x11, kAcChrBits, kAcChrVals, 162);
  static const u8 sos[] = {0xff,0xda,0,12,3, 1,0x00, 2,0x11, 3,0x11, 0,63,0};
  out.insert(out.end(), sos, sos + sizeof sos);

  JpegBits bits(out);
  int dcY = 0, dcCb = 0, dcCr = 0;
  float Y[4][64], Cb[64], Cr[64];
  int z[64];
  auto px = [&](u32 x, u32 y) -> const u8* {   // borde replicado hasta el multiplo de 16
    x = std::min(x, w - 1); y = std::min(y, h - 1);
    return rgb + ((usize)y * w + x) * 3;
  };
  for(u32 my = 0; my < h; my += 16) for(u32 mx = 0; mx < w; mx += 16) {
    float cbAcc[64] = {}, crAcc[64] = {};
    for(u32 yy = 0; yy < 16; yy++) for(u32 xx = 0; xx < 16; xx++) {
      const u8* p = px(mx + xx, my + yy);
      float r = p[0], g = p[1], b = p[2];
      float yv = 0.299f * r + 0.587f * g + 0.114f * b - 128.0f;
      float cb = -0.168736f * r - 0.331264f * g + 0.5f * b;
      float cr = 0.5f * r - 0.418688f * g - 0.081312f * b;
      Y[(yy >> 3) * 2 + (xx >> 3)][(yy & 7) * 8 + (xx & 7)] = yv;
      cbAcc[(yy >> 1) * 8 + (xx >> 1)] += cb * 0.25f;
      crAcc[(yy >> 1) * 8 + (xx >> 1)] += cr * 0.25f;
    }
    std::memcpy(Cb, cbAcc, sizeof Cb);
    std::memcpy(Cr, crAcc, sizeof Cr);
    for(int k = 0; k < 4; k++) { E.block(Y[k], 0, z); E.code(bits, z, dcY, E.dcL, E.acL); }
    E.block(Cb, 1, z); E.code(bits, z, dcCb, E.dcC, E.acC);
    E.block(Cr, 1, z); E.code(bits, z, dcCr, E.dcC, E.acC);
  }
  bits.flush();
  out.push_back(0xff); out.push_back(0xd9);
}

}  // namespace kestrel::capture
