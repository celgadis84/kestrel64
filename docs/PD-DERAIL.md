# Perfect Dark: descarrilamiento tardio (investigacion en curso)

Sintoma: con `KESTREL_PRDP=1` y trabajo fijo (`KESTREL_MAXINSN=8000000000`), PD se
descarrila en un 40-100 % de los arranques entre **retired ~1.9e9 y ~3.1e9**. El emulador
no se cuelga: el *guest* empieza a tomar excepciones. Reproduccion con
`scripts/`-externo (ver "banco de pruebas" abajo).

## Lo que YA esta descartado (medido, no razonado)

| Hipotesis | Prueba | Veredicto |
|---|---|---|
| Enlace de bloques del JIT | `KESTREL_JIT_NOLINK=1 KESTREL_JIT_NOITC=1`, 4 arranques | **4/4 fallan** -> descartado |
| Cache de destinos indirectos (ITC) | idem | descartado |
| Cuelgue del RCP / deadlock | el watchdog ve `retired` subiendo siempre | descartado |
| Fallo de host primario | el `AdES` del guest llega ANTES; el `0xc0000005` del anfitrion aparece tras ~187 M de AdES seguidos | secundario |

## Firma del primer fallo

Siempre es una **excepcion del guest** con direccion de store basura, o una `RI` sobre una
palabra que no es codigo valido. Ejemplos reales:

```
AdES(5) epc=0x7000abf0 badv=0x000000ce cause=0x80000014 ra=0x7000abf8 sp=0x803ff7c0 ret=1947993326
AdES(5) epc=0x7000db94 badv=0x00001007 cause=0x00000014 ra=0x7f069170 sp=0x803ff7a8 ret=2275825884
AdES(5) epc=0x700164a0 badv=0x7f07dcf4 cause=0x00000014 ra=0x7f07dcf8 sp=0x803ff760 ret=2749830474
RI(10)  epc=0x7004b498 badv=0                          ra=0x700497c4 sp=0x80090170 ret=2028640325
```

Lectura: un **puntero del guest esta podrido** (`0xce`, `0x1007`) o la palabra en `epc` no
es una instruccion. `sp` sigue siendo un KSEG0 valido, o sea que la pila no se ha perdido:
es un dato concreto el que se corrompe, no el marco entero. `cause` con bit 31 en un caso =
el store estaba en delay slot.

## Trampas del instrumental (corregidas)

- **`CPU::translate()` empieza con `if(memAbort) return 0;`.** Dentro del volcado de fallo
  `memAbort` SIEMPRE esta puesto (lo acaba de poner la excepcion que se vectoriza), asi que
  toda sonda `tlbProbePhys` devolvia 0 y las lineas `[fault] phys=0x00000000` mostraban el
  vector de excepciones, no el codigo del `epc`. Ahora el volcado limpia y restaura el
  pestillo. **Todo `phys=0x00000000` anterior a este arreglo es basura.**
- **El `pd.map` del decomp NO sirve como oraculo de simbolos.** El arbol de
  `../perfect_dark` esta en una variante modificada de 50/64 MB (`.lib` con
  `load address 0x03200000`, mas alla de los 32 MB de la ROM retail), y los simbolos no
  casan con `pd.ntsc-final.z64`: direcciones de codigo caen sobre simbolos de datos. Las
  identificaciones de funcion hechas con el (p.ej. "schedRenderCrashPeriodically") **no son
  fiables**. El codigo hay que leerlo de la RDRAM del emulador.
- PD ejecuta desde **kuseg mapeado por TLB en `0x70000000`**; la entrada 0 lo mapea a
  fisico 0, o sea el mismo contenido que KSEG0 `0x80000000`.

## Banco de pruebas

`pdcfg.sh <TAG> <N> [ENV=v ...]` fija el TRABAJO (8e9 instrucciones retiradas), no el
tiempo, y cuenta cuantos arranques ven algun `[fault]`. Reglas: **un solo trabajo de GPU a
la vez** -- una tanda de `base` contaminada por otro emulador vivo fallo 3/4 mientras la
unica ejecucion en solitario salio limpia.

Velocidad de referencia (limpia): 8e9 retiradas en **31 s ~ 258 Mips**.

