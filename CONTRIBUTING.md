# Contributing to kestrel64

Issues, pull requests and discussions are all welcome. This file says what the project
holds itself to, so a patch does not get bounced over a rule nobody wrote down.

## Reporting a bug

Open an issue. The single most useful thing you can include is **which build and which
knobs**, because most of the interesting behaviour is threading-dependent:

- the `KESTREL_*` environment variables you had set (paste the whole set)
- whether it reproduces with `KESTREL_THREADS=0` (lockstep) as well as threaded
- RDP backend: parallel-rdp (`-DKESTREL_PRDP=ON`) or the software path
- the game, and where it happens

A bug that appears threaded but not in lockstep is a determinism bug and is treated as a
high-severity one — say so in the title, it will get looked at first.

Do **not** attach ROM images or link to them. This repository ships no game data and does
not want any.

## Building

Toolchain is clang from MSYS2 CLANG64; the build is CMake + Ninja.

```bash
export PATH=/c/msys64/clang64/bin:$PATH
cmake -B build-prdp -G Ninja -DKESTREL_PRDP=ON
cmake --build build-prdp -j8
```

`sh scripts/release.sh` builds every tree and packages `dist/`.

## The rules a patch is held to

**1. Genuine hardware semantics only.** A fix has to be justifiable as what the real RCP
does. Never special-case a test, a ROM, a checksum or a frame number to make a suite go
green. A patch that only moves a score is not a fix.

**2. Lockstep is the oracle.** `KESTREL_THREADS=0` runs the RCP in lockstep and defines
the correct answer; the threaded path must produce the identical state hash and the
identical framebuffer md5. If your change makes the two diverge, it is wrong, even if the
threaded result looks better.

**3. Determinism is not negotiable.** Same ROM plus same input has to give the same result
every run. No wall-clock, no thread-arrival order and no host timing may reach guest-visible
state. Host-side knobs that only trade CPU time (spin lengths, poll spacing, priorities) are
fine precisely because they cannot move a guest-visible date.

**4. Performance claims need measurements.** The host wall clock on a loaded desktop is
worth about ±2 %, so a single before/after pair proves nothing. Interleave the two builds
and take the minimum of at least three runs per game, and discard the first run of a freshly
linked binary (cold page-in inflates it systematically). Negative results are valuable and
get written down in `docs/STATUS.md` next to the positive ones.

## Before opening a pull request

Run the gates:

```bash
sh scripts/gate_all.sh     # n64-systemtest (interpreter and JIT) + SM64 in six modes + krom suite
sh scripts/gate_prdp.sh    # the same against the parallel-rdp backend
```

and check that lockstep and threaded still agree on the state hash and framebuffer md5.

Record the wall-clock time of the gate run in `docs/baselines/timings.md`. This is not
bookkeeping for its own sake: a run that takes far longer than the recorded baseline is a
hang, and the fix is to bisect it, never to raise the timeout.

## Where the context lives

- `docs/STATUS.md` — running log: what was tried, what it measured, what was reverted and why
- `docs/baselines/timings.md` — wall-clock baselines for every gate and benchmark

Reading the last few sections of `STATUS.md` before proposing a performance change will
usually tell you whether the idea has already been measured.

## Style

Code and commit messages in English. Comments explain *why*, not *what* — the code already
says what it does; a comment earns its place by recording the hardware behaviour or the
measurement that forced the shape of the code.
