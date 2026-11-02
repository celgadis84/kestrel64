# Contributing to kestrel64

Issues, pull requests and discussions are all welcome. This file says what the project
holds itself to, so a patch does not get bounced over a rule nobody wrote down.

## Reporting a bug

Open an issue. The single most useful thing you can include is **which build and which
knobs**, because most of the interesting behaviour is threading-dependent:

- the `KESTREL_*` environment variables you had set (paste the whole set)
- whether it reproduces with `KESTREL_THREADS=0` (lockstep) as well as threaded
- RDP backend: GPU-RDP (`-DKESTREL_GPU=ON`) or the software path (SoftRDP)
- the game, and where it happens

A bug that appears threaded but not in lockstep is a determinism bug and is treated as a
high-severity one — say so in the title, it will get looked at first.

Do **not** attach ROM images or link to them. This repository ships no game data and does
not want any.

## Building

Toolchain is clang from MSYS2 CLANG64; the build is CMake + Ninja.

```bash
export PATH=/c/msys64/clang64/bin:$PATH
cmake -B build-gpu -G Ninja -DKESTREL_GPU=ON
cmake --build build-gpu -j8
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
get written down in the log (`docs/history/STATUS-2026.md`) next to the positive ones.

## AI-assisted code

Patches written with an AI assistant are welcome and held to the same rules, plus these,
which name the patterns such code tends to add. Each one has turned up in this tree.

- **No duplicated helpers.** Before writing a byte-swap, sign-extend or coherent-read
  lambda, look for the existing one (`loadBe32` in `src/core/types.hpp`, `sext` in
  `src/rdp/rdp.cpp`). Before: a local `auto sext = [](u32 v, int b) {...}` in two functions.
  After: one file-scope `static inline` used by both.
- **No dead code "for later".** An emitter nobody calls (the old `emitVecRight` in the RSP
  JIT), a member set but never read, a variable kept to silence a warning: delete it. Git
  remembers.
- **No compatibility shims for things that are gone.** When a backend or option is removed,
  remove its env var, its CMake alias and its launcher mapping in the same change; do not keep
  an alias that silently maps the old name to the new behaviour.
- **No defensive checks on trusted paths.** Internal callers that the code already
  guarantees (a non-null `mem`, an index masked to range) do not get a second check; validate
  at the boundary (ROM header, savestate, MCP input) instead.
- **Comments record why, not what.** `// increment the counter` above `n++` goes. A comment
  stays when it names the hardware behaviour, the measurement or the failure that forced the
  code into its shape, with a date when it is a measurement.
- **New env vars are documented.** Every `KESTREL_*` read by the code has a line in
  `docs/ENV-VARS.md`, saying its default and whether it can change guest-visible state.
- **Claims come with numbers.** "Faster" needs the interleaved measurement from rule 4;
  "same behaviour" needs the state hash (`scripts/statehash_ab.sh`).

## Before opening a pull request

Run the gates. `docs/TESTING.md` explains how to get the public test suites they use and
what runs without any ROM; commercial ROMs are optional, and their checks print `SKIP` when
not configured.

```bash
sh scripts/gate_all.sh     # unit tests + n64-systemtest and SM64 in every mode + krom suite
sh scripts/gate_gpurdp.sh  # the same against GPU-RDP (Vulkan compute), the default backend
```

and check that lockstep and threaded still agree on the state hash and framebuffer md5.

Record the wall-clock time of the gate run in `docs/baselines/timings.md`. This is not
bookkeeping for its own sake: a run that takes far longer than the recorded baseline is a
hang, and the fix is to bisect it, never to raise the timeout.

## Where the context lives

- `docs/ARCHITECTURE.md` / `docs/ARQUITECTURA.md` — architecture overview in English and
  Spanish. Change both in the same commit and bump their `Synced with:` line together;
  `scripts/check_docs_sync.py` (run in CI) fails if their section structure diverges
- `docs/TESTING.md` / `docs/PRUEBAS.md` — how to reproduce the verification; same pairing
  rule as the architecture overview
- `docs/STATUS.md` — current state per subsystem (short; updated in place)
- `docs/history/STATUS-2026.md` — running log: what was tried, what it measured, what was
  reverted and why (new entries are appended at the end)
- `docs/baselines/timings.md` — wall-clock baselines for every gate and benchmark

Searching the log for the subsystem before proposing a performance change will
usually tell you whether the idea has already been measured.

## Style

Code and commit messages in English. Comments explain *why*, not *what* — the code already
says what it does; a comment earns its place by recording the hardware behaviour or the
measurement that forced the shape of the code.

Many existing comments are in Spanish; they stay as they are (rewriting them buys nothing
and makes huge diffs). New comments are written in English. Every header that another
subsystem includes opens with an English block: responsibility, invariants and the threads
that touch it. Keep that block true when you change the contract.
