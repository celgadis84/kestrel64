# Guia MCP de kestrel64 (para otra sesion que depura/optimiza un juego)

Pensada para una sesion de Claude que trabaja en OTRO proyecto (p. ej. optimizar la decomp
de Perfect Dark en `E:\Claude\N64\perfect_dark`) y usa kestrel64 como banco de pruebas:
arrancar la ROM que acaba de compilar, llevarla a un punto concreto, mirar memoria/CPU/RCP
y perfilar. Todo por el servidor de telemetria del propio emulador.

**Reglas para esa sesion**
- **Solo se USA kestrel64, no se edita.** Si falta una capacidad, anotarla y pedirla; el
  arbol `E:\Claude\N64\kestrel64` es de otra sesion.
- **El MCP de ares esta MUERTO** (`mcp__ares64__*`, fork congelado). Usar solo
  `mcp__kestrel64__*`.
- Respetar las reglas del proyecto del juego (en PD: `perfect_dark/docs/perf/`).

## 1. Que ejecutable

| exe | para que |
|---|---|
| `E:\Claude\N64\kestrel64\build-prdp-static\kestrel64.exe` | **el normal**: RDP por GPU (parallel-rdp), JIT de CPU y de RSP, hilos |
| `E:\Claude\N64\kestrel64\build-static\kestrel64.exe` | igual con SoftRDP (rasterizador de referencia por software, sin Vulkan) |
| `E:\Claude\N64\kestrel64\dist\kestrel64.exe` | la copia del paquete publicado (= build-prdp-static) |

No compilar kestrel64 desde la otra sesion: `sh scripts/release.sh` es de su dueno.

## 2. Arrancar

El servidor de telemetria arranca SIEMPRE, en el puerto 9128 (`--port N` para cambiarlo).

```sh
EXE=/e/Claude/N64/kestrel64/build-prdp-static/kestrel64.exe
"$EXE" "<rom>"            # PAUSADO con ventana: espera run_control('resume'). Ventana negra = normal.
"$EXE" "<rom>" --play     # corriendo y con ventana (lo normal para mirar)
"$EXE" "<rom>" --run      # corriendo SIN ventana (lotes, medidas)
```

Desde Bash de Claude Code lanzarlo en segundo plano (`run_in_background`) o con
PowerShell `Start-Process`; nunca en primer plano, porque no termina solo.

**Topes obligatorios en pruebas automaticas** (si no, corre para siempre):
- `timeout <s>` delante. Un juego arrancado da campos de VI en 1-5 s; si en ~5 s no
  avanzan `viFields` en `emu_status`, es CUELGUE, no lentitud. No subir el timeout.
- `KESTREL_MAXFLIPS=<n>` (para tras n intercambios de buffer) o `KESTREL_MAXINSN=<n>`.

Cerrar: `taskkill //F //IM kestrel64.exe`. Un proceso vivo bloquea el puerto: matarlo antes
de lanzar otro.

## 3. Conectar el MCP

Ya esta registrado en `E:\Claude\N64\.mcp.json`:

```json
{ "mcpServers": { "kestrel64": {
    "command": "python",
    "args": ["E:/Claude/N64/kestrel64/tools/mcp/kestrel_mcp.py"],
    "env": { "KESTREL_TELEMETRY_HOST": "127.0.0.1", "KESTREL_TELEMETRY_PORT": "9128" } } } }
```

Una sesion abierta en otra carpeta (p. ej. `perfect_dark/`) necesita ese mismo bloque en su
`.mcp.json`. El puente necesita el paquete `mcp` de Python. Prueba sin MCP, con el emulador
vivo: `python E:/Claude/N64/kestrel64/tools/mcp/kestrel_mcp.py --selftest`.

El puente reconecta solo. Si el emulador se reinicia, la siguiente llamada vuelve a
conectar. El servidor acepta varios clientes a la vez (MCP + `scripts/pad.py`).

## 4. Herramientas (`mcp__kestrel64__*`)

