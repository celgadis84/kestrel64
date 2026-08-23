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

## Invalidation — two layers

IMEM is not read-only: the CPU DMAs new microcode into it constantly, and a
task boundary can reuse the same addresses for different code. Two independent
mechanisms cover that:

1. **Range invalidation from the DMA.** `Memory::spDma` calls
   `Rsp::jitInvalidate(off, bytes, sp.data())` for any transfer into IMEM. It
   clears the slots the DMA overwrote **plus `kMaxOps - 1` slots before them**,
   because a block that *starts* earlier can still *contain* an overwritten
   instruction.
2. **Whole-IMEM fingerprint.** `Rsp::start()` compares an FNV-1a hash of all
   4 KB against the one stored at compile time; a mismatch clears everything.
   This catches writes that never went through `spDma` (CPU stores straight
   into IMEM through the bus).

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

SM64, `bench` over 200 buffer swaps, threaded-jit:

| | realtime | RSP busy |
|---|---|---|
| `KESTREL_RSPJIT=0` | 110.2 % | 28.5 Mips |
| `KESTREL_RSPJIT=1` | 113.9 % | 30.7 Mips |

`KESTREL_RSPJIT_STATS=1` prints coverage: **76.6 %** of executed RSP
instructions run from compiled blocks. The missing quarter is branches and
their delay slots, which is exactly what Stage 2 is for.

With correct range invalidation (as opposed to the first, global version):
flushes 4156 -> 896, compiles 185088 -> 158227, coverage unchanged.

## Next stage

Absorb branches and delay slots into blocks so that a loop body compiles as one
unit. That is where coverage goes past 76.6 % and where the RSP dynarec starts
paying like the CPU one does.
