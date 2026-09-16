# kestrel64 — CLAUDE.md

From-scratch **multithreaded, telemetry-native N64 emulator** (C++20, clang). Main
active project of this workspace. Built to run N64 games (esp. Perfect Dark) fast on
weak-single-thread / idle-GPU hosts, where cooperative-single-thread cycle-accurate
emulators (ares, cen64) hit an architectural ceiling.

Live status: `docs/STATUS.md`. Backlog de huecos y pendientes: `docs/GAPS.md` (lo que
falta frente a ares/PJ64/mupen + pendientes propios, ordenado; se actualiza al cerrar
cada punto). Design docs: `docs/ARCH-THREADING.md`, `docs/JIT-PLAN.md`,
`docs/TEXTURE-FORMATS.md`, `docs/parallel-rdp-integration.md`, `docs/VI-CLOCK.md`,
`docs/RSP-JIT.md`, `docs/ROMS-COMPRIMIDAS.md`.

## Architecture bet

- **Multithreaded from line one** — real OS threads for CPU / RSP / RDP / AI, not
  cooperative cothreads. Attacks the "2/8 cores" ceiling ares hits by design.
- **Telemetry-native core** — length-framed JSON+binary server baked in (port 9128). Bulk
  data (RAM dumps, framebuffers) rides as raw bytes, zero hex overhead. MCP bridge
  `tools/mcp/kestrel_mcp.py` exposes it. Development driven by full live introspection.
- **HLE-capable RSP** — LLE interpreter for correctness + custom microcode (PD ships
  custom ucode); HLE fast paths for standard microcodes as the perf win.
- **Reuse, not rewrite**: RDP = **parallel-rdp** (Themaister, Vulkan, GPU); VU = shared
  SSE4.1 reference (same math ares/cen64 use, systemtest-verified).

## Build

Toolchain = clang at `/c/msys64/clang64/bin` (MSYS2 CLANG64). Static-linked exe.

```bash
export PATH=/c/msys64/clang64/bin:$PATH
cd /e/Claude/N64/kestrel64
sh scripts/release.sh          # <-- ESTE. Regenera los CUATRO arboles + lanzador + paquete.
```

**`cmake --build <arbol> -j` a secas NO vale como "he compilado"**: deja los otros arboles
con el .exe de hace semanas, y el que el usuario ejecuta (`build-prdp-static/`) es
justamente uno de los que se queda atras. Compilar un arbol suelto solo es legitimo como
paso intermedio de una prueba A/B; en cuanto el cambio se da por bueno, `release.sh`.

- **ALWAYS `taskkill //F //IM kestrel64.exe` before rebuild** (Windows locks the exe).
- cmake binary lives in `/c/msys64/clang64/bin`.
- `build/` = default (PRDP OFF). `build-prdp/` = configured `-DKESTREL_PRDP=ON`
  (parallel-rdp GPU backend). Default OFF so deterministic core never depends on GPU.
- `-DKESTREL_STATIC=ON` = self-contained `.exe` (libc++/GLFW inside, no DLLs) for the
  published package; `sh scripts/dist.sh` stages `dist/` + zip. See `docs/distribucion.md`.
- **REGLA: cada generacion de .exe genera TODO** — los cuatro arboles, el lanzador grafico
  congelado, el zip portable y el instalador. Un solo comando, `sh scripts/release.sh`
  (`--gates` para pasar antes `gate_all` + `gate_prdp`, `--quick` para saltarse `build/` y
  `build-prdp/` e iterar solo sobre los estaticos, `NOISS=1` para omitir el instalador). Deja
  `dist/VERSION.txt` con version, commit y md5 para que un zip suelto diga de donde salio.
- El paquete lleva **dos** ejecutables porque el plugin grafico se elige al compilar:
  `kestrel64.exe` (`build-prdp-static`, parallel-RDP, el recomendado) y `kestrel64-soft.exe`
  (`build-static`, SoftRDP, para maquinas sin Vulkan y como rasterizador de referencia).
  Cada uno con su nota `*.build` al lado, que es como el lanzador sabe cual es cual.