**Estado y control**
- `emu_status`: velocidad (`speed.cpuPct`, 100 = tiempo real), `occupancy.fps`,
  `rdpBusyPct`/`rspBusyPct`/`cpuWaitPct`, `viFields`, `viFlips`, pausado/parado.
  Es la primera llamada para saber si la cosa avanza.
- `run_control(action)`: `'pause' | 'resume' | 'reset'`. No existe `'run'`.
- `frame_advance(fields)`: avanza N campos de video y vuelve a pausar.
- `cpu_step(count)`: paso a paso de instrucciones.
- `state_save(slot)` / `state_load(slot)`: savestate completo, slots 0..9 (los mismos
  ficheros que F5/F7 en la ventana). **La forma buena de volver una y otra vez al mismo
  punto de un nivel entre experimentos.** El savestate va ligado a la ROM: si se recompila
  el juego y cambia el codigo, el estado viejo no sirve, hay que rehacerlo.
- `rewind_step(steps)`: solo si se arranco con `KESTREL_REWIND=1`.

**Memoria**
- `memory_regions`: lista de regiones (`rdram`, `dmem`, `imem`...).
- `read_memory(region, addr, length, coherent)`: `addr` fisica o KSEG0. **`coherent=True`**
  lee a traves de la D-cache de la CPU. Sin eso, las estructuras que la CPU escribio y aun
  no ha volcado (hilos de libultra, colas de mensajes, globales recien escritas) salen
  VIEJAS.
- `write_memory(region, addr, hexbytes)`: para escribir globales del juego (ver 6).
  Ojo: la palabra del mando se sobrescribe; para el mando usar `controller_set`.

**CPU / RSP / RCP**
- `cpu_registers`: gpr, hi/lo, COP0 y desensamblado en pc.
- `cpu_disasm(addr, count)`.
- `rsp_registers(vpr)`: escalares, y los vectoriales si `vpr=True`.
- `rcp_registers`: MI/SP/DPC/VI/AI/PI/SI. Sirve para ver si el RSP esta parado, si el RDP
  tiene FIFO pendiente, y el estado del VI.
- `breakpoint_add/del/list`, `run_until(addr, timeout_ms)`: se para en una PC. Con
  breakpoints armados la CPU pasa por el interprete y va mas lenta; quitarlos al acabar.

**Mando**
- `controller_set(buttons, stick_x, stick_y, polls)`: `buttons` = `"start"`, `"a,b"`,
  `"z+cup"` o una palabra de 16 bits. La pulsacion dura `polls` LECTURAS DEL JOYBUS, no
  milisegundos: son los mismos fotogramas de juego a cualquier velocidad, y al acabarse el
  juego ve la suelta.
- `controller_state`: lo que el juego leera en el proximo sondeo.
- Alternativa por linea de ordenes: `python E:/Claude/N64/kestrel64/scripts/pad.py
  start:8 wait:120 a:6 shot:/tmp/x.png` (usar el python de Windows,
  `/c/Users/celga/AppData/Local/Programs/Python/Python311/python`).

**Imagen**
- `capture_framebuffer(path)`: PNG de lo que esta mostrando el VI, mas un histograma de
  color. Para comprobar que se ve lo esperado. Leer el PNG con la herramienta Read.

**Perfilado**
- `profile_start` → dejar correr la carga → `profile_cpu(top)` / `profile_rsp(top)` →
  `profile_stop`. `profile_reset` abre una ventana nueva sin parar.
- `profile_cpu`: cubos de 16 B por direccion FISICA, con el desensamblado de la primera
  instruccion. Hay que cruzarlo con `cpu_disasm` y con el `.map` del juego para dar
  nombre a la funcion.
  **LIMITE: solo muestrea lo que ejecuta el INTERPRETE.** Con el JIT puesto (de fabrica)
  sale sesgado o casi vacio. Para un perfil de CPU de verdad, arrancar con `KESTREL_JIT=0`
  (mas lento, pero el reparto es fiel), o bien arrancar con `KESTREL_GPCPROF=20`, que
  muestrea bajo el JIT por tiempo y vuelca el top al cerrar por stderr.
