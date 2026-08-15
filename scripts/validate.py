#!/usr/bin/env python3
"""kestrel64 validation harness.

One entry point for the three gates that must pass after EVERY change:

    python scripts/validate.py systemtest     # n64-systemtest: HW correctness
    python scripts/validate.py krom           # PeterLemon suite: RDP/RSP accuracy vs PNG refs
    python scripts/validate.py sm64           # SM64 framebuffer md5: end-to-end determinism
    python scripts/validate.py all            # the three, in that order, stop on first failure

Design goal is *terse output*. Every gate prints a handful of lines; the per-ROM
detail lands in a report file under out/. A run is compared against a stored
baseline (docs/baselines/*.tsv), so a clean run says "0 regressions" instead of
dumping 371 rows that nobody reads.

Execution modes (--mode) map to the emulator's env toggles, so the same battery
can be replayed against the interpreter, the JIT, block-linking, and the
threaded RCP without editing anything:

    interp        (oracle: no JIT, lockstep RCP)
    jit           KESTREL_JIT=1
    link          KESTREL_JIT=1 KESTREL_JIT_LINK=1
    threaded      KESTREL_THREADS=1
    threaded-jit  KESTREL_THREADS=1 KESTREL_JIT=1
    threaded-link KESTREL_THREADS=1 KESTREL_JIT=1 KESTREL_JIT_LINK=1

GOTCHA baked in: SM64 writes its EEPROM (.eep) next to the ROM. Left over from a
previous run it changes the boot path and therefore the md5, with no emulator
change involved. Every SM64 run deletes the save first.
"""

import argparse
import concurrent.futures as futures
import hashlib
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ROMS = Path(os.environ.get("KESTREL_TESTROMS", r"E:\Claude\N64\test_roms"))
KROM = ROMS / "PeterLemon-N64"
EXE = Path(os.environ.get("KESTREL_EXE", ROOT / "build" / "kestrel64.exe"))
BASELINES = ROOT / "docs" / "baselines"
OUT = ROOT / "out"

MODES = {
    "interp":        {},
    "jit":           {"KESTREL_JIT": "1"},
    "link":          {"KESTREL_JIT": "1", "KESTREL_JIT_LINK": "1"},
    "threaded":      {"KESTREL_THREADS": "1"},
    "threaded-jit":  {"KESTREL_THREADS": "1", "KESTREL_JIT": "1"},
    "threaded-link": {"KESTREL_THREADS": "1", "KESTREL_JIT": "1", "KESTREL_JIT_LINK": "1"},
}

# Accuracy below this counts as "broken" rather than "imperfect" — used only to
# bucket the summary, never to gate a diff (the baseline does the gating).
BROKEN_BELOW = 50.0
# A run is a regression when it drops more than this vs the baseline. Slack
# covers ROMs that animate: a fixed instruction cap lands on a frame boundary
# that can shift by one frame between builds.
REGRESS_EPS = 0.50


def env_for(mode, extra=None):
    e = dict(os.environ)
    e.update(MODES[mode])
    if extra:
        e.update(extra)
    return e


# ---------------------------------------------------------------- image compare

def _pil():
    from PIL import Image, ImageFile, PngImagePlugin
    # Several PeterLemon references carry the whole source .asm in a zTXt chunk.
    # Pillow's default 1 MB text-chunk limit rejects those outright ("cannot
    # identify image file"), which silently drops real test cases from the sweep.
    PngImagePlugin.MAX_TEXT_CHUNK = 64 * 1024 * 1024
    ImageFile.LOAD_TRUNCATED_IMAGES = True
    return Image


def load_rgb(path):
    import numpy as np
    Image = _pil()
    with Image.open(path) as im:
        return np.asarray(im.convert("RGB"), dtype=np.int16)


