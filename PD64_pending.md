# PD64_pending — lo que la optimizacion de Perfect Dark necesita de kestrel64

Buzon de peticiones del proyecto **optimizacion de Perfect Dark** (`../perfect_dark`,
contexto en `../perfect_dark/docs/perf/README.md`) hacia el emulador. Cada punto dice
POR QUE hace falta, QUE hay que implementar y COMO se sabe que esta hecho.

Reglas de siempre: semantica HW genuina (nada a medida de PD), fuentes n64brew /
n64.dev / libdragon / MiSTer, `gate_all` + `gate_prdp` + statehash tras cada cambio,
`release.sh` al cerrar. Al terminar un punto: tacharlo aqui, fecha, y una linea en
`docs/STATUS.md`.

## Protocolo entre sesiones (PD-opt <-> kestrel64)

- **Este archivo es la fuente de verdad.** La sesion PD-opt SOLO escribe aqui (puntos
  nuevos, aclaraciones); NO toca `src/` de kestrel. La sesion kestrel64 es duena del
  codigo y del estado de cada punto.
- Estado en la cabecera de cada punto: `[ABIERTO]` · `[EN CURSO]` ·
  `[HECHO aaaa-mm-dd <commit>]` · `[RECHAZADO: motivo]` · `[BLOQUEADO: que falta]`.
  Al cerrar, kestrel anade debajo **como se usa** (comando / tool MCP / variable de
  entorno) y que gates pasaron.
- Discrepar es valido: si un punto pide algo no-HW o mal planteado, marcar
  `[RECHAZADO]` con la fuente. Nada a medida de PD.
- Aviso en vivo (opcional, si las dos sesiones estan abiertas): `SendMessage` con una
  linea `PD64_pending: P<n> <estado>`. Si no hay sesion viva, basta el archivo; cada
  sesion lo relee al arrancar (`ListAgents` para ver quien esta).
- Puntos nuevos se numeran P8, P9... al final; no renumerar los existentes.

## Contexto (por que el emulador es el cuello de la optimizacion)

- PD es **GPU-bound**: en nivel real la CPU pasa ~81 % esperando al RDP
  (`__osEnqueueAndYield`). Cada GCLK que el RDP desperdicia cuesta FPS.
- Las opts de PD que hay hechas (ramas `mods/rdp-opts`, `mods/rdp-sync-opt`,
  `mods/rdp-aaoff`) son **de RDP**: dedup de cargas TMEM (R1/R2/R3), dedup de
  `PipeSync`, AA-off del mundo. En ares **no se podian medir** (ares no modela
  tiempo del RDP) y el fork de ares con timing esta congelado.
- kestrel ya tiene modelo de coste de pixel calibrado contra HW (Thar0,
  rmse 0,133 cyc/px) y contadores DPC que avanzan. Falta lo de abajo para que un
  A/B de ROM de PD de un numero fiable.

## P1 — Coste fijo de los SYNC del RDP (bloqueante para medir `mods/rdp-sync-opt`) `[HECHO 2026-09-29 511e766]`

**Hoy**: `src/rdp/rdp.cpp:1640` — `SYNC_LOAD/PIPE/TILE → no-op`. Un sync redundante
cuesta 0 GCLK, asi que quitar syncs no mueve ningun contador.

**HW** (n64brew, RDP): coste fijo por comando, en GCLK: `SYNC_PIPE` 50,
`SYNC_TILE` 33, `SYNC_LOAD` 25. Un sync redundante paga el coste entero (no espera a
ninguna senal interna que ya este libre). `SYNC_FULL`: drena + interrupcion; revisar
fuente antes de ponerle numero.

**Hacer**: cobrar esos GCLK en `dpc_clock`/`dpc_pipebusy` y en `rdpGclk` (el regulador),
solo si `charge`. Contar por tipo (ver P3).

**Hecho cuando**: gates verdes; Thar0 rmse no empeora (sus listas llevan syncs: si
empeora, el modelo de pixel estaba absorbiendo el coste del sync y hay que re-ajustar,
anotarlo); `pd.z64` (base) vs `mods/rdp-sync-opt` dan delta de GCLK/frame > 0 en el
mismo tramo.

