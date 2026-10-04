# Filtros de imagen del presentador — `KESTREL_FILTER`

Posproceso de la imagen que se ve en la ventana: escalado inteligente (FSR, red neuronal
Anime4K) y esteticas (tubo CRT, dibujo animado, oleo). Va en compute shaders de Vulkan,
**despues** de componer el cuadro del invitado y justo antes de copiarlo a la cadena de
intercambio (`src/video/postfx.{hpp,cpp}`, enganchado en `presentFrame` de
`src/video/present.cpp`).

Es **solo presentacion**. El invitado, el framebuffer en RDRAM y todo lo que miden las
puertas (volcados, md5, `capture_framebuffer` del MCP) siguen siendo el cuadro original.
De fabrica `nearest` = el blit de siempre, sin pasar por compute: ni las puertas ni los md5
se mueven. Se cambia **en caliente** desde el menu de la ventana (`rt::videoFilter`).

| valor | que hace | pases |
|---|---|---|
| `nearest` | pixel nitido, vecino mas cercano (fiel, defecto) | blit |
| `bilinear` | suaviza al escalar | blit lineal |
| `sharp` | bilineal nitido: pixel cuadrado, sin dentado en escalas no enteras | 1 |
| `fsr` | AMD FSR 1.0: EASU (lanczos orientado por el gradiente) + RCAS (realce) | 2 |
| `a4k` | red Anime4K_Upscale_CNN_x2_M a 2x de la resolucion del invitado, luego FSR | 9 + 2 |
| `crt` | haz gaussiano entre lineas (anchura segun brillo) + rejilla de apertura a escala >= 3 | 1 |
| `cel` | dibujo animado: aplanado Kuwahara + bandas de tono + contornos Sobel | 3 + 1 |
| `oleo` | pintura al oleo: Kuwahara de radio 2 sobre la imagen escalada | 2 |

`toon` = `cel`, `anime4k` = `a4k`; tambien vale el numero (0..7).

## Por que estas esteticas son de IMAGEN y no de escena

Un cel-shading de verdad cuantiza la **iluminacion** por normal y traza siluetas con la
profundidad. En la N64 la luz ya viene horneada en el color de vertice (Gouraud) que calcula
el microcodigo, y el presentador solo tiene el cuadro ya iluminado. Lo que se puede hacer sin
tocar el juego es lo que hacen los filtros "toon" de imagen: cuantizar luminancia en bandas
conservando el tono, y trazar donde la imagen cambia de golpe, que en 3D coincide casi
siempre con siluetas y aristas. El aplanado previo (Kuwahara r=1) quita el grano de las
texturas, que si no sale en manchas al cuantizar y en garabatos al trazar. El Sobel mira el
cuadro original a pasos de 1,5 texeles (el filtro lineal promedia dos), en texeles del
invitado: el grosor del trazo no depende del tamano de la ventana.

Siguiente escalon posible (anotado en `docs/GAPS.md`): contornos por **profundidad** leyendo
el z-buffer del invitado, que separaria siluetas reales de bordes de textura.

## Shaders y licencias

- Propios: `src/video/shaders/{sharp,crt,cel,kuwahara}.comp` y `common.glsl` (cabecera
  comun: t0..t7 muestreadores, `outImg` RGBA16F en el binding 8, push constants
  `inSize/outSize/srcSize/param`, grupos de 8x8).
- FSR 1.0 (`easu.comp`, `rcas.comp`): portado de `ffx_fsr1.h`, AMD, MIT.
- Anime4K (`anime4k/Anime4K_Upscale_CNN_x2_M.glsl`): el fichero de mpv sin tocar, bloc97,
  MIT. `tools/gen_present_shaders.py` traduce el formato de usuario de mpv (`//!BIND`,
  `//!SAVE`, `//!WIDTH`) a compute con la cabecera comun.
- Atribucion en `THIRD-PARTY.txt` (puntos 6 y 7).

Todo se compila a SPIR-V y va embebido en `src/video/postfx_spv.inc` (commiteado), asi que
compilar kestrel no pide `glslc`; solo hace falta al tocar un shader:

```
python tools/gen_present_shaders.py
```

## Detalles de implementacion

- El cuadro (RGBA8 lineal, host-visible) se copia a una imagen optima muestreable; cada
  pase lee hasta 8 imagenes con muestreador lineal CLAMP y escribe una RGBA16F. Todas en
  layout GENERAL, una barrera de memoria entre pases. La salida mide exactamente el
  rectangulo util de la ventana (`dw x dh`, ya con aspecto) y se copia 1:1 al swapchain.
- Imagenes por nombre, rehechas al cambiar de tamano. Pool de descriptores reiniciado cada
  cuadro (el presentador espera la valla del anterior).
- Requisito: RGBA16F con muestreo lineal + storage + blit-src (cualquier GPU de la ultima
  decada). Si falta, o cualquier pase falla, el filtro se apaga y vuelve el blit nearest,
  con un aviso `[postfx]` en stderr.

## Verificacion

`KESTREL_FXDUMP=<f.ppm>` vuelca UNA vez la salida del filtro (lo que va a la cadena) en el
cuadro presentado numero `KESTREL_FXDUMP_AT` (300 por defecto). Existe porque capturar la
pantalla de una ventana Vulkan desde fuera no es fiable (sale el escritorio o en blanco).
Comprobado 2026-10-04 en SM64 (titulo, RX 570, ventana 960x720): las seis cadenas de
compute corren sin fallo y dan la imagen esperada.

Coste, `KESTREL_HEARTBEAT` en SM64 a 960x720: el tiempo medio del presentador pasa de
0,93 ms (`nearest`) a 3,72 ms con `a4k`, el filtro mas caro, y la velocidad del invitado
no cambia (CPU ~100 % en los dos). El resto de filtros cuesta menos.

```
KESTREL_PRDP=1 KESTREL_VIDEO=1 KESTREL_WINSCALE=3 KESTREL_FILTER=cel \
  KESTREL_FXDUMP=cel.ppm KESTREL_FXDUMP_AT=400 timeout 14 kestrel64.exe sm64.z64 --run
```
