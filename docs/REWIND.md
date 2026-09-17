# Rebobinado (rewind)

Deshacer lo que acaba de pasar: la tecla de **retroceso** hace que el juego vaya hacia atras
mientras se mantenga apretada.

Viene **apagado**, y no por pereza: cuesta caro. Aqui estan el porque, el cuanto y como se
enciende.

## Encenderlo

| Variable | Por defecto | Que hace |
|---|---|---|
| `KESTREL_REWIND` | `0` | `1` enciende el rebobinado |
| `KESTREL_REWIND_FIELDS` | `2` | campos de video entre foto y foto |
| `KESTREL_REWIND_MB` | `256` | tope de memoria de la cinta |

En el lanzador es el grupo **Rebobinado**, con las tres casillas. Por telemetria/MCP,
`rewind.step {steps}` / `rewind_step(steps)`; `status` publica `rewind{steps,bytes,interval,budget}`
cuando esta activo, o sea que se puede ver cuanta cinta queda sin abrir la ventana.

## Lo que cuesta

Medido en SM64, 200 intercambios de buffer, SoftRDP, sin video ni audio, dos tandas cada uno:

| Rebobinado | Pared | Frente a apagado |
|---|---|---|
| apagado | 4487 / 4315 ms | — |
| cada 2 campos | 6221 / 6563 ms | **+45 %** |
| cada 6 campos | 5526 / 5420 ms | **+24 %** |

Apagado no cuesta *nada*: una comparacion contra cero al cerrar cada campo. Encendido, cada
foto paga tres cosas: **parar el RCP** (drenar la cola del RDP y terminar la tarea del RSP,
que es el mismo serializado que en su dia costo un 13-15 % en Perfect Dark), **recorrer el
estado entero** (~8 MB de RDRAM mas el resto) y **compararlo** con la foto anterior. Subir
`KESTREL_REWIND_FIELDS` abarata en proporcion directa, a cambio de rebobinar a saltos mas
gordos.

## Como esta hecho

El emulador ya sabia fotografiar la maquina entera (ver `docs/SAVESTATES.md`). Rebobinar es
tener esas fotos hechas de antemano. Lo caro es la RDRAM: una foto son ~8 MB, o sea que
guardarlas enteras a treinta por segundo se come un gigabyte cada cuatro segundos.

Por eso NO se guardan enteras. Se guarda **una sola foto viva** (la mas reciente) y, detras,
una pila de **diferencias hacia atras**: cada entrada dice lo que hay que reescribir sobre la
foto de ahora para que vuelva a ser la de antes. Es la direccion util -- rebobinar recorre la
pila del final al principio -- y ademas la barata, porque entre dos campos seguidos un juego
toca una porcion pequena de la RDRAM, no los 8 MB.

Formato de una diferencia (de la foto NUEVA a la VIEJA):

```
u64  tamano de la foto vieja
registros hasta cubrirla:  u32 iguales · u32 distintos · bytes de la vieja
```

Los "iguales" se copian de la foto nueva en la misma posicion y los "distintos" van
literales. La comparacion va por palabras de 4 bytes, que es como esta escrito el estado.

Medido en la pantalla de titulo de SM64 con foto cada 2 campos: **~79 KB por paso**, o sea
que los 256 MB de fabrica dan unos 3400 pasos = ~6800 campos = **casi dos minutos** de
partida. En juego con mucho movimiento la foto es mas gorda y la cinta dura menos; el tope se
cobra siempre por el extremo VIEJO, que es el que no se va a pedir.

## Donde encaja

- La foto se toma **con el RCP parado** (`System::quiesceRcp`), igual que un estado guardado:
  una foto con el RDP a medias de una lista no se puede volver a meter en la maquina.
