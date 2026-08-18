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

Accepted for now and tracked here. The fix is separate per-counter accounting plus a
FILL-cycle path in the cost model — and it needs its own hardware oracle, because
Thar0's sweep never enters FILL mode.
