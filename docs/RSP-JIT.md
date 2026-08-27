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

Cubiertas hoy (`vuInline`): VAND/VNAND/VOR/VNOR/VXOR/VNXOR, VSAR, VADD, VSUB,
VADDC, VSUBC y VMUDL. Son ~29 % de las COP2 que ejecuta SM64. El resto sigue
saliendo por el CALL a la entrada especializada, que es exactamente el codigo de
antes: la lista se amplia una operacion a la vez, con fuzz de por medio.

El emisor reproduce los intrinsecos de `vuOpT` en el mismo orden — no hay una
segunda semantica. Y hay dos oraculos que lo demuestran:

* `--rspjitfuzz N`: monta bloques reales de cuatro operaciones vectoriales al
  azar en IMEM (los 16 modificadores de elemento, registros solapados a
  proposito), los compila, y compara el estado vectorial completo contra
  interpretar las mismas cuatro instrucciones desde el mismo estado. 200 000
  bloques, 0 diferencias.
* el de siempre: mismo md5 de framebuffer con `KESTREL_RSPJIT=0`, y el modo
  `rspinterp` de `gate_all`.

Medido (SM64, lockstep, 300 intercambios, parallel-rdp): 31.95 s -> 31.80 s. Es
poco, y era de esperar: en modo hilos el RSP tiene holgura (ocupacion 65-78 %,
`cpuWait` ~25 %), asi que ahorrar CPU en ese hilo no sube los fps todavia — baja
el consumo y deja sitio para las operaciones que faltan.

## Siguiente

La familia MAC (VMADN/VMADH/VMADM/VMULF/VMUDN/VMUDM) es el ~60 % de las COP2 y
sigue pasando por el CALL. Inline ademas abre la puerta a lo que de verdad
importa ahi: mantener el acumulador de 48 bits (`acch`/`accm`/`accl`) en
registros xmm a lo largo de una racha de operaciones dentro del bloque, en vez
de recargarlo y reescribirlo — seis accesos de 16 bytes por instruccion.
