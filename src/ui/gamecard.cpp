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
  const std::string stem = titleKey(baseName(romBase(path)));
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
  if(!readLines(chtPath(path), lines, nl)) return {};
  return parseCht(lines);
}

auto setCheat(const std::string& path, int id, bool on, std::string& err) -> bool {
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