**Como se usa** (kestrel, 2026-09-29): automatico, sin variable. `SoftRdp::accountStall`
(`src/rdp/rdp.cpp`) cobra 25/50/33 GCLK por cada SYNC_LOAD/PIPE/TILE, redundante o no, en
`DPC_CLOCK`, `DPC_PIPEBUSY`, `DPC_BUFBUSY` (el comando sigue en el FIFO mientras para) y en
`rdpGclk` (el regulador: el RDP tarda mas en guest, la CPU espera mas). Se ve en
`rcp_registers` (dp.clock/pipebusy/bufbusy) y en `emu_status` (`rdpBusyPct`). Solo cuando
`charge` (el coste se paga una vez: paseo solo-coste o el que pinta). SYNC_FULL sin numero
fijo (espera de verdad; lo modela el drenado ya existente). Vale igual con SoftRDP y con
parallel-rdp (el coste sale del paseo de SoftRdp en los dos).
**Gates**: `gate_quick.sh thar0` ALL OK (systemtest 0/3721 0/2 0/6, sm64 `d35bd8aa`,
prdp-jit `b5521b24`, Thar0 rmse 0.1332 = sin cambio). De paso se arreglo una regresion del
paseo solo-coste que habia llevado Thar0 a 0.988 (ver docs/RDP-TIMING.md).
**Pendiente del "hecho cuando"**: delta GCLK/frame `pd.z64` vs `mods/rdp-sync-opt` se mide
con P3 (`rdp.stats`) + P6 (banco en nivel); lo cierra el primer numero de P6.

## P2 — Coste de cargas de textura mas fiel `[ABIERTO]`

**Hoy**: `accountTmem` = 1 GCLK por 8 bytes, sin latencia de RDRAM ni coste fijo por
comando. Las opts R1/R2/R3 quitan CARGAS enteras; si cada carga pequena cuesta casi
nada, el win sale infravalorado.

**Hacer**: coste fijo por `LOAD_BLOCK`/`LOAD_TILE`/`LOAD_TLUT` (setup + latencia de fila
RDRAM, reutilizar `LAT`/`ROW` del modelo de pixel) + transferencia. Buscar dato HW
(n64brew, Thar0 si tiene, MiSTer `RDP_*` VHDL como oraculo de ciclos). Si no hay oraculo,
dejarlo documentado como estimacion y NO calibrar a ojo.

## P3 — `rdp.stats`: histograma por frame (comando de telemetria + tool MCP) `[HECHO 2026-09-30 500edc9]`

Lo que la optimizacion necesita ver sin volcar la lista entera:

- Por intercambio de framebuffer (o por ventana `rdp.stats.reset` → `rdp.stats`):
  cuenta de cada opcode RDP; GCLK repartidos en `pixel` / `tmem` / `sync` / `fill`;
  pixeles por modo de ciclo (1CYC/2CYC/COPY/FILL) y con/sin `IM_RD`, `Z_CMP`, `Z_UPD`.
- **Deteccion de redundancia**, contada aparte:
  - sync redundante: `SYNC_PIPE` sin ningun comando de estado entre medias que lo
    necesite desde el anterior sync / primitiva; `SYNC_LOAD`/`SYNC_TILE` idem.
  - carga TMEM redundante: `LOAD_*` cuyo origen (direccion+tamano+tile) y contenido
    de TMEM resultante es identico al que ya habia (hash del rango TMEM antes/despues).
  - `SET_OTHER_MODE`/`SET_COMBINE` repetidos identicos.
- Tool MCP `rdp_stats` en `tools/mcp/kestrel_mcp.py`.

**Hecho cuando**: en PD en nivel, la cuenta de `PipeSync` por frame y las cargas
redundantes cuadran con lo medido a mano en `../perfect_dark/docs/perf/rdp-sync-dedup.md`
(PipeSync/load 1,06 → 0,34 con sync-opt) y con el ~7 % de redundancia real de texturas.

