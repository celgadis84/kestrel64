# Texturas HD y realce de texturas — `KESTREL_TEXPACK` / `KESTREL_TEXDUMP` / `KESTREL_TEXFX`

Canal de sustitucion de texturas del SoftRDP (`src/rdp/texpack.{hpp,cpp}` + ganchos
`hd*` en `src/rdp/rdp.cpp`). Tres usos:

| variable | que hace |
|---|---|
| `KESTREL_TEXPACK=<carpeta>` | carga un pack de texturas HD en formato Rice / GLideN64 |
| `KESTREL_TEXDUMP=<carpeta>` | vuelca cada textura distinta que dibuja el juego, con nombre de pack |
| `KESTREL_TEXFX=scale4x\|cel\|poster` | realza por algoritmo las texturas originales (sin pack) |

Las tres van tambien en el lanzador y el menu (grupo de video). Apagadas no hacen nada:
cada gancho mira `texpack::active()` y sale; puertas y md5 no se mueven.

**Solo SoftRDP** (`kestrel64-soft.exe`, o `KESTREL_PRDP=0` en un exe con GPU que la
ignore). parallel-RDP muestrea TMEM dentro de sus shaders con el tamano del original y no
tiene por donde meter una textura de otro tamano; si se piden con el exe de GPU sale un
aviso `[texpack]` y no pasa nada.

## Compatibilidad con los packs existentes

Los packs de la comunidad (SM64 Reloaded, los de GLideN64/Rice) nombran cada fichero con el
hash "RiceCRC32" de la textura **en RDRAM** (no en TMEM):

```
<NOMBRE ROM>#<crc 8 hex>#<fmt>#<siz>[#<crc paleta 8 hex>]_all.png
```

kestrel reproduce exactamente esa cuenta (leida de GLideN64 / GLideNHQ para la semantica;
el codigo es propio):

- En cada LOAD_TILE / LOAD_BLOCK se apunta, por direccion de TMEM del tile de carga, la
  direccion de la imagen, el origen (SL, TL), el tamano cargado (acotado por la mascara del
  tile de carga), el ancho y tamano de texel de la imagen, y el DXT del LOAD_BLOCK.
- Al dibujar, con los datos de la carga a la TMEM del tile de dibujo: LOAD_TILE da ancho,
  alto y paso de fila directos; LOAD_BLOCK los deduce del tile de dibujo (tamano de tile,
  mascara, clamp) y el paso de fila de `line` o, si hay DXT, de `ReverseDXT` (Rice).
- El CRC se toma sobre la RDRAM vista como la ve un emulador que la guarda en palabras de
  32 bits del anfitrion (byte b = byte b^3 del invitado), que es donde nacieron los packs.
- CI4/CI8 (o TLUT activa con 4/8 bits): se anade el CRC de la paleta, sobre las entradas
  hasta el indice maximo usado, con la paleta tal como la copio LOAD_TLUT (bytes del
  anfitrion, desde `tmem-256`).

Comprobado 2026-10-04: las 10 texturas que vuelca SM64 en el logo y el titulo tienen
**exactamente** el nombre de un fichero del pack SM64 Reloaded (10/10, comparando solo la
lista de nombres del repositorio publico). O sea, un pack hecho para GLideN64 se pone tal
cual.

Busqueda dentro de `KESTREL_TEXPACK`: primero `<carpeta>/<NOMBRE ROM>`, luego cualquier
fichero de la carpeta (recursivo) cuyo nombre empiece por el nombre de la ROM. Se aceptan
`_all.png`, `_allcibyrgba.png`, `_cibyrgba.png`, `_rgb.png`; si un CRC no casa con su
formato exacto se acepta el mismo CRC con otro fmt/siz (packs con formatos mal anotados).
Los PNG se cargan la primera vez que hacen falta.

## Como se dibuja una textura HD con un rasterizador a 1x

SoftRDP pinta a la resolucion del invitado (320x240), asi que una textura 4x no puede verse
"mas nitida" en pixeles que no hay; lo que si gana es el detalle de cada texel al acercarse
y el color/forma del dibujo nuevo. Para que no centellee de lejos:

- Coordenada: la misma S/T del original, plegada igual que el muestreador (SHIFT, clamp,
  espejo, mascara) pero en continuo, y escalada por `ancho HD / ancho original`.
- Piramide de mips propia (promedio 2x2 ponderado por alfa) y nivel por la huella del pixel
  (vecinos +x/+y, los mismos que usa la unidad de LOD del RDP; en texrect, DsDx/DtDy).
- Bilineal si el juego pide filtro (`SAMPLE_TYPE`), vecino mas cercano si no.
- No se sustituye cuando el juego usa su propio mipmap (`TEX_LOD_EN`), ni en COPY a un
  framebuffer de 8 bits (escribe indices), ni YUV.

Para verlo de verdad en alta resolucion habria que rasterizar a mas resolucion, que es lo que
hace parallel-RDP con `KESTREL_UPSCALE`; las dos cosas no se combinan hoy.

## Hacer un pack propio con IA (Real-ESRGAN y similares)

1. `KESTREL_TEXDUMP=dump` y jugar el tramo: cada textura sale una vez como PNG RGBA con su
   nombre de pack (si la carpeta ya tiene ficheros no los pisa).
2. Pasar la carpeta por el escalador (Real-ESRGAN, waifu2x, Upscayl...). El escalador puede
   cambiar el tamano libremente; kestrel escala por la proporcion real. Conservar el alfa.
3. `KESTREL_TEXPACK=<carpeta de salida>`.

## Realce sin pack — `KESTREL_TEXFX`

Retoca cada textura original la primera vez que se dibuja y la cachea por hash (mismo camino
de muestreo que un pack). Un pack, si casa, manda sobre esto.

| valor | que hace |
|---|---|
| `scale4x` | Scale2x dos veces (algoritmo publico de A. Mazzoleni, implementacion propia): 4x sin inventar colores, rellena las escaleras de las diagonales. Ideal para texturas de pocos colores. |
| `cel` | scale4x + bandas de luminancia (4, tono conservado) + contorno oscuro por Sobel de luma/alfa sobre la textura escalada. |
| `poster` | solo bandas de luminancia, a 1x. |

A diferencia del filtro `cel` del presentador (`docs/FILTROS-IMAGEN.md`), que mira el cuadro
entero, aqui el contorno y las bandas estan **dentro de la textura**: salen donde cambia el
dibujo, se deforman con la geometria y no dependen de la iluminacion. Combinables: texturas
`cel` + filtro de imagen `cel` da bandas en la luz de vertice y siluetas.

## Coste

SM64, 300 intercambios, SoftRDP, i7-870: base 10,5-11,0 s; pack de 10 texturas 4x 12,1 s;
`TEXFX=cel` 11,9 s (~+10 % de pared en el titulo, que es casi todo textura). El hash se
calcula una vez por cambio de carga/TLUT/tile (memo por tile con generacion), no por pixel.

## Verificacion

- Apagado: `gate_quick krom` ALL OK, sm64 `b5521b24` en los dos modos, krom regress=0.
- Volcar no cambia el cuadro (md5 del framebuffer igual con y sin `KESTREL_TEXDUMP`).
- Pack sintetico (volcado x4 con damero y rayas cada 4 texeles HD): las rayas subtexel salen
  alineadas en cara y fondo de SM64, prueba de que el mapeo y la escala son los correctos.
