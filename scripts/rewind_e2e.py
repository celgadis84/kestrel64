#!/usr/bin/env python3
# Prueba extremo a extremo del rebobinado por telemetria (ver docs/REWIND.md).
#
# Corre A+B campos, rebobina B/intervalo pasos (foto X) y guarda el estado entero (ranura 9);
# corre B campos mas y lo guarda; y REPITE: rebobinar los mismos pasos tiene que volver a X
# byte a byte -- esa foto ya es una de las que se tomaron en la segunda pasada, o sea que
# prueba la cinta escrita tras un rebobinado --, y correr B otra vez tiene que dar el mismo
# estado final: si algo del estado restaurado quedara viejo, el futuro que sale seria otro.
# Se compara el estado entero y no el framebuffer, que puede no cambiar en 40 campos (logo
# de SM64). No se compara con el estado en el campo A: la foto cae al cerrar el campo y la
# pausa del frame_advance no, asi que no son el mismo instante.
# Lockstep y sin adelantos, para que correr dos veces el mismo tramo de el mismo resultado.
# La ROM se copia a un directorio temporal: la ranura 9 cae al lado de la ROM y no se pisa
# la del usuario.
#
#   python scripts/rewind_e2e.py [--exe build-prdp-static/kestrel64.exe] [--rom ...]
#                                [--a 200] [--b 40] [--port 9131] [--env K=V ...]

import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools" / "mcp"))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", default=str(ROOT / "build-prdp-static" / "kestrel64.exe"))
    ap.add_argument("--rom", default=str(ROOT.parent / "test_roms" / "Super Mario 64 (USA).z64"))
    ap.add_argument("--a", type=int, default=200)
    ap.add_argument("--b", type=int, default=40)
    ap.add_argument("--fields", type=int, default=2, help="KESTREL_REWIND_FIELDS")
    ap.add_argument("--port", type=int, default=9131)
    ap.add_argument("--env", action="append", default=[])
    a = ap.parse_args()
    a.exe = str(Path(a.exe).resolve())
    if a.b % a.fields:
        print("--b tiene que ser multiplo de --fields"); return 2

    env = dict(os.environ)
    env.update({"KESTREL_REWIND": "1", "KESTREL_REWIND_FIELDS": str(a.fields),
                "KESTREL_THREADS": "0", "KESTREL_LOADSTATE": "0", "KESTREL_THROTTLE": "0",
                "KESTREL_SPLEAD": "0", "KESTREL_DPLOGLEAD": "0", "KESTREL_AUDIO": "0"})
    for kv in a.env:
        k, v = kv.split("=", 1); env[k] = v
    os.environ["KESTREL_TELEMETRY_PORT"] = str(a.port)
    import kestrel_mcp as k

    tmp = Path(tempfile.mkdtemp(prefix="kestrel_rw_"))
    rom = tmp / Path(a.rom).name
    shutil.copyfile(a.rom, rom)
    p = subprocess.Popen([a.exe, str(rom), "--mcp", "--port", str(a.port)], env=env,
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(100):
            try:
                k.emu_status(); break
            except Exception:
                time.sleep(0.1)
        else:
            print("FAIL: sin telemetria"); return 1

        def fb() -> str:
            d, _ = k.query("state.save", slot=9)
            return hashlib.md5(Path(d["path"]).read_bytes()).hexdigest()

        def adv(n):
            r = k.frame_advance(n, timeout_ms=60000)
            if r.get("timedOut"): raise RuntimeError(f"frame_advance {n} agoto el tiempo")

        t0 = time.time()
        steps = a.b // a.fields
        adv(a.a); adv(a.b)
        k.rewind_step(steps); s1 = fb()
        adv(a.b); s2 = fb()
        r = k.rewind_step(steps); s3 = fb()
        adv(a.b); s4 = fb()
        print(f"rebobinado {steps} pasos: {s1} / otra vez: {s3}")
        print(f"luego +{a.b} campos:      {s2} / otra vez: {s4}")
        print(f"(cinta {r.get('steps')} pasos, {r.get('bytes')} B, {time.time() - t0:.1f} s)")
        if s1 == s2:
            print("FAIL: la prueba no discrimina (mismo estado)"); return 1
        ok = s1 == s3 and s2 == s4
        print("PASS" if ok else "FAIL")
        return 0 if ok else 1
    finally:
        p.kill()
        p.wait()
        shutil.rmtree(tmp, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
