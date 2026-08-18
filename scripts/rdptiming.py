#!/usr/bin/env python3
"""
Parse Thar0's RDP-Timing-Tests sample_results.txt (real N64 hardware, EverDrive X7)
into a machine-readable table, and score a candidate RDP cost model against it.

Every test is the same workload: one 320x240 fill rectangle into an RGBA16 color
image (76800 pixels), timed with the RDP's own PIPEBUSY/BUFBUSY counters, so the
numbers are GCLK (62.5 MHz) cycles, not host time.  What varies is the render
mode and where the buffers live in RDRAM.

Usage:
  rdptiming.py table                 # dump parsed hardware table (TSV)
  rdptiming.py fit                   # fit the cost model, print coefficients
  rdptiming.py check                 # score the model coded in model_cycles()
  rdptiming.py compare results.txt   # score kestrel's own run against hardware
"""
import re
import sys

PIXELS = 320 * 240
GCLK = 62500.0  # counter ticks per millisecond


# --- parsing ---------------------------------------------------------------
# Section headers name the render mode; the nested headers name the RDRAM bank
# layout, then image_read/z_compare, then the cycle type.
def parse(path):
    rows = []
    sect = sub = mode = None
    cyc = None
    with open(path) as f:
        lines = f.read().split("\n")
    # skip the prose preamble
    start = next(i for i, l in enumerate(lines) if l.startswith("No ZB, No VI"))
    for line in lines[start:]:
        if not line.strip():
            continue
        indent = len(line) - len(line.lstrip())
        t = line.strip()
        if indent == 0:
            sect, sub, mode = t, "", ""
        elif t.startswith(("image_read", "z_compare")):
            mode = t          # render-mode line (nesting depth varies by section)
        elif t.startswith(("FB ", "ZB ")):
            sub = t           # RDRAM bank layout line
        elif t.endswith("-cycle"):
            cyc = int(t[0])
        elif t.startswith(("Buf:", "Pipe:")):
            which = "buf" if t.startswith("Buf") else "pipe"
            vals = [float(v.strip().rstrip("ms")) for v in t.split(":", 1)[1].split(",")]
            if which == "buf":
                rows.append(dict(sect=sect, sub=sub, mode=mode, cyc=cyc, buf=vals[1]))
            else:
                rows[-1]["pipe"] = vals[1]
    for r in rows:
        r.update(features(r))
        r["cpp"] = r["buf"] * GCLK / PIXELS  # measured cycles per pixel
    return rows


