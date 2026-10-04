#pragma once
// Posproceso del presentador: escalado y filtros de imagen sobre el cuadro ya compuesto,
// en compute shaders de la GPU, justo antes de copiarlo a la cadena de intercambio.
//
// Es SOLO presentacion: el invitado, el framebuffer en RDRAM y todo lo que miden las puertas
// (volcados, md5, capturas por MCP) siguen siendo el cuadro original. Filtro 0 = el blit de
// siempre, sin pasar por aqui.
//
// Se incluye DESPUES del bloque de cabeceras de Vulkan del fichero que lo usa (volk o
// GLFW_INCLUDE_VULKAN, segun KESTREL_PRDP), porque los tipos Vk* vienen de ahi.
#include "../core/types.hpp"

namespace kestrel::postfx {

// Mismo orden que rt::videoFilter y que la opcion "filter" del lanzador.
enum Filter : int {
  Nearest = 0,    // blit de vecino mas cercano (de fabrica, sin compute)
  Bilinear = 1,   // blit lineal (sin compute)
  Sharp = 2,      // bilineal nitido: pixel cuadrado sin dentado
  Fsr = 3,        // AMD FSR 1.0 (EASU + RCAS)
  Crt = 4,        // tubo: lineas de barrido + rejilla de apertura
  Cel = 5,        // dibujo animado: bandas de tono + contornos
  Oleo = 6,       // pintura al oleo (Kuwahara)
  Anime4k = 7,    // red neuronal Anime4K x2 (M) + FSR hasta la ventana
  Count
};

struct State;

auto create(VkPhysicalDevice phys, VkDevice dev) -> State*;
auto destroy(State* s) -> void;

// Graba en `cb` la cadena del filtro `f` (>= Sharp) leyendo `src` (lineal, layout GENERAL,
// sw x sh) y dejando el resultado en una imagen dw x dh en layout GENERAL lista para usarse
// como origen de un blit 1:1. Devuelve esa imagen, o VK_NULL_HANDLE si algo fallo (el
// llamante cae al blit de siempre). Requiere que la GPU ya haya terminado el cuadro anterior
// (el presentador espera su valla antes de grabar el siguiente).
auto record(State* s, VkCommandBuffer cb, VkImage src, u32 sw, u32 sh, u32 dw, u32 dh, int f)
    -> VkImage;

// Llamar con el cuadro ya terminado en la GPU (tras su valla): remata KESTREL_FXDUMP.
auto frameDone(State* s) -> void;

}  // namespace kestrel::postfx
