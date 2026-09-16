# Submit-time RDP scheduling + exact RSP clock (in `src/`, 2026-09-11)

`memory.{cpp,hpp}.subsched` are the snapshot of the refactor as it was when it got parked.
It is now BACK in `src/`, together with the exact RSP guest clock, and the hang that caused
the parking is understood and fixed. Keep the snapshots until the combo ships.

## What the refactor does

`rdpSubmit` runs the RDP cost model under `rdpMx` (`dpScheduleSpan`) so the whole guest
timeline of a FIFO span — start instant, end instant, DPC_CURRENT boundaries, MI_DP
deadline — exists before any painting. The RDP worker only paints (`rdpRunJob(..., preCosted)`).
The schedule ring is per-SPAN instead of per-queue-job (`kDpRingN = 256`), so coalescing two
`DPC_END` writes into one worker job no longer changes anything the guest can see.

## Exact RSP guest clock

`Rsp::step` only published `cyclesRun` at chunk boundaries (8192 instructions), so when the
RSP thread stamped its own `DPC_END` with `rspGuestNow()` it read a stale, host-quantized
value. `exactEnd/exactLeft/exactPub/exactOn` + `exactCycles()`/`publishExact()` (rsp.hpp)
expose the live retired count without an atomic RMW in the 11-15 M-per-run DPC poll path;
`Memory::rspGuestNowAt(cycles)` is the same guest instant taken with a caller-supplied count.
`Rsp::mtc0` calls `publishExact()` first, so every MMIO write from microcode is stamped exact.

## The hang: ROOT CAUSE FOUND

Symptom: ~3 runs in 6 of `KESTREL_MAXFIELDS=300` DK64 never exited; `[rsp] microcodigo lleva
40M instrucciones sin BREAK (pc=0x2b0)`, RSP 0% in the GUI.

The `[dpwait]` dump (`KESTREL_RSPHANG=1`, `Rsp::dumpDpWait`) settled it. At the hang:

    [dpwait] now=444082791 cart=402314199 schedEnd=401113229 sub=218 compAt=218
             rspBusy=1 pend4=0 cur=4cc2c8
    imem 2a0: mfc0 t3, DPC_CURRENT / sub t3,t3,t8 / blez t3,exit / sub t3,t3,s3 / blez t3,2a0

with `t8 = 0x004cc1b8`, `s3 = 0x110`, `DPC_CURRENT = 0x004cc2c8 = t8 + s3`. That is the F3DEX2
FIFO flow control: it spins while `t8 < CURRENT <= t8 + s3`, i.e. while the RDP read pointer
is inside the region it wants to overwrite. Everything was already drained in guest time
(`sub == compAt`, `pend4=0`), so `CURRENT` was pinned at the last span's end and nothing could
move it — **the only thing that unpins it is the CPU installing the next command buffer**
(`DPC_START = 0x004ea720`, `DPC_END = 0x004ea720`, visible in `KESTREL_DPSYNCLOG` at every
task boundary). And `cart` was frozen: the CPU had already stopped at the field cap.

So the microcode spin was CORRECT guest behaviour — the R4300 had been switched off underneath
it. The bug was on the host side: `Memory::stopRcpThreads()` joins the RSP worker, and
`Rsp::step` had no way out of a task that never breaks, so the join never returned.

**Fix:** `Rsp::hostStop` (atomic, checked once per 8192-instruction chunk), set by
`stopRcpThreads()` before `rspStop`/notify. It is not a guest HALT and it is not a budget cap:
it is the power going off, which is the only thing that ends this spin on real hardware too.
6/6 DK64 runs now exit in 4-5 s.

## Determinism status: SOLVED (DK64, 300 fields, `KESTREL_FIELDTRACE=1`)

| build | runs | field trace |
|---|---|---|
| `build/` (SoftRDP), threaded | 4/4 | byte-identical, md5 `b43236f9c027f4a0cbaf19df040e0ffc` |
| `build-prdp/` (Parallel-RDP), threaded | 6/6 | byte-identical, md5 `d3bee5263f26e4dca3519e277220e090` |
| `build-prdp/`, lockstep | 2/2 | byte-identical, md5 `ce4ec2bc2d6298cb69d270607db199ee`, and identical to the same build run with `KESTREL_RSPIDLE=0` |

