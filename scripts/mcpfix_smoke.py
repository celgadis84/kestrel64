"""Prueba de humo de los arreglos MCP de 2026-10-23: cpu.disasm/cpu.regs leen el op por la
direccion VIRTUAL (traduccion en sondeo), vi.capture saca el alto del VI y state.* exige slot.
Uso: python scripts/mcpfix_smoke.py [--port 9174]
"""
import romcfg  # configuracion local: scripts/local.env
import argparse, os, subprocess, sys, time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools" / "mcp"))

ap = argparse.ArgumentParser()
ap.add_argument("--port", type=int, default=9174)
ap.add_argument("--rom", default=romcfg.rom("ROM_SM64"))
ap.add_argument("--exe", default=str(ROOT / "build" / "kestrel64.exe"))
args = ap.parse_args()
args.rom = str(romcfg.need(args.rom, "ROM_SM64"))

import kestrel_mcp as k  # noqa: E402
k.PORT = args.port

env = dict(os.environ, KESTREL_AUDIO="0", KESTREL_MAXINSN="4000000000")
p = subprocess.Popen([args.exe, args.rom, "--run", "--port", str(args.port)], env=env,
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
fails = 0


def check(name, good, info=""):
    global fails
    print(f"{'OK  ' if good else 'BAD '} {name} {str(info)[:150]}")
    fails += 0 if good else 1


try:
    for _ in range(50):
        try:
            k.emu_status(); break
        except Exception:  # noqa: BLE001
            time.sleep(0.2)
    time.sleep(2.0)
    k.run_control("pause")
    r = k.cpu_registers()
    pc = r.get("pc", 0) & 0xFFFFFFFF
    phys = pc & 0x1FFFFFFF
    _, blob = k.query("mem.read", region="rdram", addr=phys & ~3, len=16)
    ram = [int.from_bytes(blob[i:i + 4], "big") for i in range(0, 16, 4)]
    d, _ = k.query("cpu.disasm", addr=pc, count=4)
    ops = [e["op"] for e in d["insns"]]
    check("disasm kseg0 = RDRAM y = cpu.regs", ops == ram and ops[0] == r.get("op"),
          {"pc": hex(pc), "ops": [hex(o) for o in ops], "ram": [hex(o) for o in ram]})
    d, _ = k.query("cpu.disasm", addr=0x00001000, count=1)   # kuseg sin TLB: no mapeada
    check("disasm kuseg no mapeada = 0", d["insns"][0]["op"] == 0, d["insns"][0])
    d, blob = k.query("vi.capture")
    check("vi.capture alto del VI", d.get("height") in (240, 288, 480, 576) or d.get("source") == "gpu",
          {"h": d.get("height"), "w": d.get("width")})
    try:
        d, _ = k.query("state.save")
        check("state.save sin slot se niega", False, d)
    except Exception as e:  # noqa: BLE001
        check("state.save sin slot se niega", "slot" in str(e), str(e))
finally:
    if p.poll() is None:
        p.kill()   # solo nuestro PID
print("VEREDICTO:", "PASS" if fails == 0 else f"FAIL ({fails})")
sys.exit(0 if fails == 0 else 1)
