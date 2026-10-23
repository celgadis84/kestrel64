#!/usr/bin/env python3
"""Demo de las lianas de DK64: arranca, UNA pulsacion de START salta el rap, y saca capturas
del framebuffer cada `--period` s mientras corre la demo de atraccion. Las PNG quedan en
`--out` para mirarlas (DK cruzando por las lianas = bien; DK nadando = demo desincronizada).
Puerto propio (defecto 9176). Ver docs/STATUS.md "Estado de la prueba de la liana de DK64".

  python scripts/dk_vine_check.py --exe build/kestrel64.exe --out <dir> [--threads 1]
"""
import argparse, os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools", "mcp"))
import kestrel_mcp as k  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("--exe", default=os.path.join(ROOT, "build", "kestrel64.exe"))
ap.add_argument("--rom", default=r"E:\Claude\N64\test_roms\Donkey Kong 64 (USA).n64")
ap.add_argument("--port", type=int, default=9176)
ap.add_argument("--out", required=True)
ap.add_argument("--intro", type=float, default=10.0)
ap.add_argument("--shots", type=int, default=12)
ap.add_argument("--period", type=float, default=3.0)
ap.add_argument("--threads", default="1")
ap.add_argument("--tag", default="v")
args = ap.parse_args()
os.makedirs(args.out, exist_ok=True)
k.PORT = args.port

env = dict(os.environ, KESTREL_AUDIO="0", KESTREL_THREADS=args.threads, KESTREL_PRDP="0",
           KESTREL_GPURDP="0", KESTREL_MAXINSN="200000000000")
p = subprocess.Popen([args.exe, args.rom, "--run", "--port", str(args.port)], env=env,
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    for _ in range(100):
        try:
            k.emu_status(); break
        except Exception:  # noqa: BLE001
            time.sleep(0.2)
    time.sleep(args.intro)
    k.controller_set(buttons="start", polls=8)
    for i in range(args.shots):
        time.sleep(args.period)
        png = os.path.join(args.out, f"{args.tag}{i:02d}.png")
        cap = k.capture_framebuffer(png)
        st = k.emu_status()
        print(f"{i:02d} fields={st.get('vi', {}).get('fields', st.get('viFields'))} "
              f"col={cap.get('distinctColors')} {png}", flush=True)
finally:
    if p.poll() is None:
        p.kill()   # solo nuestro PID
