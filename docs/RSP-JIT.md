# RSP dynarec (`src/rsp/rspjit.{hpp,cpp}`)

Straight-line block compiler for the **scalar** half of the RSP core. Toggle
`KESTREL_RSPJIT` (reads a value: `=0` turns it off). Oracle = the RSP
interpreter itself: same framebuffer md5 with the dynarec on and off, in every
mode of `gate_all.sh`.

## Why the RSP is an easy target

The RSP is not the VR4300. It has **no TLB, no exceptions, no interrupts inside
a task, and 4 KB of IMEM**. That kills nearly everything that makes the CPU
dynarec hard:

- A block can never fault mid-way, so compiled blocks return `void` — there is
  no "which instruction did we die on" bookkeeping.
- 4 KB of IMEM = 1024 instruction slots, so the block table is a **flat
  1024-entry array indexed by `pc >> 2`**. No hash map, no chaining.
- The only address space is DMEM (4 KB, wraps at `& 0xfff`), so loads/stores
  need no translation.

## What gets compiled

Only the straightforward scalar ALU/shift work is emitted as native x86-64.
Everything else — COP2 (the whole VU), loads, stores, branches, jumps, COP0
(MMIO on SP/DP registers) — is a `call` back into the interpreter's own
helper (`execCop2` / `execLoad` / `execStore` / `exec`). That is deliberate:
the helpers are the oracle, so a compiled block **cannot** diverge from the
interpreter on the hard cases; the dynarec only removes decode+dispatch cost.

A block ends at the first branch/jump (they are never absorbed — that is
Stage 2), at `kMaxOps = 64` instructions, or at the end of IMEM. Runs shorter
than `kMinOps = 3` are not worth a call and stay interpreted.

Emitted prologue (Win64 ABI, `RCX` = `Rsp*`, `RDX` = opcode for helper calls):

```
push rbx ; push rdi
mov  rbx, rcx            ; rbx = Rsp*
mov  rdi, [rbx + dmpOff] ; rdi = DMEM base, hot enough to pin
sub  rsp, 40             ; shadow space + 16-byte alignment
```

Code lands in a 4 MB RWX slab; when it fills, the cache is cleared wholesale.

## Invalidation — content, not range

IMEM is not read-only: the CPU DMAs new microcode into it constantly, and a
task boundary can reuse the same addresses for different code. Everything hangs
off one idea: **what invalidates a block is the bytes changing, not the DMA
touching them**. Each `rspjit::Cache` keeps a 4 KB `shadow` of the IMEM it was
compiled from, and `Cache::syncImem()` diffs the live IMEM against it in 8-byte
chunks. F3DEX2 reloads its whole microcode at the start of every task, byte for
byte identical to the previous one: against the shadow that reload costs 512
comparisons and invalidates nothing. Only the overlays, which really do bring
different bytes, pay recompilation — and only for the slots they cover.

Invalidation inside a table is exact: a prefix sum over the dirty-chunk map
kills a slot only if the block living there actually spans a changed word
(blocks never wrap IMEM, so a block occupies `[idx, idx+nOps)`). The earlier
"kill the `kMaxOps-1` slots before each dirty chunk, just in case" version
swept half the table for a handful of scattered chunks: 72 blocks recompiled
per invalidation event in SM64.

### Microcode image cache (N ways)

The host has no reason to inherit the N64's single 4 KB IMEM. A game that
alternates graphics and audio microcode overwrites all of IMEM twice per frame,
and with one table that means recompiling everything twice per frame — measured
in SM64: 21282 whole-table flushes and 1.28 M blocks compiled per 600 buffer
swaps, with **~52 % of the RSP thread sitting inside the compiler** rather than
executing.

So the RSP keeps `kJitWays` complete tables (`Rsp::jcWay[]`, one 2 MB code
buffer plus its own shadow each, `KESTREL_RSPJIT_WAYS`, default 4), and
`Rsp::jitSelectImage()` picks one when IMEM changes:

- **By similarity, not equality.** Measured in SM64: 2000 IMEM loads produce
  ~1400 *distinct* 4 KB images, because each microcode leaves behind a
  different tail of the previous one (the audio microcode is shorter than the
  graphics one) and because a task boot DMAs in pieces, so intermediate states
  are visible too. Demanding an exact match makes the cache a mill — almost
  everything misses. Picking the table that differs in the fewest 8-byte chunks
  and patching it means returning to a known microcode only recompiles what
  actually changed.
- An exact match (zero differing chunks) is just a pointer swap.
- A large difference (`kJitNewWay`, 64 chunks) claims a free way if there is
  one; with every way in use, the closest one is patched. A whole table is
  never thrown away on a task switch — `clear()` only recycles a code buffer
  that filled up.

Where the switch happens matters: the task's boot stub DMAs its own microcode
**while the core is running, from the RSP thread**, and that DMA is requested
through COP0, which never runs inside a compiled block. So switching the active
table there is as safe as in `start()`, and it is where the task change really
happens; invalidating blindly would throw away the previous task's table right
before it could be recognised. A DMA arriving from any other thread falls back
to `jc->syncImem()`, and one arriving with the core halted is left for
`start()`.

Two traps worth remembering, both of which cost a debugging session:

- `jitInvalidate` **must not call `bindMem()`**. The microcode DMA usually
  lands *before* the first write to `SP_STATUS`, which is where `Memory` hands
  this `Rsp` its `mem` pointer — dereferencing it earlier is a null read
  (`0xc0000005` at host address `0x18`). The caller already has the SP memory
  in hand, so it passes the pointer in.
