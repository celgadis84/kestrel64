# Cobertura del RDP y antialias del VI

Estado: implementado (SoftRDP). Oraculos: `parallel-rdp` (shaders `coverage.h`, `blender.h`,
`vi_fetch.frag`, `vi_divot.frag`, `extract_vram.comp`), n64brew y las ROM de PeterLemon
`RDP/AlphaCoverage`.

## La idea que faltaba

El N64 **no guarda un alfa** en el framebuffer. Guarda la **cobertura** del pixel: cuantas
de las 8 sub-muestras que el rasterizador evalua por pixel caen dentro de la primitiva,
en 3 bits (0..7). Ese numero ocupa el sitio del alfa:

| Formato | Donde vive la cobertura |
|---|---|
| RGBA5551 | bit alto = bit 0 de la palabra de 16 bits; 2 bits bajos = **plano oculto** (noveno bit de los chips RDRAM) |
| RGBA8888 | el byte de alfa entero, `alfa = cobertura << 5` |
| I4 / I8 | no hay sitio: la cobertura de memoria se lee como 7 |

kestrel modela el plano oculto en `Memory::rdramHidden` (un byte por palabra de 16 bits),
que es exactamente lo que `extract_vram.comp` lee de su `HiddenRDRAM`.

El error conceptual de antes era tratar la cobertura como un alfa y **fundir el borde en el
blender del RDP**. En hardware casi nunca pasa eso: contra un fondo de cobertura llena
`overflow = (cvg + memCvg) >= 8` es cierto, que apaga `blend_en`, o sea que el RDP se limita
a **guardar** la cobertura. Quien suaviza el borde es el **VI** al barrer. Por eso las dos
mitades tienen que ir juntas: solo con la primera los bordes quedan mas duros que antes.

## Etapa A — el RDP guarda cobertura (`src/rdp/rdp.cpp`)

Por pixel, siguiendo `coverage.h` + `blender.h`:

1. **Alfa del combinador**: `expanded = a + ((a + 1) >> 8)`.
2. **CVG_TIMES_ALPHA** (bit 12): `modulated = (expanded * cvg + 4) >> 3`, `cvg = modulated >> 5`.
   Si no: `modulated = cvg << 5`.
3. **ALPHA_CVG_SELECT** (bit 13): `expanded = modulated` (el alfa que entra al blender ES la
   cobertura modulada).
4. Con AA armado, `cvg == 0` **descarta** el pixel.
5. `overflow = (cvg + memCvg) >= 8`; `blend_en = force_blend || (!overflow && aa_en)`.
6. **COLOR_ON_CVG** (bit 7): sin desbordamiento de cobertura el blender devuelve el color
   de memoria (el borde se pinta con el fondo), antes del atajo de `blend_en`.
7. **CVG_DEST** (bits 9:8) decide que cobertura se guarda:
   `0 CLAMP` = `blend_en ? min(7, mem+cvg) : (cvg-1)&7` · `1 WRAP` = `(cvg+mem)&7` ·
   `2 ZAP` = `7` · `3 SAVE` = `mem`.

FILL y COPY no pasan por nada de esto: escriben la palabra cruda y la cobertura sale del
bit 0 (7 o 0), plano oculto incluido.

## Etapa B — el VI filtra (`src/video/vifilter.cpp`)

Tres filtros encadenados, todos gobernados por VI_CTRL:

- `AA_MODE` (bits 9:8) `< 2` arma el AA. Con `>= 2` **toda** la cobertura se lee como 7
  (`if(!FETCH_AA) color.a = 7` en el oraculo), lo que apaga de hecho AA y divot.
- `DIVOT_ENABLE` (bit 4), `DITHER_FILTER_ENABLE` (bit 16, el de-dither).

1. **AA** (`vi_fetch.frag`): solo en pixeles con cobertura `!= 7`. Mira 6 vecinos
   —(-1,-1), (+1,-1), (-2,0), (+2,0), (-1,+1), (+1,+1)— contando **solo** los de cobertura
   llena, saca el segundo mas bajo y el segundo mas alto por canal y mezcla:
   `offset = second_lo + second_hi - (mid << 1)`, `coeff = 7 - cvg`,
   `color = mid + (((offset * coeff) + 4) >> 3)`, truncado a 8 bits.
2. **De-dither** (misma pasada, rama `cvg == 7`): acumula `clamp(vecino>>3 - mid>>3, -1, 1)`
   sobre el 3x3 y lo suma a `mid & 0xf8`. Quita el ruido de dithering que metio el RDP.
3. **Divot** (`vi_divot.frag`): mediana horizontal de 3 cuando alguno de los tres pixeles no
   tiene cobertura llena. Mata los picos que el AA deja en las esquinas.

### Donde se aplica y donde NO

Solo en el **presentador de ventana**, que es la salida de verdad. El volcado determinista
(`dumpFramebufferBmp`) sigue siendo el **framebuffer crudo**, y a proposito: las referencias
de PeterLemon son capturas del framebuffer, no del barrido, asi que pasarles el filtro las
empeora. Medido sobre las 371 ROM con el filtro puesto en el volcado:

| ROM | sin filtro | con filtro |
|---|---|---|
| RSP/Gradient/RSPGradient | 100.00 | 95.42 |
| RDP/16BPP/.../FillRectangle16BPP320X240 | 98.64 | 97.48 |
| RDP/16BPP/.../Cycle1FillZBufferTriangle16BPP320X240 | 98.15 | 97.63 |
| RDP/32BPP/.../Cycle1FillZBufferTriangle32BPP320X240 | 98.15 | 97.63 |
| Video/I4Decode/RDP/RDPI4Decode | 33.47 | 32.93 |
| EMU/GameBoy/PPU/2BPPTile8x8 | 66.93 | 66.36 |
| media global | **88.73** | 88.69 |

El de-dither es el que mas se nota (RSPGradient es un degradado con dithering: el filtro
hace justo lo que dice y la referencia no lo lleva). `KESTREL_VIFILTER=1` lo mete en el
volcado para poder repetir esta comparacion. Con `KESTREL_PRDP=1` tampoco entra por otro
motivo: parallel-rdp trae su propio VI.

**Desviacion consciente del oraculo**: parallel-rdp expande RGBA5551 a 8 bits con
`(v << 3) & 0xf8` (bits bajos a cero) y arregla el brillo mas tarde con la gamma; kestrel
replica bits (`(v << 3) | (v >> 2)`), que es la convencion que ya tenian el volcado y el
presentador. La diferencia son los 3 bits bajos y solo se nota en pixeles filtrados; que sea
la misma en las dos ramas es lo que mantiene byte a byte el camino sin filtro.
