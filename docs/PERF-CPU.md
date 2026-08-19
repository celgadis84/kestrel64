# CPU performance — interpreter + dynarec

Where the emulator's own host cycles go, how that was measured, and what each
change bought. Host: i7-870 (Nehalem, 4c/8t, 2.93 GHz), Windows, clang -O3
`-march=native`, no LTO.

Reference point: a real N64 retires ~93.75 M instructions/s at CPI 1, so
"N64 speed 100%" in the heartbeat means ~93.75 Mips of emulated CPU.

## Measuring

Three instruments, all opt-in and all committed:

| Tool | Env | What it answers |
|---|---|---|
| Heartbeat | `KESTREL_HEARTBEAT=1` | Mips, N64 speed %, and **worker occupancy** (`rdp`/`rsp` busy %, `cpuWait` %) |
| Host sampling profiler | `KESTREL_HOSTPROF=<ms>` | which *emulator* function burns host cycles (`scripts/hostprof.py` symbolises the dump) |
| JIT stats | `KESTREL_JIT_STATS=1` | block coverage, `avgK` (ops per block), decline reasons, block-terminating opcodes |

**Never benchmark with `KESTREL_MAXINSN`.** It arms `debugArmed`, which turns on
`stepTraps()` and the jump-log ring per instruction — the numbers come out biased.
Use `KESTREL_HEARTBEAT=1` plus a wall-clock `timeout`.

## Diagnosis (SM64, threaded RCP)

1. Occupancy said `rdp 32% rsp 48% cpuWait 0%` — the RCP workers are not the
   long pole and the CPU thread never waits on them. Per-thread host CPU
   confirmed one thread pinned at 100%.
2. Host profiler, JIT on: `jitTryBlock` **67%**, `compileBlock` 7.8%.
   Interpreter only: `CPU::step` 57%, `System::stepCpu` 24%, `icFetch` 6.4%.
3. JIT stats: `avgK 2.39`, 44.8 M compile declines. So ~90 % of retired ops did
   run inside JIT blocks, but each block was 2.4 ops — block-entry overhead
   dwarfed the emulated work.

## Changes

### 1. Negative compile cache (`jit.hpp` / `jit.cpp`)

`compileBlock()` fails when the *leader* op is not compilable, and in real code
that same PC is re-attempted millions of times. A direct-mapped 8192-entry table
remembers "this phys did not compile", validated against the leader word read
from the I-cache line — so self-modifying code or a DMA over the block
invalidates the entry by itself, with no explicit flush.

Compile declines 44.8 M → 11.4 M.

### 2. In-block interpreter fallback (`CPU::jitInterpOp`, `emitInterpOp`)

Previously the first unsupported op ended the block. In SM64/PD that op is almost
always FPU (`LWC1`/`SWC1`/`COP1`), which appears every few instructions, so blocks
were cut short constantly. Now such an op is emitted as a call to the interpreter:
one `CALL` of cost, but the block continues.

`jitInterpOp` reconstructs exactly the context `step()` would have (`curPc` = the
op, `pc` = delay slot, `nextPc` = pc+4), runs `execute()`, and returns 0 if
anything diverged from sequential advance (exception, halt, branch). On 0 the
block exits with the control flag set, i.e. the interpreter's `pc`/`nextPc` are
already the resume point and the driver must not recompute them — unlike the
mem-op bail, whose contract is "no side effects yet".

Admitted set — no control flow, no speculative state:
COP1 (except `BC1x`), `LWC1`/`LDC1`/`SWC1`/`SDC1`, `LWL`/`LWR`/`SWL`/`SWR`,
and `DIV`/`DIVU`/`DMULT`/`DMULTU`/`DDIV`/`DDIVU`.
Excluded on purpose: COP0, `CACHE`, `LL`/`SC`, `SYSCALL`/`BREAK`/`TRAP`, all jumps.

`avgK` 2.39 → 2.94.

### 3. Block linking enabled by default (`KESTREL_JIT_LINK`)

Step 3 of the linking work was already implemented but gated off. With
`avgK ≈ 3` the driver round-trip *is* the cost, so chaining blocks directly is
the single biggest lever. Validated on systemtest (interp/JIT/JIT+link all
0/3721·0/2·0/6) and framebuffer-md5-identical to the non-linked JIT.

### 4. Interpreter hot path

- Four function-local `static`s with dynamic initialisers lived inside
  `CPU::step()` (`fastFetch`, `intlog`, `jlogNoSpin`) and one in `dcWrite`
  (`KESTREL_DCWT`), plus `jitOn` inside `System::stepCpu`'s loop. In C++ each of
  those costs a thread-safe initialisation-guard check **per emulated
  instruction**. Moved to namespace scope, where the dynamic init happens once
  before `main()`.
