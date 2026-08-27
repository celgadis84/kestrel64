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

### Que cede el JIT al interprete, medido (2026-08-26)

Histograma temporal a la entrada de `jitInterpOp` (SM64, 200 flips, 7.64 M cesiones):

| Familia | % de las cesiones |
|---|---|
| COP1 | **99.20%** |
| SPECIAL (DIV/DDIV/DMULT...) | 0.71% |
| todo lo demas | ~0.09% |

Dentro de COP1: MUL ~37%, ADD ~25%, MTC1 ~10%, CTC1 ~6%, MFC1 ~6%, SUB ~4%, SQRT ~3%,
CFC1 ~3%, TRUNC.W ~2%, C.cond ~2%. Es decir **la FPU es lo unico que el JIT no compila**, y
una cuarta parte de eso son movimientos triviales entre gpr y el banco FPU.

### Movimientos COP1 con trampolin propio

`MFC1/DMFC1/CFC1/MTC1/DMTC1` no pueden desviar el control: mueven 32/64 bits y nada mas. Su
trampolin (`CPU::jitCop1Move<RSc>`) se salta el montaje de contexto de `jitInterpOp` -- guardar
`pc`/`nextPc`/`curPc`, fabricar la VA, y las seis comprobaciones de desviacion al volver -- y
tambien el `switch` de `cop1op`. El unico caso que si puede desviar, CU1=0, delega en
`jitInterpOp`, que monta el contexto exacto que la excepcion necesita.

`CTC1` se queda fuera a proposito: escribir FCSR con una Cause armada vectoriza una FPE ahi
mismo, con un quirk de pipeline en `Cause.CE`; ese camino tiene que seguir siendo el del
interprete.

**Banco de precision.** El banco de fps (PRDP, threaded) tiene ~±1 fps de ruido de host, que
tapa mejoras de esta talla. Para decidir se usa lockstep con tope de instrucciones
(`KESTREL_THREADS=0 KESTREL_MAXINSN=300000000`), que es reproducible al ~0.5%:

| | 3 tiradas (s) | min |
|---|---|---|
| sin | 8.764 / 8.692 / 8.628 | 8.628 |
| con | 8.602 / 8.581 / 8.609 | 8.581 |

Lockstep diluye una mejora de CPU (serializa RSP y RDP en el mismo hilo), asi que ese -0.55%
del total es una mejora mayor en el hilo de CPU. fps PRDP: sin cambio medible fuera del ruido
(~47).

### Descartado por medida: testigo de generacion para la validacion SMC

Idea: dar a cada linea de I-cache un contador `gen` que sube en cada `icFill`, y que el bloque
recuerde las gens de las lineas que cubre, para saltarse la recomparacion palabra a palabra en
cada entrada del driver. Medido (max de 7 tiradas de fps): **47.02 con, 47.62 sin**. El bucle
de validacion ya opera sobre lineas calientes en L1 del host; el testigo anade una indireccion
a heap (`std::vector`) y ramas que cuestan mas que lo que ahorran. Revertido.


## Quinta tanda (2026-08-26): FPU en el JIT y el despeñadero del regulador

Dos cambios que sólo tienen sentido juntos, porque el primero destapó al segundo.

### 1. ADD/SUB/MUL de COP1 con camino rápido

El histograma de la tanda anterior decía que el 99.20% de lo que el JIT cede al intérprete es
COP1, y que dentro de eso MUL (~37%) + ADD (~25%) + SUB (~4%) son dos tercios. `jitCop1Alu<FN,FMT>`
resuelve el caso limpio en C —operandos normales o cero, sin enables armados, resultado normal y
sin más excepción que Inexact— y delega en el intérprete todo lo demás (subnormal, NaN, inf,
overflow, underflow, trap armado). Escribe exactamente lo que escribiría `setS`/`setD`:
`fcr31` con Cause I y la bandera pegajosa, y el destino con `(u64)rb` (32 bits limpian el alto).

Validado con un oráculo temporal (`KESTREL_FPORACLE`): el camino rápido calculaba, dejaba que el
intérprete ejecutase la misma op, y comparaba destino y `fcr31`. Cero discrepancias en 1200 M
instrucciones de SM64. El oráculo se retiró después; el patrón queda anotado aquí porque es la
forma barata de validar las siguientes (DIV, SQRT, CVT, C.cond).

### 2. El regulador de ritmo estaba calibrado para un emulador más lento

Al medir el cambio anterior en PRDP+threaded salió una regresión brutal y reproducible:
**46.7 → 34 fps**, con A/B intercalado de dos binarios para descartar deriva del host. Pero en
lockstep el mismo cambio medía ligeramente *mejor*. Contradicción real, no ruido.

Callejones descartados por medida, para que nadie los repita:

- **No era divergencia numérica.** El oráculo no encontró ninguna. Ojo con el atajo de comparar
  volcados de framebuffer en modo threaded: **no son deterministas** — el mismo binario da md5 e
  `Int(0)` distintos entre corridas, porque el punto donde cae el corte por instrucciones pilla al
  RCP en otro estado. Ese veredicto no vale en threaded.
- **No era coste del camino rápido.** Un stub que delegaba en la primera línea medía igual que la
  base, así que ni el despacho ni los trampolines costaban nada.
- **No era contabilidad.** `retired` y `Count` avanzan una unidad por op pase por donde pase.

Lo que sí era, lo dijo el heartbeat: `N64 speed: CPU 202% → 451%` con el RSP clavado en ~110%.
La CPU emulada se había acelerado y gastaba lo ganado **girando**. `kPaceSlack` —cuántas
instrucciones puede adelantarse la CPU al RSP antes de dormir en el condvar— llevaba el valor de
una calibración vieja, hecha cuando el hilo de CPU rendía 29 fps. Barrido con el hilo ya en ~47:

| slack | base | +fpalu |
|-------|------|--------|
| 16 K  | 36.2 | 38.2 |
| 64 K  | 38.5 | 41.0 |
| 128 K | —    | 43.6 |
| 192 K | —    | 46.9 |
| **256 K** | **46.5** | **47.8** |
| 320 K | —    | 44.2 |
| 384 K | —    | 41.7 |
| 512 K (viejo default) | 46.4 | 36.0 |

El pico se había desplazado de 512 K a 256 K, y el viejo default había quedado justo en el borde:
acelerar la CPU un 3% costaba 11 fps. Con el default en 256 K el camino rápido de FPU gana en
**todo** el rango, que es lo que decía el lockstep desde el principio.

**Regla que sale de aquí: `kPaceSlack` es función de la velocidad del hilo de CPU. Cada vez que
el intérprete o el JIT se aceleren de forma apreciable, hay que rebarrerlo.** La señal de que se
ha quedado corto es exactamente la de esta tanda: `N64 speed: CPU` se dispara mientras el RSP no
se mueve.

Resultado: **47.2–48.0 fps** en SM64 (antes 46.2–46.9), 157–160% de consola.

## Sexta tanda (2026-08-27): limitador de velocidad

Hasta ahora el emulador **no tenia limitador**: corria a lo que diese el host. Lo detecto el
usuario por el oido — "diria que va acelerado por el audio que sale". Correcto: el bench mide
157-250% de consola, y a 250% el audio sale acelerado y el juego responde a destiempo. Servia
para medir, no para jugar.

**Reloj elegido: el campo de video.** `Memory::viTick()` ya devuelve `fieldClose` una vez por
cada `viFieldInsns` instrucciones retiradas, y ese es el mismo reloj que ancla todo el tiempo
del guest (VI_V_CURRENT, MI_VI, `aiTick`). Limitar los cierres de campo a `Clocks::viFieldHz`
(59.94 Hz) de tiempo real deja el guest exactamente a 100%, sin un segundo reloj que pueda
discrepar del primero.

Detalles que importan:

- **Vencimiento absoluto, no relativo.** `due = T0 + n * (1/59.94 s)` con `n` = campos cerrados.
  Dormir "16.68 ms desde ahora" acumula el error de cada despertar y el guest queda por debajo
  del 100% de forma creciente.
- **Reancla al atrasarse mucho.** Si vamos >250 ms tarde (pausa del depurador, carga de estado,
  un campo carisimo) se reinicia el ancla. Recuperar el tiempo perdido corriendo al doble se ve
  y se oye peor que perderlo.
- **Fuera de `coreMutex`.** `fieldClosed` se saca del bloque con el lock; dormir con el lock
  cogido bloquearia al hilo de telemetria durante todo el campo.
- **Temporizador de alta resolucion.** El sleep normal de Windows tiene granularidad ~15.6 ms,
  inservible para un campo de 16.68 ms (dormiria un campo entero de mas). Se usa
  `CreateWaitableTimerExW(..., CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, ...)` (Win10 1803+, solo
  kernel32, sin dependencias nuevas) que baja a ~0.5 ms, y el ultimo medio milisegundo se gira
  con `yield()` porque eso es lo que cuesta despertar de todas formas.
- **Armado solo con ventana** (`videoOn`). Gates, bench y krom corren headless y deben seguir a
  tope: si no, todo mediria 30 fps. `KESTREL_THROTTLE=0/1` fuerza cualquiera de los dos.

Medido (SM64, PRDP, threaded, 200 flips): `N64 speed: CPU 95.4-98.0%` con el limitador armado,
frente a 157-250% sin el. El 2-4% que falta es el coste de despertar y no se oye.

