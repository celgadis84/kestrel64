# Prueba de estado guardado. El oraculo es la propia maquina: se guarda en un punto, se
# deja correr medio segundo mas para que el estado cambie de verdad, se carga, y todo lo
# que se puede leer por telemetria (registros de CPU/RCP/RSP, trozos de RDRAM, el
# framebuffer) tiene que volver a ser IDENTICO a lo que habia al guardar. Despues se
# reanuda y se comprueba que la maquina sigue viva (el VI sigue intercambiando buffer):
# un estado que restaura los bytes pero deja el RCP muerto pasaria la primera mitad.
import hashlib, os, subprocess, sys, time
sys.path.insert(0, os.path.join(os.path.dirname(__file__), "..", "tools", "mcp"))
import kestrel_mcp as k

ROM  = sys.argv[1] if len(sys.argv) > 1 else r"E:\Claude\N64\test_roms\sm64.z64"
EXE  = os.path.join(os.path.dirname(__file__), "..", "build", "kestrel64.exe")
SLOT = 9

def flips():
    return k.query("rcp.regs")[0]["vi"]["flips"]

def wait_flips(n, timeout=90):
    t0 = time.time()
    while time.time() - t0 < timeout:
        if flips() >= n:
            return True
        time.sleep(0.05)
    return False

def snapshot():
    """Todo el estado observable, aplanado a algo comparable."""
    s = {}
    s["cpu"] = k.query("cpu.regs")[0]
    s["rcp"] = k.query("rcp.regs")[0]
    s["rsp"] = k.query("rsp.regs", vpr=True)[0]
    h = hashlib.md5()
    for off in (0x00000000, 0x00100000, 0x00300000, 0x00600000):
        _, blob = k.query("mem.read", region="rdram", addr=off, len=0x10000)
        h.update(blob)
    s["ram"] = h.hexdigest()
    try:
        _, fb = k.query("vi.capture")
        s["fb"] = hashlib.md5(fb).hexdigest()
    except k.KestrelError:
        s["fb"] = "blank"
    return s

def diff(a, b, path=""):
    out = []
    if type(a) is not type(b):
        return [f"{path}: tipo {type(a)} != {type(b)}"]
    if isinstance(a, dict):
        for key in set(a) | set(b):
            if key in ("halted", "haltReason"):
                continue
            if key not in a or key not in b:
                out.append(f"{path}.{key}: falta en un lado")
            else:
                out += diff(a[key], b[key], f"{path}.{key}")
    elif isinstance(a, list):
        if len(a) != len(b):
            return [f"{path}: longitud {len(a)} != {len(b)}"]
        for i, (x, y) in enumerate(zip(a, b)):
            out += diff(x, y, f"{path}[{i}]")
    elif a != b:
        out.append(f"{path}: {a!r} != {b!r}")
    return out

env = dict(os.environ)
env.update(KESTREL_THROTTLE="0", KESTREL_NOVIDEO="1")
# El .exe se enlaza en dinamico contra libc++ de clang64: sin su bin en el PATH no arranca.
env["PATH"] = os.path.join('C:', os.sep, 'msys64', 'clang64', 'bin') + os.pathsep + env.get("PATH", "")
def launch(threads: str):
    p = subprocess.Popen([EXE, ROM, "--run"], env=dict(env, KESTREL_THREADS=threads),
                         stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    for _ in range(150):
        try:
            k.query("ping"); return p
        except k.KestrelError:
            time.sleep(0.2)
    p.kill()
    raise SystemExit("FALLO: la telemetria no levanto")

proc = launch("1")
rc = 1
try:
    path = None
    assert wait_flips(40), "el juego no llego a 40 intercambios de buffer"
    k.query("pause")
    # El estado se toma DESPUES de guardar, no antes: guardar aquieta el RCP (drena la cola
    # del RDP y termina la tarea del RSP), y eso hace avanzar la maquina -- un SYNC_FULL mas,
    # la interrupcion de DP, los pixeles que faltaban en RDRAM. La foto de referencia tiene
    # que ser la de lo que se ha ESCRITO al fichero, que es el estado ya aquietado.
    d = k.query("state.save", slot=SLOT)[0]
    path = d["path"]
    before = snapshot()
    print(f"[1] guardado en {path} ({os.path.getsize(path)/1e6:.1f} MB)")

    k.query("resume")
    assert wait_flips(before["rcp"]["vi"]["flips"] + 40), "no avanzo tras guardar"
    k.query("pause")
    moved = snapshot()
    assert diff(before, moved), "el estado no cambio entre guardar y cargar: prueba vacia"
    print("[2] la maquina avanzo (el estado difiere)")

    k.query("state.load", slot=SLOT)
    after = snapshot()
    d = diff(before, after)
    if d:
        print(f"FALLO: {len(d)} diferencias tras cargar")
        for line in d[:20]:
            print("   ", line)
        raise SystemExit(1)
    print("[3] estado restaurado identico (CPU+RCP+RSP+RDRAM+framebuffer)")

    base = flips()
    k.query("resume")
    assert wait_flips(base + 20, timeout=30), "la maquina no sigue viva tras cargar"
    print("[4] sigue corriendo tras cargar")

    # Portabilidad entre modos de RCP. El estado se toma con el RCP aquietado justamente
    # para esto: el mismo fichero tiene que cargar en Lockstep y en Threaded. Si no, el
    # emulador tendria dos formatos y un savestate valdria solo para el modo que lo hizo.
    k.query("pause")
    k.query("state.save", slot=SLOT)
    ref = snapshot()
    proc.terminate(); proc.wait(timeout=10)
    k._drop()
    proc = launch("0")
    k.query("pause")
    k.query("state.load", slot=SLOT)
    other = snapshot()
    d = diff(ref, other)
    if d:
        print(f"FALLO: {len(d)} diferencias al cargar en Lockstep un estado de Threaded")
        for line in d[:20]:
            print("   ", line)
        raise SystemExit(1)
    base = flips()
    k.query("resume")
    assert wait_flips(base + 20, timeout=60), "no arranca en Lockstep tras cargar"
    print("[5] estado de Threaded cargado en Lockstep: identico y corriendo")

    print("ALL PASS")
    rc = 0
finally:
    try:
        k.query("pause")
    except Exception:
        pass
    proc.terminate()
    try:
        proc.wait(timeout=10)
    except subprocess.TimeoutExpired:
        proc.kill()
    if path and os.path.exists(path):
        os.remove(path)
raise SystemExit(rc)
