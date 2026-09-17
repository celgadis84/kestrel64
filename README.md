# kestrel64 — N64 emulator

Multithreaded, telemetry-native Nintendo 64 emulator, written from scratch in C++20.
Built to run N64 games (especially Perfect Dark) fast on weak-single-thread / idle-GPU
hosts, where cycle-accurate cooperative-single-thread emulators (ares, cen64) hit an
architectural ceiling — without giving up determinism or hardware accuracy.

**State: playable.** n64-systemtest scores **0 of 3721 failures** (plus 0/2 timing, 0/6
cycle). Super Mario 64, Perfect Dark, Donkey Kong 64 and libdragon homebrew boot, render
and play, with audio, four controllers, save types, save states, rewind and TAS movies.

## Why another emulator

Measured on this workspace's host (i7-870, 2009 Nehalem; RX570 mostly idle): ares runs PD
at ~45–52 vps — RSP-emulation-bound *by design*, not by a bug. A single-threaded
cooperative cothread core uses 2 of 8 CPU cores and pays full low-level VU cost per RSP
cycle on a weak single thread. No code/flag lever moves that ceiling (audited: the core is
clean, the VU is optimal SSE4.1, LTO/threading gains sit in the noise).

kestrel64's bet is architectural:

- **Multithreaded from line one.** Real OS threads for CPU / RSP / RDP / audio, not
  cooperative cothreads.
- **Deterministic anyway.** Threading is not allowed to buy speed with drift: the RCP runs
  in two modes, `Lockstep` (the oracle) and `Threaded` (default), and both must produce the
  *same guest state hash and the same framebuffer*. Everything the guest can observe —
  DPC registers, MI interrupts, DMA visibility — is dated on a guest-time schedule taken by
  the thread that issued the work, never by whichever host thread happens to finish first.
- **Telemetry-native core.** A purpose-built length-framed JSON+binary server is baked into
  the core. Bulk data — RAM dumps, framebuffers — rides as raw bytes with zero hex/base64
  overhead. A Python MCP bridge (`tools/mcp/kestrel_mcp.py`) exposes it to an agent.
- **Reuse where rewriting adds nothing.** The GPU rasterizer is **parallel-rdp**
  (Themaister, Vulkan); the RSP vector unit follows the shared SSE4.1 reference. There is
  also an own **SoftRDP** rasterizer for hosts with no usable GPU and as a second opinion
  on accuracy.

## Performance

Same host (i7-870, 2009; RX570), parallel-rdp backend, threaded RCP + both dynarecs,
measured 2026-09-17 as wall clock for a fixed amount of guest work:

| Benchmark | Wall clock |
|-----------|-----------|
| junkrunner64 (libdragon), 200 buffer swaps | 8.1 s |
| Perfect Dark, 600 buffer swaps | 13.4 s |
| Super Mario 64, 300 buffer swaps | 8.3 s |
| Donkey Kong 64, 1500 M instructions | 13.2 s |

Those numbers move most when a *host* cost is removed without touching guest semantics -
one recent example: the RSP thread was polling the CPU thread's retired-instruction
counter on every spin turn while waiting for a rendezvous, stealing that cache line from
the thread that is the actual bottleneck; sampling it once every 64 turns bought 12.8% on
junkrunner64 and 9.2% on Perfect Dark with a bit-identical framebuffer. The log of what
worked, what regressed and what was reverted lives in `docs/STATUS.md` and
`docs/baselines/timings.md`.

## What is implemented

| Area | State |
|------|-------|
| **CPU** | R4300i interpreter (full ISA, FPU, TLB, exceptions) **+ x86-64 dynarec** (default ON, oracle = interpreter): block linking, indirect-target cache, soft TLB, guest idle-loop skipping |
| **Caches / timing** | I-cache and D-cache emulated *and charged*; configurable CPI (default 1.4), cartridge/PI bus latch, physical cycle model mode |
| **RSP** | LLE interpreter with SSE4.1 vector unit **+ RSP dynarec** (default ON, oracle = RSP interpreter), per-microcode-image code cache |
| **RDP** | **parallel-rdp** (Vulkan, default build) and own SoftRDP rasterizer (fill/shade/texture triangles, z-buffer, texrects, all tile modes, combiner, blender, TLUT, 4-bit, YUV, coverage/AA subsamples) |
| **RDP timing** | GCLK cost model per span, DPC counters, SYNC_FULL fence semantics, FIFO backpressure |
| **VI / video** | Real scanout clock, filters, scaling/aspect, GLFW window, framebuffer capture |
| **Audio** | AI DAC model + host sink (waveOut), audible in all supported games |
| **Peripherals** | PIF/joybus, four controllers, Controller Pak (`.mpk`), Rumble/Transfer dialog, EEPROM 4k/16k, SRAM 256k/768k, FlashRAM |
| **Player features** | Save states, rewind, TAS movies (`.k64m`), GameShark cheats, overclock per clock domain, graphical launcher + installer |
| **Introspection** | Telemetry/MCP server, host sampling profiler, guest hot-path profiler, event ring, write tags, corruption canaries |

