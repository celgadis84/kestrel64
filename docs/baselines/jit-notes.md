
## ADDI absorbida: corrupcion del contexto de excepcion (ABIERTO)

`emitTrapAlu` absorbe ADDI/ADD/SUB emitiendo la aritmetica nativa con un `jo` de salida
(el OF de x86 tras un add/sub de 32 bits ES el desbordamiento con signo de MIPS). Con ADD/SUB
solo (`KESTREL_JIT_NOTRAPALU=3`, el defecto) systemtest pasa. Absorber ADDI tambien lo rompe.

Lo medido, para no repetirlo:

- Falla en **"Privilege: memory accesses"** -> tormenta de excepciones. Con `BRSEL=2`
  (BLEZ/BGTZ/REGIMM) falla distinto: 4/3721 en `cart-writing: Temp value decay`.
- **Solo con saltos absorbidos**: `KESTREL_JIT_NOBRANCH=1` pasa. Bisecado con
  `KESTREL_JIT_BRSEL` (mascara: 1 BEQ/BNE, 2 BLEZ/BGTZ/REGIMM, 4 likely, 8 J/JAL, 16 JR/JALR):
  fallan **2** y **8**; 1, 4 y 16 pasan.
- **Un solo bloque basta**. `KESTREL_TRAPALU_LIM=N` corta la absorcion a las N primeras;
  la biseccion binaria dio N=1, o sea la PRIMERA ADDI absorbida ya rompe. Es
  `phys=001709a0` (`addi sp,sp,-344` + 30 `sd`), el preambulo de excepcion de libultra.
- **La ADDI es correcta**: con el modo 7 el bloque queda en una sola op y el jitdiff (ya sin
  el fallo de arnes, ver abajo) no encuentra ni un desacuerdo.
- **La trampa es inocente**: el modo 6 (mismo conjunto, sin levantar desbordamiento) falla
  igual, y una sonda en el driver mostro que el bail **no llega a saltar nunca**.
- **No es enlace ni camino rapido**: `NOLINK=1` y `NOFAST=1` fallan igual. Tampoco RegCache
  (`NOREGCACHE=1`) ni SMC (`NOSMC=1`).
- La entrada al bloque es limpia: `pc=ffffffff801709a0 inDelay=0 justBr=0 R=1/1 ctrl=0`.
- Firma de la corrupcion (`KESTREL_JIT_PCCHK=1`, que ahora mira el pc en CADA despacho):
  `pc=0000008c8017551c` tomado de `$16`, y media docena de registros con **dos palabras de
  32 bits pegadas** -- `$31=807fee54801aa7cc` = [puntero de pila | ra],
  `$2=801a9468801a93ec` = dos direcciones consecutivas. Eso es un contexto de excepcion
  guardado y restaurado con desfase, no una ALU mal emitida.

Siguiente hilo: el bloque de 34 `sd` en `001709a4` ya existia antes; lo nuevo es que el
bloque empiece una instruccion antes. Mirar que cambia para el que SALTA ahi cuando el
destino pasa a tener bloque propio.

## Arreglados de camino

- **jitdiff corria el bloque sobre un `u64[32]` local.** El codigo emitido recibe en rbx la
  direccion que se le pasa y direcciona HI/LO/pc como campos del CPU a partir de ahi, asi que
  un MFLO leia ~280 bytes mas alla del array: pila del anfitrion. De ahi el
  `jit=ffffffff807ffc58` constante que acusaba al JIT de un fallo del arnes. Ahora corre sobre
  los registros reales con guardado/restaurado completo, y el diff sale limpio.
- **`R==0` pisaba el estado de flujo.** Un bloque que sale sin retirar nada (guardia del
  prologo no pasada, o bail limpio en la op 0) no ha hecho nada, pero el driver ponia igual
  `nextPc = pc+4`, `inDelay=false`, `justBranched=false`. Entrar a ese bloque desde una ranura
  de retardo perdia el salto entero. Ahora solo se avanza el flujo si `R>0`.
- **ckseg0 sin comprobar privilegio.** El despacho atajaba `phys = pc & 0x1FFFFFFF` para
  `0xFFFFFFFF8xxxxxxx` sin mirar el modo: en usuario o supervisor esa direccion no esta
  traducida y el VR4300 levanta AdEL en el fetch. Ahora se declina al interprete salvo en modo
  kernel (KSU==0, o EXL, o ERL), que es quien vectoriza la excepcion.

## Camino rapido de memoria dentro del bloque (LW / SW / LWC1 / SWC1)

El perfil del anfitrion en SM64 con `threaded-jit` decia esto, y no era una sospecha:

```
kestrel_jitLW  30.65%   jitTryBlock 19.89%   jitProceedTramp 8.60%
kestrel_jitSW   5.91%   jitLWC1      4.84%   jitSWC1         4.30%
```