**GOTCHA de la herramienta de tareas en segundo plano:** matar la tarea NO mata sus
descendientes. El `sh.exe` del lote sigue vivo y sigue relanzando el emulador, asi que la
tanda siguiente corre contra DOS emuladores a la vez y sus numeros no valen. Tras cortar un
lote hay que rematar a mano (`Get-Process sh,kestrel64 | Stop-Process -Force`) y comprobar
que quedan CERO antes de lanzar nada.

## Vuelco: NO es el JIT

Control de interprete a trabajo fijo (`KESTREL_JIT=0`, 8e9): **falla igual**
(`faults=5`, primer fallo en retired=2861591884). La conclusion previa "solo el JIT"
venia de controles que solo llegaban a ~3.2e9 y era falsa. El dynarec queda descartado
como causa.

## La corrupcion de codigo es CONSECUENCIA, no causa

`KESTREL_CODEWATCH=50` sobre 0x8000..0x50000, 3 arranques:

- Arranque 1: el primer bloque de 32 B que cambia lo hace en el MISMO `retired` que el
  primer `[fault]` (2235252848 vs 2235252857). No hay corrupcion previa.
- Arranque 3: los cambios llegan 200 K instrucciones DESPUES del primer fallo y con el
  `pc` ya paseando hacia delante (0x73850cc0 -> 0x738636f4 -> 0x73874a8c -> ...), o sea
  con el guest ya descarrilado.
- **Cero `[dmaguard]`**: el DMA del SP no escribe en el rango de codigo.

## Lo que falla primero es un DATO del monton (heap)

Volcado con ventana de codigo ya arreglada, arranque `cw` 2:

```
[fault] AdEL(4) epc=0x7002c840 badv=0xffffffff804af583 ra=0x7002d504 sp=0x803ff2b0 ret=2351778642
[fault]   epc-16 phys=0x0002c830: 450300cb 8fa90150 920d0009 01b07021 85cf0000 448f3000 ...
[fault]   badvPhys=0x004af580 w=0111012c
```

Desensamblado:

```
0x2c834  lw   t1, 0x150(sp)
0x2c838  lbu  t5, 9(s0)        ; indice de UN BYTE dentro de la estructura
0x2c83c  addu t6, t5, s0
0x2c840  lh   t7, 0(t6)        ; <-- EPC, badv impar => el indice era IMPAR
0x2c844  mtc1 t7, f6
```

`s0 = 0x804af580` (alineado y con contenido plausible); lo podrido es el **byte
`[s0+9]`**, un indice que el juego usa para leer un halfword. O sea: una estructura del
monton en ~4.9 MB (zona de Expansion Pak) tiene un byte mal. Todo lo demas -- pila,
codigo, `pc` -- sigue sano en ese instante.

## Siguiente

1. Control de **interprete a mismo trabajo fijo** (8e9). Los controles previos solo llegaron
   a ~3.2e9 por `MAXFLIPS`, apenas pasado el punto de fallo: la conclusion "solo el JIT" NO
   esta probada.
2. **JIT en lockstep** (`KESTREL_THREADS=0`): separa un fallo de codegen de una carrera
   CPU<->RCP. La no-determinacion del fallo apunta a lo segundo.
3. `KESTREL_CODEWATCH=100` (rango por defecto = fisico 0x8000..0x50000, el codigo de PD)
   para ver si alguien pisa el codigo y con que contexto.
4. Si sigue vivo: bisecar `KESTREL_JIT_NOFASTMEM`, `_NOREGCACHE`, `_BRSEL`, `_NOTRAPALU`,
   `_NOALU64`, `_NOCOP0`, `_NOXPAGE`, `_NOSMC`, y `KESTREL_FPORACLE=1`.

Nota: `KESTREL_JIT_DIFF` y `KESTREL_JIT_BRDIFF` **no cubren bloques con memoria** (correr
un load/store dos veces duplicaria efectos), que es justo la familia sospechosa. Si hace
falta, hay que construir un diff de memoria que corra el interprete PRIMERO y compare
direcciones/valores de store.

## Vuelco 2: tampoco es parallel-rdp

`soft` (SoftRDP, threaded-jit) parecia limpio 3/3, pero corria a 4e9 ops. Repetido a las
mismas 8e9 del banco:

| tanda | RDP | hilos | trabajo | malas |
|---|---|---|---|---|
| `base` | parallel-rdp | threaded | 8e9 | 3/4 |
| `nolink` | parallel-rdp | threaded (sin enlace ni ITC) | 8e9 | 4/4 |
| `interp8` | parallel-rdp | threaded, `KESTREL_JIT=0` | 8e9 | 2/3 |
| `soft8` | **SoftRDP** | threaded | 8e9 | **3/3** |
| `nothread` | parallel-rdp | **lockstep** | ~3,7e9 | 0/3 |
| `syncrdp` | SoftRDP | threaded + `KESTREL_SYNCRDP` | 8e9 | (en curso) |

El backend de RDP es indiferente. Lo unico que separa limpio de sucio es la
CONCURRENCIA CPU<->RCP.

## Que hay realmente en el codigo cuando descarrila

Con la ventana de 16 palabras (`epc-48`, `>` marca el EPC) el sintoma se ve entero.
`c_soft8_3.log`, fisica 0x00009a74:

```
03e00008 27bd0028   <- jr ra / addiu sp,sp,0x28: fin de funcion, codigo SANO
04030419 05bb0519 04030519 06bb0519 03010519 06bb0519 04030519 07bc0519
04030519 0c970519 >64610619 0c6c0919 07701119 250a0419
```

Justo detras del epilogo de una funcion hay DATOS: estructuras de 4 bytes cuyo ultimo
byte es constante (0x19 aqui, 0x03..0x05 en otro volcado). Lo mismo en `c_soft8_1.log`
en 0x00048728, donde las 16 palabras acaban todas en 0x19. O sea: alguien escribe un
array de estructuras encima del codigo del guest, y la CPU acaba ejecutandolo.

No siempre: en `c_soft8_2.log` (fisica 0x0000bcf8) el codigo esta intacto -- accesores
`lh`/`sw` perfectamente formados -- y lo podrido es `ra` (0x7f16c87c). Hay pues dos caras
del mismo destrozo: memoria de codigo y memoria de pila/heap.

## Instrumental anadido en esta tanda

- `[fault]` vuelca ahora **las entradas validas de la TLB**, marcando con `*` la que cubre
  el EPC. PD ejecuta desde kuseg 0x70000000: si la traduccion apuntase a una pagina fisica
  equivocada se veria codigo ajeno sin que nadie haya escrito nada.
- `KESTREL_SYNCRDP=1` / `KESTREL_SYNCRSP=1`: dejan el RCP en modo threaded pero obligan a
  la CPU a esperar al worker correspondiente. Biseccion de cual de los dos hilos destroza.
- `KESTREL_PRDP_SYNCALL=1`: fence de GPU tras CADA primitiva (solo backend parallel-rdp).
- `KESTREL_RDPGUARD=<lo>:<hi>`: chiva cualquier escritura de SoftRDP a RDRAM en ese rango.
- `watchHit` cubre ahora tambien **el volcado de lineas sucias de la D-cache** (sin eso
  `KESTREL_WATCH` no veia ningun store cacheado, o sea casi ninguno) y **PI DMA**.

## Vuelco 3: DPC_CURRENT mentia sobre los tramos encolados (arreglado)

Con `KESTREL_RDPGUARD` + `KESTREL_CIFLOOR` se cazo al escritor: el RDP rasterizaba
dentro del codigo del guest.

```
[rdpguard] write32 phys=0x0104d0 v=0x88980219      <- pixel RGBA5551 sobre codigo
[rdp!] SET_COLOR_IMAGE bajo: addr=055540 cmd=fffeeec000055540 fifo=000c3840
[rdp!]   dpc start=0c2840 end=0c37a8 current=0c3840
[rdp!]   ...se escribieron 97727 pixeles con el CI bajo
```

El comando malo cae SIEMPRE justo detras del DPC_END vigente y dentro de un tramo cuyo
final es el tope del buffer: el worker seguia consumiendo un tramo de la generacion
anterior mientras el microcodigo ya habia envuelto el anillo y rellenado la base.

