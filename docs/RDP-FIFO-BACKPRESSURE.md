# DPC FIFO back-pressure — the "SM64 hangs under parallel-RDP" bug

Closed 2026-08-20. Kept because the failure mode is subtle and the fix is a
threading-model invariant, not a one-off patch.

## Symptom

With `KESTREL_PRDP=1` **and** the threaded RDP worker, SM64 deadlocked in the
*guest* after ~90 VI fields. Not reproducible with SoftRDP (any mode), with
lockstep (`KESTREL_THREADS=0`), or with `KESTREL_RDPINLINE=1`.

From outside it looked like a lost interrupt: the CPU spun in SM64's boot idle
loop at `0x80246dd8` (`b .`) retiring ~330 M insn/s, VI kept raising and being
acked, and SP/AI/DP went silent. `rdpBusy=0`, `rdpQ=0`, `rspRun=0`.

## What it actually was

The guest **faulted**. `KESTREL_FAULTTRACE=1`:

```
[fault] TLBL(2) epc=0x8024695c badv=0x00000040 cause=0x00000008 ra=0x80246ac4
```

`0x8024695c` is inside SM64's `handle_dp_complete()`:

```
8024694c  addiu sp,sp,-24
80246954  lui  t6,0x8033
80246958  lw   t6,-10896(t6)     ; t6 = sCurrentDisplaySPTask   (0x8032d5b0)
8024695c  lw   t7,64(t6)         ; ->msgqueue     <-- t6 == NULL, badv = 0x40
```

A **DP interrupt with no display task outstanding**. libultra's exception handler
then did `sh 1,0x10(k0)` / `sh 2,0x12(k0)` — `state = OS_STATE_STOPPED`,
`flags = OS_FLAG_FAULT` — on thread id=3, the pri-100 interrupt/scheduler thread.
With the dispatcher dead, threads 4 (sound) and 5 (game loop) parked forever in
`osRecvMesg` at `0x80322868` and the CPU fell back to the idle thread. Hence
"everything quiet but the CPU is spinning".

The extra DP came from the RDP job queue:

```
[ev] 199198723 rdpst  00227000 00227070   <- fresh DPC_START, 828 jobs still queued
[ev] 199519622 rdpint 00229310 002293c0   <- DP #1: an OLD job, frame long finished
[ev] 199619482 rdpint 002293a0 002293a8   <- DP #2: the new frame's own SYNC_FULL
```

One gfx SP task, two DP interrupts. The second one hit `handle_dp_complete` after
the first had already set `sCurrentDisplaySPTask = NULL`.

## Root cause

Hardware has no queue. The DPC command processor has **one** read pointer:
writing a fresh `DPC_START` followed by `DPC_END` reloads `CURRENT` from `START`,
and whatever was left unread of the previous span simply ceases to exist. That is
safe on hardware because the RDP consumes the FIFO in real time — by the time the
game installs the next frame's buffer, the previous one is long drained.

kestrel's threaded RDP breaks that assumption: the producer (CPU or RSP thread)
can run whole frames ahead, leaving hundreds of spans queued. Reloading `START`
then meant the worker would rasterize those stale spans **after** the reload and
retire their `SYNC_FULL`s — one surplus DP interrupt per abandoned frame.

parallel-RDP only made it visible: it waits on the GPU timeline at every
`SYNC_FULL`, so it lags far enough behind for the producer to lap it. SoftRDP has
the same hole, it just never got deep enough to trip it.

## Fix

`src/core/memory.cpp`, DPC_END handler: a fresh `START` may not be installed while
the RDP still has work in flight. In threaded mode the writer drains first
(`rdpDrain()`), which is the faithful reading of the hardware — the producer
cannot swap the FIFO buffer under a running command processor.

Side effects, both wanted:

- `DPC_CURRENT` stays honest (F3DEX2 flow-controls its ring against it).
- The job queue is bounded to one frame instead of growing without limit.

## Diagnostics added while hunting this (kept)

| Env | What it does |
|-----|--------------|
| `KESTREL_FAULTTRACE=1` | print the first 24 guest exceptions (code, EPC, BadVAddr, RA) |
| `KESTREL_FAULTSTOP=1` | halt at the first guest fault and dump the event ring + PC ring — the fault window survives intact |
| `KESTREL_WATCHP=<phys>` | ring of the last 128 **CPU** stores to that 16-byte line, dumped by the watchdog. Goes through `dcWrite`, so it sees cached stores that `KESTREL_WATCH` (bus-level) misses |
| `KESTREL_EVDUMP=<n>` | how many events the watchdog / faultstop dumps (default 80) |
| `KESTREL_DPSYNCLOG=1` | print the RDRAM address of every `SYNC_FULL` retired, both backends. Diffing the two sequences is what proved the streams diverged |
| `ev` tags `rdpst` / `rdpint` | fresh DPC_START reload, and every DP interrupt with its span |

Diffing `KESTREL_DPSYNCLOG` output between SoftRDP and parallel-RDP is the
cheapest way to catch this class of bug: the first 85 SYNC_FULL addresses matched
exactly, and the divergence at #86 pinned the moment the streams parted.
