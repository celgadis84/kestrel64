# kestrel64 — Modelo de ejecución y comunicación entre componentes

Documento de **revisión externa**. Describe, sin adornos, cómo está modelado cada
componente de la N64 en este emulador: en qué hilo corre, qué lo planifica, por qué
canal habla con los demás y qué garantías de orden se dan. Fecha: 2026-09-24.

Lo que NO es: no hay capas HLE. No se intercepta ni una función de libultra. Todo
pasa por registros MMIO reales (SP_*, DPC_*, VI_*, AI_*, SI_*, PI_*, MI_*) y por
interrupciones MI. El microcódigo del RSP se ejecuta instrucción a instrucción (LLE).

---

## 1. Hilos del proceso

| Hilo | Creado en | Qué hace | Afinidad |
|------|-----------|----------|----------|
| **main** | `System::runLoop` | GLFW: ventana, `pumpFrame`, muestreo de los 4 mandos, presentación Vulkan | — |
| **cpu** | `System::runLoop` (`std::thread cpuThread`) | `System::run` → `stepCpu(n)`. Ejecuta el R4300i y **todo el MMIO** | núcleo físico 0 |
| **rsp** | `Memory::startRcpThreads` | `rspWorkerLoop`: ejecuta microcódigo hasta BREAK | núcleo físico 2 |
| **rdp** | `Memory::startRcpThreads` | `rdpWorkerLoop`: rasteriza tramos del FIFO (Parallel-RDP o SoftRDP) | núcleo físico 1 |
| **audio feeder** | `audio::openDevice` | waveOut: saca del anillo al DAC del anfitrión | — |
| **telemetría** | `System` (opcional) | servidor TCP/JSON (MCP), toma `coreMutex` | — |
| **hostprof** | opcional | muestreador de perfil del anfitrión | — |

Los hilos rsp/rdp **solo existen en modo Threaded**. En Lockstep el hilo de CPU hace
las tres cosas (ver §7).

No hay pool de hilos, ni cothreads/fibras, ni planificador cooperativo tipo libco.
Son tres hilos de SO con estado propio y puntos de cita explícitos.

---

## 2. El reloj: una sola unidad para todo

**Unidad base = "instrucción de invitado retirada" (op).** No hay un contador de
ciclos maestro separado.

```
Count del COP0 avanza +1 por instrucción retirada     -> CPI modelado = 2
insnTarget  = 93.75 MHz / CPI
fieldInsns  = insnTarget / 59.94        (~782.000 ops por campo de vídeo)
```
(`src/core/system.hpp`, `struct Clocks`.)

El reloj que consultan todos los componentes es `Memory::cartNow()`:

```
cartNow() = cpu.retired            (ops retiradas ya confirmadas)
          + cpu.jitPending         (ops de bloques JIT de la cadena en curso, sin commit)
          + paradas de caché convertidas a ops    (stallCycles/stallOps, si el modelo está activo)
```

Los cinco campos están en **una sola línea de caché** a propósito (`alignas(64)` en
`CPU`): quien más llama a `cartNow()` es el hilo del RSP, en cada vuelta de sus citas.

Conversión entre dominios (el RCP corre a 62.5 MHz, la CPU retira 1 op cada 2 ciclos):

```
ops de RSP por op de CPU = 62.5e6 / (93.75e6/2) = 4/3
rcpOpsToCycles / rcpCyclesToOps   (memory.hpp, con recíproco exacto pre-calculado)
```

**Punto a validar:** que derivar todo de "instrucciones retiradas" en vez de ciclos es
defendible, dado que Count en el VR4300 avanza a medio reloj y aquí avanza +1 por op
retirada. Consecuencia: un juego que ejecute instrucciones caras (mult/div, fallos de
caché) no estira el campo de vídeo salvo que el modelo de paradas esté activo, que es
justo lo que aporta `stallCycles`.

---

## 3. Qué hace de planificador

No hay cola de eventos genérica ordenada por tiempo. Hay **dos mecanismos**.

