# kestrel64 — CLAUDE.md

From-scratch **multithreaded, telemetry-native N64 emulator** (C++20, clang). Main
active project of this workspace. Built to run N64 games (esp. Perfect Dark) fast on
weak-single-thread / idle-GPU hosts, where cooperative-single-thread cycle-accurate
emulators (ares, cen64) hit an architectural ceiling.

Estado actual por subsistema: `docs/STATUS.md` (corto, se actualiza EN SITIO). Diario
cronologico: `docs/history/STATUS-2026.md` (las entradas nuevas con fecha van al FINAL de
ese fichero, no a STATUS.md). Backlog de huecos y pendientes: `docs/GAPS.md` (lo que
falta frente a ares/PJ64/mupen + pendientes propios, ordenado; se actualiza al cerrar
cada punto). Peticiones de la optimizacion de Perfect Dark al emulador:
`PD64_pending.md` (P1..P8, tachar al cerrar; **local, en .gitignore**: el repo es PUBLICO).
Configuracion de la maquina (python, carpeta y nombres de ROMs de prueba, ares): `scripts/local.env`
(ignorado; plantilla `scripts/local.env.example`), que leen `scripts/localenv.sh` y
`scripts/romcfg.py`. **Nunca** rutas personales ni nombres de ROM comerciales en ficheros
versionados. Design docs: `docs/ARCH-THREADING.md`, `docs/JIT-PLAN.md`,
`docs/TEXTURE-FORMATS.md`, `docs/VI-CLOCK.md`,
`docs/RSP-JIT.md`, `docs/ROMS-COMPRIMIDAS.md`.

## Architecture bet

- **Multithreaded from line one** — real OS threads for CPU / RSP / RDP / AI, not
  cooperative cothreads. Attacks the "2/8 cores" ceiling ares hits by design.
- **Telemetry-native core** — length-framed JSON+binary server baked in (port 9128). Bulk
  data (RAM dumps, framebuffers) rides as raw bytes, zero hex overhead. MCP bridge
  `tools/mcp/kestrel_mcp.py` exposes it. Development driven by full live introspection.
- **HLE-capable RSP** — LLE interpreter for correctness + custom microcode (PD ships
  custom ucode); HLE fast paths for standard microcodes as the perf win.
- **Own RDP, bit-exact**: RDP = **GPU-RDP** (own, Vulkan compute, bit for bit SoftRDP) +
  **SoftRDP** (CPU, the oracle). VU = shared
  SSE4.1 reference (same math ares/cen64 use, systemtest-verified).

## Build

Toolchain = clang at `/c/msys64/clang64/bin` (MSYS2 CLANG64). Static-linked exe.

```bash
export PATH=/c/msys64/clang64/bin:$PATH
cd <workspace>/kestrel64
sh scripts/release.sh          # <-- ESTE. Regenera los CUATRO arboles + lanzador + paquete.
```

**`cmake --build <arbol> -j` a secas NO vale como "he compilado"**: deja los otros arboles
con el .exe de hace semanas, y el que el usuario ejecuta (`build-gpu-static/`) es
justamente uno de los que se queda atras. Compilar un arbol suelto solo es legitimo como
paso intermedio de una prueba A/B; en cuanto el cambio se da por bueno, `release.sh`.

- **NEVER `taskkill //IM kestrel64.exe`**: otra sesion (PD-opt) corre kestrel a la vez. Antes de
  recompilar, `sh scripts/killown.sh` (solo mata exes con ruta dentro de ESTE repo; ya lo llaman
  `release.sh`/`pack.sh`/`pgo.sh`). A mano: por PID o con `timeout`, y puerto propio (`--port`).
- cmake binary lives in `/c/msys64/clang64/bin`.
- `build/` y `build-static/` = `-DKESTREL_GPU=OFF` (SoftRDP puro, el nucleo determinista no
  depende de GPU). `build-gpu/` y `build-gpu-static/` = `-DKESTREL_GPU=ON` (GPU-RDP). Opcion
  cmake `KESTREL_GPU` (defecto ON).
- **TODOS los arboles enlazan estatico (desde 2026-09-30)**: libc++/GLFW dentro, solo
  vulkan-1.dll del sistema. Un exe dinamico moria rc=127 sin clang64 en PATH (Explorador,
  lanzador, python) -- paso repetidas veces. `cmake/check_static.cmake` (POST_BUILD) FALLA el
  build si el exe importa libc++/libunwind/glfw3/libwinpthread. Opt-out solo a proposito:
  `-DKESTREL_DYNAMIC=ON`. `KESTREL_STATIC` ya no decide nada. `sh scripts/dist.sh` stages
  `dist/` + zip. See `docs/distribucion.md`.
