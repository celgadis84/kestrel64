# kestrel64 — huecos y pendientes (backlog de iteración)

Lista viva de **lo que falta**, con dos mitades:

1. **Funcionalidad que otros emuladores (ares, Project64, mupen64plus, simple64, CEN64)
   ya tienen y kestrel64 no.** Origen: pregunta del usuario del 2026-09-03. Cada punto
   está verificado contra el código, no supuesto.
2. **Pendientes técnicos propios** (rendimiento, fidelidad, robustez) que ya estaban en
   curso.

Orden = orden propuesto de ataque. Regla del proyecto: *no se descarta una optimización
por ser pequeña; se ordena por impacto, no se filtra.*

---

## Parte 1 — huecos frente a otros emuladores

### P0 — bloquean casos de uso normales

| # | Hueco | Estado real en el código | Por qué importa |
|---|-------|--------------------------|-----------------|
| ~~1~~ | ~~**Mandos 2, 3 y 4**~~ **CERRADO 2026-09-04** | `Memory::pifProcessJoybus` sirve los cuatro canales de mando con un `PadPort` por conector (conectado, accesorio, botones, stick, motor). `present.cpp` muestrea los cuatro: cada uno contra el teclado **o** un mando concreto de Windows (`rt::padDev`, guardado por NOMBRE porque Windows renumera). Telemetria `pad.set/pad.get` toman `pad` 1-4 y `status` saca los cuatro puertos. Dialogo nuevo con selector de mando 1-4. |
| ~~2~~ | ~~**PAL / región**~~ **CERRADO 2026-09-04** | `tvTypeForCountry` (`rom.cpp`) traduce el byte de pais a PAL/NTSC/MPAL y de ahi cuelgan las tres cosas que dependen de la norma: `osTvType` en RDRAM 0x300 y en s4 al entregar el control, la tasa de campo (`Clocks::viFieldHz`, 50 vs 59,94) que es el reloj de todo el emulador, y el reloj de video del que el AI saca la tasa del DAC (`Memory::aiVidClock`: 49,656530 MHz PAL / 48,628316 MPAL / 48,681812 NTSC). El decodificador de VI ya derivaba las 288 lineas activas de `viHalflines()`. `KESTREL_TVTYPE=pal|ntsc|mpal` fuerza la norma para una ROM que traiga mal la region. Esto cerro la pantalla negra de Perfect Dark PAL (`mainInit()` giraba para siempre al leer NTSC). |
| ~~3~~ | ~~**Rumble Pak**~~ **CERRADO 2026-09-04** | Accesorio por puerto: nada / Controller Pak / Rumble Pak. El Rumble Pak no tiene RAM: la ventana 0x8000-0x8FFF devuelve 0x80 (identificacion) y una escritura en 0xC000 mueve el motor; ranura vacia devuelve el CRC de datos invertido, que es como lo detecta el SDK. Salida fisica por XInput (`XInputSetState`, DLL cargada a mano) desde el hilo de video. |

### P1 — expectativas de un emulador «de usar»

| # | Hueco | Nota |
|---|-------|------|
| ~~4~~ | ~~**Trucos / GameShark**~~ **CERRADO 2026-09-04** | `src/core/cheats.{hpp,cpp}`: familias 80/81 (escritura cacheada por campo), A0/A1 (parche unico de arranque, sin cache), D0-D3 (condiciones) y el repetidor 50. El ritmo es el campo de video porque el aparato de verdad colgaba de la interrupcion del VI, y el nibble alto de la direccion decide si la escritura pasa por la D-cache (KSEG0) o va directa a la RDRAM (KSEG1). Fichero `.cht` al lado de la ROM o `KESTREL_CHEATS`, opcion en lanzador y menu. 88/89 (boton fisico del cartucho) y CC/DE/EE/FF (control interno) no se aplican y se avisa al cargar. Test propio `test/cheat_test.cpp`; detalle en `docs/CHEATS.md`. |
| ~~5~~ | ~~**ROMs comprimidas**~~ **CERRADO 2026-09-04** | `src/core/archive.{hpp,cpp}`: zip (metodos 0 y 8) y gzip, con DEFLATE propio (RFC 1951, ~200 lineas) en vez de zlib para que el `.exe` estatico siga sin dependencias. Se desempaqueta ANTES de normalizar el orden de bytes, se comprueba el CRC-32 de los dos formatos, y dentro del zip se elige la entrada con extension de ROM (o la mas grande). 7z y rar se rechazan con su nombre en vez de con "magic no reconocido". Los ficheros de al lado de la ROM (.eep/.sra/.fla/.mpk, .st0-.st9, .cht) ignoran la extension del contenedor, asi que `mario.z64.gz` y `mario.z64` comparten partida. Test `test/archive_test.cpp` y siete envoltorios de SM64 verificados extremo a extremo contra el md5 del oraculo; detalle en `docs/ROMS-COMPRIMIDAS.md`. |
| 6 | ~~**16:9 y escalado interno**~~ **CERRADO 2026-09-04** / **texturas HD** pendiente | `KESTREL_UPSCALE=1|2|4|8` pasa las banderas de escalado interno de parallel-rdp (`COMMAND_PROCESSOR_FLAG_UPSCALING_*`) al construir el `CommandProcessor`: SM64 pasa de 640x240 a 5120x1920 de scanout, y lo que el juego lee de su propia RDRAM sigue siendo 1x (los efectos que releen el framebuffer no se enteran). `KESTREL_SSAA=1` resuelve ese dominio ampliado promediando las NxN muestras al volcarlo a 1x. `KESTREL_ASPECT=4:3|16:9|estirar` (en caliente, `rt::aspectW/H`) decide el rectangulo destino en la ventana; 16:9 no ensancha el campo de vision -- eso solo lo hace el juego -- sino que estira la imagen anamorfica de los que traen modo panoramico propio. Con los valores de fabrica las banderas son 0 y los md5 no se mueven. Detalle en `docs/ESCALADO-Y-ASPECTO.md`. **Sigue pendiente**: texturas HD, que no es una opcion de video sino un canal de sustitucion de texturas (volcado + carga por hash de TMEM), y va aparte. |
| 7 | **Microcódigo gráfico HLE** | Todo es LLE (RSP real ejecutando F3DEX). Correcto y preciso, pero no hay ruta HLE, que es lo que da velocidad en máquinas flojas y lo que permite arreglar juegos con microcódigo raro. Decisión consciente; anotado por completitud. |
| 8 | **Multiplataforma** | Sólo Windows: `menu_win32.cpp`, `winsock2`, `waveOut`. GLFW y el núcleo son portables; el ruido está en UI, red y audio. |

### P2 — nicho

| # | Hueco |
|---|-------|
| 9 | 64DD (unidad de disco) |
| 10 | Netplay |
| ~~11~~ | ~~**Rewind (rebobinado)**~~ **CERRADO 2026-09-04** — `src/core/rewind.{hpp,cpp}`: una sola foto viva del estado (la maquinaria de `savestate.cpp`, ahora tambien en memoria via `captureState`/`restoreState`) mas una pila de **diferencias hacia atras**, que es la direccion en la que se rebobina y la barata (dos campos seguidos difieren en poco, no en los 8 MB). Tecla de retroceso mientras se mantenga apretada, `rewind.step` por telemetria (MCP `rewind_step`), `status` publica `rewind{steps,bytes,interval,budget}` y tres casillas en el lanzador. **Apagado de fabrica** porque cuesta: cada foto para el RCP, recorre el estado entero y lo compara — medido en SM64/200 intercambios, +45 % de pared con foto cada 2 campos y +24 % cada 6 (apagado = una comparacion contra cero por campo). Cinta de 256 MB ≈ 3400 pasos ≈ 2 min en la pantalla de titulo (~79 KB por paso). Comprobado con `test/rewind_test.cpp` (codec, casos limite + 1000 pares al azar + diferencia truncada rechazada) y extremo a extremo en SM64: campo 200 `ba62569b…` ≠ campo 240 `3cadb6c9…`, rebobinar 20 pasos vuelve a `ba62569b…` y volver a correr 40 campos da otra vez `3cadb6c9…` byte a byte. Detalle en `docs/REWIND.md`. |
| ~~12~~ | ~~**Herramientas TAS**~~ **CERRADO 2026-09-04** — `src/core/movie.hpp` (formato `.k64m`) graba y reproduce **lo que el juego lee**, no lo que aprieta el jugador: el enganche es `movie::sample()` en el comando 0x01 del joybus (`Memory::pifProcessJoybus`), con el mando del anfitrion y el inyectado por telemetria ya resueltos, que es la unica frontera que el invitado percibe. `KESTREL_MOVIE_REC` / `KESTREL_MOVIE_PLAY` y dos casillas en el lanzador. Reproducir una pelicula de otro cartucho se rechaza por CRC. Avance por fotogramas: `System::stepFields` (el cuanto es el CAMPO de video, que es donde el juego lee el mando), teclas `P` / `F` / `Shift+F` en la ventana y `frame.advance` por telemetria (herramienta MCP `frame_advance`); `emu.status` publica `movie{mode,polls,total,ended}`. Un estado guardado rebobina la cinta con el juego (seccion `MOVI`, version de estado 4→5). Comprobado: SM64 600 campos, control `109c2277…` vs grabacion con START `0ce65ed4…`, y la reproduccion da el md5 de la grabacion byte a byte con las mismas 471 muestras; pelicula de SM64 sobre DK64 rechazada; guardar en el sondeo 54, avanzar a 74 y cargar vuelve a 54. Detalle en `docs/TAS.md`. |
| 13 | Transfer Pak (Pokémon Stadium) |

### Lo que kestrel64 SÍ tiene y los demás no (para no perderlo de vista)

- RCP realmente enhebrado (RDP y RSP en hilos propios) con oráculo determinista en
  lockstep — ares es cooperativo de un hilo por diseño.
- Telemetría nativa + puente MCP: depurador completo por socket (memoria coherente por
  D-caché, capturas de framebuffer, perfilador de rutas calientes de CPU y RSP,
  puntos de ruptura, inyección de mando).
- Sobrefrecuencia por dominio (`overclock.cpu/rdram/rsp`).
- Estados guardados ya existen (`src/core/savestate.*`, `state.save`/`state.load`,
  ranuras 0..9, teclas F5/F7/F6). **No** es un hueco.

---

## Parte 2 — pendientes técnicos propios

### Robustez

- ~~**Endurecer el camino `fastmem` del JIT**~~ **CERRADO 2026-09-04**: una direccion de invitado salvaje debe dar excepcion de invitado, no una violacion de acceso del anfitrion. El camino rapido del JIT (`src/cpu/jit.cpp`) ya llevaba las guardas -- canonica de ckseg0, alineacion, modo kernel, `jitRdramSz`, guardia de escritura y etiqueta+valido de la linea de D-cache -- y los caminos lentos (`dcFill`/`dcFlush`/`dcMiss`, `spDma`) ya acotaban contra `rdram.size()`; lo que faltaba era la PRUEBA de que cada guarda se dispara y de que el dynarec no se separa del interprete al hacerlo. Nuevo `test/wildmem_test.cpp` (objetivo `wildmem_test`, dentro de `gate_all.sh`): 27 programas de invitado con direcciones imposibles -- desalineadas (`lw`/`lh`/`sw`/`ld`/`sd`/`lwc1`/`swc1`), pasadas del final de la RDRAM (incluida una `ld` a caballo del limite y la cima de ckseg0), kuseg y ksseg sin TLB, huecos de MMIO en kseg1, punteros de 64 bits NO canonicos (`0x12345678_80000000`, y ckseg0 sin extender signo) y los mismos limites con RDRAM de 4 MB. Cada caso corre dos veces sobre CPUs recien reseteadas, una a pasos del interprete y otra despachada por `jitTryBlock()`, y se comparan ExcCode, EPC, BadVAddr y el registro destino; ademas se exige el valor absoluto donde la semantica del VR4300 no admite discusion (AdEL=4 / AdES=5 con la VA salvaje entera en BadVAddr, TLBL=2 / TLBS=3 en los segmentos mapeados). Resultado: el dynarec corrio en los 27 (ninguno declinado) y coincide con el interprete en todos; ningun acceso salvaje toca al anfitrion.
- ~~**`rspAwaitIdle()`** sin tope de espera~~ **CERRADO 2026-09-04**: las tres esperas del hilo de CPU sobre los workers (`rspAwaitIdle`, la de `rspSubmitKick` y `rdpDrain`) iban a `cv.wait` pelado, asi que un worker que dejara de publicar congelaba el emulador para siempre y **sin decir nada** -- ventana viva, juego quieto, indistinguible de lentitud. No se puede abandonar la espera (seguir sin el resultado corrompe el estado del invitado, y el hardware tampoco se cansa), asi que el tope no la rompe sino que la PARTE: `awaitReporting` espera en rondas de `KESTREL_RCPWAIT` ms (2000 por defecto, `=0` vuelve a la espera muda) y en cada vuelta imprime el estado del dominio -- del RSP ocupado/kick/corriendo, `sp_status`, `sp_pc`, PC de IMEM, ciclos publicados y tipo de OSTask; del RDP cola, ocupado y `dpc start/cur/end/status`. Con eso un cuelgue se lee de un vistazo: si PC y ciclos avanzan es una tarea larga, si estan clavados es el cuelgue de verdad.
- ~~`test/rsp_test.cpp:71` no compila~~ **CERRADO 2026-09-04**: era `sp_status` convertido en
  `std::atomic` cuando el RSP paso a hilo propio (copia implicita borrada) mas que el test
  esperaba que escribir CLEAR_HALT ejecutara el microcodigo, cuando en Lockstep esa escritura
  solo ARMA el nucleo y quien lo avanza es `System::run`. Se arreglaron los dos, y de paso
  `save_test`, que llevaba roto desde que el Controller Pak se mudo a `padPort[i]`. Los cuatro
  tests unitarios (`rsp_test`, `save_test`, `cheat_test`, `archive_test`) los compila y ejecuta
  ahora `gate_all.sh` antes de nada: se pudrieron porque ningun gate los miraba.
- ~~**Modo genuino de 4 MB sin Expansion Pak**~~ **CERRADO 2026-09-04** — `Memory::reset`
  ya sabia dimensionar la RDRAM a 4 u 8 MB y `CPU::fastBoot` ya escribe el tamano real en
  `osMemSize` (RDRAM 0x318 y 0x3F0), pero `System::init` llamaba siempre con
  `expansionPak=true`: no habia manera de arrancar una consola de serie. Ahora hay
  `KESTREL_RDRAM=4|8` (opcion "Memoria RDRAM" del lanzador) y el arranque lo dice en el log.
  Comprobado con el invitado, no con el emulador: SM64 a 4 MB da el md5 de referencia
  intacto (`466282775dbd0ac084946558a1c30771`) y Donkey Kong 64 a 4 MB dibuja su pantalla
  "N64 EXPANSION PAK NOT INSTALLED" en vez del juego. Los savestates ya rechazaban un
  tamano distinto en la cabecera, asi que no hay manera de mezclar partidas de 4 y 8 MB.