### 3.1 Plazos (deadlines) en el reloj de invitado — dentro del hilo de CPU

Cada componente que tarda en terminar arma un instante futuro en `cartNow()`:

| Plazo | Se arma en | Vence en | Efecto |
|-------|-----------|----------|--------|
| `siDoneAt` | `siDma` | `siFinish()` | copia PIF->RDRAM + MI_SI |
| `piDoneAt` | `piArm` | `piFinish()` | MI_PI |
| `viNextAt` | `viTick` | `viTick` | MI_VI / cierre de campo |
| `aiNextAt` | `aiTick` | `aiTick` | MI_AI (fin de búfer del DAC) |
| `spDoneAt` | `spEndArm` (hilo RSP) | `rcpRetire` (hilo CPU) | BROKE + MI_SP |
| `dpDoneAt` | `dpEndArm` | `rcpRetire` | fin de lista + MI_DP |

El bucle de `stepCpu` comprueba los plazos **después de cada instrucción** (y también
tras cada bloque del JIT), no al final del subtramo. `evNextAt` es el mínimo plegado de
VI y AI, para que la comprobación caliente sea un entero contra otro.

El dynarec tiene prohibido tragarse un plazo dentro de un bloque: `rcpDueIn()` / `siDueIn`
acotan el presupuesto del bloque, y cualquier store que arme un plazo nuevo anula el
permiso de la cadena enlazada (`*jitGuardPtr = 0`).

### 3.2 Citas y barreras entre hilos — modo Threaded

El RSP y el RDP no se planifican por tiempo: se **acoplan al reloj de la CPU** en los
puntos donde el hardware obliga a que uno vea lo que el otro hizo. Ver §6.

### 3.3 El bucle exterior

```
while(!shutdown):
    stepCpu(clocks.tickInsns())      # = fieldInsns / viTicksPerField  (16 subtramos/campo)
    viTick(cpu.guestOps())
    ...limitador de velocidad anclado al reloj del campo de vídeo...
```

`viTicksPerField` (16) **solo trocea el bucle del anfitrión**. Medido: cambiarlo a
4 / 16 / 64 deja la lista de instantes de invitado de todas las interrupciones
byte-idéntica y el md5 del framebuffer idéntico. No es un reloj.

---

## 4. Componente por componente

### 4.1 CPU — VR4300 (`src/cpu/`)

- Intérprete de 64 bits + dynarec x86-64 opcional (`KESTREL_JIT`, **activo de fábrica**).
- TLB de 32 entradas real, excepciones vectorizadas, delay slots con el par `pc/nextPc`.
- Cachés I y D **modeladas** (tags, índice, líneas sucias, relleno); el coste en ciclos
  es opt-in (`KESTREL_CACHECOST`).
- Dueña exclusiva de todo el MMIO: **ningún otro hilo escribe registros del RCP salvo el
  RSP a través de su COP0** (ver 4.2).
- Detalle en `docs/CORES-CPU-RSP.md`.

### 4.2 RSP (`src/rsp/`)

- Intérprete LLE completo: núcleo escalar MIPS-ish + unidad vectorial de 8 carriles de
  16 bits por COP2, con acumulador de 48 bits, banderas y saturaciones. Dynarec propio
  opcional (`KESTREL_RSPJIT`, activo de fábrica).
- **Arranque**: la CPU escribe SP_STATUS con CLEAR_HALT -> `rspSubmitKick()` fecha el
  lanzamiento (`spMarkKick`: `spKickOps = cartNow()`, `spKickEdge = rcpOpsToCycles(...)`)
  y despierta al worker. El instante de lanzamiento lo toma **siempre el hilo de CPU**.
- **Ejecución**: el worker corre `rsp.step(~0)` hasta BREAK. Cuenta ciclos propios
  (`cyclesRun`), que se convierten a instantes de invitado con `spCycleAt()`.
