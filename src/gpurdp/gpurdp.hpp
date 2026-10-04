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
// siempre esta al dia. El espejo de la GPU no se sube nunca: un relleno pisa bytes enteros y al
// volver solo se copian los bytes de los rectangulos.

#include "../core/types.hpp"
#include "../vrdp/vrdp.hpp"   // SharedVk: el presentador lo consume igual venga de quien venga

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

#ifndef KESTREL_GPURDP
inline auto wanted() -> bool { return false; }
inline auto init(u32) -> bool { return false; }
inline auto shutdown() -> void {}
inline auto active() -> bool { return false; }
inline auto sharedVk() -> const vrdp::SharedVk* { return nullptr; }
inline auto queueFill(const FillRect&) -> void {}
inline auto pending() -> bool { return false; }
inline auto flush(u8*, u32, u8*) -> void {}
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
// Ejecuta la cola en la GPU, espera y copia a `rdram` los bytes de cada rectangulo (y a
// `hidden`, un byte por media palabra, los bits ocultos de los de 16bpp).
auto flush(u8* rdram, u32 size, u8* hidden) -> void;
#endif

}  // namespace kestrel::gpurdp