### Fidelidad

- ~~**Con parallel-RDP el RDP no le cuesta NADA al invitado**~~ **CERRADO 2026-09-09.**
  El reloj de coste del RDP (`rcp.rdpGclk`, el que frena a la CPU en `Memory::rdpPace`) y
  `DPC_CLOCK` solo los alimentaba `SoftRdp::accountPixels`. Con la GPU activa, `rdpRunJob`
  entrega el tramo a `vrdp::runFifo` y **vuelve antes de pasar por ahi**, asi que el
  rasterizado salia gratis: `DPC_CLOCK` clavado a **0** -- y es un registro que los juegos
  LEEN para medirse -- y el freno del dominio RDP no frenaba nunca. Medido en DK64 con la
  demo de atraccion en marcha.

  El arreglo (`Memory::rdpCostPass` + `SoftRdp::costOnly`) pasa el MISMO tramo de FIFO por
  el decodificador de SoftRdp en modo solo-coste: recorre comandos, mantiene el estado
  (scissor, modos, imagenes, tiles) y cobra los pixeles que entran al pipeline, **sin
  escribir un byte de RDRAM** -- los pixeles buenos siguen siendo los de la GPU. El bucle
  por pixel no se ejecuta: cada tramo de scanline aporta su anchura de una vez, con los
  mismos `xs`/`xe` que el rasterizado, asi que la cuenta de pixeles es identica y el coste
  del paseo es O(altura) por primitiva en vez de O(area). Los pixeles se cobran como
  escritos porque sin z-buffer fiable en RDRAM (lo tiene la GPU) no se sabe cuales moririan
  en el test, y el hardware paga casi lo mismo por uno muerto que por uno escrito.
  `KESTREL_RDPCOST=0` lo apaga para el A/B.

  Resultado en DK64 bajo parallel-RDP: `DPC_CLOCK` pasa de 0 a **192.586 GCLK por campo**,
  o sea el RDP ocupado un **18,5 %** del campo (1.042.709 GCLK disponibles a 62,5 MHz y
  59,94 Hz). Ese 18,5 % explica de paso por que el juego NO pierde fotogramas ahi: en esa
  escena el RDP no es el palo largo.

