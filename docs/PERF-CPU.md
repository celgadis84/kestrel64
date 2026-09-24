# CPU performance — interpreter + dynarec

Where the emulator's own host cycles go, how that was measured, and what each
change bought. Host: i7-870 (Nehalem, 4c/8t, 2.93 GHz), Windows, clang -O3
`-march=native`, no LTO.

Reference point: a real N64 retires ~93.75 M instructions/s at CPI 1, so
"N64 speed 100%" in the heartbeat means ~93.75 Mips of emulated CPU.

## Measuring

Three instruments, all opt-in and all committed:

| Tool | Env | What it answers |
|---|---|---|
| Heartbeat | `KESTREL_HEARTBEAT=1` | Mips, N64 speed %, and **worker occupancy** (`rdp`/`rsp` busy %, `cpuWait` %) |
| Host sampling profiler | `KESTREL_HOSTPROF=<ms>` | which *emulator* function burns host cycles (`scripts/hostprof.py` symbolises the dump) |
| JIT stats | `KESTREL_JIT_STATS=1` | block coverage, `avgK` (ops per block), decline reasons, block-terminating opcodes |

**Never benchmark with `KESTREL_MAXINSN`.** It arms `debugArmed`, which turns on
`stepTraps()` and the jump-log ring per instruction — the numbers come out biased.
Use `KESTREL_HEARTBEAT=1` plus a wall-clock `timeout`.

## Diagnosis (SM64, threaded RCP)

1. Occupancy said `rdp 32% rsp 48% cpuWait 0%` — the RCP workers are not the
   long pole and the CPU thread never waits on them. Per-thread host CPU
   confirmed one thread pinned at 100%.
2. Host profiler, JIT on: `jitTryBlock` **67%**, `compileBlock` 7.8%.
   Interpreter only: `CPU::step` 57%, `System::stepCpu` 24%, `icFetch` 6.4%.
3. JIT stats: `avgK 2.39`, 44.8 M compile declines. So ~90 % of retired ops did
   run inside JIT blocks, but each block was 2.4 ops — block-entry overhead
   dwarfed the emulated work.

## Changes

### 1. Negative compile cache (`jit.hpp` / `jit.cpp`)

`compileBlock()` fails when the *leader* op is not compilable, and in real code
that same PC is re-attempted millions of times. A direct-mapped 8192-entry table
remembers "this phys did not compile", validated against the leader word read
from the I-cache line — so self-modifying code or a DMA over the block
invalidates the entry by itself, with no explicit flush.

Compile declines 44.8 M → 11.4 M.

### 2. In-block interpreter fallback (`CPU::jitInterpOp`, `emitInterpOp`)

Previously the first unsupported op ended the block. In SM64/PD that op is almost
always FPU (`LWC1`/`SWC1`/`COP1`), which appears every few instructions, so blocks
were cut short constantly. Now such an op is emitted as a call to the interpreter:
one `CALL` of cost, but the block continues.

`jitInterpOp` reconstructs exactly the context `step()` would have (`curPc` = the
op, `pc` = delay slot, `nextPc` = pc+4), runs `execute()`, and returns 0 if
anything diverged from sequential advance (exception, halt, branch). On 0 the
block exits with the control flag set, i.e. the interpreter's `pc`/`nextPc` are
already the resume point and the driver must not recompute them — unlike the
mem-op bail, whose contract is "no side effects yet".

Admitted set — no control flow, no speculative state:
COP1 (except `BC1x`), `LWC1`/`LDC1`/`SWC1`/`SDC1`, `LWL`/`LWR`/`SWL`/`SWR`,
and `DIV`/`DIVU`/`DMULT`/`DMULTU`/`DDIV`/`DDIVU`.
Excluded on purpose: COP0, `CACHE`, `LL`/`SC`, `SYSCALL`/`BREAK`/`TRAP`, all jumps.

`avgK` 2.39 → 2.94.

### 3. Block linking enabled by default (`KESTREL_JIT_LINK`)

Step 3 of the linking work was already implemented but gated off. With
`avgK ≈ 3` the driver round-trip *is* the cost, so chaining blocks directly is
the single biggest lever. Validated on systemtest (interp/JIT/JIT+link all
0/3721·0/2·0/6) and framebuffer-md5-identical to the non-linked JIT.

### 4. Interpreter hot path

- Four function-local `static`s with dynamic initialisers lived inside
  `CPU::step()` (`fastFetch`, `intlog`, `jlogNoSpin`) and one in `dcWrite`
  (`KESTREL_DCWT`), plus `jitOn` inside `System::stepCpu`'s loop. In C++ each of
  those costs a thread-safe initialisation-guard check **per emulated
  instruction**. Moved to namespace scope, where the dynamic init happens once
  before `main()`.
- `icFetch`/`dcRead`/`dcWrite` assembled values byte by byte out of the cache
  line; `icFill`/`dcFill`/`dcFlush` copied byte by byte with a bounds test per
  byte. Replaced with a width-exact `memcpy` + `bswap` (the line holds big-endian
  guest bytes, the host is little-endian — identical result, 2 instructions) and
  a whole-line `memcpy` when the line is entirely inside RDRAM, keeping the
  byte-wise path for the RDRAM-end edge.

## Result (SM64, threaded RCP, 45 s wall)

| Config | N64 CPU speed | Insns in 45 s |
|---|---|---|
| Before this work | 19.2 % | 697 M |
| + negative cache + interp fallback | ~21 % | 884 M |
| + block linking | **~28 %** | **1280 M** |

### 5. Traces made opt-in (`KESTREL_JIT_TRACE`)

Superblock tracing (follow the taken edge of a conditional branch into the same
block) was on by default. Measured on SM64 it is **net negative**: 1237 M insns /
30.77 Mips / 24.8 % N64 speed with traces vs 1316 M / 32.76 Mips / 27.3 % without.
A traced block declares a larger `K`, and the revalidating prologue then rejects
the chained link whenever `K` does not fit the remaining window — more driver
round-trips and more emitted code for the same work. Kept, but off by default.

### 6. Caller-window guard (`jitTryBlock`)

`System::run()` asks for a fixed number of ops per tick and then ticks the VI. A
block that retires *more* ops than the caller asked for pushes the field boundary
past the requested count, so the same guest instruction lands in a different field
depending on the mode. The interpreter never overshoots; the JIT could. Blocks
whose `K` exceeds the remaining budget are now declined — bit-exact field
boundaries across interp/JIT.

### 7. Host FP environment via MXCSR, not `<cfenv>` (`cpu.cpp`)

Every computational COP1 op sets the host rounding mode and clears the IEEE status
before computing, then harvests the sticky flags. Through `<cfenv>` that is
`fesetround` + `feclearexcept` + `fetestexcept`, and under mingw each one goes into
`__mingw_setfp`, which synchronises the **x87 control word as well as MXCSR**. The
host profiler put ~32 % of the emulator's total time there:

```
51.28%  kestrel_jitProceedTramp
28.68%  __mingw_setfp
 2.07%  fenv_decode
 1.70%  __mingw_setfp_sse
```

