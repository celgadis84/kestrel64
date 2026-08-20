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
