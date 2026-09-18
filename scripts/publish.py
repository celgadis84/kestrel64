#!/usr/bin/env python
"""Publica en GitHub Releases lo que `scripts/release.sh` acaba de empaquetar.

POR QUE ESTO Y NO UNA ACTION QUE COMPILE
----------------------------------------
Un runner de GitHub puede compilar, pero NO puede validar: las puertas
(`gate_all.sh`, `gate_prdp.sh`, systemtest, la suite krom) necesitan ROMs, y en este
repositorio NO entra ni entrara una sola ROM. Un binario compilado en CI seria un
binario que nadie ha pasado por las puertas -- justo lo contrario de la regla del
proyecto. Asi que lo que se publica es el .exe DE ESTA MAQUINA, el mismo que acaba de
salir verde, y este script lo unico que hace es subirlo y dejar constancia de con que
commit se hizo y con que md5/sha256 salio.

Sube UNICAMENTE el producto (lo de `dist/`, ya empaquetado):

    kestrel64-<ver>-win64.zip     portable (los dos rasterizadores + lanzador)
    kestrel64-<ver>-setup.exe     instalador de Windows
    SHA256SUMS.txt                huellas de los dos de arriba

Uso:
    python scripts/publish.py                 # ensayo: dice que haria y para
    python scripts/publish.py --yes           # publica de verdad
    python scripts/publish.py --yes --draft   # publica como borrador (no visible)
    python scripts/publish.py --tag v0.0.2 --yes

El token sale del gestor de credenciales de Windows (`git credential fill`, el mismo
que usa `git push`). NUNCA se imprime, no pasa por la linea de ordenes -- donde
cualquier proceso lo veria en la lista de procesos -- y no se escribe en disco.
"""

import argparse
import hashlib
import json
import os
import re
import subprocess
import sys
import urllib.error
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
API = "https://api.github.com"
UPLOADS = "https://uploads.github.com"


def run(*args, **kw):
    """git y compania. Devuelve stdout pelado; revienta si el comando falla."""
    return subprocess.run(
        args, cwd=ROOT, check=True, capture_output=True, text=True, **kw
    ).stdout.strip()


def die(msg):
    print("publish: " + msg, file=sys.stderr)
    sys.exit(1)


# --------------------------------------------------------------------------- token
def github_token():
    """El token del gestor de credenciales. El valor NO se imprime nunca."""
    p = subprocess.run(
        ["git", "credential", "fill"],
        cwd=ROOT,
        input="protocol=https\nhost=github.com\n\n",
        capture_output=True,
        text=True,
    )
    if p.returncode != 0:
        die("`git credential fill` fallo; no hay credencial de github.com guardada")
    for line in p.stdout.splitlines():
        if line.startswith("password="):
            tok = line[len("password=") :].strip()
            if tok:
                return tok
    die("el gestor de credenciales no devolvio contrasena para github.com")


def api(token, method, url, data=None, ctype="application/json", raw=None):
    body = raw if raw is not None else (json.dumps(data).encode() if data else None)
    req = urllib.request.Request(url, data=body, method=method)
    req.add_header("Authorization", "Bearer " + token)
    req.add_header("Accept", "application/vnd.github+json")
    req.add_header("X-GitHub-Api-Version", "2022-11-28")
    req.add_header("User-Agent", "kestrel64-publish")
    if body is not None:
        req.add_header("Content-Type", ctype)
    try:
        with urllib.request.urlopen(req) as r:
            txt = r.read().decode("utf-8", "replace")
            return r.status, (json.loads(txt) if txt.strip() else {})
    except urllib.error.HTTPError as e:
        txt = e.read().decode("utf-8", "replace")
        # El cuerpo del error de GitHub no lleva el token; la cabecera no se imprime.
        return e.code, (json.loads(txt) if txt.strip() else {"message": e.reason})


# ----------------------------------------------------------------------- preflight
def repo_slug():
    url = run("git", "remote", "get-url", "origin")
    m = re.search(r"github\.com[/:]([^/]+)/(.+?)(?:\.git)?$", url)
    if not m:
        die("el remoto `origin` no apunta a github.com: " + url)
    return m.group(1), m.group(2)


def version():
    hpp = os.path.join(ROOT, "src", "core", "system.hpp")
    with open(hpp, "r", encoding="latin-1") as f:
        m = re.search(r'kVersion\s*=\s*"([^"]+)"', f.read())
    if not m:
        die("no encuentro kVersion en src/core/system.hpp")
    return m.group(1)


def preflight(ver):
    """Lo que se publica tiene que ser EXACTAMENTE lo que hay commiteado."""
    if run("git", "status", "--porcelain"):
        die("arbol sucio. Commitea (o descarta) antes de publicar.")

    head = run("git", "rev-parse", "--short", "HEAD")
    manifest = os.path.join(ROOT, "dist", "VERSION.txt")
    if not os.path.exists(manifest):
        die("no hay dist/VERSION.txt. Falta `sh scripts/release.sh`.")
    with open(manifest, "r", encoding="utf-8", errors="replace") as f:
        txt = f.read()
    m = re.search(r"^commit:\s*(\S+)(.*)$", txt, re.M)
    if not m:
        die("dist/VERSION.txt no dice de que commit salio")
    if "sucio" in m.group(2):
        die("dist/ se empaqueto con el arbol sucio. Repite `sh scripts/release.sh`.")
    if m.group(1) != head:
        die(
            "dist/ es de %s y HEAD es %s. Repite `sh scripts/release.sh` sobre el "
            "arbol ya commiteado." % (m.group(1), head)
        )

    assets = [
        os.path.join(ROOT, "kestrel64-%s-win64.zip" % ver),
        os.path.join(ROOT, "kestrel64-%s-setup.exe" % ver),
    ]
    faltan = [os.path.basename(a) for a in assets if not os.path.exists(a)]
    if faltan:
        die("falta por empaquetar: " + ", ".join(faltan))
    return head, txt, assets


