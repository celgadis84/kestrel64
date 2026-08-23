# Gate wall-clock baselines

Reference run times for `scripts/validate.sh`. Their purpose is not performance
tracking — it is **hang detection**. A gate that normally finishes in 17 s and is
still running at 300 s is deadlocked, not slow, and should be bisected as a bug
rather than retried with a bigger timeout.

Host: i7-870 (Nehalem, 4c/8t), Windows 11, MSYS2 CLANG64 build, `build/` (SoftRDP,
PRDP off). Numbers are wall clock, krom with the default 4 parallel jobs.

Measured 2026-08-18, commit = DPC counters + RSP threaded launch/PC-publication fix.

| Gate | interp | JIT | threaded |
|------|--------|-----|----------|
| systemtest (`n64-systemtest.z64 --run`) | 16-17 s | 25 s | 17 s |
| krom 371-ROM suite (4 jobs) | 238-250 s | 91-92 s | 140 s |
| SM64 300M ops (framebuffer md5) | 20 s | 15 s | 16 s |
| **`validate.sh all` total** | **~285 s** | **~132 s** | **~175 s** |

krom interp went 139 s -> ~240 s on 2026-08-18 when the gate stopped capping ROMs at a
fixed instruction count and started running them until the picture is finished
(`--stable/--maxflips/--maxsyncs`); the CPU decoders now run to completion. That is the
expected cost, not a regression. JIT/threaded rows are still the pre-change measurement.

Not part of `validate.sh` (run it by hand when the RDP cost model or the depth path
changes — see `docs/RDP-TIMING.md`):

| Battery | interp |
|---------|--------|
| RDP-Timing-Tests sweep (`rdp_fill_timing.z64`, `TOTAL_RUNS=8`, 100 configs) | 97 s |

systemtest's own internal report ("Finished in N s") is ~7 s in every mode; the rest
of the wall time is ROM load plus boot. The historical 6.86 s quoted throughout
STATUS.md is that internal figure, not wall clock — do not compare the two.

`validate.py` default timeouts: `--st-timeout 300`, `--sm64-timeout 600`,
`--timeout 90` (per krom ROM). Those are ~17x the measured systemtest time, so a
timeout there always means a genuine hang.

## Known hang signature (fixed 2026-08-18)

`systemtest --mode threaded` froze at *"Running RSP VRSQ (all 16 bit values)"* after
~20 s of progress and never advanced. Two distinct threaded-only bugs, both real
RCP-semantics violations, not test-specific quirks:

1. **Dropped RSP launch.** A lone CLEAR_HALT write was gated on the emulator's
   `rspBusy` worker flag, so a launch issued while the worker was still winding down
   from the previous task was silently discarded — the task never ran and the CPU
   polled a BREAK that never came. Launches are now gated on the RSP's actual HALT
   bit (the hardware condition) and ordered behind the wind-down (`rspAwaitIdle`).
2. **PC writeback published after HALT.** `Rsp::step()` set `HALT|BROKE` at BREAK and
   only afterwards wrote `sp_pc` back. The CPU treats HALT as "task over" and
   immediately writes the next task's SP_PC, which the late writeback then clobbered
   — the next task started at the old BREAK and produced nothing (`a=0x0` in
   `RSP VRCP (all 16 bit values)`, ~1 failure per 65536 iterations, nondeterministic).
   BREAK now only latches `broke`; the status publish happens after the PC writeback.

`Rcp::sp_status` was also made `std::atomic<u32>` — one register updated by both the
CPU (control writes) and the RSP worker (BREAK) cannot be a plain `u32` without
losing updates.

## Entorno del gate (dos fallos que parecen bugs y no lo son)

`scripts/validate.py` necesita a la vez:

- el **python de Windows** (`~/AppData/Local/Programs/Python/Python311/python`), que es el
  que tiene numpy — el de MSYS/CLANG64 no lo tiene, y sin numpy las 371 ROMs salen `CMPERR`;
- `/c/msys64/clang64/bin` en el **PATH**, porque el exe carga sus DLL de ahi — sin eso
  arranca con `0xC0000135` y las 371 salen `NODUMP rc=3221225781`.

