# -*- coding: utf-8 -*-
"""Compila los compute shaders del GPU-RDP propio a SPIR-V y los deja en un include.

    python tools/gen_gpurdp_shaders.py

Entrada: src/gpurdp/shaders/*.comp. Salida: src/gpurdp/gpurdp_spv.inc (commiteado), con un
array kSpv_<nombre> por shader. Igual que tools/gen_present_shaders.py: compilar kestrel no
pide glslc, solo hace falta al tocar un shader. glslc sale del PATH o de /c/msys64/clang64/bin.
"""
import os, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SHD = os.path.join(ROOT, "src", "gpurdp", "shaders")
OUT = os.path.join(ROOT, "src", "gpurdp", "gpurdp_spv.inc")


def glslc():
    g = shutil.which("glslc")
    if g:
        return g
    for c in ("C:/msys64/clang64/bin/glslc.exe", "/c/msys64/clang64/bin/glslc.exe"):
        if os.path.exists(c):
            return c
    sys.exit("glslc no encontrado")


def compile_one(src):
    with tempfile.TemporaryDirectory() as td:
        out = os.path.join(td, "o.spv")
        subprocess.run([glslc(), "-O", "--target-env=vulkan1.1", "-fshader-stage=compute",
                        src, "-o", out], check=True)
        data = open(out, "rb").read()
    return [int.from_bytes(data[i:i + 4], "little") for i in range(0, len(data), 4)]


def main():
    names = sorted(f[:-5] for f in os.listdir(SHD) if f.endswith(".comp"))
    lines = ["// GENERADO por tools/gen_gpurdp_shaders.py. NO EDITAR A MANO.",
             "// Regenerar: python tools/gen_gpurdp_shaders.py", ""]
    for n in names:
        words = compile_one(os.path.join(SHD, n + ".comp"))
        lines.append("static const uint32_t kSpv_%s[] = {" % n)
        for i in range(0, len(words), 8):
            lines.append("  " + " ".join("0x%08xu," % w for w in words[i:i + 8]))
        lines.append("};")
        lines.append("")
    with open(OUT, "w", encoding="ascii", newline="\n") as f:
        f.write("\n".join(lines))
    print("ok:", ", ".join(names), "->", os.path.relpath(OUT, ROOT))


if __name__ == "__main__":
    main()