def compare(bmp, png):
    """Return (exact%, close%, rmse, note). close = per-pixel |dR|+|dG|+|dB| <= 24.

    The N64 framebuffer is 16bpp on most of these ROMs, so a reference PNG
    captured from hardware differs from a perfect emulation only in the 5->8 bit
    expansion when the capture path differs; `close` absorbs that, `exact`
    does not. Both are reported so a change that trades one for the other is
    visible instead of averaging out.

    When the dump and the reference disagree on size the score is nearly
    meaningless, so the mismatch is reported as a note instead of being hidden by
    a rescale: a 640x480 reference against a 640x240 dump means the VI mode was
    emulated wrong, which is a finding, not a bad pixel.
    """
    import numpy as np
    Image = _pil()
    a = load_rgb(bmp)
    b = load_rgb(png)
    note = ""
    if a.shape != b.shape:
        note = f"SIZE {a.shape[1]}x{a.shape[0]} vs ref {b.shape[1]}x{b.shape[0]}"
        with Image.open(png) as im:
            # NEAREST, never a filter: resampling invents colors the RDP never
            # wrote and would move the score for reasons unrelated to the emulator.
            b = np.asarray(im.convert("RGB").resize((a.shape[1], a.shape[0]), Image.NEAREST),
                           dtype=np.int16)
    d = a - b
    man = np.abs(d).sum(axis=2)
    tot = man.size
    exact = float((man == 0).sum()) * 100.0 / tot
    close = float((man <= 24).sum()) * 100.0 / tot
    rmse = float(np.sqrt((d.astype(np.float64) ** 2).mean()))
    return exact, close, rmse, note


# ------------------------------------------------------------------- run a ROM