- `icFetch`/`dcRead`/`dcWrite` assembled values byte by byte out of the cache
  line; `icFill`/`dcFill`/`dcFlush` copied byte by byte with a bounds test per
  byte. Replaced with a width-exact `memcpy` + `bswap` (the line holds big-endian
  guest bytes, the host is little-endian — identical result, 2 instructions) and
  a whole-line `memcpy` when the line is entirely inside RDRAM, keeping the
  byte-wise path for the RDRAM-end edge.

## Result (SM64, threaded RCP, 45 s wall)

| Config | N64 CPU speed | Insns in 45 s |
|---|---|---|
| Before this work | 19.2 % | 697 M |
| + negative cache + interp fallback | ~21 % | 884 M |
| + block linking | **~28 %** | **1280 M** |

### 5. Traces made opt-in (`KESTREL_JIT_TRACE`)

Superblock tracing (follow the taken edge of a conditional branch into the same
block) was on by default. Measured on SM64 it is **net negative**: 1237 M insns /
30.77 Mips / 24.8 % N64 speed with traces vs 1316 M / 32.76 Mips / 27.3 % without.
A traced block declares a larger `K`, and the revalidating prologue then rejects
the chained link whenever `K` does not fit the remaining window — more driver
round-trips and more emitted code for the same work. Kept, but off by default.

### 6. Caller-window guard (`jitTryBlock`)

`System::run()` asks for a fixed number of ops per tick and then ticks the VI. A
block that retires *more* ops than the caller asked for pushes the field boundary
past the requested count, so the same guest instruction lands in a different field
depending on the mode. The interpreter never overshoots; the JIT could. Blocks
whose `K` exceeds the remaining budget are now declined — bit-exact field
boundaries across interp/JIT.

### 7. Host FP environment via MXCSR, not `<cfenv>` (`cpu.cpp`)

Every computational COP1 op sets the host rounding mode and clears the IEEE status
before computing, then harvests the sticky flags. Through `<cfenv>` that is
`fesetround` + `feclearexcept` + `fetestexcept`, and under mingw each one goes into
`__mingw_setfp`, which synchronises the **x87 control word as well as MXCSR**. The
host profiler put ~32 % of the emulator's total time there:

```
51.28%  kestrel_jitProceedTramp
28.68%  __mingw_setfp
 2.07%  fenv_decode
 1.70%  __mingw_setfp_sse
```