# The ROM itself prints one flat description line per configuration followed by the
# raw BUFBUSY/PIPEBUSY arrays; sample_results.txt is that output already grouped and
# averaged by the repo's analyze.py. Reading the raw form lets an emulator run be
# scored without going through numpy/matplotlib, and keeps the two paths comparable:
# both end up as the same feature dict plus a cycles-per-pixel number.
def parse_raw(path):
    with open(path) as f:
        body = f.read()
    body = body.split("!!BEGIN!!", 1)[1].split("!!DONE!!", 1)[0]
    rows, desc, vals = [], None, None
    for line in body.split("\n"):
        t = line.strip()
        if not t:
            continue
        if t in ("]",):
            continue
        if t.startswith(("BUF = [", "PIPE = [")):
            vals = "buf" if t.startswith("BUF") else "pipe"
            continue
        if vals and t[0].isdigit():
            nums = [int(v) for v in t.rstrip(", ").split(", ")]
            nums.sort()
            med = nums[len(nums) // 2]
            if vals == "buf":
                rows.append(dict(desc=desc, buf=med))
            else:
                rows[-1]["pipe"] = med
            vals = None
            continue
        desc = t
    for r in rows:
        r.update(split_desc(r["desc"]))
        r.update(features(r))
        r["cpp"] = r["buf"] / PIXELS       # counter already counts GCLK ticks
    return rows


def split_desc(desc):
    """Flat ROM description → the section / bank-layout / render-mode split that
    features() expects. e.g. "ZB Read/Write, VI, Z Pass, FB + ZB same, VI separate,
    image_read on,  2-cycle"."""
    parts = [p.strip() for p in desc.split(",")]
    sect, sub, mode, cyc = [], [], [], 1
    for p in parts:
        if p.endswith("-cycle"):
            cyc = int(p[0])
        elif p.startswith(("image_read", "z_compare")):
            mode.append(p)
        elif p.startswith(("FB ", "ZB +", "VI ")) or p in ("separate", "same"):
            sub.append(p)
        else:
            sect.append(p)
    return dict(sect=", ".join(sect), sub=", ".join(sub), mode=" ".join(mode), cyc=cyc)


def features(r):
    """Decode a row's section/subsection text into the render-mode flags."""
    s, sub, mode = r["sect"], r["sub"], r["mode"]
    f = dict(
        fbread="image_read on" in mode,
        zread=False,
        zwrite=False,
        zpass=True,
        alphafail="Alpha Compare" in s,
        vi="VI" in s and "No VI" not in s,
        fbzb_same=False,
        fbvi_same=False,
    )
    if "ZB Read-Only" in s:
        f["zread"] = True
    elif "ZB Write-Only" in s:
        f["zwrite"] = True
    elif "ZB Read/Write" in s:
        f["zread"] = f["zwrite"] = True
    if "Z Fail" in s:
        f["zpass"] = False
    if f["alphafail"]:
        f["zread"] = "z_compare on" in mode
    # bank layout: "FB + ZB same", "FB + VI same, ZB separate", "FB + ZB + VI same", ...
    if "FB + ZB + VI same" in sub:
        f["fbzb_same"] = f["fbvi_same"] = True
    else:
        f["fbzb_same"] = "FB + ZB same" in sub
        f["fbvi_same"] = "FB + VI same" in sub
    return f


# --- the model -------------------------------------------------------------
# Physical picture (matches the test author's own notes and the RDRAM datasheet):
#
#  * The RDP rasterizes into a span buffer holding ~8 RGBA16 pixels (16 bytes of
#    color, 16 bytes of depth) and then runs the RDRAM transactions for that whole
#    chunk, in the order color read, depth read, color write, depth write.
#  * The pipeline itself costs 1 GCLK per pixel in 1-cycle mode and 2 in 2-cycle
#    mode, so 8 or 16 GCLK per chunk, plus a small fixed per-span overhead.
#  * Memory and pipeline overlap: a chunk costs max(pipeline, memory), which is
#    why 2-cycle mode "catches up" to 1-cycle mode as soon as RDRAM is involved.
#  * A read stalls the span until the data arrives, so the first read of a chunk
#    pays the RDRAM latency (LAT); further transactions only pay bus occupancy
#    (XFER each).  Writes are posted, so with no read in the chunk they drain
#    into the pipeline's shadow for free.
#  * Two buffers in the same 1 MB RDRAM bank fight over the single open row
#    (rows are 0x800 bytes), so each extra transaction into a bank that another
#    buffer just touched pays a row open (ROW).
#  * The VI reads the framebuffer continuously at a higher priority than the RDP,
#    stealing bus cycles (VI); if it shares a bank with a buffer the RDP uses, it
#    also steals the open row (VIROW).
CHUNK = 8  # pixels buffered per span-buffer flush

# Calibrated against the 100 hardware configurations in sample_results.txt
# (scripts/rdptiming.py fit): rmse 0.133, mae 0.114, worst 0.30 cycles/pixel.
COEF = dict(LAT=3.583, XFER=6.677, ROW=1.905, VI=0.088, VIROW=0.595, SPAN=1.606)


def transactions(f):
    """The chunk's RDRAM transactions, in hardware order, as buffer ids."""
    kill = f["alphafail"] or not f["zpass"]   # a killed pixel writes nothing
    t = []
    if f["fbread"]:
        t.append("fb")
    if f["zread"]:
        t.append("zb")
    if not kill:
        t.append("fb")
        if f["zwrite"]:
            t.append("zb")
    return t


def model_cycles(f, c=COEF):
    """Cycles per pixel for one render-mode configuration."""
    trans = transactions(f)
    pipeline = f["cyc"] * CHUNK + c["SPAN"]
    mem = 0.0
    if trans:
        # Each transaction occupies the bus for XFER.  A read additionally stalls
        # the span until the data returns, once per chunk (LAT).
        mem = c["XFER"] * len(trans)
        if f["fbread"] or f["zread"]:
            mem += c["LAT"]
        # RDRAM keeps one open row per 1 MB bank.  When the framebuffer and the
        # z-buffer live in the same bank their rows are far apart, so every
        # alternation between them closes and reopens a row — including the wrap
        # from the previous chunk's last transaction.
        if f["fbzb_same"]:
            seq = trans[-1:] + trans
            mem += c["ROW"] * sum(a != b for a, b in zip(seq, seq[1:]))
        if f["vi"]:
            # The VI reads the framebuffer continuously and outranks the RDP on
            # the bus, so it scales every RDP transaction; sharing a bank with it
            # also costs the open row.
            mem *= 1.0 + c["VI"]
            if f["fbvi_same"]:
                mem += c["VIROW"] * sum(t == "fb" for t in trans)
    return max(pipeline, mem) / CHUNK


# --- fitting ---------------------------------------------------------------
def fit(rows):
    import itertools
    import random

    keys = list(COEF)
    best = dict(COEF)
    bs = score(rows, best)[0]
    step = {k: 0.4 for k in keys}
    rnd = random.Random(0)
    for _ in range(4000):
        cand = dict(best)
        k = rnd.choice(keys)
        cand[k] = max(0.0, cand[k] + rnd.uniform(-1, 1) * step[k])
        s = score(rows, cand)[0]
        if s < bs:
            best, bs = cand, s
        else:
            step[k] *= 0.999
    return best, bs


def score(rows, c=COEF):
    errs = [abs(model_cycles(r, c) - r["cpp"]) for r in rows]
    return sum(e * e for e in errs) / len(errs), max(errs), sum(errs) / len(errs)


def main():
    import os

    here = os.path.dirname(os.path.abspath(__file__))
    default = os.path.join(here, "..", "..", "rdp-timing-tests", "sample_results.txt")
    cmd = sys.argv[1] if len(sys.argv) > 1 else "check"
    if cmd == "compare":
        # Score a kestrel run (raw ROM output) against the hardware reference. Rows
        # are matched on the render-mode features, not on text, so the two file
        # formats line up even though their descriptions are worded differently.
        FEAT = ("cyc", "fbread", "zread", "zwrite", "zpass", "alphafail", "vi",
                "fbzb_same", "fbvi_same")
        key = lambda r: tuple(r[k] for k in FEAT)
        hw = {key(r): r for r in parse(default)}
        ours = parse_raw(sys.argv[2])
        pairs = [(r, hw[key(r)]) for r in ours if key(r) in hw]
        errs = [(abs(o["cpp"] - h["cpp"]), o, h) for o, h in pairs]
        mse = sum(e * e for e, _, _ in errs) / len(errs)
        print(f"n={len(errs)}/{len(ours)} matched  rmse={mse ** .5:.4f} "
              f"mae={sum(e for e, _, _ in errs) / len(errs):.4f} "
              f"max={max(e for e, _, _ in errs):.4f} cyc/px")
        errs.sort(key=lambda t: -t[0])
        for e, o, h in errs[:15]:
            print(f"  {h['cpp']:6.3f} hw  {o['cpp']:6.3f} kestrel  ({e:+.3f})  {o['desc']}")
        return
    rows = parse(sys.argv[2] if len(sys.argv) > 2 else default)
    if cmd == "table":
        cols = ["cyc", "fbread", "zread", "zwrite", "zpass", "alphafail", "vi",
                "fbzb_same", "fbvi_same", "cpp"]
        print("\t".join(cols) + "\tdesc")
        for r in rows:
            print("\t".join(f"{r[k]:.4f}" if isinstance(r[k], float) else str(int(r[k]))
                            for k in cols) + f"\t{r['sect']} | {r['sub']} | {r['mode']}")
    elif cmd == "fit":
        c, s = fit(rows)
        print("coef:", " ".join(f"{k}={v:.3f}" for k, v in c.items()))
        mse, mx, mae = score(rows, c)
        print(f"n={len(rows)} rmse={mse ** .5:.4f} mae={mae:.4f} max={mx:.4f} cyc/px")
    else:
        mse, mx, mae = score(rows)
        print(f"n={len(rows)} rmse={mse ** .5:.4f} mae={mae:.4f} max={mx:.4f} cyc/px")
        rows.sort(key=lambda r: -abs(model_cycles(r) - r["cpp"]))
        for r in rows[:12]:
            print(f"  {r['cpp']:.3f} hw  {model_cycles(r):.3f} model  "
                  f"{r['sect']} | {r['sub']} | {r['mode']} | {r['cyc']}-cycle")


if __name__ == "__main__":
    main()
