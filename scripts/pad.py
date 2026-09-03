#!/usr/bin/env python3
"""Guiar el mando 1 de un kestrel64 EN MARCHA desde la linea de ordenes.

Existe porque navegar un menu para llegar a gameplay (Perfect Dark tarda varias pantallas)
es una secuencia larga y repetible: hacerla a mano por el puente MCP cuesta una llamada por
pulsacion. Aqui se escribe la secuencia entera de una vez.

  python scripts/pad.py start:8 wait:60 a:4 wait:30 shot:pd.png
  python scripts/pad.py --status
  python scripts/pad.py --shot fb.png

Pasos:
  <botones>[:<polls>]   pulsa. Nombres: a b z start dup ddown dleft dright l r
                        cup cdown cleft cright, combinables con "+" o ",". polls por
                        defecto 6 (son lecturas del joybus = fotogramas del juego,
                        no milisegundos: valen igual a 30% que a 200% de velocidad).
  stick:<x>,<y>[:polls] palanca analogica, rango propio de N64 -80..80.
  wait:<n>              espera n fotogramas de juego contando campos VI de emu.status.
  shot:<ruta>           captura el framebuffer del VI a PNG.
  status                imprime fps/ocupacion.
"""
import os, sys, time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "tools", "mcp"))
import kestrel_mcp as k  # noqa: E402


def wait_fields(n):
    # emu.status da el contador de campos VI; contar campos y no segundos hace que la
    # espera signifique lo mismo aunque el emulador vaya a otra velocidad.
    st = k.emu_status()
    key = next((x for x in ("viFields", "fields", "viField", "frames") if x in st), None)
    if key is None:                       # sin contador, caer a reloj de pared a 60 Hz
        time.sleep(n / 60.0)
        return
    target = st[key] + n
    t0 = time.time()
    while k.emu_status()[key] < target and time.time() - t0 < 60:
        time.sleep(0.05)


def main(argv):
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__)
        return 0
    if argv[0] == "--status":
        print(k.emu_status()); return 0
    if argv[0] == "--shot":
        print(k.capture_framebuffer(argv[1] if len(argv) > 1 else "kestrel_fb.png")); return 0

    for step in argv:
        head, _, tail = step.partition(":")
        if head == "wait":
            wait_fields(int(tail))
        elif head == "shot":
            print(k.capture_framebuffer(tail or "kestrel_fb.png"))
        elif head == "status":
            print(k.emu_status())
        elif head == "stick":
            xy, _, polls = tail.partition(":")
            x, _, y = xy.partition(",")
            k.controller_set("", int(x), int(y or 0), int(polls or 6))
        else:
            k.controller_set(head, 0, 0, int(tail or 6))
            wait_fields(int(tail or 6) + 2)   # dejar que el juego vea el flanco de suelta
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