### 3. Conversiones de COP1 con camino rapido

Siguiente escalon del histograma tras CTC1: CVT.W.S (~16% de lo que el bloque cede al
interprete), TRUNC.W.S (~14%), CVT.D.S (~9%) y CVT.S.D (~5%). El compilador de SGI convierte a
entero cada vez que un float se usa como indice/coordenada/contador, y cambia de precision al
entrar y salir de rutinas de doble. `jitCop1Cvt<KIND>` resuelve el caso limpio -- operando
normal o cero, resultado dentro del entero de destino, ninguna trampa armada, como mucho
Inexact -- y delega en el interprete todo lo demas: CU1=0, NaN, infinito, subnormal, magnitud
fuera de rango (que en el VR4300 es Unimplemented, NO un saturado) e Inexact con su Enable.
Se anadieron tambien las variantes con fuente doble (CVT.W.D / TRUNC.W.D), gratis en la misma
plantilla.

Validado con el mismo oraculo del apartado anterior, ahora permanente y gateado
(`KESTREL_FPORACLE`, `jitCop1CvtChk`): el camino rapido calcula, se rebobina el destino y
`fcr31`, y el interprete ejecuta la MISMA op; los tres resultados tienen que coincidir. **1.4 M
conversiones de SM64 sin una sola discrepancia** (3000 M instrucciones). Las conversiones son
idempotentes, asi que repetirlas no altera nada mas y el oraculo puede quedarse en el arbol.

Medido: A/B intercalado de dos binarios, tres rondas de 3000 M instrucciones cada una.
old 19463/18992/19207 ms, new 18536/18485/18640 ms -> **+3.5% de velocidad**.

### 4. C.cond de COP1 con camino rapido

Siguiente en el histograma tras las conversiones (~15% de lo que el bloque cede al interprete):
toda comparacion en coma flotante del codigo de SGI acaba en una C.LT/C.LE/C.EQ seguida de
BC1T/BC1F. `jitCop1Cmp<FMT>` resuelve el caso ORDENADO -- ningun operando NaN --, donde ninguno
de los dieciseis predicados puede levantar Invalid: ni por el predicado senalizador (que solo
dispara con operandos no-ordenados) ni por la rareza del VR4300 con el MSB de la mantisa
invertido (que exige un NaN). Deja `Cause` limpia, no acumula Flag y escribe el bit C (23) de
`fcr31`. Con NaN en cualquiera de los dos operandos, o CU1=0, delega en el interprete. Los
subnormales SI se resuelven aqui: el interprete tampoco los filtra en la comparacion -- solo
las ops computacionales lo hacen -- y el `<`/`==` del anfitrion da el mismo orden. `fn` se lee
dentro del trampolin, asi que un unico trampolin por formato cubre los dieciseis predicados.

Validado con el mismo oraculo gateado (`KESTREL_FPORACLE`, `jitCop1CmpChk`): la comparacion
solo escribe `fcr31`, asi que rebobinandolo se repite por el interprete y se comparan los dos.
**1.29 M comparaciones de SM64 sin una sola discrepancia** (901 k en .S + 385 k en .D, 3000 M
instrucciones).

Medido: A/B intercalado de dos binarios, OCHO rondas de 3000 M instrucciones.
old media 18289 ms (min 17992), new media 18361 ms (min 18048) -> **neutro dentro del ruido**
(la dispersion del anfitrion es de ~±2%, mucho mayor que el efecto). Con 1.29 M comparaciones en
3000 M instrucciones el ahorro teorico esta por debajo del 1%, asi que el banco no puede
resolverlo. Se conserva porque es correcto y quita esas 1.29 M entradas al interprete; ninguna
de las dos puertas se mueve.

### 5. DIV.S/DIV.D y CVT.S.W / CVT.D.W

Ultimos escalones del histograma de COP1: DIV de formato (~3.3%) y las conversiones con fuente
ENTERA (rs=0x14, ~4.9%).

DIV entra en la plantilla que ya tenian ADD/SUB/MUL (`jitCop1Alu<FN=3>`): mismo cribado de
operandos (normal o cero) y mismo cribado del resultado, que es lo que hace segura la division.
Un divisor cero es "normal o cero" y pasa el filtro de entrada, pero el resultado sale infinito
o NaN y el cribado de SALIDA lo rechaza -- ademas de que DivideByZero/Invalid caen fuera de la
mascara de Inexact --, asi que el caso lo sigue resolviendo el interprete con su Cause completa.

`jitCop1CvtW<KIND>` cubre CVT.S.W y CVT.D.W. Aqui no hay casos raros de entrada: cualquier
patron de 32 bits es un entero con signo valido. .D es exacto siempre (Cause limpia, sin
banderas); .S puede redondear -- 24 bits de mantisa para 32 de entero -- y solo puede levantar
Inexact, que se acumula igual que en el resto y delega si tiene su Enable armado.