Causa: `DPC_CURRENT` es **un** puntero de lectura, el del command processor. Al terminar
un tramo publicabamos su final (tope del buffer) aunque detras quedaran tramos ENCOLADOS
que empiezan en la base. El microcodigo hace control de flujo comparando su puntero de
escritura contra CURRENT: leyendo "tope" da por consumido el anillo entero, envuelve y
reescribe comandos que el RDP aun no ha leido; el rasterizador decodifica a media
instruccion, saca un SET_COLOR_IMAGE con direccion arbitraria y pinta encima del codigo.

Arreglo (`Memory::rdpPublishCurrent`, `src/core/memory.cpp`): al cerrar un tramo se publica
el arranque del siguiente tramo pendiente en vez del final propio. El puntero de lectura
nunca va por delante del trabajo mas viejo sin consumir, que es la semantica del hardware.

Medido en PD, SoftRDP threaded, 8e9 instrucciones: parecio bajar de 3/3 a 1/3, pero
esa medida estaba INSTRUMENTADA (`KESTREL_WRTAG=1`) y el instrumento frena a la CPU
respecto al RDP, o sea ESCONDE la carrera. Repetida sin instrumentar: **3 de 4 malas**.
`rdpPublishCurrent` es semantica de hardware genuina y se queda, pero por si solo NO curaba.

## Instrumento nuevo: `KESTREL_WRTAG=1`

`src/core/wrtag.hpp`. Un tag de "ultimo escritor" por bloque de 16 B de RDRAM
(CPU-uncached / CPU-dcache / SP-DMA / PI-DMA / SI-DMA / RDP) mas el PC del guest que lo
escribio. El volcado de fallo imprime el autor de cada bloque del codigo que se estaba
ejecutando. Ademas mantiene las **ventanas de FIFO sin consumir** (el tramo en curso y la
union de los encolados) y chiva cualquier escritura de CPU/DMA dentro de ellas (`[fifo!]`).

Resultado tras el arreglo: **cero `[fifo!]` y cero `[rdp!]`** en 3 tandas de 8e9 -- ya nadie
pisa el FIFO ni el RDP escribe fuera de sitio.

## Vuelco 4: primera cura -- drenar al instalar buffer nuevo (SUPERADA, ver Vuelco 5)

El FIFO del RDP es UN anillo con UN puntero de lectura. Cuando el juego instala un
buffer de comandos FRESCO (escritura a DPC_START -> START_VALID, luego el DPC_END que
lo lanza), nuestra implementacion enhebrada dejaba VIVOS tramos del buffer ANTERIOR
por detras de la recarga. El worker acababa rasterizando comandos de memoria que el
microcodigo ya habia reescrito, se desincronizaba a media instruccion y decodificaba
el `SET_COLOR_IMAGE` basura de arriba.

En la maquina real eso no puede pasar: o el productor no instala el buffer nuevo hasta
que el RDP ha drenado el viejo (su control de flujo mira DPC_CURRENT), o la recarga de
CURRENT se lleva por delante lo que quedara. Lo unico imposible es que sobreviva trabajo
del buffer viejo DETRAS de la recarga.

Arreglo (`src/core/memory.cpp`, rama `START_VALID` de la escritura de DPC_END): en modo
enhebrado, un START fresco hace `rdpDrain()` antes de encolar. Solo ahi -- dentro de un
mismo buffer el RDP sigue corriendo en paralelo con CPU y RSP, que era el paralelismo que
merecia la pena conservar. Hoy es opt-in y solo para bisecar: `KESTREL_RDPDRAIN=1`.

Medido en PD (SoftRDP threaded, 8e9 instrucciones, SIN instrumentar):

| arm | malas |
|-----|-------|
| sin drenado | **3 de 4** |
| con drenado (opt-in) | **0 de 3** |
| con drenado por defecto | **0 de 3** |

Se creyo gratis ("solo se espera UNA vez por buffer instalado"). Falso: medido con `bench`
cuesta **13-15 % de pared** (SM64, 200 intercambios: threaded-jit 3,57 -> 4,10 s;
prdp-jit 3,56 -> 4,09 s), porque serializa RSP y RDP. Y ademas ALEJA la fidelidad del
oraculo: campos VI por intercambio 4,05 -> 3,31, cuando el lockstep da 4,09. Por eso se
sustituyo por el Vuelco 5.

