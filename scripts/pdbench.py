#!/usr/bin/env python3
"""
Banco A/B reproducible de Perfect Dark en un nivel real (PD64_pending P6).

Arranca la ROM PAUSADA y la hace avanzar por campos de video (frame_advance): espera al
titulo (g_StageNum == 0x5A) + 240 campos, escribe g_MissionConfig + g_MainChangeToStageNum
(el mismo camino que lv.c usa para la cutscene automatica), espera a que el nivel cargue,
deja pasar `--skip` ticks de 240 Hz del propio juego y mide `--measure` ticks mas (en
tandas de 120 campos). Todo punto de decision cae en un borde de campo de INVITADO, asi que
en lockstep dos corridas del mismo binario dan la misma cifra. Direcciones SIEMPRE del pd.map del build (cada rama mueve el codigo), nunca de un
savestate.

Salida: fps de juego (contadores del juego: 240 * d(lvframenum) / d(lvframe240), no depende
del anfitrion), rdp.stats de la ventana, GCLK del RDP por fotograma de juego y, en modo
threaded, segundos de pared.

Uso:
  python scripts/pdbench.py <rom> [--map pd.map] [--stage villa|defection|0x2c]
                            [--mode lockstep|threaded] [--skip 720] [--measure 2400]
  --map por defecto: pd.map junto a la ROM, si no ../perfect_dark/build/ntsc-final/pd.map
"""
import argparse, hashlib, json, os, re, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools", "mcp"))

STAGES = {"defection": (0x30, 0x00), "villa": (0x2c, 0x03)}   # stagenum, stageindex (solo)
STAGE_TITLE = 0x5A
# offsets dentro de g_Vars (src/include/types.h, struct g_vars)
OFF_LVFRAMENUM, OFF_LVFRAME240 = 0x00c, 0x030