def run_rom(rom, dump, mode, insn, timeout, extra_env=None):
    """Run one ROM headless to the instruction cap, dumping the VI framebuffer.

    Relies on System::exitOnHalt: with --run, hitting KESTREL_MAXINSN halts the
    CPU and ends the process. No taskkill dance, so ROMs can run in parallel.
    """
    env = env_for(mode, extra_env)
    env["KESTREL_MAXINSN"] = str(insn)
    env["KESTREL_FBDUMP"] = str(dump)
    if dump.exists():
        dump.unlink()
    try:
        p = subprocess.run([str(EXE), str(rom), "--run"], env=env, timeout=timeout,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        return p.returncode, p.stdout.decode("utf-8", "replace")
    except subprocess.TimeoutExpired as ex:
        out = ex.stdout.decode("utf-8", "replace") if ex.stdout else ""
        return -9, out + "\n[validate] TIMEOUT\n"


# --------------------------------------------------------------- gate: systemtest

ST_RE = re.compile(r"(Base|Timing|Cycle):\s+Failed\s+(\d+)\s+of\s+(\d+)\s+tests")


def gate_systemtest(mode, args):
    rom = ROMS / "n64-systemtest.z64"
    if not rom.exists():
        print(f"systemtest: SKIP (missing {rom})")
        return True
    log = OUT / f"systemtest-{mode}.log"
    env = env_for(mode)
    t0 = time.time()
    # n64-systemtest prints its summary and then idles, so it is bounded by the
    # timeout rather than by a halt. We read what it printed, not its exit code.
    try:
        p = subprocess.run([str(EXE), str(rom), "--run"], env=env, timeout=args.st_timeout,
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        txt = p.stdout.decode("utf-8", "replace")
    except subprocess.TimeoutExpired as ex:
        txt = ex.stdout.decode("utf-8", "replace") if ex.stdout else ""
    log.write_text(txt, encoding="utf-8")
    hits = ST_RE.findall(txt)
    if not hits:
        print(f"systemtest[{mode}]: NO SUMMARY (see {log.relative_to(ROOT)})")
        return False
    bad = [h for h in hits if int(h[1]) != 0]
    res = "  ".join(f"{k}:{f}/{t}" for k, f, t in hits)
    print(f"systemtest[{mode}]: {'FAIL' if bad else 'PASS'}  {res}  ({time.time()-t0:.0f}s)")
    if bad:
        for line in txt.splitlines():
            if "Failed" in line or "FAILURE" in line:
                print("   " + line.strip())
    return not bad


# --------------------------------------------------------------------- gate: krom

def krom_cases(filt=None):
    """Every ROM in the PeterLemon suite that ships a same-named PNG reference.

    The PNG is the hardware capture the ROM's own .asm was written to produce,
    so it is the oracle. ROMs without one (video/audio demos, interactive tests)
    have nothing to diff against and are skipped.
    """
    cases = []
    for rom in sorted(KROM.rglob("*.[Nn]64")):
        png = rom.with_suffix(".png")
        if not png.exists():
            png = rom.with_suffix(".PNG")
        if not png.exists():
            continue
        name = rom.relative_to(KROM).with_suffix("").as_posix()
        if filt and filt.lower() not in name.lower():
            continue
        cases.append((name, rom, png))
    return cases


def krom_one(job):
    name, rom, png, mode, insn, timeout, outdir = job
    dump = outdir / (re.sub(r"[^A-Za-z0-9]+", "_", name) + ".bmp")
    rc, log = run_rom(rom, dump, mode, insn, timeout)
    if not dump.exists():
        return name, None, None, None, f"NODUMP rc={rc}"
    try:
        ex, cl, rmse, note = compare(dump, png)
    except Exception as e:                      # noqa: BLE001 - report, don't abort the sweep
        return name, None, None, None, f"CMPERR {e}"
    if not os.environ.get("KESTREL_KEEP_BMP"):
        dump.unlink()
    return name, ex, cl, rmse, note


def read_baseline(path):
    base = {}
    if not path.exists():
        return base
    for line in path.read_text(encoding="utf-8").splitlines():
        if not line or line.startswith("#"):
            continue
        f = line.split("\t")
        # NODUMP rows are written with empty accuracy columns; they carry no
        # number to compare against, so they never enter the baseline.
        if len(f) >= 3 and f[1] and f[2]:
            base[f[0]] = (float(f[1]), float(f[2]))
    return base


def gate_krom(mode, args):
    cases = krom_cases(args.filter)
    if not cases:
        print(f"krom[{mode}]: SKIP (no ROM+PNG pairs under {KROM})")
        return True
    outdir = OUT / f"krom-{mode}"
    outdir.mkdir(parents=True, exist_ok=True)
    jobs = [(n, r, p, mode, args.insn, args.timeout, outdir) for n, r, p in cases]

    t0 = time.time()
    rows = []
    done = 0
    with futures.ProcessPoolExecutor(max_workers=args.jobs) as pool:
        for res in pool.map(krom_one, jobs):
            rows.append(res)
            done += 1
            if not args.quiet:
                print(f"\r  {done}/{len(jobs)} {res[0][:60]:<60}", end="", file=sys.stderr)
    if not args.quiet:
        print("\r" + " " * 78 + "\r", end="", file=sys.stderr)
    rows.sort(key=lambda r: r[0])

    report = OUT / f"krom-{mode}.tsv"
    with report.open("w", encoding="utf-8") as fh:
        fh.write("# name\texact%\tclose%\trmse\tnote\n")
        for n, ex, cl, rm, note in rows:
            if ex is None:
                fh.write(f"{n}\t\t\t\t{note}\n")
            else:
                fh.write(f"{n}\t{ex:.2f}\t{cl:.2f}\t{rm:.2f}\t{note}\n")

    ok = [r for r in rows if r[1] is not None]
    fails = [r for r in rows if r[1] is None]
    perfect = [r for r in ok if r[1] >= 99.99]
    broken = [r for r in ok if r[1] < BROKEN_BELOW]
    mean_ex = sum(r[1] for r in ok) / len(ok) if ok else 0.0
    mean_cl = sum(r[2] for r in ok) / len(ok) if ok else 0.0

    if args.update_baseline:
        BASELINES.mkdir(parents=True, exist_ok=True)
        bl = BASELINES / f"krom-{mode}.tsv"
        shutil.copyfile(report, bl)
        print(f"krom[{mode}]: baseline updated -> {bl.relative_to(ROOT)}")

    # The RDP output must not depend on how the CPU was executed, so a JIT or
    # threaded run is diffed against the interpreter baseline (--vs interp)
    # instead of needing a frozen baseline of its own.
    ref_mode = args.vs or mode
    base = read_baseline(BASELINES / f"krom-{ref_mode}.tsv")
    regress, improve, new = [], [], []
    for n, ex, cl, rm, note in rows:
        if n not in base:
            new.append(n)
            continue
        bex = base[n][0]
        cur = -1.0 if ex is None else ex
        if cur < bex - REGRESS_EPS:
            regress.append((n, bex, cur))
        elif cur > bex + REGRESS_EPS:
            improve.append((n, bex, cur))

    sizemis = [r for r in ok if r[4].startswith("SIZE")]
    print(f"krom[{mode}]: {len(ok)}/{len(rows)} ran  mean_exact={mean_ex:.2f} mean_close={mean_cl:.2f}  "
          f"perfect={len(perfect)} broken(<{BROKEN_BELOW:.0f}%)={len(broken)} size-mismatch={len(sizemis)} "
          f"nodump={len(fails)}  ({time.time()-t0:.0f}s, {args.jobs} jobs)")
    if not base:
        print(f"   no baseline for '{ref_mode}' - rerun with --update-baseline to freeze this run")
    else:
        print(f"   vs baseline: regress={len(regress)} improve={len(improve)} new={len(new)}")
        for n, b, c in regress[:args.list_max]:
            print(f"   REGRESS {n}: {b:.2f} -> {c if c >= 0 else float('nan'):.2f}")
        for n, b, c in improve[:args.list_max]:
            print(f"   improve {n}: {b:.2f} -> {c:.2f}")
    if fails[:args.list_max]:
        for n, _, _, _, note in fails[:args.list_max]:
            print(f"   NODUMP  {n}: {note}")
    print(f"   detail: {report.relative_to(ROOT)}")
    return not regress and not fails


# --------------------------------------------------------------------- gate: sm64

def gate_sm64(mode, args):
    rom = ROMS / "Super Mario 64 (USA).z64"
    if not rom.exists():
        print(f"sm64: SKIP (missing {rom})")
        return True
    save = rom.with_suffix(".eep")
    if save.exists():
        save.unlink()          # see module docstring: a stale save changes the md5
    dump = OUT / f"sm64-{mode}.bmp"
    t0 = time.time()
    rc, log = run_rom(rom, dump, mode, args.sm64_insn, args.sm64_timeout)
    (OUT / f"sm64-{mode}.log").write_text(log, encoding="utf-8")
    if save.exists():
        save.unlink()
    if not dump.exists():
        print(f"sm64[{mode}]: FAIL no framebuffer dump (rc={rc})")
        return False
    md5 = hashlib.md5(dump.read_bytes()).hexdigest()
    bl = BASELINES / "sm64.txt"
    want = bl.read_text(encoding="utf-8").split()[0] if bl.exists() else None
    if args.update_baseline:
        BASELINES.mkdir(parents=True, exist_ok=True)
        bl.write_text(md5 + "\n", encoding="utf-8")
        want = md5
    verdict = "MATCH" if want == md5 else ("no-baseline" if want is None else "DIVERGE")
    print(f"sm64[{mode}]: {verdict} md5={md5} ({time.time()-t0:.0f}s, {args.sm64_insn/1e6:.0f}M ops)")
    if want is not None and want != md5:
        print(f"   baseline {want}")
    return want is None or want == md5


# ------------------------------------------------------------------------- main

def main():
    ap = argparse.ArgumentParser(description="kestrel64 validation gates")
    ap.add_argument("gate", choices=["systemtest", "krom", "sm64", "all"])
    ap.add_argument("--mode", default="interp", choices=sorted(MODES),
                    help="emulator configuration under test (default: interp = oracle)")
    ap.add_argument("--filter", help="krom: substring of the ROM path, e.g. RDP/ or Triangle")
    ap.add_argument("--jobs", type=int, default=max(1, (os.cpu_count() or 4) // 2))
    ap.add_argument("--insn", type=int, default=20_000_000, help="krom: instruction cap per ROM")
    ap.add_argument("--timeout", type=int, default=90, help="krom: seconds per ROM")
    ap.add_argument("--sm64-insn", type=int, default=300_000_000)
    ap.add_argument("--sm64-timeout", type=int, default=600)
    ap.add_argument("--st-timeout", type=int, default=300)
    ap.add_argument("--update-baseline", action="store_true")
    ap.add_argument("--vs", choices=sorted(MODES),
                    help="diff against another mode's baseline (default: the same mode)")
    ap.add_argument("--list-max", type=int, default=25, help="max per-ROM lines printed")
    ap.add_argument("--quiet", action="store_true", help="no progress line on stderr")
    args = ap.parse_args()

    if not EXE.exists():
        print(f"error: emulator not found at {EXE}", file=sys.stderr)
        return 2
    OUT.mkdir(parents=True, exist_ok=True)

    gates = ["systemtest", "krom", "sm64"] if args.gate == "all" else [args.gate]
    fail = False
    for g in gates:
        okg = {"systemtest": gate_systemtest, "krom": gate_krom, "sm64": gate_sm64}[g](args.mode, args)
        if not okg:
            fail = True
            if args.gate == "all":
                print(f"STOP: gate '{g}' failed")
                break
    print("ALL OK" if not fail else "FAILED")
    return 1 if fail else 0


if __name__ == "__main__":
    sys.exit(main())
