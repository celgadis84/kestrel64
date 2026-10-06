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

### Fase 2 -- HECHA 2026-10-05

Triangulos CON textura en 16 bpp a la GPU. Las cargas (LOAD_TILE/BLOCK/TLUT, intercambio de
filas impares) siguen en SoftRDP, que es quien escribe su `tmem[]`/`tlut[]`: cada triangulo
texturado se lleva una **instantanea de TMEM + TLUT** (4 KB + 512 B) tal como estaba al
rasterizarlo. Las que se repiten seguidas comparten ranura (memcmp con la ultima); hasta 512
ranuras por lote, y si se llena se vacia la cola y se reintenta. Buffer en binding 5.

- Registro de 128 palabras: S/T/Z/W (inicio, DxDx, DxDe, DxDy), tile base, nivel maximo,
  prim_min_level, SET_CONVERT k0..k3, ranura y los 8 tiles ya plegados (`foldOf`: shift,
  mask, clamp/mirror, sMax/tMax, bytes por fila, base, paleta y formato).
- Shader: `foldCoord`, lectura de los 10 formatos (CI4/8 por TLUT 5551 o IA16, IA4/8/16,
  I4/8, RGBA16/32, YUV con SET_CONVERT), XOR de filas impares, filtro de 3 puntos en entero
  (y MID_TEXEL), punto, division perspectiva con la misma tabla de 64 entradas (generada
  desde `rdp.cpp`), unidad de LOD (`lodSelect`, SHARPEN/DETAIL, mipmap con vecinos +x/+y),
  TEXEL1 de 2 ciclos en tile1 y LOD_FRAC por pixel al combinador.
- Alpha compare (no COPY) con `alphaRef` (dither de alfa salvo ALPHA_CVG_SEL). Un pixel
  descartado sigue dejando su COMBINED si es el ultimo.
- Se quedan en CPU: texturas HD (`texpack`) y COPY.

Esto cubre tambien lo que la tabla pone en fase 3 (combinador, blender, fog, alpha compare,
dither): ya va todo en `tri.comp` desde las fases 1-2, con las mismas exclusiones (NOISE y
COMBINED en el primer ciclo).

Prueba: `gate_gpurdp` ALL OK (systemtest 0/3721, sm64 `b5521b24` interp y jit, krom 371/371
identicas a SoftRDP). SM64 600 cuadros: 475513 triangulos por GPU, 462104 con textura, 103464
instantaneas de TMEM, 105584 vaciados (los texrect y demas aun van por CPU y vacian la cola:
es lo siguiente).

### Fase 4 (parte 1) -- texrect HECHO 2026-10-05

- TEXTURE_RECTANGLE y FLIP en 1/2 ciclos y COPY, a 16 bpp, por el mismo registro (`TF_RECT`,
  `TF_FLIP`, `TF_COPY`) y `rectPixel` en `tri.comp`. S/T en entero: S en 1/4096 de texel
  (1/16384 en COPY, DsDx x4), T en 1/4096; los double de SoftRDP son diadicos exactos, asi que
  coincide bit a bit. Filtro: S/T a 1/32 con empate lejos del cero (como `lroundExact`).
- Alpha compare de COPY (alfa 0 fuera) y no COPY (`alphaRef`); blender con cvg 8 sin z si
  FORCE_BL, si no escritura directa con tijera. Las cuentas (rasterPx, spans) se hacen en CPU.
- Se quedan en CPU: texpack, mipmap (usesLod), COPY a 8 bpp, COMBINED en el primer ciclo.
- Vaciado evitado en cargas: LOAD_TLUT/BLOCK/TILE solo vacian si su rango de lectura (cota
  por arriba) toca lo que la cola va a escribir (hasta 8 intervalos). `KESTREL_GPUFLUSHSTAT=1`
  saca que comandos vacian. SM64 600 cuadros: 105584 -> 73698 vaciados; por comando solo 173
  (LOAD_BLOCK), el resto son el final de `run()` y las primitivas que caen a CPU (fase 5).

Prueba: `gate_gpurdp` ALL OK (7 m 22 s), krom 371/371 identicas exacto a SoftRDP; gate_quick
krom + thar0 ALL OK.

### Fase 4 (parte 2) -- color images de 8 y 32 bits HECHO 2026-10-05

- `T_CISZ` en el registro; `storePixel`/`readFb` del shader siguen a `SoftRdp`: 32 bits
  RGBA8888 tal cual (direccion multiplo de 4), 8 bits el byte bajo (lectura replicada en los
  cuatro canales), 16 bits como antes. Zonas de RDRAM con los bytes por pixel del color image;
  el z sigue a 2 bytes por pixel.