Es decir: `export PATH=/c/msys64/clang64/bin:$PATH` y llamar al python de Windows por ruta.
`preflight()` comprueba las dos cosas y aborta en un segundo en vez de a los cuatro minutos.

## 2026-08-19 — `gate_all.sh` (cinco modalidades + krom) y la puerta `bench`

`sh scripts/gate_all.sh` = systemtest + sm64 en interp / jit / jit-nolink / threaded /
threaded-jit, luego krom. Total medido: **~7 min**.

| Tramo | interp | jit | jit-nolink | threaded | threaded-jit |
|---|---|---|---|---|---|
| systemtest | 15 s | 9 s | 9 s | 16-17 s | 9 s |
| sm64 (60 campos) | 13-14 s | 8 s | 9-10 s | 11 s | 3-4 s |
| krom 371 ROMs (4 jobs) | 163-167 s | — | — | — | — |

`validate.py bench --bench-runs 2` (600 campos VI de SM64, dos pasadas) = ~25 s en
threaded-jit, ~160 s en interp. Si una pasada de threaded-jit pasa de ~15 s con la maquina
ociosa, hay contencion (otro gate corriendo) o una regresion; no subir el timeout.

## 2026-08-20 — `gate_all.sh` medido, y la prueba larga de SM64

`sh scripts/gate_all.sh`: **282-293 s** de reloj de pared (tres medidas: 282, 283, 293 s),
maquina ociosa. El propio script imprime `GATE_WALL=<s>` al final; si sale muy por encima
de ~300 s, es contencion o cuelgue, **no** subir el timeout.

Prueba larga de cuelgue (la que destapo la carrera del FIFO del RDP):

```
KESTREL_THREADS=1 KESTREL_JIT=1 KESTREL_HANGDOG=8 KESTREL_NOVIDEO=1 \
KESTREL_MAXINSN=99000000000 KESTREL_MAXFLIPS=2500 ./build/kestrel64.exe "<sm64>" --run
```

= **118-120 s** para 2500 campos (~8.6 G instrucciones), y debe terminar con
`[frames] 2500`, sin lineas `[rdp!]` ni `[hangdog]`. Borrar el `.eep` antes: con partida
guardada la demo de attract toma otro camino y la comparacion no vale. Antes del fix esta
misma prueba se iba a los 300 s y moria por timeout, con ~53 anomalias `[rdp!]`.

## 2026-08-20 — likely-branches en el JIT + regulador CPU<->RSP (Threaded)
- `sh scripts/gate_all.sh`: **ALL OK**, krom regress=0, md5 SM64 `466282775dbd0ac084946558a1c30771`
  en los 5 modos. Wall ~285s (baseline 282-293s).
- SM64 2500 swaps (`KFLIPS=2500 sh scripts/perf.sh`): **110.5s** (antes 119s), `[frames] 2500`,
  sin `[rdp!]` ni `[hangdog]`.
- SM64 500 swaps: 20.9s -> **20.2s**, 23.92 -> **24.70 swaps/s**, 1607M -> **1223M** instrucciones
  de guest (el regulador quita el giro en vacio, no trabajo util).

## 2026-08-20 — permiso de guarda con regulacion (threaded)

- `gate_all.sh`: ALL OK, krom regress=0, md5 sm64 `466282775dbd0ac084946558a1c30771`.
- SM64 2500 swaps (threaded-jit): **110.8s / 22.55 swaps/s / 6678M insns / anom=0**
  (antes del regulador: 119s).
- SM64 300 swaps: 24.50 → 25.21 swaps/s (+2.9%) al mover la regulacion CPU<->RSP dentro
  del permiso (`jitGuard`) y quitar la guarda `rsp.running` del codigo emitido en Threaded.
- Barrido `KESTREL_PACESLACK` {64,256,1024,8192,32768,131072}: el default 8192 es el mejor.
- `--vubench`: 21.28 → 18.27 ns/op tras quitar la division entera del propio banco de pruebas
  y alinear `R128` a 16 (RSP: 34.9 → 36.1 Mips).

## 2026-08-20 — interprete del RSP: donde se va el tiempo (banco `--rspbench`)