- The dispatch site copies the `Block` out of the table **by value**. The CPU
  thread can invalidate the slot from a DMA while the RSP thread is reading it,
  and loading `fn` and `nOps` separately from the live table can yield a valid
  `fn` with `nOps == 0` — a zero-length `pc` advance, i.e. an infinite loop.

## Measured

SM64, threaded-jit, `KESTREL_RSPJIT_STATS=1`. Coverage is **98.4 %** of
executed RSP instructions running from compiled blocks (Stage 2 absorbed
branches and delay slots; what is left is the handful of ops the compiler
refuses).

Compiled blocks per 60 buffer swaps, as a function of how many microcode images
the cache can hold:

| ways | blocks compiled |
|---|---|
| 1 (single table) | 55200 |
| 2 | 12027 |
| 4 (default) | 11514 |
| 8 / 16 | 11514 (only 4 ways ever get claimed) |

Two ways already capture the graphics/audio alternation; the rest is overlay
churn, which is real work. End to end, `bench` over 600 buffer swaps with
parallel-rdp: **13.3 s -> 10.1 s (295 % realtime)**.

## Etapa 3: la VU en linea

Con el compilador fuera del perfil, lo que queda en el hilo del RSP es la VU. Y
buena parte de lo que cuesta una COP2 no es la operacion: es la ABI de Win64. El
thunk especializado (`cop2Thunk<14>`, VMADN) empieza asi:

    subq $0x88, %rsp
    movdqa %xmm10, 0x70(%rsp)   ... y cuatro derrames mas (xmm9..xmm6)

Diez accesos a memoria antes de tocar un dato, mas el CALL/RET y volver a
decodificar un opcode que el compilador ya conoce. Para las operaciones cuyo
cuerpo SSE es corto, el bloque emite ESE MISMO cuerpo en linea, con `vs`/`vt`/
`vd` y el modificador de elemento resueltos como constantes y usando solo
xmm0..xmm5 — volatiles en Win64, o sea cero derrames.

Cubiertas hoy (`vuInline`): VAND/VNAND/VOR/VNOR/VXOR/VNXOR, VSAR, VABS, VADD,
VSUB, VADDC, VSUBC, **la familia MAC entera** — VMULF/VMULU, VMUDL/VMUDM/VMUDN/
VMUDH, VMACF/VMACU y VMADL/VMADM/VMADN/VMADH — y las **comparaciones simples**
VLT/VEQ/VNE/VGE mas VMRG, **las de recorte** VCH/VCL/VCR y **la familia del
reciproco** entera (VRCP/VRCPL/VRCPH, VRSQ/VRSQL/VRSQH, VMOV). Es practicamente
toda la COP2 que ejecuta SM64: por el CALL a la entrada especializada solo siguen
saliendo los movimientos escalar↔vector (sub < 0x10) y los raros sin camino SSE
(VRNDP/VRNDN, VMULQ, VMACQ, VNOP y las reservadas), que juntos no llegan al 0.1 %.

Los productos se arman igual que en `vprodSS/SU/US`: `pmullw` da el limbo bajo,
`pmulhw`/`pmulhuw` el medio y `psraw 15` del medio el alto; la correccion de
signo de las variantes mixtas (VMUDM/VMUDN) es una resta condicional guiada por
`pcmpgtw` contra cero. VMADH acumula sobre `acch:accm` como una suma de 32 bits
partida en dos bandas de 16, con el acarreo de `emitCarry16` — la misma formula
que `vcarry16` en el interprete, `(a&b) | (~s & (a|b))` desplazado 15, sin
comparaciones y sin ensanchar. `emitSatSigned` cierra VMUDH/VMADH rearmando los
pares de 16 bits en enteros de 32 con signo (`punpcklwd`/`punpckhwd`) y bajando
con `packssdw`.

El mismo `vcarry16` sustituyo en el interprete al ensanchado a 32 bits de
`vadd48` (35 -> 9 instrucciones, 3.80 -> 1.57 ns/op, bit a bit identico sobre
2 M de casos al azar).

El emisor reproduce los intrinsecos de `vuOpT` en el mismo orden — no hay una
segunda semantica. Y hay dos oraculos que lo demuestran:

* `--rspjitfuzz N`: monta bloques reales de cuatro operaciones vectoriales al
  azar en IMEM (los 16 modificadores de elemento, registros solapados a
  proposito), los compila, y compara el estado vectorial completo contra
  interpretar las mismas cuatro instrucciones desde el mismo estado. 400 000
  bloques, 0 diferencias. La rotacion de opcodes incluye las de la familia MAC:
  importa que se mezclen de verdad, porque una deja el acumulador escrito y la
  siguiente lo lee — un fallo de acarreo solo asoma con varias seguidas.
* el de siempre: mismo md5 de framebuffer con `KESTREL_RSPJIT=0`, y el modo
  `rspinterp` de `gate_all`.

Medido (SM64, lockstep, 300 intercambios, parallel-rdp): 31.95 s -> 31.80 s. Es
poco, y era de esperar: en modo hilos el RSP tiene holgura (ocupacion 65-78 %,
`cpuWait` ~25 %), asi que ahorrar CPU en ese hilo no sube los fps todavia — baja
el consumo y deja sitio para las operaciones que faltan.

## La familia MAC completa

Las nueve que faltaban se apoyan en tres piezas, y ninguna repite semantica: son
los mismos pasos que `vadd48`, `vprod*` y las saturaciones del interprete.

