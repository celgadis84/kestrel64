#include "hbmeta.hpp"
#include "archive.hpp"
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cstring>

#ifdef _WIN32
#include <windows.h>
#endif

namespace kestrel::hb {
namespace {

auto rd16(const u8* p) -> u32 { return (u32)p[0] | ((u32)p[1] << 8); }
auto rd32(const u8* p) -> u32 { return rd16(p) | (rd16(p + 2) << 16); }

auto readAll(const std::string& path, std::vector<u8>& out) -> bool {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if(!f) return false;
  std::fseek(f, 0, SEEK_END);
  const long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if(n <= 0) { std::fclose(f); return false; }
  out.resize((usize)n);
  const usize got = std::fread(out.data(), 1, out.size(), f);
  std::fclose(f);
  return got == out.size();
}

// A big-endian (.z64) en sitio; false si no es una ROM.
auto toBigEndian(std::vector<u8>& d) -> bool {
  if(d.size() < 0x40) return false;
  const u32 magic = (u32)d[0] << 24 | (u32)d[1] << 16 | (u32)d[2] << 8 | d[3];
  if(magic == 0x8037'1240) return true;
  if(magic == 0x4012'3780) {
    for(usize i = 0; i + 3 < d.size(); i += 4) { std::swap(d[i], d[i + 3]); std::swap(d[i + 1], d[i + 2]); }
    return true;
  }
  if(magic == 0x3780'4012) {
    for(usize i = 0; i + 1 < d.size(); i += 2) std::swap(d[i], d[i + 1]);
    return true;
  }
  return false;
}

auto trim(const std::string& s) -> std::string {
  const usize a = s.find_first_not_of(" \t\r\n");
  if(a == std::string::npos) return std::string();
  return s.substr(a, s.find_last_not_of(" \t\r\n") - a + 1);
}

auto lowerAscii(std::string s) -> std::string {
  for(char& c : s) if(c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  return s;
}

// Rutas de dentro del zip: relativas, sin "." ni ".." (lo pide la especificacion).
auto safeName(const std::string& s) -> bool {
  if(s.empty() || s[0] == '/' || s[0] == '\\' || s.find(':') != std::string::npos) return false;
  usize a = 0;
  while(a <= s.size()) {
    usize b = s.find_first_of("/\\", a);
    if(b == std::string::npos) b = s.size();
    const std::string part = s.substr(a, b - a);
    if(part == "." || part == "..") return false;
    a = b + 1;
  }
  return true;
}

using Section = std::vector<std::pair<std::string, std::string>>;

// Comentarios solo con ';' al principio; sin comillas, escapes ni continuaciones.
auto parseIni(const std::string& txt) -> std::vector<std::pair<std::string, Section>> {
  std::vector<std::pair<std::string, Section>> out;
  usize a = 0;
  if(txt.size() >= 3 && (u8)txt[0] == 0xEF && (u8)txt[1] == 0xBB && (u8)txt[2] == 0xBF) a = 3;
  while(a < txt.size()) {
    usize b = txt.find('\n', a);
    if(b == std::string::npos) b = txt.size();
    const std::string ln = trim(txt.substr(a, b - a));
    a = b + 1;
    if(ln.empty() || ln[0] == ';') continue;
    if(ln.front() == '[' && ln.back() == ']') {
      out.push_back({lowerAscii(trim(ln.substr(1, ln.size() - 2))), Section{}});
      continue;
    }
    const usize eq = ln.find('=');
    if(eq == std::string::npos || out.empty()) continue;
    out.back().second.push_back({lowerAscii(trim(ln.substr(0, eq))), trim(ln.substr(eq + 1))});
  }
  return out;
}

// [base] con [base.<lang>] encima, en el orden de [base] y lo nuevo detras.
auto merged(const std::vector<std::pair<std::string, Section>>& ini, const std::string& base,
            const std::string& lang) -> Section {
  Section r;
  auto put = [&](const std::string& k, const std::string& v) {
    for(auto& e : r) if(e.first == k) { e.second = v; return; }
    r.push_back({k, v});
  };
  for(const auto& s : ini) if(s.first == base) for(const auto& e : s.second) put(e.first, e.second);
  if(!lang.empty())
    for(const auto& s : ini) if(s.first == base + "." + lang) for(const auto& e : s.second) put(e.first, e.second);
  return r;
}

}  // namespace

auto Meta::get(const char* k) const -> std::string {
  for(const auto& e : kv) if(e.first == k) return e.second;
  return std::string();
}

auto Meta::file(const std::string& name, std::vector<u8>& out) const -> bool {
  return !zip.empty() && zipEntry(zip.data(), zip.size(), name, out);
}

auto zipEntry(const u8* p, usize n, const std::string& name, std::vector<u8>& out) -> bool {
  if(n < 22 || !safeName(name)) return false;
  usize eocd = 0;
  bool found = false;
  const usize floor_ = n > 66000 ? n - 66000 : 0;
  for(usize i = n - 22; ; i--) {
    if(rd32(p + i) == 0x0605'4b50u) { eocd = i; found = true; break; }
    if(i == floor_) break;
  }
  if(!found) return false;
  const u32 count = rd16(p + eocd + 10), cdSize = rd32(p + eocd + 12), cdOff = rd32(p + eocd + 16);
  // Zip pegado tras la ROM: sus posiciones cuentan desde el principio del zip, no del fichero.
  // El directorio central acaba justo donde empieza la marca de fin, asi sale la base.
  const usize base = eocd >= (usize)cdSize + cdOff ? eocd - cdSize - cdOff : 0;
  usize cd = base + cdOff;
  for(u32 i = 0; i < count && cd + 46 <= n; i++) {
    if(rd32(p + cd) != 0x0201'4b50u) return false;
    const u32 nameLen = rd16(p + cd + 28), extraLen = rd16(p + cd + 30), cmtLen = rd16(p + cd + 32);
    if(cd + 46 + nameLen > n) return false;
    const std::string nm((const char*)p + cd + 46, nameLen);
    if(nm == name) {
      const u32 method = rd16(p + cd + 10), crc = rd32(p + cd + 16), comp = rd32(p + cd + 20);
      const u32 usz = rd32(p + cd + 24);
      const usize local = base + rd32(p + cd + 42);
      if(local + 30 > n || rd32(p + local) != 0x0403'4b50u) return false;
      const usize start = local + 30 + rd16(p + local + 26) + rd16(p + local + 28);
      if(start + comp > n) return false;
      out.clear();
      if(method == 0) {
        out.assign(p + start, p + start + comp);
      } else if(method == 8) {
        std::string err;
        out.reserve(usz);
        if(!archive::inflate(p + start, comp, out, err)) return false;
      } else {
        return false;
      }
      return !crc || archive::crc32(out.data(), out.size()) == crc;
    }
    cd += 46 + nameLen + extraLen + cmtLen;
  }
  return false;
}

auto parseMeta(std::vector<u8> zip, const std::string& lang, Meta& out) -> bool {
  out = Meta{};
  std::vector<u8> ini;
  if(!zipEntry(zip.data(), zip.size(), "metadata.ini", ini)) return false;
  const auto sec = parseIni(std::string(ini.begin(), ini.end()));
  out.kv = merged(sec, "meta", lang);
  for(const auto& e : merged(sec, "boxart", lang)) if(!e.second.empty()) out.boxart.push_back(e);
  for(const auto& e : merged(sec, "cartart", lang)) if(!e.second.empty()) out.cartart.push_back(e);
  const std::string shots = out.get("screenshots");
  usize a = 0;
  while(a < shots.size()) {
    usize b = shots.find(',', a);
    if(b == std::string::npos) b = shots.size();
    const std::string s = trim(shots.substr(a, b - a));
    if(!s.empty()) out.screenshots.push_back(s);
    a = b + 1;
  }
  out.zip = std::move(zip);
  if(const std::string ld = out.get("long-desc"); !ld.empty()) {
    std::vector<u8> t;
    if(out.file(ld, t)) {
      std::string s(t.begin(), t.end());
      if(s.size() >= 3 && (u8)s[0] == 0xEF && (u8)s[1] == 0xBB && (u8)s[2] == 0xBF) s.erase(0, 3);
      out.longDesc = std::move(s);
    }
  }
  out.ok = true;
  return true;
}

auto probe(const std::string& romPath, Header& out) -> bool {
  out = Header{};
  std::vector<u8> d;
  const std::string e = lowerAscii(romPath.size() > 4 ? romPath.substr(romPath.size() - 4) : romPath);
  if(e == ".zip" || (e.size() >= 3 && e.substr(e.size() - 3) == ".gz")) {
    std::string what, err;
    if(!readAll(romPath, d) || !archive::unwrap(d, what, err)) return false;
  } else {
    std::FILE* f = std::fopen(romPath.c_str(), "rb");
    if(!f) return false;
    d.resize(0x40);
    const usize got = std::fread(d.data(), 1, d.size(), f);
    std::fclose(f);
    if(got != d.size()) return false;
  }
  d.resize(std::min<usize>(d.size(), 0x40));
  if(!toBigEndian(d)) return false;
  out = parseHeader(d.data());
  return true;
}

auto load(const std::string& romPath, const std::string& langIn) -> Meta {
  Meta m;
  const std::string lang = langIn.empty() ? userLang() : langIn;
  // Fuera manda: <rom>.meta (con la ruta sin contenedor ni extension) o <fichero>.meta.
  const std::string noCont = archive::stripContainerExt(romPath);
  std::string stem = noCont;
  if(const usize dot = noCont.find_last_of('.'), sep = noCont.find_last_of("/\\");
     dot != std::string::npos && (sep == std::string::npos || dot > sep))
    stem = noCont.substr(0, dot);
  for(const std::string& cand : {stem + ".meta", romPath + ".meta"}) {
    std::vector<u8> z;
    if(readAll(cand, z) && parseMeta(std::move(z), lang, m)) { m.source = "fichero .meta"; return m; }
  }
  Header h;
  if(!probe(romPath, h) || !h.ed || !h.hasMeta) return m;
  std::vector<u8> d;
  if(!readAll(romPath, d)) return m;
  std::string what, err;
  if(!archive::unwrap(d, what, err) || !toBigEndian(d)) return m;
  if(parseMeta(std::move(d), lang, m)) m.source = "embebido en la ROM";
  return m;
}

auto userLang() -> std::string {
  if(const char* e = std::getenv("KESTREL_LANG"); e && e[0]) return lowerAscii(std::string(e).substr(0, 2));
#ifdef _WIN32
  wchar_t loc[LOCALE_NAME_MAX_LENGTH] = {};
  if(GetUserDefaultLocaleName(loc, LOCALE_NAME_MAX_LENGTH) > 2 && loc[0] < 128 && loc[1] < 128)
    return lowerAscii(std::string{(char)loc[0], (char)loc[1]});
#endif
  if(const char* l = std::getenv("LANG"); l && std::strlen(l) >= 2) return lowerAscii(std::string(l, 2));
  return "en";
}

}  // namespace kestrel::hb
