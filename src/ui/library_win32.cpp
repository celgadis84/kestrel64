#include "library.hpp"
#include "profile.hpp"
#include "../core/archive.hpp"

#ifdef _WIN32
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <shlobj.h>
#include <mmsystem.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace kestrel::ui {
namespace {

using kestrel::u8;
using kestrel::u32;
using kestrel::u64;
using kestrel::usize;

// ============================================================ estetica (WonderMenu)
//
// La referencia no es un lanzador de PC sino un menu de cartucho: WonderMenu corre EN la
// consola, con la paleta y las limitaciones de la maquina. De ahi salen las cuatro
// decisiones de las que cuelga todo lo demas:
//
//  1. La escena entera se dibuja en un lienzo de 384x216 y se sube a la ventana con
//     StretchBlt en COLORONCOLOR (vecino mas cercano, cero suavizado) por un factor ENTERO.
//     El pixel gordo es deliberado; el parecido sale de la proporcion, no de copiar tamanos.
//  2. Tipografia de mapa de bits: la fuente "Terminal" de Windows (8x12 / 8x16, OEM), que es
//     literalmente una fuente de consola de la epoca, pedida con NONANTIALIASED_QUALITY.
//  3. Paleta corta y plana, alto contraste, cero degradados de escritorio.
//  4. Transiciones cortas y duras (~120 ms) y pitidos secos al mover y al elegir.
//
// El carrusel es 3D de verdad -- proyeccion en perspectiva y rasterizado de los cuadrilateros
// con interpolacion corregida por 1/z -- porque una caratula inclinada con PlgBlt (que solo
// sabe transformaciones afines) se ve como un paralelogramo, no como una caja girada.

constexpr int kCanW = 384, kCanH = 216;    // lienzo logico (16:9, ancho de 48 caracteres)

constexpr u32 kColBg    = 0x0a0d16;        // fondo, casi negro azulado
constexpr u32 kColBar   = 0x161c30;        // barras de cabecera y de ayuda
constexpr u32 kColPanel = 0x101728;        // panel de datos del cartucho
constexpr u32 kColInk   = 0xe6e9f2;        // texto principal
constexpr u32 kColDim   = 0x76809c;        // texto secundario
constexpr u32 kColAcc   = 0xf2ba20;        // acento calido (el amarillo de los botones C)

// Geometria del carrusel, en pixeles del lienzo. La camara mira hacia +Z desde el origen con
// distancia focal kFocal; una tarjeta a z = kDepth sale a escala 1:1, o sea que kHalfW/kHalfH
// son directamente el medio ancho y el medio alto de la caratula central en el lienzo.
constexpr float kFocal = 300.0f, kDepth = 300.0f;
constexpr float kHalfW = 33.0f, kHalfH = 46.0f;   // caja de N64: relacion ~0.72
constexpr float kSideX = 67.0f, kStepX = 24.0f;   // salto al primer lateral, y entre laterales
constexpr float kSideZ = 78.0f, kStepZ = 52.0f;
constexpr float kTilt  = 1.02f;                   // ~58 grados de giro sobre Y
constexpr float kMaxD  = 4.2f;                    // tarjetas visibles a cada lado
constexpr int   kCarY  = 76;                      // altura del centro del carrusel
constexpr float kReflFrac = 0.30f;                // trozo de caratula que se refleja
constexpr float kReflTop  = 0.34f;                // opacidad del reflejo en la costura

constexpr int kTexW = 132, kTexH = 184;    // tamano al que se normaliza toda caratula

// Bandas verticales del lienzo.
constexpr int kBarH   = 15;                // cabecera
constexpr int kTitleY = 148;               // titulo del juego elegido (fuente grande)
constexpr int kFileY  = 168;               // nombre de fichero
constexpr int kInfoY  = 182;               // panel con los datos del cartucho
constexpr int kHelpY  = 200;               // barra de ayuda

// ============================================================ color y lienzo

inline auto cref(u32 c) -> COLORREF { return RGB((c >> 16) & 255, (c >> 8) & 255, c & 255); }

inline auto lerpC(u32 a, u32 b, float t) -> u32 {
  if(t <= 0) return a;
  if(t >= 1) return b;
  int r = (int)(((a >> 16) & 255) + ((int)((b >> 16) & 255) - (int)((a >> 16) & 255)) * t);
  int g = (int)(((a >> 8) & 255) + ((int)((b >> 8) & 255) - (int)((a >> 8) & 255)) * t);
  int bl = (int)((a & 255) + ((int)(b & 255) - (int)(a & 255)) * t);
  return ((u32)r << 16) | ((u32)g << 8) | (u32)bl;
}

inline auto scaleC(u32 c, float k) -> u32 {
  int r = (int)(((c >> 16) & 255) * k), g = (int)(((c >> 8) & 255) * k), b = (int)((c & 255) * k);
  if(r > 255) r = 255;
  if(g > 255) g = 255;
  if(b > 255) b = 255;
  return ((u32)r << 16) | ((u32)g << 8) | (u32)b;
}

// Un DIB de 32 bits CON su HDC: los pixeles se escriben a pelo (el rasterizador 3D) y ademas
// se puede pintar encima con GDI (el texto). Las dos cosas sobre la misma memoria, que es
// justo lo que hace falta aqui y lo que un bitmap suelto no da.
struct Canvas {
  HDC dc = nullptr;
  HBITMAP bmp = nullptr;
  HGDIOBJ oldBmp = nullptr;
  u32* px = nullptr;
  int w = 0, h = 0;