- Triangulos FILL en 32 bits escriben el color de relleno crudo. En 8 bits SoftRDP los pinta
  con paso de 16 bits (otra zona), asi que esos y el COPY a 8 bits siguen en CPU.
- YUV de textura ya iba desde la fase 2 (`fetch` con SET_CONVERT). Triangulos en COPY siguen
  en CPU (raros: el hardware solo los define para texrect).

Prueba: `gate_gpurdp` ALL OK (7 m 29 s), krom 371/371 identicas exacto (las suites 32BPP
incluidas); gate_quick thar0 ALL OK.

### Fase 5 -- coherencia y rendimiento HECHA 2026-10-06

Medido (SM64, 600 intercambios, RX 570, i7-870 con PCIe 2.0): **57 s con GPU-RDP contra
13,5 s con SoftRDP**. Instrumentos nuevos: `KESTREL_GPUFLUSHSTAT=1` (por que se vacio la cola:
opcode, fin de `run()`, primitiva al CPU, cola llena) y la linea `[gpurdp] tiempo vaciados` al
salir (subir / grabar / enviar+esperar / bajar).

- Vaciados: 73,6 k, y 73,4 k de ellos son el fin de `run()`. No se pueden aplazar: la
  contabilidad (`nWrite`/`nZWrite` del shader -> `dpc_pipebusy`, `dpc_bufbusy`, `rdpGclk`)
  marca el ritmo del invitado, y aplazarla rompe el determinismo (md5 de sm64). Hay que
  abaratar cada vaciado, no quitarlos.
- Reparto: subir 0,66 s, grabar 1,65 s, **enviar+esperar 45,1 s**, bajar 0,55 s. Sin
  despachar nada, la espera es 12,8 s (~157 us de ida y vuelta por vaciado); el resto, ~32 s,
  es trabajo de GPU para ~6,5 primitivas por vaciado (~68 us por triangulo: absurdo para una
  RX 570).
- Descartado: atomicos por pixel al contador del triangulo. Una reduccion por subgrupo (un
  atomico por onda) dio el mismo tiempo.
- Hipotesis: **todos los buffers vivian en memoria del anfitrion** (HOST_VISIBLE |
  HOST_CACHED), y en una GPU discreta cada acceso del shader cruzaria el PCIe. **MEDIDA Y
  DESCARTADA**: gemelos DEVICE_LOCAL con subida y bajada por DMA (`vkCmdCopyBuffer`) en el
  mismo command buffer dan 37,2 s de espera contra 36,0 s del camino viejo. Ruido. Con el
  despacho por teselas ya PIERDE: pared 32,7 s con gemelos contra 28,9 s sin ellos (las
  marcas de tiempo dan 5,6 s de GPU en copias por 0,06 s del camino del anfitrion). De fabrica
  memoria del anfitrion; `KESTREL_GPURDP_LOCAL=1` = gemelos.
- Cambio 2: barrera solo entre unidades cuyas huellas de RDRAM se cruzan. Huella = caja de
  pixeles cubiertos sobre su imagen (color y z), columnas ensanchadas a grupos de 8 bytes
  alineados (palabra del espejo y palabra de bits ocultos nunca partidas, que es lo que hace
  falta para el relleno, que escribe palabras enteras); otra imagen = intervalo de bytes,
  conservador. La caja es la huella exacta aunque la zona que se sube sean filas enteras: el
  shader descarta todo pixel de fuera. Dos unidades disjuntas conmutan.
  `KESTREL_GPURDP_ALLBAR=1` = barrera entre todas. Medido por primitiva: 407 k -> 259 k
  barreras, espera 45,0 -> 37,2 s.
- **Cambio 3, el que cuenta: despacho por TRAMO con teselas.** Tramo = triangulos/texrects
  consecutivos con la misma color image y z image (y la union de color sin cruzarse con la
  union de z). Un solo despacho sobre la caja union; workgroup = tesela de 8x8; la tesela criba
  en memoria compartida que cajas la tocan (64 por pasada) y cada hilo recorre SU pixel por
  todas las primitivas del tramo en orden del FIFO. El orden por pixel es el de SoftRDP sin
  barrera ninguna entre primitivas. `KESTREL_GPURDP_NOBATCH=1` = un despacho por primitiva.
  SM64 600 intercambios: 478 k despachos -> **72 k tramos** (~1 por vaciado), 259 k -> **90
  barreras**, espera 37,2 -> **24,9 s**, pared 57 -> **37,4 s**. gate_gpurdp identico (krom
  371/371, sm64 `b5521b24`).