* `emitDouble48` dobla el producto de 48 bits desplazando uno a la izquierda y
  arrastrando el bit alto de cada rebanada. Sale exactamente lo que da
  `vadd48(p,p)`: el acarreo de cada banda **es** su bit alto, y el segundo acarreo
  de la rebanada media no puede darse nunca, porque el desplazado tiene el bit 0
  a cero. VMULF/VMULU/VMACF/VMACU lo usan; el `+0x8000` de VMUL* va detras con el
  acarreo normal.
* `emitAccAdd48` suma el triple al acumulador rebanada a rebanada con
  `emitCarry16`, dejando el nuevo `acch` en xmm0 y el nuevo `accm` en xmm5 — que es
  justo lo que piden las saturaciones. Con seis registros volatiles y tres
  acarreos vivos el reparto es apretado: cada `emitCarry16` reusa como temporal el
  operando que acaba de morir.
* Las saturaciones "sin signo" (`vsatUnsignedN`, VMULU, VMACU) las escribe el
  interprete con dos `_mm_blendv_epi8`. Con mascaras de todo-unos esa doble mezcla
  es, sin perder un bit, `(x | desborde) & ~subdesborde` — tres operaciones
  logicas. De paso evita `pblendvb`, que usa xmm0 como operando implicito y
  obligaria a mover el reparto de registros.

La estimacion del peor caso por instruccion subio de 200 a 400 bytes: VMACU son
~290 y quedarse corto no corrompe nada (el emisor detecta el desbordamiento y
tira el bloque) pero lo tira **despues** de compilarlo. Se vio en el fuzz: 13
bloques de 400 000 salian sin compilar hasta subir la cifra.

## Comparaciones y VMRG

Tres piezas nuevas y ninguna semantica nueva:

* `emitFlagMask` / `emitFlagStore`: una bandera del RSP guarda 0/1 por banda, y la
  mascara de seleccion es `pcmpgtw` contra cero; la vuelta es `pcmpeqd` + `psrlw 15`
  (el vector de unos de 16 bits) y un `pand`. Son `vmaskFromFlag` y `vflagFromMask`
  del interprete, emitidas.
* `emitSelectST`: `blendv(T, S, cm)` con mascaras de todo-unos es exactamente
  `(S & cm) | (T & ~cm)`. Se evita `pblendvb` a proposito — usa xmm0 como operando
  implicito, y xmm0 es justo donde vive S.
* VABS sale de la misma resta hecha con dos saturaciones distintas: `psubw` para el
  acumulador (T=-32768 deja 0x8000) y `psubsw` para el destino (0x7fff). El caso
  S==0 se limpia con un `pandn` en las dos salidas.

Las cuatro comparaciones comparten cuerpo: `pcmpeqw` para S==T, las mascaras de
VCO.low/high, la formula de cada una (VLT `T>S || (S==T && low && high)`, VEQ
`!high && S==T`, VNE `S!=T || high`, VGE `S>T || (S==T && !(low && high))`), la
seleccion, y despues VCC.low = la mascara y VCC.high + VCO enteros a cero. VMRG es
la misma seleccion leyendo VCC.low, sin escribirla, borrando solo VCO.

El censo (`KESTREL_VUSTAT=1`, SM64, 60 intercambios) dice lo que valen: VLT 0.85 %,
VGE 0.77 %, VMRG 0.94 %, VABS 0.51 %, VEQ/VNE ~0 de todas las instrucciones del RSP;
las de recorte que quedan fuera suman 0.84 % (VCL 0.38, VCH 0.33, VCR 0.13) y la
familia del reciproco 3.84 %. De punta a punta no se ve: bench de 300 intercambios
con parallel-rdp en lockstep da 51.45 s antes y 52.07 s despues, o sea ruido. Es lo
esperado y ya estaba dicho arriba — el ahorro cae en el hilo del RSP, que hoy tiene
holgura.

## El acumulador en registro

Una racha de operaciones MAC recarga y reescribe las mismas tres rebanadas de 16
bytes una y otra vez: `emitAccAdd48` sola son seis accesos por instruccion. xmm6,
xmm7 y xmm8 son callee-saved en Win64, asi que el bloque los salva una vez en el
prologo y a partir de ahi **acch/accm/accl viven en registro**; la memoria se pone
al dia al salir del bloque.

- El bloque decide en el prescan: con **dos o mas COP2 en linea** compensa salvar
  los tres registros. Con una sola, no.
- La cache es minima y explicita — `accValid` (la rebanada esta en el registro) y
  `accDirty` (el registro es mas nuevo que la memoria) — con `accGet`/`accPut`
  sustituyendo a los `ldx`/`stx` sobre `aOff[]`. Nada mas cambia en los emisores.
- **Antes de cada CALL, `accSpill`**: el helper del interprete lee y escribe la copia
  de memoria, asi que hay que volcar lo sucio y ademas *olvidar* lo cacheado — el
  helper pudo escribirlo. Va dentro de `emitCall`, que es por donde pasan todas las
  llamadas del bloque.
- **Trampa: un CALL emitido dentro de una rama.** `emitMem` compila la carga/almacen
  escalar como camino rapido en linea + rama lenta con CALL para la direccion que
  envuelve. El `accSpill` de ese `emitCall` emite los `stx` **solo dentro de la rama
  lenta**, pero apaga `accDirty` en tiempo de compilacion para todo lo que venga
  despues: por el camino rapido — el habitual — el bloque salia con el acumulador vivo
  solo en xmm6..xmm8 y la copia de memoria vieja. Se ve unicamente en los modos
  threaded porque ahi los cortes de presupuesto de la RSP caen en otros sitios, se
  compilan bloques con otras formas y aparece la mezcla VU-sucia + memoria; y el md5
  salia distinto en cada pasada, que es la firma de que la forma del bloque depende del
  reloj. Arreglo: `emitMem` vuelca **antes** de la bifurcacion. Volcar basta (no hace
  falta invalidar): una carga/almacen escalar de la RSP solo toca DMEM, nunca el
  acumulador. Regla general — un `emitCall` bajo condicion exige que el volcado se emita
  fuera de la condicion.
