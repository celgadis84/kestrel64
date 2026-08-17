#!/usr/bin/env python3
"""Run one krom ROM and print a side-by-side pixel dump against its reference.

The suite score tells you a ROM regressed; it does not tell you what the RDP
actually wrote. This does: it prints the most common (ours, reference) colour
pairs, which is normally enough to read off which stage is wrong (a constant
offset = combiner rounding, a 1/32 smear of the background = blender, a
completely different hue = texture decode).

usage: python scripts/probe.py <substring of ROM path> [--insn N]
"""
import argparse
import collections
import os
import pathlib
import subprocess
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parent
EXE = ROOT / "build" / "kestrel64.exe"
KROM = pathlib.Path("E:/Claude/N64/test_roms/PeterLemon-N64")
OUT = ROOT / "out"


def load_rgb(p):
    import numpy as np
    from PIL import Image
    with Image.open(p) as im:
        return np.asarray(im.convert("RGB"), dtype=np.int16)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pattern")
    ap.add_argument("--insn", type=int, default=20_000_000)
    ap.add_argument("--top", type=int, default=12)
    ap.add_argument("--env", action="append", default=[], help="EXTRA=VAL")
    a = ap.parse_args()

    cands = [p for p in KROM.rglob("*.N64") if a.pattern.lower() in str(p).lower()]
    if not cands:
        sys.exit(f"no ROM matching {a.pattern!r}")
    rom = cands[0]
    png = rom.with_suffix(".png")
    if not png.exists():
        alts = list(rom.parent.glob("*.png"))
        if not alts:
            sys.exit(f"no reference png next to {rom}")
        png = alts[0]

    OUT.mkdir(exist_ok=True)
    dump = OUT / "probe.bmp"
    if dump.exists():
        dump.unlink()
    env = dict(os.environ)
    # the exe still pulls a couple of runtime DLLs from the MSYS2 clang64 tree; without
    # them CreateProcess dies with 0xC0000135 and no output at all.
    env["PATH"] = r"C:\msys64\clang64\bin" + os.pathsep + env.get("PATH", "")
    env["KESTREL_MAXINSN"] = str(a.insn)
    # forward slashes: a backslash path reaches the emulator through the shell's
    # escape handling and comes out mangled, so the dump never lands.
    env["KESTREL_FBDUMP"] = dump.as_posix()
    for kv in a.env:
        k, _, v = kv.partition("=")
        env[k] = v
    subprocess.run([EXE.as_posix(), rom.as_posix(), "--run"], env=env, timeout=120,
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if not dump.exists():
        sys.exit("no framebuffer dump produced")

    import numpy as np
    ours, ref = load_rgb(dump), load_rgb(png)
    print(f"rom  {rom.relative_to(KROM)}")
    print(f"ref  {png.name}  ours {ours.shape[1]}x{ours.shape[0]}  ref {ref.shape[1]}x{ref.shape[0]}")
    if ours.shape != ref.shape:
        print("size mismatch — pixel pairing is meaningless, stopping")
        return
    pairs = collections.Counter(
        (tuple(int(v) for v in o), tuple(int(v) for v in r))
        for o, r in zip(ours.reshape(-1, 3), ref.reshape(-1, 3)))
    tot = ours.shape[0] * ours.shape[1]
    print(f"{'ours':>16} {'ref':>16} {'count':>8}  {'%':>6}  delta")
    for (o, r), n in pairs.most_common(a.top):
        d = tuple(int(x) - int(y) for x, y in zip(o, r))
        print(f"{str(o):>16} {str(r):>16} {n:>8} {n*100.0/tot:>6.2f}  {d}")


if __name__ == "__main__":
    main()