- **REGLA: cada generacion de .exe genera TODO** — los cuatro arboles, el lanzador grafico
  congelado, el zip portable y el instalador. Un solo comando, `sh scripts/release.sh`
  (`--gates` para pasar `gate_all` + `gate_gpurdp`, `--quick` para saltarse `build/` y
  `build-gpu/` e iterar solo sobre los estaticos, `NOISS=1` para omitir el instalador). Deja
  `dist/VERSION.txt` con version, commit y md5 para que un zip suelto diga de donde salio.
- Desde 2026-10-06 ese exe arranca con **GPU-RDP** (propio) de fabrica.
  `-DKESTREL_GPU=ON` es la opcion de cmake que compila GPU-RDP. Variable de entorno:
  `KESTREL_GPURDP=0` elige SoftRDP (las puertas la fijan); `1`/defecto = GPU-RDP.
- El paquete lleva **un** ejecutable (desde 2026-10-05): `kestrel64.exe` (`build-gpu-static`)
  trae GPU-RDP y SoftRDP, elegibles en Video > Rasterizador (`KESTREL_GPURDP=0`), y sin
  Vulkan cae a SoftRDP con la ventana pintada por GDI (`KESTREL_FORCEGDI=1` lo fuerza).
  `build-static` sigue existiendo para las puertas pero no viaja. Ver `docs/distribucion.md`.
- La carpeta de ROMs se recorre **recursiva** (biblioteca del exe y lanzador), tope 8 niveles,
  sin seguir enlaces/uniones ni carpetas ocultas o de sistema.
- Las puertas por si solas recompilan `build/` y `build-gpu/` y **nunca**
  `build-gpu-static/`, que es el exe que viajan lanzador e instalador: por eso existe
  `release.sh`. Sus piezas sueltas siguen ahi si hace falta una a mano: `scripts/pack.sh`
  (estatico -> `dist/` -> zip -> Inno Setup), `scripts/dist.sh` (empaquetar un build ya
  hecho), `scripts/gui.sh` (congelar solo el lanzador).
- **PGO (compilacion guiada por perfil): PUESTA de fabrica.** `release.sh` compila los cuatro
  arboles con `-fprofile-use=pgo/kestrel.profdata` **si ese fichero existe** (esta commiteado,
  445 KB); si falta, todo compila como siempre. MEDIDO 2026-09-24 (min de 4, intercalado,
  i7-870): **PD -7,1 % (6409 -> 5956 ms), SM64 -8,6 % (7318 -> 6688), DK64 -4,2 % (4981 ->
  4774)**, y la propia puerta `gate_all` 760 -> 720 s. **No toca
  la semantica del invitado** -- es colocacion de codigo del ANFITRION --: el statehash sale
  identico en los tres juegos, y las dos puertas pasan RC=0 con regress=0. El perfil no hace
  falta por juego: uno de PD ya mejora DK64, porque lo caliente es el codigo COMPARTIDO
  (despachador del JIT, ayudantes de memoria, cita CPU<->RSP, RSP).
  - Regenerarlo: `sh scripts/pgo.sh` (lote de los tres juegos) o, para capturar jugando de
    verdad, `sh scripts/pgo.sh --capture "<rom>"`, que construye el instrumentado y lo lanza
    con `--pgo-capture`; se juega un nivel, se sale normal y el `.profraw` cae en `pgo/raw/`.
    `sh scripts/pgo.sh --merge` fusiona lo que haya ahi sin volver a correr nada. Varias
    capturas se suman. `KESTREL_PGO_NAME` etiqueta una.
  - **Solo desarrollo, NO en la interfaz** (retirado 2026-09-29): la opcion `pgocap` del
    lanzador/menu se quito. El perfil es UNO global, horneado al compilar el paquete; el
    usuario final no tiene compilador, asi que capturar desde `dist/` no servia de nada, y
    recompilar por juego no tiene sentido. El `.profdata` no contiene nada del juego (solo
    contadores de funciones/ramas de kestrel; comprobado: 0 cadenas de ROM).
  - El binario instrumentado va **1,24x mas lento** (PD en juego 7377 contra 5951 ms), o sea
    0,90x tiempo real en el juego mas apretado. Se juega para capturar; NO se distribuye.
  - Un perfil viejo no rompe nada (clang avisa y sigue, con `-Wno-profile-instr-out-of-date`),
    solo rinde menos. Rehacerlo tras un cambio grande de codigo caliente.
  - La via de coste cero (AutoFDO: muestrear el binario NORMAL) necesita muestreo por hardware
    (LBR / Intel PT) y cadena de herramientas que en Windows no esta. Anotado, no disponible.
