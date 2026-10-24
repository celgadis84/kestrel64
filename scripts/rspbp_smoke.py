"""Prueba de humo de los puntos de ruptura del RSP (rsp.bp.*) contra un kestrel vivo.

Arranca SM64 sin ventana, pone un punto en IMEM 0x000 (rspboot: toda tarea entra por ahi),
espera el acierto, comprueba que el RSP esta quieto en ese PC y que la CPU se ha parado, sigue
con resume, exige un SEGUNDO acierto (el salto de una vez funciona y el siguiente vuelve a
parar), quita todo y comprueba que el juego avanza. Una linea por paso y veredicto final.
Uso: python scripts/rspbp_smoke.py [--threads 0|1] [--port 9172]
"""
import romcfg  # configuracion local: scripts/local.env
import argparse, os, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools" / "mcp"))

ap = argparse.ArgumentParser()
ap.add_argument("--port", type=int, default=9172)
ap.add_argument("--threads", default="1", help="KESTREL_THREADS: 1 = Threaded, 0 = Lockstep")
ap.add_argument("--rom", default=romcfg.rom("ROM_SM64"))
ap.add_argument("--exe", default=str(ROOT / "build" / "kestrel64.exe"))
args = ap.parse_args()
args.rom = str(romcfg.need(args.rom, "ROM_SM64"))

import kestrel_mcp as k  # noqa: E402
k.PORT = args.port

env = dict(os.environ, KESTREL_AUDIO="0", KESTREL_MAXINSN="4000000000", KESTREL_THREADS=args.threads)
p = subprocess.Popen([args.exe, args.rom, "--run", "--port", str(args.port)], env=env,
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
fails = 0


def check(name, good, info=""):
    global fails
    print(f"{'OK  ' if good else 'BAD '} {name} {str(info)[:150]}")
    fails += 0 if good else 1


def wait_hit(sec=5.0):
    t0 = time.time()
    while time.time() - t0 < sec:
        d = k.rsp_breakpoint_list()
        if d.get("hit"):
            return d
        time.sleep(0.02)
    return k.rsp_breakpoint_list()


try:
    for _ in range(50):
        try:
            k.emu_status(); break
        except Exception:  # noqa: BLE001
            time.sleep(0.2)
    time.sleep(1.0)                                   # que arranque y mande tareas
    d = k.rsp_breakpoint_add(0x000)
    check("add", d.get("count") == 1, d)
    d = wait_hit()
    check("hit 1", d.get("hit") and d.get("hitPc") == 0, d)
    r = k.rsp_registers()
    check("rsp quieto en el PC", r.get("pc") == 0, {"pc": r.get("pc")})
    time.sleep(0.3)
    s = k.emu_status()
    check("cpu parada", bool(s.get("paused")), {"paused": s.get("paused")})
    k.run_control("resume")
    d = wait_hit()
    check("hit 2 (tras resume)", d.get("hit") and d.get("hitPc") == 0, d)
    k.run_control("resume")
    time.sleep(0.05)
    k.run_control("pause")
    d = k.rsp_breakpoint_list()
    if d.get("hit"):
        k.run_control("resume")
        k.run_control("pause")
    d = k.rsp_breakpoint_del()
    check("del all", d.get("count") == 0, d)
    k.run_control("resume")
    v0 = k.emu_status().get("speed", {}).get("insns")
    time.sleep(1.5)
    v1 = k.emu_status().get("speed", {}).get("insns")
    check("sigue corriendo", v0 is not None and v1 is not None and v1 > v0 and not k.rsp_breakpoint_list().get("hit"),
          {"antes": v0, "despues": v1})
finally:
    if p.poll() is None:
        p.kill()   # solo nuestro PID
print("VEREDICTO:", "PASS" if fails == 0 else f"FAIL ({fails})")
sys.exit(0 if fails == 0 else 1)
