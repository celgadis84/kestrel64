#include "gamecard.hpp"
#include "profile.hpp"
#include "../core/archive.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

namespace kestrel::ui::card {
namespace {

using kestrel::u8;
using kestrel::usize;

auto lower(std::string s) -> std::string {
  for(char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

auto trim(const std::string& s) -> std::string {
  usize a = s.find_first_not_of(" \t\r\n");
  usize b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// Solo letras y digitos ASCII, en minuscula: "Space Demo 64 (USA)" -> "spacedemo6464usa".
auto norm(const std::string& s) -> std::string {
  std::string o;
  for(char c : s)
    if(std::isalnum((unsigned char)c) && (unsigned char)c < 128) o.push_back((char)std::tolower(c));
  return o;
}

auto dirOf(const std::string& p) -> std::string {
  usize k = p.find_last_of("/\\");
  return k == std::string::npos ? std::string() : p.substr(0, k);
}

auto join(const std::string& d, const std::string& f) -> std::string {
  if(d.empty()) return f;
  char last = d.back();
  return (last == '/' || last == '\\') ? d + f : d + "/" + f;
}

// Extension de un nombre (con el punto, en minuscula), sin confundir un punto del directorio.
auto extOf(const std::string& f) -> std::string {
  usize d = f.find_last_of('.');
  usize s = f.find_last_of("/\\");
  if(d == std::string::npos || (s != std::string::npos && d < s)) return {};
  return lower(f.substr(d));
}

struct Stat {
  bool file = false, dir = false;
  unsigned long long size = 0;
  long long mtime = 0;
};

auto statOf(const std::string& p) -> Stat {
  Stat st;
#ifdef _WIN32
  WIN32_FILE_ATTRIBUTE_DATA a;
  if(!GetFileAttributesExA(p.c_str(), GetFileExInfoStandard, &a)) return st;
  st.dir = (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
  st.file = !st.dir;
  st.size = ((unsigned long long)a.nFileSizeHigh << 32) | a.nFileSizeLow;
  unsigned long long t =
      ((unsigned long long)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
  st.mtime = (long long)(t / 10000000ull) - 11644473600ll;   // FILETIME -> unix
#else
  struct stat s;
  if(::stat(p.c_str(), &s) != 0) return st;
  st.dir = S_ISDIR(s.st_mode);
  st.file = S_ISREG(s.st_mode);
  st.size = (unsigned long long)s.st_size;
  st.mtime = (long long)s.st_mtime;
#endif
  return st;
}

auto listFiles(const std::string& dir) -> std::vector<std::string> {
  std::vector<std::string> out;
#ifdef _WIN32
  WIN32_FIND_DATAA fd;
  HANDLE h = FindFirstFileA(join(dir, "*").c_str(), &fd);
  if(h == INVALID_HANDLE_VALUE) return out;
  do {
    if(!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(fd.cFileName);
  } while(FindNextFileA(h, &fd));
  FindClose(h);
#else
  DIR* d = opendir(dir.c_str());
  if(!d) return out;
  while(dirent* e = readdir(d))
    if(statOf(join(dir, e->d_name)).file) out.push_back(e->d_name);
  closedir(d);
#endif
  std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) {
    return lower(a) < lower(b);
  });
  return out;
}

auto makeDir(const std::string& d) -> void {
#ifdef _WIN32
  CreateDirectoryA(d.c_str(), nullptr);
#else
  mkdir(d.c_str(), 0777);
#endif
}

auto readAll(const std::string& f, std::string& out) -> bool {
  std::FILE* fp = std::fopen(f.c_str(), "rb");
  if(!fp) return false;
  out.clear();
  char buf[65536];
  usize n;
  while((n = std::fread(buf, 1, sizeof buf, fp)) > 0) out.append(buf, n);
  std::fclose(fp);
  return true;
}

// Escritura atomica: fichero temporal y sustitucion, para que un corte a mitad no deje la
// partida de trucos (o la ficha) a medias.
auto writeAtomic(const std::string& f, const std::string& data) -> bool {
  makeDir(dirOf(f));
  const std::string tmp = f + ".tmp";
  std::FILE* fp = std::fopen(tmp.c_str(), "wb");
  if(!fp) return false;
  bool ok = std::fwrite(data.data(), 1, data.size(), fp) == data.size();
  ok = std::fclose(fp) == 0 && ok;
  if(!ok) { std::remove(tmp.c_str()); return false; }
#ifdef _WIN32
  if(!MoveFileExA(tmp.c_str(), f.c_str(), MOVEFILE_REPLACE_EXISTING)) {
    std::remove(tmp.c_str());
    return false;
  }
#else
  if(std::rename(tmp.c_str(), f.c_str()) != 0) { std::remove(tmp.c_str()); return false; }
#endif
  return true;
}

// ---------------------------------------------------------------- lineas del .cht
// Se parte en lineas como splitlines de Python (\r\n, \n o \r) y se recuerda el fin de linea
// que usaba el fichero para escribirlo igual.
auto readLines(const std::string& f, std::vector<std::string>& lines, std::string& nl) -> bool {
  std::string raw;
  if(!readAll(f, raw)) return false;
  nl = raw.find("\r\n") != std::string::npos ? "\r\n" : "\n";
  lines.clear();
  std::string cur;
  for(usize i = 0; i < raw.size(); i++) {
    char c = raw[i];
    if(c == '\r' || c == '\n') {
      lines.push_back(cur);
      cur.clear();
      if(c == '\r' && i + 1 < raw.size() && raw[i + 1] == '\n') i++;
    } else {
      cur.push_back(c);
    }
  }
  if(!cur.empty()) lines.push_back(cur);
  return true;
}

auto writeLines(const std::string& f, const std::vector<std::string>& lines,
                const std::string& nl) -> bool {
  std::string out;
  for(const auto& l : lines) out += l + nl;
  if(lines.empty()) out = nl;
  return writeAtomic(f, out);
}

auto body(const std::string& ln) -> std::string {
  usize k = ln.find_first_of("#;");
  return trim(k == std::string::npos ? ln : ln.substr(0, k));
}

auto isHex(const std::string& t) -> bool {
  if(t.empty() || t.size() > 8) return false;
  for(char c : t)
    if(!std::isxdigit((unsigned char)c)) return false;
  return true;
}

// Dos numeros hexadecimales con los separadores del emulador (blanco, coma, dos puntos).
auto code(const std::string& b, std::string& out) -> bool {
  std::vector<std::string> tok;
  std::string cur;
  for(char c : b) {
    if(c == ' ' || c == '\t' || c == ',' || c == ':' || c == '\r' || c == '\n') {
      if(!cur.empty()) tok.push_back(cur), cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if(!cur.empty()) tok.push_back(cur);
  if(tok.size() < 2 || !isHex(tok[0]) || !isHex(tok[1])) return false;
  char buf[32];
  std::snprintf(buf, sizeof buf, "%08lX %04lX", std::strtoul(tok[0].c_str(), nullptr, 16),
                std::strtoul(tok[1].c_str(), nullptr, 16) & 0xFFFFul);
  out = buf;
  return true;
}

// ---------------------------------------------------------------- JSON
auto jsonStr(const std::string& s) -> std::string {
  std::string o = "\"";
  for(unsigned char c : s) {
    switch(c) {
      case '"': o += "\\\""; break;
      case '\\': o += "\\\\"; break;
      case '\n': o += "\\n"; break;
      case '\r': o += "\\r"; break;
      case '\t': o += "\\t"; break;
      default:
        if(c < 0x20) {
          char b[8];
          std::snprintf(b, sizeof b, "\\u%04x", c);
          o += b;
        } else {
          o.push_back((char)c);   // UTF-8 tal cual, como ensure_ascii=False del lanzador
        }
    }
  }
  return o + "\"";
}

auto putUtf8(std::string& o, unsigned cp) -> void {
  if(cp < 0x80) o.push_back((char)cp);
  else if(cp < 0x800) {
    o.push_back((char)(0xC0 | (cp >> 6)));
    o.push_back((char)(0x80 | (cp & 0x3F)));
  } else if(cp < 0x10000) {
    o.push_back((char)(0xE0 | (cp >> 12)));
    o.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
    o.push_back((char)(0x80 | (cp & 0x3F)));
  } else {
    o.push_back((char)(0xF0 | (cp >> 18)));
    o.push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
    o.push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
    o.push_back((char)(0x80 | (cp & 0x3F)));
  }
}

// Decodifica una cadena JSON (con sus comillas). Vale tambien para un valor que no es cadena:
// devuelve su texto crudo.
auto jsonUnstr(const std::string& raw) -> std::string {
  if(raw.size() < 2 || raw[0] != '"') return raw;
  std::string o;
  for(usize i = 1; i + 1 < raw.size(); i++) {
    char c = raw[i];
    if(c != '\\' || i + 2 >= raw.size()) { o.push_back(c); continue; }
    char e = raw[++i];
    switch(e) {
      case 'n': o.push_back('\n'); break;
      case 'r': o.push_back('\r'); break;
      case 't': o.push_back('\t'); break;
      case 'b': o.push_back('\b'); break;
      case 'f': o.push_back('\f'); break;
      case 'u': {
        if(i + 4 >= raw.size()) break;
        unsigned cp = (unsigned)std::strtoul(raw.substr(i + 1, 4).c_str(), nullptr, 16);
        i += 4;
        // Par sustituto: Python escribe asi lo que pasa de 16 bits si ensure_ascii=True.
        if(cp >= 0xD800 && cp < 0xDC00 && i + 6 < raw.size() && raw[i + 1] == '\\' &&
           raw[i + 2] == 'u') {
          unsigned lo = (unsigned)std::strtoul(raw.substr(i + 3, 4).c_str(), nullptr, 16);
          if(lo >= 0xDC00 && lo < 0xE000) {
            cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
            i += 6;
          }
        }
        putUtf8(o, cp);
        break;
      }
      default: o.push_back(e); break;   // \" \\ \/
    }
  }
  return o;
}

// Definidas mas abajo, con el resto de la normalizacion de nombres.
auto titleKey(const std::string& name) -> std::string;
auto stemKey(const std::string& stem) -> std::string;

}  // namespace

// ================================================================ nombres y cabecera

auto romBase(const std::string& path) -> std::string {
  const std::string p = archive::stripContainerExt(path);
  usize d = p.find_last_of('.');
  usize s = p.find_last_of("/\\");
  if(d == std::string::npos || (s != std::string::npos && d < s)) return p;
  return p.substr(0, d);
}

auto baseName(const std::string& path) -> std::string {
  usize k = path.find_last_of("/\\");
  return k == std::string::npos ? path : path.substr(k + 1);
}

auto readHeader(const std::string& path) -> Header {
  Header h;
  std::string raw;
  const std::string e = extOf(path);
  if(e == ".zip" || e == ".gz") {
    if(!readAll(path, raw)) return h;
    std::vector<u8> v(raw.begin(), raw.end());
    std::string what, err;
    if(!archive::unwrap(v, what, err) || v.size() < 0x40) return h;
    raw.assign(v.begin(), v.begin() + 0x40);
  } else {
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if(!f) return h;
    char b[0x40];
    usize got = std::fread(b, 1, sizeof b, f);
    std::fclose(f);
    raw.assign(b, got);
  }
  if(raw.size() < 0x40) return h;
  u8 b[0x40];
  std::memcpy(b, raw.data(), 0x40);
  unsigned magic = ((unsigned)b[0] << 24) | ((unsigned)b[1] << 16) | ((unsigned)b[2] << 8) | b[3];
  if(magic == 0x37804012u) {
    for(int i = 0; i < 0x40; i += 2) std::swap(b[i], b[i + 1]);
  } else if(magic == 0x40123780u) {
    for(int i = 0; i < 0x40; i += 4) {
      std::swap(b[i], b[i + 3]);
      std::swap(b[i + 1], b[i + 2]);
    }
  } else if(magic != 0x80371240u) {
    return h;
  }
  h.ok = true;
  auto be = [&](int o) {
    return ((unsigned)b[o] << 24) | ((unsigned)b[o + 1] << 16) | ((unsigned)b[o + 2] << 8) | b[o + 3];
  };
  char crc[24];
  std::snprintf(crc, sizeof crc, "%08X%08X", be(0x10), be(0x14));
  h.crc = crc;
  std::string n;
  for(int i = 0x20; i < 0x34; i++)
    if(b[i] >= 32 && b[i] < 127) n.push_back((char)b[i]);
  h.name = trim(n);
  for(int i = 0x3B; i < 0x3E; i++)
    if(b[i] >= 32 && b[i] < 127) h.cart.push_back((char)b[i]);
  h.region = (b[0x3E] >= 32 && b[0x3E] < 127) ? (char)b[0x3E] : '?';
  return h;
}

// ================================================================ partidas

auto saves(const std::string& path) -> std::vector<SaveFile> {
  const std::string base = romBase(path);
  std::vector<std::pair<std::string, std::string>> kinds = {
      {".eep", "EEPROM"}, {".sra", "SRAM"}, {".fla", "FlashRAM"}, {".mpk", "Controller Pak 1"}};
  for(int i = 2; i <= 4; i++)
    kinds.push_back({".mpk" + std::to_string(i), "Controller Pak " + std::to_string(i)});
  for(int i = 0; i < 10; i++)
    kinds.push_back({".st" + std::to_string(i), "Estado " + std::to_string(i)});
  std::vector<SaveFile> out;
  for(const auto& k : kinds) {
    const std::string f = base + k.first;
    Stat st = statOf(f);
    if(!st.file) continue;
    SaveFile s;
    s.kind = k.first.substr(1);
    s.label = k.second;
    s.file = baseName(f);
    s.size = st.size;
    s.mtime = st.mtime;
    out.push_back(s);
  }
  return out;
}

// ================================================================ manuales

auto manualExtOk(const std::string& file) -> bool {
  static const char* kExt[] = {".pdf", ".html", ".htm", ".txt", ".md",
                               ".png", ".jpg", ".jpeg", ".webp"};
  const std::string e = extOf(file);
  for(const char* k : kExt)
    if(e == k) return true;
  return false;
}

auto manuals(const std::string& path, const std::string& internalName) -> std::vector<std::string> {
  const std::string d = dirOf(path);
  const std::string stem = norm(baseName(romBase(path)));
  const std::string iname = norm(internalName);
  std::vector<std::string> found, seen;
  for(const char* sub : {"", "manuals", "manuales"}) {
    const std::string dd = *sub ? join(d, sub) : d;
    if(!statOf(dd.empty() ? "." : dd).dir) continue;
    for(const std::string& f : listFiles(dd.empty() ? "." : dd)) {
      if(!manualExtOk(f)) continue;
      const std::string k = norm(f.substr(0, f.size() - extOf(f).size()));
      const std::string full = join(dd, f);
      const std::string key = lower(full);
      if(std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
      if((!stem.empty() && k.rfind(stem, 0) == 0) || (iname.size() >= 4 && k.rfind(iname, 0) == 0)) {
        seen.push_back(key);
        found.push_back(full);
      }
    }
  }
  // Biblioteca global (<estado>/manuals/): ahi los nombres son los de la fuente, con su region
  // y sus etiquetas, asi que se casa por titulo limpio ("Juego (Europe).pdf" vale para
  // "Juego (USA).z64"); con prefijo para "Juego - Manual.pdf".
  const std::string db = manualDbDir();
  if(statOf(db).dir) {
    const std::string tstem = stemKey(baseName(romBase(path)));
    for(const std::string& f : listFiles(db)) {
      if(!manualExtOk(f)) continue;
      const std::string k = titleKey(f);
      const bool hit = (tstem.size() >= 6 && k.rfind(tstem, 0) == 0) || (!tstem.empty() && k == tstem) ||
                       (iname.size() >= 4 && k == iname);
      // La carpeta del perfil puede ser la de las ROMs: no listar dos veces el mismo fichero.
      const std::string key = lower(join(db, f));
      if(hit && std::find(seen.begin(), seen.end(), key) == seen.end()) {
        seen.push_back(key);
        found.push_back(join(db, f));
      }
    }
  }
  return found;
}

auto manualStore(const std::string& path, const std::string& src, std::string& err) -> std::string {
  const std::string ext = extOf(src);
  if(!manualExtOk(src)) {
    err = "tipo de manual no admitido: " + (ext.empty() ? std::string("sin extension") : ext);
    return {};
  }
  std::string data;
  if(!readAll(src, data) || data.empty() || data.size() > 64u * 1048576u) {
    err = "manual vacio, ilegible o demasiado grande";
    return {};
  }
  const std::string dd = join(dirOf(path), "manuals");
  const std::string stem = baseName(romBase(path));
  std::string dst = join(dd, stem + ext);
  for(int n = 2; statOf(dst).file || statOf(dst).dir; n++)
    dst = join(dd, stem + " (" + std::to_string(n) + ")" + ext);
  if(!writeAtomic(dst, data)) {
    err = "no se pudo escribir " + dst;
    return {};
  }
  return dst;
}

// ---------------------------------------------------------------- fuente de manuales
namespace {

// Una linea de CSV: campos separados por coma, con comillas dobles opcionales ("" = una ").
auto csvFields(const std::string& ln) -> std::vector<std::string> {
  std::vector<std::string> out(1);
  bool q = false;
  for(usize i = 0; i < ln.size(); i++) {
    const char c = ln[i];
    if(q) {
      if(c == '"' && i + 1 < ln.size() && ln[i + 1] == '"') { out.back().push_back('"'); i++; }
      else if(c == '"') q = false;
      else out.back().push_back(c);
    } else if(c == '"') q = true;
    else if(c == ',') out.emplace_back();
    else out.back().push_back(c);
  }
  return out;
}

// Titulo comparable: sin carpeta, sin extension, sin grupos "(USA)" / "[!]", solo alfanumericos.
auto titleKey(const std::string& name) -> std::string {
  std::string s = baseName(name);
  s = s.substr(0, s.size() - extOf(s).size());
  std::string o;
  int depth = 0;
  for(char c : s) {
    if(c == '(' || c == '[') depth++;
    else if((c == ')' || c == ']') && depth > 0) depth--;
    else if(!depth) o.push_back(c);
  }
  return norm(o);
}

// Lo mismo para un nombre que ya viene SIN extension (la base de una ROM): "Dr. Mario 64 (USA)"
// no tiene extension ". Mario 64 (USA)". Y '&' cuenta como "and", como en No-Intro.
auto stemKey(const std::string& stem) -> std::string {
  std::string s;
  for(char c : stem) {
    if(c == '&') s += " and ";
    else s.push_back(c);
  }
  return titleKey(s + ".x");
}

}  // namespace

auto manualIndexParse(const std::string& csv) -> std::vector<ManualEntry> {
  std::vector<ManualEntry> out;
  usize p = 0;
  while(p < csv.size()) {
    usize e = csv.find('\n', p);
    if(e == std::string::npos) e = csv.size();
    std::string ln = csv.substr(p, e - p);
    p = e + 1;
    if(!ln.empty() && ln.back() == '\r') ln.pop_back();
    if(trim(ln).empty() || ln[0] == '#') continue;
    std::vector<std::string> f = csvFields(ln);
    if(f.size() < 2) continue;
    ManualEntry m;
    m.path = trim(f[0]);
    m.url = trim(f[1]);
    if(lower(m.path) == "path") continue;   // cabecera
    if(!manualExtOk(m.path) || m.url.empty()) continue;
    if(f.size() > 2) m.size = std::strtoull(trim(f[2]).c_str(), nullptr, 10);
    if(f.size() > 3) m.md5 = lower(trim(f[3]));
    out.push_back(m);
  }
  return out;
}

auto manualIndexMatch(const std::vector<ManualEntry>& idx, const std::string& path,
                      const std::string& internalName) -> std::vector<ManualEntry> {
  const std::string stem = stemKey(baseName(romBase(path)));
  const std::string iname = norm(internalName);
  std::vector<std::pair<int, ManualEntry>> hits;
  for(const ManualEntry& m : idx) {
    const std::string k = titleKey(m.path);
    if(k.empty()) continue;
    int score = 0;
    if(k == stem) score = 3;
    else if(iname.size() >= 4 && k == iname) score = 2;
    // "Juego - Manual de instrucciones": el titulo de la ROM entero y algo detras.
    else if(stem.size() >= 6 && k.rfind(stem, 0) == 0) score = 1;
    if(score) hits.push_back({score, m});
  }
  std::stable_sort(hits.begin(), hits.end(),
                   [](const auto& a, const auto& b) { return a.first > b.first; });
  std::vector<ManualEntry> out;
  for(const auto& h : hits)
    if(h.first == hits[0].first) out.push_back(h.second);   // solo los del mejor nivel
  return out;
}

auto manualSource() -> std::string {
  std::string s;
  readAll(join(stateDir(), "manuals_source.txt"), s);
  return trim(s);
}

auto setManualSource(const std::string& url) -> bool {
  return writeAtomic(join(stateDir(), "manuals_source.txt"), trim(url) + "\n");
}

auto manualDbDir() -> std::string { return join(stateDir(), "manuals"); }

auto dbFileName(const std::string& name) -> std::string {
  std::string o;
  for(char c : baseName(name)) {
    if((unsigned char)c < 32 || (unsigned char)c >= 127 || std::strchr("\\/:*?\"<>|", c)) o.push_back('_');
    else o.push_back(c);
  }
  return o;
}

// ================================================================ trucos

auto chtPath(const std::string& path) -> std::string { return romBase(path) + ".cht"; }

auto parseCht(const std::vector<std::string>& lines) -> std::vector<Cheat> {
  std::vector<Cheat> out;
  for(int idx = 0; idx < (int)lines.size(); idx++) {
    const std::string b = body(lines[idx]);
    if(b.empty()) continue;
    if(b[0] == '[') {
      usize e = b.find(']');
      std::string name = trim(b.substr(1, e == std::string::npos ? std::string::npos : e - 1));
      Cheat c;
      if(!name.empty() && name[0] == '-') {
        c.on = false;
        name = trim(name.substr(1));
      }
      c.name = name.empty() ? "sin nombre" : name;
      c.line = c.first = idx;
      out.push_back(c);
      continue;
    }
    if(out.empty()) {
      Cheat c;
      c.name = "sin nombre";
      c.first = idx;
      out.push_back(c);
    }
    std::string cd;
    if(code(b, cd)) out.back().codes.push_back(cd);
    else out.back().bad++;
  }
  for(Cheat& c : out)
    for(const std::string& x : c.codes) {
      const std::string p = x.substr(0, 2);
      if(p == "88" || p == "89" || p == "CC" || p == "DE" || p == "EE" || p == "FF") c.inert++;
    }
  return out;
}

auto cheats(const std::string& path) -> std::vector<Cheat> {
  std::vector<std::string> lines;
  std::string nl;
  const std::string f = cheatFile(path);
  if(f.empty() || !readLines(f, lines, nl)) return {};
  return parseCht(lines);
}

namespace {
// Antes de tocar los trucos de una ROM sin .cht propio: copiar el de la biblioteca, que es lo
// que la ficha estaba ensenando (los indices de truco siguen valiendo).
auto ownCht(const std::string& path, std::string& err) -> bool {
  const std::string mine = chtPath(path);
  if(statOf(mine).file) return true;
  const std::string db = cheatDbFile(path);
  if(db.empty()) return true;
  std::string data;
  if(!readAll(db, data) || !writeAtomic(mine, data)) {
    err = "no se pudo copiar " + baseName(db) + " a " + baseName(mine);
    return false;
  }
  return true;
}
}  // namespace

auto setCheat(const std::string& path, int id, bool on, std::string& err) -> bool {
  if(!ownCht(path, err)) return false;
  const std::string f = chtPath(path);
  std::vector<std::string> lines;
  std::string nl;
  if(!readLines(f, lines, nl)) { err = "no se puede leer " + f; return false; }
  std::vector<Cheat> lst = parseCht(lines);
  if(id < 0 || id >= (int)lst.size()) { err = "no existe ese truco"; return false; }
  const Cheat& c = lst[id];
  if(c.on == on) return true;
  const std::string head = std::string("[") + (on ? "" : "-") + c.name + "]";
  if(c.line < 0) {
    lines.insert(lines.begin() + c.first, head);   // el implicito gana cabecera propia
  } else {
    std::string& ln = lines[c.line];
    usize s = ln.find_first_not_of(" \t");
    if(s == std::string::npos) s = 0;
    usize e = ln.find(']', s);
    ln = ln.substr(0, s) + head + (e == std::string::npos ? std::string() : ln.substr(e + 1));
  }
  if(!writeLines(f, lines, nl)) { err = "no se puede escribir " + f; return false; }
  return true;
}

auto addCheat(const std::string& path, const std::string& name0, const std::string& codes, bool on,
              std::string& err) -> bool {
  std::string name;
  for(char c : name0)
    if(!std::strchr("[]\r\n#;", c)) name.push_back(c);
  name = trim(name);
  while(!name.empty() && name[0] == '-') name.erase(0, 1);
  name = trim(name);
  if(name.empty()) { err = "el truco necesita nombre"; return false; }
  std::vector<std::string> good;
  std::string cur;
  auto take = [&](const std::string& ln) -> bool {
    const std::string b = body(ln);
    if(b.empty()) return true;
    std::string cd;
    if(!code(b, cd)) { err = "linea ilegible: " + b; return false; }
    good.push_back(cd);
    return true;
  };
  for(char c : codes) {
    if(c == '\r' || c == '\n') {
      if(!take(cur)) return false;
      cur.clear();
    } else {
      cur.push_back(c);
    }
  }
  if(!take(cur)) return false;
  if(good.empty()) { err = "el truco no tiene codigos"; return false; }

  if(!ownCht(path, err)) return false;
  const std::string f = chtPath(path);
  std::vector<std::string> lines;
  std::string nl;
  if(statOf(f).file) {
    if(!readLines(f, lines, nl)) { err = "no se puede leer " + f; return false; }
    while(!lines.empty() && trim(lines.back()).empty()) lines.pop_back();
    lines.push_back("");
  } else {
    lines = {"# " + baseName(romBase(path))};
    nl = "\n";
  }
  lines.push_back(std::string("[") + (on ? "" : "-") + name + "]");
  lines.insert(lines.end(), good.begin(), good.end());
  if(!writeLines(f, lines, nl)) { err = "no se puede escribir " + f; return false; }
  return true;
}

auto delCheat(const std::string& path, int id, std::string& err) -> bool {
  if(!ownCht(path, err)) return false;
  const std::string f = chtPath(path);
  std::vector<std::string> lines;
  std::string nl;
  if(!readLines(f, lines, nl)) { err = "no se puede leer " + f; return false; }
  std::vector<Cheat> lst = parseCht(lines);
  if(id < 0 || id >= (int)lst.size()) { err = "no existe ese truco"; return false; }
  const int a = lst[id].first;
  int b = id + 1 < (int)lst.size() ? lst[id + 1].first : (int)lines.size();
  // Los comentarios y blancos justo antes del siguiente truco son suyos, no de este.
  while(id + 1 < (int)lst.size() && b > a + 1 && body(lines[b - 1]).empty()) b--;
  lines.erase(lines.begin() + a, lines.begin() + b);
  if(!writeLines(f, lines, nl)) { err = "no se puede escribir " + f; return false; }
  return true;
}

// ================================================================ biblioteca de trucos

const char* const kCheatSourceDefault =
    "https://github.com/libretro/libretro-database/tree/master/cht/Nintendo - Nintendo 64";

namespace {
// cheats_source.txt: linea 1 = fuente ("" = la de fabrica), linea 2 = "on" si el usuario
// encendio la descarga. Sin fichero: fuente de fabrica y descarga apagada.
void cheatSourceRead(std::string& src, bool& on) {
  std::string s;
  readAll(join(stateDir(), "cheats_source.txt"), s);
  const size_t nl = s.find('\n');
  src = trim(s.substr(0, nl));
  on = nl != std::string::npos && trim(s.substr(nl + 1)) == "on";
}
}  // namespace

auto cheatSource() -> std::string {
  std::string s;
  bool on = false;
  cheatSourceRead(s, on);
  return s.empty() ? std::string(kCheatSourceDefault) : s;
}

auto cheatSourceOn() -> bool {
  std::string s;
  bool on = false;
  cheatSourceRead(s, on);
  return on;
}

auto setCheatSource(const std::string& src, bool on) -> bool {
  const std::string s = trim(src) == kCheatSourceDefault ? std::string() : trim(src);
  return writeAtomic(join(stateDir(), "cheats_source.txt"), s + "\n" + (on ? "on\n" : ""));
}

auto cheatDbDir() -> std::string { return join(stateDir(), "cheats"); }

namespace {

auto urlDecode(const std::string& s) -> std::string {
  std::string o;
  for(usize i = 0; i < s.size(); i++) {
    if(s[i] == '%' && i + 2 < s.size() && std::isxdigit((unsigned char)s[i + 1]) &&
       std::isxdigit((unsigned char)s[i + 2])) {
      o.push_back((char)std::strtoul(s.substr(i + 1, 2).c_str(), nullptr, 16));
      i += 2;
    } else {
      o.push_back(s[i]);
    }
  }
  return o;
}

auto split(const std::string& s, char sep) -> std::vector<std::string> {
  std::vector<std::string> out;
  std::string cur;
  for(char c : s) {
    if(c == sep) { if(!cur.empty()) out.push_back(cur); cur.clear(); }
    else cur.push_back(c);
  }
  if(!cur.empty()) out.push_back(cur);
  return out;
}

}  // namespace

auto urlEncodePath(const std::string& s) -> std::string {
  std::string o;
  for(char c : s) {
    const unsigned char u = (unsigned char)c;
    if(std::isalnum(u) && u < 128) o.push_back(c);
    else if(std::strchr("-._~/", c) && c) o.push_back(c);
    else {
      char b[4];
      std::snprintf(b, sizeof b, "%%%02X", u);
      o += b;
    }
  }
  return o;
}

auto cheatSrcResolve(const std::string& src0) -> CheatSrc {
  CheatSrc r;
  const std::string src = trim(src0);
  if(src.empty()) return r;
  const usize sch = src.find("://");
  if(sch == std::string::npos) {
    r.local = true;
    r.base = src;
    if(r.base.back() != '/' && r.base.back() != '\\') r.base.push_back('/');
    r.ok = statOf(src).dir;
    return r;
  }
  std::string rest = src.substr(sch + 3);
  const usize q = rest.find_first_of("?#");
  if(q != std::string::npos) rest.resize(q);
  std::vector<std::string> p = split(urlDecode(rest), '/');
  std::string owner, repo, branch, dir;
  usize from = 0;
  if(p.size() >= 4 && lower(p[0]) == "raw.githubusercontent.com") {
    owner = p[1], repo = p[2], branch = p[3], from = 4;
  } else if(p.size() >= 5 && (lower(p[0]) == "github.com" || lower(p[0]) == "www.github.com") &&
            p[3] == "tree") {
    owner = p[1], repo = p[2], branch = p[4], from = 5;
  } else {
    return r;
  }
  for(usize i = from; i < p.size(); i++) dir += (dir.empty() ? "" : "/") + p[i];
  const std::string encDir = urlEncodePath(dir);
  r.base = "https://raw.githubusercontent.com/" + owner + "/" + repo + "/" + urlEncodePath(branch) +
           "/" + encDir + (encDir.empty() ? "" : "/");
  r.index = "https://api.github.com/repos/" + owner + "/" + repo + "/git/trees/" +
            urlEncodePath(branch) + (dir.empty() ? "" : ":" + encDir);
  r.ok = true;
  return r;
}

auto cheatTreeParse(const std::string& js) -> std::vector<std::string> {
  std::vector<std::string> out;
  usize p = 0;
  while((p = js.find("\"path\"", p)) != std::string::npos) {
    p += 6;
    usize c = js.find_first_not_of(" \t\r\n", p);
    if(c == std::string::npos || js[c] != ':') continue;
    usize a = js.find_first_not_of(" \t\r\n", c + 1);
    if(a == std::string::npos || js[a] != '"') continue;
    usize b = a + 1;
    while(b < js.size() && js[b] != '"') b += js[b] == '\\' ? 2 : 1;
    if(b >= js.size()) break;
    const std::string name = jsonUnstr(js.substr(a, b - a + 1));
    p = b + 1;
    if(name.find('/') == std::string::npos && lower(extOf(name)) == ".cht") out.push_back(name);
  }
  return out;
}

auto cheatNameMatch(const std::vector<std::string>& names, const std::string& path,
                    const std::string& internalName, char region) -> std::string {
  if(names.empty()) return {};
  std::vector<std::pair<std::string, std::string>> keyed;   // titulo limpio -> nombre
  keyed.reserve(names.size());
  for(const std::string& n : names) keyed.push_back({stemKey(n.substr(0, n.size() - extOf(n).size())), n});
  static const struct { char r; const char* tag; } kTags[] = {
      {'E', "USA"}, {'J', "Japan"}, {'P', "Europe"}, {'D', "Germany"}, {'F', "France"},
      {'I', "Italy"}, {'S', "Spain"}, {'U', "Australia"}, {'X', "Europe"}, {'Y', "Europe"}};
  std::string tag;
  for(const auto& t : kTags)
    if(t.r == region) tag = t.tag;
  auto pick = [&](const std::vector<std::string>& cand) -> std::string {
    for(const std::string& want : {tag, std::string("USA")}) {
      if(want.empty()) continue;
      std::string best;
      for(const std::string& c : cand)
        if(c.find("(" + want) != std::string::npos && (best.empty() || c.size() < best.size())) best = c;
      if(!best.empty()) return best;
    }
    std::string best;
    for(const std::string& c : cand)
      if(best.empty() || c.size() < best.size()) best = c;
    return best;
  };
  auto with = [&](const std::string& k) {
    std::vector<std::string> c;
    for(const auto& e : keyed)
      if(e.first == k) c.push_back(e.second);
    return c;
  };
  const std::string keys[2] = {stemKey(baseName(romBase(path))), norm(internalName)};
  for(const std::string& k : keys) {
    if(k.empty()) continue;
    std::vector<std::string> c = with(k);
    if(!c.empty()) return pick(c);
  }
  // Prefijo inequivoco: el nombre interno viene recortado ("GOLDENEYE" por "GoldenEye 007") y
  // el del fichero a veces trae cola ("mario_kart_64_rom").
  for(const std::string& k : keys) {
    if(k.size() < 6) continue;
    for(int dir = 0; dir < 2; dir++) {
      std::vector<std::string> titles;
      for(const auto& e : keyed) {
        const bool m = dir == 0 ? e.first.rfind(k, 0) == 0
                                : (e.first.size() >= 6 && k.rfind(e.first, 0) == 0);
        if(m && std::find(titles.begin(), titles.end(), e.first) == titles.end()) titles.push_back(e.first);
      }
      if(titles.size() == 1) return pick(with(titles[0]));
    }
  }
  return {};
}

namespace {
// Valor de "clave = valor" de RetroArch: comillas opcionales, con \" y \\ dentro.
auto raValue(const std::string& v0) -> std::string {
  const std::string v = trim(v0);
  if(v.size() < 2 || v[0] != '"') return v;
  std::string o;
  for(usize i = 1; i < v.size(); i++) {
    if(v[i] == '\\' && i + 1 < v.size()) { o.push_back(v[++i]); continue; }
    if(v[i] == '"') break;
    o.push_back(v[i]);
  }
  return o;
}
}  // namespace

auto chtFromRetroArch(const std::string& text, const std::string& origin) -> std::string {
  struct Ra { std::string desc, code; };
  std::vector<std::pair<int, Ra>> ra;
  auto slot = [&](int i) -> Ra& {
    for(auto& e : ra)
      if(e.first == i) return e.second;
    ra.push_back({i, Ra{}});
    return ra.back().second;
  };
  std::vector<std::string> lines;
  usize p = 0;
  while(p <= text.size()) {
    usize e = text.find('\n', p);
    if(e == std::string::npos) e = text.size();
    lines.push_back(text.substr(p, e - p));
    p = e + 1;
  }
  for(const std::string& ln : lines) {
    const usize eq = ln.find('=');
    if(eq == std::string::npos) continue;
    const std::string k = lower(trim(ln.substr(0, eq)));
    if(k.rfind("cheat", 0) != 0) continue;
    usize us = k.find('_');
    if(us == std::string::npos || us == 5) continue;
    const std::string num = k.substr(5, us - 5), field = k.substr(us + 1);
    if(num.find_first_not_of("0123456789") != std::string::npos) continue;
    const int i = std::atoi(num.c_str());
    if(field == "desc") slot(i).desc = raValue(ln.substr(eq + 1));
    else if(field == "code") slot(i).code = raValue(ln.substr(eq + 1));
  }
  std::string out = "# " + origin + "\n# Todo apagado: se enciende desde la ficha del juego.\n";
  int n = 0;
  if(ra.empty()) {
    // No es de RetroArch: quiza ya es del emulador (una coleccion propia). Se copia apagado.
    std::vector<std::string> tmp;
    for(std::string ln : lines) {
      if(!ln.empty() && ln.back() == '\r') ln.pop_back();
      tmp.push_back(ln);
    }
    for(const Cheat& c : parseCht(tmp)) {
      if(c.codes.empty() || c.bad) continue;
      out += "\n[-" + c.name + "]\n";
      for(const std::string& cd : c.codes) out += cd + "\n";
      n++;
    }
    return n ? out : std::string();
  }
  std::sort(ra.begin(), ra.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  for(const auto& e : ra) {
    std::vector<std::string> good;
    bool bad = false;
    for(std::string piece : split(e.second.code, '+')) {
      piece = trim(piece);
      if(piece.empty()) continue;
      // "8033B21E0008" sin separador: 8 + 4 cifras.
      if(piece.size() == 12 && isHex(piece.substr(0, 8)) && isHex(piece.substr(8)))
        piece = piece.substr(0, 8) + " " + piece.substr(8);
      std::string cd;
      if(!code(piece, cd)) { bad = true; break; }
      good.push_back(cd);
    }
    if(bad || good.empty()) continue;
    std::string name;
    for(char c : e.second.desc)
      if(!std::strchr("[]\r\n#;", c)) name.push_back(c);
    name = trim(name);
    while(!name.empty() && name[0] == '-') name = trim(name.substr(1));
    if(name.empty()) name = "truco " + std::to_string(e.first);
    out += "\n[-" + name + "]\n";
    for(const std::string& cd : good) out += cd + "\n";
    n++;
  }
  return n ? out : std::string();
}

auto cheatIndexCached(const std::string& src, std::vector<std::string>& names) -> bool {
  const std::string f = join(cheatDbDir(), "index.txt");
  const Stat st = statOf(f);
  if(!st.file || std::time(nullptr) - st.mtime > 7 * 86400) return false;
  std::vector<std::string> lines;
  std::string nl;
  if(!readLines(f, lines, nl) || lines.empty() || lines[0] != "# " + trim(src)) return false;
  names.assign(lines.begin() + 1, lines.end());
  while(!names.empty() && names.back().empty()) names.pop_back();
  return !names.empty();
}

auto cheatIndexStore(const std::string& src, const std::vector<std::string>& names) -> void {
  std::string s = "# " + trim(src) + "\n";
  for(const std::string& n : names) s += n + "\n";
  writeAtomic(join(cheatDbDir(), "index.txt"), s);
}

auto cheatDbStore(const std::string& dir, const std::string& name, const std::string& raw,
                  std::string& err) -> bool {
  const std::string conv = chtFromRetroArch(raw, "de " + name + " (fuente de trucos de la ficha)");
  if(conv.empty()) { err = name + ": sin trucos legibles"; return false; }
  if(!writeAtomic(join(dir, dbFileName(name)), conv)) { err = "no se pudo escribir en " + dir; return false; }
  return true;
}

auto cheatDbFile(const std::string& path) -> std::string {
  const std::string dir = cheatDbDir();
  if(!statOf(dir).dir) return {};
  std::vector<std::string> names;
  for(const std::string& f : listFiles(dir))
    if(lower(extOf(f)) == ".cht") names.push_back(f);
  if(names.empty()) return {};
  // La cabecera de la ROM (desenvuelta si viene en zip) se lee una vez por ROM, no por clic.
  static std::string lastPath;
  static Header lastHdr;
  if(lastPath != path) { lastHdr = readHeader(path); lastPath = path; }
  const std::string m = cheatNameMatch(names, path, lastHdr.name, lastHdr.region);
  return m.empty() ? std::string() : join(dir, m);
}

auto cheatFile(const std::string& path) -> std::string {
  const std::string mine = chtPath(path);
  return statOf(mine).file ? mine : cheatDbFile(path);
}

// ================================================================ metadatos

auto Meta::str(const char* k) const -> std::string {
  for(const auto& p : kv)
    if(p.first == k) return p.second == "null" ? std::string() : jsonUnstr(p.second);
  return {};
}

auto Meta::num(const char* k) const -> long long {
  for(const auto& p : kv)
    if(p.first == k) return std::strtoll(jsonUnstr(p.second).c_str(), nullptr, 10);
  return 0;
}

auto Meta::flag(const char* k) const -> bool {
  for(const auto& p : kv)
    if(p.first == k) return p.second == "true" || p.second == "1";
  return false;
}

auto Meta::setStr(const char* k, const std::string& v) -> void {
  std::string raw = jsonStr(v.size() > 4000 ? v.substr(0, 4000) : v);
  for(auto& p : kv)
    if(p.first == k) { p.second = raw; return; }
  kv.emplace_back(k, raw);
}

auto Meta::setNum(const char* k, long long v) -> void {
  for(auto& p : kv)
    if(p.first == k) { p.second = std::to_string(v); return; }
  kv.emplace_back(k, std::to_string(v));
}

auto Meta::setFlag(const char* k, bool v) -> void {
  for(auto& p : kv)
    if(p.first == k) { p.second = v ? "true" : "false"; return; }
  kv.emplace_back(k, v ? "true" : "false");
}

auto parseFlatJson(const std::string& t, Meta& out) -> bool {
  out.kv.clear();
  usize i = 0;
  auto ws = [&] { while(i < t.size() && std::isspace((unsigned char)t[i])) i++; };
  // Recorre un valor JSON cualquiera (anidados incluidos) y devuelve su texto crudo.
  auto value = [&]() -> std::string {
    ws();
    usize a = i;
    if(i < t.size() && t[i] == '"') {
      for(i++; i < t.size() && t[i] != '"'; i++)
        if(t[i] == '\\') i++;
      i++;
    } else if(i < t.size() && (t[i] == '{' || t[i] == '[')) {
      int depth = 0;
      bool inStr = false;
      for(; i < t.size(); i++) {
        char c = t[i];
        if(inStr) {
          if(c == '\\') i++;
          else if(c == '"') inStr = false;
        } else if(c == '"') inStr = true;
        else if(c == '{' || c == '[') depth++;
        else if(c == '}' || c == ']') {
          if(--depth == 0) { i++; break; }
        }
      }
    } else {
      while(i < t.size() && t[i] != ',' && t[i] != '}' && !std::isspace((unsigned char)t[i])) i++;
    }
    return t.substr(a, std::min(i, t.size()) - a);
  };
  // El lanzador escribe en UTF-8; un BOM delante no se espera pero no debe romper.
  if(t.compare(0, 3, "\xEF\xBB\xBF") == 0) i = 3;
  ws();
  if(i >= t.size() || t[i] != '{') return false;
  i++;
  while(true) {
    ws();
    if(i < t.size() && t[i] == '}') return true;
    if(i >= t.size() || t[i] != '"') return false;
    std::string key = jsonUnstr(value());
    ws();
    if(i >= t.size() || t[i] != ':') return false;
    i++;
    std::string v = value();
    if(v.empty()) return false;
    out.kv.emplace_back(key, v);
    ws();
    if(i < t.size() && t[i] == ',') { i++; continue; }
    if(i < t.size() && t[i] == '}') return true;
    return false;
  }
}

auto writeFlatJson(const Meta& m) -> std::string {
  std::string o = "{";
  for(usize k = 0; k < m.kv.size(); k++) {
    o += k ? ",\n " : "\n ";
    o += jsonStr(m.kv[k].first) + ": " + m.kv[k].second;
  }
  return o + (m.kv.empty() ? "}" : "\n}");
}

auto stateDir() -> std::string { return dirOf(profilePath()); }

auto metaKey(const std::string& path, const Header& h) -> std::string {
  if(h.ok && !h.crc.empty()) return h.crc;
  std::string k = norm(baseName(romBase(path)));
  return k.empty() ? "rom" : k;
}

namespace {
auto metaFile(const std::string& key) -> std::string {
  std::string k;
  for(char c : key) k.push_back(std::isalnum((unsigned char)c) || c == '_' || c == '-' ? c : '_');
  return join(join(stateDir(), "games"), k + ".json");
}
}  // namespace

auto metaLoad(const std::string& key) -> Meta {
  Meta m;
  std::string txt;
  if(readAll(metaFile(key), txt) && !parseFlatJson(txt, m)) m = Meta();
  return m;
}

auto metaSave(const std::string& key, const Meta& m) -> bool {
  makeDir(stateDir());
  return writeAtomic(metaFile(key), writeFlatJson(m));
}

auto metaPlayed(const std::string& key, long long seconds) -> void {
  if(seconds < 3) return;
  Meta m = metaLoad(key);
  m.setNum("plays", m.num("plays") + 1);
  m.setNum("seconds", m.num("seconds") + seconds);
  m.setNum("last", (long long)std::time(nullptr));
  metaSave(key, m);
}

}  // namespace kestrel::ui::card
