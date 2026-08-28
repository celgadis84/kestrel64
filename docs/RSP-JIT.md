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
VLT/VEQ/VNE/VGE mas VMRG. Es el grueso de las COP2 que ejecuta SM64. Lo que sigue
saliendo por el CALL a la entrada especializada son las comparaciones de recorte
(VCH/VCL/VCR), la familia del reciproco y los movimientos escalar↔vector.

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

## Siguiente

1. **VCH / VCL / VCR.** No caben en seis registros volatiles, pero ahora que el
   prologo ya salva xmm6..xmm8 hay sitio: los bloques sin racha MAC pueden usarlos de
   temporales. 0.84 % de las instrucciones del RSP.
2. **La familia del reciproco** (VRCP/VRCPL/VRCPH/VMOV, 3.84 %). No es SSE — operan
   sobre UNA banda — pero siguen pagando el CALL y el redecodificado; en linea serian
   un `pextrw` + tabla + `pinsrw`.
3. **El mix de `--rspbench` esta sesgado** y no sirve para elegir donde tocar; el
   censo bueno es `KESTREL_VUSTAT=1`.