**Como se usa** (kestrel, 2026-09-30): tool MCP `rdp_stats(action)`:
`"reset"` pone a cero y ENCIENDE, `"read"` devuelve la ventana, `"off"` apaga (apagado
cuesta un test por comando). Telemetria cruda: `rdp.stats.reset` / `rdp.stats` /
`rdp.stats.off`. Devuelve `ops` (cuenta por opcode con nombre), `gclk`
{pixel, fill, tmem, sync, total}, `pixels` {1cyc, 2cyc, copy, fill, written, imRd, zCmp,
zUpd}, `redundant` {syncLoad, syncPipe, syncTile, loads, loadBytes, loadRedundant,
loadRedundantBytes, otherModesSame, combineSame} y `perFlip` (lo mismo / intercambios de
framebuffer de la ventana; `flips`/`fields` = tamano de la ventana). Vale con SoftRDP y con
parallel-rdp (sale del paseo de coste). Definiciones: SYNC_* redundante = ninguna primitiva
(triangulo, TEXRECT, FILL_RECT) desde el anterior del mismo tipo o desde SYNC_FULL (la de
rdp-sync-dedup.md); carga redundante = TMEM y TLUT identicas antes y despues de
LOAD_BLOCK/LOAD_TILE/LOAD_TLUT; SET_OTHER_MODES/SET_COMBINE repetido = mismo valor que el
vigente. Cada comando cuenta una vez (solo el paseo que cobra).
Referencia SM64 titulo (build-prdp-static): 186 SyncPipe/flip de los que ~80 redundantes,
127 cargas/flip de las que ~29 redundantes, ~424 k GCLK/flip.
**Gates**: `gate_quick.sh thar0` ALL OK (systemtest 0/3721 0/2 0/6, sm64 `d35bd8aa`,
prdp-jit `b5521b24`, Thar0 0.1332).
**Pendiente del "hecho cuando"**: cuadrar contra rdp-sync-dedup.md en PD en nivel = P6.

## P4 — Metrica de "cuanto de GPU-bound" sin el profiler de host `[ABIERTO]`

**Nota PD-opt 2026-09-29**: `emu_status` ya trae `rdpBusyPct`/`rspBusyPct`/`cpuWaitPct`
(docs/MCP-GUIA.md). Puede bastar; PD-opt lo validara en nivel real y si cuadra con el
81 % se cierra P4 sin codigo. Lo que quedaria: poder resetear la ventana de medida.

**Nota kestrel 2026-09-30**: `rdpBusyPct`/`rspBusyPct` son PARED del hilo anfitrion, no
tiempo de invitado -> no sirven para "cuanto GPU-bound". Lo de invitado ya sale de
`pdbench.py` (`rdp_ms_per_frame` / `frame_ms`). `rspBusyPct` > 100 % arreglado (P6) y
recuerda: incluye el giro en citas con la CPU y la espera al RDP.

**Hacer (si no basta)**: en `emu_status` (o `rdp.stats`) publicar por ventana: fraccion de tiempo
guest en que el RDP esta ocupado (`rdpGclk` / GCLK de pared guest), y fraccion de
instrucciones CPU dentro del bucle ocioso del kernel (el detector de ocio ya existe,
ver GAPS.md). Con eso "81 % esperando al RDP" sale de un comando, no de un perfil.

## P5 — Simbolos: cargar `pd.map` del build `[ABIERTO]`

**Por que**: `prof.cpu` da PCs fisicos en cubos de 16 B; traducir a funciones a mano es
lento. Las ROM de las ramas de PD son `ntsc-final` y traen su `pd.map`.

**Hacer**: `--symbols <pd.map>` (formato map de GNU ld) → `prof.cpu` y `cpu.disasm`
devuelven `funcion+off`. Opcional: tool `sym_lookup name|addr`.

## P6 — Banco A/B reproducible en nivel real (ROM ntsc-final) `[HECHO 2026-09-30 f64c648]`

**Por que**: el retail de `test_roms/` es PAL y su mapa no casa (PD-GAMEPLAY.md). Las
ROMs de la optimizacion son `ntsc-final` (`../perfect_dark/build/ntsc-final/*.z64`,
`../perfect_dark/pd.ntsc-final.z64`).

