// Texturas HD: indice del pack, carga/volcado PNG. Ver texpack.hpp y docs/TEXTURAS-HD.md.
#include "texpack.hpp"

#include "../core/archive.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace kestrel::texpack {

bool g_dump = false;
bool g_load = false;
u32 g_epoch = 0;
int g_fx = FxNone;

namespace {

std::mutex g_mu;
std::string g_ident;
fs::path g_dumpDir;

// Clave completa: crc64 + formato + tamano de texel. Los packs llevan los dos en el nombre;
// si no casa con ellos se prueba solo el crc64 (hay packs con el formato mal puesto).
auto keyOf(u64 crc, u32 fmt, u32 siz) -> std::string {
  char b[40];
  std::snprintf(b, sizeof b, "%016llx%x%x", (unsigned long long)crc, fmt & 7, siz & 3);
  return b;
}

struct Entry {
  std::string path;
  bool tried = false;
  std::unique_ptr<Tex> tex;
};
std::unordered_map<std::string, Entry> g_full;          // keyOf -> fichero
std::unordered_map<u64, std::string> g_byCrc;          // crc64 -> keyOf (primer fichero)
std::unordered_set<std::string> g_dumpedKeys;
std::unordered_map<std::string, std::unique_ptr<Tex>> g_fxCache;   // keyOf -> realzada

auto lower(std::string s) -> std::string {
  for(char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

// "<ident>#CRC#F#S[#PAL]_<sufijo>.png" -> clave. Comodines ('$') no se admiten.
auto parseName(const std::string& name, std::string& ident, u64& crc, u32& fmt, u32& siz) -> bool {
  const std::string ln = lower(name);
  static const char* kEnds[] = {"_all.png", "_allcibyrgba.png", "_cibyrgba.png", "_rgb.png"};
  bool okEnd = false;
  for(const char* e : kEnds) {
    const usize n = std::strlen(e);
    if(ln.size() > n && ln.compare(ln.size() - n, n, e) == 0) { okEnd = true; break; }
  }
  if(!okEnd || name.find('$') != std::string::npos) return false;
  const usize h = name.find('#');
  if(h == std::string::npos) return false;
  ident = name.substr(0, h);
  unsigned c = 0, f = 0, s = 0, p = 0;
  const char* q = name.c_str() + h;
  if(std::sscanf(q, "#%8X#%1X#%1X#%8X", &c, &f, &s, &p) == 4) {
    crc = ((u64)p << 32) | c;
  } else if(std::sscanf(q, "#%8X#%1X#%1X", &c, &f, &s) == 3) {
    crc = c;
  } else {
    return false;
  }
  fmt = f; siz = s;
  return true;
}

auto scan(const fs::path& dir, const std::string& ident, bool anyIdent) -> int {
  int n = 0;
  std::error_code ec;
  const std::string li = lower(ident);
  for(auto it = fs::recursive_directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
      !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if(!it->is_regular_file(ec)) continue;
    const std::string name = it->path().filename().string();
    std::string id; u64 crc; u32 fmt, siz;
    if(!parseName(name, id, crc, fmt, siz)) continue;
    if(!anyIdent && lower(id) != li) continue;
    const std::string k = keyOf(crc, fmt, siz);
    if(g_full.count(k)) continue;   // el primero manda (orden del directorio)
    g_full[k].path = it->path().string();
    g_byCrc.emplace(crc, k);
    n++;
  }
  return n;
}

// --- PNG ------------------------------------------------------------------------------

auto be32(const u8* p) -> u32 { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

auto put32(std::vector<u8>& v, u32 x) -> void {
  v.push_back((u8)(x >> 24)); v.push_back((u8)(x >> 16)); v.push_back((u8)(x >> 8)); v.push_back((u8)x);
}

auto chunk(std::vector<u8>& out, const char* type, const std::vector<u8>& data) -> void {
  put32(out, (u32)data.size());
  std::vector<u8> td(type, type + 4);
  td.insert(td.end(), data.begin(), data.end());
  out.insert(out.end(), td.begin(), td.end());
  put32(out, archive::crc32(td.data(), td.size()));
}

auto paeth(int a, int b, int c) -> int {
  const int p = a + b - c, pa = std::abs(p - a), pb = std::abs(p - b), pc = std::abs(p - c);
  return (pa <= pb && pa <= pc) ? a : pb <= pc ? b : c;
}

// --- realce por algoritmo -------------------------------------------------------------

// Scale2x (Andrea Mazzoleni, algoritmo publico; implementacion propia): cada texel se parte
// en 2x2 y cada cuarto copia al vecino de su lado cuando los dos vecinos que lo flanquean
// coinciden y los opuestos no. Rellena las diagonales de escalera sin inventar colores
// nuevos, que es lo que pide una textura de pocos colores.
auto scale2x(int w, int h, const std::vector<u32>& in, std::vector<u32>& out) -> void {
  out.assign((usize)w * h * 4, 0);
  auto at = [&](int x, int y) {
    x = std::clamp(x, 0, w - 1); y = std::clamp(y, 0, h - 1);
    return in[(usize)y * w + x];
  };
  for(int y = 0; y < h; y++) {
    for(int x = 0; x < w; x++) {
      const u32 b = at(x, y - 1), d = at(x - 1, y), e = at(x, y), f = at(x + 1, y), hh = at(x, y + 1);
      u32 e0 = e, e1 = e, e2 = e, e3 = e;
      if(b != hh && d != f) {
        if(d == b) e0 = d;
        if(b == f) e1 = f;
        if(d == hh) e2 = d;
        if(hh == f) e3 = f;
      }
      const usize o = (usize)(2 * y) * (2 * w) + 2 * x;
      out[o] = e0; out[o + 1] = e1; out[o + 2 * w] = e2; out[o + 2 * w + 1] = e3;
    }
  }
}

auto luma(u32 c) -> int { return (int)((((c >> 24) & 255) * 77 + ((c >> 16) & 255) * 150 + ((c >> 8) & 255) * 29) >> 8); }

// Bandas de tono: cuantiza la LUMINANCIA en n niveles y reescala el RGB para que la tenga,
// asi el color conserva su tinte y solo pierde degradado. Alfa intacto.
auto posterize(std::vector<u32>& px, int n) -> void {
  for(u32& c : px) {
    const int l = luma(c);
    if(l == 0) continue;
    const int q = std::min(255, ((l * n / 256) * 2 + 1) * 128 / n);   // centro de la banda
    auto ch = [&](int s) { return (u32)std::min(255, (int)((c >> s) & 255) * q / l); };
    c = (ch(24) << 24) | (ch(16) << 16) | (ch(8) << 8) | (c & 255);
  }
}

// Contorno: oscurece donde el gradiente Sobel de luminancia (o del alfa: borde de recorte)
// pasa del umbral. Sobre la textura ya escalada, para que el trazo salga fino.
auto outline(int w, int h, std::vector<u32>& px) -> void {
  std::vector<u32> src = px;
  auto L = [&](int x, int y) {
    x = std::clamp(x, 0, w - 1); y = std::clamp(y, 0, h - 1);
    const u32 c = src[(usize)y * w + x];
    return luma(c) * (int)(c & 255) / 255;
  };
  for(int y = 0; y < h; y++)
    for(int x = 0; x < w; x++) {
      const int gx = L(x + 1, y - 1) + 2 * L(x + 1, y) + L(x + 1, y + 1) - L(x - 1, y - 1) - 2 * L(x - 1, y) - L(x - 1, y + 1);
      const int gy = L(x - 1, y + 1) + 2 * L(x, y + 1) + L(x + 1, y + 1) - L(x - 1, y - 1) - 2 * L(x, y - 1) - L(x + 1, y - 1);
      if(gx * gx + gy * gy > 160 * 160) {
        u32& c = px[(usize)y * w + x];
        auto dk = [&](int s) { return (((c >> s) & 255) * 70 / 255) << s; };
        c = dk(24) | dk(16) | dk(8) | (c & 255);
      }
    }
}

}  // namespace

auto writePng(const std::string& path, int w, int h, const u32* rgba) -> bool {
  // Filas con filtro 0 dentro de un zlib de bloques "stored": sin compresor, el PNG pesa
  // lo que los pixeles. Es un volcado para editar a mano o pasar por un escalador, no para
  // distribuir; cualquier editor lo vuelve a guardar comprimido.
  std::vector<u8> raw;
  raw.reserve((usize)h * (w * 4 + 1));
  for(int y = 0; y < h; y++) {
    raw.push_back(0);
    for(int x = 0; x < w; x++) {
      const u32 c = rgba[(usize)y * w + x];
      raw.push_back((u8)(c >> 24)); raw.push_back((u8)(c >> 16));
      raw.push_back((u8)(c >> 8));  raw.push_back((u8)c);
    }
  }
  std::vector<u8> z = {0x78, 0x01};
  for(usize off = 0; off < raw.size() || off == 0;) {
    const usize n = std::min<usize>(65535, raw.size() - off);
    const bool last = off + n >= raw.size();
    z.push_back(last ? 1 : 0);
    z.push_back((u8)n); z.push_back((u8)(n >> 8));
    z.push_back((u8)~n); z.push_back((u8)(~n >> 8));
    z.insert(z.end(), raw.begin() + (long)off, raw.begin() + (long)(off + n));
    off += n;
    if(last) break;
  }
  u32 a = 1, b = 0;
  for(u8 c : raw) { a = (a + c) % 65521; b = (b + a) % 65521; }
  put32(z, (b << 16) | a);
  std::vector<u8> out = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  std::vector<u8> ihdr;
  put32(ihdr, (u32)w); put32(ihdr, (u32)h);
  ihdr.insert(ihdr.end(), {8, 6, 0, 0, 0});   // 8 bits, RGBA, deflate, filtro 0, sin entrelazar
  chunk(out, "IHDR", ihdr);
  chunk(out, "IDAT", z);
  chunk(out, "IEND", {});
  std::FILE* f = std::fopen(path.c_str(), "wb");
  if(!f) return false;
  const bool ok = std::fwrite(out.data(), 1, out.size(), f) == out.size();
  std::fclose(f);
  return ok;
}

auto readPng(const std::string& path, Tex& out, std::string& error) -> bool {
  std::FILE* f = std::fopen(path.c_str(), "rb");
  if(!f) { error = "no se puede abrir"; return false; }
  std::vector<u8> d;
  u8 buf[65536];
  for(usize n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) d.insert(d.end(), buf, buf + n);
  std::fclose(f);
  static const u8 sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  if(d.size() < 8 || std::memcmp(d.data(), sig, 8) != 0) { error = "no es PNG"; return false; }
  u32 w = 0, h = 0;
  int depth = 0, ctype = 0, interlace = 0;
  std::vector<u8> idat, plte, trns;
  for(usize p = 8; p + 12 <= d.size();) {
    const u32 len = be32(&d[p]);
    if(p + 12 + (usize)len > d.size()) { error = "chunk truncado"; return false; }
    const u8* t = &d[p + 4];
    const u8* c = &d[p + 8];
    if(!std::memcmp(t, "IHDR", 4) && len >= 13) {
      w = be32(c); h = be32(c + 4); depth = c[8]; ctype = c[9]; interlace = c[12];
    } else if(!std::memcmp(t, "PLTE", 4)) {
      plte.assign(c, c + len);
    } else if(!std::memcmp(t, "tRNS", 4)) {
      trns.assign(c, c + len);
    } else if(!std::memcmp(t, "IDAT", 4)) {
      idat.insert(idat.end(), c, c + len);
    } else if(!std::memcmp(t, "IEND", 4)) {
      break;
    }
    p += 12 + len;
  }
  if(!w || !h || w > 16384 || h > 16384) { error = "tamano invalido"; return false; }
  if(interlace) { error = "PNG entrelazado (Adam7) no admitido"; return false; }
  static const int kCh[7] = {1, 0, 3, 1, 2, 0, 4};
  if(ctype > 6 || !kCh[ctype]) { error = "tipo de color invalido"; return false; }
  const int ch = kCh[ctype];
  if(depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16) { error = "profundidad invalida"; return false; }
  if(idat.size() < 6) { error = "sin datos"; return false; }
  std::vector<u8> raw;
  std::string ie;
  if(!archive::inflate(idat.data() + 2, idat.size() - 2, raw, ie)) { error = "zlib: " + ie; return false; }
  const usize bpp = std::max<usize>(1, (usize)ch * depth / 8);   // bytes por pixel para el filtro
  const usize stride = ((usize)w * ch * depth + 7) / 8;
  if(raw.size() < (stride + 1) * h) { error = "datos cortos"; return false; }
  std::vector<u8> img(stride * h), prev(stride, 0);
  for(u32 y = 0; y < h; y++) {
    const u8 ft = raw[y * (stride + 1)];
    const u8* src = &raw[y * (stride + 1) + 1];
    u8* row = &img[y * stride];
    for(usize i = 0; i < stride; i++) {
      const int a = i >= bpp ? row[i - bpp] : 0, b = prev[i], c = i >= bpp ? prev[i - bpp] : 0;
      int v = src[i];
      switch(ft) {
        case 0: break;
        case 1: v += a; break;
        case 2: v += b; break;
        case 3: v += (a + b) >> 1; break;
        case 4: v += paeth(a, b, c); break;
        default: error = "filtro invalido"; return false;
      }
      row[i] = (u8)v;
    }
    std::memcpy(prev.data(), row, stride);
  }
  // Muestra n de una fila, en 8 bits (16 bits: byte alto; <8: escalado a 0..255 salvo
  // en paleta, donde es un indice).
  auto sample = [&](const u8* row, usize n, bool index) -> u32 {
    if(depth == 8) return row[n];
    if(depth == 16) return row[n * 2];
    const usize bit = n * depth;
    const u32 v = (row[bit >> 3] >> (8 - depth - (bit & 7))) & ((1u << depth) - 1);
    return index ? v : v * 255 / ((1u << depth) - 1);
  };
  out.w = (int)w; out.h = (int)h;
  out.px.assign((usize)w * h, 0);
  for(u32 y = 0; y < h; y++) {
    const u8* row = &img[y * stride];
    for(u32 x = 0; x < w; x++) {
      u32 r, g, b, a = 255;
      switch(ctype) {
        case 0: r = g = b = sample(row, x, false);
                if(trns.size() >= 2 && depth <= 8 && sample(row, x, true) == trns[1]) a = 0;
                break;
        case 2: r = sample(row, x * 3, false); g = sample(row, x * 3 + 1, false); b = sample(row, x * 3 + 2, false); break;
        case 3: { const u32 i = sample(row, x, true);
                  r = i * 3 + 2 < plte.size() ? plte[i * 3] : 0;
                  g = i * 3 + 2 < plte.size() ? plte[i * 3 + 1] : 0;
                  b = i * 3 + 2 < plte.size() ? plte[i * 3 + 2] : 0;
                  a = i < trns.size() ? trns[i] : 255; break; }
        case 4: r = g = b = sample(row, x * 2, false); a = sample(row, x * 2 + 1, false); break;
        default: r = sample(row, x * 4, false); g = sample(row, x * 4 + 1, false);
                 b = sample(row, x * 4 + 2, false); a = sample(row, x * 4 + 3, false); break;
      }
      out.px[(usize)y * w + x] = (r << 24) | (g << 16) | (b << 8) | a;
    }
  }
  return true;
}

auto buildMips(Tex& t) -> void {
  // Promedio 2x2 ponderado por alfa: un texel transparente no oscurece el borde del nivel
  // de abajo (su RGB suele ser negro o basura). Lados impares: el ultimo texel se repite.
  t.lv.clear(); t.lw.clear(); t.lh.clear();
  t.lv.push_back(t.px); t.lw.push_back(t.w); t.lh.push_back(t.h);
  while(t.lw.back() > 1 || t.lh.back() > 1) {
    const int pw = t.lw.back(), ph = t.lh.back();
    const int w = std::max(1, pw / 2), h = std::max(1, ph / 2);
    const std::vector<u32>& p = t.lv.back();
    std::vector<u32> n((usize)w * h);
    for(int y = 0; y < h; y++)
      for(int x = 0; x < w; x++) {
        u32 r = 0, g = 0, b = 0, a = 0;
        for(int k = 0; k < 4; k++) {
          const int sx = std::min(pw - 1, x * 2 + (k & 1)), sy = std::min(ph - 1, y * 2 + (k >> 1));
          const u32 c = p[(usize)sy * pw + sx], ca = c & 255;
          r += ((c >> 24) & 255) * ca; g += ((c >> 16) & 255) * ca; b += ((c >> 8) & 255) * ca; a += ca;
        }
        n[(usize)y * w + x] = a ? ((r / a) << 24) | ((g / a) << 16) | ((b / a) << 8) | ((a + 2) / 4) : 0;
      }
    t.lv.push_back(std::move(n)); t.lw.push_back(w); t.lh.push_back(h);
  }
}

auto enhanced(u64 crc64, u32 fmt, u32 siz) -> const Tex* {
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_fxCache.find(keyOf(crc64, fmt, siz));
  return it == g_fxCache.end() ? nullptr : it->second.get();
}

auto enhance(u64 crc64, u32 fmt, u32 siz, int w, int h, const u32* rgba) -> const Tex* {
  auto t = std::make_unique<Tex>();
  std::vector<u32> px(rgba, rgba + (usize)w * h);
  if(g_fx == FxScale4x || g_fx == FxCel) {
    std::vector<u32> a;
    scale2x(w, h, px, a);
    scale2x(w * 2, h * 2, a, px);
    w *= 4; h *= 4;
  }
  if(g_fx == FxCel || g_fx == FxPoster) posterize(px, 4);
  if(g_fx == FxCel) outline(w, h, px);
  t->w = w; t->h = h; t->px = std::move(px);
  buildMips(*t);
  std::lock_guard<std::mutex> lk(g_mu);
  auto& slot = g_fxCache[keyOf(crc64, fmt, siz)];
  if(!slot) slot = std::move(t);
  return slot.get();
}

auto init(const std::string& ident) -> void {
  std::lock_guard<std::mutex> lk(g_mu);
  g_ident = ident;
  g_full.clear(); g_byCrc.clear(); g_dumpedKeys.clear(); g_fxCache.clear();
  g_epoch++;
  g_dump = g_load = false;
  g_fx = FxNone;
  if(const char* f = std::getenv("KESTREL_TEXFX"); f && *f) {
    const std::string v = lower(f);
    g_fx = v == "scale4x" || v == "1" ? FxScale4x : v == "cel" || v == "toon" || v == "2" ? FxCel
         : v == "poster" || v == "3" ? FxPoster : FxNone;
    if(g_fx) std::fprintf(stderr, "[texpack] realce de texturas: %s\n", v.c_str());
  }
  if(const char* d = std::getenv("KESTREL_TEXDUMP"); d && *d) {
    std::error_code ec;
    g_dumpDir = fs::path(d) / ident;
    fs::create_directories(g_dumpDir, ec);
    if(ec) {
      std::fprintf(stderr, "[texpack] no se puede crear %s: %s\n", g_dumpDir.string().c_str(), ec.message().c_str());
    } else {
      g_dump = true;
      // Lo que ya hay en la carpeta no se vuelve a escribir: una segunda sesion de volcado
      // solo anade lo nuevo y no pisa lo que se haya retocado a mano.
      for(auto& e : fs::directory_iterator(g_dumpDir, ec)) {
        std::string id; u64 crc; u32 fmt, siz;
        if(parseName(e.path().filename().string(), id, crc, fmt, siz)) g_dumpedKeys.insert(keyOf(crc, fmt, siz));
      }
      std::fprintf(stderr, "[texpack] volcado en %s (%zu ya presentes)\n", g_dumpDir.string().c_str(), g_dumpedKeys.size());
    }
  }
  if(const char* p = std::getenv("KESTREL_TEXPACK"); p && *p) {
    std::error_code ec;
    fs::path dir(p);
    int n = 0;
    // Orden: <dir>/<ident> (estructura de GLideN64), <dir> filtrando por ident, y si aun asi
    // no hay nada, <dir> entero (pack con otro nombre de ROM en los ficheros).
    if(fs::is_directory(dir / ident, ec)) n = scan(dir / ident, ident, true);
    if(!n && fs::is_directory(dir, ec)) n = scan(dir, ident, false);
    if(!n && fs::is_directory(dir, ec)) {
      n = scan(dir, ident, true);
      if(n) std::fprintf(stderr, "[texpack] aviso: ningun fichero se llama '%s#...'; se usan todos\n", ident.c_str());
    }
    g_load = n > 0;
    std::fprintf(stderr, "[texpack] pack %s: %d texturas\n", p, n);
  }
}

auto find(u64 crc64, u32 fmt, u32 siz) -> const Tex* {
  std::lock_guard<std::mutex> lk(g_mu);
  auto it = g_full.find(keyOf(crc64, fmt, siz));
  if(it == g_full.end()) {
    auto c = g_byCrc.find(crc64);
    if(c == g_byCrc.end()) return nullptr;
    it = g_full.find(c->second);
  }
  Entry& e = it->second;
  if(!e.tried) {
    e.tried = true;
    auto t = std::make_unique<Tex>();
    std::string err;
    if(readPng(e.path, *t, err)) { buildMips(*t); e.tex = std::move(t); }
    else std::fprintf(stderr, "[texpack] %s: %s\n", e.path.c_str(), err.c_str());
  }
  return e.tex.get();
}

auto dumped(u64 crc64, u32 fmt, u32 siz) -> bool {
  std::lock_guard<std::mutex> lk(g_mu);
  return g_dumpedKeys.count(keyOf(crc64, fmt, siz)) != 0;
}

auto dump(u64 crc64, u32 fmt, u32 siz, bool pal, int w, int h, const u32* rgba) -> void {
  std::lock_guard<std::mutex> lk(g_mu);
  if(!g_dump || w <= 0 || h <= 0 || !g_dumpedKeys.insert(keyOf(crc64, fmt, siz)).second) return;
  char name[96];
  if(pal) std::snprintf(name, sizeof name, "#%08X#%01X#%01X#%08X_all.png", (u32)crc64, fmt & 7, siz & 3, (u32)(crc64 >> 32));
  else    std::snprintf(name, sizeof name, "#%08X#%01X#%01X_all.png", (u32)crc64, fmt & 7, siz & 3);
  const fs::path p = g_dumpDir / (g_ident + name);
  if(!writePng(p.string(), w, h, rgba)) std::fprintf(stderr, "[texpack] no se pudo escribir %s\n", p.string().c_str());
}

}  // namespace kestrel::texpack
