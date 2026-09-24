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
`KESTREL_JIT_TRACE=1` superblocks = measured negative; `KESTREL_JIT_BUFMB=<1..1024>` / `KESTREL_JIT_SLOTBITS=<12..22>` (cache de codigo del JIT: por defecto 64 MB y 2^17 ranuras; a 16 MB Perfect Dark en juego la vaciaba entera sin parar, 248 k compilaciones -> 35 k) · `KESTREL_JIT_NOITC=1` apaga la cache de
destinos indirectos JR/JALR -- A/B medido ~2,5 % de pared en SM64, `KESTREL_JIT_ITCBITS=<8..20>`
su tamano (por defecto 14 = 16384 entradas = 256 KB, elegido por A/B; 16 pierde), ver
`docs/PERF-CPU.md` §20) ·
`KESTREL_DPLOGLEAD=0|1|auto` (adelanto de la CPU en las citas de lectura de DPC de `dpLogWait`: PD en juego ~20 -> ~38 fps pero threaded deja de coincidir con lockstep. De fabrica `auto` = PUESTO en velocidad libre y QUITADO con `KESTREL_SPEEDMODE=hw`; `scripts/validate.py` lo fija a 0, asi que las puertas siguen siendo el oraculo determinista. Ver STATUS 2026-09-22 (c)) ·
`KESTREL_HEARTBEAT=1` (cada 5 s: Mips, % de velocidad N64, ocupacion de los workers, trabajos/s y **hambre de audio en vivo** -- silencio acumulado, descartes y colchon minimo DE ESA VENTANA; `KESTREL_AUDIOSTAT` solo habla al cerrar y con ventana el emulador no cierra solo) · `KESTREL_HOSTPROF=<ms>` (host sampler) · `KESTREL_JIT_STATS=<n>`
(volcado cada n despachos, por defecto 4 M; con enlace+ITC una tanda entera de SM64 no llega
a 4 M, asi que hay que bajarlo para ver nada; ademas de los contadores de siempre saca
`[retorno] salto/completo/corto` = como termina el bloque que devuelve el control,
`[salidas] enlace/itc/lentaDirecta/lentaIndirecta` = por donde sale cada terminador de salto
--se emiten EN LINEA en el codigo generado, o sea que con STATS puesto el JIT emite codigo
distinto-- y `[cadena] rotasPorTramp`; con ellos se mato la hipotesis del enlace secuencial,
ver `docs/PERF-CPU.md` §20.5) ·
`KESTREL_WATCHDOG=<s>` (liveness + stuck-thread RIP) · `KESTREL_DPLOG=0` (el RSP vuelve a esperar a la CPU en cada escritura DPC -- cita `spReadSync` -- en vez de apuntarla en el diario `Memory::dpLogPush` para que la CPU la aplique al llegar a su instante; de fabrica diario, junkrunner64 -20 % de pared, mismo md5. Telemetria `[dplog]` al cerrar) · `KESTREL_SPLOG=0` (las escrituras del microcodigo a SP_STATUS vuelven a caer en el registro en el acto en vez de ir al diario con su instante; de fabrica diario, arregla el statehash variable de junkrunner64) · `KESTREL_DMARDV=<mask>` (cita del RSP con la CPU antes de lanzar un DMA del SP: bit0 SP_RD_LEN, bit1 SP_WR_LEN; de fabrica 3. Sin ella el DMA lee/escribe RDRAM en un instante al que la CPU aun no ha llegado y Threaded no da el statehash de Lockstep; `=0` mas rapido, solo para bisecar) · `KESTREL_DMALOG=0` (los DMA SP_WR_LEN del RSP vuelven a la cita en vez de ir al diario con sus bytes ya copiados; de fabrica diario, junkrunner64 -4,5 % de pared, mismo statehash) · `KESTREL_DPBARSYNC=0` (la barrera del RDP vuelve a ser el fin de TODO lo mandado tambien con Parallel-RDP; de fabrica, con la GPU, solo frena a la CPU en el tramo con SYNC_FULL mas antiguo pendiente, que es donde esta el fence y donde cae MI_DP -- junkrunner64 -7 % de pared, mismo statehash) · `KESTREL_RSPTANDA=<n>` (instrucciones por tanda de `Rsp::step` = grano con que el RSP publica su reloj a la barrera del SP; de fabrica **4096** desde el 2026-09-24 (era 1024); el 8192 original dejaba a la CPU parada en la barrera mientras el RSP la esperaba en `spReadSync` -- junkrunner64 -12 % de pared al bajar a 512, y otro -1,6 % en SM64 al subir de 512 a 1024 tras el re-barrido del 2026-09-18, que el optimo se movio arriba cuando los diarios quitaron las citas. Mismo statehash y mismo md5. RE-BARRIDO 2026-09-24 al bajar `KESTREL_SPLEAD` de 8192 a 256: el optimo se movio ARRIBA, que es lo esperable -- con menos adelanto la CPU se para mas veces en la barrera, y publicar el reloj mas a menudo no la desbloquea antes, solo anade vueltas al bucle del RSP. Parejas intercaladas, mismo statehash en todas: SM64 400 swaps 1024 7440-7595 ms, 2048 7333-7374, 4096 7254-7359, 8192 7265-7277; PD en juego 1024 6206-6311, 2048 6202-6331, 4096 6191-6212, 8192 6191-6229; DK64 plano en todos. 4096 y 8192 empatan, se coge 4096) · `KESTREL_DPCFAST=0` (quita el camino rapido del sondeo de DPC_CURRENT del microcodigo: mientras el motor del RDP este drenado, `dpSubSeq` no se mueva y el instante del RSP no pase del ultimo borde de grano al que la CPU ya llego, la respuesta esta fijada y el camino largo -- division del reloj, `cartNow`, buzon y dos barridos del anillo -- no hace falta. La ventana va guardada en CICLOS del RSP para que comprobarla no pase por la division. FIEL: mismo `[statehash]`. PD en juego -3,6 % de pared, 95 % de aciertos de 26,8 M sondeos; SM64 y DK64 planos. Telemetria `[dpcfast]`. Ver STATUS 2026-09-24 (d)) · `KESTREL_DPCJUMP=0` (el microcodigo vuelve a dar una a una las vueltas del bucle de espera del FIFO en vez de cobrarlas de golpe. Mientras vale la ventana de `KESTREL_DPCFAST` el motor sigue drenado y la respuesta es constante, y todas esas lecturas caen en granos que la CPU ya retiro: se saltan k vueltas COMPLETAS de longitud `len`, asi que el reloj aterriza en la misma rejilla y la vuelta de salida la sigue fijando la escritura de la CPU. FIEL: mismo `[statehash]`. PD en juego -5,6 % de pared, 25,7 M de vueltas absorbidas en 6796 saltos. Telemetria `[dpcfast]` campos `saltos`/`vueltas`. Ver STATUS 2026-09-24 (e)) · `KESTREL_RSPDPAWAIT=1` (el RSP vuelve a esperar al worker del RDP antes de leer DPC_CURRENT/STATUS; por defecto no: salen del horario de invitado) · `KESTREL_DMASPAN=0` (un DMA del RSP espera al RDP drenado SIEMPRE que haya trabajo en vuelo, no solo si solapa color/z image) · `KESTREL_RCPWAIT=<ms>` (cada cuanto da parte una espera del hilo de CPU sobre un worker del RCP -- `rspAwaitIdle`, el kick del RSP y `rdpDrain`; por defecto 2000, `=0` = espera muda de antes. No abandona la espera, la parte en rondas e imprime el estado del dominio: asi un worker que deja de publicar se ve como lo que es en vez de parecer lentitud) · `KESTREL_FIELDHASH=1` /
`KESTREL_FIELDDUMP=<n>` (localise a divergence) · `KESTREL_MAXFLIPS=<n>` (stop after n buffer swaps) ·
`KESTREL_VITICKS=<n>` (cada cuanto vuelve el bucle a mirar el RCP, en subtramos por campo; de fabrica 16 — ver `docs/VI-CLOCK.md`. Desde el 2026-09-18 es ajuste de ANFITRION puro: `MI_VI` y `MI_AI` tienen plazo propio y caen en la instruccion exacta, asi que cambiarlo NO mueve el `[statehash]` del invitado. Esa invariancia es la prueba de que los plazos son exactos, y se comprueba con `KESTREL_IRQTRACE`. OJO: subirlo NO es gratis en PARED. Barrido 16/64/256: SM64 +12,5 %/+43,3 %, DK64 +4,0 %/+23,1 %, jr +3,6 %/+6,9 %. El subtramo es tambien la cadencia con la que el bucle atiende audio, presentacion y a los workers del RCP. 16 se queda) ·
`KESTREL_IRQTRACE=<n>` (imprime las n primeras subidas de CADA interrupcion con el INSTANTE de invitado en que caen: `[irq] VI #3 at=... mask=.. intr=..`. Es la sonda con la que se comprueba que un plazo es exacto -- dos corridas con ajustes de anfitrion distintos tienen que dar la MISMA lista. Aviso: con JIT y `KESTREL_MAXINSN` el `[statehash]` final SI puede diferir aunque las listas sean identicas, porque el tope corta a mitad de cadena; eso es la sonda, no el emulador) ·
`KESTREL_CPI=<n>` (ciclos de CPU por instruccion retirada; **de fabrica 1,4**, que es la parte
alta del rango real del VR4300. De el cuelgan el ritmo del reloj Count y el presupuesto de
instrucciones por campo. `=2` = modelo historico bit a bit, un tick de Count por op; no se
admite mas de 2 porque las guardas de borde de timer del JIT cuentan ops. Ver `docs/STATUS.md`) ·
`KESTREL_SIINSTANT=1` (la transaccion del SI/joybus vuelve a terminar en la misma instruccion
que la arranco, como antes del 2026-09-08. De fabrica el SI factura el tiempo de la linea
joybus -- 4 us por bit, parada de consola 3 us, parada del mando 4 us -- y remata el DMA en
diferido levantando `MI_SI` al vencer el plazo; ver `docs/GAPS.md`) ·
`KESTREL_PIINSTANT=1` (igual para el PI: el DMA del cartucho vuelve a terminar en la
instruccion que lo arranca, como antes del 2026-09-18. De fabrica el PI factura la duracion
real del traslado con los tiempos que programan `PI_BSD_DOM*` -- bus de 16 bits por paginas
de `2^(PGS+2)` bytes, `LAT+1` al entrar en la pagina, `(PWD+1)+(RLS+1)` por palabra de 16
bits; con los valores de cabecera de un cartucho normal salen 5,375 MB/s -- y levanta
`MI_PI` al vencer. `loadRom` programa ademas `PI_BSD_DOM1_*` desde los bytes 0x01..0x03 de
la cabecera, que es lo que hace el IPL2 y aqui no hacia nadie por ser HLE el arranque; ver
`docs/STATUS.md`) ·
`KESTREL_PRDP=1` (GPU RDP, needs `build-prdp`) · `KESTREL_MAXINSN=N` · `KESTREL_FBDUMP=path` ·
`KESTREL_CPUIDLE=0` (apaga el cobro en bloque del hilo ocioso del invitado -- `beq $0,$0,-1`
con NOP en la ranura de retardo; el salto usa el MISMO permiso que una cadena enlazada del JIT,
asi que no cambia cuando se mira cada evento. -14 % de pared en SM64 threaded-jit; se niega con
`Status[2:0] != 1` y en Threaded sin plazos de RCP. Telemetria `[ocioso]` dentro de
`KESTREL_JIT_STATS`) ·
`KESTREL_NOFETCHFAST=1` (disable I-cache-line fetch memoization) · `KESTREL_RDRAM=4|8` (MB de RDRAM: 8 = Expansion Pak, por defecto; 4 = consola de serie. Se decide al arrancar y no cambia en caliente; el invitado lo lee en 0x318/0x3F0 y el JIT lo acota con `jitRdramSz`) · `KESTREL_SAVETYPE` · `KESTREL_CHEATS=<fichero .cht>` (trucos GameShark, ver `docs/CHEATS.md`) · `KESTREL_MOVIE_REC=<f.k64m>` / `KESTREL_MOVIE_PLAY=<f.k64m>` (peliculas TAS: graba/reproduce lo que el JUEGO LEE en cada comando 0x01 del joybus, no lo que aprieta el jugador; reproducir comprueba los CRC del cartucho y se niega si son de otro; ver `docs/TAS.md`) · `KESTREL_REWIND=1` + `KESTREL_REWIND_FIELDS=<n>` / `KESTREL_REWIND_MB=<n>` (rebobinado con la tecla de retroceso; APAGADO de fabrica porque cada foto para el RCP y recorre el estado entero: +45 % de pared con foto cada 2 campos, +24 % cada 6. La foto espera al reposo natural del RCP, `System::rcpAtRest`; `KESTREL_REWIND_RTT=1` recarga cada foto en el acto para probar que el estado guardado esta completo, modo de gate `rewind-rtt`. Ver `docs/REWIND.md`) ·
Una ROM dentro de un `.zip` o un `.gz` se abre directamente, sin variable ninguna: el desempaquetado va en `src/core/archive.cpp` (DEFLATE propio, ver `docs/ROMS-COMPRIMIDAS.md`), y las partidas/estados/trucos cuelgan del nombre SIN la extension del contenedor ·
`KESTREL_CACHESTAT=1` (cuenta tambien los ACCESOS a las caches primarias, no solo los fallos, y con eso el cierre saca la tasa de acierto de verdad -- SM64 60 campos: D$ 93,97 %, I$ 98,18 % -- en vez de solo fallos por instruccion retirada. APAGADO de fabrica y por una razon: el sitio donde hay que contarlos es `dcRead`/`dcWrite`, y el camino rapido de RDRAM del JIT no pasa por ahi, asi que con la variable puesta ese camino se apaga entero (`g_noFastMem = 255`) para que la muestra no salga sesgada hacia lo que ya era lento. O sea: mide, no corre. La cifra de I$ cuenta solo lo que pasa por `icFetch`, que es el interprete: el JIT no busca instruccion por instruccion. Los fallos a secas se cuentan SIEMPRE y desde 2026-09-22 salen tambien en el latido, `[hb] cache`, con fallos/kop y ciclos de parada por op) · `KESTREL_AUDIOSTAT=1` (al cerrar: muestras empujadas/servidas/de relleno/tiradas y nivel del anillo; con el se diagnostico el audio entrecortado) · `KESTREL_MEMPAK=0` (desenchufa el Controller Pak del mando 1; por defecto va puesto) ·
`KESTREL_DUEWATCH=0` (el prologo en linea del dynarec deja de comparar el mapa de plazos del RCP
(`Memory::rcpPend`) con el que tenia cuando se calculo el permiso de la cadena. De fabrica lo compara:
un plazo que ARMA EL OTRO HILO a mitad de cadena -- fin de tarea del SP, fin del RDP, escritura DPC del
diario -- no entraba en el permiso, y entonces `MI_SP` caia donde acabase el permiso, que lo recorta
`rcpPace` con el reloj VIVO del RSP = host-dependiente. Con el adelanto de fabrica SM64 daba TRES
estados finales distintos en ocho corridas del mismo binario; con la comprobacion, 8/8 el mismo.
Cuesta ~1,3 % de pared. Ver STATUS 2026-09-24 (j)) · `KESTREL_SPLEAD=<ops>|auto` (adelanto FIJO de la CPU sobre la barrera del SP, para romper la
alternancia estricta CPU<->RSP; `auto` = **512** en velocidad libre (8192 hasta el 2026-09-24, luego 256, y 512 desde la barrera de escritura, ver abajo) y 0 en `KESTREL_SPEEDMODE=hw`,
las puertas lo fijan a 0. PD -25 % de pared, DK64 -15 %, SM64 -8 %. Determinista (el fin de tarea
del SP se arma con la MISMA L sumada, ver `Memory::spEndArm`) pero NO fiel: `MI_SP` sube L ops mas
tarde y threaded deja de coincidir con lockstep. MEDIDO 2026-09-24: ESE adelanto es la UNICA causa de que SM64 no de el mismo estado en los dos modos -- con `=0` el modo de hilos sale identico al Lockstep (volcado completo de estado y las 6630 excepciones del arranque, una a una) y ademas el statehash deja de depender de `KESTREL_RSPTANDA`. Cuesta +22 % de pared en PD en juego (6,07 -> 7,43 s) y la tanda no lo recupera. Es el modo 'fiel al oraculo' para auditar cambios. CORREGIDO 2026-09-24 (g): con 8192 la corrida NO era reproducible. SM64 a 400 swaps daba cinco statehash distintos en ocho corridas del MISMO binario, con el contador de ops identico (o sea divergencia de verdad, no el tope): el RSP quemaba ciclos distintos y hacia otro numero de DMA, y el rastro acababa en el bit BD de Cause. La cita de DMA obliga a la CPU a LLEGAR al reloj del RSP pero no a no haberse pasado, asi que con adelanto L la CPU ya escribio RDRAM hasta L ops en el futuro del RSP. Barrido en SM64 (6 corridas por valor): 0/64/256 dan UN solo statehash, 512/1024/4096 dan dos. Coste en PD en juego: 8192 6041 ms, 256 6235 (+3 %), 0 7423 (+23 %) -- casi toda la ganancia esta en el adelanto pequeno. Y 0 tampoco compra fidelidad: threaded-0 NO es lockstep (PD 0a64f386 vs 14995cfa, SM64 b6f55124 vs b5f10ec4). De ahi el defecto 256. SUBIDO a 512 el 2026-09-24 con la barrera de escritura CPU->RSP (STATUS (i)), y `KESTREL_DUEWATCH` (STATUS (j)) le quita la mayor parte de la varianza -- sin esa comprobacion SM64 da TRES estados distintos en ocho corridas --, **pero NO la cierra: CORREGIDO 2026-09-24 (l), a 512 y con due-watch puesta SM64 da DOS estados en 12 corridas (8 + 4). Solo L=0 es reproducible.** Por encima no se puede: 768 y 1024 se parten aun con ella, aunque 1024 valdria -2,6 % en PD -- con adelanto L la CPU puede haberse PASADO del instante en que el RSP lee, y por cuanto depende del anfitrion. Ver STATUS 2026-09-23 (e), 2026-09-24 (f) y 2026-09-24 (g)) · `KESTREL_RDVSLEEP=<ops>` (hueco de invitado a partir del cual la cita del RSP DUERME en vez de girar: apunta su instante en `Memory::rspWaitAt` y el hilo de CPU lo despierta al pasar por el. De fabrica **0 = no dormir nunca**, porque la medida del 2026-09-24 es PLANA: dos rondas intercaladas de min-de-4 en PD en juego dan 6090/6095 ms con 0 contra 6089/6061 con 2048, y SM64/DK64 empatan. El invitado sale identico. Se queda como instrumento de A/B. OJO al medir: la siesta va con cadencia propia, 1 de cada 1024 vueltas; ponerla detras del portillo de `KESTREL_RDVYIELD` (1 de cada 65536) la deja sin ejecutar y la medida sale plana por eso, no por el efecto. Ver STATUS 2026-09-24 (l)) · `KESTREL_DMABARRIER=0` (quita la barrera de escritura en las transferencias que escriben RDRAM desde el hilo de CPU: `Memory::piDma` en cartucho->RDRAM y guardado->RDRAM, y `Memory::siFinish` en PIF->RDRAM. De fabrica ENCENDIDA porque es la misma semantica que la de los stores -- esas transferencias traen los datos NUEVOS que luego lee el RSP --, pero la medida del 2026-09-24 es PLANA: SM64 8/8 con ella y 6/6 sin ella a adelanto 512, PD 4/4 y pared empatada, DK64 4/4. Y NO desbloquea subir el adelanto: 1024 sigue dando dos estados en 8 corridas con ella puesta. Ver STATUS 2026-09-24 (k)) · `KESTREL_SPDATE=<ops>` (PRUEBA: separa el adelanto de la BARRERA del desplazamiento con que se FECHA el fin de tarea del SP; de fabrica los dos valen `SPLEAD`. Con barrera 8192 y fecha 0 el plazo nace vencido y el numero de vencidos depende del anfitrion -- 107 y 108 en dos corridas del mismo binario --, asi que NO es determinista. Documentado para que no se vuelva a intentar) · `KESTREL_STATEDUMP=1` (al llegar al tope de `KESTREL_MAXINSN` vuelca los 32 gpr + 32 cop0 + 32 fpr + pc/nextPc/hi/lo, uno por linea, mas `[sd] cnt` con las ops cobradas a Count, las retiradas, el resto y los ciclos parados. El statehash dice SI dos modos acabaron igual; esto dice EN QUE se separaron, y el `cnt` distingue divergencia de verdad de pasarse del tope) · `KESTREL_SPINPAUSE=0` (quita la pista `PAUSE` de las esperas activas: la CPU se vigila con el RSP y el RDP girando sobre contadores que escribe el otro hilo, y `_mm_pause` le dice al nucleo que eso es una espera para que no le robe la linea de cache ni las ranuras de emision al hermano SMT. Va a una de cada 16 vueltas a proposito: PAUSE cuesta ~9 ciclos en Nehalem y ~140 de Skylake en adelante. Es SOLO una pista de anfitrion, el resultado del invitado sale identico; medido NEUTRO en este anfitrion -- ver `docs/baselines/timings.md`) · `KESTREL_BARSPIN=<n>` (vueltas que gira la CPU en la barrera del SP antes de dormir; 0 = el defecto, **16384** desde el re-barrido del 2026-09-18 -- con los diarios puestos la barrera se abre antes y la espera cabe en el giro, -1,6 % en SM64; el 2048 viejo se midio plano de 2048 a 1 M y ya no lo es, 131072 cobra +1,9 % en junkrunner64) · `KESTREL_RDPSPIN=<n>` (vueltas que gira el worker del RDP ocioso antes de dormir en `rdpCv`; defecto **131072** desde el re-barrido del 2026-09-18, 0 = dormir enseguida. -13 % de pared con Parallel-RDP, -6 % con SoftRDP: evita despertar el hilo por el kernel en cada DPC_END. La meseta sigue empezando en 32768 -- 8192 se hunde en los cuatro juegos -- pero ya no es plana por arriba: con los diarios puestos el RSP archiva el tramo y sigue, asi que los tramos llegan mas seguidos y mas pequenos y el hueco entre dos no cabe en 32768 vueltas. Dos tandas independientes de min-de-5, 32768 -> 131072: jr 7266 -> 7105 (-2,2 %, lecturas disjuntas), PD 11501 -> 11475, SM64 7496 -> 7466 (disjuntas), DK64 11761 -> 11716. 524288 cobra +1,4 % en jr) · `PARALLEL_RDP_SINGLE_THREADED_COMMAND=0|1` (lo pone kestrel a 1 si no esta: parallel-rdp procesa los comandos en el worker del RDP sin su hilo `CommandRing`, -1,6 % de pared; 0 = anillo de siempre) · `KESTREL_RDVPOLL=<n>` (potencia de dos, defecto 64: una de cada n vueltas mira el bucle de espera del HILO DEL RSP el reloj de la CPU. `cartNow()` lee el contador de instrucciones retiradas, que el hilo de CPU escribe sin parar; mirarlo en cada vuelta le robaba la linea en exclusiva al hilo que es el cuello. -12,8 % de pared en junkrunner64, -9,2 % en PD, md5 identico. Afecta a `spReadSync`, `dpLogWait` y al giro previo a dormir de `rspParkWait`; NO al lado de la CPU, donde medido es regresion) · `KESTREL_RDVTIGHT=<n>` (vueltas iniciales en que ese bucle mira el reloj a pelo antes de espaciar; defecto 0, ~0 = comportamiento viejo) · `KESTREL_RDVYIELD=<n>` (potencia de dos, defecto **65536**: una de cada n vueltas ese mismo bucle CEDE EL NUCLEO. En Windows `std::this_thread::yield()` es `SwitchToThread()`, una llamada al kernel, y en este anfitrion -- 4 nucleos / 8 hilos con tres hilos calientes -- casi siempre no hay a quien cederselo: vuelve enseguida sin haber hecho nada. En el perfil del hilo del RSP del 2026-09-18 el 20,3 % de las muestras cae FUERA de la imagen con `dpLogWait` de llamante, y es justo ese yield: el microcodigo sondea DPC 1,81 M de veces por corrida y cada sondeo es una cita con la CPU. Dos tandas intercaladas de min-de-4, 256 -> 65536: jr -0,51 / -0,47, PD -0,65 / +0,04, SM64 -1,00 / -0,60, DK64 -0,37 / -0,29, siete de ocho a favor y md5 identico; la primera ademas sale monotona en los cuatro juegos (256 -> 4096 -> 65536). Por arriba se acaba: 262144 empata y 1 M ya es PEOR en PD (+0,46 %), porque detras del yield van el aviso al hilo de CPU (`rspCv.notify_all`) y el salvavidas de pared (`rdvWaiveDue`) y espaciarlo los retrasa. 256 = comportamiento viejo) · `KESTREL_RDVCHEAP=0` (vuelve a mirar en CADA vuelta del bucle de cita el aviso de vaciado del diario (`dpLogFlush`, que solo escribe la CPU al pararse) y la parada del anfitrion (`rspStop`/`hostStop`, que solo se escribe al cerrar). De fabrica van a la MISMA cadencia que la condicion de salida (1 de cada 64, `KESTREL_RDVPOLL`): son dos lineas compartidas que no cambian mas de un punado de veces por corrida, y sondearlas en cada vuelta es trafico de coherencia puro. El cierre se retrasa como mucho 63 vueltas de unos pocos ns. Dos tandas intercaladas de min-de-4: PD -2,94 % / -2,73 %, SM64 -1,77 % / -2,17 % con las cuatro lecturas de cada brazo DISJUNTAS en los dos juegos y las dos tandas; jr y DK64 planos. md5 identico) · `KESTREL_RSPSPIN=<n>` (igual para el worker del RSP entre tareas; defecto 0, medido neutro) · `KESTREL_DPSPIN=<n>` (vueltas que gira quien espera al RDP -- `dpBarrierWait` en la CPU, `rdpAwaitGuest` en el RSP -- antes de dormir en `rdpCv`; defecto 262144, 0 = dormir enseguida; -2 % con Parallel-RDP y -3 % con SoftRDP, meseta desde 262144. RE-BARRIDO 2026-09-18 con los diarios puestos y SIGUE PLANO: 1 M parecia ganar 0,7 % en PD y 1,2 % en SM64 en la primera tanda y la segunda no lo reprodujo -- 262144 ganaba esos mismos dos juegos --, asi que se queda) ·
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
de IMEM alrededor del PC) ·
`KESTREL_GPCPROF=<topN>` (perfil por PC de INVITADO del dynarec, muestreado por TIEMPO -- una
muestra cada 1024 ops retiradas, no por entrada al bloque, que sesga hacia los bloques cortos --
y con las 12 instrucciones de cada sitio desensambladas. Con el se vio que el 65,6 % del tiempo
de invitado de Perfect Dark cae en el hilo ocioso de libultra) · `[ociosoquien]` dentro de
`KESTREL_JIT_STATS` (reparto de ese ocio segun quien trabajaba: nadie / solo RSP / solo RDP /
los dos; en PD sale 98,2 % solo RSP) · `KESTREL_RSPPROF=<topN>` (perfil de ranura de IMEM del
microcodigo; solo cuenta el camino del interprete, asi que pide `KESTREL_RSPJIT=0`. Saca ademas
`[rspgiro]`: en que instruccion gira el microcodigo, cuantos ciclos y el volcado de IMEM
alrededor) · `KESTREL_RSPSHOW=<n>` (las n primeras vueltas saltadas por `Rsp::idleSkip`, con PC,
valor leido y registros) · `KESTREL_RSPTASKS=1` (reparto de tareas y ciclos del RSP por
`OSTask.type`) · `KESTREL_PARKLOG=1` (una linea por aparcamiento del RSP con `until`, tope, por
donde salio y el salto; con ella se vio que 38 de 38 salian por el tope del regulador, ver
STATUS 2026-09-23 (b))

