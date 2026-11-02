"""Prueba de humo de la recreacion de la cadena de intercambio (src/video/present.cpp), Windows.

Lanza una ROM con ventana (--play) y la redimensiona varias veces por Win32 (y la minimiza y
restaura). Veredicto: el proceso sigue vivo, hay lineas "[video] swapchain recreado ... ok=1"
y ninguna ok=0 tras la ultima restauracion.
Uso: python scripts/swapchain_smoke.py [--exe build-gpu/kestrel64.exe] [--port 9173]
"""
import romcfg  # configuracion local: scripts/local.env
import argparse, ctypes, os, subprocess, sys, time
from ctypes import wintypes
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ap = argparse.ArgumentParser()
ap.add_argument("--exe", default=str(ROOT / "build-gpu" / "kestrel64.exe"))
ap.add_argument("--rom", default=romcfg.rom("ROM_SM64"))
ap.add_argument("--port", type=int, default=9173)
ap.add_argument("--log", default="swapchain_smoke.log")
args = ap.parse_args()
args.rom = str(romcfg.need(args.rom, "ROM_SM64"))

u32 = ctypes.windll.user32
env = dict(os.environ, KESTREL_AUDIO="0", KESTREL_MAXINSN="4000000000")
log = open(args.log, "wb")
p = subprocess.Popen([args.exe, args.rom, "--play", "--port", str(args.port)], env=env,
                     stdout=log, stderr=subprocess.STDOUT)

def find_hwnd(pid):
    # La ventana del juego es de GLFW (clase GLFW30); se busca la de NUESTRO proceso.
    h = None
    while True:
        h = u32.FindWindowExW(None, h, "GLFW30", None)
        if not h:
            break
        wp = wintypes.DWORD()
        u32.GetWindowThreadProcessId(h, ctypes.byref(wp))
        if wp.value == pid:
            return h
    found = []
    PROC = ctypes.WINFUNCTYPE(wintypes.BOOL, wintypes.HWND, wintypes.LPARAM)
    def cb(h, _):
        wp = wintypes.DWORD()
        u32.GetWindowThreadProcessId(h, ctypes.byref(wp))
        if wp.value == pid and u32.IsWindowVisible(h):
            found.append(h)
        return True
    u32.EnumWindows(PROC(cb), 0)
    return found[0] if found else None

ok = False
try:
    h = None
    for _ in range(150):   # hasta 30 s: build-gpu tarda en crear el dispositivo Vulkan
        h = find_hwnd(p.pid)
        if h: break
        time.sleep(0.2)
    time.sleep(2.0)
    if h:
        for (w, hh) in ((800, 600), (500, 400), (1024, 768), (640, 480)):
            u32.MoveWindow(h, 50, 50, w, hh, True); time.sleep(0.8)
        u32.ShowWindow(h, 6); time.sleep(0.8)   # SW_MINIMIZE
        u32.ShowWindow(h, 9); time.sleep(1.2)   # SW_RESTORE
    alive = p.poll() is None
finally:
    if p.poll() is None:
        p.kill()   # solo nuestro PID
    log.close()
txt = Path(args.log).read_text(errors="replace")
rec = [l for l in txt.splitlines() if "swapchain recreado" in l]
nok = sum(1 for l in rec if l.rstrip().endswith("ok=1"))
last_ok = bool(rec) and rec[-1].rstrip().endswith("ok=1")
print(f"ventana={'si' if h else 'NO'} vivo={alive} recreaciones={len(rec)} ok={nok} ultima_ok={last_ok}")
ok = bool(h) and alive and nok >= 1 and last_ok
print("VEREDICTO:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