`--vubench` solo medía la ALU vectorial, que resultó ser ~52% del tiempo del RSP. Para ver el
resto se añadió **`--rspbench`**: llena la IMEM entera (1024 ranuras, el PC envuelve solo) con
la mezcla de opcodes medida sobre el arranque de SM64 — 386 COP2, 123 SPECIAL, 103 LWC2, 83
SWC2, 213 ADDI, 38 LH, 37 ANDI, 24 LW, 17 SW de cada 1000 — y con las subfamilias pesadas
igual (VMADN 17%, VMADH 13%, ... / LDV 9 de cada 25 LWC2). `KESTREL_RSPMIX=<familia>` aísla
una: `cop2`, `vecld`, `alu` (ADDI rotando destino), `alu1` (ADDI encadenado sobre el mismo
registro), `scald` (LW/SW), `nop` (SLL r0,r0,0 = coste del bucle desnudo).

Anfitrión i7-870, 2.93 GHz **medidos** (`Win32_Processor`), 1 ciclo = 0.341 ns.
**El N64 pide 62.5 Mips.**

| mezcla | antes | después |
|--------|-------|---------|
| real   | 18.8 ns / 53.2 Mips | **15.45 ns / 64.7 Mips** |
| nop    | 8.35 ns | 7.16 ns |
| alu    | 8.10 ns | 6.04 ns |
| scald  | 10.1 ns | 6.89 ns |
| cop2   | 25.9 ns | 23.6 ns |
| vecld  | 25.1 ns | 22.5 ns |

### Lo que lo explica: este anfitrión despacha UNA carga por ciclo

Nehalem tiene un solo puerto de carga. En un intérprete eso convierte cada campo del objeto
leído dentro del cuerpo del bucle en un ciclo entero, y el bucle leía doce: `budget`, `halt`,
`imp`, `pc`, la palabra de IMEM, `profOn`, la tabla de saltos, `r[rt]`, `inDelay`, `branch`,
`pendingTarget`... El suelo medido con `nop` era 8.35 ns = **24 ciclos para no hacer nada**.

Dos cambios (commit `fa308ef`), ambos de semántica idéntica:

1. **`dmp`/`imp` como punteros del propio `Rsp`** (`bindMem()`) en vez de `mem->dmem.data()`
   en cada acceso: `std::vector::data()` se recargaba en cada uso porque cualquier llamada
   interna podía aliasar el vector. **53.2 → 56.9 Mips (+7%)**.
2. **Bucle por tandas de 8K instrucciones**: fuera del cuerpo el puntero a IMEM, el
   interruptor del muestreador y los tres contadores (`maxInsns`, `budget`, `ran`) reducidos
   a uno solo. **56.9 → 64.7 Mips (+13%)**. La tanda es la misma que ya usaba la publicación
   al regulador, así que el hilo CPU no ve el avance más tarde que antes.

### Lo que se probó y NO valió (medido, revertido)

- `execVuSse(u32 fn, __m128i t, ...)` pasando el vector por valor: **peor** (17.3 → 20.0 ns).
  Win64 pasa `__m128i` por valor **en memoria**, así que la tienda que se quería quitar vuelve.
- La misma función tomando `(vt, e)` y haciendo el broadcast dentro: peor (18.6-18.8 ns).
  Dentro del callee `vpr[vt]` puede aliasar la salida `R128& D` y la carga deja de poder
  adelantarse; el temporal del llamante demostrablemente no aliasaba.
- Reescribir `vadd48` con cadena de acarreo (`vcarry16` vía xor 0x8000 + cmpgt): correcto
  (`--rspfuzz` 0 fallos) pero ~2% más lento: cambia rendimiento por latencia en serie.
- `flatten` en `Rsp::exec` además de en `step`: 44.8 vs 48.2 Mips, **peor**. En `step` solo: +3.2%.
- Mantener `pc`/`budget`/el latch de retardo en locales: **peor** (real 17.8 → 18.3 ns). El
  registro extra hace derramar otras cosas dentro del switch gigante.
- Leer la instrucción siguiente por adelantado (con un contador de generación de IMEM para el
  microcódigo que se reescribe): neutral en la mezcla real. No compensa ni la carga extra ni
  el riesgo.

### Y sin embargo, en SM64 no se nota — y eso es el dato importante

