# El reloj del VI (un solo reloj para todo el tiempo del guest)

Estado: implementado 2026-08-20. Toca `Clocks` (`src/core/system.hpp`), el bucle de
`System::run`, `Memory::viTick` y la lectura de `VI_V_CURRENT`.

## Qué había mal

Había **dos** relojes de video que no se hablaban:

1. La lectura de `VI_V_CURRENT` derivaba la halflínea de `93'750'000 / (60 * total)`
   instrucciones — un 60 Hz clavado y el reloj de CPU crudo.
2. `viTick()` avanzaba `vi_current` **+2 halflíneas por llamada**, y el bucle del sistema
   lo llamaba **una vez por lote de un campo entero**. Un campo real de 524 halflíneas
   necesitaba 262 llamadas, o sea 262 campos de tiempo emulado.

Consecuencias medidas:

- `viFields` contaba **2** campos en 421M instrucciones (deberían ser ~538).
- `vrdp::frameBegin()` cuelga del cierre de campo, así que el contexto por-cuadro de
  parallel-rdp casi nunca rotaba → los WARN de "Exhausted LinkedDeviceHost memory".
- La interrupción del VI se levantaba con la regla de NIVEL (`vi_current >= vi_intr`),
  que con ticks de sub-campo dispara en casi todas las llamadas.
- El bench medía "tiempo real" con intercambios de buffer / 60. SM64 en atracción no va a
  60 fps, así que el número salía ~3x por debajo de la verdad.

## Modelo

Un solo sitio define el tiempo: `Clocks`.

```
insnTarget() = cpuTarget() / cyclesPerInsn      // 93.75 MHz / 2 = 46.875 M ops/s
fieldInsns() = insnTarget() / viFieldHz         // /59.94 = ~782_000 ops por campo
tickInsns()  = fieldInsns() / viTicksPerField   // 16 ticks por campo (KESTREL_VITICKS)
```

`cyclesPerInsn = 2.0` sale de COP0 `Count`: el HW lo incrementa a la mitad del reloj de
CPU y nosotros lo incrementamos 1 por instrucción retirada, así que 1 op = 2 ciclos por
construcción. **Simplificación conocida**: un VR4300 real retira ~1.0–1.3M ops por campo
(CPI ~1.5), no 782k, así que le damos al guest un 25–35% menos de trabajo por campo que
el HW. Modelar el CPI medio de verdad es el siguiente paso de precisión, y hay que
hacerlo aquí, no en dos sitios.

El bucle del sistema corre `tickInsns()` ops y llama a `viTick(cpu.retired)`. Ticks de
sub-campo hacen falta porque hay ROMs que reprograman `VI_INTR` *dentro* del campo; con
un tick por campo esas reprogramaciones se pierden.

## viTick: cruce, no nivel

`viTick` recibe el contador absoluto de instrucciones retiradas y deriva todo de él:

- `vi_current = (retired % field) * total / field`, par (el bit 0 es el campo par/impar).
- La interrupción del VI se levanta cuando el intervalo `(viLastRetired, retiredNow]`
  **cruza** la línea `VI_INTR`, no cuando `vi_current` está por encima. Con ticks de
  sub-campo la regla de nivel dispararía en cada tick.
- Cierre de campo = cruzar un múltiplo de `fieldInsns()`. Ahí y solo ahí se incrementa
  `viFields`, se llama a `vrdp::frameBegin()` y se tickea el AI.

`viFields` es el reloj de tiempo del guest: N campos = N/59.94 segundos de video. El
bench mide con eso, no con intercambios de buffer.

## Lo que se probó y NO se quedó: latchear VI_ORIGIN

El HW latchea `VI_ORIGIN` al empezar el campo, así que una escritura en el vblank se ve
en el campo SIGUIENTE. Se implementó (`vi_origin_disp`) contando el intercambio en el
latch en vez de en la escritura, y se revirtió entero. Motivo medido:

- El volcado headless pasa a enseñar el buffer **viejo** — que un juego de doble buffer
  ya está reescribiendo. En modo threaded lo que se haya redibujado encima depende del
  reloj de pared, y los cinco modos dejaron de coincidir en SM64: tres md5 distintos
  (`925a85dc…` interp/jit/jit-nolink, `466282775…` threaded, `71b37446…` threaded-jit).
- Con "congelado de captura" (dejar de latchear en el campo que para el run) SM64 seguía
  divergiendo y krom regresaba en 12 ROMs (RDPTest/CPU 99.65→54.82, Video/I4LZRLE
  33.28→10.72, …), porque congelar cambia qué campo captura cada ROM.

