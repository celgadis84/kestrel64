#pragma once
// kestrel64 -- GPU-RDP propio (docs/GPU-RDP.md). Rasterizador del RDP en compute de Vulkan con
// la semantica de SoftRDP bit a bit; a la larga sustituye a parallel-RDP (o queda como
// alternativa). KESTREL_GPURDP=1 lo enciende (apagado por defecto) y entonces parallel-RDP no
// arranca: los dos usan volk, que tiene UNA tabla global de punteros, y el presentador comparte
// el contexto del que este vivo.
//
// Fase 0: SoftRDP sigue decodificando el FIFO entero y el GPU-RDP le "roba" primitivas una a
// una. De momento solo FILL_RECTANGLE en ciclo FILL/COPY (escritura pura, no lee RDRAM).
// SoftRDP encola los rectangulos y vacia la cola (flush) antes de cualquier comando que lea o
// escriba RDRAM por su cuenta y al acabar cada tramo, asi que fuera de SoftRdp::run la RDRAM
// siempre esta al dia. Un relleno pisa bytes enteros y al volver solo se copian sus bytes.
//
// Fase 1: tambien triangulos SIN textura en 16 bpp (FILL, plano, shade; z, cobertura,
// combinador y blender). Esos si leen RDRAM (z-buffer, framebuffer al mezclar), asi que el
// flush sube al espejo las filas que tocan antes de pintar. Todo lo de la cola corre en orden
// del FIFO, con barrera entre primitivas.

#include "../core/types.hpp"
#include "../vrdp/vrdp.hpp"   // SharedVk: el presentador lo consume igual venga de quien venga

#include <vector>

namespace kestrel::gpurdp {

#ifdef KESTREL_GPURDP
inline constexpr bool built = true;
#else
inline constexpr bool built = false;
#endif

// Un FILL_RECTANGLE ya recortado al scissor, con el estado del RDP que lo pinta.
struct FillRect {
  u32 addr, width, bpp;      // color image: base, ancho en pixeles, bytes por pixel (1/2/4)
  u32 x0, y0, x1, y1;        // [x0,x1) x [y0,y1), x1 <= width
  u32 color;                 // fill_color
};

// Fase 1: un triangulo SIN textura (FILL, shade o plano) en 16 bpp. `w` es el registro que lee
// shaders/tri.comp (indices R_* de alli, mismos que TriField). El CPU ya hizo el recorrido de
// cobertura para cobrarlo, asi que manda la caja exacta de pixeles cubiertos (bx, by, bw, bh)
// y las zonas de RDRAM que el triangulo lee o escribe: el flush sube esas zonas al espejo antes
// de pintar y las baja despues.
enum TriField : int {
  T_YL = 0, T_YM, T_YH, T_XL, T_DXL, T_XH, T_DXH, T_XM, T_DXM, T_FLAGS,
  T_SX0, T_SY0, T_SX1, T_SY1, T_BX, T_BY, T_BW, T_BH,
  T_CC = 18, T_CDX = 22, T_CDE = 26, T_CDY = 30,
  T_Z = 34, T_ZDX, T_ZDE, T_ZDY, T_CI, T_CIW, T_ZI, T_OLO, T_OHI,
  T_FILL, T_PRIM, T_ENV, T_BLEND, T_FOG, T_FLAT, T_FTEX, T_SEL,
  T_LOD = 54, T_PLOD, T_PZ, T_PDZ, T_PDZC, T_LASTX, T_LASTY,
  T_SIZE = 64,
};
enum TriFlag : int {
  TF_LEFT = 1, TF_DOOFF = 2, TF_FILL = 4, TF_SHADE = 8, TF_ZACT = 16, TF_ZSRC = 32, TF_AA = 64,
  TF_COMB = 128, TF_NOBLEND = 256, TF_TWO = 512, TF_NOAA = 1024,
};
struct TriRec {
  s32 w[T_SIZE];
  u32 lo[2], hi[2];          // zonas [lo, hi) de color y de z (hi == lo: sin zona)
};
// Lo que devuelve cada triangulo: pixeles escritos en color y en z, y el registro COMBINED del
// ultimo pixel cubierto (solo si el combinador estaba programado).
struct TriOut {
  s32 nWrite, nZWrite, comb[4];
};

#ifndef KESTREL_GPURDP
inline auto wanted() -> bool { return false; }
inline auto init(u32) -> bool { return false; }
inline auto shutdown() -> void {}
inline auto active() -> bool { return false; }
inline auto sharedVk() -> const vrdp::SharedVk* { return nullptr; }
inline auto queueFill(const FillRect&) -> void {}
inline auto pending() -> bool { return false; }
inline auto queueTri(const TriRec&) -> bool { return false; }
inline auto flush(u8*, u32, u8*, std::vector<TriOut>* = nullptr) -> void {}
#else
// KESTREL_GPURDP pedido (y distinto de 0).
auto wanted() -> bool;
// Levanta Vulkan (instancia, dispositivo, pipelines, espejo de RDRAM). Desde el hilo que corre
// el RDP. false = sin Vulkan util: SoftRDP sigue solo.
auto init(u32 rdramSize) -> bool;
auto shutdown() -> void;
auto active() -> bool;
// Contexto para el presentador (misma forma que vrdp::sharedVk). Las colas se guardan con
// vrdp::queueLock/queueUnlock, que es el candado que ya toma el presentador.
auto sharedVk() -> const vrdp::SharedVk*;
auto queueFill(const FillRect& r) -> void;
auto pending() -> bool;
// false = cola llena: vaciar (flush) y volver a encolar.
auto queueTri(const TriRec& t) -> bool;
// Ejecuta la cola en la GPU (rellenos y triangulos en el orden en que llegaron), espera y copia
// a `rdram` los bytes de cada primitiva (y a `hidden`, un byte por media palabra, los bits
// ocultos de los de 16bpp). Antes sube al espejo las zonas que leen los triangulos. En `outs`,
// uno por triangulo y en orden, lo que devolvio cada uno.
auto flush(u8* rdram, u32 size, u8* hidden, std::vector<TriOut>* outs = nullptr) -> void;
#endif

}  // namespace kestrel::gpurdp