**Bisecar el enlace de bloques y el regulador** (todos A/B, sin efecto cuando no se ponen):
`KESTREL_JIT_NOTLBLINK=1` (no enlazar bloques cuyo destino sale de una pagina mapeada por TLB
-- Perfect Dark ejecuta desde `0x70000000`, y el enlace ahi vale ~4x) · `KESTREL_JIT_NOTLBSTATIC=1`
(solo la ITC, sin destino estatico, en rutas TLB) · `KESTREL_JIT_NOTLBXPAGE=1` (volver a exigir
que el destino de un enlace TLB caiga en el MISMO marco de 4 KB que la entrada del bloque; de
fabrica se resuelve por `tlbProbePhys`, y como PD ejecuta desde `0x70000000` esa restriccion
dejaba sin enlazar cualquier JAL: 1,28 M de salidas lentas directas por corrida, -12 % de pared
al quitarla) · `KESTREL_JIT_NOPREARM=1` (el driver vuelve a entrar al primer bloque de la cadena
por el trampolin en vez de llamar a `jitReenterProceed` directo; -3,7 % de pared al quitarlo) ·
`KESTREL_JIT_NORSPGUARD=1` (quita del camino rapido la guarda "hay tarea de RSP en vuelo".
MEDIDO 2026-09-03: sigue haciendo falta -- sin ella PD cuelga 1 de cada 16 arranques y SM64 emite
1804 campos VI por 300 intercambios en vez de 1024, o sea la CPU gasta lo ganado girando.
Desde 2026-09-17 solo importa en Lockstep: en Threaded con plazos la guarda ya no esta, la barrera
del SP acota en tiempo de invitado) · `KESTREL_JIT_RSPGUARD=1` (vuelve a poner esa guarda en Threaded,
para bisecar) ·
`KESTREL_PACESLACK=<n>` (holgura del regulador CPU<->RSP; **DOS defectos** desde el 2026-09-18:
**65536** cuando las barreras de invitado son la autoridad -- Threaded con `SPBARRIER`, `DPBARRIER`
y `RCPDEADLINE` puestas, o sea la configuracion normal -- y **4096 = `jit::kGuardMaxOps`** cuando
alguna de esas escotillas esta apagada y el regulador vuelve a ser el unico freno. Poner la
variable fija las dos. Razon: lo que el hardware no justifica es el ADELANTO EN TIEMPO DE INVITADO,
y con las barreras puestas ese adelanto lo clava `spBarrierWait` en `spBarrierAt()` en cada retiro,
holgura aparte; entonces la holgura solo decide cada cuanto interviene el freno del ANFITRION. Ver
el comentario largo sobre `kPaceSlack`/`paceSlack()` en `src/core/memory.cpp`. 1 M sigue descartado:
es el valor que descarrilaba Perfect Dark 1 de cada 8 arranques) · `KESTREL_PACEGRAIN=<n>` (grano del PERMISO que devuelve el regulador, por defecto 1024) · `KESTREL_PACEASK=<n>` (cada cuantas ops de invitado le PREGUNTA `System::stepCpu` al regulador tras un bloque JIT; de fabrica **1024**, `=0` = tras cada bloque, que es como estaba hasta el 2026-09-18. Preguntar cuesta leer `rspBusy`, `rsp.cyclesRun` y `rdpBusy`, tres lineas que los workers reescriben sin parar, y quien acota el adelanto en tiempo de invitado es la barrera del SP, no esto. Barrido intercalado, minimos de 4-6: PD -2,5 %, SM64 -1,4 %, junkrunner64 -1,1 %, DK64 -0,3 %; meseta desde 1024, 8192 y "casi nunca" empatan. Mismo md5 en los cuatro).

