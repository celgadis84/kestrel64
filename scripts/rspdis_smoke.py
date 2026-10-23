"""Prueba de humo (2026-10-23): rsp.disasm desensambla IMEM con mnemonicos propios del RSP
(VU, LWC2/SWC2, COP0 SP_*/DPC_*) y marca los puntos de ruptura puestos con rsp.bp.add.
Uso: python scripts/rspdis_smoke.py [--port 9178]
"""
import argparse, json, os, socket, struct, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ap = argparse.ArgumentParser()
ap.add_argument("--port", type=int, default=9178)
ap.add_argument("--rom", default=str(ROOT.parent / "test_roms" / "Super Mario 64 (USA).z64"))
ap.add_argument("--exe", default=str(ROOT / "build" / "kestrel64.exe"))
args = ap.parse_args()


class Cli:
    def __init__(self, port):
        self.s = socket.create_connection(("127.0.0.1", port), timeout=30)
        self.n = 0

    def q(self, cmd, blob=b"", **a):
        self.n += 1
        j = json.dumps({"id": self.n, "cmd": cmd, "args": a}).encode()
        p = struct.pack("<I", len(j)) + j + blob
        self.s.sendall(struct.pack("<I", len(p)) + p)
        def rx(n):
            b = b""
            while len(b) < n:
                c = self.s.recv(n - len(b))
                if not c: raise OSError("cerrado")
                b += c
            return b
        (t,) = struct.unpack("<I", rx(4)); pl = rx(t)
        (jl,) = struct.unpack("<I", pl[:4])
        r = json.loads(pl[4:4 + jl].decode())
        if not r.get("ok"): raise RuntimeError(r.get("error"))
        return r.get("data", {})


env = dict(os.environ, KESTREL_AUDIO="0", KESTREL_MAXINSN="4000000000", KESTREL_THREADS="1")
p = subprocess.Popen([args.exe, args.rom, "--run", "--port", str(args.port)], env=env,
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
fails = 0


def check(name, good, info=""):
    global fails
    print(f"{'OK  ' if good else 'BAD '} {name} {str(info)[:200]}")
    fails += 0 if good else 1


try:
    c = None
    for _ in range(50):
        try:
            c = Cli(args.port); c.q("ping"); break
        except Exception:  # noqa: BLE001
            time.sleep(0.2)
    time.sleep(3.0)   # con el juego en marcha IMEM lleva el microcodigo grafico
    d = c.q("rsp.disasm", addr=0x80, count=1024)
    ins = d.get("insns", [])
    txt = [e["text"] for e in ins]
    mn = {t.split()[0] for t in txt if t}
    check("1024 instrucciones", len(ins) == 1024, len(ins))
    check("hay operaciones de la VU", any(m.startswith("vm") or m in ("vadd", "vsub", "vxor") for m in mn), sorted(mn)[:40])
    check("hay cargas/almacenes vectoriales", any(m[0] in "ls" and m.endswith("v") and len(m) == 3 for m in mn), sorted(mn)[:40])
    check("COP0 con nombre SP_/DPC_", any(("SP_" in t or "DPC_" in t) for t in txt))
    check("sin cop2 desconocidos", not any(t.startswith("cop2 ") for t in txt), [t for t in txt if t.startswith("cop2 ")][:5])
    c.q("rsp.bp.add", addr=0x100)
    d = c.q("rsp.disasm", addr=0x100, count=2)
    check("bp marcado en la fila", d["insns"][0].get("bp") is True and not d["insns"][1].get("bp"), d["insns"])
    c.q("rsp.bp.del")
    c.q("resume")
finally:
    if p.poll() is None:
        p.kill()   # solo nuestro PID
print("VEREDICTO:", "PASS" if fails == 0 else f"FAIL ({fails})")
sys.exit(0 if fails == 0 else 1)