On x86-64 every float/double operation is SSE, so MXCSR alone governs rounding and
the sticky flags, and reading/writing it is two instructions. `namespace mx` in
`cpu.cpp` does exactly that; `mx::prep()` only writes the register when the value
actually changes (the guest's rounding mode almost never moves between ops), and
`cvtInt` saves/restores only the RC field, matching what `fesetround(save)` did.

**33.99 -> 49.35 Mips** (26.0 % -> 50.1 % N64 CPU speed). The krom suite dropped
from 245 s to 158 s as a side effect.

### 8. False sharing on the fields the CPU thread polls (`memory.hpp`, `rsp.hpp`)

With the FP churn gone the profile collapsed onto two *loads* inside the block
re-entry check — 18.45 % and 12.70 % of all samples in two `cmp` instructions:

```
mem->rsp.running                       // 18.45 %
mem->rcpMode == RcpMode::Lockstep      // 12.70 %
```

Neither is contended in the logical sense: `running` changes at task start and at
BREAK, `rcpMode` is fixed at startup. They were slow because of what sat next to
them in memory. `Rsp::running` was followed by the delay-slot latch and the safety
budget, which the RSP worker writes on **every microcode instruction**; `rcpMode`
shared its line with `rdpQueue` / `rdpMx` / `rdpBusy`, which the RDP worker writes
on every job. The JIT re-entry check reads them roughly every three guest
instructions, so what should be an L1 hit was a coherence miss with another core
invalidating the line continuously.

Fix: give them their own cache lines (`alignas(64)` plus explicit padding), same
for the MI interrupt pair (`mi_mask` / `mi_intr`, read per block, written only when
an IRQ is raised — they shared a line with the DPC performance counters the RDP
bumps per span) and for those counters themselves. Also reordered the guard to test
`rcpMode` before `rsp.running`, so threaded mode never touches the RSP field at all.
No semantic change whatsoever.

**52.4 -> 70.5 Mips** (53 % -> 75 % N64 CPU speed).

### 9. `Random` advanced in O(1) (`jit.cpp`)

COP0 `Random` decrements once per instruction and snaps back to 31 when it equals
`Wired`. The chained-block prologue replayed that **as a loop**, one iteration per
deferred op, on every link. With `avgK ~= 3` that was ~30 host instructions per
block for a register the game almost never reads.

The sequence is a cycle: `d = (r - wired) mod 64` steps down to `Wired`, then a
cycle of `n = ((31 - wired) mod 64) + 1` values — which is `[wired..31]` for the
normal `Wired <= 31`, and the full 64-value sweep the loop already modelled for
`Wired > 31`. `randomAdvance()` computes the same answer directly; it was checked
exhaustively against the old loop over all 64x64x139 combinations of
(Random, Wired, steps) with zero mismatches, and n64-systemtest exercises it.

**70.5 -> 87.5 Mips** (75 % -> 86 % N64 CPU speed).

## Result after the second pass (SM64, threaded RCP + JIT)

| Step | Mips | N64 CPU speed |
|---|---|---|
| Before this pass | 33.99 | 26.0 % |
| + MXCSR instead of `<cfenv>` | 49.35 | 50.1 % |
| + cache-line isolation | 70.55 | 72-75 % |
| + `Random` in O(1) | **87.51** | **~86 %** |

For reference, the same host runs the interpreter at 13.55 Mips and lockstep JIT at
14.41 Mips, so threading and the dynarec are both carrying their weight now.

## Two bugs found by this work

### Bug 1 — `unlinkAll()` was quadratic against guest I-cache flushes

`CodeCache::unlinkAll()` walks *every* link site and *every* block. The guest
invalidates its I-cache one line at a time (libultra issues 512 back-to-back
`CACHE` ops for a full sweep), and every one of those paid the full sweep. With
tens of thousands of blocks the emulator ground to a halt — the host profiler
showed **86.79 % of samples inside `CPU::cacheOp`**.

Fix: an `anyLinked` flag, set where a link site is actually armed, checked first
in `unlinkAll()`. Nothing armed → nothing to walk.

systemtest jit 34 s → **10 s**; threaded-jit 34 s → **9.6 s**.

### Bug 2 — lost wakeup in the RSP worker (threaded + JIT hang)

`rspWorkerLoop` cleared `rspBusy` *outside* `rspMx`. The store+notify can then
land between a waiter evaluating its predicate (which saw `rspBusy == true`,
under the mutex) and the waiter registering itself on the condvar: the
notification is lost and the CPU thread sleeps forever waiting on an RSP that
already finished. The watchdog caught it exactly — `retired +0`, `rspBusy=0`,
`rdpQ=0`, every predicate true, RIP inside a kernel wait.

Fix: clear the flag under `rspMx`, and `notify_all` (not `notify_one`) on both
`rspCv` and `rdpCv`, since two different waiter classes share each condvar.

Only reachable with threads + JIT because the JIT is fast enough to make the
window between the check and the wait routinely lose the race.

## The `lockstep == threaded` md5 gate was measuring the wrong thing

The old gate ran SM64 to a fixed instruction count and compared framebuffer md5s.
That is **not** a deterministic point of the game in threaded mode: the RCP runs
on its own threads, so how many instructions the CPU burns spinning on a
`SP_STATUS`/`DP` wait depends on the worker's wall-clock. Two runs of the *same
build* stop in different animation phases. Measured: per-field state hashes
(`KESTREL_FIELDHASH=1`) differ run-to-run in threaded mode, with and without JIT,
and threaded-jit produced two different md5s across three runs.

Nothing was wrong with the emulation. The gate now stops on **VI buffer swaps**
(`KESTREL_MAXFLIPS`, exposed as `validate.py sm64 --sm64-flips`, default 60),
which *is* a game state, and there all five modes agree byte for byte:

```
sm64[interp]       MATCH 466282775dbd0ac084946558a1c30771 (14s)
sm64[jit]          MATCH 466282775dbd0ac084946558a1c30771 ( 8s)
sm64[jit-nolink]   MATCH 466282775dbd0ac084946558a1c30771 (10s)
sm64[threaded]     MATCH 466282775dbd0ac084946558a1c30771 (11s)
sm64[threaded-jit] MATCH 466282775dbd0ac084946558a1c30771 ( 4s)
```

Threaded mode is deliberately *not* cycle-deterministic: the wall-clock of the
RCP workers is the price of the parallelism. Determinism is guaranteed and gated
at frame granularity, and bit-exactly at instruction granularity in the lockstep
modes (interp / jit / jit-nolink), which are the JIT's oracle.

## Diagnostics added (all opt-in, all committed)

| Env | What it does |
|---|---|
| `KESTREL_WATCHDOG=<secs>` | prints CPU/RSP/RDP liveness every N s; if `retired` did not move, suspends the CPU thread and prints its RIP — this is what found bug 2 |
| `KESTREL_FIELDHASH=1` | FNV of CPU state at every VI field: first differing field localises a divergence |
| `KESTREL_FIELDDUMP=<n>` | full GPR/COP0/PC dump at field *n* |
| `KESTREL_JIT_CHAIN=<n>` | max blocks chained per driver entry (bisecting linking) |
| `KESTREL_JIT_NOLINK=1` | disable block linking |
| `KESTREL_JIT_TRACE=1` | enable superblock tracing (measured negative, see above) |

Note: the host profiler dump is **cumulative over the whole run**, so it cannot
profile a hang window — that misled one round of diagnosis before the watchdog
existed.