- El hueco de los tres `movdqa` va **detras** del hueco de sombra de la ABI y alineado
  a 16. Como la alineacion de RSP depende de cuantos `push` haya hecho el prologo, el
  tamaño del marco se calcula (32 de sombra si hay CALL) + 48 + 8 si los push fueron
  pares. Y `xmm8` obligo a meter REX en `sse_rr`/`sse_m`, mas un `xmmSpill` con SIB,
  porque RSP como base no se puede codificar sin el.

Medido (SM64, lockstep, 300 intercambios, parallel-rdp): **52.07 s -> 51.19 s**
(-1.7 %; una corrida cada cifra, el ruido de esta medida es de medio segundo). El
fuzz cubre el caso que importa: la rotacion de opcodes incluye a proposito VCL/VCH/VCR,
que NO estan en linea, para que los bloques mezclen CALL con VU en linea y se pruebe el
volcado. 400 000 bloques, 0 diferencias. Lo que el fuzz **no** cubre son los caminos
mixtos con memoria: sus bloques son solo COP2, asi que el fallo de `emitMem` de arriba
paso limpio por el fuzz y lo cazo el md5 de sm64 en threaded. El fuzz es un oraculo de
semantica de la VU, no de gestion de registros a lo largo del bloque.

## Las de recorte (VCH / VCL / VCR)

Son las tres unicas que no caben en seis registros volatiles: VCL sola necesita
hasta ocho mascaras vivas a la vez. El bloque que lleve alguna salva **xmm9..xmm13**
en el prologo (callee-saved en Win64, mismo mecanismo que el acumulador en
xmm6..xmm8, con su hueco propio de 80 bytes detras del de la ABI y alineado a 16).
El prescan las cuenta aparte con `vuClip`, asi que un bloque que no las tenga no
paga ni un derrame.

Ni una de las tres tiene rama emitida: las dos o cuatro ramas del interprete se
calculan **todas** y se mezclan con una mascara.

* **VCH** decide por "los signos de S y T difieren". Con signos distintos la cuenta
  es `S+T` y el umbral `<= 0`; con el mismo signo es `S-T` y el umbral `>= 0`. Se
  emiten las dos y se mezcla. Escribe las cinco banderas; VCO.high resulta ser la
  misma expresion en las dos ramas (`resultado != 0 && S != ~T`), asi que sale una
  sola vez.
* **VCR** es VCH sin VCE ni VCO. El interprete compara `S+T+1 <= 0` en 32 bits, pero
  esa rama solo corre donde los signos DIFIEREN, y ahi `S+T` cabe siempre en 16 bits
  con signo: el signo de la suma envuelta ya es el bueno. Igual con `S-T` en la otra
  rama, donde los signos coinciden. Los carriles que se desbordarian son justo los
  que la mezcla descarta — lo que ahorra los `pmovsxwd` + suma en dobles.
* **VCL** tiene cuatro ramas por carril, pero las cuatro son la misma seleccion
  `accl = mascara ? X : S`, con distinta mascara y con `X = -T` en las dos de
  VCO.low y `X = T` en las otras dos. Asi que en vez de mezclar cuatro resultados se
  mezclan las **mascaras** y se hace una sola seleccion. Las banderas viejas se
  conservan en los carriles cuya rama no las escribe, que es lo que pide el
  hardware, y eso es exactamente lo que comprueba el fuzz: el estado inicial trae
  VCO/VCC/VCE al azar.
* El acarreo sin signo de VCL se saca sesgando los dos operandos con 0x8000 y
  comparando con `pcmpgtw` con signo: no hay `pmaxuw` sin SSE4.1 y el sesgo cuesta
  dos instrucciones.

Al meter xmm9..xmm15 en juego salieron dos limites del emisor que nunca se habian
tocado: las formas de desplazamiento por inmediato (`psllw`/`psrlw`/...) y las
`0F 38` no emitian REX, asi que xmm13 se habria codificado como xmm5 — un fallo
silencioso. Van todas por `sse_i`/`sse38` con REX. Y el marco crecio hasta 168
bytes, que no cabe en el `imm8` de `sub rsp` ni en el `disp8` de `xmmSpill`: las dos
formas admiten ya 32 bits.

## La familia del reciproco

VRCP/VRCPL/VRSQ/VRSQL no son SSE: miran UNA banda, la normalizan, entran en una
tabla de 512 entradas y devuelven una banda. Pero pagaban el CALL, el redecodificado
y — lo caro — el `accSpill` del acumulador cacheado. En linea son enteros puros:
`eax` el dividendo, `edx` la mascara de signo (`sar 31`), `ecx` la magnitud y luego
el desplazamiento de normalizado. Tres detalles:

* `clz` no existe en Nehalem (LZCNT es BMI1/ABM), asi que la normalizacion sale de
  `bsr` + `31 - bsr`.
* `shl`/`shr` variables solo leen CL, y ecx es justo donde esta la magnitud: la
  mascara de signo pasa por la pila (`push rdx` / `pop rdx`) mientras ecx hace de
  contador. No hay CALL en medio, asi que la alineacion no se toca.
