# Sincronizacion CPU <-> RSP <-> RDP: analisis y rediseno (2026-09-22)

## Tesis del usuario

En la N64 la CPU y el RSP van cada uno por su lado. Se coordinan solo en puntos definidos:
lanzamiento de tarea (CLEAR_HALT), BREAK + interrupcion SP, senales de SP_STATUS
(yield), semaforo, DMA, y el RDP con SYNC_FULL + interrupcion DP. Si el emulador obliga a
esperar en MAS sitios que esos, esta mal decidido y eso es lo que lo hace lento.

## Lo que hace kestrel hoy

Co-simulacion conservadora con ANTICIPACION CERO:

- La CPU nunca pasa del reloj publicado del RSP (`spBarrierWait`): asi un evento que nace
  del RSP (BREAK, escritura en SP_STATUS, DMA a RDRAM, DPC_END) nunca le llega tarde.
- El RSP, cada vez que lee algo que la CPU PODRIA haber escrito, espera a que la CPU llegue a
  su mismo instante: `dpLogWait` al leer DPC_CURRENT/STATUS, `spReadSync` al leer SP_STATUS y
  antes de un DMA desde RDRAM.
- Ademas el estado DPC es del hilo de CPU: las escrituras DPC del RSP van a un diario que la
  CPU aplica al llegar a su instante. El RSP, para leer DPC despues de escribirlo el mismo,
  tiene que esperar a que la CPU aplique SU propio diario.

Con las dos reglas a la vez los dos hilos avanzan alternandose: el RSP para en cada sondeo,
la CPU corre hasta ahi y para en la barrera, el RSP sigue... Es ejecucion en serie con un
cambio de hilo en medio de cada paso.

## Medido (Perfect Dark en juego, ranura 5; SM64, DK64 y junkrunner64 600 cambios de buffer)

Perfil de anfitrion, PD:
- hilo del RSP: 92 % esperando (`dpLogWait` 72 %, `spReadSync` 21 %), microcodigo real ~6 %;
- hilo de CPU: 40 % parado en la barrera del SP (`rcpRetire`).
- ~1 M sondeos de DPC por corrida, y el 99 % piden cita (`cartNow() < now`).

Escrituras de la CPU con una tarea de RSP en marcha (contador temporal, no commiteado):

| juego       | DPC | SP_STATUS | semaforo | otros SP | DMEM/IMEM |
|-------------|-----|-----------|----------|----------|-----------|
| junkrunner64 | 0  | 37        | 0        | 0        | 0         |
| SM64        | 0   | 11        | 0        | 0        | 0         |
| DK64        | 0   | 0         | 0        | 0        | 0         |

**Tesis confirmada.** ~1 M citas por corrida protegen contra escrituras de CPU a DPC que no
ocurren nunca. Las unicas escrituras reales son unas decenas de senales (yield) en
SP_STATUS, y esas ya van con visibilidad cuantizada (`KESTREL_SPSIGQ`, grano 16384).

La hipotesis anterior de SYNC_FULL quedo descartada: `KESTREL_DPBARRIER=0` no sube nada
(20,8 frente a 22,9 fps).

## Rediseno: cada componente tiene dueno y cada canal su latencia

1. **El camino RSP -> RDP es del RSP mientras tiene tarea.** Aplica sus escrituras DPC en el
   acto y en su propio instante, lanza los tramos del RDP y fecha la interrupcion DP con su
   reloj. Como la CPU va siempre por detras del RSP, ese evento le llega a tiempo, igual que
   hoy el BREAK. Leer DPC deja de necesitar a la CPU: se va el 72 % de espera del RSP.
   - La CPU que LEE DPC con tarea en marcha ve el estado en SU instante, que esta en el
     pasado del RSP: hace falta un historial corto de DPC fechado. Pasa pocas veces (el
     `osDpGetStatus` del juego).
   - La CPU que ESCRIBE DPC con tarea en marcha: medido 0 veces en los cuatro juegos. Se
     trata como las senales: canal CPU -> RSP con latencia fija (punto 2). Lento si pasa,
     pero determinista.
2. **Los canales CPU -> RSP (senales de SP_STATUS, semaforo, DMEM/IMEM, DPC) tienen una
   latencia de visibilidad fija L**, en tiempo de invitado. Una escritura de la CPU en `t`
   la ve el RSP desde `t + L`. El RSP solo espera si la CPU va mas de L por detras, asi que
   los dos corren en paralelo dentro de una ventana de L ciclos. Es la generalizacion de
   `KESTREL_SPSIGQ`, que ya hace esto para las senales. Es determinista, y lockstep aplica la
   misma regla, asi que threaded == lockstep se mantiene.
3. **Los canales RSP -> CPU se quedan como estan** (diario fechado + barrera CPU <= RSP).
   Una vez el RSP deja de esperar a la CPU, va por delante casi siempre, porque su trabajo
   real es el ~6 %, y la barrera deja de frenar.
