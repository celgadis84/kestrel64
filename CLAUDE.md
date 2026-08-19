# kestrel64 — CLAUDE.md

From-scratch **multithreaded, telemetry-native N64 emulator** (C++20, clang). Main
active project of this workspace. Built to run N64 games (esp. Perfect Dark) fast on
weak-single-thread / idle-GPU hosts, where cooperative-single-thread cycle-accurate
emulators (ares, cen64) hit an architectural ceiling.

Live status: `docs/STATUS.md`. Design docs: `docs/ARCH-THREADING.md`, `docs/JIT-PLAN.md`,
`docs/TEXTURE-FORMATS.md`, `docs/parallel-rdp-integration.md`.

## Architecture bet

- **Multithreaded from line one** — real OS threads for CPU / RSP / RDP / AI, not
  cooperative cothreads. Attacks the "2/8 cores" ceiling ares hits by design.
- **Telemetry-native core** — length-framed JSON+binary server baked in (port 9128). Bulk
  data (RAM dumps, framebuffers) rides as raw bytes, zero hex overhead. MCP bridge
  `tools/mcp/kestrel_mcp.py` exposes it. Development driven by full live introspection.
- **HLE-capable RSP** — LLE interpreter for correctness + custom microcode (PD ships
  custom ucode); HLE fast paths for standard microcodes as the perf win.
- **Reuse, not rewrite**: RDP = **parallel-rdp** (Themaister, Vulkan, GPU); VU = shared
  SSE4.1 reference (same math ares/cen64 use, systemtest-verified).

## Build

Toolchain = clang at `/c/msys64/clang64/bin` (MSYS2 CLANG64). Static-linked exe.

```bash
export PATH=/c/msys64/clang64/bin:$PATH
cd /e/Claude/N64/kestrel64
cmake --build build -j
```

- **ALWAYS `taskkill //F //IM kestrel64.exe` before rebuild** (Windows locks the exe).
- cmake binary lives in `/c/msys64/clang64/bin`.
- `build/` = default (PRDP OFF). `build-prdp/` = configured `-DKESTREL_PRDP=ON`
  (parallel-rdp GPU backend). Default OFF so deterministic core never depends on GPU.

## Verification — run after EVERY change (hard rule)

Everything runs from one script; do not hand-roll the invocations.

```bash
python scripts/validate.py systemtest --mode <mode>   # 0/3721 · 0/2 · 0/6
python scripts/validate.py sm64       --mode <mode>   # framebuffer md5
python scripts/validate.py krom                       # 371-ROM RDP accuracy vs baseline
python scripts/validate.py bench      --mode <mode>   # wall clock for N VI fields (speed)
sh scripts/gate_all.sh                                # las cinco modalidades + krom, un log
```

**Speed is measured with `bench`, never with Mips.** In threaded mode a faster CPU
thread just burns more spin-wait instructions, so Mips can rise while the emulator
gets slower. `bench` fixes the guest work (N VI fields) and times the wall clock.

Modes: `interp`, `jit`, `jit-nolink`, `threaded`, `threaded-jit` (plus
`threaded-trace`, `threaded-nolink`). A change is done when systemtest and sm64
pass in **all five** and krom shows no regression.

**The sm64 gate stops on VI buffer swaps, not on an instruction count**
(`--sm64-flips`, default 60). A fixed instruction cap is not a deterministic point
of the game in threaded mode: the RCP runs on its own threads, so the number of
instructions the CPU burns in a spin-wait depends on the workers' wall-clock, and
two runs of the same build stop in different animation phases. Counting displayed
frames is a game state, and there all five modes agree byte for byte. See
`docs/PERF-CPU.md`.

Baselines live in `docs/baselines/` (may drift; the invariant is equality, not the
literal hash): SM64 60 fields `466282775dbd0ac084946558a1c30771` (SoftRDP).
Read dump text with `tr -d '\0'` (NUL-padded).

## MCP

Kestrel's OWN telemetry server — this is THE MCP for the whole workspace (ares MCP retired).

- Launch: `kestrel64.exe <rom> --mcp` (or `run_mcp.cmd`). Boots **paused** in MCP mode —
  `run_control resume` to advance CPU (`'resume'|'pause'|'reset'`, NOT `'run'`).