- Double-clicking the exe implies `--play` (run + window + ROM picker); launching from a
  shell keeps the paused-for-MCP default, so gates and debugging are unaffected.

## Verification — run after EVERY change (hard rule)

Everything runs from one script; do not hand-roll the invocations.

```bash
python scripts/validate.py systemtest --mode <mode>   # 0/3721 · 0/2 · 0/6
python scripts/validate.py sm64       --mode <mode>   # framebuffer md5
python scripts/validate.py krom                       # 371-ROM RDP accuracy vs baseline
python scripts/validate.py bench      --mode <mode>   # wall clock for N VI fields (speed)
sh scripts/gate_all.sh                                # las cinco modalidades + krom, un log
sh scripts/gate_gpurdp.sh                             # backend GPU-RDP, exe de build-gpu
sh scripts/gate_quick.sh [krom] [lock] [phys] [thar0] # puerta RAPIDA por cambio (~1,5 min)
```

**Politica de puertas (usuario, 2026-10-06, MANDA SOBRE LO DEMAS)**: probar SOLO lo que toca
el cambio, con una prueba dirigida de segundos (la ROM/escena/modo afectado). Las baterias
(`gate_*`, krom completo) solo cuando el usuario las pida, tampoco "al cerrar fase". Guia de
la prueba minima: `thar0` si toca coste RDP, una ROM krom afectada si toca rasterizador,
`lock`/`phys` si toca CPU/RCP, statehash junkrunner64 (SPLEAD=0 DPLOGLEAD=0, THREADS=0 y 1)
si toca citas RCP. Lo de abajo describe que cubre cada puerta, no cuando lanzarla.

`gate_all.sh` es el oraculo determinista y corre sobre SoftRDP, que no depende de que
haya GPU. `gate_gpurdp.sh` corre el material RDP contra el exe de `build-gpu/` (GPU-RDP,
bit a bit SoftRDP). parallel-RDP (Themaister) se borro el 2026-11-02; su historia esta en git
(commits anteriores a esa fecha).

**Speed is measured with `bench`, never with Mips.** In threaded mode a faster CPU
thread just burns more spin-wait instructions, so Mips can rise while the emulator
gets slower. `bench` fixes the guest work (N VI fields) and times the wall clock.

Modes: `interp`, `jit`, `jit-nolink`, `threaded`, `threaded-jit` (plus
`threaded-trace`, `threaded-nolink`; en `gate_all` ademas `rspinterp`, `rspnolink`, `rewind-rtt` y
`phys`/`phys-threaded` = modelo fisico de ciclos encendido, CPI 1,0 + costes de cache/FPU/MUL-DIV). A change is done when systemtest and sm64
pass in **all five** and krom shows no regression.

**The sm64 gate stops on VI buffer swaps, not on an instruction count**
(`--sm64-flips`, default 60). A fixed instruction cap is not a deterministic point
of the game in threaded mode: the RCP runs on its own threads, so the number of
instructions the CPU burns in a spin-wait depends on the workers' wall-clock, and
two runs of the same build stop in different animation phases. Counting displayed
frames is a game state, and there all five modes agree byte for byte. See
`docs/PERF-CPU.md`.

Baselines live in `docs/baselines/` (may drift; the invariant is equality, not the
literal hash): SM64 60 fields `b5521b24d8fc280fbf102df22d7d30cb` (`docs/baselines/sm64.txt`; SoftRDP
y GPU-RDP dan el mismo).
Read dump text with `tr -d '\0'` (NUL-padded).

## MCP

Kestrel's OWN telemetry server — this is THE MCP for the whole workspace (ares MCP retired).

- Launch: `kestrel64.exe <rom> --mcp` (or `run_mcp.cmd`). Boots **paused** in MCP mode —
  `run_control resume` to advance CPU (`'resume'|'pause'|'reset'`, NOT `'run'`).
