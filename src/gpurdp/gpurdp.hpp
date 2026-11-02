#pragma once
// kestrel64 -- GPU-RDP propio (docs/GPU-RDP.md). Rasterizador del RDP en compute de Vulkan con
// la semantica de SoftRDP bit a bit. De fabrica desde la fase 7 (KESTREL_GPURDP=0 lo apaga y
// queda SoftRDP). El presentador comparte su contexto Vulkan: volk tiene UNA tabla global de punteros.
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
//
// Fase 2: triangulos CON textura. Cada uno lleva una instantanea de TMEM + TLUT (la que habia
// al rasterizarlo; las que se repiten seguidas comparten ranura) y los 8 tiles ya plegados.

#include "../core/types.hpp"

#include <mutex>
#include <vector>

namespace kestrel::gpurdp {

// Handles del contexto Vulkan. El presentador los reutiliza en vez de crear un segundo
// dispositivo: volk resuelve los punteros de funcion en UNA tabla global, asi que dos contextos
// vivos se pisan las entradas y la siguiente llamada salta a un puntero nulo. Van como void*
// para no arrastrar vulkan.h hasta el nucleo; present.cpp los recastea.
struct SharedVk { void* instance; void* gpu; void* device; u32 queueFamily; void* queue; };

// Candado de la cola compartida. vkQueueSubmit/vkQueuePresentKHR sobre una misma cola no son
// seguros entre hilos, y aqui submiten dos: el RDP y el presentador.
inline auto queueMutex() -> std::mutex& { static std::mutex m; return m; }
inline auto queueLock()   -> void { queueMutex().lock(); }
inline auto queueUnlock() -> void { queueMutex().unlock(); }

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
  T_LOD = 54, T_PLOD, T_PZ, T_PDZ, T_PDZC, T_LASTX, T_LASTY, T_HD, T_KCONV, T_KEYC,
  T_TC = 64, T_TDX = 68, T_TDE = 72, T_TDY = 76, T_TINFO = 80, T_K0 = 81, T_TSLOT = 85, T_CISZ = 86, T_KEYS = 87,
  T_TILES = 88,             // 8 tiles x 5 palabras (ver tri.comp)
  T_SIZE = 128,
};
enum TriFlag : int {
  TF_LEFT = 1, TF_DOOFF = 2, TF_FILL = 4, TF_SHADE = 8, TF_ZACT = 16, TF_ZSRC = 32, TF_AA = 64,
  TF_COMB = 128, TF_NOBLEND = 256, TF_TWO = 512, TF_NOAA = 1024, TF_TEX = 2048,
  TF_PERSP = 4096, TF_LOD = 8192, TF_TEX1 = 16384, TF_FILT = 32768, TF_ACMP = 65536,
  TF_RECT = 131072, TF_FLIP = 262144, TF_COPY = 524288, TF_PIPE1 = 1048576,
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

// Contadores acumulados desde el arranque (telemetria `rdp.stats`). Los escribe el hilo del
// RDP; el lector los copia sin cerrojo (palabras de 64 bits alineadas: valor viejo o nuevo).
struct Stats {
  u64 fills = 0, tris = 0, texTris = 0, flushes = 0, prims = 0, dispatches = 0;
  u64 upNs = 0, recNs = 0, waitNs = 0, downNs = 0;   // pared de anfitrion de los vaciados
};

#ifndef KESTREL_GPURDP
inline auto stats() -> Stats { return {}; }
inline auto wanted() -> bool { return false; }
inline auto init(u32) -> bool { return false; }
inline auto shutdown() -> void {}
inline auto active() -> bool { return false; }
inline auto sharedVk() -> const SharedVk* { return nullptr; }
inline auto queueFill(const FillRect&) -> void {}
inline auto pending() -> bool { return false; }
inline auto queueTri(const TriRec&, const u8* = nullptr) -> bool { return false; }
inline auto flush(u8*, u32, u8*, std::vector<TriOut>* = nullptr) -> void {}
inline auto upscale() -> u32 { return 1; }
inline auto requestUpscale(u32) -> void {}
inline auto scanoutHi(const u8*, u32, u32, u32, u32, u32, u32, std::vector<u32>&, u32&, u32&, u32, u32) -> bool { return false; }
inline auto hdUpload(const void*, double, double) -> u32 { return 0; }
inline auto queueTriHi(const TriRec&, const u8*, const u8*, u32, const u8*) -> void {}
inline auto queueFillHi(const FillRect&, const u8*, u32, const u8*) -> void {}
inline auto submitHi(const u8*, u32, const u8*) -> void {}
#else
// De fabrica; KESTREL_GPURDP=0 pide SoftRDP.
auto wanted() -> bool;
// Levanta Vulkan (instancia, dispositivo, pipelines, espejo de RDRAM). Desde el hilo que corre
// el RDP. false = sin Vulkan util: SoftRDP sigue solo.
auto init(u32 rdramSize) -> bool;
auto shutdown() -> void;
auto active() -> bool;
// Contexto para el presentador; nullptr si no esta activo. La cola se guarda con queueLock.
auto sharedVk() -> const SharedVk*;
auto queueFill(const FillRect& r) -> void;
auto pending() -> bool;
// false = cola llena: vaciar (flush) y volver a encolar. Con `tmem` (4 KB, paleta incluida en
// la mitad alta) el triangulo lleva textura: se guarda la instantanea y su ranura va en T_TSLOT.
auto queueTri(const TriRec& t, const u8* tmem = nullptr) -> bool;
// Ejecuta la cola en la GPU (rellenos y triangulos en el orden en que llegaron), espera y copia
// a `rdram` los bytes de cada primitiva (y a `hidden`, un byte por media palabra, los bits
// ocultos de los de 16bpp). Antes sube al espejo las zonas que leen los triangulos. En `outs`,
// uno por triangulo y en orden, lo que devolvio cada uno.
auto flush(u8* rdram, u32 size, u8* hidden, std::vector<TriOut>* outs = nullptr) -> void;
// Fase 6, KESTREL_GPURDP_UPSCALE=2|4|8: factor de escalado interno (1 = apagado). Con escalado
// cada primitiva se pinta ademas en una copia de la RDRAM a S veces la resolucion (bloque SxS
// por pixel de 1x); la RDRAM y todo lo que ve el invitado siguen siendo los de 1x.
auto upscale() -> u32;
// Cambia el escalado en caliente (1, 2, 4 u 8; 0 = el de arranque). Se aplica al empezar el
// siguiente vaciado; lo pintado antes se ve a 1x hasta que el RDP lo repinta.
auto requestUpscale(u32 factor) -> void;
// Barrido del VI desde la copia de alta: imagen de `type` (2 = 5551, 3 = 8888) en `origin`, paso
// `stride` pixeles, w x h. Sale (w*S) x (h*S) en el formato del cuadro del presentador
// (0xAABBGGRR). Fila cuya RDRAM ya no es la que dejo el RDP (la escribio el CPU o un DMA): se
// replica la de 1x. Si (w*S) x (h*S) pasa de maxW x maxH sale al mayor factor que quepa.
// false = sin escalado o formato no soportado.
auto scanoutHi(const u8* rdram, u32 size, u32 origin, u32 stride, u32 w, u32 h, u32 type,
               std::vector<u32>& out, u32& ow, u32& oh, u32 maxW, u32 maxH) -> bool;
// Textura HD (texpack::Tex) con su escala sobre la original: la sube al buffer HD si no estaba
// y da el valor de T_HD (cabecera + 1). 0 = no se puede (sin shaderFloat64, sin sitio con el
// lote en curso): el primitivo va por el CPU.
auto hdUpload(const void* tex, double scX, double scY) -> u32;
// Modo sombra (con escalado): el CPU pinta la RDRAM a 1x y la primitiva se encola solo para la
// copia de alta, con la RDRAM de antes de pintarla (compara su caja con el espejo: lo escrito
// por otro se replica en alta). submitHi envia el lote sin esperarlo (fin de run() del RDP).
auto queueTriHi(const TriRec& t, const u8* tmem, const u8* rdram, u32 size, const u8* hidden) -> void;
auto queueFillHi(const FillRect& f, const u8* rdram, u32 size, const u8* hidden) -> void;
auto submitHi(const u8* rdram, u32 size, const u8* hidden) -> void;
auto stats() -> Stats;
#endif

}  // namespace kestrel::gpurdp
