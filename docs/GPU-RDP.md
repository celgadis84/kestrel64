# GPU-RDP propio — plan (GAPS #15)

Objetivo a largo plazo: un rasterizador RDP en GPU escrito en kestrel, que sustituya a
parallel-RDP (`third_party/parallel-rdp`, Themaister, MIT) y quite esa dependencia.
Hoy hay dos backends: SoftRDP (`src/rdp/rdp.cpp`, ~2,5 k lineas, CPU, oraculo de las
puertas) y parallel-RDP (~59 k lineas con Granite, Vulkan compute, el exe recomendado).

## Por que

- **Dependencia grande y ajena**: 59 k lineas que no controlamos. Cada arreglo de semantica
  del RDP se hace dos veces (SoftRDP y shaders parcheados de parallel-RDP) o solo en uno, y
  las dos ramas divergen (krom: baseline propia `krom-prdp.tsv`, md5 de SM64 por backend).
- **Ya hubo que parchearlo**: orden de bytes de RDRAM (kestrel guarda el invitado en
  big-endian, parallel-RDP asume el swizzle de ares) y banco SPIR-V regenerado con
  `tools/slangmosh_lite.py`. Ver `docs/parallel-rdp-integration.md`.
- **Cosas que no admite y queremos**: texturas HD (`docs/TEXTURAS-HD.md`, hoy solo SoftRDP),
  escalado interno combinado con ellas, contornos de cel por profundidad, telemetria por
  primitiva (coste por triangulo, quien escribio cada pixel) al estilo del resto de kestrel.
- **Un solo modelo**: con un GPU-RDP que comparta la semantica de SoftRDP, el oraculo y el
  rapido dicen lo mismo por construccion.

## API grafica: Vulkan (compute)

| opcion | Windows | Linux | macOS | compute | veredicto |
|---|---|---|---|---|---|
| **Vulkan 1.1+ compute** | si | si | via MoltenVK (sobre Metal) | completo, enteros de 8/16 bit, atomicos, subgrupos | **elegida** |
| Direct3D 12 | si | no | no | completo | solo Windows; duplicaria trabajo |
| Metal | no | no | si | completo | solo Apple; MoltenVK ya lo cubre |
| OpenGL 4.3 compute | si | si | **no** (macOS se queda en GL 4.1, sin compute, y OpenGL esta obsoleto ahi) | limitado | descartada |
| WebGPU (Dawn/wgpu) | si | si | si | sin enteros de 8/16 bit nativos, sin atomicos de 64 | interesante a futuro, inmaduro para esto |

Razones:
- kestrel **ya es Vulkan**: presentador, swapchain, filtros `postfx` (pases compute,
  SPIR-V embebido desde `tools/gen_present_shaders.py` con `glslc`). La infraestructura de
  dispositivo, colas, buffers y compilacion de shaders ya existe y esta probada.
