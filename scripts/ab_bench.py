#!/usr/bin/env python3
"""A/B de velocidad INTERCALADO: corre `validate.py bench --bench-runs 1` alternando las
variantes ronda a ronda y se queda el MINIMO de cada una. Intercalar reparte por igual el
ruido lento del anfitrion (temperatura, otros procesos) entre las variantes.

uso: python scripts/ab_bench.py [--rounds 4] [--mode threaded-jit] [--rom ROM_SM64]
                                [--flips 200] TAG=VAR=1,VAR2=2 TAG2= ...
  TAG=  sin variables es la referencia. --rom es una clave de scripts/local.env.
  KESTREL_EXE elige el exe (por defecto el de validate.py).
"""
import argparse, os, re, subprocess, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import romcfg  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("--rounds", type=int, default=4)
ap.add_argument("--mode", default="threaded-jit")
ap.add_argument("--rom", default="ROM_SM64", help="clave de local.env (ROM_SM64, ROM_DK64, ROM_PD)")
ap.add_argument("--flips", type=int, default=200)
ap.add_argument("specs", nargs="+")
a = ap.parse_args()

rom = romcfg.need(romcfg.rom(a.rom), a.rom)
variants = []
for s in a.specs:
    tag, _, envs = s.partition("=")
    env = dict(os.environ)
    for kv in filter(None, envs.split(",")):
        k, _, v = kv.partition("=")
        env[k] = v
    variants.append((tag, env))

times = {t: [] for t, _ in variants}
for r in range(a.rounds):
    for tag, env in variants:
        p = subprocess.run([sys.executable, str(HERE / "validate.py"), "bench", "--mode", a.mode,
                            "--bench-runs", "1", "--bench-flips", str(a.flips), "--bench-rom", str(rom),
                            "--quiet"], env=env, capture_output=True, text=True)
        m = re.search(r"run 1/1: ([\d.]+)s", p.stdout)
        if not m:
            print(f"{tag}: corrida INVALIDA\n{p.stdout[-400:]}")
            sys.exit(1)
        times[tag].append(float(m.group(1)))
        print(f"ronda {r+1} {tag}: {m.group(1)} s", flush=True)

base = min(times[variants[0][0]])
for tag, _ in variants:
    b = min(times[tag])
    print(f"{tag:12s} min {b:7.2f} s  ({(b / base - 1) * 100:+.1f} %)  todas {sorted(times[tag])}")
