# Fidelity score (community HW test suites)

`scripts/fidelity.py` runs krom / Dillon / n64-systemtest / libdragon on kestrel64 and on
ares and writes `out/fidelity.md`. It is NOT a gate: run on demand. `--rdp gpu|soft`
picks kestrel's RDP backend (default `gpu`; the shipped exe starts on GPU-RDP, so the backend is always fixed explicitly). The sections
below are dated 2026-10 and compare against parallel-rdp as the reference of that time.

## libdragon `testrom_emu.z64` on parallel-rdp (2026-10-08)

224 tests, 5 fail. All 5 fail identically on ares and are test/library-side, not emulator
bugs (ares additionally fails `test_timer_ticks`).

| test | why it fails (same on HW) |
|------|---------------------------|
| `test_cache_invalidate` | `dma_read` from `DFS_DEFAULT_LOCATION`, which is 0 in this libdragon; the testrom has no DFS there (PI 0 = ROM header). Stale test. |
| `test_mpeg1_block_predict` | the test calls `rsp_mpeg1_block_switch_partition(0..3)` + store (the `block_split` call is commented out), and `cmd_mpeg1_block_switch` sets `DMA_SIZE(16,16)` for every Y partition (the `li` sits in the `ble` delay slot). Partitions 1-3 store 16x16 from `addr+8` / `addr+8*pitch`, so they overwrite pixels outside the 16x16 block, e.g. x = dx+16. |
| `test_mg_draw_triangle_strip_full_cache`, `test_mg_draw_triangle_fan_full_cache`, `test_mg_draw_indexed_strip_full`, `test_mg_draw_indexed_fan_full` | the expected command stream has word 48 as a magma command, but rspq has to put an `RSPQ_CMD_JUMP` (0x02xxxxxx) there to the next block chunk. Only the jump target address differs between emulators. |

Fixed in kestrel while getting here (commit 71a6909):

- `test_rdpq_rspqwait`: `rdpq_fence` = SYNC_FULL + the RSP spinning on DPC_STATUS, then the
  CPU reads the framebuffer. The RSP answers DPC_STATUS from the guest-time schedule, so
  "idle" could be seen before the host RDP worker (and the parallel-rdp GPU fence) had
  written the pixels. `Memory::rspRdpIdleWait` waits for the host drain when the microcode
  reads DPC_STATUS idle; host time only, guest unchanged.
- `test_mpeg1_block_predict` (first failure): the SP DMA engine only keeps RDRAM address
  bits 23:3, so the low 3 bits of the skip field are lost (ares `skip.bit(3,11)`). libdragon
  `DMAExec` writes skip = pitch - width unrounded (17-byte rows of a 128 pitch); kestrel
  added the raw skip and every row after the first came from the wrong address.

## libdragon on SoftRDP and GPU-RDP (2026-10-08)

Both backends now give the same result as parallel-rdp: 211/217 (7 skipped), and the only
failures are the 6 test-side ones above. They were at 209/217. What was missing:

- **Tile origin in sampling.** The hardware subtracts SL/TL (10.2) from S/T after the tile
  SHIFT, in 10.5 fixed point (parallel-rdp `shift_coord`). SoftRDP and `tri.comp` never did,
  so any tile with SL/TL != 0 (texture scroll, `rdpq_tex_upload_sub`) sampled shifted. The
  SHIFT now also acts on the fixed-point value, so with a left shift the fraction moves into
  the integer part. krom `TexturesMaskShiftMirror`: 16 ROMs improve, 0 regress.
- **LOAD_BLOCK source row.** SL/TL of LOAD_BLOCK are whole texels, and the RDRAM source is
  `tex_addr + (ti_width*TL + SL) * bpp`. TL was ignored.
- **IA4 intensity.** The 3 bits expand by bit replication (`I<<5|I<<2|I>>1`, 6 -> 0xDB), not
  `*255/7`.
- **RGBA32 split TMEM.** RG in the low half and BA at +0x800 (load and sample).
- **Scissor and rect edges.** The high scissor bound is exclusive in 1/2-cycle (`(x+3)>>2`)
  and `(x>>2)+1` in FILL/COPY. FILL/COPY rects have inclusive right/bottom edges.

## Pipelined TEXEL1 in 1-cycle mode (2026-10-08)

In 1-cycle mode the combiner's TEXEL1 is not a second sample: the hardware hands over the TEXEL0 of the
*next* pixel along the span (parallel-rdp `combiner_uses_pipelined_texel1`, shading.h). On the last pixel
of a span with lodlength >= 8 and a valid next line (some sub-scanline alive, not entirely outside the
scissor), S/T/W come from the start of the next line instead. SoftRDP and GPU-RDP now do the same
(`pipeTex1` in `drawTriangle`, flag `TF_PIPE1` in `tri.comp`).

krom `RDPTex0And1`: 98.83 -> 99.42 on both backends, framebuffer identical to parallel-rdp (1792 -> 0
differing pixels). RDP/ subset: 0 regressions on interp and gpurdp; SM64 md5 unchanged on both.