- **Fin**: el worker arma `spDoneAt` (`spEndArm`) con el coste ya modelado; **quien lo
  hace visible es el hilo de CPU** cuando su reloj llega (`rcpRetire`), y ahí se ponen
  BROKE y MI_SP. Así el fin de tarea cae en una instrucción concreta del invitado y no
  cuando el anfitrión termina de calcular.
- **Canales hacia fuera**: DMA SP_DRAM_ADDR/SP_MEM_ADDR/SP_RD_LEN/SP_WR_LEN (DMEM/IMEM
  contra RDRAM), SP_STATUS con sus 8 bits SIGNAL para el ping-pong con la CPU, y
  escrituras a DPC_* por COP0 para patear el RDP.

### 4.3 RDP (`src/rdp/`, `src/vrdp/`)

- Dos rasterizadores: **Parallel-RDP** (Vulkan, de fábrica) y SoftRDP (CPU).
- **Arranque**: escritura a DPC_END (de la CPU o del RSP) -> `rdpSubmit` empuja un tramo
  `{current, end, xbus}` a una cola SPSC y pone GCLK|PIPE_BUSY.
- **Ejecución**: el worker saca el tramo y lo rasteriza. Antes de dormirse gira
  `KESTREL_RDPSPIN` vueltas (131072 de fábrica) sobre `dpPending`: con Parallel-RDP los
  tramos llegan a decenas de miles por segundo y la llamada al kernel del `notify_all`
  dominaba el perfil.
- **Fin**: en SYNC_FULL se limpian los bits de ocupado y sube MI_DP, otra vez fechado
  (`dpEndArm` -> `rcpRetire`), no en el instante de pared.
- **Lecturas de DPC_CURRENT / DPC_STATUS**: se evalúan en el reloj de **quien lee**
  (`dpcCurrentFor(now, who)`), porque CPU y RSP tienen relojes distintos en Threaded.

### 4.4 VI

- `viTick(retiredNow)` (`memory.cpp:2233`). Un campo dura `viFieldInsns` ops. Detecta
  **cruces**, no "estar por encima":
  - MI_VI cuando el barrido cruza la media-línea programada en VI_INTR (V_INTR=0 es una
    línea de coincidencia válida; libdragon la usa).
  - cierre de campo cuando cambia `retiredNow / field`.
  - `VI_V_CURRENT` se calcula del **mismo** `viFieldInsns`, para que interrupción y sondeo
    midan el mismo tiempo (antes había dos relojes: 750k y 1.5625M por campo, y un juego
    que mezclara ambos veía dos campos por cada uno).
- El VI no tiene framebuffer propio: relee RDRAM cada campo, y ese tráfico se contabiliza
  en el medidor de bus (`ramBytesVi`).

### 4.5 AI

- El DAC drena **por muestras**, no por campos: `rate = aiVidClock/(dacrate+1)`, crédito
  acumulado en el mismo reloj de ops, en aritmética entera (sin coma flotante, para que
  sea bit-idéntico entre modos). Al vaciarse un búfer se saca de la FIFO y sube MI_AI.
- `aiVidClock` depende de la norma de TV (NTSC 48.681812 MHz, PAL 49.656530, PAL-M
  48.628316): un cartucho PAL programa el mismo dacrate para otra frecuencia de muestreo.
- La versión anterior gastaba como mucho un búfer por campo y tiraba el crédito sobrante;
  SM64 encola búferes de ~0.75 campos, así que drenaba al 75% del ritmo real y el juego se
  bloqueaba en FIFO_FULL. Ese era el audio entrecortado, y era del invitado, no del sumidero.
- El hilo *feeder* del anfitrión solo consume de un anillo; no toca el reloj del invitado.

### 4.6 SI / PIF / joybus

- No hay "lectura de mandos por fotograma". El joybus corre **dentro del DMA de lectura
  PIF->RDRAM** (`siDma(toPif=false)`), que es donde lo corre el hardware: el PIF analiza el
  bloque de órdenes y contesta al RD64B.
- El DMA de escritura RDRAM->PIF solo deposita el bloque y consume el bit 0 del byte de
  control (0x3F). El bit 1 es el **desafío del CIC-6105** (máquina de estados sobre
  nibbles, documentada en n64brew), que no toca los mandos.