- Y ademas **en un reposo natural del invitado** (`System::rcpAtRest`): RSP sin tarea, sin
  diario DPC ni barrera pendientes, RDP drenado y con su ultimo tramo ya visible. Al cerrar el
  campo la foto queda *debida* y la CPU sigue corriendo subtramos hasta llegar a ese reposo
  (tope 16x600 subtramos; si se agota se para a la fuerza y lo dice). Antes se forzaba el
  quiesce en el acto, y eso tenia dos fallos: en Threaded **se colgaba** (el RSP a mitad de
  tarea esperaba a la CPU en una cita y la CPU esperaba al RSP: 150 s y avisos `[rcp] llevo N x
  2000 ms esperando`), y en Lockstep acababa la tarea a destiempo, o sea que la foto ya no era
  un instante del invitado y el futuro con rebobinado salia distinto del futuro sin el. Los
  estados guardados por MCP (`state.save`/`state.load`) esperan al mismo reposo.
- El estado (version 11) guarda tambien los **fines de tarea de SP/DP armados y aun sin
  vencer** (`rcpPend` bits 0-1 + `spDoneAt`/`dpDoneAt`). En un reposo natural salen siempre a
  cero (medido: 0 de 1300 fotos en SM64, 0 de 1000 en Donkey Kong), pero la parada forzada si
  puede pillarlos armados, y perderlos es perder una interrupcion.
- **Cargar un estado guardado borra la cinta**: describe una partida que ya no existe.
- La cinta de **peliculas TAS** (`docs/TAS.md`) rebobina con el juego, porque la foto incluye
  la seccion `MOVI` con la posicion de la pelicula.
- La tecla es la de **retroceso** y NO va por flanco a proposito: se rebobina mientras se
  mantenga apretada. La ventana pide pasos con un tope de dos pendientes, para que soltar la
  tecla pare en seco en vez de dejar una cola rebobinando sola.

## Que se ha comprobado

- `test/rewind_test.cpp` (dentro de `gate_all.sh`): el codec de diferencias, con casos limite
  (fotos de distinto tamano, cambios no alineados a palabra, longitudes que no son multiplo
  de cuatro, vacias, todo igual, todo distinto) y mil pares al azar con cambios dispersos;
  ademas se comprueba que una diferencia truncada se RECHAZA en vez de reventar.
- Extremo a extremo en SM64 por telemetria, y **discriminante**: campo 200 da
  `ba62569b8d8ae560a61423e9061ba380` y campo 240 da `3cadb6c96d09a9a498501ee2f381cb9d` (o sea
  que la prueba distingue); rebobinar 20 pasos vuelve al campo 200 con el framebuffer
  `ba62...` exacto, y volver a correr 40 campos da otra vez `3cad...` **byte a byte**. Esto
  ultimo es lo que prueba que el estado restaurado es el mismo, y no uno parecido: si algo del
  estado quedara viejo, el futuro que sale de el seria distinto.

- **Ida y vuelta en cada foto** (`KESTREL_REWIND_RTT=1`, modo de gate `rewind-rtt` dentro de
  `gate_all.sh`): cada foto que se toma se vuelve a cargar en el acto sobre la maquina viva. Si
  el estado guardado se dejara algo, el futuro que sale de la carga seria distinto, y la puerta
  lo ve: systemtest y el md5 de SM64 tienen que salir iguales que sin rebobinado. Verificado
  ademas el statehash con y sin RTT en Threaded y Lockstep (SM64 300 intercambios `79895dc7`,
  Perfect Dark 600 `92f83ab8`, junkrunner64 `be723abf`, Donkey Kong `e7098ab4`). Esta prueba
  destapo un segfault: cargar un estado antes del primer kick del RSP dejaba `rsp.mem` nulo y
  `bindMem` reventaba; `afterLoad` lo enlaza ahora siempre.

## Lo que falta

- El coste esta dominado por copiar y comparar la RDRAM entera. Se puede bajar mucho con
  seguimiento de paginas sucias (saber que 4 KB toco el juego desde la ultima foto y mirar
  solo esos), que es trabajo aparte y con su propio riesgo: hay que cazar TODAS las vias de
  escritura (CPU, DMA de RSP/PI/SI, el propio RDP).
- No hay barra de rebobinado en pantalla; el estado se ve por `status`.