- Bridge: `tools/mcp/kestrel_mcp.py`, port 9128. Tools surfaced as `mcp__kestrel64__*`.
- Notable capabilities: `read_memory`/`write_memory` (`coherent=1` reads via CPU D-cache =
  no STALE kernel state), `capture_framebuffer` (VI→RGBA→PNG+histogram), hotpath profiler
  (`profile_start/stop/reset`, `profile_cpu` PC-bucket 16B, `profile_rsp` IMEM-slot),
  cpu/rsp/rcp registers, disasm, breakpoints, `run_until`.
- **When a capability is missing, ADD it** to the server — don't fall back to guessing.

### MCP gotchas
- `read_memory` inside a block-capture returns 0 — read `mem->rdram` directly instead.
- Never declare "hung" from capped/truncated log output.

## Source layout (`src/`)

`core/` (memory, system, rom, bus, DMA, MMIO, scheduler) · `cpu/` (R4300i interp + `jit.*`
dynarec) · `rsp/` (LLE + HLE) · `rdp/` (SoftRDP) · `vrdp/` (parallel-rdp glue, `KESTREL_PRDP`)
· `audio/` · `video/` (`present.cpp`, VI, GLFW/Vulkan WSI) · `telemetry/` · `net/` · `main.cpp`.

## Env-var toggles

`KESTREL_THREADS` (threaded RCP, **default ON**) · `KESTREL_JIT` (dynarec, **default ON**, oracle=interp;
los dos leen VALOR: `=0` apaga. Medido en SM64: threaded-jit = 99% tiempo real, interp = 12.8%;
block linking is ON inside it, `KESTREL_JIT_NOLINK=1` / `KESTREL_JIT_CHAIN=<n>` to bisect,
`KESTREL_JIT_TRACE=1` superblocks = measured negative) ·
`KESTREL_HEARTBEAT=1` · `KESTREL_HOSTPROF=<ms>` (host sampler) · `KESTREL_JIT_STATS=1` ·
`KESTREL_WATCHDOG=<s>` (liveness + stuck-thread RIP) · `KESTREL_FIELDHASH=1` /
`KESTREL_FIELDDUMP=<n>` (localise a divergence) · `KESTREL_MAXFLIPS=<n>` (stop after n fields) ·
`KESTREL_PRDP=1` (GPU RDP, needs `build-prdp`) · `KESTREL_MAXINSN=N` · `KESTREL_FBDUMP=path` ·
`KESTREL_NOFETCHFAST=1` (disable I-cache-line fetch memoization) · `KESTREL_SAVETYPE` ·
`KESTREL_VIDEO=1` · `KESTREL_VIDEO_TEST`.

## Hard rules (NON-NEGOTIABLE)

- **Every change = genuine, generalizable VR4300/RCP HW semantics. NEVER hardcode to pass
  a test.** The oracle (systemtest / lockstep md5 / krom suite) catches shortcuts.
- **Verify systemtest (interp+JIT) + lockstep==threaded md5 + krom 47-suite after EVERY
  change.** No exceptions.
- **NEVER touch Perfect Dark** (the `../perfect_dark` decomp) until explicitly ordered.
- Context goes in `docs/` (committed), NOT in agent memory. Memory = feedback + breadcrumbs.
- Test the user's hypothesis FIRST (cheap experiment) before own theory. Never block/ask;
  pick best default and continue. Respond in Spanish, caveman style (code/commits normal).

## Current frontier (see STATUS.md for live detail)

systemtest 100%, SM64 boots+renders 3D, PD boots+renders+advances, lockstep==threaded.
Save types complete (EEPROM/SRAM/FlashRAM). Dynarec Stage-2c (block-linking = ceiling).
parallel-rdp WIRED + first light (SM64 Mario head correct; background rainbow = VI/clear
accuracy item). Real RDP accuracy frontier = coverage/AA subpixel (biggest, last).

### Queued work (autonomous order)
1. Fix parallel-RDP SM64 background rainbow (VI dedither/divot/clear path).
2. RSP VU with **SSE4.2** intrinsics (8×s16 = 1 XMM; user asked for most-advanced host CPU
   instr — i7-870 Nehalem, SSE4.2 max, NO AVX). Oracle = current scalar interp, bit-exact.
3. Savestates. 4. Dynarec block-linking. 5. Controller Pak `.mpk`. 6. PIF/CIC LLE.

Note: classic Zilmar video/audio plugin architecture = legacy that caused inaccuracy;
but backend SELECTION (SoftRDP↔parallel-RDP, audio sink) is what we already build = good.