- El joybus es serie y lento (~4 µs/bit), así que `siDma` devuelve la duración, se arma
  `siDoneAt = cartNow() + usToInsns(us)` y **la copia a RDRAM ocurre al vencer**, en
  `siFinish()`, junto con MI_SI. `KESTREL_SIINSTANT=1` vuelve al comportamiento
  instantáneo, solo para bisecar.
- Por qué importa: `osContStartReadData` solo reescribe el bloque cuando la orden previa no
  era READ_BUTTON, así que a partir del segundo fotograma el juego lanza **solo** el DMA de
  lectura. Si el joybus no corriera ahí, los botones se congelarían.
- El anfitrión muestrea los 4 mandos en `Presenter::pumpFrame` (hilo main) a ritmo de
  ventana y publica en `Memory::padPort[4]`; el invitado ve lo que haya cuando sondea.

### 4.7 PI

- `piArm` modela el coste real del traslado (`piXferCycles`, dominio y latencias del
  cartucho) y vence en `piFinish` -> MI_PI. También se modela el decaimiento del latch de
  E/S del PI (`piIoDecay`).

### 4.8 MI

- `mi_intr` es `std::atomic<u32>` con `fetch_or`/`fetch_and`: el productor (RSP/RDP)
  publica con **release** después de sus escrituras a RDRAM, y la CPU adquiere al leer.
- Contrato: el juego espera la interrupción antes de tocar lo que el RCP produjo, así que
  no hace falta coherencia por píxel — el hardware tampoco la da.

---

## 5. Diagrama de canales

```
                 +------------ hilo CPU (dueño del MMIO) -------------+
   plazos SI/PI/VI/AI  ----------------------------------------+      |
                       stores a SP_*  -> rspSubmitKick --------|----->|--> hilo RSP
                       stores a DPC_* -> rdpSubmit ---------+  |      |      |
                       lee mi_intr (acquire) <-----------+  |  |      |      | COP0 -> DPC_*
                                                         |  |  |      |      v
                 +------ rcpRetire (hace visibles -------+  |  |      |   rdpSubmit
                         spDoneAt / dpDoneAt) --------------|--+      |      |
                                                            v         |      v
                                                      hilo RDP <------+------+
                                                            |
                                                MI_DP (release) --> mi_intr
```

Reglas duras:
1. **Solo el hilo de CPU cambia el estado visible del invitado en un instante.** Los
   workers *proponen* (arman plazos); el hilo de CPU *publica*.
