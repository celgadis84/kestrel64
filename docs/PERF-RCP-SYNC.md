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

---

## Segunda tanda (2026-08-26, tras el commit de8f768)

Tras los cinco arreglos de sincronizacion el hilo del RSP seguia dando 40 Mips con 83% de
ocupacion. El perfilador de host decia "ntdll 31%, ucrtbase 20%" pero atribuia el 50% de esas
muestras a un solo llamante, `+0x6d5a0`, que resulto **no ser codigo**: `.text` acaba en
`0x6bae0`. El barrido de pila aceptaba como direccion de retorno cualquier palabra que cayera
dentro de la imagen, y `.rdata`/`.data` estan llenos de punteros que lo parecen.

### 1. Perfilador: rango real de `.text` + validacion de sitio-de-llamada

`hostprof.cpp` lee ahora las cabeceras PE del propio modulo (`initTextRange`) y exige, ademas,
que la palabra candidata tenga una instruccion `call` justo antes (`isCallSite`: `E8 rel32`, o
`FF /2` con reg=2 a 2..7 bytes de distancia). Eso descarta punteros a funcion guardados en la
pila y marcos muertos. El histograma paso de un pico falso del 50% a dos picos reales del 26%
y el 24%.

Para simbolizar hace falta DWARF, que el build normal no lleva. `build-prof/` es el mismo
`-O3 -flto=thin` con `-g -gdwarf-4`; se consulta con
`llvm-dwarfdump --lookup=$((0x140000000 + OFFSET)) build-prof/kestrel64.exe`.
`llvm-symbolizer` y `nm` NO valen aqui: con thin-LTO solo sobreviven 725 simbolos de texto y
dan el simbolo anterior mas cercano, que es otra funcion (`memset +6848`).

### 2. `getenv` fuera del camino caliente

`mmioWrite32` llamaba a `std::getenv("KESTREL_DPSYNCLOG")` en **cada escritura de DPC_END**, y
habia tres mas iguales (`KESTREL_RSPTRACE` por escritura de SP, `KESTREL_RDPTRACE` por trabajo
de RDP, `KESTREL_FPDBG`). `getenv` de la CRT recorre el bloque de entorno entero con un candado
dentro: era todo el `ucrtbase 20%` del hilo del RSP. Cacheados en `static const bool` — el
entorno no cambia despues de arrancar, asi que es exacto, no una aproximacion.

29.85 fps (desde 28.85), RSP 46.1 Mips busy.

### 3. Sin drenado al instalar un buffer nuevo de FIFO

Con `ucrtbase` fuera, el perfil quedo en **ntdll 44.7%, todo desde un solo sitio**:
`rdpDrain()` en el `case DPC_END` de `mmioWrite32`. Al instalar un START nuevo, el hilo que
escribe el registro — que es el del RSP, porque quien emite el kick es el microcodigo —
esperaba a que la cola del RDP se vaciara **entera**. RSP y RDP se turnaban: el RSP producia el
frame con el RDP parado, luego el RDP lo rasterizaba con el RSP parado. Dos hilos al 70% que
nunca coincidian.

El drenado no hace falta. La recarga de CURRENT ya viaja dentro del trabajo: `rdpRunJob`
publica `DPC_CURRENT` = inicio del span al empezar cada uno, asi que encolar el span nuevo
detras de los viejos los ejecuta en el mismo orden que el command processor del hardware y
retira exactamente los mismos SYNC_FULL — ni uno de mas, que era el motivo original del
drenado. Y el flow-control del FIFO circular de F3DEX2 sigue honesto, porque CURRENT solo
avanza cuando el rasterizador avanza de verdad: si el productor se adelanta demasiado, el
microcodigo se frena solo leyendo CURRENT, igual que en la consola.

`KESTREL_RDPDRAIN=1` recupera el comportamiento anterior para bisecar.

### Resultado

| | antes de la 2a tanda | despues |
|---|---|---|
| rsp Mips busy | 40.0 | **94.1** |
| RSP % de HW | 69% | **121%** |
| cpuWait | 38% | **4%** |
| ocupacion RDP | 72% | **79%** |
| **fps SM64** | **28.85** | **32.11** |

El cuello de botella se ha movido: el RSP ya emula por encima de la velocidad de la consola y
el RDP es ahora el hilo mas ocupado. La siguiente palanca esta en el rasterizador (SoftRDP en
esta medida) o en el reparto de nucleos, no en el RSP.