- `profile_rsp`: ranura de IMEM del microcodigo; el propio perfilado pasa el RSP al
  interprete, asi que sale fiel. Sirve para ver donde gasta el ucode grafico/audio.

## 5. Variables de entorno utiles para medir un juego

Se ponen ANTES de lanzar el exe:
- `KESTREL_HEARTBEAT=1`: cada 5 s, por consola, Mips, % de velocidad N64, ocupacion de los
  workers y hambre de audio. Es la vista rapida de "llega o no a tiempo real".
- `KESTREL_GPCPROF=<topN>`: perfil por PC de invitado bajo el JIT (ver arriba). Con el se
  vio que el 65 % del tiempo de invitado de PD se va en el hilo ocioso de libultra, o sea
  esperando al RCP.
- `KESTREL_RSPTASKS=1`: reparto de tareas y ciclos del RSP por tipo (grafico/audio).
- `KESTREL_RSPTRACE=1`: KICK y FIN de cada tarea del RSP.
- `KESTREL_DPSYNCLOG=1`: camino del RDP (SYNC_FULL, arranques, escrituras DPC).
- `KESTREL_SPEEDMODE=hw`: ritmo fiel al hardware en vez de "lo mas rapido posible".
  **Usarlo para medir el efecto de una optimizacion DEL JUEGO**: en velocidad libre el
  emulador se adapta y oculta parte de la diferencia.
- `KESTREL_RDRAM=4`: consola sin Expansion Pak (de fabrica 8 MB).
- `KESTREL_SAVETYPE=...`: forzar el tipo de guardado si la ROM modificada cambio de ID.

Lista completa: `E:\Claude\N64\kestrel64\CLAUDE.md`, seccion "Env-var toggles".

## 6. Perfect Dark en concreto

`E:\Claude\N64\kestrel64\docs\PD-GAMEPLAY.md` tiene los dos caminos para llegar a jugar un
nivel sin tocar el menu a mano:
1. Navegar el menu con el mando inyectado (`controller_set` o `scripts/pad.py`).
2. Escribir el nivel en RAM: `g_MissionConfig`, y despues `g_MainChangeToStageNum`. Las
   direcciones salen del `.map` del BUILD (`perfect_dark/build/<version>/pd.map`). Con una
   ROM compilada desde la decomp el mapa es el propio y vale. Comprobacion barata: en el
   titulo `g_StageNum` tiene que valer `0x5A`. La ROM PAL comercial NO coincide con el mapa
   de `ntsc-final`.

Flujo recomendado para A/B de una optimizacion del juego:
1. Lanzar la ROM base con `--play`, llegar al nivel y hacer `state_save(1)`. El savestate
   solo vale para ESA ROM; con la ROM optimizada hay que llegar otra vez al nivel.
2. Medir con `emu_status`/heartbeat en `KESTREL_SPEEDMODE=hw` varias veces, y quedarse con
   el minimo o la mediana.
3. Repetir con la ROM optimizada en el mismo nivel y el mismo punto.
4. Perfil de CPU del juego: `KESTREL_JIT=0` + `profile_*`, o `KESTREL_GPCPROF`.
5. Ver que la imagen esta bien: `capture_framebuffer` y comparar las dos ROMs.

## 7. Trampas conocidas

- Arrancar sin `--play`/`--run` deja el emulador PAUSADO y la ventana NEGRA. No es un
  cuelgue: falta `run_control('resume')`.
- `read_memory` sin `coherent=True` sobre estado del kernel = datos viejos.
- No declarar "colgado" por un log cortado; mirar `viFields` en `emu_status` dos veces.
- En modo hilos (de fabrica) el numero de instrucciones que gasta la CPU girando depende
  del anfitrion. Para comparar, usar campos de video o intercambios de buffer, no
  instrucciones.
- Leer texto volcado con `tr -d '\0'`: los volcados van rellenos de NUL.