- macOS sale por MoltenVK sin escribir Metal (es lo que hacen Dolphin, PPSSPP y la propia
  parallel-RDP en Mac). El gap de multiplataforma (#8) no depende del RDP.
- Un rasterizador de RDP fiel es **compute, no pipeline grafico**: coverage subpixel,
  blender con lectura del framebuffer, z con modos decal/interpenetracion, 5/5/5/1 con
  coverage en bits ocultos. Nada de eso cabe en el rasterizador fijo de ninguna API; todas
  las opciones serias acaban en compute, y ahi Vulkan es la mas portable.
- Si algun dia hiciera falta D3D12 o Metal nativo, los shaders GLSL se traducen con
  SPIRV-Cross (HLSL/MSL) sin reescribirlos.

## Arquitectura propuesta

Misma idea de base que cualquier RDP en GPU (descrita publicamente por Themaister); el
codigo es nuestro y la semantica sale de SoftRDP, que ya pasa systemtest y krom.

1. **CPU (hilo del RDP)**: decodifica el FIFO como hoy SoftRDP (`SoftRdp::process`), pero en
   vez de rasterizar guarda cada primitiva en un buffer de GPU: aristas en punto fijo,
   deltas de shade/tex/z, y un **indice de estado** (combiner, othermode, tiles, colores)
   deduplicado. Las cargas de TMEM se registran como "eventos de TMEM" en orden.
2. **Binning (compute)**: pantalla en teselas de 8x8; cada tesela recibe la lista ordenada
   de primitivas que la tocan (mascara de bits por grupo). Orden de primitivas = orden del
   FIFO, imprescindible para el blender.
3. **Raster+shade (compute, un hilo por pixel, workgroup = tesela)**: recorre su lista en
   orden, calcula coverage (4 sub-lineas por pixel, como el VHDL de MiSTer y SoftRDP),
   interpola en los mismos enteros que el HW, muestrea TMEM, combiner, blender, z. Todo en
   enteros: bit a bit con SoftRDP es el criterio de aceptacion.
4. **TMEM**: copia de TMEM por "epoca" (entre dos cargas) en un buffer; cada primitiva
   apunta a su epoca. Las texturas HD de SoftRDP pasan a ser un atlas sustituto por epoca.
5. **Coherencia con RDRAM**: el framebuffer y el z viven en la RDRAM del invitado. Al
   SYNC_FULL (o cuando la CPU/RSP lee una zona que el RDP escribio, ver `rdpDrain` y la
   barrera del DP) se baja a RDRAM; antes de rasterizar sobre una zona que la CPU escribio
   se sube. Mismo contrato que hoy cumple `vrdp::runFifo`, que ya se respeta en el nucleo.
6. **Escalado interno**: el raster trabaja en un dominio xN y solo la bajada a RDRAM
   reduce a 1x (lo que hoy da `KESTREL_UPSCALE` con parallel-RDP).

Interfaz: la misma de `src/vrdp/vrdp.hpp` (`init`, `runFifo`, `viWrite`, `scanout`...), asi
el nucleo no se entera. Seleccion `KESTREL_GPURDP=1` mientras conviven los tres.

## Fases y criterio de cada una

Oraculo = SoftRDP. Cada fase: cuadro bit a bit igual que SoftRDP en sus ROMs de krom y en
SM64; `gate_quick` + una puerta nueva `gate_gpurdp` (krom + sm64 contra la baseline de
SoftRDP, **no** contra la de parallel-RDP).

| fase | contenido | prueba |
|---|---|---|
| 0 | esqueleto: dispositivo compartido con el presentador, FILL_RECTANGLE, SYNC_FULL, bajada a RDRAM | krom fill rects |
| 1 | triangulos plano/shade sin textura, coverage, z | krom Shade/Z, SM64 sin texturas no vale -> ROMs de krom |
| 2 | TMEM + cargas (TILE/BLOCK/TLUT, intercambio de filas impares) + todos los formatos | krom Texture*, TextureCoordinates |
| 3 | combiner 1/2 ciclos, blender, fog, alpha compare, dither | krom Combiner/Blender, SM64 md5 |
| 4 | COPY/texrect, framebuffers de 8 bits, YUV | krom Copy, PD menus |
| 5 | coherencia fina (lecturas de framebuffer por CPU, efectos de PD), rendimiento | PD en juego, `pdbench` |
| 6 | escalado interno + texturas HD en GPU | visual |
| 7 | default ON en `kestrel64.exe`; parallel-RDP queda un ciclo como alternativa y luego se retira de `third_party` | `gate_all` + `gate_prdp` sustituida |

## Estado

### Fase 0 -- HECHA 2026-10-05

Codigo: `src/gpurdp/` (`gpurdp.hpp/.cpp`, `shaders/fill.comp`, SPIR-V embebido en
`gpurdp_spv.inc` por `tools/gen_gpurdp_shaders.py`). Vulkan a pelo sobre volk, nada de
Granite. Se compila en los builds con parallel-RDP (de ahi sale volk) y se enciende con
`KESTREL_GPURDP=1`; entonces parallel-RDP no arranca (comparten la tabla global de volk).

Forma elegida para ir migrando sin romper nada: **SoftRDP sigue decodificando el FIFO entero
y el GPU-RDP le roba primitivas una a una**, no la interfaz de `vrdp.hpp` desde el dia uno.
Asi cada fase mueve un trozo y lo que aun no esta en GPU sigue en SoftRDP, bit a bit.

- `SoftRdp::fillRect` en ciclo FILL/COPY encola el rectangulo (`gpurdp::queueFill`) con su
  estado (color image, fill color, rect recortado). Fallback a CPU si el rect se sale de la
  RDRAM, si x1 > ancho (alias con la fila siguiente), base no alineada al pixel o con
  `rdpGuard`/`wrtag` vivos. El coste (`addSpan`/`accountPixels`) se sigue cobrando en CPU.
- `SoftRdp::run` baja la cola (`gpuFlush`) antes de cualquier comando que lea o escriba
  RDRAM (todo lo que no es estado ni otro relleno FILL/COPY, ver `gpuNoFlush`) y al acabar el
  tramo: fuera de `run()` la RDRAM esta siempre al dia (SYNC_FULL incluido).
- Shader: un hilo por palabra de 32 bits del espejo de RDRAM (sin carreras en los bytes
  sueltos de 8/16 bpp); pase aparte para los bits ocultos de 16 bpp. El espejo no se sube
  nunca: un relleno pisa bytes enteros y al volver solo se copian los bytes de los rects.
- Dispositivo propio creado en el hilo del RDP (`Memory::vrdpBringUp`), con extensiones de
  superficie y swapchain; el presentador lo comparte (`gpurdp::sharedVk`) igual que con
  parallel-RDP, y las colas se guardan con el mismo candado (`vrdp::queueLock`).

Prueba: `scripts/gate_gpurdp.sh` (systemtest + sm64 interp/jit contra `sm64.txt` + krom contra
la referencia de interp). Resultado: systemtest 0/3721, sm64 `b5521b24` (= SoftRDP) en los dos
modos, krom 371/371 con las 372 puntuaciones IDENTICAS a la referencia de SoftRDP. SM64 en
ventana: 246 rellenos por GPU en 60 cuadros, presentador Vulkan compartido sin problemas.
`gate_quick` corre tambien `sm64 gpurdp-jit`.

### Fase 1 -- HECHA 2026-10-05

Triangulos SIN textura en 16 bpp (FILL, plano y shade, con o sin z) a la GPU, con todo lo que
el pixel atraviesa en SoftRDP: cobertura de 4 sub-scanlines x 2 columnas (AA y no-AA),
centroide, shade/z en entero, combinador 1/2 ciclos (plan de `buildCombPlan`), blender
completo (LUT de `luts.hpp`, AA con IM_RD, z compare/update con dz y bits ocultos, dither).
Shader `shaders/tri.comp`: un hilo por pixel de la caja de pixeles cubiertos.

- `SoftRdp::gpuTriangle` decide si el triangulo puede ir. Se queda en CPU (vaciando antes la
  cola) si: textura, ci_size != 16 bpp, COPY, `sx1 > ancho` (alias con la fila siguiente),
  ci/zi impares, filas fuera de la RDRAM, filas de color y z solapadas, NOISE en el
  combinador (std::rand en orden de pixel) o COMBINED en el primer ciclo que corre (depende
  del pixel anterior), y con `rdpGuard`/`wrtag`.
- Recorrido de cobertura en CPU (barato, el mismo que el paseo solo-coste): da la cuenta de
  pixeles y los tramos para cobrar, la caja exacta y el ultimo pixel cubierto.
- Cobro diferido: `accountPixels` partido en `acctSnap` (foto del estado al rasterizar) y
  `acctFinish` (lo que depende de cuantos pixeles se escribieron en color y z). La GPU
  devuelve por triangulo esas dos cuentas y el COMBINED de su ultimo pixel; `gpuFlush`
  cierra el cobro en orden de FIFO y restaura `combined`.
- Lecturas de RDRAM: el triangulo declara sus filas de color image y z image; el flush sube
  esas zonas (fundidas) al espejo y a los bits ocultos antes de pintar y las baja despues.
  La RDRAM del CPU esta al dia al empezar el lote, y lo que pinta una primitiva anterior del
  mismo lote lo ve la siguiente en el espejo porque la cola corre en orden, con barrera entre
  primitivas. Medias palabras con atomicAnd/atomicOr (dos pixeles por palabra).
- Los triangulos ya no vacian la cola en `run()` (`gpuNoFlush`): decide `drawTriangle`.

Prueba: `gate_gpurdp` igual que en fase 0 (krom contra la referencia de SoftRDP, sm64 contra
`sm64.txt`). SM64 apenas usa triangulos sin textura; la cobertura real la dan las ROMs de
krom Fill/Shade/ZBuffer Triangle (8-16 triangulos por GPU en cada una).

Siguiente (fase 1): triangulos plano/shade + coverage + z. Ahi ya hay lecturas de RDRAM
(z-buffer, blender), asi que toca subir zonas al espejo antes de pintar.

## Riesgos

- **Rendimiento**: parallel-RDP lleva anos de ajuste. Meta realista: igualarlo en el juego
  mas pesado (PD) en la RX 570 de este anfitrion. Medir con `pdbench` desde la fase 3.
- **Coverage/AA**: es la frontera abierta tambien en SoftRDP (`AlphaCoverage` krom). El
  GPU-RDP la hereda; no es un objetivo de este gap.
- **Licencias**: parallel-RDP es MIT, se puede leer y citar, pero el codigo es propio y
  sin pegar trozos; si se adapta algo concreto, atribucion en `THIRD-PARTY.txt`.