- Las puertas por si solas recompilan `build/` y `build-prdp/` y **nunca**
  `build-prdp-static/`, que es el exe que viajan lanzador e instalador: por eso existe
  `release.sh`. Sus piezas sueltas siguen ahi si hace falta una a mano: `scripts/pack.sh`
  (estatico -> `dist/` -> zip -> Inno Setup), `scripts/dist.sh` (empaquetar un build ya
  hecho), `scripts/gui.sh` (congelar solo el lanzador).
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
sh scripts/gate_prdp.sh                               # backend GPU (parallel-rdp), exe de build-prdp
```

`gate_all.sh` es el oraculo determinista y corre sobre SoftRDP, que no depende de que
haya GPU. `gate_prdp.sh` corre el mismo material con `KESTREL_PRDP=1` contra el exe de
`build-prdp/` (modos `prdp` = interp lockstep y `prdp-jit` = threaded+dynarec) y tiene
baselines propias: krom `docs/baselines/krom-prdp.tsv`, sm64 `docs/baselines/sm64-prdp.txt`.
El md5 de sm64 depende del BACKEND, no del modo de CPU. Dos divergencias PRDP conocidas y
entendidas (registro de pipeline COMBINED, latencia de sumision a GPU) en
`docs/parallel-rdp-integration.md`.

**Speed is measured with `bench`, never with Mips.** In threaded mode a faster CPU
thread just burns more spin-wait instructions, so Mips can rise while the emulator
gets slower. `bench` fixes the guest work (N VI fields) and times the wall clock.

Modes: `interp`, `jit`, `jit-nolink`, `threaded`, `threaded-jit` (plus
`threaded-trace`, `threaded-nolink`). A change is done when systemtest and sm64
pass in **all five** and krom shows no regression.

**The sm64 gate stops on VI buffer swaps, not on an instruction count**
(`--sm64-flips`, default 60). A fixed instruction cap is not a deterministic point
of the game in threaded mode: the RCP runs on its own threads, so the number of
instructions the CPU burns in a spin-wait depends on the workers' wall-clock, and
two runs of the same build stop in different animation phases. Counting displayed
frames is a game state, and there all five modes agree byte for byte. See
`docs/PERF-CPU.md`.

Baselines live in `docs/baselines/` (may drift; the invariant is equality, not the
literal hash): SM64 60 fields `466282775dbd0ac084946558a1c30771` (SoftRDP).
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
  no STALE kernel state), `capture_framebuffer` (VI→RGBA→PNG+histogram), hotpath profiler
  (`profile_start/stop/reset`, `profile_cpu` PC-bucket 16B, `profile_rsp` IMEM-slot),
  cpu/rsp/rcp registers, disasm, breakpoints, `run_until`.
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

Medir en el menu no vale. `docs/PD-GAMEPLAY.md` tiene los dos caminos: navegar el menu con
`scripts/pad.py` (mando inyectado, pulsaciones contadas en lecturas del joybus) o escribir
el nivel directo en RAM (`g_MissionConfig` 0x07dbd8 + `g_MainChangeToStageNum` 0x043c04;
`g_StageNum` 0x043d60 vale 0x5A en el titulo y confirma que la ROM comparte mapa).

### MCP gotchas
- `read_memory` inside a block-capture returns 0 — read `mem->rdram` directly instead.
- Never declare "hung" from capped/truncated log output.

### Editar fuente con python desde bash (trampas repetidas)
- El heredoc `<<'PYEOF'` **se come las barras invertidas dobles**: un patron con `\\n`
  NO casa con el `\n` del fichero. Construir siempre la barra con `chr(92)`.
- Codificacion y finales de linea son **por fichero**: `src/cpu/jit.{hpp,cpp}` y `src/main.cpp`
  son latin-1 con LF; `src/core/system.cpp` es UTF-8 con **CRLF**. Abrir con el `encoding`
  correcto y `newline=''`, y no meter acentos escritos en UTF-8 dentro de un fichero latin-1
  (anclas de busqueda: usarlas SIN acentos).

## Source layout (`src/`)

`core/` (memory, system, rom, bus, DMA, MMIO, scheduler) · `cpu/` (R4300i interp + `jit.*`
dynarec) · `rsp/` (LLE + HLE) · `rdp/` (SoftRDP) · `vrdp/` (parallel-rdp glue, `KESTREL_PRDP`)
· `audio/` · `video/` (`present.cpp`, VI, GLFW/Vulkan WSI) · `telemetry/` · `net/` · `main.cpp`.

## Env-var toggles

`KESTREL_THREADS` (threaded RCP, **default ON**) · `KESTREL_JIT` (dynarec, **default ON**, oracle=interp;
los dos leen VALOR: `=0` apaga. Medido honesto en SM64, `bench` de 200 intercambios de
buffer: **threaded-jit 9.2 s = 131% de tiempo real**, threaded (CPU interp) 26.6 s = 38%,
jit lockstep 30.6 s = 35%, interp lockstep 39.6 s = 27%. Los tres ultimos son oraculos de
correctitud, no configuraciones de uso. Las cifras viejas (33.9%/11.0%) salian de contar
un intercambio de buffer como un campo de video, cuando SM64 gasta tres; ver
`docs/PERF-CPU.md` §12-bis;
block linking is ON inside it, `KESTREL_JIT_NOLINK=1` / `KESTREL_JIT_CHAIN=<n>` to bisect,
`KESTREL_JIT_TRACE=1` superblocks = measured negative; `KESTREL_JIT_NOITC=1` apaga la cache de
destinos indirectos JR/JALR -- A/B medido ~2,5 % de pared en SM64, `KESTREL_JIT_ITCBITS=<8..20>`
su tamano (por defecto 14 = 16384 entradas = 256 KB, elegido por A/B; 16 pierde), ver
`docs/PERF-CPU.md` §20) ·
`KESTREL_HEARTBEAT=1` (cada 5 s: Mips, % de velocidad N64, ocupacion de los workers, trabajos/s y **hambre de audio en vivo** -- silencio acumulado, descartes y colchon minimo DE ESA VENTANA; `KESTREL_AUDIOSTAT` solo habla al cerrar y con ventana el emulador no cierra solo) · `KESTREL_HOSTPROF=<ms>` (host sampler) · `KESTREL_JIT_STATS=<n>`
(volcado cada n despachos, por defecto 4 M; con enlace+ITC una tanda entera de SM64 no llega
a 4 M, asi que hay que bajarlo para ver nada; ademas de los contadores de siempre saca
`[retorno] salto/completo/corto` = como termina el bloque que devuelve el control,
`[salidas] enlace/itc/lentaDirecta/lentaIndirecta` = por donde sale cada terminador de salto
--se emiten EN LINEA en el codigo generado, o sea que con STATS puesto el JIT emite codigo
distinto-- y `[cadena] rotasPorTramp`; con ellos se mato la hipotesis del enlace secuencial,
ver `docs/PERF-CPU.md` §20.5) ·
`KESTREL_WATCHDOG=<s>` (liveness + stuck-thread RIP) · `KESTREL_RCPWAIT=<ms>` (cada cuanto da parte una espera del hilo de CPU sobre un worker del RCP -- `rspAwaitIdle`, el kick del RSP y `rdpDrain`; por defecto 2000, `=0` = espera muda de antes. No abandona la espera, la parte en rondas e imprime el estado del dominio: asi un worker que deja de publicar se ve como lo que es en vez de parecer lentitud) · `KESTREL_FIELDHASH=1` /
`KESTREL_FIELDDUMP=<n>` (localise a divergence) · `KESTREL_MAXFLIPS=<n>` (stop after n buffer swaps) ·
`KESTREL_VITICKS=<n>` (VI ticks per field, default 16 — ver `docs/VI-CLOCK.md`) ·
`KESTREL_CPI=<n>` (ciclos de CPU por instruccion retirada; **de fabrica 1,4**, que es la parte
alta del rango real del VR4300. De el cuelgan el ritmo del reloj Count y el presupuesto de
instrucciones por campo. `=2` = modelo historico bit a bit, un tick de Count por op; no se
admite mas de 2 porque las guardas de borde de timer del JIT cuentan ops. Ver `docs/STATUS.md`) ·
`KESTREL_SIINSTANT=1` (la transaccion del SI/joybus vuelve a terminar en la misma instruccion
que la arranco, como antes del 2026-09-08. De fabrica el SI factura el tiempo de la linea
joybus -- 4 us por bit, parada de consola 3 us, parada del mando 4 us -- y remata el DMA en
diferido levantando `MI_SI` al vencer el plazo; ver `docs/GAPS.md`) ·
`KESTREL_PRDP=1` (GPU RDP, needs `build-prdp`) · `KESTREL_MAXINSN=N` · `KESTREL_FBDUMP=path` ·
`KESTREL_CPUIDLE=0` (apaga el cobro en bloque del hilo ocioso del invitado -- `beq $0,$0,-1`
con NOP en la ranura de retardo; el salto usa el MISMO permiso que una cadena enlazada del JIT,
asi que no cambia cuando se mira cada evento. -14 % de pared en SM64 threaded-jit; se niega con
`Status[2:0] != 1` y en Threaded sin plazos de RCP. Telemetria `[ocioso]` dentro de
`KESTREL_JIT_STATS`) ·
`KESTREL_NOFETCHFAST=1` (disable I-cache-line fetch memoization) · `KESTREL_RDRAM=4|8` (MB de RDRAM: 8 = Expansion Pak, por defecto; 4 = consola de serie. Se decide al arrancar y no cambia en caliente; el invitado lo lee en 0x318/0x3F0 y el JIT lo acota con `jitRdramSz`) · `KESTREL_SAVETYPE` · `KESTREL_CHEATS=<fichero .cht>` (trucos GameShark, ver `docs/CHEATS.md`) · `KESTREL_MOVIE_REC=<f.k64m>` / `KESTREL_MOVIE_PLAY=<f.k64m>` (peliculas TAS: graba/reproduce lo que el JUEGO LEE en cada comando 0x01 del joybus, no lo que aprieta el jugador; reproducir comprueba los CRC del cartucho y se niega si son de otro; ver `docs/TAS.md`) · `KESTREL_REWIND=1` + `KESTREL_REWIND_FIELDS=<n>` / `KESTREL_REWIND_MB=<n>` (rebobinado con la tecla de retroceso; APAGADO de fabrica porque cada foto para el RCP y recorre el estado entero: +45 % de pared con foto cada 2 campos, +24 % cada 6. Ver `docs/REWIND.md`) ·
Una ROM dentro de un `.zip` o un `.gz` se abre directamente, sin variable ninguna: el desempaquetado va en `src/core/archive.cpp` (DEFLATE propio, ver `docs/ROMS-COMPRIMIDAS.md`), y las partidas/estados/trucos cuelgan del nombre SIN la extension del contenedor ·
`KESTREL_AUDIOSTAT=1` (al cerrar: muestras empujadas/servidas/de relleno/tiradas y nivel del anillo; con el se diagnostico el audio entrecortado) · `KESTREL_MEMPAK=0` (desenchufa el Controller Pak del mando 1; por defecto va puesto) ·
`KESTREL_SPINPAUSE=0` (quita la pista `PAUSE` de las esperas activas: la CPU se vigila con el RSP y el RDP girando sobre contadores que escribe el otro hilo, y `_mm_pause` le dice al nucleo que eso es una espera para que no le robe la linea de cache ni las ranuras de emision al hermano SMT. Va a una de cada 16 vueltas a proposito: PAUSE cuesta ~9 ciclos en Nehalem y ~140 de Skylake en adelante. Es SOLO una pista de anfitrion, el resultado del invitado sale identico; medido NEUTRO en este anfitrion -- ver `docs/baselines/timings.md`) · `KESTREL_BARSPIN=<n>` (vueltas que gira la CPU en la barrera del SP antes de dormir; 0 = el defecto, 2048) · `KESTREL_RDPSPIN=<n>` (vueltas que gira el worker del RDP ocioso antes de dormir en `rdpCv`; defecto 32768, 0 = dormir enseguida. -13 % de pared con Parallel-RDP, -6 % con SoftRDP: evita despertar el hilo por el kernel en cada DPC_END) · `PARALLEL_RDP_SINGLE_THREADED_COMMAND=0|1` (lo pone kestrel a 1 si no esta: parallel-rdp procesa los comandos en el worker del RDP sin su hilo `CommandRing`, -1,6 % de pared; 0 = anillo de siempre) · `KESTREL_RSPSPIN=<n>` (igual para el worker del RSP entre tareas; defecto 0, medido neutro) · `KESTREL_DPSPIN=<n>` (vueltas que gira quien espera al RDP -- `dpBarrierWait` en la CPU, `rdpAwaitGuest` en el RSP -- antes de dormir en `rdpCv`; defecto 262144, 0 = dormir enseguida; -2 % con Parallel-RDP y -3 % con SoftRDP, meseta desde 262144; `KESTREL_BARSPIN` en cambio medido plano de 2048 a 1 M) ·
`KESTREL_RSPJIT` (RSP dynarec, **default ON**, oracle=RSP interp; `=0` off) /
`KESTREL_RSPJIT_STATS=1` (coverage, ver `docs/RSP-JIT.md`) / `KESTREL_RSPJIT_LINK=0`
(A/B: bloques de RSP sin encadenar; modo `rspnolink`) / `KESTREL_RSPJIT_NOVECMEM=1`
(A/B: LWC2/SWC2 vuelven al CALL) / `KESTREL_RSPJIT_NOVECMOVE=1` (A/B: MFC2/CFC2/MTC2/CTC2 igual) / `KESTREL_RSPJIT_NOVECPACK=1` (A/B: LPV/LUV/LRV vuelven al CALL) / `KESTREL_RSPJIT_WAYS=<n>`
(imagenes de microcodigo cacheadas, por defecto 16) / `KESTREL_RSPJIT_NEWWAY=<n>`
(trozos de 8 B de IMEM que justifican estrenar imagen, por defecto 8) · fuzz diferencial de la VU:
`--rspfuzz N` (escalar vs SSE) y `--rspjitfuzz N` (VU en linea del dynarec vs interprete) · `KESTREL_NORSPSSE=1` (VU escalar en vez de SSE4.1) ·
`KESTREL_VIDEO=1` · `KESTREL_VIDEO_TEST` · `KESTREL_FAULTSTOP=1` (halt on a guest fault with
the RCP event ring intact) · `KESTREL_WATCHP=<phys>` (store watchpoint hooked in the D-cache;
bus-level `KESTREL_WATCH` misses cacheable CPU stores) · `KESTREL_EVDUMP=<n>` ·
`KESTREL_DPSYNCLOG=1` (camino del RDP entero: direccion de cada SYNC_FULL retirado,
`[dpkick]` por cada arranque del command processor y `[dpwr]` por cada escritura a un
registro DPC con el estado antes de aplicarla -- con esta traza se vio que Perfect Dark
perdia el FIFO al congelar el RDP) · `KESTREL_RSPTRACE=1` (KICK y FIN de cada tarea del
RSP: cabecera OSTask de DMEM 0xFC0, PC del BREAK, ciclos, ventana DPC) ·
`KESTREL_RSPHANG=1` (si el RSP agota el presupuesto: OSTask, los 32 GPR y +-8 instrucciones
de IMEM alrededor del PC).

**Bisecar el enlace de bloques y el regulador** (todos A/B, sin efecto cuando no se ponen):
`KESTREL_JIT_NOTLBLINK=1` (no enlazar bloques cuyo destino sale de una pagina mapeada por TLB
-- Perfect Dark ejecuta desde `0x70000000`, y el enlace ahi vale ~4x) · `KESTREL_JIT_NOTLBSTATIC=1`
(solo la ITC, sin destino estatico, en rutas TLB) · `KESTREL_JIT_TLBXPAGE=1` (permitir enlaces TLB
que crucen pagina, resolviendo por `tlbProbePhys` en vez de exigir misma pagina) ·
`KESTREL_JIT_NORSPGUARD=1` (quita del camino rapido la guarda "hay tarea de RSP en vuelo".
MEDIDO 2026-09-03: sigue haciendo falta -- sin ella PD cuelga 1 de cada 16 arranques y SM64 emite
1804 campos VI por 300 intercambios en vez de 1024, o sea la CPU gasta lo ganado girando) ·
`KESTREL_PACESLACK=<n>` (holgura del regulador CPU<->RSP, por defecto **4096 = `jit::kGuardMaxOps`**;
ver el comentario largo sobre `kPaceSlack` en `src/core/memory.cpp`: por encima de la granularidad
del dynarec la holgura la tendria que justificar el hardware, y no la justifica) · `KESTREL_PACEGRAIN=<n>`.

**Python**: `scripts/validate.py` pide numpy y el python de MSYS (`/c/msys64/clang64/bin/python`,
el primero del PATH cuando se exporta clang64) NO lo tiene. Usar siempre el de Windows,
`/c/Users/celga/AppData/Local/Programs/Python/Python311/python`, que es el que ya fijan
`scripts/gate_all.sh` y `gate_prdp.sh` en su variable `PY`.

**RCP enhebrado, semantica y biseccion**: el consumidor del FIFO del RDP NO lee los
comandos de la RDRAM viva sino de una **instantanea** que el productor copia al encolar el
tramo (`Memory::rdpSnapshot`, dos buffers alternos indexados por generacion de buffer de
comandos). El productor ya habia escrito esos bytes antes del kick, o sea que leerlos en el
kick es un instante de lectura que el hardware tambien puede elegir; dentro de una generacion
protege el control de flujo del propio juego (DPC_CURRENT) y entre generaciones el buffer
alterno. Solo se redirigen los COMANDOS: pixeles, texturas y TLUT siguen leyendo RDRAM viva.
Sin esto Perfect Dark descarrila 3 de cada 4 tandas (ver `docs/PD-DERAIL.md`).
`KESTREL_RDPDRAIN=1` recupera el drenado del RDP en cada START fresco, que fue la primera
cura -- SOLO para bisecar: medido cuesta 13-15 % de pared y aleja la fidelidad del oraculo
lockstep · `KESTREL_SYNCRDP=1` / `KESTREL_SYNCRSP=1` dejan uno de los dos
workers en su hilo y el otro sincrono, para bisecar de quien es una corrupcion ·
`KESTREL_PRDP_SYNCALL=1` espera a la GPU tras cada primitiva (solo `build-prdp`).

**Quien escribio esto** (caros, solo para depurar): `KESTREL_WRTAG=1` mantiene un tag de
ultimo escritor por bloque de 16 B de RDRAM (CPU-uncached / D-cache / SP-DMA / PI-DMA /
SI-DMA / RDP) con el PC del guest, lo imprime en el volcado de fallo, y chiva (`[fifo!]`)
cualquier escritura dentro del FIFO del RDP aun sin consumir · `KESTREL_RDPGUARD=<lo>:<hi>`
chiva cualquier escritura del RDP a RDRAM en ese rango fisico · `KESTREL_CIFLOOR=<phys>`
baja el suelo por debajo del cual un SET_COLOR_IMAGE se considera basura (por defecto los
vectores de excepcion).

**Canarios de corrupcion** (caros, solo para depurar): `KESTREL_CODEWATCH=<n>` compara cada n
campos el codigo del guest contra una copia de referencia y dice el primer byte que cambio
(`_LO`/`_HI` acotan el rango fisico, `_AFTER` retrasa el armado) · `KESTREL_DMAGUARD=<lo>:<hi>`
chiva cualquier DMA del SP que escriba dentro de ese rango fisico · `KESTREL_REGCHK=1` hace que
el JIT compruebe los registros contra el interprete · `KESTREL_EXCODD=<n>` vuelca contexto y los 32
GPR en las <n> primeras excepciones IMPOSIBLES en un juego sano -- instruccion reservada,
coprocesador no usable, error de direccion, y tambien TLBL/TLBS cuando el TLB no tiene NI UNA
entrada valida (ahi no es paginacion, es puntero salvaje) · `KESTREL_GUESTTHREADS=1` anade el volcado de OSThread
al latido del watchdog.

La ventana del emulador lleva **barra de menu nativa con el catalogo entero de opciones**,
el mismo que el lanzador (`docs/LAUNCHER.md`). Fuente unica = `tools/launcher/options.py`;
al tocarla hay que **`python tools/gen_optdefs.py`** (regenera `src/ui/optdefs.cpp`, que NO
se edita a mano). Lo que se puede cambiar en marcha vive en `src/core/runtime.hpp`, sembrado
del entorno por `rt::initFromEnv()` para que lote y gates sean bit-identicos a antes; el
resto se guarda en `profile.json` y el emulador se relanza a si mismo.

Producto (ver `docs/LAUNCHER.md`): `KESTREL_OC` / `KESTREL_OC_CPU` / `KESTREL_OC_RSP` /
`KESTREL_OC_RDRAM` (multiplicadores de reloj por dominio) · `KESTREL_WINSCALE=N` /
`KESTREL_WINSIZE=WxH` / `KESTREL_FULLSCREEN=1` (presentacion; el guest sigue en 320x240) ·
`KESTREL_PAD1=<fichero>` (mapa de mando; sin fichero, teclado de siempre). Todo esto lo
escribe el lanzador grafico `tools/launcher/run_launcher.cmd`.

## Hard rules (NON-NEGOTIABLE)

- **Every change = genuine, generalizable VR4300/RCP HW semantics. NEVER hardcode to pass
  a test.** The oracle (systemtest / lockstep md5 / krom suite) catches shortcuts.
- **Verify systemtest (interp+JIT) + lockstep==threaded md5 + krom 47-suite after EVERY
  change.** No exceptions.
- **NEVER touch Perfect Dark** (the `../perfect_dark` decomp) until explicitly ordered.
- Context goes in `docs/` (committed), NOT in agent memory. Memory = feedback + breadcrumbs.
- Test the user's hypothesis FIRST (cheap experiment) before own theory. Never block/ask;
  pick best default and continue. Respond in Spanish, caveman style (code/commits normal).

## Current frontier (see STATUS.md for live detail)

systemtest 100%, SM64 boots+renders 3D, PD boots+renders+advances, lockstep==threaded.
Save types complete (EEPROM/SRAM/FlashRAM + Controller Pak `.mpk`). Dynarec Stage-2c (block-linking = ceiling).
parallel-rdp VENDORED under `third_party/` and correct on SM64 — the background rainbow was
RDRAM byte order (kestrel keeps guest big-endian, parallel-rdp assumes ares' word swizzle),
see `docs/parallel-rdp-integration.md`; shaders patched, SPIR-V bank regenerated with
`tools/slangmosh_lite.py` (upscaling still unsupported). RDP FIFO back-pressure fixed the
threaded DP-interrupt surplus, `docs/RDP-FIFO-BACKPRESSURE.md`. Real RDP accuracy frontier =
coverage/AA subpixel (biggest, last).

### Queued work (autonomous order)
1. RSP VU with **SSE4.2** intrinsics (8×s16 = 1 XMM; user asked for most-advanced host CPU
   instr — i7-870 Nehalem, SSE4.2 max, NO AVX). Oracle = current scalar interp, bit-exact.
2. Savestates. 3. Dynarec block-linking. 4. ~~Controller Pak `.mpk`~~ DONE. 5. PIF/CIC LLE.

Note: classic Zilmar video/audio plugin architecture = legacy that caused inaccuracy;
but backend SELECTION (SoftRDP↔parallel-RDP, audio sink) is what we already build = good.
