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

Change is only done when all pass:

1. **systemtest, interpreter**: `kestrel64.exe n64-systemtest.z64 --run`
   (ROM at `/e/Claude/N64/n64-systemtest.z64`) → expect **Failed 0 of 3721 · 0 of 2 · 0 of 6**.
2. **systemtest, JIT**: same with `KESTREL_JIT=1` → same **0/3721·0/2·0/6**.
3. **Lockstep == Threaded framebuffer md5**: run SM64 with
   `KESTREL_MAXINSN=300000000 KESTREL_FBDUMP=<path>` (dumps RDRAM framebuffer BMP, no HUD),
   compare default (Lockstep) vs `KESTREL_THREADS=1` (Threaded). md5 must be **identical**.
4. **krom 47-suite** RDP accuracy (SoftRDP path).

Baselines (may drift; the invariant is equality, not the literal hash): SM64 300M md5
`cbf8aa761b92adab89ddde949b6ff24b` (SoftRDP). Read dump text with `tr -d '\0'` (NUL-padded).

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

`KESTREL_THREADS=1` (threaded RCP) · `KESTREL_JIT=1` (dynarec, default OFF, oracle=interp) ·
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