* Las dos tablas son miembros del propio `Rsp`, o sea que la direccion sale de
  `rbx + offset` sin ninguna constante de 64 bits: `add rax,rax` + `add rax,rbx` +
  `movzx`.

Las tres bifurcaciones (magnitud cero, `-32768`, y el `divdp` de las variantes L)
son `jcc8` con hueco a rellenar, como las de `emitMem`; el cuerpo entero se queda
en ~110 bytes, muy por debajo del alcance del rel8.

VMOV/VRCPH/VRSQH ya estaban en linea desde antes (`pextrw` + store de 16 bits).

## Los tramos de DMEM en linea (LWC2 / SWC2)

Con la COP2 ya casi entera en linea, el censo (`-DKESTREL_VUSTAT=1`, y **con
`KESTREL_RSPJIT=0`**, que si no las cuentas se mezclan con las entradas que llama el
propio dynarec) deja claro quien es el siguiente bloque grande: LWC2+SWC2 son el
**19.8 %** de las instrucciones RSP ejecutadas en SM64, y de eso el **19.2 %** cae en
solo cuatro subcodigos:

| sub | op | % |
|-----|----|---|
| 0x04 | SSV | 4.6 |
| 0x03 | SDV | 3.7 |
| 0x03 | LDV | 3.7 |
| 0x02 | LSV | 2.9 |
| 0x02 | LLV | 1.8 |
| 0x04 | LQV | 1.3 |
| 0x04 | SQV | 0.8 |
| 0x02 | SLV | 0.5 |

Todas pagaban un CALL a la entrada ya especializada por sub. `emitVecMem` se las
queda cuando el tramo se puede resolver **con longitud constante en tiempo de
compilacion**, que es exactamente la condicion del camino rapido del interprete
(`vecFast`: elemento par, longitud par, ni el registro ni DMEM envuelven):

- **elemento impar → fuera** (el barajado byte a byte no vale la pena en linea).
- **carga corta** (LSV/LLV/LDV): la longitud se recorta a `16 - e`; el registro no
  envuelve, asi que sale constante.
- **guardado corto** (SSV/SLV/SDV): si `e + n > 16` se cae al helper, porque ahi el
  interprete envuelve el elemento y `vecFast` no lo cubre.
- **LQV**: solo si la direccion es multiplo de 16; longitud `16 - e`.
- **SQV**: solo si es multiplo de 16 **y** `e == 0`; longitud 16.
- **LBV/SBV y los barajados** (LRV/LPV/LUV/LHV/LFV/LTV): siempre al helper.

Lo que no entra no duplica reglas en ninguna parte: cae al **mismo** helper que
llama el interprete, colgado de un `jcc8` que comprueba en ejecucion lo unico que
no se sabe al compilar — que la direccion no envuelva (`cmp eax, 0x1000-len` +
`ja`) o que este alineada a 16 en el caso del cuarteto (`and ecx,15`, que ya deja
ZF, + `jne`).

El vaiven de orden de bytes (byte logico `k` vive en el byte `k^1` del anfitrion)
se hace **sin constante en memoria**: para elemento y longitud pares es un
intercambio dentro de cada banda de 16 bits, o sea

```
movdqa t, x ; psllw x, 8 ; psrlw t, 8 ; por x, t
```

en vez del `pshufb` + `kLaneSwap` del interprete, que obligaria a tener 16 bytes
direccionables desde el codigo emitido. Los tramos son los mismos que en
`dmemToVec`: 16 de golpe, o 8 → 4 → 2; el de 2 sale mas corto por el banco entero
(`movzx` + `rol r16, 8` + store).

El oraculo tuvo que crecer con el codigo: `--rspjitfuzz` ahora sortea **1 de cada 3
instrucciones** como LWC2/SWC2 sobre los 12 subcodigos, randomiza el banco escalar
y los 4 KB de DMEM, y compara ambos ademas del estado de la VU — un fallo de
direccion solo asoma comparando DMEM entero. Que la red pesca de verdad se
comprobo rompiendo `swap16` a proposito (`psllw 7`): 31 diferencias en 300
bloques; con el codigo bueno, **500 000 bloques y 0 diferencias**.

Medida honesta (SM64, `bench`, minimo de 2 pasadas), con el interruptor de A/B
`KESTREL_RSPJIT_NOVECMEM=1`:

| modo | en linea | por CALL |
|------|----------|----------|
| `threaded-jit` (200 intercambios) | 5.37 s | 5.38 s |
| `jit` lockstep (100 intercambios) | 8.76 s | 8.83 s |

En threaded es ruido, y era lo esperado: el hilo del RSP va sobrado y lo que se le
ahorra no lo espera nadie. En lockstep, donde el RSP si esta en el camino critico,
es un **0.8 %** consistente. Se queda porque el ahorro es real y el coste en
codigo es un camino rapido con su vuelta al helper de siempre.

## Los movimientos escalar↔vector (MFC2 / CFC2 / MTC2 / CTC2)

Lo ultimo de COP2 que salia por el puente generico `kestrel_rspjit_cop2` — que ademas
del CALL vuelve a decodificar el sub. Son cuatro cuerpos cortos y **sin ningun caso
que se escape**, asi que se emiten enteros: no hay envoltura de respaldo.

- **MFC2**: el byte logico `k` vive en el `k^1` del anfitrion, asi que son dos
  `movzx` de byte en `e^1` y `((e+1)&15)^1`, `shl 8`, `or` y `movsx` de 16 bits.
  Con `rt == 0` no se emite nada (`setR` ignora r0).
