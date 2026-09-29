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

## P2 — Coste de cargas de textura mas fiel `[BLOQUEADO: falta medida en consola del coste de LOAD_BLOCK/LOAD_TILE/LOAD_TLUT]`

**Hoy**: `accountTmem` = 1 GCLK por 8 bytes, sin latencia de RDRAM ni coste fijo por
comando. Las opts R1/R2/R3 quitan CARGAS enteras; si cada carga pequena cuesta casi
nada, el win sale infravalorado.

**Hacer**: coste fijo por `LOAD_BLOCK`/`LOAD_TILE`/`LOAD_TLUT` (setup + latencia de fila
RDRAM, reutilizar `LAT`/`ROW` del modelo de pixel) + transferencia. Buscar dato HW
(n64brew, Thar0 si tiene, MiSTer `RDP_*` VHDL como oraculo de ciclos). Si no hay oraculo,
dejarlo documentado como estimacion y NO calibrar a ojo.

**Busqueda 2026-09-30 (kestrel)**: no hay oraculo publico.
- n64brew "Reality Display Processor/Commands": ciclos solo para SYNC_* (25/50/33); de las
  cargas solo "Load Block is the fastest way to move data from RDRAM into TMEM".
- n64brew "Reality Display Processor/Pipeline": las cargas comparten recursos con el
  pipeline de render, sin cifras.
- Manual de programacion N64 cap. 13: LoadBlock "more memory-bandwidth efficient" que
  LoadTile, sin cifras.
- Thar0 (`../rdp-timing-tests`) solo mide rectangulos de relleno.
- MiSTer: sus ciclos son los de su SDRAM/DDR3, no los de la RDRAM de la consola; no vale
  como oraculo de latencia.

Se queda en 1 GCLK por 8 bytes (documentado como estimacion en `accountTmem`). **Falta**:
una ROM al estilo Thar0 que borre contadores, lance LOAD_BLOCK / LOAD_TILE / LOAD_TLUT de
tamanos y formatos variados + SYNC_FULL, y lea `DPC_TMEM_BUSY` / `DPC_PIPE_BUSY` en consola
real (TMEM_BUSY no cuenta las paradas por RDRAM: la diferencia con PIPE_BUSY da la latencia).
Con esos datos se ajusta el fijo + transferencia. kestrel puede escribir la ROM; la medida
tiene que salir de hardware.

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

## P4 — Metrica de "cuanto de GPU-bound" sin el profiler de host `[HECHO 2026-09-30 74443f8]`

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

**Hecho (kestrel)**: `rdp_stats("reset")` ... `rdp_stats("read")` trae `guest` = reparto en
TIEMPO DE INVITADO de la misma ventana: `rdpBusyPct` (GCLK / 62,5 MHz), `rspRunPct` (ciclos
que el RSP ejecuto / 62,5 MHz), `rspPollPct` (ciclos del RSP absorbidos en bucles de sondeo
reconocidos: el RSP esperando, no trabajando), `cpuIdlePct` (instrucciones de CPU dentro
del bucle ocioso del kernel, `beq $0,$0,-1` cobrado en bloque), `ms`, `cpuOps`,
`msPerFlip`. Son cotas inferiores del modelo: el RSP cuenta 1 ciclo por instruccion sin
paradas del vectorial ni latencia de DMA. `pdbench` lo pone en `out["guest"]` y en la
columna `invitado % RDP/RSP (sondeo)/CPU-ocio`; `--prof N` ademas guarda en `out["prof"]` el
top N de CPU (con simbolo, P5) y de RSP (hueco de IMEM + palabra) de la ventana.

Uso:

```sh
python scripts/pdbench.py $R/v2-03-collsq.z64 --map $R/v2-03.map --mode threaded --prof 80
```

Cruce: `rdpBusyPct` = `rdp_frame_pct` de pdbench (54 = 54, 44 = 44).

**Resultado Villa (spawn, threaded = lockstep)**:

| rom | fps | RDP % | RSP % (sondeo) | CPU ocio % | instr RSP / fotograma |
|---|---|---|---|---|---|
| ref-stock | 44.06 | 44 | 99 (15) | 58 | 1,204 M |
| v2-03-collsq | 52.96 | 54 | 100 (0) | 67 | 1,178 M |
| v2-04-aaoff | 53.03 | 48 | 100 (0) | 67 | - |

- **v2 esta limitado por el RSP**, no por el RDP ni la CPU: 1,178 M instrucciones de RSP por
  fotograma = 18,84 ms a 62,5 MHz, y el fotograma dura 18,88 ms. Sin sondeo: trabaja todo
  el rato. Por eso AA-off (-10,6 % GCLK) no mueve fps.
