"""Prueba de humo (2026-10-23): (1) una orden que espera al nucleo (cpu.run_until) ya no
coge dispatchMx, asi que otro cliente sigue atendido mientras tanto; (2) mem.write a IMEM
con el RSP corriendo en Threaded+JIT deja la sincronizacion de la tabla del JIT al hilo del
RSP (jitImemSyncReq) y el juego sigue vivo.
Uso: python scripts/mcpwait_smoke.py [--port 9175]
"""
import romcfg  # configuracion local: scripts/local.env
import argparse, json, os, socket, struct, subprocess, sys, threading, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
ap = argparse.ArgumentParser()
ap.add_argument("--port", type=int, default=9175)
ap.add_argument("--rom", default=romcfg.rom("ROM_SM64"))
ap.add_argument("--exe", default=str(ROOT / "build" / "kestrel64.exe"))
args = ap.parse_args()
args.rom = str(romcfg.need(args.rom, "ROM_SM64"))


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
        return r.get("data", {}), pl[4 + jl:]


env = dict(os.environ, KESTREL_AUDIO="0", KESTREL_MAXINSN="4000000000", KESTREL_THREADS="1")
p = subprocess.Popen([args.exe, args.rom, "--run", "--port", str(args.port)], env=env,
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
fails = 0


def check(name, good, info=""):
    global fails
    print(f"{'OK  ' if good else 'BAD '} {name} {str(info)[:150]}")
    fails += 0 if good else 1


try:
    a = None
    for _ in range(50):
        try:
            a = Cli(args.port); a.q("ping"); break
        except Exception:  # noqa: BLE001
            time.sleep(0.2)
    b = Cli(args.port)
    time.sleep(2.0)
    # (2) IMEM con el RSP vivo: reescribe sus propios bytes y tambien unos distintos y vuelta.
    f0 = b.q("status")[0].get("speed", {}).get("insns", 0)
    for i in range(40):
        _, im = b.q("mem.read", region="imem", addr=0, len=4096)
        b.q("mem.write", im, region="imem", addr=0)
    time.sleep(1.0)
    st = b.q("status")[0]
    check("IMEM poke con RSP vivo, sigue corriendo", st.get("speed", {}).get("insns", 0) > f0 and not st.get("halted"),
          {"f0": f0, "f1": st.get("speed", {}).get("insns")})
    # (1) run_until a una direccion que no se alcanza, 3 s; mientras, pings desde b.
    res = {}
    t = threading.Thread(target=lambda: res.setdefault("r", a.q("cpu.run_until", addr=0x80000004, timeout_ms=3000)))
    t.start(); time.sleep(0.3)
    worst = 0.0
    for _ in range(10):
        t0 = time.perf_counter(); b.q("ping"); worst = max(worst, time.perf_counter() - t0)
        time.sleep(0.05)
    t.join()
    check("ping atendido durante run_until", worst < 0.25, {"peor_s": round(worst, 3)})
    check("run_until vuelve por plazo", res.get("r", ({},))[0].get("timedOut") is True, res.get("r", ({},))[0])
finally:
    if p.poll() is None:
        p.kill()   # solo nuestro PID
print("VEREDICTO:", "PASS" if fails == 0 else f"FAIL ({fails})")
sys.exit(0 if fails == 0 else 1)