- **SoftRDP saca mal la geometria texturada de libdragon; parallel-RDP la saca perfecta**
  (repro limpio y determinista, 2026-09-09). `test_roms/homebrew/junkrunner64.z64`,
  `KESTREL_MAXFLIPS=400 KESTREL_FBDUMP=...`, sin ventana, mismo campo en los dos:

  | punto del cuadro | parallel-RDP | SoftRDP |
  |---|---|---|
  | suelo del canon | (255, 99, 24) | (90, 8, 0) |
  | caja de la izquierda | (74, 33, 16) | (0, 0, 0) |
  | pared del canon | (99, 41, 57) | (66, 90, 107) |
  | cuadro del menu (plano, sin textura) | (255, 255, 255) | (255, 255, 255) |

  El menu plano y los rectangulos salen IDENTICOS: lo que falla es el camino de textura sobre
  triangulo. La geometria texturada sale oscura (~0,35x) o directamente negra, con el tono
  cambiado, y el logo del titulo destrozado. Sospecha principal: SoftRDP **no hace mipmapping
  ni seleccion de tile por LOD** (`src/rdp/rdp.cpp:1665`, "unused while we do not mipmap"), y
  el `rdpq` de libdragon si los usa; con dos ciclos y TEX1 la muestra del nivel que falta se
  cuela como negro y arrastra el combinador. Confirma lo que ya dijo el usuario ("el softrdp
  siempre salio mal, no es de hoy") y ahora con un caso reproducible en lote y un oraculo al
  lado. NO se persigue por ahora: la direccion es parallel-RDP, y el exe principal de `dist/`
  ya es el de PRDP (`kestrel64-soft.exe` es el respaldo para maquinas sin GPU).

- **El stick del mando no tenia PUERTA OCTOGONAL: la diagonal entregaba un valor que la
  consola no puede producir** — **ARREGLADO 2026-09-09** (`padOctagon`, `src/video/present.cpp`).
  El tope del stick de la N64 es fisico: el anillo tiene ocho lados, ~85 en los cuatro ejes
  y ~69 en las cuatro diagonales. Kestrel recortaba por EJES a +-80, o sea una puerta
  CUADRADA: teclado en diagonal daba (80,80), modulo 113, y un mando moderno de recorrido
  cuadrado lo mismo. Ningun mando de N64 pasa de ~85 de modulo. Los juegos que sacan la
  velocidad de andar del modulo del stick (Mario, DK64, Zelda) corrian en diagonal un 33%
  mas rapido de lo que jamas corrieron en hardware. Ahora el recorte va a lo largo del rayo
  (borde del octante `u + v*(C-D)/D = C`, con `C=85`, `D=69`, `u=max(|x|,|y|)`), que es lo
  que hace el anillo de plastico: se respeta la DIRECCION y solo se acorta el alcance.
  De paso la zona muerta del mando del anfitrion pasa de ser por ejes a ser RADIAL y con
  reescalado desde su borde: la de ejes recortaba un cuadrado y dejaba colar la esquina (un
  stick con deriva en las dos direcciones entregaba una diagonal), y sin reescalar el valor
  saltaba de 0 a 17 al cruzar el umbral. Calibracion del octogono contrastada con ares
  (`controller/gamepad/gamepad.cpp:372`), que usa los mismos 85/69.

- **El RDP no cuesta tiempo de INVITADO, cuesta tiempo de PARED: en enhebrado el juego
  perdia la mitad de los cuadros con SoftRDP** — **ARREGLADO 2026-09-09** (`Memory::rdpPace`),
  medido 2026-09-09. El worker del
  RDP tarda lo que tarda el rasterizador del anfitrion, y mientras tanto la CPU emulada
  sigue retirando instrucciones en su bucle de espera. Como el reloj de video del invitado
  se deriva de las instrucciones retiradas (`Memory::viTick(cpu.retired)`), esas vueltas de
  espera se cobran como TIEMPO DEL JUEGO: pasan campos de video sin que salga un cuadro.
  Medido en Donkey Kong 64, escena del rap, 3000 intercambios de buffer:

  | modo | campos VI | fps del invitado | instrucciones |
  |---|---|---|---|
  | lockstep-jit (SoftRDP sincrono) | 6153 | 29.2 | 6881 M |
  | `KESTREL_SYNCRDP=1` (solo el RDP sincrono) | 6153 | 29.2 | 6881 M |
  | enhebrado-jit + SoftRDP (por defecto) | 10830 | **18.0** | 12112 M |
  | enhebrado-jit + parallel-RDP | 6161 | 29.2 | 6890 M |

  O sea: el invitado ejecuta el DOBLE de instrucciones para dibujar los mismos 3000 cuadros,
  y las de mas se las come esperando. `KESTREL_SYNCRSP=1` no cambia nada: el RSP y su
  regulador (`rcpPace`) son inocentes, el RSP ya esta acoplado al reloj del invitado. El RDP
  no tiene regulador ninguno.

  Consecuencias, por orden de importancia:
  1. **Un juego que cuenta cuadros se desincroniza.** Es la via por la que la demo de
     apertura de DK64 se rompe con el exe de SoftRDP (`kestrel64-soft.exe`).
  2. **La velocidad del invitado depende del anfitrion.** Un PC lento no solo va lento: el
     JUEGO va a otra velocidad. Eso el hardware no lo hace.
  3. **`bench` sobre enhebrado+SoftRDP mide de menos.** Fija campos de video, no cuadros; si
     los campos se inflan, el reloj de pared sale bueno haciendo menos trabajo.

  Con parallel-RDP no se notaba porque la GPU acaba antes de que la espera cueste.

  **Arreglo aplicado**: `Memory::rdpPace`, gemelo del freno del RSP. Mientras hay trabajo de
  RDP en vuelo, la CPU no puede haber retirado mas de `paceCpuNum/paceCpuDen` instrucciones
  por cada GCLK que el RDP ha consumido DE VERDAD, medido con el modelo de coste ya calibrado
  contra hardware que alimenta DPC_CLOCK (`SoftRdp::accountPixels`/`accountTmem`), publicado
  en una copia monotona `rcp.rdpGclk` que el invitado no puede poner a cero (el juego si puede
  borrar DPC_CLOCK con los bits 6..9 de DPC_STATUS). Frenar no es esperar: el hilo de CPU
  duerme en tiempo de PARED y no retira instrucciones, o sea que el tiempo del invitado se
  para y el RDP cuesta lo que dice su modelo, corra el anfitrion lo que corra. Cuando el RDP
  va sobrado (GPU) el freno no muerde y el solape se conserva. Medido despues, mismos 3000
  intercambios de DK64: enhebrado-jit + SoftRDP **6174** campos (antes 10830) contra 6153 del
  oraculo lockstep y 6161 de parallel-RDP. Salvavidas identico al del RSP (`kPaceMaxWait`).

  **Lo que sigue abierto**: el RDP todavia no ENTREGA `DP_DONE` en un plazo del reloj del
  invitado, solo impide que la CPU adelante al trabajo ya hecho. Con un anfitrion muy rapido
  el juego ve el RDP como instantaneo (que es lo que hace el oraculo lockstep desde siempre).
  Cerrarlo del todo pide el planificador por marcas de tiempo que ya pide el punto de las DMA,
  y para parallel-RDP ademas un estimador de coste sobre el flujo de comandos, porque la GPU
  no alimenta `accountPixels`.

- **SoftRDP dibuja contorno claro en cada arista en juegos reales (DK64)** — abierto,
  NO es regresion de la cobertura de 2026-09-08. Capturas del RDRAM crudo (sin filtro VI)
  de la intro de Donkey Kong 64: paraLLEl-RDP la saca correcta, SoftRDP saca un reborde
  blanco/discontinuo en TODAS las aristas de triangulo. `KESTREL_NOAA=1` no lo quita, o sea
  que no lo manda `aaEn`. El usuario confirma que el SoftRDP «siempre salio mal» en este
  juego, y el ejecutable con el que lo vio (dist/, 2026-09-08 19:00) es anterior al trabajo
  de cobertura, asi que el defecto es viejo.

  Pistas para cuando se retome: el reborde aparece donde el poligono TERMINA, no donde
  empieza, lo que apunta al ultimo span de cada arista (redondeo de `xleft/xright` o de la
  fraccion de la sub-linea) mas que al combinador. Reproducir: `KESTREL_PRDP=0` + DK64,
  capturar por MCP (`capture_framebuffer` lee RDRAM, no pasa por el VI) y comparar contra la
  misma escena con `KESTREL_PRDP=1`.

- **Frontera AA/cobertura del SoftRDP** — el único frente de precisión grande que queda
  (subpíxel, 4 sub-líneas por píxel según el VHDL de MiSTer). Suite krom: media exacta
  ~89, media sólo-estáticos ~96,5.

  **Avance 2026-09-08 — el patrón de tomas ya es el del hardware, y lo que NO era el hueco.**
  Las ocho tomas de cobertura no están en las mismas dos columnas: el RDP las **escalona**
  fila a fila. En octavos de píxel, las filas pares muestrean en 0 y 4, las impares en 2 y 6.
  Dos oráculos independientes coinciden byte a byte: `compute_coverage()` de parallel-rdp
  (`xshift = u16x4(0,4,2,6) + (x << 3)`, contra `xleft.xxyy` y luego `xleft.zzww`, con pesos
  (1,2,4,8) y (16,32,64,128)) y `raster_coverage.c` de **PixelMechanic0/softrdp** (MIT,
  clonado en `../softrdp-ref`), que además documenta el reparto de bits y el uso del bit más
  bajo del mask como corrección de centroide. Nosotros teníamos (0,25 / 0,75) en las cuatro
  filas. Corregido.

  Lo importante es lo que la medida dijo **que no** era el hueco. Con el escalonado puesto y
  nada más, la suite krom filtrada a triángulos da **0 cambios en los 46 tests**: el patrón
  solo redistribuye `cvg`, y `cvg` únicamente se usa cuando el juego pide antialias —
  `coverPixel()` se desvía a `blendPixel()` si `!aaEn`. O sea que el patrón estaba mal desde
  siempre y no costaba ni una décima, porque los tests de triángulo de krom van con AA
  apagado.

  Y una vía cerrada, con número: intentar aplicar aquí las otras dos reglas del hardware
  — cobertura cero = el píxel no existe, y con AA apagado decide **una sola** toma
  (`if (!aa_enable && (coverage & 1) == 0) return false;`) — **empeora**, porque ya las
  aplicamos: el recorte del tramo (`xs = ceil(xLeft)`, `xe = ceil(xRight)`, con los bordes
  evaluados en la línea) ES esa regla. Aplicarla dos veces, la segunda con la toma de la
  sub-scanline 0, bajó `RSPPlotTriangle` de **100,000 a 99,740** (perdiendo el único
  "perfecto" del lote) y `Cycle1FillZBufferTriangle` de 98,150 a 97,950. Revertido.

  SM64 **sí** se movió, y ahí está la prueba de que el cambio es el bueno: su md5 de 60
  campos pasó de `466282775dbd0ac084946558a1c30771` a `195a464c80e6216af78121fd24242f3e`,
  **el mismo en los cuatro modos** (threaded, threaded-jit, rspinterp, rspnolink), o sea
  determinista y con JIT == intérprete. Se aisló la causa dejando la tabla de tomas uniforme
  (0,25 / 0,75 en las cuatro filas) y nada más: eso reproduce el md5 viejo **exacto**, luego
  el único causante es el escalonado. Y el diff de los dos vuelcos dice que es graduado de
  borde y no corrupción: 3.768 píxeles (4,9 % del cuadro), delta medio 24,8 y la moda en 8
  (= un LSB de un canal de RGB565), con **gradiente horizontal medio 50,1 en los píxeles
  cambiados frente a 7,7 en el resto** -- caen sobre los bordes, que es exactamente donde
  vive `cvg`. SM64 dibuja con AA encendido; krom no. Baseline de SM64 re-bendecida.

  Conclusión para quien siga: el hueco que queda **no** está en dónde se toman las muestras
  ni en quién existe, sino en el camino de AA encendido — cómo `cvg` entra al blender, los
  cuatro modos de `blend_coverage` (CLAMP/WRAP/ZAP/SAVE, ver `coverage.h` de parallel-rdp),
  el desbordamiento `coverage + memory_coverage >= 8` y la cobertura guardada en el bit bajo
  del píxel de 16 bits. Eso es lo que mide `AlphaCoverage` (21 %), y es donde hay que ir.

  **HECHO 2026-09-08 — el camino de AA encendido, entero.** Detalle en
  `docs/RDP-COVERAGE-AA.md`. Dos mitades, porque el AA del N64 no lo hace el RDP solo:

  1. *El RDP guarda cobertura, no alfa.* `Memory::rdramHidden` modela el noveno bit de los
     chips RDRAM (los 2 bits bajos de `cvg`; el alto es el bit 0 del píxel de 16 bits), y
     `blendPixel` hace la cadena completa: alfa expandido del combinador, `CVG_TIMES_ALPHA`,
     `ALPHA_CVG_SELECT`, descarte con `cvg == 0`, `overflow`, `COLOR_ON_CVG`, y los cuatro
     modos de `CVG_DEST`. FILL/COPY escriben cobertura 7 o 0 según el bit 0. El plano viaja
     en la foto de estado (versión 8).
  2. *El VI es quien suaviza el borde.* `src/video/vifilter.cpp`: filtro de AA
     (`vi_fetch.frag`), de-dither y divot (`vi_divot.frag`), con la puerta real de VI_CTRL
     (`AA_MODE` bits 9:8 < 2, divot bit 4, de-dither bit 16). Va en el **presentador**; el
     volcado determinista sigue siendo el framebuffer crudo porque las referencias de krom
     son capturas del framebuffer, no del barrido (`KESTREL_VIFILTER=1` para el A/B; medido:
     mete 6 regresiones y baja la media a 88,69).

  Resultado en la suite: `AlphaCoverage` **72,28 → 74,76**, media 88,72 → **88,73**, cero
  regresiones. Lo que queda de este frente es sub-píxel fino, no estructura.
- **Modelo de CPI = fase propia, la mayor de fidelidad de temporizacion** (medido 2026-09-04
  a raiz de que el usuario viera a Donkey Kong fallar la liana en la intro de DK64). El
  emulador ata «instruccion retirada» a «tick de COP0 Count», y en el VR4300 Count avanza a
  medio reloj, asi que el modelo entero queda clavado en **CPI 2**: 93,75 MHz / 2 / 59,94 =
  **782 k instrucciones por campo**. El VR4300 real retira mucho mas que eso (CPI efectivo
  ~1,2-1,4 en codigo de juego, o sea ~1,1-1,3 M instrucciones por campo), asi que a los juegos
  les falta ~35 % de CPU por cuadro y pierden el fotograma. DK64 gasta 2,4-3,0 M instrucciones
  por cuadro en la intro; en 2 campos solo le caben 1,56 M.

  Medido en DK64 (ventana de intercambios 400→600, SoftRDP threaded-jit), campos VI por
  intercambio (2,00 = 30 fps):

  | Reloj | campos/intercambio |
  |---|---|
  | x1,0 (CPI 2, actual) | 3,46 |
  | `KESTREL_OC_CPU=1,35` (≈CPI 1,5) | 2,98 |
  | `KESTREL_OC_CPU=1,6` (≈CPI 1,25) | 2,80 |
  | `KESTREL_OC_RSP=1,5` (CPU sin tocar) | 3,49 = **sin efecto** |

  O sea: el limitante es el presupuesto de instrucciones de CPU por campo, no el RSP. Sintoma
  visible: animaciones que se desincronizan de la logica (la liana de DK64), y sensacion de
  lentitud general.

  Por que NO es un cambio corto: hay que separar «ciclo» de «instruccion retirada» (Count debe
  avanzar CPI/2 por instruccion, o sea ~0,7 y no 1), y de eso cuelga medio dynarec — adelanto
  de Count por bloque al salir, la frontera Count==Compare que decide el troceado, la lectura
  de Count a mitad de bloque, y el estado guardado. Ademas mueve TODAS las baselines (md5 de
  sm64 a 60 campos incluido) y hay que ver que dicen los tests Timing/Cycle de n64-systemtest,
  que son el oraculo. Pendiente antes de tocar nada: medir que fraccion de esas 2,4-3,0 M
  instrucciones por cuadro es espera ociosa (`osRecvMesg`) y no trabajo real, porque eso
  cambia el CPI que hay que elegir.

  **Medido 2026-09-04 (ese paso previo, hecho).** El giro ocioso de libultra en DK64 son dos
  instrucciones en `0x8000094c` (`beq zero,zero,8000094c` + `nop`), o sea dos cubos del
  perfilador; el perfilador cuenta en el *fetch* del interprete, asi que la medida va con
  `KESTREL_JIT=0` (con dynarec solo veria los bloques que el JIT declina = muestra sesgada).
  Barrido de la partida entera, tramos de ~50 intercambios de buffer:

  | tramo (intercambios) | campos/intercambio | instr/campo | ocio % | trabajo/campo |
  |---|---|---|---|---|
  | 0-56 | 6,16 | 783 874 | 45,5 | 427 149 |
  | 56-111 | 1,84 | 777 193 | 85,2 | 115 254 |
  | 180-245 | 1,65 | 784 773 | 85,7 | 111 981 |
  | 378-448 | 2,36 | 782 921 | 55,6 | 347 816 |
  | 768-873 | 2,00 | 782 032 | 61,5 | 301 201 |
  | 1205-1300 | 1,98 | 782 292 | 64,1 | 280 802 |

  Dos cosas, y una corrige lo de arriba:

  1. `instr/campo` sale 782 k en TODOS los tramos. No es que el juego gaste eso: es que el
     modelo no deja gastar mas. La identidad 93,75 MHz / 2 / 59,94 = 782 032 se ve al digito.
  2. **El ocio no baja del 45 % en ningun tramo.** En regimen (2,00 campos/intercambio) la CPU
     gira ociosa el 61-65 %: el trabajo real es 0,55-0,82 M instrucciones por cuadro MOSTRADO
     contra 1,56 M que caben en dos campos, o sea **2-3x de margen**. Ahi el presupuesto no
     limita nada y el juego va a 30 fps porque pide 30. La unica fase que si lo agota es el
     arranque/intro (6,16 campos/intercambio): 6,16 x 427 149 = **2,63 M de trabajo por cuadro**
     contra 1,56 M disponibles, y de ahi salen los 3,4 campos minimos que el modelo impone.

  Tambien se descarto de camino la sospecha de que el reparto lo decidiera el reloj del
  anfitrion (la regulacion CPU<->RCP) en vez del invitado: mismo punto del juego con
  interprete y con dynarec da lo mismo -- 3,97 vs 4,30 campos/intercambio en el arranque y
  2,00 vs 2,00 en regimen. Es del invitado.

  O sea: el cambio de CPI hay que justificarlo con una escena PESADA, no con la media de una
  partida, y la cifra que decide es **trabajo por cuadro mostrado**, no instrucciones por
  campo. Para que la intro de DK64 quepa en dos campos harian falta 1,31 M instrucciones por
  campo = CPI ~1,19, que cae justo en el rango real del VR4300 (1,2-1,4) y por tanto no es un
  numero inventado para que salga bien.

  **Segunda medida, Perfect Dark (PAL), misma herramienta.** Aqui el ocio se busca solo (el
  auto-salto de libultra cae en `0x80001938`, no donde DK64) y la norma es otra, o sea que es
  una comprobacion independiente y no una repeticion:

  | tramo | campos/intercambio | instr/campo | ocio % | trabajo/cuadro |
  |---|---|---|---|---|
  | 3781-3851 | 1,00 | 933 303 | 32,4 | 631 155 |
  | 3940-4144 | 1,13 | 937 488 | 17,9 | 867 691 |
  | 4144-4267 | **2,89** | 938 643 | **4,5** | **2 587 974** |
  | 4267-4413 | 1,00 | 938 692 | 13,8 | 809 482 |

  `instr/campo` = 937 488 = 93,75 MHz / 2 / **50** al digito: la identidad se cumple igual en
  PAL. Y el tramo pesado es el caso que faltaba -- ocio del 4,5 %, o sea la CPU no esperando
  sino ahogada, con 2,59 M de trabajo por cuadro contra 1,87 M que caben en dos campos PAL.

  Los dos juegos convergen sin que se les haya impuesto nada:

  | juego | trabajo/cuadro | instr/campo necesarias | CPI implicito |
  |---|---|---|---|
  | DK64 (NTSC) | 2,63 M | 1,31 M | **1,19** |
  | Perfect Dark (PAL) | 2,59 M | 1,29 M | **1,45** |

  Los dos caen dentro del CPI efectivo real del VR4300 (~1,2-1,4). Con dos normas de TV y dos
  juegos distintos dando lo mismo, el CPI 2 del modelo queda como lo que es: pesimista de
  verdad, no una eleccion conservadora defendible.

  **Aviso sobre la cifra de trabajo**: solo se descuenta el giro del hilo ocioso de libultra.
  Cualquier otra espera activa (una bandera sondeada, un `osRecvMesg` que gire) cuenta como
  trabajo, asi que 2,6 M es una COTA SUPERIOR y el CPI que hace falta podria ser mayor (menos
  agresivo) que 1,19-1,45. Ningun paso siguiente debe apoyarse en esa cifra como si fuera exacta.

  **Camino seguro para tocarlo -- HECHO 2026-09-04 (el factor ya existe; el defecto NO se ha
  movido).** `KESTREL_CPI` = ciclos de CPU por instruccion. Por dentro es un entero,
  `cpi256 = 128 * CPI` = ticks de Count x256 por instruccion retirada, con un resto acumulado
  (`countFrac`) para que la division no pierda nada: 256 = un tick por op = exactamente lo de
  siempre. Sin variable de entorno nada cambia, ni un bit.

  Por que en 1/256 y no en coma flotante: el reloj del invitado tiene que ser reproducible
  entre maquinas y entre intérprete y JIT. Un acumulador entero con arrastre da la misma
  secuencia de ticks en cualquier sitio; un `double` no lo garantiza.

  Por que se acota a `cpi256 <= 256` (CPI >= 2 no se admite): TODAS las guardas de borde de
  timer del JIT comparan la distancia a Compare contra el numero de **ops** del bloque
  (`if((u32)(cmp - cnt) <= K) return 0;`). Mientras `ticks(ops) <= ops`, esas guardas siguen
  siendo conservadoras sin tocarlas: como mucho devuelven el control de mas (coste), nunca de
  menos (bug). Con un factor > 1 tick/op habria que reescribirlas todas, y una sola olvidada
  es una interrupcion de temporizador perdida — silenciosa y practicamente imposible de
  bisecar. Por eso el limite es duro y esta puesto en el parser.

  Sitios que avanzan Count, todos pasados por `countTicks()`: intérprete (`cpu.cpp`), commit
  diferido de la cadena del JIT, salida de bloque, commit de eslabones pendientes y el arnes
  de brdiff. El `countFrac` es estado de invitado: entra en el savestate (version 6) porque
  cargar un estado con la fase del divisor equivocada desvia el timer un tick, y se guarda y
  restaura tambien en los arneses de diff, que corren el bloque y el intérprete sobre el mismo
  punto de partida.

  Lo unico que hubo que rendir: el atajo de `mfc0 Count` **dentro** del bloque (jit.cpp:780)
  emitia "valor de entrada + idx ops ya retiradas". Eso solo es exacto con un tick por op; con
  cualquier otro factor los ticks de esas `idx` ops dependen del resto acumulado, que vive en
  el CPU y no en el bloque. Con el factor movido, esa op cede al intérprete en vez de emitir
  una aproximacion.

  **Que dice el oraculo** (medido, no supuesto): n64-systemtest pasa **igual** con el defecto,
  con CPI 1,5 y con CPI 1,25, en `threaded` y en `threaded-jit` — `Base 0/3721 Timing 0/2
  Cycle 0/6` en los seis casos. Y el md5 de SM64 a 60 campos es el mismo con CPI 1,25 que con
  el defecto, y el mismo en intérprete que en JIT. Conclusiones, en orden:
    1. los tests Timing/Cycle **no** estan atados a la relacion instruccion-Count actual, asi
       que no bloquean el cambio... pero tampoco lo deciden: no distinguen 2 de 1,25, luego no
       sirven como oraculo para elegir el valor.
    2. intérprete y JIT siguen siendo bit a bit iguales con el factor movido, que era la parte
       arriesgada.
  **Lo que NO escala con el factor, a proposito**: `Random` (baja un paso por instruccion
  retirada, no por ciclo) y el reloj de decaimiento del pestillo de escritura del PI
  (`cartClock`, tambien en instrucciones). En hardware los dos van por ciclo, luego con CPI<2
  irian mas deprisa que ahora; pero el comportamiento actual es justo el que fijan los tests
  de TLB/Random de n64-systemtest, que hoy pasan 3721/3721. Moverlos es otro cambio, con su
  propia evidencia, y no se mezcla con este.

  **Medido en DK64 con el factor movido (2026-09-04, mismo binario, intérprete, tramos de 60
  intercambios).** Instrucciones por campo: 782 k con el defecto, 1 250 k con CPI 1,25,
  1 118 k con CPI 1,4 -- o sea el presupuesto SI se mueve, que era lo que faltaba cablear.

  | tramo | CPI 2 campos/int | CPI 1,4 | CPI 1,25 | CPI 2 ocio% | CPI 1,4 | CPI 1,25 |
  |---|---|---|---|---|---|---|
  | arranque/intro | **5,52** | 2,99 | **2,94** | 47,5 | 69,8 | 69,5 |
  | juego temprano | 1,75 | 1,86 | 1,65 | 85,5 | 82,9 | 91,0 |
  | ... | 1,69 | 2,16 | 2,09 | 76,9 | 70,3 | 71,0 |
  | estable | 2,01 | 2,00 | 1,99 | 66,5 | 74,7 | 78,1 |
  | estable | 1,97 | 2,02 | 2,00 | 62,0 | 73,1 | 76,1 |

  Dos lecturas, y la segunda importa mas que la primera:
    - donde el juego estaba **saturado** (el arranque: 5,52 campos por cuadro mostrado, o sea
      un cuadro cada 92 ms) el cuadro pasa a 2,94-2,99, casi el doble de fluido. Ahi el
      presupuesto SI era el limitante, tal y como decia la medida de trabajo/cuadro.
    - en regimen **estable** los campos por intercambio se quedan clavados en ~2,00 con los
      tres factores, y el trabajo por campo converge a ~298 k en los tres. Lo unico que sube
      es el ocio (62 % -> 76 %). Es decir: el juego ya iba a sus 30 fps de diseno y el
      presupuesto extra se lo come el giro ocioso, exactamente como en la consola. Bajar el
      CPI **no acelera** lo que no estaba limitado, que es la prueba de que no es un truco de
      velocidad sino un cambio de unidad.

  **Y en Perfect Dark (PAL, secuencia de atraccion, sin salto de nivel -- el mapa de warp es
  del build ntsc-final y esta ROM es PAL, ver `docs/PD-GAMEPLAY.md`).** Instrucciones por
  campo 937 k con el defecto, 1 341 k con CPI 1,4 (= 937 488 x 2/1,4, la identidad cuadra).

  | tramo | CPI 2 campos/int | CPI 1,4 | CPI 2 ocio% | CPI 1,4 | CPI 2 trabajo/int | CPI 1,4 |
  |---|---|---|---|---|---|---|
  | arranque | 5,55 | 6,94 | 62,0 | 70,1 | 1,98 M | 2,79 M |
  | transicion | 1,58 | 1,49 | 79,2 | 84,1 | 308 k | 318 k |
  | atraccion | 1,00 | 1,00 | 91,2 | 93,8 | 83 k | 82 k |
  | atraccion | 1,00 | 1,00 | 90,9 | 93,5 | 85 k | 87 k |
  | atraccion | 1,00 | 1,00 | 88,8 | 92,0 | 105 k | 106 k |
  | atraccion | 1,00 | 1,00 | 89,7 | 92,7 | 97 k | 99 k |

  La parte estable repite lo de DK64 y lo repite mas limpio: **campos por cuadro identicos
  (1,00) y trabajo por cuadro identico** con los dos factores; lo unico que cambia es el ocio
  (89-91 % -> 92-94 %). El tramo de arranque **no es comparable** y no hay que leerlo como una
  regresion: los tramos son por numero de intercambios, asi que con presupuestos distintos
  cubren contenido distinto (0-74 contra 0-47), y ademas ahi es donde mas se nota lo que a
  proposito NO escala -- el decaimiento del pestillo del PI va en instrucciones retiradas, y el
  arranque es justo la fase que vive de DMA de cartucho. Para juzgar el arranque hace falta
  una medida anclada a contenido (misma escena, no mismo numero de intercambios), que es otra
  tarea.

  **Que falta para mover el defecto** (por orden, y ninguno es opcional):
    1. repetir el barrido de arriba en Perfect Dark (PAL) y en SM64. DK64 solo dice que en
       ESE juego el cambio arregla lo saturado y no toca lo demas; un juego que ate logica al
       reloj lo diria distinto, y es justo el que hay que encontrar antes y no despues.
    2. escuchar el audio con el factor movido: el drenaje del AI convierte instrucciones
       retiradas en segundos de invitado (`viFieldHzMilli`), asi que cambiar cuantas
       instrucciones caben en un campo cambia el ritmo con que se sirven las muestras. El
       contador `KESTREL_AUDIOSTAT` (empujadas/servidas/descartadas) es la medida, no el oido.
    3. elegir el numero con un argumento, no con un gusto. Hoy la evidencia da 1,19 (DK64) y
       1,45 (PD), las dos COTAS SUPERIORES de agresividad porque solo se resta el giro ocioso
       de libultra y todo otro busy-wait sigue contando como trabajo. Lo honesto con esa
       evidencia es la parte alta del rango real (1,4), no la baja.
  **Los tres se cumplieron el 2026-09-08 y el defecto se movio a 1,4** (`kCpiDefault256 = 179`,
  en `src/cpu/cpu.hpp`; el razonamiento largo esta en `cpiFromEnv()`). (1) Mas juegos: DK64
  (arriba) y SM64, que da campos por intercambio en arranque 3,41 con CPI 2 · 2,84 con 1,5 ·
  **2,76 con 1,4** · 2,69 con 1,25, o sea rendimiento decreciente por debajo de 1,4. (2) Audio:
  con 1,4 `KESTREL_AUDIOSTAT` da `silencio=0 (0.00%)` y `cortas=0`, igual que con 2; el colchon
  minimo del anillo baja de 9504 a 3232 muestras de 22050 pero no llega a pasar hambre. (3) El
  numero: 1,4 es la parte ALTA del rango real del VR4300, que es lo honesto cuando las dos
  medidas propias (1,19 y 1,45) son cotas superiores de agresividad. `KESTREL_CPI=2` devuelve
  el comportamiento historico bit a bit, y sigue siendo la herramienta de medida.

  **Hipotesis del usuario, medida 2026-09-08: «la intro de DK64 falla porque la lectura del
  mando no es la que toca».** Se comprobo antes que ninguna otra cosa, y da una respuesta
  clara. Se anadio al corte de `[frames]` la cuenta de lecturas del mando (comandos 0x01 del
  joybus en el conector 1, `Memory::padPolls`), que es el unico «tiempo del mando» que el
  invitado percibe, y se barrio la intro entera (SoftRDP, threaded, dynarec):

  | ventana (intercambios) | campos VI | lecturas de mando | lecturas/campo | campos por lectura |
  |---|---|---|---|---|
  | 240-480 | 758 | 156 | 0,206 | 4,9 |
  | 480-720 | 1235 | 141 | 0,114 | 8,8 |
  | 720-1000 | 869 | 277 | 0,319 | 3,1 |
  | 1000-1400 | 1000 | 415 | 0,415 | 2,4 |

  Lo que dice el dato: **lecturas de mando ~= SYNC_FULL del RDP** (1060 frente a 1074 en la
  tanda de 1400 intercambios), o sea que DK64 lee el mando **una vez por cuadro que dibuja**,
  no una vez por interrupcion de video. El camino de lectura en si esta bien: una lectura por
  cuadro es lo que hace el juego en la consola. Lo que esta mal es **cuantos cuadros hay**: en
  hardware, a 30 fps, una lectura por cuadro son 0,5 lecturas por campo; aqui salen 0,11-0,42.

  Y el A/B con el factor de CPI lo ata al mismo sitio de siempre (misma tanda, 1400
  intercambios): CPI 2 da 1051 lecturas en 5229 campos = **0,201**, CPI 1,4 da 1223 en 5077 =
  **0,241**, un +20 % de lecturas por campo por darle mas CPU al invitado y nada mas. La
  cadencia del mando es una FUNCION del presupuesto de instrucciones, no un canal aparte.

  Conclusion para la liana: la intro avanza por cuadro, y como el emulador solo le deja
  completar la mitad de los cuadros que la consola, todo lo que la intro programa «dentro de N
  cuadros» cae en otro instante. La intuicion del usuario apunta al sitio correcto -- el ritmo
  con el que la intro consume entradas -- pero la causa no esta en el joybus sino en el
  presupuesto de CPU por campo, que es este mismo punto. Nota aparte, real pero de otro orden:
  nuestras DMA de SI se completan al instante (seccion «las DMA no cuestan tiempo»), lo que
  adelanta la FASE de cada lectura unos cientos de microsegundos, pero no cambia su RITMO, que
  es lo que rompe la intro.


- ~~**Artefacto de audio de Perfect Dark**: `descartadas=1126078` en marcha libre~~
  **CERRADO 2026-09-04: no habia artefacto.** Medido con `KESTREL_AUDIOSTAT` a 600 campos,
  el mismo arranque en las dos marchas: en marcha libre `empujadas=1065360 servidas=317024
  descartadas=727310 silencio=0,13%`, con throttle `empujadas=855232 servidas=852576
  descartadas=0 silencio=0,04%`. Los descartes son el emulador produciendo ~3x mas rapido
  de lo que el DAC puede tocar: el anillo de 250 ms se llena y se tira lo viejo, que es lo
  unico correcto (el sumidero va al reloj del dispositivo, no al del emulador). En tiempo
  real no se descarta ni una muestra y el hueco es del 0,04%. El `silencio 14-17%` que
  apuntaba el latido el 2026-09-02 tampoco esta: se lo llevaron los arreglos del RCP
  posteriores.
- ~~Decidir si `KESTREL_LLE_IPL3` pasa a ser el valor por defecto y se retira el parche
  `0xC86E2000`~~ **DECIDIDO 2026-09-04: sigue opcional; el parche ya no existe.** El
  hardcode a Perfect Dark murio cuando el arranque paso a reproducir el IPL3 del 6105 para
  CUALQUIER cartucho de ese CIC (copia de ROM 0x554..0x888 a RDRAM 0x004..0x338 y el DMA
  con paso del microcodigo): en `cpu.cpp` no queda mas que la tabla de CICs indexada por
  CRC de la imagen del IPL3, que no es un juego sino una firma. Medido HLE vs
  `KESTREL_LLE_IPL3=1`, todo en lockstep porque es el unico modo determinista aqui:
  n64-systemtest 0/3721+0/2+0/6 en los dos, SM64 md5 identico
  (`466282775dbd0ac084946558a1c30771`) y DK64 md5 identico
  (`5683d22e66c393d50602b648b1ec660d`); Perfect Dark difiere en 1123 pixeles de 331776,
  todos dentro del logo que esta apareciendo por fundido, o sea un fotograma de desfase por
  lo que cuesta ejecutar el IPL3 de verdad, no un fallo. Aun asi NO se cambia el defecto:
  las cuatro ROMs disponibles cubren tres CICs (6102, 6105, 7105) y no hay con que probar
  6101/6103/6106/5101, cuyos IPL3 reales no ha ejecutado nadie aqui. Cambiar el arranque de
  todo el parque apoyandose en tres CICs seria justo lo contrario de verificar. Se queda
  como opcion, y se revisa cuando haya ROMs de esos CICs.
- ~~Ampliar `KESTREL_EXCODD` a TLBL(2)~~ **CERRADO 2026-09-04** — el filtro ya no va por
  numero de codigo sino por el estado real del TLB: TLBL(2)/TLBS(3) se vuelcan cuando
  `tlbAnyValid()` es falso, es decir cuando no hay una sola entrada con el bit V puesto y
  por tanto ninguna traduccion mapeada puede acertar. Comprobado en los dos sentidos:
  `wildmem_test` con `KESTREL_EXCODD=4` pasa de 0 a 4 volcados de code=2 y 2 de code=3, y
  SM64 y DK64 (120 campos) no ganan ni un aviso nuevo.

- **El coste de los fallos de cache primaria ya se puede cobrar (HECHO 2026-09-10, apagado de
  fabrica).** Las dos caches estaban emuladas desde hace mucho -- datos, tags, bit sucio,
  write-back, la instruccion `CACHE`, integradas en el dynarec y guardadas en el savestate --
  pero **no costaban ni un ciclo**: `dcMiss()` y `icFill()` solo incrementaban un contador. Con
  eso, un bucle que pasea por RDRAM y otro que cabe entero en la D-cache corren a la MISMA
  velocidad de invitado, que es falso y desplaza la fase del juego respecto al VI. Es el mismo
  agujero que m64p cerro para las lianas de DK64.

  Lo que se ha hecho (`KESTREL_CACHECOST=<ciclos>`, `1`/`on` = 60, `0` = apagado = defecto):

  | pieza | donde |
  |---|---|
  | `chargeMiss()` en los dos caminos de fallo | `src/cpu/cpu.cpp` (`dcMiss`, `icFill`) |
  | `stallCycles` -> ticks de Count | `CPU::countTicks()`, `src/cpu/cpu.hpp` |
  | `stallCycles` -> ops equivalentes (`stallOps`) | idem, para el reloj de video y PI/SI |
  | latch de timer por CRUCE, no por igualdad | `CPU::countAdd()`, usado en interprete y JIT |
  | guardas de borde contra la COTA de ticks | `CPU::countTicksMax()` en `jitTryBlock` y `jitReenterProceed` |
  | permiso de cadena convertido ticks->ops | `CPU::opsForTicks()` |
  | estado de invitado en el savestate (v10) | `stallCycles`, `stallOps`, `stallOpsRem` |
  | linea `[cpi] base X real Y` al cortar | `src/core/system.cpp` |

  Los dos obstaculos reales, y como se han quitado:
  1. **El latch del timer era una igualdad.** `if(Count == Compare) timerIntr = true` solo vale
     mientras Count avanza 0 o 1 por op. Un fallo mete ~30 ticks de golpe y Count pasaria POR
     ENCIMA de Compare: la interrupcion se perderia. Ahora `countAdd()` comprueba si Compare
     cae DENTRO del tramo `(old, old+ct]`, que con `ct==1` es exactamente la igualdad de antes.
  2. **Las guardas de borde del JIT comparaban ops contra ticks.** `if(d <= K) bail` solo es
     conservador mientras `ticks(ops) <= ops` -- por eso `cpiFromEnv()` acota `cpi256 <= 256`.
     Ahora se comparan contra `countTicksMax(K)`, la cota superior de ticks del bloque (peor
     racha: 2 fallos por op). Con el coste apagado `countTicksMax(K) == K` y las guardas quedan
     byte a byte como estaban.

  **Y el reloj no se parte en dos.** El campo de video, la lectura de `VI_V_CURRENT`, el plazo
  del SI y el decaimiento del pestillo del PI miden el tiempo en *instrucciones retiradas*. Si
  las paradas solo movieran Count, Count y el video correrian a ritmos distintos -- el bug de
  "dos relojes" que documenta `Clocks::cyclesPerInsn`. Por eso las paradas se traducen tambien
  a ops equivalentes (`stallOps`, `1 op = cpi256/128 ciclos`) y `viTick`/`cartNow` las suman.

  **Lo que mide, con el coste puesto en 60 ciclos** (400 intercambios, tanda limpia):

  | juego | fallos D$ | fallos I$ | CPI que anaden | CPI total con base 1,0 | CPI medido aparte |
  |---|---|---|---|---|---|
  | SM64 | 0,745 % | 0,006 % | +0,451 | 1,451 | -- |
  | DK64 | 0,399 % | 0,006 % | +0,243 | 1,243 | **1,19** (cota inferior) |
  | Perfect Dark | 0,250 % | 0,013 % | +0,158 | 1,158 | **1,45** (cota inferior) |

  Eso es exactamente lo que hacia falta para *poder* justificar el numero: el CPI deja de ser
  un gusto y pasa a ser `1,0 de canalizacion + lo que la cache le cueste a ESE juego`. DK64
  cuadra (1,243 contra 1,19). **Perfect Dark no** (1,158 contra 1,45, y 1,45 es cota INFERIOR
  de lo que hace falta): sale demasiado barato.

  **Por que sigue apagado de fabrica, y que falta.** El `kCpiDefault256 = 179` de hoy (CPI 1,4)
  NO es el CPI de canalizacion del VR4300: es un CPI *efectivo* medido sobre juegos, o sea que
  ya lleva dentro el coste medio de los fallos, promediado. Encenderlo sin bajar antes la base
  a la canalizacion pura contaria la penalizacion DOS VECES. Y bajar la base a 1,0 hoy dejaria
  a Perfect Dark por debajo de su propia cota inferior. Falta, por orden:
    1. ~~**Cobrar los accesos NO cacheados.**~~ **HECHO 2026-09-10 -- y la hipotesis era FALSA.**
       Un load a KSEG1 paga la latencia entera de RDRAM (~60 ciclos, lo dice el propio
       `CLAUDE.md`) y aqui no costaba nada. Ya se cobra, con perilla propia
       `KESTREL_UNCACHEDCOST=<ciclos>` (`1`/`on` = 60, defecto 0) separada de `KESTREL_CACHECOST`
       para poder medir una sin la otra. Enganchado en los cuatro `CPU::readN`, en `LL`/`LLD` y
       en los DOS ayudantes de memoria del dynarec (el camino rapido que emite el JIT solo cubre
       ckseg0 dentro de RDRAM, asi que todo lo no cacheado cae en el ayudante). Solo se cobran
       las LECTURAS: en la VR4300 los stores no cacheados son *posted* -- el bufer de escritura
       se los queda y la CPU sigue --, y cobrarlos como sincronos seria inventar una parada que
       el HW no tiene.

       **Pero no era la explicacion.** Medido a 400 intercambios: SM64 **0,021 %** de las
       instrucciones hacen una lectura no cacheada, DK64 **0,005 %**, Perfect Dark **0,032 %**.
       A 60 ciclos eso son +0,013 / +0,003 / +0,019 de CPI: ruido. Con todo junto y base 1,0
       queda SM64 1,463 · DK64 1,244 · **PD 1,199** frente a su 1,45 medido. Perfect Dark
       sigue saliendo barato, asi que el agujero esta en otro sitio.
    2. ~~**Latencia de multiplicar y dividir ENTERO.**~~ **HECHO 2026-09-10 -- y tambien era
       FALSA.** Manual NEC, tabla 3-12: `MULT`/`MULTU` 5, `DMULT`/`DMULTU` 8, `DIV`/`DIVU`
       **37**, `DDIV`/`DDIVU` **69** PCycles, y ademas *"the VR4300 stalls the ENTIRE
       pipeline"* -- no es un enclavamiento perezoso sobre `MFLO`, para siempre. Implementado
       con perilla propia `KESTREL_MULDIVCOST` (`0`/`off` defecto, `stat` = contar sin cobrar,
       `1`/`on` = cobrar N-1), cobrando en las ocho ramas SPECIAL del interprete y, para
       `MULT`/`MULTU`, tambien en linea desde el dynarec (`emitMulDivCharge`, jit.cpp), que es
       donde el JIT las emite sin ceder al interprete. Medido:

       | Juego | mult/div enteras / retiradas | +CPI |
       |---|---|---|
       | DK64 (400 flips, JIT) | 0,007 % | +0,001 |
       | DK64 (120 flips, interprete) | 0,002 % | +0,000 |
       | Perfect Dark (400 flips, JIT) | < 0,0005 % | +0,000 |

       Una `MULT` cada ~14 000 instrucciones. El motivo es el que dice el propio `CLAUDE.md`
       en la seccion de optimizacion ("Never divide in inner loops", "Replace divisions with
       reciprocal multiply"): el codigo de la epoca **ya** evita la ALU entera para la
       aritmetica de verdad y lo hace todo en la **FPU**. Se queda implementado porque es
       semantica real del hardware, pero como explicacion del hueco de PD esta refutada.

    2b. **Latencia de la FPU (tabla 7-14)** -- sospechoso principal ahora, por eliminacion y
       por sentido: un motor 3D del 2000 hace su matematica en `float`, no en `MULT`.
       Reconstruida del manual: Add/Sub `.S`/`.D` 3; Mul `.S` 5 `.D` 8; Div y Sqrt `.S` **29**
       `.D` **58**; Abs/Mov/Neg 1; Round/Trunc/Ceil/Floor `.W`/`.L` 5; Cvt.S desde D 2, desde
       W/L 5; Cvt.D desde S 1, desde W/L 5; Cvt.W y Cvt.L 5; C.cond 1. Nota al pie: *"If the
       result of a floating-point instruction is needed by the subsequent instruction, one
       additional pipeline clock is required"* -- o sea que la FPU es un **enclavamiento**, no
       una parada de tuberia entera como el entero.

       Instrumentado ya con `KESTREL_FPUCOST` (`0`/`off` defecto, `stat` = contar, `block` =
       cobrar N-1 siempre). **`block` es una cota superior declarada, no el modelo del HW**:
       sobrecobra cada vez que detras hay trabajo entero independiente que el VR4300 si
       solapa. Sirve para acotar cuanto CPI cabe aqui antes de pagar el coste del modelo fino
       (fecha-de-listo por registro FP). El cobro es unico por instruccion aunque el JIT tenga
       camino rapido: los trampolines COP1 (`KC1A`, `KC1C`, `kestrel_jitCMPS/D`) cobran al
       entrar y silencian el cobro del interprete con `CPU::FpuCharge`, porque su camino lento
       delega en `jitInterpOp` -> `cop1op` y contaria dos veces la misma op.
    3. ~~**Asociatividad.**~~ **CERRADO 2026-09-17 -- no habia sesgo.** Se creia que la VR4300
       tenia caches de 2 vias. Falso: manual de usuario NEC VR4300/VR4305/VR4310 (U10504EJ7V0UM),
       "Instruction Cache is direct-mapped, virtually-indexed, and physically-tagged. The capacity
       is 16 KB." y "Data Cache is a direct-mapped, virtually-indexed and physically-tagged
       writeback cache. The capacity is 8 KB." (tabla de resumen: lineas de 32 / 16 bytes, "Direct
       map, virtual index"). El modelo de aqui ya es de mapeo directo: coincide con el HW.
    3b. **Muestra de PD en nivel -- HECHA 2026-09-17.** Defeccion (warp por trucos): D$ 2,29 %,
       I$ 0,38 %, CPI 2,73 con base 1,0. El hueco era la muestra de titulo. Detalle y por que el
       defecto no se mueve sin captura de HW: `docs/STATUS.md`, seccion del 2026-09-17.
    4. **Recalibrar la base y rehacer el barrido** de "Que falta para mover el defecto" de mas
       arriba, entero (mas juegos, `KESTREL_AUDIOSTAT`, y el numero con un argumento).
       **Modelo fisico completo 2026-09-17** (perillas por fuente: I$ 48, D$ 44, sin cache 38,
       MULT/DIV tabla NEC, FPU `block`, enclavamientos LDI/DCB; ver `docs/STATUS.md`). Lockstep,
       Threaded e interprete dan lo mismo con cada perilla. **El defecto sigue sin moverse**:
       ninguna fuente (ares, libdragon, n64brew, cen64, NEC, SGI) da verdad de consola de campos
       por intercambio; falta la captura de HW.

    Sesgo conocido y anotado del punto 1: `SWL`/`SWR`/`SDL`/`SDR` se emulan como
    lee-modifica-escribe, asi que un store parcial a memoria NO cacheada cobra una lectura que
    el HW no hace (alli el controlador escribe con byte-enables). En la version cacheada SI es
    correcto: un store parcial que falla en D-cache rellena la linea de verdad. Frecuencia
    medida: despreciable, pero queda escrito.
  Mientras tanto: `KESTREL_CACHECOST=0` es el defecto y el emulador se comporta exactamente
  como antes (`systemtest` 0/3721 · 0/2 · 0/6 en interprete y dynarec, con el coste apagado Y
  encendido a 60).

### Rendimiento (ordenado por perfil de anfitrion, 2026-09-11)

> **REMEDIDO el 2026-09-18.** El perfil de abajo es de antes de las dos barreras de invitado y
> de `KESTREL_PACEASK`, y ya no describe la maquina: `cpuWait` bajo del 20,2 % a ~5 %. El
> reparto nuevo (Perfect Dark, 600 campos, `build-prof-prdp`, 1243 muestras, 89 % dentro de
> imagen) es `PaceDiv::div` 15,1 % · `atomic_load<u64>` 9,3 % · `spBarrierWait` 6,5 % ·
> `jitTryBlock` 6,0 % · `dpLogApply` 5,9 % · `rcpRetire` 3,9 % · `spCycleAt` 3,8 % ·
> `rspPace` 2,3 % · `rdpPace` 1,6 %. `dpBarrierWait` ha DESAPARECIDO de la cabeza.
> Las tres primeras son `spBarrierWait` y sus inlinees: ~44 % del hilo, la mitad aritmetica.
> **Y esa aritmetica no se cobra** -- memoizarla entera salio en nada medible en dos tandas
> (ver `docs/STATUS.md`, 2026-09-18). El hilo de CPU esta bloqueado esperando al RSP, asi que
> su trabajo local es gratis: en este hilo solo paga quitar TRAFICO DE COHERENCIA (que es lo
> que hizo `KESTREL_PACEASK`, -2,5 % en PD) o quitar BLOQUEO. Las muestras, por si solas,
> enganan.
> **Y EL PALO LARGO NO ES ESE HILO.** Perfilado el mismo dia el hilo del RSP (`KESTREL_HOSTPROF_WHO=rsp`,
> Perfect Dark, 600 campos): esta ocupado el 63,4 % del tiempo, contra un `cpuWait` del 7,5 %. De sus
> muestras ~38 % caen en `dpLogWait` (17,9 % dentro de imagen + 20,3 % FUERA, que es
> `std::this_thread::yield()`) y ~15,6 % en `spReadSync`. Las `1 807 544 esperas` del `[dplog]` son las
> ~1,81 M lecturas de DPC que hace el microcodigo, y cada una es una cita de orden de invitado con la CPU
> (`Rsp::mfc0`): la cita es SEMANTICA, no se puede quitar. Lo que si se puede es abaratarla -- ver
> `KESTREL_RDVYIELD` abajo.


La lista de antes (`dcFill`/`dcFlush` 4,5 %, `rcpPace` 2 %) venia de un perfil viejo, de cuando
el interprete mandaba y el RCP iba en el mismo hilo. **Ya no es cierta**: medido de nuevo con
`KESTREL_HOSTPROF=1` sobre SM64 en `build-prdp` (threaded-jit, Parallel-RDP, 400 intercambios,
906 muestras, 769 dentro de la imagen), simbolizado contra `llvm-nm --numeric-sort` con
`ImageBase 0x140000000`, el hilo de CPU reparte asi:

| % de muestras | simbolo | que es |
|---|---|---|
| 43,43 % | `Memory::spBarrierWait` | casi todo **espera activa**, no bloqueo |
| 34,46 % | `Memory::dpBarrierWait` | **bloqueado** en la condvar esperando al RDP |
| 6,63 % | `kestrel_jitProceedTramp` | trampolin del dynarec |
| 5,72 % | `Memory::rspPace` | regulador CPU<->RSP |
| 5,07 % | `CPU::jitIdleSkip` | salto del bucle ocioso del invitado |
| 1,30 % | `Memory::rdpPace` | |
| 0,65 % | `Memory::rcpPace` | |
| 0,65 % | `CPU::jitTryBlock` | |

`dcFill` y `dcFlush` **no aparecen**: los caminos calientes de D-cache ya estan en linea dentro
de `cpu.hpp` y lo que queda fuera de linea solo lo pisa la instruccion CACHE.

Y el reparto de pared del mismo arranque (`[block]`):

```
pared 8.68 s | cpuWait 20.2% (freno 0.0% barSP 1.2% barDP 31.5%)
rsp ocupado 65.7% aparcado 0.0% | rdp ocupado 16.8%
CPU real: cpu 98.9% rsp 65.8% rdp 45.1%
```

**Lectura: en SM64 la CPU emulada ya no es el palo largo.** El hilo de CPU quema el 98,9 % de un
nucleo pero solo una parte es trabajo util; `barSP` bloquea apenas el 1,2 % del `cpuWait`, asi
que ese 43 % de muestras en `spBarrierWait` es la espera ACTIVA de 2048 vueltas, girando sobre
lineas de cache que escribe el otro hilo. El palo largo de verdad es el RSP (65,7 % ocupado) y
detras la espera del DP.

Por ahi va el orden nuevo:

- **Espera activa de las barreras** (en curso). La CPU gira sobre `rcpPend`/`spBarrierEff` y el
  RSP gira sobre `cartNow()`: cada vuelta pide la linea en exclusiva y le quita al otro hilo el
  ciclo con el que iba a soltarla. Primer paso hecho: pista `PAUSE` en los tres bucles y largo
  del giro ajustable (`KESTREL_SPINPAUSE=0`, `KESTREL_BARSPIN=<n>`). Lo siguiente seria un
  retroceso exponencial y, sobre todo, mirar si el giro hace falta tan largo.
  **Contestado a medias (2026-09-18):** el giro hace falta MAS largo, no menos. Re-barrido
  `KESTREL_BARSPIN` ya con los diarios puestos: 2048 -> 16384 gana -1,6 % en SM64 y empata en
  los otros tres (131072 ya pierde). Lo que queda de esta via no es acortar el giro sino
  ABARATAR la vuelta: el bucle leia seis lineas por vuelta (`rcpPend`, `rspLogWait` y las
  cuatro de `spBarrierEff`), todas escritas por el hilo del RSP, o sea que cada vuelta le
  quitaba en exclusiva lineas que necesita para avanzar -- y avanzar es lo que esperamos.
  **CERRADO 2026-09-18: medido dos veces, PIERDE.** Variante de detector de cambio +0,1..0,4 %
  en los cuatro juegos; variante de prueba rapida sobre `spBarrierAt()` -0,1 % en jr y DK64
  pero +0,84 % en PD y +0,59 % en SM64. De las seis lineas, cinco (`rspPark`, `rspParkWake`,
  `rspRdvAt`, `rcpPend`, `rspLogWait`) casi nunca se escriben, o sea que estan en estado
  compartido en la L1 y leerlas es una carga de L1; la unica que el RSP reescribe sin parar es
  `rsp.cyclesRun`, y esa ES la barrera, hay que mirarla. La vuelta ya era barata y la rama que
  se le anade cuesta mas. **No volver a adelgazar este bucle**: lo que se paga es la latencia
  de ida y vuelta entre los dos relojes de invitado, no el ancho de la vuelta. Tablas en
  `docs/STATUS.md`. Queda sin probar solo el retroceso exponencial.
- **El RSP**, que es el palo largo medido (65,7 % ocupado). **Perfil propio del hilo del RSP,
  tomado el 2026-09-11** (`KESTREL_HOSTPROF_WHO=rsp`, build-prof-prdp con DWARF, SM64 /
  Parallel-RDP / threaded-jit / 400 intercambios, 926 muestras, `scripts/hostprof_sym.py`).
  Los numeros que llevaba la cabecera de `src/rsp/rspjit.hpp` eran de ANTES del JIT del RSP y
  ya estan sustituidos:
  | muestras | donde | nota |
  |---|---|---|
  | 32,07 % | fuera de imagen desde `Rsp::step` (rsp.cpp:1950) | el `blk.fn(this)`: **microcodigo compilado** |
  | 26,89 % | `condition_variable::wait_for` | el hilo **OCIOSO** |
  | 10,58 % | fuera de imagen desde `Memory::rdpSubmit` | el RSP patea el RDP (driver) |
  | 2,16 % | `Rsp::exec` | interprete, lo que el JIT no se lleva |
  | 2,05 % | `Memory::dpCompletedAt` | |
  | 1,73 % | `SoftRdp::drawTriangle` | modelo de pixeles, tambien bajo Parallel-RDP |
  | 1,40 % | `Rsp::publishExact` / `Rsp::step` | |
  24,6 % dentro de la imagen, 75,4 % fuera, y de TODAS las muestras el 32,1 % caen en codigo
  emitido. **El bucle de despacho ya no aparece**: ni una funcion del interprete pasa del
  2,2 %, que es exactamente para lo que se hizo el dynarec del RSP. Lo que manda ahora en
  este hilo no es el interprete sino (a) esperar, 26,9 % --- con `[block]` diciendo `rsp
  ocupado 65,1 %`, o sea que **el hilo del RSP no esta saturado y el 65 % de ocupacion es
  reloj de invitado, no de anfitrion** --- y (b) el pateo del RDP, 10,6 %. Sigue pendiente
  COP0 del RSP dentro del JIT del RSP, pero hay que bajarle la prioridad: la parte
  interpretada que queda ya es ruido.


  Lo de `[det] idle=0/0` en SM64 **ya esta contestado y no es un fallo**. La linea `[det]`
  saca ahora el desglose `idle=saltos/vueltas(sigN/drnN/roomN)`, y en SM64 (300 campos,
  threaded-jit) los 1 041 389 sondeos se van ENTEROS por `sig`: la firma no se repite nunca.
  Desglosada la firma, el PC si se repite (1 038 015 de 1 041 389) y la longitud del cuerpo
  cae dentro de [2,64] en 665 177, pero la huella de `r[1..31]` **no coincide ni una sola
  vez** y el valor leido solo 1 356 veces. O sea: en SM64 el RSP sondea `DPC_CURRENT`
  mientras el RDP esta MASTICANDO (`rdp ocupado 54,5 %`, `open=317 k` de 1 041 k), asi que el
  valor se mueve en cada vuelta. No hay bucle de espera con valor fijo que aparcar, que es
  exactamente la condicion del aparcamiento; en DK64 si se cumple porque alli el microcodigo
  espera con el motor drenado. Y el sondeo tampoco es el gasto: son ~5 M de instrucciones de
  RSP sobre 291 M de ciclos, un 1,7 %. Lo que cuesta es el trabajo vectorial de verdad, y eso
  lo ataca el JIT del RSP.
- ~~**`CPU::jitIdleSkip` cuando NO casa** (5,07 % de las muestras): memoizar el veredicto
  negativo por PC~~ **MEDIDO Y DESCARTADO (2026-09-18)**. Tabla de 256 entradas sellada con el
  numero de relleno de la linea de I-cache: en Threaded neutro (dentro del +-2 % de ruido en los
  cuatro juegos, min de 6 rondas) y en JIT lockstep, que es donde la CPU si es el palo largo,
  PIERDE +0,7..+1,2 % de forma consistente. El rechazo rapido ya era mas barato que el memo: la
  sonda de I-cache y la tabla de 2 KB cuestan mas cargas que los dos `jitFetchWord` sobre una
  linea que el despachador acaba de leer. Detalle y tablas en `docs/STATUS.md`.
- **`rspPace`/`rdpPace`** (7,0 % entre los dos): HECHO el reciproco exacto (`struct PaceDiv`,
  `m = ceil(2^79/d)` con su `maxX`, division de verdad por encima del limite), y **no era ahi**.
  A/B intercalado de dos rondas de 5: 4,95/4,96 s con reciproco contra 4,93/4,98 s con division.
  Ese 7 % es la lectura de `rspBusy` y `rsp.cyclesRun`, dos lineas que el worker del RSP
  reescribe sin parar -- fallo de cache compartida, inherente al acoplamiento CPU-RCP. `paceGrant`
  ya las lee una sola vez por vuelta y `kPaceGrain` ya evita la cola de permisos que encogen.
  **PARTE COBRADA 2026-09-18**: no se puede abaratar la lectura, pero si hacerla menos veces. El camino del JIT preguntaba al freno tras CADA bloque mientras el del interprete lo hacia cada 64 instrucciones; con `KESTREL_PACEASK` puesto a 1024 (meseta medida) el barrido intercalado da PD -2,5 %, SM64 -1,4 %, jr -1,1 %, DK64 -0,3 %, mismo md5. Ver `docs/STATUS.md`.
- **Coste de `rdpSubmit`, desglosado (2026-09-18).** Con `KESTREL_DPSUBPROF=1` el emulador saca
  `[dpsnap]`, que parte la llamada en sus piezas. SM64 300 campos, Parallel-RDP + threaded-jit:
  **519 603 envios para 518 473 comandos**, o sea el microcodigo escribe `DPC_END` una vez por
  comando, y el tramo medio son 77 B. Descontando el coste del propio reloj (~50 ns por lectura
  de `steady_clock::now()` en esta maquina, y el sondeo hace diez por llamada), el reparto real
  de los 0,32 s que cuesta `rdpSubmit` sobre 7,9 s de pared -- **4,0 %** -- es:
  | pieza | s | que es |
  |---|---|---|
  | paseo de coste (`rdpCostPass`) | 0,129 | modelo de ciclos del RDP: **exactitud, no se toca** |
  | papeleo de `dpScheduleSpan` | 0,062 | anillo, `dpWrLo/dpWrHi`, plazos |
  | cola + banderas | 0,123 | `rdpQueue`, `dpPending`, `rcpPend` |
  | coger `rdpMx` | ~0 | **no hay contencion**, al contrario de lo que se suponia |
  | copia a la sombra (77 B) | ~0 | el volumen es trivial |
  | `notify_all` | 0,004 | solo 771 veces de 519 603: el giro del worker ya lo evita |
  O sea el techo de esta via es ~0,19 s = 2,4 % de pared, y solo si se borrara TODO el papeleo.
  Y ademas cae en el hilo del RSP, que tiene holgura (70 % de nucleo), no en el de CPU, que
  esta al 99 %: recortar aqui puede no mover la pared en absoluto. **MEDIDO Y DESCARTADO en la
  primera hipotesis:** se sospechaba falso compartir sobre `dpPending` -- el worker del RDP la
  lee en cada vuelta de su giro y el RSP le hace un `fetch_add` en cada envio --, asi que se
  metio `KESTREL_RDPSPINMASK` para espaciar esas lecturas (una de cada 16 o 64) sin cambiar el
  largo del giro. El coste de `rdpSubmit` apenas se movio (0,597 -> 0,578 s, dentro del ruido)
  y el barrido intercalado de pared no dio ganador en tres rondas. La perilla se retiro del arbol.
- **Afinidad de hilos a nucleos fisicos: MEDIDA Y DESCARTADA (2026-09-18).** Con SMT el
  planificador puede emparejar dos de los tres hilos calientes en los hermanos del mismo
  nucleo. Se probo `KESTREL_AFFINITY=1` (un nucleo fisico entero por hilo). Barrido
  intercalado de cuatro rondas, minimo de cuatro: jr +1,2 %, PD -0,5 %, SM64 +1,8 %,
  DK64 +1,8 % -- tres perdidas y una ganancia dentro del ruido. Los hilos no estan calientes
  a la vez (el del RDP solo trabaja el 11,8 % del tiempo), asi que una particion fija le
  quita al hilo de CPU los hermanos que los otros dejan libres. Codigo retirado; los md5
  salieron identicos en las dos ramas, o sea que era ajuste de anfitrion puro. Detalle en
  `docs/STATUS.md`.
- **Memoizar `spBarrierAt()` en el hilo de CPU: MEDIDO Y DESCARTADO (2026-09-18).** Es funcion
  pura de `spKickEdge`/`spKickCycles` (que solo escribe el hilo de CPU) y de `rsp.cyclesRun`
  (que el worker publica cada pocos miles de instrucciones), y se pregunta en cada retiro: el
  memo acertaba casi siempre y daba el mismo numero bit a bit (md5 identico en los 4 juegos y
  en los 4 cruces memo x Lockstep/Threaded). Pared: jr -0,70, PD +0,03 / -0,26, SM64 +1,47 /
  +0,52, DK64 +0,38. Dos tandas, nada reproduce. Tampoco tiene version fuerte: saltarse ademas
  la LECTURA de `cyclesRun` no vale, porque `spBarrierEff()` puede bajar sin que ese contador
  se mueva (aparcamiento del RSP, cierre de cita).
- **Ceder el nucleo menos veces en las citas del hilo del RSP: COBRADO (2026-09-18).**
  `KESTREL_RDVYIELD`, 256 -> 65536. El `std::this_thread::yield()` de los tres bucles de cita
  es `SwitchToThread()` en Windows, o sea una llamada al kernel que en esta maquina casi nunca
  encuentra a quien cederle nada. Dos tandas intercaladas de min-de-4: jr -0,51 / -0,47,
  PD -0,65 / +0,04, SM64 -1,00 / -0,60, DK64 -0,37 / -0,29; siete de ocho a favor, md5
  identico, y la primera tanda monotona en los cuatro juegos. La meseta acaba ahi: 262144
  empata y 1 M es peor en PD, porque detras del yield van el aviso al hilo de CPU y el
  salvavidas de pared. Queda pendiente el OTRO 17,9 % de `dpLogWait` que si esta dentro de
  imagen.
- **Sacar de la vuelta de la cita las dos lineas que casi nunca se mueven: COBRADO (2026-09-18).**
  `KESTREL_RDVCHEAP`. `dpLogFlush` (solo lo escribe la CPU al pararse) y `rspStop`/`hostStop`
  (solo al cerrar) se miraban en CADA vuelta de `dpLogWait` y `spReadSync`; ahora van a la misma
  cadencia que la condicion de salida (1 de cada 64). Dos tandas intercaladas de min-de-4:
  PD -2,94 % / -2,73 %, SM64 -1,77 % / -2,17 %, con las cuatro lecturas de cada brazo disjuntas
  en los dos juegos y las dos tandas; jr y DK64 planos. md5 identico. Es la contraparte de la
  leccion del memo de `spBarrierAt()`: en un hilo que gira, quitar trafico de coherencia SI paga.
- **Los cinco campos del reloj de invitado en UNA linea de cache: COBRADO (2026-09-18).**
  `retired`, `jitPending`, `stallCycles`, `stallOps` y `stallOpsRem` estaban sueltos por el
  struct `CPU`; `Memory::cartNow()` los lee los cinco y quien mas lo llama es el hilo del RSP en
  cada vuelta de sondeo de sus citas. Juntos (28 B, `alignas(64)`, detras de `gpr` porque el JIT
  exige `gpr` en offset 0) el sondeo tira de una linea en vez de varias. Dos tandas intercaladas
  de min-de-4, OCHO de ocho a favor y lecturas disjuntas en los cuatro juegos: PD -2,96 / -2,18,
  jr -2,69 / -2,79, DK64 -1,56 / -1,53, SM64 -1,29 / -1,67. Colocacion pura: md5 identico y
  `[statehash]` de jr a 400 M `dc07d7ac23fef2e1` en los cuatro cruces.
- Presentacion sin copia (zero-copy).
- Sombra de MXCSR — 0,7 %.
- Coste de llamada de `runFifo`.
- **Seguimiento de paginas sucias para el rebobinado** (nuevo 2026-09-04). Hoy cada foto
  para el RCP y recorre el estado ENTERO comparandolo: +45 % de pared con foto cada 2
  campos, +24 % cada 6 (`docs/REWIND.md`). Con un mapa de paginas sucias la foto solo
  miraria lo que cambio. No es corto: hay que cazar TODAS las vias de escritura a la
  RDRAM -- CPU cacheada y sin cachear, DMA de RSP/PI/SI y el propio RDP -- y una sola que
  se escape corrompe el estado rebobinado en silencio, que es peor que el coste.

### Fidelidad de temporizacion — las DMA no cuestan tiempo (parcial: SI hecho 2026-09-08)

**SI: HECHO.** `Memory::siDma()` ya no termina en la misma instruccion que la arranco. Ahora
factura el tiempo real de la linea joybus leyendo el bloque de ordenes de la PIF RAM (4 us por
bit ⇒ 32 us por byte, mas la parada de la consola de 3 us y la del mando de 4 us por orden,
mas ~5 us de traslado de los 64 bytes entre RDRAM y PIF RAM), deja `SI_STATUS` con
`DMA_BUSY` puesto y arma un plazo en el reloj de invitado (`siDoneAt`, en instrucciones via
`usToInsns()`). `siFinish()` copia la respuesta a la RDRAM y levanta `MI_SI` cuando vence.
Una lectura de botones de los cuatro mandos sale en el orden de los cientos de microsegundos,
como en la consola. Con eso el hilo que hace `osContStartReadData` y se duerme en
`osRecvMesg(&siEventQueue)` **cede de verdad**, y el reparto de trabajo dentro del cuadro se
parece al del aparato.

El plazo es determinista en los siete modos porque vence contra `cpu.retired`: el interprete lo
mira por instruccion en `System::stepCpu()` y el JIT tiene prohibido compilar un bloque que se
tragaria el vencimiento (`jitTryBlock` consulta `siDueIn()` igual que ya hacia con el borde de
`Count`==`Compare`, y recorta el permiso de encadenado). El estado en vuelo (`siBusy`,
`siToPif`, `siDram`, `siDoneAt`) viaja en la foto de estado, que subio a version 7.
`KESTREL_SIINSTANT=1` recupera el comportamiento viejo (final instantaneo) para comparar.

**PI: CERRADO 2026-09-18.** `piDma()` ya no pone `pi_status = 0x8` (hecho) nada mas empezar:
arma un plazo (`piArm`) con la duracion que sale de los tiempos de latencia/pulso/liberacion
programados en `PI_BSD_DOM*`, y `MI_PI` no se levanta hasta que el reloj de invitado llega al
final (`piFinish`). Misma maquinaria que el SI: campo de ocupado (`piBusy`/`piDoneAt`),
vencimiento contra `cpu.retired`, guarda del JIT (`ioDueIn()` = el minimo del SI y el PI) y
foto de estado (version 13). Ademas `loadRom` programa `PI_BSD_DOM1_*` desde los bytes
0x01..0x03 de la cabecera, que es lo que hace el IPL2 en la consola y aqui no hacia nadie
porque el arranque es HLE. `KESTREL_PIINSTANT=1` recupera el final instantaneo para bisecar.
Detalle y cuentas en `docs/STATUS.md`.

- (3) **CERRADO 2026-09-18.** Agenda de eventos por instante. Los plazos del VI (cruce de la
  linea de `VI_INTR` y cierre de campo) y del AI (fin de bufer del DAC) se arman igual que el
  del SI y el del PI, y los cinco se pliegan en un solo `eventDueIn(now)` que consultan las
  tres guardas de `jit.cpp`. `MI_VI` llegaba hasta ~1 ms tarde (caia en el borde del subtramo
  del bucle, no en el cruce) y `MI_AI` igual; ahora los dos caen en la instruccion exacta. La
  prueba es que `KESTREL_VITICKS` -- un ajuste de ANFITRION -- ya no mueve el `[statehash]`
  del invitado, de 1 a 64, ni en Lockstep ni en Threaded. Detalle en `docs/STATUS.md`.
  Queda FUERA, y sigue suelto: el borde `Count`==`Compare` del COP0, que vive en la CPU y no
  en `Memory`, y los plazos del RCP (`rcpDueIn`), que se pliegan aparte en los mismos tres
  sitios. Meterlos en la misma llamada es cosmetica, no exactitud: los dos ya son exactos.


### snapper64: lo que la bateria mide y nosotros no modelamos

`test_roms/snapper64.z64` (HailToDodongo) compara superficies **byte a byte** contra una
referencia capturada en consola real. Marcador completo en `build-prdp` (2026-09-10): 4182 de
6632. Lo que falla NO es precision del rasterizador -- parallel-RDP y nuestro SoftRDP puntuan
**exactamente igual, grupo a grupo**, asi que son funciones que faltan, no error de redondeo.

Lo cerrado: `RDP Test-Mode - Span R/W` 32/32 desde que existen los registros DPS
(`0x0420_0000`, ver `src/core/memory.cpp`).

Lo que queda, por tamano:

- **Triangulos en ciclo FILL** (`Fill Mode Tri (Sweep)` 2048 + `(Random)` 7). Nuestro trazo es
  el triangulo geometricamente exacto; la referencia de consola sale **desplazada** y con
  **escrituras de byte parcial** en los bordes del tramo -- hay pixeles donde solo aterrizo el
  byte R (`0xFF0000xx`). Eso dice que en FILL el tramo se calcula en **bytes / palabras de 64
  bits**, no en pixeles: el RDP rellena el ancho completo de la palabra y los extremos quedan a
  medias. Ni nuestro SoftRDP ni parallel-RDP lo hacen. Es la pieza mas gorda y la unica con
  evidencia numerica ya recogida.
- **`Undefined Shade 1C`** (64). Comportamiento del combinador cuando el sombreado no esta
  definido; hay que averiguar que valor deja el aparato en ese camino.
- **Noveno bit oculto de la RDRAM, RDP->CPU** (32). Necesita los bits de `MI_MODE` 9/10 (EBUS)
  y 12/13 (UPPER) con lectura de estado, y un plano oculto **por byte**. Hoy `rdramHidden` es
  `rdram.size()>>1`: un byte de cobertura por pixel de 16 bits (lo usan `vi::fetchFiltered` en
  `src/core/cpu.cpp:111`, `src/video/present.cpp:1192` y `src/video/rdp.cpp:274`). El grupo
  CPU->CPU (4) si pasa.
- **`Rect-Tri Slopes`** (3 de 250): los indices **186, 187 y 188**, y solo en su **segundo**
  aserto, el que pone `isl = i*0x333*1000` (~0x0914_6EB0, unas 2324 lineas de pendiente
  inversa). Con una pendiente asi la X del andador de bordes **desborda y da la vuelta**, y la
  referencia de consola sale como filas sueltas de ancho completo salpicadas de filas cortas:
  el test mide exactamente donde envuelve el acumulador. Los vecinos 185 y 189 pasan, asi que
  es un borde estrecho de anchura/saturacion del acumulador. Ojo: parallel-RDP falla los mismos
  tres, asi que puede que el desvio este aguas arriba y no en nuestro SoftRDP.

Y dos que **no** son alcanzables con parallel-RDP tal cual:

- **`Test-Mode Span Tri`** (216). Llena el buffer de tramos con `0x55555555`, dibuja un
  triangulo sombreado y lee el buffer para ver que dejo el rasterizador dentro. Es estado
  **interno** del RDP: el propio test dice que comprueba "si los emuladores sobreescriben de
  verdad el buffer de tramos". parallel-RDP no expone nada equivalente, asi que solo se puede
  cerrar por el camino del rasterizador propio.
- **`Rect No-Sync` 1C / 2C / Fill** (20+20+20). Lanza un rectangulo y detras, **sin sync**,
  cambia el color de relleno tres veces seguidas: mide la **latencia del cauce** del RDP,
  cuantos pixeles salen todavia con el color viejo. parallel-RDP aplica cada orden de forma
  atomica, asi que por construccion no puede reproducirlo. Es el unico bloque de la bateria que
  de verdad es un problema de tiempos (60 tests de 6632).

**La referencia de consola se puede leer en el PC**, sin emulador de por medio: `tests.pack`
y `tests.pack.idx` salen del ROM con `dumpdfs -e` (herramienta de libdragon), y cada entrada es
un asset `DCA3` de libdragon comprimido con **Shrinkler**. El indice es
`{u32 fileCount, {u32 groupHash, u32 testHash, u32 offset}[]}` con 7095 entradas ordenadas por
`(groupHash, testHash)`; el bit 31 del desplazamiento marca "tamano impar" y el tamano sale de
restar el desplazamiento siguiente. La cabecera del asset (version 3, la que usa esta ROM) es
`magic[4]="DCA3", u16 algo, u16 flags, u32 cmp_size, u32 orig_size, u32 inplace_margin` = 20
bytes. Hay un portado en Python del decodificador de Shrinkler de libdragon
(`src/compress/shrinkler_dec.c`) en el cuaderno de esta sesion; con eso se saca cualquier
superficie de referencia byte a byte y se compara sin capturar pantalla.

Asi salio que los triangulos en ciclo FILL no son "nuestro rasterizador desalineado" sino
**comportamiento indefinido del aparato**: en el barrido `0.25 | 0.25`, la fila 9 de la
referencia escribe las palabras 49..53 enteras, la 54 solo su ultimo byte, **se salta la 55**, y
vuelve en la 56 con los cinco primeros bytes. Las mascaras de byte que aparecen son siempre
`0xFF`, `0xF8`, `0x80` y `0x01` sobre palabras de 64 bits. Ningun juego dibuja triangulos en
FILL (usan rectangulos), asi que esto es curiosidad de precision, no funcionalidad.

Como se reproduce en el emulador: `KESTREL_NOVIDEO=1 KESTREL_PADS=1`, arrancar con `--run`, y por telemetria
escribir en el `ctx` global de snapper64 (se localiza barriendo la RDRAM fisica buscando
`SAVE_DATA_MAGIC = 0xABCD02` en big-endian): `ctx.nextTest = <grupo>` entra directo a ese
grupo, `ctx.saveData.autoAdvanceTest = 0` lo aparca en un test, y `ctx.diffMode` 0..5 recorre
`Actual/Diff/Ref` de color y de cobertura -- los modos `Ref` copian la referencia de consola
**dentro** de la superficie, asi que la captura da nuestro pixel contra el del aparato.

### Interfaz / herramientas

- Modelo de mando 3D (el analógico como plano de mando, no como par de ejes).
- Huecos del lanzador: puntos de observación (watchpoints) y puntos de ruptura del RSP.

---

## Cómo se mantiene esta lista

Cuando un punto se cierra, se borra de aquí y se cuenta en `docs/STATUS.md` con la
medición. Cuando aparece uno nuevo, entra aquí con su verificación de código (fichero y
línea), nunca de memoria.


## Deudas que ABRE encender el coste de cache (2026-09-10)

Encender `KESTREL_CACHECOST` hace que la CPU retire menos instrucciones por unidad de tiempo
de invitado. Todo lo que mide a la CPU en *instrucciones retiradas* en vez de en *tiempo de
invitado* se descalibra en cuanto la perilla deja de estar apagada. Inventario:

1. **HECHO -- plazo del SI.** Se armaba en `cartNow()` y se vencia contra `cpu.retired`.
   Colgaba SM64 entero (0 lecturas de mando). Arreglado vencindolo en `cartNow()`; ver
   `docs/STATUS.md`, seccion "Dos relojes, un plazo".

2. **HECHO 2026-09-10 -- interleave CPU:RSP de Lockstep** (`src/core/system.cpp`). Era
   `rspPhase += rspStepNum` una vez por instruccion retirada. El ratio de `Clocks` es RSP por
   instruccion-*equivalente* de CPU, y una instruccion que se para 60 ciclos en la cache no
   vale lo mismo que una que retira limpia: el RSP corre igual durante esa latencia de RDRAM.
   El sesgo iba en direccion "RSP demasiado lento" justo en las escenas con mas fallos de
   cache. Arreglado midiendo el avance en `cpu.guestOps()` (retiradas + paradas ya convertidas
   a equivalentes, el MISMO reloj en que nacen y vencen el campo de video, el plazo del SI y
   el latch del PI):

   ```cpp
   u64 nowGuestOps = cpu.guestOps();
   u64 dGuestOps   = nowGuestOps - lastGuestOps;
   lastGuestOps    = nowGuestOps;
   if(memory.rcpMode == Memory::RcpMode::Lockstep && memory.rsp.running && dGuestOps) {
     rspPhase += rspStepNum * dGuestOps;
   ```

   La referencia se reengancha tambien al salir de un bloque del dynarec: ese camino solo se
   toma con el RSP parado, asi que lo que avance el reloj ahi dentro no le toca al RSP y no
   debe acumular fase. Con `KESTREL_CACHECOST` apagado `dGuestOps` vale 1 por instruccion y
   queda byte a byte igual que antes.

3. **HECHO 2026-09-10 -- regulador de Threaded** (`Memory::rcpPace`/`rspPace`/`rdpPace`, y la
   llamada del prologo del dynarec en `jit.cpp`). Tenia el mismo problema que el 2, con el
   ratio invertido (`paceCpuNum/paceCpuDen`). Era coherente consigo mismo -- mide deltas contra
   su propia base -- asi que no colgaba, pero contaba un reloj distinto del interleave de
   Lockstep, y que los dos modos den el MISMO md5 es una invariante de las puertas. Los tres
   puntos de llamada pasan ya `cpu.guestOps()` y el parametro se llama `cpuOps`, no
   `cpuRetired`. El permiso que devuelve se sigue usando como cupo de ops del dynarec: en
   unidades de invitado es una cota conservadora (ops <= guestOps siempre), asi que nunca
   concede de mas.

   **Verificado (DK64 PAL, 300 campos).** Lockstep es determinista y lo sigue siendo: 3 de 3
   corridas identicas sin coste (74 intercambios / 56 syncs / 402M) y 3 de 3 con `CACHECOST=60`
   (76 / 31 / 320M; antes del cambio 74 / 31 / 316M, tambien 3 de 3 -- el RSP recibe ahora los
   pasos que le tocan durante las paradas). Threaded no se puede comparar corrida a corrida
   porque no era determinista de base: ver 3b (resuelto el 2026-09-10). Puertas verdes: `gate_all` rc=0 y `gate_prdp`
   rc=0, siete modos `Base 0/3721 Timing 0/2 Cycle 0/6`, md5 de sm64 sin cambio en interp y en
   parallel-RDP.

3b. **RESUELTO (2026-09-10, cerrado del todo el 2026-09-11) -- el modo Threaded ya es
   determinista y clavado a Lockstep, columna `rsp=` incluida.**

   **El problema.** Salio buscando otra cosa y la primera version de esta nota estaba MAL: se
   escribio con UNA corrida por configuracion y decia que con `CACHECOST=0` Threaded era
   determinista. Con cuatro corridas se caia. DK64 PAL, 300 campos, mismo binario, misma
   maquina:

   | modo | intercambios | syncs RDP | retiradas | origin |
   |---|---|---|---|---|
   | Lockstep x3 | 74 / 74 / 74 | 56 / 56 / 56 | 402M | 0283c0 siempre |
   | Threaded x4 | **72 / 70 / 73 / 73** | **56 / 53 / 55 / 56** | 402M | 0283c0 / 0be3c0 alterna |

   SM64 igual: 71 / 95 / 95 intercambios en tres corridas de Threaded (Lockstep clava 95).

   Las retiradas SI eran estables (402M) porque el bucle del sistema pide exactamente
   `viFieldInsns` instrucciones por campo; lo que bailaba es *donde* dentro del campo caian las
   cosas. Causa: en Threaded los workers levantaban `MI_SP` y `MI_DP` cuando terminaban en
   tiempo de **pared**, no de invitado.

   **Como se encontro.** Traza por campo (`KESTREL_FIELDTRACE=1`, `src/core/system.cpp`): al
   cerrar cada campo se imprime lo que el invitado puede ver -- retiradas, ops, ciclos de RSP,
   GCLK del RDP, armados de SP/DP, intercambios, syncs, `VI_ORIGIN`, `MI_INTR` -- y se diffean
   dos corridas para encontrar el PRIMER campo que difiere. En DK64 divergian en el campo 195 y
   solo en `gclk` y en el numero de armados de DP; el estado visible seguia igual y se rompia
   dos campos despues. Eso senalaba al *instante en que se armaba el plazo* de fin de tarea, no
   al plazo en si. Los contadores `spLate`/`dpLate` (plazos que nacen YA vencidos) lo
   confirmaron: 2-4 de cada 56 tareas de DP, y 1 de cada ~370 de SP.

   Dos hipotesis se probaron y se DESCARTARON por medida, no por argumento: apretar el
   regulador (`PACESLACK=0 PACEGRAIN=1`) solo movia la divergencia del campo 195 al 206; y
   `KESTREL_DPFAKE=1` (lecturas de DPC del RSP deterministas) tampoco la quitaba.

   **El arreglo, en dos piezas.** La regla de siempre: una tarea del RCP tiene que terminar en
   un instante de INVITADO. Armar el plazo no bastaba, porque el plazo se calcula cuando el
   worker TERMINA y para entonces el reloj de invitado podia haberse pasado -- plazo nacido
   vencido, `MI_x` publicado donde llegase el anfitrion.

   1. **Coste primero** (`Memory::rdpRunJob`): el paseo de coste (`rdpCostPass`, SoftRdp en modo
      `costOnly`) va DELANTE de `vrdp::runFifo`, con sus propios punteros de reanudacion. Antes
      iba detras y dejaba `rcp.rdpGclk` clavado durante todo el trabajo: `rdpPace` frenaba
      contra cero mientras la GPU dibujaba, y el plazo nacia vencido. Cobrando primero, el coste
      del tramo entero se conoce antes de dibujarlo.
   2. **Barreras de invitado** (`dpBarrierOps` / `spBarrierAt`, bits 2 y 3 de `rcpPend`): la CPU
      no puede pasar del instante de invitado en que la tarea en vuelo termina. Para el RDP ese
      instante se publica en cuanto el paseo de coste lo conoce; para el RSP sale de
      `Rsp::cyclesRun`, que ya es monotono y se publica cada pocos miles de instrucciones, asi
      que la barrera avanza sola con el trabajo real. `rcpDueIn` las mete en el presupuesto del
      dynarec para que un bloque no se las trague, y las dos tienen salvavidas de 20 ms: la
      fidelidad nunca puede ser una via de bloqueo. Perillas `KESTREL_DPBARRIER=0` /
      `KESTREL_SPBARRIER=0`.

   Con las dos, `spLate` y `dpLate` valen **0** en las dos ROMs. Que es la prueba de que el
   arreglo es el correcto y no un parche: ningun plazo nace vencido, o sea que ninguno se
   publica en tiempo de anfitrion.

   **Medida (300 campos, traza por campo completa):**

   | ROM | corridas Threaded | trazas identicas | Lockstep == Threaded | plazos tarde |
   |---|---|---|---|---|
   | DK64 PAL | 10 + 3 | si | si (diff = 0 lineas) | `spArm 174/0` `dpArm 56/0` |
   | SM64 USA | 4 | si | si | `spArm 388/0` `dpArm 96/0` |

   DK64 clava 74 intercambios / 56 syncs / `origin=0283c0` y SM64 95 / 96 / `3b5280`, que son
   justo los numeros de Lockstep. Puertas verdes con el arreglo dentro: `gate_all` rc=0 (460 s,
   krom interp 371/371 88.73/92.08, regress=0) y `gate_prdp` rc=0 (370 s, krom prdp 371/371
   89.27/92.56, regress=0, las dos mejoras conocidas del Cube animado).

   **Coste de rendimiento** (DK64, 900 campos, pared): con las dos barreras 11,67 s, sin la del
   RDP 10,58 s -- **~10 %**, y sin ella el determinismo se cae (415 -> 417 intercambios). La del
   RSP sale gratis (11,64 s). Sigue por encima de tiempo real (900 campos PAL = 18 s de video).

   **Lo que quedaba de este hilo: RESUELTO (2026-09-11).** `Rsp::cyclesRun` variaba entre
   corridas porque el microcodigo sondea `DPC_CURRENT`/`DPC_STATUS` y esos registros los
   publicaba el worker segun donde iba la GPU en tiempo de pared. El arreglo fue el que se
   apuntaba aqui -- publicar el estado del DPC desde el modelo de coste -- mas dos piezas que
   no se veian desde este lado:

   - **Horario en tiempo de envio.** `rdpSubmit` corre el modelo de coste bajo `rdpMx` y archiva
     el tramo entero (`dpScheduleSpan`) con instante de arranque y de cierre en un anillo de 256;
     el worker solo pinta. Todo lo que el invitado puede ver del motor -- ocupado/libre,
     `DPC_CURRENT`, `END_VALID` -- sale de ese anillo y del reloj de QUIEN pregunta
     (`dpcCurrentFor`/`dpcStatusFor`, con `Memory::rspGuestNowAt` para el lado del RSP).
   - **Aparcamiento del RSP en la espera del FIFO.** Instrumentado, el desorden de envio no
     existia (`ooo=0` siempre): la fuga era que el microcodigo sondeaba `DPC_CURRENT` desde un
     instante POSTERIOR al de la CPU y la CPU archivaba luego el siguiente buffer con
     `kick = cartNow()`, o sea ANTES de instantes ya contestados (42-47 por 300 campos). Como
     el bucle de espera de F3DEX2 no tiene efecto lateral y con el motor drenado `DPC_CURRENT`
     es constante, el RSP se aparca (`Rsp::idleSkip` + `Memory::rspParkWait`), la CPU corre
     libre, y lo despierta el `kick` del tramo siguiente -- instante de invitado -- cobrandole
     las iteraciones enteras que caben. Detalle completo en `docs/wip/README.md`.

   Medida final, DK64 PAL 300 campos, traza por campo entera: `build-prdp/` Threaded **6/6**
   byte-identico (md5 `d3bee5263f26e4dca3519e277220e090`, columna `rsp=` incluida), `build/`
   SoftRDP **4/4** (`b43236f9c027f4a0cbaf19df040e0ffc`), Lockstep 2/2
   (`ce4ec2bc2d6298cb69d270607db199ee`) e identico a la corrida con `KESTREL_RSPIDLE=0`.
   `rspCycles=60771479` clavado, `spArm=173/0 tarde`, `dpArm=56/0 tarde`, `stale=0`, `ooo=0`,
   `park=48/0`. Puertas verdes: `gate_all` rc=0 468 s, `gate_prdp` rc=0 334 s, krom regress=0
   en las dos e improve=2 en prdp.

   **Coste:** 13,9 M sondeos emulados pasan a 5,7 k, pero el aparcamiento serializa los dos
   hilos a grano de tarea y la corrida de 300 campos va de 6 s (no determinista) a 9 s SoftRDP /
   8 s Parallel-RDP. Recuperar ese solape es el siguiente punto de rendimiento y es
   independiente de la correccion.

4. **CERRADO 2026-09-17 -- modos `phys` (Lockstep+JIT) y `phys-threaded` en `gate_all.sh`**
   (`KESTREL_CPI=1.0 CACHECOST=60 UNCACHEDCOST=60 FPUCOST=stat MULDIVCOST=stat`, systemtest +
   sm64 md5). Nada mas encenderlos cazaron un plazo podrido del mismo patron: el latch del bus
   del PI (`cart-writing: Temp value decay`, variante SH) moria a las 10 vueltas porque su vida
   eran 200 ops fijas y las paradas de cache de la propia prueba (dos accesos sin cache + fallos
   de I-cache) se comian el plazo. La descarga del bus es tiempo: ahora
   `CART_LATCH_TTL_CYCLES = 330` ciclos (~70 vueltas de 3 instrucciones medidas en consola por
   n64-systemtest + los dos accesos sin cache) pasados a ops con el CPI vigente
   (`cartLatchTtl`; 235 ops de fabrica, 165 con CPI 2, 330 con CPI 1). systemtest 0/3721 en
   phys, phys-threaded, interp, jit, threaded-jit y CPI=2; statehash de jr/pd/sm/dk sin cambio.
   Texto original del pendiente:
   **las puertas no encendian la perilla.** Ni `gate_all` ni `gate_prdp` ponen
   `KESTREL_CACHECOST`, asi que los puntos de arriba pueden pudrirse sin que nadie se entere;
   el del SI vivio asi hasta que se busco otra cosa. Ya no hay excusa de determinismo: con el
   3b resuelto, Threaded tambien clava la traza, asi que el modo que se anada puede ser de los
   dos. Cuando el modelo de CPI este calibrado, la perilla tiene que entrar en al menos un modo
   de la bateria.

El patron general a vigilar: **un plazo tiene que nacer y morir en el mismo reloj.** Vale para
el latch del PI (`cartLatchExpiry`, hoy correcto: `cartNow()` en los dos lados), para el SI y
para el planificador de eventos con marca de tiempo que esta pendiente de escribir.


## Medida de la FPU, ya con el coste de cache funcionando (2026-09-10)

Hasta hoy no se podia medir: SM64 se colgaba con `KESTREL_CACHECOST` encendido (ver el fallo de
los dos relojes en `docs/STATUS.md`). Arreglado eso, tanda de 600 campos de video con JIT,
`CACHECOST=60`, `FPUCOST=stat` contra `block`:

| Juego | modo | fallos D$ | ops FP / retiradas | ciclos parados / retiradas | CPI con base 1,0 |
|---|---|---|---|---|---|
| SM64 | stat  | 0,622 % | 0,888 % | 0,377 | 1,377 |
| SM64 | block | 0,855 % | 1,381 % | 0,567 | **1,567** |
| DK64 | stat  | 0,373 % | 0,508 % | 0,227 | 1,227 |
| DK64 | block | 0,378 % | 0,512 % | 0,250 | **1,250** |

La FPU aporta **+0,048 de CPI en SM64 y +0,019 en DK64**, y eso es la COTA SUPERIOR (`block`
cobra la latencia entera siempre; el hardware solapa todo el trabajo entero independiente que
haya detras). O sea que el modelo fino de enclavamiento por registro FP puede aportar como
mucho eso, y menos en la practica.

**Consecuencia para el hueco de Perfect Dark: la FPU tampoco lo explica.** Los tres sospechosos
del CPI (KSEG1, mul/div entera, FPU) suman juntos menos de 0,1 de CPI, y a PD le faltaban 0,29
(1,158 medido contra 1,45 de cota inferior).

**El que queda en pie es que la muestra de PD es invalida**: 6799 ops FP y 15 728 mul/div
enteras en ~1,5-2,2 mil millones de instrucciones retiradas es una pantalla de TITULO, no un
motor 3D. Cada cifra de CPI de PD publicada hasta ahora compara un titulo quieto contra un
suelo medido en juego. Antes de tocar la base hay que rehacer la medida de PD **dentro de un
nivel**, no en el menu.

DK64 en cambio si cuadra: 1,250 con base 1,0 contra su cota inferior de 1,19.

## Lockstep y Threaded no dan el mismo rastro en DK64 (2026-09-11)

`KESTREL_FIELDTRACE=1`, DK64 PAL 300 campos, `build-prdp`, JIT. Cada modo es determinista
consigo mismo — Threaded `4fc7dc59e262d5a8a0cd2c89712b8ae3` 3/3 (y el mismo md5 en SoftRDP 2/2),
Lockstep `8c18b4c8cfbd806a1cbb8f4d5168cc2d` 2/2 — pero **entre ellos difieren 79 lineas de 300
desde f=191**, y la primera columna que se mueve es `syncs=` (`rcp.dpSyncs`): Lockstep va un
SYNC_FULL por delante (`dp=2 syncs=2` contra `dp=1 syncs=1`) y a partir de ahi el desfase se
mantiene.

Es anterior al cambio de columnas del 2026-09-11 (`syncs` no lo toca ese cambio). En Lockstep
los dos chips comparten hilo y el RSP corre intercalado con la CPU, asi que el instante de
invitado en que se archiva cada tramo del FIFO cae en otro sitio; que eso mueva *cuando* vence
un SYNC_FULL es esperable, que lo mueva un campo entero no esta explicado.

Que NO es: los dos modos pasan las puertas con los mismos md5 de framebuffer (sm64
`d35bd8aa9b13d459ce9332c07a79a53a` interp / `b5521b24d8fc280fbf102df22d7d30cb` prdp en los ocho
modos), o sea que la imagen coincide; lo que no coincide es el reparto por campo.

Por donde empezar: `[ds]` (`KESTREL_DPSCHED=1`) de las dos corridas, primer tramo cuyo `kick`
difiera. El horario es todo tiempo de invitado en los dos modos, asi que la diferencia tiene que
estar en el instante en que la CPU llega a la escritura de `DPC_END`, no en el coste.

## junkrunner64 no es determinista en Threaded a 3e9 instrucciones (2026-09-18, ABIERTO)

Salio buscando otra cosa (el `#DE` de `aiArm`, ya arreglado) y sobrevive al arreglo.

`KESTREL_PRDP=1 KESTREL_AUDIO=0 KESTREL_MAXINSN=3000000000`, borrando el estado del invitado
entero antes de cada corrida (`.eep`/`.sra`/`.fla` **y el `.mpk` del Controller Pak**, que la
primera version de la prueba se dejaba puesto y es estado persistente que el invitado lee):

| modo | corridas | `[statehash]` |
|---|---|---|
| Lockstep (`KESTREL_THREADS=0`) | 3 | `17c2962b082d80b9` las tres |
| Threaded | 3 | `47d2193726e88bcc`, `3c9a4e01ffb4732c`, `47d2193726e88bcc` |

O sea: Lockstep es determinista, Threaded no. Las corridas terminan con `rc=0` y cero
`HOST EXCEPTION` desde el arreglo de la mascara AI, asi que no es el fallo del anfitrion; y no
es el Controller Pak, porque se borra. El rastro de las corridas divergentes muestra al
invitado saltando a direcciones imposibles (`jl 0xa0020ee8 -> 0xea242004`), o sea que el
programa se ha descarrilado de verdad, no es solo la huella.

Alcance, que importa para no exagerarlo: **el punto de validacion normal no lo ve**. A 400 M
instrucciones jr sigue dando el mismo `[statehash]` en Lockstep y en Threaded, y es lo que
cruzan las puertas. Esto aparece mucho mas tarde, con una ROM cuyo proposito es meter basura
en los registros del RCP.

Por donde empezar, en este orden (son las perillas que deciden el orden entre hilos):
`KESTREL_SPLOG=0`, `KESTREL_DPLOG=0`, `KESTREL_DMALOG=0`, `KESTREL_DMARDV=3/0`,
`KESTREL_DPBARSYNC=0`, `KESTREL_RSPDPAWAIT=1`. La que vuelva a hacerlo determinista senala el
camino por el que se cuela un instante de invitado que no se esta clavando. Despues,
`KESTREL_FIELDHASH=1` para acotar el campo exacto en que las dos corridas se separan.

### Descartado: los salvavidas de PARED no son la causa (2026-09-18)

La sospecha mas gorda era `Memory::rdvWaiveDue` (memory.cpp): cuando una cita se alarga, la
suelta por reloj de ANFITRION -- 20 ms si la CPU esta esperando al RCP, 2000 ms si no --, y una
sola renuncia basta para que las dos corridas se separen. **Medido y descartado.** Tres corridas
a `KESTREL_MAXFIELDS=2600` y tres a 1200, con la telemetria nueva `[pared]` puesta:

```
[pared] renuncias sp=0 dp=0 diario=0 barSP=0 barDP=0 aparcado=0/...
```

Cero en las seis, en los seis contadores, y aun asi divergen. Que divergen de verdad y no es
la huella lo dice `[dplog] apuntadas`, que es cuenta de INVITADO: 2403074 / 2041802 / 2401940.

Con `KESTREL_FIELDTRACE=1` la primera linea distinta sale en el campo 715 de 1200:

```
A: [ft] f=715 ret=799741361 ops=799741361 sp=315 dp=334 flips=330 syncs=334 org=0a1480 mi=1c
B: [ft] f=715 ret=799748782 ops=799748782 sp=315 dp=334 flips=331 syncs=334 org=0c6cc0 mi=10
```

Mismo campo de video, distinto numero de instrucciones retiradas, distinto `vi_origin` y
distinta mascara de MI. Las tareas de SP y DP van igualadas (315/334 en las dos), asi que **no
es que el RCP haya hecho mas o menos trabajo: es que el invitado tomo otro camino**. Otro par
de corridas se separo en el campo 836.

Aparecio de paso un defecto REAL pero distinto: una corrida de la tanda de 2600 dio
`[det] spArm=329/1 tarde`, o sea un plazo de fin de tarea del SP **nacido ya vencido**, que es
exactamente lo que avisa el comentario de `kRdvLead`. Cuando eso pasa, `MI_SP` cae donde haya
llegado el anfitrion. No es la causa unica -- las otras dos corridas dieron `0 tarde` y tambien
divergieron -- pero hay que cerrarlo igual. Para eso esta ahora `[spvenc]`, que reparte los
vencidos por escotilla (adelanto de la cita / tope del aparcamiento / otro) y da el mayor
rebase en ops.

Siguiente corte, ya escrito: cuatro brazos `prdp` / `prdp+SYNCRDP` / `soft` / `soft+SYNCRDP`.
La hipotesis viva es el RDP: con Parallel-RDP el motor lee pixeles, texturas y TLUT de la RDRAM
**viva** y escribe color/z desde la GPU en tiempo de ANFITRION, y junkrunner64 programa
`SET_COLOR_IMAGE` a direcciones arbitrarias a proposito -- o sea que el RDP pinta encima de los
datos del propio invitado, y cuando eso aterriza respecto a las lecturas de la CPU lo decide el
reloj del anfitrion. Si con `SYNCRDP=1` (RDP sincrono) deja de divergir en los dos backends,
es eso.

## Crecimiento de memoria con el invitado descarrilado (2026-09-18, SIN REPRODUCIR)

Durante la caceria de arriba quedo un `kestrel64.exe` huerfano con **5,2 GB** de conjunto de
trabajo, suficiente para que el sistema matara otros procesos. Al volver a medirlo no
reproduce: siete corridas de junkrunner64 a 3e9 instrucciones muestreadas cada 15 s se quedan
planas en **282 MB de WS / 608 MB privado** de principio a fin.

Descartado por lectura de codigo como origen posible de esa cifra: el buffer de codigo emitido
del dynarec esta topado en 16 MB y se recicla entero (`CodeCache::clear`), la sombra del FIFO
son dos buffers del tamano de la RDRAM (`rdpShadow[2]`), el anillo de eventos es de 16384
entradas fijas y `dpLog`/`dmaPay` son anillos de tamano fijo. Queda por mirar el lado de
parallel-rdp y el camino con ventana/presentacion, que es el que NO tenia la corrida que se
midio plana.