- Bridge: `tools/mcp/kestrel_mcp.py`, port 9128. Tools surfaced as `mcp__kestrel64__*`.
- Notable capabilities: `controller_set`/`controller_state` (inject the player-1 pad; the
  press lasts N **joybus polls**, not milliseconds, so it is the same number of game frames
  at any emulator speed and the game sees a real release edge -- `KESTREL_BUTTONS` is a
  fixed variable and `mem.write` on the pad word is overwritten by the window loop),
  `read_memory`/`write_memory` (`coherent=1` reads via CPU D-cache =
  no STALE kernel state), `capture_framebuffer` (VI→RGBA→PNG+histogram), `screenshot` /
  `video_start`/`video_stop`/`video_status` (PNG / MP4 H.264+AAC via Media Foundation / AVI MJPEG+PCM written by the emulator in
  guest time; hotkeys F9/F8, `KESTREL_RECORD`, `src/video/capture.hpp`), juego en red
  (`netplay_host/join/stop/status/view`: lockstep de entradas, pantalla partida por el anfitrion,
  F4, menu Red, `KESTREL_NETPLAY_*`, `src/net/netplay.hpp`), hotpath profiler
  (`profile_start/stop/reset`, `profile_cpu` PC-bucket 16B -- `metric=dmiss|imiss|dline` = fallos
  de D$/I$ por PC o por linea de datos, solo con `KESTREL_JIT=0` --, `profile_rsp` IMEM-slot; con `KESTREL_RSP_TIMING=1`
  modelo de ciclos del pipeline RSP (paradas + dual-issue SU/VU, `src/rsp/rsptiming.hpp`):
  `cycles/stalls/pairs` por ranura, `metric=cycles|stalls`, y `timing` en `rsp_registers`),
  cpu/rsp/rcp registers, disasm, breakpoints, `run_until`, simbolos del invitado (`--symbols
  <map GNU ld>` / `sym_load` / `sym_lookup`; `profile_cpu` y `cpu_disasm` dan `funcion+off`).
- Depurador (2026-10-23): grabadora de cuadro RDP/RSP, timeline, TMEM, TLB, ev.log, mem.watch,
  usb.log (SC64), code/rsp/rom.load en caliente, puntos de ruptura del RSP (rsp.bp.*), y ventana ImGui (`--debugger`, menu Depurar)
  que habla con el mismo servidor. `docs/DEBUGGER.md`.
- **When a capability is missing, ADD it** to the server — don't fall back to guessing.

### Ejecutar el emulador a mano (reglas de depuracion)

- **SIEMPRE `--run`.** Sin `--run` el exe arranca PAUSADO (modo MCP, espera `resume`) y
  ademas abre ventana: se queda ahi para siempre y la ventana se ve NEGRA. No es un cuelgue
  del emulador, es que nadie le dijo que corriera.
- **SIEMPRE `timeout <segundos>` delante**, con un numero sacado del baseline de
  `docs/baselines/timings.md` (una ROM de krom = ~2 s, SM64 60 campos = ~15 s). Si se pasa
  del baseline es CUELGUE, no lentitud: cortar y bisecar, nunca subir el timeout.
- **SIEMPRE topes de parada**: `KESTREL_MAXINSN`, y `KESTREL_MAXFLIPS`/`KESTREL_MAXSYNCS`
  o `KESTREL_STABLE` segun el caso. Sin tope, una ROM que no intercambia buffer corre eterna.
- Lo normal es no lanzarlo a mano: `scripts/validate.py` ya pone entorno, topes y timeout.
- **Umbral de paciencia = segundos, no minutos.** Un juego arrancado emite campos de video
  sin parar (59.94/s de guest, y el emulador va a ~1x). Cualquier prueba de una ROM da
  senal en 1-5 s: si en ~5 s no suben campos VI / swaps, es CUELGUE, no lentitud. Timeout
  de una ROM de krom = 10 s, SM64 60 campos = 30 s. Nada de esperas de minutos "a ver si
  sale".

### Perfect Dark: llegar a gameplay para medir