## FILL triangles cover the whole span (2026-10-08)

`FillTriangle 16/32BPP`: 98.89 -> 98.94 on SoftRDP and GPU-RDP, framebuffer identical to
parallel-rdp pixel for pixel. `RDP/` subset 133: 0 regress over the gate threshold. SM64 md5
unchanged. `FillLine 16/32BPP` goes 99.26 -> 99.18 against the krom PNG, but its framebuffer is now
also identical to parallel-rdp (which scores 99.18): the extra edge pixels are what parallel-rdp draws.

- In FILL cycle the RDP does not look at coverage: every pixel between the span's start and
  end (`min(xleft) >> 3` .. `max(xright) >> 3`, both inclusive) is written, like COPY
  (parallel-rdp `shading.h`, `fill_en`). We required sample 0 to be covered, so the right edge lost
  one pixel per row on slanted edges.

## YUV textures (2026-10-08)

`Cycle1TextureRectangle/Triangle 16/32BPP YUV16B`: 97.37/96.98/98.92/98.49 -> 99.74/99.74/99.73/99.73
on SoftRDP and GPU-RDP (= parallel-rdp; rect16 framebuffer identical to parallel-rdp pixel for pixel).
`RDP/` subset: 133 ROMs, 0 regress, both backends identical. SM64 md5 unchanged on both.

- Load: a YUV 16bpp load uploads each pixel pair U Y0 V Y1 as one 32bpp texel, U,V in the low
  TMEM half and Y0,Y1 in the high half (parallel-rdp `update_tmem_32` with yuv); LOAD_TILE width
  rounded up to 4 pixels.
- Sample: luma from the high half at pixel s, chroma from the low half at column s>>1. The
  filter takes chroma at columns `s0>>1` and `(2*s1-s0)>>1` with its own fraction
  `(s0&1)<<4 | fx>>1`; luma uses the normal one (parallel-rdp `sample_texture`).
- Convert (TX): without BI_LERP for the cycle the texel is converted with factors `2K+1`,
  `>>8` with `+0x80` rounding, U/V signed, alpha = Y; with BI_LERP it passes raw.
  CONVERT_ONE without BI_LERP_1 makes TEXEL1 = convert(TEXEL0).
- The converted value is 9-bit; with 8-bit texels it is cut with the combiner's 9-bit clamp
  ([-256,-129] -> 255). Exact when the combiner passes the texel through; approximate when it
  is a multiplier.
- GPU-RDP: same in `tri.comp`. The bigger shader costs ~45 s of AMD driver pipeline compile on
  the first run after a change (cached afterwards).

## TEXTURE_RECTANGLE as a flat triangle (2026-10-08)

`TextureCoordinates1b` 96.67, `TextureCoordinates2` 95.29, `TextureCoordinates2b` 97.35 -> 100
on SoftRDP and GPU-RDP (= parallel-rdp). `RDP/` subset: 133 ROMs, 0 regress, both backends
identical on all 133. SM64 md5 unchanged on both.

- The rect is built as a triangle (parallel-rdp `op_texture_rectangle`): major edge at XH,
  XM = XL, YM = YL, YL |= 3 in COPY/FILL; S/T = s10.5 << 16, DsDx/DtDy << 11 along +x/+y
  (axes crossed with FLIP). SoftRDP feeds it to `drawTriangle`, so the rect inherits the
  integer row anchor, the XH fraction discount (none in COPY), coverage (AA-on rects with a
  fractional edge get partial coverage) and COPY's 4-pixel word grouping.
  No perspective on rects (parallel-rdp does apply it; W = 0 here).
- COPY to an 8-bit framebuffer (raw indices) keeps the old loop.
- GPU-RDP: 1/2-cycle rects go through `gpuTriangle`; COPY 16bpp rects keep the rect record
  with the triangle's box (columns [XH>>2, XL>>2], rows with a sub-scanline in [YH, YL|3))
  and group stepping in the shader (`copyRectPixel`).

## krom coverage/combiner on SoftRDP and GPU-RDP (2026-10-08)

`AlphaCoverage` 74.82 -> 100, `CombinerOverflow` 45.41 -> 100, `CombinerLongTailConstants`
74.31 -> 100, on both backends (same as parallel-rdp). `RDP/` subset: 133 ROMs, 0 regress.

- **Rect depth.** TEXTURE_RECTANGLE and 1/2-cycle FILL_RECTANGLE are primitives with zeroed
  attributes (parallel-rdp `op_texture_rectangle` / `op_fill_rectangle`): span z = 0, or
  SET_PRIM_DEPTH with Z_SOURCE_SEL, and Z_COMPARE / Z_UPDATE apply as for any primitive.
  The texrect never touched the z-buffer, and the fill rect only did with Z_SOURCE_SEL.