Identical means every column, `rsp=` included. `[det]` is identical too: `rspCycles=60771479`,
`stale=0`, `ooo=0`, `spArm=173/0 tarde`, `dpArm=56/0 tarde`, `park=48/0`.

Before this work the guest columns matched only through f=206 and `rsp=` drifted over
104-115 M.

**Ordering hypothesis: DISPROVEN.** Instrumented (`ooo=` in `[det]`): a span whose kick instant
falls before the previous span's kick instant. **0 in every run.** The SP guest barrier already
holds `cartNow() <= spBarrierAt()`, so the CPU can never overtake the RSP, and submits always
reach `rdpMx` in guest order. The FIFO does not need reordering.

**The real leak was stale schedule reads.** Second counter (`stale=` in `[det]`): a span whose
kick instant falls *before the highest guest instant at which somebody had already queried the
schedule*. 42-47 per 300 fields, every single one from the CPU thread (`C.../R0`), i.e.
essentially every buffer the CPU installs.

The mechanism is the mirror image of the hang:

- The SP barrier pins the CPU *behind* the RSP in guest time, by construction. So during a
  graphics task the RSP guest instant is always the larger one.
- The microcode polls `DPC_CURRENT` from that later instant. 13.9 M polls per 300 fields, and
  `open=235` of them see an *open* span: virtually all of them see a **drained** FIFO. That is
  the F3DEX2 flow-control spin waiting for the CPU to install the next command buffer.
- The CPU then installs it, with `kick = cartNow()`, which is *earlier* than instants already
  answered. Every answer given in that window was computed against a FIFO that was missing
  work belonging to it.

So the RSP was reading the RDP future, and how far into that future it got before the CPU
landed the write was pure host timing. That was the whole of the remaining divergence, and it
matched the symptom exactly: `ret`/`ops` bit-identical every field, only *which field* an SP/DP
interrupt landed in moved.

## The fix: park the RSP on the FIFO wait

`Rsp::idleSkip` (rsp.cpp) + `Memory::rspParkWait` (memory.cpp).

The F3DEX2 wait loop at IMEM 0x2a0 is five instructions with **no side effect**: it reads
`DPC_CURRENT`, subtracts, branches. The only thing it produces is RSP clock. And while the
engine is drained, `DPC_CURRENT` is a *constant* (the last span end address) that cannot change
until the CPU installs the next buffer. Since a submit is stamped `kick = cartNow()` and
`cartNow` is monotonic, no future span can start before the instant the CPU has already reached.

So the loop is modelled as what it is:

1. **Recognise it from the stream.** Same PC, same FNV hash of `r[1..31]`, same value returned,
   same cycle distance as the previous DPC read, body between 2 and 64 cycles, engine drained at
   `now` (`Memory::dpDrainedAt`). No pattern is hardcoded: any side-effect-free poll loop on a
   constant `DPC_CURRENT` qualifies, and anything that touches a register drops out immediately.
2. **Park.** `rspPark` is published, which lifts the SP guest barrier (`spBarrierEff` returns
   `rspPark + kParkLead`) and the pace throttle, because a parked RSP provably cannot raise an
   interrupt, write `DPC_END` or touch memory. The CPU runs free.
3. **Wake on the guest event.** `dpScheduleSpan` hands the parked RSP the `kick` of the span it
   just filed and notifies `parkCv`. That instant is guest time, so it is deterministic.
4. **Charge the whole loop iterations that fit.** `k = rcpOpsToCycles(tgt - now) / len`, then
   `cyclesRun += k * len`. Exactly the cycles emulating them would have produced.

Two details carry the determinism:

- **The jump target is never `cartNow()`.** That is host time: how far the CPU happened to get
  in that particular run. It is always either the `kick` of the next span or `now + kParkLead`,
  both guest instants. `kParkLead = 1 << 24` ops is also the cap on how far the CPU may run
  while the RSP is parked, so even the fallback lands somewhere exact.
- **The host lifeguard resets on progress**, the same rule as `rspPace`: what is not tolerated
  is the CPU *stalling*, not the CPU being slow. DK64 has two stretches (around f=137 and f=140)
  where the CPU crawls at ~1.2 M ops/s; a flat 200 ms cut fired there and was the last column of
  the trace that would not settle. With the reset, `park=48/0`: zero waivers.

