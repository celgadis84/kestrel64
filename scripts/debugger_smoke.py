"""Prueba de humo de la ventana del depurador (docs/DEBUGGER.md), solo Windows.

Lanza una ROM sin ventana de juego (--run) con --debugger en un puerto propio y
KESTREL_DEBUGGER_SHOT, enciende la grabadora por MCP y espera a que la ventana vuelque su
backbuffer (a los 180 cuadros de interfaz). Veredicto: la ventana existe, el proceso sigue
vivo y la foto tiene contenido (no es un color plano). La foto queda en --out (BMP).
Uso: python scripts/debugger_smoke.py [--port 9143] [--out captura.bmp]
"""
import romcfg  # configuracion local: scripts/local.env
import argparse, ctypes, os, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools" / "mcp"))
ap = argparse.ArgumentParser()
ap.add_argument("--port", type=int, default=9143)
ap.add_argument("--rom", default=romcfg.rom("ROM_SM64"))
ap.add_argument("--exe", default=str(ROOT / "build" / "kestrel64.exe"))
ap.add_argument("--out", default="debugger_smoke.bmp")
ap.add_argument("--wait", type=float, default=15.0)
args = ap.parse_args()
args.rom = str(romcfg.need(args.rom, "ROM_SM64"))
os.environ["KESTREL_TELEMETRY_PORT"] = str(args.port)
import kestrel_mcp as k  # noqa: E402
k.PORT = args.port

out = Path(args.out).resolve()
if out.exists():
    out.unlink()
env = dict(os.environ, KESTREL_AUDIO="0", KESTREL_MAXINSN="4000000000", KESTREL_DEBUGGER_SHOT=str(out))
p = subprocess.Popen([args.exe, args.rom, "--run", "--debugger", "--port", str(args.port)], env=env,
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
ok = False
try:
    for _ in range(50):
        try:
            k.emu_status(); break
        except Exception:  # noqa: BLE001
            time.sleep(0.2)
    k.dbg_enable(True)
    end = time.time() + args.wait
    while time.time() < end and not out.exists():
        time.sleep(0.3)
    time.sleep(0.5)
    hwnd = ctypes.windll.user32.FindWindowW("kestrel64_debugger", None)
    alive = p.poll() is None
    ncol = 0
    if out.exists():
        b = out.read_bytes()[54:]
        ncol = len({b[i:i + 3] for i in range(0, len(b), 4 * 97)})
    print(f"ventana={'si' if hwnd else 'NO'} vivo={alive} foto={'si' if out.exists() else 'NO'} colores~{ncol} -> {out}")
    ok = bool(hwnd) and alive and ncol > 8
finally:
    if p.poll() is None:
        p.kill()   # solo nuestro PID
print("VEREDICTO:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
