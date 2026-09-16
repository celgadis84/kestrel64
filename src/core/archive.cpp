#include "archive.hpp"
#include <cstdio>
#include <cstring>

namespace kestrel::archive {

namespace {

// --- lector de bits, bit menos significativo primero (el orden de DEFLATE) ------------
struct Bits {
  const u8* in; usize n; usize pos = 0;
  u32 buf = 0, cnt = 0;
  bool err = false;
  auto get(u32 need) -> u32 {
    while(cnt < need) {
      if(pos >= n) { err = true; return 0; }
      buf |= (u32)in[pos++] << cnt;
      cnt += 8;
    }
    u32 v = buf & ((1u << need) - 1u);
    buf >>= need; cnt -= need;
    return v;
  }
  auto align() -> void { pos -= cnt / 8; buf = 0; cnt = 0; }   // devuelve los bytes enteros ya leidos
};

// Arbol canonico de Huffman en la forma compacta de la propia RFC: cuantos codigos hay de
// cada longitud, y los simbolos ordenados por (longitud, simbolo). Decodificar es recorrer
// las longitudes acumulando bits, sin construir tabla de busqueda.
struct Huff {
  u16 count[16] = {};
  u16 symbol[288] = {};
};

auto build(Huff& h, const u8* lens, u32 n) -> bool {
  for(u32 i = 0; i < 16; i++) h.count[i] = 0;
  for(u32 i = 0; i < n; i++) h.count[lens[i]]++;
  if(h.count[0] == n) return false;                  // ningun codigo
  // Kraft: el codigo puede quedar incompleto (un solo simbolo), pero nunca sobre-suscrito.
  int left = 1;
  for(u32 len = 1; len < 16; len++) {
    left <<= 1;
    left -= h.count[len];
    if(left < 0) return false;
  }
  u16 offs[16] = {};
  for(u32 len = 1; len < 15; len++) offs[len + 1] = (u16)(offs[len] + h.count[len]);
  for(u32 i = 0; i < n; i++) if(lens[i]) h.symbol[offs[lens[i]]++] = (u16)i;
  return true;
}

auto decode(Bits& b, const Huff& h) -> int {
  int code = 0, first = 0, index = 0;
  for(u32 len = 1; len < 16; len++) {
    code |= (int)b.get(1);
    if(b.err) return -1;
    int count = h.count[len];
    if(code - count < first) return h.symbol[index + (code - first)];
    index += count;
    first = (first + count) << 1;
    code <<= 1;
  }
  return -1;
}

const u16 kLenBase[29]   = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
const u16 kLenExtra[29]  = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
const u16 kDistBase[30]  = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
const u16 kDistExtra[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};

auto rd16(const u8* p) -> u32 { return (u32)p[0] | ((u32)p[1] << 8); }   // zip/gzip: LSB primero
auto rd32(const u8* p) -> u32 { return rd16(p) | (rd16(p + 2) << 16); }

auto lower(std::string s) -> std::string {
  for(auto& c : s) if(c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
  return s;
}

auto looksLikeRom(const std::string& name) -> bool {
  const std::string n = lower(name);
  static const char* ext[] = {".z64", ".n64", ".v64", ".rom", ".bin", ".u64", ".ndd"};
  for(const char* e : ext) {
    const usize L = std::strlen(e);
    if(n.size() >= L && n.compare(n.size() - L, L, e) == 0) return true;
  }
  return false;
}

}  // namespace

auto crc32(const u8* p, usize n) -> u32 {
  static u32 tab[256];
  static bool init = false;
  if(!init) {
    for(u32 i = 0; i < 256; i++) {
      u32 c = i;
      for(int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB8'8320u ^ (c >> 1)) : (c >> 1);
      tab[i] = c;
    }
    init = true;
  }
  u32 c = 0xFFFF'FFFFu;
  for(usize i = 0; i < n; i++) c = tab[(c ^ p[i]) & 0xff] ^ (c >> 8);
  return c ^ 0xFFFF'FFFFu;
}

auto inflate(const u8* in, usize n, std::vector<u8>& out, std::string& error) -> bool {
  Bits b{in, n};
  Huff fixedLit, fixedDist;
  bool haveFixed = false;
  for(;;) {
    const u32 last = b.get(1);
    const u32 type = b.get(2);
    if(b.err) { error = "deflate: datos truncados"; return false; }
    if(type == 0) {
      // Bloque sin comprimir: alineado a byte, con longitud y su complemento.
      b.align();
      if(b.pos + 4 > n) { error = "deflate: bloque crudo truncado"; return false; }
      const u32 len = rd16(in + b.pos), nlen = rd16(in + b.pos + 2);
      b.pos += 4;
      if((len ^ 0xffffu) != nlen) { error = "deflate: longitud de bloque crudo incoherente"; return false; }
      if(b.pos + len > n) { error = "deflate: bloque crudo se sale"; return false; }
      out.insert(out.end(), in + b.pos, in + b.pos + len);
      b.pos += len;
    } else if(type == 1 || type == 2) {
      Huff litH, distH;
      const Huff* lit; const Huff* dist;
      if(type == 1) {
        if(!haveFixed) {   // arbol fijo de la RFC: 288 literales, 30 distancias de 5 bits
          u8 lens[288];
          for(u32 i = 0;   i < 144; i++) lens[i] = 8;
          for(u32 i = 144; i < 256; i++) lens[i] = 9;
          for(u32 i = 256; i < 280; i++) lens[i] = 7;
          for(u32 i = 280; i < 288; i++) lens[i] = 8;
          build(fixedLit, lens, 288);
          u8 dl[30];
          for(u32 i = 0; i < 30; i++) dl[i] = 5;
          build(fixedDist, dl, 30);
          haveFixed = true;
        }
        lit = &fixedLit; dist = &fixedDist;
      } else {
        // Arbol dinamico: primero viene el arbol con el que se codifican las longitudes.
        const u32 nlen  = b.get(5) + 257;
        const u32 ndist = b.get(5) + 1;
        const u32 ncode = b.get(4) + 4;
        if(b.err || nlen > 286 || ndist > 30) { error = "deflate: cabecera dinamica invalida"; return false; }
        static const u8 ord[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
        u8 clens[19] = {};
        for(u32 i = 0; i < ncode; i++) clens[ord[i]] = (u8)b.get(3);
        Huff codeH;
        if(!build(codeH, clens, 19)) { error = "deflate: arbol de longitudes invalido"; return false; }
        u8 lens[286 + 30] = {};
        for(u32 i = 0; i < nlen + ndist; ) {
          const int sym = decode(b, codeH);
          if(sym < 0) { error = "deflate: simbolo de longitud invalido"; return false; }
          if(sym < 16) { lens[i++] = (u8)sym; continue; }
          u32 rep = 0; u8 val = 0;
          if(sym == 16) {
            if(i == 0) { error = "deflate: repeticion sin longitud previa"; return false; }
            val = lens[i - 1]; rep = 3 + b.get(2);
          } else if(sym == 17) rep = 3 + b.get(3);
          else                 rep = 11 + b.get(7);
          if(b.err || i + rep > nlen + ndist) { error = "deflate: repeticion se sale"; return false; }
          while(rep--) lens[i++] = val;
        }
        if(!build(litH, lens, nlen)) { error = "deflate: arbol de literales invalido"; return false; }
        build(distH, lens + nlen, ndist);   // puede quedar vacio si el bloque es todo literales
        lit = &litH; dist = &distH;
      }
      for(;;) {
        int sym = decode(b, *lit);
        if(sym < 0) { error = "deflate: literal invalido"; return false; }
        if(sym < 256) { out.push_back((u8)sym); continue; }
        if(sym == 256) break;                         // fin de bloque
        sym -= 257;
        if(sym >= 29) { error = "deflate: codigo de longitud fuera de rango"; return false; }
        const u32 len = kLenBase[sym] + b.get(kLenExtra[sym]);
        const int ds = decode(b, *dist);
        if(ds < 0 || ds >= 30) { error = "deflate: codigo de distancia invalido"; return false; }
        const u32 d = kDistBase[ds] + b.get(kDistExtra[ds]);
        if(b.err) { error = "deflate: datos truncados"; return false; }
        if(d == 0 || d > out.size()) { error = "deflate: referencia por detras del inicio"; return false; }
        const usize from = out.size() - d;
        for(u32 k = 0; k < len; k++) out.push_back(out[from + k]);   // se solapa a proposito
      }
    } else {
      error = "deflate: tipo de bloque reservado";
      return false;
    }
    if(last) break;
  }
  return true;
}

namespace {

auto unwrapGzip(std::vector<u8>& data, std::string& what, std::string& error) -> bool {
  const u8* p = data.data();
  const usize n = data.size();
  if(n < 18) { error = "gzip: fichero demasiado corto"; return false; }
  if(p[2] != 8) { error = "gzip: metodo de compresion no es deflate"; return false; }
  const u8 flg = p[3];
  usize off = 10;
  if(flg & 0x04) {                                            // FEXTRA
    if(off + 2 > n) { error = "gzip: cabecera truncada"; return false; }
    off += 2 + rd16(p + off);
  }
  if(flg & 0x08) { while(off < n && p[off]) off++; off++; }    // FNAME
  if(flg & 0x10) { while(off < n && p[off]) off++; off++; }    // FCOMMENT
  if(flg & 0x02) off += 2;                                     // FHCRC
  if(off + 8 >= n) { error = "gzip: cabecera truncada"; return false; }

  const u32 isize = rd32(p + n - 4);                           // tamano final mod 2^32: solo pista
  std::vector<u8> out;
  out.reserve(isize ? isize : n * 4);
  if(!inflate(p + off, n - off - 8, out, error)) return false;
  const u32 want = rd32(data.data() + n - 8);
  if(crc32(out.data(), out.size()) != want) { error = "gzip: CRC32 no cuadra (fichero corrupto)"; return false; }
  char buf[96];
  std::snprintf(buf, sizeof buf, "gzip -> %.2f MB", out.size() / (1024.0 * 1024.0));
  what = buf;
  data.swap(out);
  return true;
}

auto unwrapZip(std::vector<u8>& data, std::string& what, std::string& error) -> bool {
  const u8* p = data.data();
  const usize n = data.size();
  if(n < 22) { error = "zip: fichero demasiado corto"; return false; }
  // El directorio central se localiza por su marca de fin, que puede llevar detras hasta
  // 64 KB de comentario; por eso se busca hacia atras, como manda el formato.
  usize eocd = 0;
  const usize floor_ = n > 66000 ? n - 66000 : 0;
  for(usize i = n - 22; ; i--) {
    if(rd32(p + i) == 0x0605'4b50u) { eocd = i; break; }
    if(i == floor_) break;
  }
  if(!eocd) { error = "zip: no se encuentra el directorio central"; return false; }
  const u32 count = rd16(p + eocd + 10);
  usize cd = rd32(p + eocd + 16);

  // Se elige la entrada: primero una con extension de ROM y, si no la hay, la mas grande
  // (los zip de ROMs suelen traer ademas un .txt o un .nfo).
  usize bestOff = 0; u32 bestSize = 0; bool bestIsRom = false; std::string bestName;
  for(u32 i = 0; i < count && cd + 46 <= n; i++) {
    if(rd32(p + cd) != 0x0201'4b50u) break;
    const u32 nameLen = rd16(p + cd + 28), extraLen = rd16(p + cd + 30), cmtLen = rd16(p + cd + 32);
    const u32 usz = rd32(p + cd + 24);
    if(cd + 46 + nameLen > n) break;
    const std::string name((const char*)p + cd + 46, nameLen);
    const bool isRom = looksLikeRom(name);
    if((isRom && !bestIsRom) || (isRom == bestIsRom && (bestOff == 0 || usz > bestSize))) {
      bestOff = cd; bestSize = usz; bestIsRom = isRom; bestName = name;
    }
    cd += 46 + nameLen + extraLen + cmtLen;
  }
  if(!bestOff) { error = "zip: no hay ninguna entrada utilizable"; return false; }

  const u32 method  = rd16(p + bestOff + 10);
  const u32 wantCrc = rd32(p + bestOff + 16);
  const u32 comp    = rd32(p + bestOff + 20);
  const usize local = rd32(p + bestOff + 42);
  if(local + 30 > n || rd32(p + local) != 0x0403'4b50u) { error = "zip: cabecera local invalida"; return false; }
  const usize start = local + 30 + rd16(p + local + 26) + rd16(p + local + 28);
  if(start + comp > n) { error = "zip: datos comprimidos fuera del fichero"; return false; }

  std::vector<u8> out;
  if(method == 0) {                                   // guardado, sin comprimir
    out.assign(p + start, p + start + comp);
  } else if(method == 8) {                            // deflate
    out.reserve(bestSize ? bestSize : comp * 4);
    if(!inflate(p + start, comp, out, error)) return false;
  } else {
    char buf[112];
    std::snprintf(buf, sizeof buf, "zip: metodo de compresion %u no soportado (solo guardado y deflate)", method);
    error = buf;
    return false;
  }
  if(wantCrc && crc32(out.data(), out.size()) != wantCrc) { error = "zip: CRC32 no cuadra (fichero corrupto)"; return false; }
  char buf[256];
  std::snprintf(buf, sizeof buf, "zip -> \"%s\" (%.2f MB)", bestName.c_str(), out.size() / (1024.0 * 1024.0));
  what = buf;
  data.swap(out);
  return true;
}

}  // namespace

auto stripContainerExt(const std::string& path) -> std::string {
  const usize dot = path.find_last_of('.');
  const usize sep = path.find_last_of("/" + std::string(1, (char)92));
  if(dot == std::string::npos || (sep != std::string::npos && dot < sep)) return path;
  const std::string ext = lower(path.substr(dot));
  if(ext == ".zip" || ext == ".gz") return path.substr(0, dot);
  return path;
}

auto unwrap(std::vector<u8>& data, std::string& what, std::string& error) -> bool {
  what.clear();
  if(data.size() < 8) return true;
  const u8* p = data.data();
  if(p[0] == 'P' && p[1] == 'K' && p[2] == 3 && p[3] == 4) return unwrapZip(data, what, error);
  if(p[0] == 0x1f && p[1] == 0x8b) return unwrapGzip(data, what, error);
  if(p[0] == '7' && p[1] == 'z' && p[2] == 0xbc && p[3] == 0xaf) {
    error = "7z no soportado (LZMA): descomprimelo a .z64/.n64/.v64, .zip o .gz";
    return false;
  }
  if(p[0] == 'R' && p[1] == 'a' && p[2] == 'r' && p[3] == '!') {
    error = "rar no soportado: descomprimelo a .z64/.n64/.v64, .zip o .gz";
    return false;
  }
  return true;   // no es contenedor: la ROM va cruda
}

}  // namespace kestrel::archive