**Hacer**:
1. Verificar que `pd.ntsc-final.z64` y `build/ntsc-final/pd.z64` arrancan en kestrel
   y que el warp por RAM funciona (`g_StageNum` = `0x5A` en titulo; escribir
   `g_MissionConfig` + `g_MainChangeToStageNum` con Villa `0x2c` / Defection `0x30`).
   Las direcciones salen del `pd.map` de CADA build — con P5 se resuelven por nombre.
2. `scripts/pdbench.sh <rom> <stage> <frames>`: arranque → warp → esperar carga →
   N frames en **lockstep** (determinista) sin mando → volcar `rdp.stats` + GCLK/frame
   + fps de pared threaded. Mismo guion para todas las ROMs de rama.
3. Tabla de salida en `docs/baselines/pd-opt.md` con fecha y md5 de ROM.

Ojo: las ramas cambian codigo → cambian direcciones → un savestate de una ROM no vale
para otra. Por eso warp por simbolo, no savestate.

**Cerrado 2026-09-30.** Uso:

```sh
R=../perfect_dark/roms_v2
sh scripts/pdbench.sh --mode threaded $R/v2-03-collsq.z64=$R/v2-03.map ...   # fila por ROM
python scripts/pdbench.py <rom> --map <map> [--stage villa|defection|0x..]     [--mode lockstep|threaded] [--skip 720] [--measure 2400] [--intro skip|keep] [--png out/x]
```

- Arranca PAUSADO y avanza por campos (`frame_advance`): titulo + 240 campos, warp, START
  salta la intro (`--intro keep` la mide), 720 ticks, ventana de 2400 ticks. Todo en
  bordes de campo de invitado: **dos lockstep = misma fila exacta; threaded (SPEEDMODE=hw,
  SPLEAD=0) = lockstep**. Sale JSON (`per_frame`, `rdp_ms_per_frame`, `frame_ms`,
  `rdp_frame_pct`, `rdp_stats` entero) + fila markdown.
- **Pasar SIEMPRE `--map`** (en el .sh, `rom=map`): sin el cae al `pd.map` de
  `build/ntsc-final` y en otra rama lee basura (avisa por stderr).
- Columnas nuevas pedidas por PD-opt: `sync_pipe (a/b)` con b = ni primitiva NI carga de
  TMEM desde el anterior (`rdp.stats` `redundant.syncPipePure`/`syncLoadPure`/
  `syncTilePure`), `px IM_RD`/f y ms de RDP de INVITADO frente al fotograma.
- Tabla con fecha y md5: `docs/baselines/pd-opt.md` (7 ROMs de `roms_v2`, Villa spawn).
- Tambien arreglado de paso: `rspBusyPct` > 100 % (se cobraba la tarea entera a la ventana
  donde acababa; `Memory::rspBusyNow()` suma la parte en vuelo) y `release.sh`/`pack.sh`/
  `pgo.sh` ya no hacen `taskkill //IM` (mataban corridas de otra sesion): `scripts/killown.sh`
  solo mata exes con ruta dentro del repo.
- Gates: `gate_quick.sh thar0` ALL OK (rmse 0.1332), tras `release.sh --quick`.
- Hallazgo para P4: el modelo de coste del RDP no cobra setup por primitiva ni por span
  (sin oraculo HW de triangulos). En Villa da RDP ~54 % del fotograma y AA-off no mueve
  fps. Anotado en `docs/GAPS.md` ("Coste del RDP: sin setup...").

## P7 — FILL cycle y contadores separados (menor para PD, anotado en RDP-TIMING.md) `[ABIERTO]`

`CLOCK = BUFBUSY = PIPEBUSY` identicos y FILL ~2x lento. PD borra con FILL cada frame:
inflar el FILL sesga el reparto de P3. Separar contadores y dar a FILL su camino de
64 bits/clock cuando haya oraculo HW.

## Orden sugerido

P1 → P3 → P6 → P5 → P4 → P2 → P7. Con P1+P3+P6 ya se puede dar el primer numero
fiable de las ramas `mods/rdp-*` de PD.
