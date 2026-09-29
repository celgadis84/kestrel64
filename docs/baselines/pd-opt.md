# Perfect Dark: banco A/B de las ramas de optimizacion (PD64_pending P6)

Guion: `scripts/pdbench.py` (una ROM) / `scripts/pdbench.sh` (tanda, una fila por ROM).
Metodo, en corto:

- Arranca la ROM **pausada** y la hace avanzar por campos de video con `frame_advance`:
  titulo (`g_StageNum == 0x5A`) + 240 campos, warp por RAM (`g_MissionConfig` +
  `g_MainChangeToStageNum`, direcciones del `.map` de CADA build), START inyectado para
  saltar la cutscena de intro (pulsacion contada en lecturas del joybus), 720 ticks de
  240 Hz del juego, y mide 2400 ticks mas en tandas de 120 campos.
- Todo punto de decision cae en un borde de campo de INVITADO: **dos corridas lockstep del
  mismo binario dan la misma fila byte a byte**, y threaded (con `KESTREL_SPEEDMODE=hw`,
  `KESTREL_SPLEAD=0`) da la misma que lockstep. La primera version del guion escribia el
  warp tras `sleep(4)` de pared y dos corridas lockstep salian 44,06 / 43,98 fps.
- `fps` = contadores del propio juego, `240 * d(lvframenum) / d(lvframe240)`: es tiempo de
  INVITADO, no depende del anfitrion. Una ROM con mas coste de CPU emulada saca menos.
- `gclk` = ciclos del RDP por fotograma de juego (modelo de coste, ver `docs/RDP-TIMING.md`);
  `ms RDP invitado` = gclk / 62,5 MHz, y `%` su parte del fotograma. **`rdpBusyPct` del
  latido/`emu_status` es otra cosa**: pared del hilo anfitrion, no sirve para esto.
- `sync_pipe (a/b)`: total; a = sin primitiva desde el SYNC_PIPE anterior; b = ni primitiva
  ni carga de TMEM desde el anterior (el que se puede quitar sin discutir; el SYNC_PIPE tras
  un LOAD_BLOCK de `gDPLoadTextureBlock` cuenta en a y no en b). Igual para `sync_load`.
- `loads (x)`: cargas de TMEM; x = las que dejaron TMEM+TLUT byte a byte iguales.
- `wall_s` = pared de la ventana medida, sin limitador (`KESTREL_THROTTLE=0`); ruidosa si
  otra sesion carga el anfitrion.

Uso:

```sh
R=../perfect_dark/roms_v2
sh scripts/pdbench.sh --mode threaded $R/v2-03-collsq.z64=$R/v2-03.map $R/v2-04-aaoff.z64=$R/v2-04.map
python scripts/pdbench.py <rom> --map <map> --stage villa|defection|0x.. --mode lockstep \
    [--intro keep] [--png out/villa] [--prof 80]   # --png: fotogramas inicio/fin; --prof: top CPU/RSP
```

**Pasar siempre el map**: sin el, `pdbench.py` cae al `pd.map` de `build/ntsc-final` y en
una ROM de otra rama lee direcciones basura y se queda esperando un titulo que no ve
(avisa por stderr).

## 2026-09-30 — Villa (0x2c), spawn tras saltar intro, kestrel baf3c31 + P6

Escena: el jugador quieto en el punto de aparicion (fotogramas de inicio y fin iguales).
ROMs de PD-opt en `../perfect_dark/roms_v2/`.

