# kestrel64 — status

## M0 — DONE (2026-07-25)

Skeleton + memory + telemetry. Builds static (clang MSYS CLANG64), runs standalone,
MCP bridge green.

### Delivered
- **Build**: CMake + Ninja, C++20, clang. `-march=native` (host-tuned) default, static-linked
  (`-static`) so `kestrel64.exe` runs outside the MSYS shell. 841 KB exe.
- **Memory map** (`src/core/memory.{hpp,cpp}`): RDRAM (8 MB, Expansion Pak), SP DMEM/IMEM (4 KB),
  PIF RAM (64 B), CART_ROM. KSEG0/KSEG1 → physical masking. Big-endian bus (8/16/32/64).
  MMIO register blocks reserved, land in M2.
- **ROM loader** (`src/core/rom.{hpp,cpp}`): detects z64/n64/v64 by magic, normalizes to
  big-endian, parses header (entry/crc/name/cartId/country/version).
- **Telemetry server** (`src/telemetry/server.{hpp,cpp}`, `src/net/tcp.{hpp,cpp}`): own optimal
  protocol — length-framed JSON+binary over TCP (port 9128). Bulk data (RAM dumps) rides as raw
  bytes, zero hex overhead. Runs on its own thread. Commands: `ping`, `status`, `mem.regions`,
  `mem.read`, `mem.write`.
- **MCP bridge** (`tools/mcp/kestrel_mcp.py`): fresh, reassembles frames, FastMCP tools
  (`emu_status`, `memory_regions`, `read_memory`, `write_memory`). `--selftest` validates the
  full loop.

### Validated
`kestrel64 n64-systemtest.z64` → bridge selftest:
- status: name "n64-systemtest", entry 0x800a15f8, z64.
- regions: correct N64 physical bases.
- ROM magic 80371240 (big-endian normalized), header name correct.
- RDRAM write/read round-trip (deadbeef) OK.

## M1 — DONE (2026-07-25)

R4300i interpreter. Fetch/decode/execute across RDRAM + SP DMEM, correct branch
delay slots, exact 64-bit register telemetry, HLE fast-boot. Steps + free-runs the
real n64-systemtest IPL over MCP.

### Delivered
- **CPU core** (`src/cpu/cpu.{hpp,cpp}`): 64-bit GPRs (32-bit ops sign-extend),
  pc/nextPc delay-slot model, hi/lo, COP0 (Status/Cause/EPC/Count/Compare/PRId/Config),
  COP1 register file (FP load/store/move; FP *arithmetic* stubbed → M1.5). Full integer
  ISA: SPECIAL, REGIMM, immediates, loads/stores incl. unaligned L/SWL/R + L/SDL/R,
  mult/div + 64-bit d-variants, jumps/branches + likely (nullifying) branches, LL/SC,
  CACHE/SYNC no-ops, ERET. Unimplemented op → `halt(reason)`, surfaced over telemetry
  (never crashes).
- **Address translation**: kernel-segment shortcut (`paddr = vaddr & 0x1fffffff`), exact
  for KSEG0/KSEG1. TLB → M2.
- **HLE fast-boot** (`CPU::fastBoot`): copies boot segment (≤1 MB from ROM+0x1000) to RDRAM
  at the entry's physical address, sets CIC-6102 register hand-off (sp/ra/s4/s6), PC=entry.
- **System wiring** (`src/core/system.cpp`): owns the CPU, runs it in batches on the run
  thread; `coreMutex` serializes CPU/memory access against the telemetry thread. Starts
  **paused** (deterministic stepping); `--run` for free-run.
- **Telemetry**: `cpu.regs` (all GPRs by name + pc/hi/lo/COP0 + disasm@pc), `cpu.step`
  {count}, `cpu.disasm` {addr,count}, `pause`/`resume`/`reset`. New MCP tools land next.
- **disasm**: compact mnemonics for the common ops (telemetry-only).

### Bugs fixed this milestone
- **JSON number precision**: `json::Value` stored every number as `double` — 64-bit register
  values lost precision and `(s64)double` overflow printed `0x8000000000000000`. Added an
  exact-integer path (`isInt`/`sign`/`u`), fixed serialize + parse. Register telemetry is now
  bit-exact.
- **step() double-execute**: original `nextPc = OLDpc+4` re-fetched each sequential instruction
  (every op ran twice, then diverged). Corrected to `pc = nextPc; nextPc = pc + 4;`.

### Validated
`kestrel64 n64-systemtest.z64` → over MCP: reset, single-step trace shows clean sequential
execution + correct delay slots (bne→delay slot→back-edge), 64-bit regs bit-exact
(sp=…a4001ff0, t1=…a4000000). Boot copy loop runs, `jr` hands off into RSP DMEM (0xa40004xx),
execution continues back into RDRAM. Free-run ~78 MIPS interpreted, **zero** unimplemented-op
halts. Further progress now blocks on RCP MMIO polls (status bits read 0) → M2.

## Next — M2: RCP registers + RSP + DMA
- MMIO register blocks: MI, SP, DP, VI, AI, PI, SI (currently read 0). Wire PI DMA
  (cart→RDRAM), SP DMA (RDRAM↔DMEM/IMEM), SI (PIF).
- RSP: LLE core reusing the audited SSE4.1 VU reference; scalar SU + vector VU.
- TLB (deferred from M1) + full exception model (interrupts via MI, Count/Compare timer).
- Milestone: systemtest advances past the IPL hardware handshake.

## Architecture reminders
- Multithread from the start: telemetry already on its own thread. When CPU/RSP land on their
  own threads, telemetry handlers reading shared core state get proper sync (atomics/seqlock);
  M0 is single-mutator so none needed yet.
