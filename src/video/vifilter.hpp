#pragma once
#include "../core/types.hpp"

// Filtros de salida del Video Interface.
//
// El RDP no guarda un alfa en el framebuffer: guarda la COBERTURA del pixel (3 bits) en el
// sitio del alfa. Quien convierte esa cobertura en un borde suave no es el RDP sino el VI,
// en el camino de barrido: filtro de antialias sobre los pixeles de cobertura parcial,
// filtro de divot (mediana de 3 en horizontal) para matar los picos que deja el AA en las
// esquinas, y filtro de "de-dither" que promedia el ruido de dithering que metio el RDP.
//
// Oraculo: parallel-rdp `shaders/vi_fetch.frag`, `shaders/vi_divot.frag` y
// `shaders/extract_vram.comp`.
namespace kestrel::vi {

// Bits de VI_CTRL que gobiernan el camino de filtrado.
inline auto ctrlType(u32 ctrl) -> u32 { return ctrl & 3; }             // 2 = 16bpp, 3 = 32bpp
inline auto ctrlAA(u32 ctrl) -> bool { return ((ctrl >> 8) & 3) < 2; } // AA_MODE 0/1 = filtro activo
inline auto ctrlDivot(u32 ctrl) -> bool { return (ctrl & 0x10) != 0; }
inline auto ctrlDedither(u32 ctrl) -> bool { return (ctrl & 0x1'0000) != 0; }

// Cierto cuando el VI haria algo distinto de copiar el framebuffer tal cual. Con AA_MODE >= 2
// toda la cobertura se lee como 7 (parallel-rdp: `if(!FETCH_AA) color.a = 7`), asi que ni el
// filtro de AA ni el de divot tienen efecto y solo queda el de-dither.
auto active(u32 ctrl) -> bool;

// Saca el frame filtrado como 0x00RRGGBB, un u32 por pixel, w*h entradas.
// `hid` es el plano oculto de RDRAM (un byte por palabra de 16 bits) y puede ser nullptr.
auto fetchFiltered(const u8* ram, usize ramSize, const u8* hid, usize hidSize,
                   u32 origin, u32 stride, u32 w, u32 h, u32 ctrl, u32* out) -> void;

}  // namespace kestrel::vi