| rom | md5 | fps | gclk/f (ms RDP = % frame) | px IM_RD/f | gclk_sync | gclk_tmem | tris | sync_pipe (a/b) | sync_load (a/b) | loads (iguales) |
|---|---|---|---|---|---|---|---|---|---|---|
| ref-stock | 2ca14261 | 44.06 | 626405 | - | 20546 | 19887 | 955 | 293.6 (184.6/62.7) | 231.9 (133.9/0.0) | 230.9 (11.1) |
| ref-perf | 0a9c9713 | 53.08 | 637654 | - | 20434 | 19888 | 967 | 291.8 (183.3/62.3) | 231.1 (133.6/0.0) | 230.1 (11.2) |
| v2-00-base | 1fc8433d | 52.98 | 631981 | - | 20451 | 19922 | 972 | 292.1 (183.2/62.2) | 231.1 (133.2/0.0) | 230.1 (11.2) |
| v2-01-cpu-exact | 7e0d1f04 | 53.06 | 636708 | - | 20347 | 19731 | 970 | 290.3 (181.6/60.9) | 230.7 (133.0/0.0) | 229.7 (11.0) |
| v2-02-memwords | 077a425f | 53.00 | 637231 | - | 20409 | 19828 | 971 | 291.3 (182.5/61.6) | 231.1 (133.4/0.0) | 230.1 (11.2) |
| v2-03-collsq | 283ae27a | 52.96 | 637072 (10.19 ms = 54 %) | 115387 | 20368 | 19771 | 973 | 290.6 (182.0/61.2) | 230.8 (133.1/0.0) | 229.8 (11.2) |
| v2-04-aaoff | 43cf8702 | 53.03 | 569602 (9.11 ms = 48 %) | 52441 | 20376 | 19791 | 970 | 290.8 (182.2/61.5) | 230.7 (133.1/0.0) | 229.7 (11.0) |

Lectura:

- stock (gcc `MATCHING=0`) -> perf: +20 % fps, todo CPU (el RDP no cambia).
- v2-00..v2-03: planos entre si (+-0,2 %, dentro de lo que mueve la escena de un fotograma
  a otro: todas son deterministas, la diferencia es de la ROM).
- AA fuera de las salas (v2-04): -10,6 % GCLK, IM_RD 115 k -> 52 k px/f, **fps igual**. En
  el modelo de kestrel el RDP ocupa ~54 % del fotograma en esta escena: no es el palo
  largo, asi que quitarle trabajo no sube fps. OJO: el modelo cobra pixel (con IM_RD/Z,
  calibrado con Thar0 a 0,13 ciclos/px de rmse), TMEM y SYNC, pero **no cobra setup por
  primitiva ni por span** (no hay oraculo de hardware de triangulos; Thar0 solo mide fill
  rects). Con ~970 triangulos por fotograma el RDP de kestrel probablemente se queda corto.
  Ver `docs/GAPS.md`.
- sync_load b = 0: cada SYNC_LOAD va pegado a una carga. sync_pipe b ~61/f = los duplicados
  que se pueden quitar sin discutir (~3 k GCLK/f = 0,5 % del RDP).

## 2026-09-30 — reparto de tiempo de invitado (PD64_pending P4)

Misma escena. `invitado %` = `rdp_stats` `guest` de la ventana (columna nueva de
`pdbench.sh`): RDP ocupado / RSP ejecutando (sondeando) / CPU en el bucle ocioso.

| rom | fps | RDP % | RSP % (sondeo) | CPU ocio % | instr RSP / fotograma |
|---|---|---|---|---|---|
| ref-stock | 44.06 | 44 | 99 (15) | 58 | 1,204 M |
| v2-03-collsq | 52.96 | 54 | 100 (0) | 67 | 1,178 M |
| v2-04-aaoff | 53.03 | 48 | 100 (0) | 67 | - |

Lectura: v2 esta **limitado por el RSP** (1,178 M instr/f = 18,84 ms a 62,5 MHz contra un
fotograma de 18,88 ms, sin sondeo). stock hace el mismo trabajo de RSP pero la CPU va
detras (RSP 15 % sondeando). Perfil del RSP plano (`--prof`): trabajo real del ucode, no
esperas. Detalle en `PD64_pending.md` P4.

## 2026-09-29 — primer resultado (guion viejo, NO comparable)

`build/ntsc-final/pd.z64` 283ae27a, Villa, warp tras `sleep(4)` de pared y ventana que
podia empezar durante la carga / cutscena: lockstep 44,06 y 43,98 fps en dos corridas,
threaded 43,88. Retirado por no reproducible.
