# Escalado interno y relacion de aspecto

Cerrado 2026-09-04. Parte del hueco P1 #6 de `docs/GAPS.md` (queda fuera el capitulo de
texturas HD, que es otra cosa: un canal de sustitucion de texturas, no una opcion de video).

Dos ajustes independientes, uno de rasterizado y otro de presentacion.

## Escalado interno — `KESTREL_UPSCALE=1|2|4|8`

Solo con **GPU-RDP** (ejecutable de `build-gpu`). El SoftRDP rasteriza siempre a
resolucion nativa y ahi no hay nada que decidir.

GPU-RDP mantiene copias de alta resolucion (xN) del color y la profundidad en la GPU (modo
sombra, ver `docs/GPU-RDP.md`): el RDP se rasteriza tambien a NxN muestras por pixel del N64
y la presentacion usa esa copia. Lo que el juego ve en SU RDRAM sigue siendo 1x, asi que los
efectos que releen el framebuffer (blur, capturas, transiciones) siguen funcionando igual.

El factor decide el tamano de esas copias, o sea que **cambiarlo exige relanzar** — por eso
vive en el entorno y no en `src/core/runtime.hpp`, y el menu lo marca con `(*)`.

## Relacion de aspecto — `KESTREL_ASPECT=4:3|16:9|estirar`

De presentacion pura: decide el rectangulo destino dentro de la ventana
(`src/video/present.cpp`), con bandas negras en lo que sobra.

- **4:3** (defecto, fiel). El VI saca **siempre** una senal 4:3 sea cual sea la resolucion
  del framebuffer: por eso la relacion NO se saca de `srcW/srcH` (un framebuffer 320x120
  con `Y_SCALE` a la mitad saldria aplastado al doble de ancho).
- **16:9**. Estirar no ensancha el campo de vision — eso solo lo puede hacer el juego,
  dibujando mas mundo. Sirve para los juegos con **modo panoramico propio** (Perfect Dark,
  GoldenEye, Turok, Rush 2), que dibujan un encuadre ancho aplastado dentro del mismo
  framebuffer contando con que la tele lo estire.
- **estirar**: llenar la ventana entera, deformando. Se admite ademas cualquier `W:H` o
  `WxH`.

Esta si se aplica **en caliente** (`rt::aspectW/aspectH`, atomicos leidos por el
presentador en cada cuadro): cambiarla desde el menu se ve en el cuadro siguiente.

## Filtros de imagen

Escalado inteligente (FSR, Anime4K) y esteticas (CRT, dibujo animado, oleo) sobre la imagen
ya compuesta: ver `docs/FILTROS-IMAGEN.md` (`KESTREL_FILTER`).

## Verificacion

- El volcado de framebuffer lee la RDRAM del invitado y ahi el escalado no se ve: la
  comprobacion del tamano real va por la imagen presentada (ver `docs/GPU-RDP.md`).
- Con los valores de fabrica (`UPSCALE=1`, `ASPECT=4:3`) el rectangulo destino es el de
  siempre: `gate_all` y `gate_gpurdp` siguen dando los mismos md5.
