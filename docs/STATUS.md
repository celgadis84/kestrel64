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
  fichero `.mpk` por slot. HECHO 2026-08-28 para el mando 1 (ver entrada al final); vive junto
  al ROM, migrar a `pak/<game-id>.mpk` cuando se haga este layout, y falta el resto de mandos.
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
  `options.py` es el esquema unico: **116 opciones en 11 categorias** que cubren ~110 banderas
  `KESTREL_*`; anadir una bandera al emulador = anadir una fila.
- **Biblioteca de ROMs** con seis vistas, tres de ellas en 3D de verdad sobre WebGL propio
  (coverflow, carrusel de anillo al estilo USB Loader GX y pared de cajas; las otras tres son
  filas estilo Netflix, tabla y estante), cabecera de cartucho leida de verdad
  (z64/v64/n64 normalizados) y
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

### Depurador en la ventana de telemetria

La ventana gana pestana **Depurador** sobre las ordenes que ya tenia el servidor
(`cpu.disasm`, `cpu.step`, `cpu.run_until`, `cpu.bp.*`, `mem.read`): desensamblado con el PC
resaltado, puntos de ruptura pulsando la linea, paso de 1/10/1000, "correr hasta" con tope de
tiempo propio, y visor de memoria por region con lectura coherente por la cache de datos.

Bug real encontrado al probarlo contra SM64: `pc` y los registros de 64 bits llegaban al
navegador por `JSON.parse` como `double` y perdian los **bits bajos** (el PC
`0xffffffff80246dd8` se convertia en otra direccion, y un punto de ruptura puesto ahi habria
caido en otro sitio). Arreglado en el puente, que aun tiene el entero exacto: todo entero que
no cabe en 2^53 sale como cadena hexadecimal. Nada de esto toca el binario del emulador.

## 2026-08-28 — Savestates + lanzador congelado en un solo .exe

**Estados guardados** (`src/core/savestate.cpp`, detalle en `docs/SAVESTATES.md`). Un
unico visitante bidireccional: la lista de campos se escribe una vez y sirve para guardar
y para cargar, asi que las dos direcciones no pueden desincronizarse. Sale de la maquina
con el RCP quieto (`rdpDrain` + `rspAwaitIdle` + terminar la tarea RSP en lockstep), que
es lo que hace que un fichero guardado en modo hilos cargue en lockstep y viceversa.
Se guardan tambien las caches de la CPU — el VR4300 no tiene coherencia, una linea sucia
de la D-cache existe SOLO ahi — y el medio de guardado del cartucho.

- Teclas F5 (guardar) / F7 (cargar) / F6 (cambiar ranura, 0-9), y ordenes de telemetria
  `state.save` / `state.load`. Ni la ventana ni el hilo de telemetria guardan: dejan la
  ranura en un buzon (`System::stateSaveReq/stateLoadReq`) y el bucle de la CPU lo
  atiende bajo `coreMutex`, tambien en pausa.
- `test/state_test.py`: guarda, avanza, carga y compara CPU + RCP + RSP (con `vpr`) +
  cuatro trozos de RDRAM + el framebuffer; luego relanza en lockstep un estado guardado
  en hilos. ALL PASS con interprete y con JIT.
- De paso, `rcp.regs` publica `vi.flips`/`vi.fields`/`vi.syncs`, que antes no salian.

**`kestrel64-gui.exe`** (`scripts/gui.sh`): PyInstaller congela el lanzador con el
interprete de Python y `web/` dentro, 8.4 MB, sin exigir Python instalado. El lanzador
sabe donde vive (`FROZEN`): recursos en `sys._MEIPASS`, emulador AL LADO del `.exe` en vez
de `build*/`, y perfil/mando/caratulas en `%LOCALAPPDATA%\kestrel64` porque Archivos de
programa no es escribible. `dist.sh` lo mete en `dist/` y deja `kestrel64.build` con el
backend con el que se compilo el emulador (desde fuera del binario no se ve). El
instalador ya apunta el icono del escritorio y del menu al lanzador; la asociacion de ROM
sigue yendo a `kestrel64.exe --run "%1"`. Multi-SO queda para la ultima fase.

## 2026-08-28 — Controller Pak (Memory Pak) por joybus, `.mpk` al lado del ROM

El pak es del **mando**, no del cartucho: 32 KiB de SRAM con pila dentro de la ranura del
mando, que se leen y escriben por joybus en bloques de 32 bytes. Va aparte del save de la
cartuchera y existe aunque el juego no tenga bateria, asi que tiene su propio fichero
`.mpk` (convencion mupen/ares) y su propio marcador de sucio.

**Protocolo** (`Memory::pifProcessJoybus`, canal 0). La direccion no viaja cruda: son
`(bloque << 5) | CRC5(bloque)`, con el bloque en unidades de 32 bytes — 11 bits, o sea
64 KiB de espacio de direcciones para 32 KiB de pak. La respuesta lleva ademas un CRC de
datos de 8 bits.

| Orden | tx | rx | Contenido |
|---|---|---|---|
| `0x00` / `0xFF` estado | 1 | 3 | tipo `0x0005` + byte de estado (`CONT_CARD_ON` = 0x01 si hay pak) |
| `0x02` leer  | 3  | 33 | 32 bytes del bloque + CRC de datos |
| `0x03` escribir | 35 | 1 | CRC de datos **de lo que mando el juego** |

Los dos CRC estan portados tal cual de `__osContAddressCrc` / `__osContDataCrc` del SDK
(`libreultra/src/io/crc.c`), incluida la vuelta numero 33 del de datos, que no lee dato: mete
un byte de ceros por la derecha y es lo que deja el resto en su sitio. No es cosmetico —
`__osContRamRead` compara el CRC de CADA bloque, reintenta tres veces y luego va a preguntar
el estado del canal, asi que devolverlo mal equivale a un pak roto.

Detalles que hay que acertar o el SDK se planta:

- **`CONT_CARD_PULL` (0x02) se deja a cero.** Con `CONT_CARD_ON` y `CONT_CARD_PULL` puestos a
  la vez `__osPfsGetStatus` devuelve `PFS_ERR_NEW_PACK` ("pak recien cambiado"), que el juego
  ensena como error. Es un estado de HW real, pero solo tras un cambio en caliente.
- **Sin pak, el CRC de datos sale INVERTIDO.** Es asi como el SDK nota la ausencia antes de ir
  a mirar el estado del canal; devolver el CRC bueno con datos a cero haria que el juego se
  creyera un pak vacio y utilizable. `KESTREL_MEMPAK=0` desenchufa el pak.
- **Por encima de 32 KiB no hay nada.** El bloque 0x400 (byte 0x8000) es la ventana del Rumble
  Pak; un Controller Pak devuelve ceros ahi, que es justo lo que separa un accesorio del otro.

**El pak sale FORMATEADO de fabrica** (`mempakFormat`). Sin eso `osPfsInitPak` lo da por
inservible: la suma del bloque de ID no cuadra, `__osCheckPackId` devuelve `PFS_ERR_ID_FATAL`
y el juego pide formatear en el primer arranque. Se construye el mismo contenido que deja
`osPfsReFormat`: bloque de ID por cuadruplicado en los bloques 1/3/4/6 (con `deviceid` impar
— `__osGetId` lo exige — `banks=1` y las dos sumas de `__osIdCheckSum`), tabla de inodos en el
bloque 8 con su copia de respaldo en el 16 (cinco paginas de sistema a 0, el resto a 3 =
`PFS_PAGE_NOT_USED`, y la suma de `__osSumcalc` en la entrada 0) y directorio a cero.

**El pak entra en el savestate** (`kVersion` 1 → 2): es RAM viva, y rebobinar un estado sin
rebobinarlo dejaria el sistema de ficheros del pak adelantado respecto al juego.

**Test** (`test/save_test.cpp`, seccion nueva). Conduce el camino real — bloque de ordenes en
RDRAM, SI DMA a PIF RAM, `pifProcessJoybus`, SI DMA de vuelta — igual que `__osContRamRead`, y
**recalcula los dos CRC a partir del codigo del SDK en vez de reusar los del emulador**: si
ambos estuvieran mal a la vez el test no valdria nada. Comprueba el formato de fabrica (sumas
del ID en los cuatro bloques, `deviceid` impar, suma de inodos, copia identica), estado con
pak, escritura + relectura + los dos CRC, la ventana del Rumble a ceros, el CRC invertido sin
pak, y el viaje de ida y vuelta al `.mpk`.

De paso, `save_test` y `rsp_test` **volvian a compilar pero no a arrancar**: les faltaban
`src/cpu/cpu.cpp` y `src/telemetry/hostprof.cpp` y el `-march` del binario principal (rsp.cpp
usa SSSE3), y `Memory` ha crecido tanto que tres en la pila revientan el megabyte de Windows
— ahora van al monton. `rsp_test` sigue sin compilar por otra cosa (copia de un `std::atomic`
en su linea 71), anotado y aparte.

## 2026-08-28 — RSP JIT etapa 3: enlace de bloques, y el dynarec que no se usaba en Lockstep

Dos cosas, la segunda encontrada midiendo la primera.

**Enlace de bloques.** El epilogo de cada bloque mira el ya la tabla de 1024 ranuras
en el PC de salida y, si hay bloque vivo que cabe en lo que queda de tanda, salta a
el con una cola (`jmp` tras desmontar el marco, no `call`). La pila queda como a la
entrada, asi que el ultimo de la cadena vuelve al bucle en C con su propio `ret` y la
pila no crece. SM64, 200 intercambios: **7 369 631 -> 1 245 702 entradas al
despachador, -83 %**. Con el RSP como cuello (`--rspbench`): **230,8 vs 225,5 Mips,
+2,4 %**, y eso con bloques de 64 instrucciones, que es el caso peor. Detalle de
diseno —enlace indirecto por tabla (no `jmp rel32`, la IMEM se reescribe con
overlays), PC escrito siempre por el bloque, destino estatico en caida y en J/JAL,
saldo debitado por quien salta— en `docs/RSP-JIT.md`.

**El dynarec del RSP no se ejecutaba en Lockstep, pero se compilaba.** `bench --mode
jit` iba mas rapido con `KESTREL_RSPJIT=0` (22,3 s) que con el dynarec (24,1 s). En
Lockstep el bucle del sistema llama a `memory.rsp.step(1)`: la tanda vale 1 y un
bloque necesita `kMinOps` = 2, asi que ningun bloque llega a ejecutarse — pero cada
PC nuevo si se compilaba entero antes de caer al interprete. Guarda `c >= kMinOps` en
el despachador (no se mira la tabla ni se compila lo que no cabe en la tanda):
**`bench --mode jit` 24,10 -> 22,60 s, -6,2 %**. `threaded-jit` sin cambio.

Interruptores y validacion: `KESTREL_RSPJIT_LINK=0` para bisecar, modo `rspnolink`
anadido a `validate.py` y a `gate_all.sh` (ahora 14 pasos). gate_all 14/14 y
gate_prdp 3/3 con `regress=0` en las dos suites krom y los md5 de sm64 sin cambio
(`466282775dbd0ac084946558a1c30771` SoftRDP, `b5521b24d8fc280fbf102df22d7d30cb` PRDP).

## 2026-09-01 — El presentador recortaba el cuadro: media imagen con parallel-rdp

Bug de VISUALIZACION, no de emulacion: las puertas nunca lo vieron porque el volcado de
framebuffer (`dumpFramebufferBmp`, `KESTREL_FBDUMP`) sale del RDRAM y no pasa por el
presentador. Solo se ve en la ventana.

**Sintoma.** Con `KESTREL_PRDP=1` la ventana ensenaba la mitad izquierda del cuadro.

**Raiz.** `src/video/present.cpp` tenia la imagen fuente de Vulkan creada UNA vez a
320x240 (`kSrcW`/`kSrcH`) y todo lo demas recortado contra ella:

- `width = sw > kSrcW ? kSrcW : sw` en el camino de GPU. parallel-rdp entrega el scanout
  del VI ya a la resolucion de salida — en SM64 son **640x240** — asi que se copiaban las
  320 primeras columnas y se tiraba la mitad derecha.
- El mismo recorte en el camino de RDRAM (SoftRDP): un juego de alta resolucion con
  `VI_WIDTH` 640 perdia igual la mitad derecha.
- La altura estaba clavada a `kSrcH` = 240 en el camino de RDRAM, ignorando `Y_SCALE`.
- `presentFrame` volvia a recortar al subir los pixeles (`y < v.srcH`, `min(w, v.srcW)`).

**Arreglo.** La imagen fuente pasa a ser del tamano EXACTO del cuadro y se rehace cuando ese
tamano cambia (`createSrcImage`/`destroySrcImage`, con `vkQueueWaitIdle` bajo `QueueGuard`
porque puede haber un blit en vuelo). Los tres sitios que recortaban usan ahora una cota de
cordura (`kMaxSrcW/H` = 1024, para que un registro VI a medio escribir no reserve una imagen
absurda) en vez de la resolucion nominal. La altura del camino de RDRAM sale de `Y_SCALE`
sobre las lineas activas del campo (NTSC 240 / PAL 288 segun `V_SYNC`), que es la misma
derivacion que ya usaba el volcado.

Dos cosas que se caen de ahi:

- **El aspecto de presentacion es 4:3 SIEMPRE**, no el del framebuffer. El VI escala su
  ventana activa a la salida de television pase lo que pase la resolucion de origen; sacarlo
  de `srcW/srcH` aplastaba las resoluciones no-4:3 (un framebuffer 320x120 con `Y_SCALE` a la
  mitad salia al doble de ancho).
- **El HUD se escala con el framebuffer**, y con un factor por eje (`sx = w/320`,
  `sy = h/240`): a 640x240 el texto necesita ir al doble de ancho en pixeles de origen para
  salir del mismo tamano en pantalla.

Verificado con `KESTREL_VIDEO_DUMP` (vuelca lo que compone el presentador, no el RDRAM):
antes 320x240 con PRDP, ahora **640x240 con contenido en las dos mitades** (85.9 % / 86.2 %
de pixeles no negros). SoftRDP sigue en 320x240. Puertas verdes en las dos suites.

## 2026-09-01 — La cache de imagenes del RSP JIT estaba mal calibrada (27x menos compilacion)

Empezando la fase de optimizacion se midio primero, con `KESTREL_RSPJIT_STATS=1` sobre SM64
(500 intercambios de buffer): **241162 bloques de RSP compilados** en la corrida y 15862
fallos de imagen contra 1155 aciertos. La cache de imagenes de microcodigo (una tabla de
bloques por imagen de IMEM, con su sombra de 4 KB) existia desde la etapa 2, pero con dos
constantes puestas a ojo: 4 ranuras y umbral 64 trozos de 8 B para estrenar ranura.

Barrido de las dos perillas (detalle y tabla completa en `docs/RSP-JIT.md`):

- 4 ranuras -> el juego se queda sin sitio y parchea la ranura mas parecida (media
  recompilacion por cambio de tarea).
- umbral 64 -> casi toda diferencia se considera "parecida", asi que aunque sobren ranuras
  no se estrenan: SM64 usa solo 6 de 16.
- **16 ranuras + umbral 8 -> 3948 bloques** (repetible +-1 %), 9403 aciertos / 8265 fallos.
- Umbral 4 es PEOR (50389): se estrenan ranuras por diferencias de 32 B y los casi-duplicados
  echan a las plantillas grandes. Mas de 16 ranuras no aporta (con 32, SM64 usa 17 y compila
  los mismos bloques); `kJitWaysMax` sube a 32 solo para poder medir otros microcodigos.

Semantica intacta: la correctitud la da `syncImem` (invalidacion exacta por palabra de 64
bits), no el umbral; la cache solo decide QUE tabla se parchea.

Medida limpia con el heartbeat (SM64, 1200 intercambios, SoftRDP threaded-jit):

| | rsp Mips ocupado | ocupacion rsp | ocupacion rdp | pace |
|---|---|---|---|---|
| antes (4/64) | 144,9 | 69 % | 94 % | 18 % |
| ahora (16/8) | **186,9 (+29 %)** | 69 % | 95 % | **9 %** |

El reloj de `bench` se mueve poco (5,4 s, ~575 % -> ~620 % de tiempo real) porque en SM64 con
SoftRDP **el palo largo es el RDP** (94-95 % de ocupacion) y el hilo del RSP va holgado: lo
que se gana es CPU del anfitrion, que es lo que hace falta en microcodigos pesados (PD) y en
maquinas flojas.

Perillas nuevas: `KESTREL_RSPJIT_WAYS` (por defecto 16) y `KESTREL_RSPJIT_NEWWAY` (8).
Gates: gate_all 14/14 (334 s), gate_prdp 3/3 (297 s), krom regress=0 en los dos, md5 sin
cambio.

De paso queda corregida una nota falsa de `docs/RSP-JIT.md`: decia que los subcodigos
barajados de LWC2/SWC2 "en SM64 no aparecen". El censo `KESTREL_VUSTAT` (166,4 M
instrucciones de microcodigo) los ve: LRV 0,60 %, LUV 0,55 %, LPV 0,40 %, ~1,65 % con las
tiendas. Nunca se habia corrido el censo.

## 2026-09-01 — MUL.S sin MXCSR y los tres barajados del RSP en linea

Dos cosas, las dos con oraculo propio antes de tocar los gates.

**MUL.S sin MXCSR.** El perfilador de host del hilo de CPU con parallel-rdp ponia
`jitADDS` 14,7 % y `jitMULS` 11,6 % de las muestras dentro de la imagen, y el coste
era `mx::prep`: la bandera PE del MXCSR es pegajosa, asi que despues de la primera
operacion inexacta TODAS escriben el registro para limpiarla, y `ldmxcsr` serializa
el pipe. El producto de dos `float` normales o cero es exacto en `double`, asi que se
hace en doble, se redondea una sola vez a simple con el modo del guest puesto y el
Inexact sale de comparar. Suma, resta y division NO entran, y lo dijo el fuzz, no la
intuicion: en add/sub el doble tambien redondea (la suma exacta necesita ~277 bits de
rango) y la comparacion mentiria en ~1 de cada 3 pares al azar. Del producto se ceden
al interprete los que se salen de `[2^-126, maxfloat]`, porque con redondeo hacia cero
un desbordamiento da el maximo finito — que parece un numero normal — y lleva Overflow
ademas de Inexact. Fuzz diferencial con exponentes sesgados a los extremos, los cuatro
modos de redondeo: 0 discrepancias. Detalle en `docs/PERF-CPU.md` §14.

**LPV/LUV/LRV en linea** (`emitVecPack`, `emitVecRight` en `src/rsp/rspjit.cpp`). Eran
los tres barajados que pesan en SM64 (0,60 / 0,55 / 0,40 % de las instrucciones de
microcodigo) y salian por CALL a `lwc2Thunk<N>`, con ocho o dieciseis lecturas de byte.
Los tres son la misma figura — una ventana de 16 bytes alineada y un reparto que depende
de la DIRECCION — o sea un `pshufb` con la mascara armada en tiempo de ejecucion, a
partir de dos tablas nuevas dentro de `Rsp` (`byteHalf`, `laneIdx`). LRV ademas no
necesita salida al helper: la direccion se alinea a 16 y la ventana nunca cruza el final
de DMEM. `--rspjitfuzz 300000` y `--rspldfuzz 300000`: 0 diferencias. En SM64 no se nota
(`rsp Mips busy` 188,0 -> 187,7, ruido) porque el hilo del RSP va al 58 % y el palo largo
es el RDP; se queda porque es correcto y gratis en el camino caliente, y en un microcodigo
que use estas cargas de verdad el CALL si se paga. A/B `KESTREL_RSPJIT_NOVECPACK=1`.
Detalle en `docs/RSP-JIT.md`.

Gates de las dos: 14/14 + prdp 3/3, regress=0, md5 sin cambio (335 s / 310 s).

## 2026-09-01 — El regulador de ritmo estaba mal calibrado: +16 % de fotogramas con parallel-rdp

`kPaceSlack` (`KESTREL_PACESLACK`, `src/core/memory.cpp`) llevaba en 256 K desde el
2026-08-26, y su propio comentario avisa de que «el óptimo se MUEVE con la velocidad del
hilo de CPU». Este ciclo lo aceleró tres veces (fetch fast-path, MUL.S sin MXCSR, los
barajados del RSP en línea), así que tocaba rebarrer.

**Trampa de medida encontrada de paso** (y documentada en `PERF-CPU.md`): el «% de tiempo
real» NO sirve para calibrar esto. Su numerador son campos VI emitidos, y aflojar el freno
los infla sin hacer más trabajo — el guest quema ciclos emulados girando a la espera del
RCP. Con SoftRDP, 256 K → 1 M sube el «%» de 537 % a 647 % mientras la pared por 300
intercambios se queda en 8,06 → 7,93 s, o sea nada. El primer barrido de esta sesión se
tragó ese +20 % fantasma antes de cazarlo.

Con la métrica sana (pared por 300 intercambios de buffer = fotogramas de juego, min de 3
pasadas):

| holgura | prdp-jit | int/s | campos/int | threaded-jit |
|---------|----------|-------|------------|--------------|
| 256 K   | 4,40 s   | 68,2  | 2,73 | 8,06 s |
| 384 K   | 4,09 s   | 73,3  | 2,80 | — |
| 512 K   | 3,89 s   | 77,1  | 3,12 | 8,03 s |
| 768 K   | 3,85 s   | 77,9  | 3,20 | — |
| 1 M     | 3,80 s   | 78,9  | 3,24 | 7,93 s |

Codo en 512 K, y la ganancia real es **de parallel-rdp: +16 % de fotogramas de juego por
segundo**; SoftRDP sale plano dentro del ruido. El despeñadero de 512 K que motivó el
default viejo ha desaparecido, porque entonces el hilo de CPU competía por el núcleo del
worker y desde el arreglo de `rdpSubmit` (38 K despertares/s) ya no.

Y la fidelidad va en la misma dirección, no en contra: campos VI por intercambio con
**lockstep de oráculo (4,09)**, prdp-jit da 2,73 → 3,24 al aflojar, o sea todos por debajo
del oráculo y la holgura larga es la que más se le acerca. Sin canje. Default nuevo **1 M**,
con meseta ancha a ambos lados.

Gates: 14/14 + prdp 3/3, `regress=0`, md5 sin cambio (el regulador solo decide *cuándo*
duerme el hilo de CPU; el estado del guest no lo toca).


## 2026-09-01 — JIT de CPU: fin del «líder no compilable» (614 k → 25 k entradas/ventana)

Un bloque JIT corta en la primera op que no sabe emitir. Si esa op es la PRIMERA, el bloque
sale con `nOps == 0`, el fallo se memoiza y **ese PC cae al intérprete para siempre**. Con
`KESTREL_JIT_STATS=1` se cuenta por opcode: en SM64 (300 intercambios, ventana de 4,19 M de
entradas al conductor) eran ~614 k — BEQ 322 k, COP0 179 k, SPECIAL 82 k, BNE 31 k.

Cinco arreglos, todos semántica de HW genuina (detalle en `PERF-CPU.md` §15):

1. **Ranura de retardo con ALU-que-atrapa o MFC0.** `compileDelay` no probaba `emitTrapAlu`
   ni `emitCop0`. Ambas emiten su bail ANTES de escribir el destino, así que el bail puede
   apuntar al SALTO y el intérprete re-ejecuta salto + ranura con la semántica de excepción
   en ranura (EPC = el salto, `Cause.BD` = 1). El enlace de JAL/JALR es idempotente.
2. **Cruzar la página 4K, solo desde ckseg0**, donde VA→PA es un desplazamiento fijo y la
   contigüidad está garantizada. El bloque se marca `crossPage` y el despacho lo **rechaza si
   el mismo phys llega por TLB**; la caché negativa guarda también la ruta. Esto era la causa
   real de los 353 k de BEQ/BNE: salto en la última palabra de la página. `KESTREL_JIT_NOXPAGE`.
3. **MTC0/ERET/TLBR/TLBWI/TLBP como op TERMINAL** (cierran el bloque, el conductor re-muestrea
   interrupciones). Fuera **Count, Compare, Random, Wired y TLBWR**: el bloque adelanta Count y
   Random de golpe al salir, y ese sumando machacaría el valor recién escrito. n64-systemtest
   cazó el desfase exacto al no excluir Wired: *«Random, 1 instruction after setting Wired = 0»*,
   esperaba `0x1f`, salía `0x1e`.
4. **MFC0 Count dentro del bloque**: el atraso es exacto y conocido — durante la op `idx` el
   valor visible es `entrada + idx`, así que se emite `cop0[Count] + idx`, bit a bit lo del
   intérprete. La frontera `Count == Compare` no cabe dentro del bloque (guarda `DR_TIMER`).
   **Cause sigue fuera**: sus IP2/IP7 solo se refrescan al entrar, leerlo dentro expondría el
   sesgo de muestreo del JIT al guest.
5. **ALU de 64 bits (`emitAlu64`)** — DADDU/DSUBU/DADDIU y los desplazamientos dobles — bajo
   guardia de modo kernel en runtime (`KSU == 0`), que reproduce `CPU::reserved64` exacto:
   fuera de kernel sale por el bail sin tocar nada y el intérprete levanta la RI. El idioma de
   SGI para la mitad alta de un doble (`dsll32` + `dsra32`) valía él solo 54 k entradas.
   `KESTREL_JIT_NOALU64`.

Resultado en la misma ventana: **`[compfail] OP10=25301`** y nada más — solo MFC0 Cause, dejado
a propósito. `ops/entrada` 333 → **445**, `avgK` 10,25, `cover` 1000,8 %.

En pared **SM64 no se mueve** (threaded-jit 7,96 → 7,89 s; prdp-jit 3,84 → 3,81 s por 300
intercambios): con SoftRDP o parallel-rdp el palo largo es el RCP, no el hilo de CPU. En carga
CPU-pura sí: n64-systemtest a 1500 M de instrucciones, 8,95–9,04 → 8,83–8,94 s (≈ 2 %) sumando
los dos bisectores. Lo que importa no es ese 2 %, es que el intérprete deje de comerse 590 k
entradas por ventana — que es justo lo que estrangula a un juego CPU-bound como PD.

Gates: 14/14 + prdp 3/3, `regress=0`, md5 sin cambio (sm64 `466282775dbd0ac084946558a1c30771`,
prdp `b5521b24d8fc280fbf102df22d7d30cb`). `gate_all` 339 s, `gate_prdp` 289 s.

## 2026-09-01 — SoftRDP: el palo largo del build por defecto, −28 % de pared

`KESTREL_HEARTBEAT=1` deja claro quién manda sin parallel-rdp: **rdp 89 % de ocupación, rsp 58 %,
cpuWait 0 %**. Cinco pasos sobre el rasterizador software, medidos con pared para 300 intercambios de
buffer (SM64, `threaded-jit`, mínimo de 3 pasadas — el "% de tiempo real" no sirve, sube cuando el
emulador va peor):

1. **`getenv` y `lround` fuera del camino caliente (17,3 % del hilo).** `if(std::getenv("KESTREL_TRIDBG"))`
   se evaluaba **por triángulo** (8,2 %); `std::lround` no se puede alinear porque devuelve `long` y
   arrastra errno/dominio de la CRT (6,7 %) — sustituida por `ceil/floor` (que sí bajan a `roundsd`),
   bit a bit idéntica en el rango del rasterizador. **7,89 → 6,50 s.**
2. **Pliegue del tile una vez, no por toma.** `foldOf(tile) -> TexFold` + `texelAt(fold, s, t)`:
   shift/máscara/clamp/mirror/base/paleta dejan de recalcularse en cada una de las 3-4 tomas del
   filtro. **6,50 → 6,40 s.**
3. **Formato especializado.** `template<u32 K> texelK/filterK` con lista-macro `KEST_TEXKINDS`; el
   `switch` de formato sale del bucle de tomas. Trampa medida: clang **no alineaba** `texelK<5>`
   (RGBA16, 35 % del perfil) hasta ponerle `[[gnu::always_inline]]`. **6,40 → 6,30 s.**
4. **Invariantes de primitiva fuera del bucle de píxeles**: tile, tipo de ciclo y bit de
   alpha-compare no cambian dentro de un triángulo. **6,30 → 6,20 s.**
5. **Plan del combinador.** Los ocho selectores son constantes hasta el siguiente `SET_COMBINE`:
   `buildCombPlan()` los traduce una vez a filas de tabla y el camino por píxel materializa sólo las
   filas que hacen falta. El 13 % del hilo era `switch` de despacho puro. **6,20 → 5,68 s.**

Dos decisiones de corrección dentro del punto 5, no de velocidad: la llave del plan es
`(combine_hi, combine_lo, cycleType())` —no una bandera de suciedad, que un savestate o una escritura
por MCP dejarían rancia—, y si un ciclo **que se ejecuta** selecciona NOISE se cae a
`combineColorSlow`, porque NOISE consume `std::rand()` y el orden de consumo es observable.

**Gates:** 14/14 + prdp 3/3, `regress=0 improve=0 new=0`, krom `mean_exact` 88,71 / 89,26 idénticos al
baseline, md5 de SM64 sin cambio. Detalle completo en `docs/PERF-CPU.md` § 16.

Perfil resultante (ya sin desperdicio puro): `run` 25,6 % · `sampleTexFold` 23,1 % ·
`combineColor` 23,8 % · `putPixel` 9,3 % · `blendPixel` 7,0 % · `loadTile` 7,0 %.

## 2026-09-01 — SoftRDP tanda 2: combinador en SSE4.1, −37,5 % acumulado

Sigue al apunte anterior, mismo método de medida (pared para 300 intercambios de buffer, SM64,
`threaded-jit`, mínimo de 3):

- **Plan del blender.** Los muxes P/A/M/B, IM_RD, FORCE_BLEND, AA_EN y el modo de dither se
  redescodificaban por píxel desde `other_lo`/`other_hi`, y `blendPixel` llamaba a `cycleType()` tres
  veces. Plan con llave `(other_hi, other_lo)`, como el del combinador.
- **El test de scissor iba tres veces por píxel** (`coverPixel → blendPixel → putPixel`). `storePixel`
  (escritura sola, `always_inline`) para los que ya entraron por el test; de paso se alinea dentro de
  `blendPixel`.
- **`loadTile` copiaba byte a byte con dos comprobaciones de límite por byte** (7 % del hilo). Las filas
  son contiguas en origen y destino → `memcpy`, recortando la cuenta una vez: mismo prefijo exacto,
  porque los dos límites son monótonos.
- **Combinador en SSE4.1.** Era el 38 % del hilo. Los cuatro canales corren la misma ecuación entera →
  un vector de 4×int32; el carril de alpha se trae de su propia fila con `_mm_blend_epi16(...,0xC0)`.
  Aritmética entera pura, valores idénticos; el escalar queda como referencia y como camino de NOISE.
  **−9,9 %**, y el combinador baja al 19 %.
- **La división del blender a multiplicador mágico.** `sum` ∈ 1..15 y numerador de 11 bits por
  construcción, así que `n/d == (n * ceil(2^16/d)) >> 16` exacto (comprobado exhaustivamente).
  Neutro en SM64 —sus píxeles se van por los atajos— pero paga en las roms que usan el divisor.
- **Lectura de framebuffer elidida.** El blender llegaba a sus dos atajos *después* de leer el
  framebuffer, y esos atajos no usan el color de memoria salvo que P sea CLR_MEM. **−2,5 %**.

Pared 7,89 → **4,93 s** entre las dos tandas (**−37,5 %**), md5 de SM64 sin cambio en cada paso.
Detalle en `docs/PERF-CPU.md` § 17.

### SoftRDP tanda 3: interpolación en SSE y el pliegue de coordenadas compartido (−43,5 % acumulado)

- **Sombra y S/T por escanlínea en SSE.** Ocho interpolaciones `double` escalares por píxel (RGBA +
  S/T) pasan a tres multiplicaciones vectoriales `__m128d`. Anfitrión sin FMA → mismo bit. El primer
  intento perdió tiempo por rebotar el vector en memoria; empaquetando con `_mm_shuffle_epi8`,
  **−1,4 %**.
- **Filtro de 3 tomas en SSE.** Los cuatro canales comparten pesos, `>>5` y clamp → un `__m128i`.
  **−1,7 %**.
- **Pliegue/lectura separados.** `texelK` se parte en `foldCoord` (SHIFT + clamp/mirror/mask) y
  `fetchK<K>` (lectura + decodificación). El filtro de 3 tomas sólo toca cuatro coordenadas
  distintas (`s0`, `s0+1`, `t0`, `t0+1`), así que pliega cuatro veces en vez de seis. El pliegue es
  puro → compartirlo no puede cambiar un texel. **−6,5 %**, el mayor salto de la tanda.

Pared 7,89 → **4,46 s** en tres tandas (**−43,5 %**), md5 de SM64 idéntico en cada paso.
`sampleTexFold` vuelve a ser el líder (39 %) por trabajo real, no por desperdicio.
Detalle en `docs/PERF-CPU.md` § 18.

### SoftRDP tanda 4: el techo del hilo del RDP (pared sin cambio, y por qué)

Primera tanda que **no mueve la pared**, y el resultado negativo es el hallazgo. Matriz de
sensibilidad (SM64 / 300 intercambios, hilos + JIT, mínimo de tres):

| configuración | pared |
|---|---|
| línea base | **4,46 s** |
| `KESTREL_NORASTER=1` | 3,11 s |
| `KESTREL_JIT=0` | 24,34 s |
| `KESTREL_NOVIDEO=1` | sin cambio |

Ocupación: `rdp 82 % · rsp 57 % · cpuWait 0 % · pace 0 %`. O sea: el rasterizado son ~1,35 s de
los 4,46 y el hilo del RDP es **el único en el camino crítico**; CPU y RSP tienen holgura, así que
el trabajo que se les quite no se ve. Con un suelo de ruido de ±1,5 %, cualquier mejora que valga
el 1-2 % del hilo del RDP es inmedible de una en una.

Se quedan cuatro cambios exactos y estrictamente menos trabajo, todos dentro del ruido:
`stFixed` (las dos coordenadas a punto fijo en un `cvttpd2dq`), filtro de textura a carriles de
16 bits (`pmullw` = 1 µop vs 6 de `pmulld`, mismo entero por rango acotado), pliegue rápido por
tile (sin SHIFT + con mask + sin clamp/mirror → un AND) y `MFC0` del RSP directo al decodificador
de MMIO (`Memory::rcpReg32`, saltando el recorrido completo de regiones de `resolve` — era el 32 %
del hilo del RSP).

Se revierte uno: la granularidad de `rcpPace`. Neutro con y sin rasterizado, y cambiaba la
heurística de un regulador a cambio de nada.

Siguientes palancas reales, por tamaño: `parallel-rdp` por defecto (se lleva los 1,35 s enteros,
pero es política de build), enlace directo de bloques del JIT (14,4 % del hilo de CPU) y FPU del
JIT en línea (13 %). Detalle en `docs/PERF-CPU.md` § 19.

## 2026-09-02 — el audio entrecortado era el DAC del AI, no el sumidero del host

Sintoma del usuario: "noto el audio entrecortado". Lo primero fue medirlo, porque "se oye mal"
no distingue entre el emulador que produce de menos (hueco = silencio) y el que produce de mas
(descarte = latencia). `KESTREL_AUDIOSTAT=1` imprime al cerrar cuantas muestras se empujaron,
cuantas se sirvieron, cuantas salieron de relleno y cuantas se tiraron, mas el minimo y el
maximo del anillo.

SM64, 600 intercambios de buffer con el limitador puesto (`KESTREL_THROTTLE=1`, que es como lo
oye el usuario), antes:

```
[audio] rate=32006 empujadas=1072864 servidas=1072704 silencio=350656 (24.63%) descartadas=0
[audio] recargas=1390 cortas=394 anillo min=0 max=4864 de 22050 muestras
```

Una de cada cuatro muestras que sonaban era relleno. Y el anillo nunca pasaba de 4864 de 22050:
no es que el sumidero fuera lento, es que **no le llegaba audio**. Las cuentas lo cierran: 22.4 s
de tiempo de guest (1340 campos a 59.94 Hz) produjeron 16.76 s de audio, o sea el 75%.

Raiz: `Memory::aiTick` drenaba la FIFO del AI **por campos de video**, un bufer como mucho por
campo, y tiraba el credito sobrante. El AI real no funciona asi: es un DAC que consume 4 bytes
(16 bits estereo) cada 1/rate segundos, continuamente, con rate = vid_clock/(dacrate+1). Si el
juego encola bufers mas cortos que un campo caben VARIOS por campo; si los encola largos, uno
tarda varios campos. SM64 los encola a ~0.75 campos, asi que el limite de "uno por campo" le
drenaba la FIFO al 75% del ritmo real, el driver de audio se quedaba bloqueado en FIFO_FULL y
generaba justo ese 75% del audio. El emulador iba al 100%; el que iba al 75% era el reloj del
audio del guest.

Arreglo (semantica de HW, no ajuste): el credito se acumula en el mismo reloj que todo lo demas
del emulador -- instrucciones retiradas -- y se drenan tantos bufers como quepan en el tiempo
transcurrido:

```cpp
u64 delta = retiredNow - rcp.aiLastRetired;          // tiempo de guest desde la ultima vez
if(delta > viFieldInsns * 4) delta = viFieldInsns * 4;   // arranque/savestate no vacian la FIFO
u32 rate = rcp.ai_dacrate ? (48'681'812u / (rcp.ai_dacrate + 1)) : 32'000u;
u64 den  = viFieldInsns * (u64)viFieldHzMilli;       // instrucciones por segundo x1000
rcp.aiAcc += delta * ((u64)rate * 4ull * 1000ull);
u64 bytes = rcp.aiAcc / den;  rcp.aiAcc -= bytes * den;   // el resto NO se tira
while(bytes && rcp.ai_fifo_count) { ... pop, raiseIntr(MI_AI), siguiente ... }
```

`aiTick` sale ademas de dentro del `if(fieldClose)`: se llama en cada subtramo, que es donde
`viTick` ya recibe el contador de instrucciones. El acumulador es entero (unidades de
byte*instruccion) para que el resultado sea bit-identico entre lockstep y threaded, y entra en
el savestate (version 2 -> 3).

Con eso, la misma medida:

```
[audio] rate=32006 empujadas=1416192 servidas=1414880 silencio=5408 (0.38%) descartadas=0
[audio] recargas=1387 cortas=6 anillo min=0 max=4192 de 22050 muestras
```

24.63% de relleno -> 0.38%, y las 1416192 muestras empujadas son 22.1 s de audio para 22.4 s de
guest, o sea el reloj del audio ya va a la par del de video.

Dos remates en el sumidero (`src/audio/audio.cpp`), que valen para cualquier juego:
- **Cebado**: el hilo alimentador no manda el primer bufer hasta que el anillo tiene dos bufers
  de dispositivo. Arrancar con el anillo vacio regala el arranque en silencio y esa desventaja
  no se recupera nunca, porque el hueco se rellena con ceros en vez de esperar. Tras un hueco se
  vuelve a cebar, para reconstruir el colchon en lugar de encadenar hipidos.
- **Histeresis de tasa**: `init()` reabria el dispositivo con CUALQUIER cambio de dacrate, y
  reabrir corta el sonido. Por debajo del 1% de diferencia el tono no se distingue y el corte si,
  asi que se sigue tocando con el dispositivo ya abierto.

## 2026-09-02 — parallel-rdp pasa a ser el build por defecto, y el paquete deja de ir por detras

### El rasterizador ya no es una opcion de compilacion escondida

La tanda 4 del SoftRDP cerro con el hilo del RDP como unico palo largo (ocupacion 82 %, ~1,35 s
de los 4,46 s de pared) y con la unica palanca capaz de llevarse esos 1,35 s enteros anotada como
«politica de build»: `parallel-rdp`. Se ejecuta esa politica.

- `CMakeLists.txt`: `KESTREL_PRDP` pasa a **ON** por defecto. `build/` sigue existiendo como
  oraculo determinista compilandolo con `-DKESTREL_PRDP=OFF`, que es lo que hace `gate_all.sh`.
- `src/vrdp/vrdp.cpp`: el valor por defecto en tiempo de ejecucion se invierte. `KESTREL_PRDP=0`
  fuerza el rasterizador en CPU; si Vulkan no se puede usar (sin driver, sin cola, sin dispositivo)
  se cae solo al SoftRDP en vez de morir. Un emulador que no arranca en una maquina sin Vulkan no
  es un emulador mas rapido, es uno roto.
- `scripts/validate.py`: los modos software pasan `KESTREL_PRDP=0` explicito en vez de confiar en
  el valor por defecto, que ya no es el suyo. Con el arreglo del `== "1"` en la comparacion de
  linea base, el oraculo sigue siendo bit a bit el mismo: md5 de SM64 `466282775dbd0ac084946558a1c30771`
  en los siete modos software, `b5521b24d8fc280fbf102df22d7d30cb` con parallel-rdp.

### El MMIO del RSP, por el decodificador y no por el mapa de regiones

La tanda 4 metio `MFC0` del RSP directo a `Memory::rcpReg32`. Faltaba la mitad simetrica:
`Rsp::mtc0` hacia el recorrido completo de `resolve` para cada escritura. Ahora va por
`Memory::rcpRegWrite32`, el mismo decodificador. Era el 32 % del hilo del RSP entre las dos.

### El freno y el permiso, de dos lecturas cruzadas a una

`rcpPace` (frena la CPU si adelanta al RSP) y `paceAllowance` (cuantas ops de CPU quedan antes de
volver a preguntar) salian de los **mismos dos valores**: `rspBusy` y `rsp.cyclesRun`. Los dos
viven en lineas que el worker del RSP reescribe sin parar, asi que leerlos es un fallo de cache
compartida, no una lectura local — y se pagaba **dos veces por vuelta al trampolin**. `rcpPace`
devuelve ahora el permiso calculado con lo que ya tenia cargado; `paceAllowance` desaparece y en
su sitio queda `paceGrant(ahead, allow)`, que no relee nada. Estrictamente menos trabajo cruzado,
misma heuristica.

### ADD.S / SUB.S sin MXCSR, cuando los exponentes estan cerca

Mismo truco que MUL.S (§ 17), pero **condicionado**: `a+b` en `double` es exacto cuando los dos
exponentes estan cerca — el resultado exacto ocupa `|ea-eb|+25` bits de mantisa, luego con
`|ea-eb| <= 28` cabe en los 53 del doble — y entonces un unico redondeo a simple da el bit del
guest y el Inexact sale de comparar. Un cero cuenta como «cerca» de cualquier cosa: sumar cero es
exacto siempre. Cuando los exponentes se separan mas, el doble **tambien** redondea, la comparacion
mentiria, y se vuelve al MXCSR. Es el caso raro: el codigo de juego suma magnitudes parecidas.

Para poder afirmarlo se anade `jitCop1AluChk`, oraculo diferencial hermano de los de CVT y CMP:
con `KESTREL_FPORACLE` puesto ejecuta el camino rapido, rebobina `fpr[fd]`/`fcr31` y repite por el
interprete, comparando resultado, registro y banderas. **6,3 M comparaciones, cero discrepancias.**
Pared SM64/300 intercambios con parallel-rdp: A/B en la misma sesion **3,852 s sin -> 3,820 s con**.

### El paquete iba por detras del codigo (esto es lo que se rompio de verdad)

Las dos baterias recompilan `build/` y `build-prdp/`. El arbol **estatico** — el unico `.exe` que
arranca fuera de MSYS2, porque lleva libc++ y GLFW dentro — no lo recompila nadie. Resultado: el
`dist/` y el instalador se quedaron en el 2026-08-28 mientras el codigo seguia, y lo que se probaba
desde el lanzador no era el emulador actual.

Se cierra con `scripts/pack.sh`, un solo comando: mata cualquier `kestrel64.exe` vivo (Windows no
deja reenlazar un fichero abierto), configura `build-prdp-static` si falta, compila, llama a
`scripts/dist.sh` (que ademas congela el lanzador con PyInstaller y hace el zip portable) y compila
el instalador con Inno Setup. La version sale de `kVersion` en `src/core/system.hpp` y se le pasa a
`ISCC` con `/DAppVer=`, para que no haya una segunda copia en el `.iss` que se quede vieja; el `.iss`
la deja como respaldo para quien lo invoque a mano. Dos detalles de entorno que costaron un intento
cada uno: Inno Setup esta instalado como **7**, no como 6 (por eso `pack.sh` busca en vez de fijar la
ruta), y MSYS2 traduce a ruta de Windows cualquier argumento que empiece por `/`, asi que `/DAppVer=`
llegaba a ISCC como un segundo nombre de script — se excluye con `MSYS2_ARG_CONV_EXCL="/D"`.

## 2026-09-02 — ITC: los saltos indirectos ya no salen del código generado

El enlace de bloques del JIT sólo sabe atar destinos **estáticos**. `JR`/`JALR` — o sea, el
retorno de toda función del guest — salían siempre por el despachador, y el trampolín se llevaba
el 14,4 % del hilo de CPU.

Ahora hay una **caché de destinos indirectos** (ITC): tabla de correspondencia directa VA →
`linkEntry`, 4096 entradas de 16 B (64 KB), índice `(va >> 2) & 4095`. La sonda va emitida dentro
del bloque, justo antes de la salida lenta: índice, comparar el VA guardado, y si acierta
`jmp qword [rdx+8]` sin desmontar el marco. Fallo = cuatro instrucciones y un salto no tomado.

Sólo se arma con las mismas condiciones que el enlace (`g_jitLink && !g_jitDiffAny && ck0Entry`),
es decir sólo ckseg0: sin TLB la traducción es fija, así que ninguna entrada puede quedar
apuntando a otro código. Se limpia entera en `clear()`, en `unlinkTo(phys)` (SMC) y en
`unlinkAll()` (invalidación de I-caché), esta última **antes** del corto-circuito por `anyLinked`.

**Medido** (SM64, 200 intercambios, threaded-jit, mínimo de 3, tres tandas): **3,11–3,13 s** con
ITC vs **3,19–3,20 s** con `KESTREL_JIT_NOITC=1` → **≈2,5 % de pared**, distribuciones sin
solaparse. md5 de SM64 idéntico en jit y threaded-jit, `systemtest[jit]` 0/3721, los dos portones
verdes. Detalle en `docs/PERF-CPU.md` §20.

### Afinado de la ITC (2026-09-02, mismo día)

Tres ajustes medidos encima de la ITC: índice mezclado `((va ^ (va>>12))>>2) & mask` contra los
fallos por conflicto de una tabla directa (+2,5 % en despachos), bandera `itcAny` para no barrer
la tabla en los 1549 barridos de I-caché por tanda, y tamaño ajustable `KESTREL_JIT_ITCBITS` con
nuevo defecto **14 bits** (16384 entradas, 256 KB), elegido por A/B de pared: 12 → 2,439 s,
13 → 2,451 s, **14 → 2,418 s**, 16 → 2,515 s. Retirado el atajo inseguro `KESTREL_JIT_NOINVAL`.

Diagnóstico que queda anotado: el permiso de cadena concedido es **siempre** el tope de 4096 ops
(`[permiso] tope=3,1 M`) pero sólo se ejecutan ~605 ops (12-13 bloques) por entrada al driver.
Ni el desenlace por barrido de I-caché ni el aliasing de la tabla lo explican (los dos medidos,
+2 % cada uno). Candidato vivo: la ITC sólo la rellena el driver, así que un bloque al que sólo
se llega por cadena nunca recibe entrada — es cobertura, no conflicto. Detalle en
`docs/PERF-CPU.md` §20.4-20.5.

Portones: gate_all 355 s, gate_prdp 298 s, ambos verdes, `nodump=0` en los dos krom.

## 2026-09-02 — Perfect Dark: donde se para exactamente, y por que no es una regresion

> **SUPERADA por la seccion "RAIZ del cuelgue de Perfect Dark" mas abajo.** El diagnostico
> de aqui (un solo SYNC_FULL, no se vuelve a mandar trabajo al RDP) era correcto como
> observacion pero no llegaba a la causa: el trabajo SI se mandaba, kestrel lo tiraba por
> llegar con el RDP congelado. Se deja por el metodo, no por la conclusion.

El usuario pidio arrancar Perfect Dark y ver si ya funciona. **No funciona todavia**: arranca,
abre ventana, pinta la pantalla de copyright de Rare (capturada, 576x240, 47 colores distintos)
y ahi se queda. Esto es lo que se ha medido, para no volver a empezar de cero.

### No es una regresion de este trabajo

Se construyo un binario de referencia en un worktree del commit `0f6634f` (anterior a toda la
tanda de optimizaciones de esta sesion) y se corrio el mismo ROM con el mismo entorno:

```
actual  : 400 swaps, 2784 campos VI, 1 RDP syncs, 2177M insns, [statehash] 71b8f8a731083f52
0f6634f : 400 swaps, 2784 campos VI, 1 RDP syncs, 2177M insns, [statehash] 71b8f8a731083f52
```

Hash de estado y cuenta de instrucciones **identicos al bit**. El paron es anterior, y ninguna
de las optimizaciones de esta sesion lo ha causado ni lo ha empeorado.

El dynarec tampoco diverge: interprete y JIT dan el mismo `[statehash]` y la misma cuenta de
instrucciones retiradas con `KESTREL_THREADS=0`.

### El 54,8 % de CPU en `0x70001930` **no** es un panico

El perfilador colocaba mas de la mitad de las muestras en un bucket de 16 bytes en el fisico
`0x1930`, que desensamblado es `sw a0,0(sp)` seguido de `beq zero,zero,.` (`1000ffff`). Leyendo
la RDRAM viva (no el ROM: de `0x1050` en adelante el ROM lleva `libzip` comprimido y el
desensamblado del ROM ahi es basura) y cruzandolo con `src/lib/boot.c` del decomp, esa funcion
es `idleproc`:

```c
static void idleproc(void *data)
{
	while (true);
}
```

O sea, el **hilo idle**. Que se lleve la mitad de la CPU solo significa que el juego no tiene
nada que hacer. El sintoma real es otro.

### El estado real: todo el mundo esperando

El escaner de hilos (`KESTREL_THREADSCAN=1`) ya reconoce hilos de juegos con TLB y lee las
OSThread de forma **coherente** con la D-cache; antes no veia ninguno en PD. Con eso:

```
thr id=1  pri=0   RUNNING  pc=70001938                      <- idle
thr id=3  pri=10  WAITING  mq=8008db30 (0 de 32)            <- MAIN,  g_SchedMesgQueue
thr id=2  pri=30  WAITING  mq=8008dc10 (0 de 8)             <- SCHED, cola de interrupciones
thr id=4  pri=20  WAITING  mq=80091810 (0 de 8)             <- AUDIO
thr id=6  pri=11  WAITING  mq=8008faa8 (0 de 10)            <- RESET
thr id=5  pri=40  WAITING  mq=80094ab0 (0 de 1)             <- FAULT
thr id=0  pri=254 WAITING  mq=80090230 (0 de 5)             <- vimgr (libultra)
thr id=0  pri=150 WAITING  mq=80099a00 (0 de 64)            <- pimgr (libultra)
```

(prioridades segun `THREADPRI_*` de `include/constants.h` del decomp).

La cadena de despertar es: interrupcion VI -> `send_mesg(OS_EVENT_VI)` -> vimgr -> retrace a los
clientes -> `osSched` -> `g_SchedMesgQueue` -> hilo MAIN. Las interrupciones VI **si** llegan y
**si** se reconocen: `KESTREL_VILOG=1` cuenta 449 escrituras de `VI_CURRENT` (el ACK que hace
`exceptasm.s` justo antes de `send_mesg 0x38`) en 520 campos. Y `mi_intr=00` al final confirma
que nada queda pendiente sin atender.

Con las colas vacias no se puede distinguir «nunca se envio» de «se envio y se consumio», asi
que el siguiente paso es instrumentar `send_mesg`/`osRecvMesg` por direccion, no seguir mirando
el estado final.

### Lo demas que se sabe

- RCP parado y limpio: `sp_status=00000203` (HALT|BROKE|SIG2 = tarea terminada), `rspRun=0`,
  `dpc_status=00000000`. Solo **1 RDP sync** en 400 swaps: despues del primer cuadro no se
  vuelve a mandar trabajo al RDP.
- `[exchist] Int(0)=11071 CpU(11)=4`. Los dos **AdEL** que salian antes eran **falsa alarma
  auto-infligida**: los provocaba el propio volcado de hilos del emulador. Su lambda `rd`
  llamaba a `read32()`, que lanza la excepcion de verdad -- `takeException` ya ha escrito
  EPC/Cause/BadVAddr para cuando el volcado vuelve a poner `memAbort=false` --, y el bucle
  que sondea las dos bases (`0x80000000` y `0x70000000`) fallaba a proposito en una de ellas.
  `badv` coincidia bit a bit con `base+0x463e4` y `base+0x463f0`, las dos direcciones del
  walker. Arreglado: la lectura va por `probing=true` (translate sin efectos secundarios,
  `~0` si no traduce) + `peekPhysCoherent`. `[exchist]` queda limpio y el `[statehash]` no
  cambia. Ademas era un bug de verdad: el volcado corrompia EPC/Cause/BadVAddr del guest.
- La Expansion Pak esta puesta (`RDRAM_SIZE_EXPANDED`, 8 MB) y `osMemSize` en `0x318` lleva el
  tamano real, asi que no es el chequeo de memoria de PD.
- El apano del CIC-6105 (`*(0xA00002E8) = 0xC86E2000`) sigue haciendo falta mientras el IPL3 LLE
  no este; sin el PD no pasa del cargador.

### Herramientas arregladas por el camino

- `KESTREL_THREADSCAN` leia la RDRAM cruda: el estado de las OSThread salia de hace varios
  cambios de contexto (aparecian dos hilos en RUNNING a la vez). Ahora lee por
  `peekPhysCoherent`, o sea a traves de la D-cache write-back de la CPU.
- El mismo escaner exigia punteros y PC guardados en KSEG0, asi que era ciego justo en los
  juegos con TLB, y descartaba `id == 0`, que es como se registran los hilos que crea la propia
  libultra (vimgr, pimgr) — los dos primeros eslabones de la cadena de retrace.
- El servidor de telemetria escuchaba con `listen(sock, 1)`. Atiende a un cliente cada vez, asi
  que una orden larga (`cpu.run_until` con timeout grande) bloquea el unico hilo y **rechaza**
  las conexiones siguientes: parece que el emulador se ha caido cuando solo esta ocupado.
  Backlog a 8: el que llega espera turno.

## 2026-09-02 — RAÍZ del cuelgue de Perfect Dark: el RDP congelado perdía el FIFO

Perfect Dark llevaba desde el principio pintando la pantalla de copyright de Rare y no pasando
de ahí. El síntoma que se veía por fuera era desconcertante: la máquina **no** estaba
bloqueada. Audio sonando, VI generando campos, SI/PI/AI/SP con sus interrupciones subiendo y
el guest reconociéndolas todas… y **una sola interrupción DP en 68 segundos de tiempo de
guest**. Sólo el camino gráfico estaba muerto.

### Cómo se llegó

El dato que rompió el caso salió de `KESTREL_RSPTRACE`, ampliado con un volcado al **final** de
cada tarea del RSP (PC de BREAK, ciclos, cabecera OSTask, ventana DPC). Con él la traza queda
así:

```
KICK #1 type=1 (gfx) dl=0005d968  → END pc=0x7bc ciclos=1863 → 17 spans RDP → DP intr #1
KICK #2 type=1 (gfx) dl=0005d968  → END pc=0x7bc ciclos=1867 → CERO spans RDP → sin DP
KICK #3..∞  type=2 (audio) para siempre
```

Las dos tareas gráficas son **idénticas**: mismo microcódigo, misma display list, mismo PC
final, prácticamente los mismos ciclos. La primera rasteriza; la segunda no produce ni un
píxel. Y al acabar la segunda quedaban 128 bytes de comandos RDP colgados entre
`DPC_CURRENT` y `DPC_END`.

Instrumentando **cada escritura a los registros DPC** (`KESTREL_DPSYNCLOG`, ahora también
imprime `[dpwr]`) aparece la secuencia exacta:

```
END #1
[dpwr] reg=0c v=00000008 st=00000000   <- SET_FREEZE  (DPC_STATUS bit 3) -> st=0x02
KICK #2
[dpwr] reg=04 v=0075e1a8 st=00000402   <- DPC_END ... con FREEZE puesto
[dpwr] reg=04 ... x15    st=00000002      128 bytes de comandos, incluido el SYNC_FULL
END #2
[dpwr] reg=0c v=00000004 st=00000002   <- CLEAR_FREEZE, DESPUÉS de todo
```

### El bug

En hardware, congelar el RDP **para** el procesador de comandos pero no descarta nada: los
comandos siguen en el FIFO y, al limpiar FREEZE, el RDP **reanuda desde `DPC_CURRENT` hasta
`DPC_END`**. kestrel implementaba la mitad buena (`if(!frozen)` alrededor del arranque en la
escritura de `DPC_END`) y se dejaba la otra: **nadie volvía a arrancar el FIFO al descongelar**.
Todo `DPC_END` que llegase congelado se perdía para siempre.

El efecto en cascada explica cada síntoma observado:

- sin `SYNC_FULL` retirado no hay interrupción DP;
- sin DP, `__scHandleRDP` de libultra no corre y el planificador da el RDP por ocupado
  eternamente: no despacha otra tarea gráfica ni manda `OS_SC_DONE_MSG`;
- MAIN se queda clavado en el bucle de 6 retrazas del copyright;
- el audio sigue porque va por su propia lista de tareas del RSP;
- y **SI se congela** (569 transacciones joybus y ni una más) porque `joysTick()` cuelga del
  mismo camino — de ahí también que el mando "no respondiera".

### El arreglo

`Memory::dpcAdvance()` (nuevo, `src/core/memory.cpp`) concentra el arranque del procesador de
comandos: consume de `dpc_submitted` a `dpc_end`, marca `START_GCLK|PIPE_BUSY` y encola el
trabajo (worker en threaded, síncrono en lockstep). Se llama desde dos sitios:

1. la escritura de `DPC_END`, como siempre;
2. **al limpiar FREEZE** en `DPC_STATUS`, si quedaba algo pendiente (`dpc_submitted != dpc_end`).

Es semántica genuina del hardware, no un apaño para PD: cualquier juego que congele el RDP
mientras reprograma tenía exactamente el mismo agujero. La contabilidad de `dpc_submitted`
(vista del productor) ya era la correcta — sólo faltaba la reanudación.

Resultado: PD encadena tareas gráficas sin parar, una interrupción DP por cuadro, completa las
6 retrazas del copyright, intercambia búferes (origin alternando) y sigue cargando desde el
cartucho. El cuelgue de años era esto.

### Dos hipótesis descartadas por el camino (con evidencia)

- **Expansion Pak**: kestrel siempre da 8 MB (`RDRAM_SIZE_EXPANDED`, `reset(expansionPak=true)`)
  y escribe `osMemSize = 0x800000`. PD ni siquiera lee `0x80000318`: su `osGetMemSize()`
  **sondea** RDRAM en `0xA0000000 + size` de 4 a 8 MB, y el sondeo pasa. Con arranque HLE PD
  toma **cero** excepciones TLBL, o sea el camino de mapeo estático (8 MB), y `mainInit`
  hardcodea `K0BASE + 8 MB`. El framebuffer enseñaba la pantalla de copyright, no un aviso.
  **Hueco real que queda**: kestrel no tiene modo 4 MB ni interruptor. `Memory::reset(false)` ya
  lo soporta; falta el toggle + la opción del lanzador, que es lo que otros emuladores exponen.
- **CIC / IPL3**: `KESTREL_LLE_IPL3` ya ejecuta el IPL3 **propio del cartucho**, sea cual sea el
  CIC — no hay que implementar chip por chip, el CIC sólo cambia semilla y hand-off. Con el
  arreglo del hand-off de CIC-6105 (abajo) PD arranca por su IPL3 real y acaba en el **mismo**
  estado colgado que con boot HLE, así que el CIC no tenía nada que ver.

## 2026-09-02 (bis) — hand-off de CIC-6105 en el arranque LLE

`KESTREL_LLE_IPL3` moría en un TLBL con los cartuchos CIC-6105 (Perfect Dark, Zelda, Banjo).
Su IPL3 arranca con un descifrador que lee su tabla con `lw t2, 0x44(t3)`, o sea DMEM+0x84,
justo detrás del stub: sin `t3` cargado la primera lectura va a la dirección `0x44` y el IPL3
muere. El hand-off real de IPL2 deja `t3 = 0xA4000040` (base del propio IPL3 ya copiado en
DMEM) y `ra = 0xA4001550`. Añadidos en `CPU::fastBoot`.

Queda pendiente antes de que el camino LLE pueda ser el de por defecto: arranca el RSP con
IMEM vacía (el volcado `KESTREL_RSPHANG` enseña IMEM a ceros y los 32 GPR del RSP a cero, y el
mismo ROM con boot HLE no emite ni un `[rsp] WARNING`). Y hay que validarlo contra la suite
krom, que trae IPL3 no estándar.

## 2026-09-02 (ter) — COP1 simple en línea en el dynarec: MEDIDO Y REVERTIDO

Se implementó el camino rápido en línea de `ADD.S`/`SUB.S`/`MUL.S` en el dynarec de la CPU
(mismas guardas y misma aritmética que `jitCop1Alu<>`, con el trampolín de siempre como
oráculo para todo lo que no las cumpla). Correcto: systemtest 0/3721·0/2·0/6 y los statehash
de lockstep de SM64 y PD idénticos con y sin él a 600 M de instrucciones.

Pero **no acelera**: SM64 tardaba 29,6 s con el camino en línea contra 29,31 s sin él, un 0,8 %
*peor*. Las ALU de COP1 son ~0,7 % de las instrucciones retiradas (≥4,19 M de 600 M), así que
el techo teórico de la optimización era ~0,24 % — por debajo del ruido, y el código extra
emitido por bloque lo comía. Revertido. El diseño completo queda escrito aquí por si el reparto
de instrucciones cambia con otro juego.

## 2026-09-02 (quater) — hasta donde llega Perfect Dark ahora, y mando inyectable por MCP

Con el FIFO del RDP arreglado, PD deja de estar clavado en el copyright. Medido sin tocar
nada mas (`KESTREL_MAXFLIPS`, arranque HLE, ROM NTSC final):

| tope | resultado |
|------|-----------|
| 200 intercambios | logo de Rare, resolucion alta (576x480) |
| 700 | **logo de Nintendo 64 en 3D**, girando |
| 1500 | **cinematica de intro en 3D** (interior de dataDyne), 1423 sincronizaciones de RDP en 1500 intercambios |
| ~4000+ | **logo PERFECT DARK** y despues el **menu "Select Location"** (Game Pak 4 / Controller Pak 4) navegable |

O sea: el juego arranca entero. Los `[frames]` cuadran (1500 intercambios / 2176 campos VI /
1423 sincronizaciones), la SI sigue sondeando el joybus hasta el ultimo cuadro (antes se
congelaba en 569 transacciones) y el mando responde.

### Mando inyectable desde la telemetria (`pad.set` / `pad.get`)

Para comprobar lo anterior sin nadie delante hacia falta poder pulsar botones. `KESTREL_BUTTONS`
no sirve: es una variable fija, deja el boton pisado para siempre y un menu espera un FLANCO DE
BAJADA. Y escribir `padButtons` por `mem.write` tampoco, porque el bucle de la ventana lo
reescribe cada cuadro.

Nuevo par de ordenes del servidor (`pad.set`, `pad.get`) y sus herramientas MCP
(`controller_set`, `controller_state`). El mando remoto pisa al del anfitrion mientras le
queden sondeos, y **la duracion se cuenta en sondeos del joybus del mando 1, no en
milisegundos**: es el unico reloj que el juego percibe, asi que una pulsacion dura los mismos
cuadros de juego con el emulador al 30 % o al 200 % de tiempo real, en lockstep o en threaded.
`polls=-1` mantiene pulsado hasta nueva orden, `polls=0` suelta. Los botones se pueden dar como
palabra de 16 bits o por nombre (`"start"`, `"z+cup"`, `"a,b"`).

Con esto se llego al menu solo: cinco pulsaciones de START espaciadas, capturando el
framebuffer en cada una.

### Lo que se ve de diferente entre backends

Mismo punto del juego, mismos cuadros:

- **parallel-rdp**: limpio. Logo PERFECT DARK y menu correctos.
- **SoftRDP**: llega al mismo sitio y el menu es legible, pero la cinematica de intro sale
  cubierta de **lineas blancas por las aristas de los triangulos** y el fondo del menu sale
  lavado/borroso. El logo de Nintendo 64 sale con **grietas negras** en las caras.

No es una regresion de este cambio (krom regress=0, sm64 md5 sin cambio en los seis modos): es
la frontera de accuracy que ya estaba documentada como la ultima —- cobertura/AA subpixel —-
vista por primera vez en un juego real en vez de en una ROM de krom. parallel-rdp es el backend
por defecto y ahi PD se ve bien, asi que esto no bloquea nada; queda apuntado como el caso de
prueba mas util que tenemos para el rasterizador propio.

### Lo que si conviene mirar despues

El latido en PD dice `audio: silencio 14-17% descartadas=53550`. En SM64 el silencio esta en
0,15-0,54 % y los descartes en 0, o sea que esto es de PD: produce audio a rafagas mas grandes
que el colchon. No es el mismo fallo que se arreglo en el DAC del AI; es el sumidero del
anfitrion quedandose corto con este juego.

## 2026-09-03 — el cuelgue intermitente de Perfect Dark: dos fallos, ninguno de PD

Desde que el enlace de bloques del dynarec alcanza al codigo mapeado por TLB (PD ejecuta desde
`0x70000000`), Perfect Dark se descarrilaba en **~1 de cada 8 arranques**. Bisectado por
configuraciones, con 1200 intercambios de buffer y clasificando por CODIGO DE SALIDA (0 = bien,
124 = colgado, 139 = fallo del anfitrion), nunca por reloj de pared:

| Configuracion | Resultado | Pared |
|---|---|---|
| por defecto (con enlace TLB) | 8 de 12 bien, 4 colgadas | 9-10 s |
| `KESTREL_THROTTLE=1` | 8/8 | 26 s |
| `KESTREL_THREADS=0` (lockstep) | 8/8 | 54 s |
| `KESTREL_JIT_NOTLBLINK=1` | 15/15 | 38 s |
| `KESTREL_JIT_NOTLBSTATIC=1` (solo ITC) | 10/10 | 40 s |
| `KESTREL_JIT_NOFAST=1` | 6/6 | 15 s |
| `KESTREL_JIT_CHAIN=8` | 2 de 6 bien | 10 s |

O sea: **todo lo que estabiliza, frena al hilo de CPU o aprieta el acoplamiento CPU<->RSP.**
Ningun diagnostico que cambie la planificacion de hilos lo reproduce (con el servidor de
telemetria + `KESTREL_HEARTBEAT` salieron 12/12 limpias), asi que hubo que ir por canarios que
no tocan el reparto de tiempo.

### Fallo 1 — la ranura vacia de la ITC era una VA que el guest puede pedir

`struct ItcEnt { u64 va = 0; ... }`. Pero 0 es un destino legitimo: un `jr` con el registro a
cero. Y `itcIndex(0) == 0`, asi que la sonda emitida —- que solo compara `va` -— casaba con la
ranura 0 vacia y ejecutaba `jmp [rdx+8]` con `code == 0`. Reproducido tal cual:

```
==== HOST EXCEPTION 0xc0000005 at host RIP 0000000000000000 ====
  access violation: EXEC at host addr 0x0
  guest pc=0x0000000000000000 nextPc=0x0000000000000004 halted=0
```

La ranura vacia lleva ahora la misma VA imposible que ya usaban las guardas de enlace estatico
(`kNoLink = 1`: impar, y por tanto nunca igual a un pc alineado a instruccion). Semantica de HW:
un `jr` a 0 tiene que buscar instruccion en la VA 0 y tomar la excepcion que toque, jamas saltar
al host 0. Arreglado, pero **no era el cuelgue**: seguia colgandose.

### El cuelgue no tenia corrupcion de memoria

Con `KESTREL_CODEWATCH=20 KESTREL_DMAGUARD=0x1000:0x100000` armados, la corrida 6 de 20 se colgo
y el canario de codigo **no salto ni una vez**: el codigo de PD estaba byte a byte igual todo el
rato, y el guardian de DMA solo vio DMAs legitimos de salida de audio. Ni excepcion previa, ni
presupuesto de RSP agotado. Livelock puro.

El volcado del watchdog (`KESTREL_WATCHDOG=15`, elegido para que apenas dispare en una corrida
sana de ~13 s pero vuelque dos veces dentro de un cuelgue) lo describio entero:

```
[wdog] retired=1366752580 (+1366752580) pc=70003590 sp_status=00000203 sp_pc=144
       rspRun=0 rspBusy=0 rspKick=0 rdpBusy=0 rdpQ=0 mi_intr=2d mi_mask=3f
[wdog] status=02008080 cause=1000042c epc=0000000070003678 count=51776788 compare=00000000
[mi]   VI      1748       144   1603          0        <- raise / fundidas / clear
...
[wdog] retired=1605598303 (+238845723) pc=700035f0 ...
[mi]   VI      2054       450   1603          0
```

Es decir: `retired` sube (la CPU **no** esta bloqueada), todo el RCP parado, las VI se levantan y
se funden pero el contador de `clear` **congelado en 1603** -— nadie las atiende -— porque
`status` trae **IE=0** y **CU1=0**, y `cause` dice excepcion 11 (Coprocessor Unusable) con
`epc` DENTRO del bucle en el que gira la pc. Un hilo del invitado con el contexto mal, girando
con las interrupciones cerradas: nada puede desalojarlo.

### Fallo 2 — `kPaceSlack` llevaba calibrado contra un hilo de CPU cuatro veces mas lento

La holgura del regulador CPU<->RSP estaba en **1 M instrucciones**. El propio codigo ya lo
denunciaba: con esa holgura `rcpPace` no frena hasta que la CPU adelanta 1 M instrucciones al
RSP, o sea el hilo de CPU corre al ~1200 % de la velocidad del N64 y deja a la CPU emulada
~11 ms —- dos tercios de campo -— por delante de una tarea de RSP en vuelo. Con ese adelanto el
juego puede reescribir la display list que el RSP todavia esta leyendo. **El hardware no da esa
holgura**: en el N64 los dos relojes van acoplados por el bus.

La unica holgura que justifica el EMULADOR es la granularidad del dynarec: una cadena de bloques
enlazados retira hasta `jit::kGuardMaxOps` instrucciones sin volver al bucle del sistema, asi que
por debajo de eso el freno no puede mandar. El 1 M se habia calibrado por rendimiento en
2026-09-01, y el comentario que lo acompanaba avisaba: *"el optimo se MUEVE con la velocidad del
hilo de CPU"*. El enlace de bloques sobre codigo TLB acelero ese hilo ~4x y nadie recalibro.

Bajado a **4096 = `jit::kGuardMaxOps`**:

- Perfect Dark: **40 arranques limpios de 40** (16 + 24). Antes fallaba ~1 de cada 8; la
  probabilidad de 40 limpias por azar es ~0,5 %.
- Coste (SM64, 300 intercambios, minimo de 3), 4096 vs 1 M:
  `prdp-jit` 5,08 s vs 5,06 s -— ruido, y misma fidelidad (3,32 vs 3,31 campos VI por
  intercambio); `threaded-jit` 5,18 s vs 5,06 s (-2,4 %).
- El despenadero de las calibraciones anteriores **ha desaparecido**: la curva es plana de 4 K a
  1 M. Lo que antes castigaba la holgura corta era el hilo de CPU comiendose el nucleo del
  worker, y eso ya se arreglo por otro lado.

### La guarda `rsp.running` del camino rapido SIGUE haciendo falta

Con el regulador ya apretado se volvio a medir `KESTREL_JIT_NORSPGUARD=1`. Sin la guarda PD se
cuelga **1 de cada 16** arranques, y SM64 emite **1804 campos VI por 300 intercambios** en vez de
1024 (el oraculo lockstep da ~4,09 campos por intercambio; 3,41 con guarda, 6,01 sin). La pared
"mejora" de 5,50 s a 4,51 s justo por eso: son campos girados, no trabajo hecho. Se queda.

### Herramienta arreglada de paso

El volcado `[wdog] code @` traducia la pc con `& 0x1fffffff`. Para una VA mapeada por TLB
(`0x70003590`) eso da `0x10003590`, fuera de la RDRAM, y salian cinco ceros —- que se leen como
un colchon de NOPs que no existe. Ahora traduce por `cpu.tlbProbePhys`, igual que la CPU.

Puertas: `gate_all` 375 s, `gate_prdp` 307 s, 15/15 + prdp 3/3, `regress=0` y `nodump=0` en los
dos krom (mean_exact interp 88,71 / prdp 89,26), md5 de sm64 sin cambio en los siete modos.

## 2026-09-04 — los cuatro mandos, con accesorio de verdad y dialogo nuevo

Hasta ahora el emulador tenia UN mando. El joybus servia el canal 0 y contestaba
`NO_DEVICE` (0x80) en 1..3, el Controller Pak era una variable global y el Rumble Pak no
existia. Cerrado entero:

**Nucleo (joybus, `src/core/memory.cpp`).** `padPort[4]`, cada uno con `connected`,
`accessory`, `mempak`/`mempakPath`, `rumble`. `pifProcessJoybus` sirve cualquier puerto
conectado (`if(channel < 4)`), no solo el 0; un puerto desconectado contesta ausente y el
resto del paquete sigue. El estado (cmd 0x00/0xFF) devuelve `0x05,0x00, accessory?1:0` —
CONT_CARD_PULL (0x02) queda a cero a proposito: los dos bits juntos son «el pak acaba de
cambiar» y la libultra lo traduce en `PFS_ERR_NEW_PACK`.

**Accesorio por puerto.** Cable de direcciones `(bloque << 5) | CRC5(bloque)`, bloques de
32 B, 64 KiB de espacio. Controller Pak = 32 KiB de RAM en las direcciones bajas, con su
`.mpk` propio por puerto (`rom.mpk`, `rom.mpk2`..`rom.mpk4`), formateado y resuelto en
`attachSaveFile` para los cuatro aunque ahora mismo no lleven pak — el accesorio se puede
cambiar en marcha desde el dialogo, y sin la ruta resuelta ese puerto volcaria un pak
recien formateado encima del fichero que ya existia. **Rumble Pak** = sin RAM: la ventana
0x8000-0x8FFF se lee como 0x80 (identificacion) y una escritura a 0xC000 enciende o apaga
el motor. Ranura vacia = CRC de datos **invertido**, que es como el SDK detecta la
ausencia.

**Entrada (`src/video/present.cpp`).** Bucle de los cuatro puertos. Cada uno elige su
aparato: teclado, un mando concreto de Windows, o el mixto de siempre (teclado + primer
mando) que sigue siendo el defecto del puerto 1. Mando via `glfwGetGamepadState`
(stick izquierdo → analogico con zona muerta 0,2 y tope ±80, stick derecho → botones C,
gatillos como ejes). **La identidad del aparato se guarda por NOMBRE, no por indice**:
Windows renumera los joysticks al enchufar y desenchufar, y un indice guardado apuntaria
al mando de otro jugador. Un nombre sin resolver se reintenta en cada barrido (0,5 s), asi
que enchufar el mando a medio juego reclama su puerto solo. Las consultas de GLFW solo son
seguras desde el hilo del presentador, por eso hay instantanea publicada
(`rt::joyName`/`rt::joyGen` bajo `rt::joyMx`) para la UI.

**Vibracion real.** GLFW no tiene API de rumble: en Windows se carga XInput a mano
(`xinput1_4` → `1_3` → `9_1_0`) y se llama `XInputSetState` solo en el flanco. GLFW no da
la ranura de XInput, asi que se asume que el n-esimo gamepad es la ranura n — aproximacion
documentada que como mucho pierde vibracion, nunca entrada.

**Perfil y lanzador.** Un mapeo por conector (`pad` para el 1, `pad2`..`pad4`, que el
lanzador conserva sin tocar), mas `padN_on` / `padN_acc` / `padN_dev`. `toEnv` emite
`KESTREL_PAD1..4` (ficheros de mapeo), `KESTREL_PADDEV1..4` (`auto` / `kb` / nombre),
`KESTREL_PADS` (4 digitos de conectado) y `KESTREL_PADACC` (4 digitos de accesorio).
`tools/launcher/kestrel_launcher.py` escribe exactamente lo mismo.

**Dialogo nuevo** (`src/ui/menu_win32.cpp`). Fuera el bloque de texto explicativo. Arriba:
selector **Mando 1-4**, casilla Conectado, combo Accesorio (nada / Controller Pak / Rumble
Pak) y combo Aparato, que lista los mandos que Windows tiene enchufados AHORA (por nombre,
refrescado con un timer de 0,5 s; un mando guardado y desenchufado se queda como «(no
conectado)»). A la izquierda, el mando de N64 **dibujado con GDI** (tres asas, cruceta,
START rojo, B verde, A azul, diamante C amarillo, stick, L/R/Z) y **clicable**: pinchar un
boton selecciona su fila y arranca la captura de tecla o despliega el combo de boton segun
que columna este activa. La columna TECLA se apaga si el aparato no es teclado, y la
columna MANDO si es teclado.

Puertas: `gate_all` 356 s, `gate_prdp` 370 s, 15/15 + prdp 3/3, `regress=0` y `nodump=0` en los dos
krom (mean_exact interp 88,71 / prdp 89,26), md5 de sm64 sin cambio en los siete modos.

---

## 2026-09-04 — Pantallas en negro: region del cartucho + CIC-NUS-6105

Dos arranques rotos, dos causas distintas, ninguna parcheada por juego.

### 1. osTvType por region del cartucho (Perfect Dark en negro)

libultra publica el estandar de TV en RDRAM `0x300` y en `s4` al entregar el control al
juego (`OS_TV_PAL=0`, `OS_TV_NTSC=1`, `OS_TV_MPAL=2`). El arranque HLE lo dejaba fijo, asi
que a Perfect Dark (Europa, pais `'P'`) se le decia NTSC y `mainInit()` se quedaba en su
`while(1)`. Ahora sale del byte de pais de la cabecera, y con el la tasa de campos
(PAL 50 Hz, NTSC/MPAL 59,94 Hz) y el reloj de video del RCP que fija la tasa del DAC del AI
(NTSC 48 681 812, PAL 49 656 530, MPAL 48 628 316 Hz; `dac = vidClock / (dacrate + 1)`).
El arranque lo dice en claro: `[system] region 'P' -> PAL (50.00 campos/s)`.

### 2. Las dos cosas que el IPL3 de CIC-NUS-6105 hace y ningun otro (DK64 en negro)

Los cartuchos firmados con 6105 (Perfect Dark, Donkey Kong 64, Majora's Mask,
Banjo-Tooie...) comparten IPL3, y ese IPL3 deja dos rastros en RDRAM que los juegos
comprueban. Si faltan, el juego gira para siempre — no es un cuelgue del emulador:

- **Etapa 1 — imagen del propio IPL3 en RDRAM baja.** El bucle `lw`/`sw` de DMEM
  `0x524..0x538` copia DMEM `0x554..0x888` a RDRAM `0x004..0x338` (sesgo fijo `-0x550`) y
  sigue ejecutando desde la copia. Son los mismos bytes que el cartucho lleva en ROM
  `0x554..0x888`. **Perfect Dark** gira salvo que `*(0xA00002E8) == 0xC86E2000`, que no es
  mas que la palabra de codigo ROM `0x838` cayendo en RDRAM `0x2E8`.
- **Etapa 2 — microcodigo de arranque del RSP.** El IPL3 arranca el RSP (`SP_STATUS = 0xAD`,
  DMEM `0x550`) con un microcodigo que **descifra por XOR dentro de IMEM a partir del codigo
  de IPL2 que la PIF ROM dejo ahi** (tabla de claves en DMEM `0x84`). Ese microcodigo hace
  DMA de RDRAM `0x1E8` (`0x1F0` bytes) a IMEM `0x120` y emite UN solo DMA de escritura con
  paso: `SP_DRAM_ADDR = 0x2FB1F0`, `SP_WR_LEN = 0xFE817000` (largo 8, cuenta 24, salto
  `0xFE8`) — 24 filas de 8 bytes separadas `0xFF0`. **Donkey Kong 64** comprueba la fila 3:
  `*(0xA02FE1C0) == 0xAD170014`, o sea RDRAM `0x200` (el `sw s7,0x14(t0)` del IPL3) en
  `0x2FB1F0 + 3*0xFF0`.

El microcodigo no se puede ejecutar de verdad sin la PIF ROM (su texto sale del residuo de
IPL2 en IMEM, que un arranque HLE nunca deja), pero su efecto en memoria es **fijo**, asi
que se reproduce el efecto. Se aplica a **cualquier cartucho 6105**, y tambien en la ruta
`KESTREL_LLE_IPL3` (donde el IPL3 real rehace la etapa 1 con los mismos bytes). Con esto
desaparece el unico hardcode a un juego que quedaba en el arranque (la palabra `0x2E8` de
Perfect Dark, que se escribia si la ROM contenia la magia en `0x838`).

Ademas se escribe RDRAM `0x3F0` con el tamano de RDRAM: es donde la autoprueba de RDRAM de
la PIF deja el resultado, y el IPL3 de 6105 lo lee (`lw t1,0xf0(t0)` con `t0 = 0xA0000300`)
para rellenar `osMemSize` en `0x318` — sin el, la ruta LLE calcula `osMemSize = 0`.

### Estado

- **Donkey Kong 64 arranca y renderiza 3D** (intro de las lianas, «DONKEY KONG IS HERE!»).
- **Perfect Dark arranca y renderiza** la intro sin ningun hardcode.
- Retirado tambien el volcado de depuracion especifico de PD del manejador de `KESTREL_BP`
  (escaneo de `0x70000000..0x70002000` buscando `c86e`); en su sitio queda un volcado
  generico del bloque de arranque `0x2E0..0x320`.

### Herramienta que lo resolvio

Ejecutar el IPL3 real (`KESTREL_LLE_IPL3=1`) con `KESTREL_MEMDUMP` y comparar con el
arranque HLE: bajo LLE aparecian `0x200 = AD170014` y `0x2E8 = C86E2000` pero `0x2FE1C0`
seguia a cero — eso separo la etapa 1 (que el IPL3 sabe hacer solo) de la etapa 2 (que
necesita la PIF ROM). El disassembly de DMEM `0x7D4..0x86C` en el punto del arranque del RSP
da los tres inmediatos del DMA (`0x2FB1F0`, `0xFE817000`, `0x1E8`) sin ambiguedad.

### Medida de timing en la intro de las lianas (DK64)

Es la prueba que mejor separa un timing bueno de uno malo, porque la camara sube por la
liana a ritmo fijo y cualquier desajuste CPU/RSP/RDP se ve como campos de video de mas.
200 intercambios de buffer, mismo `origin` final (`0x0ca500`) en los dos modos, o sea que
paran en el mismo punto de la intro:

| Modo | Campos VI | Campos/intercambio | Sincronizaciones RDP | Pared |
|---|---|---|---|---|
| threaded-jit (uso real) | 610 | 3,05 | 156 | 3 s |
| interp lockstep (oraculo) | 597 | 2,99 | 169 | 22 s |

2,2 % de separacion frente al oraculo determinista, en la linea de Perfect Dark
(4,08 vs 4,09). El arranque anuncia `[boot] CIC detected: 6105 (seed 0x91)` y
`[system] region 'E' -> NTSC (59.94 campos/s)` — la ROM USA de DK64 lleva pais `'E'`,
que es NTSC-America, no Europa.

## 2026-09-04 — Particularidades de cada CIC-NUS

El arranque rapido (`CPU::fastBoot`, `src/cpu/cpu.cpp`) ya no trata a todos los cartuchos
igual. La tabla `kCicTable` identifica el chip por el **CRC32 de la imagen IPL3** del propio
cartucho (ROM `0x40..0xFFF`) y de ahi salen cuatro cosas que el juego puede ver.

Fuente: tabla CIC-NUS y PIF-NUS de n64brew (wikitexto crudo), `cic/cic.cpp` de ares para
las semillas y `CRegisters::Reset` de Project64 para el estado de registros posterior al
IPL3. La eleccion NTSC/PAL **no** sale del CRC: 6102/7101, 6103/7103, 6105/7105 y 6106/7106
comparten imagen IPL3 byte a byte, asi que quien decide es el byte de pais del cartucho.
6101 (solo NTSC) y 7102 (solo PAL) si son imagenes distintas.

| CIC (NTSC/PAL) | CRC32 IPL3 | Semilla | Direccion de arranque |
|---|---|---|---|
| 6101 / — | `6170A4A1` | `0x3F` | cabecera (`u32@0x08`) |
| 6102 / 7101 | `90BB6CB5` | `0x3F` | cabecera |
| — / 7102 | `009E9EA3` | `0x3F` | **fija `0x80000480`** (ignora la cabecera) |
| 6103 / 7103 | `0B050EE0` | `0x78` | cabecera **− 1 MB** |
| 6105 / 7105 | `98BC2C86` | `0x91` | cabecera |
| 6106 / 7106 | `ACC8580A` | `0x85` | cabecera **− 2 MB** |
| 5101 (Aleck 64) | `0E018159` | `0xAC` | cabecera **− 1 MB** |

La resta se aplica **tanto al destino del DMA del primer megabyte como al salto**: el IPL3
lleva una sola direccion de arranque en un solo registro, y cargar el juego en otro sitio
lo dejaria sin cargar donde fue enlazado. Es ofuscacion anticopia, no una funcion.

### Estado de registros con el que el IPL3 entrega el control

La mayoria de esos valores no son «inicializacion»: son los restos del checksum que el IPL3
hace sobre el primer megabyte del cartucho, asi que dependen del **chip** (semilla y magia
distintas) y de la **region** (la PIF ROM contra la que corre el checksum no es la misma).
Para 6102 el checksum del IPL2 es `0xA536C0F1D859` y el arranque deja literalmente
`a0 = 0xA536` y, en PAL, `a1 = 0xC0F1D859` — o sea que la tabla es estado real, no invento.
Se dan v0/v1/a0/a1/at/t4/t5/t6/t7/t9 por CIC y region, mas los comunes: `s4 = osTvType`,
`s6 = semilla del CIC`, `s7 = osVersion` (6 en PAL, 0 en NTSC), `t3 = 0xA4000040` (el propio
IPL3 aun en DMEM), `ra = 0xA4001550` NTSC / `0xA4001554` PAL (la cola del IPL3 PAL cae una
instruccion mas alla). En RDRAM `0x310` se escribe el `osCicId` ya en la variante correcta
de region (6102 o 7101, etc.).

De 6101, 7102 y 5101 solo se conoce la semilla con certeza, asi que ahi no se toca el resto
de registros en vez de inventarlos.

### Residuo del 6105 en IMEM

El arranque en dos etapas del 6105 (ver seccion anterior) deja la primera palabra del
microcodigo en IMEM `0x004`, y los juegos de ese chip la leen: `0x8DA807FC` en NTSC y
`0xBDA807FC` en PAL. Depende de la region porque el microcodigo es el XOR de la tabla de
claves del IPL3 con el residuo del IPL2 en la PIF ROM, y la PIF ROM PAL es otra imagen.

### Lo que se decidio NO emular

La PIF escribe region y semillas en la palabra de arranque de PIF RAM `0xBFC007E4`, pero
`memSwapSecrets()` de la PIF de ares muestra que el hardware **las vuelve a esconder** antes
de que corra el IPL3. Emular esa escritura seria dar al juego algo que en consola no ve.

## 2026-09-04 -- Trucos (GameShark)

`src/core/cheats.{hpp,cpp}` mete el motor de codigos que faltaba (GAPS P1 #4). Detalle
completo, formato de fichero y tabla de familias en `docs/CHEATS.md`; aqui las dos
decisiones que son semantica de hardware y no gusto:

- **El ritmo es el campo de video.** El cartucho de verdad sustituia el arranque y
  enganchaba la interrupcion del VI: su motor recorria la lista una vez por campo. Por eso
  el enganche esta en `System::run`, pegado al `memory.viTick()` que cierra el campo y
  dentro de `coreMutex`, y no en el intercambio de buffer del juego. Un juego a 20 fps
  recibe el parche tres veces por fotograma suyo, que es lo que hace que "vidas infinitas"
  gane la carrera contra el codigo que las resta.
- **El nibble alto de la direccion es el segmento MIPS.** `0x80xxxxxx` = KSEG0 = escritura
  cacheada (por `CPU::pokePhysCoherent` -> `dcWrite`): el juego la ve al momento aunque la
  linea tarde en bajar a la RDRAM. `0xA0xxxxxx` = KSEG1 = escritura sin cache, directa a la
  RDRAM, que **no** invalida la linea de D-cache -- igual que un store KSEG1 del VR4300. Esa
  asimetria es justo por lo que las familias A0/A1 se publican como parche unico de
  arranque, y esta fijada como invariante en `test/cheat_test.cpp`.

Familias: 80/81 (byte/media palabra por campo), A0/A1 (una vez al arrancar), D0-D3
(condiciones sobre la linea siguiente) y el repetidor `50 00CCII 0000VV`. Las lecturas de
las condiciones usan una vista coherente que **no toca la cache** (ni rellena ni desaloja):
armar un truco no debe mover el estado del juego mas alla de las escrituras que pide.

No se aplican, y se avisa al cargar: 88/89 (solo mientras se pulsa el boton fisico del
propio cartucho, que esta maquina no tiene) y CC/DE/EE/FF (control interno del aparato).

Fuente: `.cht` con el nombre de la ROM al lado de la ROM, o `KESTREL_CHEATS`; opcion nueva
en `tools/launcher/options.py` (Cartucho -> Fichero de trucos), o sea tambien en el menu de
la ventana. Sin fichero no hay motor ni coste.

Verificado: `cheat_test` ALL PASS (parser, las cuatro familias, condiciones, repetidor,
truco apagado, asimetria cache/sin cache) y end-to-end sobre SM64 -- con `80700000 0064` +
`A0700010 0077` la RDRAM queda con `64` en `0x700000` reescrito en cada campo y `77` en
`0x700010` una sola vez.


## 2026-09-04 -- ROMs comprimidas (.zip, .gz) y tests unitarios rescatados

Hueco P1 #5. `src/core/archive.{hpp,cpp}`: el emulador abre una ROM metida en un zip o en un
gzip tal como se descargo. Detalle completo en `docs/ROMS-COMPRIMIDAS.md`.

Dos decisiones que no son de gusto:

- **DEFLATE propio en vez de zlib.** El `.exe` que se publica se enlaza con
  `-DKESTREL_STATIC=ON` para ser autocontenido (`docs/distribucion.md`). Anadir zlib al
  enlace por un descompresor que corre UNA vez al abrir la ROM no sale a cuenta: son ~200
  lineas, el formato lleva treinta anos congelado (RFC 1951) y el decodificador usa la forma
  compacta de la propia norma (cuentas por longitud + simbolos ordenados), sin tablas.
- **El desempaquetado va antes de normalizar el orden de bytes.** Lo que hay que reconocer
  como z64/n64/v64 es la ROM de DENTRO, no la cabecera del contenedor; asi un `.v64` metido
  en un zip se reordena igual que si estuviera suelto.

Ademas: dentro del zip se elige la entrada con extension de ROM (o la mas grande, porque el
reparto tipico es ROM + `.txt`), se comprueba el CRC-32 que traen los dos formatos, y 7z y
rar se rechazan **con su nombre** en vez de con "unrecognized ROM magic", que hacia pensar
que la ROM estaba rota. Los ficheros que viven al lado de la ROM (`.eep/.sra/.fla/.mpk`,
`.st0`-`.st9`, `.cht`) ignoran la extension del contenedor, asi que `mario.z64.gz` y
`mario.z64` comparten partida guardada. El lanzador lee la cabecera dentro del contenedor y
el dialogo de abrir ROM ya ofrece `*.zip;*.gz`.

Verificado: `archive_test` ALL PASS (tres clases de bloque DEFLATE, copia solapada de
distancia 1, flujo truncado, CRC-32, gzip con FEXTRA/FNAME/FCOMMENT, eleccion de entrada,
zip guardado, CRC roto en ambos formatos, metodo 12, firmas 7z/rar, ROM cruda intacta) y SM64
arrancado 60 campos desde **siete** envoltorios distintos, los siete con el md5 del oraculo
`466282775dbd0ac084946558a1c30771`.

**Tests unitarios rescatados.** Al tocar `memory.cpp` salieron a la luz dos tests podridos que
ningun gate compilaba: `rsp_test` (`sp_status` paso a `std::atomic` cuando el RSP se fue a su
hilo -> copia implicita borrada; y el test daba por hecho que escribir CLEAR_HALT ejecuta el
microcodigo, cuando en Lockstep esa escritura solo ARMA el nucleo y quien lo avanza es
`System::run`) y `save_test` (el Controller Pak se mudo a `padPort[i]` al implementar los
cuatro mandos). Los dos arreglados, y **`gate_all.sh` compila y ejecuta ahora los cuatro**
(`rsp_test`, `save_test`, `cheat_test`, `archive_test`) antes de systemtest: cuestan segundos
y era justo la falta de mirarlos lo que los pudrio.


## Direcciones salvajes: el invitado puede pedir cualquier cosa (2026-09-04)

Hueco de robustez de `docs/GAPS.md`: **el camino `fastmem` del JIT**. Cuando el codigo de
invitado se descarrila -- un puntero corrupto, un indice negativo, una estructura leida con el
desplazamiento equivocado -- la direccion que llega a la memoria no se parece a nada legal, y
el emulador tiene dos obligaciones: levantar la excepcion que levantaria el VR4300, y no
tocar ni un byte fuera de su propio bloque de RDRAM.

Auditados los caminos, las guardas YA estaban: el camino rapido del JIT (`src/cpu/jit.cpp`)
solo se dispara tras comprobar forma canonica de ckseg0 (`rax = a + 0x80000000` y
`cmp < 0x20000000`), alineacion, modo kernel (KSU/EXL/ERL), tope `jitRdramSz`, la guardia de
escritura `CPU::stGuard` y la etiqueta+valido de la linea de D-cache en una sola comparacion;
`dcFill`/`dcFlush`/`dcMiss` caen a un bucle byte a byte si `base + 16 > rdram.size()`, y
`Memory::spDma` corta cuando el destino se sale. Lo que faltaba no era codigo sino **prueba**:
nadie ejercitaba esas guardas, y menos aun comprobaba que el dynarec y el interprete se
comportan igual al dispararlas.

**`test/wildmem_test.cpp`** (objetivo `wildmem_test`, ya dentro de `gate_all.sh` con los otros
cuatro). Monta un `Memory` real, escribe un programa MIPS diminuto en la RDRAM en ckseg0 y lo
corre DOS veces sobre CPUs recien reseteadas: una a pasos de `CPU::step()` y otra despachada
por `CPU::jitTryBlock()`. De cada corrida saca ExcCode, EPC, BadVAddr y el registro destino, y
exige que las dos coincidan. Ademas fija el valor absoluto alli donde la semantica del VR4300
no admite discusion (AdEL=4 / AdES=5 con la VA salvaje **entera** en BadVAddr, TLBL=2 /
TLBS=3 en los segmentos mapeados); los casos cuyo resultado es de bus abierto -- una lectura
pasada del final de la RDRAM -- solo se cruzan entre motores, que es lo unico honesto: ahi lo
que se afirma es "no revienta el anfitrion y los dos motores dicen lo mismo", no un numero
inventado.

Los 27 casos: desalineacion en `lw`/`lh`/`sw`/`ld`/`sd` y tambien en `lwc1`/`swc1` (el camino
`fastmem` tiene entradas propias para COP1); accesos justo pasados el final de la RDRAM, en la
ultima palabra valida, a caballo del limite, en la cima de ckseg0 y con `ldc1`; kuseg y ksseg
sin ninguna entrada de TLB; huecos de MMIO en kseg1 (RI, hueco del RCP, pasado el PIF, DMEM);
punteros de 64 bits NO canonicos (`0x12345678_80000000` y un ckseg0 sin extender el signo,
que es lo que deja un registro medio pisado); y los mismos limites con una RDRAM de 4 MB, que
de paso comprueba que `jitRdramSz` sigue al tamano real y no a un 8 MB fijo.

Resultado: **el dynarec compilo y corrio los 27** (ninguno declinado) y coincide con el
interprete en todos. Un detalle que salio del propio test y que conviene recordar: el
programa se carga en `0xFFFFFFFF_80001000`, no en `0x80001000` -- en modo kernel de 32 bits
toda VA valida es la extension de signo de sus 32 bits bajos, asi que un `pc` de 64 bits sin
extender falla en el PROPIO FETCH con AdEL antes de ejecutar nada. El emulador lo hacia bien;
el test estaba mal, y esa es exactamente la clase de detalle por la que el oraculo es el
interprete y no lo que uno espera.


## Una consola de serie tambien es una consola: 4 MB de verdad (2026-09-04)

La N64 de fabrica trae 4 MB de RDRAM. El Expansion Pak sube a 8. Kestrel llevaba desde el
principio el codigo para las dos -- `Memory::reset(bool expansionPak)` reserva
`RDRAM_SIZE_EXPANDED` (0x0080'0000) o los 0x0040'0000 de serie, y `CPU::fastBoot` copia
`rdram.size()` a las dos ventanas por las que el invitado pregunta el tamano (`osMemSize` en
RDRAM 0x318 y la copia de 0x3F0) -- pero `System::init` la llamaba siempre con `true`. O sea:
la mitad baja del parque de juegos nunca se habia visto tal y como se ve en una consola sin
el cacharro.

Ahora se elige: `KESTREL_RDRAM=4|8` (por defecto 8; el lanzador lo saca como "Memoria RDRAM"
en el grupo Cartucho, con relanzado obligatorio porque el tamano se fija al arrancar y hay
punteros del invitado colgando de el) y `System::init` lo anuncia en el log:
`[system] RDRAM 4 MB (consola de serie)`. Lo unico que hace falta subrayar es POR QUE no
puede ser en caliente: cambiar el tamano a medio juego no es "reasignar un vector", es mover
el suelo debajo del mapa de memoria del invitado -- su heap, sus framebuffers y sus tablas
estan colocados a partir de lo que le dijimos al arrancar.

La prueba no es del emulador sino del invitado, que es la unica que vale:

| ROM | RDRAM | md5 del framebuffer |
|-----|-------|---------------------|
| Super Mario 64 | 8 MB | `466282775dbd0ac084946558a1c30771` |
| Super Mario 64 | 4 MB | `466282775dbd0ac084946558a1c30771` |
| Donkey Kong 64 | 8 MB | `5683d22e66c393d50602b648b1ec660d` |
| Donkey Kong 64 | 4 MB | `0d05190dc6efd189dcee266cc108827a` |

SM64 no nota nada porque nunca pidio el Expansion Pak: mismo fotograma bit a bit con la
mitad de memoria, que es exactamente lo que tiene que pasar. DK64 si: a 4 MB su fotograma es
OTRO, y al volcarlo se lee "N64 EXPANSION PAK NOT INSTALLED / THE N64 EXPANSION PAK ACCESSORY
MUST BE INSTALLED IN THE N64 FOR THIS GAME". Es decir, el numero que escribe `fastBoot`
llega hasta el codigo del juego y le cambia el comportamiento -- no es una bandera decorativa
en la configuracion. Las lecturas fisicas por encima del final de la RDRAM ya devolvian 0 y
el `jitRdramSz` del dynarec ya se toma del tamano real por compilacion, ambas cosas cubiertas
con casos propios de 4 MB en `wildmem_test`; y la cabecera del savestate lleva `rdramSize` y
rechaza cargar en una maquina de otro tamano, asi que no hay forma de cruzar partidas.


## El TLB vacio no falla: delata (2026-09-04)

`KESTREL_EXCODD` existe para pillar el primer sintoma cuando el invitado se descarrila: filtra
las excepciones que un juego sano toma a millones y vuelca contexto entero (EPC, BadVAddr, ra,
sp, las 8 palabras que hay REALMENTE en la direccion que fallo y los 32 GPR) en las que no
deberia ver nunca. El filtro era por numero: `excCode > 3 && != 8`, o sea fuera Int, TLBMod,
TLBL, TLBS y Syscall.

Meter TLBL/TLBS en la lista a secas seria falso -- un juego que use el TLB los toma a punta
pala y son trabajo normal. Pero eso depende del ESTADO, no del codigo de excepcion: si el TLB
no tiene ni una entrada con el bit V puesto, ninguna traduccion mapeada puede acertar jamas, y
entonces un TLBL no es paginacion bajo demanda sino un puntero que se fue a kuseg por
accidente -- exactamente igual de interesante que una instruccion reservada. Asi que el filtro
pregunta por el TLB (`tlbAnyValid()`, un barrido de 32 entradas que sale al primer V y que solo
corre en excepciones de TLB) en vez de por el nombre del juego.

Comprobado en los DOS sentidos, que es lo unico que distingue un filtro de un ruido nuevo:
`wildmem_test` con `KESTREL_EXCODD=4` pasa de cero volcados de TLB a cuatro de code=2 y dos de
code=3 (sus casos de kuseg/ksseg sin mapear, con el TLB virgen); Super Mario 64 y Donkey Kong
64 a 120 campos no ganan ni un aviso: siguen con los mismos code=11 de siempre.


## IPL3 real contra IPL3 fingido: que dicen los cuatro cartuchos que hay (2026-09-04)

Kestrel sabe arrancar de dos maneras: fingiendo el RESULTADO del IPL3 (`fastBoot`, el defecto)
o ejecutando el IPL3 firmado que trae el propio cartucho (`KESTREL_LLE_IPL3=1`). La pregunta
pendiente era si lo segundo debia pasar a ser el defecto.

Primero hubo que arreglar el metodo. El A/B en el modo normal (con hilos) no vale para Perfect
Dark: cuatro arranques identicos dan cuatro md5 distintos. No es un fallo del emulador ni un
dato que se pueda leer -- PD anima el logo mientras arranca y el trabajo del RDP corre en un
hilo del anfitrion, asi que el campo 150 cae en un punto distinto del fundido cada vez. La
diferencia se ve exactamente donde se espera: un rectangulo dentro del logo, cero pixeles fuera.
En lockstep (`KESTREL_THREADS=0`) el mismo arranque repetido da el mismo md5 dos de dos, asi
que el A/B se hace ahi. SM64 y DK64 salen deterministas en los dos modos.

| ROM (CIC) | HLE | LLE |
|-----------|-----|-----|
| n64-systemtest | 0/3721 · 0/2 · 0/6 | 0/3721 · 0/2 · 0/6 |
| Super Mario 64 (6102) | `466282775dbd0ac084946558a1c30771` | igual |
| Donkey Kong 64 (6105) | `5683d22e66c393d50602b648b1ec660d` | igual |
| Perfect Dark (7105) | `2e385d1857e7388572b4bfc8677db154` | 1123 px de 331776, todos en el logo |

O sea: el IPL3 real deja la maquina donde la deja el HLE. Aun asi el defecto NO cambia, y el
motivo es lo que NO se ha medido: aqui hay cuatro ROMs que cubren tres CICs (6102, 6105, 7105)
y ninguna firmada 6101, 6103, 6106 o 5101. Sus IPL3 reales no los ha ejecutado nadie en este
emulador, y son justo los que se salen del guion (direcciones de arranque desplazadas, tablas
propias). Cambiar como arranca TODO el parque apoyandose en tres CICs es lo contrario de
verificar. `KESTREL_LLE_IPL3` se queda como opcion y la decision se revisa cuando haya ROMs de
esos CICs con que probarlo.

De paso queda cerrado el otro medio punto del hueco: el parche `0xC86E2000` ya no existe. Era
el ultimo hardcode a un juego del arranque y murio cuando el 6105 paso a reproducirse entero
(la copia de ROM 0x554..0x888 a RDRAM 0x004..0x338 y el DMA con paso del microcodigo) para
cualquier cartucho de ese CIC. Lo unico que queda en `cpu.cpp` es una tabla indexada por el CRC
de la imagen del IPL3, que identifica una FIRMA, no un titulo.

## La cinta no graba al jugador: graba lo que el juego lee

El hueco P2 #12 de `docs/GAPS.md` pedia dos cosas: grabar y reproducir entradas, y avanzar por
fotogramas. Lo primero tiene una decision de diseno delante que lo decide todo, y es *que* se
graba.

Lo facil es apuntar el mando del anfitrion una vez por cuadro. Es mentira. Hay juegos que
sondean el mando dos veces en un campo, otros que se saltan campos enteros, y el orden en que
se leen los cuatro conectores no lo decide el emulador sino el bloque de comandos que el juego
escribe en la PIF RAM. Una cinta hecha "por cuadro" reproduce bien mientras el emulador corra
igual y se desmonta en cuanto cambie el ritmo.

Lo que se graba aqui es la **respuesta al comando 0x01 del joybus**: leer botones, por
conector, con el mando del anfitrion y el inyectado por telemetria ya resueltos. Es la unica
frontera que el invitado percibe. El enganche es una linea en `Memory::pifProcessJoybus` justo
antes de escribir la respuesta, y por eso una pelicula vale igual al 30% que al 200% de
velocidad, y vale igual en interprete que en JIT: cuenta sondeos, no milisegundos.

El formato (`.k64m`, cabecera de 64 bytes y muestras de 5) esta en `docs/TAS.md`. Lo unico que
merece contarse aqui es que reproducir una pelicula de **otro cartucho** no se avisa: no se
reproduce. Inyectar los botones de otra partida produce basura que parece un fallo del
emulador, y perseguir ese fantasma cuesta mas que el mensaje: *"esta pelicula es de otro
cartucho: grabada con crc 635a2bff/8b022326, cargado ec58eabf/ad7c7169"*.

La prueba tenia que ser discriminante, y el primer intento no lo era. Grabar 60 campos de SM64
con START pisado da el mismo md5 que sin tocar nada -- START no hace nada tan pronto -- asi
que "graba y reproduce igual" no demostraba nada: una pelicula que no inyectase nada habria
dado el mismo resultado. Con 600 campos si se separa: sin botones `109c2277…`, grabando con
START `0ce65ed4…`, y la reproduccion (sin botones, solo la cinta) devuelve `0ce65ed4…` byte a
byte con las mismas 471 muestras.

La otra mitad, el avance por fotogramas, es `System::stepFields`: campos que el bucle corre
**aunque la pausa este puesta**, descontados al cerrar cada campo. El cuanto es el campo de
video y no la instruccion ni el volteo de buffer, porque el campo es donde el juego lee el
mando: un avance = una muestra de la pelicula, que es lo que hace falta para colocar una
pulsacion en el fotograma exacto. Teclas `P` (pausa) y `F` / `Shift+F` (uno / ocho campos) en
la ventana, `frame.advance` por telemetria, y `emu.status` publica por donde va la cinta.

Lo tercero salio de mirar el conjunto: guardar el estado y cargarlo rebobinaba el juego pero
no la cinta. Con eso, el uso que junta las dos herramientas -- rehacer un tramo -- se desviaba
siempre. El numero de sondeos consumidos es estado de la partida igual que la RDRAM, asi que
va en el estado guardado (seccion `MOVI`) y al cargar la pelicula se rebobina con el. Medido
por el contador que ahora publica `emu.status`: guardar en el sondeo 54, avanzar 40 campos
(74), cargar, y vuelve a 54. El precio es la version del formato de estado, que pasa de 4 a 5.


## El rebobinado no guarda fotos: guarda lo que cambio

Rebobinar es tener estados guardados hechos de antemano. Lo obvio -- una foto entera cada
pocos campos -- no vale: la RDRAM son 8 MB, treinta fotos por segundo es un gigabyte cada
cuatro segundos. Y lo obvio-pero-listo, comprimir cada foto, tampoco: comprimir 8 MB por
campo cuesta mas tiempo que el campo entero.

Lo que se guarda es **una sola foto viva** (la mas reciente, entera) y detras una pila de
**diferencias hacia atras**: cada entrada dice lo que hay que reescribir sobre la foto de
ahora para recuperar la de antes. Es la direccion en la que se rebobina -- del final de la
pila al principio -- y es la barata, porque entre dos campos seguidos el juego toca el
framebuffer que dibuja y sus estructuras vivas, no los 8 MB. En la pantalla de titulo de SM64
salen ~79 KB por paso de dos campos.

Lo que **si** cuesta, y por eso viene apagado: parar el RCP en cada foto (drenar el RDP,
terminar la tarea del RSP; el mismo serializado que en su dia costo 13-15 % en Perfect Dark),
recorrer el estado entero y compararlo. Medido en SM64/200 intercambios: +45 % de pared con
foto cada 2 campos y +24 % cada 6, contra una comparacion contra cero cuando esta apagado.
El camino para bajarlo esta claro y es otra tarea: seguimiento de paginas sucias, que exige
cazar todas las vias de escritura (CPU, DMA de RSP/PI/SI y el propio RDP).

La prueba que vale no es "el fotograma se ve igual" sino que el FUTURO sea el mismo: rebobinar
20 pasos desde el campo 240 devuelve el framebuffer del campo 200 exacto, y volver a correr
esos 40 campos da otra vez el framebuffer del 240 byte a byte. Si algo del estado hubiera
quedado viejo, el futuro que sale de el seria distinto aunque la foto pareciera igual.


## Cuantas instrucciones caben en un cuadro (el CPI)

Hay un numero del que cuelga todo lo demas y hasta ahora no se podia tocar: **cuantas
instrucciones retira la CPU en un campo de video**. No salia de ninguna medida, salia de un
atajo. El unico anclaje duro entre "instruccion retirada" y "ciclo de CPU" es el registro
Count de COP0: en el VR4300 avanza a medio reloj, y aqui avanzaba exactamente un paso por
instruccion. Eso equivale a decir que cada instruccion cuesta dos ciclos -- CPI 2 -- y de ahi
sale, sin que nadie lo decidiera, que un campo NTSC son 782 032 instrucciones (937 488 en PAL).

El VR4300 real no gasta dos ciclos por instruccion: gasta entre 1,2 y 1,4 en codigo de juego.
Con CPI 2 el emulador le da al juego **la mitad del presupuesto** que tenia en la consola. En
las escenas tranquilas da igual, porque el juego termina su cuadro y se queda girando en el
hilo ocioso; en las pesadas no da igual en absoluto: es la diferencia entre llegar al cuadro
y no llegar.

Ahora el ratio es un numero explicito (`KESTREL_CPI`). Nacio valiendo 2 -- lo de siempre bit a
bit -- y el 2026-09-08 el defecto se movio a **1,4** (ver el cierre de esta seccion); `KESTREL_CPI=2`
devuelve el modelo historico. Por dentro no es un decimal sino un entero en 1/256 de tick con el resto
acumulado, para que la division no se pierda por el camino y la secuencia de ticks sea la
misma en cualquier maquina y en los dos motores (interprete y JIT). Se lee una sola vez y de
ese unico numero cuelgan las dos cosas que dependen de el: el ritmo del reloj Count y el
presupuesto de instrucciones por campo del bucle principal. Derivar la segunda de la primera
no es elegancia: ya hubo una vez dos relojes distintos conviviendo en el emulador -- el tic
del VI contaba 750 k instrucciones por campo y la lectura de VI_V_CURRENT contaba 1,56 M -- y
un juego que mezclara interrupcion y sondeo veia dos campos por cada uno.

Que pasa al moverlo, medido en DK64: en el arranque, que iba **ahogado** a 5,5 campos por
cuadro mostrado (un cuadro cada 92 ms), pasa a 2,9. En regimen estable no cambia nada: los
campos por cuadro se quedan clavados en 2,00 con CPI 2, 1,4 y 1,25, y el trabajo por campo
converge al mismo numero en los tres; lo unico que sube es el ocio, del 62 % al 76 %. Eso es
justo lo que tiene que pasar y es la prueba de que no es un truco de velocidad: el juego ya
iba a sus 30 fps de diseno y el presupuesto extra se lo come el giro ocioso, igual que en la
consola. Solo se nota donde el presupuesto era de verdad el limitante.

El defecto NO se movio con una corazonada: se pusieron tres condiciones y se movio cuando las
tres se cumplieron (2026-09-08). Los tests de temporizacion de n64-systemtest pasan igual con
2, con 1,5, con 1,4 y con 1,25, o sea que no bloquean el cambio -- pero tampoco lo deciden.
(1) Mas juegos: SM64 en arranque da campos por intercambio 3,41 / 2,84 / **2,76** / 2,69 con
CPI 2 / 1,5 / 1,4 / 1,25 = rendimiento decreciente por debajo de 1,4, el mismo patron que
DK64. (2) Audio: con 1,4, `KESTREL_AUDIOSTAT` da `silencio=0 (0.00%)` y `cortas=0` igual que
con 2; el colchon minimo del anillo baja de 9504 a 3232 muestras de 22050, o sea se estrecha
pero no pasa hambre. (3) El numero elegido es 1,4, la parte ALTA del rango real del VR4300,
porque las dos medidas propias (DK64 1,19, PD 1,45) son cotas superiores de agresividad y lo
honesto con evidencia asi es quedarse arriba. Sigue siendo una aproximacion de un solo numero
a un CPI que en el silicio depende de fallos de cache y del mix de instrucciones: no es un
modelo de ciclos, es un presupuesto menos falso que el 2. `KESTREL_CPI=2` restaura el
comportamiento historico bit a bit y sigue siendo la herramienta de medida.


## El mando de DK64 no lee mal: lee pocas veces (2026-09-08)

El usuario apunto a la lectura del mando como causa de que la intro de DK64 falle la liana.
Se comprobo antes que nada, y la medida deja el sitio exacto. Para poder medirlo, el corte de
`[frames]` dice ahora tambien **cuantas veces ha leido el juego el mando** (comandos 0x01 del
joybus en el conector 1): es la unica cuenta de tiempo del mando que el invitado percibe, y
sin ella no hay forma de comparar su cadencia con los campos de video.

Barrida la intro entera, las lecturas del mando **coinciden con los SYNC_FULL del RDP**: 1060
lecturas frente a 1074 cuadros en la tanda de 1400 intercambios de buffer. O sea que DK64 lee
el mando una vez por cuadro dibujado, que es lo que hace en la consola: el camino de lectura
esta bien. Lo que no esta bien es cuantos cuadros hay. En hardware, a 30 fps, una lectura por
cuadro son 0,5 lecturas por campo de video; aqui salen entre 0,11 y 0,42 segun el tramo.

El A/B lo ata: con CPI 2 la intro da 0,201 lecturas por campo, con CPI 1,4 da 0,241, un 20 %
mas por darle mas CPU al invitado y nada mas. La cadencia del mando es una funcion del
presupuesto de instrucciones por campo, no un canal aparte. La intro avanza por cuadro, asi
que si solo completa la mitad de los cuadros, todo lo que programa "dentro de N cuadros" cae
en otro instante: eso es la liana. La intuicion era correcta en el sintoma -- el ritmo con el
que la intro consume entradas -- y la causa es el mismo CPI de la seccion anterior.

Queda ademas apuntado, como fallo real de otro orden, que nuestras DMA de SI se completan en
la misma instruccion que las lanza: eso adelanta la FASE de cada lectura unos cientos de
microsegundos frente al hardware, pero no cambia su RITMO. Ver `docs/GAPS.md`.
**Hecho el mismo dia**: ver la seccion siguiente.

## La lectura del mando cuesta tiempo (el SI ya no es instantaneo, 2026-09-08)

Pregunta del usuario: como lee el mando el hardware, y si libultra hace algo que aqui no se
tiene en cuenta. Se miro la fuente antes de tocar nada.

En la consola el juego no habla con el mando: habla con la PIF. Escribe un bloque de 64 bytes
de ordenes joybus en la PIF RAM (`0x1FC007C0`) y pide la DMA de lectura (`SI_PIF_ADDR_RD64B`).
La PIF **corre el protocolo joybus antes de contestar**. Ese protocolo es un solo cable serie a
**4 us por bit** -- 32 us por byte --, con una parada de la consola de 3 us al acabar de
transmitir y una parada del mando de 4 us al acabar de responder. Una lectura de botones (1
byte de orden, 4 de respuesta) sale sobre 167 us; los cuatro conectores, sobre 670 us; una
transaccion de 64 bytes medida en hardware anda entre 170 y 340 us. Por eso `SI_STATUS` tiene
bit de ocupado (bit 0 `DMA_BUSY`, bit 1 `IO_BUSY`) y por eso libultra hace lo que hace:
`osContStartReadData` lanza la DMA y **duerme el hilo** en la cola de mensajes del SI;
`osContGetReadData` recoge cuando el `MI_SI` despierta al hilo. Es decir, la lectura del mando
es un punto de cesion del planificador del juego, y en la consola cede de verdad.

Aqui no cedia: `siDma()` corria el bloque joybus entero, copiaba los 64 bytes y levantaba
`MI_SI` en la misma instruccion, con `si_status` de vuelta a 0 sin haber estado ocupado ni un
ciclo. El `osRecvMesg` volvia con el mensaje ya puesto y el hilo no soltaba la CPU. Cambiado:

- `siDma()` factura el tiempo de linea **leyendo el bloque de ordenes de verdad** -- cuenta los
  bytes de transmision y de respuesta de cada orden del bloque, a 4 us por bit, mas 3 us de
  parada de consola por orden transmitida y 4 us de parada del mando por cada una que responde,
  mas ~5 us por el traslado de los 64 bytes entre RDRAM y PIF RAM. Una orden a un conector
  vacio no cobra respuesta, que es lo que pasa en el aparato.
- Deja `SI_STATUS = DMA_BUSY` y arma un plazo `siDoneAt` en el reloj de invitado
  (microsegundos convertidos a instrucciones con el presupuesto por campo, el mismo que fija
  `KESTREL_CPI`). `siFinish()` copia la respuesta a la RDRAM y levanta `MI_SI` al vencer.

Lo delicado no es el retardo: es que venza en **el mismo instante en los siete modos**, porque
de eso vive el md5 de lockstep. Vence contra `cpu.retired`, que es la misma cuenta en todos:
el interprete lo comprueba por instruccion en `System::stepCpu()`, y el JIT tiene prohibido
compilar un bloque que se tragaria el vencimiento -- `jitTryBlock` consulta `siDueIn()` igual
que ya hacia con el borde de `Count`==`Compare`, y recorta ademas el permiso de encadenado.
El estado en vuelo (`siBusy`, `siToPif`, `siDram`, `siDoneAt`) va en la foto de estado, que
sube a version 7. `KESTREL_SIINSTANT=1` recupera el final instantaneo para comparar.

El A/B en la intro de DK64, 400 intercambios de buffer: con final instantaneo, 1719 campos y
255 lecturas de mando; con el retardo puesto, 1760 campos y 243 lecturas. El coste aparece,
que es justo lo que hace la consola. No es una mejora de exactitud medible en las suites --
`systemtest`, el md5 de SM64 en los siete modos y las 371 de krom salen igual -- sino la
mitad que faltaba del modelo de reloj: `KESTREL_CPI` arreglo cuantas instrucciones caben en un
cuadro, esto arregla que las transferencias ocupen su parte de ese cuadro.

Queda por hacer lo mismo con el PI (`PI_BSD_DOM*`), y cuando haya un tercer plazo, unificar los
tres en una agenda de eventos en vez de seguir sumando guardas en `jitTryBlock`. Ver
`docs/GAPS.md`.

## Fiel a consola: un solo interruptor para la velocidad del N64 real (2026-09-09)

El menu `Velocidad` de la ventana solo ofrecia tres formas de LIMITAR (automatico, siempre
59,94, sin limite), y el overclock vivia en otro sitio. El usuario pidio lo que faltaba: un
modo que signifique «la velocidad que tiene la maquina de verdad», sin tener que saber que
tres o cuatro variables hay que dejar quietas para conseguirlo.

`KESTREL_SPEEDMODE=hw` -- en la interfaz, **Fiel a consola (velocidad del N64 real)**, primer
elemento del menu `Velocidad` y opcion nueva en la categoria de relojes del lanzador. Lo que
hace, y por que cada cosa:

- **Ignora `KESTREL_OC`, `KESTREL_OC_CPU`, `KESTREL_OC_RSP` y `KESTREL_OC_RDRAM`.** No avisa
  del conflicto ni los respeta: un multiplicador heredado del entorno o de un perfil viejo
  falsearia la velocidad sin que se note, que es justo lo que el modo existe para impedir.
- **Ignora `KESTREL_CPI`** y vuelve al calibrado de fabrica (1,3984). Es la otra mitad del
  presupuesto de tiempo del invitado: de nada sirve el reloj nativo si las instrucciones que
  caben en un campo salen de un numero puesto a mano.
- **Deja el limitador en automatico**, que ya significa «limitar solo si hay ventana»: con
  ventana clava los 59,94 campos/s, sin ventana corre a ciegas a tope. **Corregido 2026-09-09**
  (antes lo clavaba tambien headless). Lo que el JUEGO ve es el reparto de trabajo por campo, y
  eso sale de los relojes y del CPI, no del ritmo al que corra el anfitrion: sin pantalla no hay
  nada que respetar, y clavar un gate o un bench a 59,94 Hz solo los hace tardar en tiempo real
  lo que dura la partida sin cambiar ni un bit del resultado. Una peticion EXPLICITA
  (`KESTREL_THROTTLE=0/1`) sigue mandando sobre el modo: quien la escribe lo hace a proposito.

Los gates y el banco NO lo ponen, y por eso siguen corriendo headless a toda velocidad: el
modo es del usuario, no del arnes. El valor de fabrica del perfil sigue siendo `libre`.

El limitador se aplica en caliente; los relojes y el CPI se montan al arrancar, asi que el
menu solo ofrece relanzar cuando el perfil traia overclock de verdad (algun multiplicador
distinto de 1,00). Sin overclock guardado no hay nada que anular y el cambio es instantaneo:
no se molesta al usuario con un dialogo que no cambia nada.

Para que sirve, ademas de por gusto: es el modo con el que hay que medir contra hardware. La
prueba de la liana de DK64 -- la intro reproduce entradas contando campos -- no significa nada
si el emulador va con la CPU dopada o con el limitador suelto.

## El RDP no costaba tiempo de invitado: `Memory::rdpPace` (2026-09-09)

El freno entre dominios del RCP solo existia para el RSP. `Memory::rcpPace` acoplaba la CPU
emulada al RSP (`Rsp::cyclesRun`) y **al RDP no le acoplaba nada**: el worker del RDP tardaba
lo que tardase el rasterizador del anfitrion, y mientras tanto la CPU emulada seguia retirando
instrucciones en su bucle de espera del `DP_DONE`. Como el reloj de video del invitado se
deriva de las instrucciones retiradas (`Memory::viTick(cpu.retired)`), esas vueltas de espera
**se cobraban como tiempo del juego**: pasaban campos de video sin que saliera un cuadro.

Medido en Donkey Kong 64, escena del rap, 3000 intercambios de buffer:

| modo | campos VI | instrucciones |
|---|---|---|
| lockstep + jit | 6153 | 6881 M |
| `KESTREL_SYNCRDP=1` (RDP en linea) | 6153 | — |
| enhebrado + jit + **SoftRDP** | **10830** | 12112 M |
| enhebrado + jit + parallel-RDP | 6161 | — |
| enhebrado + jit + SoftRDP, **con `rdpPace`** | **6174** | — |

El `SYNCRDP` identico al lockstep exculpa al RSP: el desvio era del RDP y solo del RDP. 10830
campos para 3000 cuadros son 18,0 fps de invitado contra los 30 de la consola — el juego
perdia la mitad de los cuadros, y **cuanto mas lento el anfitrion, mas perdia**. Tres
consecuencias, todas malas: las demos que cuentan campos (la liana de DK64) se desincronizan,
la velocidad del invitado depende de la maquina del usuario, y `bench` sobre enhebrado+SoftRDP
*infra-mide* porque el juego hace menos trabajo real del que dice.

El arreglo: el RDP tiene el MISMO reloj que el RSP (GCLK = reloj del RCP = 62,5 MHz), asi que
el ratio `paceCpuNum/paceCpuDen` ya calculado para el RSP vale tal cual. La medida de trabajo
sale del modelo de coste que ya existia (`SoftRdp::accountPixels` / `accountTmem`, calibrado
contra hardware por `scripts/rdptiming.py`), publicada en un espejo **monotono**
`rcp.rdpGclk` — `dpc_clock` no sirve porque el invitado lo puede borrar (DPC_STATUS bits 6..9).
`Memory::rcpPace` pasa a ser el combinador de los dos frenos y devuelve el permiso mas corto:
la CPU no puede adelantar ni al RSP ni al RDP mas de lo que permite el hardware.

Queda abierto (etapa 2): `DP_DONE` todavia no se entrega sobre un plazo de tiempo de invitado,
y parallel-RDP no tiene estimador de coste porque la GPU no alimenta `accountPixels` — con
PRDP el freno no actua, que es por lo que sus 6161 campos ya salian bien por otro camino (la
GPU va tan sobrada que el bucle de espera casi no da vueltas). Un estimador sobre el flujo de
ordenes le daria la misma temporizacion de invitado.

## El stick del mando no tenia puerta octogonal (2026-09-09)

El usuario pregunto si la lectura del mando estaba bien hecha. El joybus si: orden `0x01` con
respuesta de 4 bytes (dos de botones, uno por eje), bits en su sitio (A `0x8000` ... C-derecha
`0x0001`), coste de linea facturado a 4 us por bit y plazo del SI en tiempo de invitado. La
cadencia tambien: DK64 lee el mando **una vez por cuadro** (15000 intercambios de buffer ->
14802 lecturas), que es lo correcto para un juego de 30 fps.

El fallo estaba en el **stick**. El tope del stick de la N64 es fisico: el anillo tiene ocho
lados, ~85 en los cuatro ejes y solo ~69 en las cuatro diagonales. Kestrel recortaba **por
ejes a +-80**, o sea una puerta CUADRADA:

| entrada | Kestrel (antes) | consola |
|---|---|---|
| teclado en diagonal | (80, 80), modulo 113 | (69, 69), modulo 97 |
| mando moderno de recorrido cuadrado | (80, 80) | imposible pasar de ~85 |

Los juegos que sacan la velocidad de andar del **modulo** del stick (Mario, DK64, Zelda)
corrian en diagonal un 33% mas rapido de lo que jamas corrieron en hardware.

`padOctagon` (`src/video/present.cpp`) recorta **a lo largo del rayo**, que es lo que hace el
anillo de plastico: se respeta la DIRECCION que pide el jugador y solo se acorta el alcance.
Borde del octante `u + v*(C-D)/D = C`, con `C=85`, `D=69`, `u=max(|x|,|y|)`, `v=min(|x|,|y|)`.
Es lo ultimo que toca la senal, igual que en la consola. Calibracion contrastada con ares
(`controller/gamepad/gamepad.cpp:372`), que usa los mismos 85/69.

De paso la zona muerta del mando del anfitrion pasa de ser por ejes a ser **radial y con
reescalado desde su borde**: la de ejes recortaba un cuadrado y dejaba colar la esquina (un
stick con deriva en las dos direcciones entregaba un diagonal fantasma), y sin reescalar el
valor saltaba de 0 a 17 al cruzar el umbral en vez de arrancar desde cero.

## Con la GPU el RDP no costaba tiempo de invitado ni movia DPC_CLOCK (2026-09-09)

`Memory::rdpPace` (del cambio anterior) frena a la CPU del invitado contra el reloj de coste
del RDP, `rcp.rdpGclk`. Ese reloj -- y `DPC_CLOCK`, que es el registro que los juegos LEEN
para medirse -- solo los alimenta `SoftRdp::accountPixels`. Y `Memory::rdpRunJob`, cuando
parallel-RDP esta vivo, entrega el tramo de FIFO a `vrdp::runFifo` y **vuelve antes de pasar
por ahi**. Resultado: con la GPU activa el rasterizado le salia GRATIS al juego. `DPC_CLOCK`
clavado a cero y el freno del dominio RDP sin frenar jamas.

Es un hueco de semantica de hardware por si solo, y ademas el emulador corria mas suelto de
lo que corrio la consola justo en el modo que es la direccion del proyecto.

**Arreglo: un paseo de solo-coste sobre el mismo FIFO.** `SoftRdp::costOnly` (nuevo) hace que
el decodificador recorra los comandos y mantenga todo el estado (scissor, modos, imagenes,
tiles) pero **no escriba un solo byte de RDRAM** -- los pixeles buenos son los de la GPU. El
bucle por pixel no se ejecuta: cada tramo de scanline aporta su anchura de una vez, con los
mismos `xs`/`xe` que usa el rasterizado, asi que la cuenta de pixeles es IDENTICA y el paseo
cuesta O(altura) por primitiva en vez de O(area). Triangulos, `FILL_RECTANGLE` y
`TEXTURE_RECTANGLE` tienen los tres su atajo; las cargas de TMEM ya eran internas y siguen
cobrando por `accountTmem`.

Los pixeles se cobran como **escritos**. Sin z-buffer fiable en RDRAM (lo tiene la GPU) no se
puede saber cuales moririan en el test de profundidad, y el modelo de coste calibrado paga casi
lo mismo por un pixel muerto que por uno escrito (la diferencia es un trozo de escritura).
`Memory::rdpCostPass` lo engancha en la rama de la GPU, justo tras `vrdp::runFifo` y con el
MISMO punto de parada que devolvio la GPU, para que un comando partido por el borde del tramo
se cobre una sola vez. `KESTREL_RDPCOST=0` lo apaga para el A/B.

Medido en DK64 bajo parallel-RDP, durante la intro y la demo de atraccion:

| | antes | ahora |
|---|---|---|
| `DPC_CLOCK` por campo | 0 | 192.586 GCLK |
| ocupacion del RDP por campo | no medible | 18,5 % (de 1.042.709 GCLK a 62,5 MHz / 59,94 Hz) |

Ese 18,5 % contesta de paso una pregunta de la prueba de la liana: en esa escena el RDP **no**
es el palo largo, asi que cobrarlo no hace que DK64 pierda fotogramas ahi.

## Estado de la prueba de la liana de DK64 (2026-09-09)

Secuencia correcta, corregida por el usuario: los logos, el rap de DK, **UNA sola pulsacion de
START salta el rap**, y al poco arranca la demo de atraccion; la primera es la de las lianas.
Una segunda pulsacion en el titulo entra en el menu de partida, y ahi no salen demos nunca.
`scripts/dk_demo_hunt.py` lleva esa nota y el guion de caza reproduce el arranque completo.

Reproducido bajo parallel-RDP: tras el titulo, Donkey Kong aparece **nadando**, o sea que ha
caido al agua en vez de cruzar por las lianas. Se reproduce igual con SoftRDP.

Lo que este barrido DESCARTA, con medida:

| sospecha | medida | veredicto |
|---|---|---|
| el juego pierde fotogramas y la demo se desplaza | campos/flip = **2,00** sostenido (231 campos / 115 flips en la ventana de la demo, PRDP; 617/308 con SoftRDP) | descartada: 30 fps clavados, el arreglo de CPI de 2026-09-08 ya hizo su trabajo |
| el RDP no cuesta y por eso va suelto | ocupacion del RDP 18,5 % del campo (arriba) | no es el limite en esa escena |
| la lectura del mando | joybus 0x01 correcto y una lectura por cuadro (2026-09-08); puerta octogonal del stick arreglada (2026-09-09) | correcta |

Queda abierto. Lo siguiente a medir es si hay fotogramas perdidos **sueltos** que la media de
2,00 tapa: un solo cuadro de 3 campos ya desplaza el resto de la demo. `<scratchpad>/lag.py`
cuenta las transiciones de flip una a una en vez de promediarlas.

## La demo de DK64 NO pierde fotogramas: medido uno a uno (2026-09-09)

La media de campos por intercambio ya salia 2,00, pero una media tapa justo lo que importa:
a la demo de la liana le basta **un** cuadro de tres campos para que el resto de la
reproduccion se desplace. Asi que se ha contado cada intercambio por separado con
`KESTREL_FLIPLOG=1`, que imprime el campo de video exacto de cada escritura de `VI_ORIGIN`
desde DENTRO del emulador (sondear el contador por telemetria tiene jitter de +-1 campo y
falsearia precisamente los deltas de 1 y 3 que se buscaban).

Corrida de 330 s con parallel-RDP, un solo START en el campo 1523 para saltarse el rap:

| tramo | intercambios | 2 campos/cuadro | anomalias |
|---|---|---|---|
| logos + rap (campos 39..1522) | 685 | 88,91 % | 76 (66 de ellas de **1** campo) |
| titulo + demos DK TV (campos 1524..2695) | 547 | **99,45 %** | 3 |

Las tres anomalias de la demo son la entrada al titulo (delta=60, pantalla fija, y el delta=4
siguiente) y el ultimo intercambio al matar el proceso. Dentro de la demo la cadencia es
**exactamente 2 campos por cuadro, 30 fps clavados, cero fotogramas perdidos**.

Conclusion: el desfase de la demo de la liana **no** es perdida de fotogramas. Junto con lo
ya medido (ocupacion del RDP 18,5 %, lectura del mando correcta), la causa tiene que ser una
divergencia de LOGICA del juego, no de ritmo: la demo de atraccion es una grabacion de mandos
y algo que el juego lee vale distinto que en la consola.

Suelto, para mirar aparte: los 66 intercambios de **1 campo** durante el rap salen en parejas
cada ~10 campos. Huele a que ahi el juego mueve `VI_ORIGIN` por el desplazamiento de linea del
entrelazado (mismo buffer, direccion distinta), con lo que `viFlips` cuenta un intercambio que
no lo es. No afecta al invitado, pero si a la metrica.

## El RSP tenia un tope de instrucciones por tarea: era una suposicion de libultra (2026-09-10)

`junkrunner64` (SpellCraft, hecho con **libdragon**) se congelaba al empezar partida. En el
log salia esto en bucle:

```
[rsp] WARNING: budget exhausted at pc=0x244 (microcode hang?)
[dma!] SP->RDRAM sobre vectores: dram=0x000000/0x000040 len=64
```

Lo segundo es consecuencia de lo primero, y lo primero era **nuestro**.

`Rsp::start()` ponia `budget = 40'000'000` y `step()` corria
`while(!halt && maxInsns && budget)`. Al agotarse el saldo el nucleo metia un **BREAK
forzado** (HALT|BROKE + MI_SP). Eso da por bueno el modelo de libultra: *una tarea = un
BREAK*, el microcodigo arranca, hace su trabajo y para. Con ese modelo, 40 M instrucciones
sin parar solo puede ser un microcodigo colgado.

libdragon no funciona asi. `rspq` es una **cola persistente**: el planificador del RSP vive
en IMEM y su `RSPQCmd_WaitNewInput` (`include/rsp_queue.inc:417`) solo hace `break` cuando
la cola se **vacia**. Un juego que la mantiene alimentada encadena decenas de campos de
video sin que el RSP pare ni una vez, y se come el tope. Ahi le metiamos el BREAK: la CPU
veia una tarea terminada que no lo estaba, `rspq` perdia su estado, y los DMA siguientes
salian con la cabecera de tarea ya basura -- de ahi los `SP->RDRAM` a `dram=0x000000`.

En hardware real **no existe ningun tope de instrucciones**. El RSP corre hasta que su
microcodigo hace BREAK o hasta que la CPU le escribe HALT en `SP_STATUS`. Y quien devuelve
el control al llamante ya era `maxInsns`, no `budget`: Lockstep llama `step(1)` por
instruccion de CPU, el worker en modo hilos llama `step(~0)` con la tarea entera.

Asi que el saldo pasa a ser un **vigilante que solo avisa**. `Rsp::watchdog()` (rsp.cpp) se
llama cuando el saldo llega a cero, imprime una linea, vuelca la escena si
`KESTREL_RSPHANG=1` y **re-arma** otros 40 M. El tope duro sigue disponible a mano con
`KESTREL_RSPBUDGET=<instrucciones>` para bisecar un microcodigo de verdad colgado; por
defecto esta apagado. El aviso va limitado (los 4 primeros y luego 1 de cada 64) porque un
`rspq` vivo lo dispara cada ~0,6 s de tiempo invitado.

Medido en `junkrunner64` a 600 flips, las cuatro combinaciones de RCP x RDP:

| KESTREL_THREADS | parallel-RDP | antes | ahora |
|---|---|---|---|
| 1 | si | 27 s limpio | 23 s limpio |
| 1 | no | 37 s limpio | 34 s limpio |
| 0 | si | **rc=124, colgado** | 135 s, 4 avisos, **rc=0** |
| 0 | no | (colgaba a ratos) | 146 s, 4 avisos, **rc=0** |

Los avisos solo salen en Lockstep, y tiene sentido: ahi el RDP tambien lo mueve el hilo de
la CPU, asi que el `mfc0 DP_STATUS` / `bnez` de libdragon gira muchas mas instrucciones
esperando al RDP que en modo hilos, donde el RDP va en paralelo y contesta enseguida.

### De propina: `[dma!] ... sobre vectores` era un falso positivo

"Los vectores de excepcion (0x0-0x400) no son destino legitimo de ningun DMA" tambien es un
invariante de **libultra**, no del hardware: ahi el kernel del juego pone sus manejadores y
pisarlos es un fallo real. El N64 no protege esa zona de ninguna manera, y libdragon mete
estructuras suyas en RDRAM baja (`dram=0x0001a0` / `0x0001a8` en junkrunner64), disparando
el aviso decenas de veces por campo. Pasa a ser opt-in con `KESTREL_DMAWARN=1` y cortado a
16 lineas: es diagnostico para depurar un kernel libultra, no una condicion de error.

## `PI_STATUS` IO_BUSY se apaga solo: snapper64 no arrancaba (2026-09-10)

`snapper64` (bateria de tests RDP de HailToDodongo, hecha con **libdragon**) no llegaba ni a
configurar el VI: tras 1151 campos seguia con `origin=0, width=0, ctrl=0, flips=0`. La CPU
estaba clavada en `pc=0x8005b6c8`, que es el `dma_wait()` de libdragon:

```mips
    lw   v0, 16(v1)        # v1 = 0xA4600000, +0x10 = PI_STATUS
    andi v0, v0, 0x3       # DMA_BUSY | IO_BUSY
    bnez v0, -8
```

El bit que no bajaba era **IO_BUSY** (`PI_STATUS` bit 1). Lo ponia `Memory::cartWrite` cuando
una escritura de la CPU se engancha al bus del PI, y lo bajaba **solo** `Memory::cartRead`.
Es otra suposicion de libultra: alli el patron es escribir y despues leer del cartucho, asi
que el latch siempre encontraba quien lo caducase. libdragon escribe y despues espera en
`PI_STATUS` sin tocar el cartucho: el bit se quedaba puesto para siempre.

En hardware la escritura pendiente dura lo que dura el ciclo del bus del PI y despues el bit
cae; **nadie tiene que leer el cartucho para que baje**. El plazo ya existia en el nucleo
(`CART_LATCH_TTL`, medido en reloj de invitado), solo faltaba dejarlo vencer tambien por el
camino de lectura del registro. `Memory::piIoDecay()` (memory.cpp) hace exactamente eso y se
llama desde `mmioRead32` case 0x10 y desde `cartRead`, que antes repetia el codigo inline.
Una lectura de `PI_STATUS` inmediatamente despues de la escritura sigue viendo IO_BUSY
puesto, igual que en HW: el plazo no cambia, solo deja de depender de que alguien lea el
cartucho.

Con el arreglo, snapper64 arranca y renderiza: `fields=4440, flips=4025, origin=6651904,
width=320` en menos de 15 s. `gate_all` 430 s y `gate_prdp` 323 s, ambos rc=0: systemtest
0/3721 en los siete modos, md5 sm64 `d35bd8aa9b13d459ce9332c07a79a53a` (soft) y
`b5521b24d8fc280fbf102df22d7d30cb` (prdp) sin cambio, krom 371/371 con regress=0 y nodump=0
en los dos (mean_exact 88,73 interp / 89,27 prdp).

## Por que las ROMs de libdragon iban lentas: dos causas, ninguna era el RDP (2026-09-10)

Medido a 600 intercambios en `junkrunner64` (libdragon) contra `Super Mario 64` (libultra),
modo por defecto (hilos + parallel-RDP):

| | antes | tras LDL/LDR | tras `wasHalted` |
|---|---|---|---|
| pared (junkrunner64, 600 flips) | 21 s | 21 s | **17 s** |
| Mips emulados | 64,7 | 66,4 | **82,6** |
| velocidad N64 (CPU) | 89,3% | 90,7% | **115,9%** |
| `cpuWait` | 37% | 36% | **5%** |

SM64 tambien sube de paso: 151 -> 161 Mips, 172% -> 186%, `cpuWait` 17% -> 10%.

Antes de encontrarlas se descarto el sospechoso obvio: el **regulador del RDP**
(`Memory::rdpPace`). Con `KESTREL_PACESLACK` gigante (freno desactivado de hecho) el tiempo de
pared no se movia -- 21 s las dos veces, `cpuWait` seguia en 36%. No era el freno.

### 1. El JIT de CPU no absorbia LDL/LDR/SDL/SDR

`emitInterpOp` (`jit.cpp`) admitia LWL/LWR/SWL/SWR pero no sus versiones de 64 bits, asi que
el bloque se declinaba entero en la primera. No es un caso raro: **GCC (libdragon) resuelve
una copia desalineada de 64 bits con `ldl`/`ldr` + `sdl`/`sdr`, mientras que IDO (libultra)
usa las de 32.** El perfil de junkrunner64 daba el 52% de las muestras en un bucle de
`0x8001b184` que las lleva cada cuatro instrucciones:

```mips
    ldl v1,0(v0)
    ldr v1,7(v0)
    sdl v1,8(s2)
    sdr v1,15(s2)
```

Van por el mismo trampolin de interprete que las de 32 (memoria, sin control de flujo); el
volcado dirigido ya nombraba `rs` y `rt`, solo faltaba anadir LDL/LDR a las que escriben
`gpr[rt]`. **+4,8%** en junkrunner64, sin cambio en SM64 (no emite ninguna).

### 2. `rspAwaitIdle()` bloqueaba en CLEAR_HALT aunque el RSP ya estuviese corriendo

La escritura de `SP_STATUS` (`memory.cpp`) llamaba a `rspAwaitIdle()` ante **cualquier**
CLEAR_HALT suelto, en modo hilos. El motivo era real pero solo aplica a un LANZAMIENTO: la
tarea anterior tiene que publicar su estado antes de que la CPU vea "terminada". Con el nucleo
**ya corriendo**, un CLEAR_HALT es un no-op en hardware -- limpia un bit que ya esta limpio, no
lanza nada, no hay nada que ordenar.

Y libdragon lo escribe constantemente. `rspq_flush_internal()` (`src/rspq/rspq.c:1165`), en
CADA vaciado de cola y **dos veces seguidas a proposito**:

```c
*SP_STATUS = SP_WSTATUS_SET_SIG_MORE | SP_WSTATUS_CLEAR_HALT | SP_WSTATUS_CLEAR_BROKE;
__asm("nop; nop; nop; b 1f; 1:nop; nop; nop; nop; nop; nop;");
*SP_STATUS = SP_WSTATUS_SET_SIG_MORE | SP_WSTATUS_CLEAR_HALT | SP_WSTATUS_CLEAR_BROKE;
```

Como `rspq` es cola persistente y solo hace break cuando se **vacia** (ver la seccion del
vigilante del RSP), cada uno de esos avisos clavaba a la CPU emulada hasta que el worker se
comiese la cola entera: **37% del tiempo de pared**, con el worker del RSP ocupado solo un 30%.
El comentario del propio libdragon lo dice ahi mismo: un emulador deberia *resincronizar* CPU
y RSP en `SP_STATUS`, no bloquear hasta el BREAK.

El arreglo ata la espera a `wasHalted`, la MISMA condicion que ya usaba el lanzamiento tres
lineas mas abajo. Lockstep no se toca (133 s a 600 flips, igual que antes).

Los dos cambios pasan `gate_all` y `gate_prdp` con systemtest 0/3721 en los siete modos, md5
de SM64 sin cambio en las dos ramas y krom 371/371 con regress=0 y nodump=0.

## La captura de telemetria metia la cobertura en el canal alfa (2026-09-10)

`capture_framebuffer` sobre snapper64 devolvia una pantalla **en blanco**: el menu no salia
por ningun lado. En RDRAM estaba dibujado perfectamente -- se leia letra por letra con
`mem.read` -- y aun asi el PNG salia vacio. La sospecha inicial (que el modo repeat del MI,
que snapper64 usa via `__mi_memset64` de libdragon, no estuviera emulado y las escrituras de
64 bits de la CPU se perdieran) era **falsa**: `Memory::miRepeatStore` + el armado en
`MI_MODE` coinciden instruccion a instruccion con `libdragon/src/mi_memset.S`, y snapper64 no
imprime su aviso "MI-Rep. not emulated, using fallback".

El fallo estaba en el **visor**, no en el emulador. `cmdViCapture` (`src/telemetry/server.cpp`)
copiaba el ultimo byte del pixel de 32bpp al canal alfa del PNG. Ese byte **no es alfa**: es
la **cobertura** (`coverage`) que el VI usa para el antialias de bordes, y el DAC del N64 saca
siempre imagen opaca. Todo lo que dibuja la CPU deja cobertura 0, asi que salia con alfa 0 --
transparente -- y el visor lo componia sobre blanco. Solo se veia lo que habia pintado el RDP,
que si deja cobertura. En 16bpp era todavia peor: ahi la cobertura es el bit 0 del pixel, asi
que desaparecia cualquier pixel cuyo bit menos significativo fuera 0.

La captura pasa a salir **opaca** siempre. La cobertura, si algun dia hace falta, es un plano
aparte, no el alfa. La rama de GPU (`vrdp::scanout()`) no se toca: parallel-RDP ya entrega
RGBA opaco.

Tras el arreglo el menu de snapper64 se captura tal cual:

```
             < Failed [ALL] Options >
  Run All                          Results
  RAM 9th Bit - CPU->CPU           ----/0004
  RDP Fill Mode Tri (Sweep)        ----/2048
  ...
  C: Select / A: Run Test / B: Dump Test / S: Stop Test
```

Es un arreglo de **clase**: explica todas las "capturas en blanco" anteriores, que se venian
achacando al nucleo. Cambio de telemetria puro, sin efecto en emulacion (systemtest 0/3721 en
los siete modos, md5 de sm64 identico en interp y prdp, krom regress=0).

## Los registros DPS (0x0420_0000) no existian: el puerto de test del RDP (2026-09-10)

`snapper64` trae un grupo entero, `RDP Test-Mode - Span R/W`, que daba **0 de 32**. No era
precision: era que el bloque de registros **DPS** (Display Processor Span) sencillamente no
estaba mapeado. Lo comprobado antes de tocar nada: no lo implementan ni kestrel64, ni ares, ni
libdragon, ni parallel-rdp (barrido local exhaustivo, cero resultados).

Que es. El RDP guarda internamente un **buffer de tramos** (spans) y `0x0420_0000` es el puerto
por el que la CPU puede leerlo y escribirlo:

| Registro | Desplazamiento | Que hace |
|---|---|---|
| `DPS_TBIST` | +0x00 | autotest de la TMEM (11 bits) |
| `DPS_TEST_MODE` | +0x04 | abre el puerto (1 bit) |
| `DPS_BUFTEST_ADDR` | +0x08 | elige la palabra, **7 bits** |
| `DPS_BUFTEST_DATA` | +0x0C | mueve el dato de/hacia esa palabra |

La forma del RAM interno es lo interesante, y es lo que mide el test: el registro de direccion
es de **7 bits**, asi que la ventana da la vuelta cada **128 palabras**. Esas 128 palabras son
**32 entradas de tramo de 4 ranuras**, y de las cuatro ranuras solo tres tienen registro fisico
detras:

- ranura 0 y 1: palabra completa de 32 bits,
- ranura 2: **solo 8 bits** -- es la cobertura del tramo, el resto se pierde,
- ranura 3: **no existe** -- se lee como cero y las escrituras se van a la nada.

snapper64 escribe 1024 palabras seguidas con cuatro patrones distintos y comprueba las 4096
lecturas contra exactamente ese enmascarado (`maskValue()` en `RDPTestModeRW.cpp`: `i%4==2` ->
`& 0xFF`, `i%4==3` -> `0`). Cualquier programa que barra la ventana ve ese patron, no solo un
test: es la forma real de la memoria interna del RDP.

Implementado en `src/core/memory.{hpp,cpp}` como almacenamiento mas los dos ayudantes
`dpsSpanRead()` / `dpsSpanWrite()` que aplican el enmascarado, y anadido a la foto de estado
(`savestate.cpp`, version 8 -> **9**). Lo que **no** se modela es que el rasterizador alimente
ese buffer al dibujar: eso es estado interno del RDP que parallel-RDP no expone, y es
justamente el otro grupo (`Test-Mode Span Tri`, 216 tests) -- queda anotado en `docs/GAPS.md`.

Resultado: `RDP Test-Mode - Span R/W` pasa de **0000/0032** a **0032/0032**.

### El marcador completo de snapper64, y que dice

Aprovechando la bateria entera en `build-prdp`: **4182 / 6632**. Dos conclusiones que valen mas
que el numero:

1. **parallel-RDP puntua exactamente igual que nuestro SoftRDP, grupo a grupo.** Los mismos
   2450 fallos en los mismos sitios. Eso descarta de golpe "precision del rasterizador": son
   **funciones que faltan** en los dos.
2. **Casi nada de esto es un problema de tiempos.** El unico bloque que mide latencia del cauce
   del RDP es `Rect No-Sync` (60 tests de 6632), y ahi parallel-RDP no puede ganar por
   construccion: aplica cada orden de forma atomica.

El bloque gordo que **si** es arreglable es el triangulo en ciclo FILL (2055 tests): comparando
pixel a pixel con la referencia de consola (los modos `Ref` de snapper64 copian la captura de
hardware dentro de la superficie) sale que la consola escribe **bytes parciales** en los bordes
del tramo -- pixeles con solo el byte R puesto. El tramo en FILL se calcula en bytes/palabras de
64 bits, no en pixeles. Ese es el siguiente objetivo.

## La cache estaba emulada pero no costaba nada (2026-09-10)

Pregunta del usuario: *"y no tendras que meter eso igual de cache i?? como el otro emulador
para corregir las lianas??"*. La respuesta corta es **la cache ya esta, lo que falta es su
coste**, y ahora ya se puede cobrar.

Lo que habia desde hace mucho, y sigue estando:

| pieza | donde |
|---|---|
| `ICacheLine icache[512]` (16 KB, linea de 32 B) | `src/cpu/cpu.hpp` |
| `DCacheLine dcache[512]` (8 KB, linea de 16 B, bit sucio) | `src/cpu/cpu.hpp` |
| `dcRead`/`dcWrite` en linea con comparacion de tag | `src/cpu/cpu.hpp` |
| `dcMiss` (write-back + relleno), `dcFlush`, `dcFill` | `src/cpu/cpu.cpp` |
| `icFetch`/`icFill` -- ejecuta codigo *stale* si un DMA pisa RDRAM sin invalidar, como el HW | `src/cpu/cpu.cpp` |
| la instruccion `CACHE` completa (`cacheOp`) | `src/cpu/cpu.cpp` |
| integracion en el dynarec (`kestrel_jitCACHE`, `dcOff`) | `src/cpu/jit.cpp` |
| las dos caches en el savestate | `src/core/savestate.cpp` |

Lo que NO habia: **ni un ciclo de coste**. `dcMiss()` empezaba con `dcMisses++;` y ahi se
acababa. El reloj era plano -- `cpi256 = 179` (CPI 1,4) para toda instruccion, pasee por RDRAM
o no. Es justo el agujero que m64p cerro para las lianas de DK64: la fase del juego respecto al
VI depende de donde caen los fallos, y con un CPI constante no cae en ningun sitio.

### Los dos obstaculos, que no eran la cache

**1. El latch del timer era una igualdad.** `if(Count == Compare) timerIntr = true` solo funciona
mientras Count avanza 0 o 1 por instruccion. Un fallo cuesta ~60 ciclos de CPU = ~30 ticks de
Count *de golpe*: Count pasaria POR ENCIMA de Compare y la interrupcion del temporizador se
perderia entera. Ahora lo hace `CPU::countAdd()`, que comprueba si Compare cae DENTRO del tramo
`(old, old+ct]`; con `ct == 1` es exactamente la igualdad de antes, bit a bit.

**2. Las guardas de borde del dynarec comparaban ops contra ticks.** En `jitTryBlock`:

```cpp
u32 d = cmp - cnt;                 // ticks hasta Count==Compare
if(d <= K) { JDECL(DR_TIMER); return 0; }   // K = OPS del bloque
```

Eso solo es conservador mientras `ticks(ops) <= ops` -- y por eso `cpiFromEnv()` acota
`cpi256 <= 256` con un comentario que lo dice. Ahora se compara contra `countTicksMax(K)`, la
cota superior de ticks que puede costar el bloque (peor racha: 2 fallos por op). Con el coste
apagado `countTicksMax(K) == K` y las guardas quedan **byte a byte** como estaban. El permiso
de la cadena, que se descuenta en ops pero lo acota un margen en ticks, se convierte con
`opsForTicks()`, la inversa conservadora.

### Y el reloj no se parte en dos

El campo de video (`Memory::viTick` / `viFieldInsns`), la lectura de `VI_V_CURRENT`, el plazo
del SI y el decaimiento del pestillo del PI (`Memory::cartNow`) miden el tiempo en
**instrucciones retiradas**. Si las paradas solo movieran Count, el reloj del invitado y el del
video correrian a ritmos distintos -- que es el bug de "dos relojes" que documenta
`Clocks::cyclesPerInsn` y que ya paso una vez (750 k instrucciones por campo en el tick contra
1,56 M en la lectura). Por eso las paradas se traducen tambien a **ops equivalentes**
(`stallOps`, con `1 op = cpi256/128 ciclos`) y las suman `viTick` y `cartNow`. Un campo con
muchos fallos hace MENOS trabajo de CPU, que es lo que pasa en la consola.

### Lo que mide

`KESTREL_CACHECOST=<ciclos>` (`1`/`on` = 60 ciclos = ~640 ns de latencia de RDRAM a 93,75 MHz;
`0` = apagado = **defecto**). Al cortar, el emulador imprime `[cpi] base X real Y`. Con 400
intercambios por juego:

| juego | fallos D$ | fallos I$ | CPI que anaden | total con base 1,0 | CPI medido aparte |
|---|---|---|---|---|---|
| SM64 | 0,745 % | 0,006 % | +0,451 | 1,451 | -- |
| DK64 | 0,399 % | 0,006 % | +0,243 | 1,243 | **1,19** |
| Perfect Dark | 0,250 % | 0,013 % | +0,158 | 1,158 | **1,45** |

Esto es lo que hacia falta para poder *justificar* el numero en vez de elegirlo: el CPI deja de
ser un gusto y pasa a ser "1,0 de canalizacion mas lo que la cache le cueste a ESE juego". DK64
cuadra con su medida independiente (1,243 contra 1,19). Perfect Dark no: sale demasiado barato.

### Por que sigue apagado de fabrica

El `kCpiDefault256 = 179` de hoy **no es** el CPI de canalizacion del VR4300, es un CPI
*efectivo* medido sobre juegos: ya lleva dentro el coste medio de los fallos, promediado.
Encender el coste sin bajar antes la base a la canalizacion pura contaria la penalizacion dos
veces. Y bajarla a 1,0 hoy dejaria a Perfect Dark por debajo de su propia cota inferior.

Lo que falta esta en `docs/GAPS.md`, por orden: (1) cobrar los accesos **no cacheados** -- un
load a KSEG1 paga la latencia entera de RDRAM y aqui no cuesta nada, y Perfect Dark corre
mapeado por TLB usando KSEG1 a manos llenas; (2) la asociatividad (aqui las dos caches son de
mapeo directo, en la VR4300 son de 2 vias, o sea aqui se falla de MAS); (3) recalibrar la base
y rehacer entero el barrido de "que falta para mover el defecto".

Con el defecto (`KESTREL_CACHECOST=0`) el emulador se comporta exactamente como antes.
`systemtest` da `0/3721 · 0/2 · 0/6` en interprete y dynarec **con el coste apagado y tambien
encendido a 60**. El savestate sube a version 10 (`stallCycles`, `stallOps`, `stallOpsRem`: son
reloj de invitado a medio consumir, igual que `countFrac`).

## Los accesos no cacheados SI cuestan, pero NO son la explicacion (2026-09-10)

Con la cache ya cobrando ciclos ([[seccion anterior]]) el CPI de Perfect Dark se quedaba en
1,158 contra un suelo medido de 1,45. El sospechoso obvio: **KSEG1**. Un load a memoria no
cacheada no mira la cache, va al bus y paga la latencia entera de RDRAM (~640 ns = ~60
ciclos a 93,75 MHz). PD toca registros del RCP constantemente. Parecia cerrado.

Se implemento con su propia perilla, `KESTREL_UNCACHEDCOST` (`0`/`off` por defecto,
`1`/`on` = 60 ciclos, o un numero), separada de `KESTREL_CACHECOST` a proposito: aunque la
cifra coincida son dos costes DISTINTOS, y solo con perillas separadas se puede medir uno
sin el otro.

**Solo se cobran las LECTURAS.** En la VR4300 los stores no cacheados son *posted*: el bufer
de escritura se los queda y la CPU sigue; solo para si llega otro store antes de que el
anterior drene. Cobrar cada store como si fuera sincrono seria inventarse una parada que el
hardware no tiene, y falsearia al alza justo los juegos que mas escriben en registros del
RCP -- es decir, justo el caso que se queria medir.

Puntos de cobro (`chargeUncached()`): las cuatro entradas `CPU::read8/16/32/64` cuando el
destino no es cacheable, `LL` y `LLD`, y los dos ayudantes de memoria del dynarec. El camino
rapido que emite el JIT no hace falta tocarlo: solo cubre ckseg0 dentro de RDRAM
(`cmp eax, jitRdramSz` / `jae`), asi que todo lo no cacheado cae ya en los ayudantes en C.

### El resultado mata la hipotesis

| Juego | lecturas no cacheadas / retiradas | +CPI que aportan | CPI final | suelo medido |
|---|---|---|---|---|
| SM64 | 0,021 % | +0,013 | 1,463 | ~1,45 |
| DK64 | 0,005 % | +0,003 | 1,244 | ~1,19 |
| Perfect Dark | 0,032 % | +0,019 | 1,199 | **~1,45** |

Dos por diez mil. El "nunca en bucles calientes" de la guia de optimizacion funciona: los
juegos comerciales ya evitan KSEG1 en lo que se ejecuta mucho. Perfect Dark sigue a 1,199
contra 1,45. **KSEG1 no explica nada.** Queda implementado igual porque es semantica real
del hardware y suma cuando se enciende, pero como hipotesis esta refutada, y asi consta en
`docs/GAPS.md` -- refutada, no callada.

### Sesgo conocido que queda

`SWL`/`SWR`/`SDL`/`SDR` se emulan como lectura-modificacion-escritura. Sobre memoria
cacheada eso es correcto (un store parcial que falla en D-cache rellena la linea de verdad),
pero sobre memoria NO cacheada se cobra una lectura que el hardware no hace. Con tasas de
0,03 % es ruido; anotado por si algun dia deja de serlo.


## Dos relojes, un plazo: el cuelgue del SI con el coste de cache (2026-09-10)

Al intentar medir el CPI de la FPU con `KESTREL_CACHECOST=60` salio una regresion que las
puertas no cogian, porque `gate_all` nunca enciende esa perilla: **SM64 arrancaba y no
dibujaba nada**. 300 campos de video, 0 intercambios de buffer, 0 syncs del RDP, **0 lecturas
de mando**, `vi_origin=0x27f`. El perfilador decia que el 99,04 % de las muestras estaban en
`0x80246DD8 beq zero,zero,80246dd8`, justo detras de un `jal 803236f0` (`osSetThreadPri(NULL,0)`):
el hilo ocioso de SM64. Los campos del VI SI avanzaban y la interrupcion del VI SI llegaba;
lo que no corria nunca era el hilo principal.

Era dependiente de la magnitud (`=2` -> 87 intercambios, `=8` -> 1, `=30`/`=60` -> 0) y no era
cosa del dynarec (el interprete se colgaba igual). Para partirlo en dos hicieron falta dos
herramientas nuevas:

* **`KESTREL_MAXFIELDS=<n>`** (`src/core/system.cpp`): tope de parada por CAMPOS de video. Los
  otros dos topes (`MAXFLIPS`, `MAXSYNCS`) cuentan trabajo del RCP, o sea que no sirven justo
  cuando mas falta hacen -- un juego que arranca y nunca dibuja no se para solo y hay que
  matarlo por timeout a ciegas, sin la telemetria de cierre.
* **`KESTREL_STALLCLOCK`** (`src/cpu/cpu.{hpp,cpp}`): perilla de biseccion del ACOPLE de las
  paradas al reloj de invitado. `1` (defecto) los dos acoples; `vi` solo `guestOps()` ->
  `viTick`; `cart` solo `cartNow()` -> PI/SI; `0` ninguno. No apaga el cobro de ciclos ni la
  telemetria: separa "cobrar mal" de "acoplar mal".

La biseccion dio el culpable en un paso: `vi` funcionaba (95 intercambios), `cart` no (0).

### La raiz

`Memory::siDma()` arma el plazo de la transaccion del joybus en el reloj de invitado:

```cpp
siDoneAt = cartNow() + usToInsns(us);   // cartNow() = retired + jitPending + stallOps
```

y el bucle principal lo vencia en OTRO reloj:

```cpp
if(memory.siBusy && cpu.retired >= memory.siDoneAt) memory.siFinish();   // <- solo retired
```

Con el coste de cache apagado los dos relojes son el mismo y no se nota. Encendido, `siDoneAt`
nace desplazado por **todas las paradas acumuladas desde el arranque**, no por lo que dura la
transaccion: cuanto mas lleva corriendo el juego, mas lejos queda el plazo. `retired` acaba
alcanzandolo, pero cientos de miles de instrucciones tarde, asi que `SI_STATUS.DMA_BUSY` se
queda pegado, la interrupcion del SI no llega, `osContStartReadData` no vuelve nunca y el
kernel de libultra no despierta al hilo del juego. De ahi las 0 lecturas de mando y el hilo
ocioso al 99 %.

**Arreglo**: vencer el plazo en el reloj en que se arma (`memory.cartNow() >= memory.siDoneAt`).
Una linea, y es la semantica correcta con o sin la perilla: un plazo tiene que nacer y morir en
el mismo reloj. La guarda del JIT (`jit.cpp`, `mem->siDueIn(mem->cartNow())`) ya usaba
`cartNow()`, o sea que ademas los siete modos vuelven a estar de acuerdo en el instante exacto
de `MI_SI`.

Verificado: SM64 con `KESTREL_CACHECOST=60` pasa de 0 a **95 intercambios / 96 syncs / 96
lecturas de mando** en 300 campos, identico a la corrida sin coste, y `STALLCLOCK=1` y
`STALLCLOCK=cart` dan ya el mismo resultado.

### Leccion para las puertas

Ningun gate enciende `KESTREL_CACHECOST`, asi que este fallo podia vivir indefinidamente. El
patron -- "plazo armado en un reloj, vencido en otro" -- hay que buscarlo tambien en el PI
(`cartLatchExpiry`, que si usa `cartNow()` en los dos lados) y en cualquier plazo futuro del
planificador de eventos.


## El audio de DK64: el ritmo era exacto, el hipo era del cebado (2026-09-10)

Reporte del usuario: "el audio se entrecorta, se hace mas grave" y, en DK64, "va acelerado".
Medido en vez de supuesto, con `KESTREL_AUDIOSTAT=1` y el limitador puesto:

**1. El ritmo de produccion es exacto.** DK64 es PAL (`region 'P' -> PAL (50.00 campos/s)`,
bien detectado) y el AI abre el sumidero a 22 049 Hz. Restando tandas para quitar el silencio
del arranque:

| tramo | campos | segundos de invitado | fotogramas empujados | Hz efectivos |
|---|---|---|---|---|
| 250 -> 500 | 250 | 5,0 | 110 216 | 22 043 |
| 500 -> 1000 | 500 | 10,0 | 220 432 | 22 043 |

22 043 contra 22 049 = **0,03 % de desviacion**. No hay deriva de tono ni de velocidad en el
audio; el juego produce exactamente lo que le toca.

**2. "Acelerado" es correr sin limitador.** 500 campos PAL = 10,0 s de invitado; medido:
`KESTREL_THROTTLE=1` -> 10,48 s de pared (los 0,48 son el arranque), `KESTREL_THROTTLE=0` ->
**5,70 s = 1,76x tiempo real**. El automatico (`rt::throttle == -1`) limita solo si hay ventana,
asi que una sesion con ventana ya va bien; una corrida sin ventana va a 1,76x y suena rapida.

**3. El hipo SI era nuestro, y estaba en el cebado del sumidero.** `feederLoop` esperaba un
colchon de `kPrimeSamples = kBufSamples * 2` (dos bufers) y, en cuanto lo tenia, encolaba los
**cuatro** bufers del dispositivo de golpe. Los dos ultimos salian a medias: `ringPull` rellena
de ceros lo que no hay. Y la comprobacion de "se acabo el colchon" estaba DESPUES de servir el
hueco, o sea que el silencio ya iba encolado. Medido en DK64: **9 616 muestras de silencio por
arranque, la misma cifra corriera 250, 500 o 1000 campos** -- un hipo fijo al empezar a sonar,
no hambre continua.

Arreglo, dos piezas:
* `kPrimeSamples = kBufSamples * kNumBufs`: el colchon minimo es lo que el alimentador va a
  encolar de golpe, no la mitad.
* Antes de cada `ringPull`, si no hay bufer ENTERO se descebra y se sale del bucle. Lo ya
  encolado sigue sonando mientras se rehace el colchon, y nunca se encola un bufer medio mudo.

Resultado, con el limitador puesto:

| ROM | antes | despues |
|---|---|---|
| DK64 250/500/1000 campos | silencio 5,69 % / 2,61 % / 1,17 %, cortas 17 | **0,00 % / 0,00 % / 0,00 %, cortas 0** |
| SM64 600 campos (32 006 Hz) | -- | **0,00 %, cortas 0** |

Es cambio del anfitrion: no toca ni un bit de estado de invitado.

## El RSP tambien corre mientras la CPU esta parada en la cache (2026-09-10)

Segunda entrega de "un plazo nace y muere en el mismo reloj", esta vez sin cuelgue: el
acoplamiento CPU:RSP media la CPU en **instrucciones retiradas** y no en tiempo de invitado.

**Donde.** Dos sitios, el mismo error:

- Lockstep, `src/core/system.cpp`: `rspPhase += rspStepNum` una vez por instruccion retirada.
- Threaded, `Memory::rcpPace` / `rspPace` / `rdpPace` (y la llamada del prologo del dynarec en
  `jit.cpp`): la base del episodio y el adelanto se tomaban de `cpu.retired`.

**Por que esta mal.** El ratio que sale de `Clocks` es pasos de RSP por instruccion-equivalente
de CPU. Una instruccion que falla en la D$ cuesta ~60 ciclos de latencia de RDRAM, y durante
esos 60 ciclos el RSP **sigue corriendo a 62,5 MHz**. Contando solo retiradas, al RSP le tocan
menos pasos de los que le tocan de verdad, y el sesgo aprieta justo en las escenas con mas
fallos de cache. En Threaded era ademas coherente consigo mismo pero con un reloj distinto del
de Lockstep, y que los dos modos den el MISMO md5 es una invariante de las puertas.

**Arreglo.** Los dos lados miden ya en `cpu.guestOps()` = retiradas + paradas convertidas a
instrucciones-equivalentes (`stallOps`), que es el mismo reloj en que nacen y vencen el campo
de video, el plazo del SI y el latch del PI:

```cpp
u64 nowGuestOps = cpu.guestOps();
u64 dGuestOps   = nowGuestOps - lastGuestOps;
lastGuestOps    = nowGuestOps;
if(memory.rcpMode == Memory::RcpMode::Lockstep && memory.rsp.running && dGuestOps) {
  rspPhase += rspStepNum * dGuestOps;
```

La referencia se reengancha al salir de un bloque del dynarec: ese camino solo se toma con el
RSP parado, asi que lo que avance el reloj ahi dentro no le toca al RSP y no debe acumular
fase. Con `KESTREL_CACHECOST` apagado `dGuestOps` vale 1 por instruccion y el binario se
comporta byte a byte como antes.

**Medido** (DK64 PAL, 300 campos de video, `KESTREL_MAXFIELDS=300`):

| coste | modo | intercambios | syncs RDP | retiradas |
|---|---|---|---|---|
| 0  | Lockstep | 74 | 56 | 402M |
| 0  | Threaded | 74 | 56 | 402M |
| 60 | Lockstep (antes) | 74 | 31 | 316M |
| 60 | Lockstep (ahora) | **76** | 31 | 320M |

Sin coste los dos modos coinciden EXACTO. Con coste, Lockstep sigue siendo determinista (3 de 3
corridas identicas antes y despues) y el RSP recibe ahora los pasos que le tocan durante las
paradas.

**De paso, y esto es lo gordo del dia: el modo Threaded NO es determinista.** No lo trae este
cambio y no tiene que ver con el coste de cache -- la primera version de esta nota decia lo
contrario porque se escribio con UNA corrida por configuracion. Con cuatro, DK64 PAL a 300
campos y `CACHECOST=0` da **72 / 70 / 73 / 73** intercambios y 56 / 53 / 55 / 56 syncs, con
`origin` alternando entre `0283c0` y `0be3c0`, mientras Lockstep clava 74 / 56 las tres veces.
SM64 igual: 71 / 95 / 95 en Threaded contra 95 fijo en Lockstep. Tampoco es el acople de las
paradas: con `CACHECOST=60 STALLCLOCK=0` sigue bailando.

Las retiradas SI son estables (402M) porque el bucle pide exactamente `viFieldInsns` por campo;
lo que baila es donde caen las cosas dentro del campo. La causa es que los workers levantan
`MI_SP` (`src/rsp/rsp.cpp`, al llegar al BREAK) y `MI_DP` cuando terminan en tiempo de **pared**:
el invitado ve la interrupcion en una instruccion distinta cada corrida y el hilo que esperaba
despierta antes o despues. Las puertas no lo cazan porque validan "Lockstep == Threaded" con
SM64 a 60 campos, que aun no ha divergido.

**Threaded determinista (2026-09-10, docs/GAPS.md 3b -- RESUELTO).** En Threaded los workers
levantaban `MI_SP`/`MI_DP` en tiempo de PARED: dos corridas del mismo binario daban partidas
distintas (DK64 a 300 campos: 72/70/73/73 intercambios contra 74 fijo de Lockstep; SM64
71/95/95 contra 95). Arreglado con dos piezas que aplican la regla de siempre -- *un plazo nace
y muere en el mismo reloj*: (1) el **paseo de coste del RDP va delante del dibujado**, asi que
`rcp.rdpGclk` lleva el coste del tramo desde el principio en vez de saltar al final; (2)
**barreras de invitado** para los dos dominios (`dpBarrierOps`, `spBarrierAt`, bits 2 y 3 de
`rcpPend`, metidas en `rcpDueIn` y con salvavidas de 20 ms): la CPU no puede pasar del instante
de invitado en que la tarea en vuelo termina. Resultado: `spLate`/`dpLate` = **0** -- ningun
plazo nace vencido --, DK64 clava la traza por campo 13 corridas de 13 y SM64 4 de 4, y las dos
coinciden con Lockstep linea a linea a 300 campos. Cuesta ~10 % de pared en DK64 (barrera del
RDP; la del RSP es gratis) y sigue por encima de tiempo real. Perillas `KESTREL_DPBARRIER=0` /
`KESTREL_SPBARRIER=0` / `KESTREL_FIELDTRACE=1` (traza por campo, para bisecar).

**Verificacion.** `gate_all` rc=0 en 460 s y `gate_prdp` rc=0 en 368 s: siete modos
`Base 0/3721 Timing 0/2 Cycle 0/6`, md5 de sm64 `d35bd8aa9b13d459ce9332c07a79a53a` (interp) y
`b5521b24d8fc280fbf102df22d7d30cb` (parallel-RDP) sin cambio, krom interp 371/371
`88,73`/`92,08` con regress=0 improve=0 new=0, krom prdp 371/371 `89,27`/`92,56` con regress=0
improve=2 (los dos Cube animados de siempre, desfase de cuadro), nodump=0.


## 2026-09-11 — Threaded determinista de verdad: horario del RDP en tiempo de envio + aparcamiento del RSP

Lo de arriba (2026-09-10) dejaba clavadas todas las columnas que el invitado puede ver, pero
quedaba una que no: `rsp=` (los ciclos del RSP). Derivaba sobre 104-115 M y las columnas de
invitado de DK64 solo cuadraban hasta el campo 206. Cerrado hoy. Detalle completo en
`docs/wip/README.md`; resumen:

- **Horario del RDP en tiempo de envio.** `rdpSubmit` corre el modelo de coste bajo `rdpMx` y
  archiva el tramo entero (`dpScheduleSpan`) con instante de arranque y de cierre en un anillo
  de 256; el worker solo pinta. Ocupado/libre, `DPC_CURRENT` y `END_VALID` salen de ese anillo
  y del reloj de QUIEN pregunta, nunca del estado del anfitrion.
- **Reloj exacto del RSP** (`exactCycles`/`publishExact`, `Memory::rspGuestNowAt`): el lado del
  RSP pregunta con su propio instante de invitado.
- **La fuga que quedaba no era desorden de envio** (`ooo=0` en todas las corridas: la barrera de
  invitado del SP ya ata la CPU por detras del RSP) sino **lecturas de horario rancias**: el
  microcodigo sondea `DPC_CURRENT` desde un instante POSTERIOR al de la CPU, y la CPU archiva
  despues el siguiente buffer con `kick = cartNow()`, o sea ANTES de instantes ya contestados.
  42-47 por cada 300 campos, todos del hilo de la CPU.
- **Aparcamiento del RSP** (`Rsp::idleSkip` + `Memory::rspParkWait`). El bucle de espera del
  FIFO de F3DEX2 (IMEM 0x2a0, cinco instrucciones) no tiene efecto lateral y con el motor
  drenado `DPC_CURRENT` es constante. Se reconoce por su firma leida del flujo -- mismo PC,
  mismo FNV-1a de `r[1..31]`, mismo valor devuelto, misma distancia en ciclos, cuerpo de 2 a 64
  ciclos, `dpDrainedAt(now)` -- sin hardcodear nada. Aparcado, el RSP no puede levantar
  interrupcion ni escribir `DPC_END` ni tocar memoria, asi que la barrera del SP se levanta y la
  CPU corre libre; lo despierta el `kick` del tramo siguiente (instante de invitado) y se le
  cobran las iteraciones enteras que caben. **Solo en Threaded**: en Lockstep los dos chips
  comparten hilo y aparcar al RSP para la maquina entera (DK64 se quedaba muerto en f=67).
  `KESTREL_RSPIDLE=0` lo apaga sin mover el md5 del framebuffer.
- **La cita de lectura del FIFO** (`dpReadSync`, `KESTREL_DPRDV`) resolvia lo mismo por la via
  cara y pasa a nacer APAGADA: su ventana `kRdvLead` era la unica causa de que 10-13 fechas de
  fin de SP nacieran tarde. Se deja a mano porque cubre un caso que el aparcamiento no toca
  (sondear `DPC_CURRENT` con un tramo ABIERTO y por delante de la CPU).
- **Dos carreras mas**, que a 3 corridas no se veian y a 6 divergian 1 de cada 3-4: el aviso de
  despertar se perdia si el tramo se archivaba entre que `idleSkip` veia el motor drenado y que
  `rspParkWait` publicaba el aparcamiento (ahora se comprueba a mano con `dpSubSeq`/`seq0`), y
  la barrera del SP se quedaba abierta durante toda la latencia de despertar del anfitrion
  (ahora se cierra en cuanto hay fecha de despertar publicada, que la pone el propio hilo de
  CPU). Y el `[ft]` se escribia con `fprintf` y salia partido entre hilos: ahora es un solo
  `fwrite`.
- **Savestates**: el horario no se serializa (es derivado, y el estado se toma con el RCP en
  reposo) sino que se REINICIA al cargar con `Memory::rcpSchedReset` desde `afterLoad`.
  `System::quiesceRcp` despierta antes al RSP aparcado (`rspParkNudge`).

**Medida** (DK64 PAL, 300 campos, traza por campo entera, columna `rsp=` incluida):

| build | corridas | md5 de la traza |
|---|---|---|
| `build-prdp/` Threaded | 6/6 | `d3bee5263f26e4dca3519e277220e090` |
| `build/` (SoftRDP) Threaded | 4/4 | `b43236f9c027f4a0cbaf19df040e0ffc` |
| `build-prdp/` Lockstep | 2/2 | `ce4ec2bc2d6298cb69d270607db199ee`, identico con `KESTREL_RSPIDLE=0` |

`[det]` clavado: `rspCycles=60771479`, `spArm=173/0 tarde`, `dpArm=56/0 tarde`, `stale=0`,
`ooo=0`, `park=48/0`, `idle=48/6621806`, `dpcRd=5658`.

**Coste**: 13,9 M sondeos emulados pasan a 5,7 k, pero el aparcamiento serializa los dos hilos a
grano de tarea: la corrida de 300 campos va de 6 s (no determinista) a 9 s SoftRDP / 8 s
Parallel-RDP. Recuperar ese solape es el siguiente punto de rendimiento y es independiente de la
correccion.

**Verificacion.** `gate_all` rc=0 en 468 s y `gate_prdp` rc=0 en 334 s (linea base 481 / 376):
ocho modos `Base 0/3721 Timing 0/2 Cycle 0/6`, `rsp_test`/`save_test`/`cheat_test`/
`archive_test`/`wildmem_test`/`rewind_test` ALL PASS, md5 de sm64
`d35bd8aa9b13d459ce9332c07a79a53a` (interp) y `b5521b24d8fc280fbf102df22d7d30cb` (parallel-RDP)
sin cambio -- tambien con `KESTREL_RSPIDLE=0` --, krom interp 371/371 `88,73`/`92,08` regress=0,
krom prdp 371/371 `89,27`/`92,56` regress=0 improve=2 (los dos Cube animados de siempre).

## 2026-09-11 — Medidor de ocupacion del bus de RDRAM (se acabo el `RAM0%`)

El pie de velocidad de la ventana enseña `CPU% RSP% RAM%` y el tercero salia SIEMPRE 0:
`System::rdramSpeedPct` nacia `{0.0}` y no lo escribia nadie ("RDRAM has no per-transaction
cycle model yet"). Ya lo hay, y no como estimacion: se cuentan bytes donde el trafico ocurre.

**Que se cuenta y donde.**

| maestro | contador | de donde salen los bytes |
|---|---|---|
| CPU | `CPU::ramCpuBytes` | relleno de linea de D$ 16 B (`dcMiss`/`dcFill`), volcado de linea sucia 16 B (`dcFlush` y la rama sucia de `dcMiss`), relleno de I$ 32 B (`icFill`), y accesos NO cacheados -- KSEG1 o pagina de TLB con C=2 -- que caen dentro de la RDRAM (`CPU::ramUncached`, en el interprete y en los dos ayudantes de memoria del dynarec) |
| RSP | `Memory::ramBytesRsp` | `spDma`: `length * count`; el salto entre filas no se transfiere |
| RDP | `Memory::ramBytesRdp` | derivado de la MISMA lista de transacciones que ya usa el modelo de coste calibrado (`SoftRdp::accountPixels`): por chunk, `ciBpp` si hay IM_RD mas 2 B si hay Z_CMP, y encima la escritura de color mas la de z para los pixeles que no mueren en alfa/profundidad. Mas `accountTmem` en bytes directos. **Ninguna constante nueva que calibrar** |
| VI | `Memory::ramBytesVi` | el VI no tiene memoria propia: relee la imagen de RDRAM cada campo, una linea por linea de salida. `vi_width * lines * bpp`, con `lines` sacado de `VI_V_START` (va en medias lineas) y `bpp` del tipo de `VI_CONTROL` |
| PI | `Memory::ramBytesPi` | las tres ramas de `piDma`, incluida RDRAM -> ROM (en la consola el motor LEE la RDRAM aunque no aterrice nada: el bus se ocupa igual) |
| AI | `Memory::ramBytesAi` | el buffer al aceptarlo en `AI_LEN`; el DAC acaba leyendolo entero |
| SI | `Memory::ramBytesSi` | los 64 B del bloque del PIF por DMA |

**El divisor es tiempo de INVITADO, no de pared.** `retiradas / Clocks::insnTarget()`. El bus de
la consola es de 562,5 MB/s (2 chips de 9 bits a 250 MHz DDR = 4,5 Gbit/s) pase lo que pase, asi
que el porcentaje tiene que salir igual corra el emulador al 20 % o al 300 %: es una propiedad
del juego. Con la ventana parada (cero instrucciones) se conserva el valor anterior en vez de
dividir por cero.

**Es un SUELO declarado, no una cota** (`Memory::kRdramPeakBps` lo dice en el codigo): no entran
el refresco de RDRAM, el noveno bit (paridad / cobertura oculta) ni las lecturas del latch del
cartucho, que no tocan RDRAM.

**Salida nueva.** Linea `[rdram]` al terminar la corrida y otra igual en el `[hb]` cada 5 s, con
el reparto por maestro en MB/s de invitado:

```
[rdram] 7.8% del bus (43.8 MB/s de invitado en 6.67 s): cpu 8.3 rdp 20.3 vi 9.1 rsp 5.7 pi 0.3 ai 0.1 si 0.0   <- SM64, titulo, 400 campos
[rdram] 11.4% del bus (64.3 MB/s de invitado en 8.00 s): cpu 7.5 rdp 30.2 vi 21.8 rsp 4.2 pi 0.2 ai 0.0 si 0.0  <- DK64 PAL, 400 campos
```

La comprobacion de que el numero no esta inventado es el VI: 320x237x2 a 60 Hz son ~9 MB/s con
la pantalla quieta, y eso es exactamente lo que mide en SM64. En DK64 sube a 21,8 porque el
scanout es de 640 de ancho.

Tambien: `rspParkMiss` (las carreras de publicacion del aparcamiento cazadas a mano) ya sale en
`[det]`, que pasa a `park=aparcadas/salvavidas/carreras`.

**Verificacion.** Nada de esto es entrada de control -- atomicos `relaxed` y, en la CPU, un
contador liso que solo escribe su propio hilo --, y se comprueba en vez de suponerse: DK64 PAL
Threaded en `build-prdp`, 300 campos, 3/3 `d3bee5263f26e4dca3519e277220e090`, el MISMO md5 que
antes del cambio. `gate_all` rc=0 en 484 s y `gate_prdp` rc=0 en 343 s: ocho modos
`Base 0/3721 Timing 0/2 Cycle 0/6`, sm64 `d35bd8aa9b13d459ce9332c07a79a53a` (interp) y
`b5521b24d8fc280fbf102df22d7d30cb` (parallel-RDP), krom interp 371/371 `88,73` regress=0, krom
prdp 371/371 `89,27` regress=0 improve=2.

## 2026-09-11 — El plazo de la barrera del SP se media con el reloj equivocado (DK64 7,4 s → 2,83 s)

La fila de arriba dejaba el aparcamiento del RSP correcto pero **caro**: DK64 PAL a 300 campos
pasaba de 6 s (no determinista, sin aparcamiento) a 8-9 s. Se atribuyo a "el aparcamiento
serializa los dos hilos a grano de tarea". Era falso: el aparcamiento no serializaba nada, lo
que se caia era el **dynarec**, y por un error de una linea.

`Memory::rcpDueIn(now)` devuelve el plazo del RCP mas cercano en unidades de reloj de invitado,
y `CPU::jitTryBlock` lo usa para recortar el bloque para que ninguno se trague un evento. Para
el bit 8 (`rcpPend`, fin de barrera del SP) usaba `spBarrierAt()`:

```cpp
spBarrierAt() = spKickOps + rcpCyclesToOps(rsp.cyclesRun - spKickCycles);
```

Con el RSP **aparcado** su `cyclesRun` esta congelado por definicion, asi que `spBarrierAt()` se
queda clavado detras del invitado y `rcpDueIn` devolvia **0** durante todo el aparcamiento. En
`jitTryBlock`, `if(siDue <= kTicks) return 0;` declinaba entonces *cada* bloque, la concesion
encadenada (`jitGuard`) se venia abajo y la CPU bajaba a interpretar de una en una --
exactamente en la ventana en la que es el unico hilo que puede desatascar la escena, porque el
RSP esta esperando el buffer que ella tiene que instalar.

Pero quien decide de verdad donde se para la CPU no es `spBarrierAt()` sino `spBarrierWait`, y
`spBarrierWait` mira `spBarrierEff()`, que con el RSP aparcado y sin fecha de despertar
publicada vale `rspPark + kParkLead`. Un plazo tiene que ser el instante en que la CPU se va a
PARAR de verdad. Arreglo, una linea:

```cpp
if(pend & 8u) {
  u64 b = spBarrierEff();          // antes: spBarrierAt()
  u64 e = b > now ? b - now : 0;
  if(e < d) d = e;
}
```

**Cadena de diagnostico** (las tres piezas de telemetria que la cerraron se quedan):

1. Linea `[block]` nueva al terminar: reparto del tiempo de PARED entre espera de CPU (freno /
   barrera SP / barrera DP), ocupacion del RSP, **aparcamiento** del RSP y ocupacion del RDP,
   mas el tiempo de CPU REAL de los tres hilos (`GetThreadTimes`). Decia que con el
   aparcamiento puesto no habia nada ocupado y aun asi sobraba un 63 % de pared.
2. `rspParkNs`: el sueno del aparcamiento cae DENTRO de `rsp.step()`, asi que sin descontarlo
   `rspBusyNs` contaba como trabajo un hilo dormido -- DK64 marcaba "rsp 92 % ocupado" con un
   2 % de CPU real. Se descuenta en `rspWorkerLoop`.
3. `cpuCpuNs` (mismo muestreo para el hilo de CPU) daba `cpu 99,8 %`: el hilo de CPU estaba
   QUEMANDO ciclos de anfitrion, no durmiendo. Y `KESTREL_JIT_STATS` remataba: **43 M** entradas
   al driver del JIT con `cover=3,9 % avgK=0,08` contra **menos de 1 M** sin aparcamiento, con
   las dos corridas retirando las MISMAS 402 M instrucciones.
4. `KESTREL_PARKLOG` (opt-in, clase `KESTREL_DPSYNCLOG`) imprime `[pk]` por aparcamiento: via de
   salida, salto, `cartNow()` y vueltas. Los 48 salian `via=wake` con `spins=3..10`, o sea la
   CPU tardaba 60-300 ms de pared en avanzar 520 k-1,2 M ops de invitado.

**Medida** (DK64 PAL, 300 campos, `build-prdp`, Threaded):

| | antes del arreglo | despues |
|---|---|---|
| aparcamiento ON | 7,33-7,71 s | **2,83 s** |
| aparcamiento OFF (`KESTREL_RSPIDLE=0`) | 4,0-4,5 s | 4,01 s |
| `[block]` con ON | `cpuWait 2,7 % / rsp ocupado 2,2 % aparcado 76,5 % / rdp 3,9 % / CPU real cpu 99,8 %` | `cpuWait 7,3 % (freno 0,0 barSP 0,3 barDP 2,2) / rsp ocupado 5,6 % aparcado 47,0 % / rdp 10,0 % / CPU real cpu 100,3 %` |

El aparcamiento ya no cuesta: sale **mas rapido que apagarlo** (2,83 s vs 4,01 s) y mas rapido
que la linea base de 6 s de antes de todo esto. El solape perdido no se recupera, se invierte.

**Determinismo**. 3/3 trazas byte-identicas, md5 `ccf3fd5bf216c429db341ddc5164df31`, `[det]`
clavado (`rspCycles=60771443`, `spArm=173/0 tarde`, `dpArm=56/0 tarde`, `ooo=0`, `stale=0`,
`idle=48/6621800`, `park=48/0/0`). Contra un binario de antes del arreglo reconstruido a
proposito (`d3bee5263f26e4dca3519e277220e090`, `rspCycles=60771479`, `idle=48/6621806`):
difieren **44 lineas de 300, desde f=257, y SOLO en la columna `rsp=`**; quitando esa columna
las dos corridas dan el mismo md5 `417ae0aa5c60abf6dae491c9cd4af994`. Todas las columnas que el
invitado puede ver (`ret`, `ops`, `gclk`, `sp`, `dp`, `flips`, `syncs`, `org`, `mi`) son
identicas byte a byte. El desplazamiento de 36 ciclos viene de que el largo del bloque del JIT
cambia la cuantizacion del instante `kick` que la CPU estampa en los tramos que archiva, y ese
`kick` entra en `k = rcpOpsToCycles(tgt - now) / len` dentro de `idleSkip`.

**Verificacion.** `gate_all` rc=0 en 466 s y `gate_prdp` rc=0 en 344 s (linea base 481 / 376 y
468 / 334): ocho modos `Base 0/3721 Timing 0/2 Cycle 0/6`, sm64
`d35bd8aa9b13d459ce9332c07a79a53a` (interp, los seis modos) y
`b5521b24d8fc280fbf102df22d7d30cb` (parallel-RDP), krom interp 371/371 `mean_exact` 88,73 /
`mean_close` 92,08 regress=0 improve=0 new=0, krom prdp 371/371 89,27 / 92,56 regress=0
improve=2 (los dos Cube animados de siempre), nodump=0 en ambos.

## 2026-09-11 — El freno del dynarec frenaba tambien al RSP aparcado (DK64 2,98 s → 1,71 s)

El camino rapido del prologo del dynarec (`src/cpu/jit.cpp`, ~1181-1245) comprueba cuatro cosas
antes de saltar al cuerpo del bloque y encadenar con el siguiente: el permiso restante
(`jitGuard`), `MI_INTR & MI_MASK`, el pestillo de `timerIntr` y un byte del RSP. Si alguna falla
devuelve el control al trampolin, que es una llamada Win64 por bloque. El byte del RSP era
`Rsp::running`.

`[tramp] guard/MI/timer/rsp/otro` lo delato: **22 M** rebotes por el RSP en una corrida de 300
campos de DK64, y el `[block]` de la misma corrida decia que el RSP estaba **aparcado el 47,1 %
del tiempo de pared**. `Rsp::running` sigue puesto durante todo el aparcamiento
(`Memory::rspParkWait`), asi que la cadena se rompia en cada eslabon justo en la ventana en la
que la CPU es el unico hilo que puede desatascar la escena: es ella quien archiva el tramo cuyo
`kick` despierta al RSP.

Y ahi el freno no regula NADA. Un RSP aparcado no ejecuta microcodigo, no escribe MMIO y no
puede levantar interrupcion; a la CPU la siguen acotando la barrera de invitado del SP
(`spBarrierEff()` → `rspPark + kParkLead`) y `rcpPace`, las dos en reloj de invitado. El freno
del prologo hace falta cuando el RSP **si** corre — ver la nota larga del prologo en `jit.cpp` —
y solo entonces.

**El cambio.** Bandera nueva `Rsp::brake` = `running` MENOS el aparcamiento. Vive en la misma
linea fria de 64 bytes que `running` (que esta `alignas(64)` y aislada a proposito: el hilo de
CPU la lee sin parar y la invalidacion entre nucleos costo una vez el 18 % del emulador entero).
Se pone y se quita en los cinco sitios donde `running` cambia de verdad — `Rsp::start`, el
camino de BREAK, el de presupuesto agotado, el `mtc0` de SP_STATUS y la lambda del banco de
pruebas — y ademas `rspParkWait` la SUELTA antes de publicar el aparcamiento y la vuelve a poner
antes de retirarlo. El prologo y la clasificacion del trampolin miran `brake`. La comprobacion
de Lockstep en `jitReenterProceed` sigue mirando `running`, que es lo que quiere saber ahi.

**Medida** (DK64 PAL, 300 campos, `build-prdp/`, Threaded + JIT):

| | antes | despues |
|---|---|---|
| pared | 2,98 s | **1,71 s** (−43 %) |
| `[tramp] rsp=` | 22 M | 0 |
| rastro de campos | `ccf3fd5bf216c429db341ddc5164df31` | **el mismo**, 3/3 byte a byte |
| Lockstep | `ce4ec2bc2d6298cb69d270607db199ee` | **el mismo**, 2/2 |

`[block]` despues: `pared 1.71 s | cpuWait 14.4% (freno 0.0% barSP 0.6% barDP 4.3%) | rsp
ocupado 9.6% aparcado 10.8% | rdp ocupado 16.8% | CPU real: cpu 99.5% rsp 6.3% rdp 14.6%`. El
`[det]` no se mueve en ninguna columna salvo `await=`, que es un contador de telemetria.

Esto cierra el apunte "recuperar el solape CPU/RSP que se perdio con el aparcamiento" que dejo
la entrada del 2026-09-10: el aparcamiento ya no cuesta nada, es la mas rapida de las tres
configuraciones.

## 2026-09-11 — El rastro por campo llevaba cuatro columnas medidas en tiempo de anfitrion

`KESTREL_FIELDTRACE=1` existe para una cosa: comparar por md5 dos corridas del mismo binario y
encontrar el primer campo que difiere. Cuatro de sus once columnas no podian usarse para eso.

**`sp=` y `dp=` eran ARMADOS, no retiros.** Salian de `spArms`/`dpArms`, que suben en
`Memory::spEndArm` y `Memory::dpEndArmAt` — y en Threaded a esas dos las llama el hilo del
RSP/RDP cuando *termina el trabajo en tiempo de pared*. El plazo que arman si esta en reloj de
invitado y lo publica el hilo de CPU en el instante exacto que le toca, asi que el invitado
nunca ve nada distinto; pero el contador sube en un campo o en el siguiente segun como se
crucen los hilos. Contadores nuevos `spRets`/`dpRets`, que suben SIEMPRE donde la interrupcion
se hace visible al invitado: `Memory::rcpFlushPending` (el camino normal, hilo de CPU) y los
cuatro sitios de publicacion directa (`rsp.cpp` para el camino sin plazo, los dos caminos de
SYNC_FULL sin diferir). `[ft]` mira los retiros; `[det]` sigue enseñando `spArm=`/`dpArm=`,
que es donde ese dato tiene sentido.

**`rsp=` y `gclk=` son avance real de los workers y no se pueden arreglar**, porque son
justamente eso: `Rsp::cyclesRun` y `rcp.rdpGclk` muestreados en el limite de campo valen lo que
valgan segun donde estuviera cada hilo en ese instante. Pasan a `KESTREL_FIELDTRACE=2`. El
nivel 1, que es el que se difunde y se compara, solo lleva columnas que el invitado puede ver.

**Resultado.** Con el nivel 1, DK64 PAL 300 campos da `4fc7dc59e262d5a8a0cd2c89712b8ae3`
**3/3 en Parallel-RDP y 2/2 en SoftRDP — el mismo md5 en los dos motores**, y SM64 NTSC
`a022f09184a1fa61917565cc7909630f` **6/6**.

De paso, una falsa alarma que conviene dejar escrita: SM64 parecia no determinista con 2 trazas
distintas en 6 corridas, y no lo era. La primera corrida no encontraba
`Super Mario 64 (USA).eep` (lo crea al salir) y la segunda si, o sea que arrancaban con estado
inicial distinto: sin partida guardada el juego tarda 72 campos mas en llegar a la primera tarea
grafica (71 intercambios contra 95 en 300 campos). **Borrar el fichero de guardado antes de cada
corrida forma parte del experimento.** Y `ret`/`ops` NO sirven para ver si el invitado progresa:
son `viFields * instrucciones-por-campo`, o sea el reloj, y salen identicos aunque el juego se
quede parado.

**Abierto.** DK64 en Lockstep y en Threaded no dan el mismo rastro: divergen desde f=191 y la
columna que se mueve primero es `syncs=` (`rcp.dpSyncs`), que este cambio no toca. Es anterior a
el. Apuntado en `docs/GAPS.md`.

## 2026-09-11 — El hilo ocioso del invitado se cobra de golpe (SM64 threaded-jit -14 %)

`beq $0,$0,-1` con `nop` en la ranura de retardo es el hilo ocioso de libultra. Ese bucle no
escribe ningun registro, no toca memoria y lo unico que produce es Count: el VR4300 no sale de
ahi mas que por una excepcion. Medido con `KESTREL_PCSAMPLE=0x80000000` (que ahora, cuando el
valor es una direccion de KSEG0, anade un histograma por instruccion de 1024 ranuras de esa
pagina ademas del de paginas): en DK64 son **251,5 M de las 402,3 M** instrucciones de invitado
de los primeros 300 campos, el **62,5 %**, todas en `0x80000a08`.

Emular esas vueltas una a una no produce NADA observable. `CPU::jitIdleSkip(u32 phys)` las cobra
de golpe: reconoce la firma en el flujo (BEQ rX,rX,-1 o REGIMM BGEZ $0,-1, mas ranura de retardo
= NOP exacto) y suma `retired`, Count y Random igual que hace el commit del trampolin.

**Lo que hace que esto NO sea un atajo que cambie el emulador es el limite.** El salto es
EXACTAMENTE el permiso que `jitReenterProceed` le concede a una cadena enlazada, calculado con
la misma aritmetica: borde de Compare, plazo del SI (`siDueIn`), plazo del RCP (`rcpDueIn`),
`jitOpsBudget` (la ventana de campo del bucle del sistema), el regulador `rcpPace` en Threaded y
el tope `kGuardMaxOps` = 4096. O sea que **no se cambia CUANDO se vuelve a mirar cada evento**,
solo se deja de emular lo que hay entre dos miradas. Y se redondea a iteraciones ENTERAS
(`k & ~1`) para que el pc no se quede a medias del par salto+ranura.

Dos negativas que son parte de la semantica, no prudencia:

- **`Status[2:0] != 1` (IE=0, o EXL/ERL puestos): no se salta.** Ahi ninguna interrupcion puede
  sacar al invitado del bucle, o sea que el juego esta colgado DE VERDAD; saltarle el reloj
  esconderia el cuelgue en vez de ensenarlo. Que lo ejecute el camino normal, que es donde
  estan el watchdog y los volcados.
- **Threaded SIN plazos (`KESTREL_RCPDEADLINE=0`): no se salta.** Es el unico modo en el que un
  worker publica `MI_SP`/`MI_DP` por su cuenta en tiempo de PARED, sin plazo de invitado que
  acote el salto; la interrupcion se veria hasta 4096 ops tarde.

`KESTREL_CPUIDLE=0` lo apaga, y con el apagado tiene que salir todo identico: es atajo de
anfitrion, no de semantica. `KESTREL_JIT_STATS` saca ademas la linea `[ocioso] saltos=N ops=NM
(x% de las retiradas)`.

**Medido** (SM64, `bench` de 200 intercambios = 597 campos VI = 9,96 s de video, minimo de tres
corridas, `threaded-jit`): **5,13 s con el salto contra 5,98 s sin el**, o sea **-14,2 % de
pared**, 194,2 % de tiempo real contra 166,6 %. En DK64 el techo teorico es mayor (62,5 % de las
instrucciones contra ~20 % en SM64) pero ahi manda el RCP.

**Certificacion.** `gate_all` rc=0 en **458 s** (linea base 473-488) y `gate_prdp` rc=0 en
**350 s** (linea base 347). Seis `*_test` ALL PASS; ocho modos `systemtest Base 0/3721 Timing
0/2 Cycle 0/6`; sm64 `d35bd8aa9b13d459ce9332c07a79a53a` en interp / jit / jit-nolink / threaded
/ threaded-jit / rspinterp / rspnolink y `b5521b24d8fc280fbf102df22d7d30cb` en prdp y prdp-jit;
krom interp 371/371 `mean_exact` 88,73 / `mean_close` 92,08 regress=0; krom prdp 371/371 89,27 /
92,56 regress=0 improve=2 (los dos `CubeFillTriangle` animados, 53,09 -> 55,27, fase de
animacion); nodump=0 en ambas.

## 2026-09-11 — La biblioteca del lanzador web pasa a 3D de verdad (carrusel, coverflow, pared)

Las caratulas del lanzador eran laminas con `transform: perspective()` de CSS. Ahora son
**cajas de carton con volumen** en el motor WebGL propio (`web/gl.js` + `MODELS.buildBox`), y el
selector de vista ofrece las tres colocaciones al estilo de USB Loader GX: anillo giratorio,
coverflow y pared de rejilla. Detalle de diseno en `docs/LAUNCHER.md`; lo que importa aqui es
que lo hace posible:

- **Matriz por tramo de malla** (`part.mat`, `part.dim`, `part.hidden` y `Scene.order` en
  `gl.js`). Las K cajas se suben a la GPU UNA vez; moverse por la biblioteca o cambiar de
  colocacion no reconstruye geometria ni buffers, solo cambia K uniformes por cuadro. 25 cajas =
  2900 vertices, 2500 triangulos. `Scene.order` permite reordenar el dibujo de atras hacia
  delante cada cuadro, que es lo que hace que la caja elegida tape a las vecinas.
- **Casillas recicladas por modulo** (`SCENE3D.slotItem`, funcion pura). Solo hay K = 25 cajas y
  el juego `i` vive siempre en la casilla `i % K`: al desplazarse la unica casilla que cambia de
  juego -- y por tanto de textura -- es la que acaba de salir de la ventana. Mil cartuchos
  cuestan por cuadro lo mismo que veinticinco.

Pruebas: `node tools/launcher/carousel_test.js` (sin navegador) comprueba, para listas de 0 a
397 juegos, que ninguna casilla repite juego, que el juego elegido SIEMPRE tiene casilla (si no,
el centro del carrusel saldria vacio), que la ventana no tiene huecos, que un centro fraccionario
reparte como el entero, y que la malla de 25 cajas cabe en `Uint16` con una portada por casilla
con su id de seleccion. `web/smoke_car.html` monta las tres colocaciones con caratulas pintadas
al vuelo, sin servidor ni ROMs; verificado sin ventana con Edge headless + SwiftShader
(`--virtual-time-budget=4000`, que hace falta porque el dibujo va por `requestAnimationFrame`).

Sin WebGL las tres vistas caen a las de CSS de antes, que se quedan justo para eso.

## 2026-09-11 — El orden de las optimizaciones vuelve a salir de una medida, no de la memoria

La lista de "Rendimiento" de `docs/GAPS.md` estaba ordenada por una medida vieja que ya no era
cierta: encabezaba `dcFill`/`dcFlush`, que en un perfil de anfitrion de hoy **no aparecen**. Se
ha vuelto a medir y se ha reescrito el orden entero.

**Como se midio.** `KESTREL_HOSTPROF=1` sobre SM64, `build-prdp`, `threaded-jit`, PRDP, 400
intercambios: 906 muestras del hilo de CPU, 769 dentro de la imagen, simbolizadas contra
`llvm-nm --numeric-sort` con `ImageBase 0x140000000`. Reparto:

| % | funcion |
|---|---------|
| 43,43 | `Memory::spBarrierWait` |
| 34,46 | `Memory::dpBarrierWait` |
| 6,63 | `kestrel_jitProceedTramp` |
| 5,72 | `Memory::rspPace` |
| 5,07 | `CPU::jitIdleSkip` |
| 1,30 | `Memory::rdpPace` |
| 0,65 | `Memory::rcpPace` |
| 0,65 | `CPU::jitTryBlock` |

Cruzado con `[block]` (`cpuWait 20,2 % — freno 0,0 % barSP 1,2 % barDP 31,5 %`, `rsp ocupado
65,7 %`, `rdp ocupado 16,8 %`), la lectura es que **ese 43 % NO es tiempo bloqueado**: `barSP`
bloquea el 1,2 % del `cpuWait`. Es la espera ACTIVA de 2048 vueltas girando sobre lineas de
cache que escribe el hilo del RSP. El palo largo de verdad es el RSP.

**Pista `PAUSE` en las tres esperas activas** (`Memory::spBarrierWait` y las dos fases de
`dpReadSync`). `_mm_pause` le dice al nucleo que la vuelta es una espera: no le roba la linea en
exclusiva al que la va a soltar ni le quita ranuras de emision al hermano SMT. Va a **una de cada
16 vueltas** a proposito, porque PAUSE no cuesta lo mismo en todos los anfitriones (~9 ciclos en
Nehalem, ~140 de Skylake en adelante) y una por vuelta estiraria un giro de 2048 dos ordenes de
magnitud en una maquina moderna. Apagable con `KESTREL_SPINPAUSE=0` y largo del giro con
`KESTREL_BARSPIN=<n>` (0 = el defecto, 2048); las dos salen en el lanzador (avanzadas).

**Medida honesta: en ESTE anfitrion es NEUTRA.** Un primer A/B de 3 corridas daba -2,3 %, pero
al repetirlo intercalado (dos rondas de 5 corridas, `bench --mode threaded-jit`) el minimo sale
OFF 4,93 s / 5,04 s contra ON 4,98 s / 4,98 s: ruido. Se queda puesta igual porque es la
semantica correcta de una espera activa y porque el i7-870 es justo el caso donde menos da (SMT
viejo, PAUSE barato); lo que NO se hace es apuntarse una ganancia que no existe.

**`[det] idle=` ya dice POR QUE no se aparca el RSP.** El desglose nuevo es
`idle=saltos/vueltas(sigN/drnN/roomN)`: firma distinta, motor del RDP sin drenar, o sin hueco
donde aparcar. Con el queda **contestado y cerrado** el `idle=0/0` de SM64: los 1 041 389 sondeos
de `DPC_CURRENT` se van ENTEROS por `sig`. Desglosada la firma, el PC si se repite (1 038 015) y
la longitud del cuerpo cae en [2,64] en 665 177, pero la huella de `r[1..31]` **no coincide ni
una vez** y el valor leido solo 1 356. Es decir: el RSP sondea mientras el RDP esta MASTICANDO
(`rdp ocupado 54,5 %`, `open=317 k`), asi que el valor se mueve en cada vuelta y no hay bucle de
espera con valor fijo que aparcar — que es exactamente la condicion del aparcamiento. En DK64 si
se cumple porque alli el microcodigo espera con el motor drenado. **No es un fallo.** Y el sondeo
tampoco es el gasto: ~5 M de instrucciones de RSP sobre 291 M de ciclos, un 1,7 %. Lo que cuesta
es el trabajo vectorial, y eso lo ataca el JIT del RSP.

Los contadores nuevos (`idleNoSig`, `idleNoDrain`, `idleNoRoom`) los toca solo el hilo del RSP,
por eso no son atomicos, y los lee el volcado final con el RCP ya parado. `rcpSchedReset` los
pone a cero con el resto del aparcamiento.

**Certificacion.** `gate_all` rc=0 en **473 s** (linea base 458-488) y `gate_prdp` rc=0 en
**381 s** (linea base 347-350; el krom de GPU sube a 315 s). Seis `*_test` ALL PASS; ocho modos
`systemtest Base 0/3721 Timing 0/2 Cycle 0/6`; sm64 `d35bd8aa9b13d459ce9332c07a79a53a` en
interp / jit / jit-nolink / threaded / threaded-jit / rspinterp / rspnolink y
`b5521b24d8fc280fbf102df22d7d30cb` en prdp y prdp-jit; krom interp 371/371 `mean_exact` 88,73 /
`mean_close` 92,08 regress=0 improve=0; krom prdp 371/371 89,27 / 92,56 regress=0 improve=2 (los
dos `CubeFillTriangle` animados, 53,09 -> 55,27, fase de animacion); nodump=0 en ambas.

## 2026-09-11 — `PaceDiv`: el freno del RCP deja de dividir (exacto, no aproximado)

`Memory::rspPace` y `Memory::rdpPace` calculan el permiso de la CPU con
`(trabajo_del_RCP * paceCpuNum) / paceCpuDen`, y eso es una **division entera de 64 bits por
llamada** al trampolin. El divisor no cambia nunca en toda la ejecucion: `System` fija el modelo
de reloj una vez (`setPaceRatio(rspStepDen, rspStepNum)`, o sea 65536/43691 — y **no** es 3/4,
como decia el valor inicial de la cabecera). Con divisor fijo se puede precalcular el reciproco.

**Lo que hace exacto el reciproco** (`struct PaceDiv` en `memory.hpp`): con `m = ceil(2^79/d)` y
`e = m*d - 2^79` (que cumple `0 <= e <= d`), `floor(x*m >> 79) == floor(x/d)` **exactamente** para
todo `x <= floor(2^79/e)`. Ese `maxX` se guarda con el multiplicador. Si `m` no cabe en 64 bits, o
si el `x` que llega se pasa de `maxX`, se divide de verdad. No es una aproximacion con error
acotado: o da el MISMO numero que la division, o no se usa. Comprobado a mano en una prueba
aparte, 32 000 070 casos y 0 fallos, con los bordes metidos a proposito (`maxX-1`, `maxX`,
`maxX+1`, `2^63`, `~0ull`) y barriendo todos los divisores que el modelo de reloj puede producir.
`d` potencia de dos da `e == 0` y vale para todo `x`.

Lo tocan `rspPace`, `rdpPace`, `rcpCyclesToOps` y `rcpOpsToCycles`; `setPaceRatio` es ahora el
UNICO sitio que fija el ratio, para que el multiplicador no pueda quedarse viejo.

**Medida honesta: NEUTRA en este anfitrion.** A/B intercalado de dos rondas de 5 corridas
(`bench --mode threaded-jit`, minimo de cada ronda): con reciproco 4,95 s / 4,96 s, con division
4,93 s / 4,98 s. Ruido. El perfil daba `rspPace` 5,72 % + `rdpPace` 1,30 % del hilo de CPU, pero
ese tiempo **no era la division**: es la lectura de `rspBusy` y `rsp.cyclesRun`, dos lineas de
cache que el worker del RSP reescribe sin parar. `paceGrant` ya esta escrito para pagarlas una
sola vez por vuelta y `kPaceGrain` ya evita la cola de permisos que encogen; lo que queda es el
fallo de cache compartida, que es inherente al acoplamiento CPU-RCP. Se queda el reciproco porque
es estrictamente menos trabajo y esta probado exacto, pero **no se le apunta ninguna ganancia**.

## 2026-09-11 — El lanzador tiene temas, y el primero replica WonderMenu

Peticion del usuario: *"este proyecto que es un menu/lanzador de flashcart N64 que quiero
replicar la estetica para el lanzador del emulador: https://github.com/lmcd/WonderMenu"*.

**Lo que se ha hecho no es una piel pegada encima sino una capa de tokens.** Todo el aspecto
de `tools/launcher/web/style.css` ya salia de variables de `:root`; faltaban las que hacian
falta para que un tema pueda cambiar algo mas que colores planos, y siete sitios que todavia
tenian un color a pelo. Ahora hay `--font-display` (con `--disp-track`/`--disp-weight`),
`--bgimg`, `--glass`/`--glass2`, `--onacc` y `--selbg`/`--seltxt`/`--selr`. Un tema es un
bloque `:root[data-theme="..."]` y nada mas; `applyTheme()` en `app.js` solo pone el atributo
en `<html>` y el navegador reevalua los tokens solo --- no repinta nada a mano, no vuelve a
montar las escenas 3D. Tema desconocido cae al de fabrica. Se guarda en el perfil como
`theme`, con el mismo camino que `viewmode`.

**Tema WonderMenu** (`docs/LAUNCHER.md` lleva el detalle). Sale de leer la fuente del
original, no de mirar capturas: su `util/Color.h` solo define `CLEAR`/`BLACK`/`WHITE`/`RED`/
`GREEN`/`BLUE` (de ahi el monocromo), el subtitulo es el MISMO blanco del titulo con el alfa a
la mitad (`a *= 0.5`), lo elegido es una pastilla MACIZA de radio 17 expandida y 8 normal que
sobresale por los dos lados y crece 5 px de alto, la etiqueta de una fila arranca a 112 px
del borde con el subtitulo 20 px por debajo, los accesorios van a 12 px de separacion con 15
de margen y el contador ocupa 50 px, y los titulares van en Unbounded 900. **No se copia ni
un recurso**: el proyecto es AGPLv3. La fuente va en pila con caida (`Unbounded`, `Archivo
Black`, `Segoe UI Variable Display`, ...) porque el lanzador tiene que funcionar sin red.

Un desvio a proposito: **el fondo no es negro**. La pastilla del original es negra maciza con
letra blanca y sobre negro no se ve; medido en la captura sin cabeza, con `--bg:#08080b` la
pastilla salia `(0,0,0)` sobre `(8,8,11)`. Se subio a gris carbon `#2b2b32`, que la deja
resaltar sin salirse del monocromo.

**Vista nueva "Lista Wonder"** (`wmrows`, septimo modo de biblioteca): una fila alta por
cartucho con la caratula pequena a la izquierda, titulo en la letra de titular, subtitulo a
media tinta y los datos del cartucho a la derecha, con la pastilla de lo elegido. Sale entera
de tokens, asi que **funciona igual con los dos temas** (con el de fabrica la pastilla es el
ambar al 10 % y el radio 0, con el de WonderMenu negra y de radio 17) sin una sola regla
duplicada.

**Verificacion** (`web/smoke_theme.html`, Edge sin cabeza): selector presente con dos
opciones, `applyTheme()` cambia el atributo, `--selr` 0px contra 17px, `--bg` `#0b0d12` ->
`#2b2b32`, `--acc` `#ffab2e` -> `#ffffff`, `--font-display` con Unbounded, el selector se
sincroniza, un tema inventado cae a `kestrel`, `body` pinta fondo propio `rgb(43,43,50)` (si
fuera transparente tomaria el del anfitrion) y el evento `change` aplica el tema: **TODO
BIEN**. La persistencia se mira desde fuera, porque `CFG` es un `let` de guion clasico y no
cuelga de `window`: tras la pasada, `tools/launcher/profile.json` tiene `theme: "wonder"` (el
fichero se salva y se restaura alrededor de la prueba). Capturas de los dos temas en las dos
vistas. `node --check web/app.js` limpio y `node tools/launcher/carousel_test.js` TODO BIEN.

**GOTCHA que costo un rato**: `--use-gl=swiftshader --disable-gpu` revienta el render sin
cabeza de Edge en cuanto la pagina lleva un iframe que hace `fetch` (`Abnormal renderer
termination`, volcado vacio). Sin esas dos banderas va bien. Y `--dump-dom` vuelca en el
evento `load`, asi que sin `--virtual-time-budget` no se ve nada de lo que escriben los
`setTimeout`.

Cero cambios en el emulador: no toca gates.

## 2026-09-16 — Ficha de juego en el lanzador (info, manual, trucos, partidas)

Elegir un juego ya no lo arranca: abre una ficha con cuatro pestanas. Detalle y API en
`docs/LAUNCHER.md` "Ficha de juego". Doble clic sigue lanzando; `select_action: "play"`
devuelve el comportamiento viejo.

- `tools/launcher/gamecard.py`: nombres de fichero iguales a los del emulador, lector de
  `.cht` igual al de `cheats.cpp`, reescritura que solo toca el `-` de las cabeceras
  (conserva comentarios y CRLF), manuales por nombre de fichero o nombre interno, subida de
  manual a `manuals/`, metadatos por CRC con estadisticas de juego.
- Rutas nuevas en `kestrel_launcher.py`, todas con validacion de ROM previa; manuales
  servidos solo por indice. `scripts/gui.sh` congela el modulo nuevo.
- `web/card.js` + modal `#m-game` + bloque CSS solo de tokens (vale para los dos temas).

**Verificacion.** `gamecard_test.py` ALL PASS. Prueba de API contra el servidor real
(copias de SM64 y DK64): cabecera y CRC `635A2BFF8B022326`, `.st3` detectado, anadir y
apagar truco, codigo invalido rechazado, manual subido y servido `text/plain`, `.exe`
rechazado, rutas fuera de la ficha 404, meta guardada. **El exe real carga el `.cht` que
escribio la ficha**: `2 trucos (1 encendidos, 1 lineas sin efecto, 0 ilegibles)`, lo mismo
que muestra la ficha. `web/smoke_card.html` en Edge sin cabeza: TODO BIEN (ficha abierta,
titulo, cuatro pestanas pintan), capturas de info/trucos/manual/partidas revisadas.

Cero cambios en el emulador: no toca gates.

## 2026-09-16 — Hilos del RCP: menos viajes por el kernel (giro del RDP, avisos al RSP)

Perfil de anfitrion de los tres hilos (SM64, Parallel-RDP, threaded-jit, 400 intercambios)
y tres cambios, todos solo de anfitrion: el invitado sale identico en todos los modos.

- **Giro del worker del RDP ocioso** (`KESTREL_RDPSPIN`, defecto 32768). Antes de dormir en
  `rdpCv` gira sobre `dpPending`. Bench SM64 200 intercambios: prdp-jit 3,98 -> 3,45 s
  (**-13 %**), threaded-jit (SoftRDP) 4,93 -> 4,63 s (-6 %).
- **Avisos al RSP solo si alguien duerme**: `rspSubmitKick` mira `rspIdleWaiting` (con
  `rspMx`) y el fin de tarea mira `rspWaiters`. 3,55 -> 3,51 s. Giro del worker del RSP
  (`KESTREL_RSPSPIN`) medido neutro, defecto 0.
- **Giro previo en las esperas del RDP** (`KESTREL_DPSPIN`, defecto 2048): `dpBarrierWait`
  (CPU) y `rdpAwaitGuest` (RSP) miran `rcpPend&4`/`dpSchedEnd`/`dpCompSeq` antes de dormir.
  ~-1,5 % (3/3 rondas).
- Descartado por medida: memorizar la division del giro de `spBarrierWait` (12 % del perfil
  de CPU). Neutro: el giro gasta el mismo tiempo de pared, solo da mas vueltas.

Las tres variables salen en el lanzador (avanzadas). Gates: gate_all 445 s / gate_prdp
339 s, todos los modos PASS y md5 iguales, krom interp regress=0, prdp regress=0 improve=2.
Lo siguiente del perfil: contencion de `rdpMx` (el `rdpSubmit` fecha el tramo con el mutex
cogido y el worker se bloquea al publicar DPC_CURRENT), `jitIdleSkip`, COP0 del RSP en el JIT.

- Giro de quien espera al RDP (`KESTREL_DPSPIN`) sube a 262144 vueltas por defecto: -1 % mas en los dos RDP (meseta medida). `KESTREL_BARSPIN` medido plano, sin cambio.
- Perfil tras esto: en el hilo de CPU `jitIdleSkip` pesa 0,4 % (no es palanca); la CPU gira esperando al RSP (`spBarrierWait` ~35 %), y en el hilo del RSP el codigo emitido es el 43 %. La palanca que queda es el coste del microcodigo compilado.
- Dynarec del RSP: MFC0/MTC0 ya no cortan el bloque (puente `Rsp::jitCop0`, reloj exacto, corte si SET_HALT o DMA a IMEM). Interprete 8,26 M -> 0,70 M instrucciones en SM64; pared neutra. Gates verdes.
- DESCARTADO por medida: envio adelantado a la GPU con Parallel-RDP (`CommandProcessor::flush()` cada N primitivas, sin esperar). SM64 prdp-jit min de 5, 2 rondas: N=0 3,43/3,44 s, 32 4,18/4,17, 128 3,48/3,48, 512 3,48/3,48. Partir el cuadro en mas envios cuesta mas de lo que gana la GPU pintando antes. Perfil tras MTC0-en-JIT: el hilo del RSP gasta ~16 % girando en `dpSpinUntil` (espera al RDP en el sondeo de DPC), la CPU ~35 % en `spBarrierWait` y ~16 % en `dpSpinUntil`: la cadena acaba en el fence de la GPU.
- Parallel-RDP con procesado de comandos DIRECTO en el worker del RDP (`PARALLEL_RDP_SINGLE_THREADED_COMMAND=1` por defecto desde `vrdp::init`): se quita el hilo `CommandRing` y un salto entre hilos por comando. El aviso de ocio del anillo (`Op::MetaIdle`) lo da `vrdp::idle()`: el worker al irse a dormir tras el giro, o el cierre de campo en lockstep / `KESTREL_RDPINLINE`. Sin el, `HelloWorldRDP16BPP` (lista sin SYNC_FULL) quedaba negra (100 -> 1). SM64 prdp-jit min de 5, 4 rondas: 3,41-3,45 s -> 3,38-3,39 (-1,6 %). gate_prdp regress=0, md5 iguales. `PARALLEL_RDP_SINGLE_THREADED_COMMAND=0` vuelve al anillo.
- Medido y NO adoptado: saltarse el fence de SYNC_FULL (no esperar a la GPU) da techo 3,43 -> 3,14 s (-9 %), pero deja la CPU leer RDRAM (framebuffer, texturas escritas por el RDP) antes de que la GPU acabe = infiel al HW. Un fence perezoso por proteccion de paginas lo haria fiel, pero es cirugia grande; queda anotado como palanca.
- Contadores de diagnostico del sondeo de DPC por hilo lector y sin prefijo LOCK (`bumpOwned` en `types.hpp`). ~-0,3 % prdp-jit, cuentas exactas. `cyclesRun` se deja con `fetch_add` a proposito (Dekker de `rspWaiters`).
- El RSP lee DPC_CURRENT/STATUS sin esperar al worker del RDP (salen del horario de invitado de `dpScheduleSpan`; `KESTREL_RSPDPAWAIT=1` la devuelve). La espera por pixeles pasa a los DMA del RSP (`rspDmaRdpWait`), y solo si el DMA solapa la zona color/z que el pase de coste marca al encolar (`KESTREL_DMASPAN=0` = siempre). SM64 prdp-jit 3,40 -> 3,34 s (-2 %), md5 igual. Fence de SYNC_FULL diferido: probado, neutro, descartado.
- Zona que puede estar pintando el RDP en 4 intervalos separados (antes uno de color y otro de z): SM64 borra el z-buffer (0x400) como color image y el intervalo unico cubria casi toda la RDRAM, asi que cada DMA del RSP esperaba al motor. Esperas 401 -> 0, SM64 prdp-jit 3,34 -> 3,26 s. Publicacion con seqlock. Siguiente palanca anotada: fence perezoso para la CPU (proteger las paginas de la zona hasta que el motor drene, en vez de `dpBarrierWait`), techo medido -9 %.

## 2026-09-16 — Fidelidad de tiempos: Threaded y JIT = Lockstep, evento a evento (DK64 1.500 M)

Objetivo: que Lockstep (`KESTREL_THREADS=0`) sea el oraculo determinista y que Threaded y
el JIT den EXACTAMENTE lo mismo, no solo el mismo md5 de cuadro. Metodo:
`KESTREL_INTLOG=2` imprime cada entrega de interrupcion (`[deliver] cnt cmp fr ret pc cause mi`)
y se comparan las lineas `cnt/cmp/fr/ret` entre modos. DK64 USA, 1.500 M instrucciones,
10.845 entregas: **identicas en lockstep/threaded x interprete/JIT, y repetible**.

Convenio de visibilidad (lo que lo sostiene todo): una tienda de la CPU en la op n
(`cartNow()` = n) surte efecto en el flanco n+1; una escritura del RSP sellada con el
instante de SU ciclo la ve la CPU en cuanto lee en `now >= sello`.

Cambios, por orden:

1. **Reloj del RCP absoluto** (`spMarkKick`, `spKickEdge`, `spCycleAt`): el ciclo j de una
   tarea cae en el flanco `spKickEdge + j` del reloj libre de 62,5 MHz, no j ciclos despues
   del CLEAR_HALT redondeado.
2. **Reloj de MMIO dentro de bloques JIT** (`emitMemOp` pasa las ops previas del bloque;
   `jitInterpOp` suma `off>>2` a `jitPending`): un evento de MMIO se fecha en su
   instruccion, no al principio del bloque.
3. `siFinish` y `rcpRetire` tras cada bloque JIT (como tras cada instruccion interpretada).
4. `jitGuardPtr`: quien arma un plazo nuevo desde el hilo de CPU pone a 0 el permiso de la
   cadena del JIT, que se calculo sin el.
5. MFC0 Count en el JIT incluye `jitPending + idx` con `countFrac`/`stallCycles`.
6. `rspInterleave` en Lockstep tras pasos del interprete y tras bloques JIT.
7. **Lockstep usa el horario de invitado del RDP** (`dpScheduleSpan` + plazo MI_DP), igual
   que Threaded; `lockRspExec` hace que durante el RSP intercalado el reloj sea el del RSP.
8. **Visibilidad del FIFO del RDP** (`dpJobKickG`, `dpVisibleAt`): un tramo lanzado despues
   del instante del lector no existe para DPC_CURRENT/STATUS. La CPU sella `kick+1`, el RSP
   `kick`. Sin esto Threaded acababa una tarea de DK64 6 ciclos antes.
9. `idleSkip` cuenta vueltas en ciclos absolutos (`rcpOpsToCycles(tgt-1)`).
10. **SP_STATUS en tiempo de invitado para el RSP** (`Memory::spReadSync`,
    `spStatusForRsp`, anillo `spSigRing`): F3DEX sondea SIG0 (osSpTaskYield) en bucle y la
    CPU lo escribe en tiempo de pared. La tarea lanzada en 870.553.865 duraba 55.876 ciclos en
    Lockstep y 66.060 / 201.692 en Threaded (segun RSPJIT). Ahora el RSP espera a que la CPU
    llegue a su instante (sin adelanto kRdvLead: con la CPU por delante el fin de tarea naceria
    tarde) y deshace las escrituras de senales de la CPU con sello posterior. 55.876 en todos.

Descartado: fence perezoso (ver arriba), y la cita `KESTREL_DPRDV` sigue apagada (el
aparcamiento cubre DPC; SP_STATUS tiene su propia cita sin adelanto).

Pendiente anotado: carrera esporadica en Threaded vista antes de estos cambios (un MI_SP de
mas/menos en 605 M, dos corridas distintas a 877 M) -- no reaparece en 3 corridas a 1.500 M
tras el cambio 10; `KESTREL_RSPIDLE=0` en Threaded diverge (tareas de 7,5 M ciclos), sin
investigar; el savestate no guarda el anillo del horario del RDP ni `dpDoneAt`.
**Cerrado 2026-09-17:** `KESTREL_RSPIDLE=0` ya no diverge (statehash iguales en T0/T1: jr
`be723abf`, pd `92f83ab8`, sm `79895dc7`, dk `e7098ab4`), y el savestate (v11) guarda los fines
de tarea armados `spDoneAt`/`dpDoneAt`; el anillo del horario no hace falta porque la foto se
toma en reposo natural (ver la seccion del rebobinado al final y `docs/REWIND.md`).

**Gates.** gate_all 477 s / gate_prdp 313 s. systemtest PASS en todos los modos, sm64 md5
iguales. krom: `RDPTest/CPU` y `RDPTest/RSP` bajan 99,65 -> 99,07 en interp y 100 -> 99,07 en
prdp, y se acepta con baseline nueva. Motivo: Lockstep antes pintaba el buffer del RDP en el
instante del lanzamiento; ahora sigue el horario de invitado igual que Threaded, y la CPU lee
DPC_CURRENT/STATUS con el relleno de 320x240 (83.177 GCLK) aun en curso. Con
`KESTREL_CACHECOST=60` (fallos de cache cobrados, la CPU de arranque va a su velocidad real)
vuelve a 99,65 con los valores de la consola (CURRENT=END, STATUS=0x80). El fallo esta en el
reloj de CPU plano por defecto (CPI medio 1,4), no en el horario del RDP; la recalibracion
del CPI base sigue pendiente.

**Hueco nuevo anotado (Threaded, preexistente, sin baseline krom threaded):** los pixeles que
el RDP escribe en RDRAM no tienen instante de invitado para la CPU. En RDPTestCPU la CPU pinta
la "R" del titulo mientras el worker aun no ha hecho el relleno negro; Threaded la borra,
Lockstep no (98,98 vs 99,07). El arreglo fiel es el fence perezoso por paginas (la CPU no
toca la zona de pintado pendiente hasta que el motor drene).

## 2026-09-17 -- DK64 Threaded determinista con grano SIG (Q=14)

Oraculo: DK64, 1.500 M instrucciones, traza `[deliver]` (KESTREL_INTLOG=2). Antes 1 de cada
4-6 corridas Threaded salia distinta (md5 3683d83f/30953df9, luego un baile de MI_DP de una op).
Ahora **6/6 Threaded identicas y == Lockstep** (e0006a16), 14 s de pared.

Cuatro fugas de tiempo de anfitrion, cerradas en orden:

1. **Carrera Dekker del aparcamiento del RSP** (`dpScheduleSpan` vs `rspParkWait`): los dos
   miraban antes de publicar. Ahora el tramo se publica (`dpSubSeq`) y DESPUES se mira
   `rspPark`, con el aviso bajo `parkMx`. 3 de cada 10 corridas perdian una tarea de SP.
2. **Grano de visibilidad de SIG0..7** (`KESTREL_SPSIGQ`, log2, defecto 14, 0 = exacto): la
   escritura de CPU sella `roundup(cartNow()+1, Q)` y el RSP solo espera a la CPU hasta
   `floor(now, Q)`. Se apunta en los dos modos. `spSigAtKick` (el lanzamiento ve todo lo
   escrito antes) y `spLateClearHalt` (CLEAR_HALT con SIG aplazado relanza tras el BREAK,
   la carrera de rspq de libdragon). Con Q=0 Threaded ya era == Lockstep; con Q=14 quedaban
   las tres de abajo.
3. **Lecturas DPC del RSP con tramo abierto por delante de la CPU**: con `KESTREL_DPRDV`
   apagado el RSP leia DPC_CURRENT/STATUS sin esperar a la CPU (contador `stale` 21-23 y el
   resultado cambiaba con el). Ahora pasa por `spReadSync` (cita exacta, sin adelanto).
   `dpMaxQuery` se actualiza tambien en `dpcStatusFor`, que antes dejaba ciego el contador.
4. **Salvavidas de 20 ms de las citas del RSP con la CPU en la barrera del RDP**: la CPU
   parada en `dpBarrierWait` esperando a la GPU hacia saltar la renuncia del RSP. Nuevo
   `cpuDpBarWait`: mientras esta puesto, el reloj de pared del salvavidas no corre (el RDP no
   depende del RSP, no hay bloqueo mutuo; la barrera tiene su propio kBarrierMaxWait).
5. **Escrituras DPC del RSP** (`Rsp::mtc0`, rd&8): con el motor congelado el RSP escribia
   DPC_END en un instante de invitado futuro y la CPU, al descongelar antes en invitado pero
   despues en pared, encolaba el tramo con ese END. Ahora la misma cita que las lecturas.
   Con la tarea en marcha la CPU nunca va por delante del RSP (barrera del SP).

Nota: el dynarec del RSP no actualiza `exactLeft` antes de llamar a helpers, asi que el reloj
de MFC0 dentro de un bloque no es exacto; no ha hecho falta para este oraculo (RSPJIT=0 tambien
divergia por las causas de arriba).
**Cerrado 2026-09-17:** MFC0/MTC0 ya salen del bloque por `Rsp::jitCop0`, que pone `exactLeft =
jitBudget + rem` (el saldo exacto de esa instruccion); `jitExec` solo lo usan helpers que no
leen el reloj. Comprobado: statehash con `KESTREL_RSPJIT=0` identico al del dynarec (jr
`be723abf`, pd `92f83ab8`, sm `79895dc7`, dk `e7098ab4`).

**krom prdp preexistente:** `gate_prdp` marca regress=6 improve=45 (GRB12/15/24Decode 100->0,
I8Decode, PPU2BPPTile8x8, Cycle1ShadeTriangle16BPP). El exe de d75ca9b da exactamente las
mismas cifras, asi que no es de este cambio; baseline prdp desfasada o dependiente del
anfitrion GPU.

**Resuelto:** la `krom-prdp.tsv` que escribio d75ca9b llevaba cifras de SoftRDP (las filas
de GRB12Decode 100,00, I8Decode 78,73, PPU2BPPTile8x8 66,93 y Cycle1ShadeTriangle16BPP 96,08
coinciden al centesimo con `krom-interp.tsv`): se genero con el exe equivocado. La salida
actual de parallel-rdp (mean 89,27 / 92,56, la misma de todas las entradas anteriores de esta
bitacora) es la buena; baseline regenerada con `build-prdp`, `gate_prdp` 319 s regress=0.
Queda abierto como hueco de accuracy real: GRB12/15/24Decode salen NEGROS en parallel-rdp.

## Salto del bucle de espera del RSP tambien con el RDP OCUPADO: junkrunner64 2x (2026-09-17)

Tras hacer Threaded == Lockstep (d75ca9b, 3dcc2c2) `junkrunner64` (libdragon) se habia vuelto
muy lento: 200 intercambios de buffer en 37 s, con el hilo del RSP ocupado un 95 % pero
retirando solo ~9 Mips. El `[det]` lo decia: **8.062.518 lecturas de DPC del RSP** en 300 M
instrucciones de CPU, `idle=0/0(... drn8043252 ...)`. Es la hipotesis del usuario: `rdpq`
sondea DP_STATUS en bucle mientras el RDP acaba el tramo, y cada vuelta pedia cita a la CPU
(`spReadSync`). El salto de `Rsp::idleSkip` solo existia con el motor DRENADO, que es el caso
de F3DEX (DK64 espera al siguiente buffer); libdragon espera con el motor OCUPADO.

Con el motor ocupado DPC_STATUS tampoco cambia hasta el siguiente instante YA FECHADO del
horario: el cierre del primer tramo abierto o el lanzamiento del primero aun no visible
(`Memory::dpNextChangeAt`). Un tramo nuevo de la CPU nace con kick >= cartNow y lo caza el
aparcamiento igual que antes. Asi que `idleSkip` aparca tambien en ese caso, con el tope
`rspParkCap = min(cambio, now + kParkLead)`: la barrera efectiva del SP deja a la CPU llegar
justo hasta ahi y no mas, y el RSP aterriza en ese instante de invitado. `missed()` devuelve
el menor entre arranque y lanzamiento del tramo colado (con el motor drenado son iguales;
ocupado, END_VALID cambia ya en el lanzamiento).

Dos detalles de coste: con tope corto la CPU llega enseguida y se queda en la barrera sin
avisar, y el RSP dormia el plazo entero del condvar (~32 ms de pared por aparcamiento, peor
que antes). Ahora `rspParkWait` gira 65536 vueltas antes de dormir y `spBarrierWait` notifica
`parkCv` antes de dormir si el RSP esta aparcado.

Medido, parallel-RDP, threaded-jit:

| | antes | ahora |
|---|---|---|
| junkrunner64 200 flips | 37,2 / 36,4 s | **17,9 / 18,0 s** |
| lecturas DPC del RSP (300 M insns) | 8.062.519 | 27.645 |
| md5 framebuffer 200 flips | `75e331cb` | `75e331cb` (== Lockstep, 3/3) |
| DK64 1.500 M insns | `eca336ea` 13 s | `eca336ea` 13 s (4/4, == Lockstep) |

gate_all 485 s y gate_prdp 327 s, rc=0, regress=0 en los dos. Sigue por debajo de lo de
2026-09-10 (17 s por 600 flips, entonces sin ninguna cita): el RSP va a ~22 Mips ocupado y
queda por perfilar.

Correccion a lo de arriba: la comparacion con 2026-09-10 no vale. Los commits de entonces
(b91848b, bd89130, 6f92c93, 52c197a) compilados hoy NO llegan a 200 intercambios en 60-120 s:
el RSP se va apagando (jobs/s 10 -> 2) y el juego se queda parado. No habia regresion que
bisecar; el perfil de hoy es el punto de partida.

## Guarda `rsp.brake` del prologo del JIT fuera en Threaded con plazos: junkrunner64 -33 % (2026-09-17)

Perfil de anfitrion con simbolos (`build-prof-prdp`) sobre junkrunner64: el hilo del RSP pasa
~80 % girando en `spReadSync` esperando a la CPU, y el hilo de CPU va al 99 % pero a solo
26 Mips. `KESTREL_JIT_STATS` lo explica: `[tramp] rsp=55M` -- el prologo de cada bloque
enlazado caia al trampolin por la guarda "hay tarea de RSP en vuelo", una vez cada ~8
instrucciones. libdragon deja `rspq` corriendo siempre, asi que la guarda saltaba siempre.

La guarda era un freno de PARED (sin ella la CPU corria por delante del RCP y giraba). Desde
la barrera del SP ese adelanto no existe en tiempo de invitado: `spBarrierEff` entra en el
permiso por `rcpDueIn`. Asi que en Threaded con plazos y barrera ya no se emite. Lo unico que
cubria de verdad era el LANZAMIENTO de tarea en mitad de una cadena (plazo nuevo que el
permiso no conocia): ahora `rspKick` anula `jitGuard`, igual que `siDma` y `dpScheduleSpan`.
En Lockstep sigue. `KESTREL_JIT_RSPGUARD=1` la vuelve a poner para bisecar.

Citas del RSP por sitio (antes del cambio, 200 flips): DPC_END escrito 351.250 (el 15 % de
pared si se quitan, medido como cota -- NO es correcto quitarlas), SP_STATUS por grano 16.121,
lectura DPC 43.925, BREAK 297.

Medido, parallel-RDP, threaded-jit, md5 y retiradas identicos con y sin guarda:

| | con guarda | sin guarda |
|---|---|---|
| junkrunner64 200 flips | 17,9 s | **11,9 s** (`75e331cb`, 470 M) |
| DK64 1.500 M insns | 13,0 s | **11,7 s** (`eca336ea`) |
| SM64 300 flips | 7,0 s | 6,7 s (`29a0e995`, 725 campos) |
| Perfect Dark 600 flips | - | 16/16 arranques limpios, `31784f9d` las 16 |

Los motivos historicos para no quitarla (PD 1/16 colgado, SM64 1804 campos por 300 flips) no
se reproducen: eran de antes de la barrera. gate_all 479 s, gate_prdp 318 s, regress=0.

Pendiente visto al medir: SM64 300 flips da 797 campos VI en Lockstep y 725 en Threaded, con
el MISMO md5 de framebuffer; igual con y sin guarda, asi que no es de este cambio.

## Traduccion directa ckseg0/ckseg1 tambien con KX=1: junkrunner64 -30 % (2026-09-17)

Perfil de CPU de junkrunner64 tras quitar la guarda `rsp.brake`: `translate()` 9,3 % + `xlatDirect`
1,4 %. libdragon deja Status = `...e3` (KX|SX|UX), y tanto `CPU::xlatDirect` como el camino en
linea de `emitMemOp` en el JIT exigian KX=0, asi que TODOS los accesos a memoria caian en
`translate()` completo. Semantica HW: en kernel con KX=1 las direcciones extendidas en signo
`0xFFFFFFFF_80000000..BFFFFFFF` son ckseg0/ckseg1, mapeo directo identico a kseg0/kseg1 (el mismo
`& 0x1FFFFFFF` que ya hacia `translate()`). El chequeo de kernel (EXL/ERL o KSU==0) se mantiene.

| Prueba (prdp threaded-jit) | Antes | Despues | md5 |
|---|---|---|---|
| junkrunner64 200 flips | 13,6 / 12,0 s | 8,2 / 8,4 s | `75e331cb` ambos |
| DK64 1.500 M | 11,7 s | 11,8 s | `eca336ea` |
| SM64 300 flips | 6,7 s | 6,7 s | `29a0e995` |
| PD 600 flips | - | 9,0 s x4 | `31784f9d` x4 |

Juegos de libultra corren con KX=0: neutros, como se espera.

## Diario de escrituras DPC del RSP: fuera la cita en cada DPC_END (2026-09-17)

La pregunta del usuario (rdpq/rspq de libdragon como colas que nunca se vacian y se comen la
CPU) apuntaba bien: el coste no era de las colas sino de la CITA entre RSP y CPU en cada
escritura del microcodigo a DPC. rdpq escribe DPC_END por cada tanda de comandos: 351.250
citas en 200 flips de junkrunner64, cada una parando el hilo del RSP hasta que la CPU llegase
a su instante de invitado. Cota medida quitandolas (incorrecto): -31 % de pared.

Arreglo correcto: el RSP apunta `{instante, registro, valor}` en un anillo SPSC
(`Memory::dpLogPush`, bit 16 de `rcpPend`) y sigue. La CPU lo aplica en su hilo exactamente al
llegar a ese instante: `rcpRetire` tras cada bloque/instruccion, con el plazo metido en
`rcpDueIn` para que ningun bloque del JIT se lo salte, y tambien antes de que la propia CPU
lea o escriba un registro DPC. Mientras se aplica (`tlDpLogApply`) los sellos de
`rdpSubmit`/`dpScheduleSpan` salen con el instante del RSP y la escritura cuenta como del RSP.
Por que el orden de invitado es el mismo que con la cita: el RSP publica su reloj exacto antes
de apuntar y la barrera del SP no deja a la CPU pasar de ese reloj, asi que el instante
apuntado nunca queda detras de la CPU; empate = la del RSP primero, igual que antes.

Lo que el RSP podria observar de una escritura aun no aplicada espera a que el diario se vacie
(`dpLogWait`): lecturas DPC (CURRENT/STATUS y el resto) y lanzamientos de DMA (SP_RD/WR_LEN),
porque la instantanea de comandos se toma al aplicar y las zonas de pintado se calculan ahi.
Cota de afinar los DMA por solape: ~2 % en junkrunner64, no hecho. XBUS puesto o una escritura
de STATUS que lo toca van por el camino viejo (espera + escritura directa).
`KESTREL_DPLOG=0` vuelve a la cita.

Salvavidas de las citas (`rdvWaiveDue`, compartido con `spReadSync` y `dpReadSync`): antes
soltaba a los 20 ms de pared sin mirar que hacia la CPU. Con el diario salian 4 renuncias en
cada arranque de junkrunner64: el hilo de CPU estaba en `audio::init` (abrir waveOut, ~80 ms,
visto suspendiendo el hilo y simbolizando la pila), no bloqueado. Soltar ahi aplica escrituras
por delante de la CPU. Ahora los 20 ms solo cuentan desde que la CPU esta DENTRO de una espera
sobre un worker del RCP (`cpuRcpWait`, marcado con `RcpWaitMark` en rspPace, rdpPace,
rdpDrain, rspAwaitIdle, spBarrierWait); fuera de eso el tope es 2 s. Resultado: 0 renuncias.
`System::quiesceRcp` vacia el diario (`dpLogFlush`).

Medido parallel-RDP threaded-jit, A/B intercalado `KESTREL_DPLOG=0/1`, md5 identicos:

| Prueba | cita | diario | md5 |
|---|---|---|---|
| junkrunner64 200 flips | 8,1-8,5 s | **6,8 s** | `75e331cb` |
| Perfect Dark 600 flips | 10,8-11,3 s | **9,5-10,3 s** | `31784f9d` |
| SM64 300 flips | 7,1-7,2 s | **6,7 s** | `29a0e995` |
| DK64 1.500 M | 11,9-12,0 s | **11,4-11,5 s** | `eca336ea` |

`[sprdv]` en junkrunner64: 410.633 citas -> 15.600. gate_all 490 s, gate_prdp 320 s, regress=0.

Visto de paso, ya existia con la cita: `[statehash]` al parar por flips en junkrunner64 varia
entre corridas (EPC dentro del bucle ocioso a 3 instrucciones), con framebuffer identico.
Resuelto en la seccion siguiente: no era el punto de parada.

## Escrituras del RSP a SP_STATUS en el diario (2026-09-17)

La variacion de `[statehash]` en junkrunner64 era una divergencia real del invitado. Con
`KESTREL_INTLOG=2` los avisos caian en la misma instruccion retirada pero con otro PC: la CPU
recorria caminos distintos entre avisos. Traza de lecturas MMIO de la CPU (temporal) en 4
corridas: 1,7 M lecturas, el 99 % `SP_STATUS` en bucle de sondeo, y la primera diferencia en la
lectura 163.377: `0x5800 -> 0x5400` (SIG4 -> SIG3) una vuelta antes o despues segun la corrida.

Causa: el microcodigo (rspq) escribe las senales de SP_STATUS por MTC0 y eso iba directo al
registro en tiempo de pared. El RSP va por DELANTE de la CPU en tiempo de invitado (la barrera
solo impide lo contrario), asi que la CPU veia cambios de su futuro en cuanto el anfitrion los
hacia. El camino inverso (CPU -> RSP) ya estaba fechado (`spSigRing`/`spStatusForRsp`); este no.

Arreglo: la escritura del RSP a SP_STATUS va al mismo diario que DPC (`reg = 8|4`), con su
instante exacto, y la CPU la aplica al llegar ahi (tambien antes de leer o escribir cualquier
registro SP). Aplicada desde el diario cuenta como del RSP (`fromRsp` incluye `tlDpLogApply`), y
un SET_INTR levanta MI_SP en ese instante y no antes. Excepciones que siguen en el acto:
SET_HALT/CLEAR_HALT (paran o lanzan el nucleo). El RSP no lee SP_STATUS con escrituras suyas
aun en el diario (`spLogPend` -> `dpLogWait`), y su BREAK espera si alguna toca INTR_ON_BREAK
(`spLogCrit`). `KESTREL_SPLOG=0` vuelve a la escritura directa.

A/B intercalado prdp threaded-jit, audio off, 2 rondas:

| Prueba | directo | diario | statehash | md5 |
|---|---|---|---|---|
| junkrunner64 200 flips | 7,08-7,15 s | 6,92-7,04 s | varia -> `51c2098c` 8/8 | `75e331cb` |
| Perfect Dark 600 flips | 10,6-10,7 s | 9,9-10,6 s | `92f83ab8` | `0f0adee7` |
| SM64 300 flips | 6,83-6,84 s | 6,70 s | `79895dc7` | `29a0e995` |
| DK64 1.500 M | 11,24-11,25 s | 11,29-11,33 s | `e7098ab4` | `eca336ea` |


## Threaded == Lockstep en statehash: cita DMA, MI_DP del diario y aparcamiento (2026-09-17)

Objetivo: el `[statehash]` de Threaded igual al de Lockstep (el oraculo), no solo el md5 del
fotograma. Antes junkrunner64 daba `51c2098c` en Threaded contra `be723abf` en Lockstep.
Cuatro arreglos, todos de semantica de tiempo de invitado:

1. **Cita en los DMA del SP** (`Rsp::mtc0`, SP_RD_LEN/SP_WR_LEN). El DMA lee o escribe RDRAM en
   el instante del RSP, que en Threaded va por delante de la CPU: rspq de libdragon bajaba un
   buffer que la CPU aun no habia escrito. Ahora el RSP publica su reloj y espera en
   `spReadSync` a que la CPU llegue. MI_DP de junkrunner64 iba 66 ops tarde; con la cita, igual.
   `KESTREL_DMARDV` (mascara: bit0 RD, bit1 WR; de fabrica 3, `=0` la quita para bisecar).
   `rspSyncWait` hace que el regulador (`rspPace`) suelte a la CPU mientras dura la cita: si no,
   los dos hilos quedaban mirandose hasta el timeout del condvar.
2. **MI_DP armado por el diario no vence en el mismo retiro** (`tlRetireArmed`). En Lockstep el
   RSP en linea corre DESPUES de `rcpRetire`, asi que un SYNC_FULL escrito en `t` vence en el
   retiro de `t+1`. El diario aplicado dentro del retiro de `t` lo vencia ahi: MI_DP una op
   antes (junkrunner64). Solo cuenta si la entrada es del instante actual y el plazo no estaba
   ya vencido.
3. **Aparcamiento del RSP con tramo archivado pero aun no visible** (`Rsp::idleSkip`). Un tramo
   que la CPU lanza en la misma op que el RSP sondea se ve en el flanco siguiente
   (`dpJobKickG = kick+1`), y con coste 0 cierra antes de verse: `dpSchedEnd <= now` decia
   "drenado", el RSP aparcaba esperando un lanzamiento que YA estaba archivado y solo salia por
   el tope de la CPU (`kParkLead`, 16 M ops) o el salvavidas. Perfect Dark threaded perdia un
   MI_DP entero (152.932.562) al activar el punto 2. Ahora `dpNextChangeAt` acota tambien con
   el motor drenado: ese lanzamiento es un cambio ya fechado.
4. **JIT: no compilar con un plazo encima** (`jitTryBlock`). Con Count/Compare, SI o RCP a menos
   de un bloque maximo (65 ops) el bloque nuevo no se iba a poder ejecutar y cada op
   interpretada compilaba otro. Solo coste de anfitrion: DK64 threaded con la cita DMA
   23,6 -> 12,2 s.

prdp threaded-jit, audio off (`ab.sh`), Threaded == Lockstep en las cuatro:

| Prueba | statehash | md5 | Threaded |
|---|---|---|---|
| junkrunner64 200 flips | `be723abf` | `75e331cb` | ~10,5 s |
| Perfect Dark 600 flips | `92f83ab8` | `0f0adee7` | ~13,2 s |
| SM64 300 flips | `79895dc7` | `29a0e995` | ~8,2 s |
| DK64 1.500 M | `e7098ab4` | `eca336ea` | ~12,2 s |

Coste: la cita DMA sube la pared frente a la seccion anterior (junkrunner64 7,0 -> 10,5 s,
PD 10 -> 13 s): la mayor parte es la CPU esperando al RDP en la GPU (barrera DP) mientras el RSP
espera a la CPU. `KESTREL_DMARDV=0` recupera la velocidad a cambio del statehash de Lockstep.
Sin el punto 2 (`tlRetireArmed`) junkrunner64 vuelve a `014e5f41`.

## Dos pendientes cerrados sin cambio de codigo (2026-09-17)

- **SM64 300 flips, 797 campos VI en Lockstep y 725 en Threaded**: falso. Las corridas se
  hicieron seguidas y la primera dejaba `Super Mario 64 (USA).eep` escrito; la segunda arrancaba
  con partida guardada (otro camino de menus: 811 M ops, statehash `5527b2d8`). Borrando el
  `.eep` antes de cada corrida los dos modos dan 797 campos, 891 M ops y `79895dc7`. Al medir a
  mano hay que borrar el `.eep` como hace `ab.sh`.
- **DK64 1.500 M en Lockstep ~83 s "frente a ~60 s"**: no es regresion. Medido compilando
  `0d782d9`, `53de02e`, `27f4ccc` y `ab038e5`: 83-85 s los cuatro, statehash `e7098ab4`. En
  Lockstep el JIT cede casi todo por la guarda de tarea de RSP en vuelo (cobertura 3-5 %,
  `jitdecl rsp=` domina; la guarda nueva de plazo solo declina ~400 veces).

## Caches de la VR4300: mapeo directo, confirmado en el manual NEC (2026-09-17)

`docs/GAPS.md` tenia como sesgo pendiente "las caches de aqui son de mapeo directo y las de la
VR4300 de 2 vias". El manual de usuario de NEC (U10504EJ7V0UM) lo desmiente: I-cache de 16 KB y
D-cache de 8 KB, ambas **direct-mapped, virtually-indexed, physically-tagged**, lineas de 32 y
16 bytes. El modelo del emulador ya era ese. Cerrado sin cambio de codigo. Del hueco de CPI de
Perfect Dark queda solo lo apuntado: medir dentro de un nivel, no en el titulo.

## CPI de Perfect Dark DENTRO de un nivel: la muestra de titulo era la que salia barata (2026-09-17)

`docs/GAPS.md` dejaba el hueco de PD asi: con base 1,0 y los costes de cache PD salia a 1,158-1,199
contra una cota de 1,45, y la sospecha final era que la muestra (pantalla de titulo) no valia.
Confirmado. Warp autonomo a Defeccion sin mando ni menu con un fichero de trucos
(`tools/cheats/pd-ntsc-decomp-warp-defection.cht`: rellena `g_MissionConfig` y pide el cambio de
nivel mientras `g_StageNum` sea el titulo; direcciones del `pd.map` de la build NTSC del decomp,
ROM copiada fuera del arbol del decomp para que el `.eep` no caiga alli).

`KESTREL_CPI=1.0 KESTREL_CACHECOST=60 KESTREL_UNCACHEDCOST=60 KESTREL_FPUCOST=stat
KESTREL_MULDIVCOST=stat`, prdp threaded-jit, cortes a 900 y 2700 intercambios (el primero ya
dentro del nivel, intro de Defeccion; el segundo ya jugando, HUD del Falcon 2). Tramo = diferencia:

| | titulo (medida vieja) | tramo en nivel 900 -> 2700 |
|---|---|---|
| retiradas | -- | 1.768 M |
| fallos D$ | 0,250 % | **2,29 %** |
| fallos I$ | 0,013 % | **0,38 %** |
| ciclos parados / retirada | +0,158 | **+1,73** |
| ops FP / retiradas | -- | 5,7 % |
| CPI con base 1,0 | 1,158 | **2,73** |

Dentro del nivel PD falla nueve veces mas de D-cache que en el titulo. Con eso el modelo fisico
(canalizacion 1,0 + fallos a 60 ciclos) queda muy por ENCIMA de la cota de 1,45, que es cota de
agresividad (el CPI de verdad puede ser mayor), asi que ya no hay contradiccion: DK64 1,25 frente a
1,19, SM64 ~1,4-1,5, PD en nivel 2,7.

**El defecto NO se mueve.** Con el defecto actual (CPI plano 1,4, sin costes) el mismo tramo hace
2.712 M retiradas y 1,35 campos por intercambio; con el modelo fisico 1,71. Son juegos visiblemente
distintos en velocidad (la intro acaba antes en campos con el modelo fisico porque va por tiempo), y
elegir entre los dos exige una referencia de consola real (campos por intercambio de una escena
fija medida en HW) que no tenemos. Sin ella, cambiarlo seria elegir a ojo. Lo que falta, en orden:
(1) una captura de HW de una escena determinista (intro de Defeccion, contador de campos), (2)
confirmar la latencia de 60 ciclos por fallo contra la documentacion de la RCP, (3) entonces si,
mover el defecto y meter la perilla en un modo de gate (punto 4 de GAPS).

## Estados y rebobinado solo en reposo natural del RCP; ida y vuelta en cada foto (2026-09-17)

`KESTREL_REWIND=1` en Threaded se colgaba: la foto de cada campo forzaba `quiesceRcp` a mitad de
tarea del RSP, el RSP esperaba a la CPU en una cita y la CPU al RSP (150 s, avisos `[rcp] llevo N
x 2000 ms esperando a que termine la tarea del RSP`). En Lockstep no se colgaba pero terminaba la
tarea a destiempo, asi que la foto no era un instante del invitado.

- `System::rcpAtRest()`: RSP sin tarea, `rcpPend` sin diario DPC ni barrera (bits 2-4), RDP
  drenado y su ultimo tramo visible. La foto de rebobinado queda *debida* al cerrar el campo y se
  toma en el primer subtramo en reposo; `state.save`/`state.load` por MCP esperan igual (tambien
  con la emulacion en pausa: el bucle sigue corriendo subtramos mientras haya peticion). Tope
  16x600 subtramos, despues parada forzada con aviso `[state]`/`[rewind]`.
- Savestate **v11**: `rcpPend & 3` + `spDoneAt`/`dpDoneAt` (fines de tarea armados sin vencer).
  `rcpSchedReset` conserva esos dos bits. En reposo natural salen 0 (0/1300 fotos SM64, 0/1000
  DK), pero la via forzada si los ve (pend=2/3 medido con el codigo viejo).
- `afterLoad` enlaza `rsp.mem` antes de `bindMem`: un estado cargado antes del primer kick del RSP
  reventaba (segfault en `afterLoad+136`, visto con `lldb --batch`).
- `KESTREL_REWIND_RTT=1`: cada foto se recarga en el acto. Modo de gate `rewind-rtt` (threaded+jit,
  foto cada campo) en `gate_all.sh`, systemtest + sm64 md5.

Statehash con/sin rebobinado y con/sin RTT, Threaded y Lockstep: SM64 300 intercambios
`79895dc77397e307`, PD 600 `92f83ab859532dd0`, junkrunner64 `be723abf` md5 `75e331cb`, DK
`e7098ab4` md5 `eca336ea`, todos iguales. Pared T1: sin 9/13 s, rew 11/15 s, RTT 19/18 s.

## Modo de puerta con el modelo fisico de ciclos; el latch del bus del PI decae en tiempo (2026-09-17)

`gate_all.sh` gana `phys` (Lockstep+JIT) y `phys-threaded`: `KESTREL_CPI=1.0 CACHECOST=60
UNCACHEDCOST=60 FPUCOST=stat MULDIVCOST=stat`, systemtest + sm64 (GAPS punto 4). Primer fruto:
`cart-writing: Temp value decay` (variante SH) fallaba 1/3721 porque el valor escrito al bus del
PI vivia 200 ops fijas y las paradas de cache de la propia prueba se comian el plazo. Es descarga
de las lineas, tiempo: `Memory::CART_LATCH_TTL_CYCLES = 330` pasado a ops con el CPI vigente
(`cartLatchTtl`, 235 ops de fabrica). systemtest 0/3721 en todos los modos y con CPI=2; sm64 md5
igual en los dos modos nuevos; statehash de jr/pd/sm/dk sin cambio en T1 y T0.


## Calibrar el CPI: enclavamientos, costes por fuente, reloj de cartucho con paradas y I-cache exacta en el JIT (2026-09-17)

Peticion: calibrar el CPI con lo que digan ares, libdragon y n64brew. **No hay verdad de
consola** en ninguna de las fuentes; lo que dan es la semantica de la tuberia y latencias
documentadas. El defecto (CPI plano 1,4 sin costes) NO se toca: sin captura de HW (campos por
intercambio de una escena fija) moverlo seria elegir a ojo. Lo que se hace es que el modelo
fisico sea completo, con numeros de fuente, y que lockstep/threaded/interprete den lo mismo.

Fuentes y numeros:

| pieza | valor | fuente |
|---|---|---|
| canalizacion | 1 ciclo/op | SGI R4300 spec rev 2.2; libdragon `TICKS_PER_SECOND = CPU_FREQUENCY/2` (Count a medio ciclo) |
| fallo I-cache | 48 ciclos | cen64 `vr4300/fault.h`; ares cobra 48 igual |
| fallo D-cache | 44 ciclos | cen64 (ares 40+40 con volcado) |
| lectura sin cache | 38 ciclos | cen64 |
| MULT/DIV enteros | 5/8/37/69, parada de tuberia | NEC tabla 3-12, SGI spec |
| FPU | tabla 7-14 en bloque (cota superior) | NEC |
| enclavamientos | LDI (load seguido de uso), DCB (fallo de dato en rama), sin puente FP | n64brew VR4300, SGI spec |

- **Perillas separadas**: `KESTREL_ICACHECOST` / `KESTREL_DCACHECOST` (antes una sola
  `CACHECOST`), `KESTREL_INTERLOCK=stat|on`. Savestate **v12** (estado de enclavamiento).
- **Enclavamientos** en interprete y dynarec (`ilkPre` en la frontera de op, deshecho en bail
  con `bailUndo`). Frecuencia medida: junkrunner64 LDI 16,8 % / DCB 4,4 % (0,212 de CPI),
  Perfect Dark 1,0 % / 1,0 % (0,020), SM64 1,4 % / 1,96 % (0,034).
- **Raiz de un descuadre lockstep/interprete con costes encendidos**: `guestOps`/`cartNow`
  (reloj de SI, PI, VI y RCP) no incluia los `stallCycles` aun no volcados a `stallOps`. El
  reloj de cartucho iba por detras segun cuando cayera el volcado. Ahora suma lo pendiente
  (`cartStallRem`/`cartStallCyc`/`cartCpi256` en `Memory`).
- **Guardas del JIT en unidades de guestOps**: los plazos del SI y del RCP estan en guestOps
  (op = 1, ciclo parado = 128/cpi256); Compare en ticks de Count. `CPU::guestOpsMax(K)` acota
  cuanto reloj de cartucho puede mover un bloque de K ops con el peor coste por op, y
  `opsForGuest(g)` es su inversa. Sin costes son la identidad, y el defecto no cambia ni un bit.
- **I-cache exacta en el dynarec** (`CPU::jitIcExact`, solo con `ICACHECOST > 0`): el interprete
  rellena y cobra una linea en el fetch de la primera op que la toca; el JIT rellenaba todas las
  lineas del bloque en el driver y los saltos enlazados/ITC no rellenaban nada (576k fallos en el
  interprete contra 259k en el JIT en jr). Ahora cada bloque emite, en la primera op y en cada
  frontera de linea, el `valid && ptag == base` de `icFetch`; el fallo va a un stub que llama a
  `jitIcRefill` (rellena, cobra, y comprueba que las palabras siguen siendo las compiladas; si no,
  bloque muerto y bail). El compilador lee con `jitPeekWord`, que no rellena.

Resultado, jr 40 intercambios lockstep, JIT=0 contra JIT=1, cada perilla sola: statehash igual en
las seis (DCACHE 594cb750, INTERLOCK 8589a116, UNCACHED 1fc6545b, FPU block 19c7a4fa, ICACHE
8957b819, PHYS completo a445ad5c6e8ceee9). Con PHYS el `[cpi]` real de jr sale **1,943**.

Mas arreglos que salieron al cruzar lockstep/threaded con cada perilla:

- **Count leido por MFC0** incluye tambien las paradas aun no volcadas (igual que `cartNow`).
- **`rcpRetire(opStart)`**: los plazos del RCP que vencen DENTRO de una op con parada larga se
  publican con el instante de arranque de la op (`retireOpStart`), no al final de la parada; y
  `dpEndArmAt` compara contra ese mismo instante.
- **`uncachedRead` cobra DESPUES de leer**: el registro se muestrea al principio del acceso por el
  bus; cobrar antes lo leia 38 ciclos en el futuro.
- **Salto del bucle de espera del RSP con DPC_CURRENT interpolado** (`dpNextChangeAt(now, cur)`):
  `idleSkip` aparcaba el RSP hasta el cierre del tramo, pero DPC_CURRENT avanza por interpolacion
  lineal en pasos de 8 bytes dentro del tramo. Si el bucle sondea CURRENT el despertar es el
  siguiente paso, no el cierre. PD con CPI 1,0 + enclavamientos divergia T0/T1 por aqui.
- **Recarga START->CURRENT con el RDP congelado** (`Memory::dpScheduleReload`). DK64 congela el
  RDP despues de MI_DP (DPC_STATUS set freeze), el microcodigo instala START/END con el RDP
  congelado y sondea CURRENT hasta verlo recargado. El horario de invitado (`dpcCurrentFor`) solo
  mira el anillo de tramos, y congelado no se lanzaba ninguno: devolvia el cierre del ultimo tramo
  para siempre. El RSP no terminaba la tarea y el juego se quedaba en el hilo ocioso
  (`pc=0x8000094c`) en T0 Y T1 con CPI 1,0 (con CPI 1,4 la CPU descongelaba antes de que el RSP
  llegara al sondeo). Ahora la recarga se fecha como un tramo vacio de coste cero. Cambia el
  statehash final de PD por defecto (`92f83ab8` -> `6caa8f2b`, mismo framebuffer `0f0adee7`, T0 == T1).

Matriz de perillas (PD 600 intercambios / DK64 1.500 M, JIT, T0 contra T1): con los dos ultimos
arreglos todos los campos (`KESTREL_FIELDHASH`) son identicos; el unico statehash distinto es el
del punto de parada de MAXFLIPS, con el framebuffer igual.

PHYS completo en T0 JIT / T1 JIT / T0 interprete:

| ROM | T1 JIT | T0 JIT | T0 interprete | framebuffer |
|---|---|---|---|---|
| junkrunner64 (40 int.) | d0fcb456 41,1 s | d0fcb456 44,4 s | d0fcb456 48,3 s | 75e331cb |
| Perfect Dark (600 int.) | eb0e7a76 52,2 s | 28c17850 44,1 s | 28c17850 107,1 s | 07e7ff48 |
| SM64 | 6ea2db66 63,9 s | 6ea2db66 41,0 s | 6ea2db66 73,4 s | 29a0e995 |
| DK64 (1.500 M ops) | 30516c91 48,7 s | 30516c91 45,6 s | ead6e1cb 128,0 s | d05cd603 |

Las dos celdas distintas son punto de parada, no estado: PD T1 para en MAXFLIPS en otra
instruccion del spin (campos identicos), y DK64 interprete para exactamente en 1.500 M mientras
el JIT termina el bloque. Comprobado con `KESTREL_FIELDHASH` en DK64 PHYS interprete contra JIT:
1393 de 1393 campos iguales. Para eso el hash de campo mezcla `dcbR & 3`: solo bit0 (store en
curso) y bit1 (store anterior sin primer acceso) tienen semantica; los bits altos son historia
desplazada que nunca vuelve a bit1, y el JIT la limpia (`=1`) donde el interprete la conserva
(`|=1`), lo que daba 27 campos "distintos" sin diferencia de comportamiento.

## 2026-09-17 — DMA SP->RDRAM del RSP en el diario (`KESTREL_DMALOG`)

Pedido: "ataca al JIT del RSP". Medido antes de tocar nada (hostprof sobre el hilo del RSP,
junkrunner64 200 intercambios prdp threaded-jit): el codigo que emite el dynarec del RSP es ~9 %
del hilo. El resto es espera: el RSP va por delante de la CPU y se para en citas
(`spReadSync`) cada vez que tiene que tocar algo que la CPU ve. Censo por sitio: 564k citas antes
de un DMA del SP (277k SP_RD_LEN, 287k SP_WR_LEN) y 266k esperas al diario vacio antes de ese
mismo DMA. libdragon (rspq/rdpq) lanza un DMA por tramo de comandos del RDP.

- **SP_WR_LEN (DMEM/IMEM -> RDRAM) va al diario** (`Memory::spDmaLogPush`, reg 16). Los bytes se
  copian YA de la memoria del SP a un anillo de 1 MB (`dmaPay`), los registros SP_MEM_ADDR /
  SP_DRAM_ADDR / *_LEN cambian en el instante del RSP, y la CPU deja los bytes en RDRAM al llegar
  al instante (`spDmaLogApply` desde `dpLogApply`), en orden con las escrituras DPC/SP_STATUS del
  mismo diario. Cae al camino de siempre (cita + copia) con `watchAddr`, `KESTREL_DMAGUARD`,
  traza del SP, transferencias de mas de 256 KB o si el rango pisa una imagen de color/z que el
  RDP tiene en vuelo.
- SP_RD_LEN (RDRAM -> SP) sigue con cita: el microcodigo necesita los bytes que la CPU haya
  escrito HASTA su instante, y eso solo se sabe con la CPU alli.
- junkrunner64 200 intercambios, A/B x3 con `KESTREL_DMALOG=0/1`: 11,45 s -> 10,93 s (-4,5 %),
  statehash `be723abf`, framebuffer `75e331cb`. Techo sin ninguna cita (`KESTREL_DMARDV=0`,
  incorrecto): 7,0 s. El diario no sale gratis: cada entrada es un plazo que corta los bloques
  del JIT de la CPU (`rcpDueIn`). PD 600 / SM64 300 / DK64 1.500 M: T0 == T1 (`6caa8f2b`,
  `79895dc7`, `e7098ab4`).

## 2026-09-17 — Tanda del RSP 8192 -> 512: el RSP esperaba a una CPU parada en su barrera (`KESTREL_RSPTANDA`)

Histograma de las citas `spReadSync` en junkrunner64 (instrumentacion temporal, ya retirada):
**6,6 s de 10,7 s** de pared eran el hilo del RSP esperando a la CPU, con huecos tipicos de 256 a
16K ops. La CPU no iba lenta: estaba parada en la barrera del SP, que solo se mueve cuando el RSP
publica su reloj, y `Rsp::step` lo publicaba una vez por tanda de 8192 instrucciones. El RSP
llegaba a un `DMAIn` de rspq (trae el buffer de comandos desde RDRAM, cita obligada por HW),
pedia a la CPU llegar a su instante, y la CPU llevaba parada hasta 8K instrucciones atras.

Barrido de la tanda en junkrunner64 200 flips (pared): 8192 10,9 s · 2048 10,0 · 1024 10,07 ·
**512 9,6** · 384 9,58 · 256 9,7 · 128 10,15. Meseta en 384-512; por debajo gana el coste de
publicar. PD/SM64/DK64 neutros. De fabrica 512 (`kRspTanda` en rsp.cpp). Solo cambia el grano de
publicacion y de sondeo de `hostStop`; el reloj de invitado es el mismo y los statehash no se
mueven (jr `be723abf`, PD `6caa8f2b`, SM64 `79895dc7`, DK64 `e7098ab4`, T0 == T1).

Lo que queda en el hilo CPU de jr (hostprof, tanda 512): JIT ~23 %, giro en la barrera del **RDP**
(`dpSpinUntil`) ~14 %, giro en la barrera del SP ~8 %, dormido en ntdll ~9 %. Cadena: RSP espera
a CPU, CPU espera al RDP.

## 2026-09-17 — La barrera del RDP, con Parallel-RDP, solo en SYNC_FULL (`KESTREL_DPBARSYNC`)

La barrera del RDP paraba a la CPU en el instante de invitado en que cierra **todo** lo mandado
al motor. Con SoftRDP eso es lo correcto: el rasterizador lee texturas y escribe pixeles en RDRAM
mientras pinta, asi que la CPU no puede dejar atras un tramo sin pintar. Con Parallel-RDP no
protege nada: el worker solo ENCOLA los comandos (que ademas ya van copiados, `rdpSnapshot`), y la
GPU lee texturas y escribe el color image de RDRAM cuando le toca, que es mas tarde. La unica
garantia de RDRAM es el fence de `SYNC_FULL` (`vrdp::runFifo`), y ese instante es justo el que la
CPU no puede pasar: es donde cae MI_DP y donde el juego queda autorizado a reescribir sus buffers.

Con libdragon eso se nota porque rdpq manda el FIFO a trocitos: junkrunner64 hace **325.551
tramos** en 200 cuadros (~1.600 por cuadro) y solo **203** traen SYNC_FULL. La CPU se paraba en
cada tramo a esperar el relevo del worker: 44.347 paradas, ~19 us cada una, **11,3 % de pared**.

Ahora `dpBarrierAt()` devuelve, con el worker de Parallel-RDP vivo, el fin del tramo con SYNC_FULL
**mas antiguo pendiente** (`dpSyncEnds`, cola con `rdpMx`; `~0` = ninguno). Detalle que importa:
un tramo VACIO (`DPC_END == DPC_CURRENT`, 82.940 de los 325.551 en junkrunner) no pasa por el pase
de coste, y darlo por "puede traer SYNC_FULL" dejaba la mitad de la ganancia sin coger.

**Medido** (200 flips jr / 600 PD / 300 SM64 / 1.500 M DK64, Parallel-RDP, threaded+JIT):

| | antes | ahora |
|---|---|---|
| junkrunner64 | 9,76 s | **9,0-9,4 s** |
| SM64 | 8,76 s | **8,48 s** |
| Perfect Dark | 14,79 s | 14,64 s |
| DK64 | 13,82 s | 13,73 s |

Paradas de la barrera en jr: 44.347 (0,85 s) -> **174 (0,40 s)**, y esas 174 son el fence de verdad
(~2,4 ms cada una). `cpuWait` 13,0 % -> 8,4 %. Statehash sin mover en los cuatro (jr `be723abf`,
PD `6caa8f2b`, SM64 `79895dc7`, DK64 `e7098ab4`), dos corridas iguales cada uno. `KESTREL_DPBARSYNC=0`
vuelve al comportamiento de SoftRDP. Con SoftRDP en su hilo no cambia nada: la barrera sigue entera.

## 2026-09-17 — Tres callejones sin salida en el reparto CPU->RDP (medidos y descartados)

Perfil del hilo de CPU (junkrunner64, 200 cuadros, Parallel-RDP, 1.832 muestras): **14,36 %** cae
fuera de imagen (ntdll) y **8,73 %** de ese total entra desde `dpcAdvance`. Desensamblando las dos
direcciones de retorno calientes: estan justo detras de `std::mutex::unlock` y de
`condition_variable::notify_all` dentro de `rdpSubmit` en linea. O sea, la CPU paga dos llamadas al
kernel por DPC_END para despertar a un worker dormido, ~308.000 veces en 200 cuadros (rdpq de
libdragon manda ~1.600 tramos diminutos por cuadro). Tres intentos de quitarlo, los tres fallidos:

1. **Sacar varios trabajos de golpe en el worker.** Instrumentada la profundidad de la cola justo
   despues de cada `pop`: `0:304290 1:3578 2:369 3:13`. La cola esta VACIA el 98,7 % de las veces;
   no hay nada que agrupar. Instrumentacion retirada.

2. **Girar mas antes de dormir** (`KESTREL_RDPSPIN`). A/B de cuatro juegos: 32768 -> jr 9061 /
   PD 14631 / SM64 8449 / DK64 13860 ms; 131072 -> jr 9045 / PD 14419 / SM64 8408 /
   DK64 **14661** ms (+5,8 %). El anfitrion (i7-870, 4c/8t) va con CPU ~100 %, RSP ~86 % y RDP
   63-86 %: esta sin nucleos, y el worker girando se los roba a los otros dos. Se queda 32768.

3. **Aplazar el reparto**: juntar hasta 32 tramos contiguos antes de pasarselos al worker. Es
   invisible para el invitado (el horario lo fecha `dpScheduleSpan` y las lecturas de DPC salen de
   el, no de la cola), asi que la idea era legitima; pero **pierde por goleada**: PD 15,0 -> 18,8 s,
   SM64 8,4 -> 16,3 s, y junkrunner64 **se cuelga**. Dos razones: (a) con la cola vacia el 98,7 %
   del tiempo, retener trabajo no esconde nada, solo quita solape CPU<->RDP y amontona el pintado
   en la barrera de SYNC_FULL; (b) `rspDmaRdpWait` espera a `rcpPend & 4`, que el tramo aplazado no
   arma nunca, asi que el RSP gira para siempre. Revertido.

**Conclusion**: el coste de despertar al worker no se quita ni agrupando, ni girando, ni
aplazando. Mientras el RDP viva en otro hilo y el juego mande tramos diminutos, esas dos llamadas
al kernel por tramo son el precio del solape, y el solape vale mas que ellas.

## 2026-09-17 — El RSP le robaba la linea de cache al hilo de CPU mirando su reloj

Perfil del hilo del RSP (junkrunner64, 200 cuadros, Parallel-RDP, 1.733 muestras): **57 %** de su
pared esta dentro de `Memory::spReadSync` (36 % en imagen + 21 % de salidas a ntdll desde el
`yield` de su bucle). Son las 290.421 citas del sondeo de SP_STATUS: el microcodigo lee
SP_STATUS, el RSP va por delante de la CPU en tiempo de invitado y tiene que esperar a que la
CPU retire hasta ese instante para no perderse una escritura suya de SIG.

Esa espera es semantica y no se puede quitar. Lo que SI se podia quitar es lo que la espera le
cuesta **al otro hilo**: el bucle miraba `cartNow()` en cada vuelta, y `cartNow()` lee
`cartClock`, el contador de instrucciones retiradas que el hilo de CPU esta escribiendo sin
parar. Cada lectura del RSP le pasa esa linea a compartida y obliga a la CPU a volver a pedirla
en exclusiva en su siguiente op. Con el RSP dentro de la cita el 57 % del tiempo, eso es
martillear al hilo que ES el cuello de botella.

Ahora los bucles de espera del hilo del RSP miran el reloj de la CPU **una de cada 64 vueltas**
(`KESTREL_RDVPOLL`, potencia de dos; `KESTREL_RDVTIGHT=<n>` vuelve a mirarlo a pelo las primeras
n vueltas, por si otro anfitrion prefiere latencia). Afecta a `spReadSync`, a `dpLogWait` y al
giro previo a dormir de `rspParkWait`. La pausa `PAUSE` sigue a una de cada 16.

**Medido** (Parallel-RDP, threaded+JIT, 200 flips jr / 600 PD / 300 SM64 / 1.500 M DK64):

| | antes | ahora | |
|---|---|---|---|
| junkrunner64 | 9.239 ms | **8.059 ms** | -12,8 % |
| Perfect Dark | 14.809 ms | **13.439 ms** | -9,2 % |
| SM64 | 8.365 ms | **8.283 ms** | -1,0 % |
| DK64 | 13.688 ms | **13.186 ms** | -3,7 % |

Framebuffer md5 identico en los cuatro (jr `75e331cb`, PD `0f0adee7`, SM64 `29a0e995`,
DK64 `eca336ea`). Muestras del hilo del RSP en jr: 1.733 -> **1.454** (-16 % de tiempo de hilo),
que es justo el nucleo que se le devuelve a la CPU y al worker del RDP.

Barrido del espaciado (jr/PD/SM64/DK64, ms): cada vuelta 9.239/14.809/8.365/13.688 ·
16 -> 8.620/14.335/8.260/13.527 · **64 -> 8.342/14.100/8.254/13.134** · 256 ->
8.226/14.108/8.470/13.179. Desde 256 SM64 empeora: se queda 64.

**Lo que NO vale**: el mismo espaciado en el lado de la CPU (`spBarrierWait`, `dpSpinUntil`)
es una REGRESION clara -- jr 7.892 -> 9.936 ms, SM64 8.299 -> 8.727. Alli la latencia manda,
porque quien espera es el palo largo y cada microsegundo que tarda en ver la barrera levantada
lo paga la corrida entera. Revertido; el espaciado es solo del hilo del RSP.

## 2026-09-17 -- Cuatro intentos mas de recortar coste de anfitrion (los cuatro, negativos)

Tras el espaciado del sondeo del reloj (seccion anterior) el perfil del hilo del RSP seguia
diciendo 31,8 % fuera de la imagen. Cuatro hipotesis, las cuatro medidas y las cuatro
descartadas. Bench: minimo de 2-3 corridas intercaladas A/B por juego, ruido de pared ~2 %.

**1. Periodo del `yield` del hilo del RSP.** `spReadSync` y `dpLogWait` ceden el nucleo una de
cada 256 vueltas; con el sondeo espaciado cada vuelta cuesta una fraccion de lo que costaba, o
sea que el mismo periodo cede muchas mas veces por segundo. Barrido (ms, minimo de la tanda):

| periodo | jr | PD | SM64 | DK64 |
|---------|----|----|------|------|
| 16 | 7.968 | 13.609 | **8.746** | **13.642** |
| 256 (actual) | 7.995 | 13.349 | 8.224 | 13.233 |
| 1024 | 7.855 | 13.569 | 8.185 | 13.304 |
| 4096 | 7.914 | 13.663 | 8.283 | 13.075 |
| 65536 | 8.047 | 13.624 | 8.158 | 13.160 |

Meseta plana de 256 en adelante; solo ceder MUCHO (16) hace dano (SM64 +6 %, DK64 +3 %). O sea
que el `yield` no es el coste: el 27 % que el perfil apunta a `rspCv.notify_all()` es tiempo de
un hilo que YA esta esperando, no camino critico. Knob revertido (no se anade un knob que no
mueve nada).

**2. Memo de la barrera del SP en el hilo de CPU.** El giro de `spBarrierWait` convierte el
reloj publicado del RSP a ops en cada vuelta (multiplicacion de 128 bits + ancla del
lanzamiento) sobre un valor que el worker solo publica cada pocos miles de instrucciones de
microcodigo. Memoizado de dos formas:

- con `thread_local` en `spBarrierAt()`: **peor en los cuatro** (+1,4 % jr, +1,0 % PD, +1,4 %
  SM64, +0,2 % DK64). En Windows la indireccion de TLS cuesta mas que la propia multiplicacion.
- con variables locales del propio bucle (sin TLS, sin estado compartido): empate en PD y
  ligeramente peor en los otros tres.

`PaceDiv` ya es multiplicacion-desplazamiento: el `div` que el perfil marca al 5,3 % es codigo
inlineado de toda la ruta de retiro atribuido a esa linea, no una division lenta. Revertido.

**3. Reorden del despacho del dynarec.** `jitTryBlock` (7,9 % del hilo de CPU) mira la cache
negativa -- con su `icFetch` de validacion -- ANTES de buscar el bloque. Mirarla solo cuando no
hay bloque (con bloque compilado no puede haber fallo de compilacion pendiente) quita ese
`icFetch` del camino comun. Medido: empate en jr/SM64/DK64 y **PD 1,5 % peor de forma
consistente** -- PD acierta mucho en la cache negativa, y esos aciertos ahora pagan primero una
busqueda en la tabla. El intercambio no sale a cuenta. Revertido (el `cartNow()` duplicado de la
comprobacion de plazo tambien iba en el mismo parche y tampoco se aprecia).

**4. Giro adaptativo del worker del RDP.** Ver la seccion siguiente: el giro constante no se
puede mejorar leyendo la profundidad del acierto.

## 2026-09-17 -- El worker del RDP quema 65 % de un nucleo estando 10 % ocupado

Dato del latido (`KESTREL_HEARTBEAT=1`), junkrunner64 200 intercambios:

```
[hb] occupancy: rdp 10%(cpu 65%) rsp 77%(cpu 81%) cpuWait 11% | jobs/s: rsp=33 rdp=37697
```

El worker del RDP pinta el 10 % del tiempo de pared y sin embargo gasta el 65 % de un nucleo:
la diferencia es el giro de `rdpSpinLen()` (32768 vueltas) entre trabajo y trabajo. Con 37.700
tramos por segundo en jr y 44.250 en PD, el giro casi nunca llega a agotarse.

Ese giro NO esta ahi por latencia -- 131072 vueltas son ~200 us, mucho mas que cualquier
despertar del planificador --: esta ahi porque mientras el worker gira, `rdpWaiting` es falso y
el PRODUCTOR (el hilo de CPU o el del RSP) se ahorra el `notify_all`, que a 44.000 tramos/s es
una llamada al kernel constante. Es un trueque directo: nucleo del worker a cambio de llamadas
al kernel del productor. Y el lado bueno del trueque depende del juego:

| giro | jr | PD | SM64 | DK64 |
|------|----|----|------|------|
| 2048 | 8.365 | 13.841 | 8.915 | 13.584 |
| 8192 | 8.122 | 13.635 | 8.545 | 13.521 |
| 32768 (actual) | 7.886 | 13.442 | 8.217 | **13.037** |
| 131072 | 7.864 | **12.989** | 8.304 | 14.107 |

PD gana un 3,4 % subiendo a 131072; DK64 pierde un 8 % con ese mismo valor, repetido en dos
rondas. Bajar de 32768 es peor en todos. 32768 se queda como el compromiso.

**Lo que no funciona para resolver la tension**:

- *Giro adaptativo por profundidad del acierto* (media movil de la vuelta en la que llega el
  tramo, girar el cuadruple de esa media, acotado): con tope 131072 DK64 sigue en 14.044 ms. Los
  aciertos de DK64 tambien son profundos, asi que la regla no distingue "acierto que valia la
  pena esperar" de "aqui habria que haber dormido". Adaptativo == constante.
- *Mas `pause` en el giro* (una por vuelta o una de cada 4, en vez de una de cada 16), por si el
  dano fuera de hermano logico SMT: DK64 sigue en 14.050-14.146 ms con tope 131072. No es SMT,
  es nucleo fisico entero.

La salida real no es tunear el giro, es que haya menos tramos: 44.000 `DPC_END` por segundo es
lo que hace cara cualquiera de las dos opciones. La union de tramos contiguos en el productor ya
esta (`rdpSubmit`), y la union en el consumidor esta medida y descartada (seccion del 2026-09-17
sobre los tres callejones del reparto CPU->RDP).

## 2026-09-18 -- Quien mueve de verdad el bus por el motor de DMA del SP (`[spdma]`)

Pregunta del usuario: en el telemetro `[rdram]` de junkrunner64 el RSP salia a 55-80 MB/s, y
eso choca con lo que hace `rspq` de libdragon, cuyo bufer de comandos en DMEM es
`RSPQ_DMEM_BUFFER_SIZE = 0x100` -- **256 bytes**, no KB -- y que en cada recarga refetch-ea el
bufer ENTERO (`jal DMAIn` con `li t0, DMA_SIZE(RSPQ_DMEM_BUFFER_SIZE, 1)`, `rsp_queue.inc:454`).
Con eso no salen 80 MB/s ni de lejos.

**La contabilidad estaba bien.** Los dos unicos sitios que suman bytes del SP
(`Memory::spDma` y `Memory::spDmaLogPush`) decodifican SP_RD_LEN/SP_WR_LEN igual y correcto:
`length = ((len & 0xfff) + 8) & ~7` es el campo `len-1` mas uno, redondeado a multiplo de 8, y
`count = ((len >> 12) & 0xff) + 1`. El `skip` NO se suma, que es lo que toca: saltar direcciones
entre filas no ocupa el bus. No hay off-by-one por ningun lado.

**Lo que faltaba era el desglose.** Un agregado de bytes mezcla cosas que no se parecen en nada:
la recarga de una cola de comandos (256 B), un lote de `rdpq`, y un cambio de sobrecapa de
microcodigo (IMEM entero = 4 KB). Ahora `[spdma]` saca, por direccion, cuenta + MB + **tamano
medio por transferencia**, mas un histograma en cuatro cestas y cuantas transferencias tocan
IMEM (= cambio de microcodigo).

Medido con Parallel-RDP, tope por intercambios de bufer:

| ROM | rsp MB/s | lee B/transf | escribe B/transf | DMA a IMEM |
|---|---|---|---|---|
| junkrunner64 (600 campos) | 80,2 | **1465** | 459 | **106296 (347,6 MB)** |
| SM64 (300 flips) | 9,2 | 187 | 80 | 4604 (5,7 MB) |
| DK64 (300 flips) | 5,2 | 232 | 291 | 1973 (2,8 MB) |
| Perfect Dark (600 flips) | 7,0 | 229 | 71 | 3416 (4,1 MB) |

Conclusiones:

1. **En juegos reales el RSP no es el maestro gordo**: 5-9 MB/s, 1-2 % del pico del bus, con
   tamanos medios de 70-290 B que encajan con recargas de cola y listas cortas. Quien manda es
   el RDP (24-37 MB/s: textura + z + color por pixel) y detras el VI (9-30 MB/s: el framebuffer
   entero por campo). Esto es justo lo que dice el hardware.
2. **junkrunner64 es patologico y no vale de referencia de ancho de banda.** De sus 601,6 MB
   leidos, **347,6 MB (58 %) son cargas de IMEM**: 106296 cambios de microcodigo a ~3270 B cada
   uno, o sea IMEM casi entero cada vez. Un juego normal hace 2000-4600 cambios en la misma
   ventana; junkrunner hace cien mil, a proposito. Ese es el origen de los 1465 B/transferencia:
   la media mezclaba recargas de rspq de 256 B con volcados de IMEM completo.
3. junkrunner64 **si** es una ROM de libdragon (`libdragon`, `rspq`, `RSPQ` en la imagen; nombre
   interno `SpellCraft`), o sea que el camino de rspq esta de verdad en juego; lo que pasa es que
   no es lo que domina su trafico.

Aviso de lectura de `[rdram]`: los numeros por maestro son **MB/s, no porcentajes**. El unico
porcentaje es el de cabecera, y es contra `Memory::kRdramPeakBps` = 562,5 MB/s.

### La cache de imagenes de microcodigo, comprobada contra ese mismo caso

Pregunta encadenada del usuario: si hay una cache de cambios de IMEM, esos cambios deberian
salir casi gratis. **Salen.** Son dos cosas distintas y conviene no mezclarlas:

- El DMA de 4 KB a IMEM es trafico de INVITADO. En consola real esos bytes viajan por el bus
  de RDRAM, asi que `[spdma]`/`[rdram]` tienen que contarlos. Quitarlos seria falsear el
  hardware.
- El coste de ANFITRION de un cambio de imagen es retraducir el microcodigo, y eso es lo que
  ahorra `Rsp::jitSelectImage`.

Medido con `KESTREL_RSPJIT_STATS=1`, misma ventana que la tabla de arriba:

| ROM | cambios de IMEM | aciertos / fallos | tasa | bloques compilados | ranuras usadas |
|---|---|---|---|---|---|
| junkrunner64 | 106296 | 104715 / 1870 | **98,2 %** | 2882 | 8 de 16 |
| Perfect Dark | 3416 | 3406 / 10 | **99,7 %** | 1680 | 11 de 16 |
| SM64 | 4746 | 2777 / 1969 | 58,5 % | 4038 | 16 de 16 |

junkrunner64, que es el caso extremo, cambia de microcodigo 106296 veces y compila 2882 bloques
en toda la corrida: ~37 cambios por bloque compilado, y sin agotar ranuras (usa 8 de 16). O sea
que la parte cara del cambio ya es gratis y lo que queda en `[spdma]` es el bus del invitado,
que es irreductible por definicion.

El unico con mala tasa es SM64 (58,5 %), y es tambien el unico que agota las 16 ranuras. Ya
estaba medido que subir a 32 no cambia nada (usa 17 y compila los mismos bloques), y el umbral
`kJitNewWay` esta barrido: 4 y 64 son peores. Se queda.

## 2026-09-18 -- El bug #2 de junkrunner64 NO es el enlace de bloques: es una carrera de ANFITRION

Corte por brazos, ocho corridas de 800 campos por brazo, sonda `KESTREL_FIELDHASH=1` (FNV
del estado de CPU al cierre de cada campo, mas fina que las columnas de `[ft]`). La
divergencia de junkrunner64 es INTERMITENTE, asi que un brazo solo se declara limpio con
las ocho corridas identicas entre si, no con una pareja.

| brazo | corridas identicas | primera divergencia |
|---|---|---|
| base (threaded + JIT de CPU + JIT de RSP) | 3 de 8 | **715** |
| `KESTREL_RSPJIT=0` | -- | 715 |
| `KESTREL_JIT=0` | 8 de 8 | -- |
| `KESTREL_JIT_NOLINK=1` | 8 de 8 | -- |
| `KESTREL_JIT_NOITC=1` | 8 de 8 | -- |
| `KESTREL_JIT_CHAIN=1` | 7 de 8 | 715 |
| `KESTREL_JIT_CHAIN=8` | 6 de 8 | 715 y 716 |
| **`KESTREL_VITICKS=64` (control de velocidad)** | 8 de 8 | -- |
| `KESTREL_THREADS=0` (lockstep, el oraculo) | 8 de 8 | -- |

EL CONTROL MATA LA HIPOTESIS DEL ENLACE. `KESTREL_VITICKS=64` no toca NI UNA semantica del
invitado -- los plazos de `MI_VI` y `MI_AI` caen en la instruccion exacta y por eso subirlo
no mueve el `[statehash]`, cosa que ya estaba comprobada con `KESTREL_IRQTRACE` --, solo
frena el anfitrion. Y hace desaparecer el 715 con la misma fuerza que `NOLINK` (0 de 8
frente a 3 de 8 del brazo base, p ~ 0,02). O sea que lo que quitan `JIT=0` y `NOLINK=1` no
es el enlace de bloques: es VELOCIDAD. Frenar suprime el 715 se frene como se frene.

Lo que queda indicado, entonces, es una carrera sensible al tiempo de ANFITRION, no una
semantica del dynarec. El dynarec del RSP sigue absuelto por su lado (con el apagado la
divergencia sale en el mismo campo).

CORRECCION DE METODO, dos veces. Primera: comparar todas las corridas contra la corrida 1
esta MAL cuando el fallo es intermitente, porque la 1 puede ser ella la rara; hay que
agrupar en clases de equivalencia y buscar donde se separa la minoria. Segunda: comparar
los ficheros LINEA A LINEA tambien esta mal. Las "divergencias del campo 2" que aparecian
en varios brazos eran ARTEFACTO DEL LOG -- una linea `[fh]` comida o pegada a la anterior
por el volcado `[fd]`, que hasta ahora salia en unos setenta `fprintf` sueltos y se
entrelazaba con lo que escriben los hilos del RCP en el mismo stderr. Al reparsear con
expresion regular `\[fh\] <campo> <hash>` y comparar POR CAMPO, esas divergencias
desaparecen: 48 corridas con `VITICKS=64` dan el mismo hash en todos los campos. El
volcado esta arreglado (una sola `fwrite`, `src/core/system.cpp`), y la regla nueva es que
una sonda de divergencia se compara por CLAVE, nunca por posicion de linea.

Escotillas de pared a cero en las tres corridas del barrido largo anterior
(`[pared] renuncias sp=0 dp=0 diario=0 barSP=0 barDP=0`, `[spvenc] 0 vencidos`, sin
excepcion de anfitrion), o sea que no es un salvavidas disparando.

Siguiente paso: dejar de barrer perillas y VOLCAR ESTADO en el campo 715 del brazo base,
muchas corridas, para poder decir que registro o que contador se separa primero.

## 2026-09-18 -- RESUELTO el bug #2: la sombra del FIFO del RDP se reutilizaba SUCIA

Volcando el estado entero en el campo 715 (`KESTREL_FIELDDUMP=715`, 24 corridas del brazo
base) la divergencia deja de ser un hash y se lee de un vistazo: las corridas minoritarias
**CASCAN**. Dos clases distintas, y las dos con la misma pinta.

| | clase mayoritaria (12/22) | minoria A (5) | minoria B (3) |
|---|---|---|---|
| `pc` | `80059128` (codigo normal) | `800007dc` | `800041c4` |
| `cop0_13` Cause | 0 | `0x408` = IP2 + TLBL | `0x28` = instruccion reservada |
| `cop0_08` BadVAddr | 0 | `3c0239e0` | `80003767` |
| `cop0_14` EPC | `80031df0` | `3c0239e0` | `80006be8` |

`3c0239e0` no es una direccion: es la palabra de una instruccion (`lui $2,0x39e0`). O sea
que el invitado esta ejecutando datos, y salta al manejador de excepciones. Eso no es un
desfase de temporizacion, es MEMORIA PISADA -- la misma firma que `docs/PD-DERAIL.md`.

Quien la pisa: el rasterizador leyendo comandos reescritos por debajo. La sombra del FIFO
(`Memory::rdpShadow`) tenia DOS generaciones alternas, y el razonamiento era que con dos
basta porque entre un buffer y el siguiente la copia vive en el otro. Falso en cuanto el
productor se adelanta DOS buffers: al tercer START fresco vuelve a la generacion 0, que
todavia tiene tramos sin pintar, y la copia nueva les reescribe los comandos. Como el
adelanto depende de lo rapido que vaya el ANFITRION, encaja exactamente con lo que decia el
barrido de perillas: frenar como sea (`JIT=0`, `NOLINK=1`, `VITICKS=64`) lo hace
desaparecer, y ninguna de esas perillas era la causa.

Medido con un contador nuevo (`[dpgen]`, reutilizaciones sucias por corrida):

| generaciones | reutilizaciones sucias | corridas identicas (800 campos) |
|---|---|---|
| 2 (historico) | 842 -- 1036 en CADA corrida | **6 de 8** (divergen en 715 y en 775) |
| 8, por turno | 0 -- 7 | **8 de 8** |
| 8, cogiendo la libre mas baja | 0 -- 2 | **8 de 8** |

Arreglo: `kRdpGens = 8` sombras y, al instalar un START fresco, se coge la generacion LIBRE
mas baja distinta de la actual (`rdpGenBusy[]` = trabajos vivos que leen de cada una,
encolados mas el que se esta pintando). La mas baja a proposito: en regimen normal el
worker va al dia y siempre queda libre la 0 o la 1, asi que las de arriba ni se reservan --
cada sombra ocupa una RDRAM entera y se pide al vuelo. Si no hubiera ninguna libre se drena
el RDP, que es lento pero correcto. Ojo al leer `[dpgen]` tras el arreglo: con mas de dos
generaciones el contador ya no son lecturas sucias sino DRENADOS forzosos, seguros por
definicion -- salen 0-2 por corrida de junkrunner64 y las ocho corridas siguen siendo
identicas bit a bit. Con `KESTREL_RDPGENS=2` no hay adonde huir y vuelve a contar
reutilizaciones sucias de verdad; esa perilla recupera el comportamiento viejo para bisecar.
`[dpgen]` sale en el cierre.

Coste en pared: CERO. Min-de-4 intercalado con el MISMO exe, cambiando solo la perilla:
jr 7.738 -> 7.785 (+0,61 %), PD 10.976 -> 10.981 (+0,05 %), SM64 7.532 -> 7.531 (-0,01 %),
DK64 11.702 -> 11.720 (+0,15 %), con md5 del framebuffer identico en los cuatro juegos. Las
sombras de arriba solo se reservan cuando el rasterizador se queda atras de verdad, y la
copia es la misma que ya se hacia.

Lo que NO era, y queda documentado para no volver: el enlace de bloques del dynarec de CPU,
la cache de destinos indirectos, el dynarec del RSP, los salvavidas de pared (`[pared]` y
`[spvenc]` a cero en todas las corridas) y el diario de DPC.

## 2026-09-18 -- Auditoria de licencias antes de publicar binarios

Pregunta del usuario: si hay algun problema de licencias, poner el repositorio privado.
No lo hay, y el repositorio se queda publico. Toda la cadena es permisiva:

| pieza | licencia |
|---|---|
| kestrel64 | MIT, (c) 2026 celgadis84 |
| `third_party/parallel-rdp` | MIT, (c) 2020 Themaister |
| volk (dentro de parallel-rdp) | MIT, (c) Arseny Kapoulkine |
| Vulkan-Headers (dentro) | Apache-2.0, The Khronos Group |
| libc++ / libc++abi / libunwind | Apache-2.0 WITH LLVM-exception |
| GLFW | zlib |

Ni una licencia viral. Ni una ROM ni BIOS en el arbol (`git ls-files` no tiene un solo
`.z64/.n64/.v64/.rom/.bin`), y el README lo dice explicito.

Las citas a ares, cen64, angrylion y Project64 que hay en los comentarios de `src/` se
revisaron una a una: son referencias a como esos proyectos DOCUMENTAN un detalle de
hardware -- un numero de ciclos de parada de cache, la expansion de 5 a 8 bits de un canal
por replicacion de bits, la tabla de longitudes de comando del RDP, el estado de los
registros tras el IPL3. Comportamiento de hardware es un HECHO, no una obra: no hay codigo
copiado y ninguno de esos proyectos entra en el binario.

EL HUECO QUE SI HABIA, y estaba en el paquete, no en el repositorio: el zip no llevaba
ningun aviso de licencia. Los ejecutables que se reparten van enlazados ESTATICOS, o sea
que parallel-rdp, volk, libc++ y GLFW estan literalmente dentro del binario, y MIT y zlib
exigen que el aviso viaje con la copia. Arreglado: `THIRD-PARTY.txt` en la raiz con los
avisos completos, y `scripts/dist.sh` lo copia al paquete junto con `LICENSE.txt`.

## 2026-09-18 -- Bajar la prioridad de los workers del RCP: medido, empate (descartado)

Hipotesis del anfitrion, no del invitado: tres hilos calientes del RCP sobre cuatro nucleos
fisicos (i7-870). Cuando no caben, el planificador de Windows reparte a partes iguales, y eso
no es lo que interesa -- el hilo del RSP gira porque ESPERA al de CPU, y el worker del RDP gira
para ahorrarle al productor un `notify_all` (seccion anterior). Los dos pueden ceder sin perder
nada; el de CPU no. Bajarles la prioridad deberia devolverle nucleo al camino critico.

Parche: `setSelfPriority(const char*)` leyendo `KESTREL_PRIORSP` / `KESTREL_PRIORDP` en el rango
-2..2 sobre `SetThreadPriority(GetCurrentThread(), ...)`, llamado desde cada worker justo tras
publicar su asa. Sin variable puesta no hace nada, o sea que las puertas son bit-identicas.

Los dos workers a `THREAD_PRIORITY_BELOW_NORMAL`, minimo de tres corridas intercaladas por juego:

| juego | base | -1/-1 | delta |
|-------|------|-------|-------|
| junkrunner64 | 7.837 | 7.760 | -1,0 % |
| Perfect Dark | 13.370 | 13.468 | +0,7 % |
| SM64 | 8.169 | 8.184 | +0,2 % |
| DK64 | 13.019 | 13.080 | +0,5 % |

Todo dentro del ruido de pared (~2 %), md5 de framebuffer identico en los cuatro. Tres de los
cuatro salen peor por poco: no hay senal. **Descartado**, parche fuera del arbol.

Por que no funciona, que es lo que se aprende: el problema no es COMO reparte el planificador
cuando los tres hilos estan listos, es que los workers estan listos casi siempre aunque no
tengan trabajo -- el worker del RDP quema el 65 % de un nucleo pintando el 10 % del tiempo. Un
hilo de prioridad baja que gira sigue ocupando el nucleo entero mientras el de prioridad normal
no lo pida en ese instante exacto. La prioridad no convierte un giro en un hueco. La palanca que
si movia la aguja fue espaciar el sondeo del reloj dentro del giro (seccion del 2026-09-17), que
ataca el coste real: la linea de cache que el worker le robaba al hilo de CPU.

La ronda que intentaba separar RDP-solo de RSP-solo salio contaminada (el usuario estaba jugando
en la misma maquina: jr 16.303 ms, dk 23.930 ms contra ~8.000 / ~13.100 normales) y se tira, no
se interpreta.

## 2026-09-18 -- No encolar los tramos VACIOS: medido, neutro (descartado)

Seguia el hilo de la seccion anterior ("la salida no es tunear el giro, es que haya menos
tramos"). Contados en junkrunner64: de los 325.551 tramos de 200 cuadros, **82.940 son vacios**
-- `DPC_END` reescrito con el valor que ya tenia, `current == end` -- o sea uno de cada cuatro
no tiene un solo byte que pintar. La idea era no meterlos en `rdpQueue`: el horario del invitado
(`dpScheduleSpan`) se hace igual, asi que es reparto de trabajo del anfitrion, invisible para el
juego, exactamente como la union de tramos contiguos que ya esta ahi.

El parche (fuera del arbol) marcaba `nothingToPaint` ANTES del horario -- `dpScheduleSpan`
machaca `rdpCostHasResume`/`rdpCostLastEnd` -- y exigia tres cosas, no solo `current == end`:
ningun comando del tramo anterior partido por el borde (`rdpHasResume` del pase de pintado ni
`rdpCostHasResume` del de coste; un tramo de longitud cero SI pinta cuando reanuda uno de esos
trozos, via `if(rdpHasResume && rawCur == rdpLastEnd)` en `rdpRunJob`) y `dpPending == 0`, que
ademas es lo que hace legitimo leer desde el productor una variable del worker: con el motor
parado, el mutex que tenemos cogido es el mismo con el que el worker publico su ultimo fin de
trabajo. Tambien habia que dejar `rdpBusy` sin levantar y `wake` en falso para el tramo saltado,
o el bit de ocupado se quedaba en alto para siempre.

A/B intercalado, `build-prdp`, minimo de tres rondas por juego, md5 de framebuffer identico en
las 24 corridas:

| juego | encolando (ms) | saltando (ms) | delta |
|-------|----------------|---------------|-------|
| junkrunner64 | 7.359 | 7.348 | -0,1 % |
| Perfect Dark | 11.861 | 11.816 | -0,4 % |
| SM64 | 7.708 | 7.761 | +0,7 % |
| DK64 | 11.963 | 11.937 | -0,2 % |

Todo dentro del ruido (~2 %). **Descartado.** La primera ronda daba -5,7 % en jr y era
calentamiento: el lado A corre siempre primero y paga el disco frio, lo que sesga cualquier
lectura de ronda unica. Min de tres rondas intercaladas existe justamente para eso.

Por que no mueve, que es lo que se aprende y mata la linea entera:

1. **La fusion ya se los comia.** La rama de fusion va ANTES y tiene prioridad: si el tramo
   vacio continua donde acababa el ultimo encolado, `rdpQueue.back().end = end` con `end ==
   current == back().end` es una asignacion que no hace nada. Esos vacios ya costaban cero.
2. **Cuando no se fusiona, la vuelta de cola es gratis.** El salto solo entra con la cola
   drenada (`dpPending == 0`), y ahi el worker esta GIRANDO -- quema el 65 % de un nucleo
   pintando el 10 % del tiempo (seccion del giro). Una vuelta mas de su bucle no le cuesta a
   nadie tiempo de pared, porque ese nucleo ya estaba ocupado girando.
3. **El `notify_all` tampoco estaba ahi.** `wake = rdpWaiting && ...`, y `rdpWaiting` es falso
   casi siempre precisamente por el giro de `KESTREL_RDPSPIN`. La llamada al kernel que se
   queria ahorrar ya se la ahorraba el giro desde 2026-09-16.

O sea: de los 44.000 `DPC_END`/s, lo que cuesta NO es el enqueue. Encolar un tramo es un
`push_back` y dos atomicos sobre un mutex que ya esta cogido. Lo que cuesta es llegar hasta
ahi -- la escritura MMIO, el mutex `rdpMx` y el horario -- y de eso el horario no se puede
saltar (es lo que el invitado ve) y el mutex lo pide la propia estructura de dos hilos. Queda
descartada tambien la variante "menos tramos" por el lado de la cola: la cola nunca fue el
coste. Si hay una palanca aqui, esta en el camino de escritura a `DPC_END`, no en `rdpQueue`.

### Corolario medido: el traspaso al worker tampoco es el coste

Misma tanda, el probe que ya existia (`KESTREL_RDPINLINE=1`: en Threaded el RDP corre en el
hilo que escribe `DPC_END`, sin cola, sin mutex de traspaso, sin worker que despertar). Si el
coste de los 44.000 `DPC_END`/s fuera la entrega entre hilos, esto tendria que ganar mucho:

| juego | worker (ms) | en linea (ms) | |
|-------|-------------|---------------|--|
| junkrunner64 | 8.066 | 8.260 | +2,4 %, md5 igual |
| SM64 | 7.968 | 8.072 | +1,3 %, md5 igual |
| Perfect Dark | 12.656 | 2.230 | md5 distinto (el probe no hace el horario: se sale pronto) |
| DK64 | 12.474 | 1.336 | idem |

Pierde en los dos que llegan al final. O sea que el hilo del RDP, con su cola y su mutex, no
esta costando tiempo de pared: quitarlo entero sale peor. (El probe no vale como configuracion
-- en Threaded se salta `dpScheduleSpan`, asi que el invitado ve otra maquina y PD/DK64 no
terminan la tanda. Sirve exactamente para lo que se uso: acotar por arriba lo que se podria
ganar quitando la entrega entre hilos, y ese techo es negativo.)

Con esto quedan medidas y descartadas las **seis** variantes de "menos coste por tramo" en el
reparto CPU->RDP: juntar pops en el consumidor, girar mas, aplazar el reparto, giro adaptativo,
bajar la prioridad de los workers, no encolar los vacios, y ahora tambien no tener worker. El
perfil del hilo de CPU en jr lo explica: 22 % en codigo generado por el JIT, ~15 % en
`rcpRetire` (que es el giro de las barreras del RDP y del SP, ya contabilizado en 2026-09-11),
13 % dormido en ntdll. Lo que la CPU espera no es al anfitrion, es al RELOJ DEL INVITADO -- la
barrera no se abre hasta que el tramo con SYNC_FULL vence en el horario. Acelerar el anfitrion
por debajo de esa barrera no devuelve tiempo de pared. **Esta linea esta agotada**; lo que
queda aqui es horario de invitado, no reparto de hilos.

## 2026-09-18 -- Memoizar el veredicto de `jitIdleSkip`: medido, PIERDE (descartado)

Punto del backlog de `docs/GAPS.md`: el examen del bucle ocioso corre en CADA despacho del JIT
(5,07 % de las muestras del hilo de CPU en el perfil del 2026-09-11) y casi siempre sale que no,
asi que memoizar el veredicto NEGATIVO por PC deberia dejarlo en una comparacion.

Implementado tal cual: tabla de 256 entradas `{phys, seq}` en `CPU`, sellada con el numero de
relleno de la linea de I-cache (`ICacheLine::seq`) -- la misma prueba con la que se revalida un
bloque, o sea que memo y bloque ven el codigo igual de fresco. Solo se apuntan los rechazos por
la FORMA del codigo (no es un salto a si mismo, la ranura de retardo no es NOP); los que dependen
del estado (Status, modo, presupuesto) no se apuntan, que cambian sin que cambie ni un byte.

Correcto: `systemtest[prdp]` 0/3721 · 0/2 · 0/6, `sm64[prdp]` y `sm64[prdp-jit]` md5 `b5521b24`,
y los 48 framebuffers de la tanda A/B identicos entre las dos ramas.

**Threaded (el modo de uso), min de 6 rondas intercaladas, dos exes:**

| juego | base (ms) | memo (ms) | |
|-------|-----------|-----------|--|
| junkrunner64 | 7.313 | 7.341 | +0,4 % |
| Perfect Dark | 11.650 | 11.581 | -0,6 % |
| SM64 | 7.688 | 7.710 | +0,3 % |
| DK64 | 11.939 | 11.854 | -0,7 % |

Todo dentro del +-2 % de ruido de este anfitrion. Ojo con las rondas 1-3, que daban **-4,2 % en
SM64**: en las rondas 4-6 la maquina entera bajo ~10 % y el supuesto margen desaparecio. Es el
mismo sesgo que ya mordio con los tramos vacios; min de tres rondas NO basta cuando el efecto
buscado es del orden del ruido.

**JIT lockstep (`KESTREL_THREADS=0 KESTREL_JIT=1`), donde la CPU SI es el palo largo**, 3 rondas:

| juego | base (ms) | memo (ms) | |
|-------|-----------|-----------|--|
| junkrunner64 | 15.671 | 15.866 | +1,2 % |
| SM64 | 5.509 | 5.570 | +1,1 % |
| DK64 | 4.872 | 4.904 | +0,7 % |

Aqui el ruido es minimo (jr base 15.671/15.739/16.041 contra memo 15.866/15.914/15.940: los dos
grupos ni se tocan) y el memo **pierde de forma consistente**. Por que: el rechazo rapido de
`jitIdleSkip` ya era barato -- `jitPending`, la guarda de tamano, y dos `jitFetchWord` sobre una
linea de I-cache que acaba de leer el despachador. El memo mete DELANTE una sonda de I-cache mas
una tabla de 2 KB: mas cargas que las que ahorra, y una estructura extra que compite por la
cache del anfitrion en el bucle mas caliente que hay.

Leccion general, que es la misma de la linea CPU->RDP: **un % del perfil no es un % de pared**.
En Threaded no se mueve porque el hilo de CPU pasa el 77 % de las muestras en barreras
(`spBarrierWait` 43 % activa + `dpBarrierWait` 34 %) esperando al reloj del INVITADO; quitarle
trabajo a un hilo que espera no devuelve pared. Y en lockstep, donde si la devolveria, resulta
que el trabajo quitado era mas barato que la memoizacion. Revertido; el punto queda cerrado en
`docs/GAPS.md`.

## 2026-09-18 -- El grano con que el RSP publica su reloj: 512 -> 1024

Despues de dos negativos seguidos (los tramos vacios y el memo de `jitIdleSkip`) el perfil
apunta a lo mismo desde dos lados: el hilo de CPU pasa el 77 % de sus muestras en barreras
(`spBarrierWait` 43 % girando + `dpBarrierWait` 34 %) esperando al RSP, y el hilo del RSP pasa
el 26,9 % OCIOSO esperando a la CPU. Los dos esperan y ninguno esta saturado: eso no es falta
de caudal, es **latencia de ida y vuelta** entre los dos relojes de invitado. La palanca de esa
latencia ya existe y es `kRspTanda`, el grano con el que `Rsp::step` publica su reloj a la
barrera del SP. Estaba en 512 desde que se bajo de 8192, y aquel barrido es de ANTES de que
los diarios (`dpLog`, `spLog`, `dmaLog`) y `dpBarSync` quitaran casi todas las citas
`spReadSync`. Con menos citas, publicar fino ya no compra nada y solo cuesta el `fetch_add` y
el `notify` por tanda, asi que el optimo tenia que haberse movido. Re-barrido.

**Min de 4 rondas intercaladas, un solo barrido (las comparaciones entre barridos NO valen: se
midio 1,8 % de deriva de maquina entre dos de ellos), Parallel-RDP, ms de pared:**

| grano | junkrunner64 | Perfect Dark | SM64 | DK64 |
|-------|--------------|--------------|------|------|
| 512 (lo de antes) | 7.326 | 11.674 | 7.628 | 11.854 |
| **1024 (nuevo)** | **7.321** | **11.653** | **7.508** | **11.801** |
| 2048 | 7.391 | 11.549 | 7.428 | 11.774 |
| 4096 | 7.571 | 11.517 | 7.389 | 11.761 |

Se elige **1024 porque DOMINA al 512**: igual o mejor en los cuatro juegos, y -1,6 % en SM64.
2048 y 4096 compran otro 1-3 % en SM64/PD/DK64 pero se lo cobran a junkrunner64 (+0,9 % y
+3,3 %), que es justamente el que mas cita el RSP con la CPU y el que descubrio el problema en
su dia; ganar en tres juegos pagandolo en el cuarto no es una mejora, es mover el bulto.

La forma de las dos curvas dice de que va cada juego: junkrunner64 se degrada de forma
**monotona** por encima de 1024 (reproducible en tres barridos: 1024 7.296, 2048 7.384,
4096 7.503, 8192 7.628) porque ahi la CPU de verdad espera al reloj publicado; SM64 en cambio
sigue mejorando hasta 4096, porque lo suyo es el coste de publicar, no la espera. El barrido
viejo, con las citas puestas, daba el orden INVERTIDO en la parte baja (128 era mejor que
2048); hoy 128 es el peor de todos. O sea que el numero no es una constante del emulador sino
del acoplamiento que tenga en cada momento, y hay que re-barrerlo cuando ese acoplamiento
cambie.

Es un cambio de grano de publicacion y de sondeo, no de semantica: el reloj de invitado es el
mismo. Comprobado -- los 16 framebuffers del barrido con md5 identico por juego, y el
`[statehash]` de junkrunner64 a 400 M de instrucciones da `7f1b537e69aad3f4` en las cuatro
combinaciones de {512, 1024} x {lockstep, threaded}.

## 2026-09-18 -- Re-barrido de las perillas que se afinaron ANTES de los diarios

La tanda del RSP no fue un caso aislado. Varias perillas de espera se calibraron entre el
2026-09-11 y el 2026-09-16, y despues de eso entraron los diarios (`dpLog`, `spLog`, `dmaLog`) y
`dpBarSync`, que quitaron casi todas las citas `spReadSync` entre la CPU y el RSP. Una perilla de
espera no es una constante del emulador: es una constante del ACOPLAMIENTO que haya en ese
momento. Cambiado el acoplamiento, hay que re-barrer. Barridas las tres que quedaban, min de 4-5
rondas intercaladas, un solo barrido por perilla, md5 identico por juego en todas.

### `KESTREL_BARSPIN` 2048 -> 16384 (se coge)

Vueltas que gira la CPU en la barrera del SP antes de dormir.

| vueltas | jr | PD | SM64 | DK64 |
|---------|----|----|------|------|
| 256 | 7.525 | 11.788 | 7.662 | 11.984 |
| 2048 (lo de antes) | 7.245 | 11.568 | 7.520 | 11.827 |
| **16384 (nuevo)** | **7.240** | 11.571 | **7.484** | **11.773** |
| 131072 | 7.386 | **11.564** | 7.493 | 11.756 |

Confirmado con un A/B a dos bandas y 5 rondas: jr 7.286 -> 7.261, PD 11.579 -> 11.551,
DK64 11.810 -> 11.802, **SM64 7.599 -> 7.474 (-1,6 %)**. En SM64 las cinco lecturas de cada lado
casi no se solapan ({7.474, 7.490, 7.577, 7.579, 7.613} contra {7.599, 7.599, 7.646, 7.659,
7.808}); en los otros tres es empate limpio. El 2026-09-16 esto se midio **plano de 2048 a 1 M** y
por eso se dejo en 2048; hoy ya no es plano, y 131072 cobra +1,9 % en junkrunner64, asi que el
optimo esta en 16384 y no mas arriba.

Por que se movio: con los diarios la barrera se abre mucho antes -- el RSP apunta su escritura y
sigue en vez de citarse con la CPU -- asi que la espera que antes obligaba a dormir ahora cabe
dentro del giro, y dormir cuesta un viaje al kernel que el giro se ahorra. Antes girar mas no
servia porque la espera era larga de todos modos; ahora si.

### `KESTREL_RDVPOLL` se queda en 64 (medido, ya estaba en su optimo)

Una de cada n vueltas el bucle de espera del HILO DEL RSP mira el reloj de la CPU.

| n | jr | PD | SM64 | DK64 |
|---|----|----|------|------|
| 16 | 7.779 | 11.988 | **7.506** | 12.032 |
| **64 (actual)** | 7.254 | **11.656** | 7.521 | **11.806** |
| 256 | **7.190** | 11.954 | 8.093 | 11.919 |
| 1024 | 7.955 | 13.504 | 9.959 | 13.483 |

64 gana PD y DK64, empata con 16 en SM64 y solo pierde 0,9 % contra 256 en junkrunner64, que a
cambio cuesta +7,6 % en SM64. 1024 se hunde en los cuatro (+5 a +16 %). Es un optimo estrecho de
verdad y ya estaba puesto: no se toca.

### Regla que sale de aqui

Cuando se cierre una via que quite citas entre hilos, **re-barrer las perillas de espera** antes
de dar por buena ninguna. Dos de las tres se habian quedado desfasadas, y las dos por el mismo
motivo: menos citas ⇒ esperas mas cortas ⇒ conviene girar mas y publicar menos fino.

## 2026-09-18 -- KESTREL_DPSPIN re-barrido: sigue PLANO desde 262144, se queda

Tercera perilla de espera del re-barrido post-diarios. A diferencia de `KESTREL_BARSPIN`, esta
NO se ha movido: 262144 sigue siendo el sitio.

Primer barrido (min de 5 rondas intercaladas, Parallel-RDP, ms de pared):

| juego | 32768 | 262144 | 1048576 |
|---|---|---|---|
| junkrunner64 | 7294 | 7230 | 7230 |
| Perfect Dark | 11678 | 11670 | **11594** |
| SM64 | 7499 | 7563 | **7472** |
| DK64 | 11799 | 11824 | 11823 |

1 M no perdia en ninguno y ganaba 0,7 % en PD y 1,2 % en SM64, con las lecturas de SM64 casi sin
solaparse ({7472,7512,7515,7553,7682} contra {7563,7563,7565,7707,7964}). Con eso solo habria
bastado para subirlo. Pero el efecto era del tamano del ruido, asi que se pidio confirmacion --
segundo barrido, 262144 contra 1 M contra 4 M, otras 5 rondas:

| juego | 262144 | 1048576 | 4194304 |
|---|---|---|---|
| junkrunner64 | 7268 | 7264 | 7242 |
| Perfect Dark | **11551** | 11604 | 11556 |
| SM64 | **7479** | 7526 | 7494 |
| DK64 | 11802 | 11800 | 11743 |

**No reproduce.** En la segunda tanda 262144 gana PD y SM64, justo los dos juegos donde 1 M
parecia ganar en la primera, y 4 M solo roza en jr y DK64. La curva es plana desde 262144 hacia
arriba y lo que se vio antes era la maquina, no la perilla. **DPSPIN se queda en 262144.**

Es exactamente el fallo que casi se cuela con el memo de `jitIdleSkip`: una ventaja del tamano
del ruido en una sola tanda de 5 no es una ventaja. La regla de la casa: **si el margen cabe en
el ruido, hace falta una SEGUNDA tanda intercalada, y si no reproduce, no existe.** Lo de
`KESTREL_BARSPIN` paso el mismo filtro (distribuciones casi disjuntas y mecanismo claro: los
diarios abren la barrera antes, asi que la espera cabe en el giro); esto no.

Guest-neutro en las dos tandas: 15 volcados de framebuffer por juego, un solo md5 cada uno
(jr `75e331cb`, PD `0f0adee7`, SM64 `29a0e995`, DK64 `eca336ea`).

## 2026-09-18 -- Abaratar la vuelta del giro del SP: medido DOS veces, PIERDE (descartado)

La entrada del backlog decia "mirar si el giro hace falta tan largo". El re-barrido de
`KESTREL_BARSPIN` contesto que hace falta MAS largo, asi que lo que quedaba de esa via era
abaratar la VUELTA, no acortarla. El bucle de `Memory::spBarrierWait` lee seis lineas atomicas
por vuelta: `rcpPend`, `rspLogWait` y las cuatro de `spBarrierEff()` (`rspPark`, `rspParkWake`,
`rspRdvAt` y `rsp.cyclesRun`). Todas las escribe el hilo del RSP. La sospecha era que cada
vuelta le pedia en exclusiva lineas que el necesita para avanzar -- y avanzar es justo lo que
estabamos esperando.

**Variante B (detector de cambio).** Vigilar solo `rsp.cyclesRun` y hacer el predicado entero
cuando ese contador se mueva o una de cada 16 vueltas. Min de 5 rondas intercaladas, Threaded,
Parallel-RDP: jr +0,4 %, PD +0,1 %, SM64 +0,4 %, DK64 +0,3 %. **Pierde en los cuatro.** Y el
motivo deja claro que ni siquiera probaba la hipotesis: `cyclesRun` se mueve casi en cada vuelta
del giro -- es el reloj del RSP corriendo --, asi que `cy != seen` era cierto casi siempre y la
variante hacia el chequeo completo IGUAL, mas la carga y la comparacion de mas.

**Variante C (prueba rapida sobre la condicion de verdad).** La vuelta rapida mira solo
`spBarrierAt()`, o sea `rsp.cyclesRun` y nada mas, y nunca decide por su cuenta: solo abre la
puerta al predicado entero, que ademas se comprueba una de cada 16 vueltas pase lo que pase.
Asi los casos raros (aparcado, donde la barrera efectiva es el tope y esta POR DEBAJO de
`spBarrierAt`) se siguen resolviendo igual, y como mucho nos enteramos 15 vueltas tarde de un
cambio que no venga del reloj del RSP. Esta si ahorra las cinco cargas en las vueltas con la
barrera cerrada. Min de 5 rondas intercaladas: jr -0,11 %, PD **+0,84 %**, SM64 **+0,59 %**,
DK64 -0,07 %. **Tambien pierde.**

**Hipotesis refutada, via cerrada.** Las cinco lineas de mas no cuestan: `rspPark`,
`rspParkWake`, `rspRdvAt`, `rcpPend` y `rspLogWait` casi nunca se escriben, asi que viven en
estado compartido dentro de la L1 del hilo de CPU y leerlas es una carga de L1. La unica linea
que el RSP reescribe sin parar es `rsp.cyclesRun`, y esa hay que mirarla si o si porque ES la
barrera. O sea que la vuelta del giro ya era barata y la rama que se le anade cuesta mas que lo
que ahorra. **No volver a intentar adelgazar este bucle**: lo que se paga aqui no es el ancho de
la vuelta, es la latencia de ida y vuelta entre los dos relojes de invitado.

Guest-neutro las dos: `[statehash]` de jr a 400 M instrucciones `7f1b537e69aad3f4` en
{HEAD, B, C} x {lockstep, threaded}, y 20 volcados de framebuffer por juego con un solo md5.

## 2026-09-18 -- El giro del worker del RDP: 32768 -> 131072

Cuarta perilla del re-barrido post-diarios, y la segunda que SE MUEVE. `rdpSpinLen()` en
`src/core/memory.cpp` -- vueltas que gira el hilo del RDP sin trabajo antes de dormir en
`rdpCv`. Solo coste de anfitrion: el horario del tramo ya lo fecha quien lo lanza
(`dpScheduleSpan`), el invitado no ve cuando se pinta.

Tanda 1 (min de 5 rondas intercaladas, Parallel-RDP, ms de pared):

| juego | 8192 | 32768 | 131072 |
|---|---|---|---|
| junkrunner64 | 7664 | 7251 | **7185** |
| Perfect Dark | 11657 | 11569 | **11475** |
| SM64 | 7946 | **7483** | 7516 |
| DK64 | 12301 | 11770 | **11701** |

8192 se hunde en los cuatro, o sea que la meseta del 2026-09-16 sigue empezando en 32768. Lo
nuevo es que ya no es plana por arriba. Tres de cuatro a favor de 131072 y SM64 en contra por
0,4 %: margen de ruido, asi que segunda tanda obligatoria, y de paso 524288 para buscar el borde:

| juego | 32768 | 131072 | 524288 |
|---|---|---|---|
| junkrunner64 | 7266 | **7105** | 7207 |
| Perfect Dark | 11501 | **11475** | 11520 |
| SM64 | 7496 | 7466 | **7446** |
| DK64 | 11761 | 11716 | **11697** |

**Reproduce y ademas SM64 se da la vuelta.** 131072 gana los cuatro, y en junkrunner64 (-2,2 %)
y SM64 las cinco lecturas de cada lado son DISJUNTAS: jr {7105,7153,7191,7240,7249} contra
{7266,7292,7294,7327,7781}, SM64 {7466,7467,7468,7474,7484} contra {7496,7518,7564,7566,7570}.
524288 rasca en SM64 y DK64 pero cobra +1,4 % en junkrunner64, asi que el sitio es 131072.

**Por que se movio.** Es el mismo mecanismo que subio `KESTREL_BARSPIN`: con los diarios
(`dpLog`, `spLog`, `dmaLog`) el RSP archiva el tramo y sigue en vez de citarse con la CPU, asi
que los tramos llegan mas seguidos y mas pequenos -- y el hueco entre dos ya no cabe en 32768
vueltas, con lo que el worker se dormia y habia que pagarle un viaje al kernel para volver a
levantarlo. Tercera vez que el mismo cambio de acoplamiento mueve una perilla de espera.

Guest-neutro: `[statehash]` de junkrunner64 a 400 M instrucciones `7f1b537e69aad3f4` en
{32768, 131072} x {lockstep, threaded}, y 15 volcados de framebuffer por juego con un solo md5
en cada tanda (jr `75e331cb`, PD `0f0adee7`, SM64 `29a0e995`, DK64 `eca336ea`).

## 2026-09-18 -- KESTREL_RSPSPIN re-barrido: sigue NEUTRO en 0, se queda

Quinta y ultima perilla de espera del re-barrido post-diarios. El giro del worker del RSP entre
tareas estaba en 0 con la nota "medido neutro", y esa nota es de antes de los diarios, asi que
tocaba mirarla igual que las otras.

Tanda 1 (min de 5 rondas intercaladas, Parallel-RDP, ms de pared):

| juego | 0 | 8192 | 131072 |
|---|---|---|---|
| junkrunner64 | **7150** | 7186 | 7161 |
| Perfect Dark | 11501 | **11429** | 11561 |
| SM64 | 7478 | **7425** | 7482 |
| DK64 | **11729** | 11734 | 11733 |

8192 rascaba 0,6 % en PD y 0,7 % en SM64 y cobraba 0,5 % en jr, todo con las distribuciones muy
solapadas. Margen de ruido, o sea segunda tanda:

| juego | 0 | 8192 | 32768 |
|---|---|---|---|
| junkrunner64 | 7162 | **7151** | 7164 |
| Perfect Dark | **11359** | 11473 | 11425 |
| SM64 | 7517 | **7477** | 7502 |
| DK64 | 11725 | 11767 | **11720** |

**No reproduce.** En PD se da la vuelta entera (0 gana por 114 ms, justo donde 8192 ganaba por 72
en la primera) y en DK64 tambien. Lo unico que sobrevive a las dos tandas es SM64 por medio
punto, que es menos que el ruido de esta maquina. **RSPSPIN se queda en 0.**

Tiene sentido con lo que dice el perfil: aqui no hay decenas de miles de lanzamientos por
segundo como en el RDP sino unos cientos, asi que el viaje al kernel por despertar al worker se
reparte entre muchisimo mas trabajo. Es la misma razon por la que el giro del RDP SI importa y
este no.

**Con esto se cierra el re-barrido de las perillas de espera**: `KESTREL_RSPTANDA` 512 -> 1024,
`KESTREL_BARSPIN` 2048 -> 16384 y `KESTREL_RDPSPIN` 32768 -> 131072 se movieron;
`KESTREL_RDVPOLL` (64), `KESTREL_DPSPIN` (262144) y `KESTREL_RSPSPIN` (0) quedan confirmadas
donde estaban. Tres de seis. La regla que deja: **cuando se cierre una via que quite citas entre
hilos, re-barrer TODAS las perillas de espera antes de dar por buena ninguna**, porque una
perilla de espera es constante del acoplamiento y no del emulador.

Guest-neutro: 15 volcados de framebuffer por juego y tanda, un solo md5 cada uno
(jr `75e331cb`, PD `0f0adee7`, SM64 `29a0e995`, DK64 `eca336ea`).

## 2026-09-18 -- La holgura del regulador cuando las barreras son la autoridad: 4096 -> 65536

`kPaceSlack` (`KESTREL_PACESLACK`) es la ultima perilla del regulador que quedaba sin re-barrer
desde que los diarios (`dpLog`/`spLog`/`dmaLog`) y `dpBarSync` cambiaron el acoplamiento. El
barrido decia 65536 y el 2026-09-03 esa perilla se BAJO de 1 M a 4096 por **correccion**, no por
velocidad, asi que la primera reaccion fue no tocarla. Lo que cambio el veredicto es que las
barreras de invitado llegaron DESPUES de aquella nota.

**Lo que el hardware no justifica es el adelanto en tiempo de INVITADO.** Y hoy ese adelanto ya no
lo acota el regulador: `rspKick` arma `rcpPend|8` en CADA lanzamiento de tarea (Threaded con
`spBarrierOn()` y `rcpDeadlineOn()`), y `rcpRetire` llama a `spBarrierWait` en cada retiro, que
para el reloj del invitado en `spBarrierAt()` -- el instante al que el RSP ha trabajado de verdad
-- pase lo que pase con la holgura. El RDP tiene lo suyo con `dpBarrierWait` (`rcpPend|4`). Con las
dos puestas la holgura del regulador ya no decide CUANTO se adelanta la CPU emulada, solo cada
cuanto interviene el freno del ANFITRION antes de que mande la barrera. Ahi si es una perilla de
rendimiento, y una corta cuesta: el freno entra tantas veces por campo que la CPU pasa mas tiempo
en el condvar que emulando.

Por eso el cambio NO es subir la constante, es `Memory::paceSlack()`: 65536 cuando las barreras son
la autoridad (Threaded + `SPBARRIER` + `DPBARRIER` + `RCPDEADLINE`, o sea la configuracion normal)
y 4096 cuando alguna de esas escotillas de depuracion esta apagada y el regulador vuelve a ser el
unico freno de invitado. `KESTREL_PACESLACK`, si se pone, fija las dos: una perilla, un valor.
El regulador solo corre en Threaded (`system.cpp`, `const bool paced = rcpMode == Threaded`), asi
que Lockstep no se entera de nada de esto.

Barrido intercalado, min de 5, Parallel-RDP, ms de pared, DOS tandas independientes:

| juego | tanda 1: 4096 | 65536 | tanda 2: 4096 | 65536 | 1048576 |
|---|---|---|---|---|---|
| junkrunner64 | 7195 | **7051** | 7205 | **7057** | 7089 |
| Perfect Dark | 11423 | **11353** | 11445 | **11406** | 11239 |
| SM64 | 7459 | **7425** | 7485 | **7354** | 7341 |
| DK64 | 11741 | **11674** | 11774 | **11604** | 11598 |

Ocho de ocho a favor de 65536, y md5 de framebuffer identico en las 30 corridas (jr `75e331cb`,
PD `0f0adee7`, SM64 `29a0e995`, DK64 `eca336ea`).

**El liston de esta perilla NO es la pared.** Es el mismo con que se fijo el 4096: arranques
limpios de Perfect Dark. Con 1 M descarrilaba ~1 de cada 8 y acababa girando en un hilo con IE=0
tomando una excepcion de coprocesador por vuelta. Prueba hecha, 600 intercambios por arranque:
**30 de 30 limpios con 65536** (15 intercalados contra un control de 15 con 4096, mas 15 sueltos),
todos `rc=0` y todos con md5 `0f0adee7`. A una tasa de fallo de 1 de cada 8, 30 arranques limpios
dejan un 1,8 % de probabilidad de no haberlo visto.

**1 M sigue descartado** aunque en la tanda 2 midiera bien en tres juegos: es el orden de magnitud
que descarrilaba, no hay nada que lo justifique, y la barrera del SP se puede apagar por variable.

Solo coste de anfitrion: `[statehash]` de junkrunner64 a 400 M `7f1b537e69aad3f4` en Lockstep y en
Threaded con el cambio puesto. systemtest/sm64 PASS en los diez modos, md5 `d35bd8aa` y `b5521b24`,
krom interp 88,72/92,07 regress=0, krom prdp 89,27/92,56 regress=0.

## 2026-09-18 -- KESTREL_PACEGRAIN re-barrido: se queda en 1024

Ultima perilla del frente abierto el 2026-09-18 ("las perillas que se afinaron ANTES de los
diarios"). `kPaceGrain` es el suelo del permiso que el regulador concede de una vez: en
`Memory::paceGrant`, `if(left < kPaceGrain) left = kPaceGrain;`. Grano pequeno = el freno
interviene mas veces y mas fino; grano grande = menos intervenciones pero el adelanto real
se pasa del objetivo antes de que nadie lo mire.

Min de 5, intercalado, Parallel-RDP, pared en ms:

| juego | 256 | **1024 (actual)** | 4096 |
|---|---|---|---|
| junkrunner64 | 7563 | **7128** | 7531 |
| Perfect Dark | 12142 | 11805 | **11761** |
| SM64 | 7888 | 7734 | **7605** |
| DK64 | 11822 | **11746** | 11749 |

md5 identico en los cuatro por juego (jr `75e331cb`, PD `0f0adee7`, SM64 `29a0e995`,
DK64 `eca336ea`): la perilla es de anfitrion, el invitado sale igual.

**Veredicto: 1024 se queda, y no hace falta segunda tanda.** 256 pierde en los cuatro, o
sea que el suelo importa y esta por encima de 256. Entre 1024 y 4096 no hay ganador: 4096
gana PD por 0,4 % y SM64 por 1,7 %, empata DK64, y **cobra +5,7 % en junkrunner64**
(7531 contra 7128). La regla de la casa pide segunda tanda cuando el margen cabe en el
ruido, pero aqui no hace falta llegar a eso: aunque los tres margenes pequenos fueran
reales, el de junkrunner64 los triplica y va en contra. Una perilla que gana 1 % de media
en tres juegos y pierde 5,7 % en el cuarto no se toca.

El porque encaja con el mecanismo: junkrunner64 es el juego con las tareas de RSP mas
cortas y seguidas, o sea el que mas veces cruza el umbral del regulador por unidad de
tiempo; con grano 4096 cada concesion se pasa, la CPU adelanta de mas y acaba esperando en
la barrera del SP, que es exactamente el coste que el grano fino evita. Los juegos con
tareas largas (PD, SM64) apenas notan el grano porque entre dos intervenciones hay trabajo
de sobra.

Con esto el re-barrido queda **CERRADO**: ocho perillas revisadas, tres movidas
(`RSPTANDA` 512->1024, `BARSPIN` 2048->16384, `RDPSPIN` 32768->131072), una movida con
condicion (`PACESLACK` 4096->65536 solo cuando las barreras de invitado mandan) y cuatro
confirmadas donde estaban (`RDVPOLL` 64, `DPSPIN` 262144, `RSPSPIN` 0, `PACEGRAIN` 1024).

## 2026-09-18 -- El DMA del PI dura lo que dura: plazo con los tiempos de PI_BSD_DOM*

Segundo plazo de periferico, hermano del que el SI estreno el 2026-09-08. Hasta hoy
`piDma()` copiaba los bytes y en la MISMA instruccion ponia `PI_STATUS = 0x8` (DMA hecho) y
levantaba `MI_PI`. O sea que un `osPiStartDma` de 64 KB de cartucho terminaba en cero tiempo
de invitado: el hilo que se dormia en `osRecvMesg(&dmaMessageQ)` se despertaba antes de
ceder, y el reparto de trabajo dentro del cuadro no se parecia al del aparato.

### El bus del cartucho, y de donde salen sus tiempos

Fuente: n64brew, *Peripheral Interface* y *ROM Header*. El bus del PI es de **16 bits** y va
por **paginas**: se manda la direccion base una vez y luego se leen `2^(PGS+2)` bytes
seguidos; al cruzar la pagina hay que volver a mandarla. Los cuatro registros
`PI_BSD_DOM1_{LAT,PWD,PGS,RLS}` (`0x0460_0014`..`0x0460_0020`) son todos **ciclos del RCP
menos uno**:

- `LAT+1` ciclos entre el flanco de la direccion (ALE_L) y el primer `/RD` de la pagina
- `PWD+1` ciclos con `/RD` abajo, por cada 16 bits
- `RLS+1` ciclos con `/RD` arriba entre dos palabras de 16 bits

`Memory::piXferCycles` recorre la transferencia pagina a pagina con esa cuenta y devuelve
ciclos de RCP; `rcpCyclesToInsns` los pasa a instrucciones retiradas con la misma regla de
tres que ya usaba el VI (`viFieldInsns` / `viFieldHzMilli`), que es el reloj con el que
vence todo lo demas.

**Cuadre con la realidad.** Con los valores de casi cualquier cartucho comercial
(`0x80371240` -> LAT 64, PWD 18, PGS 7, RLS 3) sale, por pagina de 512 B:
`65 + 256*(19+4) = 65 + 5888 = 5953` ciclos de RCP. A 62,5 MHz son **95,2 us por pagina**,
o sea **5,375 MB/s**: exactamente la tasa de lectura de cartucho que la documentacion da
por conocida. El modelo no se calibro para que diera eso; da eso porque los tiempos son los
del hardware.

### El agujero que habia debajo: los PI_BSD_DOM1 estaban a CERO

Al escribir el modelo salio un cartucho imposible (tiempos cero = ancho de banda infinito).
La razon es que en la consola quien programa esos cuatro registros es el **IPL2**, copiando
los bytes 0x01..0x03 de la cabecera de la ROM; aqui el arranque es HLE y no lo hacia nadie.
Ahora lo hace `Memory::loadRom`. Reparto de bits, que se documenta mal por ahi:

| byte | contenido |
|---|---|
| 0x00 | **reservado** (0x80 en los comerciales; NO es configuracion, pese a lo que se lee a veces) |
| 0x01 | bits 4-5 = RLS, bits 0-3 = PGS |
| 0x02 | PWD |
| 0x03 | LAT |

El **dominio 2** (SRAM/FlashRAM, bus de `0x0500_0000`) se deja como estaba, a cero: la
cabecera no lo lleva, el IPL2 no lo toca y el juego que use la pila del save lo programa el
mismo antes de su primer DMA. Inventarle un valor de reinicio seria fingir un dato que no
tenemos; con los registros a cero el modelo cuenta de menos, que es justo lo que hacia antes
de existir el plazo.

### Maquinaria

Identica a la del SI, que para eso se hizo generica:

- `piBusy` / `piDoneAt` = el motor esta ocupado hasta ese valor de `cpu.retired`.
  `PI_STATUS` devuelve `PI_DMA_BUSY` mientras tanto, que es lo que el invitado sondea.
- `piDma()` empieza con `if(piBusy) piFinish();` -- en el aparato un DMA nuevo mientras hay
  otro en vuelo no existe (el juego mira el bit de ocupado antes), pero si pasa, el anterior
  se da por terminado en vez de perderse.
- **La copia se hace al armar, no al vencer.** El motor del aparato va escribiendo la RDRAM
  durante la ventana, asi que un invitado que lea antes de la interrupcion lee basura en las
  dos maquinas; lo unico que observa de verdad es el FINAL, y eso es lo que se retrasa.
- El vencimiento se mira por instruccion en `System::stepCpu()` (dos sitios, en espejo de los
  del SI) y el JIT tiene prohibido compilar un bloque que se tragaria el plazo: los tres
  `siDueIn()` de `jit.cpp` pasan a `ioDueIn()`, que es el minimo de los dos plazos de E/S.
- `piArm` pone `*jitGuardPtr = 0` porque lo dispara un store que con el JIT puede ir en mitad
  de una cadena enlazada cuyo permiso no conocia este plazo (mismo caso que `siDma`).
- La foto de estado lleva `piBusy` y `piDoneAt`; `kVersion` 12 -> 13.

De paso se arreglo la **escritura a PI_STATUS**, que zapeaba el registro entero. En el
hardware es un registro de ordenes: bit 0 = reiniciar el controlador (aborta el DMA en
vuelo), bit 1 = limpiar la interrupcion (y SOLO eso, `pi_status &= ~0x8`). Un juego que
escriba 2 para reconocer la interrupcion ya no borra de paso el bit de ocupado.

### Validacion

Este es el primer cambio del dia que toca **semantica de invitado**, no coste de anfitrion,
asi que se esperaba que alguna huella se moviera y habia que mirar cual y por que.

| prueba | resultado |
|---|---|
| systemtest interp | PASS 0/3721 - 0/2 - 0/6 |
| systemtest threaded-jit | PASS 0/3721 - 0/2 - 0/6 |
| sm64 60 campos, los 9 modos de `gate_all` | md5 `d35bd8aa...` **sin cambio** |
| krom 371 ROMs | 88,72 / 92,07, regress=0 improve=0 new=0 |
| junkrunner64 `[statehash]` Lockstep | `6c21ef859df54730` |
| junkrunner64 `[statehash]` Threaded x3 | `6c21ef859df54730` (3/3, **== Lockstep**) |
| Perfect Dark, 15 arranques de 600 campos | **15/15 limpios**, md5 `6ff31a96` los quince |

El `[statehash]` de junkrunner64 **si se movio**, de `7f1b537e69aad3f4` a `6c21ef859df54730`,
y es legitimo: la huella son los registros arquitectonicos de la CPU en el campo 60, o sea
donde estaba el programa en ese instante, y ahora el programa pasa por un `osPiStartDma` que
tarda. Que `KESTREL_PIINSTANT=1` tampoco devuelva el valor viejo (`5c38c4a6c74d3be4`) lo
confirma desde el otro lado: parte del desplazamiento no es el plazo sino los
`PI_BSD_DOM1_*` ya programados, que antes valian cero. Las dos mitades son cambios de
hardware genuinos y las dos tenian que mover la huella. Lo que NO podia moverse -- y no se
movio -- es la igualdad Lockstep == Threaded, que es el oraculo de verdad.

El md5 del framebuffer de Perfect Dark en el campo 600 tambien se movio, de `0f0adee7` a
`6ff31a96`, por la misma razon: el juego llega a ese campo habiendo hecho las cargas de
cartucho en un instante distinto. Lo que importa de esa prueba no es el valor sino que sea
**el mismo en los quince arranques** y que los quince acaben limpios: Perfect Dark es el
juego que descarrila cuando el acoplamiento CPU-RCP se afloja de mas (1 de cada 8 con
`kPaceSlack` a 1 M), y es el unico liston que dice si un cambio de tiempos de invitado ha
roto algo que las puertas no ven.

## 2026-09-18 -- MI_VI y MI_AI dejan de llegar tarde: agenda de eventos por instante

El SI y el PI ya rematan su plazo en la instruccion exacta. El barrido de video no: `viTick`
se llamaba UNA vez por subtramo del bucle (`Clocks::tickInsns()` = campo / `viTicksPerField`,
16 por defecto), asi que `MI_VI` no caia en el cruce de la linea programada en `VI_INTR` sino
en el borde del subtramo siguiente -- **hasta ~1 ms tarde**. Y `aiTick`, que cuelga de
`viTick`, arrastraba el mismo defecto para `MI_AI`.

Lo del AI merece una nota, porque en el barrido anterior lo di por bueno y NO lo era. `aiTick`
drena por muestras del DAC y acumula el credito en `aiAcc`, asi que la CANTIDAD de audio que
consume es correcta e independiente de la granularidad. Lo que no era independiente es el
INSTANTE en que se ve que un bufer se ha acabado: eso se miraba solo cuando alguien llamaba a
`aiTick`, o sea en el borde del subtramo. Drenar bien la cantidad y levantar tarde la
interrupcion son dos cosas distintas.

### La sonda: un ajuste de anfitrion no puede mover al invitado

`viTicksPerField` es un ajuste de ANFITRION -- cada cuanto vuelve el bucle a mirar el RCP --
y por tanto cambiarlo no puede cambiar ni una instruccion de lo que ve el juego. Esa es la
prueba, y es binaria. Antes de este cambio, junkrunner64 a 200 M instrucciones daba:

| `KESTREL_VITICKS` | 1 | 4 | 16 | 64 |
|---|---|---|---|---|
| `[statehash]` ANTES | `045d2dfe…` | `49f0daf8…` | `e403ad4e…` | `49f0daf8…` |
| `[statehash]` AHORA | `e403ad4e65ba9c4d` | igual | igual | igual |

Cuatro huellas distintas donde tenia que haber una. Ahora hay una, y la misma en Lockstep y
en Threaded. Con SM64 (que es el que tiene audio de verdad) pasa lo mismo: `cec13be0a57c7e8a`
con `VITICKS` 4, 16 y 64 en interprete.

Aviso para la proxima vez: con JIT y `KESTREL_MAXINSN` la huella SI puede moverse aunque el
emulador sea exacto, porque el tope corta a mitad de cadena y el ultimo bloque se pasa por un
puñado de ops. Eso es la sonda, no el emulador. Se distingue mirando la traza de
interrupciones (`KESTREL_IRQTRACE=<n>`, que ahora imprime el instante de invitado de cada
subida): si las dos listas son identicas y solo cambia el corte final, no hay divergencia.

### La maquinaria

Un plazo armado por evento, en el mismo reloj de invitado que ya usan el SI y el PI:

- `viNextEvent(now)` resuelve la MISMA aritmetica que `viTick` usa para detectar cruces
  (mismo `off`, misma division entera) pero al reves: cuando cae el proximo cruce de la linea
  de `VI_INTR` o el proximo cierre de campo, lo que llegue antes. Si no coincidieran, el
  plazo apuntaria a una instruccion en la que `viTick` no dispara nada.
- `aiNextEvent()` hace lo propio con el DAC: cuantas instrucciones faltan para que el credito
  acumulado llegue a los bytes que le quedan al bufer en curso.
- `evNextAt` es el minimo de los dos, ya plegado, para que la comprobacion del camino caliente
  sea UNA comparacion por instruccion.
- `eventDueIn(now)` = min(`ioDueIn`, `evDueIn`) es lo que consultan las TRES guardas de
  `jit.cpp`, que antes plegaban a mano `siDueIn`/`piDueIn`. Un plazo nuevo ya no es otra
  guarda suelta: se mete en `eventDueIn` y las tres guardas lo respetan solas.

El subtramo se queda como estaba: `stepCpu` remata el plazo en la instruccion exacta y le
pasa el cierre de campo al bucle por `viFieldPend`, porque los trucos, el avance por
fotogramas y los volcados de diagnostico siguen colgando de ahi.

### El detalle que costo encontrar

La primera version dejaba `MI_AI` **peor** que antes. La traza lo señalo en una linea: la
PRIMERA `MI_AI` caia en 53844235 con `VITICKS=4` y en 53844052 con 16, 183 instrucciones de
diferencia. El motivo es que `aiAcc`/`aiLastRetired` son un credito FECHADO, y al encolar un
bufer (escritura de `AI_LEN`) se armaba el plazo sobre un `aiLastRetired` viejo -- tan viejo
como llevara el AI sin mirarse, o sea un trozo de subtramo. La correccion es poner el DAC al
dia **antes** de tocar la cola: `aiTick(cartNow())` al principio del manejador de `AI_LEN` y
de `AI_DACRATE`. El VI no necesita esto porque sus eventos son posiciones ABSOLUTAS del
campo, no un credito acumulado.

### Efecto lateral: `viTicksPerField` se queda sin trabajo de precision

Su unica razon de ser era esa: cuanto mas fino el subtramo, menos tarde llegaba `MI_VI`. Ya
no. A partir de ahora es solo cada cuanto vuelve el bucle a mirar, o sea coste de anfitrion
puro, y subirlo es gratis en exactitud.

Barrido hecho el mismo dia, y el resultado es el CONTRARIO del esperado: subirlo es una
perdida pura. Minimo de 5 rondas intercaladas, Parallel-RDP, ms de pared:

| `KESTREL_VITICKS` | 16 | 64 | 256 |
|---|---|---|---|
| junkrunner64 | 7766 | 8046 (+3,6 %) | 8299 (+6,9 %) |
| Perfect Dark | 11981 | 11945 (-0,3 %) | 12166 (+1,5 %) |
| SM64 | 7572 | 8516 (+12,5 %) | 10851 (+43,3 %) |
| DK64 | 12082 | 12568 (+4,0 %) | 14878 (+23,1 %) |

El subtramo no era peso muerto para el anfitrion: es tambien la CADENCIA DE SERVICIO de todo
lo demas que hace el bucle exterior -- empuje de audio, presentacion, marcapasos -- y sobre
todo de cuando los workers del RCP vuelven a encontrar a la CPU. Con subtramos largos la CPU
corre tramos mas largos de un tiron y los hilos del RSP y del RDP esperan mas. Se queda en 16.

De propina, la huella del framebuffer sale IDENTICA con los tres valores en los cuatro juegos
(`75e331cb`, `817b7132`, `29a0e995`, `af650b0b`), que es evidencia extra de la invariancia de
arriba: la perilla mueve pared, no invitado.

### Validacion

`gate_all` 734 s: systemtest `Base 0/3721 Timing 0/2 Cycle 0/6` y sm64
`d35bd8aa9b13d459ce9332c07a79a53a` en los diez modos software, krom interp 371/371
`mean_exact` 88,72 / `mean_close` 92,07 regress=0. `gate_prdp` 518 s: systemtest 0/3721,
sm64 `b5521b24d8fc280fbf102df22d7d30cb` en prdp y prdp-jit -- o sea la huella de
parallel-RDP tampoco se mueve --, krom prdp 371/371 89,27 / 92,56 regress=0. Perfect Dark
con Parallel-RDP y threaded-jit, 15 arranques de 15 limpios. Las dos puertas recompilan
`build/` y `build-prdp/` enteros porque cambia `memory.hpp`.

## 2026-09-18 — De que esta hecho `rdpSubmit`: el sondeo `[dpsnap]`

El hilo del RSP es el que patea el RDP, y el perfil de anfitrion del 2026-09-11 le ponia un
10,6 % de sus muestras "fuera de imagen desde `Memory::rdpSubmit`". Eso no dice QUE cuesta.
Ahora el emulador lo parte en piezas: `KESTREL_DPSUBPROF=1` mide con `steady_clock` el tramo
entero, lo que cuesta COGER `rdpMx`, la copia a la sombra del FIFO, `dpScheduleSpan`, el paseo
de coste de dentro y el `notify_all` de fuera, y `System::run` lo saca en `[dpsnap]` al final.
Apagado no cuesta nada: un `bool` estatico por rama.

Lo primero que dice es el TAMANO del problema, y no era el que se suponia:

```
[dpsnap] 519603 copias del FIFO, 38.6 MB (77 B de media), ... / 518473 cmds
```

**Un envio por comando.** El microcodigo escribe `DPC_END` 519 603 veces en 300 campos de SM64
para 518 473 comandos de RDP, con 77 B de media por tramo (Perfect Dark: 464 639 envios, 75 B).
O sea que aqui no hay ancho de banda que optimizar -- 38 MB en 8 s es nada --, lo que se paga
es la LLAMADA. Eso ya mato por si solo la idea de copiar con almacenes no temporales
(`_mm_stream_si128`): el umbral razonable son 4 KB y el tramo medio son 77 B, asi que la rama
no llegaria a entrar nunca; y ademas obligaria al paseo de coste, que lee esa misma copia acto
seguido, a traersela de vuelta de la DRAM.

Descontando el coste del propio reloj --- medido de paso: subir el sondeo de 2 a 10 lecturas de
`steady_clock::now()` por llamada sube la cuenta de 0,372 a 0,578 s, o sea **~50 ns por lectura**
en esta maquina --- el reparto real de los 0,32 s (de 7,9 s de pared, **4,0 %**) es:

| pieza | s | % del envio |
|---|---|---|
| paseo de coste (`rdpCostPass`) | 0,129 | 40 % |
| cola y banderas (`rdpQueue`, `dpPending`, `rcpPend`) | 0,123 | 39 % |
| papeleo de `dpScheduleSpan` (anillo, `dpWrLo/dpWrHi`, plazos) | 0,062 | 19 % |
| `notify_all` | 0,004 | 1 % |
| coger `rdpMx` | ~0 | ~0 |
| copia a la sombra | ~0 | ~0 |

Dos sorpresas. **El mutex no tiene contencion** y **la copia no se nota**: las dos sospechas
"naturales" son falsas. Y el `notify_all` sale 771 veces de 519 603, que es la prueba de que el
giro del worker del RDP (`KESTREL_RDPSPIN`) hace exactamente lo que se le pidio.

Lo que queda es papeleo (0,185 s) y exactitud (0,129 s, el modelo de ciclos del RDP, intocable).
El techo de esta via es por tanto 2,4 % de pared, y encima cae en el hilo del RSP, que va al
70 % de nucleo mientras el de CPU va al 99 %: recortarlo puede no mover la pared.

### Hipotesis de falso compartir: MEDIDA Y DESCARTADA

El worker del RDP gira sobre `dpPending` leyendola en CADA vuelta, asi que su linea vive
permanentemente en estado compartido en ese nucleo; el hilo del RSP le hace un `fetch_add` en
cada envio, medio millon por corrida, y cada uno tiene que pedir la linea en exclusiva contra
un nucleo que no para de leerla. Encaja con los ~237 ns por envio del bloque "cola y banderas".

Se anadio `KESTREL_RDPSPINMASK=<n>`: leer `dpPending` una vuelta de cada `n+1` sin cambiar el
numero de vueltas, o sea misma duracion del giro y 16 o 64 veces menos trafico de coherencia.
Un solo binario, los dos brazos por entorno. Resultado en SM64, 300 campos:

| mascara | `rdpSubmit` | pared |
|---|---|---|
| 0 (cada vuelta) | 0,597 s | 8,01 s |
| 15 | 0,588 s | 8,01 s |
| 63 | 0,578 s | 7,91 s |

Un 3 % sobre el coste del envio, dentro del ruido, y el barrido intercalado de pared sobre los
cuatro juegos no dio ganador en tres rondas (SM64 7749 contra 7942 ms de minimo, jr 8389 contra
8241, PD 11528 contra 11926, DK64 12121 contra 12422 -- reparto, no tendencia). La perilla se retira del arbol
(no se deja una perilla muerta). **El coste de la cola no es coherencia de cache.**

## 2026-09-18 — Clavar los hilos a nucleos fisicos: MEDIDO Y DESCARTADO

La telemetria `[bloque]` muestra tres hilos calientes muy desiguales en este anfitrion
(i7-870, 4 nucleos / 8 hilos): CPU al 99 % de nucleo, RSP al ~70 %, RDP al ~53 % de reloj
aunque solo este ocupado el 11,8 % del tiempo. Con SMT el planificador de Windows puede
dejar dos de esos tres en los DOS HERMANOS del mismo nucleo fisico, donde comparten unidades
de emision y L1: ahi uno le roba al otro de verdad, y el que suele perder es el hilo de CPU,
que es el palo largo. Nunca se habia probado a clavarlos (cero coincidencias de
`SetThreadAffinityMask` en el arbol).

Se metio `KESTREL_AFFINITY=1`: `GetLogicalProcessorInformationEx(RelationProcessorCore)` da
la mascara de cada nucleo fisico, y cada hilo se clava a un nucleo DISTINTO -- a los dos
hermanos de ese nucleo, no a un procesador logico suelto, para que una interrupcion que se
lleve a un hermano no deje al hilo sin sitio. Un solo binario, los dos brazos por entorno.

Barrido intercalado de cuatro rondas, minimo de cuatro por juego:

| juego | suelto | clavado | |
|---|---|---|---|
| junkrunner64 | 7579 ms | 7672 ms | +1,2 % |
| Perfect Dark | 11571 ms | 11511 ms | −0,5 % |
| SM64 | 7631 ms | 7768 ms | +1,8 % |
| Donkey Kong 64 | 11424 ms | 11629 ms | +1,8 % |

Tres perdidas y una ganancia que cabe en el ruido: **no hay ganador, y si tendencia a
perder**. Tiene sentido a posteriori. Los tres hilos no estan calientes a la vez: el del RDP
esta ocupado el 11,8 % del tiempo y el del RSP tiene holgura, asi que lo que el planificador
hace por su cuenta -- mover el hilo que despierta al nucleo que en ese instante esta libre y
con la cache aun templada -- es mejor que una particion fija, que ademas impide que el hilo
de CPU use los dos hermanos de su nucleo cuando los otros dos duermen. Clavar solo ganaria
si los tres estuvieran saturados de verdad.

El codigo se quita del arbol (no se deja una perilla muerta). Lo que si queda anotado es que
los md5 del framebuffer salieron **identicos en los cuatro juegos y en las dos ramas**: la
afinidad es ajuste de anfitrion puro y no mueve ni un bit de invitado, que es lo que tenia
que pasar.

## 2026-09-18 — El freno del RCP se preguntaba demasiado: `KESTREL_PACEASK`

Leyendo `System::stepCpu` salta una asimetria vieja. El camino del interprete pregunta al
regulador **una vez cada 64 instrucciones** (`if(paced && (i & 0x3F) == 0)`); el camino del
JIT lo hacia **tras cada bloque, sin filtro**. Como un bloque son unas pocas decenas de ops,
el dynarec acababa preguntando mas veces por op que el interprete, justo al reves de lo que
uno esperaria del camino rapido.

Y preguntar no es gratis. `rcpPace` -> `rspPace`/`rdpPace` lee `rspBusy`, `rsp.cyclesRun` y
`rdpBusy`: tres lineas que los workers reescriben sin parar. Cada consulta se las pide en
exclusiva al nucleo que las esta escribiendo, o sea que el hilo de CPU frena precisamente al
hilo que tiene que avanzar para que el freno se suelte. `docs/GAPS.md` ya atribuia ~7 % de
las muestras del hilo de CPU a estas dos funciones.

Tampoco hace falta esa cadencia. Quien acota el adelanto **en tiempo de invitado** es la
barrera del SP (`spBarrierWait` en `spBarrierAt()`, en cada retiro); el regulador es un freno
de ANFITRION con una holgura de 65536 ops, asi que mirar cada mil no mueve donde muerde. Y
la cadena enlazada del JIT ya vuelve a preguntar por su cuenta cuando agota el permiso
(`jitReenterProceed`), que es el camino que de verdad regula.

`KESTREL_PACEASK=<ops>` pone el grano de esa pregunta; `=0` es el comportamiento de antes.

Barrido intercalado con Parallel-RDP + threaded-jit, minimos por juego:

| juego | preguntando cada bloque | cada 1024 ops | |
|---|---|---|---|
| Perfect Dark | 11734 ms | 11341 ms | **−2,5 %** |
| SM64 | 7544 ms | 7453 ms | **−1,4 %** |
| junkrunner64 | 7518 ms | 7437 ms | **−1,1 %** |
| Donkey Kong 64 | 11403 ms | 11370 ms | −0,3 % |

PD y SM64 se midieron **dos veces** por la regla de la casa (el margen rozaba el ruido en la
primera tanda): −2,3 %/−3,0 % y −0,1 %/−1,0 %, y en la segunda las lecturas de los dos
brazos ni se solapan. La calibracion del grano es monotona hasta 1024 y ahi hace **meseta**:
64 → 1024 vale otro −2,2 % en PD y −1,4 % en SM64, pero 1024, 8192 y "casi nunca" (10^6)
empatan dentro del ruido. Se queda **1024**, que es el primer punto de la meseta y el que
deja el freno mas despierto.

El md5 del framebuffer es identico en los cuatro juegos y en todos los granos, incluido el
de "casi nunca": el regulador es ajuste de anfitrion y no mueve estado de invitado.

## 2026-09-18 -- Perfil fresco del hilo de CPU, y por que el 15 % de `div` NO se cobra

El perfil de `docs/GAPS.md` era del 2026-09-11 y ya no valia. Tomado uno nuevo con
`build-prof-prdp` (Perfect Dark, 600 campos, Parallel-RDP + threaded-jit, 1243 muestras,
89 % dentro de imagen):

| % | funcion | donde |
|---|---------|-------|
| 15,1 | `PaceDiv::div` | dentro de `spBarrierWait` |
| 9,3 | `atomic_load<u64>` | idem (`rsp.cyclesRun`) |
| 6,5 | `spBarrierWait` | |
| 6,0 | `jitTryBlock` | compilador del dynarec |
| 5,9 | `dpLogApply` | + 1,9 % fuera de imagen en su `notify_all` |
| 3,9 | `rcpRetire` | |
| 3,8 | `spCycleAt` | |
| 2,3 + 1,6 | `rspPace` + `rdpPace` | el 7,0 % viejo sigue ahi, ya cobrado en parte |

Sumando lo que cae dentro de `spBarrierWait` y sus inlinees salen ~44 % del hilo de CPU, y
de eso la mitad es ARITMETICA: `spBarrierAt()` convierte ciclos de RCP a ops en cada retiro
con un multiplicar de 64 bits mas el reciproco de 128 de `PaceDiv`.

### MEDIDO Y DESCARTADO: memoizar `spBarrierAt()`

`spBarrierAt()` es funcion pura de `spKickEdge`, `spKickCycles` (que solo escribe el hilo de
CPU al lanzar) y `rsp.cyclesRun` (que el worker publica cada pocos miles de instrucciones de
RSP). Se pregunta en CADA retiro, o sea que la inmensa mayoria de las llamadas repiten la
misma cuenta. Se probo un memo exacto -- si el contador no se ha movido, se devuelve el valor
de antes; si se ha movido, se recalcula entero -- en una entrada aparte (`spBarrierAtCpu`),
porque `spBarrierAt()` la llama TAMBIEN el hilo del RSP por `rspGuestNow()` y un memo en
miembros planos ahi seria una carrera.

md5 de framebuffer identico en los cuatro juegos y en los cuatro cruces memo-si/no x
Lockstep/Threaded. O sea que la cuenta sale bit a bit igual. Pero la pared no se mueve:

| juego | tanda 1 (min de 5) | tanda 2 (min de 6) |
|-------|--------------------|--------------------|
| jr | -0,70 % | -- |
| PD | +0,03 % | -0,26 % |
| SM64 | +1,47 % | +0,52 % |
| DK64 | +0,38 % | -- |

Dos tandas, ninguna reproduce nada y SM64 sale peor en las dos. Codigo retirado del arbol.

### La leccion, que vale mas que la perilla

**Quitarle ARITMETICA al hilo de CPU no se cobra: ese hilo esta bloqueado en la barrera
esperando al RSP, asi que su trabajo local es gratis.** Un 15 % de muestras no es un 15 % de
pared cuando el hilo se pasa la vida esperando -- ahorrarle ciclos solo le hace llegar antes
al mismo `wait_for`.

Lo que SI se cobro (`KESTREL_PACEASK`, -2,5 % en PD el mismo dia) no quitaba aritmetica:
quitaba LECTURAS de lineas que los workers reescriben. Esa es la regla para el proximo
candidato del hilo de CPU: se mide en trafico de coherencia y en bloqueos, no en muestras.

Y por eso mismo el memo no se puede llevar mas lejos. La variante fuerte seria saltarse
tambien la lectura de `cyclesRun` cuando el valor memorizado ya prueba que la barrera no
muerde, pero no vale: `spBarrierEff()` puede BAJAR sin que `cyclesRun` se mueva (el RSP se
aparca y manda `rspParkCap`, o se cierra una cita y cae el `kRdvLead`), asi que la lectura es
inherente. No hay version de esta idea que ahorre coherencia.


## 2026-09-18 -- El palo largo es el hilo del RSP, y ceder el nucleo 256 veces menos

Con el perfil del hilo de CPU ya explicado (la seccion de arriba), perfilo el OTRO lado:
`KESTREL_HOSTPROF=1 KESTREL_HOSTPROF_WHO=rsp`, Perfect Dark, 600 campos, `build-prof-prdp`,
Parallel-RDP + threaded-jit.

| | |
|---|---|
| hilo del RSP ocupado | **63,4 %** del tiempo de pared |
| `cpuWait` | 7,5 % |
| `dpLogWait` | ~38 % de sus muestras (17,9 % dentro de imagen + **20,3 % fuera**) |
| `spReadSync` | ~15,6 % |

El 20,3 % que cae fuera de la imagen con `dpLogWait` de llamante es `std::this_thread::yield()`.
En Windows eso es `SwitchToThread()`: una llamada al kernel que solo hace algo si hay otro hilo
listo en ESTE nucleo. Aqui hay 4 nucleos / 8 hilos y tres hilos calientes que ya viven cada uno
en el suyo, asi que la inmensa mayoria de las veces entra al kernel, mira, y vuelve sin haber
hecho nada.

Las esperas no sobran: `[dplog] ... 1 807 544 esperas` es, una a una, cada lectura de DPC que
hace el microcodigo, y cada lectura es una cita de orden de invitado con el hilo de CPU
(`Rsp::mfc0`, `rsp.cpp:494-498`). Eso es SEMANTICA, no se toca. Lo que se toca es el precio de
cada vuelta de la cita.

### `KESTREL_RDVYIELD`

Los tres bucles de cita (`dpLogWait`, la fase obligatoria de `rspParkWait`, `spReadSync`) cedian
el nucleo una de cada 256 vueltas. Ahora una de cada **65536**, con la constante sacada a un
`KESTREL_RDVYIELD` (potencia de dos; 256 = comportamiento viejo).

Barrido intercalado, minimo de 4, ms de pared, Parallel-RDP. **Dos tandas independientes**, que
es lo que pide la casa cuando el margen cabe en el ruido:

| juego | tanda A (256 -> 4096 -> 65536) | tanda B (256 -> 65536 -> 262144 -> 1 M) |
|---|---|---|
| junkrunner64 | 8041 -> 8014 (-0,34) -> 8000 (**-0,51**) | 8023 -> 7985 (**-0,47**) -> 7947 (-0,95) -> 7993 (-0,37) |
| Perfect Dark | 11456 -> 11428 (-0,24) -> 11382 (**-0,65**) | 11418 -> 11422 (**+0,04**) -> 11403 (-0,13) -> 11471 (+0,46) |
| SM64 | 7930 -> 7900 (-0,38) -> 7851 (**-1,00**) | 7867 -> 7820 (**-0,60**) -> 7811 (-0,71) -> 7838 (-0,37) |
| DK64 | 11909 -> 11873 (-0,30) -> 11865 (**-0,37**) | 11905 -> 11870 (**-0,29**) -> 11907 (+0,02) -> 11836 (-0,58) |

Siete de ocho lecturas a favor, y md5 del framebuffer IDENTICO en las 4 x 7 corridas (el pomo no
toca estado de invitado, solo cuando se entra al kernel). Lo que separa esto del ruido es que la
tanda A sale **monotona en los cuatro juegos**.

Por que 65536 y no 262144, que en tanda B sale un pelin mejor en jr y SM64: porque detras del
mismo `if` van tambien el aviso al hilo de CPU (`rspCv.notify_all`) y el salvavidas de pared
(`rdvWaiveDue`), asi que espaciarlo mas los retrasa a los dos. Y se nota: a 1 M, PD ya es PEOR
(+0,46 %) y DK64 no reproduce su ganancia. 65536 es el primer punto de la meseta -- el que deja
el aviso y el salvavidas mas despiertos -- y es el que reproduce en tanda A y tanda B.

Comprobado ademas lo que este pomo pone en riesgo:

- `scratchpad/pdboot.sh`: **ok=15 bad=0**.
- `[sprdv] 93762 citas, 0 renuncias, 0 relanzados` y `[dplog] 1182524 apuntadas, 1134580 esperas,
  0 renuncias` -- el salvavidas de 20 ms no llega a dispararse ni una vez. Tiene sentido: 65536
  vueltas de unos pocos ns siguen cayendo muy por debajo de 20 ms.

Que queda: el OTRO 17,9 % de `dpLogWait`, el que si esta dentro de la imagen.


## 2026-09-18 -- Lo que quedaba dentro de la vuelta de la cita: dos lineas que casi nunca se mueven

Con el yield ya espaciado a 65536 vueltas, la vuelta del bucle de cita se queda en esto: mirar la
condicion de salida (`ready()` / `cartNow()`, ya espaciada a 1 de cada 64 por `KESTREL_RDVPOLL`),
y DOS lecturas atomicas que no son condicion de salida y que si se hacian en cada vuelta:

- `dpLogFlush` -- aviso de vaciado del diario, que solo escribe la CPU al pararse
  (`System::quiesceRcp`).
- `rspStop` / `rsp.hostStop` -- la parada del anfitrion, que solo se escribe al cerrar.

Ninguna de las dos cambia mas de un punado de veces por corrida. `KESTREL_RDVCHEAP` (por defecto
puesto) las mete en la MISMA cadencia que la condicion de salida, o sea 1 de cada 64 vueltas. El
cierre se retrasa como mucho 63 vueltas de unos pocos ns.

Dos tandas intercaladas de min-de-4, Parallel-RDP, ms de pared:

| juego | tanda A (0 -> 1) | tanda B (0 -> 1) |
|---|---|---|
| Perfect Dark | 10925 -> 10604 (**-2,94 %**) | 10936 -> 10637 (**-2,73 %**) |
| SM64 | 7354 -> 7224 (**-1,77 %**) | 7404 -> 7243 (**-2,17 %**) |
| junkrunner64 | 7414 -> 7395 (-0,26 %) | 7337 -> 7428 (+1,24 %) |
| DK64 | 11276 -> 11301 (+0,22 %) | 11323 -> 11300 (-0,20 %) |

En PD y SM64 las CUATRO lecturas de cada brazo son disjuntas en las dos tandas (PD tanda A:
10604..10814 contra 10925..11066), o sea que no es el minimo el que gana, es la distribucion
entera. jr y DK64 salen planos: jr tiene la muestra sucia (su brazo `0` trae 7947 y 7821 de
cola) y es ademas el juego que menos se cita por el diario DPC. md5 del framebuffer identico en
las 4 x 4 corridas: esto no toca estado de invitado.

Por que pega tanto en PD: es el que mas sondea DPC (1,81 M de citas por corrida), o sea el que
mas vueltas de bucle da, y cada vuelta se ahorra dos lecturas de lineas compartidas. Es la misma
leccion de la manana pero por el lado bueno -- quitar TRAFICO DE COHERENCIA si paga.


## 2026-09-18 -- Los cinco campos del reloj de invitado, en UNA linea de cache

Tercera del dia por el mismo hilo, y la mas gorda: no cambia ni una instruccion de logica, solo
DONDE viven cinco campos de `CPU`.

`Memory::cartNow()` -- lo que contesta "por donde va la CPU" -- lee cinco sitios:

| campo | donde estaba |
|---|---|
| `retired` | bloque de control de ejecucion |
| `jitPending` | bloque del JIT |
| `stallCycles` | bloque de coste de cache |
| `stallOps` | idem, unas lineas mas abajo |
| `stallOpsRem` | idem |

Sueltos por el struct, a cientos de bytes unos de otros. Y quien mas llama a `cartNow()` es el
HILO DEL RSP: cada vuelta de sondeo de sus citas (`spReadSync`, `dpLogWait`) pregunta por ahi.
O sea que cada sondeo tiraba de VARIAS lineas que el hilo de CPU reescribe sin parar, y cada
una es un viaje de coherencia entre nucleos.

Juntos ocupan 28 bytes: **una sola linea**. Van con `alignas(64)` justo detras de `gpr[32]`
(offset 256, ya alineado; el JIT exige que `gpr` siga siendo el primer miembro, `RBX == cpu`).
Escribirlos los escribe solo el hilo de CPU, asi que compartir linea entre ellos no cuesta nada.

Dos tandas intercaladas de min-de-4, dos binarios que solo se diferencian en esto:

| juego | tanda A | tanda B |
|---|---|---|
| Perfect Dark | 10666 -> 10350 (**-2,96 %**) | 10609 -> 10378 (**-2,18 %**) |
| junkrunner64 | 7438 -> 7238 (**-2,69 %**) | 7430 -> 7223 (**-2,79 %**) |
| DK64 | 11351 -> 11174 (**-1,56 %**) | 11320 -> 11147 (**-1,53 %**) |
| SM64 | 7236 -> 7143 (**-1,29 %**) | 7204 -> 7084 (**-1,67 %**) |

Ocho de ocho, y en las dos tandas las cuatro lecturas de cada brazo son DISJUNTAS en los cuatro
juegos. md5 del framebuffer identico en las 4 x 4 x 2 corridas, y el `[statehash]` de
junkrunner64 a 400 M da `dc07d7ac23fef2e1` en los CUATRO cruces {base, colocado} x {lockstep,
threaded}: es colocacion pura.

Por que esta sale mas cara que la del yield: aqui no se ahorra una llamada al kernel de vez en
cuando, se ahorra un viaje de coherencia en CADA sondeo, y jr -- que en los dos cambios
anteriores salia plano porque casi no se cita por el diario DPC -- si lo nota, porque `cartNow()`
se pregunta pase lo que pase.

Cambio de colocacion, no de semantica: la foto de estado serializa campo a campo
(`savestate.cpp`, `io.pod(c.retired)`) y el JIT saca los desplazamientos con `offsetof`.

## 2026-09-18 -- junkrunner64 tumbaba el emulador: division entera por cero en `aiArm()`

`junkrunner64` es, por diseno, una ROM que escribe basura en los registros del RCP. Con
`KESTREL_MAXINSN=3000000000` el anfitrion se moria:

```
HOST EXCEPTION 0xc0000094
```

0xc0000094 es `STATUS_INTEGER_DIVIDE_BY_ZERO`: no es un fallo del invitado, es el
procesador del ANFITRION lanzando `#DE`. El RIP crudo no dice nada -- con ASLR cambia en
cada arranque --, asi que la primera pieza fue saber de que imagen era. Con el modulo y su
base, RVA = `0x9a993`, y `llvm-symbolizer` sobre el exe lo puso en `kestrel::Memory::aiArm()`.

Debajo, `aiNextEvent()`:

```cpp
const u64 rate = rcp.ai_dacrate ? (u64)(aiVidClock / (rcp.ai_dacrate + 1)) : 32'000ull;
```

`rcp.ai_dacrate` es `u32`. Con `0xffffffff` dentro, el `+ 1` da la vuelta a 0 EN ARITMETICA
DE 32 BITS antes de la division, y `aiVidClock / 0` mata el proceso. La guarda de `?:` no
sirve de nada: 0xffffffff no es cero.

La division no era el fallo, era el sintoma. El fallo estaba en la ESCRITURA: el registro se
guardaba crudo. AI_DACRATE tiene CATORCE bits -- n64brew, Audio Interface, `DACRATE[13:0]` --
y AI_BITRATE tiene CUATRO, `BITRATE[3:0]`; lo que se escriba por encima el hardware no lo
guarda. O sea que en una consola real `0xffffffff` se queda en `0x3fff` y `+1` no da la
vuelta nunca. Enmascarar en la escritura es la semantica de hardware, y de paso el `#DE` se
vuelve imposible por construccion:

```cpp
case 0x10: aiTick(cartNow()); rcp.ai_dacrate = v & 0x3fff; aiArm(); ...
case 0x14: rcp.ai_bitrate = v & 0xf; break;
```

Es la misma convencion que ya seguia el fichero tres lineas mas abajo con el PI
(`rcp.pi_dram_addr = v & 0x00ff'fffe;`).

Con el arreglo, cuatro corridas de junkrunner64 a 3e9 instrucciones terminan con `rc=0` y
cero `HOST EXCEPTION`.

### El manejador de fallos ahora dice DONDE

Lo que costo tiempo aqui fue el diagnostico, no el arreglo, asi que el manejador aprende la
leccion: antes de nada mira si el RIP cae dentro de una imagen cargada. Si cae, imprime el
fichero, la base y el RVA -- que es lo estable y lo que se le puede dar a `addr2line`. Si no
cae en ninguna, lo dice, porque en este emulador eso significa una cosa concreta: el codigo
que el dynarec genera al vuelo. Esa distincion decide a que mitad del programa se mira.

## 2026-09-18 -- Separar en lineas de cache los indices del diario DPC: NO REPRODUCE

Hipotesis razonable y de la misma familia que las tres anteriores: `dpLogHead` la escribe
SOLO la CPU y `dpLogTail` SOLO el hilo del RSP, pero cada uno LEE la del otro en su vuelta
caliente, y estaban en la MISMA linea. Ping-pong de escritura puro. Se probo el reparto
(`alignas(64)` a cada lado, los contadores con la cola) y lo mismo con el anillo `dmaPay`.

Dos tandas intercaladas, min-de-N, mismos binarios:

| juego | tanda 1 | tanda 2 |
|---|---|---|
| junkrunner64 | -0,96 % | **+0,21 %** |
| Perfect Dark | -0,41 % | -0,55 % |
| SM64 | -0,56 % | **+0,03 %** |
| DK64 | -0,30 % | -0,13 % |

Ni una sola lectura disjunta en ningun juego, y jr y SM64 cambian de signo entre tandas. Por
la regla de la casa -- si el margen cabe dentro del ruido hace falta una SEGUNDA tanda
intercalada, y si no reproduce no existe -- se revierte.

Por que esta no paga y la de los cinco campos del reloj si: alli lo que se junto era lo que
lee `cartNow()`, que el hilo del RSP pregunta en CADA vuelta de sondeo. Aqui el diario DPC se
toca una vez por tramo, no por vuelta, asi que el trafico de coherencia que se ahorra es de
otro orden de magnitud.

## 2026-09-18 -- El centinela de "no enlazado" del JIT era una direccion que el invitado SI puede pedir

junkrunner64 tumbaba el anfitrion, otra vez, y esta vez dentro del codigo emitido:

```
==== HOST EXCEPTION 0xc0000005 at host RIP 0000000000000000 ====
  RIP FUERA de toda imagen -> codigo emitido por el dynarec
  access violation: EXEC at host addr 0x0
  guest pc=0x0000000000000001 nextPc=0x0000000000000005 halted=0
```

`jit::kNoLink` vale **1**, y su comentario decia "VA imposible (impar: toda PC de N64 esta
alineada a 4)". La premisa es falsa: un `JR`/`JALR` con un registro impar produce una PC impar
-- lo que toca entonces es AdEL en el fetch, no que la direccion no exista. Y ese mismo 1 es:

- el valor con el que nace DESACTIVADA la guarda de cada sitio de enlace, y
- la etiqueta de cada entrada VACIA de la cache de destinos indirectos (ITC).

O sea que un salto indirecto a la VA 1 -- junkrunner64 los hace a proposito -- casaba con TODAS
las entradas vacias de la ITC, y el `jmp qword [rdx+8]` se iba al `code` de una entrada sin
rellenar, que es 0. Salto a RIP 0.

Arreglo (`src/cpu/jit.cpp`, cola de salida de control): antes de comparar nada, la ruta
INDIRECTA descarta los destinos desalineados y los manda a la salida lenta, donde el interprete
levanta el AdEL que el hardware levanta (`src/cpu/cpu.cpp:1212`). Con eso el centinela vuelve a
ser inalcanzable por construccion. Son tres instrucciones (`mov rax,rcx` / `test al,3` / `jne`) y
solo en `JR`/`JALR`: el destino de un `J` o de un branch sale de la codificacion de la
instruccion y siempre esta alineado, asi que en esa ruta no se emite nada.

La guarda del enlace ESTATICO comparte el centinela pero no comparte el agujero, y por la misma
razon: alli el `RCX` de tiempo de ejecucion tambien viene de la codificacion.

## 2026-09-18 -- Telemetria: todos los salvavidas de PARED, juntos y en una linea

Perseguir una divergencia obligaba a descartar uno por uno los puntos donde decide el anfitrion
y no el invitado, y varios de sus contadores existian pero **no se imprimian en ningun sitio**
(`dpRdvWaives`, `rspParkWv`, `spBarWaives`, `dpBarWaives`). Ahora el bloque de cierre saca dos
lineas mas:

```
[pared]  renuncias sp=.. dp=.. diario=.. barSP=.. barDP=.. aparcado=../..
[spvenc] N vencidos: adelanto=.. aparcado=.. otro=.., rebase max=.. ops
```

`[spvenc]` reparte los plazos de fin de SP nacidos vencidos por la escotilla que dejo pasar a la
CPU -- el adelanto `kRdvLead` de la cita, el tope del aparcamiento, o ninguno de los dos -- y da
el mayor rebase en ops. Un plazo vencido significa que `MI_SP` cae donde haya llegado el
anfitrion, que es divergencia directa; hasta ahora solo se sabia CUANTOS, no POR QUE.

OJO: estas lineas solo salen si la corrida para por `KESTREL_MAXFLIPS`, `KESTREL_MAXSYNCS` o
`KESTREL_MAXFIELDS`. Con `KESTREL_MAXINSN` el bloque de cierre no se ejecuta.
