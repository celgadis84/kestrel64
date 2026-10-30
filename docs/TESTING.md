# kestrel64 — Testing

Synced with: f6d6c24

How to reproduce kestrel64's verification on your own machine. Every check is an oracle (a
test ROM that reports its own result, a hardware capture, or a stored baseline), never a
visual judgement. The Spanish version, `docs/PRUEBAS.md`, has the same sections in the same
order; change both in the same commit (`scripts/check_docs_sync.py` checks it in CI).

The repository ships no ROMs. The test suites below are public projects: you fetch or build
them yourself and point kestrel64 at them. Commercial games are only ever optional local
checks; nothing here requires one.

## 1. Three levels

| Level | Needs | What it proves |
|---|---|---|
| Unit tests | nothing but a build | components in isolation (RSP vector unit, saves, PIF, cheats, archives, rewind, watchpoints, Transfer Pak, wild memory) |
| Public test suites | the ROMs of section 4 | CPU/RCP correctness, RDP accuracy against hardware captures, RDP timing |
| Local commercial ROMs | your own dumps | end-to-end determinism (framebuffer md5 after N fields), performance benchmarks |

CI (`.github/workflows/build.yml`) runs only the first level: it builds the tree and runs the
nine unit tests. There is deliberately no CI job that downloads or runs test ROMs.

## 2. Without ROMs: unit tests

The unit tests live in `test/` and are CMake targets excluded from the default build. Each one
prints `ALL PASS` and exits 0, or exits non-zero on the first failure.

```sh
export PATH=/c/msys64/clang64/bin:$PATH
cmake --build build -j8 --target rsp_test save_test cheat_test archive_test wildmem_test \
      rewind_test watch_test pif_test tpak_test
./build/rsp_test.exe
```

`scripts/gate_all.sh` runs all nine before anything else.

## 3. Local configuration

Copy `scripts/local.env.example` to `scripts/local.env` (ignored by git) and edit it. Lines
are `KEY="value"`; an environment variable with the same name overrides the file.

| Key | Meaning |
|---|---|
| `PY` | Python used by the `.sh` scripts. It needs numpy and Pillow for the krom gate; on Windows use the Windows Python, not MSYS's. |
| `KESTREL_TESTROMS` | folder holding the test suites (default `../test_roms`) |
| `ROM_SM64`, `ROM_PD`, `ROM_DK64` | optional commercial ROMs (section 7); empty means the checks that need them are skipped |
| `ARES_EXE` | optional, only for the ares comparison in `scripts/fidelity.py` |

`scripts/validate.py` uses `build/kestrel64.exe` unless `KESTREL_EXE` says otherwise.

## 4. Public test suites

