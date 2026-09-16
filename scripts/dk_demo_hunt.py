#!/usr/bin/env python3
"""Cazar el negro intermitente de DK64 en las demos de gameplay.

Como lo ve el usuario: logos -> intro rapeada (se salta con START) -> demos de gameplay.
Al acabar la primera demo (la de la liana) a veces se queda en NEGRO en vez de saltar a la
de las vagonetas; otras veces salta bien. Intermitente = no es un fallo determinista del
invitado, sale del reparto real entre hilos, asi que hay que correrlo muchas veces en la
configuracion de verdad (hilos + GPU) y vigilar.

Que mira, cada muestra:
  - origin del VI (si deja de moverse, el juego dejo de intercambiar buffer),
  - colores distintos del framebuffer (1-2 = pantalla plana, negro),
  - instrucciones retiradas y sondeos del mando (dicen si el invitado SIGUE corriendo).

Un negro con el invitado vivo y sondeando el mando es "se quedo en negro"; un negro con
las instrucciones paradas es un cuelgue de otra clase. Distinguirlos es el objetivo.

  python scripts/dk_demo_hunt.py --runs 6 --minutes 5
"""
import argparse, os, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools", "mcp"))
import kestrel_mcp as k  # noqa: E402

ROM = r"E:\Claude\N64\test_roms\Donkey Kong 64 (USA).n64"


def snap(png):
    st = k.emu_status()
    cap = k.capture_framebuffer(png)
    return dict(origin=cap.get("origin"), colors=cap.get("distinctColors", -1),
                insns=st.get("speed", {}).get("insns", 0),
                polls=st.get("pad", {}).get("polls", 0),
                fps=st.get("speed", {}).get("occupancy", {}).get("fps", 0))


def one_run(idx, minutes, exe, env, period, intro):
    png = os.path.join(ROOT, "out", f"dk_hunt{idx}.png")
    proc = subprocess.Popen([exe, ROM, "--mcp", "--run"], env=env,
                            stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
    try:
        # El backend de GPU tarda en levantar; esperar al puerto en vez de dormir a ojo.
        t0 = time.time()
        while time.time() - t0 < 40:
            try:
                k.emu_status(); break
            except Exception:
                time.sleep(1.0)
        else:
            return "el emulador no levanto la telemetria"
        # UNA sola pulsacion de START: salta la intro rapeada y deja el titulo. Mas
        # pulsaciones entran en el menu de partida y ahi NO salen las demos.
        time.sleep(intro)
        try: k.controller_set(buttons="start", polls=8)
        except Exception: pass
        t0 = time.time()
        prev = None
        frozen = dark = 0
        worst_dark = worst_frozen = 0
        while time.time() - t0 < minutes * 60:
            try:
                s = snap(png)
            except Exception as e:
                return f"telemetria caida a los {time.time()-t0:.0f}s: {e}"
            same_origin = prev is not None and s["origin"] == prev["origin"]
            alive = prev is None or s["insns"] > prev["insns"]
            frozen = frozen + 1 if same_origin else 0
            dark = dark + 1 if s["colors"] <= 2 else 0
            worst_dark = max(worst_dark, dark); worst_frozen = max(worst_frozen, frozen)
            print(f"  run{idx} t={time.time()-t0:6.0f}s origin=0x{s['origin']:06x} "
                  f"col={s['colors']:5d} fps={s['fps']:3d} vivo={int(alive)} "
                  f"negro={dark} congelado={frozen}", flush=True)
            if dark >= 8 or frozen >= 8:
                sev = "NEGRO CON INVITADO VIVO" if alive else "PARADO DEL TODO"
                return f"SOSPECHA a los {time.time()-t0:.0f}s: {sev} (negro={dark}, congelado={frozen})"
            prev = s
            time.sleep(period)
        return f"ok (racha maxima negro={worst_dark}, congelado={worst_frozen})"
    finally:
        proc.kill()
        time.sleep(1.0)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs", type=int, default=6)
    ap.add_argument("--minutes", type=float, default=5.0)
    ap.add_argument("--period", type=float, default=3.0)
    ap.add_argument("--exe", default=os.path.join(ROOT, "build-prdp", "kestrel64.exe"))
    ap.add_argument("--prdp", type=int, default=1)
    ap.add_argument("--intro", type=float, default=10.0,
                    help="segundos antes de pulsar START para saltar la intro rapeada")
    args = ap.parse_args()
    env = dict(os.environ)
    if args.prdp: env["KESTREL_PRDP"] = "1"
    # El .exe de GPU carga DLL del toolchain: sin este PATH arranca y muere sin decir nada.
    env["PATH"] = r"C:\msys64\clang64\bin;" + env.get("PATH", "")
    for i in range(args.runs):
        print(f"== run {i} ({args.minutes} min)", flush=True)
        print(f"== run {i}: {one_run(i, args.minutes, args.exe, env, args.period, args.intro)}", flush=True)


if __name__ == "__main__":
    sys.exit(main() or 0)
