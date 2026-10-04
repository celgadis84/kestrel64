# -*- coding: utf-8 -*-
"""Compila los pases de posproceso del presentador a SPIR-V y los deja en un include.

    python tools/gen_present_shaders.py

Entrada: src/video/shaders/*.comp (pases propios + FSR) y los shaders de Anime4K en el
formato de usuario de mpv (src/video/shaders/anime4k/*.glsl), que se traducen aqui a
compute shaders con la misma cabecera comun.
Salida: src/video/postfx_spv.inc (commiteado). Asi compilar kestrel no pide glslc; solo
hace falta al tocar un shader. glslc sale del PATH o de /c/msys64/clang64/bin.

Traduccion de mpv: cada bloque //!DESC ... es un pase. //!BIND NAME enlaza la textura NAME
(MAIN = el cuadro del invitado) a t0, t1, ... en orden; //!SAVE da nombre a la salida y
//!WIDTH/HEIGHT su tamano (solo "X.w" o "X.w 2 *"). Para cada NAME enlazado se definen
NAME_tex(p), NAME_pos, NAME_size, NAME_pt y NAME_texOff(o) como los define mpv. El cuerpo
(defines go_*/g_* y hook()) se copia tal cual.
"""
import os, re, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SHD = os.path.join(ROOT, "src", "video", "shaders")
OUT = os.path.join(ROOT, "src", "video", "postfx_spv.inc")

# pases propios: nombre del fichero sin extension
OWN = ["sharp", "easu", "rcas", "crt", "cel", "kuwahara"]
# redes de Anime4K: (prefijo, fichero)
A4K = [("a4k_m", "Anime4K_Upscale_CNN_x2_M.glsl")]


def glslc():
    g = shutil.which("glslc")
    if g:
        return g
    for c in ("C:/msys64/clang64/bin/glslc.exe", "/c/msys64/clang64/bin/glslc.exe"):
        if os.path.exists(c):
            return c
    sys.exit("glslc no encontrado")


def parse_mpv(path):
    passes = []
    cur = None
    with open(path, encoding="utf-8") as f:
        lines = f.read().split("\n")
    header = []
    for ln in lines:
        if ln.startswith("//!DESC"):
            cur = dict(desc=ln[8:].strip(), binds=[], save=None, w=None, body=[])
            passes.append(cur)
            continue
        if cur is None:
            header.append(ln)
            continue
        if ln.startswith("//!"):
            key, _, val = ln[3:].partition(" ")
            val = val.strip()
            if key == "BIND":
                cur["binds"].append(val)
            elif key == "SAVE":
                cur["save"] = val
            elif key == "WIDTH":
                cur["w"] = val
            continue
        cur["body"].append(ln)
    return header, passes


def scale_of(w):
    # "X.w" -> 1, "X.w 2 *" -> 2
    t = w.split()
    if len(t) == 1:
        return t[0][:-2], 1
    if len(t) == 3 and t[2] == "*":
        return t[0][:-2], int(t[1])
    sys.exit("WIDTH no soportado: " + w)


def mpv_to_comp(header, p):
    lic = "\n".join(l for l in header if l.startswith("//"))
    s = ["#version 450", "#extension GL_GOOGLE_include_directive : require",
         '#include "common.glsl"', "",
         "// GENERADO por tools/gen_present_shaders.py desde un shader de Anime4K (mpv).",
         "// " + p["desc"], lic, "", "vec2 g_pos;"]
    for i, n in enumerate(p["binds"]):
        t = "t%d" % i
        s.append("#define %s_tex(p) texture(%s, (p))" % (n, t))
        s.append("#define %s_pos g_pos" % n)
        s.append("#define %s_size vec2(textureSize(%s, 0))" % (n, t))
        s.append("#define %s_pt (1.0 / %s_size)" % (n, n))
        s.append("#define %s_texOff(o) texture(%s, g_pos + %s_pt * (o))" % (n, t, n))
    s += p["body"]
    s += ["void main() {", "  ivec2 ip;", "  if(!outPixel(ip)) return;",
          "  g_pos = outUv(ip);", "  imageStore(outImg, ip, hook());", "}", ""]
    return "\n".join(s)


def compile_comp(gc, src_text, name, tmp):
    src = os.path.join(tmp, name + ".comp")
    with open(src, "w", encoding="utf-8", newline="\n") as f:
        f.write(src_text)
    spv = os.path.join(tmp, name + ".spv")
    subprocess.check_call([gc, "-fshader-stage=compute", "--target-env=vulkan1.0", "-O",
                           "-I", SHD, "-o", spv, src])
    with open(spv, "rb") as f:
        b = f.read()
    assert len(b) % 4 == 0
    return [int.from_bytes(b[i:i + 4], "little") for i in range(0, len(b), 4)]


def main():
    gc = glslc()
    blobs = []
    chains = []
    with tempfile.TemporaryDirectory() as tmp:
        for n in OWN:
            with open(os.path.join(SHD, n + ".comp"), encoding="utf-8") as f:
                blobs.append((n, compile_comp(gc, f.read(), n, tmp)))
        for pre, fn in A4K:
            header, passes = parse_mpv(os.path.join(SHD, "anime4k", fn))
            chain = []
            for i, p in enumerate(passes):
                name = "%s_%d" % (pre, i)
                blobs.append((name, compile_comp(gc, mpv_to_comp(header, p), name, tmp)))
                ref, sc = scale_of(p["w"])
                chain.append((name, p["save"], ref, sc, p["binds"]))
            chains.append((pre, chain))

    w = []
    w.append("// GENERADO por tools/gen_present_shaders.py. NO EDITAR A MANO.")
    w.append("// Regenerar: python tools/gen_present_shaders.py")
    w.append("// FSR 1.0 (easu, rcas): Copyright (c) 2021 Advanced Micro Devices, Inc., MIT.")
    w.append("// Anime4K (a4k_*): Copyright (c) 2019-2021 bloc97, MIT. Ver THIRD-PARTY.txt.")
    w.append("")
    for n, words in blobs:
        w.append("static const uint32_t kSpv_%s[] = {" % n)
        for i in range(0, len(words), 8):
            w.append("  " + ", ".join("0x%08xu" % x for x in words[i:i + 8]) + ",")
        w.append("};")
    w.append("")
    w.append("static const SpvBlob kSpvBlobs[] = {")
    for n, words in blobs:
        w.append('  {"%s", kSpv_%s, sizeof(kSpv_%s)},' % (n, n, n))
    w.append("};")
    w.append("")
    for pre, chain in chains:
        w.append("// pase, salida, tamano relativo a, factor, entradas t0..")
        w.append("static const NetPass kNet_%s[] = {" % pre)
        for name, save, ref, sc, binds in chain:
            ins = ", ".join('"%s"' % b for b in binds)
            w.append('  {"%s", "%s", "%s", %d, %d, {%s}},' % (name, save, ref, sc, len(binds), ins))
        w.append("};")
    w.append("")
    with open(OUT, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(w))
    print("ok: %d shaders -> %s" % (len(blobs), os.path.relpath(OUT, ROOT)))


if __name__ == "__main__":
    main()