4. **Los DMA del RSP desde RDRAM mantienen la cita exacta**: la CPU escribe ahi las display
   lists y el juego las entrega con writeback + lanzamiento. Contarlos y ver si pesan.

**Fidelidad.** En el hardware nada fija el orden relativo CPU/RSP al ciclo: la contencion de
RDRAM lo mueve de una pasada a otra, y los juegos se coordinan por los puntos de
sincronizacion, no por el ciclo. Una latencia fija L es una eleccion determinista dentro de
lo que el hardware ya varia. El modo "Fiel a consola" puede bajar L (hasta 1 = el modelo
exacto de hoy) para comparar.

**Lo que desaparece.** El adelanto de `dpLogWait` (`KESTREL_DPLOGLEAD`) sobra: el punto 1
quita la cita que tapaba.

## Orden de trabajo

1. Contar DMA-desde-RDRAM y lecturas de SP_STATUS por tarea (dimensionar los puntos 2 y 4).
2. Punto 1 (propiedad de DPC en el RSP). Es la palanca grande. Comprobar con statehash de
   junkrunner64 threaded xN == lockstep, las puertas y fps de PD.
3. Punto 2 (latencia L en CPU -> RSP), barrido de L.
4. Quitar `KESTREL_DPLOGLEAD` y las citas que queden sin uso.

## Revision contra el hardware (2026-09-22)

Hechos del HW en `docs/HW-RCP-COMM.md`. Cruce con el codigo (`src/rsp/rsp.cpp` mfc0/mtc0/BREAK,
`src/core/memory.cpp`):

| # | sitio en kestrel | que hace hoy | HW | veredicto |
|---|------------------|--------------|----|-----------|
| 1 | `Rsp::mfc0` DPC_CURRENT/STATUS -> `dpLogWait(now, true)` | espera a que la CPU llegue a `now` en CADA sondeo | la CPU no escribe DPC con tarea en marcha (medido 0) | espera inutil, 72 % del RSP. Punto 1 |
| 2 | `Rsp::mfc0` DPC con `dpLogPending()` | el RSP espera a que la CPU aplique SUS escrituras DPC | el RSP ve lo suyo al instante | espera inutil. Punto 1 |
| 3 | `Rsp::mfc0` SP_STATUS con `spLogPend` -> `dpLogWait(0, false)` | el RSP espera a que la CPU aplique SUS senales | el RSP ve lo suyo al instante | espera inutil. Basta superponer lo pendiente propio al leer |
| 4 | BREAK con `spLogCrit` -> `dpLogWait(0, false)` | idem, para INTR_ON_BREAK escrito por el propio RSP | idem | espera inutil, misma superposicion |
| 5 | `Rsp::mfc0` resto de SP/DPC con `dpLogPending()` | idem | idem | espera inutil |
| 6 | `Rsp::mfc0` SP_STATUS -> `spReadSync(vis)` | cita con grano `KESTREL_SPSIGQ` | sin latencia definida | ya es el modelo de latencia L. Se queda (y se generaliza) |
| 7 | `Rsp::mtc0` SP_RD_LEN -> `spReadSync(now)` | cita exacta antes de cada DMA desde RDRAM | datos entregados antes por el protocolo (writeback + lanzamiento, o escritura + SIG en rspq) | necesaria para ser deterministas sin versionar la RDRAM. Contar primero |
| 8 | `Rsp::mtc0` DPC fuera del diario (XBUS o FREEZE) -> `dpLogWait(now, true)` | cita | raro | se queda, es poco frecuente |
| 9 | `rcp.sp_semaphore` (`memory.hpp:55`) | `u32` normal; leer-y-poner a 1 no atomico entre hilos | el HW lo hace atomico | CARRERA en Threaded si CPU y RSP lo usan a la vez (medido 0 usos de CPU con tarea). Hacerlo `std::atomic` con `exchange(1)` |
| 10 | `rcpReg32` 0x14 SP_DMA_FULL | devuelve el bit de DMA_BUSY | DMA_FULL es el bit 3 de SP_STATUS (hay cola); baja antes que BUSY | detalle de fidelidad, sin coste. Revisar cuando se toque el DMA |
| 11 | DPC START/END doble buffer (`memory.cpp` 1165-1250) | modela START_PENDING (0x400) | coincide | bien |

Nada del HW obliga a la CPU a esperar al RSP fuera de BREAK/MI_SP, SIG, semaforo y DMA. Nada
obliga al RSP a esperar a la CPU fuera de SIG, semaforo y DMA desde RDRAM. Las citas 1 a 5
son artefactos de que el estado DPC y SP_STATUS tenga como unico dueno al hilo de CPU.

Arreglo comun de 2 a 5: el RSP guarda sus propias escrituras pendientes (DPC y SP_STATUS)
y, al leer, las superpone al valor aplicado. La CPU sigue aplicando el diario a su hora, y
el RSP no espera nunca por lo suyo.