Salud de una tanda de 8e9 en este build: pared ~90 s y `[wdog] exc: Int(0)` ~ 69k-78k =
sana; 30-61 s con `Int(0)` ~ 34k-37k = descarrilada.

Puertas con el arreglo dentro: `gate_all` 15/15 verde (376 s, krom regress=0, sm64 md5
sin cambio) y `gate_prdp` 3/3 verde (krom prdp mean_exact 89,26, regress=0).

## Vuelco 5: la cura de verdad -- instantanea del tramo al encolar (arreglado, sin coste)

El drenado curaba el sintoma pagando paralelismo. La pregunta correcta no es "cuando puede
el RDP leer" sino "que bytes tiene derecho a ver". El productor (RSP o CPU) ya habia escrito
el tramo entero ANTES de escribir DPC_END; leerlo en ese instante es un momento de lectura
que el hardware real tambien puede elegir. El hardware nunca para a la CPU en la escritura de
DPC_END, asi que drenar nunca fue fiel.

Arreglo:

- `Memory::rdpSnapshot(current, end)` copia el tramo `[current, end)` de RDRAM a un buffer
  sombra. Se llama desde `rdpSubmit`, **con el mutex cogido y antes de publicar el trabajo**:
  el worker no puede ver el tramo hasta que sus bytes estan a salvo.
- Hay **dos** sombras alternas, `rdpShadow[2]`, indexadas por `rdpGen`. Un START fresco ya no
  drena: hace `rdpGen ^= 1`, para que la copia del buffer nuevo no pise la del viejo que
  todavia no se ha consumido. Dentro de una misma generacion el control de flujo del propio
  juego (DPC_CURRENT) es lo que protege el anillo, exactamente como en la maquina real.
- Cada trabajo lleva su `gen`; el worker lee de `rdpShadow[job.gen]`. En modo `xbus` no hay
  copia: los comandos viven en DMEM y el RSP no la reescribe mientras su tarea corre.
- Se redirige **solo la busqueda de comandos** (`SoftRdp::cmdSrc`, y el puntero que recibe
  `vrdp::runFifo`). Pixeles, texturas y cargas de TLUT siguen leyendo la RDRAM viva, que es
  lo correcto: el RDP los lee cuando los lee.
- En lockstep no hay sombra (`cmdSrc = rdram.data()`), asi que el oraculo determinista no
  cambia ni un byte.

Medido (PD, SoftRDP threaded, 8e9 instrucciones, SIN instrumentar):

| arm | malas | bench SM64 200 intercambios | campos VI por intercambio |
|-----|-------|------------------------------|----------------------------|
| sin nada | 3 de 4 | 3,57 / 3,56 s | 4,05 |
| drenado (Vuelco 4) | 0 de 3 | 4,10 / 4,09 s (**+15 %**) | 3,31 |
| instantanea (Vuelco 5) | **0 de 4** | **3,55 / 3,54 s** | **~4,08-4,15** (oraculo 4,09) |

O sea: cura completa, velocidad intacta y fidelidad temporal MAS cerca del oraculo lockstep
(4,09) que ninguno de los otros dos brazos.

Puertas: `gate_all` ALL OK (krom interp mean_exact 88,71, regress=0, sm64 md5
`466282775dbd0ac084946558a1c30771`) y `gate_prdp` ALL OK (krom prdp mean_exact 89,26,
regress=0, sm64 md5 `b5521b24d8fc280fbf102df22d7d30cb`).

**Aviso metodologico**: instrumentar (`KESTREL_WRTAG=1`) ralentiza la CPU respecto al RDP y
por tanto ESCONDE la carrera. Cualquier A/B de esto se corre sin instrumentar.

## Lo que queda

El fallo residual que se vio antes de hacer el drenado por defecto (puntero nulo del
guest, `TLBS badv=0x000000c8` con MIPS sano `sw $s0,0xc8($t0)` y `$t0 = 0`) era el patron
de "interrupcion DP de mas": el kernel atiende una tarea que no existe. El drenado lo
elimina tambien, porque los SYNC_FULL sobrantes que la cola retiraba venian justo del
buffer abandonado. No ha vuelto a aparecer en 6 tandas de 8e9.