El punto de captura tiene que ser **el buffer recién publicado, que nadie está tocando**.
Contar el intercambio en la escritura de `VI_ORIGIN` da exactamente eso. Si algún día
hace falta el latch para precisión de presentación, tiene que ser solo para el
presentador/scanout, nunca para el punto de parada del gate.

## Inconsistencia pendiente: el core tiene DOS CPI

Al unificar el reloj de video sale a la luz que el emulador usa dos conversiones
distintas de "instrucción retirada → ciclos de CPU":

| Sitio | Regla | CPI implícito |
|-------|-------|---------------|
| COP0 `Count` / `Clocks::fieldInsns()` / VI | `Count += 1` por op, y `Count` corre a medio reloj | **2.0** |
| Interleave CPU↔RSP en Lockstep (`rspPhase += 2; while(>=3) rsp.step()`) | 2 pasos de RSP por 3 ops de CPU | **1.0** |
| Regulador Threaded (`Memory::rcpPace`) | `opsCPU <= 1.5 * ciclosRSP` | **1.0** (espeja el de arriba) |

Con CPI=1 el reparto CPU:RSP sale de 93.75/62.5 = 1.5 ops por ciclo de RSP; con el CPI=2
que ya usa el reloj de video saldría 0.75. O sea: al RSP le estamos dando **la mitad** del
tiempo relativo que implica nuestro propio modelo de `Count`.

Los dos tienen que salir del mismo `Clocks::cyclesPerInsn`. No se toca en este cambio
porque mover el interleave altera el orden de eventos CPU/RSP (md5 de SM64, suite krom,
tests de timing de systemtest) y además hace la CPU emulada **más lenta** en relación al
RSP, no más rápida: es trabajo de precisión, no de rendimiento, y hay que medirlo aparte.
El CPI de verdad de un VR4300 es ~1.5, así que ninguno de los dos números es el correcto;
lo correcto es un modelo de ciclos por instrucción y un solo sitio del que salga.

## Efecto en la suite krom (re-muestreo, no regresión de render)

El arnés de krom para cada ROM en el primer intercambio de buffer (`KESTREL_MAXFLIPS=1`)
o el primer `SYNC_FULL` (`KESTREL_MAXSYNCS=1`), con parada por imagen estable
(`KESTREL_STABLE=8000000,3`) para las que dibujan por CPU. Arreglar el reloj del VI
cambia CUÁNTO trabajo ha hecho la ROM al llegar ahí, porque antes:

- la interrupción del VI se levantaba por NIVEL en casi cada tick → la ROM recibía
  interrupciones de video a un ritmo absurdo;
- el campo casi nunca cerraba → una ROM que anima por campo se quedaba congelada.

Medido tras el cambio, ya con `--maxsyncs 2` de serie (interp, 371 ROMs):
`mean_exact` 88.83 → **88.71**, `regress=3 improve=2`.

| ROM | antes | ahora | qué es |
|-----|-------|-------|--------|
| Interrupt/VIScrollingBGDMA32BPP | 100.00 | 0.21 | **animación**: scrollea +1 línea por campo moviendo `VI_ORIGIN`. Antes el emulador no avanzaba campos y capturaba el cuadro 0, que es la referencia. Con `--maxflips` 2/3/4 se aleja más (0.11/0.08/0.10): puro desfase de fase, no de render |
| RDP/{16,32}BPP/…/CubeFillTriangle | 59.54 | 53.10 | **animación** (cubo que rota +1°/cuadro); insensible a `--maxsyncs` |
| RDPTest/{CPU,RSP} | 99.65 | 99.65 | **escena a medio dibujar** con el default viejo (88.60): la ROM emite DOS display lists, y un `SYNC_FULL` es el final de UNA. Con `--maxsyncs 2` vuelve a 99.65 exacto → era el punto de captura del arnés, no el render. Por eso el default del arnés pasa a 2 |
| CPUTest/Exceptions/VIIntr/ExceptionVIIntrDisabled | 99.36 | **100.00** | mejora real: la ROM comprueba que con la interrupción de VI enmascarada NO entra al manejador. Con el disparo por nivel entraba igual |
| RDP/RDPModeInput | 6.50 | **59.49** | mejora real: dibujaba a medias porque las interrupciones de vídeo espurias le comían el hilo |

(`RSP/DCT/FastZigZag…16BIT` osciló durante el ajuste; con el default final es
determinista y **sube**: 0.65 con `--maxsyncs 1`, 1.68 con 2, estable a partir de ahí.
Sigue rota (<50%) en ambos casos.)

Ninguna de las tres bajadas es una regresión del RDP: las tres son el mismo patrón ya identificado
en `docs/STATUS.md` (las referencias de krom son un cuadro concreto de una
animación; la precisión real se mide sobre las ROMs estáticas).