`KFLIPS=250 sh scripts/perf.sh`, media de 5 pasadas: **24.78 → 24.79 swaps/s**, y el latido
sigue diciendo **`rsp 35.3 Mips busy`** igual que antes del +21% aislado. O sea: el tiempo
del worker del RSP **no** es ejecutar instrucciones. Lo que hay dentro de `step()` y no
ejecuta instrucciones es el DMA del SP (una instrucción `MTC0` que mueve kilobytes).

Barrido de elasticidad sobre SM64 (250 swaps, attract intercambia a 30/s = tiempo real):

| variante | swaps/s | % de tiempo real |
|----------|---------|------------------|
| base                        | 25.20 | 84% |
| `KESTREL_NORASTER=1`        | 28.95 | **96%** |
| `KESTREL_PACESLACK=1000000` | 23.82 | 79% |
| `KESTREL_JIT=0`             | 10.42 | 35% |
| `KESTREL_THREADS=0`         |  6.65 | 22% |

**Conclusión: SM64 corre al 84% de tiempo real y casi todo el hueco que queda es el
rasterizador por software.** El siguiente palo largo no es el intérprete ni el JIT: es
parallel-RDP.

---

## 2026-08-20 — parallel-RDP: de regresión a empate (RX 570)

Punto de partida medido (SM64, `KFLIPS=250`, `build-prdp`):

| variante | swaps/s |
|----------|---------|
| SoftRDP (`KESTREL_PRDP` sin poner) | 23.95 |
| parallel-RDP (`KESTREL_PRDP=1`)    | **19.54** |

El backend de GPU era **más lento** que el rasterizador por software. Dos causas reales,
las dos arregladas.

### 1. La RDRAM no estaba alineada a página → cero copia perdida

parallel-rdp intenta importar la RDRAM del invitado directamente en la GPU con
`VK_EXT_external_memory_host` (`create_imported_host_buffer`): la GPU lee y escribe la
memoria del emulador *sin copia*. La importación exige que el puntero esté alineado a
`minImportedHostPointerAlignment` = **4096 B** en escritorio. `Memory::rdram` era un
`std::vector<u8>` corriente, que solo garantiza 16 B:

```
[vrdp] gpu="Radeon RX 570 Series" ext_mem_host=1 align=4096 rdram=0000018F2767B040  MISALIGNED -> slow copy path
```

Al fallar, parallel-rdp cae **en silencio** (el LOGW va al hilo con la interfaz de log
silenciada) a un espejo de 16 MB en la GPU con máscara de escritura, que hay que sincronizar
alrededor de cada sync: 8 MB de PCIe por campo.

Arreglo: `AlignedAllocator<u8, 4096>` en `core/types.hpp` y `using GuestBytes` para la
RDRAM. Los ayudantes `rd32/rd64/wr8/wr16/wr32` de `rdp/rdp.cpp` pasan a plantilla sobre el
contenedor. Nada de esto toca semántica de la máquina: es alineación del host.

**19.54 → 21.69 swaps/s (+11%).**

### 2. El scanout esperaba a la GPU dos veces por campo

`produceScanout()` se hacía **dentro del SYNC_FULL** y esperaba el fence al momento:
`scanout_async_buffer()` → `fence->wait()` → `map` → `memcpy`. Es decir, justo después de
haber esperado ya la línea de tiempo del DP, se volvía a bloquear el hilo esperando a la GPU.
Cero solape.

Modelo correcto (y el que usa ares): **el scanout lo manda el VI, no el DP**. El VI escupe
lo que haya en el framebuffer en cada frontera de campo, pase lo que pase con el pipe del
RDP. Ahora `pumpViState()`, cuando ve la frontera de campo:

1. `harvestScanout()` — recoge el campo *anterior* (su fence ya está firmado hace rato),
2. `begin_frame_context()`,
3. `issueScanout()` — pide el siguiente y **no** espera.

La GPU trabaja un campo entero mientras el emulador sigue.

| tiempo del hilo RDP (250 campos, `KESTREL_PRDP_STATS=1`) | antes | después |
|---|---|---|
| recorrer/encolar la FIFO | 397 ms | 408 ms |
| esperar la línea de tiempo en SYNC_FULL | 615 ms | 549 ms |
| scanout (fence + map + memcpy) | **579 ms** | **0.55 ms** |

