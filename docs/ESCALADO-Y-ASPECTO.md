# Escalado interno y relacion de aspecto

Cerrado 2026-09-04. Parte del hueco P1 #6 de `docs/GAPS.md` (queda fuera el capitulo de
texturas HD, que es otra cosa: un canal de sustitucion de texturas, no una opcion de video).

Dos ajustes independientes, uno de rasterizado y otro de presentacion.

## Escalado interno — `KESTREL_UPSCALE=1|2|4|8`

Solo con **paraLLEl-RDP** (`KESTREL_PRDP=1`, ejecutable de `build-prdp`). El SoftRDP
rasteriza siempre a resolucion nativa y ahi no hay nada que decidir.

parallel-rdp mantiene un **dominio de RDRAM ampliado** en la GPU: los mismos comandos del
RDP se rasterizan a NxN muestras por pixel del N64, y el VI escanea ese dominio. Lo que el
juego ve en SU RDRAM sigue siendo 1x, asi que los efectos que releen el framebuffer (blur,
capturas, transiciones) siguen funcionando igual.

| `KESTREL_UPSCALE` | scanout de SM64 |
|---|---|
| 1 (defecto) | 640x240 |
| 2 | 1280x480 |
| 4 | 2560x960 |
| 8 | 5120x1920 |

(El 640x240 de partida no es un error: es el VI, que saca dos muestras por pixel en
horizontal.)

Es una bandera del `CommandProcessor`, no del scanout: el factor decide el tamano de los
buffers que se crean al construirlo, o sea que **cambiarlo exige relanzar** — por eso vive
en el entorno y no en `src/core/runtime.hpp`, y el menu lo marca con `(*)`.

`KESTREL_SSAA=1` (avanzada) cambia como se resuelve ese dominio ampliado cuando hay que
volcarlo al framebuffer de 1x del juego: promedia las NxN muestras en vez de coger una, con
tramado. Es antialiasing por supermuestreo para lo que el juego relee. No encoge la imagen
del scanout. parallel-rdp rechaza la combinacion con factor 1, asi que solo se manda cuando
hay escalado.

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

## Verificacion

- Las cuatro escalas comprobadas en SM64 leyendo el tamano real del scanout que publica
  `[vrdp] scanout WxH` (linea que se emite al cambiar de tamano, precisamente porque el
  volcado de framebuffer lee la RDRAM del invitado y ahi el escalado no se ve).
- Con los valores de fabrica (`UPSCALE=1`, `ASPECT=4:3`) las banderas que se le pasan a
  parallel-rdp son **0** y el rectangulo destino es el de siempre: `gate_all` y `gate_prdp`
  siguen dando los mismos md5.