---

## Que significan estos "fps" (leer antes de comparar)

`viFlips` cuenta **cambios de VI_ORIGIN**, es decir frames de juego presentados, no campos VI.
SM64 corre a **30 fps de juego** en la consola (dos campos VI por frame). Asi que la escala es:

| medida | frames/s | % de consola |
|---|---|---|
| antes de esta sesion, SoftRDP | 23.04 | 77% |
| tras la 1a tanda, SoftRDP | 28.85 | 96% |
| tras la 2a tanda, SoftRDP | 32.11 | 107% |
| tras la 2a tanda, parallel-rdp | 36.74 | **122%** |

El contador `N64 speed: CPU` del heartbeat coincide (122%), porque compara instrucciones
retiradas contra el ritmo de retirada que equivale a tiempo real, no contra el reloj de ciclos.
SM64 con parallel-rdp corre **por encima de la consola** en este host (i7-870 de 2009).

El JIT de CPU no cambia el resultado (35.1 con `KESTREL_JIT=1`, 36.5 con `+JIT_LINK`, 36.7 con
interprete): en esta carga el hilo de CPU no es el palo largo. El que sigue por debajo es el
RSP, al 74-77% del reloj del hardware, y ese es el que marcara el techo en un juego con mas
carga de microcodigo.

## Tercera tanda (2026-08-26): getenv en el hilo de CPU

El perfilador de host sobre el hilo de CPU (build `build-prof-prdp`, parallel-rdp) daba
`ucrtbase` al 7.17% del hilo, alcanzado desde dos sitios de `src/cpu/cpu.cpp`:

| Sitio | % hilo CPU | Frecuencia |
|---|---|---|
| `cpu.cpp:1681` (`KESTREL_STATTRACE`) | 6.77% | cada `MTC0` a `C0_Status` |
| `cpu.cpp:1706` (`KESTREL_SEENFIND`) | 1.20% | cada `ERET` |

Misma raiz que en `memory.cpp`: `std::getenv` de la CRT recorre el bloque de entorno
entero con un candado dentro. En un kernel de libultra que entra y sale de excepciones
constantemente, `MTC0 Status` + `ERET` son de lo mas caliente que hay. Cacheados en
`static const bool` (mas los dos de `KESTREL_TRAPRI` y `KESTREL_FAULTSTOP`, del mismo
camino de excepcion). Exacto: el entorno no cambia despues de arrancar.

Medido con SM64, `KESTREL_MAXFLIPS=400`, parallel-rdp:

```
antes:  36.74 frames/s  (122% de consola)
despues 41.07 / 41.95   (137-140% de consola)
[hb] N64 speed: CPU 147.2%  RSP 89.1% | occupancy: rdp 17%(cpu 28%) rsp 51% cpuWait 8%
```

### Recorrido completo de la sesion (SM64, consola = 30 fps)

| Estado | frames/s | % consola |
|---|---|---|
| Punto de partida | 23.04 | 77% |
| Sync RCP (commit `de8f768`) | 28.85 | 96% |
| Sin drenado RDP en START + getenv de `memory.cpp` (`4d6569d`) | 32.11 (SoftRDP) | 107% |
| parallel-rdp | 36.74 | 122% |
| getenv de `cpu.cpp` | 41.95 | 140% |

### Palancas que quedan, medidas (no adivinadas)

- **Regulador de ritmo**: ~15% del hilo de CPU en `condition_variable.h:142` llamado desde
  `memory.cpp`. Es el mayor bloque unico que queda en ese hilo.
- **Trafico de submit al RDP**: ~40 000 trabajos/s bajo parallel-rdp. El worker de GPU vacia
  al instante, asi que no coalesce nada. `KESTREL_RDPINLINE=1` solo vale +2.4%.
- **RSP al 87-89% del reloj de hardware**: sera el techo en un juego con microcodigo pesado.
  Toca enlazado de bloques y asignacion de registros en el dynarec del RSP.

## Cuarta tanda (2026-08-26): el hilo de CPU deja de pagar peajes

El perfil de host (sampler de alta resolucion + `scripts/hostprof_sym.py`) dejaba claro que solo
~20-25% del hilo de CPU era codigo invitado emitido por el JIT. El resto eran peajes del propio
JIT. Cuatro cambios, todos semantica HW intacta:

