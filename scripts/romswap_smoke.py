"""Prueba de humo de rom.load: arranca SM64 en un puerto propio, cambia a n64-systemtest
por MCP y comprueba que el proceso relanzado responde con la ROM nueva. Los dos procesos
llevan KESTREL_MAXINSN bajo, asi que mueren solos (el relanzado no es hijo nuestro).
Uso: python scripts/romswap_smoke.py [--port 9142]
"""
import romcfg  # configuracion local: scripts/local.env
import argparse, os, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools" / "mcp"))
ap = argparse.ArgumentParser()
ap.add_argument("--port", type=int, default=9142)
ap.add_argument("--exe", default=str(ROOT / "build" / "kestrel64.exe"))
args = ap.parse_args()
os.environ["KESTREL_TELEMETRY_PORT"] = str(args.port)
import kestrel_mcp as k  # noqa: E402
k.PORT = args.port

roms = romcfg.testroms()
a, b = romcfg.need(romcfg.rom("ROM_SM64"), "ROM_SM64"), roms / "n64-systemtest.z64"
env = dict(os.environ, KESTREL_AUDIO="0", KESTREL_MAXINSN="400000000")
p = subprocess.Popen([args.exe, str(a), "--run", "--port", str(args.port)], env=env,
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def wait_status(t=10.0):
    end = time.time() + t
    while time.time() < end:
        try:
            return k.emu_status()
        except Exception:  # noqa: BLE001
            time.sleep(0.2)
    return None


ok = True
s0 = wait_status()
print("antes :", s0 and s0.get("rom", s0))
r = k.rom_load(str(b))
print("rom.load:", r)
p.wait(timeout=10)
print("proceso viejo salio rc=", p.returncode)
time.sleep(1.0)
s1 = wait_status()
print("despues:", s1 and s1.get("rom", s1))
ok = s1 is not None and "systemtest" in str(s1).lower()
print("VEREDICTO:", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
