# kestrel64 — Estado actual

Estado a 2026-10-30. Este fichero es la referencia corta del estado del emulador: qué
funciona hoy en cada subsistema, las cifras vigentes, dónde está la documentación de
profundidad y cuál es la limitación principal de cada parte. No es un diario: el registro
cronológico de lo hecho hasta ahora está en `docs/history/STATUS-2026.md`, y la lista de
huecos abiertos, con su número, en `docs/GAPS.md`. Cuando un subsistema cambie, se actualiza
su sección aquí y el detalle va al documento de profundidad correspondiente.

## Resumen

- Emulador N64 LLE en C++20 (clang, MSYS2 CLANG64), con hilos reales para CPU, RSP, RDP y
  audio, dynarec de CPU y de RSP activos de fábrica, y dos rasterizadores propios: SoftRDP
  (oráculo bit a bit) y GPU-RDP (Vulkan compute, el de fábrica).
- Fidelidad: n64-systemtest sin fallos (0/3721, Timing 0/2, Cycle 0/6); Super Mario 64 en
  SoftRDP idéntico bit a bit a parallel-RDP; batería krom de 371 ROMs con media exacta ~89.
- Se distribuye como un único `kestrel64.exe` enlazado estático, con menú propio, biblioteca
  de juegos, servidor MCP para telemetría y depuración, y juego en red.

## CPU (VR4300)

- **Intérprete** (`src/cpu/cpu*.cpp`): MIPS III de 64 bits con delay slots, COP0 completo,
  FPU con redondeo y banderas, TLB de 32 entradas con sus excepciones, cachés I y D modeladas
  con tags, y excepción Watch. Es el oráculo: el dynarec llama a sus helpers en lo difícil.
- **Dynarec x86-64** (`src/cpu/jit*.cpp`), activo de fábrica (`KESTREL_JIT=0` lo apaga):
  residencia de registros, memoria rápida con guardas, enlace de bloques, caché de destinos
  indirectos, bloques que cruzan página, COP0 en bloque y control de código automodificable
  por línea de I-caché. El nivel 2 del plan se cerró el 2026-10-01.
- **Modelo de ciclos**: de fábrica, CPI plano de 1,4 (`KESTREL_CPI=2` es el valor histórico).
  Existe un modelo físico opcional (modo `phys`: CPI 1,0 más coste de fallo de I-caché,
  D-caché y acceso sin caché, latencias de MULT/DIV y FPU por tabla, y enclavamientos), con
  sus perillas `KESTREL_ICACHECOST`, `DCACHECOST`, `UNCACHEDCOST`, `FPUCOST`, `MULDIVCOST` e
  `INTERLOCK`. Está apagado de fábrica y cubierto por los modos `phys` de `gate_all.sh`.
- **Memoria de 4 MB** seleccionable con `KESTREL_RDRAM=4|8`.
- **PGO de fábrica**: perfil `pgo/kestrel.profdata` commiteado. Medido el 2026-09-24 en un
  i7-870: Perfect Dark -7,1 %, Super Mario 64 -8,6 %, DK64 -4,2 %, con statehash idéntico.
  Se regenera con `sh scripts/pgo.sh`; es solo de desarrollo (no está en la interfaz).
- **Velocidad** (2026-09-24, i7-870, antes del PGO): Perfect Dark PAL en juego 1,13x tiempo
  real, Super Mario 64 2,24x, DK64 2,70x. La velocidad se mide con `bench`, nunca con Mips.
- Profundidad: `docs/CORES-CPU-RSP.md` (§2-3), `docs/PERF-CPU.md`, `docs/JIT-PLAN.md`
  (plan original, histórico).
- Limitación: falta una comparativa nueva lado a lado contra ares (la única es de
  2026-08-20; GAPS, Rendimiento). Pendiente de baja prioridad: enlace y EPC con direcciones
  virtuales de 64 bits (GAPS, Fidelidad).

## RSP

- **LLE puro**: intérprete con unidad vectorial en SSE4.1 (`src/rsp/rsp.cpp`) y **RSP JIT**
  activo de fábrica (`KESTREL_RSPJIT=0` lo apaga), con las etapas 1, 2, 2b y 3 hechas:
  línea recta, saltos absorbidos, COP0 en bloque y enlace por tabla directa de 1024 ranuras.
  Partes de COP2 se emiten en línea. Mismo md5 de framebuffer con el JIT encendido y apagado
  en todos los modos de `gate_all.sh`.