| Cambio | Que hacia antes | Que hace ahora |
|---|---|---|
| Spill perezoso (`jit.cpp`) | Cada CALL a un helper hacia `rc.writeback()` completo | Solo se vuelca lo que el helper vaya a leer; cada sitio de salida lleva su `RcSnap` y el stub escribe lo sucio |
| Forget dirigido (`jit.cpp`) | `forgetAll()` tras el interprete tiraba el cache entero | Se olvida solo el gpr que la op puede escribir |
| Dispatch COP1 (`cpu.cpp`) | `jitInterpOp` entraba por `execute()` (decode completo) | Las ops COP1 van directas a `cop1op()` |
| Helpers de memoria especializados (`cpu.cpp` + `jit.cpp`) | `jitMem(op)` decodificaba la op en un switch de 11 casos y leia `gpr[rs]`/`gpr[rt]` por el puntero | `jitMemOp<OPc>` con tamano/signo/store en `constexpr`; el bloque pasa direccion y dato en registros, ya residentes en el cache |

Los `kRcRegs` (`RSI/RDI/R13/R14/R15`) son todos callee-saved en la ABI Win64, asi que sobreviven
al CALL intactos: volcarlos y olvidarlos era trabajo tirado.

El helper especializado ademas quita el `writeback` de `rs`/`rt` antes del CALL: como ya no lee
`cpu->gpr`, la unica coherencia que hace falta es en el camino de fallo, y de eso se encarga el
`RcSnap` del stub de bail.

**Medida** (SM64, `KESTREL_PRDP=1 KESTREL_MAXFLIPS=400 KESTREL_THREADS=1`, consola = 30 fps):

| Punto | fps | % consola |
|---|---|---|
| Tercera tanda (getenv cacheado) | 41.95 | 140% |
| + spill perezoso + forget dirigido | ~42.0 | 140% |
| + dispatch COP1 directo | 42.0-42.5 | 141% |
| + helpers de memoria especializados | **46.2-46.9** | **155%** |

CPU N64 en el heartbeat: 153.4% -> **163.9%**. RSP 97.9%, ocupacion RDP 19%, RSP 58%.

Puertas: `gate_all` regress=0, seis modos MATCH `466282775dbd0ac084946558a1c30771`;
`gate_prdp` regress=0, ambos modos MATCH `b5521b24d8fc280fbf102df22d7d30cb`.

### Palancas que quedan, por tamano medido en el hilo de CPU

1. **COP1 nativo en el JIT** (~15-19%) - hoy toda la FPU pasa por el interprete.
2. **Micro-TLB de datos** (~5.2% en `translate`) - la traduccion se repite por acceso.
3. **Camino rapido de D-cache inline** (~6.4% entre `dcRead`/`dcWrite`/`dcFill`).
4. **Block linking** (~7.2% en `jitTryBlock`) - cada salto vuelve al driver a buscar bloque.

### LWC1/LDC1/SWC1/SDC1 al JIT (2026-08-26)

Los cuatro accesos de memoria COP1 seguian yendo por `jitInterpOp` -> `cop1op`. Ahora los
cubre la misma plantilla `jitMemOp<OPc>`: el "rt" indexa `fpr` en vez de `gpr`, el dato del
store lo saca el helper de `fprGet32/64` (el bloque no lo pasa) y con CU1=0 se bailea para
que el interprete levante la Coprocessor Unusable exacta. 46.6 -> ~47.0 fps.

### Camino rapido de kseg0/kseg1 en `translate`: MEDIDO PEOR, descartado

Anadir al principio de `translate()` un atajo para `0xFFFF'FFFF'8000'0000..BFFF'FFFF` en modo
kernel (mismo resultado en direccionamiento de 32 y de 64 bits) hundio el rendimiento:
**42.3/41.4/43.8 fps frente a 46.4/46.8/46.8/46.7 sin el**. A/B con `git stash`, mismo binario,
mismo protocolo. La rama extra no ahorra trabajo real -- el camino de 32 bits ya salia por
`seg`/`direct` en pocas comparaciones -- y en cambio engorda `translate`, que esta en linea en
los caminos calientes. Revertido; `translate` sigue como estaba. Si se vuelve a atacar el 4.85%
de `translate`, la via es cachear la traduccion (micro-TLB de datos indexado por pagina), no
anadir ramas al principio.