On x86-64 every float/double operation is SSE, so MXCSR alone governs rounding and
the sticky flags, and reading/writing it is two instructions. `namespace mx` in
`cpu.cpp` does exactly that; `mx::prep()` only writes the register when the value
actually changes (the guest's rounding mode almost never moves between ops), and
`cvtInt` saves/restores only the RC field, matching what `fesetround(save)` did.

**33.99 -> 49.35 Mips** (26.0 % -> 50.1 % N64 CPU speed). The krom suite dropped
from 245 s to 158 s as a side effect.

### 8. False sharing on the fields the CPU thread polls (`memory.hpp`, `rsp.hpp`)

With the FP churn gone the profile collapsed onto two *loads* inside the block
re-entry check — 18.45 % and 12.70 % of all samples in two `cmp` instructions:

```
mem->rsp.running                       // 18.45 %
mem->rcpMode == RcpMode::Lockstep      // 12.70 %
```

Neither is contended in the logical sense: `running` changes at task start and at
BREAK, `rcpMode` is fixed at startup. They were slow because of what sat next to
them in memory. `Rsp::running` was followed by the delay-slot latch and the safety
budget, which the RSP worker writes on **every microcode instruction**; `rcpMode`
shared its line with `rdpQueue` / `rdpMx` / `rdpBusy`, which the RDP worker writes
on every job. The JIT re-entry check reads them roughly every three guest
instructions, so what should be an L1 hit was a coherence miss with another core
invalidating the line continuously.

Fix: give them their own cache lines (`alignas(64)` plus explicit padding), same
for the MI interrupt pair (`mi_mask` / `mi_intr`, read per block, written only when
an IRQ is raised — they shared a line with the DPC performance counters the RDP
bumps per span) and for those counters themselves. Also reordered the guard to test
`rcpMode` before `rsp.running`, so threaded mode never touches the RSP field at all.
No semantic change whatsoever.

**52.4 -> 70.5 Mips** (53 % -> 75 % N64 CPU speed).

### 9. `Random` advanced in O(1) (`jit.cpp`)

COP0 `Random` decrements once per instruction and snaps back to 31 when it equals
`Wired`. The chained-block prologue replayed that **as a loop**, one iteration per
deferred op, on every link. With `avgK ~= 3` that was ~30 host instructions per
block for a register the game almost never reads.

The sequence is a cycle: `d = (r - wired) mod 64` steps down to `Wired`, then a
cycle of `n = ((31 - wired) mod 64) + 1` values — which is `[wired..31]` for the
normal `Wired <= 31`, and the full 64-value sweep the loop already modelled for
`Wired > 31`. `randomAdvance()` computes the same answer directly; it was checked
exhaustively against the old loop over all 64x64x139 combinations of
(Random, Wired, steps) with zero mismatches, and n64-systemtest exercises it.

**70.5 -> 87.5 Mips** (75 % -> 86 % N64 CPU speed).

## Result after the second pass (SM64, threaded RCP + JIT)

| Step | Mips | N64 CPU speed |
|---|---|---|
| Before this pass | 33.99 | 26.0 % |
| + MXCSR instead of `<cfenv>` | 49.35 | 50.1 % |
| + cache-line isolation | 70.55 | 72-75 % |
| + `Random` in O(1) | **87.51** | **~86 %** |

For reference, the same host runs the interpreter at 13.55 Mips and lockstep JIT at
14.41 Mips, so threading and the dynarec are both carrying their weight now.

## Two bugs found by this work

### Bug 1 — `unlinkAll()` was quadratic against guest I-cache flushes

`CodeCache::unlinkAll()` walks *every* link site and *every* block. The guest
invalidates its I-cache one line at a time (libultra issues 512 back-to-back
`CACHE` ops for a full sweep), and every one of those paid the full sweep. With
tens of thousands of blocks the emulator ground to a halt — the host profiler
showed **86.79 % of samples inside `CPU::cacheOp`**.

Fix: an `anyLinked` flag, set where a link site is actually armed, checked first
in `unlinkAll()`. Nothing armed → nothing to walk.

systemtest jit 34 s → **10 s**; threaded-jit 34 s → **9.6 s**.

### Bug 2 — lost wakeup in the RSP worker (threaded + JIT hang)

`rspWorkerLoop` cleared `rspBusy` *outside* `rspMx`. The store+notify can then
land between a waiter evaluating its predicate (which saw `rspBusy == true`,
under the mutex) and the waiter registering itself on the condvar: the
notification is lost and the CPU thread sleeps forever waiting on an RSP that
already finished. The watchdog caught it exactly — `retired +0`, `rspBusy=0`,
`rdpQ=0`, every predicate true, RIP inside a kernel wait.

Fix: clear the flag under `rspMx`, and `notify_all` (not `notify_one`) on both
`rspCv` and `rdpCv`, since two different waiter classes share each condvar.

Only reachable with threads + JIT because the JIT is fast enough to make the
window between the check and the wait routinely lose the race.

## The `lockstep == threaded` md5 gate was measuring the wrong thing

The old gate ran SM64 to a fixed instruction count and compared framebuffer md5s.
That is **not** a deterministic point of the game in threaded mode: the RCP runs
on its own threads, so how many instructions the CPU burns spinning on a
`SP_STATUS`/`DP` wait depends on the worker's wall-clock. Two runs of the *same
build* stop in different animation phases. Measured: per-field state hashes
(`KESTREL_FIELDHASH=1`) differ run-to-run in threaded mode, with and without JIT,
and threaded-jit produced two different md5s across three runs.

Nothing was wrong with the emulation. The gate now stops on **VI buffer swaps**
(`KESTREL_MAXFLIPS`, exposed as `validate.py sm64 --sm64-flips`, default 60),
which *is* a game state, and there all five modes agree byte for byte:

```
sm64[interp]       MATCH 466282775dbd0ac084946558a1c30771 (14s)
sm64[jit]          MATCH 466282775dbd0ac084946558a1c30771 ( 8s)
sm64[jit-nolink]   MATCH 466282775dbd0ac084946558a1c30771 (10s)
sm64[threaded]     MATCH 466282775dbd0ac084946558a1c30771 (11s)
sm64[threaded-jit] MATCH 466282775dbd0ac084946558a1c30771 ( 4s)
```

Threaded mode is deliberately *not* cycle-deterministic: the wall-clock of the
RCP workers is the price of the parallelism. Determinism is guaranteed and gated
at frame granularity, and bit-exactly at instruction granularity in the lockstep
modes (interp / jit / jit-nolink), which are the JIT's oracle.

## Diagnostics added (all opt-in, all committed)

| Env | What it does |
|---|---|
| `KESTREL_WATCHDOG=<secs>` | prints CPU/RSP/RDP liveness every N s; if `retired` did not move, suspends the CPU thread and prints its RIP — this is what found bug 2 |
| `KESTREL_FIELDHASH=1` | FNV of CPU state at every VI field: first differing field localises a divergence |
| `KESTREL_FIELDDUMP=<n>` | full GPR/COP0/PC dump at field *n* |
| `KESTREL_JIT_CHAIN=<n>` | max blocks chained per driver entry (bisecting linking) |
| `KESTREL_JIT_NOLINK=1` | disable block linking |
| `KESTREL_JIT_TRACE=1` | enable superblock tracing (measured negative, see above) |

Note: the host profiler dump is **cumulative over the whole run**, so it cannot
profile a hang window — that misled one round of diagnosis before the watchdog
existed.

## 10. Lo que de verdad se estaba midiendo mal: los DEFAULTS

`Mips` no es una metrica valida en modo threaded (una CPU mas rapida solo gasta mas
instrucciones girando en el spin-wait), asi que se anadio una puerta que mide **tiempo
de pared para trabajo guest FIJO**: N campos VI de SM64.

```
python scripts/validate.py bench --mode <modo> [--bench-flips 600] [--bench-runs 3]
```

600 campos = 10 s de video guest. Medido en el i7-870, `build/` (SoftRDP):

| Modo | 600 campos VI | % tiempo real |
|---|---|---|
| interp (lockstep) | 78.2 s | 12.8 % |
| jit (lockstep) | 73.7 s | 13.6 % |
| threaded (interp CPU) | 44.7 s | 22.4 % |
| **threaded-jit** | **10.1 s** | **99.1 %** |

El emulador ya corria **a tiempo real** — pero solo si el usuario sabia exportar dos
variables de entorno. `KESTREL_THREADS` y `KESTREL_JIT` iban en OFF por defecto, asi que
`kestrel64.exe rom.z64` daba el 12.8 %: 7.7x mas lento que el mismo binario bien
configurado. Eso, y no el interprete, era la razon de "va peor que ares".

**Cambio**: los dos van ON por defecto. Los conmutadores pasan a leer VALOR
(`envFlag(name, def)` en `core/types.hpp`): `KESTREL_JIT=0` apaga el dynarec,
`KESTREL_THREADS=0` vuelve a lockstep. Las puertas de validacion ponen el valor
explicito en `MODES`, de modo que `interp` sigue siendo el oraculo puro.

## 11. RSP: quitar coste por instruccion del interprete LLE

Con la CPU al ~100% de la velocidad N64, el palo largo pasa a ser el RSP
(`occupancy rsp ~62%`, `rsp 33.6 Mips busy`). Cuatro costes tontos, todos de HW
semanticamente neutro:

1. **Guarda de `static` local en el modificador de broadcast**. `R128::operator()(e)`
   construia su tabla de mascaras `pshufb` en un `static` local con constructor → una
   comprobacion de guarda de inicializacion (thread-safe statics, atomica) en **cada
   operacion COP2**. Ahora es `constexpr` a nivel de fichero.
2. **`imword()` montaba la instruccion byte a byte**. El PC del RSP siempre esta alineado
   (avanza de 4 en 4, `take()` enmascara con `0xffc`), asi que el camino normal es una
   carga de 32 bits + `bswap`; el camino byte a byte se queda para lecturas no alineadas.
3. **LQV/SQV alineados** (`e == 0`, direccion multiplo de 16 — el caso que usan los
   microcodigos de graficos para un vertice entero): 16 lecturas/escrituras de byte con
   enmascarado → una carga/almacen de 128 bits + `pshufb` que intercambia los bytes de
   cada banda. Mismos bytes, mismo orden. Bajo el mismo interruptor `KESTREL_NORSPSSE`
   que el resto del camino SIMD, para poder bisecar.
4. **Los 4 KB de contadores del muestreador** (`profPc[1024]`) estaban DENTRO del struct
   entre los GPR escalares y los registros vectoriales, que son calientes los dos. Movidos
   al final.

Y en el rasterizador, lo mismo: `KESTREL_NOBLEND` / `KESTREL_NOAA` / `KESTREL_NOFILTER`
eran `static` locales dentro de funciones **por pixel**. Subidos a ambito de fichero.

Resultado: `rsp 33.6 → 36.5 Mips` emulados (+9 %) sin tocar una sola semantica.

## 12. El `bench` medía mal (y el 99% de tiempo real era falso)

`bench` fija el trabajo guest en N campos VI y cronometra. Pasaba además
`KESTREL_MAXINSN=900M` como red de seguridad — y ese tope saltaba ANTES que los campos.
Lo que se estaba midiendo era "tiempo hasta 900M instrucciones", que en modo threaded es
justo la métrica envenenada que `bench` existe para evitar: el hilo de CPU gasta
instrucciones girando en el spin-wait del guest, así que una CPU más rápida llega al tope
antes y "mejora" el cronómetro sin que el guest haya avanzado un solo campo de más.

Corregido: `--bench-insn` (20e9 por defecto, red de seguridad de verdad) y verificación de
la línea `[frames] N buffer swaps` del emulador — si la corrida no llegó a los N campos, el
gate falla en vez de dar un número. `--bench-flips` baja a 200: SM64 se atasca entre 200 y
300 campos (no progresa ni en 19.000M instrucciones, con y sin JIT — defecto aparte, ver
STATUS), así que 600 campos nunca se alcanzaban.

**Números honestos, SM64, 200 campos VI (i7-870):**

| modo | tiempo | % tiempo real |
|------|--------|---------------|
| threaded-jit | 9.84 s | 33.9 % |
| jit (lockstep) | 30.4 s | 11.0 % |

## 12-bis. El `bench` seguía midiendo mal: un intercambio de buffer NO es un campo

Corregido el tope de instrucciones (§12), quedaba un segundo error, y este valía un
factor 3,4. `--bench-flips 200` para en **200 intercambios de buffer**, y el cálculo del
porcentaje los trataba como **200 campos de vídeo** = 3,34 s de vídeo a 59,94 Hz. Pero un
juego no intercambia una vez por campo: SM64 va a ~20 fps internos, o sea **tres campos
por intercambio**. El vídeo realmente producido son ~690 campos = 11,5 s.

No se pudo ver hasta arreglar el reloj del VI (`docs/VI-CLOCK.md`): el contador de campos
estaba roto — 2 campos en 421M instrucciones — así que no había de dónde sacar el número
bueno. Ahora `bench` cuenta los campos que el emulador declara e imprime los dos:
`200 intercambios (721 campos VI = 12.03s de video) min 9.19s -> 130.9% realtime`.

**Números honestos, SM64, 200 intercambios de buffer (i7-870):**

| modo | vídeo producido | tiempo de pared | % tiempo real |
|------|-----------------|-----------------|---------------|
| **threaded-jit** (los dos defaults ON) | 12.03 s | 9.19 s | **130.9 %** |
| threaded (CPU interp) | 10.16 s | 26.58 s | 38.2 % |
| jit (lockstep) | 10.59 s | 30.56 s | 34.7 % |
| interp (lockstep) | 10.59 s | 39.57 s | 26.8 % |

La configuración por defecto corre SM64 **por encima de tiempo real** en este anfitrión.
Los otros tres modos son oráculos de correctitud, no configuraciones de uso: pagan
lockstep o intérprete a propósito.

Las cifras de §12 y las que estaban en `CLAUDE.md` (33,9 % / 11,0 %) son ese mismo error:
divídanse por 0,29 para leerlas.

### El regulador no se toca

Tentación obvia: `rcpPace` frena la CPU el ~41 % del tiempo, quitarlo tiene que ir más
rápido. Medido: con el freno suelto (`KESTREL_PACESLACK=1e9`) el mismo bench da 307 %
"realtime"… porque el guest emite **2005 campos para los mismos 200 intercambios**. Eso no
es velocidad, es el juego perdiendo la noción del tiempo: la CPU adelanta tanto al RSP que
SM64 se cree en cámara lenta y suelta campos repetidos. El trabajo deja de ser fijo y la
métrica deja de significar nada.

### …pero la holgura sí se recalibra (2026-09-01)

El aviso de arriba también invalida la manera obvia de calibrar `KESTREL_PACESLACK`: el
«% de tiempo real» tiene los campos VI en el numerador, así que aflojar el freno lo sube
sin que el emulador haga un ápice más de trabajo. Medido en este ciclo, SoftRDP en hilos,
300 intercambios: 256 K da 2800 campos en 8,06 s y 1 M da 3075 campos en 7,93 s. El «%
realtime» pasa de 537 % a 647 % — un +20 % que no existe: la pared por intercambio es la
misma dentro del ruido.

La métrica sana es la **pared por un número fijo de intercambios de buffer**, que son los
fotogramas de juego que ve el jugador. Con ella, el barrido (SM64, 300 intercambios, min
de 3 pasadas) sale así:

| holgura | prdp-jit | int/s | threaded-jit (SoftRDP) |
|---------|----------|-------|------------------------|
| 256 K   | 4,40 s   | 68,2  | 8,06 s |
| 384 K   | 4,09 s   | 73,3  | — |
| 512 K   | 3,89 s   | 77,1  | 8,03 s |
| 768 K   | 3,85 s   | 77,9  | — |
| 1 M     | 3,80 s   | 78,9  | 7,93 s |
| 2 M     | —        | —     | 7,86 s |

O sea: **el codo está en 512 K y la ganancia es de parallel-rdp (+16 % de fotogramas de
juego por segundo), no de SoftRDP**, que sale plano. El despeñadero de 512 K que motivó
bajar el default a 256 K el 2026-08-26 ha desaparecido; entonces el hilo de CPU corriendo
por delante se comía el núcleo del worker, y desde que `rdpSubmit` dejó de despertar al
RDP 38 K veces por segundo (`PERF-RCP-SYNC.md`) ya no compiten por él.

Segundo eje, y es el que decide: **fidelidad**, medida como campos VI por intercambio con
**lockstep de oráculo** (4,09; determinista y con md5 idéntico). prdp-jit da 2,73 / 2,80 /
3,12 / 3,20 / 3,24 en la misma serie: todos por *debajo* del oráculo, y la holgura larga
es la que más se le acerca. No hay canje — 1 M es a la vez lo más rápido y lo más parecido
al oráculo. (SoftRDP en hilos se va al otro lado, ~9-10 campos por intercambio, pero esa
desviación es del backend, no del regulador: apenas se mueve con la holgura.)

Default nuevo: **1 M**. La meseta es ancha a ambos lados, que es lo que hay que buscar en
un parámetro cuyo óptimo se mueve cada vez que el hilo de CPU se acelera.

## 12-ter. Dónde NO optimizar el RSP: el mix real del microcódigo

Antes de escribir una línea de SSE nueva, medir qué ejecuta de verdad el microcódigo.
Build instrumentado (el contador está fuera del camino caliente salvo que se compile):

```bash
cmake -S . -B build-vustat -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS=-DKESTREL_VUSTAT=1
KESTREL_VUSTAT=1 ./build-vustat/kestrel64.exe <rom> --run
```

SM64, 60 intercambios, **37,0 M instrucciones de RSP**:

| grupo | % de las instrucciones RSP |
|-------|---------------------------|
| COP2 (matemática vectorial) | **41,6 %** |
| LWC2 + SWC2 (load/store vectorial) | **19,3 %** |
| SPECIAL | 11,2 % |
| resto escalar MIPS | 27,9 % |

Dos conclusiones que **descartan** el trabajo que parecía obvio:

1. **COP2 ya está vectorizado al 85,6 %.** De ese 41,6 %, solo **5,99 %** del total cae al
   camino escalar, y **3,84 %** de ese resto es `VRCP*`/`VMOV`, que operan sobre UNA banda
   y no son vectorizables ni en principio. Portar `VABS` + `VSAR` + `VCL` a SSE tocaría el
   **2,15 %** de las instrucciones del RSP: no mueve la aguja.
2. **Los load/store vectoriales ya tienen camino rápido** (`vecFast` + `pshufb` para el
   registro entero, 64 bits por paso para el resto). No queda ahí el bulto que sugiere
   su 19,3 %.

Y el perfilador de host sobre el hilo del RSP (`KESTREL_HOSTPROF=1
KESTREL_HOSTPROF_WHO=rsp`, símbolos resueltos con `llvm-nm`, RVA + 0x140000000):

| símbolo | % del hilo RSP |
|---------|----------------|
| `Rsp::step` (fetch + despacho + escalar) | 59,9 % |
| `Rsp::execCop2` | 31,6 % |
| `Rsp::execStore` | 4,8 % |
| `Rsp::execLoad` | 3,7 % |

COP2 es el 41,6 % de las instrucciones y solo el 31,6 % del tiempo: la parte vectorial
**rinde por encima de su peso**. El coste está en el bucle de despacho, que es el problema
que en la CPU resolvió el dynarec. El RSP no tiene JIT. Esa es la palanca que queda, y es
obra grande — no micro-optimización de opcodes.

## 13. El camino rápido del prólogo JIT: correcto, y casi irrelevante

El perfilador de host decía `kestrel_jitProceedTramp` = 34% del HILO de CPU (se llamaba en
cada entrada de bloque, ~cada 3 instrucciones guest). El prólogo emitido ahora comprueba en
línea lo único que puede cambiar dentro de una cadena — permiso en ops (`jitGuard`, fijado
por el trampolín a partir del borde Count==Compare y de la ventana del bucle del sistema),
`MI_INTR & MI_MASK`, el latch de timer y `rsp.running` — y solo llama al trampolín cuando
alguno falla. Todo lo demás que el trampolín mira (Status/Cause vía mtc0, `halted`) solo
cambia en ops que terminan el bloque.

Rinde lo prometido en throughput del hilo de CPU: 456M → 991M instrucciones ejecutadas en
el mismo intervalo de pared. Y **no mueve la aguja**: −5% en threaded-jit (9.37 → 9.84 s),
+3% en lockstep (31.4 → 30.4 s). Porque el hilo de CPU **no es el camino crítico**: esas
instrucciones de más son spin-wait del guest esperando al RCP. Ocupación medida en la
corrida de 200 campos: **RDP 49%, RSP 40%, y suman ≈ el 100% del tiempo de pared** — es
decir, RSP y RDP se serializan. Ahí está el techo, no en la CPU.

Se queda encendido (`KESTREL_JIT_NOFAST=1` lo apaga): es una mejora real y generalizable del
dynarec, y el −5% es el síntoma de otro defecto (amplificación del spin-wait), no suyo.

## 14. MUL.S sin tocar el MXCSR: el producto exacto en `double`

El perfilador de host del hilo de CPU con parallel-rdp (donde el RDP deja de ser el palo
largo y el hilo de CPU pasa a mandar) ponía `kestrel_jitADDS` en 14,7 % y `kestrel_jitMULS`
en 11,6 % de las muestras dentro de la imagen. El coste no era la aritmética: era `mx::prep`,
que en cada operación hace `stmxcsr` + comparación + `ldmxcsr` + `stmxcsr`. Y el `ldmxcsr`
**no se ahorraba nunca**, porque la bandera PE del MXCSR es pegajosa: en cuanto una operación
sale inexacta, todas las siguientes tienen que escribir el registro para limpiarla antes de
poder leer su propio Inexact. `ldmxcsr` serializa el pipe del anfitrión.

**Lo que se puede quitar es solo el producto.** El producto de dos `float` normales o cero es
EXACTO en `double`: la mantisa cabe (24+24 = 48 bits contra 53) y el exponente también
(2^-252 .. 2^256, dentro del rango normal del doble). Así que multiplicar en doble y redondear
UNA vez a simple da el mismo bit que multiplicar en simple — no hay doble redondeo posible — y
el Inexact sale de comparar: si el `float` redondeado vuelto a doble no es el producto exacto,
se perdió algo. Solo queda escribir el modo de redondeo cuando el guest lo cambia (`mx::prepRc`).

Las otras tres **no entran**, y el fuzz diferencial es quien lo dijo:

- **ADD.S/SUB.S**: la suma exacta de dos `float` necesita hasta ~277 bits de rango (exponentes
  muy separados), así que el doble también redondea, y entonces la comparación diría "exacto"
  cuando en simple no lo es. Medido: pasaba en ~1 de cada 3 pares al azar. El VALOR salía
  siempre bien; era la bandera la que mentía.
- **DIV.S**: a/b en doble y luego a simple sí sufre doble redondeo.

Y hay dos casos del producto que tampoco se pueden deducir, porque llevan Overflow o Underflow
además del Inexact y de aquí solo sale el Inexact: con redondeo hacia cero (o al infinito
contrario) un producto que desborda **no da inf sino el máximo finito**, que a ojos de `plain`
es un número normal y se colaría. Se miran los bits del producto exacto en doble contra
`[2^-126, maxfloat]` y fuera de ahí se cede al intérprete. El fuzz con exponentes sesgados a
los extremos (24 M de pares, los cuatro modos de redondeo) da **0 discrepancias** en valor y
bandera, con el 62 % de los casos yéndose al intérprete precisamente por ese sesgo; con
operandos de juego (exponentes normales) el camino rápido se toma siempre.

Gates 14/14 + prdp 3/3, regress=0, md5 sin cambio.


## 15. Cobertura del JIT de CPU: matar al "líder no compilable" (2026-09-01)

Un bloque JIT se compila desde una dirección física y **corta en la primera op que no sabe
emitir**. Si esa op es la PRIMERA (el *líder*), el bloque sale con `nOps == 0`, el fallo se
memoiza en la caché negativa y **cada vuelta por ese PC cae al intérprete para siempre**. Con
`KESTREL_JIT_STATS=1` eso se cuenta por opcode. En SM64 (300 intercambios, hilo de RCP), en la
primera ventana de 4,19 M de entradas al conductor:

| Líder que no compilaba | Entradas | Causa real |
|---|---:|---|
| BEQ (0x04) | 322 k | salto en la ÚLTIMA palabra de la página 4K → su ranura de retardo caía en la página siguiente |
| BNE (0x05) | 31 k | idem, más ranuras con ALU-que-atrapa |
| COP0 (0x10) | 179 k | MTC0 Status 117 k, MFC0 Count 33 k, MTC0 EPC 10 k, ERET/TLB* |
| SPECIAL (0x00) | 82 k | JR con ranura no compilable + DSLL32/DSRA32 (ISA de 64 bits) |

Cinco cambios, todos con semántica de HW genuina y ninguno atado a un test:

**1. Ranura de retardo con ALU-que-atrapa o MFC0.** `compileDelay` probaba `emitSafeOp` →
`emitMemOp` → `emitInterpOp`, pero no `emitTrapAlu` ni `emitCop0`. Las dos emiten su bail
ANTES de tocar el destino, así que el rollback es limpio y el bail puede apuntar al SALTO: el
intérprete re-ejecuta salto + ranura y aplica la semántica de excepción en ranura (EPC = el
salto, `Cause.BD` = 1). El enlace de JAL/JALR ya emitido se reescribe con el mismo valor, que
es idempotente.

**2. Cruzar la página 4K desde ckseg0.** El bloque cortaba SIEMPRE en el borde de página
porque en código TLB-mapeado los VA contiguos no son físicas contiguas. Desde **ckseg0** sí lo
son (VA→PA es un desplazamiento fijo), así que ahí el bloque puede pasarse — y con él la ranura
de retardo de un salto que viva en la última palabra. El bloque que se pasa se marca
`crossPage` y el despachador **se niega a reutilizarlo si el mismo phys se alcanza por TLB**,
donde la página siguiente puede mapear a otro sitio. La caché negativa lleva ahora también la
ruta con la que se anotó el fallo, por lo mismo. Bisector: `KESTREL_JIT_NOXPAGE=1`.

**3. MTC0 / ERET / TLBR / TLBWI / TLBP como op TERMINAL.** Escribir COP0 no se puede absorber a
media faena: Status o Cause pueden dejar una interrupción lista y el bloque no vuelve a mirar
las interrupciones hasta salir. Pero **sí** se puede ejecutar como ÚLTIMA op, cerrando el
bloque detrás; el conductor re-muestrea antes del siguiente. Lo que se gana no es la op sino
que MTC0 deje de ser LÍDER: antes el bloque cortaba justo ANTES de ella y la siguiente entrada
aterrizaba encima. Quedan fuera los registros que el propio bloque adelanta de golpe al salir
(**Count**, y **Random** con su **Wired**, que lo recarga), porque ese sumando machacaría el
valor recién escrito; **Compare** también, porque la guarda de frontera del timer se calculó
con el Compare de la ENTRADA; y **TLBWR**, que indexa por Random. Sin excluir Wired,
n64-systemtest cazaba el desfase exacto: *"Random, 1 instruction after setting Wired = 0"*
esperaba `0x1f` y salía `0x1e`.

**4. MFC0 Count dentro del bloque.** Estaba excluido porque a mitad de bloque va atrasado: el
JIT sube `Count += ops retiradas` al SALIR. Pero el atraso es **exacto y conocido** — el
intérprete lo sube una vez al final de cada `step`, luego durante la op número `idx` del bloque
el valor visible es `entrada + idx`. Se emite `cop0[Count] + idx`, que es el mismo bit que
daría el intérprete, no una aproximación. La frontera `Count == Compare` no puede caer dentro
del bloque (guarda `DR_TIMER`), así que no se pierde ningún latch de IP7. **Cause sigue fuera**:
sus bits IP2/IP7 se refrescan al ENTRAR al bloque y una interrupción que llegue mientras corre
no se ve — leerlo dentro expondría ese sesgo al guest.

**5. ALU de 64 bits con guardia de modo kernel.** DADDU/DSUBU/DADDIU y la familia de
desplazamientos dobles (DSLL/DSRL/DSRA, sus formas `32` y las variables) estaban fuera porque
el VR4300 las reserva (RI, ExcCode 10) en usuario/supervisor sin UX/SX. Pero **en kernel están
permitidas siempre** (`CPU::reserved64`), y el modo se puede comprobar en runtime con la misma
guardia que ya usa el resto del JIT (`KSU == 0` en `Status[4:3]`); cualquier otro caso sale por
el bail sin haber tocado nada y el intérprete levanta la RI exacta. Los desplazamientos de x86
en 64 bits enmascaran la cuenta a `&63` igual que MIPS. DADD/DSUB (con trampa de
desbordamiento) no entran. Bisector: `KESTREL_JIT_NOALU64=1`. El idioma del compilador de SGI
para sacar la mitad alta de un doble — `dsll32` + `dsra32` — valía él solo 27 k + 27 k entradas.

**Resultado** (SM64, 300 intercambios, misma ventana de 4,19 M de entradas):

| | antes | después |
|---|---:|---:|
| Líderes no compilables | ~614 k | **25 k** (solo MFC0 Cause) |
| ops por entrada al conductor | 333 | **445** |
| ops medias por bloque (`avgK`) | — | 10,25 |

En tiempo de pared **SM64 no se mueve**, y es esperable: con SoftRDP o con parallel-rdp el palo
largo es el RCP, no el hilo de CPU (§12-bis). Donde sí se ve es en carga CPU-pura:
n64-systemtest a 1500 M de instrucciones baja de 8,95–9,04 s a 8,83–8,94 s (≈ 2 %) sumando los
dos bisectores. El valor de fondo no es ese 2 %: es que el intérprete deja de comerse 590 k
entradas por ventana, que es exactamente lo que estrangula a un juego CPU-bound como PD.

Gates 14/14 + prdp 3/3, regress=0, md5 sin cambio.

---

## 16. El SoftRDP era el palo largo: −28 % de pared en cinco pasos (2026-09-01)

### El diagnóstico: quién manda en el build por defecto

`KESTREL_HEARTBEAT=1` publica ocupación por hilo. Con SM64 en `threaded-jit` y SoftRDP:

```
[hb] ... occupancy: rdp 89%(cpu 88%) rsp 58%(cpu 57%) cpuWait 0% | rsp 12 Mips busy | pace 61%
```

El hilo del RDP está ocupado el **89 %** del tiempo, el del RSP el 58 %, y la CPU **no espera nunca**
(`cpuWait 0 %`). Con `parallel-rdp` este cuadro cambia por completo — ahí el trabajo se va a la GPU —
pero el build por defecto (y el que corre en máquinas sin Vulkan usable) está limitado por el
rasterizador software. Todo el trabajo de este apartado va ahí.

Métrica honesta: **pared para un número fijo de intercambios de buffer** (`KESTREL_MAXFLIPS=300`),
mínimo de 3 pasadas. El "% de tiempo real" tiene campos de VI en el numerador y sube cuando el
emulador va *peor* (más campos por el mismo trabajo), así que no sirve para esto.

Perfilado: `KESTREL_HOSTPROF=1 KESTREL_HOSTPROF_WHO=rdp`, simbolizado con
`python scripts/hostprof.py <log> build/kestrel64.exe`.

### Paso 1 — dos llamadas a la CRT en el camino caliente (17,3 % del hilo)

El perfil de partida tenía dos entradas que no son trabajo de RDP en absoluto:

| símbolo | % hilo RDP | qué era |
|---|---|---|
| `getenv` | 8,2 % | `if(std::getenv("KESTREL_TRIDBG"))` **por triángulo**, dentro de `run()` |
| `lround` | 6,7 % | `std::lround` en la interpolación de Z y en el paso a S/T de 5.5 |

`getenv` recorre el entorno entero cada vez. Pasa a `static const bool g_triDbg`, igual que el resto
de toggles del fichero.

`std::lround` devuelve `long` y arrastra el contrato de errno/dominio de la CRT, así que clang **no la
puede alinear** ni con `-O3 -march=native`: queda como llamada. El sustituto es exacto en el rango que
usa el rasterizador (`|v| < 2^31`), y `floor`/`ceil` sí se alinean a `roundsd` (SSE4.1, el host lo tiene):

```cpp
static inline auto lroundExact(double v) -> int {
  return (int)(v < 0.0 ? std::ceil(v - 0.5) : std::floor(v + 0.5));
}
```

Redondeo a medio camino: `lround` va *away from zero* y esta versión también (`ceil(-0.5-0.5) = -1`,
`floor(0.5+0.5) = 1`). Bit a bit idéntico, no es una aproximación.

**7,89 s → 6,50 s.**

### Paso 2 — el pliegue del tile, una vez por píxel en vez de por toma

`sampleTexel` volvía a leer el descriptor del tile y a recalcular shift, máscara, clamp/mirror, ancho
de fila, base y paleta **en cada una de las 3-4 tomas del filtro**. Todo eso es constante dentro del
tile. Se parte en dos: `foldOf(tile) -> TexFold` (el pliegue) y `texelAt(fold, s, t)` (la toma).

**6,50 s → 6,40 s.** Menos de lo esperado: el pliegue no era el coste dominante, la decodificación sí.
Se queda igualmente — el reparto que deja es lo que hace posibles los pasos 3 y 4.

### Paso 3 — especializar el formato, y forzar la alineación

El `switch(tl.size)` por formato estaba *dentro* del bucle de tomas. Ahora `TexFold` lleva un `kind`
resuelto una vez, y la decodificación va en `template<u32 K> texelK(...)`, con el filtro de 3 tomas
completo también especializado en `template<u32 K> filterK(...)`. El despacho queda en una macro-lista:

```cpp
#define KEST_TEXKINDS(X) X(TK_CI4) X(TK_IA4) X(TK_I4) X(TK_YUV) X(TK_IA16) \
                         X(TK_RGBA16) X(TK_RGBA32) X(TK_CI8) X(TK_IA8) X(TK_I8)
```

Trampa medida: tras plantillizar, el perfil **seguía** mostrando `texelK<5>` (RGBA16) fuera de línea al
35 %. Con una plantilla instanciada desde tres sitios el heurístico de clang decide no alinear; hace
falta decirlo:

```cpp
template<u32 K> [[gnu::always_inline]] inline auto texelK(const TexFold&, int, int) const -> u32;
```

**6,40 s → 6,30 s.**

### Paso 4 — sacar del bucle de píxeles lo que es constante por primitiva

Dentro de un triángulo no cambian ni el tile, ni el tipo de ciclo, ni el bit de alpha-compare. Se
suben los tres justo antes del bucle de spans, y el filtro se llama ya con el pliegue armado
(`sampleTexFold(texF, su, tu)` en vez de `sampleTexFiltered(texTile, ...)`):

```cpp
const TexFold texF       = foldOf(texTile);
const bool    copyCy     = cycleType() == 2;
const bool    alphaCmpEn = (other_lo & 1) != 0;
```

**6,30 s → 6,20 s.**

Cota superior del camino de texturas, medida con un A/B: `KESTREL_NOFILTER=1` (3 tomas → 1) da
6,5 → 5,4 s, o sea el muestreo de textura es **~25 % de la pared**. Ya no queda mucho ahí sin tocar
exactitud, y eso no se toca.

### Paso 5 — el combinador: plan en vez de `switch` por canal y por píxel

`combineColor` resolvía **ocho selectores** (a/b/c/d de RGB y de alpha) con un `switch` cada uno, para
cada canal, para cada píxel — el 13 % del hilo era despacho puro. Pero esos selectores son constantes
hasta el siguiente `SET_COMBINE`, y el número de ciclos hasta el siguiente `SET_OTHER_MODES`.

`buildCombPlan()` los traduce una vez a índices de fila de una tabla de fuentes; el camino por píxel
materializa **solo** las filas que el plan pide (`need`, máscara de bits) y evalúa la ecuación de 9
bits leyendo `rows[sel][canal]`. Sin ramas de selector.

Tres detalles que son de corrección, no de velocidad:

- **La llave es el estado, no una bandera.** El plan se invalida comparando
  `(combine_hi, combine_lo, cycleType())`. Con una bandera "sucio" puesta desde el comando
  `SET_COMBINE`, una restauración de savestate o una escritura por MCP dejarían el plan rancio; así
  no hay forma de que eso pase.
- **NOISE se sale del camino rápido.** Si un ciclo **que de verdad se ejecuta** selecciona NOISE, el
  plan marca `fast = false` y se cae a `combineColorSlow`, que es la versión genérica intacta. Motivo:
  NOISE consume `std::rand()`, y el orden de consumo es observable en el framebuffer. En 1-cycle el
  hardware evalúa la ecuación del *segundo* ciclo, así que el primero no cuenta para esto.
- **COMBINED del segundo ciclo es el resultado CRUDO del primero**, no el clampado; y el alpha que se
  latchea es el *expandido* (0xff pasa a 0x100). Ambos comportamientos vienen tal cual de la versión
  genérica, que sigue en el fichero como referencia ejecutable.

**6,20 s → 5,68 s.**

### Resultado

| paso | pared SM64 / 300 intercambios |
|---|---|
| partida | 7,89 s |
| 1 — `getenv` + `lround` fuera | 6,50 s |
| 2 — pliegue del tile una vez | 6,40 s |
| 3 — formato especializado + `always_inline` | 6,30 s |
| 4 — invariantes de primitiva fuera del bucle | 6,20 s |
| 5 — plan del combinador | **5,68 s** |

**−28 % acumulado**, sin una sola concesión de exactitud: 14/14 modos + prdp 3/3, `regress=0`,
krom `mean_exact` 88,71 (interp) / 89,26 (prdp) idénticos al baseline, md5 de SM64 sin cambio.

### Dónde queda el perfil ahora

```
25,6 %  SoftRdp::run              (montaje de spans, aritmética en double)
23,1 %  SoftRdp::sampleTexFold
23,8 %  SoftRdp::combineColor (+ el lambda de ciclo)
 9,3 %  SoftRdp::putPixel
 7,0 %  SoftRdp::blendPixel
 7,0 %  SoftRdp::loadTile
```

Ya no hay ningún desperdicio puro. Lo siguiente, por tamaño: `run()` monta cada span con aritmética en
`double` donde el hardware usa punto fijo, y `putPixel`/`blendPixel`/`coverPixel` repiten los mismos
chequeos de límites tres veces por píxel escrito.

---

## 17. SoftRDP, segunda tanda: el combinador en SSE y dos lecturas que sobraban (2026-09-01)

Continuación directa del § 16, mismo método (pared para 300 intercambios de buffer, SM64,
`threaded-jit`, mínimo de 3 pasadas) y mismo perfilador (`KESTREL_HOSTPROF_WHO=rdp`).

### Plan del blender

Igual que el combinador, el blender vivía de redescodificar `other_lo`/`other_hi` **por píxel**: los
cuatro muxes P/A/M/B (con su desplazamiento de 2 según el ciclo), IM_RD, FORCE_BLEND, ANTIALIAS_EN y el
modo de dither. `blendPixel` además llamaba a `cycleType()` tres veces por píxel. `buildBlendPlan()` lo
resuelve una vez, con la misma disciplina de llave: `(other_hi, other_lo)`, no una bandera de suciedad
que un savestate o una escritura por MCP dejarían rancia.

### El test de scissor, tres veces por píxel

`coverPixel → blendPixel → putPixel` es una cadena, y cada eslabón repetía el mismo
`x < sx0 || x >= sx1 || y < sy0 || y >= sy1`. Se parte `putPixel` en el test más `storePixel`
(la escritura sola, `always_inline`), y los llamadores que ya entraron por el test escriben directo.
El efecto secundario importante es que `storePixel` se alinea dentro de `blendPixel` y desaparece del
perfil como llamada.

### `loadTile`: `memcpy` por fila

La carga de textura copiaba **byte a byte con dos comprobaciones de límite por byte** (7,0 % del
hilo). Los texeles de una fila son contiguos en el texture image (paso `bpt`) y también en TMEM, así
que la fila es un `memcpy`. El recorte se hace una vez:

```cpp
if(dst >= 0x1000 || src >= m.size()) return;
u32 lim = std::min(0x1000u - dst, (u32)(m.size() - src));
if(n > lim) n = lim;
```

Copia **exactamente el mismo prefijo** que el bucle que sustituye: los dos límites son monótonos y la
copia iba en orden ascendente, así que el bucle original también paraba en el primero que fallara.

**5,68 → 5,56 s** (las tres cosas juntas).

### El combinador en SSE4.1

Tras el § 16 el combinador era el **38 %** del hilo (26,5 % la función más 11,6 % del lambda de ciclo,
que clang no alineaba). Y los cuatro canales corren *la misma ecuación entera con operandos distintos*:
son un vector de 4×int32 pidiéndolo a gritos.

La única arruga es que los selectores de alpha no son los de RGB. Se resuelve con un blend por carril:

```cpp
auto pick = [&](int i) { return _mm_blend_epi16(rowv[s[i]], rowv[s[i + 4]], 0xC0); };
__m128i A = sexpv(pick(0)), B = sexpv(pick(1)), C = pick(2), D = sexpv(pick(3));
__m128i t = _mm_mullo_epi32(_mm_sub_epi32(A, B), C);
return _mm_add_epi32(_mm_srai_epi32(_mm_add_epi32(t, k80), 8), D);
```

(`0xC0` en `_mm_blend_epi16` son los bytes 12..15, o sea el int de índice 3 — el carril de alpha.)

`special_expand` también vectoriza limpio, y de paso queda más definido que el escalar:

```cpp
__m128i x = _mm_and_si128(_mm_sub_epi32(v, k80), k1ff);
x = _mm_sub_epi32(_mm_xor_si128(x, k100), k100);   // extensión de signo desde el bit 8
return _mm_add_epi32(x, k80);
```

Aritmética entera pura: los valores son idénticos al camino escalar, que sigue en el fichero como
referencia ejecutable y como camino de NOISE. Bajo `#if defined(__SSE4_1__)`, con el escalar de
respaldo para hosts sin SSE4.1.

**5,56 → 5,01 s (−9,9 %).** El combinador cae del 38 % al 19 % del hilo.

### La división del blender, a multiplicador

Sin FORCE_BLEND el RDP normaliza por el peso real de los coeficientes, y eso era **una división entera
por canal y por píxel** (20-26 ciclos cada una). Los rangos están acotados por construcción: los
coeficientes son de 5 bits, así que `sum = (a0>>2) + (a1>>2) + 1` cae en 1..15, y el numerador va
enmascarado a 11 bits. En ese rango `n / d` es **exactamente** `(n * ceil(2^16/d)) >> 16` — comprobado
exhaustivamente para todos los `d` y todos los `n` posibles, no por muestreo.

En SM64 la medida sale **neutra**: casi todos sus píxeles se van por los atajos del blender y nunca
llegan al divisor. Se queda igualmente — es estrictamente menos trabajo, y las roms que sí usan ese
camino lo pagan entero.

### La lectura de framebuffer que se tiraba

Ese resultado neutro apunta a lo de verdad caro. El blender tiene dos atajos que devuelven el color P
intacto (blender deshabilitado; y el caso opaco clásico A=IN alpha, B=1−A, alpha==0xff), y el camino
llegaba a ellos **después** de haber leído el framebuffer. Los dos atajos se deciden con datos que ya
están en la mano, y el color de memoria sólo hace falta si P es CLR_MEM:

```cpp
const bool shortcut  = !blendEn || (bp.asel == 0 && bp.bsel == 0 && (src & 0xff) == 0xff);
const bool memUnused = shortcut && bp.psel != 1;
u32 memc = (bp.usesMem && !memUnused) ? readFb(mem, x, y) : 0;
```

Cuando el color de memoria sí cuenta la regla es la de siempre (`usesMem ? readFb : 0`), así que no
cambia ningún resultado: sólo se deja de leer RDRAM para tirarlo.

**5,05 → 4,93 s (−2,5 %).**

### Resultado de la tanda

| paso | pared SM64 / 300 intercambios |
|---|---|
| fin del § 16 | 5,68 s |
| plan del blender + `storePixel` + `memcpy` en `loadTile` | 5,56 s |
| combinador en SSE4.1 | 5,01 s |
| división del blender a multiplicador | 5,05 s (neutro en SM64) |
| lectura de framebuffer elidida | **4,93 s** |

**Acumulado de las dos tandas: 7,89 → 4,93 s, −37,5 %**, con md5 de SM64 sin cambio en todo el camino.

Perfil al cierre: `run` 29,8 % · `blendPixel` 23,2 % (ya con `storePixel` y el dither dentro) ·
`sampleTexFold` 21,6 % · `combineColor` 19,0 % · `blendColor` 4,5 % · `coverPixel` 1,9 %.
Ya no queda desperdicio identificable: lo que hay es el trabajo.

---

## 18. Tercera tanda del SoftRDP: vectorizar la interpolación y plegar cada coordenada una vez

El cierre del § 17 decía que ya no quedaba desperdicio identificable. Era falso: quedaba, sólo que no
en forma de *trabajo tirado* sino de *trabajo repetido*. Tres pasos, todos exactos, todos guiados por
el perfil de muestreo del hilo del RDP (`KESTREL_HOSTPROF=1 KESTREL_HOSTPROF_WHO=rdp`).

### Sombra y S/T por escanlínea, en SSE

`run` era el líder del perfil (29,8 %). Dentro del bucle por píxel había ocho interpolaciones lineales
`e + d/dx * dx` en `double` — R, G, B, A, S, T — cada una seguida de un redondeo y un clamp escalares.
Los cuatro canales de color hacen exactamente la misma cuenta, y S/T también entre sí: dos parejas de
`__m128d` por escanlínea (valor de arranque y pendiente) y la cuenta sale en tres multiplicaciones
vectoriales en vez de seis escalares.

```cpp
const __m128d vRG = _mm_set_pd(eG, eR), vBA = _mm_set_pd(eA, eB), vST = _mm_set_pd(eT, eS);
const __m128d cRG = _mm_set_pd(dGdx, dRdx), cBA = _mm_set_pd(dAdx, dBdx), cST = _mm_set_pd(dTdx, dSdx);
```

El anfitrión es un Nehalem sin FMA, así que `a*b+c` escalar y `mul`+`add` vectoriales dan el mismo
bit; las conversiones son `cvttpd_epi32` sobre el mismo `+0.5` que hacía el `clamp8` escalar, y el
clamp es `min/max` entero. Mismo resultado, no parecido.

**El primer intento no ganó nada** (4,926 → 4,977 s): empaquetaba el vector en un `alignas(16) int
t[4]` y volvía a leerlo con desplazamientos escalares, y ese ida y vuelta por memoria paga un
store-forwarding entero. Empaquetando con `_mm_shuffle_epi8` + `_mm_cvtsi128_si32`, sin tocar
memoria, **4,926 → 4,856 s**.

### El filtro de 3 tomas, también en vector

Misma idea un piso más abajo. `filterK` hacía la interpolación del filtro canal a canal en un bucle
`for(i = 0; i < 4; i++)` sobre un `int o[4]`. Los cuatro canales comparten pesos (`wx`, `wy`), el
mismo `>>5` aritmético y el mismo clamp final, así que van en un `__m128i`:

```cpp
__m128i acc = _mm_add_epi32(_mm_mullo_epi32(_mm_sub_epi32(v10, b), _mm_set1_epi32(wx)),
                            _mm_mullo_epi32(_mm_sub_epi32(v01, b), _mm_set1_epi32(wy)));
acc = _mm_add_epi32(_mm_srai_epi32(_mm_add_epi32(acc, _mm_set1_epi32(0x10)), 5), b);
return packRgbaClamped(acc);
```

Las dos ayudas de empaquetado (`unpackRgba` / `packRgbaClamped`) quedan a nivel de fichero porque las
usan el filtro y el camino MID_TEXEL. La versión escalar se conserva bajo el `#if` como referencia
legible del mismo cálculo. **4,856 → 4,772 s**.

### Cuatro pliegues por píxel, no seis

El paso que más dio, y el que no era vectorización sino contar. `texelK<K>` hacía dos cosas pegadas:
**plegar** la coordenada (SHIFT del tile, luego clamp/mirror/mask) y **leer** el texel de TMEM. El
filtro de 3 tomas la llamaba tres veces, así que plegaba seis coordenadas por píxel. Pero sus tres
tomas sólo usan **cuatro** coordenadas distintas: `s0`, `s0+1`, `t0`, `t0+1`.

Separadas las dos mitades — `foldCoord` (una coordenada) y `fetchK<K>` (lectura + decodificación) —
el filtro pliega cuatro veces y comparte:

```cpp
const int sw0 = foldCoord(s0,     f.shiftS, f.maskS, f.cmS, f.sMax),
          sw1 = foldCoord(s0 + 1, f.shiftS, f.maskS, f.cmS, f.sMax),
          tw0 = foldCoord(t0,     f.shiftT, f.maskT, f.cmT, f.tMax),
          tw1 = foldCoord(t0 + 1, f.shiftT, f.maskT, f.cmT, f.tMax);
```

`texelK<K>` sigue existiendo, ahora como `fetchK<K>(f, foldCoord(s...), foldCoord(t...))`, que es
literalmente lo que hacía antes: el muestreo puntual y `texelAt` no cambian de semántica. El pliegue
es puro (depende sólo de la coordenada y del tile), así que compartirlo entre tomas no puede cambiar
un solo texel. **4,772 → 4,461 s (−6,5 %)**, el mayor salto de la tanda.

Dos tropiezos de compilación por el camino, ambos de sintaxis y ninguno de semántica: `static
[[gnu::always_inline]] inline` no se acepta en ese orden (`an attribute list cannot appear here`), y
el parche dejó un `template<u32 K>` colgando delante de la definición de `foldCoord`, que no es
plantilla — de ahí el `out-of-line definition ... does not match any declaration`.

### Resultado de la tanda

| paso | pared SM64 / 300 intercambios |
|---|---|
| fin del § 17 | 4,93 s |
| sombra + S/T por escanlínea en SSE | 4,86 s |
| filtro de 3 tomas en SSE | 4,77 s |
| pliegue/lectura separados (4 pliegues, no 6) | **4,46 s** |

**Acumulado de las tres tandas: 7,89 → 4,46 s, −43,5 %**, md5 de SM64 idéntico en cada paso.

Perfil al cierre: `sampleTexFold` 39,0 % · `run` 20,4 % · `combineColor` 18,0 % ·
`blendPixel` 11,1 % · `coverPixel` 5,8 % · `blendColor` 4,1 % · `ditherRgb` 1,7 %.
El muestreo de textura vuelve a ser el líder claro, y ahora por trabajo real: por píxel filtrado
quedan dos redondeos a punto fijo, cuatro pliegues y tres lecturas de TMEM con decodificación.

## 19. Cuarta tanda: dónde se acaba el filón, y por qué

Las tres tandas anteriores bajaron la pared de SM64/300 intercambios de 7,89 a 4,46 s. La cuarta
tanda es la primera que **no mueve la pared**, y merece quedar escrita entera porque el resultado
negativo es el hallazgo, no el fracaso.

### 19.1 La matriz de sensibilidad

Mismo binario, SM64, 300 intercambios de buffer, hilos + JIT, mínimo de tres tiradas:

```
4.610 4.463 4.491     <- KESTREL_JIT=1                        (línea base)
3.164 3.109 3.153     <- KESTREL_JIT=1 KESTREL_NORASTER=1
24.340 24.337 24.343  <- KESTREL_JIT=0
```

`KESTREL_NOVIDEO=1` no cambia nada: no estamos atados a la presentación ni al vsync.
Latido de ocupación: `rdp 82 %(cpu 82 %) rsp 57 %(cpu 56 %) cpuWait 0 % | pace 0 %`.

De ahí sale el diagnóstico completo:

- El rasterizado del SoftRDP son **~1,35 s de los 4,46** (4,46 − 3,11).
- El hilo del RDP va al 82 % y es **el único en el camino crítico**. El del RSP tiene holgura
  (57 %) y el de CPU no espera a nadie (`cpuWait 0 %`).
- El suelo de ruido medido en este anfitrión es **±1,5 %**. Una mejora que valga el 1-2 % del hilo
  del RDP vale <1 % de la pared: **no se puede ver de una en una**, ni con el mínimo de tres.

Consecuencia dura: mientras el RDP sea el palo largo, **el trabajo que se le quite a la CPU o al
RSP no se ve en la pared**. Eso no lo hace inútil (baja el consumo, y en cuanto el RDP deje de ser
el limitante — con `parallel-rdp`, por ejemplo — pasa a contar), pero sí lo hace inmedible aquí.

### 19.2 Lo que se hizo, y qué midió cada cosa

Las cuatro son exactas (mismo bit) y estrictamente menos trabajo. Todas midieron dentro de la
banda de ruido (4,44-4,50 s). **Se quedan**: son menos trabajo real, la regla de la casa es que
todo suma, y ninguna añade complejidad que haya que mantener a cambio de nada.

1. **`stFixed`** — las dos coordenadas de textura a punto fijo 10.5 en un solo `cvttpd2dq`.
   `trunc(v + copysign(0.5, v))` es exactamente el `floor(v+0.5)` / `ceil(v-0.5)` del escalar:
   para `v >= 0` el argumento es positivo (trunc == floor) y para `v < 0` es negativo
   (trunc == ceil). Sin ramas, sin dos llamadas.

2. **Filtro de textura a carriles de 16 bits.** El rango del filtro cabe de sobra en `int16`:
   diferencia de texeles ±255, peso 0..32, producto ±8160, suma de dos más el `+0x10` de redondeo
   = ±16336. El resultado entero es **el mismo** que en 32 bits, pero `pmullw` es una micro-op en
   Nehalem contra las **seis** de `pmulld`.

3. **Pliegue rápido por tile.** El caso común (`shift == 0`, con `mask`, sin clamp ni mirror) se
   detecta una vez por tile en `foldOf` (`fastS`/`fastT` + máscara `andS`/`andT`) y el pliegue por
   coordenada queda en un AND en vez de la cadena SHIFT → mirror → mask → clamp.

4. **`MFC0` del RSP directo al decodificador de MMIO** (`Memory::rcpReg32`). `Memory::read32`
   empieza por cart, dominio de save y `resolve`, y **`resolve` recorre la lista de regiones entera**
   antes de rendirse para una dirección de MMIO. Las bases de SP y DPC que lee el COP0 del RSP
   nunca caen en una región, ni en cart, ni en el dominio de save, así que el valor es idéntico.
   El microcódigo sondea `SP_DMA_BUSY`/`SP_STATUS` en bucle: ese recorrido salía **el 32 % del hilo
   del RSP**. Quitarlo: **0 en la pared**, exactamente lo que predice la matriz de arriba.
   `mtc0` se deja adrede en `write32`, que es lo que conserva la trampa `KESTREL_TRAPSPREG`.

### 19.3 Lo que se revirtió

**Granularidad de `rcpPace`** (`KESTREL_PACECALL`): llamar a `memory.rcpPace(cpu.retired)` cada N
ops retiradas en vez de cada 64. `rcpPace` es el 16,9 % del perfil del hilo de CPU, así que parecía
la palanca obvia. Midió **exactamente neutro** con y sin rasterizado — de nuevo, el hilo de CPU
tiene holgura. **Revertido**: cambiaba la heurística de un regulador de sincronía a cambio de cero
medible, y eso es complejidad que se paga en riesgo, no una optimización.

Es la línea que separa las cuatro que se quedan de la que se va: menos trabajo exacto se queda
aunque no se vea; tocar un regulador para nada, no.

### 19.4 Perfil del hilo de CPU, para cuando toque

Anotado aquí porque será el siguiente palo largo en cuanto el RDP deje de serlo:

`jitTryBlock` 37 % · `rcpPace` 16,9 % · `kestrel_jitProceedTramp` 14,4 % · `dcFill` 11,5 % ·
`jitMULS` 7,0 % · `jitADDS` 6,4 % · `dcFlush` 3,5 %.

El 14,4 % del trampolín es exactamente lo que se llevaría el enlace directo de bloques
(`jmp rel32` con backlinks por bloque, punto 0 de `docs/RSP-JIT.md ## Siguiente`), y el 13 % de
`jitMULS`+`jitADDS` es FPU que hoy sale por llamada a helper. Ninguna de las dos mueve la pared
**hoy**, por lo de § 19.1.

### 19.5 Las tres palancas que sí quedan

Por tamaño esperado, no por facilidad:

1. **`parallel-rdp` por defecto** (`-DKESTREL_PRDP=ON`). Se lleva los 1,35 s de rasterizado de
   golpe. Es decisión de política de build (exige Vulkan en el anfitrión), no una optimización.
2. **Enlace directo de bloques del JIT de CPU** — 14,4 % del hilo de CPU, invisible hoy.
3. **FPU del JIT en línea** — 13 % del hilo de CPU, invisible hoy.

Resumen de la tanda: pared **4,46 s**, sin cambio; md5 de SM64 idéntico; los dos portones verdes.

---

## 20. La caché de destinos indirectos (ITC): que JR/JALR no salga del código generado (2026-09-02)

### 20.1 El agujero que dejaba el enlace de bloques

El enlace de bloques del JIT ata un sitio de salida a un destino **estático**: la dirección del
salto se conoce al compilar (rama condicional, `J`, `JAL`, caída al bloque siguiente), así que se
parchea un `jmp rel32` y el bloque siguiente se entra sin volver al despachador.

`JR`/`JALR` no tienen destino estático. Su salida siempre iba por el camino lento: escribir `pc`,
desmontar el marco, volver a `jitTryBlock`, buscar el bloque en el mapa, montar marco otra vez.
Y `JR $ra` es el retorno de **toda** llamada a función del guest — en el perfil, el trampolín
(`kestrel_jitProceedTramp`) se comía el 14,4 % del hilo de CPU (§ 19.4). El enlace estático no
podía tocarlo por construcción.

### 20.2 Lo que se ha hecho

Una caché de correspondencia directa VA → entrada de bloque, consultada **dentro** del código
generado, justo antes de la salida lenta de control:

- `CodeCache::itc`: 4096 entradas de 16 B (`{u64 va; u64 code;}`), 64 KB, índice `(va >> 2) & 4095`.
  `va = 0` nunca es un destino válido, así que la tabla vacía siempre falla.
- Al compilar un bloque enlazable se escribe su par `{pc, linkEntry}` en la ranura que le toca.
  Direct-mapped: la última escritura gana, sin política de reemplazo ni contadores.
- En `emitCtrlExit`, cuando el destino no es constante y el bloque es enlazable, se emite la
  sonda: cargar el puntero a la tabla, `shr rax,2` / `and` con la máscara / `shl rax,4`, sumar,
  comparar el VA guardado con el VA de destino en `rcx` y, si coincide, contabilizar las
  instrucciones pendientes y `jmp qword [rdx+8]`. Si falla, cae a la salida lenta de siempre.
  Todo el coste del fallo son cuatro instrucciones y un salto no tomado.

Sólo se arma cuando ya se cumplen las condiciones del enlace (`g_jitLink && !g_jitDiffAny &&
ck0Entry`), es decir **sólo VAs de ckseg0**: sin TLB, la traducción VA→física es fija, así que
una entrada cacheada no puede quedar apuntando a otro código por un cambio de mapeo. La
invalidación va con la que ya existía: `itcClear()` en `CodeCache::clear()`, al principio de
`unlinkTo(phys)` (SMC) y al principio de `unlinkAll()` (invalidación de I-caché) — en `unlinkAll`
**antes** del corto-circuito por `anyLinked`, que si no dejaría entradas vivas apuntando a código
muerto.

El prólogo de `linkEntry` no cambia: sigue re-comprobando interrupciones, flanco del temporizador
y saldo de cadena, así que entrar por la ITC es exactamente tan seguro como entrar por un enlace
estático. `KESTREL_JIT_NOITC=1` la apaga para el A/B.

### 20.3 Medida

`validate.py bench`, SM64, 200 intercambios de buffer, threaded-jit, mínimo de 3 pasadas,
tres tandas independientes:

| | pared (min de 3) |
|---|---|
| ITC puesta | **3,11 – 3,13 s** |
| `KESTREL_JIT_NOITC=1` | 3,19 – 3,20 s |

**≈ 2,5 % de pared**, con las distribuciones sin solaparse en ninguna de las tres tandas. Es una
mejora pequeña, sí, y entra igual: el techo no lo mueve una palanca, lo mueven todas.

### 20.4 Tamaño de la tabla y guardas de barrido

Tres cosas más, medidas después:

1. **Índice mezclado.** El índice era `(va>>2) & mask`: directo, y por tanto dos destinos que
   disten un múltiplo exacto del tamaño de la tabla (16 KB de VA con 4096 entradas) se echan
   mutuamente para siempre. Ahora es `((va ^ (va>>12)) >> 2) & mask` — cinco instrucciones en vez
   de tres, dos más en la sonda. Ganancia en despachos: 605 → 620 ops por entrada al driver
   (+2,5 %). Pequeña: el aliasing no era el cuello.
2. **`itcAny`.** `itcClear()` recorría la tabla entera en cada barrido de I-caché. Con 1549
   barridos por tanda de SM64, eso son 1549 memset de 64 KB (256 KB ahora) casi siempre sobre
   una tabla ya vacía. Con la bandera `itcAny`, igual que `anyLinked` para los enlaces estáticos,
   el barrido sólo se paga si hay algo que borrar.
3. **Tamaño.** `KESTREL_JIT_ITCBITS` lo hace ajustable en ejecución (8..20, por defecto 14).
   A/B en pared (200 intercambios, mínimo de 3-4 pasadas, dos tandas):

   | bits | entradas | tabla | pared (min) |
   |---|---|---|---|
   | 12 | 4096 | 64 KB | 2,439 s |
   | 13 | 8192 | 128 KB | 2,451 s |
   | **14** | **16384** | **256 KB** | **2,418 s** |
   | 16 | 65536 | 1 MB | 2,515 s |

   Entre 12 y 14 hay ~1 %, casi ruido, pero 14 gana las dos tandas y en despachos gana claro
   (626 → 646 ops por entrada). 16 ya pierde de verdad: 1 MB de tabla desaloja de la caché del
   anfitrión los datos que el guest está usando. Por defecto **14**.

### 20.5 Lo que NO era el cuello (tres hipótesis muertas)

Con la ITC puesta se despachan ~605 ops por entrada al driver, unos 12-13 bloques encadenados,
mientras el permiso concedido es siempre el tope de 4096 ops (`[permiso] tope=3,1 M`). O sea:
aproximadamente **una salida de cada doce no encadena**. Dos candidatos, los dos descartados por
medida:

- *«El desenlace tras cada barrido de I-caché»*: con un cortocircuito temporal del `unlinkAll`
  las ops por entrada pasaron de 604,88 a 616,84 (+2 %). Sólo hay 1549 barridos por tanda. No es.
- *«Los fallos por conflicto de la tabla directa»*: el índice mezclado dio +2,5 % (arriba). Casi
  tampoco.

Queda como candidato vivo, sin probar: la ITC **sólo la rellena el driver**, en un despacho ya
validado. Un bloque al que sólo se llega por cadena nunca recibe entrada, así que el hueco sería
de **cobertura**, no de conflicto. Rellenar al enlazar/compilar exigiría guardar la VA de
compilación del bloque para que la entrada siga siendo segura.

#### La causa real de las cadenas cortas — y por qué arreglarla salió CARO

Para dejar de adivinar se añadieron contadores (todos detrás de `KESTREL_JIT_STATS`, sin coste
cuando están apagados):

- `[salidas] enlace / itc / lentaDirecta / lentaIndirecta` — se emiten **en línea** en el código
  generado, uno por cada camino de `emitCtrlExit`. Dicen por dónde sale un terminador de salto.
- `[retorno] salto / completo / corto` — los cuenta el driver mirando el `Rraw` del bloque:
  salió por un terminador de salto, agotó el bloque sin salto, o abortó a media altura.
- `[cadena] rotasPorTramp` — veces que el trampolín deniega el permiso y rompe la cadena.

El reparto en SM64 fue concluyente: **salto 86 514 · completo 2 248 311 · corto 345 754**. El
84 % de los despachos del driver no venían de un salto mal enlazado, sino de bloques que se
**acababan sin salto** (tope de `kMaxOps`, o siguiente op no compilable) y devolvían el control
para que el driver avanzase `pc` y volviera a entrar. Ni el trampolín (0,02 roturas por entrada)
ni las salidas lentas de salto (43 k frente a 2,88 M entradas) explicaban nada.

Así que se implementó el **enlace secuencial**: en la salida sin salto, emitir la misma cola de
`emitCtrlExit` con el destino constante `entryVA + 4*nOps` como único candidato. Funcionó
exactamente como se esperaba a nivel estructural…

| | entradas al driver | ops por entrada | `completo` |
|---|---|---|---|
| sin enlace secuencial | 2 883 584 | 580 | 2,25 M |
| con enlace secuencial |   327 680 | 4902 | 0 |

…y **midió peor**:

| carga | sin | con | con, sólo `nOps==kMaxOps` |
|---|---|---|---|
| `n64-systemtest` (única CPU-bound aquí) | **13,27 s** | 14,84 s (+12 %) | 13,80 s (+1,6 %) |
| SM64 800 intercambios | 8,536 s | 8,531 s | — |

SM64 empata porque ahí el palo largo es el RSP (CPU 465-477 %, `cpuWait 0 %`, `rsp 68 %`): el
hilo de CPU no es el que manda y no se puede medir nada suyo con esa pared. En la carga que sí
es CPU-bound la pérdida es clara y se reprodujo dos veces, y restringir el enlace a los bloques
que terminan por tope (código recto largo, sin los finales por op no compilable) sólo reduce el
daño, no lo invierte.

La lectura: **el viaje de vuelta al driver es barato** — revalidar la línea de I-caché son un
par de comparaciones y una llamada indirecta ya predicha — y sale más caro pagar en *cada* final
de bloque los cuatro almacenamientos de control (`pc`, `nextPc`, `inDelay`, `justBranched`) más
la guarda del enlace que ahorrarse el viaje. Revertido; queda el comentario largo en
`compileBlock` para que nadie lo reintente a ciegas. Los contadores se quedan: son la única
forma de ver este reparto.

**Corolario, que es lo importante:** el cuello del hilo de CPU **no es el despacho**
(`jitTryBlock` ~5,8 % de las muestras). Es el **código generado**. Las siguientes palancas van
ahí: COP1 simple en línea (~9-10 %), sombra de MXCSR (~0,7 %), `dcFill`/`dcFlush` (4,5 %).

### 20.6 Correctitud

Integridad por construcción (la sonda sólo elige *por dónde* se entra a un bloque que el
despachador habría encontrado igual, con el mismo prólogo). Comprobado además:

- `sm64[jit]` y `sm64[threaded-jit]` md5 idéntico al de siempre.
- `systemtest[jit]` 0/3721 · 0/2 · 0/6.
- Los dos portones verdes con la ITC puesta.

## 21. Propuesta externa de optimizacion, punto por punto (2026-09-25)

Llego un documento de propuestas de rendimiento. Se comprobo cada punto contra el codigo y,
donde hacia falta, con una medida. Resultado: **nada accionable**. Se deja escrito para no
volver a evaluarlo.

| propuesta | veredicto |
|---|---|
| RDRAM de solo lectura + `VirtualProtect`/VEH para SMC | **Descartada**: techo medido ~1 % y rompe fidelidad (ver abajo) |
| Enlace directo de bloques, sin volver al despachador | **Ya hecho** de fabrica (+ ITC para `JR`/`JALR`) |
| Mapeo fijo de gpr MIPS a registros x86-64 | **Ya hecho, y mejor**: cache DINAMICA de 5 registros |
| `[[unlikely]]` en el camino caliente | **Redundante** con PGO puesta |
| COP2 con AVX2/AVX-512 | **Imposible en este anfitrion**: no hay AVX |
| HLE de microcodigo por firma de IMEM | **Ya existe** la via HLE; el LLE es el oraculo |
| Espera adaptativa en `spReadSync` (girar y luego dormir) | **Ya hecho y barrido**; la siesta midio PLANA |
| MMIO sin mutex, registros sombra | **Ya es asi**: `coreMutex` no aparece en la ruta MMIO |
| LTO / LTCG | **Ya puesta**: `-flto=thin` de fabrica |
| `always_inline` en `cartNow` y compania | Marginal con LTO + PGO ya puestas |

### Lo unico que pedia medida: cuanto cuesta hoy la validacion SMC

`KESTREL_JIT_NOSMC=1` quita la validacion ENTERA, o sea que da el **techo absoluto** de
cualquier esquema de paginas protegidas. Dos rondas, min de 4, 300 intercambios,
`threaded-jit`:

| juego | base | `NOSMC=1` |
|---|---|---|
| SM64 | 7,70 / 7,61 s | 7,54 / 7,55 s |
| Perfect Dark | 4,53 / 4,54 s | 4,55 / 4,52 s |

SM64 ~**1,3 %**, PD **plano** (las lecturas se cruzan). Y eso es el techo de borrar la
comprobacion; un esquema con excepciones del SO se come parte de ese 1,3 % en el manejador.

Ademas **no seria fiel**. La validacion no mira RDRAM: mira la **linea de I-cache**, por sello,
y solo compara palabras cuando la linea se ha vuelto a rellenar (un bloque de 16 ops = 3
comparaciones de `u32`). Es asi a proposito, porque en el VR4300 un DMA que reescribe RDRAM sin
invalidar deja a la CPU ejecutando codigo **stale** de I-cache. Proteger paginas de RDRAM
invalidaria justo donde el hardware NO invalida. Y las escrituras a RDRAM salen de cuatro
sitios -- CPU, DMA del SP, PI, RDP --, o sea excepciones de kernel en los hilos worker.

### Por que el mapeo fijo de registros ya esta superado

`jit.cpp:311` tiene `kRcRegs = { RSI, RDI, R13, R14, R15 }`, cinco ranuras que se asignan
**por bloque y por uso**, con volcado dirigido (`writebackOne`) y con instantanea para los stubs
de salida. Fijar `$sp` o `$v0` a un registro seria un caso PARTICULAR peor: gasta una ranura
aunque ese gpr no se toque en el bloque.

### AVX

Preguntado al propio compilador con `-march=native` en este anfitrion (i7-870, Nehalem, 2009):
solo `__SSE4_2__` y `__POPCNT__`, **ningun `__AVX__`**. `_mm256_madd_epi16` no existe aqui.