Medir en el menu no vale. `docs/local/PD-GAMEPLAY.md` (local, no versionado) tiene los dos caminos: navegar el menu con
`scripts/pad.py` (mando inyectado, pulsaciones contadas en lecturas del joybus) o escribir
el nivel directo en RAM (`g_MissionConfig` 0x07dbd8 + `g_MainChangeToStageNum` 0x043c04;
`g_StageNum` 0x043d60 vale 0x5A en el titulo y confirma que la ROM comparte mapa).
Banco A/B reproducible de ROMs de PD (fps de invitado, GCLK, SYNC redundantes):
`scripts/pdbench.sh` / `pdbench.py`, metodo y tabla en `docs/baselines/pd-opt.md`. Pasar
SIEMPRE el `.map` de cada ROM (`rom=map`).

### MCP gotchas
- `read_memory` inside a block-capture returns 0 — read `mem->rdram` directly instead.
- Never declare "hung" from capped/truncated log output.

### Editar fuente con python desde bash (trampas repetidas)
- El heredoc `<<'PYEOF'` **se come las barras invertidas dobles**: un patron con `\\n`
  NO casa con el `\n` del fichero. Construir siempre la barra con `chr(92)`.
- Codificacion y finales de linea son **por fichero**: `src/main.cpp` es latin-1 con LF
  (`src/cpu/jit*.{hpp,cpp}` son UTF-8 con LF); `src/core/system.cpp` es UTF-8 con **CRLF**. Abrir con el `encoding`
  correcto y `newline=''`, y no meter acentos escritos en UTF-8 dentro de un fichero latin-1
  (anclas de busqueda: usarlas SIN acentos).

## Source layout (`src/`)

`core/` (memory, system, rom, bus, DMA, MMIO, scheduler) · `cpu/` (R4300i interp + `jit.*`
dynarec) · `rsp/` (LLE + HLE) · `rdp/` (SoftRDP)
· `gpurdp/` (GPU-RDP propio en Vulkan compute, bit a bit SoftRDP; **rasterizador de fabrica desde 2026-10-06** -- `KESTREL_GPURDP=0` lo apaga; contexto Vulkan compartido con el presentador: `gpurdp::sharedVk()` + `gpurdp::queueLock`; `KESTREL_GPURDP_MINPX` reparto CPU/GPU por tramo, defecto 4096; `docs/GPU-RDP.md`, puerta `scripts/gate_gpurdp.sh`)
· `audio/` · `video/` (`present.cpp`, VI, GLFW/Vulkan WSI) · `telemetry/` · `net/` · `main.cpp`.

## Env-var toggles

Referencia completa de `KESTREL_*`: `docs/ENV-VARS.md`. Las mas usadas: `KESTREL_THREADS`,
`KESTREL_MAXINSN`, `KESTREL_FBDUMP`, `KESTREL_GPURDP`, `KESTREL_SPLEAD`/`KESTREL_DPLOGLEAD`,
`KESTREL_CACHECOST`, `KESTREL_RSP_TIMING`, `KESTREL_TELEMETRY_PORT`.

## Hard rules (NON-NEGOTIABLE)

- **Every change = genuine, generalizable VR4300/RCP HW semantics. NEVER hardcode to pass
  a test.** The oracle (systemtest / lockstep md5 / krom suite) catches shortcuts.
- **Verify systemtest (interp+JIT) + lockstep==threaded md5 + krom 47-suite after EVERY
  change.** No exceptions.
- **NEVER touch Perfect Dark** (the `../perfect_dark` decomp) until explicitly ordered.
- Context goes in `docs/` (committed), NOT in agent memory. Memory = feedback + breadcrumbs.
- Test the user's hypothesis FIRST (cheap experiment) before own theory. Never block/ask;
  pick best default and continue. Respond in Spanish, caveman style (code/commits normal).

## Current frontier (see docs/STATUS.md for live detail)

systemtest 100%, SM64/PD/DK64 jugables, lockstep==threaded. Saves completos, PIF, dynarec con
block-linking, RSP JIT, savestates/rewind. RDP: GPU-RDP propio (Vulkan compute) de fabrica,
SoftRDP y GPU-RDP a la par en la suite krom RDP/ (2026-10-08). Ultimo: SummerCart64
+ SD (carpeta de ROMs como FAT32 virtual, 6f76432). Peticiones de otros proyectos: `PD64_pending.md`
(PD, local) y mensajes de las sesiones SDK/PSX. Huecos conocidos: `docs/GAPS.md`.

Prioridad (usuario): breadth funcional antes que accuracy subpixel de color; ampliar segun
lo pidan el SDK, el port PSX y PD.