Validado con el oraculo gateado, ahora con las ocho variantes de conversion instanciadas: las
OCHO llegan a dispararse en SM64 y ninguna produce discrepancia (1500 M instrucciones, umbral
del contador bajado temporalmente a 1024 para verlas todas).

Medido: A/B intercalado, cuatro rondas de 3000 M instrucciones. antes media 18351 ms, despues
media 18195 ms -> **~0.9%**, en el limite del ruido del anfitrion. Ambas puertas verdes.

### 6. Traduccion de direcciones en linea (`xlatDirect`)

Perfil del anfitrion (KESTREL_HOSTPROF, 3000 M instrucciones de SM64 con JIT) despues de cerrar
COP1: `CPU::translate` 27.3%, `jitTryBlock` 24.9%, `kestrel_jitLW` 16.5%, `dcWrite` 6.1%,
`rcpPace` 5.8%, `jitLWC1` 5.5%. `translate` era la primera: una LLAMADA fuera de linea por cada
acceso a memoria, aunque el 99% de esos accesos sean kseg0/kseg1 en modo kernel, donde la
traduccion entera es un AND.

`CPU::xlatDirect` (en cpu.hpp, en linea en el llamador) resuelve ese caso: modo kernel
(EXL/ERL puestos o KSU==0) con KX claro, direccion de compatibilidad -- los 32 bits altos son
la extension de signo del bit 31 -- y segmento kseg0/kseg1 (`va & 0xC0000000 == 0x80000000`).
Esos dos segmentos son directos: no pasan por la TLB, no pueden fallar, no miran el ASID y no
tocan `xlatCacheable`, exactamente como el `return va & 0x1FFF'FFFF` de `translate`. Cuando NO
aplica devuelve false y el llamador llama a `translate`, que sigue siendo la unica implementacion
del caso general (TLB, 64 bits, usuario/supervisor, xkphys, AdE). `cacheable()` tambien pasa a
ser en linea: se llama una vez por acceso y es una comparacion de tres bits.

Puesto en `jitMemOp` (los quince trampolines de load/store del JIT) y en los ayudantes del
interprete (`read8..read64`, `write8..write64`, las rutas SB/SH/SW/SD/SWL/SWR/SDL/SDR y LL/LLD).

Medido, A/B intercalado de 3000 M instrucciones:
- JIT: antes 18446 ms de media, despues 17238 -> **+7.0%**
- interprete (400 M instrucciones): 21637 -> 20624 ms -> **+4.9%**

Ambas puertas verdes, 12/12 y krom regress=0 en las dos.

### 7. D-cache en linea (`dcRead` / `dcWrite`)

Perfil tras el cambio anterior: `jitTryBlock` 30.7%, `kestrel_jitLW` 20.1%, `dcWrite` 11.0%,
`rcpPace` 9.5%, `jitLWC1` 6.8%, `dcFill` 6.1%. `dcRead`/`dcWrite` eran otra LLAMADA fuera de
linea por acceso a memoria cacheable, y su cuerpo caliente es corto: indice = `(phys >> 4) &
0x1ff`, comparar etiqueta, y extraer/insertar 1/2/4/8 bytes big-endian de la linea de 16.

Ahora viven en `cpu.hpp`, en linea, con el fallo de linea (`dcFlush` + `dcFill`) marcado como
improbable y fuera de linea. El acceso a la linea pasa a `memcpy` + `bswap` por tamano en vez
del bucle byte a byte; la semantica es identica (la linea guarda bytes big-endian).

La cola de depuracion del store -- punto de vigilancia `KESTREL_WATCHP` y write-through de
diagnostico `KESTREL_DCWT` -- sale del camino caliente a `dcWriteDbg`, tras una bandera
`dcDbgOn` que solo se arma si una de las dos esta puesta. Antes se comprobaban las dos
condiciones en CADA store de 32 bits.

Medido, A/B intercalado de 3000 M instrucciones (SM64, JIT): antes media 17528 ms, despues
16941 -> **+3.4%**. Ambas puertas verdes.

Nota de la bateria: en la corrida de `gate_all` de este cambio, cuatro ROMs de krom
(SHIFT/DSLLV, DSRA, DSRA32, DSRAV) salieron `NODUMP rc=1` y volvieron a 100.00 al re-correr la
bateria sola. No es una regresion del cambio: `rc=1` solo lo devuelve `System::init`, o sea
abrir la ROM. Para que un transitorio asi no obligue a re-correr los 371 casos, `validate.py`
guarda ahora la cola del log del proceso en la nota de la fila NODUMP.