| Suite | Where to get it | Place it at | Run by |
|---|---|---|---|
| n64-systemtest | [lemmy-64/n64-systemtest](https://github.com/lemmy-64/n64-systemtest): build it as its README explains | `$KESTREL_TESTROMS/n64-systemtest.z64` | `validate.py systemtest` (gate) |
| PeterLemon N64 tests ("krom") | [PeterLemon/N64](https://github.com/PeterLemon/N64): clone it; the assembled ROMs sit next to their hardware captures (`.png`) | `$KESTREL_TESTROMS/PeterLemon-N64/` | `validate.py krom` (gate) |
| RDP timing tests | [Thar0/RDP-Timing-Tests](https://github.com/Thar0/RDP-Timing-Tests): build `rdp_fill_timing.z64` as its README explains | `../rdp-timing-tests/` next to the repository | `gate_quick.sh thar0` |
| Dillon's n64-tests | [Dillonb/n64-tests](https://github.com/Dillonb/n64-tests) | `$KESTREL_TESTROMS/dillon-n64-tests/*.z64` | `fidelity.py --suite dillon` (score, not a gate) |
| libdragon test ROM | [DragonMinded/libdragon](https://github.com/DragonMinded/libdragon): build `tests/testrom_emu.z64` with the libdragon toolchain | `$KESTREL_TESTROMS/libdragon-tests/testrom_emu.z64` | `fidelity.py --suite libdragon` (score, not a gate) |

What each one checks:

- **n64-systemtest** covers every CPU, COP0 and COP1 opcode and exception case, plus RSP and
  RCP behaviour and timing. The gate reads the "Failed X of Y" summary the ROM prints and
  passes only with zero failures; the current figure is in `docs/STATUS.md`.
- **krom**: every ROM that ships a same-named `.png` hardware capture (371 today) is run until
  its framebuffer stops changing, then compared with the capture. ROMs without a capture are
  skipped.
- **RDP timing** measures fill-rate cycles on the console; `scripts/rdptiming.py compare`
  reports the error of kestrel64's RDP cost model against it.
- **Dillon** and **libdragon** feed `scripts/fidelity.py`, an on-demand score that can also
  run the same ROMs on ares. It is not run after every change.

## 5. Gates

| Script | Content | Time |
|---|---|---|
| `scripts/validate.py <gate> [--mode M]` | one gate (`systemtest`, `krom`, `sm64`, `pd`, `bench`) in one execution mode | seconds to minutes |
| `scripts/gate_quick.sh [krom] [lock] [phys] [thar0]` | per-change subset on the static builds: systemtest threaded-jit, sm64 interp and gpurdp-jit; extras on request | ~1.5 min |
| `scripts/gate_all.sh` | unit tests, rewind end-to-end, then systemtest and sm64 in every CPU/RSP mode on SoftRDP, then krom | ~20 min |
| `scripts/gate_gpurdp.sh` | the same material on GPU-RDP (needs a Vulkan GPU); results must equal SoftRDP's | ~7.5 min |

The execution modes (`--mode`) map to environment switches: `interp` (interpreter, Lockstep:
the oracle), `jit`, `threaded`, `threaded-jit`, `rspinterp`, `phys` and others; the full list,
with the reason each exists, is at the top of `scripts/validate.py`. All of them keep the host
shortcuts `KESTREL_SPLEAD` and `KESTREL_DPLOGLEAD` at 0.

Without commercial ROMs the gates still run: the `sm64` and `pd` gates and the rewind
end-to-end test print `SKIP` and count as passed, so `gate_all.sh` reduces to unit tests,
systemtest in every mode and krom.

## 6. Baselines and reading results

Results are compared against files in `docs/baselines/`, so a clean run prints a few lines
instead of hundreds:

- `krom-interp.tsv`: per-ROM accuracy against the capture. A ROM regresses when it drops more
  than 0.5 points. Other modes compare against it with `--vs interp`.
- `sm64.txt`, `pd.txt`: framebuffer md5 of the local commercial-ROM checks. Every mode, and
  GPU-RDP, must produce the same md5.
- `timings.md`: wall-clock time of each gate, to notice a check that got slower.

`--update-baseline` rewrites a baseline. Only do it when the change in result is understood
and intended, and say so in the commit message. Per-ROM detail and logs go to `out/`.

## 7. Commercial ROMs (local only)

Some checks use commercial games because no public ROM exercises the same path end to end:
Super Mario 64 for the framebuffer md5, the rewind round trip and the benchmark, and Perfect
Dark for the GPU-RDP gate. You need your own dumps, configured in `scripts/local.env`. They
are never committed, never required, and never shown in public material.

Performance is measured with `scripts/validate.py bench`: wall-clock time for a fixed number of
guest video fields, minimum of several runs. MIPS are not a speed metric here: in Threaded
mode a faster CPU thread can retire more busy-wait instructions while the emulator is slower.

## 8. Determinism

Two properties are checked on every gate run, and break loudly when lost:

- **Mode equivalence.** With the host shortcuts at 0, Threaded must reach the same guest state
  as Lockstep, and the JIT the same as the interpreter: the systemtest result and the md5
  checks are identical across modes, and `scripts/gate_all.sh` runs them all.
- **State round trip.** The `rewind-rtt` mode reloads a rewind snapshot every field; any
  piece of guest state the snapshot misses changes the md5. `scripts/rewind_e2e.py` and
  `test/state_test.py` check save, run, load and compare through the telemetry server.
