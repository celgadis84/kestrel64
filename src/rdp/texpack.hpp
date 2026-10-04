#pragma once
// Texturas HD: volcado y sustitucion de texturas por hash, compatible con los packs de
// Rice Video / GLideN64 (GLideNHQ). Ver docs/TEXTURAS-HD.md.
//
// El nombre de cada fichero lleva el hash "RiceCRC32" de los texeles tal como estan en
// RDRAM (no de TMEM) mas el de la paleta en las texturas CI:
//
//   <NOMBRE ROM>#<crc 8 hex>#<fmt>#<siz>[#<crc paleta 8 hex>]_all.png
//
// El hash se calcula sobre la RDRAM vista como la ve un emulador que guarda la memoria en
// palabras de 32 bits del anfitrion (byte b del anfitrion = byte b^3 del invitado), que es
// donde nacieron esos packs; asi un pack hecho con GLideN64 casa aqui sin convertir nada.
//
// Solo lo usa SoftRDP (kestrel64-soft.exe): parallel-rdp muestrea TMEM en la GPU y no tiene
// por donde meter una textura de otro tamano. Apagado (sin variables) no hace nada.
#include "../core/types.hpp"

#include <string>
#include <vector>

namespace kestrel::texpack {

// Textura de sustitucion ya decodificada: RGBA8 empaquetado como el muestreador de
// SoftRDP (r << 24 | g << 16 | b << 8 | a).
// `lv` es la piramide de mips (lv[0] = la imagen entera, cada nivel la mitad del anterior
// promediando 2x2): SoftRDP pinta a la resolucion del invitado, asi que una textura 4x vista
// de lejos tiene que bajar de nivel o centellea.
struct Tex {
  int w = 0, h = 0;
  std::vector<u32> px;
  std::vector<std::vector<u32>> lv;
  std::vector<int> lw, lh;
};
auto buildMips(Tex& t) -> void;

// Realce por algoritmo de las texturas originales (sin pack), KESTREL_TEXFX:
//   scale4x  Scale2x dos veces: agranda 4x sin emborronar, conserva bordes de pixel-art
//   cel      scale4x + tonos en bandas + contorno oscuro donde cambia la textura
//   poster   tonos en bandas a la resolucion original (paleta reducida)
enum Fx : int { FxNone = 0, FxScale4x = 1, FxCel = 2, FxPoster = 3 };
extern int g_fx;
// Textura realzada de (clave, rgba original). Cacheada por clave: la segunda vez basta con
// `enhanced`, que no pide decodificar nada.
auto enhanced(u64 crc64, u32 fmt, u32 siz) -> const Tex*;
auto enhance(u64 crc64, u32 fmt, u32 siz, int w, int h, const u32* rgba) -> const Tex*;

// Lee KESTREL_TEXDUMP (carpeta de volcado), KESTREL_TEXPACK (carpeta del pack) y
// KESTREL_TEXFX (realce por algoritmo, ver abajo) y prepara
// el indice. `ident` = nombre interno de la ROM (cabecera 0x20, sin espacios al final).
auto init(const std::string& ident) -> void;

extern bool g_dump;   // KESTREL_TEXDUMP puesto
extern bool g_load;   // KESTREL_TEXPACK puesto y con algun fichero valido
inline auto active() -> bool { return g_dump || g_load || g_fx != FxNone; }

// RiceCRC32 sobre una vista de bytes `hb(i)` (i con signo: el original lee antes del
// origen cuando una fila mide menos de 4 bytes, y aqui se reproduce sin salirse).
// bytesPerLine = width << size >> 1; filas a paso rowStride.
template<typename HB>
auto riceCrc32(HB hb, s64 src, int width, int height, int size, int rowStride) -> u32 {
  auto rd32 = [&](s64 a) -> u32 {
    return (u32)hb(a) | ((u32)hb(a + 1) << 8) | ((u32)hb(a + 2) << 16) | ((u32)hb(a + 3) << 24);
  };
  u32 crc = 0;
  const int bpl = (width << size) >> 1;
  for(int y = height - 1; y >= 0; y--) {
    u32 esi = 0;
    int x = bpl - 4;
    do {
      esi = rd32(src + x) ^ (u32)x;
      crc = (crc << 4) + ((crc >> 28) & 15);
      crc += esi;
      x -= 4;
    } while(x >= 0);
    esi ^= (u32)y;
    crc += esi;
    src += rowStride;
  }
  return crc;
}

// Busca la sustitucion de (crc64, fmt, siz). Carga el PNG la primera vez. nullptr si no
// hay. crc64 = crc de la paleta << 32 | crc de los texeles.
auto find(u64 crc64, u32 fmt, u32 siz) -> const Tex*;

// Vuelca la textura una vez por clave (las siguientes llamadas no hacen nada).
auto dump(u64 crc64, u32 fmt, u32 siz, bool pal, int w, int h, const u32* rgba) -> void;
// true si esa clave ya se volco (para no decodificar TMEM en balde).
auto dumped(u64 crc64, u32 fmt, u32 siz) -> bool;

// PNG minimo (sin dependencias): escribe RGBA8 sin comprimir (bloques "stored") y lee
// PNG de 1-16 bits, todos los tipos de color, sin entrelazado.
auto writePng(const std::string& path, int w, int h, const u32* rgba) -> bool;
auto readPng(const std::string& path, Tex& out, std::string& error) -> bool;

}  // namespace kestrel::texpack