- Medida con marcas de tiempo de la GPU (`KESTREL_GPURDP_GPUTIME=1`, linea `[gpurdp] tiempo
  GPU` al salir): subida 1,4 s, despachos 7,9 s, bajada 4,3 s = 13,5 s de GPU; la espera es
  25,5 s, o sea **~12 s son ida y vuelta pura de envio + fence** (~165 us por vaciado, 73 k
  vaciados). Lo siguiente que mas pesa es el NUMERO de vaciados, no el shader.
- `KESTREL_GPURDP_SPIN=1`: sondeo activo del fence (`vkGetFenceStatus`) en vez de dormir en
  `vkWaitForFences`. ~1 s menos en 600 intercambios (28,9 -> 27,6 s) a costa de un nucleo
  quemado mientras la GPU trabaja; apagado de fabrica.
- Incidente: a las 16:25 el driver AMD cayo (`VK_ERROR_DEVICE_LOST`) durante un experimento
  con un shader que retornaba al instante, y el adaptador quedo en CM_PROB_FAILED_ADD (sin ICD
  de Vulkan para nadie, parallel-RDP incluido) hasta reiniciar. No repetir ese experimento.
  Ese dia hubo un corte de luz por tormenta a las 15:08, y la GPU funciono despues.
- Perfect Dark (PAL, intro en 3D): GPU-RDP ~2x mas rapido de pared que SoftRDP, mismo md5
  del framebuffer a 1200 y a 4000 campos VI. Entra en la puerta: `validate.py pd` (1200 campos,
  baseline `docs/baselines/pd.txt` = SoftRDP, que el GPU-RDP tiene que dar bit a bit; la ROM
  se copia a `out/pd/` sin partidas guardadas).

Cierre: `gate_gpurdp` ALL OK (7 m 00 s, con pd), krom 371/371 identicas a interp. Suelo que
queda: ~165 us de envio + fence por vaciado, y los vaciados los marca la contabilidad del
invitado (ver arriba). parallel-RDP no paga eso porque no devuelve la contabilidad por
primitiva.

### Fase 6 (parte 1) -- escalado interno HECHO 2026-10-06

`KESTREL_GPURDP_UPSCALE=2|4` (apagado de fabrica). El invitado sigue viendo su RDRAM de 1x
bit a bit: el pase de 1x se despacha igual que antes y es el que manda (contabilidad, md5).
Ademas, por cada tramo, un segundo despacho del mismo `tri.comp` con `sh = log2(S)` pinta en
copias de alta (`hram`/`hhid`: cada pixel de 1x es un bloque SxS) usando las mismas aristas,
coberturas y gradientes evaluados en el subpixel de alta. Sus barreras son las de 1x (la huella
de alta es la de 1x en bloques).

- Coherencia: antes de cada lote, lo que la RDRAM tiene distinto del espejo dentro de las zonas
  que el lote va a leer lo escribio otro (CPU, DMA); ese pixel se replica en su bloque de alta
  (`resyncHi`). Lo que pinto el RDP coincide con el espejo y conserva el detalle.
- Presentacion: `gpurdp::scanoutHi` arma la imagen de alta para el presentador (tope 2560x2048):
  fila igual a la del espejo = de la copia de alta; si no (la CPU la toco) = la de 1x replicada.
- Sin memoria para las copias (8 MB x S^2) sigue a 1x y lo dice.

Verificado: a 1x nada cambia (gate_gpurdp ALL OK 7 m 25 s: sm64 `b5521b24`, pd `a4e2cbcf`,
krom 371/371 identicas a interp); a x2/x4 SM64 y PD salen con bordes de alta, sin grietas.

## Riesgos

- **Rendimiento**: parallel-RDP lleva anos de ajuste. Meta realista: igualarlo en el juego
  mas pesado (PD) en la RX 570 de este anfitrion. Medir con `pdbench` desde la fase 3.
- **Coverage/AA**: es la frontera abierta tambien en SoftRDP (`AlphaCoverage` krom). El
  GPU-RDP la hereda; no es un objetivo de este gap.
- **Licencias**: parallel-RDP es MIT, se puede leer y citar, pero el codigo es propio y
  sin pegar trozos; si se adapta algo concreto, atribucion en `THIRD-PARTY.txt`.