- Reuse, don't rewrite: parallel-rdp (RDP, M3), SSE4.1 VU reference (RSP, M2).
- Protocol is ours (not ares'): binary-framed, so framebuffer/RAM pulls stay cheap for the agent.

---

## M2 → PD boots (2026-07-28)

RCP MMIO + DMA + full interrupt/exception model landed (MI aggregation → IP2, Count/Compare
timer → IP7, PI/SP/SI DMA, VI field tick). **Perfect Dark now boots and runs the game loop**:
free-run reaches **10.5 B+ instructions with no crash, no unimplemented-op halt**, PC cycling
through libultra + game code (frame loop servicing VI interrupts). Full video awaits the LLE
RSP/RDP (M3, HLE-stubbed today), so this is the *CPU* boot milestone.

### The bug that blocked PD boot — missing COP1 usability enforcement (CU1)

**Symptom.** ~283 M instructions in, PD's rzip/inflate decompressor built a corrupt Huffman
table: `buildHufts` (0x80006054) received a distance-table pointer `a0 = ll_base + 512*4`
where it should have been `ll_base + nl*4` (nl≈282, and 512 is impossible for deflate — max
286 literal codes). The bad pointer read stale huft-table data as code-lengths, overran the
stack `count[]`, and a runaway store walked up into the SP DMA registers at 0x04040000 →
garbage DMA zeroed the interrupt dispatch table → every subsequent IP2 did `jr 0` → exception
storm.

**Root cause.** IDO keeps `nl`/`nd` in FP registers `f0`/`f1` as callee-saved scratch across
the `buildHufts` calls (`MTC1 t2,f0` before, `MFC1 t2,f0` after). Mid-decompress a **VI
interrupt** fired (exc code 0, IP2/MI_VI, EPC inside buildHufts) and its handler ran float-
heavy game code that clobbers `f0`. On real hardware the decode thread runs with **Status.CU1
set**, so libultra's `__osException` saves/restores the thread's full FP register file across
the interrupt and `f0` survives. kestrel64's `cop1op` **never checked CU1** — it happily ran
FP ops for a thread whose context has CU1=0 (`SR=0x0400ff01`), so `__osException` saw CU1=0,
**skipped the FP-context save**, and the handler's `f0` clobber was never undone. `nl` came
back as 512 → corrupt table → storm.

**Fix** (`src/cpu/cpu.cpp`, `cop1op`): enforce coprocessor-1 usability. If `Status.CU1`
(bit 29) is clear, raise **Coprocessor Unusable** (ExcCode 11) with `Cause.CE = 1` and return
without executing the FP op. PD's OS then manages FP per thread as designed; the interrupt no
longer eats `f0`. Decompression is clean, PD advances from 283 M → billions of instructions.

Lesson: an emulator being *too permissive* (running privileged/guarded ops the real CPU would
trap) is as fatal as being too strict — here it silently broke the guest OS's FP-context
contract. Diagnosis path: bisected the bad `buildHufts` arg → traced `nl` to the `f0` stash →
`fpr[0]` change-log pinned the clobber to a game function reached via a VI interrupt whose
saved `SR` had CU1=0.

## Boot accuracy — CIC detection + LLE IPL3 (2026-07-28)

Two-step tightening of the boot path.

**Step 1 — real CIC detection (HLE hand-off).** `fastBoot` used to hardcode CIC-6102
(seed `0x3f`, `osCicId=0x3f`). PD NTSC is actually **CIC-6105** (seed `0x91`). Added
`detectCic()` (`src/cpu/cpu.cpp`): CRC32 of the cart's IPL3 image (ROM `0x40..0xFFF`) matched
against the community table → picks the right seed for `s6` and writes the numeric `osCicId`
(6101/6102/6103/6105/6106) to the boot globals. PD now boots with the correct 6105 hand-off;
verified `[boot] CIC detected: 6105 (seed 0x91)` and a steady game loop (RSP gfx task dispatched
each field), no regression.

**Step 2 — LLE IPL3 (`KESTREL_LLE_IPL3=1`).** Instead of faking IPL3's *result*, run the cart's
own CIC-signed IPL3: copy the 0xFC0-byte image to SP DMEM `0x04000040`, set the IPL2→IPL3
register hand-off (`s3..s7` + seed, `sp=0xA4001FF0`), and start the CPU at `0xA4000040`.
- **Works for self-contained IPL3 (libdragon homebrew).** n64-systemtest (libdragon-linked,
  6102): its open IPL3 is plain MIPS — inits RDRAM (clears each 1 MB bank via RSP DMA), copies
  the boot segment, verifies its checksum, and **reaches the game entry `0x800a15f8` in 5636
  instructions**. LLE hand-off proven.
- **Does NOT work for retail PD (6105) standalone — by design, not a bug.** PD's IPL3 body at
  ROM `0x84+` is **high-entropy/scrambled**; the head (`0x40..0x78`) is a self-modifying
  descramble loop that expects registers (t1/t3) **seeded by IPL2**. IPL2 lives in the
  proprietary **PIF ROM** (not in the cartridge, not dumped here), so the real boot code never
  materializes and raw execution hits garbage at `0x84`. True LLE of a retail cart needs IPL1/2.
  → Retail ROMs stay on the (now CIC-accurate) HLE `fastBoot`; LLE is the path for open
  homebrew and a future PIF-ROM/CIC-serial milestone.

Net: HLE boot is now correct per-CIC for retail; LLE is a validated, opt-in path for
self-contained IPL3 (the libdragon case — a stated workspace target).

## M3 — real video (goal: PD renders). M3.1 done (2026-07-28)

Priority: get **Perfect Dark** displaying, homebrew later. PD boots and runs its game loop
(billions of instructions, RSP gfx tasks dispatched each field) but shows nothing because
RSP/RDP are HLE-stubbed — the SP task "finishes" instantly and DP_END just raises the DP
interrupt; **no pixels are ever written to RDRAM**. M3 fills that in, in three increments:

- **M3.1 — VI presentation (DONE, cross-platform Vulkan).** `src/video/present.{hpp,cpp}`: a
  **Vulkan + GLFW** window (not Win32 GDI — the presenter must be portable; the dev box GPU is a
  Radeon RX 570). Each ~16 ms it reads the VI framebuffer from RDRAM (VI_ORIGIN / VI_WIDTH /
  VI_CTRL pixel type: 16bpp RGBA5551 or 32bpp RGBA8888), packs it into a host-visible LINEAR
  `R8G8B8A8` `VkImage`, and `vkCmdBlitImage`s it (scaled, NEAREST, FIFO/vsync) to the swapchain —
  no shaders. This is the same Vulkan stack `parallel-rdp` will render into at M3.2. Opt-in
  `KESTREL_VIDEO=1`; closing the window shuts the emulator down.

  **Threading (critical):** GLFW/Vulkan own the **main thread** (`System::runLoop`); the CPU
  emulation runs on a **worker**. And Vulkan is brought up **before** the worker starts —
  `presenter.open()` then spawn CPU thread then loop `presenter.pumpFrame()`. On this box a
  CPU-bound sibling thread stalls the AMD driver's instance/device bring-up *indefinitely*
  (empirically: paused → init completes; `--run` during init → hang). Two more driver quirks
  worked around: (a) `vkCreateInstance` hangs if `glfwInit()` ran first → create the instance
  first with hardcoded surface extensions, then `glfwInit`; (b) if the surface is unusable
  (`vkGetPhysicalDeviceSurfaceCapabilitiesKHR` returns `VK_ERROR_UNKNOWN`), the presenter drops
  to **compose-only** (`presentable=false`, no swapchain) instead of crashing in
  `vkAcquireNextImageKHR`.

  Verified headless via `KESTREL_VIDEO_TEST` (RGB gradient + 32px grid) dumped to BMP
  (`KESTREL_VIDEO_DUMP`) → channel order and blit path correct. PD runs stable with the presenter
  attached (12 s+, no crash). **Caveat:** surface *presentation* couldn't be self-validated —
  processes spawned from this non-interactive shell get a window-station with no usable desktop
  surface (`surfaceCaps rc=-13`), so init degrades to compose-only. A real interactive launch
  (`KESTREL_VIDEO=1 kestrel64.exe <rom> --run` from the user's own terminal) gets the full
  swapchain path. Needs a runtime `vulkan-1.dll` (system) + `glfw3.dll`/`libc++.dll` (clang64/bin
  on PATH); the build is no longer `-static`.

## M1.5 — ISA/exception accuracy so n64-systemtest runs its whole suite (2026-07-28)

n64-systemtest is the HW-accuracy baseline ("proves the emulator behaves like the hardware"), so
its `--run` host crash was a real, priority bug — not a side issue. Diagnosed with a permanent
**SEH crash filter** (`main.cpp`, `SetUnhandledExceptionFilter`) that dumps guest pc/nextPc +
jumplog on any host fault.

- **Root cause of the "crash":** it was `0xC0000095` STATUS_INTEGER_OVERFLOW, not an access
  violation. MIPS `div`/`ddiv` never trap on divide-by-zero or `INT_MIN / -1`; they yield defined
  R4300i results. The host x86 `idiv` faults (#DE) on both. `cpu.cpp` only guarded `/0` (and even
  then left HI/LO untouched, which is wrong). Fixed DIV/DIVU/DDIV/DDIVU with exact VR4300 results:
  `/0` → LO=(a<0?1:-1) [unsigned: LO=~0], HI=a; `INT_MIN/-1` → LO=INT_MIN, HI=0.
- Then walked the halt-by-halt gaps the test ROM exposes and implemented, in `cpu.cpp`:
  - FP int64 conversions `.L` (ROUND/TRUNC/CEIL/FLOOR.L for S and D) — were missing.
  - `CVT.W`/`CVT.L` now honor the FCSR rounding mode (roundRM helper).
  - Invalid COP1 encodings (e.g. `CVT.S.S`) raise the FP **Unimplemented-Operation** exception
    (FCSR Cause E=bit17, ExcCode 15) instead of halting.
  - **COP2** (`cop2op`): VR4300 latch model (no functional unit — MxC2 read/write `cp2latch`,
    CFC2=0, CTC2 nop), CU2 gate (Status bit30) → Coprocessor-Unusable CE=2, reserved sub-op → RI.
  - **Trap** instructions: TGE/TGEU/TLT/TLTU/TEQ/TNE (SPECIAL) + TGEI…TNEI (REGIMM) → ExcCode 13.
  - **`unimplemented()` now raises Reserved Instruction (ExcCode 10) by default** — the real
    VR4300 behavior for an unrecognized encoding — instead of a machine halt. `KESTREL_HALT_UNIMPL=1`
    restores the halt+dump for hunting genuine op gaps.
- Added an **IS-Viewer** debug channel in `memory.cpp` (phys 0x13FF0000 overlay: chars staged at
  0x13FF0020, a length write to 0x13FF0014 flushes to stdout). systemtest does not use it here (it
  renders results to the framebuffer), but it's the standard homebrew/libdragon text path.

**Result:** `n64-systemtest --run` now executes **1.2B+ instructions with no host crash**,
exercising AdEL(4) / Coprocessor-Unusable(11, CE=2) / Reserved-Instruction(10) / Trap(13)
exceptions and TLB-mapped code, then parks in its terminal spin (`j 0x00114a24` to itself, a
TLB-mapped address) — i.e. it ran the suite to the end. The actual pass/fail verdict is drawn as
glyphs into the framebuffer, so **reading it requires the RDP (M3.2)** — there is no ASCII result
text in RDRAM to scrape. Iteration method: build → run headless with a timeout → read the `[halt]`
opcode from stderr → implement → repeat; use `KESTREL_MAXINSN` to stop cleanly (frees `coreMutex`)
then inspect over the telemetry MCP.

- **M3.2 — RDP (NEXT).** Rasterize the DPC command FIFO (`DPC_START..DPC_END`, currently faked
  to finish instantly) into the RDRAM framebuffer/z-buffer. Plan: a **software RDP** first
  (fill rects, shaded/textured triangles, SET_* state, sync) for debuggable first-light, since
  it needs no Vulkan and its output is directly inspectable via the BMP dump. `parallel-rdp`
  (Vulkan, vendored in ares-64) is the later accuracy/perf path once first-light works.
- **M3.3 — RSP gfx (LLE).** Produce the RDP command stream from PD's display lists by running
  the actual gfx microcode on an LLE RSP (reuse the audited SSE4.1 VU + scalar interpreter from
  ares-64 `n64/rsp`). PD ships a *custom* F3DEX2-derived ucode, so LLE is the faithful route
  (HLE'ing Rare's custom microcode is fragile). This is the from-scratch core piece.

Iterate M3.2↔M3.3 until PD's intro/menus render. Reuse assets confirmed on disk:
`ares-64/ares/n64/rsp/interpreter-vpu.cpp` (VU) and `ares-64/ares/n64/vulkan/parallel-rdp`.

## M1.5b — systemtest now scores itself as text; COP1 CVT accuracy pass (2026-07-30)

n64-systemtest emits its pass/fail verdict as ASCII over the **emu-extension channel**
(`emuxOp`, xlog) — so `--run` prints `Running <name>...`, per-value `Test '<name>' ... failed: <why>`,
and a final `Base: Failed N of 3721 tests`. No RDP needed to read the score. Iteration is now
build → `kestrel64.exe n64-systemtest.z64 --run 2>&1 | tr -d '\0'` → grep the fails.
(Run the binary with `PATH=/c/msys64/clang64/bin:$PATH` or Windows can't find its DLLs → exit 127.)

**Measured baseline (COP1 input-NaN/subnormal fix built): `Failed 295 of 3721` (92%).**

Two COP1 CVT accuracy fixes this session (`cpu.cpp` `cop1op`), both matching VR4300 exactly and
verified against `tests/cop1/mod.rs` value tables:

- **CVT.D.S / CVT.S.D input operand check.** The NaN/subnormal→Unimplemented operand guard was
  gated `fn <= 0x07`, so the two float→float converts (fn 0x21 / 0x20) bypassed it and only
  checked the *result*. Now a subnormal or signalling-NaN **source** raises Unimplemented (E),
  and a quiet-NaN source raises Invalid + yields the canonical qNaN **in the destination format**
  (CVT changes format — single↔double — so the canonical pattern must follow the dest, not src).
- **CVT.S.L / CVT.D.L input range check.** The VR4300 only converts a 64-bit integer whose
  bits [63:55] all equal the sign bit — i.e. it fits in a signed 56-bit field, range
  `[-2^55, 2^55-1]`. Anything larger raises Unimplemented (the convert hardware doesn't handle it).
  Implemented as `s64 top = v >> 55; if(top != 0 && top != -1) unimpl()`. Clears all 13 CVT
  "Exception expected but none seen" value-fails; **`COP1: CVT.D.fmt` now passes fully.**

**RESOLVED — the "hang" was two RSP jump-register bugs, NOT a CVT-timing race.**
Earlier notes blamed `RSP SW (unaligned)` and a CPU↔RSP scheduling race; both were wrong. Full
`KESTREL_RSPTRACE` capture showed `RSP SW (unaligned)` *passes* (`halt=1`). The real time-sink was
the `RSP JR/JALR` block: 5+ tests hit `budget exhausted at pc=0x…` (RSP ran the full 40M-insn
budget without ever reaching BREAK) and the `RSP hidden flag register` test does a 65 536-iteration
`run_and_wait` loop — so the suite wasn't deadlocked, just crawling through ~200M wasted interp
insns. Two root-cause fixes:
- **`take()` (rsp.hpp) masked jump targets `& 0xfff` instead of `& 0xffc`.** The JR test loads
  `AT = 0xFFFFF00B` ("target 0x008 with unnecessary bits"); real RSP PC is word-aligned so
  `0xFFFFF00B & 0xFFC = 0x008`. My mask gave `0x00B` → `imword` read a misaligned/garbage opcode →
  runaway loop → 40M budget. Fix aligns to `& 0xffc` (J/relative branches already aligned, no-op).
- **`JALR` (rsp.cpp) linked `rd` before reading `rs`.** When `rd == rs` (`JALR ra,at` with
  `ra==at`) `setR(rd,…)` clobbered the target before `take(r[rs])` read it. Fixed by capturing
  `tgt = r[rs]` first. Clears "JALR: return register is equal to target register".
All `RSP JR`/`RSP JALR` tests now pass, zero `budget exhausted`. The CVT-L fix was never implicated
and stays. **Full re-measure: `Base: Failed 272 of 3721 (92%)` in 6.55s** (was 295 failed → the two
RSP jump fixes cleared ~23 tests and, more importantly, removed the ~200M-insn budget-exhaust drag
so the suite finishes in seconds). Note: the exe does not self-exit after the summary — it wedges at
~0% CPU in headless Vulkan teardown (known quirk); `taskkill` it. Read output via
`tr -d '\0' < file` (binary NULs).

**Next accuracy targets (from the 272-fail breakdown):** biggest bucket is still the FP
**exception-leak** — `MUL.S`/`DIV.S`/`CVT.W` with sNaN or denormal operands expect `Err(())`
(Unimplemented, fcr31 bit17) but the emu raises a real FPE instead (~30+ cases). Then 8× "SP
Set/Clear Signal" (SP_STATUS signal set+clear in one write must be a no-op), 4× RDP-status timeout
(RDP unimplemented), 4× TLB EntryHi, a few PIFRAM reads.

Remaining COP1 buckets (from the partial, all cop1 runs before RSP): ~47 `failed with exception:
FPE` (the exception-**leak** divergence — guest reports an FPE it didn't arm for; hardest bucket,
carried over) and ~17 `CVT.L/CVT.W … a == b` (wrong FCSR flags / rounding on the int converts).

---

## 2026-08-08 — n64-systemtest 100% (0 of 3721 failed)

Suite passes completely: `Base: Failed 0 of 3721 tests (100% success rate)` in ~6.9s.
The path from the 272-fail snapshot above closed the FP exception-leak, SP signal,
TLB EntryHi and PIFRAM buckets in intervening work; this session cleared the last
11 → 0. The final fixes, all genuine VR4300/RCP hardware semantics (no test-specific
hacks):

**CPU D-cache write path (11 → 6).** `CPU::write32`/`write64` — the helpers behind
SWL/SWR/SDL/SDR/SWC1/SDC1/SC — bypassed the D-cache while `SW`/`SD` (inline) and every
read helper went through it. That asymmetry let a stack slot stay cached with a value
that no longer matched RDRAM, derailing a dispatch table (`jalr $t9` where `$t9`=0 →
jump-to-0 exception storm, "no tests executed"). Fix routes those helpers through
`dcWrite` when the address is cacheable + in RDRAM, so *all* CPU stores share one
coherent cache view — exactly hardware behaviour. D+I caches now fully enabled.

**TLB C-field honored in `cacheable()` (6 → 5).** `cacheable()` decided by segment
only and treated a `C==2` (uncached) useg/xkphys mapping as cached, returning a stale
line a prior test had cached at the same physical address. `translate()`/`tlbLookup()`
now record the matched entry's cache-coherency attribute (`xlatCacheable`, `C==2` →
uncached) and `cacheable()` consults it for mapped segments. Standard MIPS semantics.

**RSP↔CPU interleaved execution (5 → 4).** The RSP previously ran atomically to BREAK
inside the SP_STATUS write handler, so a microcode spinning on a SIGNAL the CPU sets
mid-run (and vice-versa) could never resolve. Refactored `Rsp::run()` into
`start()` + `step(maxInsns)` with a persistent branch-delay latch, and `System::stepCpu`
now advances the RSP interleaved at the true 2:3 clock ratio (RSP 62.5 MHz : CPU
93.75 MHz). Fixes "RSP running in parallel to the CPU". No regression to the other 3716.

**RDP status flags + xbus (4 → 0).** The soft RDP ran synchronously on DPC_END but
never modelled DP_STATUS, so `wait_for_status(...)` spun forever. Added the flag model:
COMMAND_BUFFER_READY (0x80) always set (the synchronous RDP is always ready);
START_GCLK|PIPE_BUSY (0x28) set when the FIFO is kicked and cleared when a SYNC_FULL
drains the pipe — reproducing the exact 0xa8→0x80 sequence the "Flags during a run"
test polls. And `SoftRdp::run` gained an `xbus` mode: with DP_STATUS_XBUS set, command
words are fetched from DMEM (12-bit wrapping, so the overflow variant reads past 0xFFF
and wraps) instead of RDRAM; pixel output still lands in the RDRAM color image. Clears
all four "RDP STATUS: Run from DMEM (xbus)" / "Flags during a run" tests.

### 2026-08-08 — validación cruzada con krom (PeterLemon/N64)

Segundo oráculo HW además de n64-systemtest: las test-ROMs de PeterLemon/N64 (en
`E:\Claude\N64\N64\`) renderizan tablas PASS/FAIL a framebuffer y traen PNG de
referencia (salida de consola real). Volcado headless vía `KESTREL_MAXINSN` +
`KESTREL_FBDUMP=<archivo.bmp>` (path relativo — MSYS mangle paths con `:`; convertir
BMP→PNG con `py`, no la python de clang64 que no trae PIL). Resultados:

- **RDRAMTest** — arranca y renderiza (texto RDRAM/Address/Result).
- **CP1 ADD/DIV/SQRT** (.S y .D) — todas las filas PASS. FP exacto.
- **RSP CP2 LTV/STV** — todas PASS. Load/store vectorial + DMEM correctos.
- **CP0 COP0Register** — 3 FAIL → **arreglados**, ahora todo PASS (= referencia).

**Fix (genuino, global): estado COP0 post-IPL3 en `fastBoot` (HLE).** El test lee
`mfc0` de los 32 registros CP0 y compara `(valor & máscara) == esperado` contra los
valores que el IPL3 real deja al entrar el juego. Nuestro HLE dejaba tres mal:
`Status=0x34000000` (era CU1|CU0|FR) debía ser **0x241000E0** = CU1|FR|SR + 64-bit
addressing en todos los modos (KX|SX|UX) — el estado documentado del hand-off CIC-6102
(coincide con n64brew "initial register state"); y `EPC`/`ErrorEPC` debían conservar su
valor power-on **0xFFFFFFFF** (la cadena de arranque nunca los escribe), no 0. Añadidos
en el path HLE de `fastBoot` justo antes de saltar al entry. En modo kernel CU0 es
innecesario, así que quitarlo no afecta acceso a COP0. **n64-systemtest sigue 0/3721**
(sin regresión) y COP0Register pasa completo.

## 2026-08-09 — Medidor de velocidad realtime (% N64) + PD pinta boot screen

**Contador de rendimiento estilo "% de N64 real"** (sustituye la métrica contraintuitiva
tipo vps): en vez de instrucciones/seg crudas, se reporta velocidad como **% de una N64
real (100% = tiempo real)**, **por dominio hardware**.

- Modelo de reloj en `System::Clocks` (system.hpp): `cpuHz=93.75M`, `rspHz=62.5M`,
  `rdramHz=250M`, con multiplicadores de **overclock** por dominio (`cpuOc/rspOc/rdramOc`,
  1.0 = stock). El objetivo contra el que se mide es `hz*oc`, así que subir un multiplicador
  reescala el 100% de ese dominio — base lista para "overclock por región" a futuro.
- El intérprete modela **1 ciclo CPU emulado por instrucción retirada** (CPI≈1 baseline);
  `speed% = ciclos_emulados/seg ÷ reloj_objetivo × 100`. Ventana deslizante (~4/seg) →
  refleja velocidad *actual*, no promedio de vida. Tiempo en pausa NO se cuenta.
- Dominios: **CPU** (=instrucciones), **RSP** (=pasos RSP interleaved 2:3, cuenta en
  `stepCpu`), **RDRAM** (0 hasta modelar transacciones/ciclo).
- Superficie: heartbeat `[hb] ... | N64 speed: CPU x%  RSP y%` (env `KESTREL_HEARTBEAT`),
  y objeto `speed{cpuPct,rspPct,rdramPct,insns,overclock{}}` en telemetría `status`.

**Medido en PD**: CPU ~18% realtime (16.5 MIPS ÷ 93.75 MHz = 17.6% ✓), RSP 0% (PD gira en
spin-loop de boot, tarea RSP inactiva — correcto, no falseado).

**PD renderiza**: el soft-RDP pinta la pantalla "Nintendo 64 Product Identification"
(Product Code NUS-NPDE-USA). Arranca, corre game loop, pinta texto; luego spin `b .`
esperando (no llega a in-game aún). El cambio de Status boot (0x34000000→0x241000E0) NO
rompió PD.

## 2026-08-09 — Optimización intérprete +21% (protegido 0/3721) + plan dynarec

Host i7-870: baseline 16.5 MIPS ≈ 18% realtime = ~180 ciclos host/instr emulada (un
intérprete normal hace 30-50). Bloat identificado y recortado, TODO sin tocar 0/3721:
- **`debugArmed`**: OR de todos los traps de debug (bp/wild/pcwin/pcsample/audiohook/huft/
  maxinsn). El prólogo de `step()` (~140 líneas) se salta con UNA rama en vez de ~12. +16%.
- **`stepTraps()` fría** (`[[gnu::noinline, gnu::cold]]`): saca el prólogo debug del cuerpo
  de `step()` → hot loop compacto, mejor I-cache host. +2%.
- **jump-log gated**: `jlog*` solo lo usan dumps debug; se escribía en cada branch tomada
  (~1/6 instr). Ahora bajo `debugArmed`. +~2%.
- CMake: `-O2` incondicional iba DESPUÉS de `-O3` del perfil Release → clang usaba -O2
  (downgrade). Subido a -O3 + thin-LTO (LTO neutral, hot loop en 1 TU).

Total: **16.5 → ~20 MIPS (+21%), CPU ~21.5% realtime**. Techo del intérprete puro en este
host ~25-30%. Camino a 100% = **dynarec** (ver `JIT-PLAN.md`): etapas flag-gated con el
intérprete como oráculo/fallback para no arriesgar el 0/3721.

## 2026-08-09 (cont.) — Techo intérprete confirmado + cimiento dynarec (Etapa 1)

Tras el +21%, bisecté el coste por-instrucción con experimentos A/B/C (revertidos, eran
profiling):
- **A** throttle interrupt-poll 1/32 → +5% PERO rompe modelo Timing (descartado).
- **B** skip fetch (translate+icFetch) → **0%**.
- **C** skip D-cache sim (RDRAM directo) → **0%**.

Quitar fetch = 0, quitar dcache = 0, aislar ALU = 0, pero el agregado son ~150 cyc host/
instr. Diagnóstico: el cuello NO es ninguna operación concreta, es **thrash de I-cache del
host** — `execute()` (~3000 líneas) + `special()/cop1op()/…` gigantes desbordan la L1I
(32 KB). Por eso quitar ops sueltas no mueve nada. Corolario: la Etapa-1 "block-cache que
salta fetch" daría poco (B=0%); el único fix arquitectónico es **codegen** (bloque = código
host recto y diminuto, sin dispatch, gpr calientes en regs host).

Micro-opts extra post-+21% (lossless, **neutrales**, en árbol): interrupt-poll inline con
`deliverInterrupt()` frío; `reserved64` por bitmask (mata un 2º switch-dispatch por instr,
clang ya lo fusionaba). 0/3721 intacto.

**Cimiento dynarec montado (Etapa 1, `src/cpu/jit.{hpp,cpp}`)**: `CodeBuffer` RWX
(VirtualAlloc/mmap + FlushInstructionCache), `Emitter` x86-64 (REX/ModRM, mov/add/ret,
mem-operand disp8/32), `Block` cache. **NO cableado al hot loop** → 0/3721 por construcción.
Self-test (`KESTREL_JIT_SMOKE=1`): emite en runtime una función que suma dos slots y la
ejecuta → **PASS** (13 bytes, resultado correcto). Emisor+buffer+ABI Win64 validados.
Siguiente: Etapa 2 = compilar bloques de guest (ALU/loads/stores/branches) con fallback a
intérprete, validado en modo-diff. Ver `JIT-PLAN.md`.

## Dynarec Etapa 2a — HECHO y byte-exacto (2026-08-09), pero NO acelera PD todavía

Cableado el JIT al hot loop (`System::stepCpu`, gated `KESTREL_JIT`, default OFF → cero
regresión). Un bloque = run secuencial de ops **puras ALU** (ADDIU/ANDI/ORI/XORI/SLTI/
SLTIU/LUI + SPECIAL SLL/SRL/SRA/SLLV/SRLV/SRAV/ADDU/SUBU/AND/OR/XOR/NOR/SLT/SLTU) desde
una PC física KSEG0 alineada, sin fault/branch/mem/cop/HI-LO. Emite x86-64 recto
(`emitSafeOp`), firma `void(u64* gpr)`, base RBX. Guardas: RSP parado, no delay-slot, no
borde de timer/interrupt, revalida cada palabra vs `src[]` (SMC/DMA).

**Validación doble — oracle intacto**:
- Modo diff (`KESTREL_JIT_DIFF`): corre bloque + intérprete K pasos, compara gpr → **0
  mismatches** en systemtest.
- Modo nativo (`KESTREL_JIT=1`): systemtest **Failed 0 of 3721** (Timing 0/2, Cycle 0/6).

**2 bugs HW genuinos hallados+arreglados (generalizan, NO parches al test)**:
1. **SRA/SRAV quirk VR4300**: son shifts aritméticos de **64 bits** (signo de bit63 del
   registro completo), luego low32 sign-extend — NO shift de 32 bits. El intérprete ya lo
   hacía (`(s64)gpr[RT] >> SA`); el codegen inicial hacía 32b → 11 fallos. Fix: `ld64`+
   `shift64_imm/cl` (SAR de 64b). SLL/SRL/SLLV/SRLV sí son 32b.
2. **Guarda KSEG0 de 64 bits**: `(u32)pc` truncaba una dirección TLB de 64b
   (0x000000ff80204000) haciéndola pasar por KSEG0 → 1 fallo. Fix: exigir el rango
   sign-extendido completo `(pc & 0xFFFFFFFF_E0000000) == 0xFFFFFFFF_80000000`.

**Por qué NO acelera PD (medido, definitivo)**: `KESTREL_JIT_STATS` sobre PD →
`cover=0.0% avgK=2.11` (9 bloques en 67M llamadas). Dos causas independientes:
- **PD ejecuta desde memoria TLB-mapeada 0x0000000070xxxxxx**, NO KSEG0 (solo el 1er pc
  0x80001000 del IPL era KSEG0). La guarda KSEG0 correcta rechaza el 100% del código PD.
  Un JIT solo-KSEG0 no puede cubrir PD jamás.
- **avgK≈2**: aun con cobertura, un run de ops puras-ALU entre memoria/branch es ~2 instr;
  el overhead por bloque (sample interrupt + hash + loop de validación) supera 2 pasos de
  intérprete → Stage 2a es net-negativo (~6% más lento en PD con JIT ON).

Conclusión: la Etapa 2a es el **cimiento correcto** (emisor+cache+diff-harness validados) y
prueba el flujo end-to-end, pero un JIT ALU-solo-KSEG0 es **arquitectónicamente incapaz**
de acelerar PD. El win exige Etapa 2b (abajo). JIT sigue default-OFF: el intérprete enviado
es 0/3721 sin cambios.

Diagnósticos retenidos (todos env-gated, off por defecto): `KESTREL_JIT_DIFF`,
`KESTREL_JIT_STATS`, `KESTREL_JIT_ONLY1`, `opSelfTest` (bajo `KESTREL_JIT_SMOKE`).

### Etapa 2b paso 1 — entrada de bloque TLB-mapeada (2026-08-09): cobertura 0%→40%

Primer incremento de la Etapa 2b: permitir que un bloque arranque desde una PC
TLB-mapeada (PD corre desde `0x00000000_70xxxxxx`, no KSEG0). Diseño **sin riesgo al
hot-path** del intérprete:
- **Probe sin efectos**: `CPU::probing` flag + `tlbProbePhys(vaddr)`. Con `probing=true`,
  `translate()`/`tlbLookup()` reusan el match EXACTO pero cualquier path de fallo (AdE /
  TLB Invalid/Modified/Refill) retorna sentinela `~0ull` sin tocar estado (sin BadVAddr/
  EntryHi/memAbort/excepción). Los paths de fallo son COLD → el intérprete no cambia. Cero
  duplicación de lógica = cero drift.
- **jitTryBlock** entrada dual: ckseg0 directo (`pc & 0x1FFFFFFF`) o `tlbProbePhys`; ambas
  exigen RDRAM cacheable (pre-limpio `xlatCacheable` antes del probe → segmentos
  directo-uncached kseg1/ckseg1 declinan).
- **compileBlock** capa el bloque al borde de página 4K: en TLB-map los VA contiguos no son
  phys contiguos al cambiar de página, así que `phys+4*i` dejaría de corresponder. Cap
  correcto también para KSEG0 (solo acorta).

**Validado**: systemtest nativo `KESTREL_JIT=1` **Failed 0/3721** (Timing 0/2, Cycle 0/6) —
prueba fuerte: los resultados JIT REALES condujeron toda la suite (incl. tests TLB) y pasan.

**Medición decisiva** (`KESTREL_JIT_STATS` sobre PD): cover **0%→~40%**, pero **avgK cae a
~1.1** (blocksRun≈opsJIT). El código PD real intercala ALU con memoria/branch → los runs
PUROS-ALU son ~1 instr. Confirma sin ambigüedad: **Stage 2a (ALU-solo) no puede acelerar
aunque cubra el 100%** — el overhead por-bloque se paga por ~1 instrucción → net-negativo.
El siguiente paso obligatorio de la 2b es **compilar basic-blocks reales con loads/stores
(y branches terminales)** para que avgK suba a ~5-7 y el codegen recto amortice.

### Etapa 2b paso 2 — maquinaria call+bail validada aislada (2026-08-09)

Cimiento para loads/stores in-block, construido y self-testeado SIN cablear (oracle 0/3721
por construcción, patrón Etapa-1). Emitter nuevo (`jit.hpp`): `push_reg/pop_reg`,
`sub/add_rsp_imm8` (shadow space), `call_reg`, `test_al_al`, `je/jmp_rel32_placeholder` +
`patchRel32` (saltos hacia delante con parcheo). Self-test `callBailSelfTest`
(`KESTREL_JIT_SMOKE=1`): emite `fn(gpr,ctx)` = [ALU op0][call helper C; si retorna 0 →
**bail** return índice][ALU op2] return K. Verifica los 2 caminos → **PASS/PASS**: sin
fault corre op0+op2 y devuelve K=3; con fault corre solo op0, salta op2 y devuelve 1.
Valida ABI Win64 (RCX/RDX args, alineación RSP 16 en el CALL, shadow 32B) + patch de rel32
+ índice de bail en compile-time.

**Crux pendiente de cablear** (siguiente sesión): el helper `CPU::jitMem(op)` que ejecuta
un load/store espejando EXACTO el intérprete (mismos `readN/writeN`/`dcWrite`/storeRepeat/
storeCart/wordStoreQuirk). Coordinación de fault-en-bloque: si `jitMem` vectoriza la
excepción (cambia pc/EPC), el driver nativo NO debe re-avanzar pc — debe detectar `memAbort`
y contabilizar solo las ops retiradas. Firma de bloque pasa a `u32 fn(u64* gpr, CPU* cpu)`
(rbx=gpr, r12=cpu; ALU codegen intacto). Validar con `KESTREL_JIT_DIFF` antes de activar.

## Dynarec Etapa 2b — loads/stores en bloque: CORRECTO (0/3721) pero SIN speedup todavía

**Hecho.** Los bloques JIT ahora incluyen loads/stores alineados simples
(LB/LH/LW/LBU/LHU/LWU/LD/SB/SH/SW/SD) además de la aritmética ALU. Firma de bloque:
`u32 fn(u64* gpr, void* cpu)` (rbx=gpr, r12=cpu). Cada mem-op emite
`call jitMemThunk(cpu,op)` + `test al,al` + `je bail`. `jitMem` (cpu.cpp) ejecuta el
load/store espejando EXACTO el intérprete pero **nunca vectoriza**: un `translate` en
modo `probing` (sin efectos) decide bail-o-ejecuta; si faultaría (misalign/TLB/ADE, o
LWU/LD/SD reservados en modo no-kernel sin UX/SX) devuelve 0 → el bloque retorna el índice
de la op (retiro parcial) y el intérprete re-ejecuta esa op para levantar la excepción
exacta. El driver avanza pc/Count/retired/Random por el valor de retorno `R` (no por K).
`jitMem` traduce UNA sola vez (captura la phys del probe y la reúsa) para igualar el coste
del intérprete.

**Oráculo verde:** systemtest con `KESTREL_JIT=1` = **Failed 0 of 3721** (Timing 0/2,
Cycle 0/6), tanto en nativo como en `KESTREL_JIT_DIFF` (los bloques con memoria se validan
solo por systemtest; los solo-ALU siguen con diff contra el intérprete). Un bug se pilló y
arregló: LWU/LD/SD son ops de 64b reservadas (RI) en modo no-kernel — se añadió el gate
side-effect-free (mismo criterio que `reserved64()`, sin vectorizar → bail).

**avgK subió de ~1.1 a ~3.2** (KESTREL_JIT_STATS sobre PD): los mem-ops ya no cortan el
bloque, así que un run medio captura ~3 ops guest en vez de ~1.

**PERO: JIT ON es MÁS LENTO que el intérprete en PD** (~13.5 vs ~19.4 Mips en host i7-870).
Bisección por probes de decline (medido, luego revertido):

| Configuración | Mips | Coste incremental |
|---|---|---|
| Intérprete puro | 19.4 | — |
| + call jitTryBlock + guards baratos | 17.3 | dispatch **2.1** |
| + sample de interrupt | 16.6 | sample **0.7** |
| + `tlbProbePhys` (probe TLB por instr) | 13.7 | **probe 2.9** |
| full JIT (ejecuta bloques) | 13.5 | ejecución de bloque **≈0 (neutral)** |

Dos hechos duros:
1. El **probe TLB por instrucción** (2.9) es el mayor impuesto, luego el dispatch (2.1).
   Se pagan en el **58% de los steps que declinan** (delay-slots, branches, cop1, mult/div,
   y leaders cuyo primer op no es compilable). `jitTryBlock` corre en CADA instrucción.
2. **La ejecución de bloque es neutral** (full-JIT 13.5 ≈ probe-solo 13.7): los bloques
   corren a la par del intérprete porque los mem-ops siguen llamando al helper C `jitMem`
   (call indirecto + `translate` completo dentro). El ALU inline SÍ es más barato, pero el
   ~40% de mem-ops lo iguala.

**Conclusión:** recortar overhead no basta — aun con un cache de traducción perfecto se
llegaría a ~16.6, todavía < 19.4, porque el coste por-op del bloque no está por debajo del
intérprete. El speedup real exige **(a) softTLB + fast-path de RDRAM inline en el código
emitido** (sin call C, sin re-traducir: puntero host a rdram + índice), **(b) branches +
delay-slot dentro del bloque** para bajar la tasa de decline del 58%, y **(c) cache de
traducción de la PC de entrada** (invalidado por TLB/ASID/modo). Es Etapa 2c/3, un build
aparte y grande. Hasta entonces **JIT queda default-OFF** (es honestamente más lento hoy);
el intérprete shippeado sigue 0/3721 intacto.

## Dynarec Etapa 2c-parcial — softTLB de entrada + RAÍZ del colapso de cobertura

**softTLB (hecho, 0/3721).** La traducción TLB de la PC de entrada del bloque hacía un scan
lineal de 32 entradas por instrucción (el impuesto "probe 2.9"). Añadida una cache de UNA
entrada `jitTlb{Vpn,Asid,Phys,Cacheable,Valid}` (cpu.hpp): un hit del mismo (vpn,asid) salta
el scan. Tag incluye ASID → cambio de contexto = miss natural que re-prueba. Invalidada solo
en `tlbWrite` (único punto que muta el TLB). Oráculo systemtest **0/3721** con JIT ON. Es la
base correcta para bajar el coste por-entrada, pero **por sí sola no voltea el veredicto**.

**RAÍZ del "JIT ON más lento" en PD (diagnóstico afilado, medido).** A/B mismo build,
MAXINSN, fase: intérprete **16.2 Mips** (CPU ~18% N64) vs JIT ON **~12–14 Mips y bajando**.
El `KESTREL_JIT_STATS` sobre 800M insns lo explica:

```
cover 48.2% avgK 3.31  (temprano, boot)
cover 40.6% ... 31.6% ... y blocksRun/opsJIT CONGELADOS ~56M/191M mientras calls sube a 600M
```

`opsJIT` y `blocksRun` se **congelan**: a partir de ~544M insns `jitTryBlock` declina el
**100%** de las llamadas. No es churn de SMC (los bloques que corren siguen con avgK 3.4). Es
que **la fase steady de PD = game-loop dominado por branches/jumps** (spin esperando RSP/RDP/
VI, dispatch de actores, saltos indirectos). El JIT actual compila SOLO runs rectos y **corta
en el primer branch** → en el boot (memcpy/descompresión) cubre ~48%, pero en el bucle de
juego no encuentra nada recto que compilar → cobertura ≈0%, y queda como puro overhead de
dispatch sobre el intérprete.

**Conclusión honesta.** El techo del block-JIT recto NO es el probe (ya cortado por softTLB)
sino la **cobertura**: sin branches-en-bloque el dynarec no toca el hot-loop real de PD. El
speedup exige DOS builds grandes, ambos necesarios (ninguno solo voltea):
1. **Branches + delay-slot dentro del bloque** (con salidas laterales por condición) → sube la
   cobertura del ~0% steady a la mayoría del game-loop. *Es la palanca dominante para PD.*
2. **Fast-path RDRAM inline en x86** (puntero host directo, sin call C a `jitMem`) → hace que
   los bloques cubiertos corran de verdad por debajo del intérprete.

**JIT sigue default-OFF.** El intérprete shippeado (16.2 Mips, 0/3721) es hoy lo rápido y
correcto. softTLB queda dentro (lossless, off-path cuando JIT off).

---

## Branches-en-bloque + primer speedup del dynarec (2026-08-09)

El block-JIT ahora **absorbe BEQ/BNE + su delay-slot** y escribe el control de flujo directo
(cmov target/fallthrough → pc/nextPc), cerrando el bloque con flag de control. Esto sube la
cobertura del game-loop de PD (que colapsaba a ~0% porque el JIT recto cortaba en cada branch):
avgK 3.4→5.54.

**Resultado:** primer build donde el dynarec **gana** al intérprete en PD:
~23 Mips → **~32 Mips (+~40%)** (misma ventana 35s: 601M vs 965M insns). **systemtest sigue
0/3721** (base/timing/cycle) con JIT ON, RI=6 (los 6 tests intencionales, sin loop).

**Bug root-caused y arreglado (regresión de la feature):** R12 no sobrevivía el `call
jitMemThunk` de forma fiable → `cpu` corrupto tras la 1ª mem-op → (a) part2 escribía pc en
`gpr[]` y (b) la 2ª mem-op mal-direccionaba stores, corrompiendo la copia IPL→DMEM del boot →
RI infinito. Fix genérico y HW-agnóstico: direccionar `cpu` vía RBX (== `&gpr[0]`, con
`static_assert(offsetof(CPU,gpr)==0)`), sin depender de R12. Detalle en docs/JIT-PLAN.md.

JIT sigue **default-OFF** (activar con `KESTREL_JIT=1`); el intérprete shippeado es correcto y
ya no es el único camino rápido.

### Saltos absorbidos + SMC inline (2026-08-09)

Extendida la absorción a **J/JAL/JR/JALR** (retornos/llamadas de PD) y **inlineado el loop de
validación SMC** (mismo comportamiento byte-a-byte, sin K llamadas cross-TU a icFetch). Números
limpios en misma ventana 25s: intérprete **21.2 Mips** → JIT BEQ-solo **26.4** (1.24x) → JIT full
**29.5 Mips (1.39x)**. Jumps = **+12%** limpio (A/B `KESTREL_JIT_NOJMP`), SMC-inline ~+5%.
**systemtest 0/3721** intacto. avgK baja 5.54→~2.5 porque ahora capturamos bloques de retorno
(JR `$ra`) antes 100%-declinados → el cuello se desplaza al **overhead del driver por bloque**.

**Nota honesta sobre "10x":** el dynarec hoy es **~1.4x** el intérprete, no 10x. El 10x es techo
teórico que exige register-allocation cross-op + mem-ops inline + **block linking**. Siguiente
palanca (la de más impacto ahora): **block linking** (bloque→bloque sin volver al driver;
requiere validación SMC por-bloque para invalidar enlaces sin `clear()` global). Ver JIT-PLAN.md.

## 2026-08-10 — Autocorrección: PD SÍ ejecuta RSP+RDP y renderiza (mi "benchmark inválido" era ventana corta)

El usuario cuestionó el benchmark previo ("emulador roto, no ejecuta RSP o tienes bucle stall") y
mi propia nota "6 kicks gfx y silencio, RSP 0%". **Tenía razón en desconfiar, pero la causa que yo
di era falsa.** Diagnóstico con trazas (`KESTREL_RSPTRACE`/`RDPOPS`/`IRQTRACE`, dump CP0 al cap,
`FBDUMP`):

- **Los "6 kicks y silencio" eran artefacto del cap corto (200M instr).** Con cap 800M aparecen
  **20 kicks**: #1-6 `type=1 ucode=0005a0b0` (init RDP, DL 0005dcc8), luego **#7-20 `type=2
  ucode=0005b4d0`** (ucode gfx custom PD) alternando DOS DLs `000df0f0`/`000e2f70` =
  **display-lists double-buffered = game loop renderizando frames**.
- **RSP SÍ ejecuta el ucode gfx custom** y produce stream RDP real. Con RDPOPS: **SET_COLOR_IMAGE
  (0x3f)** addr framebuffer 0x489800, **FILL_RECT (0x36)**, **TEXTURE_RECTANGLE (0x24)**, SET_TILE,
  LOAD_BLOCK, SET_SCISSOR, SET_COMBINE, SET_PRIM_COLOR. (Aún sin triángulos 0x08-0x0f = render 2D,
  no 3D todavía.)
- **Interrupts OK:** `Int(0)=9493` en 800M (el "Int=6" era del run 200M). VI retrace, SP, DP, SI,
  PI todos disparan (`mi_mask=3f`). Status `IE=1 EXL=0 IM=ff`. El spin `b .` en pc=0x70001938 es el
  **idle thread legítimo entre VI fields**, NO un stall bug.
- **FBDUMP** (576x240, origin swapping 0x76a440↔0x400480): pinta la pantalla **"Nintendo 64 Product
  Identification"** vía texture-rects. Emu renderiza el boot 2D de PD correctamente.

**Estado real:** el emu ejecuta RSP+RDP y PD renderiza su boot. PERO se queda en la pantalla
product-ID (frames alternan pero contenido estático) — gate de boot o simplemente lento a ~16%
realtime; **no llega a gameplay 3D**, por eso pocos kicks / RSP-load bajo. NO es "emu roto": es PD
en boot/menú, no en combate.

**Corolario benchmark:** para medir carga RSP/RDP representativa hace falta **PD en gameplay 3D**
(triángulos), no el boot. Vía = autowarp (`mem.write` de `g_MissionConfig` + `g_MainChangeToStageNum`,
documentado) o avanzar el boot. Las cifras Mips/% previas medían boot/idle, no gameplay → no
representativas (el usuario tenía razón en eso).

**"170 vs 30-40" (host-cyc/instr):** CONFIRMADO real y **NO corregido del todo**. Intérprete
~180 cyc/instr vs 30-50 normal; raíz = thrash I-cache host por `execute()` gigante; dynarec da
~1.4x parcial, techo intérprete ~21%. Fix pleno = block-linking dynarec (ver arriba).

systemtest sigue **0/3721** (0/2 Timing, 0/6 Cycle) tras estos cambios (solo diagnósticos:
dump CP0 al cap `[cp0]`, gated).

---
## 2026-08-10 (bis) — Segunda autocorrección: PD NO se queda en product-ID, PROGRESA

**El "se queda en product-ID / pocos kicks" era OTRO artefacto de cap de log**, hermano gemelo
del error RSP-0%. El trace RSP capa el LOG a `kicks<=20 || kicks%100==0` → yo leí "kicks paran en
20". FALSO. Con cap 800M el log muestra **KICK #300** (y subiendo): PD renderiza cientos de frames.

**Evidencia dura (lectura directa RDRAM, KSEG0→phys `&0x1fffffff` big-endian; `read32`-en-cap
estaba roto y devolvía 0 → por eso los thread-walks previos daban basura):**

| Global (addr)                    | cap 300M       | cap 800M        | veredicto        |
|----------------------------------|----------------|-----------------|------------------|
| `__osViIntrCount` (0x80078104)   | 137            | **595**         | VI avanza        |
| `g_TitleMode` (0x800473d0)       | −1             | **4**           | modo avanza      |
| `g_TitleTimer` (0x800473c0)      | 0              | **1**           | avanza           |
| `g_Vars.lvupdate60` (0x80075784) | 0              | **4**           | frame-delta vivo |
| `__osRunningThread` (0x800463e4) | g_MainThread   | g_MainThread    | main vivo        |
| kick ucode                       | 0x0005b4d0     | **0x00041b60**  | ucode gfx juego  |
| kick DL                          | 000df0f0/e2f70 | **20a950/20e7d0** | contexto nuevo |

PD progresó legal(mode −1) → **title mode 4**, ucode cambió al gfx real de juego (0x00041b60),
DLs nuevos. **El emulador ejecuta RSP+RDP y PD avanza correctamente por el arranque. NO está roto
ni colgado. Solo va lento (~16-18% realtime).**

**Lección (3ª vez):** NUNCA concluir "colgado/roto/parado" desde un LOG CAPADO. Los tres sustos
("BLOQUEO SW", "RSP 0%", "freeze en 14 frames") fueron artefactos de instrumentación, no bugs del
emu. Verificar SIEMPRE con lectura de estado real (globals RDRAM, contadores) antes de declarar
patología. El usuario tenía razón las tres veces.

**GOTCHA nuevo:** `read32()` llamado desde el bloque de cap devuelve 0 para todo (KSEG0 incluido)
— path de lectura CPU no válido en ese contexto. Para inspección de memoria en cap usar acceso
DIRECTO a `mem->rdram[va&0x1fffffff]` big-endian (como `dumpFramebufferBmp`). `KESTREL_MEMDUMP`
ya lo hace así.

**Dirección REAL de trabajo:** el único cuello genuino es VELOCIDAD (dynarec block-linking), no
ningún hang. PD ya renderiza y avanza; para llegar a gameplay 3D basta correr más tiempo (o
autowarp). Dejar de cazar hangs fantasma; invertir en dynarec. systemtest sigue 0/3721.

---
## 2026-08-10 (ter) — Dynarec: absorber BLEZ/BGTZ/BLTZ/BGEZ (branches 1-reg vs 0)

Extendida la absorción de branches-en-bloque (`jit.cpp compileBlock`) a los cuatro branches
condicionales de un registro contra cero, que dominan los loops de PD junto a BEQ/BNE:

| op        | encoding            | condición (signed 64b) | setcc |
|-----------|---------------------|------------------------|-------|
| BLEZ      | LO=0x06, rt=0       | rs <= 0                | 0x9E  |
| BGTZ      | LO=0x07, rt=0       | rs >  0                | 0x9F  |
| BLTZ      | REGIMM LO=0x01 rt=0 | rs <  0                | 0x9C  |
| BGEZ      | REGIMM LO=0x01 rt=1 | rs >= 0                | 0x9D  |

Comparten fases B/C con BEQ (mismo target `VA+4(idx+1)+SIMM*4`, fallthrough `VA+4(idx+2)`);
solo cambia la fase A: `ld64 rs; cmp64_imm rs,0; setcc(cc)→[rsp+32]` (cmp vs 0 nunca desborda →
OF=0 → setl/setge/setle/setg válidos). **EXCLUIDAS** las variantes *likely* (BLEZL/BGTZL 0x16/0x17,
REGIMM rt bit1 = BLTZL/BGEZL/…) porque anulan el delay slot cuando NO se toma — semántica distinta
a esta maquinaria (que siempre ejecuta el delay slot); caen al intérprete. Las de enlace
(BLTZAL/BGEZAL) también quedan en intérprete de momento.

**Validación (regla: semántica HW genuina, nada de hacks al test):**
- `KESTREL_JIT=1` systemtest **Base 0/3721, Timing 0/2, Cycle 0/6** — el codegen con bcondz activo
  condujo TODA la suite (incl. tests de branch y TLB de 64-bit).
- `KESTREL_JIT_BRDIFF=1` (valida bloques con branch pure-ALU contra el intérprete paso a paso):
  **0 mismatches** (el único "mismatch" en el log era el NOMBRE de un test TLB).

**Perf PD (i7-870, secuencial limpio, cap 600M, heartbeat Mips):**
`intérprete OFF ~16.0 Mips → JIT ON ~29 Mips = ~1.8x` (avgK 2.55–2.87, ctrl-exits 6.6M/600M).
Mejor que el 1.4x previo: las nuevas condiciones extienden la cobertura del game-loop de PD.
JIT sigue **default-OFF** (intérprete = oráculo enviado, 0/3721). Diag retenido: JIT_BRDIFF,
JIT_NOBRANCH, JIT_NOJMP.

Sig palanca de mayor impacto (JIT-PLAN.md): **block-linking** (bloque→bloque sin volver al driver;
exige invalidación SMC por-bloque) + fast-path RDRAM inline (puntero host, sin `call jitMem`).

---

## M3-thread — Reconstrucción multihilo (paso 1): RDP async + VI continuo (2026-08-10)

Inicio del mandato usuario: reconstruir core a multihilo e implementar todo RSP/RDP/VI/input,
LUEGO optimizar intérprete, LUEGO dynarec, y solo entonces volver a PD. Diseño completo en
`docs/ARCH-THREADING.md` (mapa de puntos de sync + estrategia de determinismo).

### Entregado este paso
- **Modo RCP dual** (`Memory::RcpMode`): `Lockstep` (default, = comportamiento previo exacto,
  ruta determinista que systemtest valida) vs `Threaded` (`KESTREL_THREADS=1`).
- **RDP en hilo propio** (threaded): `DPC_END` ya no rasteriza dentro del store CPU; encola
  `{current,end,xbus}` en una cola SPSC y un worker lo mastica async, subiendo MI_DP en SYNC_FULL
  — como el HW real (el RDP mastica la FIFO mientras el CPU sigue). Ambas rutas pasan por el mismo
  `rdpRunJob` → resultados idénticos, solo difiere el *timing* del interrupt/visibilidad de píxeles.
- **Sincronización**: `mi_intr` y `dpc_status` → `std::atomic<u32>`. El `fetch_or` de `raiseIntr(MI_DP)`
  es la barrera release que publica los writes de píxeles antes de que el CPU observe el interrupt
  (acquire en su load de mi_intr). Contrato HW: el juego espera el interrupt antes de tocar el buffer,
  así que no hace falta coherencia por-píxel.
- **VI_CURRENT continuo** (`mmioRead32` VI 0x10): antes `viTick` avanzaba el contador +2 por batch
  de 750k insns → 125× demasiado lento; un rom que hace spin en `VI_CURRENT==N` (el vsync-wait
  estándar bare-metal) tardaba ~192M insns en pintar. Ahora el valor *leído* se deriva del reloj de
  instrucciones retiradas (CPI≈1) a ~60 campos/s: `hl = (retired/cyclesPerHalfline) % total`. HW-fiel
  (el contador free-runs con el reloj de vídeo). El path de *interrupt* (viTick) NO se toca →
  systemtest Timing intacto.
- **RDP fill-triangle** (`drawTriangle`): en `CYCLE_TYPE_FILL` el triángulo se pinta con el
  `FILL_COLOR` empaquetado (2 píxeles/word, como FILL_RECTANGLE) — es lo que emite el
  `Fill_Triangle` bare-metal. Antes ignoraba fill_color/cycle-type y pintaba un gris fallback.

### Validación (oráculo krom, sin tocar PD)
- `RDP/16BPP/Triangle/FillTriangle` renderiza: rojo/verde/azul/blanco sobre fondo amarillo,
  **98.9% exact-match vs el PNG referencia HW** (el 1.1% restante = píxeles AA de borde que el
  raster no-AA no produce; geometría y color exactos).
- **Lockstep == Threaded byte-a-byte** (mismo md5 del framebuffer) — la ruta threaded es correcta,
  no solo rápida.
- **systemtest intacto: Base 0/3721, Timing 0/2, Cycle 0/6** tras todos los cambios (lockstep default).

- **RDP Gouraud-triangle** (`drawTriangle`): en 1/2-cycle con bloque shade (words≥12) se interpolan
  los RGBA por-vértice. Coeficientes s16.16 partidos int-hi/frac-lo en dos words 64-bit: start=w4/w6,
  DcDx=w5/w7 (por +x), DcDe=w8/w10 (por +y). Por scanline el color arranca en el edge mayor
  (start + DcDe·(y−yh)) y avanza por-píxel (+ DcDx·(x−xA)); byte = clamp(v+0.5, 0, 255). Además
  se arregló el conteo de words del FIFO: shade +8, textura +8, zbuffer +2 (antes +4/+4/+1, mitad →
  desincronizaba el FIFO en triángulos con shade/tex/z; FillTriangle funcionaba por suerte, op 0x08
  sin flags → n=4 correcto).

### Validación (oráculo krom, sin tocar PD)
- `RDP/16BPP/Triangle/FillTriangle` renderiza: **98.9% exact-match vs PNG referencia HW**.
- `RDP/16BPP/Triangle/ShadeTriangle` (Gouraud): **90.5% exact, 98.45% cerca (≤8/canal), RMSE 2.11**
  vs PNG referencia HW (el resto = AA de borde + redondeo subpíxel; el gradiente es correcto).
- **Lockstep == Threaded byte-a-byte** (mismo md5) en ambos roms.
- **systemtest intacto: Base 0/3721, Timing 0/2, Cycle 0/6** tras todos los cambios.

- **RDP textura**: `loadTile` copia texels de la texture-image (RDRAM) a TMEM en LOAD_TILE
  (rectángulo 10.2) y LOAD_BLOCK (run lineal), y **latchea la caja de clamp del tile** (SL/TL/SH/TH),
  que en HW comparte registros con SET_TILE_SIZE — sin esto el sampler clampa todo a (0,0). `sampleTexel`
  decodifica RGBA5551/IA16/I8/IA8/RGBA32 desde TMEM (point-sample, wrap por mask o clamp a la caja).
  Path textura en `drawTriangle`: coeficientes S/T s10.5 (start w0/w2, DsDx w1/w3, DsDe w4/w6; bloque
  tras edge+shade), texel = `floor(S)>>5`, alfa 5551 respetado (A=0 → transparente).
  Validado krom `TextureTriangleRGBA16`: flechas 8×8 renderizan con forma/posición/color/alfa correctos
  (84% exact). **Limitación conocida**: cola negra en flechas magnificadas = clamp de T repite la fila-7
  (stem opaco) donde el triángulo cubre de más — refinamiento de coverage/clamp, NO de decode.

### Validación textura
- `TextureTriangleRGBA16`: **84% exact**, flechas correctas; lockstep==threaded byte-idéntico;
  systemtest sigue **Base 0/3721, Timing 0/2, Cycle 0/6**.

### Input real (teclado → pad P1)
- `pumpFrame` (present.cpp) lee el teclado GLFW cada frame y publica el estado del pad P1 en
  `mem->padButtons` + `padStickX/Y`. Mapa: X=A C=B Space=Z Enter=Start Q=L E=R, flechas=D-pad,
  I/J/K/L=C-buttons, W/A/S/D=stick analógico (±80). El joybus (`pifProcessJoybus` cmd 0x01) ya
  devolvía los 2 bytes de botones; ahora también rellena bytes 2/3 con el stick s8. `KESTREL_BUTTONS`
  sigue sirviendo headless. systemtest intacto (no testea el pad): **Base 0/3721, Timing 0/2, Cycle 0/6**.

### RSP en hilo propio (threaded mode)
- Worker RSP espejo del RDP (`rspWorkerLoop`/`rspSubmitKick`). En threaded, un SP-release del CPU
  (clear-HALT) despierta al worker que ejecuta la tarea completa a BREAK vía `rsp.run()` (sube MI_SP
  él mismo). El interleave 2:3 del hilo CPU se **desactiva** en threaded (`stepCpu` gated en Lockstep)
  para no doble-ejecutar el core. `rspBusy` (atomic) evita re-kick mientras la tarea corre. Los
  DPC_END que emita el microcódigo pasan por `mmioWrite`→`rdpSubmit`, así el RDP pipelinea detrás.
- Mismo contrato productor/consumidor que el RDP: el juego espera el interrupt SP antes de leer la
  salida, así que la barrera release del `raiseIntr(MI_SP)` basta (sin coherencia por-byte). Lockstep
  sigue siendo el default determinista que valida systemtest.
- **Flat-color de triángulo** (op sin shade/tex/fill): sin modelo de combiner/blender, el color
  constante se toma de prim→blend→env→gris. Corrige demos que ponen el color por-triángulo en
  BLEND_COLOR (RSPPlotTriangle).

### Validación RSP threaded (oráculo krom)
- `RSP/Gradient` (RSP escribe framebuffer 32bpp): lockstep==threaded byte-idéntico y **100% exact,
  RMSE 0 vs PNG HW**.
- `RSP/XBUS/RSPPlotTriangle` (RSP→RDP xbus, 8 triángulos fill con blend-color): lockstep==threaded
  byte-idéntico y **100% exact, RMSE 0 vs PNG HW**.
- **systemtest intacto: Base 0/3721, Timing 0/2, Cycle 0/6** tras RSP-thread + flat-color.

### Z-buffer (RDP depth test/update)
- **Formato z N64 genuino** (`SoftRdp::zEncode/zDecode`, rdp.hpp): profundidad de pipeline =
  entero 18-bit (0..0x3FFFF), almacenada en 16 bits como float lineal-a-trozos: exponente 3-bit +
  mantisa 11-bit empaquetados en bits[15:2] (los 2 bits bajos = dz, sin modelar — irrelevante a
  fill/compare). 8 segmentos `zBase`/`zShift`; encode/decode inversos exactos al base del segmento
  → orden preservado como en HW. El clear 0xFFFF decodifica a 0x3FFFF (lo más lejano).
- **Coeffs Z** (bloque op&1, 2×u64 tras edge+shade+tex): word0[63:32]=Z, word0[31:0]=DzDx,
  word1[63:32]=DzDe (por +y en edge mayor). Todos s16.16. Interp per-pixel z = Z + DzDe*(y-yh) +
  DzDx*(x-xA), clamp [0,0x3FFFF].
- **Modos** (other_lo): Z_COMPARE_EN(0x10) gate del test, Z_UPDATE_EN(0x20) gate del write-back,
  Z_SOURCE_SEL(0x04) toma prim-depth constante (SET_PRIM_DEPTH op 0x2e, prim_z=(cmd>>16)&0xffff) en
  vez del z interpolado. Z-mode opaco: pasa si newz < old (estricto). En pass con Z_UPDATE re-encode
  y escribe raw16 a zi_addr. El z-test envuelve los 3 caminos de píxel (textured/gouraud/flat), no
  fillMode; textura respeta alfa antes del z (sin z-update si el texel mata el píxel).
- **Validación krom**: FillZBufferTriangle 16BPP y 32BPP ambos **98.1% exact (1.90% diff), RMSE ~22,
  y crucialmente interior_block_diff=0** (erosión: TODO píxel interior casa con HW; los 1459 diffs
  son 100% bordes de silueta = anti-aliasing no modelado, misma clase que FillTriangle 1.1%).
  Z-compare elige el ganador rojo/verde exacto en cada solape. lockstep==threaded byte-idéntico (md5).

### TEXTURE_RECTANGLE sampleado real + tile shift/mask/mirror/clamp
- **SET_TILE** ahora parsea cmS/maskS/shiftS/cmT/maskT/shiftT (antes solo fmt/size/line/tmem/pal).
- **sampleTexel**: aplica SHIFT (1..10→>>, 11..15→<<) a la coord, luego wrap por eje —
  maskN da periodo 2^mask, cmN bit0=mirror (onda triangular sobre 2·periodo), bit1/sin-máscara=clamp
  a la caja SET_TILE_SIZE.
- **texRect real** (antes pintaba color plano): word0 XL/YL/tile/XH/YH(10.2), word1 S/T(s10.5,
  1/32-texel) + DsDx/DtDy(s5.10, 1/1024 texel/px). S arranca en XH pasos DsDx/px-x, T en YH pasos
  DtDy/px-y; FLIP intercambia ejes S/T. Muestrea el tile por texel, respeta alfa 5551.
- **Validación krom**: TextureRectangle RGBA16 96.4% (3.64% diff), interior_block=27 (solo el rect
  mirror 32×32). TextureTriangle RGBA16 **saltó 84%→96.4%** — el parse shift/mask/clamp de SET_TILE
  arregló la mayor parte de la "cola negra" (era clamp a (0,0) por caja mal). El residual restante
  (~2700 px, uniforme) = filtrado **bilineal** (BI_LERP en other_modes) que HW aplica y aún hacemos
  point-sample → bordes de texel suavizados; misma clase que el AA. lockstep==threaded md5-idéntico.
  systemtest 0/3721 (6.86s) intacto tras tocar sampleTexel/SET_TILE compartidos.

### Filtro de textura 3-point N64 (SAMPLE_TYPE)
- **sampleTexFiltered** (rdp.cpp): point-sample salvo que SAMPLE_TYPE (other_hi bit 13 = full-mode
  bit 45; G_TF_BILERP/AVERAGE ponen bit 13, POINT=0) pida el filtro. El RDP **no es bilineal 4-tap**:
  toma el triángulo de 3 texeles alrededor de la muestra y lerpa por las fracciones. Si sf+tf<=1 usa
  taps (0,0),(1,0),(0,1); si no, (1,1),(1,0),(0,1). **Sin bias -0.5** (eso es GL-ism; el RDP direcciona
  orígenes de texel, sfrac/tfrac = bits fraccionales bajos de S/T). Cableado en textured-triangle y texRect.
- **Validación**: en el oráculo krom Cycle1Texture* el modo es **G_TF_POINT** → el branch de filtro no
  se toma (POINT==FILTER byte-idéntico, md5 igual, 96.36% exact, mismos 2796 diffs = AA de silueta). El
  path point queda intacto. El filtro solo se activa en contenido BILERP real (PD es textura-intensivo);
  matemática de esquinas verificada a mano (continua en la diagonal sf+tf=1). Los 95.94/96.06 vistos en
  runs intermedios fueron **races de captura** (BMP leído a medio escribir), no regresión: el md5 vs
  baseline validado (27f11e77) es idéntico.

### Color combiner (SET_COMBINE) — genuino, 1-ciclo completo + 2-ciclo aprox
- **Decode** (case 0x3c): dos sets `comb[0]`(cyc0)/`comb[1]`(cyc1), cada uno A/B/C/D para RGB y alpha,
  con el packing GBI exacto (aR=hi>>20&f, bR=lo>>28&f, cR=hi>>15&1f, dR=lo>>15&7; alpha aA=hi>>12&7…;
  cyc1 usa hi>>5, lo>>24, hi>>0, lo>>6, y los campos alpha de lo). 
- **combineColor(tex0,tex1,shade)**: por canal `out = (A-B)*C/255 + D` redondeado (el **/255 mantiene la
  identidad (1-0)·x lossless** — con >>8/256 el modo C=TEXEL0 oscurecía 255→254). Mux estándar: sub_a
  {COMBINED,TEX0,TEX1,PRIM,SHADE,ENV,1,NOISE}, sub_b/add sin el 1/noise, mul de 16 con los *_ALPHA
  (idx 7..12) y LOD/K5→0; alpha con su tabla (mul idx0=LOD→0). **1-ciclo evalúa comb[1]** (quirk RDP:
  la pasada única usa la ecuación del 2º ciclo); **2-ciclo** corre comb[0]→COMBINED→comb[1]. LOD/key/K4/K5
  y el offset de texel del 2-ciclo aún no modelados (documentado).
- **Ruteo**: los 3 caminos de triángulo (textured/gouraud/flat) y texRect pasan por el combiner cuando
  está programado, con **fallback al comportamiento viejo si combine_hi|lo==0** (evita negro pre-SET_COMBINE).
  Textura además pasa shade al combiner → MODULATE (texel·shade) ya funciona. **Gate de visibilidad =
  alfa del TEXEL** (bit 5551 de transparencia), NO el alfa combinado (la ecuación alpha suele ser un factor
  de blend, no una máscara de cobertura) — esto arregló el bug de "todo negro" (comb alpha C=LOD=0).
- **Validación**: TextureTriangle **byte-idéntico al baseline** (md5 27f11e77, modo = identidad TEXEL0) →
  cero regresión. ShadeTriangle **RMSE 2.11, close 98.45%** (shade por combiner casa con HW; el exact 90.5%
  es el dither/AA de siempre). lockstep==threaded md5-idéntico (SHADE e7c8471c, TRI 27f11e77). systemtest
  **0/3721** (6.86s) intacto tras tocar drawTriangle/texRect/SET_COMBINE compartidos.

### TLUT + texels 4-bit (CI4/CI8/IA4/I4/IA8/I8) — genuino
- **LOAD_TLUT** (case 0x30): copia entradas 16-bit del texture image a `tlut[256]`; `tlutMode()` = bits
  14-15 de other_hi (2=RGBA5551, 3=IA16). `sampleTexel` decodifica CI4 (`palette*16+nib`) / CI8 (byte)
  vía `tlutLookup`. Formatos 4-bit (size 0): CI4, IA4 (3I/1A), **I4 → intensidad replicada a R,G,B Y ALFA**
  (HW: los formatos I ponen alpha=intensidad, igual que I8 en size 1; antes lo ponía opaco A=255 → rompía
  el blend). loadTile ahora maneja block+tile de 4-bit (nibble granular).

### Blender (alpha-blend contra framebuffer) — genuino, gated en IM_RD
- **blendPixel(src)**: sólo engancha si **IM_RD (bit 0x40 de other_lo)** está set; si no, escribe directo
  (modos opacos sin lectura de FB = cero cambio en ROMs oráculo opacos, cero riesgo). Lee el pixel actual
  del framebuffer como RGBA32, aplica el mux del render-mode: **out = P·a + M·b** con P/M ∈ {CLR_IN, CLR_MEM,
  BLEND, FOG}, a ∈ {A_IN(alfa combinado), FOG_a, SHADE_a, 0}, b ∈ {1-a, MEM_a, 1.0, 0}. Lerp cuando b=1-a.
- **BUG CLAVE del ciclo blender**: **1-cycle usa los campos del CICLO-0** (GBL_c1: m1a<<30, m1b<<26, m2a<<22,
  m2b<<18), NO del ciclo-1. 2-cycle usa cyc1 (<<28/24/20/16). Leer cyc1 en 1-cycle daba M=CLR_IN → out=IN
  → sin blend (arrows negras sobre negro). Con cyc0: olo=0x00404040 → M=CLR_MEM, B=1-A → alpha blend real.
- **LOD_FRACTION = 1.0 (255) en 1-cycle / sin mipmap** (default HW). El alpha combiner de estos ROMs es
  `TEX0a · LOD_FRAC`; con LOD=0 daba alpha combinado 0 → blend a=0 → **todo el framebuffer = MEM (amarillo)**,
  arrows desaparecían. Con LOD=255 el alfa combinado = intensidad del texel → blend correcto (arrows blancas
  sobre amarillo con bordes suaves). aC idx0(LOD) e idx6(PRIM_LOD) → 255.
- **Ruteo**: texRect + los 3 caminos de triángulo → `if(other_lo&0x40) blendPixel else putPixel`. FILL_RECTANGLE
  ahora cycle-aware: FILL(3)/COPY(2) = fill_color crudo; 1-/2-cycle = combiner+blender (primitiva sin shade).
- **Validación** (krom, 16BPP rect): I4 **85→94.91%** (RMSE 65→15.6), IA4 **85→94.98%**, I8 92→**96.30%**,
  TLUT-IA4 89.6%. Sin regresión: RGBA16 rect 96.34%, RGBA16 tri 96.04%, ShadeTri RMSE 2.11, FILL-cycle 98.64%.
  **systemtest 0/3721** (6.86s) y **lockstep==threaded md5-idéntico** (I4 209095ee) intactos.
- **Gap conocido**: Cycle1FillRectangle (fill rect en 1-cycle) sigue 52% — los rects amarillo/cyan no salen;
  fuente del color no es fill/prim/env/combiner obvia; caso niche pre-existente, no regresión. Debug env-gated
  KESTREL_FILLDBG dejado. Residual ~3-5% en TODOS = AA de silueta (coverage) aún no modelado.

### Blender corre SIEMPRE en pipe-mode (fix RAÍZ — cascada en toda la suite)
Descubierto vía el .asm fuente de krom (Rotate FillTriangle): `Set_Other_Modes ...|B_M1A_0_2` +
`Set_Combine_Mode` con RGB=(1,·,TEX0,0) y Alpha=(TEX0a,·,LOD,0), luego `Set_Blend_Color rojo`. El
triángulo/rect NO lleva textura; el color sale por el **BLENDER** (P=BLEND_COLOR), no por el combiner.
Dos bugs míos:
1. **flatTexel = 0xFFFFFFFF** (antes 0): una primitiva SIN bloque de textura presenta el bus de texel en
   todo-unos (opaco blanco). Así el alpha combinado = TEX0a(255)·LOD_FRAC(255)=1 → el peso del blender es 1
   → blend_color sale sólido. Aplica a flat-triangle y a FILL_RECTANGLE en 1-/2-cycle.
2. **blendPixel ya NO se gatea en IM_RD.** El blender corre en TODA primitiva 1-/2-cycle; IM_RD (0x40) sólo
   habilita **lecturas del framebuffer**, o sea sólo importa cuando el mux elige CLR_MEM (M) o MEM_alpha (B).
   Si el mux referencia memoria y las lecturas están off → escribe directo; si no → evalúa el blender con
   memc=0. COPY/FILL lo puentean. Antes, con IM_RD off (olo=0x80000000) saltaba el blender → escribía el
   combiner (negro) en vez de blend_color (rojo).
- **fillRect** pipe-mode ahora: `combineColor(flatTexel,flatTexel,0)` → `blendPixel` (nunca fill_color crudo).
- **Impacto (krom 16BPP, 0 regresiones, mean exact 88.6→91.9):** Cycle1FillRect **52→98.74**, FillZBufferRect
  **58→96.15**, FillZBufferTri **77.6→98.10**, Cube **54.5→71.5**, Cycle1FillTri **86.5→98.97**, Plot L/R
  98.3→99.75, Rotate L/R 48→56/58 (tope por ser test ANIMADO: mi frame-dump ≠ frame ref, no bug rasterizador).
- **Invariantes intactos:** systemtest 0/3721/0/2/0/6 (6.86s), lockstep==threaded md5 (aa52d61a en FillRect).
- Sample-at-center (y+0.5,x+0.5) PROBADO y DESCARTADO: empeoró triángulos; offset de borde real = mediana 0px
  (el "1px" era outlier de fila apex).

### YUV texel format + SET_CONVERT (fmt 1) — IMPLEMENTADO
Formato YUV 4:2:2 (fmt 1) no existía → 89.50% (ambos rect+tri). Añadido:
- SET_CONVERT (cmd 0x2C): parse K0..K5 (9-bit signed) `k0=(cmd>>45)&0x1ff` etc.
- Decode YUV en sampleTexel size==2 fmt==1. Layout TMEM confirmado por brute-force vs ref (UYVY,
  err 11778 vs millones para YUYV/YVYU/VYUY): par de 32-bit = [U, Y0, V, Y1]. Luma en byte impar de
  cada texel, croma en bytes pares compartido por el par (4:2:2).
- Convert: `R=Y+((k0*dV+0x40)>>7)`, `G=Y+((k1*dU+k2*dV+0x40)>>7)`, `B=Y+((k3*dU+0x40)>>7)`, dU/dV
  signed sobre 128, /128 escala, redondeo HW +0x40. Alpha=255.
- K4/K5 (range-scale studio→full) NO aplicado: es etapa TF de 2-cycle; test es 1-cycle. Residual ~5%
  = redondeo FB 5-bit + bordes bilinear (SAMPLE_TYPE), no falta de K4/K5.
- Resultado: 89.50 → 94.27 (rect) / 94.61 (tri), close 98%, RMSE 46→20/15.
- Invariantes OK: systemtest 0/3721·0/2·0/6 (6.86s); lockstep==threaded md5 9dd1a4a8 en YUV rom.

### Line/Rotate/FillLine 0% = FALSA ALARMA (artefacto de referencia)
Mismo fill 0xFF01 (G5=28→231) en FillLine+Plot da ref bg (255,231,0) = coincide emu (expansión 5-bit
correcta, verificada 2/3 refs). Pero Rotate ref bg = (255,255,0), outlier de captura. 0% exact = artefacto
ref + frame de animación; close 99.41% confirma match estructural. NO es bug del emu.

### Siguiente
TLUT IA4B 89.6% (close 90%, error de decode genuino), TLUT RGBA4B 92.5%, ShadeTriangle 90.5 exact/98.45
close (subpixel, capado). Cube/Rotate = animados (frame-capped, saltar). Luego fog, AA silueta.
Después: optimizar intérprete → dynarec → (solo entonces) PD.

### Cierre iteración RDP softrender (limpieza + reencuadre IA4)
- **Cruft debug purgado**: quitados 3 `std::getenv` env-gated de rdp.cpp (KESTREL_FILTDBG en
  sampleTexFiltered, KESTREL_FILLDBG en FILL_RECTANGLE, KESTREL_RECTDBG en texRect). Rebuild limpio.
- **Batch fiable**: el batch paralelo viejo daba números BASURA (p.ej. Cycle1FillRect 0.32% cuando a mano
  da 98.7%) — `taskkill //PID` falla en MSYS → 42 procs huérfanos peleando por el MISMO out.bmp → carrera.
  Nuevo `scratchpad/batch2.sh`: SERIAL, bmp único por rom (`b_N.bmp`), `taskkill //F //IM` antes/después de
  cada rom. Correr con `bash batch2.sh < list.txt`.
- **Estado real (batch2 limpio, 47 tests 16BPP): mean exact = 92.09%, CERO regresión** vs pre-YUV. Peores no
  animados: TLUT-IA4 tri 89.56/rect 89.64, ShadeTri 90.54 (close 98.45=subpixel), TLUT-RGBA4 92.4/92.5.
  Animados (Rotate 0/56/58, Cube 71) = frame-capped, saltar.
- **REENCUADRE IA4** (antes se creía "bug decode genuino"): NO lo es. RGBA4B(92.5) e IA4B(89.6) usan el MISMO
  load/swizzle (RGBA16 Load_Tile → reinterpret CI4) y el MISMO tlutLookup (verificado correcto). El gap de ~3%
  es que el contenido IA es alto-contraste (I∈{0,128,255} + idx0 transparente) → los errores de borde
  subpixel/filtro 3-point saltan como diffs grandes (RMSE 27) mientras en RGBA el contenido suave los esconde.
  Volcado de pixels lo confirma: ref (123,115,0)=50%texel-negro+50%bg-amarillo (borde alpha filtrado), emu
  pone texel sólido → mismatch de PRECISIÓN DE BORDE, no de decodificación.
- **Corolario**: el techo estático de TODA la suite (~92 mean, residual 3-8%) = **precisión de coverage-AA /
  subpixel de borde**, un solo mecanismo, no bugs por-formato. Es el rework de mayor palanca (subiría docenas
  de tests a la vez) y el más arriesgado (toca el rasterizador que hoy da RGBA16 99%). Aplazado: no meter en
  caliente. Invariantes de esta iteración: systemtest 0/3721·0/2·0/6 (6.86s), lockstep==threaded md5 aa52d61a.

### Texture wrap/mirror/clamp — fix negativos + precedencia clamp (HW-accurate)
Gauge de categorías krom NO usadas antes reveló bugs de features (no subpixel). El primero atacado:
**wrap de coordenadas de textura**. `TexturesMaskShiftMirror` arranca S/T en **-14** (negativo) y stepea +1;
mi `sampleTexel` hacía `if(s<0)s=0; if(t<0)t=0` ANTES de enmascarar → todo coord negativo colapsaba a texel 0
en vez de wrap/mirror. Además ignoraba el bit CLAMP (cm&2) cuando había mask.
- **Fix (2 etapas como el sampler RDP real):** (1) clamp-stage: si `(cm&2) || mask==0` → pin a [0,lim]
  (único sitio que fija negativos); (2) mask-stage: si mask≠0 → mirror por `~c` en odd period (two's
  complement, maneja negativos), luego `c &= (1<<mask)-1`. Quitado el pre-clamp de negativos.
- Equivalente al viejo para c≥0 (mirror `~c`==triangular verificado), solo añade negativos correctos.
- **Impacto:** mean 47-suite 92.09→**92.16**, TODOS los deltas ≥0 (TextureTriangle* +0.3 c/u por bordes
  negativos), **CERO regresión**. MaskShiftMirror T0_1 68→71.7. Invariantes: systemtest 0/3721·0/2·0/6,
  lockstep==threaded md5 aa52d61a.

### Gauge de categorías nuevas (bugs reales pendientes, feature-level, bajo riesgo)
Corrido `batch2.sh` sobre 12 roms fuera de RDP/16BPP. Resultados (pixel-exact vs ref):
- **OK**: RDP/32BPP FillRect 98.7, FillRect(fill) 98.6, TexRect-RGBA16 93.5; AlphaCompare 95.2; LoadTLUT 95.8.
- **BUGS pendientes**: 8BPP InternalPalette **0%**, AlphaCoverage **21%** (=coverage-AA universal, rework
  grande diferido), CombinerOverflow **42%** (clamp/overflow del combiner), TextureCoordinates **65%**
  (direccionamiento S/T), MaskShiftMirror **65-72%** (parcial tras fix).
- **RSPTest/CPUTest: la métrica pixel% ENGAÑA** — esos roms pintan TABLAS DE TEXTO PASS/FAIL; la lógica
  RSP/CPU puede pasar y aun así el % baja porque los glyphs difieren 1px. LTV 69.7% ≠ fallo RSP (memory ya
  confirmó LTV/STV PASS leyendo el texto). Para RSP/CPU validar por systemtest (0/3721), no por pixel de krom.
- Orden sugerido próximas iteraciones (tratable→arriesgado): CombinerOverflow, 8BPP-InternalPalette,
  TextureCoordinates, MaskShiftMirror(resto), y por último AlphaCoverage (=el rework coverage-AA universal).

### Combiner reescrito HW-faithful (parallel-rdp exacto) + veredicto CombinerOverflow
Reescrito `combineColor` en `rdp.cpp` para replicar EXACTO el combinador de parallel-rdp (mi oráculo
autoritativo, el que mantiene systemtest verde). Modelo en punto-fijo 9-bit signed donde **0x100==1.0**:
- `special_expand(v) = bitfieldExtract(v-0x80,0,9)+0x80` para A/B/D; multiplicador C = sign-extend 9-bit.
- Ecuación por canal: `out = (((sexp(A)-sexp(B))*sex9(C) + 0x80) >> 8) + sexp(D)`  (D SUMA post-shift).
- **SIN clamp por-ciclo**: cycle0 devuelve RAW (i16, puede ser <0 o >255), alimenta cycle1 crudo; solo el
  ciclo FINAL pliega a [0,255] con `clamp_9bit_notrunc` (banda overflow: sumas que pasan +256 vuelven
  negativas→0; banda [-129,-256]→255). ONE=0x100. 1-ciclo evalúa comb[1]; 2-ciclo comb[0]→COMBINED→comb[1].
- Verificado contra `combiner.h` (combiner_equation L34, combiner_cycle0/1) y caller `shading.h` L290-320:
  el caller NO clampa el combined intermedio (`combined_inputs.combined = combiner_cycle0(...)` crudo).
- **CERO regresión** 47-suite (ShadeTri 90.5, Cube 71.5, TexTri-RGBA16 96.5, YUV 94.8), systemtest
  **0/3721·0/2·0/6** (6.86s), lockstep==threaded md5 **aa52d61a**. Win bancado (más HW-fiel que antes).

**Veredicto CombinerOverflow (queda 42%, NO se persigue):** torture-test de overflow deliberado donde los
DOS oráculos DISCREPAN. Caso 2-ciclo "2*combined" con env=$FF000000 (envR=255): cycle0 R=255+texR≈342;
parallel-rdp da cycle1 R = sex9(342)=-170, +342 = **172**; el PNG de krom (silicio real) muestra **255 plano**.
G/B casan perfecto; solo R diverge por sensibilidad numérica en la banda de flip de signo (env=255 empuja el
combined>255 al sign-extend negativo). Perseguir el 255 de krom = DESVIARSE de parallel-rdp = romper el
invariante que mantiene systemtest 0/3721. Es techo quirk-de-silicio parallel-rdp-vs-real, NO bug del emu.

**Diagnóstico 8BPP-InternalPalette (0%, diferido — no guessing):** demo pone Set_Color_Image FORMAT=COLOR_INDX
SIZE=**8B** width 320, pero VI muestra 16BPP width **160** (mismo base) → framebuffer 8bpp reinterpretado como
16bpp, escalado 2x horizontal por VI hasta el ref 320-wide. Copy-mode + EN_TLUT: CI8 indexa TLUT en TMEM $100.
Mi `putPixel` para ci_size==1 (8bpp) cae al path 16bpp (stride*2) → direcciones mal. Requiere 4 piezas
acopladas (write 8bpp byte-addressed + semántica exacta copy+TLUT en color-image-8bpp + reinterpret 160→320 +
escala VI 2x) con comportamiento HW incierto → alto esfuerzo/baja certeza, riesgo de regresión en el path VI.
NO se implementa por adivinación (violaría "HW genuino, nunca hardcode"); pendiente de leer spec RDP autoritativa.

---

## Alpha-compare RDP — dos formas HW (TextureCoordinates 65→86.5, copy-regresión curada)

**Bug raíz:** gates de dibujo (texRect + triángulo) descartaban por alpha CRUDA del texel
(`tex & 0xff`), matando TODO texel alpha-0 en CUALQUIER modo. Rompía RGBA5551 con bit-transparencia:
$F800 (rojo, alpha bit=0) desaparecía. TextureCoordinates.asm (1-ciclo, Color=Tex0) → 65%.

**Semántica HW correcta (dos ramas bajo ALPHA_COMPARE_EN, other_lo bit0):**
- **COPY mode** (cycleType==2): transparencia 1-bit clásica de sprite — descarta si `alpha == 0`.
  Verificado en CopyTextureRectangleTLUTRGBA4B.asm: `CYCLE_TYPE_COPY|EN_TLUT|ALPHA_COMPARE_EN`,
  sin Set_Blend_Color (threshold=0), TLUT index0=$0000 (alpha 0, transparente) vs 1/2/3 alpha=1.
- **1-/2-ciclo**: compara alpha COMBINADA contra threshold blend_color.a; falla si `< threshold`
  (dither aproximado por el mismo compare).
- Deshabilitado → texel escrito siempre, sin importar el bit 5551.

**Fallo del primer intento (unificado a `<threshold`):** en copy con threshold=0, `alpha<0`
nunca dispara → index0 transparente se pintaba → regresión −2.8% en Copy*TLUTRGBA4B (rect+tri),
−0.5% en varios Copy*. Split copy/non-copy lo cura: copy vuelve a baseline, se conservan las
ganancias Cycle1 (I4B/I8B/IA4B +0.5–0.7 por dejar de tirar texels alpha-0 legítimos).

**Código:** rdp.cpp gate texRect (~L568) y gate triángulo textura (~L312). `bool copy = cycleType()==2`;
combiner sólo si `combProg && !copy`; rama alpha `copy ? (a==0) : (a < blend.a)`.

**Invariantes (todos verdes tras el fix):** systemtest **0/3721·0/2·0/6** (6.86s);
lockstep==threaded md5 **aa52d61a** (Cycle1FillRectangle16BPP, KESTREL_THREADS=1);
47-suite mean **92.09→92.22** (sin regresión, Cycle1 +; copy sin cambio) + TextureCoordinates
**65→86.5** (fuera de la lista-47). Residual TextureCoordinates = subpixel/centrado-T, no alpha.

Ref extra del usuario: n64.dev (portal specs N64). Oráculo decisivo sigue siendo el .asm de krom +
parallel-rdp shaders.

## Reframe gauge: 4 tests "bajos" son ANIMACIÓN (frame-mismatch), NO bugs RDP

Rotate-Line (0%), Rotate-Tri LeftMajor (56%), RightMajor (58%), Cube (71%) — los 4 más bajos —
son demos con **loop infinito que rota +1° por frame** (`Loop:` … `Rotation += 1` … `j Loop`,
verificado en los .asm). El PNG de krom captura UN frame concreto; kestrel captura a MAXINSN, que
cae en OTRO ángulo → el % es sólo la fracción de píxeles que coinciden entre dos rotaciones. Las
variantes **Plot** (mismos primitivos, ESTÁTICOS) dan **99.75%** → el rasterizer es correcto.
NO perseguir por pixel% (mismo caso que RSPTest/CPUTest glyph-table): requeriría capturar el frame
exacto de krom, match frágil de timing sin relación con correctitud RDP.

**Accuracy real (sólo-estáticos, n=43): mean 96.47** (vs 92.22 con animados metiendo ruido).
Peores estáticos GENUINOS: IA4-TLUT 89.6 (contraste alto amplifica error borde subpixel, NO decode),
ShadeTri 90.5, RGBA4B-TLUT 92.5-92.8, YUV 94.3. Todos = mismo techo subpixel-edge universal → única
frontera RDP-correctitud real restante = rework coverage/AA subpixel (grande, arriesgado, ÚLTIMO).

## InternalPalette (8BPP color-image) — DIFERIDO, gap real VI+RDP (no perseguir por pixel%)

`RDP/8BPP/.../TLUT/InternalPalette` da exact 0%. Investigado 2026-08-11:
- Demo: `Set_Color_Image FORMAT COLOR_INDX, SIZE 8B, WIDTH 320, $00100000` → **framebuffer de 8bpp
  índices** (no 16bpp). `ScreenNTSC(160,240,BPP16,AA_MODE_2)` → VI lee 160 px 16bit (=320 bytes/fila).
- Mi FBDUMP = **160×240 uniforme = sólo el fill background**; los texrect copy-mode CI8 **no escriben
  nada** al color-image de 8bpp (mi RDP asume stride/putPixel de 16bpp). Ref de krom = 320×240 con
  contenido de textura → capturado de HW con VI x_scale 160→320 + resample (AA_MODE_2).
- Dos gaps reales: (1) **RDP write a color-image de 8bpp** (índices de 1 byte, addressing distinto);
  (2) **VI x_scale/resample** 160→320. Bajar ref 320→160 (BOX) y comparar a mi bmp = 0% igual → el
  fallo dominante es (1), el contenido de textura falta, no sólo la escala.
- **Semántica HW genuina** exigiría modelar color-image 8bpp + VI CI display. Deep, 1 solo test en la
  suite-47. DIFERIDO. No hardcodear. Breadcrumb para cuando toque barrer formatos de framebuffer.

## Coverage subsample 8× (4y×2x) — IMPLEMENTADO 2026-08-11, win limpio

Reemplazado el coverage de fracción-horizontal-pura por **multisample estilo RDP_raster.vhd**:
por pixel se evalúan las dos aristas (major xA / minor xB, lineales en y) en **4 sub-scanlines**
(y+{0.125,0.375,0.625,0.875}) × **2 sub-columnas** (x+{0.25,0.75}) = 8 subsamples; cvg = hits/8,
cuantizado a pasos de 1/8 como el HW. Da AA graduado también en aristas **casi-horizontales** (la
caja horizontal pura las perdía) y coincide con los 3-bit de coverage reales. Código: rdp.cpp lambda
`edgesAt` + doble bucle subY/subX dentro del scanline (sustituye el `cvg = min(x+1,R)-max(x,L)`).

**Riesgo bajo por diseño:** coverPixel sólo pliega si `AA_EN` (other_lo&0x08); tests AA-off toman
blendPixel/putPixel igual → invisibles al cambio. Sólo afecta el peso del fold en aristas AA-on.

**Resultado:** delta aislado **+0.165** full-47 vs baseline post-alpha (92.05→92.22). Sube copy-mode
textura: CopyTexTriangle/Rect TLUTRGBA4B **+2.8**, RGBA16B/TLUTRGBA8B **+0.5**; cero regresión en 47.
Static-only mean **96.47**. Invariantes: systemtest **0/3721·0/2·0/6** (6.86s); lockstep==threaded
md5 **70050ab2** (Cycle1TextureTriangleIA4B, KESTREL_THREADS 0 vs 1, idéntico).

## 32BPP refs = artefacto captura NTSC compuesto, NO bug mío (verificado 2026-08-11)

Usuario preguntó "¿el fallo no será tu screenshot del RDP software?". Verificado: **mi dump lee el
framebuffer FIEL**. 32BPP/SetPrimColor: fondo = `Fill_Rectangle $FFFF00FF` → mi dump da (255,255,0)
= valor literal exacto. El dump NO es el bug; mi RDP renderiza el pixel correcto.

Los refs 32bpp de krom difieren por **cross-channel**: White(255,255,255)→ref limpio;
Red(255,0,0)→ref(255,8,0); Green(0,255,0)→ref(8,255,0); Yellow-fill(255,255,0)→ref(255,231,0).
El verde de salida depende del AZUL de entrada → imposible por gamma/rango per-canal → es **sangrado
de croma NTSC compuesto** (krom capturó 32bpp por compuesto/S-video; saturados sangran ±8, blanco
sin croma queda limpio). Artefacto de captura aguas arriba del emulador. 16bpp puntúa 98% = capturado
digital limpio; 32bpp ~0% = compuesto. **NO perseguir** (falsear croma rompería los tests digitales).
Método dump/readFb 32bpp (cpu.cpp dumpFramebufferBmp type==3: R=ram[p],G=[p+1],B=[p+2]) = correcto.

**Barrido 127 roms RDP** (breadth). Bugs REALES míos a atacar (no captura, no animación):
TextureCoordinates1 45.7 / 1b 39.3, RDPModeInput 6.5, RDPTex0And1 59.0, TexturesMaskShiftMirror ~66,
CombinerLongTailConstants 30.4. = features de addressing/mask/shift/mirror y combine incompletas.

## FIX altura FBDUMP (hardcode h=240) — win AMPLIO 2026-08-11 (hipótesis usuario)

Usuario insistió "¿el fallo no será tu screenshot del RDP software?". ACERTÓ y generalizó: mi
`dumpFramebufferBmp` (cpu.cpp) **hardcodeaba h=240**. Los demos low-res de krom (`ScreenNTSC(w,120,...)`)
renderizan 160×120 pero yo dumpeaba 160×240 → mitad inferior negra → comparación desalineada aunque
el render fuera pixel-idéntico (gray count 18528==18528 verificado).

**Fix HW-genuino:** altura = `(baseH * (vi_yscale&0xfff)) >> 10`, baseH = 240 NTSC / 288 PAL (por
vi_vsync total ≥550). Medido: 160×120 test yscale=0x200→120; 320×240 yscale=0x400→240 (exactos).
vi_width ya daba el ancho nativo bien. (La variante `disp*yscale` vía vstart/vend daba 118, imprecisa;
base fija por campo es exacta contra los refs.)

**Resultado (barrido 127 roms):** TextureCoordinates1 45.7→**100.0**, 1b 39.3→95.3,
CombinerLongTailConstants 30.4→67.8, RDPTex0And1 59.0→91.9, AlphaCoverage 21.3→41.1. Media-127
85.34→**86.92** (+1.58), **cero regresiones**. 47-suite 92.22 intacta (todos 320×240). Invariantes:
systemtest **0/3721·0/2·0/6**; lockstep==threaded md5 **323773f9** (TextureCoordinates1 low-res).

**Patrón confirmado:** varios "fallos" bajos eran artefactos de mi SCREENSHOT (altura, ancho
InternalPalette, croma-compuesto 32bpp), NO del rasterizer. La intuición del usuario fue clave.
Pendientes REALES restantes: RDPModeInput 6.5 (revisar si otro dump-artifact), CombinerLongTail 67.8,
AlphaCoverage 41 (coverage), MaskShiftMirror ~70 (wrap/mirror).

## 2026-08-11 — 8bpp CI "internal palette" render target + VI X_SCALE resample (dump)

Objetivo suite: RDP/8BPP/.../TLUT/InternalPalette (único test 8bpp-CI). Framebuffer =
COLOR_INDX 8bpp; el RDP escribe bytes-índice crudos; el VI (config 16bpp) reinterpreta
pares de bytes como RGBA5551 al mostrar. Copy-mode texrect blitea índices.

### Fixes rasterizer (genuinos, generalizables — NO hardcode)
1. **Render target 8bpp CI** (rdp.cpp putPixel/readFb/fillRect + `wr8`): `ci_size==1`
   escribe/lee low-byte índice con stride 1. Antes todo caía en la rama 16bpp (stride 2)
   → framebuffer corrupto.
2. **Copy-mode DsDx /4** (texRect): CYCLE_TYPE=COPY procesa 4 px/reloj RDP, DsDx viene a
   escala 4× → `if(cycleType()==2) dsdx/=4.0`. Confirmado en 4 .asm copy-mode (todos 4<<10).
   **Mejoró 3 tests copy 16bpp**: CopyTextureRectangleRGBA16 +0.51, TLUTRGBA4 +2.83,
   TLUTRGBA8 +0.51. Media 47-suite 92.22→92.30.
3. **Copy-mode CI8 alpha-key por índice** (texRect): `sampleRawIndex()` devuelve índice
   pre-TLUT; alpha-compare keya `idx==0` (no el bit-alpha del TLUT, que aquí es
   TLUT[idx]=idx<<8 → LSB siempre 0 → dropearía todo).

### Fix presentación VI (dump — genuino, cero regresión)
4. **VI X_SCALE horizontal resample** (cpu.cpp dumpFramebufferBmp): salida = (H_VIDEO
   activo)/2 px; cada px muestrea source vía `sx=(x*xscale)>>9`. Estándar 320 (xscale $200,
   H_VIDEO $6C02EC→640) → 320 con sx==x (idéntico, cero regresión verificada). Half-width
   160 (xscale $100) → 320 con columnas source duplicadas 2×, como el VI real. Corrige la
   discrepancia de dims 160-vs-320.

### Estado / gap restante
InternalPalette ahora 320×240 estructural correcto (bg amarillo + tiles-índice) pero
exact 0% / close ~67%: el ref difumina los tiles hacia amarillo (top ref todo 255,25x,0) =
**VI AA_MODE_2 resample filter**, NO modelado. Mismo caso diferido que 32bpp-composite:
habilitar filtro global arriesga difuminar tests nítidos que ya pasan → NO tocar ahora.
El rasterizer es correcto; el gap es filtro VI de presentación.

### Invariantes (TODOS verdes, build nuevo)
- systemtest `KESTREL_JIT=1`: Base **0/3721**, Timing **0/2**, Cycle **0/6**.
- lockstep==threaded md5 idéntico (copy RGBA16 `027e7280…`, 8bpp CI `6b93b385…`).
- 47-suite 92.22→92.30, cero regresión, 3 copy tests suben.

Patrón confirmado (3ª vez): cuando ref discrepa y el render interno es plausible,
sospechar el pipeline MI/VI de presentación (dump) ANTES que el rasterizer.

## 2026-08-11 (bis) — MaskShiftMirror diagnosticado = VI-filter, NO rasterizer

Suite RDP/TexturesMaskShiftMirror (12 roms, ~71%) investigada. Textura 12×8 RGBA16,
7 columnas (mask_t 0..6) × 4 filas (clamp_t×mirror_t), shift_t 0/1/15, texrect arranca
S,T=-14.0, DsDx/Dt=1.0, TileSize TL/TH sub-rango (incl. TH<TL en _3).

### Hallazgos
- **Wrap clamp/mask NO son mutuamente exclusivos**: HW (angrylion tcclamp→tcmask) aplica
  clamp (si cs||!mask) LUEGO mask (si mask!=0), en secuencia. Con 2^mask>tMax el mask es
  idempotente (clamp visible); con 2^mask<=tMax re-wrappea. Mi código original YA lo hacía.
  Probé mutual-exclusive → score neutro y NO HW-accurate → **revertido a secuencial**.
- **Addressing PROBADO correcto**: muestreo de los 28 centros de celda coincide exacto con
  ref (wht/wht, red/red…). Point-sampling correcto (SAMPLE_TYPE off = 1x1).
- **El 20% de mismatch es filtro VI**: `ScreenNTSC(320,240, BPP16|AA_MODE_2)` = "Resample
  Only". Los pares que difieren son de BORDE de texel: blu→mag, blu→cyn (cyan NO existe en
  la textura → sólo puede venir de mezclar blue+green vecinos), wht→red, wht→blu. Colores
  INTERMEDIOS entre texels adyacentes = el filtro resample/blur de la captura de krom
  suaviza bordes; mi dump da píxeles nítidos correctos.

### Conclusión (clase entera de parciales)
InternalPalette, MaskShiftMirror (~71%) y el croma-32bpp comparten RAÍZ: filtro
VI AA_MODE_2 resample / blur de captura NO modelado. El rasterizer es correcto en los tres
(centros/estructura exactos). Modelar el filtro VI arriesga regresar los tests planos que ya
pasan a 96-100% (sus pocos bordes) → NO tocar sin gate preciso + A/B. Los parciales
restantes genuinamente rasterizer son otros (CombinerLongTail 67.8 = overflow combinador,
AlphaCoverage 41 = coverage). Rasterizer NET este sesión: 8bpp-CI + copy DsDx/4 +
index-alpha + VI xscale; wrap sin cambios (revertido).

## 2026-08-11 (quater) — CombinerOverflow caracterizado (siguiente target rasterizer)

RDP/CombinerOverflow 42% (exact) / RMSE 94. Mitad DERECHA (gradientes stripe) coincide bien;
mitad IZQUIERDA (6 bloques overflow) diverge: mine vívido/saturado (avg ~142, leve magenta),
ref gris neutro uniforme (~105, R=G=B) en LOS 6 bloques pese a combiner distinto.

Setup: 1cyc/2cyc FORCE_BLEND|IMAGE_READ_EN|B_M2A_0_1, texturas 16×16 32bpp (AlphaGradient
alpha 0..255, RedGradient red 0..255), Env/Prim variando. Cada bloque empuja el combiner a
rangos que desbordan 9-bit (Env+Tex>1.0, negativos 0..-2, rangos 2..4) para ver DÓNDE clampea
(por-ciclo vs total). Mi clampN SÍ wrappea extremos (510→0), pero el mid-range sale vívido.

El gris-uniforme del ref = wrap-overflow + **RGB dither** (cuantización 5551 con magic-square) +
VI AA_MODE_2, promediando alta-freq a gris. Mine: sin dither + posible off en el wrap
mid-range + blend. Fix = combiner 9-bit wrap bit-exacto + blender FORCE_BLEND/B_M2A + RGB
dither, los tres. Investigación propia (no quick-fix). Es genuinamente rasterizer (a diferencia
de MaskShiftMirror/InternalPalette = VI-filter). Prioridad tras esto: AlphaCoverage (coverage-AA).

### Estado neto sesión (todo validado, invariantes verdes)
rdp.cpp: 8bpp-CI render target + copy DsDx/4 (+3 tests) + copy CI8 index-alpha; wrap SIN
cambios (mutual-exclusive probado y REVERTIDO). cpu.cpp: VI X_SCALE resample en FBDUMP.
systemtest 0/3721·0/2·0/6, lockstep==threaded md5 idéntico, 47-suite 92.22→92.30.

## 2026-08-11 (quinquies) — Expansión 5→8 = bit-replicación (fuente angrylion, NO ensayo-error)

RAÍZ (nota usuario "formato RGB N64 igual es distinto"): 3 sitios en rdp.cpp expandían
5-bit→8-bit con `(v*255)/31` (escala lineal). MAL. HW N64 = **bit-replicación** `(v<<3)|(v>>2)`.
FUENTE AUTORITATIVA: angrylion-rdp-plus core/n64video/rdp/tmem.c:999
`replicated_rgba[i] = (i << 3) | (i >> 2)`, usado por RGBA16_EXTEND_{R,G,B}. Difieren en
mid-range (v=16: replicación 132 vs lineal 131; v=20: 165 vs 164).

Sitios corregidos → helper `exp5(v)=(v<<3)|(v>>2)`:
- readFb() (blender IMAGE_READ_EN framebuffer readback)
- tlutLookup RGBA5551 palette (CI+TLUT)
- sampleTexel RGBA5551 default
Ya consistente con dump (cpu.cpp exp5) y present.cpp expand5.

INVARIANTES: systemtest 0/3721·0/2·0/6 · lockstep==threaded 027e7280 · 47-suite 92.30
(static n=43 96.47→96.56, +0.09, leve subida). Genuino HW, cero regresión.

LECCIÓN (regla usuario): en punto de duda → FUENTE (angrylion/MiSTer/parallel-rdp local en
ares-64/thirdparty) ANTES que ensayo-error. angrylion checkout: /e/Claude/N64/ares-64/thirdparty/angrylion-rdp-plus/src

## 2026-08-11 (sexies) — CombinerOverflow: DIAGNÓSTICO FIRME (fuente angrylion, no VI-filter)

Fui a fuente (ares-64/thirdparty/angrylion-rdp-plus/src) en vez de ensayo-error (regla usuario).

HALLAZGOS DEFINITIVOS:
- **5→8 expansión** = bit-replicación `(v<<3)|(v>>2)` (tmem.c:999 replicated_rgba). YA arreglado
  (readFb/palette/texel). NO movió CombinerOverflow (no era readback).
- **Combiner clamp** (combiner.c:381 special_9bit_clamptable): 9-bit slice del (17bit)>>8:
  0..255 pass · 256..383 → 255 (sat) · 384..511 → 0 (WRAP negro). Mi `clampN` vía `sexp`
  YA replica esto exacto (tracé: clampN(455)=0, clampN(300)=255). Combiner CORRECTO.
- **NO es VI-filter** (a diferencia InternalPalette/MaskShiftMirror): datos blkavg.py muestran
  inversiones por-canal + promedios difieren 100+. Bloque (240,10) Env=255+tex_red overflow:
  ref R=21 (wrapeado a 0), mine R=216 (mal). Genuino rasterizer.
- **RAÍZ = BLENDER FORCE_BLEND** (blender.c:195-203). Todos los bloques usan FORCE_BLEND|
  IMAGE_READ_EN|B_M2A_0_1, pixel final = blend(combiner_out, framebuffer). HW:
    blend1a=a>>3; blend2a=b>>3; mulb=blend2a+1;
    r = ((P*blend1a + M*mulb) >> 5) & 0xff    // WRAP mod-256, coef 5-bit, SIN normalizar
  Mi blendColor usa `(P*a+M*b+127)/255` + clamp (aprox normalizada). Diverge en overflow.
  Non-force usa tabla división bldiv_hwaccurate_table (otra tabla). blshift especial cuando
  M-alpha == framebuffer alpha.
- Intento rápido force_blend (>>5&0xff): CombinerOverflow 42.24→40.70 (PEOR). El >>5 fijo
  daña pixels normales (÷32 vs ÷255) más que arregla overflow. REVERTIDO. Necesita decode
  EXACTO coeficientes B_M2A_0_1 + inv_pixel + mulb, no atajo. Reescritura blender HW-completa
  (force + non-force división-table), ALTO RIESGO a tests 96-100%, sesión propia + A/B suite.

ESTADO: solo fix 5→8 vivo. Invariantes verdes (binario = build 5→8 ya validado 92.30, static 96.56).
SIGUIENTE: o (a) blender HW-exacto con A/B cuidadoso, o (b) target sin blender-overflow
(CombinerLongTailConstants ~67%). AlphaCoverage último.

## AUDIO AUDIBLE — sink host waveOut (breadth, 2026-08-11)

REFRAME usuario: prioridad = breadth funcional (¿corre rom? ¿hay 3D? ¿hay audio?),
NO color-textura subpixel. Estado verificado: 3D SÍ (RSP LLE F3DEX→SoftRDP), runs SÍ
(systemtest 100%). GAP real = audio modelado (AI FIFO/IRQ/timing en memory.cpp) pero
SIN salida host → no sonaba. CERRADO.

NUEVO módulo `src/audio/{audio.hpp,audio.cpp}`:
- Backend Windows `waveOut` (winmm). Fuera de Windows = no-op (core portable).
- Ring buffer s16 stereo host-endian + hilo feeder con pool 4×512-frame WAVEHDR.
  waveOut relojea a la tasa abierta; feeder solo drena el ring, rellena con silencio
  en underrun (nunca stall). CALLBACK_EVENT, no callback-context (waveOut* prohibido ahí).
- `pushRdram(base,size,addr,len,rate)`: lee bytes big-endian de RDRAM, byteswap s16→host,
  cola. Bounds-checked, recorta a frames L/R enteros. Abre device lazy en 1ª llamada.
- Cap ring ~250ms: emu más rápido que realtime dropea samples viejos (latencia acotada).
- env KESTREL_AUDIO=0 desactiva; KESTREL_AUDIO_TRACE=1 traza 12 primeros push.

WIRING: memory.cpp AI_LEN handler (0x04) — al aceptar buffer en FIFO, hace
`audio::pushRdram(rdram.data(), size, ai_dram, len, rate)` con rate=48681812/(dacrate+1).
SOLO lee rdram, cero escritura estado emulado → determinismo intacto por construcción.
main.cpp: `kestrel::audio::shutdown()` tras runLoop. CMake: +src/audio/audio.cpp, +winmm
(kestrel64 y rsp_test, este último compila memory.cpp).

VERIFICADO ruta end-to-end con krom PCM/LongShot/STEREO/16BIT:
`[audio] push #1..12 addr=00108c len=32767 rate=44135 dev=1` — device host abrió (dev=1),
dacrate correcto (44135 Hz), bytes reales RDRAM, streaming continuo, sin crash.
(Headless: no "oigo" pero cadena AI_LEN→pushRdram→byteswap→ring→feeder→waveOut→DAC viva.)

INVARIANTES (audio no toca ruta RDP/CPU, solo lee rdram):
- systemtest **0/3721·0/2·0/6** (6.88s, con KESTREL_AUDIO=0).
- lockstep==threaded md5 **aa52d61a** (Cycle1FillRectangle16BPP, THREADS 0 vs 1, = valor documentado).
- 47-suite intacta por construcción (md5 byte-idéntico prueba ruta RDP sin cambio).

SIGUIENTE breadth: input (¿mapeo controller host?), luego optimización intérprete → dynarec.
Color-textura subpixel (CombinerOverflow/blender HW-exacto) = ÚLTIMO tramo, diferido.

## 2026-08-12 — Super Mario 64 ARRANCA: root-cause deadlock EEPROM + vi.capture MCP

Hito "conseguir que arranque Mario 64" = **CUMPLIDO**. SM64 (USA, cartId SM, entry
0x80246000) pasa el boot, corre >1.8B instrucciones estable (~8% realtime host), setea
framebuffers VI (double/triple buffer, origins 0x38fa80/0x3b5280/0x3daa80, 320×240 16bpp
5551) y **renderiza 3D** — el primer frame del intro sale coherente (ver `sm64_boot.png`).

### Root cause del hang de boot (device-modeling, no bug de CPU/RCP)

Cadena confirmada por trazado sobre MCP (peekPhysCoherent + disasm capstone sobre RDRAM):
`save_file_load_all` → `osEepromLongRead` (0x80324690) → `__osEepRead` (0x80329150).
`__osEepRead` toma el mutex SI vía `__osSiGetAccess`, lee el status del EEPROM por joybus,
y en 0x803291b8 hace `beq t9, 0x8000` — **exige que el status halfword sea 0x8000 = chip
EEPROM de 4-Kbit**. Nuestra emulación reportaba 0xC000 (16-Kbit). En mismatch, libultra
salta al error-exit 0x80329330 que hace `lw ra,0x14(sp); addiu sp,0x38; jr ra` — **retorna
SIN llamar `__osSiRelAccess` → el token del mutex SI se FILTRA**. El siguiente `__osEepRead`
(hilo game5) bloquea para siempre en `GetAccess` sobre el token nunca devuelto → deadlock.
Es un quirk genuino de libultra que en HW real nunca dispara porque el chip real reporta su
tipo verdadero; nuestro tamaño/tipo de EEPROM equivocado activaba la rama de error.

Traza decisiva: `EepStatusCheck t9=c000 (wants 0x8000)` seguido de `__osEepRead ENTER` (la
siguiente llamada que se cuelga con el token filtrado).

### Fix (semántica HW genuina, generalizable — NO hardcode a SM64)

El tipo de chip EEPROM es una **propiedad física del cartucho** que los emuladores resuelven
del código del juego (resolución estándar save-type por game-code / cart-ID header 0x3C-0x3D).
`Memory::resolveEepromType()` (llamado en `loadRom`): tabla de IDs 4-Kbit conocidos
(SM/MK/WR/FZ/SF/KT…) → `EepType::K4`; **default K16** para no romper Perfect Dark. El backing
se dimensiona al tipo (512 B vs 2048 B) y el status joybus reporta el byte correcto
(`eepromTypeByte()`: 0x80 para 4-Kbit, 0xC0 para 16-Kbit) en vez del 0xC0 hardcodeado previo.
SM64 ("SM") → 4-Kbit → status 0x8000 → `__osEepRead` toma la ruta normal que SÍ llama
`RelAccess` → sin fuga de mutex → boot completo.

### Nueva capacidad MCP (por directiva usuario: falta capacidad → meterla en el MCP, no hacks)

- **`mem.read coherent=1`** (RDRAM): lee a través del write-back D-cache del CPU
  (`CPU::peekPhysCoherent`). Las escrituras cacheadas del CPU a estructuras del kernel
  (thread structs, validCount de colas, scheduler) NO tocan el array backing hasta flush —
  la lectura plana las veía STALE. Coherent devuelve la vista real del CPU. Imprescindible
  para inspeccionar estado vivo de libultra de un juego corriendo.
- **`vi.capture`** (nuevo): decodifica el framebuffer VI vivo (origin/width/ctrl desde el
  register file RCP) a RGBA8888 — expande 5551 (16bpp) u 8888 (32bpp) como el DAC. El bridge
  Python `capture_framebuffer(path)` lo guarda como PNG (writer zlib propio, sin deps) +
  histograma de colores distintos (rendered vs solid-fill de un vistazo). Ruta 100% propia
  de kestrel, cero readback de GPU host — es la RDRAM que la consola mostraría.

### Estado del render y frontera siguiente

Primer frame del intro = coherente. Frames posteriores (pantalla de título / logo) muestran
**corrupción** (streaks diagonales + ruido magenta; `sm64_paused.png` — persiste en pausa →
NO es tearing de captura, es estado real). Bug de accuracy RDP/geometría-RSP separado del
hito boot; a investigar aparte. Responde a la pregunta del usuario: el core está ~completo en
teoría (systemtest 3721/2/6, lockstep==threaded, 3D vía RSP LLE, 47-suite ~92-96) pero **juegos
reales exponen huecos de device-modeling (EEPROM/PIF/SI) y de accuracy RDP que los tests
sintéticos no cubren** — exactamente este EEPROM-type y la corrupción de la pantalla de título.

### Invariantes (fix EEPROM = save-device; peekPhysCoherent/vi.capture = telemetría read-only; todo ortogonal a RDP/CPU)

- systemtest intérprete **0/3721·0/2·0/6** (6.86s).
- systemtest `KESTREL_JIT=1` **0/3721·0/2·0/6** (6.88s).
- lockstep==threaded md5 **idéntico**: FillRectangle f1bb6ea1, ShadeTriangle **e7c8471c**
  (= valor documentado) → ruta RDP/CPU sin cambio de comportamiento.
- 47-suite intacta por construcción (cambios no tocan código RDP; md5 idéntico lo prueba).

SIGUIENTE (plan usuario): SM64 arranca → **volver al CPU: optimizar intérprete → dynarec JIT**.
Corrupción pantalla-título SM64 = accuracy RDP, tramo posterior. NO tocar Perfect Dark.

---

## 2026-08-12 · Save types cartucho (EEPROM/SRAM/FlashRAM) completos

Petición usuario: "implementa los distintos savetype que existen". El backup del cartucho es
propiedad **física** de la placa, NO va en la cabecera del ROM → se resuelve por cart-ID
(bytes 0x3C-0x3D = ID 2-char, 0x3E = región) contra tablas curadas, con override
`KESTREL_SAVETYPE` (none/eep4k/eep16k/sram256/sram768/flash) para bring-up.

### Los 5 dispositivos (semántica HW genuina, no hardcode)

| Tipo | Tamaño | Bus | Detalle |
|------|--------|-----|---------|
| EEPROM 4-Kbit | 512 B | joybus canal 4 | status byte 0x80; SM64 lo exige (fix deadlock) |
| EEPROM 16-Kbit | 2048 B | joybus canal 4 | status byte 0xC0; PD |
| SRAM 256-Kbit | 32 KB | PI dom-2 @ 0x08000000 | direccionable directo, lineal |
| SRAM 768-Kbit | 96 KB | PI dom-2 @ 0x08000000 | 3 bancos plegados (off>>16 → banco*32KB) |
| FlashRAM 1-Mbit | 128 KB | PI dom-2 @ 0x08000000 | máquina de estados por comandos |

FlashRAM: lee en 0x08000000 = status/silicon-ID (modo Status, doubleword 0x11118001_00C2001E
Macronix) o array (modo Read); registro comando en 0x08010000. Comandos: 0xE1 status, 0xF0
read-array, 0x4B set-erase-page, 0x78 erase-mode, 0xA5 set-write-offset, 0xB4 write-mode,
0xD2 execute (erase sector 16 KiB → 0xFF, o commit del page-buffer 128 B). Página 128 B,
sector 16 KiB.

### Cableado
- `memory.hpp`: enum SaveType + FlashMode, backing `eeprom`/`saveRam`, helpers isEeprom/
  isSram/isFlash/eepromTypeByte/saveSize/saveOffset/saveRead/saveWrite/flashCommand.
- `memory.cpp`: read8/16/32 + write8/16/32 interceptan `isSaveDomain(0x0800_0000..0x1000_0000)`
  tras isCart; piDma() ruta save-domain byte-a-byte en ambas direcciones; resolveSaveType()
  en loadRom(); reset() dimensiona backing (flash rellena 0xFF).
- `server.cpp`: cmdStatus expone `saveType` + `saveBytes`.

### Validación
- **`test/save_test.cpp`** (target CMake `save_test`, mismo patrón que rsp_test): ejercita el
  bus real — SRAM 256k round-trip (word/byte/half + backing), SRAM 768k 3-bancos plegados,
  FlashRAM status/silicon-ID, page-write+commit+read-array, sector-erase→0xFF. **ALL PASS
  (0 failures)**. La máquina de estados FlashRAM es la parte no-trivial que ningún ROM real
  disponible cubría → test unitario la cierra.
- SM64 default → eeprom4k, sigue arrancando+renderizando.
- `KESTREL_SAVETYPE=flash` → flash1m, región SAVE 128 KB presente.

### Persistencia a disco (.eep/.sra/.fla)
El save de batería/flash sobrevive apagado en HW → se espeja a un fichero junto al ROM,
extensión estilo mupen/ares: `.eep` (EEPROM), `.sra` (SRAM), `.fla` (FlashRAM).
`Memory::attachSaveFile(romPath)` (llamado en `System::init` tras loadRom) deriva la ruta
(reemplaza extensión del ROM) y carga la imagen existente al backing activo; tolera fichero
corto/largo. `flushSaveFile()` (en `~System`) reescribe **solo si `saveDirty`** — flag que se
arma en las 3 rutas de escritura del guest (joybus EEPROM write 0x05, saveWrite SRAM, flash
execute-commit). Sin dirty NO se crea fichero → homebrew/tests que no guardan no ensucian el
directorio. Round-trip validado en save_test (write→flush→nueva Memory→attach→relee 0xCAFEF00D,
byte no tocado sigue 0x00).

### Invariantes (save = device nuevo, ortogonal a RDP/CPU/threading)
- systemtest intérprete **0/3721·0/2·0/6** (6.86s).
- systemtest `KESTREL_JIT=1` **0/3721·0/2·0/6** (6.88s).
- lockstep==threaded md5 **idéntico y = baseline**: FillRectangle **f1bb6ea1**, ShadeTriangle
  **e7c8471c** → ruta RDP sin cambio (save no toca RDP/RSP).
- save_test **ALL PASS**.

SIGUIENTE (plan usuario): **volver al CPU: optimizar intérprete → dynarec JIT**.
Persistencia save a disco HECHA (arriba). NO tocar Perfect Dark.

---

## FASE POSTERIOR (anotado 2026-08-12) · Layout de datos por-juego en subdirectorios

Idea usuario: organizar los datos de usuario/emulador en subdirectorios **por tipo**, no sueltos
junto al ROM. Estructura destino (raíz p.ej. `~/.kestrel64/<game-id>/` o carpeta config):

- **`save/`** — battery/flash saves (`.eep/.sra/.fla`). HOY viven junto al ROM (attachSaveFile);
  migrar a `save/<game-id>.<ext>`.
- **`pak/`** — Controller Pak (Memory Pak, 32 KB por pak, 4 controladores). Backing joybus +
  fichero `.mpk` por slot. AÚN NO implementado (device joybus pendiente).
- **`textures/`** — **export** de las texturas de un juego a `textures/<game-id>/` (dump al vuelo
  desde loads RDP: hash de tile+TLUT+fmt → PNG). Objetivo: **custom textures / texture packs**
  (HD, cell-shading, upgrades vía IA, etc.) — cargar override desde el mismo subdir por hash.
  Requiere: (1) hook en la ruta de load-texture del RDP (TMEM) para dump, (2) tabla de override
  hash→imagen cargada al boot, (3) sustitución en el sampler soft-RDP (y eventual parallel-rdp).
- **`cover/`** — carátula/boxart por ROM para un futuro **menú de ROMs / launcher** (metadata +
  imagen; game-id desde header cart-ID).
- **`cheat/`** — cheat codes (GameShark/estilo) por game-id; motor de parcheo RAM por-frame.

TODO fase posterior — NO ahora. Prioridad vigente sigue: opt intérprete → dynarec JIT.

---

## MCP hotpath profiler (2026-08-12) · dónde se va el rendimiento

Peticion usuario: metodos MCP para detectar hotpaths CPU y RSP y ver donde se gasta el
tiempo emulado (lo mas pedido para saber que optimizar). Implementado, **opt-in** (coste cero
cuando apagado: una rama `if(profOn)` nunca tomada en runs normales).

**Sampler CPU** (`src/cpu/cpu.{hpp,cpp}`): cuenta por **PC fisico** en `step()`, justo tras
`translate(pc, AccFetch)`. Bucket = `phys>>4` (resolucion 16 B = 4 instrucciones), 512 K buckets
cubren los 8 MB de RDRAM. Clave usar fisico: KSEG0 y codigo mapeado por TLB (p.ej. PD @0x70xxxxxx)
que aliasan el mismo RDRAM **funden** en el mismo bucket. `profBuckets` se aloja al primer enable.

**Sampler RSP** (`src/rsp/rsp.{hpp,cpp}`): un contador `profPc[1024]` por **slot IMEM** (4 B),
incrementado en `step()` tras `curpc=pc`. Resolucion exacta por-instruccion → fija la rutina de
microcodigo caliente (graficos vs audio vs custom).

**Telemetria** (`src/telemetry/server.{hpp,cpp}`): comandos `prof.start` / `prof.stop` /
`prof.reset` (arman/limpian ambos samplers) y `prof.cpu` / `prof.rsp` (top-N descendente). CPU
devuelve por entrada `{phys, kseg0, count, pct, disasm}` (disasm de la 1ª instr del bucket) +
`{total, enabled, resolutionBytes}`; RSP `{imem, count, pct}` + `{total, enabled}`.

**Bridge** (`tools/mcp/kestrel_mcp.py`): `profile_start/stop/reset` + `profile_cpu(top)` /
`profile_rsp(top)`. Los dos ultimos **pausan el core, toman el snapshot y restauran** el estado
de run previo — porque el bucle free-run retiene `coreMutex` en lotes de ~un campo de video, y una
query read-only lanzada a mitad de run puede inanicionarse esperando la ventana entre lotes.
Pausar cede el lock al instante y da un snapshot sin torn. Si ya estaba pausado, se deja pausado.

### Flujo de uso
`profile_start()` → dejar correr la carga → `profile_cpu(top=N)` / `profile_rsp(top=N)`.

### Primer resultado (SM64 boot, ~300M instr)
- **CPU: 62.78 % en UN bucket** `kseg0=80246dd0` (`jal 803236f0`) → bucle spin/idle-wait domina
  el arranque (resto <1 % por bucket, cola larga). Confirma que el techo ahi NO es codigo caliente
  optimizable sino espera — el trabajo real esta en la cola.
- **RSP: cluster IMEM 0x79c–0x7b8** = inner loop del microcodigo caliente.

### Invariantes (profiler = rama muerta con `profOn=false`, ortogonal a semantica)
- systemtest intérprete **0/3721·0/2·0/6** (6.86s).
- systemtest `KESTREL_JIT=1` **0/3721·0/2·0/6** (6.88s).
- lockstep==threaded md5 **idéntico** (SM64, budget 300M): `2728103c3f0f7dcf6e38141fafa853f6`.

---

## Interpreter fetch fast-path (2026-08-12) · memoizar traducción por línea de I-cache

Peticion usuario ("si, tb, aplica eso"): cortar el coste per-instruccion del fetch cacheando la
traduccion pagina/linea para no llamar a `translate()`+`cacheable()` en cada instruccion.

**Que hace** (`CPU::step`, `src/cpu/cpu.{hpp,cpp}`): cachea la traduccion de la **linea de I-cache
de 32 B** que se esta ejecutando — `{fetchLineVBase (pc&~31, clave 64-bit COMPLETA), fetchLinePhys,
fetchLineCache, fetchLineReXor, fetchLineEpoch}`. Mientras el PC no salga de la linea y `xlatEpoch`
no cambie, se salta `translate()`+`cacheable()` (el coste dominante: en codigo mapeado por TLB es
un scan lineal de 32 entradas **por instruccion**). **NO** cachea los bytes: el fetch sigue pasando
por `icFetch(phys)`, asi que la semantica de I-cache y de codigo automodificable (snapshot stale
hasta invalidar) queda intacta.

**Por que es correcto**: una linea de 32 B nunca cruza una pagina TLB (minimo 4 KB, alineada) → las
8 instrucciones comparten mapeo; si la primera tradujo sin fallo, el resto tambien (sin fallos de
permiso perdidos). `xlatEpoch` se incrementa (`bumpXlat()`) en **todo** evento que puede alterar
una traduccion: `tlbWrite` (TLBWI/R), `writeCop0` a **Status** (modo/RE/bit64) o **EntryHi** (ASID),
entrada de excepcion (`takeException`, cambia EXL/modo) y **ERET**. Un epoch distinto fuerza miss y
re-traduce.

**BUG cazado + arreglado durante la implementacion**: la clave inicial era **low-32** de PC
(`u32`). En modo 64-bit, dos PC con igual low-32 pero distinta region alta (xkphys/xkuseg/ckseg…)
traducen distinto → **falso hit** → traduccion stale → fetch erroneo → **loop infinito** (colgaba
n64-systemtest, que ejercita direccionamiento 64-bit). Fix: clave = PC **completo u64**. El
oraculo (systemtest) cazo el fallo exactamente como debe.

**Ganancia**: SM64 free-run **~6 %** (12.4 → 13.1 Mips). Modesto **porque SM64 corre en KSEG0**
(translate = solo mascara, poco que ahorrar). El win grande es codigo **mapeado por TLB** (PD
@0x70xxxxxx: scan de 32 entradas/instr), no medible ahora (PD no se toca hasta orden). Tambien
acelera el fallback+validacion del dynarec (que reusa el intérprete). Toggle diagnostico
`KESTREL_NOFETCHFAST=1` (default OFF → fast-path ON).

### Invariantes (fast-path = memoizacion de traduccion, byte-identica al slow path)
- systemtest intérprete **0/3721·0/2·0/6** (6.86s).
- systemtest `KESTREL_JIT=1` **0/3721·0/2·0/6** (6.88s).
- lockstep==threaded md5 **idéntico y = baseline pre-cambio**: SM64 budget 300M
  `2728103c3f0f7dcf6e38141fafa853f6`.

## RSP VU — SSE 8-lane fast path (2026-08-13)

El unidad-vectorial (COP2) del RSP era **100 % escalar**: cada op un bucle `for(n=0..7)`.
El README prometia "VU = SSE4.1 reference" pero era el PLAN, no el codigo. La VU es el techo
de rendimiento (ares es *RSP-emulation-bound by design*). Ahora las ops paralelizables
corren **8 lanes = 1 registro XMM** (`src/rsp/rsp.cpp`, host i7-870 Nehalem = SSE4.2 max, sin AVX).

**Diseño**: `execVuSse(fn, vte, S, D)` intercepta antes del switch escalar en `execCop2`.
Devuelve `true` si acelera `fn` (bit-exacto), `false` = fallback escalar. Toggle A/B
`KESTREL_NORSPSSE=1` (default OFF → SSE ON). R128 (`u16 el[8]`) ↔ `__m128i` via `loadu/storeu`
(el[] no garantiza align-16).

**Ops aceleradas** (29):
- **Lote A**: logicos VAND/VNAND/VOR/VNOR/VXOR/VNXOR; VADD/VSUB (wrap→accl, `packs_epi32`=sclamp16,
  limpia VCO); VADDC/VSUBC (carry/borrow via `cvtepu16_epi32`+shift); comparaciones
  VLT/VEQ/VNE/VGE (mask logic `cmpgt/cmpeq`+`blendv`); VMRG.
- **Lote B (MAC, el premio en F3DEX2 T&L)**: VMULF/VMULU, VMACF/VMACU, VMUDL/M/N/H, VMADL/M/N/H.
  Acumulador **48-bit = 3 limbs de 16-bit** (`V48{h,m,l}`); primitiva `vadd48` con acarreo entre
  limbs via unpack-a-32-bit. Helpers: `vprodSS/SU/US` (productos 16×16 con correccion
  signed×unsigned = `mulhi_epu16 - (S<0?T:0)`), `vsatSigned`=`packs_epi32(unpack(m,h))`,
  `vsatUnsignedN`, `vsatMulU/MacU` (saturaciones custom).
- **Lote C (clip-compare, element-wise)**: VCH (0x25) y VCR (0x26). Puramente por-lane pero
  branchy → SSE via `blendv` de las dos ramas (signos opuestos = `cmpgt(0, S^T)`). VCH fija los
  5 halves de flags (VCC/VCO/VCE) fresco desde S,T; VCR fija VCC y limpia VCO/VCE. `cclA` de VCR
  (`S+T+1<=0` ⟺ suma-verdadera `<0`) se computa en 32-bit (`cvtepi16_epi32`) por overflow de s16.
  **VCL (0x24) queda escalar** — 4-vias, lee flags previos (vcol/vcoh/vce) y solo fija vccl/vcch
  condicional; su complejidad no vale la pena vs su baja frecuencia.

**Bug cazado por el fuzz** (semantica HW genuina, no hardcode): VMADL sumaba el parcial-alto
**unsigned** (`mulhi_epu16` zero-extendido, sin sign-ext). Mi primer intento sign-extendia a L1;
el fuzz mostro ~54 % mismatch en 0x0c. El C escalar `(u32)(u16*u16 >> 16)` compila a shift
**logico** (producto de dos u16 = no-negativo) = comportamiento HW real (VMADL acumula el parcial
bajo sin signo). Fix: addend `{0,0,mulhi_epu16}`.

### Verificacion (oraculo = intérprete escalar, byte-exacto)
- **Fuzz diferencial** `--rspfuzz` (nuevo): dos pasadas (escalar vs SSE) sobre estado VU random
  identico, compara estado completo (vpr[32]+acc+flags+div). Flags restringidos a 0/1 (espacio
  HW valido). **40M iters = 0 mismatches** en las 29 ops (Lote A+B+C).
- systemtest intérprete **0/3721·0/2·0/6**; `KESTREL_JIT=1` **0/3721·0/2·0/6**.
- lockstep==threaded md5 SM64 300M **idéntico y = baseline pre-cambio** (`cbf8aa76…b6ff24b`).
- **krom 47-suite A/B** `KESTREL_NORSPSSE=1` (VU escalar) vs SSE: **byte-idéntico** (ambos mean
  exact 92.30) → la VU SSE es ortogonal al SoftRDP, no mueve ni un pixel del render RDP.

### Broadcast de elemento — pshufb (2026-08-13)
`R128::operator()(e)` (construye `vte` = vt(e) segun la tabla de broadcast n64brew) era un gather
escalar `v.el[n]=el[src[e][n]]`. Ahora **un `_mm_shuffle_epi8`** (SSSE3) con 16 mascaras
precomputadas una vez (byte 2n,2n+1 del destino ← 2s,2s+1 de la fuente, preserva layout el[] →
byte-identico). Acelera **todo** uso de broadcast, incluidas las ops VU aun escalares
(VRCP/VCH/VCL/VMOV…), no solo el fast-path SSE. Re-verificado: fuzz 20M=0, systemtest 0/3721
interp+JIT, SM64 lockstep==threaded==baseline `cbf8aa76`.

### Rendimiento
`--vubench` (nuevo, mezcla ponderada F3DEX2, 100M ops): **2.10x** en el stream VU
(scalar 45.0 ns/op → sse 21.5 ns/op). El pshufb bajo ambos paths (scalar 54.5→45.0,
sse 28.5→21.5). Techo restante = dispatch de `execCop2` + ops no aceleradas
(VRCP/VRSQ/VCL/VRND/VMOV) que siguen escalares por lane. VRCP/VRSQ/VMOV son element-serial
(1 lane activo) → SIMD no ayuda; VCL/VRND poco frecuentes. VU SSE = objetivo cumplido.

## 2026-08-13 (cont.) — Dynarec: ampliación de cobertura de ops (avgK) — MULT/MULTU + HI/LO + MOVZ/MOVN

Tras BLTZAL/BGEZAL (absorción de branch de enlace REGIMM, verificada 0/3721 interp+JIT +
SM64 JIT BRDIFF sin divergencia), la palanca era **cobertura de ops**: cada opcode que
`emitSafeOp` no sabe emitir **corta el bloque** (→ avgK bajo, más overhead de entrada/salida
por op retirada). Se amplió el conjunto compilable con ops **base ISA de 32-bit, no
mode-gated**, semántica HW genuina (oráculo = intérprete):

- **MULT/MULTU** (SPECIAL 0x18/0x19): `imul64` sobre operandos extendidos a 64b (movsxd para
  MULT, zero-ext para MULTU); el producto de dos valores de 32b cabe en 64b, así que `low64`
  del imul = producto exacto. LO=`sext32(low32)`, HI=`sext32(high32)` vía `shr 32`+`movsxd`.
  Escriben los campos `hi`/`lo` del CPU (offset `offsetof(CPU,hi/lo)`, direccionados por RBX).
- **MFHI/MFLO/MTHI/MTLO** (0x10/0x12/0x11/0x13): mueven HI/LO ↔ gpr (64b completos).
- **MOVZ/MOVN** (0x0a/0x0b): `cmovz`/`cmovnz` de 64b; rd intacto si la condición no se cumple.
  No mode-gated (conditional-move ISA, disponibles en todos los modos).

**Bug cazado por el oráculo (no hardcode)**: el primer intento añadió también las ops de
**64-bit** (DADDU/DSUBU/DADDIU/DSLL*/DSRA*/DSLLV*). systemtest JIT falló
`daddiu_supervisor_32`: en el VR4300 las instrucciones de 64-bit **trapean RI** cuando el modo
64-bit está off (supervisor/user 32-bit; depende de Status.KX/SX/UX + KSU = estado runtime).
El JIT las emitía incondicionalmente. **Revertidas**: no se JITean (terminan el bloque → el
intérprete las ejecuta con la semántica de trap correcta). DIV/DIVU tampoco se JITean: `idiv`
x86 lanza `#DE` en div-por-0 y en `0x80000000/-1`, casos **definidos** (no-trap) en el VR4300;
emularlos exigiría saltos-guardia en el código generado.

Emitter nuevo: `imul64` (REX.W 0F AF), `cmovz` (REX.W 0F 44).

### Verificación (4 puertas, oráculo = intérprete)
- systemtest intérprete **0/3721·0/2·0/6**; `KESTREL_JIT=1` **0/3721·0/2·0/6** (cubre
  mult/multu/mfhi/mflo/movz/movn como base tests + `daddiu_supervisor_32` de nuevo verde).
- SM64 (matriz-math pesado = MULT/MFLO por doquier) framebuffer **JIT == intérprete** md5.
- krom 47-suite intacto (cambio CPU-only, ortogonal al SoftRDP).

## 2026-08-14 — Dynarec: block-linking Steps 1&2 + **JIT concurrente con RSP en THREADED (2.33x SM64)**

Tres cambios de dynarec, todos oráculo-verificados (systemtest interp+JIT 0/3721·0/2·0/6 +
SM64 300M framebuffer md5). Solo tocan `src/cpu/{jit.cpp,jit.hpp,cpu.hpp}` (cero RDP).

### 1. Block-linking Step 1 — prólogo re-validable (gated `KESTREL_JIT_LINK`, default OFF)
Groundwork para el linking real (Step 3). Cada bloque emite un prólogo que llama
`kestrel_jitProceedTramp(cpu, K)` → `CPU::jitReenterProceed`: re-muestrea interrupt (Cause
IP2/IP7 desde `interruptPending()`/`timerIntr`) + borde de timer (`(cmp-cnt)<=K`), **espejo
exacto del driver**. Si bail → `eax=0`, cae al epílogo con pc intacto → driver re-despacha ruta
lenta. K desconocido hasta compilar el cuerpo → placeholder `imm32` + `pokeU32` al cerrar.
Neutro por diseño (sin linking aún): con LINK off no emite nada (byte-idéntico); con LINK=1
systemtest interp+JIT pasan Timing/Cycle → prólogo correcto. Emitter nuevo: `pokeU32`.

### 2. Block-linking Step 2 — SMC por-bloque (no clear global)
SMC detectado (código bajo el bloque cambió) antes hacía `cc->clear()` **global** (destruye
todo el cache) — mortal para el linking del Step 3. Ahora marca `blk.dead=true` y `find()` lo
trata como miss → recompila **in-place** (insert sobrescribe la ranura del mismo phys). Otros
bloques intactos. Auto-reclaim: si el buf ejecutable desborda (fugas por dead-mark en SMC
pesado), `cc->clear()` antes de recompilar (sin esto el JIT quedaría muerto). `emit()` está
bounds-checked (jit.hpp:29) → overflow no corrompe. Campo nuevo `Block::dead`.

### 3. JIT concurrente con RSP en modo THREADED — **la palanca grande**
Profiling JIT (`KESTREL_JIT_STATS`) en SM64: decline dominante = **rsp=RSP corriendo** (55M de
134M calls ≈ 41%), compile-fail insignificante (2.4M, avgK=2.03 ya bueno). El driver declinaba
JIT SIEMPRE que `mem->rsp.running` (jit.cpp:639) para no romper el interleave RSP 2:3. Pero eso
**solo aplica a LOCKSTEP** (donde el hilo CPU pisa pasos RSP). En **THREADED** el RSP va en su
worker propio y el hilo CPU no lo toca → JIT es tan válido como el intérprete (mismo thunk de
memoria, misma concurrencia ya existente). Cambio: `if(mem->rsp.running && rcpMode==Lockstep)`.
Es exactamente la apuesta del proyecto (multihilo + CPU rápida).

**Matriz SM64 300M (todas byte-idénticas `cbf8aa761b92adab89ddde949b6ff24b`):**

| Config | Wall | Speedup vs oráculo |
|--------|------|--------------------|
| lockstep-interp (oráculo) | 21s | 1.00x |
| lockstep-JIT | 17s | 1.24x |
| threaded-interp | 17s | 1.24x |
| **threaded-JIT** | **9s** | **2.33x** |

threaded-JIT sobre threaded-interp = **1.9x**. Antes del cambio threaded-JIT ≈ threaded-interp
(JIT apagado durante gráficos). Framebuffer **idéntico al oráculo lockstep-interp** → el cambio
es correcto (la determinación lockstep==threaded no depende de la granularidad temporal CPU).
NOTA: baseline CLAUDE.md `2728103c` estaba driftado; nuevo `cbf8aa76` (este build, SoftRDP).

### Pendiente dynarec
- Step 3 (jmp rel32 real con back-patch): AÚN NO. Riesgo alto de corrupción silenciosa
  (bookkeeping Count/Random cross-bloque + re-traducción TLB del sucesor), validación md5 lenta;
  mejor con el usuario presente. Steps 1&2 son el fundamento seguro ya asentado.
- GOTCHA: modo threaded cuelga al salir (join de workers) tras `[fbdump] wrote` → `taskkill`
  el zombie; el framebuffer ya está escrito, no afecta al md5.

---

## 2026-08-15 — Dynarec Step 3: block-linking real (guarda + ranura indirecta)

**Estado: implementado, gated por `KESTREL_JIT_LINK=1` (default OFF). Lockstep VALIDADO.
Threaded con cadena larga: NO DETERMINISTA → ver "abierto" abajo.**

### Qué se emite

Salida de control con destino estático predecible (BEQ/BNE/Bcond ambas ramas, J/JAL; JR/JALR no,
son dinámicos) emite por candidato:

```
mov rdx, imm64 <VA destino>     ; guarda: kNoLink=1 al nacer (VA imposible, todo PC N64 es 4-align)
cmp rcx, rdx                    ; rcx = pc que el bloque acaba de escribir
jne slow                        ; no coincide → salida normal por el epílogo
add dword [rbx+jitPending], K   ; contabilidad diferida de ESTE bloque
jmp qword [rip+slot]            ; ranura de 8 bytes, DATOS tras el `ret` (nunca se ejecuta)
```

Enlazar/desenlazar = **escrituras de datos**, jamás reescritura de código ejecutable (sin
problemas de coherencia I-cache del host ni de otro hilo ejecutando el bloque).

- **Sólo ckseg0** (`0xFFFFFFFF_8000_0000..9FFF_FFFF`): ahí VA→phys es la máscara arquitectónica
  de un segmento NO mapeado, así que el enlace no depende del TLB.
- `linkEntry` = justo detrás de `sub rsp,40` del prólogo → el sucesor reusa el marco del
  predecesor y su epílogo retorna al driver que llamó al PRIMER bloque (profundidad de pila
  constante).
- Contabilidad diferida: `jitPending` lo commitea el prólogo del sucesor (retired/Count/Random/
  hits) y lo pone a 0 ⇒ en cualquier retorno al driver `pending==0` y el valor devuelto es exacto.
- Invalidación: la ÚNICA vía por la que el HW puede ejecutar código nuevo en una dirección ya
  ejecutada es invalidar la I-cache (el RCP/DMA no espían caches) ⇒ `cacheOp` I-side (fn 0/2/4)
  → `unlinkAll()` + `linkEpoch++`; re-enlace perezoso en la siguiente entrada validada por el
  driver. SMC (dead-mark) → `unlinkTo(phys)`. `clear()` tira todos los enlaces.

### Tres bugs encontrados al medir (los tres eran del banco, no del enlace)

1. **`jitTryBlock` devolvía sólo las ops del ÚLTIMO bloque de la cadena.** `stepCpu` mide la
   ventana de campo (750k ops → `viTick`) con ese valor, así que la cadena "escondía" ops: el
   VI tickeaba tardísimo, el juego se quedaba en el spin de espera de interrupción y el bench
   marcaba un falso **3.42s**. Fix: `jitChainOps` acumula lo commiteado y el driver devuelve
   `R + jitChainOps`. **Sin esto, block-linking rompe el pacing del VI de forma silenciosa.**
2. **La cadena podía pasarse de la ventana de campo.** Fix: `jitOpsBudget` (ops que quedan en
   la llamada a `stepCpu`); un eslabón enlazado no arranca si no cabe entero. El PRIMER bloque
   lo sigue despachando el driver sin tocar → comportamiento idéntico al de antes.
3. **`KESTREL_MAXINSN` sólo se comprobaba en el intérprete** (cpu.cpp, ruta debug op-a-op), así
   que con JIT el cap saltaba en otro punto del programa y el bench NO comparaba el mismo
   trabajo. Fix: el driver recorta `jitOpsBudget` al resto y declina el bloque que cruzaría el
   cap, cediendo las últimas ops al intérprete.

### Dos gotchas del banco de pruebas (documentados aquí porque cuestan horas)

- **SM64 escribe su EEPROM `.eep` junto a la ROM.** Si queda de una corrida previa, el arranque
  toma otro camino y el md5 cambia sin que el emulador haya cambiado. `scripts/bench.sh` borra
  el save antes de CADA corrida. Todo md5 comparado sin eso es basura.
- **`--run` headless no salía tras el cap de maxinsn**: `run()` veía `cpu.halted` y dormía para
  siempre. Ahora `System::exitOnHalt` (lo pone `--run`) termina el bucle; en modo MCP NO, ahí un
  halt es punto de inspección y el proceso debe seguir vivo.

### Resultados (SM64 300M ops, save limpio, SoftRDP)

| Config | Wall | md5 |
|--------|------|-----|
| lockstep-interp (oráculo) | 22s | `cbf8aa761b92adab89ddde949b6ff24b` |
| lockstep-JIT | 15s | idéntico |
| **lockstep-JIT+LINK** | **14s** | **idéntico** |
| threaded-JIT | 8s | idéntico |
| threaded-JIT+LINK (chain=1) | 12-17s | idéntico |
| threaded-JIT+LINK (chain=256) | 5s | **VARÍA entre corridas** |

- systemtest **0/3721 · 0/2 · 0/6** en interp, JIT y JIT+LINK (los tests Timing y Cycle pasan con
  el enlace activo ⇒ el commit diferido de Count/Random es correcto).
- Ganancia real del enlace en lockstep: **~7%** (15s→14s). Modesta: SM64 pasa mucho tiempo fuera
  de bloques enlazables (JR/JALR dinámicos, RSP corriendo en lockstep).
- A 20M ops interp y JIT paran en el MISMO pc/sp/ra. A 300M el pc del cap difiere (jitter de
  frontera de campo, ±ops por bloque) pero el framebuffer es idéntico.

### ABIERTO — threaded + cadena larga no es determinista

`KESTREL_THREADS=1 KESTREL_JIT=1 KESTREL_JIT_LINK=1` con `chain=256` dio 2 md5 distintos en 3
corridas (`3fe185fa` y `21ec9ed5` x2) y baja a 5s. Con `KESTREL_JIT_CHAIN=1` (toda la maquinaria
de enlace en pie, cero saltos encadenados) vuelve a ser estable e idéntico al oráculo, y
threaded-JIT sin enlace es estable 3/3. Conclusión: **no lo rompen las guardas ni la contabilidad
diferida, sino correr mucho rato sin volver al driver mientras los workers RSP/RDP avanzan en
paralelo** — hay un punto de sincronización que hoy sólo ocurre por vuelta al driver. Siguiente
paso: encontrarlo (candidato: muestreo de estado del RCP/MI entre bloques) o, si no lo hay,
limitar la cadena en modo Threaded. Hasta resolverlo, **LINK sólo con lockstep**.

Diagnóstico: `KESTREL_JIT_CHAIN=N` (default 256) acota la cadena; `scripts/bench.sh` corre la
matriz con save limpio.

### Pendiente
- Gate 4 (suite krom 47) con LINK: sin correr todavía.
- RSP HLE / recompilador RSP = la palanca grande medida (mayor que el enlace de bloques).

## 2026-08-15 — Batería de validación automatizada + bug del volcado de framebuffer

### `scripts/validate.py` (wrapper `scripts/validate.sh`)

Un solo punto de entrada para las tres puertas obligatorias. Sustituye a los batch ad-hoc
(`batch2.sh` + `cmpg.py` en scratchpad, que se perdían con la sesión):

```bash
bash scripts/validate.sh systemtest --mode interp
bash scripts/validate.sh krom       --mode link --vs interp      # 371 ROMs, ~100-140s
bash scripts/validate.sh sm64       --mode threaded-jit
bash scripts/validate.sh all        --mode jit                    # las tres, para en la 1a que falla
```

- **Salida mínima por diseño**: cada puerta imprime 2-4 líneas. El detalle por ROM va a
  `out/krom-<modo>.tsv`; la corrida se diffea contra `docs/baselines/krom-<modo>.tsv`, así que
  una corrida limpia dice `regress=0 improve=0` en vez de escupir 371 filas.
- `--mode` mapea los toggles del emulador: `interp` (oráculo) · `jit` · `link` · `threaded` ·
  `threaded-jit` · `threaded-link`. `--vs interp` compara un modo contra la baseline del
  intérprete: **la salida del RDP no puede depender de cómo se ejecutó la CPU**.
- Corre en paralelo (`--jobs`, default = núcleos/2). Ya no hace falta el baile de `taskkill`
  del batch viejo: con `System::exitOnHalt` el proceso termina solo al llegar al cap.
- `--filter RDP/16BPP` para barridos rápidos; la batería completa NO hace falta en cada cambio.
- El `.eep` de SM64 se borra antes de cada corrida (gotcha ya documentado arriba).

Dos arreglos del comparador que cambiaban la nota sin que cambiara el emulador:
- Pillow rechaza los PNG de referencia que llevan el `.asm` entero en un chunk `zTXt`
  (límite 1 MB) → `MAX_TEXT_CHUNK` subido; si no, esos ROMs desaparecían del barrido.
- Cuando volcado y referencia no miden lo mismo se reescala con **NEAREST**, nunca con filtro
  (un filtro inventa colores que el RDP nunca escribió), y se anota `SIZE WxH vs ref WxH`.

### BUG ENCONTRADO — el volcado de framebuffer sacaba el ancho equivocado

`dumpFramebufferBmp` derivaba el ancho de la ventana activa de H_VIDEO (`(hend-hstart)/2`) y
luego remuestreaba por X_SCALE. Eso es el trabajo del *presentador*, no del oráculo: clavaba
**todo volcado a 320 columnas** (H_VIDEO estándar $6C02EC = 640 activos / 2) daba igual lo que
el ROM hubiera renderizado. Un framebuffer de 640 salía a la mitad; uno de 160, al doble.

**181 de 371 referencias discrepaban en tamaño sólo por esto**, así que toda nota calculada
sobre ellas era ruido. Ahora el volcado es el framebuffer fuente tal cual: `w = VI_WIDTH`, un
píxel de salida por píxel almacenado, sin remuestreo.

| | antes | después |
|---|---|---|
| mean exact (371 ROMs) | 82.91 | **86.51** |
| ROMs perfectos (>=99.99%) | 19 | **144** |
| discrepancia de tamaño | 181 | 31 |

De las 31 que quedan, 22 son `640x240 vs ref 640x480` con **exact 100.00%**: el ROM renderiza
240 líneas fuente y la captura de referencia es la salida entrelazada de 480; decimando da
identidad exacta ⇒ no son bugs. Reales pendientes: `RDP/8BPP InternalPalette` (160x240, 0%,
ya diferido), `EMU/SNES PPU*` (272 vs 320) y `N64NICCC` (256x199 vs 320x240).

### Gate 4 (suite krom) CERRADO — block-linking validado

| Puerta | interp | jit | link |
|---|---|---|---|
| systemtest | 0/3721 · 0/2 · 0/6 (16s) | idem (25s) | idem (38s) |
| krom 371 | mean 86.51, 144 perfectos (141s) | — | **idéntico, regress=0** (98s) |
| SM64 300M md5 | `cbf8aa761b92adab89ddde949b6ff24b` | MATCH | MATCH |

SM64 también MATCH en `threaded` y `threaded-jit`. `threaded-link` sigue fuera (no
determinista, ver sección anterior).

### Bugs que destapó el primer barrido completo (pendientes)

- **Vídeo decodificado en negro**: `Video/GRB12Decode`, `GRB15Decode`, `GRB12/GRB15/YUV8/YUV16
  LZDIFFRLEVideo` dan framebuffer 100% negro (exact 0.00). No es falta de instrucciones: a 20M
  y a 200M ops el resultado es el mismo, y el ROM termina en su bucle final normal.
- `RDP/RDPModeInput` 6.49 · `HelloWorld/32BPP RDP` 4.22 · `RDP/32BPP SetPrimColor` 0.08.
- `Compress/DCT` y `RSP/DCT` (quantization multi-block) 5-11%.
- Animados con desfase de frame (NO bugs, ya sabido): `Rotate*`, `CP1/Fractal`, `VIScrollingBG`.

## 2026-08-17 — RDP: filtro 3-point en enteros, expansión 5→8 fijada, y dos PNG de referencia falsos en krom

Segunda pasada sobre los fallos que destapó el barrido completo. Tres cosas, todas
semántica de HW genuina (ninguna tocada para pasar un test).

### 1. El filtro de textura ahora es entero, como el HW

`sampleTexFiltered` interpolaba en `double` y redondeaba al final. El RDP trabaja en
10.5 fijo y hace la mezcla en enteros:

```
frac = st & 31;  st >>= 5
si frac.x+frac.y >= 32:  base = texel(s0+1,t0+1),  wx = 32-frac.y, wy = 32-frac.x
si no:                   base = texel(s0,t0),      wx = frac.x,    wy = frac.y
out = (((t10-base)*wx + (t01-base)*wy + 0x10) >> 5) + base
```

(oráculo: parallel-rdp `texture.h`, bloque `bilerp && (sample_quad || tlut)`). El `>>5`
es aritmético, así que una pendiente negativa trunca hacia -inf — con `double` +
redondeo-al-más-cercano el resultado caía un nivel arriba justo en los bordes .5, y en
un framebuffer de 16bpp eso se convierte en una banda visible.

Añadido también **MID_TEXEL** (Set_Other_Modes bit 44): con `frac == (16,16)` el filtro
degenera a la media de los 4 texels `(a+b+c+d+2)>>2` (compensación de medio píxel MPEG).
Antes se ignoraba el bit.

### 2. Expansión 5→8 bits: replicación en TODAS las rutas

Se probó truncar (`v<<3`) en TMEM/TLUT. Es **incorrecto**. Dos oráculos independientes:

- `RDP/TextureCoordinates` está construido justo para esto (textura 16x8 RGBA16 ampliada
  8x con y sin SAMPLE_TYPE). En (164,69) el combinador es un paso directo de TEXEL0, la
  muestra cae entre el texel `$0000` (R=0) y `$F800` (R=31) con `tfrac=4/32`, así que el
  nivel escrito es `((R01-R00)*4 + 0x10) >> 5`. Truncando: `(248*4+16)>>5 = 31` → nivel 3.
  Replicando: `(255*4+16)>>5 = 32` → nivel 4, que es lo que tiene la captura de HW.
- Para la TLUT, las referencias de `EMU/SNES/PPU/*Tile8x8` y `EMU/GameBoy/PPU/2BPPTile8x8`
  cumplen `v == ((v>>3)<<3)|((v>>3)>>2)` en el **100%** de 230400 muestras. Truncación
  sólo acierta 22-49%. No hay dos decodificadores: texel y paleta expanden igual.

`RDPGRB15Decode` parecía decir lo contrario (sus valores son casi todos múltiplos de 8).
No sirve como oráculo: sus canales tienen pasos de 4 (R) y de 1 (B), o sea reconstruye
color en varias pasadas — está roto por otra razón (sigue en 0.00).

### 3. El blender NO reexpande el alfa... salvo que sí

Se quitó `a0 += (a0+1)>>8` siguiendo a parallel-rdp (que expande dentro del combinador y
vuelve a clampar a 0xff, así que el blender vería 31 como máximo). Con eso `GRB15Decode`
pasó a dar exactamente ref-1 en TODO el frame: el HW no pierde ese 1/32. La línea vuelve
a estar, ahora con el testigo apuntado en el comentario.

### PNG de referencia duplicados en krom (¡no usarlos como oráculo!)

Dos capturas están copiadas de otro test — mismo md5, mismo contenido:

| test | png | md5 | de quién es en realidad |
|---|---|---|---|
| `HelloWorld/32BPP/HelloWorldRDP320x240` | `HelloWorldRDP32BPP320X240.png` | `d8b8ef4c…` | de la versión **16BPP** (su fondo es (255,231,0) = `Set_Fill_Color $FF01FF01` del test 16bpp, no el `$FFFF00FF` del 32bpp) |
| `Video/GRB24Decode/RDP` | `RDPGRB24Decode.png` | `6a701a0f…` | de **GRB15** |

Por eso `HelloWorldRDP32BPP` marca 1.00-4.22 pase lo que pase: es incomparable.

### Herramientas nuevas (baratas en tokens)

- `scripts/probe.py <trozo-de-ruta>` — corre un ROM krom headless y saca los N pares
  (nuestro, referencia) más frecuentes con conteo/%/delta. Un delta constante señala
  redondeo del combinador; un tinte, decodificación de textura. Ojo: lanzarlo **sin**
  `clang64/bin` en el PATH (ese python no tiene numpy).
- `scripts/imgstat.py` — tamaño/media/colores distintos + la terna de precisión.

### Verificación

| Puerta | resultado |
|---|---|
| systemtest interp | 0/3721 · 0/2 · 0/6 |
| systemtest JIT | 0/3721 · 0/2 · 0/6 |
| SM64 300M lockstep vs threaded | `cbf8aa761b92adab89ddde949b6ff24b` == `cbf8aa76…` |
| krom 371 | mean_exact **86.51**, 144 perfectos, regress=0 |

Mejoras contra la baseline anterior: `RDP/AlphaCoverage` 41.31 → **44.72**,
`RDP/CombinerLongTailConstants` 67.77 → **71.05**. Baseline congelada.

---

## 2026-08-18 — Contadores DPC + modelo de coste RDP + 2 bugs RSP threaded

### Contadores de rendimiento DPC (antes eran ceros fijos)
`DPC_CLOCK/BUFBUSY/PIPEBUSY/TMEM` (0x10/0x14/0x18/0x1C) implementados de verdad:
`std::atomic<u32>` en `Rcp`, lectura enmascarada a 24 bits, y los bits 6..9 de
escritura a `DPC_STATUS` limpian TMEM/PIPE/BUF/CLOCK. Expuestos por telemetría
(`dp.clock/bufbusy/pipebusy/tmem`). El RDP los acumula desde su worker mientras
rasteriza, la CPU los lee y limpia → atómicos obligatorio.

### Modelo de coste del RDP (calibrado contra HW real)
Derivado de las 100 configuraciones medidas en hardware de
[Thar0/RDP-Timing-Tests](https://github.com/Thar0/RDP-Timing-Tests) (`sample_results.txt`).

El RDP procesa el span en trozos de ~8 píxeles RGBA16 y ejecuta las transacciones de
ese trozo en orden **color read, depth read, color write, depth write**.
`coste_trozo = max(pipeline, memoria)`:

- `pipeline` = 1 GCLK/px (1-cycle) o 2 (2-cycle); FILL/COPY = 0.25 (64 bit/ciclo), más
  un overhead fijo por trozo.
- `memoria` = `XFER` por transacción + un `LAT` si el trozo lee algo (las escrituras
  son *posted*: una escritura de color sola se esconde bajo el pipeline) + `ROW` por
  cada alternancia FB↔ZB cuando comparten banco de 1 MB (la fila abierta del trozo
  anterior cuenta) + escalado del VI `×(1+VI)` y `VIROW` por transacción de FB cuando
  el VI comparte banco con el framebuffer.

Constantes ajustadas: `LAT=3.583 XFER=6.677 ROW=1.905 VI=0.088 VIROW=0.595
CHUNKOVH=1.606`. Error vs HW: **rmse 0.133, mae 0.114, peor 0.30 ciclos/px** sobre un
rango medido de 1.01–4.69 ciclos/px. El modelo aditivo plano anterior daba mae 0.167 /
peor 0.49. Script reutilizable: `scripts/rdptiming.py` (`table` / `fit` / `check` /
`compare <results.txt>`), así que revalidar no cuesta tokens de análisis.

Enganchado en `fillRect`, `drawTriangle` y `texRect` (`accountPixels`, que distingue
píxeles escritos de píxeles matados por alpha/z: en HW eso vale ~1 ciclo/px) y en
`loadTile` (`accountTmem`). **Sólo contabilidad — nunca condiciona la ejecución.**

### fillRect 1-/2-cycle: alpha-compare y prim-depth
Leyendo la display list de la ROM de timing apareció un hueco real: `fillRect` en
1-/2-cycle ignoraba el alpha-compare y el z. Ahora aplica alpha-compare (alfa
COMBINED contra el umbral de `blend_color`) y test/update de z **sólo bajo
`Z_SOURCE_SEL`** — un fill rect no lleva pendiente de z, así que su única fuente de
profundidad definida es `SET_PRIM_DEPTH`; sin ese bit el z se deja intacto. El test de
profundidad se extrajo del triángulo a `SoftRdp::depthTest` (compartido, misma
semántica). krom: `Cycle1FillZBufferRectangle` 16/32BPP y `RSPXBUSRDP` **96.15 → 98.60**,
regress 0.

### DOS bugs de concurrencia RSP (threaded), preexistentes
`validate.sh --mode threaded` se colgaba en *"RSP VRSQ (all 16 bit values)"*. Bisecado
con stash: **preexistente**, no de este trabajo. Dos violaciones de semántica RCP:

1. **Arranque de RSP descartado.** Un CLEAR_HALT solitario se condicionaba a la
   bandera de emulador `rspBusy`; si llegaba mientras el worker aún cerraba la tarea
   anterior, el arranque se tiraba en silencio → tarea nunca ejecutada → la CPU espera
   un BREAK eterno. Ahora la condición es el bit HALT real del RSP (lo que ve la CPU) y
   el lanzamiento se ordena tras el wind-down (`Memory::rspAwaitIdle`). Un poke a
   SP_STATUS a mitad de tarea (abort, signal) NO espera — sólo los lanzamientos.
2. **Writeback del PC publicado después de HALT.** `Rsp::step()` ponía `HALT|BROKE` en
   el BREAK y *después* escribía `sp_pc`. La CPU toma HALT como "tarea terminada" y
   escribe inmediatamente el SP_PC de la siguiente; ese writeback tardío lo pisaba y la
   tarea nueva arrancaba en el BREAK viejo → no escribía nada → `a=0x0` en
   `RSP VRCP (all 16 bit values)`, ~1 fallo por cada 65536 iteraciones, no determinista.
   Ahora BREAK sólo engancha `broke` y la publicación del estado va tras el writeback
   del PC. En HW, cuando la CPU observa HALT el PC ya es definitivo.

Además `Rcp::sp_status` pasa a `std::atomic<u32>`: un único registro que actualizan la
CPU (escrituras de control) y el worker RSP (BREAK) no puede ser `u32` plano sin perder
updates (RMW entrelazados).

### Invariantes (los tres modos)
| Modo | systemtest | krom 371 | SM64 300M md5 |
|------|-----------|----------|----------------|
| interp | 0/3721 · 0/2 · 0/6 | mean **86.53**, regress 0 | `cbf8aa76…b6ff24b` |
| JIT | 0/3721 · 0/2 · 0/6 | mean **86.53** | idéntico |
| threaded | 0/3721 · 0/2 · 0/6 (4/4 corridas limpias) | mean 86.50 | idéntico |

Tiempos de referencia de cada gate documentados en `docs/baselines/timings.md` —
sirven para distinguir *lento* de *colgado* sin volver a bisecar a ciegas.

## 2026-08-18 — RDP-Timing-Tests (Thar0) corriendo: rmse 0.948 → 0.133 cyc/px

Tercera batería de validación, la única que mira **tiempo** en vez de píxeles:
[Thar0/RDP-Timing-Tests](https://github.com/Thar0/RDP-Timing-Tests) barre 100
configuraciones de fill y publica contadores DPC medidos en N64 real. Detalle completo
(build, harness, modelo, huecos) en **`docs/RDP-TIMING.md`**.

### Camino para poder correrla
- **ISViewer lectura** (`Memory::isvRead`, gancho al principio de `cartRead`): libdragon
  sondea el magic en `0x13FF0000` antes de usar el canal; sin respuesta decide que no
  hay debug channel y la ROM no imprime nada. Ahora responde header + buffer.
- ROM local parcheada: `debug_init_isviewer()` y `TOTAL_RUNS` overridable
  (`make EXTRA_CFLAGS=-DTOTAL_RUNS=8`). Los 1000 repeats de upstream promedian jitter de
  refresh RDRAM/VI en HW; nuestro modelo es determinista y da el mismo número siempre.
- `scripts/rdptiming.py compare` parsea la salida cruda y casa cada config con la
  referencia HW **por tupla de features**, no por orden de línea. Barrido = 97 s.

### El bug que encontró: el dominio de profundidad es 18-bit, no 15
Todo el error estaba en las configs "ZB Read/Write, **Z Pass**": el test de profundidad
no pasaba, así que nunca se pagaba la escritura de z. Causa: `SET_PRIM_DEPTH` guardaba
el campo Z de 15 bits pelado. El HW lo mete en el mismo atributo s15.16 que produce el
interpolador de z del triángulo, y la unidad de profundidad consume **bits 31:13** — 18
bits, 3 de ellos fraccionarios (oráculo parallel-rdp: `rdp_renderer.cpp:3510` y
`shaders/interpolation.h`, neto `>> 13`).

Esos 3 bits no son adorno: el formato almacenado (exp 3 bits + mantisa 11) cerca de
`z = 0x7FFF` avanza de 1 en 1 en el dominio de 18 bits pero de **64 en 64** en el de 15.
Un test que camina prim depth de 1 en 1 cuantizaba todos los pasos al mismo valor y
fallaba cada compare menos el primero. Fix: `prim_z = ((cmd >> 16) & 0x7fff) << 3` y
coeficientes Z del triángulo `/ 8192.0` en vez de `/ 65536.0`. Semántica RDP genuina, no
parche a medida del test — sube además 5 tests de z de krom (96.15 → 98.60 ×3,
98.10 → 98.15 ×2).

`n=100/100 matched  rmse=0.1332  mae=0.1137  max=0.2992 cyc/px` — mismo residuo que
tiene el propio modelo contra HW; lo que queda es modelo, no implementación.

### Hueco aceptado: `RDPTest/CPU` y `RDPTest/RSP` 100.00 → 99.65
Esas ROMs de krom imprimen los registros DPC como texto y comparan pantalla contra PNG.
Su trabajo RDP es un fill rect FILL-cycle a pantalla completa — **sin z ni prim depth**,
así que el fix de profundidad no puede ser la causa; lo que movió el score es que el
modelo de coste ya produce contadores no-cero donde antes había ceros. Nosotros pintamos
`CLOCK = BUFBUSY = PIPEBUSY = $000144E9`; la referencia pinta `$00000000` en los cuatro.

No se toca, y no se hardcodea: (1) en HW los contadores no son cero — el dataset de Thar0
mide miles de ticks GCLK y el profiler de libultra lee `DPC_CLOCK` sin escribir nunca
`DPC_STATUS`; una referencia de cero exacto tras un fill de pantalla completa huele a
captura de emulador que los deja stub. (2) Nuestros tres contadores salen idénticos y el
coste de FILL-cycle está sin calibrar (el modelo se ajustó sólo con fills de 1/2-cycle
RGBA16; FILL escribe 64 bits por reloj). Ambas cosas son huecos de modelado reales,
apuntados en `docs/RDP-TIMING.md`, y necesitan su propio oráculo HW porque el barrido de
Thar0 nunca entra en FILL.

### Invariantes tras el cambio
| Modo | systemtest | krom 371 | SM64 300M md5 |
|------|-----------|----------|----------------|
| interp | 0/3721 · 0/2 · 0/6 | mean **86.53**, regress 0 | `cbf8aa76…b6ff24b` |
| JIT | 0/3721 · 0/2 · 0/6 | — | — |
| threaded | — | — | idéntico a interp |

## 2026-08-18 (bis) — El gate krom paraba las ROMs a medio dibujar: 86.53 → 87.36

Investigando `RDP/RDPModeInput` (6.49%, negro en pantalla) salió un fallo del **harness**,
no del emulador: el gate krom corría cada ROM con un cap fijo de **20M instrucciones**.
Las ROMs que decodifican una imagen en CPU (DCT, Huffman, I4/I8, GRB, Mandelbrot) todavía
iban por el primer tercio del cuadro cuando el cap disparaba, así que el compare puntuaba
una imagen a medio pintar — y ~33% *parece* un bug de emulación sin serlo.

Regla nueva, tres cotas en el emulador, todas semánticas y ninguna por-ROM:

- **`KESTREL_STABLE=<insns>,<checks>`** — hash del framebuffer VI cada N instrucciones;
  para cuando lleva `checks` comprobaciones idéntico *habiendo cambiado alguna vez* (si no,
  una ROM que aún no ha dibujado contaría como "estable" en negro). Es el criterio normal.
- **`KESTREL_MAXFLIPS=<n>`** — para tras N *swaps* de buffer mostrado (VI_ORIGIN cambia a
  otra dirección). Cota para lo que nunca se estabiliza: vídeo, demos. No afecta a los
  decoders, que pintan un cuadro en un solo buffer y no hacen flip nunca.
- **`KESTREL_MAXSYNCS=<n>`** — para tras N SYNC_FULL del RDP (= display lists completadas).
  Misma idea para animación que redibuja el mismo buffer en sitio.

El cap de instrucciones queda de red de seguridad (150M) y es lo único que acota a las
ROMs que animan sin flip y sin SYNC_FULL (el demo del cubo genera su lista cada cuadro y
no emite ninguno).

Con `--stable 8000000,3 --maxflips 1 --maxsyncs 1`: los PNG de referencia de krom son
capturas del **primer cuadro**, así que las cotas de cuadro además aciertan mejor que el
cap viejo. Mean 86.53 → **87.36**, broken(<50%) 42 → **38**, cero regresiones estáticas:

| test | antes | ahora |
|---|---|---|
| `Compress/HUFFMAN/HUFFMANGFX` | 39.63 | **99.41** |
| `Compress/HUFFMAN/HUFFMANROMGFX` | 29.47 | **99.33** |
| `N64NICCC` | 80.83 | **97.97** |
| `CP1/Fractal/Mandelbrot 320x240` (×2) | 30.6 | **84.3** |
| `CP1/Fractal/Mandelbrot 640x480` (×2) | 17.47 | **41.61** |
| `Compress/DCT/QuantizationMultiBlockGFX8BIT` | 14.49 | 32.17 |
| `RDP/Triangle/Rotate/*` (×4) | 64-66 | 69-72 |
| `RDP/Triangle/Cube/*` (×2) | 77.58 | 56.72 ← lotería de frame, sin cota |

Coste: barrido krom 137 s → ~235 s (los decoders ahora corren hasta terminar; Mandelbrot
solo ya son 99M instrucciones). Sigue siendo un gate de 4 min.

`RDPModeInput` pasa de negro a pintar las dos columnas de texto, pero le falta la imagen
central: el decode GRB por RDP sigue roto (mismo grupo que `Video/GRB12Decode` 0%). Ese sí
es bug de emulación y queda apuntado como siguiente frente.

## 2026-08-18 (ter) — TLUT: destino real del LOAD_TLUT, expansión sin replicar, y dither RGB

Frente abierto: `Video/GRB12Decode` **0.00** (pantalla negra) y `RDP/RDPModeInput` sin la
imagen central. Mismo grupo: decoders que meten los planos de color en TMEM y los releen
como CI4 con paleta. Cuatro hallazgos, todos de semántica HW y ninguno atado a un test.

### 1. `LOAD_TLUT` escribe en la dirección TMEM del **tile de destino**, no en el índice SL

En HW la paleta vive en los 2 KB altos de TMEM (palabras de 64 bits `0x100..0x1ff`, una
entrada por palabra con el valor de 16 bits replicado 4×), y `LOAD_TLUT` empieza a escribir
en `tiles[t].tmem`. Nuestro `tlut[]` modela esa región plana, así que la entrada *n* de la
carga cae en `(tile.tmem & 0xff) + n` — exactamente la sub-paleta que luego selecciona el
campo `PALETTE` del dibujo CI4 (paleta *p* = entradas planas `p*16 .. p*16+15`).

Antes ignorábamos el tile y escribíamos en `SL`. Los decoders GRB cargan sus tres planos en
`PALETTE_3/4/5` (tile TMEM `$130/$140/$150` → índices planos 48/64/80): las tres paletas se
apilaban en el banco 0 y **cada lookup leía ceros** → negro.

### 2. Una entrada de TLUT NO se expande como un texel: rellena con ceros (`v<<3`)

La ruta de lectura de paleta rellena los 3 bits bajos con **cero**; la de texel de TMEM los
**replica** (`v<<3 | v>>2`). angrylion mantiene dos macros distintas justo por esto
(`GET_HI_RGBA16_TLUT = (x>>8)&0xf8` vs `GET_HI_RGBA16_TMEM = replicated_rgba[...]`);
parallel-rdp replica en ambas, que es una divergencia conocida de su lado.

Los decoders GRB son el testigo: escriben a un color-image de **32bpp**, así que el valor de
8 bits se ve entero sin que la cuantización a 5 bits lo tape, y sus referencias de HW dan
exactamente `v<<3` en todos los canales (los valores de paleta son los impares 1,3,…,31, así
que replicar y rellenar con cero difieren en `v>>2` = 0..7). Ninguna etapa posterior puede
bajar un valor: el dither sólo redondea hacia arriba.

### 3. Dither RGB (`RGB_DITHER_SEL`) — feature que faltaba entera

`SET_OTHER_MODES` bits 39:38: 0 = magic square, 1 = Bayer estándar, 2 = ruido, 3 = off. No es
un offset con signo: sube el canal al **siguiente múltiplo de 8** cuando sus 3 bits bajos
superan el umbral de la matriz, y satura a 255 desde ≥248. Se aplica a la salida del blender
justo antes del writeback (parallel-rdp `memory_interfacing.h`, tras `blender()` y antes de
`write_color`), es independiente de la profundidad del color-image, y FILL/COPY lo saltan
porque no pasan por el blender. El modo ruido usa un hash determinista de (x,y,canal) para
que el invariante lockstep==threaded siga siendo comprobable.

### 4. El color de memoria del blender también rellena con ceros

`decode_memory_color` de parallel-rdp: `FB_FMT_RGBA5551 -> rgb & 0xf8`. El readback de 16bpp
entra al camino de 8 bits con los 3 bits bajos a **cero**, no replicados. Sin dither la
diferencia es invisible (el writeback vuelve a truncar a 5 bits), pero con dither un readback
replicado (bits bajos `111`) sube un nivel **cada píxel que el blender deja pasar tal cual**.
Se vio al instante: las 15 ROMs de `RDP/16BPP/.../TextureRectangle` cayeron ~3.5 puntos, con
2732 píxeles de fondo exactamente +1 nivel de verde — los texels transparentes mezclan el
fondo a través y la captura de HW conserva el nivel exacto.

### Resultado (barrido 371 ROMs, interp)

mean_exact **87.36 → 87.54**, broken(<50%) 38 → 37, 13 mejoras y 0 regresiones válidas:

| test | antes | ahora |
|---|---|---|
| `Video/GRB12Decode` | 0.00 | **19.11** |
| `Video/GRB15Decode` / `GRB24Decode` | 0.45 / 0.43 | **11.08 / 11.07** |
| `RDP/AlphaCoverage` | 44.72 | **72.28** |
| `RDP/16BPP/Triangle/ShadeTriangle` | 90.54 | **96.08** |
| `Cycle1Texture{Rectangle,Triangle}YUV16B` (16/32bpp, ×4) | 93.97-94.82 | **97.11-98.45** |
| `RDP/Triangle/Cube/FillTriangle` (×2) | 56.72 | 59.54 |
| `RDP/RDPTex0And1` | 91.93 | 92.68 |
| `RDP/CombinerOverflow` | 42.18 | 42.71 |

### Dos referencias de krom que no pueden calificar nada

El gate las puntúa y las lista, pero ya no cuenta como regresión (`BAD_ORACLE` y filas con
nota `SIZE` en `scripts/validate.py`):

- `HelloWorld/32BPP/HelloWorldRDP320x240`: su PNG es **byte a byte el mismo** que el de la ROM
  de 16BPP (mismo md5). El fondo de la referencia es el fill de 16bpp `$FF01FF01` (niveles
  31,28,0 → 255,231,0) mientras esta ROM llena 32bpp `$FFFF00FF` (255,255,0), y todos sus
  colores son niveles de 5 bits replicados. `mean_close` da 100.00: sólo difiere en la
  cuantización. No es un bug nuestro.
- Las 31 filas `SIZE`: dump y referencia no coinciden en resolución, así que la nota puntúa un
  `NEAREST` reescalado y se mueve por cualquier desplazamiento de píxel. El bug de modo VI que
  señalan es el hallazgo; el número no sirve de oráculo (las 3 `EMU/SNES/PPU/*` que "caían"
  eran esto: 272×240 vs referencia 320×240).

### Lo que queda de este frente

- `EMU/GameBoy/PPU/2BPPTile8x8` **66.93 → 63.51**, única regresión real y aceptada. Su combiner
  es `(TEXEL0 - COMBINED) * TEXEL0`, o sea depende del **latch COMBINED del modo 1-cycle**, la
  misma familia que `RDP/CombinerOverflow` (parada porque los oráculos discrepan). Medido: el
  verde pre-dither del HW es ~158 (mezcla de niveles 19/20 en proporción 6/16), y ni rellenar
  con ceros ni replicar la paleta lo produce con `COMBINED = 0`; el valor de COMBINED es la
  incógnita, no la expansión. Con paleta replicada el test vuelve a 66.93 pero los GRB caen de
  9.14 a 2.70 de media, así que la expansión se queda como la deja el oráculo de 32bpp.
- Los GRB siguen en 11-19%: la estructura ya es correcta, el residual es filtrado bilineal/
  3-point sobre texrects magnificados (con 4× de magnificación las filas que casan caen cada 4
  líneas) más el redondeo del blender.

### Invariantes tras el cambio
| Modo | systemtest | krom 371 | SM64 300M md5 |
|------|-----------|----------|----------------|
| interp | 0/3721 · 0/2 · 0/6 (20 s) | mean **87.54**, regress 0 | `cbf8aa76…b6ff24b` |
| JIT | 0/3721 · 0/2 · 0/6 (28 s) | — | — |
| threaded | — | — | idéntico a interp |

## 2026-08-18 (quater) — Los decoders GRB, resueltos a bit exacto: LOD_FRAC=0xff, TLUT replicada, blender sin fudge

Frente heredado: `Video/GRB12Decode` 19.11, `GRB15Decode` 11.08, `GRB24Decode` 11.07, y la
teoría apuntada arriba ("residual = filtrado bilineal/3-point sobre texrects magnificados").
**Esa teoría era falsa** y el .asm lo desmiente: en `RDPGRB12Decode.asm` el plano verde se
dibuja con `DSDX = DTDY = 1<<10` (1:1, sin magnificar), el rojo a 2× (`$200`) y el azul a 4×
(`$100`); un filtro no puede explicar un error que aparece también en el plano 1:1.

El error real era un **escalón constante por nivel**. Los tres decoders montan la misma
tubería: cargan cada plano de color como CI4 con paleta propia y lo mezclan sobre el
framebuffer con el blender en modo aditivo. Con 16 niveles de paleta observados en la
captura de HW, el sistema queda sobredeterminado, así que se resolvió por búsqueda
exhaustiva sobre las tres incógnitas — (expansión de la entrada de paleta) × (multiplicador
y redondeo del combinador) × (coeficiente y redondeo del blender) — contra los 16 niveles.
**Solución única: `('rep', 254, 128, 31, 0)`.** Cada componente tiene además su oráculo
independiente en parallel-rdp:

### 1. `LOD_FRACTION` sin mipmap vale `0xff`, no `0x100`

Los GRB enrutan `TEXEL0_ALPHA` por `LOD_FRAC` hacia `COMBINED_ALPHA` y luego escalan el
texel por él. Devolvíamos `0x100` (= paso directo). En HW, con `max_level == 0`, la unidad de
LOD siempre reporta "distant": en `compute_lod_2cycle` (parallel-rdp `shaders/texture.h`) la
rama de magnificación toma `distant = max_level == 0` y la de mipmap `distant = mip_base >=
max_level`, y ambas fijan `lod_frac = 0xff` cuando no hay SHARPEN ni DETAIL. El puerto del
multiplicador es de 9 bits, así que `0xff` es un valor positivo normal ahí: `x*LOD_FRAC =
(x*0xff + 0x80) >> 8`, que para `x = 0xff` da **254**, no 255. Ese es el 254/256 que se ve en
la captura.

### 2. La entrada de TLUT SÍ se replica (revierte el punto 2 de la sección anterior)

La expansión con relleno de ceros que se dedujo ayer de los GRB era una **compensación** del
escalón que en realidad metía el `0x100`: al pasar el texel entero, la única forma de bajar
el nivel era truncar la paleta. Con `LOD_FRAC = 0xff` en su sitio, el ajuste exhaustivo
descarta el relleno de ceros para *cualquier* par de escalas y deja sólo la replicación
(`v<<3 | v>>2`), que es lo que ya decían los otros dos oráculos (`EMU/*/PPU/*Tile8x8`,
100% de 230400 muestras) y lo que hace parallel-rdp en las dos rutas de lectura.

**Ojo, son rutas distintas**: el *color de memoria* del blender (punto 4 de ayer) sigue
rellenando con ceros — `decode_memory_color` hace `rgb & 0xf8` — y no se ha tocado.

### 3. El blender no redondea hacia arriba su coeficiente P (revierte el punto 3 anterior)

`a0 += (a0+1)>>8` desaparece. El blender trabaja en 5 bits: `a0 >>= 3; a1 >>= 3; blended =
rgb0*a0 + rgb1*(a1+1); rgb0 = blended >> 5` (parallel-rdp `shaders/blender.h`). El `+1` es
**sólo** del término de memoria, y es justo lo que permite que un pase aditivo con `B = ONE`
arrastre el framebuffer intacto mientras el color entrante pierde su 1/32. Un alfa de `0xff`
pesa 31/32, no 32/32.

### Resultado

Los tres decoders GRB pasan a **100.00 % exacto** (todos los canales, todos los píxeles).

### El otro hallazgo: 35 PNG de referencia de krom están estirados verticalmente

Con las tres correcciones puestas, `GRB12Decode` daba 18.16 — pero el diff era un patrón de
franjas periódico, no ruido. La referencia tiene **líneas de barrido duplicadas byte a byte**
en las filas 20/100/180: es una captura de **237 líneas activas** reescalada a 240 con vecino
más próximo. Afecta a toda una familia de assets: los decoders GRB12/15/24, I4 e I8, el par
de Mandelbrot, `RDPModeInput`, las traducciones de Kira, el par DCT multi-bloque y
`Devo-TimeOutForFun` — 35 PNG, con tres pitches distintos (`20/100/180`, `40/120/200`,
`1/101/141`, este último de capturas de 238 líneas).

`compare()` lo deshace: quita de la **referencia** las filas duplicadas y recorta el mismo
número de filas por abajo de nuestro volcado. Sólo mira el asset — las posiciones salen del
PNG, nunca de nuestra salida — así que no puede maquillar un render incorrecto. Guardas: como
mucho 4 filas duplicadas (una imagen realmente plana tiene cientos y se deja en paz) y paso
constante entre ellas (un artefacto de reescalado es periódico; el contenido repetido de
verdad, no). Se anota `REF-DEDUP n` en el TSV.

### Impacto en el barrido (371 ROMs, interp)

mean_exact **87.54 → 88.87**, perfectos 143 → **148**, broken(<50%) 37 → **32**.

| test | antes | ahora |
|---|---|---|
| `Video/GRB12Decode` | 19.11 | **100.00** |
| `Video/GRB15Decode` | 11.08 | **100.00** |
| `Video/GRB24Decode` CPU y RDP | 11.07 | **100.00** |
| `CP1/Fractal/Mandelbrot 320x240` Double / Single | 84.26 / 84.25 | **100.00 / 99.72** |
| `Compress/DCT/QuantizationMultiBlockGFX8BIT` | 32.17 | **88.02** |
| `RDP/16BPP/SetPrimColor` | 98.03 | **100.00** |
| `RDP/32BPP/.../Texture{Rectangle,Triangle}*` (×18) | 89.1-93.5 | **89.8-96.2** |
| `EMU/GameBoy/PPU/2BPPTile8x8` | 63.51 | **66.93** (vuelve al valor previo a ayer) |

### Las 6 "regresiones" son en realidad una convergencia

Los seis tests que bajan son los de textura de 4 bits en 16BPP (`I4`, `IA4`, `TLUT RGBA4`,
rect y triángulo). Bajan al **mismo número exacto** al que suben sus gemelos de 32BPP:

| par | 16BPP | 32BPP |
|---|---|---|
| `I4` | 94.91 → **93.96** | 89.11 → **93.96** |
| `IA4` | 94.97 → **94.21** | 89.15 → **94.25** |
| `TLUT RGBA4` | 92.52 → **89.73** | 89.15 → **89.77** |

Antes el resultado dependía de la profundidad del color-image, que para el camino de textura
es irrelevante; ahora no. Lo que queda es **un solo residual compartido** en el manejo de
texturas de 4 bits (~6 % de píxeles), que es el siguiente frente concreto en vez de dos
síntomas distintos.

### Invariantes tras el cambio
| Puerta | resultado |
|---|---|
| systemtest interp | 0/3721 · 0/2 · 0/6 (16 s) |
| systemtest JIT | 0/3721 · 0/2 · 0/6 (25 s) |
| SM64 300M lockstep vs threaded | `cbf8aa761b92adab89ddde949b6ff24b` MATCH |
| krom 371 (interp) | mean_exact **88.87**, mean_close 92.18, 148 perfectos |

## 2026-08-19 — Velocidad: el emulador ya iba a tiempo real, pero con los DEFAULTS apagados

Contexto: la orden era "revisar el interprete y luego el dynarec; no puede ir peor que
ares". Antes de tocar nada hacia falta **medir bien**, y la metrica que se estaba usando
(Mips) es falsa en modo threaded: si la CPU va mas rapida, gasta mas instrucciones
girando en el spin-wait del RCP, asi que el numero sube cuando el emulador empeora.

### Puerta nueva: `bench` (trabajo guest FIJO, reloj de pared)

```
python scripts/validate.py bench --mode <modo> [--bench-flips 600] [--bench-runs 3]
sh scripts/gate_all.sh     # las cinco modalidades + krom en un solo log
```

600 campos VI de SM64 = 10 s de video guest. i7-870, `build/` (SoftRDP):

| Modo | 600 campos | % tiempo real |
|---|---|---|
| interp (lockstep) | 78.2 s | 12.8 % |
| jit (lockstep) | 73.7 s | 13.6 % |
| threaded (CPU interp) | 44.7 s | 22.4 % |
| **threaded-jit** | **10.1 s** | **99.1 %** |

> **ESTA TABLA ES FALSA — no se reproduce.** Se midio antes de que `bench` exigiera la
> linea `[frames] N buffer swaps`: las corridas cortaban por el tope de instrucciones sin
> haber pintado los 600 campos, asi que lo cronometrado no era el mismo trabajo guest en
> todos los modos. Numeros honestos (mismo host, 600 campos verificados) en la entrada
> **2026-08-19 — DPC_CURRENT** de mas abajo.

O sea: el binario ya corria SM64 a tiempo real, pero `KESTREL_THREADS` y `KESTREL_JIT`
estaban en OFF por defecto. Quien lanzaba `kestrel64.exe rom.z64` a secas se llevaba el
12.8 % — 7.7x mas lento que el MISMO binario bien configurado. Esa era la comparacion
perdida contra ares, no el interprete.

**Arreglo**: los dos conmutadores van ON por defecto y pasan a leer VALOR, no presencia
(`envFlag(name, def)` en `core/types.hpp`): `KESTREL_JIT=0` / `KESTREL_THREADS=0` apagan.
`scripts/validate.py` pone el valor explicito en `MODES`, asi que el modo `interp` sigue
siendo el oraculo puro y las cinco modalidades siguen comparandose entre si.

### Coste por instruccion quitado del RSP (el nuevo palo largo)

Con la CPU al ~100 % de velocidad N64, el que manda es el RSP LLE (`occupancy rsp ~62 %`,
`rsp 33.6 Mips` emulados). Perfilando el HILO DEL RSP (`KESTREL_HOSTPROF_WHO=rsp`, nuevo):
`Rsp::step` 73.3 %, `Rsp::execCop2` 26.7 %, con los buckets muy repartidos = coste de
despacho, no una operacion cara concreta. Cuatro costes tontos, cero semantica tocada:

1. `R128::operator()(e)` (modificador de broadcast) construia su tabla de mascaras
   `pshufb` en un `static` LOCAL con constructor → comprobacion de guarda de
   inicializacion atomica **en cada op COP2**. Ahora `constexpr` a nivel de fichero.
2. `imword()` montaba la instruccion con cuatro lecturas de byte. El PC del RSP siempre
   esta alineado (avanza de 4 en 4, `take()` enmascara con `0xffc`) → carga de 32 bits +
   `bswap`, dejando el camino byte a byte para lecturas no alineadas (depuracion).
3. LQV/SQV con `e == 0` y direccion multiplo de 16 — el caso que usan los microcodigos de
   graficos para traer/soltar un vertice entero — pasan de 16 accesos de byte a una
   carga/almacen de 128 bits + `pshufb` que intercambia los bytes de cada banda de 16
   bits (DMEM es big-endian dentro de la banda; `el[]` es un u16 del host). Mismos bytes,
   mismo orden, bajo el mismo interruptor `KESTREL_NORSPSSE` para poder bisecar.
4. Los 4 KB de contadores del muestreador (`profPc[1024]`) vivian DENTRO del struct entre
   los GPR escalares y los registros vectoriales, calientes los dos. Movidos al final.

En el rasterizador, el mismo patron: `KESTREL_NOBLEND` / `KESTREL_NOAA` /
`KESTREL_NOFILTER` eran `static` locales dentro de funciones **por pixel**. Subidos a
ambito de fichero (inicializacion antes de `main`, sin guarda).

`rsp 33.6 → 36.5 Mips` emulados (+9 %).

### Ademas
- **Bug real**: `KESTREL_MAXFLIPS` no paraba nada. `cpu.maxInsn` solo se mira dentro de la
  guarda de depuracion del interprete, y esa guarda se calcula UNA vez al parsear el
  entorno; ponerlo tarde dejaba el corte inerte y el aviso `[frames] ... stopping` salia en
  cada campo para siempre. Se llama a `cpu.refreshDebugArmed()` al armarlo. Quedaba tapado
  porque el gate pasaba ademas `KESTREL_MAXINSN`.
- **Contador de ciclos del RSP** (`Rsp::cyclesRun`, publicado una vez por `step()`, no por
  instruccion): en modo threaded nadie los contaba y el heartbeat decia "RSP 0.0 %" justo
  cuando el RSP era el cuello de botella. El heartbeat ahora imprime `rsp N Mips busy`.
- **hostprof por hilo**: `KESTREL_HOSTPROF_WHO=cpu|rsp|rdp` elige a quien muestrear (antes
  solo la CPU). Aviso: muestrear a 1 ms suspende el hilo y **falsea el tiempo de pared** —
  medir velocidad con `bench`, nunca con hostprof activo.

Validado: systemtest 0/3721·0/2·0/6 y sm64 md5 en las cinco modalidades, krom 371/371 sin
regresion (mean_exact 88.87 / mean_close 92.18).

## 2026-08-19 — DPC_CURRENT era mentira: el cuelgue de modo threaded, resuelto

### Sintoma

Modo `threaded` (RCP en hilos, CPU interprete) se colgaba en SM64 pasados unos cientos de
campos. Bajo el MCP el estado del cuelgue era inconfundible:

- `emu_status`: fps 0, `rdpBusyPct` 0, `rspBusyPct` 0, `cpuWaitPct` 0, insns subiendo.
- `cpu_registers`: `Status=0xFF03` (IE=1 **y EXL=1** → interrupciones bloqueadas),
  `Cause=0x80008414` (BD=1, IP2+IP7 pendientes, **ExcCode=5 = AdES**), `pc=0x80246dbc`.
- `profile_cpu`: 66 % en el fisico `0x360`, 33 % en `0x45a0` — o sea, la CPU giraba
  DENTRO del area de vectores de excepcion.
- `read_memory rdram 0x180` (crudo **y** `coherent=1`): todo ceros. El manejador general
  de libultra no estaba. Cualquier excepcion caia por `nop`s hasta basura, tomaba AdES,
  re-vectorizaba y volvia a empezar: bucle infinito con EXL=1.

En una corrida sana `0x180` contiene `3c1a8032 275a7650 03400008 00000000`
(`lui k0,0x8032; addiu k0,k0,0x7650; jr k0`). Alguien lo borraba en marcha.

### Quien lo borraba

Sonda nueva en `SoftRdp` (se queda): si un `SET_COLOR_IMAGE` apunta por debajo de `0x400`
se avisa con el comando crudo y la posicion del FIFO, y al cambiar de imagen se dice
cuantos pixeles se llegaron a escribir ahi. Ningun juego pinta encima de los vectores.
(Ojo con el umbral: SM64 SI pone su **z-buffer en 0x80000400**, por eso el limite es
`0x400` y no "el primer mega": el borrado de z con `ci=0x400` es legitimo y son 320x224
pixeles por campo.)

En la corrida colgada aparecian comandos IMPOSIBLES en el FIFO:

```
[rdp!] SET_COLOR_IMAGE bajo: addr=000000 cmd=ffff03e000000000 fifo=0022bd00
[rdp!] SET_COLOR_IMAGE bajo: addr=0003c0 cmd=ffff1c20000003c0 fifo=0022cc20
[rdp!]   ...se escribieron 84 pixeles con el CI bajo
```

`fmt/size/width` basura y direccion 0 / 0x3c0 → 84 pixeles escritos justo encima de
`0x180`. El RDP estaba rasterizando **basura**, y esa basura mataba el kernel del juego.

Biseccion con los conmutadores de diagnostico: `KESTREL_RSPINLINE=1` (RSP en el hilo CPU)
o `KESTREL_RDPINLINE=1` (RDP en el hilo CPU) → cero comandos basura, 300 campos limpios.
Hacia falta que los DOS trabajadores corrieran a la vez.

### Raiz: CURRENT es el puntero del que LEE, no del que ESCRIBE

En `mmioWrite` de `DPC_END` estaba esto:

```cpp
u32 cur = rcp.dpc_current;
rcp.dpc_current = rcp.dpc_end;   // "CURRENT tracks END immediately (CPU view)"
```

Es decir: al dar la patada, CURRENT saltaba a END **antes de que el RDP hubiera leido
nada**. En lockstep da igual (el trabajo se ejecuta entero dentro de esa misma llamada),
pero con el RDP en su hilo es una mentira con consecuencias: en hardware **DPC_CURRENT es
el puntero de lectura del rasterizador**, y el microcodigo grafico (F3DEX2) lo consulta
—via `mfc0` del COP0 del RSP, que enruta a los registros DPC— justamente para saber que
parte del buffer de salida puede reutilizar sin pisar comandos no leidos. Con CURRENT ==
END el microcodigo creia SIEMPRE que el RDP habia drenado todo, daba la vuelta al buffer
y reescribia comandos que el hilo del RDP todavia estaba leyendo → el RDP decodificaba
mitad de un comando viejo y mitad de uno nuevo → `SET_COLOR_IMAGE` basura → pintaba sobre
los vectores. No era una carrera de memoria del emulador: era el **control de flujo
productor/consumidor del hardware** roto por un registro falseado.

### Arreglo (semantica de hardware, no parche)

- `rcp.dpc_current` pasa a `std::atomic<u32>` y lo publica **el rasterizador**: `SoftRdp`
  guarda su puntero de lectura al principio de cada comando (`curOut`), y al terminar el
  trabajo se fija en `end`. El camino paraLLEl-RDP (GPU) lo fija al terminar el FIFO.
- Nuevo `rcp.dpc_submitted` = vista del PRODUCTOR (hasta donde se ha encolado ya). La
  patada de `DPC_END` encola `[dpc_submitted, end)` en vez de `[dpc_current, end)`, que
  ahora puede ir por detras.
- `START_VALID` recarga los dos punteros desde START, como antes.

Efecto: cuando el microcodigo se acerca a la cola del FIFO, ve CURRENT real, espera, y el
RDP no vuelve a leer basura nunca. Es exactamente la contrapresion del hardware.

### Guardas que se quedan (invariantes, coste cero en camino normal)

- `[rdp!] SET_COLOR_IMAGE bajo` — imagen de color por debajo de `0x400` (+ contador de
  pixeles escritos ahi). `KESTREL_CITRACE=1` lista todos los cambios de imagen de color.
- `[dma!] SP->RDRAM / PI->RDRAM sobre vectores` — cualquier DMA con destino < `0x400`.

### Velocidad honesta despues del arreglo (600 campos VI de SM64 = 10 s de video)

Host i7-870, `build/` con SoftRDP, `[frames] 600 buffer swaps` verificado en todas:

| Modo | 600 campos | % tiempo real |
|---|---|---|
| interp (lockstep) | 126.0 s | 7.9 % |
| jit (lockstep) | 126.9 s | 7.9 % |
| threaded (CPU interp) | 58 s | 17.2 % |
| **threaded-jit** | **26.7 s** | **37.5 %** |

ares en este mismo host va al 50-65 % de tiempo real: **kestrel64 sigue por detras**. El
palo largo esta medido y no es el interprete — con la telemetria de ocupacion nueva
(`emu_status.occupancy`, tambien en la linea `[hb]`): **RDP 87 % ocupado**, RSP 67 %,
`cpuWait` ~0. Es decir, el hilo CPU casi nunca espera: manda SoftRDP en el host, y
despues el RSP LLE. Orden de ataque que sale de ahi: (1) backend paraLLEl-RDP en GPU,
(2) vectorizar el COP2 del RSP, (3) intereses menores del JIT.

## 2026-08-20 — SM64 se cuelga tras ~1 min de attract; no es el rasterizador

Sesion con el usuario delante, SM64 en ventana (`KESTREL_VIDEO=1`, threaded+JIT).

**Sintoma.** Arranca, titulo, entra en la demo de attract, y al cabo de ~1-2 minutos
de emulacion la imagen se congela: `fps 0`, `rdpBusyPct 0`, `rspBusyPct 0`, y la CPU
sigue retirando instrucciones a toda velocidad (`cpuPct` 120-385 %). Ese trio
(fps 0 + RCP 0 % + CPU alta) es la firma del cuelgue en `emu_status`.

**Dos capturas del estado, dos finales distintos:**

| backend | pc | pistas |
|---------|----|--------|
| SoftRDP | `0x835CE314` (fuera de RDRAM: 4 MB acaban en `0x803FFFFF`) | ejecutando ceros en el vacio; `EPC=0x80328578`, `ra=0x80328500` (rutina de DMA de PI: `lui t1,0xa460` … `sw t0,8(t1)`), `k0=0xA430000C` (MI_INTR_MASK) → venia del handler de interrupcion |
| paraLLEl-RDP | `0x80246ddc`, `nextPc=EPC=ra=0x80246dd8` | bucle cerrado de dos instrucciones en el arranque de SM64 |

**Lo que descarta el experimento.** La hipotesis era "sera SoftRDP en vez de
paraLLEl-RDP". Se compilo el backend GPU (`-DKESTREL_PRDP=ON`, `build-prdp/`) y se
corrio la misma sesion con `KESTREL_PRDP=1`: **se cuelga igual**. El rasterizador no
es la causa; el fallo esta aguas arriba (CPU / RSP / DMA).

**Lo que si cambia entre backends** (mismo frame, pantalla de titulo):
- paraLLEl-RDP saca el Mario con la textura correcta y filtrada; SoftRDP lo saca
  facetado y con bloques de basura → hay fallos de precision propios del SoftRDP.
- El fondo de mosaicos sale roto en LOS DOS (SoftRDP: tiles repetidos con basura;
  paraLLEl: ruido verde/rosa). Si los dos rasterizadores coinciden en romperlo, los
  comandos/texeles ya llegan corruptos: mirar RSP (F3DEX2) y el camino de textura,
  no el rasterizador.

**Ruido del backend GPU a resolver antes de usarlo en serio:** spam continuo de
`Exhausted LinkedDeviceHost memory` / `Will exceed memory budget` (heap host-visible
de 256 MB de la RX 570) — se esta pidiendo memoria por trabajo sin reciclarla.

**Gotcha de metodo (me ha mordido dos veces).** El video era opt-in silencioso
(`KESTREL_VIDEO`): sin esa variable la ventana ni se abre y parece que el emulador
"sale sin graficos". Cambiado: la ventana viene por defecto en sesion interactiva y
se calla en lote (`--run`), con `KESTREL_VIDEO` / `KESTREL_NOVIDEO` para forzar.

### 2026-08-20 - el cuelgue de SM64 es anterior a los cambios de RSP

Bisect contra un worktree limpio en e6330bc (`k64-head`), misma ROM, mismo modo
(JIT + hilos, que ya vienen por defecto), 2500 campos de tope:

| binario | resultado |
|---|---|
| HEAD e6330bc, sin fichero de save | COLGADO a los 301 s (sin `[frames]`) |
| arbol actual (RSP nuevo), sin save | cuelga igual |
| HEAD, con save presente | 2500 campos limpios, 127 s |

O sea: **el cuelgue no lo introducen los caminos rapidos del RSP**; sale tambien
en HEAD. Lo que cambia el escenario es el fichero `.eep`: sin partida guardada
SM64 recorre otra secuencia de attract y ahi aparece.

Sintomas en el momento del cuelgue (`KESTREL_HANGDOG=<segundos>` fuerza el volcado):

- CPU viva girando en `0x80246dd8`, que es el `while(1)` legitimo del hilo idle
  de `main_func` (tras `osSetThreadPri(NULL,0)`). No es un bucle parasito.
- `EXL` NO esta pegado: el rastro `[exl]` dice que se pone y se limpia cada
  ~750k instrucciones, o sea las interrupciones entran y el handler retorna.
- `mi_intr=08` (VI pendiente), `sp_status=0x203` (HALT|BROKE|SIG2 = tarea
  terminada), `dpc_status=0`, `rspRun=0`: el RCP esta parado y en reposo.
- Conclusion: no hay ningun hilo ejecutable. Alguien espera un mensaje que no
  llega. Falta ver que hilo y en que cola.
- Antes del cuelgue aparecen comandos `SET_COLOR_IMAGE addr=000000` con basura
  (`[rdp!]`), tambien en HEAD: la lista de display llega corrupta al RDP.

Herramientas anadidas para esto: `KESTREL_HANGDOG`, el rastro `[exl]` (quien
pone y quita EXL) y `KESTREL_THREADSCAN`, que localiza las OSThread por su forma
(prioridad, estado, id, punteros a RDRAM, PC guardado) en vez de por la direccion
de `__osRunningThread`, que es distinta en cada juego.

### 2026-08-20 — RESUELTO: el cuelgue de SM64 era una carrera en DPC_CURRENT

**Sintoma**: SM64 en modo threaded (por defecto) se colgaba tras ~1-2k campos. El RDP
recibia un `SET_COLOR_IMAGE` con `addr=000000`, rasterizaba sobre la fisica 0 y borraba
el vector de excepcion 0x80000180; a partir de ahi la CPU saltaba a basura
(`Cause=80000428`, Reserved Instruction en bucle) y ningun hilo del juego volvia a correr.
Reproducible en HEAD e6330bc, o sea anterior al trabajo de RSP.

**Causa raiz**: `DPC_CURRENT` es el puntero de LECTURA del command processor del RDP; en HW
su unico dueno es el propio RDP. En kestrel lo escribian DOS hilos:

- el worker del RDP, publicando su avance real mientras consume el FIFO, y
- **el hilo CPU**, en la escritura de `DPC_END` con `START_VALID`, recargando
  `dpc_current = dpc_start` (memory.cpp caso 0x04).

F3DEX2 usa el buffer de salida del RDP como **FIFO circular** y hace flow-control leyendo
DPC_CURRENT (medido: ~2.8M lecturas por run) antes de DMAear comandos nuevos encima. Con el
worker por detras, la recarga desde la CPU y el `store` posterior del worker (direcciones
altas del span viejo) se pisaban: el microcodigo leia un CURRENT **adelantado**, concluia que
el RDP ya habia consumido la cabeza del buffer y la reescribia con comandos nuevos por debajo
del rasterizador. De ahi los comandos rotos (`ffff000300000000`, mitad alta con datos, mitad
baja a cero) y el `SET_COLOR_IMAGE addr=0`.

**Fix** (semantica HW, no parche): la recarga CURRENT<-START sigue ocurriendo en el kick y la
CPU la ve al momento (systemtest la exige: "RDP STATUS: Flags during a run", "RDP START & END
REG (masking)" — y ocurre incluso congelado, sin rasterizar nada), pero **solo se publica desde
el hilo CPU si el rasterizador esta parado**. Si el worker sigue consumiendo un span anterior,
la recarga viaja con el trabajo — `rdpRunJob` publica `dpc_current = inicio del span` al abrirlo, y desde ahi
solo avanza el consumidor. Como los trabajos se ejecutan en orden FIFO, CURRENT nunca puede
indicar mas consumido de lo que realmente se consumio, que es la invariante que el ucode
necesita. En lockstep el comportamiento es identico (rdpRunJob corre en el hilo CPU).

**Verificacion**: SM64 2500 campos, 3 runs, 118-119 s, `anom=0 hangdog=0` (antes: colgado a
los 300 s con 53 anomalias). Bisect previo que apuntaba al hilo del RDP:
`KESTREL_THREADS=0` limpio, `KESTREL_THREADS=1 KESTREL_RDPINLINE=1` limpio,
`KESTREL_THREADS=1` (worker RDP) roto — consistente con la carrera.

**Herramientas nuevas** (se quedan): `KESTREL_HANGDOG=<s>` (vigilante de cuelgue con volcado),
traza `[exl]` de quien puso/quito EXL, `KESTREL_THREADSCAN` (escaneo de OSThread agnostico de
juego, no depende de simbolos de PD), aviso `[rdp!]` de SET_COLOR_IMAGE bajo con volcado del
vecindario del FIFO, contador `dpcCurReads` (lecturas de DPC_CURRENT por el ucode) y
`KESTREL_RDPINLINE=1` (rasterizar en el hilo CPU aun en modo threaded, para bisecar carreras).

## 2026-08-20 — El cuelgue de SM64 RESUELTO: contrapresion del FIFO del RDP

Cerrado el cuelgue de la seccion anterior. **No era el rasterizador ni la ruta VI: era una
interrupcion DP de mas.** Detalle completo en `docs/RDP-FIFO-BACKPRESSURE.md`; resumen:

El command processor del HW tiene **un solo puntero de lectura**. Instalar un buffer nuevo
(`DPC_START` fresco + `DPC_END`) recarga `CURRENT` desde `START`, y lo que quedara sin leer
del span anterior deja de existir. En HW eso no pierde nada porque el RDP consume el FIFO en
tiempo real. Con el RDP en su propio hilo deja de ser automatico: el productor se adelanta
frames enteros (828 trabajos encolados medidos), y tras recargar `START` el worker rasteriza
DESPUES esos spans viejos y retira sus `SYNC_FULL` -> DP de mas. El kernel del juego la
atiende sin tarea viva: `handle_dp_complete` de SM64 deref de `sCurrentDisplaySPTask == NULL`
-> TLBL con `BadVAddr=0x40` -> libultra deja el hilo de interrupciones (pri 100) en
`OS_STATE_STOPPED` con `OS_FLAG_FAULT` -> todos los demas hilos esperan para siempre en
`osRecvMesg` y la CPU cae al bucle ocioso `0x80246dd8`.

**Arreglo**: en modo threaded el escritor de `DPC_END` hace `rdpDrain()` antes de instalar un
START nuevo — el productor no puede instalar buffer nuevo con el anterior en vuelo. Dos
efectos colaterales buscados: `DPC_CURRENT` queda honesto para el control de flujo del anillo
de F3DEX2, y la cola de trabajos queda acotada a un frame.

parallel-rdp solo lo **destapo** (espera la timeline de la GPU en cada SYNC_FULL, asi que se
retrasa lo bastante como para que el productor le saque una vuelta). SoftRDP tiene el mismo
agujero pero nunca encola tan hondo.

Verificado: `gate_all` verde en los cinco modos (4 min 53 s), 300 flips en interp y 400 en JIT
bajo `KESTREL_PRDP=1` sin cuelgue, y `bench` threaded-jit 8.80 s / 200 campos frente a 9.84 s
de antes del cambio (o sea, la contrapresion **no** cuesta velocidad).

Ademas: el camino PRDP de `rdpRunJob` no hacia la contabilidad que si hace el de SoftRDP al
retirar un SYNC_FULL (limpiar `START_GCLK|PIPE_BUSY`, incrementar `dpSyncs`).

**Herramientas nuevas** (se quedan): `KESTREL_FAULTSTOP=1` (parar en un fallo del guest con el
anillo de eventos del RCP intacto — la ventana exacta del bug), `KESTREL_WATCHP=<fis>`
(watchpoint de escritura por linea de 16 B enganchado en la **D-cache**; el `KESTREL_WATCH` de
bus no ve las escrituras cacheables de la CPU), `KESTREL_EVDUMP=<n>`, `KESTREL_DPSYNCLOG=1`
(direccion de cada SYNC_FULL retirado, en SoftRDP y en PRDP) y las etiquetas de evento
`rdpst` / `rdpint`.

**Tambien cerrado**: el fondo arcoiris de SM64 bajo parallel-rdp era **orden de bytes de
RDRAM**, no la ruta VI. parallel-rdp asume el almacenamiento de ares (palabras nativas, byte
en `addr^3`) y kestrel guarda big-endian del guest. Convenio y regeneracion del banco SPIR-V
en `docs/parallel-rdp-integration.md`. Arbol vendorizado en `third_party/parallel-rdp/`.

**Pendiente conocido**: SoftRDP corrompe el titulo de SM64 a los 250 flips (80.0 % de pixeles
distintos frente a parallel-rdp, delta medio 28.69); la puerta de 60 flips no lo ve.
parallel-rdp escupe `[WARN]: Exhausted LinkedDeviceHost memory` en corridas largas.

## 2026-08-20 (bis) — Un solo reloj de video: el tick del VI contaba 263 veces menos campos de los que había

Detalle completo en `docs/VI-CLOCK.md`. Resumen de lo medido:

**El bug.** `Memory::viTick()` avanzaba `vi_current` **+2 medias-líneas por llamada** y el
bucle del sistema lo llamaba **una vez por cada lote de un campo entero**. Un campo real
(524 medias-líneas) necesitaba 263 llamadas → 263 campos de tiempo emulado por campo de
verdad. Consecuencias medidas:

- `viFields` = **2** en 421M instrucciones (deberían ser ~538).
- `vrdp::frameBegin()` cuelga del cierre de campo → el contexto por-cuadro de parallel-rdp
  casi nunca rotaba. Es el origen de los WARN `Exhausted LinkedDeviceHost memory`.
- La interrupción del VI se levantaba por NIVEL (`vi_current >= vi_intr`), o sea en casi
  todas las llamadas.
- Y había **dos** relojes de video: la lectura de `VI_V_CURRENT` usaba `93.75e6/(60*total)`
  (1.5625M instrucciones por campo) y el tick usaba otra cuenta (750k). Un juego que
  mezclara interrupción y sondeo veía dos relojes distintos.

**El arreglo.** Un solo sitio define el tiempo (`Clocks`): `fieldInsns()` = 782k
instrucciones por campo (46.875 Mops/s ÷ 59.94; CPI 2 sale de que `Count` avanza +1 por
instrucción y en el VR4300 corre a medio reloj). El bucle corre `tickInsns()` (1/16 de
campo, `KESTREL_VITICKS`) y `viTick(retired)` deriva todo del contador absoluto:
interrupción por **cruce** de `VI_INTR`, cierre de campo por cruce de múltiplo de campo.
`Rcp::viHalflines()` unifica las cuatro copias de "medias-líneas por campo" que había
(máscaras `0x3ff`/`0x3fe` y defaults 524/525 mezclados).

**Lo que se probó y NO entró: latchear `VI_ORIGIN`.** Es lo que hace el HW (una escritura
en el vblank se ve en el campo siguiente), pero mueve el punto de captura del volcado al
buffer VIEJO, que un juego de doble buffer ya está reescribiendo: en threaded eso depende
del reloj de pared y los cinco modos dejaron de coincidir en SM64 (tres md5 distintos).
Revertido entero; el intercambio se sigue contando en la escritura de `VI_ORIGIN`.

**El gate de krom capturaba escenas a medias.** `--maxsyncs` pasa de 1 a 2: un `SYNC_FULL`
es el final de UNA display list y hay ROMs que dibujan la escena con dos. `RDPTest/CPU` y
`RDPTest/RSP` marcaban 88.60 con uno y **99.65 exacto** con dos, con el mismo render.

**Resultado.** systemtest 0/3721·0/2·0/6 y SM64 `466282775dbd0ac084946558a1c30771` en los
cinco modos (lockstep == threaded). krom 371/371, `mean_exact` 88.83 → **88.71**, con tres
movimientos, los tres de fase de animación y ninguno de render: `VIScrollingBGDMA32BPP`
100.00 → 0.21 (scrollea +1 línea por campo moviendo `VI_ORIGIN`; antes el emulador no
avanzaba campos y capturaba justo el cuadro 0 de la referencia) y `CubeFillTriangle`
{16,32}BPP 59.54 → 53.10 (cubo que rota +1° por cuadro).

**Pendiente que esto destapa.** El core lleva DOS conversiones instrucción→ciclo: `Count`
y el reloj de video usan CPI 2, mientras el interleave CPU↔RSP (lockstep y el regulador
threaded) usa CPI 1. Al RSP le estamos dando la mitad del tiempo relativo que implica
nuestro propio `Count`. Los dos tienen que salir de `Clocks::cyclesPerInsn`; se trata
aparte porque mueve el orden de eventos CPU/RSP (md5, krom, tests de timing).

## 2026-08-20 (ter) — Un solo modelo de CPI: al RSP le faltaba la mitad de su tiempo

Cerrado el pendiente de la entrada anterior. El error estaba a la vista en el comentario
del interleave: *"interleave the RSP at ~2/3 the CPU rate (62.5 MHz vs 93.75 MHz)"*. Eso
compara **reloj contra reloj**, pero la unidad con la que avanza `stepCpu` son
**instrucciones retiradas**, y una retirada cuesta dos ciclos de CPU (CPI 2, que es lo que
fija COP0 `Count`). El RSP retira una instrucción por ciclo suyo, así que el reparto bueno
es `62.5 / (93.75/2) = 4/3` instrucciones de RSP por instrucción de CPU, y `3/4` para el
regulador de threaded. Los dos sitios estaban exactamente al **doble** de lo debido.

Ahora el ratio sale una sola vez de `Clocks::rspInsnsPerCpuInsn()`. De ahí se derivan el
acumulador entero del interleave de Lockstep (`rspStepNum/rspStepDen` en 16.16 — enteros
para que el reparto no dependa del redondeo de un `double`) y la fracción del regulador
(`Memory::paceCpuNum/paceCpuDen`). Tocar los relojes o el CPI en `Clocks` mueve los tres.

Lo que NO cambia: el CPI real de un VR4300 es ~1.5 y depende de la instrucción y de los
fallos de caché; `cyclesPerInsn` sigue siendo un promedio fijo. La ganancia de este cambio
no es ese modelo, es que cuando se haga habrá **un solo sitio** que tocar.

## 2026-08-23 — parallel-rdp pasa a ser la vía principal; dos divergencias entendidas

Orden del usuario: *"SoftRDP no me interesa. Vamos con parallel-rdp y en un futuro igual
creamos en nuestro propio sobre Vulkan, OpenGL o Directx11-12, lo que entregue mas
rendimiento"*. Con la barrida completa hecha, el backend GPU ya gana en el material de
krom, y a partir de aquí es la vía de trabajo. Lo que faltaba para poder decirlo con
datos, y lo que salió al medirlo:

**El volcado de framebuffer estaba corriendo una carrera contra el RDP.** `KESTREL_FBDUMP`
se atendía sin drenar el RCP. Con el dynarec la CPU quema un tope de 8M instrucciones en
~20 ms; el hilo del RDP puede no haber consumido aún la lista, y parallel-rdp encima paga
arranque de GPU. La matriz PRDP×JIT sobre `FillRectangle32BPP` lo enseñaba: tres celdas
con 3 colores y la celda PRDP+JIT con **uno solo** (todo negro). Subir el tope a 300M hacía
aparecer la imagen correcta, que es lo que identificó el problema como temporal y no de
render. `mem->rdpDrain()` antes del volcado; las cuatro celdas coinciden. Esto contaminaba
cualquier medida de PRDP hecha con JIT.

**Barrida krom con `KESTREL_PRDP=1`** (baseline congelado en `docs/baselines/krom-prdp.tsv`,
modo `prdp`): 371/371, `mean_exact=88.95`, **perfect=178** frente a 88.71 / perfect=148 de
SoftRDP. Ganancias grandes donde SoftRDP arrastraba deuda: CombinerOverflow 42.71→100,
CombinerLongTailConstants 71.13→100, AlphaCoverage 72.28→100, los 12 `TexturesMaskShiftMirror`
→100, TextureCoordinates 87.39→100, todos los `LoadTLUT_*` →100.

**Dos divergencias, ninguna arreglable sin mentir sobre el hardware** (ambas con su
análisis completo en `docs/parallel-rdp-integration.md`):

1. *GRB12/15/24 Decode 100→0.* Esos decodificadores hacen `TEXEL0 * COMBINED_ALPHA` en
   modo 1-ciclo. `COMBINED` en el RDP es un **registro de pipeline**: conserva el resultado
   del píxel anterior, que en régimen estacionario vale 1.0. SoftRDP lo modela; parallel-rdp
   mete un cero literal, porque sombrea píxeles en paralelo en la GPU y ahí "el píxel
   anterior" no existe. 3 ROMs de 371, todas sintéticas.
2. *Cuatro casos "RDP STATUS" de systemtest en `prdp-jit`.* No es que se olviden los flags:
   es que SYNC_FULL bloquea ~1.6 ms de host esperando a la GPU, y en threaded la CPU sigue
   corriendo y agota el presupuesto de ciclos de guest del test. El mismo backend en
   lockstep pasa 0/3721, que es la confirmación limpia.

**Infra nueva**: modos `prdp` / `prdp-jit` en `validate.py` (el exe se elige con
`KESTREL_EXE`), `scripts/gate_prdp.sh`, baselines krom+sm64 propias del backend GPU
(sm64 PRDP = `b5521b24d8fc280fbf102df22d7d30cb`, idéntico en lockstep y threaded = el
backend es determinista), y `KESTREL_DPSYNCLOG=1` ahora también registra cada `[dpkick]`
encolado, no solo los SYNC_FULL retirados — que es lo que permitió ver que el segundo kick
del test no llegaba a existir. `gate_all.sh` sigue siendo el oráculo determinista sobre
SoftRDP y no depende de que haya GPU.

**Siguiente en el RDP**: dejar de bloquear en SYNC_FULL (retirar la interrupción DP ya y
sincronizar sólo cuando alguien lea esos píxeles) — mejora de latencia real, y de paso se
lleva por delante la divergencia (2).

## 2026-08-27 — Lanzador grafico, overclock, ventana y mando configurable

Primera capa de producto por encima del nucleo. Detalle completo en `docs/LAUNCHER.md`.

- **Lanzador** (`tools/launcher/`): servidor HTTP de biblioteca estandar de Python + interfaz
  web servida en ventana `--app` de Edge/Chrome. Cero dependencias nuevas en el build de C++.
  `options.py` es el esquema unico: **101 opciones en 9 categorias** que cubren ~110 banderas
  `KESTREL_*`; anadir una bandera al emulador = anadir una fila.
- **Biblioteca de ROMs** con cinco vistas (coverflow 3D, filas estilo Netflix, rejilla, rueda
  estilo Hyperspin, tabla), cabecera de cartucho leida de verdad (z64/v64/n64 normalizados) y
  caratulas de libretro-thumbnails con cache local. (NNID resulto ser un identificador de
  cuenta de Wii U / 3DS, no tiene arte de N64.)
- **Overclock** (`src/core/system.cpp`): `KESTREL_OC` global y `KESTREL_OC_CPU/_RSP/_RDRAM`
  por dominio. `System::Clocks` ya tenia los multiplicadores; nadie los escribia. Ancla = el
  campo de video, asi que overclock = mas trabajo de guest por campo con los campos saliendo
  a 59.94 Hz.
- **Ventana** (`src/video/present.cpp`): `KESTREL_WINSCALE=N`, `KESTREL_WINSIZE=WxH`,
  `KESTREL_FULLSCREEN=1`. El framebuffer del guest sigue siendo 320x240; esto solo decide la
  resolucion de presentacion. Pantalla completa toma el modo del monitor, no lo cambia.
- **Mando configurable**: `KESTREL_PAD1=<fichero>` con una linea por control
  (`<CONTROL> <TECLA> <BOTON_GAMEPAD>`). **Sin fichero el camino es el de siempre, instruccion
  por instruccion.** Los gatillos son ejes, no botones, y se resuelven aparte. En la interfaz
  es un mando de N64 en CSS 3D con un cajon por boton y captura en vivo de tecla y de gamepad.

Trampa evitada al cablearlo: la mayoria de las banderas `KESTREL_*` se activan por
**presencia** (`getenv() != nullptr`), asi que exportar `X=0` las ENCENDERIA. Solo unas pocas
leen el valor (`KESTREL_JIT`, `THREADS`, `RSPJIT`, `AUDIO`, `FULLSCREEN`, `PRDP`, `THROTTLE`).
La primera version del traductor exportaba ~60 banderas como `"0"` y habria encendido el
trazado entero en cada arranque.

Puertas verdes en las seis modalidades + PRDP, `regress=0`.
