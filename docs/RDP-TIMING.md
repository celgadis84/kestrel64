# RDP timing: the hardware dataset, and what kestrel matches

Second validation battery, alongside n64-systemtest (CPU) and the krom/PeterLemon
suite (RDP pixels): **[Thar0/RDP-Timing-Tests](https://github.com/Thar0/RDP-Timing-Tests)**.
Where krom's ROMs check *what* the RDP draws, this one checks *how long it takes* —
it sweeps 100 fill configurations and reports the DPC performance counters for each,
with a reference file of values measured on real hardware.

That makes it the only oracle we have for the RDP↔RDRAM cost model. krom's PNGs
cannot see timing at all; systemtest does not touch the RDP.

## Running it

The ROM is built from a local checkout at `../rdp-timing-tests` (libdragon):

```bash
# libdragon host tools, once:  make tools-install   (N64_INST=/root/n64tool)
cd /e/Claude/N64/rdp-timing-tests
PATH=/root/n64tool/bin:$PATH make EXTRA_CFLAGS=-DTOTAL_RUNS=8
```

Two local changes to the upstream ROM, both for emulator use:

- `src/test_main.c` uses `debug_init_isviewer()` instead of `debug_init_usblog()`.
  Output then comes out over ISViewer (`0x13FF0000`), which kestrel implements
  including the **magic readback** libdragon's `isviewer_init()` probes for — without
  that probe answering, the ROM decides no debug channel exists and prints nothing.
- `TOTAL_RUNS` is overridable (`#ifndef`) and the Makefile forwards `EXTRA_CFLAGS`.
  Upstream repeats every configuration 1000 times to average out RDRAM refresh and
  VI jitter on hardware. kestrel's cost model is deterministic — every run returns
  the identical number — so those repeats are pure wall time. 8 is enough.

Then run it and score it:

```bash
./build/kestrel64.exe ../rdp-timing-tests/rdp_fill_timing.z64 --run > out/rdptiming.txt
python scripts/rdptiming.py compare out/rdptiming.txt
```

`compare` parses our raw ROM output, matches each configuration to the hardware
reference by *feature tuple* (cycle count, fb read, z read/write/pass, alpha fail,
VI on, fb/zb and fb/vi bank sharing) rather than by line order, and reports
rmse/mae/worst in cycles per pixel. Takes ~97 s.

## Current agreement

```
n=100/100 matched  rmse=0.1332  mae=0.1137  max=0.2992 cyc/px
```

`sh scripts/gate_quick.sh thar0` runs it against `build-static/` and fails unless rmse is
exactly 0.1332.

2026-09-29: the cost-only walk (`SoftRdp::costOnly`, added in b91848b, which runs the FIFO
span before the painting walk and pays the cost) had silently taken this to
**rmse 0.988**: its FILL_RECTANGLE shortcut charged every pixel as written, so the
"Z Fail" configurations (prim depth, Z_CMP fails) paid color+depth writes they never do
on hardware (+2.7 cyc/px), and "Alpha Compare" paid writes for a rect whose constant alpha
fails the compare (+0.74). The fix evaluates both in the cost walk: the rect's colour and
depth are constants, so alpha compare is one evaluation and the depth test is a read-only
compare against the z image per pixel (`depthPasses`). Triangles in the cost walk still
assume every rasterized pixel passes (approximation; exact would need the full depth
interpolation, O(area)). The fixed SYNC_LOAD/PIPE/TILE stalls (PD64_pending P1) leave
Thar0 unchanged: the test lists carry no syncs inside the measured window.

0.133 rmse is the *same residual the fitted model itself has* against the hardware
numbers — i.e. the emulator now reproduces the model as well as the model reproduces
hardware, and closing the rest means a better model, not a better implementation.

Model shape (fitted to the 100 HW configurations): RDRAM is worked in chunks of
~8 RGBA16 pixels; per chunk the transactions are issued in the order color read,
depth read, color write, depth write; a chunk costs `max(pipeline, memory)`.
Constants `LAT=3.583 XFER=6.677 ROW=1.905 VI=0.088 VIROW=0.595 CHUNKOVH=1.606`.

## The bug this battery found: the depth domain is 18-bit, not 15-bit

Starting rmse was **0.948**, and essentially all of the error sat in the
"ZB Read/Write, Z Pass" configurations — the ones where the depth test must *pass*.
It did not pass in kestrel, so those runs never paid for the depth write.

Root cause: `SET_PRIM_DEPTH` was storing the command's bare 15-bit Z field. Hardware
loads that field into the same s15.16 attribute the triangle Z interpolator produces,
and the depth unit then consumes **bits 31:13** of it — an 18-bit value with 3
fractional bits. parallel-rdp, as the oracle:

- `rdp_renderer.cpp:3510` — `constants.prim_depth = int32_t(prim_depth & 0x7fff) << 16;`
- `shaders/interpolation.h` — `snapped_z = z >> 10; snapped_z <<= 2; ...; snapped_z >>= 5;`
  (net `>> 13`)

Those 3 bits are not decoration. The z-buffer's stored format is a 3-bit exponent +
11-bit mantissa; near `z = 0x7FFF` it steps by 1 in the 18-bit domain but by **64** in
the 15-bit one. So a test that walks prim depth by 1 quantized every step to the same
stored value in kestrel, and every compare after the first failed.

Fix (`src/rdp/rdp.cpp`): `prim_z = ((cmd >> 16) & 0x7fff) << 3`, and the triangle Z
coefficients scaled by `/ 8192.0` (>>13) instead of `/ 65536.0` (>>16). Generalizable
RDP semantics, not a test-shaped patch — it also gained five krom z-buffer tests:

| krom test | before | after |
|---|---|---|
| `Cycle1FillZBufferRectangle16BPP320X240` | 96.15 | 98.60 |
| `Cycle1FillZBufferRectangle32BPP320X240` | 96.15 | 98.60 |
| `RSPTest/XBUS/RSPXBUSRDP` | 96.15 | 98.60 |
| `Cycle1FillZBufferTriangle16BPP320X240` | 98.10 | 98.15 |
| `Cycle1FillZBufferTriangle32BPP320X240` | 98.10 | 98.15 |

## Known gap: `RDPTest/CPU` and `RDPTest/RSP` print counter digits (99.65)

krom's `RDPTest` ROMs print the DPC registers on screen as text and compare the
screen against a reference PNG. Their RDP work is a FILL-cycle full-screen rectangle
plus a small one — **no z-buffer, no prim depth** — so the depth fix cannot be what
moved them; what moved them is the cost model now producing non-zero counters where
kestrel previously reported zero.

kestrel prints `CLOCK = BUFBUSY = PIPEBUSY = $000144E9`, `TMEM = $00000000`.
The reference PNG shows all four as `$00000000`.

Two open discrepancies behind that, both real modelling gaps, neither worth
hardcoding away:

1. **Counters are not zero on hardware.** Thar0's dataset is measured on a real N64
   and returns thousands of GCLK ticks for comparable work, and libultra's own
   profiler reads `DPC_CLOCK` without ever writing `DPC_STATUS`. The difference
   between the two ROMs is that Thar0 clears the counters (`DPC_CLR_*_CTR`) before
   each run and krom never touches `DPC_STATUS`. A reference of exactly zero after a
   full-screen fill is not consistent with counters that free-run, so the PNG is
   suspect — likely captured from an emulator that stubs them.
2. **Our three counters are identical, and FILL-cycle cost is unvalidated.** On
   hardware CLOCK, BUFBUSY and PIPEBUSY diverge. And the fitted model was calibrated
   entirely on 1-/2-cycle RGBA16 fills; the FILL cycle type writes 64 bits per clock,
   which the model does not represent, so `0x144E9` for 320x240x32bpp is likely ~2x
   too slow.

Update 2026-09-30 (PD64_pending P7): `DPC_CLOCK` now free-runs at the RCP clock from
boot and is never stopped, not even by FREEZE (n64brew, "Reality Display
Processor/Interface"); a write of DPC_STATUS bit 9 rebases it. Only BUF/PIPE/TMEM still
accumulate modelled RDP work. Thar0's hardware data has Buf and Pipe equal within noise
(-3..40 GCLK over ~80 k) for its single-fillrect case, so their equality is right there;
they only diverge across FIFO gaps before SYNC_FULL, which is not modelled.

Accepted for now and tracked here. The fix is separate per-counter accounting plus a
FILL-cycle path in the cost model — and it needs its own hardware oracle, because
Thar0's sweep never enters FILL mode.

## FILL/COPY path and per-line cost (2026-10-02)

Requested by the kestrel64-sdk session (bench `demos/09_rdp_bench`). Before this,
FILL/COPY went through the same chunk/memory model as 1-cycle and came out
memory-bound at ~0.97 clk/px, only ~20 % faster than 1-cycle. There was also no
per-line cost, so 1200 small triangles cost the same as 2 big ones.

There is still no hardware measurement of FILL/COPY. The new shape comes only from
documented hardware behaviour, with no new constant fitted:

- **FILL/COPY bypass the span buffer.** n64brew "Reality Display Processor/Pipeline"
  says: "Writes are committed straight to RDRAM without passing through the span
  buffers", "Pixels are written out 64-bits (8 8-bit pixels, 4 16-bit pixels,
  2 32-bit pixels) at a time". The N64 programming manual agrees: four 16-bit or two
  32-bit pixels per cycle.
  - The model charges one GCLK per *aligned* 64-bit word that the line touches.
  - That cost is scaled by the existing calibrated VI contention `T_VI`.
  - No chunk overhead and no read latency are added.
- **One dead cycle per line** (n64brew: "1 dead cycle at the end of every line in a
  primitive where the pipeline is cycled but no pixel is output"). Every line of every
  primitive pays it, in all cycle types.
- **Chunks never span two lines.** In 1/2-cycle mode each line costs `ceil(px/8)`
  span-buffer chunks, so a 2-pixel line pays a whole chunk.

Results (09_rdp_bench, 320x240 RGBA16, VI on):

| test | before | after |
|---|---:|---:|
| fill rect | 75 451 | 21 150 |
| copy 1 wrapped rect | 74 902 | 21 062 |
| 1-cycle rect | 92 217 | 92 457 |
| 1-cycle 2 tris per 8x8 cell | 91 200 | 140 400 |
| prim canvas 2x2 | 191 160 | 440 320 |

- FILL/COPY now run about 4.4x faster than 1-cycle, which matches libdragon's
  "approximately 4 times faster".
- Thar0 is unchanged at rmse 0.1332: all its rects are 320 wide, so chunk rounding does
  nothing and only the 240 dead cycles per rect remain. Max error moves 0.2992 -> 0.2967.

Still not modelled: a per-primitive setup cost (edge walker / command fetch). No source
gives a number for it, so none is invented. The command words themselves still cost
nothing beyond the FIFO.