def sha256sums(assets):
    out = os.path.join(ROOT, "SHA256SUMS.txt")
    lines = []
    for a in assets:
        h = hashlib.sha256()
        with open(a, "rb") as f:
            for chunk in iter(lambda: f.read(1 << 20), b""):
                h.update(chunk)
        lines.append("%s *%s" % (h.hexdigest(), os.path.basename(a)))
    with open(out, "w", encoding="utf-8", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    return out, lines


NOTES = """\
Binarios de Windows de 64 bits construidos y **validados en la maquina de desarrollo**,
no en CI: las puertas de este proyecto (systemtest, la suite krom, lockstep contra
threaded) necesitan ROMs, y aqui no se distribuye ninguna.

### Que bajar

| Fichero | Para que |
|---|---|
| `kestrel64-{ver}-setup.exe` | Instalador. Lo normal. |
| `kestrel64-{ver}-win64.zip` | Portable: descomprimir y listo. |

Dentro van los dos rasterizadores: `kestrel64.exe` (Parallel-RDP, por GPU, el
recomendado) y `kestrel64-soft.exe` (por CPU, para una maquina sin Vulkan), mas el
lanzador `kestrel64-gui.exe`.

**Requisitos:** Windows de 64 bits y una GPU con Vulkan. `vulkan-1.dll` la instala el
driver de la grafica; no se distribuye aqui.

### Procedencia

```
{manifest}```

Huellas en `SHA256SUMS.txt`. Codigo en `{commit}`.
"""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--tag", help="por defecto v<kVersion>")
    ap.add_argument("--yes", action="store_true", help="publicar de verdad")
    ap.add_argument("--draft", action="store_true", help="dejarlo en borrador")
    ap.add_argument("--prerelease", action="store_true")
    args = ap.parse_args()

    ver = version()
    tag = args.tag or ("v" + ver)
    head, manifest, assets = preflight(ver)
    sums, sumlines = sha256sums(assets)
    assets = assets + [sums]
    owner, repo = repo_slug()
    notes = NOTES.format(ver=ver, manifest=manifest, commit=head)

    print("repositorio : %s/%s" % (owner, repo))
    print("etiqueta    : %s  (commit %s)" % (tag, head))
    print("borrador    : %s   prerelease: %s" % (bool(args.draft), bool(args.prerelease)))
    print("ficheros    :")
    for a in assets:
        print("  %-34s %8.1f MB" % (os.path.basename(a), os.path.getsize(a) / 1e6))
    for l in sumlines:
        print("  sha256 " + l)

    if not args.yes:
        print("\nENSAYO. Nada subido. Repite con --yes para publicar de verdad.")
        return 0

    # La etiqueta primero: una release cuelga de una etiqueta que tiene que existir.
    if tag in run("git", "tag", "--list", tag).splitlines():
        en = run("git", "rev-parse", tag + "^{commit}")
        if not run("git", "rev-parse", "HEAD").startswith(en[:7]):
            die("la etiqueta %s ya existe y apunta a otro commit (%s)" % (tag, en[:7]))
    else:
        run("git", "tag", "-a", tag, "-m", "kestrel64 " + ver)
    run("git", "push", "origin", tag)
    print("etiqueta %s empujada" % tag)

    token = github_token()
    base = "%s/repos/%s/%s/releases" % (API, owner, repo)

    st, rel = api(token, "GET", "%s/tags/%s" % (base, tag))
    payload = {
        "tag_name": tag,
        "name": "kestrel64 " + ver,
        "body": notes,
        "draft": bool(args.draft),
        "prerelease": bool(args.prerelease),
    }
    if st == 200:
        st, rel = api(token, "PATCH", "%s/%d" % (base, rel["id"]), payload)
        if st != 200:
            die("no pude actualizar la release: %s" % rel.get("message"))
        print("release existente actualizada")
    else:
        st, rel = api(token, "POST", base, payload)
        if st != 201:
            die("no pude crear la release: %s" % rel.get("message"))
        print("release creada")

    rid = rel["id"]
    st, existing = api(token, "GET", "%s/%d/assets" % (base, rid))
    have = {a["name"]: a["id"] for a in (existing if st == 200 else [])}

    for a in assets:
        name = os.path.basename(a)
        if name in have:  # subir dos veces el mismo nombre da 422
            api(token, "DELETE", "%s/assets/%d" % (base, have[name]))
        with open(a, "rb") as f:
            blob = f.read()
        url = "%s/repos/%s/%s/releases/%d/assets?name=%s" % (
            UPLOADS, owner, repo, rid, name
        )
        st, res = api(
            token, "POST", url, ctype="application/octet-stream", raw=blob
        )
        if st != 201:
            die("fallo subiendo %s: %s" % (name, res.get("message")))
        print("subido %s" % name)

    print("\nlisto: " + rel["html_url"])
    return 0


if __name__ == "__main__":
    sys.exit(main())