`KESTREL_RSPIDLE=0` turns the whole thing off (host shortcut only: the framebuffer md5 must not
move). **Threaded only.** In Lockstep both chips share a thread, so parking the RSP parks the
only thread there is and the CPU that would wake it can never run; DK64 stopped dead at f=67.

Two races had to be closed before the park was actually deterministic. At 3 runs they were
invisible; at 6 runs one in three or four diverged, always with `spArm=.../1 tarde`.

- **The wake publication race.** `dpScheduleSpan` looks at `rspPark` to decide whether to leave
  a wake instant behind, so a span filed between `idleSkip` seeing a drained engine and
  `rspParkWait` publishing the park left none: the notice was lost and the RSP slept until the
  cap or the *next* span, depending on how the two threads happened to cross. `idleSkip` now
  captures `dpSubSeq` at the same point where it checks `dpDrainedAt` and passes it as `seq0`;
  `rspParkWait` checks by hand — before parking and on every turn — whether that counter moved,
  and if it did takes `dpJobStartG[seq0]`, which with a drained engine *is* that span's kick.
  The acquire load of `dpSubSeq` is what makes `dpScheduleSpan`'s writes visible. Counter:
  `rspParkMiss`.
- **The SP barrier stayed open through the whole wake latency.** `spBarrierEff` returned
  `rspPark + kParkLead` for as long as `rspPark` was set — but the wake instant is published by
  the *CPU thread itself* when it files the span, so from that moment the RSP is going to resume
  exactly where the CPU is and the barrier has to take over again. Without that the CPU kept
  running free until the worker noticed, the task ended at an instant the CPU had already passed,
  and the SP end deadline was born late. `spBarrierEff` and the `rspPace` early grant now only
  lift the throttle while `rspParkWake` is still zero.

One more thing was not a race at all: `[ft]` was written with `fprintf` and came out torn when
another thread printed at the same time. A split line reads as a divergence that is not there.
It is now formatted into a `char[192]` and released with a single `fwrite`.

## The read rendezvous is now OFF by default

`Memory::dpReadSync` (`KESTREL_DPRDV`) solved the same problem from the expensive side: make
the RSP wait for the CPU on *every* poll. It worked (`stale=0`, trace identical bar `rsp=`) but
it cost a thread hand-off per poll, and its `kRdvLead` window is what made 10-13 SP deadlines
per run be born late. Measured, DK64 300 fields, `build/`:

| mode | wall | `spArm` | trace |
|---|---|---|---|
| park only (default) | 9 s | 173/**0** late | identical 4/4 **including `rsp=`** |
| park + rendezvous | 11 s | 173/11 late | identical only with `rsp=` stripped |
| neither (old behaviour) | 6 s | 173/0 late | diverges from f=206 |

It stays available by hand because it covers a case the park does not: polling `DPC_CURRENT`
with a span *open* and ahead of the CPU. In DK64 that is ~235 polls out of 13.9 M and none of
them diverge, but another game could live there.

## Savestates

The new schedule is **not serialised**. It is derived timetable, not machine state, and a state
is always taken with the RCP at rest (`System::quiesceRcp`: engine drained, task finished). What
the guest can actually see — `DPC_CURRENT`, `DPC_STATUS`, the FIFO resume pointers — already
travels in the file on its own.

It is **reset on load** instead, by `Memory::rcpSchedReset` from `afterLoad`: the 256-entry ring,
`dpSchedEnd`, `dpSubSeq`/`dpCompSeq`, `dpMaxQuery`, `dpLastKick`, `rcpPend`, both deadlines,
`rspRdvAt`/`dpRdv`, `rspPark`/`rspParkWake` and the RSP's idle-loop signature; the SP barrier
anchors (`spKickOps`/`spKickCycles`) are re-tied to the clock that just came in. Those are guest
instants belonging to a game that has stopped existing, and against the restored clock they are
garbage — a `dpSchedEnd` from the future leaves the engine permanently "busy" and pushes every
new span behind it. With the ring at zero `dpcCurrentFor` falls through to `rcp.dpc_current`,
which is in the file.

`quiesceRcp` also wakes a parked RSP first (`rspParkNudge`, with the same target the cap would
have given it, so it is still a guest instant). Otherwise the CPU stops dead, nobody files the
span that would wake it, and the wait would only end through the 200 ms lifeguard.

## Cost: none. The park is now the fastest of the three configurations

13.9 M emulated polls to 5.7 k. 48 parks per 300 fields, covering 6.62 M loop iterations.

The first measurement said wall went 6 s to 9 s (SoftRDP) / 8 s (Parallel-RDP), and blamed the
park for serialising the two threads at task granularity. That attribution was wrong, and it was
never measured. The park serialises nothing: what collapsed was the **dynarec**.

`Memory::rcpDueIn` computed the SP-barrier deadline (`rcpPend` bit 8) from `spBarrierAt()` =
`spKickOps + rcpCyclesToOps(rsp.cyclesRun - spKickCycles)`. While the RSP is parked its
`cyclesRun` is frozen by construction, so that value stays pinned behind the guest clock and
`rcpDueIn` returned **0** for the whole park. `jitTryBlock`'s `if(siDue <= kTicks) return 0;`
then declined *every* block, the chained grant (`jitGuard`) collapsed, and the CPU dropped to
single-instruction interpretation — precisely in the window where it is the only thread that can
unblock the scene, since the RSP is waiting for the buffer the CPU has to install.

The value that decides where the CPU actually stops is not `spBarrierAt()` but the one
`spBarrierWait` enforces, `spBarrierEff()`, which while parked with no wake published is
`rspPark + kParkLead`. One line:

```cpp
if(pend & 8u) {
  u64 b = spBarrierEff();          // was: spBarrierAt()
  u64 e = b > now ? b - now : 0;
  if(e < d) d = e;
}
```

DK64 PAL, 300 fields, `build-prdp`, threaded:

| | before the fix | after |
|---|---|---|
| park ON | 7.33-7.71 s | **2.83 s** |
| park OFF (`KESTREL_RSPIDLE=0`) | 4.0-4.5 s | 4.01 s |
| `[block]` with park ON | `cpuWait 2.7% / rsp busy 2.2% parked 76.5% / rdp 3.9% / real CPU: cpu 99.8%` | `cpuWait 7.3% (pace 0.0 spBar 0.3 dpBar 2.2) / rsp busy 5.6% parked 47.0% / rdp 10.0% / real CPU: cpu 100.3%` |

Park-on is now faster than park-off *and* faster than the 6 s non-deterministic baseline from
before any of this. The overlap is not recovered, it is reversed.

Determinism is untouched: 3/3 byte-identical traces, md5 `ccf3fd5bf216c429db341ddc5164df31`,
`[det]` pinned (`rspCycles=60771443`, `spArm=173/0 late`, `dpArm=56/0 late`, `ooo=0`, `stale=0`,
`idle=48/6621800`, `park=48/0/0`). Against a deliberately rebuilt pre-fix binary
(`d3bee5263f26e4dca3519e277220e090`, `rspCycles=60771479`, `idle=48/6621806`) exactly 44 of 300
lines differ, from f=257 on, and **only in the `rsp=` column**; stripping that column gives both
runs the same md5 `417ae0aa5c60abf6dae491c9cd4af994`, and every guest-visible column (`ret`,
`ops`, `gclk`, `sp`, `dp`, `flips`, `syncs`, `org`, `mi`) is byte-identical. The 36-cycle shift
comes from JIT block length changing the quantisation of the `kick` instant the CPU stamps on
the spans it files, which feeds `k = rcpOpsToCycles(tgt - now) / len` inside `idleSkip`.

## Diagnostics added

- `KESTREL_DPSCHED=1` — `[ds]`, the whole guest schedule per span: sequence, filing thread,
  `kick`/`t0`/`t1`, the FIFO addresses and the modelled cost. Two runs of the same binary must
  produce the same file line for line. Guest instants only -- the companion `[ds+]` line, which
  printed `cartNow()` and RSP cycles, is gone: host time has no place in a trace that is
  compared across runs.
- `KESTREL_RSPHANG=1` — now also `[dpwait]` per watchdog tick (`Rsp::dumpDpWait`): guest clock,
  `dpSchedEnd`, `sub`/`compAt`, `rspBusy`, `pend4`, the modelled `DPC_CURRENT` and the last
  three spans in the ring.
- `KESTREL_DPSYNCLOG=1` — `[dpwr]`/`[dpkick]`, every DPC register write. Pre-existing.
- `[det]` gains `ooo=%u(C%u/R%u)` (out-of-guest-order submits), `stale=%u(C%u/R%u)`
  (submits landing before an already-answered query instant), `rdv=%llu/%u` (read rendezvous
  / waivers), `idle=%llu/%llu` (loop skips / iterations charged) and `park=%u/%u` (parks /
  host waivers). Always on, cost is one relaxed store on the poll path. `ooo`/`stale`/`rdv`
  are TEMPORARY; `idle`/`park` are worth keeping -- a non-zero park waiver count means the
  run is not reproducible.
  `rspParkMiss` counts parks resolved by the hand-checked `dpSubSeq` path (the wake
  publication race); it is not in `[det]` yet.
- `KESTREL_DPSYNCLOG=1` also prints `[park] salvavidas ...` when a park ends on the host
  lifeguard instead of on a guest instant.
- **`[block]`** — always on, one line at the end of a run: the wall-clock split of the CPU
  thread and the workers (cpuWait, broken into pace / SP barrier / DP barrier; RSP busy; RSP
  **parked**; RDP busy) plus the **real CPU time** of the three threads via `GetThreadTimes`.
  This is the view that separates "emulating is expensive" from "a thread is asleep waiting for
  another one": a CPU thread at 100 % of wall and 25 % of CPU is not emulating. It is what
  caught the `rcpDueIn` bug — nothing was busy and 63 % of the wall was unaccounted for.
- **`rspParkNs` / `cpuCpuNs`** — the park sleep happens *inside* `rsp.step()`, so without
  subtracting it `rspBusyNs` counts a sleeping thread as work (DK64 read "rsp 92 % busy" at 2 %
  real CPU). `rspWorkerLoop` subtracts it; `sampleWorkerCpu` now also samples the CPU thread.
- **`KESTREL_PARKLOG`** — `[pk]` per park: guest `now`, wall ms, exit route
  (`wake`/`miss`/`cap`/`lifeguard`), the jump distance, `cartNow()` and the spin count. Opt-in,
  same class as `KESTREL_DPSYNCLOG`. All 48 DK64 parks leaving `via=wake` with `spins=3..10` is
  what pointed at the CPU thread rather than at the park itself.

## And the dynarec was braking on the parked RSP too (2.98 s → 1.71 s)

Same shape as the `rcpDueIn` bug, one layer up. The JIT prologue's inline fast path
(`src/cpu/jit.cpp` ~1181-1245) checks four things before jumping to the block body and chaining
on: the remaining grant (`jitGuard`), `MI_INTR & MI_MASK`, the `timerIntr` latch, and a byte for
the RSP. Any failure returns to the trampoline, which is a Win64 call per block. That byte was
`Rsp::running`.

`[tramp] guard/MI/timer/rsp/otro` gave it away: **22 M** RSP bounces per 300-field DK64 run,
while `[block]` for the same run said the RSP was **parked 47.1 % of wall**. `Rsp::running`
stays set for the whole park, so the chain broke on every link exactly in the window where the
CPU is the only thread that can move the scene — it is the CPU that files the span whose `kick`
wakes the RSP.

And there the brake regulates nothing: a parked RSP runs no microcode, writes no MMIO and cannot
raise an interrupt, and the CPU is still bounded by the SP guest barrier (`spBarrierEff()` →
`rspPark + kParkLead`) and by `rcpPace`, both in guest clock. The brake is needed when the RSP
*is* running — see the long prologue note in `jit.cpp` — and only then.

New flag `Rsp::brake` = `running` MINUS the park, in the same cold 64-byte line as `running`
(which is `alignas(64)` and deliberately isolated: the CPU thread reads it constantly and
cross-core invalidation once cost 18 % of the whole emulator). Set/cleared at the five places
where `running` genuinely changes, and additionally released by `rspParkWait` before it publishes
the park and restored before it withdraws it. The prologue and the trampoline classification read
`brake`; `jitReenterProceed`'s Lockstep check still reads `running`, which is what it wants.

| DK64 PAL, 300 fields, `build-prdp`, threaded + JIT | before | after |
|---|---|---|
| wall | 2.98 s | **1.71 s** (−43 %) |
| `[tramp] rsp=` | 22 M | 0 |
| field trace | `ccf3fd5bf216c429db341ddc5164df31` | **same**, 3/3 byte-identical |
| Lockstep | `ce4ec2bc2d6298cb69d270607db199ee` | **same**, 2/2 |

`[block]` after: `pared 1.71 s | cpuWait 14.4% (freno 0.0% barSP 0.6% barDP 4.3%) | rsp ocupado
9.6% aparcado 10.8% | rdp ocupado 16.8% | CPU real: cpu 99.5% rsp 6.3% rdp 14.6%`. `[det]` is
unchanged in every column except the `await=` telemetry counter.

## The field trace was carrying four host-timed columns

`KESTREL_FIELDTRACE=1` exists to md5 two runs of the same binary and find the first field that
differs. Four of its eleven columns could not be used for that.

`sp=`/`dp=` came from `spArms`/`dpArms`, which are incremented in `spEndArm`/`dpEndArmAt` — and
in threaded mode those are called by the RSP/RDP worker when it finishes the work *in wall time*.
The deadline they arm is in guest clock and the CPU thread publishes it at the exact instant it
is due, so the guest never sees a difference; but the counter ticks in one field or the next
depending on how the threads happened to cross. New `spRets`/`dpRets` tick where the interrupt
becomes visible to the guest: `rcpFlushPending` (the normal path, CPU thread) plus the four
direct publication sites. `[det]` keeps showing `spArm=`/`dpArm=`, which is where that datum
belongs.

`rsp=`/`gclk=` are `Rsp::cyclesRun` and `rcp.rdpGclk` — worker progress, unfixable by
construction. Moved to `KESTREL_FIELDTRACE=2`. Level 1, the one that gets diffed, carries only
columns the guest can observe.

DK64 PAL 300 fields now gives `4fc7dc59e262d5a8a0cd2c89712b8ae3` 3/3 under Parallel-RDP **and**
2/2 under SoftRDP — the same md5 from both backends — and SM64 NTSC gives
`a022f09184a1fa61917565cc7909630f` 6/6.

A false alarm worth writing down: SM64 looked non-deterministic, 2 distinct traces in 6 runs. It
was not. The first run did not find `Super Mario 64 (USA).eep` (it creates it on exit) and the
rest did, so they started from different initial state — with no save the game takes 72 more
fields to reach its first graphics task (71 swaps against 95 in 300 fields). **Deleting the save
file before each run is part of the experiment.** And `ret`/`ops` are useless as a progress
signal: they are `viFields * instructions-per-field`, i.e. the clock, and come out identical even
when the game is stuck.

## The guest idle thread is now charged in bulk

`beq $0,$0,-1` with a `nop` delay slot is libultra's idle thread: no register written, no memory
touched, nothing produced but Count. With `KESTREL_PCSAMPLE=0x80000000` (which now adds a
per-instruction 1024-slot histogram of that page when the value is a KSEG0 address) it is
**251.5 M of DK64's 402.3 M** guest instructions over the first 300 fields — 62.5 %, all at
`0x80000a08`.

`CPU::jitIdleSkip` recognises the shape in the instruction stream and charges the whole run at
once (retired, Count, Random — the same commit the trampoline does). What keeps it honest is the
limit: it is EXACTLY the permit `jitReenterProceed` grants a linked chain (Compare edge, SI
deadline, RCP deadline, `jitOpsBudget`, `rcpPace` in threaded mode, `kGuardMaxOps` = 4096). The
instant each event is next examined does not move; only the emulation between two looks goes
away. It refuses when `Status[2:0] != 1` (no interrupt can leave the loop = the guest is really
hung, and skipping its clock would hide that) and in threaded mode without RCP deadlines (the
only mode where a worker publishes `MI_SP`/`MI_DP` on wall time with no guest-clock bound).

`KESTREL_CPUIDLE=0` turns it off and everything must come out identical — it is a host shortcut,
not a semantic one. `KESTREL_JIT_STATS` prints `[ocioso] saltos=/ops=`.

| SM64, 200 swaps (597 VI fields = 9.96 s of video), threaded-jit, min of 3 | wall | realtime |
|---|---|---|
| `KESTREL_CPUIDLE` on (default) | **5.13 s** | 194.2 % |
| off | 5.98 s | 166.6 % |

Gates: `gate_all` rc=0 458 s, `gate_prdp` rc=0 350 s, eight modes `0/3721 · 0/2 · 0/6`, sm64
`d35bd8aa9b13d459ce9332c07a79a53a` / `b5521b24d8fc280fbf102df22d7d30cb`, krom interp 371/371
88.73 regress=0, krom prdp 371/371 89.27 regress=0 improve=2 (the two animated Cube cases).
