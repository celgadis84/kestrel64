# kestrel64 — N64 emulator

Multithreaded, telemetry-native N64 emulator. Built to run N64 games (esp. Perfect Dark)
fast on weak-single-thread / idle-GPU hosts, where cycle-accurate cooperative-single-thread
emulators (ares, cen64) hit an architectural ceiling.

## Why another emulator

Measured on this workspace's host (i7-870, 2009 Nehalem; RX570 idle): ares runs PD at
~45–52 vps — RSP-emulation-bound by *design*, not a bug (the timer bug was already fixed).
ares is a single-threaded cooperative cothread core: it uses 2 of 8 CPU cores and pays full
low-level VU cost per RSP cycle on a weak single thread. No code/flag lever moves that ceiling
(audited: core is clean, VU is optimal SSE4.1, LTO/threading gains sit in the ±3 vps noise).

kestrel64's bet is architectural, not micro-optimization:

- **Multithreaded from line one.** Real OS threads for CPU / RSP / RDP / AI, not cooperative
  cothreads. Directly attacks the "2/8 cores" ceiling.
- **Telemetry-native core.** A purpose-built length-framed JSON+binary server is baked into the
  core (its own optimal protocol, not ares'). Bulk data — RAM dumps, framebuffers — rides as raw
  bytes with zero hex/base64 overhead. A fresh Python MCP bridge (`tools/mcp/kestrel_mcp.py`)
  exposes it to the agent. Development is driven by full live introspection.
- **HLE-capable RSP.** LLE interpreter for correctness and custom microcode (PD ships custom
  ucode); HLE fast paths for standard microcodes (F3DEX2/S2DEX) as the performance win.

## What kestrel64 does NOT rewrite

- **RDP rasterizer:** reuse **parallel-rdp** (Themaister; Vulkan; GPU-accelerated). Cycle-accurate
  RDP on the GPU is a solved problem and the idle RX570 runs it for free.
- **VU reference:** the RSP vector unit SIMD implementation follows the widely-shared SSE4.1
  reference (same math ares/cen64 use, systemtest-verified).

The from-scratch surface is therefore: R4300i CPU (interpreter → dynarec), RSP, the RCP bus /
DMA / peripherals, the multithread scheduler, and the glue.

## Technology

| Area | Choice |
|------|--------|
| Language | C++20 |
| Toolchain | MSYS CLANG64 (same as ares build) |
| Build | CMake |
| Threading | std::thread + std::atomic; component threads, lock-light handoff |
| CPU | R4300i interpreter first, x86-64 dynarec later |
| RSP | LLE interpreter (SIMD VU), HLE fast paths later |
| RDP | parallel-rdp (Vulkan), integrated M3 |
| Telemetry/MCP | in-core length-framed JSON+binary server (own optimal protocol) |

## Telemetry / MCP protocol

Purpose-built for an agent-driven emulator: JSON for structure, raw bytes for bulk. TCP,
default port **9128**. Length-prefixed frames (no line scanning; binary-safe).

Frame on the wire:

```
[u32 LE totalLen][totalLen bytes payload]
payload = [u32 LE jsonLen][jsonLen bytes JSON][ (totalLen-4-jsonLen) bytes binary blob ]
```

- Request:  JSON `{"id": <n>, "cmd": "<name>", "args": {...}}`, no blob.
- Reply OK:  JSON `{"id": <n>, "ok": true, "data": {...}}`, optional trailing blob.
- Reply err: JSON `{"id": <n>, "ok": false, "error": "<msg>"}`.

Bulk replies (`mem.read`, `fb.read`) put metadata in `data` and the raw bytes in the blob —
so a 4 KB RAM dump is 4 KB on the wire, not 8 KB of hex. The Python bridge
(`tools/mcp/kestrel_mcp.py`) reassembles frames and hands the agent hex/base64 only at the tool
boundary. Command set grows with the milestones (`status`, `mem.regions`, `mem.read` now;
`cpu.regs`, `rsp.regs`, `rdp.state`, `step`, ... as those subsystems land).

## Roadmap

- **M0** — skeleton, CMake, memory map (RDRAM/DMEM/IMEM/PIF), ROM loader, telemetry TCP server.
  Boots nothing; MCP can already inspect memory. ← current
- **M1** — R4300i interpreter; MCP step/regs/PC.
- **M2** — RCP registers (MI/SP/DP/VI/AI/PI/SI), RSP LLE + DMA; get past boot.
- **M3** — integrate parallel-rdp; first triangle / framebuffer.
- **M4** — VI scanout → real video window.
- **M5** — audio (AI) + PIF/controller.
- **M6** — CPU dynarec + HLE RSP + tuned multithread scheduler.

## Status

M0 in progress. See `docs/` for design notes.