- **MTC2**: los dos `mov` de byte simetricos. El segundo se salta cuando `e == 15`,
  que es donde el interprete NO envuelve.
- **CFC2**: las banderas se guardan como 0/1 por banda. `psllw 15` lleva ese bit al
  de signo, `packsswb` de (baja, alta) satura `0x8000` a `0x80` y el `pmovmskb`
  entrega justo el entero que quiere el HW: bit `n` = banda baja `n`, bit `8+n` =
  alta `n`. Un `movsx` de 16 bits y listo. VCE no tiene mitad alta: ahi va un
  `pxor` y el byte alto sale cero solo.
- **CTC2**: la inversa. `movd` + `pshuflw`/`pshufd` difunde los 8 bits bajos a las
  ocho bandas, un `pand` contra `{1,2,4,…,128}` deja el bit `n` solo en la banda
  `n`, `pcmpeqw` contra la misma tabla lo vuelve mascara y `psrlw 15` lo deja en
  0/1. Para la mitad alta, `shr eax, 8` y otra vez.

La tabla de bits es lo unico que hubo que anadir: el codigo emitido solo sabe
direccionar cosas dentro del propio `Rsp` (`rbx + desplazamiento`), asi que
`bitLane[8]` vive ahi al lado de las tablas del reciproco. Es constante, no
estado: la savestate no la toca.

El fuzz tambien tuvo que crecer — hasta ahora **jamas generaba un sub < 0x10**
(ponia el bit 25 a uno siempre). Ahora una de cada seis instrucciones es un
movimiento, con `rt = 0` entre los posibles y `vs` recorriendo los cuatro valores
de `cr`, VCE incluido. Coberura comprobada rompiendo los cuatro cuerpos de uno en
uno: MFC2 52, CFC2 53, MTC2 52 y CTC2 49 diferencias en 400 bloques; con el codigo
bueno, **500 000 bloques y 0 diferencias**.

Medida (`bench` SM64, `jit` lockstep, 100 intercambios, minimo de 2), con el
interruptor `KESTREL_RSPJIT_NOVECMOVE=1`: **8.55 s en linea contra 8.64 s por
CALL**, un **1.0 %**. Pesan poco en el censo pero cada uno costaba un CALL con su
prologo y su re-decodificacion, y ahora COP2 entera — aritmetica, recorte,
reciproco y movimientos — esta en linea salvo VRNDP/VRNDN, VMULQ, VMACQ y VNOP.

## Etapa 3 — enlace de bloques (cola directa entre bloques)

Hasta aqui cada bloque volvia al bucle en C de `Rsp::step`, que releia el PC,
indexaba la tabla y volvia a llamar. Medido en SM64 (200 intercambios,
`threaded-jit`): **7 369 631 idas y vueltas** para 71,5 M de instrucciones, o sea
un bloque de ~10 instrucciones por vuelta de despachador.

Ahora el epilogo mira la tabla **el mismo** y, si en el PC de salida hay un bloque
vivo que cabe en lo que queda de tanda, salta a el con una **cola** (`jmp`, no
`call`) una vez desmontado el marco. La pila queda exactamente como a la entrada
—con la direccion de retorno del bucle en C encima—, asi que el ultimo bloque de
la cadena vuelve alli con su propio `ret` y **la pila no crece** por larga que sea
la cadena. Mismas 200 vueltas: **1 245 702 entradas, un 83 % menos**.

Tres decisiones que son la diferencia entre que funcione y que no:

- **El enlace es INDIRECTO, por tabla, nunca un `jmp rel32` cableado al destino.**
  En el RSP la IMEM se reescribe todo el rato (overlays por DMA), y un enlace
  directo obligaria a mantener retroenlaces por bloque para poder desengancharlos
  al invalidar. Con la tabla, poner `nullptr` en la ranura ya desengancha a todo
  el que apuntara ahi, gratis. Y cubre igual los saltos a registro (JR/JALR),
  cuyo destino no se conoce hasta ejecutarlo.
- **Todo bloque deja `Rsp::pc` escrito al salir.** El que cierra en salto ya lo
  hacia; el que cae por el final lo escribe ahora con una constante en el epilogo.
  Cuesta una tienda por bloque y a cambio el epilogo puede saltar a CUALQUIER
  bloque sin saber como termina, y el bucle en C deja de tener que avanzar el PC
  (desaparece `Block::setsPc`).
- **Destino estatico cuando se sabe al compilar.** Un bloque que cae por el final
  sigue en la instruccion siguiente, y uno que cierra en `J`/`JAL` tiene el
  destino en el opcode: en los dos casos la ranura es una constante y el sondeo se
  ahorra releer el PC, desplazar y sumar. Solo los condicionales (dos destinos) y
  los saltos a registro leen el PC recien escrito.

**El presupuesto lo debita quien salta.** La primera version se lo cobraba cada
bloque en su prologo (`sub dword [rbx+budOff], n`): una lectura-modificacion-
escritura en TODOS los bloques, enlacen o no, y encima encadenada con la lectura
del sondeo. Medida: **1 % peor que sin enlazar**. Ahora el que no enlaza paga una
lectura y nada mas, y el que enlaza paga la resta que habria que hacer de todas
formas. Al bloque de entrada lo debita el bucle en C.