  auto create(int W, int H) -> bool {
    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof bi.bmiHeader;
    bi.bmiHeader.biWidth = W;
    bi.bmiHeader.biHeight = -H;            // negativo = de arriba abajo, como el lienzo
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    bmp = CreateDIBSection(nullptr, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if(!bmp) return false;
    dc = CreateCompatibleDC(nullptr);
    oldBmp = SelectObject(dc, bmp);
    px = (u32*)bits;
    w = W;
    h = H;
    return true;
  }
  auto destroy() -> void {
    if(dc) { SelectObject(dc, oldBmp); DeleteDC(dc); dc = nullptr; }
    if(bmp) { DeleteObject(bmp); bmp = nullptr; }
    px = nullptr;
  }
  auto fill(int x0, int y0, int x1, int y1, u32 c) -> void {
    if(x0 < 0) x0 = 0;
    if(y0 < 0) y0 = 0;
    if(x1 > w) x1 = w;
    if(y1 > h) y1 = h;
    for(int y = y0; y < y1; y++)
      for(int x = x0; x < x1; x++) px[(usize)y * w + x] = c;
  }
  inline auto blend(int x, int y, u32 c, float a) -> void {
    if((unsigned)x >= (unsigned)w || (unsigned)y >= (unsigned)h) return;
    u32& d = px[(usize)y * w + x];
    d = a >= 0.999f ? c : lerpC(d, c, a);
  }
};

// Linea con paso uniforme: el marco de la caratula elegida sigue el cuadrilatero proyectado,
// que no es un rectangulo, asi que no vale un Rectangle() de GDI.
auto line(Canvas& cv, float x0, float y0, float x1, float y1, u32 c, float a) -> void {
  int steps = (int)(std::max(std::fabs(x1 - x0), std::fabs(y1 - y0)) + 1.0f);
  for(int i = 0; i <= steps; i++) {
    float t = steps ? (float)i / steps : 0.0f;
    cv.blend((int)(x0 + (x1 - x0) * t + 0.5f), (int)(y0 + (y1 - y0) * t + 0.5f), c, a);
  }
}

// ============================================================ decodificador PNG minimo
//
// Las caratulas que cachea el lanzador son PNG. Traer una biblioteca de imagenes (GDI+, WIC)
// por esto seria pagar una dependencia entera por un decodificador que corre un punado de
// veces al abrir un menu -- y el DEFLATE ya esta escrito en el proyecto, que es la unica
// parte dificil del formato (src/core/archive.cpp, el mismo que abre las ROMs en .zip).
// Se admite lo que sale de las fuentes reales: 8/16 bits, gris, gris+alfa, RGB, RGBA y
// paleta, sin entrelazar. Cualquier otra cosa cae al marcador de posicion.

struct Image {
  int w = 0, h = 0;
  std::vector<u32> px;   // 0x00RRGGBB, alfa ya compuesto contra el fondo del panel
};

auto decodePng(const u8* d, usize n, Image& out) -> bool {
  static const u8 kSig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
  if(n < 8 || std::memcmp(d, kSig, 8) != 0) return false;
  auto be32 = [](const u8* p) -> u32 {
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | (u32)p[3];
  };
  usize p = 8;
  int w = 0, h = 0, depth = 0, ctype = 0, inter = 0;
  std::vector<u8> idat, plte;
  while(p + 12 <= n) {
    u32 len = be32(d + p);
    const u8* type = d + p + 4;
    if((usize)len > n || p + 12 + (usize)len > n) break;
    const u8* body = d + p + 8;
    if(!std::memcmp(type, "IHDR", 4) && len >= 13) {
      w = (int)be32(body);
      h = (int)be32(body + 4);
      depth = body[8];
      ctype = body[9];
      inter = body[12];
    } else if(!std::memcmp(type, "PLTE", 4)) {
      plte.assign(body, body + len);
    } else if(!std::memcmp(type, "IDAT", 4)) {
      idat.insert(idat.end(), body, body + len);
    } else if(!std::memcmp(type, "IEND", 4)) {
      break;
    }
    p += 12 + (usize)len;
  }
  if(w <= 0 || h <= 0 || w > 4096 || h > 4096 || inter != 0 || idat.size() < 3) return false;
  int ch = ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : ctype == 4 ? 2 : ctype == 6 ? 4 : 0;
  if(!ch) return false;
  const bool pal = ctype == 3;
  if(depth != 8 && depth != 16 && !(pal && (depth == 1 || depth == 2 || depth == 4)))
    return false;

  // El bloque IDAT lleva envoltura zlib: 2 bytes de cabecera delante y un adler32 detras.
  // Dentro es DEFLATE crudo, que es lo que entiende archive::inflate.
  std::vector<u8> raw;
  std::string err;
  if(!archive::inflate(idat.data() + 2, idat.size() - 2, raw, err)) return false;

  const int bpl = (w * ch * depth + 7) / 8;   // bytes por linea, sin el byte de filtro
  const int bpp = (ch * depth + 7) / 8;       // distancia al pixel anterior, minimo 1
  if(raw.size() < (usize)(bpl + 1) * (usize)h) return false;

  std::vector<u8> img((usize)bpl * (usize)h);
  for(int y = 0; y < h; y++) {
    const u8* src = raw.data() + (usize)(bpl + 1) * (usize)y;
    const u8 f = src[0];
    src++;
    u8* cur = img.data() + (usize)bpl * (usize)y;
    const u8* up = y ? cur - bpl : nullptr;
    for(int x = 0; x < bpl; x++) {
      int a = x >= bpp ? cur[x - bpp] : 0;
      int b = up ? up[x] : 0;
      int c = (up && x >= bpp) ? up[x - bpp] : 0;
      int v = src[x];
      switch(f) {
        case 0: break;
        case 1: v += a; break;
        case 2: v += b; break;
        case 3: v += (a + b) / 2; break;
        case 4: {
          int pa = std::abs(b - c), pb = std::abs(a - c), pc = std::abs(a + b - 2 * c);
          v += (pa <= pb && pa <= pc) ? a : (pb <= pc ? b : c);
          break;
        }
        default: return false;
      }
      cur[x] = (u8)v;
    }
  }

  out.w = w;
  out.h = h;
  out.px.assign((usize)w * (usize)h, 0);
  const int step = depth == 16 ? 2 : 1;   // de 16 bits nos quedamos con el byte alto
  for(int y = 0; y < h; y++) {
    const u8* r = img.data() + (usize)bpl * (usize)y;
    for(int x = 0; x < w; x++) {
      int cr = 0, cg = 0, cb = 0, ca = 255;
      if(pal) {
        int idx;
        if(depth == 8) {
          idx = r[x];
        } else {
          const int per = 8 / depth, mask = (1 << depth) - 1;
          idx = (r[x / per] >> (8 - depth * (x % per + 1))) & mask;
        }
        usize o = (usize)idx * 3;
        if(o + 2 < plte.size()) { cr = plte[o]; cg = plte[o + 1]; cb = plte[o + 2]; }
      } else {
        const u8* q = r + (usize)x * ch * step;
        if(ctype == 0 || ctype == 4) {
          cr = cg = cb = q[0];
          if(ctype == 4) ca = q[step];
        } else {
          cr = q[0];
          cg = q[step];
          cb = q[2 * step];
          if(ctype == 6) ca = q[3 * step];
        }
      }
      u32 c = ((u32)cr << 16) | ((u32)cg << 8) | (u32)cb;
      out.px[(usize)y * (usize)w + x] = ca >= 255 ? c : lerpC(kColPanel, c, ca / 255.0f);
    }
  }
  return true;
}

// ============================================================ catalogo de ROMs

struct Entry {
  std::string path, file, base;   // ruta completa, nombre de fichero, nombre sin extension
  std::string title;              // nombre interno del cartucho (o el del fichero)
  std::string cart, region, fmt;
  u64 size = 0;
  u32 crc1 = 0, crc2 = 0;
  bool hdr = false;
  std::vector<u32> tex;           // caratula normalizada a kTexW x kTexH
  int state = 0;                  // 0 sin cargar, 1 caratula de disco, 2 marcador
};

auto upper(std::string s) -> std::string {
  for(char& c : s) c = (char)std::toupper((unsigned char)c);
  return s;
}

auto trim(std::string s) -> std::string {
  usize a = s.find_first_not_of(" \t\r\n");
  usize b = s.find_last_not_of(" \t\r\n");
  return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

auto extOf(const std::string& f) -> std::string {
  usize d = f.find_last_of('.');
  return d == std::string::npos ? std::string() : upper(f.substr(d + 1));
}

auto regionName(char r) -> std::string {
  switch(r) {
    case 'E': return "USA";
    case 'J': return "JAPON";
    case 'P': case 'X': case 'Y': return "EUROPA";
    case 'D': return "ALEMANIA";
    case 'F': return "FRANCIA";
    case 'I': return "ITALIA";
    case 'S': return "ESPANA";
    case 'U': return "AUSTRALIA";
    case 'A': return "ASIA";
    case 'B': return "BRASIL";
    case 'C': return "CHINA";
    case 'K': return "COREA";
    case 'N': return "CANADA";
    default: return "?";
  }
}

// Cabecera del cartucho, con el orden de bytes normalizado: cada volcador guarda uno
// distinto y sin esto el nombre interno sale hecho jirones.
auto readHeader(Entry& e) -> void {
  std::FILE* f = std::fopen(e.path.c_str(), "rb");
  if(!f) return;
  std::fseek(f, 0, SEEK_END);
  long sz = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  e.size = sz > 0 ? (u64)sz : 0;
  u8 h[0x40] = {};
  usize got = std::fread(h, 1, sizeof h, f);
  std::fclose(f);
  if(got < sizeof h) return;

  u32 magic = ((u32)h[0] << 24) | ((u32)h[1] << 16) | ((u32)h[2] << 8) | (u32)h[3];
  if(magic == 0x37804012u) {              // v64: pares de bytes intercambiados
    for(int i = 0; i < 0x40; i += 2) std::swap(h[i], h[i + 1]);
  } else if(magic == 0x40123780u) {       // n64: palabras del reves
    for(int i = 0; i < 0x40; i += 4) {
      std::swap(h[i], h[i + 3]);
      std::swap(h[i + 1], h[i + 2]);
    }
  } else if(magic != 0x80371240u) {
    return;                               // no es una cabecera de N64 reconocible
  }
  e.hdr = true;
  e.crc1 = ((u32)h[0x10] << 24) | ((u32)h[0x11] << 16) | ((u32)h[0x12] << 8) | (u32)h[0x13];
  e.crc2 = ((u32)h[0x14] << 24) | ((u32)h[0x15] << 16) | ((u32)h[0x16] << 8) | (u32)h[0x17];
  std::string name;
  for(int i = 0x20; i < 0x34; i++) name.push_back(h[i] >= 32 && h[i] < 127 ? (char)h[i] : ' ');
  name = trim(name);
  if(!name.empty()) e.title = upper(name);
  char id[5] = {(char)h[0x3b], (char)h[0x3c], (char)h[0x3d], (char)h[0x3e], 0};
  for(int i = 0; i < 4; i++)
    if(id[i] && (id[i] < 32 || id[i] >= 127)) id[i] = '?';
  e.cart = id;
  e.region = regionName((char)h[0x3e]);
}

auto dirExists(const std::string& d) -> bool {
  if(d.empty()) return false;
  DWORD a = GetFileAttributesA(d.c_str());
  return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

auto scanDir(const std::string& dir, std::vector<Entry>& out) -> void {
  out.clear();
  WIN32_FIND_DATAA fd;
  HANDLE hf = FindFirstFileA((dir + "\\*").c_str(), &fd);
  if(hf == INVALID_HANDLE_VALUE) return;
  do {
    if(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
    Entry e;
    e.file = fd.cFileName;
    std::string ext = extOf(e.file);
    if(ext != "Z64" && ext != "N64" && ext != "V64" && ext != "ROM" && ext != "ZIP" &&
       ext != "GZ")
      continue;
    e.path = dir + "\\" + e.file;
    e.base = e.file.substr(0, e.file.size() - ext.size() - 1);
    e.fmt = ext;
    e.title = upper(e.base);
    // De un contenedor no se lee la cabecera: habria que descomprimir 8-64 MB por cada linea
    // de la lista. Se queda con el nombre del fichero, que es lo que hay.
    if(ext != "ZIP" && ext != "GZ") readHeader(e);
    else e.size = ((u64)fd.nFileSizeHigh << 32) | fd.nFileSizeLow;
    out.push_back(std::move(e));
  } while(FindNextFileA(hf, &fd));
  FindClose(hf);
  std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) {
    if(a.title != b.title) return a.title < b.title;
    return a.file < b.file;
  });
}

// ============================================================ caratulas

auto readFileAll(const std::string& p, std::vector<u8>& out) -> bool {
  std::FILE* f = std::fopen(p.c_str(), "rb");
  if(!f) return false;
  std::fseek(f, 0, SEEK_END);
  long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if(n <= 0 || n > 32 * 1024 * 1024) { std::fclose(f); return false; }
  out.resize((usize)n);
  usize got = std::fread(out.data(), 1, (usize)n, f);
  std::fclose(f);
  return got == (usize)n;
}

auto exeDir() -> std::string {
  char buf[MAX_PATH] = {0};
  GetModuleFileNameA(nullptr, buf, MAX_PATH);
  std::string s = buf;
  usize k = s.find_last_of("/\\");
  return k == std::string::npos ? std::string(".") : s.substr(0, k);
}

// El lanzador cachea las caratulas en <perfil>/cache/boxart con el nombre saneado. Aqui se
// lee ESE mismo cache -- una descarga hecha desde el lanzador vale para el menu de dentro y
// al reves -- y ademas se miran las carpetas donde es natural dejarlas a mano.
auto sanitizeKey(const std::string& s) -> std::string {
  std::string o = s;
  for(char& c : o)
    if(std::strchr("\\/:*?\"<>|", c)) c = '_';
  return o;
}

auto profileDir() -> std::string {
  std::string p = profilePath();
  usize k = p.find_last_of("/\\");
  return k == std::string::npos ? std::string(".") : p.substr(0, k);
}

auto findCoverFile(const Entry& e, const std::string& romDir) -> std::string {
  const std::string cache = profileDir() + "\\cache\\boxart";
  const std::string keys[2] = {sanitizeKey(e.base), sanitizeKey(e.title)};
  const std::string dirs[4] = {cache, romDir + "\\boxart", romDir, exeDir() + "\\boxart"};
  for(const std::string& d : dirs) {
    for(const std::string& k : keys) {
      if(k.empty()) continue;
      std::string p = d + "\\" + k + ".png";
      WIN32_FILE_ATTRIBUTE_DATA fa;
      if(!GetFileAttributesExA(p.c_str(), GetFileExInfoStandard, &fa)) continue;
      // Un fichero VACIO es la marca de "ya se busco y no hay": el lanzador la deja para no
      // volver a salir a la red en cada refresco. Aqui significa lo mismo, marcador y ya.
      if(fa.nFileSizeLow == 0 && fa.nFileSizeHigh == 0) return std::string();
      return p;
    }
  }
  return std::string();
}

// Encaja la imagen en la caja de la caratula recortando por el lado que sobra (nunca
// deformando) y promediando el bloque de origen de cada pixel: bajar de 600x800 a 132x184
// tomando muestras sueltas convierte cualquier texto de la caja en ruido.
auto fitCover(const Image& im, std::vector<u32>& out) -> void {
  out.assign((usize)kTexW * kTexH, kColPanel);
  if(im.w <= 0 || im.h <= 0) return;
  const double target = (double)kTexW / kTexH;
  const double src = (double)im.w / im.h;
  int cw = im.w, chh = im.h, cx = 0, cy = 0;
  if(src > target) { cw = (int)(im.h * target + 0.5); cx = (im.w - cw) / 2; }
  else { chh = (int)(im.w / target + 0.5); cy = (im.h - chh) / 2; }
  if(cw < 1) cw = 1;
  if(chh < 1) chh = 1;
  for(int y = 0; y < kTexH; y++) {
    int sy0 = cy + y * chh / kTexH, sy1 = cy + (y + 1) * chh / kTexH;
    if(sy1 <= sy0) sy1 = sy0 + 1;
    for(int x = 0; x < kTexW; x++) {
      int sx0 = cx + x * cw / kTexW, sx1 = cx + (x + 1) * cw / kTexW;
      if(sx1 <= sx0) sx1 = sx0 + 1;
      u32 r = 0, g = 0, b = 0, n = 0;
      for(int sy = sy0; sy < sy1 && sy < im.h; sy++)
        for(int sx = sx0; sx < sx1 && sx < im.w; sx++) {
          u32 c = im.px[(usize)sy * im.w + sx];
          r += (c >> 16) & 255;
          g += (c >> 8) & 255;
          b += c & 255;
          n++;
        }
      if(!n) continue;
      out[(usize)y * kTexW + x] = ((r / n) << 16) | ((g / n) << 8) | (b / n);
    }
  }
}

auto hash32(const std::string& s) -> u32 {
  u32 h = 2166136261u;
  for(char c : s) {
    h ^= (u8)std::toupper((unsigned char)c);
    h *= 16777619u;
  }
  return h;
}

// Sin caratula NO se deja un hueco gris: se fabrica una portada plana con el color derivado
// del hash del titulo, para que cada juego sea reconocible de un vistazo aunque no haya arte.
// La paleta es corta a proposito: doce colores planos, como los de un menu de consola.
auto makePlaceholder(const Entry& e, HFONT small, HFONT big, std::vector<u32>& out) -> void {
  static const u32 kPlate[12] = {
    0x2e54be, 0x1f7a5a, 0x8a2b2b, 0x6b3fa0, 0xb06a12, 0x1d6f86,
    0x8f7a1f, 0x3f5a2a, 0xa03f6b, 0x2b3f8a, 0x7a4a25, 0x2f6f4f,
  };
  const u32 hh = hash32(e.title.empty() ? e.file : e.title);
  const u32 base = kPlate[hh % 12];
  out.assign((usize)kTexW * kTexH, scaleC(base, 0.35f));

  Canvas c;
  if(!c.create(kTexW, kTexH)) return;
  c.fill(0, 0, kTexW, kTexH, scaleC(base, 0.35f));
  c.fill(4, 4, kTexW - 4, kTexH - 4, base);
  c.fill(4, 4, kTexW - 4, 30, scaleC(base, 1.45f));       // banda de arriba, como el lomo
  c.fill(4, 30, kTexW - 4, 32, scaleC(base, 0.30f));
  c.fill(4, kTexH - 22, kTexW - 4, kTexH - 4, scaleC(base, 0.55f));

  SetBkMode(c.dc, TRANSPARENT);
  RECT r = {6, 8, kTexW - 6, 28};
  SelectObject(c.dc, big);
  SetTextColor(c.dc, cref(0xffffff));
  const std::string tag = e.cart.empty() ? std::string("N64") : e.cart;
  DrawTextA(c.dc, tag.c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

  SelectObject(c.dc, small);
  r = {8, 40, kTexW - 8, kTexH - 26};
  SetTextColor(c.dc, cref(0xf4f4f4));
  const std::string t = e.title.empty() ? e.file : e.title;
  DrawTextA(c.dc, t.c_str(), -1, &r, DT_CENTER | DT_WORDBREAK | DT_NOPREFIX);

  r = {6, kTexH - 20, kTexW - 6, kTexH - 6};
  SetTextColor(c.dc, cref(scaleC(base, 1.9f)));
  DrawTextA(c.dc, "SIN CARATULA", -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);

  GdiFlush();
  std::memcpy(out.data(), c.px, (usize)kTexW * kTexH * sizeof(u32));
  c.destroy();
}

// ============================================================ rasterizado en perspectiva

struct Vtx { float x, y, iz, uz, vz; };   // pantalla, 1/z, y u/z, v/z para corregir la textura

auto drawTri(Canvas& cv, const Vtx& a, const Vtx& b, const Vtx& c, const u32* tex,
             float bright, float amt, bool reflect) -> void {
  const float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
  if(std::fabs(area) < 0.0001f) return;
  const float inv = 1.0f / area;
  int x0 = (int)std::floor(std::min(std::min(a.x, b.x), c.x));
  int x1 = (int)std::ceil(std::max(std::max(a.x, b.x), c.x));
  int y0 = (int)std::floor(std::min(std::min(a.y, b.y), c.y));
  int y1 = (int)std::ceil(std::max(std::max(a.y, b.y), c.y));
  if(x0 < 0) x0 = 0;
  if(y0 < 0) y0 = 0;
  if(x1 > cv.w) x1 = cv.w;
  if(y1 > cv.h) y1 = cv.h;
  for(int y = y0; y < y1; y++) {
    const float py = y + 0.5f;
    for(int x = x0; x < x1; x++) {
      const float px = x + 0.5f;
      const float wc = ((b.x - a.x) * (py - a.y) - (b.y - a.y) * (px - a.x)) * inv;
      const float wa = ((c.x - b.x) * (py - b.y) - (c.y - b.y) * (px - b.x)) * inv;
      const float wb = 1.0f - wa - wc;
      if(wa < 0.0f || wb < 0.0f || wc < 0.0f) continue;
      const float iz = wa * a.iz + wb * b.iz + wc * c.iz;
      if(iz <= 0.0f) continue;
      const float u = (wa * a.uz + wb * b.uz + wc * c.uz) / iz;
      const float v = (wa * a.vz + wb * b.vz + wc * c.vz) / iz;
      int tx = (int)(u * kTexW);
      int ty = (int)(v * kTexH);
      if(tx < 0) tx = 0;
      if(tx >= kTexW) tx = kTexW - 1;
      if(ty < 0) ty = 0;
      if(ty >= kTexH) ty = kTexH - 1;
      float f = amt;
      if(reflect) {
        // El reflejo se desvanece hacia abajo y se raya en las lineas impares: se lee como
        // un tubo de rayos catodicos, no como el brillo de cristal de una pagina web.
        f *= kReflTop * (v - (1.0f - kReflFrac)) / kReflFrac;
        if(y & 1) f *= 0.45f;
        if(f <= 0.004f) continue;
      }
      cv.blend(x, y, scaleC(tex[(usize)ty * kTexW + tx], bright), f);
    }
  }
}

auto drawQuad(Canvas& cv, const Vtx q[4], const u32* tex, float bright, float amt,
              bool reflect) -> void {
  drawTri(cv, q[0], q[1], q[2], tex, bright, amt, reflect);
  drawTri(cv, q[0], q[2], q[3], tex, bright, amt, reflect);
}

// ============================================================ estado de la biblioteca

struct HitBox { int idx; float x[4], y[4]; };

struct Lib {
  HWND win = nullptr;
  Canvas cv;
  HFONT fSmall = nullptr, fBig = nullptr, fTex = nullptr;
  std::vector<Entry> roms;
  std::vector<HitBox> hits;
  std::string dir;
  std::string result;
  int sel = 0;
  float pos = 0.0f;
  bool dirty = true;
  bool sound = true;
  int scale = 3, offX = 0, offY = 0;
  int loaded = 0;             // caratulas con textura viva (para poder soltar las lejanas)
  int padDir = 0;             // direccion mantenida en el mando
  DWORD padNext = 0;          // cuando toca repetir
  u32 padBtns = 0;            // flanco de los botones
};

Lib g;
std::atomic<bool> g_open{false};
std::atomic<bool> g_beeping{false};

// Pitido seco de menu. Va en un hilo suelto porque Beep() bloquea lo que dura el tono y el
// bucle de la ventana esta animando el carrusel; y solo uno a la vez, que si no una pulsacion
// mantenida deja una cola de tonos sonando por detras.
auto beep(int freq, int ms) -> void {
  if(!g.sound) return;
  if(g_beeping.exchange(true)) return;
  std::thread([freq, ms] {
    Beep((DWORD)freq, (DWORD)ms);
    g_beeping.store(false);
  }).detach();
}

auto ensureTex(Entry& e) -> void {
  if(e.state) return;
  Image im;
  std::vector<u8> buf;
  const std::string p = findCoverFile(e, g.dir);
  if(!p.empty() && readFileAll(p, buf) && decodePng(buf.data(), buf.size(), im) && im.w > 0) {
    fitCover(im, e.tex);
    e.state = 1;
  } else {
    makePlaceholder(e, g.fTex, g.fBig, e.tex);
    e.state = 2;
  }
  g.loaded++;
}

// ============================================================ texto

auto tprint(Canvas& cv, HFONT f, int x, int y, u32 col, const std::string& s) -> void {
  SelectObject(cv.dc, f);
  SetTextColor(cv.dc, cref(col));
  TextOutA(cv.dc, x, y, s.c_str(), (int)s.size());
}

auto tcenter(Canvas& cv, HFONT f, int x0, int x1, int y, int h, u32 col,
             const std::string& s) -> void {
  SelectObject(cv.dc, f);
  SetTextColor(cv.dc, cref(col));
  RECT r = {x0, y, x1, y + h};
  DrawTextA(cv.dc, s.c_str(), -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

auto tright(Canvas& cv, HFONT f, int x1, int y, int h, u32 col, const std::string& s) -> void {
  SelectObject(cv.dc, f);
  SetTextColor(cv.dc, cref(col));
  RECT r = {0, y, x1, y + h};
  DrawTextA(cv.dc, s.c_str(), -1, &r, DT_RIGHT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
}

auto fmtSize(u64 n) -> std::string {
  char b[32];
  if(n >= 1024 * 1024) std::snprintf(b, sizeof b, "%llu MB", (unsigned long long)(n >> 20));
  else std::snprintf(b, sizeof b, "%llu KB", (unsigned long long)(n >> 10));
  return b;
}

// ============================================================ la escena

auto renderCarousel(Canvas& cv) -> void {
  g.hits.clear();
  const int n = (int)g.roms.size();
  if(!n) return;

  // De lejos a cerca: la del centro se pinta la ultima y tapa a las demas.
  std::vector<int> order;
  for(int i = 0; i < n; i++)
    if(std::fabs(i - g.pos) <= kMaxD) order.push_back(i);
  std::sort(order.begin(), order.end(), [](int a, int b) {
    return std::fabs(a - g.pos) > std::fabs(b - g.pos);
  });

  int budget = 2;   // caratulas nuevas por cuadro: descodificar 40 PNG de golpe se nota
  for(int idx : order) {
    Entry& e = g.roms[idx];
    const float d = idx - g.pos;
    const float ad = std::fabs(d);
    if(!e.state) {
      if(budget <= 0) { g.dirty = true; continue; }
      budget--;
      ensureTex(e);
      g.dirty = true;
    }
    const float s = d < -1.0f ? -1.0f : d > 1.0f ? 1.0f : d;   // rampa de inclinacion
    const float ang = s * kTilt;
    const float ca = std::cos(ang), sa = std::sin(ang);
    const float cx3 = s * kSideX + (d - s) * kStepX;
    const float cz3 = kDepth + std::fabs(s) * kSideZ + (ad - std::fabs(s)) * kStepZ;
    const float bright = 1.0f - 0.42f * std::min(1.0f, ad / 2.2f);
    const float amt = ad <= kMaxD - 1.0f ? 1.0f : std::max(0.0f, kMaxD - ad);

    auto vtx = [&](float u, float yl, float tu, float tv) -> Vtx {
      const float X = cx3 + u * ca;
      const float Z = cz3 + u * sa;
      const float iz = 1.0f / Z;
      Vtx v;
      v.x = kCanW * 0.5f + kFocal * X * iz;
      v.y = kCarY - kFocal * yl * iz;
      v.iz = iz;
      v.uz = tu * iz;
      v.vz = tv * iz;
      return v;
    };

    const u32* tex = e.tex.data();
    const Vtx q[4] = {vtx(-kHalfW, kHalfH, 0, 0), vtx(kHalfW, kHalfH, 1, 0),
                     vtx(kHalfW, -kHalfH, 1, 1), vtx(-kHalfW, -kHalfH, 0, 1)};
    // Reflejo: la misma caratula espejada bajo su canto de abajo, recortada a kReflFrac.
    const float ry = -kHalfH - 2.0f * kHalfH * kReflFrac;
    const Vtx rq[4] = {vtx(-kHalfW, -kHalfH, 0, 1), vtx(kHalfW, -kHalfH, 1, 1),
                       vtx(kHalfW, ry, 1, 1.0f - kReflFrac),
                       vtx(-kHalfW, ry, 0, 1.0f - kReflFrac)};
    drawQuad(cv, rq, tex, bright, amt, true);
    drawQuad(cv, q, tex, bright, amt, false);

    HitBox hb;
    hb.idx = idx;
    for(int k = 0; k < 4; k++) {
      hb.x[k] = q[k].x;
      hb.y[k] = q[k].y;
    }
    g.hits.push_back(hb);

    // Marco de la elegida. Se desvanece con la distancia al centro, asi que durante la
    // transicion no salta de una caratula a otra: se traslada.
    const float hi = 1.0f - std::min(1.0f, ad * 2.0f);
    if(hi > 0.01f)
      for(int k = 0; k < 4; k++)
        line(cv, q[k].x, q[k].y, q[(k + 1) & 3].x, q[(k + 1) & 3].y, kColAcc, hi);
  }
  // Las mas cercanas quedan al final del vector; para acertar el clic hay que probar por ahi.
  std::reverse(g.hits.begin(), g.hits.end());

  // Soltar lo que ya no se ve ni se va a ver pronto: 132x184x4 son 97 KB por caratula y una
  // carpeta de doscientas ROMs no cabe entera en memoria sin razon.
  if(g.loaded > 64) {
    for(int i = 0; i < n; i++) {
      if(!g.roms[i].state || std::fabs(i - g.pos) <= 16.0f) continue;
      g.roms[i].tex.clear();
      g.roms[i].tex.shrink_to_fit();
      g.roms[i].state = 0;
      g.loaded--;
    }
  }
}

auto render(Canvas& cv) -> void {
  cv.fill(0, 0, kCanW, kCanH, kColBg);

  // Suelo: una raya tenue a la altura de la costura del reflejo. Sin ella las caratulas
  // flotan y el carrusel no se apoya en nada.
  const int seam = kCarY + (int)kHalfH;
  cv.fill(0, seam, kCanW, seam + 1, lerpC(kColBg, kColAcc, 0.14f));

  renderCarousel(cv);

  cv.fill(0, 0, kCanW, kBarH, kColBar);
  cv.fill(0, kBarH, kCanW, kBarH + 1, kColAcc);
  cv.fill(0, kInfoY, kCanW, kInfoY + 15, kColPanel);
  cv.fill(0, kHelpY, kCanW, kCanH, kColBar);

  // A partir de aqui SOLO GDI: los pixeles a pelo van todos antes, que si no habria que
  // vaciar la cola de GDI (GdiFlush) entre unos y otros.
  SetBkMode(cv.dc, TRANSPARENT);
  tprint(cv, g.fSmall, 6, 2, kColAcc, "KESTREL64");
  tprint(cv, g.fSmall, 6 + 10 * 8, 2, kColInk, "BIBLIOTECA");
  char buf[256];
  if(g.roms.empty()) std::snprintf(buf, sizeof buf, "0 ROMS");
  else std::snprintf(buf, sizeof buf, "%d / %d", g.sel + 1, (int)g.roms.size());
  tright(cv, g.fSmall, kCanW - 6, 2, 12, kColDim, buf);

  if(g.roms.empty()) {
    tcenter(cv, g.fBig, 0, kCanW, 92, 18, kColInk, "NO HAY ROMS EN ESTA CARPETA");
    tcenter(cv, g.fSmall, 0, kCanW, 116, 12, kColDim, g.dir);
    tcenter(cv, g.fSmall, 0, kCanW, 136, 12, kColAcc,
            "F2 ELEGIR CARPETA    F3 ABRIR UN FICHERO");
  } else {
    const Entry& e = g.roms[g.sel];
    tcenter(cv, g.fBig, 0, kCanW, kTitleY, 18, kColInk, e.title);
    tcenter(cv, g.fSmall, 0, kCanW, kFileY, 12, kColDim, e.file);
    if(e.hdr)
      std::snprintf(buf, sizeof buf, "%s  %s  %s  %s  CRC %08X %08X", e.cart.c_str(),
                    e.region.c_str(), e.fmt.c_str(), fmtSize(e.size).c_str(), e.crc1, e.crc2);
    else
      std::snprintf(buf, sizeof buf, "%s  %s  -  comprimida, cabecera sin leer",
                    e.fmt.c_str(), fmtSize(e.size).c_str());
    tcenter(cv, g.fSmall, 0, kCanW, kInfoY + 1, 13, e.hdr ? kColInk : kColDim, buf);
  }

  tcenter(cv, g.fSmall, 0, kCanW, kHelpY + 2, 12, kColDim,
          "<- -> MOVER   ENTER JUGAR   F2 CARPETA   F5 RELEER   ESC SALIR");
}

// ============================================================ acciones

auto moveSel(int delta) -> void {
  if(g.roms.empty()) return;
  const int n = (int)g.roms.size();
  int s = g.sel + delta;
  if(s < 0) s = 0;
  if(s >= n) s = n - 1;
  if(s == g.sel) return;
  g.sel = s;
  // Un salto largo (Inicio/Fin) no cruza la biblioteca entera volando: la transicion es
  // corta y dura, asi que la posicion se acerca de golpe y solo se anima el ultimo tramo.
  if(std::fabs(g.pos - g.sel) > 3.5f) g.pos = g.sel + (g.pos > g.sel ? 3.5f : -3.5f);
  beep(920, 10);
  g.dirty = true;
}

auto chooseSel() -> void {
  if(g.roms.empty()) return;
  g.result = g.roms[g.sel].path;
  beep(1400, 40);
  PostMessageA(g.win, WM_CLOSE, 0, 0);
}

auto rescan() -> void {
  g.roms.clear();
  g.loaded = 0;
  scanDir(g.dir, g.roms);
  if(g.sel >= (int)g.roms.size()) g.sel = (int)g.roms.size() - 1;
  if(g.sel < 0) g.sel = 0;
  g.pos = (float)g.sel;
  g.dirty = true;
}

// El perfil se guarda releyendolo del disco y tocando SOLO la carpeta: el menu de la ventana
// tiene su propia copia viva del perfil y sobrescribirla entera desde aqui le borraria
// cualquier cambio que estuviera a medias.
auto saveDir() -> void {
  Profile p = loadProfile();
  p.set("romdir", g.dir);
  saveProfile(p);
}

auto pickFolder() -> void {
  BROWSEINFOA bi = {};
  bi.hwndOwner = g.win;
  bi.lpszTitle = "Carpeta de ROMs";
  bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
  LPITEMIDLIST id = SHBrowseForFolderA(&bi);
  if(!id) return;
  char path[MAX_PATH] = {0};
  if(SHGetPathFromIDListA(id, path) && dirExists(path)) {
    g.dir = path;
    saveDir();
    g.sel = 0;
    rescan();
  }
  CoTaskMemFree(id);
}

// El dialogo de siempre sigue estando: una ROM suelta fuera de la carpeta de la biblioteca
// se abre sin tener que mover ficheros de sitio.
auto pickFile() -> void {
  char file[MAX_PATH] = {0};
  OPENFILENAMEA ofn = {};
  ofn.lStructSize = sizeof ofn;
  ofn.hwndOwner = g.win;
  static const char kFilter[] =
      "ROM de Nintendo 64\0*.z64;*.n64;*.v64;*.rom;*.zip;*.gz\0Todos los archivos\0*.*\0";
  ofn.lpstrFilter = kFilter;
  ofn.lpstrTitle = "Elige una ROM de Nintendo 64";
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  if(!GetOpenFileNameA(&ofn) || !file[0]) return;
  g.result = file;
  PostMessageA(g.win, WM_CLOSE, 0, 0);
}

// ============================================================ mando

// Se sondea con joyGetPosEx (winmm, ya enlazado) en vez de GLFW: la biblioteca puede correr
// antes de que exista la ventana del juego, y en todo caso en OTRO hilo, y a GLFW no se le
// puede llamar desde donde no se le inicializo.
auto pollPad() -> void {
  JOYINFOEX ji = {};
  ji.dwSize = sizeof ji;
  ji.dwFlags = JOY_RETURNBUTTONS | JOY_RETURNPOV | JOY_RETURNX | JOY_RETURNY;
  if(joyGetPosEx(JOYSTICKID1, &ji) != JOYERR_NOERROR) {
    g.padDir = 0;
    g.padBtns = 0;
    return;
  }

  int dir = 0;
  if(ji.dwXpos < 16000) dir = -1;
  else if(ji.dwXpos > 49000) dir = 1;
  if(ji.dwPOV != 0xffff && ji.dwPOV != JOY_POVCENTERED) {
    if(ji.dwPOV > 22500 && ji.dwPOV < 31500) dir = -1;
    else if(ji.dwPOV > 4500 && ji.dwPOV < 13500) dir = 1;
  }

  const DWORD now = GetTickCount();
  if(dir != g.padDir) {
    g.padDir = dir;
    if(dir) { moveSel(dir); g.padNext = now + 340; }   // primer paso al momento, luego espera
  } else if(dir && now >= g.padNext) {
    moveSel(dir);
    g.padNext = now + 90;
  }

  const u32 b = (u32)ji.dwButtons;
  const u32 edge = b & ~g.padBtns;
  g.padBtns = b;
  if(edge & (1u << 0)) chooseSel();                                // A / boton 1: jugar
  else if(edge & (1u << 7)) chooseSel();                           // Start en casi todos
  else if(edge & (1u << 1)) PostMessageA(g.win, WM_CLOSE, 0, 0);   // B: salir
}

// ============================================================ ventana

auto layout(HWND h) -> void {
  RECT rc;
  GetClientRect(h, &rc);
  const int cw = rc.right, chh = rc.bottom;
  int s = std::min(cw / kCanW, chh / kCanH);
  if(s < 1) s = 1;
  g.scale = s;
  g.offX = (cw - kCanW * s) / 2;
  g.offY = (chh - kCanH * s) / 2;
}

// Punto (en el lienzo) dentro del cuadrilatero de una caratula.
auto inQuad(const HitBox& q, float px, float py) -> bool {
  auto side = [&](int a, int b) -> float {
    return (q.x[b] - q.x[a]) * (py - q.y[a]) - (q.y[b] - q.y[a]) * (px - q.x[a]);
  };
  const float s0 = side(0, 1), s1 = side(1, 2), s2 = side(2, 3), s3 = side(3, 0);
  return (s0 >= 0 && s1 >= 0 && s2 >= 0 && s3 >= 0) ||
         (s0 <= 0 && s1 <= 0 && s2 <= 0 && s3 <= 0);
}

auto hitTest(int mx, int my) -> int {
  if(!g.scale) return -1;
  const float px = (mx - g.offX) / (float)g.scale;
  const float py = (my - g.offY) / (float)g.scale;
  for(const HitBox& q : g.hits)
    if(inQuad(q, px, py)) return q.idx;
  return -1;
}

LRESULT CALLBACK libProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch(m) {
    case WM_ERASEBKGND:
      return 1;   // se pinta todo en WM_PAINT; borrar antes solo produce parpadeo
    case WM_SIZE:
      layout(h);
      InvalidateRect(h, nullptr, TRUE);
      return 0;
    case WM_TIMER: {
      // Transicion corta y dura: 0.28 por tic de 16 ms deja el viaje en ~120 ms sin rebote.
      const float d = (float)g.sel - g.pos;
      if(std::fabs(d) > 0.002f) { g.pos += d * 0.28f; g.dirty = true; }
      else if(g.pos != (float)g.sel) { g.pos = (float)g.sel; g.dirty = true; }
      pollPad();
      if(g.dirty) { g.dirty = false; InvalidateRect(h, nullptr, FALSE); }
      return 0;
    }
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      render(g.cv);
      RECT rc;
      GetClientRect(h, &rc);
      // Bandas del encaje: el lienzo solo sube por factores enteros, asi que casi siempre
      // sobra un borde. Se pinta del color del fondo, no de negro puro.
      HBRUSH bg = CreateSolidBrush(cref(kColBg));
      RECT b1 = {0, 0, rc.right, g.offY};
      RECT b2 = {0, g.offY + kCanH * g.scale, rc.right, rc.bottom};
      RECT b3 = {0, g.offY, g.offX, g.offY + kCanH * g.scale};
      RECT b4 = {g.offX + kCanW * g.scale, g.offY, rc.right, g.offY + kCanH * g.scale};
      FillRect(dc, &b1, bg);
      FillRect(dc, &b2, bg);
      FillRect(dc, &b3, bg);
      FillRect(dc, &b4, bg);
      DeleteObject(bg);
      SetStretchBltMode(dc, COLORONCOLOR);   // vecino mas cercano: el pixel gordo es el fin
      StretchBlt(dc, g.offX, g.offY, kCanW * g.scale, kCanH * g.scale, g.cv.dc, 0, 0, kCanW,
                 kCanH, SRCCOPY);
      EndPaint(h, &ps);
      return 0;
    }
    case WM_MOUSEWHEEL:
      moveSel(GET_WHEEL_DELTA_WPARAM(w) > 0 ? -1 : 1);
      return 0;
    case WM_LBUTTONDOWN: {
      const int i = hitTest(GET_X_LPARAM(l), GET_Y_LPARAM(l));
      if(i < 0) return 0;
      if(i == g.sel) chooseSel();
      else moveSel(i - g.sel);
      return 0;
    }
    case WM_LBUTTONDBLCLK: {
      const int i = hitTest(GET_X_LPARAM(l), GET_Y_LPARAM(l));
      if(i == g.sel) chooseSel();
      return 0;
    }
    case WM_KEYDOWN:
      switch(w) {
        case VK_LEFT: case 'A': moveSel(-1); return 0;
        case VK_RIGHT: case 'D': moveSel(1); return 0;
        case VK_PRIOR: moveSel(-10); return 0;
        case VK_NEXT: moveSel(10); return 0;
        case VK_HOME: moveSel(-(int)g.roms.size()); return 0;
        case VK_END: moveSel((int)g.roms.size()); return 0;
        case VK_RETURN: case VK_SPACE: chooseSel(); return 0;
        case VK_ESCAPE: PostMessageA(h, WM_CLOSE, 0, 0); return 0;
        case VK_F2: pickFolder(); return 0;
        case VK_F3: pickFile(); return 0;
        case VK_F5: rescan(); return 0;
        case 'O':
          if(GetKeyState(VK_CONTROL) < 0) pickFile();
          return 0;
        default: return 0;
      }
    case WM_CLOSE:
      DestroyWindow(h);
      return 0;
    case WM_DESTROY:
      KillTimer(h, 1);
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcA(h, m, w, l);
}

// Fuente de mapa de bits. "Terminal" es la fuente de trama que Windows arrastra desde la
// epoca de VGA (vgaoem.fon): 8x12 y 8x16 clavados, sin suavizar y sin escalar, que es
// exactamente lo que pide la referencia. Si un dia no estuviera, GDI cae a la de sistema de
// paso fijo y la pantalla se sigue leyendo.
auto makeFont(int height, int weight) -> HFONT {
  return CreateFontA(height, 0, 0, 0, weight, FALSE, FALSE, FALSE, OEM_CHARSET,
                     OUT_RASTER_PRECIS, CLIP_DEFAULT_PRECIS, NONANTIALIASED_QUALITY,
                     FIXED_PITCH | FF_MODERN, "Terminal");
}

// Donde empezar a mirar: lo que dejo el lanzador, si no la carpeta de la ultima ROM, si no
// las carpetas donde el paquete y el arbol de desarrollo dejan las ROMs.
auto startDir(const Profile& p) -> std::string {
  const std::string d = p.get("romdir");
  if(dirExists(d)) return d;
  const std::string r = p.get("rom");
  if(!r.empty()) {
    usize k = r.find_last_of("/\\");
    if(k != std::string::npos && dirExists(r.substr(0, k))) return r.substr(0, k);
  }
  const std::string ex = exeDir();
  static const char* kTry[] = {"\\roms", "\\ROMs", "\\..\\test_roms", "\\..\\..\\test_roms"};
  for(const char* t : kTry)
    if(dirExists(ex + t)) return ex + t;
  return ex;
}

}  // namespace

auto libraryOpen() -> bool { return g_open.load(); }

auto pickRomLibrary(void* ownerHwnd) -> std::string {
  if(g_open.exchange(true)) return {};   // una sola biblioteca por proceso

  // SHBrowseForFolder y el dialogo de fichero quieren COM inicializado en ESTE hilo.
  const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

  static bool cls = false;
  if(!cls) {
    WNDCLASSA wc = {};
    wc.style = CS_DBLCLKS;
    wc.lpfnWndProc = libProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = "kestrel64_library";
    RegisterClassA(&wc);
    cls = true;
  }

  g = Lib();
  Profile prof = loadProfile();
  g.dir = startDir(prof);
  g.sound = prof.getBool("audio");
  g.fSmall = makeFont(12, FW_NORMAL);
  g.fBig = makeFont(16, FW_BOLD);
  g.fTex = makeFont(12, FW_BOLD);
  g.cv.create(kCanW, kCanH);
  scanDir(g.dir, g.roms);
  // Arrancar sobre la ROM que se jugo la ultima vez: volver a abrir el emulador cae donde
  // uno lo dejo, no al principio del alfabeto.
  const std::string last = prof.get("rom");
  if(!last.empty())
    for(int i = 0; i < (int)g.roms.size(); i++)
      if(_stricmp(g.roms[i].path.c_str(), last.c_str()) == 0) { g.sel = i; break; }
  g.pos = (float)g.sel;

  const int scale = 3;
  RECT want = {0, 0, kCanW * scale, kCanH * scale};
  const DWORD style = WS_OVERLAPPEDWINDOW;
  AdjustWindowRect(&want, style, FALSE);
  const int ww = want.right - want.left, wh = want.bottom - want.top;
  int wx = CW_USEDEFAULT, wy = CW_USEDEFAULT;
  if(ownerHwnd) {   // centrada sobre la ventana del juego, que es de donde viene la orden
    RECT o;
    if(GetWindowRect((HWND)ownerHwnd, &o)) {
      wx = o.left + ((o.right - o.left) - ww) / 2;
      wy = o.top + ((o.bottom - o.top) - wh) / 2;
    }
  }
  HWND h = CreateWindowExA(0, "kestrel64_library", "kestrel64 - biblioteca", style, wx, wy, ww,
                           wh, nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
  if(h) {
    g.win = h;
    layout(h);
    SetTimer(h, 1, 16, nullptr);
    ShowWindow(h, SW_SHOW);
    SetForegroundWindow(h);
    MSG msg;
    while(GetMessageA(&msg, nullptr, 0, 0) > 0) {
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }
  }

  const std::string out = g.result;
  if(!out.empty()) saveDir();
  g.cv.destroy();
  if(g.fSmall) DeleteObject(g.fSmall);
  if(g.fBig) DeleteObject(g.fBig);
  if(g.fTex) DeleteObject(g.fTex);
  g = Lib();
  if(SUCCEEDED(co)) CoUninitialize();
  g_open.store(false);
  return out;
}

}  // namespace kestrel::ui

#else   // !_WIN32

namespace kestrel::ui {
auto pickRomLibrary(void*) -> std::string { return {}; }
auto libraryOpen() -> bool { return false; }
}  // namespace kestrel::ui

#endif
