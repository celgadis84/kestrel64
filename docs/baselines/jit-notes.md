
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
