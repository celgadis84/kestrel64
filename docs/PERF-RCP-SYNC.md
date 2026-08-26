# Coste de sincronizacion del RCP (2026-08-26)

Punto de partida: SM64, 400 campos, i7-870 (4C/8T, 2009), modo Threaded, SoftRDP.
`KESTREL_PACESLACK=524288` ya aplicado a mano en las medidas previas.

    18.6 fps  (holgura por defecto de entonces, 8 K)
    23.0 fps  (holgura 512 K)

El hilo del RSP declaraba `rsp 83% ocupacion` pero solo **32.6 Mips**. SM64 emite ~1.05 M
instrucciones de microcodigo por campo, o sea satura el RSP real: para ir a 60 fps hay que
sostener ~62.5 Mips. Faltaba casi un factor 2 y no se sabia en que.

## Lo que hizo falta para verlo: telemetria

Tres cosas que antes no estaban, todas en este ciclo:

1. **Tiempo de CPU real por worker** (`GetThreadTimes` sobre el hilo, `Memory::sampleWorkerCpu`).
   El heartbeat ahora imprime `rsp 83%(cpu 66%)`: ocupacion de pared frente a nucleo de
   verdad gastado. La diferencia es nucleo perdido (SMT/planificador), no coste de emular.
2. **hostprof deja de tirar todo lo de fuera de la imagen a un cubo "extern"**. Ahora agrupa
   por `AllocationBase` de la region y resuelve el nombre del modulo, asi que se distingue
   *ntdll* (esperar) de *ucrtbase* (memcpy) del **buffer RWX del dynarec** (codigo nuestro).
   Ademas, con el hilo suspendido barre el principio de su pila y atribuye la muestra al
   primer retorno que cae dentro de la imagen: dice QUIEN llamo a la DLL.
3. **Puerta de muestreo** (`hostprof::gate`, instalada con `rspBusy`). Sin ella, el sueno
   entre tareas del worker entraba en el histograma como "ntdll 50%" y tapaba todo.
4. **`jobs/s` en el heartbeat**: tareas de RSP y trabajos de RDP por segundo.

## Diagnostico

Con la puerta puesta, el hilo del RSP gastaba **45% ntdll + 16% ucrtbase**, y `jobs/s`
cantaba el porque:

    [hb] jobs/s: rsp=176 rdp=38274

**38.000 trabajos de RDP por segundo** — ~1.500 por campo. Cada escritura de DPC_END hacia
`rdpSubmit`: coger `rdpMx`, `push_back`, y `notify_all`. Con el worker dormido, ese
`notify_all` es una llamada al kernel. El hilo del RSP no iba lento emulando: iba lento
despertando al RDP.

El segundo trozo, `ucrtbase` con `Memory::spDma` como llamante: el motor DMA del SP mueve
tramos de 8 a 128 bytes constantemente (matrices, vertices, trozos de display list) y cada
uno salia a `memcpy` de la CRT, cuyo despacho por tamano cuesta mas que la copia.

## Arreglos (todos semantica de hardware, ninguno atado a un test)

1. **Coalescer el FIFO del RDP** (`Memory::rdpSubmit`). El FIFO del RDP es UNO: DPC_END no
   encola "un trabajo", solo adelanta el puntero final del mismo buffer. Si el ultimo tramo
   encolado aun no ha empezado — el worker saca de la cola ANTES de ejecutar, asi que todo lo
   que queda en ella esta sin empezar — y el nuevo continua exactamente donde acababa con el
   mismo modo de bus, es el mismo tramo partido en dos escrituras. Unirlos es lo que hace el
   hardware: se ejecutan los mismos comandos y DPC_CURRENT acaba en el mismo sitio.
2. **`rdpWaiting`** (bool bajo `rdpMx`): el productor solo notifica si el worker esta de
   verdad dormido en el condvar. Si no lo esta, volvera a coger el mutex al terminar el
   trabajo en curso y vera la cola llena. Se pone y se quita con el mutex cogido, que es el
   mismo con el que el productor lo lee: no hay ventana de wakeup perdido.
3. **`rspWaiters`** (contador atomico): igual para el `notify_all` que `Rsp::step` hacia cada
   8 K instrucciones al publicar progreso al regulador. Patron de Dekker — el notificador
   publica `cyclesRun` antes de leer el contador, el esperador incrementa el contador antes
   de comprobar el predicado — y el `wait_for` de 500 us del regulador sigue de red.
4. **`dmaCopy`**: copia inline de 16 bytes por vuelta para tramos < 256 B en `spDma`; por
   encima se sigue delegando en `memcpy`.
5. **Holgura del regulador por defecto 8 K -> 512 K** (`kPaceSlack`).

## Resultado

    trabajos de RDP   38.274/s  ->  4.629/s
    rsp Mips busy       32.6    ->  40.0
    RSP % de N64         51%    ->   69%
    cpuWait              59%    ->   38%
    fps (400 campos)    23.04   ->  28.85     (+25%)

Barrido de la holgura con todo lo demas igual: 8 K 22.0 | 128 K 26.1 | **512 K 29.0** | 2 M 26.7.

## Lo que queda

`rsp 40 Mips busy` sigue por debajo de los ~62.5 que pide SM64, y el hilo aun tiene
`cpu 59%` frente a `76%` de ocupacion de pared: hay nucleo perdido. Siguientes palancas, por
impacto medido:

- **Enlazado de bloques en el dynarec del RSP.** Hoy 8,5 instrucciones por despacho: cada
  bloque paga prologo, epilogo y una llamada indirecta mal predicha.
- **Asignacion de registros en el dynarec del RSP.** Hoy cada operacion es
  `mov eax,[rbx+off]` / op / `mov [rbx+off],eax`.
- **Nucleo perdido**: 4 nucleos fisicos para hilo de CPU + RSP + RDP + presentacion.