Docs: live status in `docs/STATUS.md`, remaining gaps in `docs/GAPS.md`, design notes in
`docs/ARCH-THREADING.md`, `docs/JIT-PLAN.md`, `docs/RDP-TIMING.md`, `docs/RSP-JIT.md`,
`docs/VI-CLOCK.md`, `docs/parallel-rdp-integration.md`, and the rest of `docs/`.

## Build

Toolchain is clang from MSYS2 CLANG64; the build is CMake + Ninja.

```bash
export PATH=/c/msys64/clang64/bin:$PATH
sh scripts/release.sh        # all build trees + launcher + packaged dist/
```

Individual trees, when a single A/B build is enough:

```bash
cmake -B build      -G Ninja                        # SoftRDP only (no GPU needed)
cmake -B build-prdp -G Ninja -DKESTREL_PRDP=ON      # parallel-rdp (Vulkan) backend
cmake --build build-prdp -j8
```

`-DKESTREL_STATIC=ON` produces a self-contained `.exe` (libc++/GLFW linked in).

## Run

```bash
build-prdp/kestrel64.exe game.z64 --run       # free-run; without --run it starts paused
```

Behaviour is driven by `KESTREL_*` environment variables — the full, annotated list lives
in `CLAUDE.md`. The ones worth knowing first:

| Variable | Meaning |
|----------|---------|
| `KESTREL_PRDP=1` | use the GPU RDP (needs a `-DKESTREL_PRDP=ON` build) |
| `KESTREL_THREADS=0` | Lockstep RCP — the determinism oracle |
| `KESTREL_JIT=0` / `KESTREL_RSPJIT=0` | fall back to the interpreters |
| `KESTREL_MAXINSN`, `KESTREL_MAXFLIPS` | stop caps (always use one in scripted runs) |
| `KESTREL_HEARTBEAT=1` | live MIPS, % of N64 speed, worker occupancy, audio starvation |
| `KESTREL_VIDEO=1` | open the video window |

## Validation

Every change has to survive the gates before it counts:

```bash
sh scripts/gate_all.sh      # systemtest (interp/JIT/threaded), krom RDP suite, game hashes
sh scripts/gate_prdp.sh     # the same material against the parallel-rdp build
```

`scripts/validate.py` is the engine underneath; `docs/baselines/timings.md` records the
wall-clock baseline of each battery, because a gate that normally takes 17 s and is still
running at 300 s is a deadlock, not slowness.

The two invariants that decide whether a change is acceptable:

1. **Lockstep == Threaded** — same guest state hash and same framebuffer md5.
2. **Interpreter == dynarec** — for both the CPU and the RSP.

Accuracy references: n64-systemtest (0/3721), the krom RDP suite (371 ROMs, per-pixel
against PNG references; mean 96.47 on the static subset), and Thar0's RDP-Timing-Tests.

## Telemetry / MCP protocol

Purpose-built for agent-driven development: JSON for structure, raw bytes for bulk. TCP,
default port **9128**, length-prefixed frames (no line scanning; binary-safe).

```
[u32 LE totalLen][totalLen bytes payload]
payload = [u32 LE jsonLen][jsonLen bytes JSON][ (totalLen-4-jsonLen) bytes binary blob ]
```

- Request:  JSON `{"id": <n>, "cmd": "<name>", "args": {...}}`, no blob.
- Reply OK:  JSON `{"id": <n>, "ok": true, "data": {...}}`, optional trailing blob.
- Reply err: JSON `{"id": <n>, "ok": false, "error": "<msg>"}`.

Bulk replies (`mem.read`, `fb.read`) put metadata in `data` and the raw bytes in the blob,
so a 4 KB RAM dump is 4 KB on the wire, not 8 KB of hex. The Python bridge reassembles
frames and only hexes at the tool boundary. Commands cover CPU/RSP/RCP registers, memory
(including cache-coherent reads), disassembly, stepping and breakpoints, frame advance,
framebuffer capture, controller injection and the hot-path profilers.

## Layout

```
src/core/     memory map, RCP bus, DMA, scheduler, system loop
src/cpu/      R4300i interpreter + dynarec
src/rsp/      RSP LLE interpreter, vector unit, RSP dynarec
src/rdp/      SoftRDP rasterizer
src/vrdp/     parallel-rdp glue (KESTREL_PRDP)
src/video/    VI scanout, presentation, window
src/audio/    AI + host sink
src/ui/       launcher and in-emulator UI
src/telemetry/ length-framed JSON+binary server
tools/mcp/    Python MCP bridge
scripts/      build, gates, packaging, profiling helpers
docs/         status, design notes, baselines
test/         unit and subsystem tests
```

## Credits

- **parallel-rdp** — Hans-Kristian Arntzen (Themaister), the Vulkan RDP.
- **n64-systemtest** — Lemmy, the hardware conformance suite this core is held to.
- **krom / PeterLemon** N64 demos and **Thar0**'s RDP-Timing-Tests, used as RDP oracles.
- **n64brew**, **n64.dev** and the **libdragon** and **MiSTer N64** projects for the
  hardware documentation this emulator is written against.
