# Llegar a gameplay real en Perfect Dark (para medir, no para jugar)

Medir FPS en el menu no dice nada: PD gasta ahi una fraccion del RDP. La cifra que vale es
la de dentro de un nivel. Hay dos caminos y conviene tener los dos, porque se validan entre
si.

## Camino 1 -- navegar el menu con el mando inyectado (funciona con cualquier ROM)

El emulador tiene que estar EN MARCHA (`--play` o `--run`). El mando 1 se conduce desde
fuera con `scripts/pad.py`, que habla con el servidor de telemetria del propio emulador:

```sh
PY=/c/Users/celga/AppData/Local/Programs/Python/Python311/python
$PY scripts/pad.py --status                  # fps y ocupacion ahora mismo
$PY scripts/pad.py --shot /tmp/pd.png        # ver donde esta
$PY scripts/pad.py start:8 wait:120 a:6 wait:60 shot:/tmp/pd.png
```

La duracion de la pulsacion se cuenta en **lecturas del joybus**, no en milisegundos: son
los mismos fotogramas de juego tanto si el emulador va al 30 % como al 200 %, y al agotarse
vuelve el mando del anfitrion, o sea que el juego ve un flanco de suelta de verdad (que es
lo que esperan los menus).

Secuencia tipica desde el arranque en frio: logos (se saltan con `start`) -> pantalla de
nombre de agente (viene con "Dark" escrito; hay que llevar el cursor a `OK` con la cruceta
y `a`) -> menu de mision -> elegir mision y dificultad -> `a` para empezar.

**Ojo**: hasta que el servidor de telemetria admitio varios clientes, el puente MCP
acaparaba el socket y este script se quedaba colgado en el `accept`. Si vuelve a pasar algo
asi, el sintoma es un `telemetry lost (timed out)` con el emulador vivo y respondiendo por
el otro lado.

## Camino 2 -- escribir el nivel en RAM (salta el menu entero)

Perfect Dark decide el nivel con dos variables globales. Escribiendo las dos desde el titulo,
el juego carga el nivel en la siguiente transicion, sin tocar el menu:

| simbolo | KSEG0 | fisica | que es |
|---|---|---|---|
| `g_MissionConfig` | `0x8007dbd8` | `0x07dbd8` | configuracion de mision (dificultad + nivel) |
| `g_MainChangeToStageNum` | `0x80043c04` | `0x043c04` | "cambia a este nivel ya" |
| `g_StageNum` | `0x80043d60` | `0x043d60` | nivel actual; en el titulo vale `0x5A` |

Niveles usados para medir: Defection `0x30`, Villa `0x2c`. Escribir `g_MissionConfig` con el
nivel (dificultad Perfect Agent) y luego `g_MainChangeToStageNum` con el mismo numero; se
confirma que ha entrado porque `g_StageNum` pasa de `0x5A` al numero del nivel.

Las direcciones salen del mapa de enlace del arbol de decompilacion
(`perfect_dark/build/ntsc-final/pd.map`) y por tanto **son de ESE build**. Antes de fiarse en
otra ROM hay que comprobar la barata: leer `g_StageNum` en el titulo y ver que vale `0x5A`.
Si no vale, esa ROM tiene otro mapa y toca el Camino 1.

**Comprobado 2026-09-04 con la ROM que hay aqui** (`Perfect Dark (Europe) (En,Fr,De,Es,It).n64`):
NO vale. Leyendo `0x043d60` con la maquina en el titulo sale `0x27`, no `0x5A`, o sea que el
mapa de `ntsc-final` no describe esta ROM PAL y escribir esas dos direcciones no lleva a
ningun nivel (escribe encima de otra cosa). Para medir gameplay en PAL: Camino 1, o sacar el
mapa del build PAL. Anotado para no repetir el intento.

Este es el metodo con el que se perfilo el nivel Villa en su dia (ver
`perfect_dark/docs/perf/villa-combat-profile.md`); aqui queda anotado porque el arbol de
Perfect Dark no se toca.

## Que medir una vez dentro

`scripts/pad.py --status` (o `emu_status` por MCP) da lo que importa:

- `speed.cpuPct` -- velocidad del R4300i respecto a la consola real. 100 % = tiempo real.
- `occupancy.fps` -- fotogramas que el juego esta emitiendo de verdad.
- `occupancy.rdpBusyPct` / `rspBusyPct` -- cuanto trabajan los workers; si el RDP se acerca
  al 100 % el limite es el rasterizador, no la CPU.
- `occupancy.cpuWaitPct` -- cuanto espera la CPU a los workers.

Y con `KESTREL_HEARTBEAT=1` el emulador escupe lo mismo cada 5 s por consola, mas el hambre
de audio, que es el primer sintoma de que no se llega a tiempo real.