La comparacion del saldo va **con signo**: si una invalidacion simultanea desde el
hilo del CPU deja un par `(fn, nOps)` incoherente, lo peor que puede pasar es
pasarse de tanda una vez; con el saldo ya negativo, `jg` corta la cadena en el
acto. El codigo de los bloques muertos sigue siendo valido hasta el reciclado del
buffer, que solo ocurre dentro de `compile()` y por tanto nunca bajo una cadena.

Medida con el RSP como cuello de botella (`--rspbench 300000000`, tres pares):
**230,8 Mips con enlace contra 225,5 sin el, +2,4 %** — y eso con bloques de 64
instrucciones (`kMaxOps`), que es el caso PEOR para enlazar porque el despachador
ya estaba amortizado entre 64 operaciones. En `bench` de SM64 no se ve en el reloj
porque el hilo del RSP tiene holgura de sobra (570 % de tiempo real): lo que baja
es el trabajo por instruccion de RSP, no el tiempo de pared de ese juego.

Interruptor de biseccion: `KESTREL_RSPJIT_LINK=0`, y modo `rspnolink` en
`validate.py` (dentro de `gate_all.sh`).

## El dynarec del RSP no se usa en Lockstep — y antes se PAGABA igual

Encontrado midiendo lo anterior: en `bench --mode jit` el emulador iba **mas
rapido con `KESTREL_RSPJIT=0`** (22,3 s) que con el dynarec puesto (24,1 s).

La razon esta en `system.cpp`: en Lockstep el bucle del sistema intercala
`memory.rsp.step(1)` — **una instruccion de RSP por vuelta**. La tanda vale 1, y
un bloque necesita al menos `kMinOps` = 2 para existir, asi que **ningun bloque
llega a ejecutarse jamas** en ese modo. Lo que si ocurria era compilarlos: cada PC
nuevo entraba por `State::Unknown`, se compilaba entero... y acto seguido no cabia
en la tanda y salia por el interprete. Se pagaba el compilador completo a cambio
de exactamente nada.

El arreglo es una guarda en el despachador, `c >= rspjit::kMinOps`: no se mira la
tabla —y sobre todo no se compila— lo que no cabe en lo que queda de tanda. Es
semantica general, no un parche para Lockstep: compilar lo que no se puede
ejecutar nunca es trabajo perdido en cualquier modo. En Threaded la tanda es la
tarea entera y la guarda no descarta absolutamente nada.

**`bench --mode jit`: 24,10 s -> 22,60 s, un 6,2 %.** `threaded-jit` sin cambio
(5,37 s), md5 identico en los catorce pasos de `gate_all`.

## La cache de imagenes estaba mal calibrada: 27x menos compilacion

La cache de IMAGENES de microcodigo (N tablas de bloques, cada una con su sombra de
los 4 KB de IMEM) ya existia, pero con dos numeros puestos a ojo: **4 ranuras** y
**umbral 64** trozos de 8 B para decidir que una imagen es "nueva" en vez de una
variante de la que hay puesta. Censando `KESTREL_RSPJIT_STATS` en SM64 (500
intercambios de buffer) se ve lo que hacian de verdad — bloques compilados en toda
la corrida:

| ranuras | umbral | bloques compilados | aciertos / fallos de imagen |
|---------|--------|--------------------|------------------------------|
| 4  | 64 | 241162 | 1155 / 15862 |
| 6  | 64 | 105066 | 2617 / 14902 |
| 16 | 64 | ~107000 | 2776 / 14997 |
| 16 | 16 | 52739 | 6847 / 10861 |
| 16 | 4  | 50389 | 6199 / 11489 |
| **16** | **8** | **3948** | **9403 / 8265** |
| 32 | 8  | 3983 (usa 17 ranuras) | 10140 / 7651 |

Las dos perillas van juntas y ninguna sirve sola:

- Con **4 ranuras** el juego se queda sin sitio enseguida y cada tarea parchea la
  ranura mas parecida, que es media recompilacion. Subir a 6-8 ya baja de 241 k a
  105 k bloques.
- Con **umbral 64** casi toda diferencia se considera "parecida", asi que aunque
  haya ranuras libres no se estrenan: SM64 solo llega a usar 6 de 16. Bajando el
  umbral a 8, cada overlay se queda con su propia tabla y volver a el cuesta un
  `memcmp` de 4 KB y un puntero.
- Bajarlo **mas** es peor (umbral 4 -> 50389): se estrena ranura por diferencias de
  32 B, los casi-duplicados llenan las 16 y echan a las plantillas grandes.
- Pedir mas de 16 ranuras no aporta: con 32 disponibles SM64 usa 17 y compila los
  mismos bloques. `kJitWaysMax` sube a 32 igualmente para poder medir otros
  microcodigos sin recompilar.

Nada de esto cambia la semantica: la correctitud la sigue dando `syncImem`, que
invalida por palabra de 64 bits exactamente los bloques que cubren algo que cambio.
La cache solo elige QUE tabla se parchea.

**Medida limpia** (heartbeat, SM64, 1200 intercambios): el rendimiento del emulador
de RSP sube de **144,9 a 186,9 Mips ocupado (+29 %)** con la misma ocupacion de
worker (69 %), y el freno de la CPU (`pace`) baja del 18 % al 9 % de pared. En el
reloj de `bench` se nota poco (5,4 s en los dos casos, ~575 % -> ~620 % de tiempo
real) porque en SM64 con SoftRDP **el palo largo es el RDP** (94-95 % de ocupacion)
y el hilo del RSP va holgado: lo que se gana es CPU del anfitrion, que es justo lo
que hace falta en microcodigos pesados (PD) y en maquinas flojas.