2. Todo lo que cruza hilos va por atómicos con release/acquire o bajo `rspMx`/`rdpMx`.
3. Las variables de condición se notifican con `notify_all`, nunca `notify_one`: en el
   mismo condvar esperan dos clases de hilo con predicados distintos (worker = "hay
   trabajo", drenador = "cola vacía") y `notify_one` pierde despertares.

---

## 6. Primitivas de sincronización (modo Threaded)

| Primitiva | Quién espera | Condición | Por qué existe |
|-----------|-------------|-----------|----------------|
| **`spBarrierWait`** | CPU | `cartNow() < ops(ciclos hechos por el RSP)` | la CPU no puede adelantarse al RSP más de `spLeadOps()` (512) |
| **`spReadSync(now)`** | RSP | `cartNow() >= now` | el RSP no puede leer estado de la CPU (SP_STATUS, RDRAM por DMA) antes de que la CPU llegue a ese instante |
| **`dpReadSync` / `dpLogWait`** | RSP | ídem, para DPC | lecturas de DPC_CURRENT/STATUS coherentes |
| **`rspDmaRdpWait`** | RSP | el RDP no está pintando la zona que el DMA va a leer | evita leer un framebuffer a medio pintar |
| **`dpBarrierWait`** | CPU | el RDP ha llegado al instante pedido | sondeo de DPC_STATUS desde el juego |
| **`cpuRamWrBarrier`** | CPU | adelanto 0 en el instante de la escritura | una escritura de la CPU solo se ve desde el RSP cuando **llega a RDRAM**: vuelco de línea sucia de D-caché o escritura no cacheada |
| **`rcpPace`** | CPU | reparto 4/3 de ops | regulador: el equivalente al interleave de Lockstep |
| **diarios (`dpLog`, `spLog`, `dmaLog`)** | — | — | el RSP **archiva** su escritura fechada y sigue, en vez de citarse; la CPU la aplica cuando su reloj llega (`dpLogApply`) |
| **aparcamiento (`rspParkWait`)** | RSP | `cartNow() >= tope` | el RSP duerme en vez de girar cuando la CPU está lejos |

Detalles de coste que hacen falta para leer el perfil:

- Las esperas usan `PAUSE` cada vuelta (`KESTREL_RDVPAUSE=0` de fábrica) porque un hilo
  girando sin PAUSE se lleva media capacidad de emisión del núcleo SMT, y el hermano suele
  ser el hilo de CPU, que es el palo largo. Medido: 4818 -> 4489 ms, con los contadores de
  invitado idénticos (el cambio solo toca cómo espera el anfitrión).
- Con hueco grande (más de `rdvSleepGap` ops) el RSP se duerme y la CPU lo despierta al
  pasar por el instante (`rspWakeIfDue`), con tope de 200 µs por si se pierde el aviso. La
  condición de salida sigue siendo `cartNow() >= now`, así que el invitado sale idéntico.
- **Salvavidas**: si la CPU no avanza (parada, o esperando algo del RSP), la cita se
  renuncia por tiempo de pared y se cuenta (`spRdvWaives`). En una corrida sana es 0.

### Ocupación medida (Perfect Dark PAL, 5.70 s de pared, Threaded + JIT + Parallel-RDP)

```
cpuWait 15.0%  (freno 0.0%  barSP 0.6%  barDP 13.3%)
rsp ocupado 87.8%  (esperaDP 0.0%)  aparcado 0.4%
rdp ocupado 19.7%
CPU real del anfitrión: cpu 100.5%   rsp 89.5%   rdp 43.8%
```

Interpretación: el RSP **va por delante** en tiempo de invitado y su "87.8% ocupado" es en
su mayoría giro en `spReadSync` (95% de las muestras del perfilador de anfitrión sobre ese
hilo). El techo es el hilo de CPU al 100%. Quitar la barrera DP (`KESTREL_DPBARRIER=0`) no
da ganancia de pared (5980/5926 ms contra 6057/5901) y da el mismo statehash.

---

## 7. Los dos modos

| | **Lockstep** | **Threaded** (`KESTREL_THREADS=1`) |
|---|---|---|
| Hilos RCP | ninguno | rsp + rdp |
| RSP | intercalado dentro de `stepCpu`, 4/3 ops por op de CPU | hilo propio, acoplado por citas |
| RDP | síncrono dentro del store a DPC_END | cola SPSC + worker |
| Uso | referencia determinista, `n64-systemtest` | jugar y medir |

Lockstep es el **oráculo**: cualquier cambio en Threaded se difea contra él. `n64-systemtest`
corre en Lockstep (0/3721).

---

## 8. Determinismo: qué se garantiza y qué no

**Se garantiza**, corrida a corrida con el mismo binario y la misma ROM:
- Lockstep: determinista por construcción.
- Threaded: Perfect Dark y DK64 dan statehash idéntico en todas las corridas medidas.

**Agujero conocido y honesto**: con adelanto de la CPU sobre la barrera del SP
(`KESTREL_SPLEAD`) por encima de 512, **SM64 da dos statehash distintos entre corridas**.
Diagnóstico hecho:

- No es el punto de parada: parando por número de intercambios, el contador de ops es
  idéntico.
- Es estado real: el RSP quema ciclos distintos (420826365 contra 420770470) y hace DMA
  distintos (540918 contra 540986 lecturas); el rastro acaba en el bit BD de Cause, o sea
  la última interrupción cayendo en otra instrucción.
- 0 renuncias de cita y 0 plazos vencidos: no se escapa por ninguna escotilla contada.
- **Causa**: el adelanto en sí. La cita obliga a la CPU a *llegar* al reloj del RSP, pero no
  a no haberse *pasado*: con adelanto L, la CPU ya ha escrito en RDRAM hasta L ops en el
  futuro del RSP, y lo que el RSP lee de esa zona depende de por dónde vaya el anfitrión.
- **Media solución aplicada**: `cpuRamWrBarrier` para la CPU con adelanto 0 en los sitios
  donde una escritura llega de verdad a RDRAM, y la deja correr libre entre medias. Con eso
  el adelanto reproducible sube a 512 (16/16 corridas iguales) y ahí se acaba: 1024 -> 8/2,
  2048 -> 10/2, 4096 -> 7/3. Los dos hashes de cada valor comparten los 28 bits bajos, o sea
  que se mueve **un campo**, no el estado entero.
- Descartado: el grano del RSP (`KESTREL_RSPTANDA=64` filtra igual) y DMEM (extender la
  barrera ahí no limpia 1024 ni 4096).
- Coste de bajar el adelanto a 0: +23% de pared en PD, **y tampoco se llega al estado de
  Lockstep**. O sea 0 no compra fidelidad, solo lentitud.

**Pregunta abierta relacionada**: por qué Threaded no converge a Lockstep aunque las citas
se cumplan. Medido: PD lockstep `14995cfadedc7f38` contra threaded-0 `0a64f3863fd236b1`.

---

## 9. Qué se pide validar

1. **Modelo de reloj** (§2): derivar todo de ops retiradas con CPI 2 y Count a +1/op.
   ¿Rompe algún caso conocido de hardware que no cubra `n64-systemtest`?
2. **Fechado de las interrupciones del RCP** (§4.2, §4.3): el worker calcula el coste y el
   hilo de CPU lo publica cuando su reloj llega. ¿Es equivalente a lo que hace el hardware,
   o hay algún caso en el que el RCP tenga que ser visible antes de que la CPU ejecute la
   instrucción del plazo?
3. **Joybus dentro del DMA de lectura** (§4.6). ¿Es correcto que el bloque de órdenes se
   analice en la lectura y no en la escritura? ¿Y que MI_SI espere a la ventana serie?
4. **La lista de citas de §6**: ¿falta alguna dirección de visibilidad CPU-RSP-RDP que en
   hardware esté ordenada y aquí no? En concreto: escrituras de la CPU a DMEM/IMEM,
   SP_SEMAPHORE, y el ping-pong de bits SIGNAL.
5. **El agujero de determinismo de §8**: qué otra escritura de la CPU puede ver el RSP sin
   pasar por `cpuRamWrBarrier`.
6. **AI** (§4.5): drenaje por muestras con `aiVidClock` por norma de TV.
7. **VI** (§4.4): cruces de media-línea y V_INTR=0 como línea de coincidencia válida.

---

## Ficheros

| Tema | Fichero |
|------|---------|
| Bucle, relojes, plazos | `src/core/system.cpp`, `src/core/system.hpp` |
| MMIO, DMA, citas, barreras, workers | `src/core/memory.cpp` (4882 líneas), `src/core/memory.hpp` |
| CPU | `src/cpu/cpu.cpp`, `src/cpu/jit.cpp` |
| RSP | `src/rsp/rsp.cpp`, `src/rsp/rspjit.cpp` |
| RDP | `src/rdp/`, `src/vrdp/` |
| Presentación e input | `src/video/present.cpp` |

Docs hermanos: `docs/ARCH-SYNC.md` (auditoría de esperas), `docs/HW-RCP-COMM.md` (hechos de
n64brew sobre SP/DPC/MI), `docs/CORES-CPU-RSP.md` (intérpretes y dynarecs),
`docs/PERF-RCP-SYNC.md`, `docs/RDP-TIMING.md`, `docs/VI-CLOCK.md`.