Es decir: casi la mitad de las muestras se van en cuatro helpers que, en el caso comun,
hacen siempre lo mismo -- direccion en ckseg0, alineada, modo kernel, dentro de RDRAM, linea
de D-cache presente. Ese caso comun ahora se emite **dentro del bloque**; lo que no lo cumpla
cae al helper de siempre, que sigue siendo la semantica exacta y el oraculo.

Las comprobaciones emitidas son **suficientes, no necesarias**. Ninguna intenta reproducir la
traduccion general: cada una es un `jcc` que, si duda, se va al helper.

- `rax = a + 0x80000000; cmp rax, 0x20000000; jae helper`. Una sola comparacion sin signo
  resuelve tres cosas a la vez: que el segmento es ckseg0, que la direccion de 64 bits estaba
  **canonicamente** extendida en signo, y -- de propina -- `rax` ya ES la fisica
  (`a & 0x1FFFFFFF`). ckseg0 implica cacheable, y en modo kernel `reXor()` es la identidad.
- `test al,3` -- desalineada al helper (el interprete levanta AdEL con su BadVAddr).
- `test byte [status], 0x98` -- exige KSU==0 y KX==0. Es la condicion suficiente de
  "kernel de 32 bits"; cualquier otra cosa va a la traduccion general.
- `cmp eax, [cpu->jitRdramSz]; jae helper` -- fuera de RDRAM es MMIO. El campo se copia al
  CPU en la compilacion de bloque para no perseguir `mem->rdram.size()` (puntero + vector) en
  tiempo de ejecucion. Vale 0 sin bus atado, y ese 0 hace fallar el rango: es el
  `if(!mem) return 0` del helper, gratis.
- Solo stores: `mi_repeat_on` armado (difusion de escritura de MI) y `dcDbgOn` armado
  (punto de vigilancia / write-through de diagnostico) mandan al helper. El camino rapido no
  puede tragarse una escritura que el depurador espera ver.
- Solo COP1: `Status` bit29 (CU1) claro va al helper, que levanta Coprocessor Unusable con su
  CE. Y **bit26 (FR)**: con FR=1 un acceso de 32 bits es la mitad baja de `fpr[rt]`, igual que
  con FR=0 si `rt` es PAR. El unico caso distinto es impar en modo mitad, que va a la mitad
  ALTA del companero par -- ese se declina, y como la paridad de `rt` se sabe al compilar, la
  comprobacion de FR **solo se emite para registros impares**.

Luego la linea de D-cache a mano: indice `(phys>>4)&0x1ff`, `idx*24` como
`lea rcx,[rcx+rcx*2]` + `shl rcx,3`, comparar `ptag`, mirar `valid`. Fallo o linea invalida =
helper, que es quien sabe rellenarla. El dato vive big-endian dentro de la linea, de ahi el
`bswap`.

### El bug que costo el rato

El primer SW emitido pasaba systemtest en LW pero lo rompia entero con
`Got unhandled exception` desde `StartupTest`. Bisecado con la mascara
`KESTREL_JIT_NOFASTMEM` (1=LW 2=SW 8=LWC1 16=SWC1, y **4 = hacer todas las comprobaciones y
irse igual al helper sin escribir**): solo-LW PASS, solo-SW FAIL, SW-solo-comprobaciones
PASS. O sea, las guardias estaban bien y el fallo estaba en la escritura.

Era el orden: la marca de `dirty` se emitia con `rcx` ya desplazado por el offset intra-linea,
asi que el `1` aterrizaba **dentro de `data[]`** y corrompia en silencio la palabra recien
escrita. Ahora se marca con `rcx` todavia en la base de la linea, antes de sumar el offset.
Ese bit 4 de la mascara se queda: separar "una guardia deja pasar algo" de "la escritura esta
mal" es lo que convirtio el fallo en una tarde y no en una semana.

### Lo que dio

Modesto, y era de esperar: el cuello de este emulador es el RCP, no la CPU.

| | antes | LW/SW/COP1-32 | los 15 opcodes |
|---|---|---|---|
| `jit` lockstep (SM64, 200 flips) | 64.1% de tiempo real | 65.0% | - |
| `threaded-jit` | 450.3% | 456.8% | **461.4%** |
| systemtest[jit] | 40 s | 27 s | 24 s |

`kestrel_jitLW` desaparece del top del perfil, pero no porque el trabajo se evapore: se
mudo al codigo emitido, que `hostprof.py` no sabe simbolizar. Se queda por la regla de
que todo suma.

### Extension al resto de anchos (LB/LBU/LH/LHU/LWU/LD/SB/SH/SD/LDC1/SDC1)

Una vez verde el de 32 bits, el mismo esqueleto cubre los quince opcodes de memoria sin
guardias nuevas, porque dentro de RDRAM las rutas que faltaban no pueden dispararse:

- `storeCart` exige `isCart(phys)` y `wordStoreQuirk` exige DMEM/SP o PIF_RAM. Ninguna de las
  dos regiones solapa RDRAM, asi que SB/SH/SD las saltan por construccion.