Perillas: `KESTREL_RSPJIT_WAYS=<n>` (por defecto 16) y `KESTREL_RSPJIT_NEWWAY=<n>`
(por defecto 8).

## LPV / LUV / LRV en linea: un `pshufb` con la mascara armada al vuelo

Los tres barajados que si pesan en SM64 (LRV 0,60 %, LUV 0,55 %, LPV 0,40 % de las
instrucciones de microcodigo) salian por CALL a `lwc2Thunk<N>`, que hace ocho o
dieciseis lecturas de byte con su enmascarado. Los tres son la MISMA figura: los
bytes que quiere la instruccion estan todos dentro de una ventana de 16 bytes que
empieza en una direccion alineada, solo cambia que byte va a que sitio — y ese
reparto depende de la direccion, no del opcode. O sea, un `pshufb` con la mascara
armada en tiempo de ejecucion.

- **LPV/LUV** (`emitVecPack`): `V.u(o) = dmem[a + ((index + o) & 15)] << 8` (LUV
  desplaza 7). La mascara sale de difundir `index` a los dieciseis bytes y sumarle
  la tabla `{0,0,1,1,...,7,7}` de `Rsp::byteHalf`; no hace falta el AND con 15
  porque `pshufb` ya se queda con los cuatro bits bajos, solo hay que evitar que el
  bit 7 se encienda — de ahi el sesgo `+16` sobre `index`, que va de -15 a 7.
  Despues un AND con `0xff00` por banda deja el byte en la mitad alta y borra la
  basura de la baja. Se cede al helper si la ventana cruzara el final de DMEM.
- **LRV** (`emitVecRight`): rellena los bytes logicos `>= start` con el cuarteto
  alineado y deja los de abajo intactos. Como el byte logico `k` vive en el byte
  `k^1` del anfitrion, con la tabla `Rsp::laneIdx` = `{1,0,3,2,...}` sale todo con
  dos mascaras de bytes: los indices del `pshufb` son `laneIdx - start` (negativo
  -> bit 7 -> cero, y esa banda la tapa la mezcla) y la mascara de mezcla es
  `pcmpgtb(laneIdx, start-1)`. **No tiene salida al helper**: la direccion se alinea
  a 16, asi que la ventana nunca cruza el final de DMEM.

**Verificado** con `--rspjitfuzz 300000` (oraculo = interprete, compara el banco
vectorial, el escalar y los 4 KB de DMEM enteros; el fuzz ya sorteaba los doce
subcodigos, incluidos estos) y `--rspldfuzz 300000`: 0 diferencias. Gates 14/14 +
prdp 3/3, regress=0, md5 sin cambio.

**Medido**: en SM64 no se nota (`rsp Mips busy` 188,0 -> 187,7, dentro del ruido;
bench 537 % -> 542 % de tiempo real, tambien ruido). Es lo esperable con 1,55 % de
las instrucciones y el RSP al 58 % de ocupacion: el hilo del RSP va sobrado y el
palo largo es el RDP. Se queda porque es correcto, no cuesta nada en el camino
caliente y en un microcodigo que use estas cargas de verdad (los de audio y los
que empaquetan normales) el CALL si se paga. A/B: `KESTREL_RSPJIT_NOVECPACK=1`.

## El dynarec del RSP NO es el cuello en libdragon (2026-09-17)

hostprof del hilo del RSP en junkrunner64 (200 intercambios, prdp threaded-jit): el codigo
emitido es ~9 % del hilo; el grueso son las citas con la CPU (`spReadSync`, `dpLogWait`)
alrededor de los DMA del SP que lanza rspq. Mejorar el codegen aqui no mueve la pared; lo que la
mueve es quitar citas sin perder semantica (ver STATUS 2026-09-17, DMA SP->RDRAM en el diario).

## Siguiente

0. **Enlace directo con retroenlaces** (`jmp rel32` cableado en vez del sondeo por
   tabla) es lo unico que queda por probar del enlace: quita la lectura de la
   tabla y el salto indirecto, pero obliga a mantener por bloque la lista de quien
   le apunta para poder despatchar al invalidar. Con el hilo del RSP tan holgado
   como esta ahora no hay donde cobrarlo; tiene sentido cuando el RSP sea el palo
   largo (microcodigos mas pesados que el de SM64).
1. **Lo que sigue saliendo por CALL** ya no es COP2 ni los tres barajados
   gordos: quedan LHV/LFV/LTV y las tiendas barajadas (SPV/SUV/SRV/STV), mas los
   tramos impares o que envuelven. **Censo** (`KESTREL_VUSTAT=1` con
   `KESTREL_RSPJIT=0`, SM64, 166,4 M instrucciones de microcodigo): LRV 0,60 %,
   LUV 0,55 %, LPV 0,40 % — **los tres ya van en linea** (ver mas abajo) — y
   LTV/SPV/SUV/STV entre 0,01 y 0,07 % cada uno, que no pagan el emisor.
   (La nota anterior decia que en SM64 no aparecian: era falsa, nunca se habia
   corrido el censo.)
2. **COP0 del RSP** (MFC0/MTC0 a los registros de SP y DP) — tambien por CALL, y
   ahi la llamada es lo de menos: la semantica toca el bus y los semaforos.
3. **El mix de `--rspbench` esta sesgado** y no sirve para elegir donde tocar; el
   censo bueno es `KESTREL_VUSTAT=1` (y hay que correrlo con `KESTREL_RSPJIT=0`,
   que si no las cuentas mezclan lo que despacha el interprete con lo que llama el
   dynarec).