def load_map(path, names):
    syms = {}
    rx = re.compile(r"^\s+0x([0-9a-fA-F]{8,16})\s+(\S+)\s*$")
    with open(path, errors="replace") as f:
        for line in f:
            m = rx.match(line)
            if m and m.group(2) in names:
                syms[m.group(2)] = int(m.group(1), 16) & 0x1fffffff
    miss = [n for n in names if n not in syms]
    if miss:
        sys.exit(f"pdbench: {path} no tiene {miss}")
    return syms


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("rom")
    ap.add_argument("--map")
    ap.add_argument("--stage", default="villa")
    ap.add_argument("--mode", default="lockstep", choices=["lockstep", "threaded"])
    ap.add_argument("--skip", type=int, default=720)
    ap.add_argument("--measure", type=int, default=2400)
    ap.add_argument("--exe", default=os.path.join(ROOT, "build-prdp-static", "kestrel64.exe"))
    ap.add_argument("--port", type=int, default=9131)
    ap.add_argument("--intro", default="skip", choices=["skip", "keep"],
                    help="skip = START salta la cutscena de intro y se mide jugando (defecto)")
    ap.add_argument("--png", help="guarda el fotograma del INICIO y del FIN de la ventana "
                    "(<png>_0.png, <png>_1.png) para ver que escena se midio")
    ap.add_argument("--timeout", type=int, default=900, help="tope de pared total, s")
    a = ap.parse_args()

    if a.stage in STAGES:
        stagenum, stageindex = STAGES[a.stage]
    else:
        stagenum, stageindex = int(a.stage, 0), 0
    mp = a.map or os.path.join(os.path.dirname(os.path.abspath(a.rom)), "pd.map")
    if not os.path.exists(mp):
        mp = os.path.join(ROOT, "..", "perfect_dark", "build", "ntsc-final", "pd.map")
        # Cada rama mueve los simbolos: un map de otro build lee direcciones basura y el
        # banco se queda esperando un titulo que nunca ve. Avisar siempre.
        print(f"pdbench: AVISO sin --map, uso {mp}; si la ROM no es ese build, fallara",
              file=sys.stderr)
    syms = load_map(mp, ["g_StageNum", "g_MissionConfig", "g_MainChangeToStageNum", "g_Vars"])
    with open(a.rom, "rb") as f:
        md5 = hashlib.md5(f.read()).hexdigest()

    env = dict(os.environ)
    env.update({"KESTREL_SPEEDMODE": "hw", "KESTREL_SPLEAD": "0",
                "KESTREL_THROTTLE": "0"})   # con ventana el modo fiel limita a 59,94 Hz
    if a.mode == "lockstep":
        env.update({"KESTREL_THREADS": "0", "KESTREL_JIT": "1"})
    else:
        env.update({"KESTREL_THREADS": "1", "KESTREL_JIT": "1"})
    os.environ["KESTREL_TELEMETRY_PORT"] = str(a.port)
    import kestrel_mcp as k

    # SIN --run: arranca pausado y el warp se hace avanzando CAMPOS de video (frame_advance),
    # asi el instante en que se escribe g_MissionConfig es de INVITADO y lockstep repite.
    proc = subprocess.Popen([a.exe, a.rom, "--port", str(a.port)], env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    t0 = time.time()
    deadline = t0 + a.timeout

    def rd32(phys):
        return int(k.read_memory("rdram", hex(phys), 4, coherent=True)["hex"], 16)

    def wait(pred, what, limit):
        end = min(deadline, time.time() + limit)
        while time.time() < end:
            if proc.poll() is not None:
                sys.exit(f"pdbench: el emulador salio (rc={proc.returncode}) esperando {what}")
            try:
                if pred():
                    return
            except (OSError, RuntimeError, ValueError):
                pass
            time.sleep(0.25)
        proc.kill()
        sys.exit(f"pdbench: TIMEOUT esperando {what}")

    try:
        stage = lambda: rd32(syms["g_StageNum"])
        wait(lambda: k.emu_status() is not None, "servidor MCP", 30)
        fields = 0
        while stage() != STAGE_TITLE:
            if time.time() > deadline or fields > 20000:
                proc.kill()
                sys.exit("pdbench: TIMEOUT esperando titulo (g_StageNum == 0x5A)")
            k.frame_advance(30, timeout_ms=60000)
            fields += 30
        # dejar que el titulo termine de montarse: 240 campos = 4 s de invitado (mismo
        # margen que usaba PD-opt en pared)
        k.frame_advance(240, timeout_ms=120000)
        # g_MissionConfig limpio: difficulty 0 (Agent), stagenum, stageindex, sin coop/anti,
        # pdmode a cero. Sin esto el cambio de nivel crashea (ver memoria autowarp).
        cfg = bytes([0, stagenum, stageindex]) + bytes(17)
        k.write_memory("rdram", hex(syms["g_MissionConfig"]), cfg.hex())
        k.write_memory("rdram", hex(syms["g_MainChangeToStageNum"]), f"{stagenum:08x}")
        adv = lambda n: k.frame_advance(n, timeout_ms=int(max(1, deadline - time.time()) * 1000))
        fields = 0
        while stage() != stagenum:
            if time.time() > deadline or fields > 20000:
                proc.kill()
                sys.exit(f"pdbench: TIMEOUT esperando nivel 0x{stagenum:x}")
            adv(30)
            fields += 30
        gv = syms["g_Vars"]
        f240 = lambda: rd32(gv + OFF_LVFRAME240)
        # lvframe240 arrastra el valor del titulo hasta que el nivel lo reinicia: ver primero
        # el reinicio (valor bajo).
        while not (0 < f240() < a.skip):
            if time.time() > deadline:
                proc.kill()
                sys.exit("pdbench: TIMEOUT esperando el reinicio de lvframe240")
            adv(10)
        if a.intro == "skip":
            # El nivel arranca con la cutscena de intro (letterbox): START la salta. La
            # pulsacion dura N lecturas del joybus, no ms, asi que cae en el mismo fotograma
            # de juego en cualquier modo. 60 campos antes para que el juego ya lea el mando.
            adv(60)
            k.controller_set("start", polls=4)
            adv(120)
        base = f240()
        while f240() - base < a.skip:
            if time.time() > deadline:
                proc.kill()
                sys.exit(f"pdbench: TIMEOUT esperando {a.skip} ticks de nivel")
            adv(30)

        # Ventana medida: se abre y se cierra con el core PARADO en un borde de campo.
        if a.png:
            k.capture_framebuffer(f"{a.png}_0.png")
        k.rdp_stats("reset")
        n0, s0, w0 = rd32(gv + OFF_LVFRAMENUM), f240(), time.time()
        while f240() - s0 < a.measure:
            if time.time() > deadline:
                proc.kill()
                sys.exit(f"pdbench: TIMEOUT midiendo {a.measure} ticks")
            adv(120)
        n1, s1, w1 = rd32(gv + OFF_LVFRAMENUM), f240(), time.time()
        st = k.rdp_stats("read")
        k.rdp_stats("off")
        if a.png:
            k.capture_framebuffer(f"{a.png}_1.png")
    finally:
        if proc.poll() is None:
            proc.kill()

    frames = n1 - n0
    ticks = s1 - s0
    g = st["gclk"]
    ops = st["ops"]
    red = st["redundant"]
    per = lambda v: v / frames if frames else 0.0
    out = {
        "rom": os.path.basename(a.rom), "md5": md5[:8], "map": mp, "stage": hex(stagenum),
        "mode": a.mode, "intro": a.intro, "ticks": ticks, "frames": frames,
        "fps": 240.0 * frames / ticks if ticks else 0.0,
        "wall_s": round(w1 - w0, 2),
        "per_frame": {
            "gclk": per(g["total"]), "gclk_pixel": per(g["pixel"]), "gclk_fill": per(g["fill"]),
            "gclk_tmem": per(g["tmem"]), "gclk_sync": per(g["sync"]),
            "tris": per(sum(v for n, v in ops.items() if n.startswith("TRI_"))),
            "sync_pipe": per(ops.get("SYNC_PIPE", 0)), "sync_pipe_red": per(red["syncPipe"]),
            "sync_pipe_pure": per(red.get("syncPipePure", 0)),
            "sync_load": per(ops.get("SYNC_LOAD", 0)), "sync_load_red": per(red["syncLoad"]),
            "sync_load_pure": per(red.get("syncLoadPure", 0)),
            "loads": per(red["loads"]), "loads_red": per(red["loadRedundant"]),
            "px_imrd": per(st["pixels"]["imRd"]), "px_zcmp": per(st["pixels"]["zCmp"]),
            "px_written": per(st["pixels"]["written"]),
            "px_1cyc": per(st["pixels"]["1cyc"]), "px_2cyc": per(st["pixels"]["2cyc"]),
            "other_modes": per(ops.get("SET_OTHER_MODES", 0)),
            "other_modes_same": per(red["otherModesSame"]),
        },
        # Tiempo de INVITADO del RDP: GCLK a 62,5 MHz frente a la duracion del fotograma de
        # juego. rdpBusyPct del latido es PARED del hilo anfitrion y no sirve para esto.
        "rdp_ms_per_frame": per(g["total"]) / 62.5e3,
        "frame_ms": (1000.0 * ticks / 240.0 / frames) if frames else 0.0,
        "rdp_stats": st,
    }
    out["rdp_frame_pct"] = (100.0 * out["rdp_ms_per_frame"] / out["frame_ms"]
                            if out["frame_ms"] else 0.0)
    print(json.dumps(out, indent=1))
    p = out["per_frame"]
    print(f"| {out['rom']} | {out['md5']} | {out['stage']} | {a.mode} | {out['fps']:.2f} | "
          f"{p['gclk']:.0f} ({out['rdp_ms_per_frame']:.2f} ms = {out['rdp_frame_pct']:.0f}%) | "
          f"{p['px_imrd']:.0f} | {p['gclk_sync']:.0f} | {p['gclk_tmem']:.0f} | {p['tris']:.0f} | "
          f"{p['sync_pipe']:.1f} ({p['sync_pipe_red']:.1f}/{p['sync_pipe_pure']:.1f}) | "
          f"{p['sync_load']:.1f} ({p['sync_load_red']:.1f}/{p['sync_load_pure']:.1f}) | "
          f"{p['loads']:.1f} ({p['loads_red']:.1f}) | {out['wall_s']} |")


if __name__ == "__main__":
    main()