- **Combiner constants.** sub_b 6 = KEY_CENTER, sub_b 7 = K4 (9 raw bits through
  special_expand), mul 6 = KEY_SCALE, mul 15 = K5 (9 bits sign-extended). They were 0.
  SET_KEY_R / SET_KEY_GB are now stored (savestate v21).

## krom texture clamp/mask on SoftRDP and GPU-RDP (2026-10-08)

`TexturesMaskShiftMirror` T0_3/T0_4/T15_3/T15_4/T1_3/T1_4 (84.7-91.7) -> 100 and
`TextureCoordinates` 99.46 -> 100, on both backends. `RDP/` subset: 0 regress, SoftRDP and
GPU-RDP give identical scores on all 133. SM64 md5 unchanged.

- **Clamp stage** (parallel-rdp `clamp_and_shift_coord`). After the SHIFT the high edge is
  compared in 10.2 against the ABSOLUTE SH/TH (before subtracting SL/TL) and pins the texel
  `((SH>>2) - (SL>>2)) & 0x3ff` with zero fraction; otherwise the origin is subtracted and
  negatives go to 0. It only applies to the sampled coordinate: the filter neighbour
  (s0 + 1, t0 + 1) goes through the mask alone. MASK > 10 acts as 10.
- **T to 8 bits** in 1/2-cycle sampling, with the filter neighbour kept as a difference
  clamped to -255.
- **COPY** has no clamp stage, no T truncation and no filter (`sample_texture_copy`), even
  with SAMPLE_TYPE set.
- GPU tile record: words 1/2 carry `sMax/tMax | SH/TH << 16`.

## LOAD_TLUT writes real TMEM (2026-10-08)

11 `LoadTLUT` ROMs (95.7-99.0) -> 100 on SoftRDP and GPU-RDP; now equal to parallel-rdp on
all 14. `RDP/` subset: 0 regress, both backends identical on all 133. SM64 md5 unchanged.

- The palette is no longer a separate `tlut[256]` array: it lives in the upper half of TMEM,
  one 64-bit word per entry with the value in all four banks, and CI sampling reads
  `0x800 + idx * 8` (parallel-rdp `sample_texel_ci*_tlut`, bank 0).
- LOAD_TLUT follows parallel-rdp `update_tmem_lut`: starts at the destination tile's TMEM
  address, wraps across the 4 KB, and the image size vs tile size decides which 16-bit TMEM
  words get written and from which texel (16b->4b tile = contiguous quadrupled entries;
  same size = first 8 bytes of every 32; etc.). A LOAD_BLOCK/TILE into the upper half now
  overwrites the palette, as on HW.
- Not modelled (parallel-rdp neither): 4-bit image, 32-bit tile (`*_b32` stay at parallel-rdp's
  score), height > 1.
- GPU-RDP TMEM slot shrinks to 4 KB (palette included). Savestate v22; v14-v21 states are
  converted on load.

## TLUT sampling: color-index by tile size and per-tap palette banks (2026-10-08)

`TEXRECT_Palette` 88.07 -> 100 on SoftRDP and GPU-RDP (= parallel-rdp). `RDP/` subset:
0 regress on both backends, identical scores. SM64 md5 unchanged.

- With EN_TLUT the TX treats RGBA/CI/IA/I as a colour index chosen by tile SIZE
  (parallel-rdp `sample_texture`): 4b = nibble | PALETTE << 4, 8b = the byte, 16b/32b = the
  high byte of the 16-bit texel (new kind `TK_CI16`). Index reads wrap at 2 KB (& 0x7ff).
  YUV and formats 5..7 keep their own path.
- Each filter tap reads its own palette bank: base 0, the two neighbours 1 and 2 (swapped
  past the diagonal), MID_TEXEL 3/2/1/0. Irrelevant when LOAD_TLUT quadrupled the entries,
  visible when the palette came in through LOAD_BLOCK.
- TLUT + BI_LERP of the cycle without SAMPLE_TYPE still runs the 3-tap filter, with all taps
  on the same texel (s0, t0) and only the bank changing ("bilinear footprint with the 4
  banks of TLUT"). BI_LERP_0 for cycle 0, BI_LERP_1 for TEXEL1.
- COPY triangles at 16 bpp follow parallel-rdp `interpolate_st_copy` (SoftRDP; GPU-RDP
  sends COPY triangles to the CPU): a pixel is drawn when it lies in the span
  [start_x, end_x], coverage is not checked, S/T/W are stepped once per 64-bit word
  (4 pixels) counting from the span's start edge, and pixel `s_offset` of the group reads
  texel s + s_offset and, with TLUT, palette bank s_offset. `CopyTextureTriangle*` (RGBA16,
  TLUT RGBA4/RGBA8) 96.86/99.06 -> 99.40/99.54, pixel-identical to parallel-rdp.
- Not yet: the same grouping for COPY texture rectangles (they score the same as
  parallel-rdp already at 1:1 scale), nor COPY at 8/32 bpp.
