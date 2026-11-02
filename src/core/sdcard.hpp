#pragma once
// Tarjeta SD del flashcart (la que expone la SummerCart64, ver Memory::sc*). Solo es un
// almacen de sectores de 512 bytes; el protocolo del cartucho vive en memory_cart.cpp.
//
// Dos respaldos, segun lo que apunte KESTREL_SD:
//   * un fichero: imagen cruda (.img) de la tarjeta, leida y escrita en su sitio;
//   * una carpeta: la carpeta del anfitrion se presenta como un volumen FAT32 sin tabla de
//     particiones, construido al vuelo. Solo se guardan en memoria las entradas de directorio
//     y la lista de tramos de clusters; los datos de los ficheros se leen del disco cuando el
//     juego pide su sector. Lo que el juego escriba se queda en una capa en memoria (no toca
//     la carpeta) y se pierde al salir.
//
// El volumen virtual sigue Microsoft "FAT32 File System Specification" 1.03: sector de
// arranque con BPB de FAT32, FSInfo en el sector 1, copia del arranque en el 6, dos FAT, y
// directorios con entradas cortas 8.3 mas las de nombre largo (LFN) cuando el nombre no cabe.
// FAT32 exige mas de 65524 clusters, asi que el volumen se rellena con clusters libres hasta
// pasar ese minimo.
#include "types.hpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace kestrel {

// E/S de 64 bits portable: las tarjetas pasan de 2 GB, que es donde `long` de 32 bits (Win64)
// deja de servir. En POSIX off_t ya es de 64 bits en x86-64.
namespace sdio {
#ifdef _WIN32
inline auto seek(FILE* f, long long off) -> int { return _fseeki64(f, off, SEEK_SET); }
inline auto tell(FILE* f) -> long long { return _ftelli64(f); }
inline auto openRead(const std::filesystem::path& p) -> FILE* { return _wfopen(p.c_str(), L"rb"); }
#else
inline auto seek(FILE* f, long long off) -> int { return fseeko(f, (off_t)off, SEEK_SET); }
inline auto tell(FILE* f) -> long long { return (long long)ftello(f); }
inline auto openRead(const std::filesystem::path& p) -> FILE* { return std::fopen(p.c_str(), "rb"); }
#endif
}  // namespace sdio

class SdCard {
public:
  ~SdCard() { close(); }

  auto open(const std::string& path) -> bool {
    close();
    std::error_code ec;
    if(std::filesystem::is_directory(path, ec)) return buildVirtual(path);
    img = std::fopen(path.c_str(), "r+b");
    if(!img) img = std::fopen(path.c_str(), "rb");
    if(!img) return false;
    std::fseek(img, 0, SEEK_END);
    const long long sz = sdio::tell(img);
    total = sz > 0 ? (u64)sz / 512 : 0;
    return total > 0;
  }
  auto close() -> void {
    if(img) { std::fclose(img); img = nullptr; }
    for(auto& n : nodes) if(n.fp) { std::fclose(n.fp); n.fp = nullptr; }
    nodes.clear(); extents.clear(); overlay.clear(); total = 0;
  }
  auto present() const -> bool { return total > 0; }
  auto sectors() const -> u64 { return total; }
  auto isVirtual() const -> bool { return !img && total > 0; }

  auto read(u64 lba, u8* dst) -> bool {
    if(lba >= total) return false;
    if(img) {
      if(sdio::seek(img, (long long)(lba * 512))) return false;
      const usize got = std::fread(dst, 1, 512, img);
      if(got < 512) std::memset(dst + got, 0, 512 - got);
      return true;
    }
    if(auto it = overlay.find(lba); it != overlay.end()) { std::memcpy(dst, it->second.data(), 512); return true; }
    virtualSector(lba, dst);
    return true;
  }
  auto write(u64 lba, const u8* src) -> bool {
    if(lba >= total) return false;
    if(img) {
      if(sdio::seek(img, (long long)(lba * 512))) return false;
      return std::fwrite(src, 1, 512, img) == 512;
    }
    std::memcpy(overlay[lba].data(), src, 512);
    return true;
  }

private:
  static constexpr u32 kSecPerClus = 8;            // 4 KiB por cluster
  static constexpr u32 kClus = kSecPerClus * 512;
  static constexpr u32 kReserved = 32;
  static constexpr u32 kMinClusters = 65525 + 64;   // FAT32: > 65524 clusters