- La reserva **RI** de LWU/LD/SD (no-kernel sin UX/SX) solo muerde fuera de kernel, y la
  guardia `Status & 0x98` ya exige KSU==0.
- `reXor()` -- el swizzle de endianness inverso -- devuelve la direccion tal cual salvo en
  modo **Usuario** con RE puesto. Misma guardia, misma conclusion: `pe == phys` para todo ancho.
- Un acceso de 8 bytes esta alineado a 8, asi que cae entero dentro de su linea de 16.

Lo unico con trabajo propio es el orden de bytes, que en la linea es el del guest:

| ancho | carga | store |
|---|---|---|
| 1 | `movzx` (y `movsx` para LB) | `mov byte` tal cual |
| 2 | `movzx16` + `bswap32` + `shr 16` (+`movsx` para LH) | `shl 16` + `bswap32` + `mov word` |
| 4 | `mov r32` + `bswap32` (+`movsxd` para LW) | `bswap32` + `mov dword` |
| 8 | `mov r64` + `bswap64` | `bswap64` + `mov qword` |

LDC1/SDC1 heredan la regla de FR de sus hermanos de 32 bits: `fprGet64`/`fprSet64` con FR=0
usan `fpr[rt & ~1]`, que para `rt` PAR es el mismo registro que con FR=1 -- solo el impar se
declina al helper, y la paridad se conoce al compilar.

## BC1F / BC1T / BC1FL / BC1TL absorbidos

Tras el camino rapido de memoria, el perfil del anfitrion puso `jitTryBlock` en el 36% de las
muestras: el coste ya no estaba en ejecutar el bloque sino en **entrar y salir** de el. Con
33.5M despachos para 210M instrucciones, cada salida al interprete se paga entera en volcado
de residencia + retorno + busqueda del bloque siguiente.

El histograma de "el lider del bloque no compila" (`g_compFailOp`) senalaba OP11 = COP1 con
2.1M, pero el opcode primario no basta: bajo OP11 conviven MFC1/CFC1 con toda la aritmetica de
coma flotante, que **ya** se compila. El sub-histograma por el campo `rs` (`g_compFailCop`)
dejo el culpable en una linea: `COP1.rs08 = 2.1M`, o sea BC1x, el **100%** del compile-fail de
COP1.

Es un salto que ya se sabia emitir. Compartir comparte casi todo con BEQ:

- el destino es la misma cuenta, `pc + (SIMM << 2)` (cpu.cpp:1310 y cpu.cpp:2261 son identicas);
- la variante *likely* sigue la regla de siempre (delay slot anulado si no se toma);
- lo unico distinto es de donde sale la condicion: no de comparar dos GPR sino del bit **COND**
  (23) de FCR31, contra `TF` = bit 0 de `rt`.

Como `TF` se conoce al compilar, la condicion sale en dos instrucciones sin ramas:

```
test byte [rbx + fcr31 + 2], 0x80    ; COND = bit 23 -> byte 2, bit 7
setne/sete al                        ; setne si TF=1, sete si TF=0
```

y a partir de ahi entra por el mismo camino que BEQ (condicion en `[rsp+32]`), sin tocar las
fases B/C. **CU1** claro se declina con un bail normal: el interprete re-ejecuta el BC1 y
levanta el Coprocessor Unusable con su `CE=1` exacto, que es la unica forma de no inventarse la
excepcion.

### Un rollback que dejaba basura

La maquinaria de salto se echa atras (`buf.used = beforeBranch`) cuando el delay slot no
compila. Hasta ahora ninguna fase A registraba sitios de bail, asi que descartar el codigo
bastaba. BC1 si registra uno, y un rollback dejaba ese `bailSite` apuntando a una direccion
que la siguiente op iba a reescribir: el parcheo posterior habria escrito un rel32 en mitad de
otra instruccion. Los tres vectores (`bailSites`/`bailIdx`/`bailSnap`) se recortan ahora junto
con el buffer, en `rollbackBranch()`.

### Resultado

| | despachos | cobertura | `compile` | `ctrl` | bench threaded-jit |
|---|---|---|---|---|---|
| antes | 33.5M | 745.4% | 6.89M | 3.61M | 461.4% |
| BC1 | **25.2M** | **836.5%** | **4.46M** | **1.45M** | **471.1%** |

`ctrl` cae a menos de la mitad sin haberlo tocado: un BC1 no compilado no solo devolvia el
lider al interprete, tambien su delay slot y el salto entero. OP11 desaparece del compile-fail.

Se bisecta con el bit **32** de `KESTREL_JIT_BRSEL` (apagarlo devuelve BC1 al interprete).

El siguiente de la lista es COP0: `MFC0` 1.30M + `MTC0` 1.09M, bloqueado por otra cosa -- el
valor de `Count` a mitad de bloque, que no esta puesto al dia hasta que el bloque sale.
