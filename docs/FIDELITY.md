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
