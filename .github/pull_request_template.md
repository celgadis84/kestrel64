## What this changes

<!-- What behaviour changes, and why. For an accuracy fix, name the hardware oracle that
     says the old behaviour was wrong (doc section, RTL file, test ROM). -->

## Validation

- [ ] `sh scripts/gate_all.sh` passes
- [ ] `sh scripts/gate_prdp.sh` passes
- [ ] lockstep (`KESTREL_THREADS=0`) and threaded agree: same state hash, same framebuffer md5
- [ ] gate wall-clock recorded in `docs/baselines/timings.md`

<!-- If you skipped one, say which and why. An honest gap is fine; a silent one is not. -->

## If this is a performance change

<!-- Wall clock here is worth about ±2 %, so paste interleaved A/B numbers: minimum of at
     least three runs per game, first run of a freshly linked binary discarded. Say which
     games got worse as well as which got better. -->

## Confirmations

- [ ] No test, ROM, checksum or frame number is special-cased to make a suite pass
- [ ] No guest-visible state depends on wall-clock time or thread arrival order
- [ ] No ROM images or other copyrighted game data are included