- **Modelo de tiempos del pipeline** opcional (`KESTREL_RSP_TIMING=1`, desde 2026-10-01):
  solo contabiliza ciclos, fuerza el intérprete y no mueve el reloj global.
- Puntos de ruptura y desensamblado del RSP disponibles en el depurador (2026-10-23).
- Profundidad: `docs/RSP-JIT.md`, `docs/CORES-CPU-RSP.md` (§4-5).
- Limitación: no hay HLE de microcódigo gráfico, por decisión consciente (GAPS #7).

## RDP

- **SoftRDP** es el oráculo bit a bit: LOD y mipmaps, corrección de perspectiva,
  interpolación entera, delta-z, intercambio de filas impares en TMEM y antialiasing por
  cobertura. Super Mario 64 es idéntico a parallel-RDP desde 2026-10-03 (md5 en
  `docs/baselines/sm64.txt`). En la batería krom (371 ROMs) la media exacta ronda 89 y está a
  la par de parallel-RDP en la carpeta RDP (2026-10-08).
- **GPU-RDP** propio (Vulkan compute, `src/gpurdp/`), rasterizador de fábrica desde
  2026-10-06. Escalado interno x2, x4 y x8 (x8 desde 2026-10-30), cambiable en caliente, y 4x
  automático al partir pantalla.
- **Modo sombra** (2026-10-30): la CPU pinta a 1x y la GPU produce la copia de alta
  resolución de forma asíncrona. Super Mario 64, 100 intercambios: x2 5,9 s, x4 5,9-6,0 s
  (parallel-RDP 6,1 s), x8 7,2 s (parallel-RDP 52,5 s). `KESTREL_GPURDP_HISYNC=1` vuelve al
  camino anterior. A 1x la diferencia de velocidad con parallel-RDP está en el ruido (Super
  Mario 64 ~0-3 %, DK64 ~1-1,5 %).
- **Texturas HD** en formato Rice/GLideN64 (`KESTREL_TEXPACK`, `TEXDUMP`, `TEXFX`), en
  SoftRDP y GPU-RDP, activables en caliente desde 2026-10-30.
- **parallel-RDP retirado** de menú, lanzador, `release.sh --gates` y pdbench el 2026-10-30
  (fase 1). Solo queda a mano con `KESTREL_PRDP=1`, con aviso. La fase 2 (borrar
  `third_party/`, la parte Granite de `src/vrdp/`, `gate_prdp.sh` y sus baselines) está
  pendiente: GAPS #15 y `docs/GPU-RDP.md`, Fase 7.
- **Coste del RDP en tiempo de invitado**: modelo actual con rmse 0,1332 contra las medidas
  de Thar0; falta el coste de setup por primitiva y por span. Hay datos de consola
  (2026-10-24) pero no se calibra todavía. `KESTREL_RDP_STRICT=1` avisa de escrituras a
  DPC_START/END durante SYNC_FULL. Desde 2026-11-02 el paseo de coste lleva sombra de z
  (píxeles muertos por el test de z ya no se cobran como escritos; `KESTREL_RDPCOST_Z=0`) y
  `rdp.stats` da `pixelDetail` (px por chunk/línea, gclk/px, parte de z, IM_RD, primitivas
  por tamaño).
- Profundidad: `docs/GPU-RDP.md`, `docs/FIDELITY.md`, `docs/RDP-COVERAGE-AA.md`,
  `docs/RDP-TIMING.md`, `docs/TEXTURAS-HD.md`, `docs/TEXTURE-FORMATS.md`.
- Limitaciones: falta el contorno cel por profundidad en texturas HD (GAPS #6); casos
  abiertos de snapper64; calibración del coste del RDP (GAPS, «Coste del RDP»).

## VI, vídeo y presentación

- Presentador GLFW/Vulkan; sin Vulkan, presentación GDI (`KESTREL_FORCEGDI=1` la fuerza).
- El filtro del VI (antialiasing, dedither, divot) se aplica en el presentador.
- Filtros de imagen en caliente con `KESTREL_FILTER`: FSR 1.0, Anime4K, nitidez, CRT, cel y
  óleo (`docs/FILTROS-IMAGEN.md`). Aspecto 4:3, 16:9 o estirado, y SSAA
  (`docs/ESCALADO-Y-ASPECTO.md`). Reloj del VI: `docs/VI-CLOCK.md`.
- **Captura** (2026-10-30): PNG con F9; vídeo con F8, en MP4 H.264 + AAC por Media
  Foundation o AVI MJPEG + PCM, a tiempo de invitado. También por MCP (`screenshot`,
  `video_*`) y con `KESTREL_RECORD` (`src/video/capture.hpp`).

## Audio

- Salida waveOut desacoplada del núcleo; AI_LEN y AI_STATUS se mantienen al día
  (2026-10-23). Con el CPI de fábrica de 1,4 el silencio medido es 0 %.
- Diagnóstico: `KESTREL_AUDIOSTAT` y volcado a WAV con `KESTREL_WAVDUMP`.
- Limitación: solo Windows (GAPS #8, multiplataforma).

## Hilos y modelo de tiempo

- Hilos reales para CPU, RSP, RDP y audio, activos de fábrica (`KESTREL_THREADS`). El modo
  lockstep es el oráculo determinista.
- Agenda de eventos por instante (VI, AI, SI y plazos de PI, desde 2026-09-18); los DMA de SI
  y PI tienen duración, y el RDP cuesta tiempo de invitado (`rdpPace`).
- **Adelantos**: `KESTREL_SPLEAD` (auto = 512) y `KESTREL_DPLOGLEAD` se usan en velocidad
  libre y valen 0 con `KESTREL_SPEEDMODE=hw` («fiel a consola») y en las puertas. Con ambos a
  0, threaded da el mismo statehash que lockstep. SPLEAD acelera Perfect Dark un 25 %, DK64 un
  15 % y Super Mario 64 un 8 %, pero no es fiel ni reproducible.
- Profundidad: `docs/ARCH-THREADING.md`, `docs/ARCH-SYNC.md`, `docs/PERF-RCP-SYNC.md`,
  `docs/HW-RCP-COMM.md`.
- Limitación: el reparto diario y la cita del DMA del SP aún dependen del anfitrión (GAPS,
  sección 2026-09-24).

## Memoria, bus, DMA, PIF, partidas y SummerCart64

- Cuatro mandos con Rumble Pak, Controller Pak y Transfer Pak (MBC1/2/3 con RTC y MBC5).
  Regiones PAL, NTSC y MPAL.
- Partidas: EEPROM, SRAM (incluida la de 768K en bancos de 256 KiB), FlashRAM con el
  protocolo de libultra, y sram1m. Cabecera ED de homebrew y metadatos de n64brew
  (2026-10-30). ROMs sueltas o dentro de zip/gzip (`docs/ROMS-COMPRIMIDAS.md`).
- PIF con modo de arranque, suma IPL2 real y CIC detectado por firma (GAPS #14 cerrado:
  `pif_test` 23/23 y 59/59 ROMs locales). Botón de reset.
- **SummerCart64 v2** con tarjeta SD: la carpeta de la ROM se presenta como FAT32 virtual
  (`KESTREL_SD`, `docs/FLASHCART-SD.md`). El estado del SC64 no viaja en los savestates.
- Trucos GameShark (`docs/CHEATS.md`).
- Limitación: falta el 64DD (GAPS #9).

## Savestates, rebobinado y TAS

- Savestates en versión 22 (carga v14 a v21), ranuras 0-9, atajos F5, F6 y F7. Solo se toman
  en un reposo natural del RCP (`docs/SAVESTATES.md`).
- Rebobinado apagado de fábrica; con foto cada 2 campos cuesta un +28 % (`docs/REWIND.md`).
- Grabación y reproducción TAS en formato `.k64m` (`docs/TAS.md`).
- Limitaciones: falta un mapa de páginas sucias para abaratar el rebobinado (GAPS,
  Rendimiento); un estado guardado antiguo de Perfect Dark en juego ya no carga (GAPS,
  2026-09-24).

## Telemetría, MCP y depurador

- Servidor MCP propio en el puerto 9128 con puente `tools/mcp/kestrel_mcp.py`
  (`docs/MCP-GUIA.md`). Símbolos con `--symbols`, estadísticas `rdp.stats` y profilers
  (`profile_cpu` con métricas de fallos de caché, solo con `KESTREL_JIT=0`).
- **Depurador** (2026-10-23): grabadora de RDP y RSP, línea de tiempo, visor de TMEM y TLB,
  `ev.log`, `mem.watch`, `usb.log` del SC64, carga en caliente (`code`/`rsp`/`rom.load`) y
  ventana ImGui con `--debugger` (`docs/DEBUGGER.md`).

## Interfaz

- Menú Win32 en el propio exe, biblioteca con carátulas y ficha de juego (información,
  manual, trucos, partidas y pestaña Homebrew). Biblioteca de trucos (libretro-database,
  CC BY-SA) y de manuales; las descargas en línea están apagadas de fábrica.
- Opciones en caliente (trucos, rebobinado, texturas HD) por `options_live`.
- La carpeta de ROMs se explora de forma recursiva (hasta 8 niveles).
- Lanzador web Python congelado, `kestrel64-gui.exe` (`docs/LAUNCHER.md`).
- Limitación: queda retirar el lanzador y llevar todo al exe (GAPS, anotado 2026-10-24).

## Juego en red

- Lockstep de entradas con retardo `KESTREL_NETPLAY_DELAY` (2 de fábrica), anfitrión más
  hasta 3 invitados, estado común al empezar y hash cada 60 campos con resincronización.
- Pantalla partida (F4) a elección del anfitrión. Relé con código de sala
  (`kestrel64 --relay`, puerto 6465) y UPnP. GAPS #10 cerrado el 2026-10-30.
- Limitaciones: sin rollback ni NAT sin relé; el mapeo UPnP real está sin probar en una red
  con UPnP.

## Build y distribución

- clang de MSYS2 CLANG64, todo estático desde 2026-09-30: solo se importa `vulkan-1.dll` del
  sistema, y `cmake/check_static.cmake` rompe el build si aparece libc++, libunwind, glfw3 o
  libwinpthread.
- `sh scripts/release.sh` regenera los cuatro árboles (build, build-prdp, build-static,
  build-prdp-static), el lanzador congelado, el zip y el instalador Inno Setup, y deja
  `dist/VERSION.txt`. Opciones: `--gates` (gate_all + gate_gpurdp), `--quick`, `NOISS=1`.
- El paquete trae un solo exe desde 2026-10-05; GPU-RDP y SoftRDP se eligen en
  Video > Rasterizador, y sin Vulkan cae a SoftRDP con presentación GDI. Doble clic arranca
  jugando (`--play`); desde la consola arranca en pausa para el MCP.
- Para cerrar procesos propios, `scripts/killown.sh` (nunca por nombre de imagen).
- Profundidad: `docs/distribucion.md`.

## Verificación

- `scripts/validate.py`: systemtest, Super Mario 64, krom y bench. Fija los adelantos a 0.
- `scripts/gate_all.sh`: oráculo SoftRDP en los modos interp, jit, jit-nolink, threaded y
  threaded-jit, más rspinterp, rspnolink, rewind-rtt, phys y phys-threaded, krom y tests
  unitarios.
- `scripts/gate_gpurdp.sh`: puerta específica del GPU-RDP.
- `scripts/gate_quick.sh [krom] [lock] [phys] [thar0]`: puerta rápida, ~1,5 min.
- `scripts/gate_prdp.sh`: en retirada junto con parallel-RDP (GAPS #15).
- Política desde 2026-10-06: probar solo lo cambiado con una prueba dirigida; las baterías
  completas solo cuando se piden.
- Baselines en `docs/baselines/` (`sm64.txt`, `krom-interp.tsv`, `krom-prdp.tsv`,
  `sm64-prdp.txt`, `timings.md`, `pd-opt.md`). El invariante es la igualdad entre modos, no
  un hash literal: el statehash se compara con SPLEAD=0 y DPLOGLEAD=0, threaded contra
  lockstep.