**Python**: `scripts/validate.py` pide numpy y el python de MSYS (`/c/msys64/clang64/bin/python`,
el primero del PATH cuando se exporta clang64) NO lo tiene. Usar siempre el de Windows,
`/c/Users/celga/AppData/Local/Programs/Python/Python311/python`, que es el que ya fijan
`scripts/gate_all.sh` y `gate_prdp.sh` en su variable `PY`.

**RCP enhebrado, semantica y biseccion**: el consumidor del FIFO del RDP NO lee los
comandos de la RDRAM viva sino de una **instantanea** que el productor copia al encolar el
tramo (`Memory::rdpSnapshot`, OCHO buffers indexados por generacion de buffer de
comandos; `KESTREL_RDPGENS=<2..8>`, 2 = los dos alternos historicos). El productor ya habia escrito esos bytes antes del kick, o sea que leerlos en el
kick es un instante de lectura que el hardware tambien puede elegir; dentro de una generacion
protege el control de flujo del propio juego (DPC_CURRENT) y entre generaciones el buffer
alterno. Solo se redirigen los COMANDOS: pixeles, texturas y TLUT siguen leyendo RDRAM viva.
Sin esto Perfect Dark descarrila 3 de cada 4 tandas (ver `docs/PD-DERAIL.md`).
Dos generaciones NO bastan: si el productor se adelanta dos buffers, la tercera copia
reescribe comandos que el rasterizador aun no ha leido y el invitado acaba ejecutando datos
(bug #2 de junkrunner64, ver `docs/STATUS.md` 2026-09-18). Al instalar un START fresco se
coge la generacion LIBRE mas baja; si no hubiera ninguna se drena (lento pero correcto) y
`[dpgen]` cuenta esos drenados forzosos, que en junkrunner64 son 0-2 por corrida. `KESTREL_RDPDRAIN=1` recupera el drenado del RDP en cada START fresco, que fue
la primera cura -- SOLO para bisecar: medido cuesta 13-15 % de pared y aleja la fidelidad del
oraculo lockstep · `KESTREL_SYNCRDP=1` / `KESTREL_SYNCRSP=1` dejan uno de los dos
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
