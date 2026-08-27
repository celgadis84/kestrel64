#!/usr/bin/env python3
"""Agrega un volcado crudo de hostprof por funcion.

    KESTREL_HOSTPROF=1 KESTREL_HOSTPROF_WHO=cpu KESTREL_HOSTPROF_OUT=prof.txt ./kestrel64.exe ...
    python scripts/hostprof_sym.py prof.txt build-prof-prdp/kestrel64.exe

El perfilador solo sabe de cubos de 16 bytes dentro de la imagen; una funcion caliente sale
repartida en veinte lineas del 1%. Aqui se simboliza el histograma entero con
llvm-symbolizer (una sola invocacion, direcciones por stdin) y se suma por funcion.

Necesita un binario con DWARF (-g -gdwarf-4): los build-prof/*. Con LTO fina y sin DWARF
la tabla de simbolos que queda es de 725 entradas y el "simbolo mas cercano" miente.
"""
import subprocess, sys, collections

PREFERRED_BASE = 0x140000000     # base preferida del PE; los cubos son RVA


def symbolize(exe, rvas):
    if not rvas:
        return {}
    inp = "".join("0x%x\n" % (PREFERRED_BASE + r) for r in rvas)
    p = subprocess.run(["llvm-symbolizer", "--obj=" + exe, "--functions=short",
                        "--output-style=LLVM"],
                       input=inp, capture_output=True, text=True)
    out, res, cur = p.stdout.splitlines(), {}, []
    i = 0
    for rva in rvas:
        fn, loc = "??", ""
        # cada direccion produce N pares (funcion, fichero:linea) y una linea en blanco
        while i < len(out) and out[i].strip():
            if not fn or fn == "??":
                fn = out[i].strip()
            if i + 1 < len(out):
                loc = out[i + 1].strip()
            i += 2
        i += 1
        res[rva] = (fn, loc)
    return res


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 1
    raw, exe = sys.argv[1], sys.argv[2]
    img, extcall, extmod, samples = {}, {}, {}, 0
    jit = {}   # codigo emitido por el dynarec: fuera de la imagen, sin simbolo posible
    for line in open(raw):
        if line.startswith("# samples"):
            samples = int(line.split()[2]); continue
        k, a, c = line.split()
        {"img": img, "extcall": extcall, "extmod": extmod, "jit": jit}[k][int(a, 16)] = int(c)
    n = max(samples, 1)

    syms = symbolize(exe, list(img.keys()) + list(extcall.keys()))
    agg, aggloc = collections.Counter(), {}
    for rva, c in img.items():
        fn, loc = syms.get(rva, ("??", ""))
        agg[fn] += c
        aggloc.setdefault(fn, loc)
    ext_total = sum(extmod.values())
    jit_total = sum(jit.values())

    print("muestras: %d   dentro de imagen: %.1f%%   fuera: %.1f%%   (de la cual, codigo JIT: %.1f%%)"
          % (n, 100.0 * sum(img.values()) / n, 100.0 * ext_total / n, 100.0 * jit_total / n))
    print("\n-- por funcion (dentro de la imagen) --")
    for fn, c in agg.most_common(30):
        print("%6.2f%%  %-55s %s" % (100.0 * c / n, fn[:55], aggloc.get(fn, "")))

    if extcall:
        print("\n-- fuera de la imagen, por quien llamo --")
        callagg, callloc = collections.Counter(), {}
        for rva, c in extcall.items():
            fn, loc = syms.get(rva, ("??", ""))
            callagg[fn] += c
            callloc.setdefault(fn, loc)
        for fn, c in callagg.most_common(15):
            print("%6.2f%%  %-55s %s" % (100.0 * c / n, fn[:55], callloc.get(fn, "")))
    return 0


if __name__ == "__main__":
    sys.exit(main())
