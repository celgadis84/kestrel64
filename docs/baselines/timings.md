# Gate wall-clock baselines

Reference run times for `scripts/validate.sh`. Their purpose is not performance
tracking — it is **hang detection**. A gate that normally finishes in 17 s and is
still running at 300 s is deadlocked, not slow, and should be bisected as a bug
rather than retried with a bigger timeout.

Host: i7-870 (Nehalem, 4c/8t), Windows 11, MSYS2 CLANG64 build, `build/` (SoftRDP,
PRDP off). Numbers are wall clock, krom with the default 4 parallel jobs.

Measured 2026-08-18, commit = DPC counters + RSP threaded launch/PC-publication fix.

| Gate | interp | JIT | threaded |
|------|--------|-----|----------|
| systemtest (`n64-systemtest.z64 --run`) | 16-17 s | 25 s | 17 s |
| krom 371-ROM suite (4 jobs) | 139-143 s | 91-92 s | 140 s |
| SM64 300M ops (framebuffer md5) | 20 s | 15 s | 16 s |
| **`validate.sh all` total** | **~180 s** | **~132 s** | **~175 s** |

Not part of `validate.sh` (run it by hand when the RDP cost model or the depth path
changes — see `docs/RDP-TIMING.md`):

| Battery | interp |
|---------|--------|
| RDP-Timing-Tests sweep (`rdp_fill_timing.z64`, `TOTAL_RUNS=8`, 100 configs) | 97 s |

systemtest's own internal report ("Finished in N s") is ~7 s in every mode; the rest
of the wall time is ROM load plus boot. The historical 6.86 s quoted throughout
STATUS.md is that internal figure, not wall clock — do not compare the two.

`validate.py` default timeouts: `--st-timeout 300`, `--sm64-timeout 600`,
`--timeout 90` (per krom ROM). Those are ~17x the measured systemtest time, so a
timeout there always means a genuine hang.

## Known hang signature (fixed 2026-08-18)

`systemtest --mode threaded` froze at *"Running RSP VRSQ (all 16 bit values)"* after
~20 s of progress and never advanced. Two distinct threaded-only bugs, both real
RCP-semantics violations, not test-specific quirks:

1. **Dropped RSP launch.** A lone CLEAR_HALT write was gated on the emulator's
   `rspBusy` worker flag, so a launch issued while the worker was still winding down
   from the previous task was silently discarded — the task never ran and the CPU
   polled a BREAK that never came. Launches are now gated on the RSP's actual HALT
   bit (the hardware condition) and ordered behind the wind-down (`rspAwaitIdle`).
2. **PC writeback published after HALT.** `Rsp::step()` set `HALT|BROKE` at BREAK and
   only afterwards wrote `sp_pc` back. The CPU treats HALT as "task over" and
   immediately writes the next task's SP_PC, which the late writeback then clobbered
   — the next task started at the old BREAK and produced nothing (`a=0x0` in
   `RSP VRCP (all 16 bit values)`, ~1 failure per 65536 iterations, nondeterministic).
   BREAK now only latches `broke`; the status publish happens after the PC writeback.

`Rcp::sp_status` was also made `std::atomic<u32>` — one register updated by both the
CPU (control writes) and the RSP worker (BREAK) cannot be a plain `u32` without
losing updates.
