# kestrel64 — depurador (capas de depuración, ventana y MCP)

Reimplementación propia (no copia) de las capas de depuración que la comunidad homebrew
usa en ares: grabadora del cuadro RDP/RSP, línea de tiempo, TMEM, TLB, framebuffer, eventos
RCP, puntos de observación, USB SC64, carga de código en caliente. Todo vive en el servidor
de telemetría (`src/telemetry/server_dbg.cpp`, grabadora `src/debug/dbg.{hpp,cpp}`) y la
ventana es un cliente más del mismo servidor, en el mismo proceso.

## Órdenes MCP nuevas (2026-10-23)

| Orden | Qué |
|-------|-----|
| `dbg.enable` / `dbg.freeze` / `dbg.status` | enciende la grabadora; congela el último cuadro completo |
| `dbg.rdp.frame` | lista de comandos RDP del cuadro (job = índice de kick del DPC), con filtro |
| `dbg.rsp.tasks` | tareas OSTask del cuadro (ucode, dirección, ciclos) |
| `dbg.timeline` | CPU/RSP/RDP/VI/AI en el tiempo de invitado |
| `dbg.fb` / `dbg.tmem` | framebuffer bruto e imagen de TMEM (con tiles) |
| `cpu.tlb` | las 32 entradas del TLB decodificadas |
| `ev.log` | anillo de eventos RCP (interrupciones, DMA, kicks) |
| `mem.watch` | puntos de observación de lectura/escritura sobre RDRAM física |
| `code.load` / `rsp.load` | carga en caliente de un binario en RDRAM / IMEM-DMEM |
| `rom.load` | cambia la ROM y relanza el sistema sin salir del proceso |
| `usb.log` | salida del USB SC64 (`osSyncPrintf`/debug USB del homebrew), ver `KESTREL_USB` |
| `rsp.bp.add/del/list` | puntos de ruptura del RSP sobre PC de IMEM (`del` sin `addr` borra todos) |

### Puntos de ruptura del RSP

- Mapa de 1024 ranuras (una por palabra de IMEM). Con alguno puesto, el RSP va por el
  intérprete (el JIT se apaga mientras haya puntos) y la CPU también, para pararse en la
  instrucción exacta en que se ve el acierto (`rspBpArmed`).
- Acierto: el RSP se queda ANTES de ejecutar ese PC, `bpHit`/`hitPc` se publican y la CPU se
  pausa (`status.paused`). Threaded: el hilo del RSP espera dentro de `step()`; Lockstep: sale
  del tramo y no vuelve a entrar hasta `resume`.
- `run_control resume` suelta el acierto y ese mismo PC se ejecuta una vez sin volver a parar
  (`bpSkip`); la siguiente pasada vuelve a parar.
- Con el RSP parado en un punto, `rsp.regs` y `rsp.bp.list` responden sin candado. `add`/`del`
  toman el CoreMutex: si la CPU está en una barrera esperando al RSP, hacer `resume` antes.
- Prueba: `scripts/rspbp_smoke.py --threads 1|0 --port 9172` (SM64, punto en 0x000).

Wrappers en `tools/mcp/kestrel_mcp.py`. Prueba de humo: `scripts/dbg_smoke.py --port 9171+`,
`scripts/romswap_smoke.py`.

## Ventana

`src/ui/debugger_win32.cpp` — Win32 + D3D11 + Dear ImGui (v1.91.9b-docking, MIT,
`third_party/imgui`, THIRD-PARTY.txt item 8) en hilo propio. Cada panel pide sus datos con
`Server::call` igual que el MCP: lo que se ve en la ventana se puede pedir por socket.

- Abrir: `--debugger`, `KESTREL_DEBUGGER=1` o menú **Depurar > Depurador...** de la ventana.
  Cerrarla no cierra el emulador.
- Paneles (menú **Ventanas**): Control, CPU, Desensamblado (puntos de ruptura, ejecutar hasta
  la selección), Memoria (hex editable, mem.watch), RDP: comandos del cuadro, RSP: tareas,
  Línea de tiempo, Framebuffer (zoom, píxel bajo el ratón), TMEM (decodificada con el
  intercambio de palabras de filas impares, RGBA32 partido, TLUT en 0x800), TLB, Perfilador,
  Eventos RCP, USB (SC64), Registros RCP / RSP.
- Disposición inicial con DockBuilder; la que deje el usuario se guarda en
  `kestrel64_debugger.ini` junto al exe.
- Sin GPU cae a WARP.
- `KESTREL_DEBUGGER_SHOT=ruta.bmp`: vuelca el backbuffer a los 180 cuadros de interfaz (para
  pruebas sin mirar la pantalla). Prueba: `scripts/debugger_smoke.py --port 9171`.

## Candado del núcleo (CoreMutex)

`std::mutex` en Windows (SRWLock) no es justo: el hilo de la CPU lo soltaba y lo volvía a coger
entre subtramos antes de que despertara quien esperaba, y una orden de telemetría llegaba a
esperar ~1,9 s con la emulación libre. `System::CoreMutex` apunta a quien espera (`waiting`) y
`runLoop` cede mientras haya alguien. No toca la semántica del invitado.

## Pendiente (fase 2)

Recarga de funciones en caliente al estilo deployer SC64 (parchear una función sin reiniciar,
con invalidación del JIT por rango): ver `docs/GAPS.md`, Interfaz / herramientas.
