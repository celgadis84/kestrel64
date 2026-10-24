"""Prueba de humo de las ordenes del depurador (docs/DEBUGGER.md) contra un kestrel vivo.

Lanza SM64 sin ventana en un puerto propio, activa la grabadora, espera unos cuadros y
pasa por cada orden nueva imprimiendo una linea de veredicto por orden. Sale con 0 si
todas responden bien. Uso: python scripts/dbg_smoke.py [--port 9141] [--rom ruta]
"""
import romcfg  # configuracion local: scripts/local.env
import argparse, os, subprocess, sys, time, tempfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools" / "mcp"))

ap = argparse.ArgumentParser()
ap.add_argument("--port", type=int, default=9141)
ap.add_argument("--rom", default=romcfg.rom("ROM_SM64"))
ap.add_argument("--exe", default=str(ROOT / "build" / "kestrel64.exe"))
ap.add_argument("--romswap", default="", help="ROM para probar rom.load (opcional)")
args = ap.parse_args()
args.rom = str(romcfg.need(args.rom, "ROM_SM64"))

os.environ["KESTREL_TELEMETRY_PORT"] = str(args.port)
import kestrel_mcp as k  # noqa: E402
k.PORT = args.port

env = dict(os.environ, KESTREL_AUDIO="0", KESTREL_MAXINSN="4000000000")
p = subprocess.Popen([args.exe, args.rom, "--run", "--port", str(args.port)], env=env,
                     stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
fails = 0
tmp = Path(tempfile.mkdtemp(prefix="k64dbg_"))


def check(name, fn, ok=lambda d: True):
    global fails
    try:
        d = fn()
        good = ok(d)
        short = {kk: d[kk] for kk in list(d)[:6]} if isinstance(d, dict) else d
        print(f"{'OK  ' if good else 'BAD '} {name}: {str(short)[:160]}")
        fails += 0 if good else 1
        return d
    except Exception as e:  # noqa: BLE001
        print(f"FAIL {name}: {e}")
        fails += 1
        return None


def _rsp():
    b = tmp / "dmem.bin"
    b.write_bytes(bytes(4))
    try:
        return k.rsp_load(str(b), mem="dmem", offset=0xff0)
    except k.KestrelError as e:
        return {"refused": str(e)}


try:
    for _ in range(50):
        try:
            k.emu_status(); break
        except Exception:  # noqa: BLE001
            time.sleep(0.2)
    check("dbg_enable", lambda: k.dbg_enable(True), lambda d: d["on"])
    time.sleep(3)
    st = check("dbg_status", k.dbg_status, lambda d: d["framesClosed"] > 0)
    check("dbg_rdp_frame", lambda: k.dbg_rdp_frame(count=5), lambda d: d["total"] > 0 and len(d["cmds"]) > 0)
    check("dbg_rdp_frame prims", lambda: k.dbg_rdp_frame(count=3, prims=True), lambda d: all("name" in c for c in d["cmds"]))
    check("dbg_rsp_tasks", k.dbg_rsp_tasks, lambda d: len(d["tasks"]) > 0)
    check("dbg_timeline", k.dbg_timeline, lambda d: d["span"] > 0)
    check("dbg_fb color", lambda: k.dbg_fb(str(tmp / "c.png")), lambda d: d["nonzero"] > 0)
    check("dbg_fb depth", lambda: k.dbg_fb(str(tmp / "z.png"), kind="depth"), lambda d: d["width"] > 0)
    check("dbg_fb coverage", lambda: k.dbg_fb(str(tmp / "cv.png"), kind="coverage"), lambda d: d["width"] > 0)
    check("dbg_tmem", k.dbg_tmem, lambda d: d["bytes"] == 4096 and len(d["tiles"]) == 8)
    check("cpu_tlb", k.cpu_tlb, lambda d: len(d["entries"]) == 32)
    check("event_log", lambda: k.event_log(10, on=True), lambda d: "events" in d)
    check("usb_log", k.usb_log, lambda d: "packets" in d)
    # Observacion de escritura sobre una palabra que SM64 toca cada cuadro: el contador de
    # cuadros de la VI no vale (lo escribe el VI, no la CPU); se usa la pila del hilo de
    # juego, que la CPU escribe sin parar. Basta con que algo pare: rango amplio.
    check("mem_watch arm", lambda: k.mem_watch(0x100000, 0x200000, write=True), lambda d: d["armed"])
    time.sleep(1.0)
    check("mem_watch hit", k.mem_watch, lambda d: d["hit"] and d["paused"])
    check("mem_watch clear", lambda: k.mem_watch(clear=True), lambda d: not d["armed"])
    # code.load: bin crudo a una zona libre alta y comprobacion por lectura coherente.
    blob = tmp / "code.bin"
    blob.write_bytes(bytes.fromhex("3c08dead3508beef"))   # lui t0,0xdead ; ori t0,t0,0xbeef
    check("code_load raw", lambda: k.code_load(str(blob), addr=0x803ff000), lambda d: d["bytes"] == 8)
    check("read back", lambda: k.read_memory("RDRAM", 0x3ff000, 8, coherent=True), lambda d: d["hex"] == "3c08dead3508beef")
    check("rsp_load (corriendo: debe negarse o ir)", lambda: _rsp(), lambda d: True)
    k.run_control("resume")
    if args.romswap:
        check("rom_load", lambda: k.rom_load(args.romswap), lambda d: "path" in d)
        time.sleep(3)
        check("reconnect", k.emu_status, lambda d: True)
finally:
    time.sleep(0.2)
    if p.poll() is None:
        p.kill()   # solo nuestro PID
print("VEREDICTO:", "PASS" if fails == 0 else f"FAIL ({fails})")
sys.exit(0 if fails == 0 else 1)