  struct Node {
    std::filesystem::path host;
    std::string name;
    bool dir = false;
    u64 size = 0;                  // ficheros: bytes; directorios: bytes de entradas
    u32 first = 0, count = 0;      // tramo de clusters (0 si fichero vacio)
    int parent = -1;
    std::vector<int> kids;
    std::vector<u8> bytes;         // directorios: sus entradas
    std::FILE* fp = nullptr;
  };
  struct Extent { u32 first, count; int node; };

  std::FILE* img = nullptr;
  u64 total = 0;
  std::vector<Node> nodes;
  std::vector<Extent> extents;     // ordenados por cluster
  std::unordered_map<u64, std::array<u8, 512>> overlay;
  u32 fatSz = 0, clusters = 0, dataStart = 0;

  static auto put16(u8* p, u32 v) -> void { p[0] = (u8)v; p[1] = (u8)(v >> 8); }
  static auto put32(u8* p, u32 v) -> void { put16(p, v); put16(p + 2, v >> 16); }

  // Nombre 8.3 valido tal cual (mayusculas, sin caracteres prohibidos): no necesita LFN.
  static auto plain83(const std::string& n, u8 out[11]) -> bool {
    std::memset(out, ' ', 11);
    const auto dot = n.rfind('.');
    const std::string base = dot == std::string::npos ? n : n.substr(0, dot);
    const std::string ext = dot == std::string::npos ? "" : n.substr(dot + 1);
    if(base.empty() || base.size() > 8 || ext.size() > 3) return false;
    if(dot != std::string::npos && n.find('.') != dot) return false;
    auto ok = [](char c) {
      return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || std::strchr("$%'-_@~`!(){}^#&", c);
    };
    for(char c : base) if(!ok(c)) return false;
    for(char c : ext) if(!ok(c)) return false;
    std::memcpy(out, base.data(), base.size());
    std::memcpy(out + 8, ext.data(), ext.size());
    return true;
  }
  // Nombre corto de reserva "BASE~N.EXT" para un nombre largo.
  static auto alias83(const std::string& n, u32 seq, u8 out[11]) -> void {
    std::memset(out, ' ', 11);
    const auto dot = n.rfind('.');
    std::string base, ext;
    auto keep = [](char c) -> char {
      if(c >= 'a' && c <= 'z') return (char)(c - 32);
      if((c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) return c;
      return 0;
    };
    for(usize i = 0; i < (dot == std::string::npos ? n.size() : dot); i++) if(char c = keep(n[i])) base += c;
    if(dot != std::string::npos) for(usize i = dot + 1; i < n.size() && ext.size() < 3; i++) if(char c = keep(n[i])) ext += c;
    const std::string tail = "~" + std::to_string(seq);
    if(base.empty()) base = "_";
    base = base.substr(0, 8 - tail.size()) + tail;
    std::memcpy(out, base.data(), base.size());
    std::memcpy(out + 8, ext.data(), ext.size());
  }
  static auto lfnSum(const u8 sfn[11]) -> u8 {
    u8 s = 0;
    for(int i = 0; i < 11; i++) s = (u8)(((s & 1) << 7) + (s >> 1) + sfn[i]);
    return s;
  }
  static auto utf16(const std::string& s) -> std::u16string {
    std::u16string o;
    for(usize i = 0; i < s.size();) {
      u32 c = (u8)s[i];
      int n = c < 0x80 ? 1 : c < 0xe0 ? 2 : c < 0xf0 ? 3 : 4;
      if(n > 1) c &= 0x3f >> (n - 1);
      for(int k = 1; k < n && i + k < s.size(); k++) c = (c << 6) | ((u8)s[i + k] & 0x3f);
      i += n;
      if(c >= 0x10000) { c -= 0x10000; o += (char16_t)(0xd800 + (c >> 10)); o += (char16_t)(0xdc00 + (c & 0x3ff)); }
      else o += (char16_t)c;
    }
    return o;
  }

  // Entradas que ocupa un hijo en el directorio padre (LFN + la corta).
  struct Entry { u8 sfn[11]; std::u16string lfn; bool needLfn; };

  auto scan(int idx) -> void {
    std::error_code ec;
    std::vector<std::filesystem::directory_entry> list;
    for(auto& e : std::filesystem::directory_iterator(nodes[idx].host, ec)) list.push_back(e);
    std::sort(list.begin(), list.end(), [](auto& a, auto& b) { return a.path().filename() < b.path().filename(); });
    for(auto& e : list) {
      Node n;
      n.host = e.path();
      { auto u = e.path().filename().u8string(); n.name.assign(u.begin(), u.end()); }
      n.dir = e.is_directory(ec);
      n.size = n.dir ? 0 : (u64)e.file_size(ec);
      if(!n.dir && n.size > 0xffff'ffffull) {
        std::fprintf(stderr, "[sd] %s > 4 GiB, no cabe en FAT32: omitido\n", n.name.c_str());
        continue;
      }
      n.parent = idx;
      nodes.push_back(std::move(n));
      const int k = (int)nodes.size() - 1;
      nodes[idx].kids.push_back(k);
      if(nodes[k].dir) scan(k);
    }
  }
  auto entriesOf(int idx) -> std::vector<Entry> {
    std::vector<Entry> out;
    u32 seq = 1;
    for(int k : nodes[idx].kids) {
      Entry e{};
      e.needLfn = !plain83(nodes[k].name, e.sfn);
      if(e.needLfn) {
        // El alias no puede repetir otro nombre corto del mismo directorio.
        for(;; seq++) {
          alias83(nodes[k].name, seq, e.sfn);
          bool clash = false;
          for(auto& o : out) clash |= !std::memcmp(o.sfn, e.sfn, 11);
          if(!clash) break;
        }
        e.lfn = utf16(nodes[k].name);
      }
      out.push_back(e);
    }
    return out;
  }
  static auto dirEntry(u8* p, const u8 sfn[11], bool dir, u32 clus, u32 size) -> void {
    std::memcpy(p, sfn, 11);
    p[11] = dir ? 0x10 : 0x20;
    const u32 date = ((2026 - 1980) << 9) | (1 << 5) | 1, time = 0;
    put16(p + 14, time); put16(p + 16, date); put16(p + 18, date);
    put16(p + 20, clus >> 16); put16(p + 22, time); put16(p + 24, date);
    put16(p + 26, clus & 0xffff); put32(p + 28, dir ? 0 : size);
  }

  auto buildVirtual(const std::string& path) -> bool {
    Node root;
    root.host = std::filesystem::path(path);
    root.dir = true;
    nodes.push_back(std::move(root));
    scan(0);
    // Tamano de cada directorio y reparto de clusters, en orden de nodos (contiguo por nodo).
    std::vector<std::vector<Entry>> ents(nodes.size());
    u32 next = 2;
    for(int i = 0; i < (int)nodes.size(); i++) {
      auto& n = nodes[i];
      if(n.dir) {
        ents[i] = entriesOf(i);
        u64 cnt = i ? 2 : 0;                              // "." y ".."
        for(auto& e : ents[i]) cnt += 1 + (e.needLfn ? (e.lfn.size() + 12) / 13 : 0);
        n.size = (cnt + 1) * 32;                          // + marca de fin
      }
      n.count = (u32)((n.size + kClus - 1) / kClus);
      if(n.dir && !n.count) n.count = 1;
      if(n.count) { n.first = next; next += n.count; extents.push_back({n.first, n.count, i}); }
    }
    clusters = std::max(next - 2 + 1024, kMinClusters);
    fatSz = ((clusters + 2) * 4 + 511) / 512;
    dataStart = kReserved + 2 * fatSz;
    total = (u64)dataStart + (u64)clusters * kSecPerClus;
    // Entradas de directorio.
    for(int i = 0; i < (int)nodes.size(); i++) {
      auto& n = nodes[i];
      if(!n.dir) continue;
      n.bytes.assign((usize)n.count * kClus, 0);
      u8* p = n.bytes.data();
      if(i) {
        u8 dot[11], dotdot[11];
        std::memset(dot, ' ', 11); dot[0] = '.';
        std::memset(dotdot, ' ', 11); dotdot[0] = dotdot[1] = '.';
        dirEntry(p, dot, true, n.first, 0); p += 32;
        dirEntry(p, dotdot, true, n.parent > 0 ? nodes[n.parent].first : 0, 0); p += 32;
      }
      for(usize j = 0; j < n.kids.size(); j++) {
        const auto& e = ents[i][j];
        const auto& c = nodes[n.kids[j]];
        if(e.needLfn) {
          const u8 sum = lfnSum(e.sfn);
          const int parts = (int)((e.lfn.size() + 12) / 13);
          for(int part = parts; part >= 1; part--) {
            static const int off[13] = {1, 3, 5, 7, 9, 14, 16, 18, 20, 22, 24, 28, 30};
            p[0] = (u8)(part | (part == parts ? 0x40 : 0));
            p[11] = 0x0f; p[12] = 0; p[13] = sum; put16(p + 26, 0);
            for(int k = 0; k < 13; k++) {
              const usize ci = (usize)(part - 1) * 13 + k;
              const u32 ch = ci < e.lfn.size() ? (u32)e.lfn[ci] : ci == e.lfn.size() ? 0u : 0xffffu;
              put16(p + off[k], ch);
            }
            p += 32;
          }
        }
        dirEntry(p, e.sfn, c.dir, c.first, (u32)c.size);
        p += 32;
      }
    }
    std::fprintf(stderr, "[sd] carpeta %s -> FAT32 virtual: %zu entradas, %u clusters de 4 KiB\n",
                 path.c_str(), nodes.size() - 1, clusters);
    return true;
  }

  auto findExtent(u32 clus) -> const Extent* {
    auto it = std::upper_bound(extents.begin(), extents.end(), clus,
                               [](u32 c, const Extent& e) { return c < e.first; });
    if(it == extents.begin()) return nullptr;
    --it;
    return clus < it->first + it->count ? &*it : nullptr;
  }

  auto virtualSector(u64 lba, u8* d) -> void {
    std::memset(d, 0, 512);
    if(lba == 0 || lba == 6) {                              // arranque (y su copia)
      d[0] = 0xeb; d[1] = 0x58; d[2] = 0x90;
      std::memcpy(d + 3, "KESTREL ", 8);
      put16(d + 11, 512); d[13] = (u8)kSecPerClus; put16(d + 14, kReserved);
      d[16] = 2; d[21] = 0xf8; put16(d + 24, 63); put16(d + 26, 255);
      put32(d + 32, (u32)total); put32(d + 36, fatSz);
      put32(d + 44, 2); put16(d + 48, 1); put16(d + 50, 6);
      d[64] = 0x80; d[66] = 0x29; put32(d + 67, 0x4b363453);
      std::memcpy(d + 71, "KESTREL SD ", 11); std::memcpy(d + 82, "FAT32   ", 8);
      d[510] = 0x55; d[511] = 0xaa;
      return;
    }
    if(lba == 1 || lba == 7) {                              // FSInfo
      put32(d, 0x41615252); put32(d + 484, 0x61417272);
      put32(d + 488, 0xffffffff); put32(d + 492, 0xffffffff);
      put32(d + 508, 0xaa550000);
      return;
    }
    if(lba < kReserved) return;
    if(lba < dataStart) {                                   // las dos FAT, iguales
      const u32 s = (u32)((lba - kReserved) % fatSz);
      for(u32 k = 0; k < 128; k++) {
        const u32 e = s * 128 + k;
        u32 v = 0;
        if(e == 0) v = 0x0ffffff8;
        else if(e == 1) v = 0x0fffffff;
        else if(const Extent* x = findExtent(e)) v = e + 1 < x->first + x->count ? e + 1 : 0x0fffffff;
        put32(d + k * 4, v);
      }
      return;
    }
    const u64 rel = lba - dataStart;
    const u32 clus = (u32)(rel / kSecPerClus) + 2;
    const Extent* x = findExtent(clus);
    if(!x) return;
    Node& n = nodes[x->node];
    const u64 off = (u64)(clus - x->first) * kClus + (rel % kSecPerClus) * 512;
    if(n.dir) { std::memcpy(d, n.bytes.data() + off, 512); return; }
    if(off >= n.size) return;
    if(!n.fp) n.fp = sdio::openRead(n.host);
    if(!n.fp || sdio::seek(n.fp, (long long)off)) return;
    std::fread(d, 1, (usize)std::min<u64>(512, n.size - off), n.fp);
  }
};

}  // namespace kestrel
