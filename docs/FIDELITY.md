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