**21.69 → 23.17 swaps/s.** El scanout deja de existir como coste: el fence siempre llega ya
firmado.

### Resultado y techo

| variante | swaps/s | % de tiempo real (30/s) |
|----------|---------|------------------------|
| parallel-RDP inicial | 19.54 | 65% |
| + RDRAM alineada     | 21.69 | 72% |
| + scanout encauzado  | **23.17** | 77% |
| SoftRDP              | 24.61 | 82% |
| sin rasterizar (`KESTREL_NORASTER=1`) | 28.95 | 96% |

En esta máquina parallel-RDP **empata** con SoftRDP, no gana. Lo que queda es
`wait_for_timeline` en cada SYNC_FULL: 549 ms / 250 = **2.2 ms por campo** de latencia
GPU+driver, y esa espera está en el camino serie del invitado (el juego duerme hasta la
interrupción del DP). Es latencia de sumisión de una RX 570 en Windows, no trabajo nuestro.

Ruido pendiente, medido pero no arreglado: **641 fallos de reserva por campo-y-pico**
(`Exhausted LinkedDeviceHost memory` + `Will exceed memory budget` + `garbage_collect`).
Granite quiere un bloque de 64 MiB en el heap BAR host-visible (256 MiB en la RX 570, ~199
MiB ya ocupados por el escritorio) para un búfer de pocos cientos de bytes que
`bind_horizontal_info_view()` crea en cada pasada del VI; falla, recolecta basura y cae a
memoria de sistema **cada vez**, sin recordar el fallo. Arreglarlo exige parchear el árbol
de parallel-rdp, que hoy vive dentro del checkout **congelado** de ares
(`PRDP_DIR`); el paso limpio sería vendorizar parallel-rdp dentro de kestrel.

## `gate_all.sh` completo (2026-08-18/20)

Medido tras el fix de contrapresion del FIFO del RDP (`docs/RDP-FIFO-BACKPRESSURE.md`),
mismo host. Dos pasadas consecutivas dieron lo mismo dentro del ruido: **4 min 53 s**
de reloj de pared en total (15:56:03 -> 16:00:56).

| Tramo | interp | jit | jit-nolink | threaded | threaded-jit |
|-------|--------|-----|------------|----------|--------------|
| systemtest | 16-17 s | 9-11 s | 9 s | 17-18 s | 9-10 s |
| sm64 (60 campos VI) | 14 s | 7 s | 9-10 s | 11-12 s | 2-3 s |
| krom 371 ROMs (4 jobs, solo interp) | 164-186 s | — | — | — | — |

Los tramos JIT bajaron mucho respecto a la tabla de arriba porque aquella medicion
era anterior al block linking.

Re-medido 2026-08-20 con el reloj del VI unificado (`docs/VI-CLOCK.md`) y el gate de krom
parando en 2 SYNC_FULL en vez de 1: systemtest 16 s / 8-9 s / 9 s / 18 s / 9-10 s, sm64
14-15 s / 7 s / 10 s / 11-12 s / 2 s, krom 181-246 s. Igual dentro del ruido; los 246 s de
krom son el coste de dejar terminar la segunda display list.

El gate de sm64 para a los 60 **campos de video**, que es un estado del juego y no del
reloj de pared: por eso threaded-jit tarda 2 s y interp 14 s para la misma imagen. Esa
diferencia entre modos es normal; lo que seria fallo es que el md5 no coincidiera.

Si un tramo pasa de ~3x su fila, es cuelgue: bisecar con `git stash`, no subir el timeout.

### Pasada con el dynarec del RSP (2026-08-23)

`gate_all.sh` completo **5 min 53 s** (18:54:51 -> 19:00:44) con `KESTREL_RSPJIT` por
defecto ON. systemtest 24/16/17/18/10 s, sm64 14/6/9/11/2 s, krom 218 s (4 jobs).
La diferencia con los 4 min 53 s de la fila anterior esta toda en krom, que ya venia
oscilando 181-246 s entre pasadas; ningun tramo se salio de su rango.
