# Fidelity score (community HW test suites)

`scripts/fidelity.py` runs krom / Dillon / n64-systemtest / libdragon on kestrel64 and on
ares and writes `out/fidelity.md`. It is NOT a gate: run on demand. `--rdp prdp|gpu|soft`
picks kestrel's RDP backend (default `prdp`, the reference while GPU-RDP and SoftRDP are
incomplete; the shipped exe starts on GPU-RDP, so the backend is always fixed explicitly).

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
- Not modelled (parallel-rdp neither): 4-bit image, 32-bit tile (`*_b32` stay at prdp's
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