- stock hace el mismo trabajo de RSP por fotograma (1,204 M) pero la CPU tarda mas: el
  RSP sobra 15 % sondeando. stock -> perf gano fps hasta chocar con el RSP.
- Perfil RSP (`--prof 1024`): plano por todo el IMEM, el hueco mas caliente es el bucle de
  despacho de comandos del ucode grafico (~5,9 k comandos por fotograma) con 0,5 %. No hay
  bucle de espera escondido: es trabajo real del microcodigo (grafico + audio).
- Siguiente palanca de fps en PD = trabajo del RSP (lista de visualizacion mas corta /
  ucode). Ojo: en hardware el RSP sera MAS lento que este modelo (paradas del vectorial,
  latencia de DMA), asi que el techo por RSP ya es real aqui.

## P5 — Simbolos: cargar `pd.map` del build `[HECHO 2026-09-30 927d71d]`

**Por que**: `prof.cpu` da PCs fisicos en cubos de 16 B; traducir a funciones a mano es
lento. Las ROM de las ramas de PD son `ntsc-final` y traen su `pd.map`.

**Hacer**: `--symbols <pd.map>` (formato map de GNU ld) → `prof.cpu` y `cpu.disasm`
devuelven `funcion+off`. Opcional: tool `sym_lookup name|addr`.

**Cerrado 2026-09-30.** Uso:

```sh
kestrel64.exe <rom> --port 9130 --symbols ../perfect_dark/roms_v2/v2-03.map
```

- `profile_cpu`: cada cubo trae `sym` (`frametimeCalculate+0x44`) y `va` = la direccion por
  la que se EJECUTA: se buscan primero las paginas que el TLB mapea sobre ese fisico (la ROM
  matching corre lib/juego desde 0x70000000/0x7F000000) y luego KSEG0. `cpu_disasm`: `sym` por
  instruccion. Sin simbolo: vacio (p.ej. vector de excepcion 0x180).
- Solo se da nombre de CODIGO si la direccion cae en un tramo `.text` del map; una funcion
  static sin simbolo sale como `objeto.o+off` (el `idleproc` de v2-03 = `boot.o`).
- `sym_load(path)` (orden `sym.load`) carga en caliente; `sym_lookup(addr=..)` -> `sym`,
  `code` (true si es .text), `base`; `sym_lookup(name=..)` -> `addr`.
- `pdbench.py` ya lanza con `--symbols <map>`.
- Probado: v2-03 (KSEG0, 6865 simbolos) `osGetCount`, `frametimeCalculate+0x44`, `mainProc+0x2ec`;
  ref-stock (TLB, 9985) `idleproc` @0x700016c0, `__osException+0x30`, `_n_loadBuffer`;
  `g_Vars+0x10` como dato. pdbench v2-03 da la fila de la tabla exacta.
- Gates: `release.sh --quick` + `gate_quick.sh` ALL OK.

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

## P7 — FILL cycle y contadores separados (menor para PD, anotado en RDP-TIMING.md) `[BLOQUEADO: falta oraculo HW del ciclo FILL; DPC_CLOCK hecho COMMIT]`

`CLOCK = BUFBUSY = PIPEBUSY` identicos y FILL ~2x lento. PD borra con FILL cada frame:
inflar el FILL sesga el reparto de P3. Separar contadores y dar a FILL su camino de
64 bits/clock cuando haya oraculo HW.

**Hecho (kestrel 2026-09-30)**: `DPC_CLOCK` corre libre al reloj del RCP (62,5 MHz) desde
el arranque y no para nunca, ni con FREEZE (n64brew, "Reality Display Processor/Interface").
Antes solo sumaba trabajo del RDP, igual que BUF/PIPE. Ahora es el reloj de invitado del
lector desde el ultimo borrado (bit 9 de DPC_STATUS). Leer `DPC_CLOCK` alrededor de un
fotograma da ya su duracion en ciclos del RCP.

**Queda**:
- `CMD_BUSY` / `PIPE_BUSY`: siguen iguales. Thar0 (consola real, un fillrect + SYNC_FULL)
  da Buf y Pipe iguales dentro del ruido (diferencia -3..40 GCLK sobre ~80 k), asi que en
  el caso medido ya cuadran. Solo se separan con huecos: PIPE_BUSY sigue contando mientras
  el FIFO esta vacio esperando al RSP hasta el SYNC_FULL. Modelarlo pide el tiempo de
  invitado entre comandos; sin oraculo de cuanto.
- FILL a 64 bits/clock: Thar0 nunca entra en FILL. Bloqueado hasta medirlo en consola (la
  misma ROM de P2 puede llevar casos FILL).

## Orden sugerido

P1 → P3 → P6 → P5 → P4 → P2 → P7. Con P1+P3+P6 ya se puede dar el primer numero
fiable de las ramas `mods/rdp-*` de PD.
