# -*- coding: utf-8 -*-
"""Genera src/ui/optdefs.cpp a partir del catalogo del lanzador (tools/launcher/options.py).

El catalogo vive en un solo sitio. Este generador lo vuelca a una tabla estatica de C++ para
que el menu DENTRO de la ventana del emulador ensene exactamente las mismas opciones que el
lanzador, sin transcribirlas a mano (que es como se desincronizan).
"""
import io, os, sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "launcher"))
import options as OPT

# Opciones que el emulador sabe cambiar EN CALIENTE. El resto exige relanzar el proceso, y el
# menu lo dice en vez de fingir que se aplico.
LIVE = {
  "winscale", "winsize", "fullscreen", "hud", "audio", "volume", "throttle", "aspect",
}

TYPE = {"bool": "Bool", "int": "Int", "float": "Float", "text": "Text",
        "path": "Path", "choice": "Choice", "hex": "Hex"}


FOLD = {}
for _a, _b in zip(u'áéíóúñ¿¡—–',
                  u'aeioun?!--'):
    FOLD[_a] = _b


def cstr(s):
    if s is None:
        return "nullptr"
    s = str(s)
    out = []
    for ch in s:
        if ch == chr(34):
            out.append(chr(92) + chr(34))
        elif ch == chr(92):
            out.append(chr(92) + chr(92))
        elif ch == chr(10):
            out.append(chr(92) + 'n')
        elif ord(ch) < 128:
            out.append(ch)
        else:
            # El fichero se compila como latin-1; se degradan los pocos no-ASCII que haya.
            out.append(FOLD.get(ch, '?'))
    return chr(34) + ''.join(out) + chr(34)


def defstr(o):
    d = o["default"]
    t = o["type"]
    if d is None:
        return cstr("")
    if t == "bool":
        return cstr("1" if d else "0")
    if t == "float":
        return cstr(("%.4f" % float(d)).rstrip("0").rstrip("."))
    return cstr(str(d))


def main():
    w = io.StringIO()
    w.write("// GENERADO por tools/gen_optdefs.py desde tools/launcher/options.py. NO EDITAR A MANO.\n")
    w.write("// Regenerar: python tools/gen_optdefs.py\n")
    w.write('#include "optdefs.hpp"\n\nnamespace kestrel::ui {\n\n')

    # tablas de valores de los `choice`
    for c in OPT.CATEGORIES:
        for o in c["options"]:
            if o["type"] == "choice" and o.get("values"):
                w.write("static const Choice kCh_%s[] = {\n" % o["id"])
                for v, l in o["values"]:
                    w.write("  {%s, %s},\n" % (cstr(v), cstr(l)))
                w.write("};\n")
    w.write("\n")

    for c in OPT.CATEGORIES:
        w.write("static const Option kOpt_%s[] = {\n" % c["id"])
        for o in c["options"]:
            ch = "kCh_%s" % o["id"] if (o["type"] == "choice" and o.get("values")) else "nullptr"
            nch = len(o.get("values") or []) if o["type"] == "choice" else 0
            w.write("  {%s, %s, %s, OType::%s, %s, %s, %s, %s, %s, %g, %g, %g, %s, %d, %s},\n" % (
                cstr(o["id"]), cstr(o["env"]), cstr(o["label"]), TYPE[o["type"]],
                defstr(o), cstr(o.get("help") or ""),
                "true" if o.get("adv") else "false",
                "true" if o.get("invert") else "false",
                "true" if o.get("tri") else "false",
                float(o.get("min", 0) or 0), float(o.get("max", 0) or 0),
                float(o.get("step", 0) or 0),
                ch, nch,
                "true" if o["id"] in LIVE else "false"))
        w.write("};\n\n")

    w.write("static const Category kCats[] = {\n")
    for c in OPT.CATEGORIES:
        w.write("  {%s, %s, %s, kOpt_%s, (int)(sizeof kOpt_%s / sizeof(Option))},\n" % (
            cstr(c["id"]), cstr(c["label"]), cstr(c["desc"]), c["id"], c["id"]))
    w.write("};\n\n")
    w.write("auto categories() -> const Category* { return kCats; }\n")
    w.write("auto categoryCount() -> int { return (int)(sizeof kCats / sizeof(Category)); }\n\n")

    w.write("static const PadCtl kPads[] = {\n")
    for b in OPT.PAD_BUTTONS:
        w.write("  {%s, %s, 0x%04x, %s, %s},\n" % (
            cstr(b["id"]), cstr(b["label"]), b["bit"], cstr(b["key"]), cstr(b["gp"])))
    for a in OPT.PAD_AXES:
        w.write("  {%s, %s, 0, %s, %s},\n" % (
            cstr(a["id"]), cstr(a["label"]), cstr(a["key"]), cstr("")))
    w.write("};\n\n")
    w.write("auto padControls() -> const PadCtl* { return kPads; }\n")
    w.write("auto padControlCount() -> int { return (int)(sizeof kPads / sizeof(PadCtl)); }\n\n")
    w.write("}  // namespace kestrel::ui\n")

    dst = os.path.join(os.path.dirname(HERE), "src", "ui", "optdefs.cpp")
    with open(dst, "w", encoding="latin-1", newline="\n") as f:
        f.write(w.getvalue())
    print("escrito %s (%d bytes)" % (dst, len(w.getvalue())))


main()
