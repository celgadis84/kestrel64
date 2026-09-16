# -*- coding: utf-8 -*-
"""Lanzador de kestrel64: servidor local + interfaz web.

Por que un servidor y no una ventana nativa: no anade toolchain. Python de la biblioteca
estandar sirve `web/`, y la interfaz corre en el navegador que ya hay en la maquina, en
modo aplicacion (sin barra de direcciones). Cero dependencias que compilar.

    python tools/launcher/kestrel_launcher.py            # abre el navegador
    python tools/launcher/kestrel_launcher.py --no-open  # solo servidor
    python tools/launcher/kestrel_launcher.py --port 9130

API
    GET  /api/schema           esquema de opciones + mando (de options.py)
    GET  /api/config           perfil guardado
    POST /api/config           guardar perfil
    GET  /api/builds           compilaciones disponibles (SoftRDP / paraLLEl-RDP)
    GET  /api/roms?dir=...     escaneo de carpeta -> lista con cabecera leida
    GET  /api/browse?dir=...   navegador de carpetas (el navegador web no puede)
    GET  /api/boxart?id=...    caratula (cache local, descarga si hay red)
    POST /api/launch           arranca el emulador con el perfil
    POST /api/stop             lo mata
    GET  /api/status           estado del proceso + ultimas lineas de salida
    GET  /api/game?rom=...     ficha: cabecera, partidas, manuales, trucos, notas
    GET  /api/manual?rom=&i=   manual N de esa ficha (solo los que la ficha encontro)
    POST /api/game/meta        notas del juego (anio, estudio, genero...)
    POST /api/game/cheats      encender/apagar trucos del .cht de la ROM
    POST /api/game/cheat/add   anadir truco    POST /api/game/cheat/del  borrarlo
    POST /api/game/manual?rom=&name=   subir un manual (cuerpo = el fichero)
"""

import argparse
import base64
import io
import json
import mimetypes
import os
import re
import subprocess
import sys
import threading
import time
import urllib.parse
import urllib.request
import webbrowser
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

# Dos formas de vivir: dentro del arbol de fuentes (desarrollo) y dentro de
# kestrel64-gui.exe (lo que se instala). Cambian tres cosas y solo tres:
#
#   - los recursos (web/) los desempaqueta PyInstaller en un temporal, sys._MEIPASS;
#   - el emulador no esta en build/ sino AL LADO del .exe, en el directorio de instalacion;
#   - el perfil y las caratulas no pueden escribirse en Archivos de programa, asi que se van
#     a %LOCALAPPDATA%\kestrel64.
#
# Todo lo demas -- el servidor, la API, la interfaz -- es identico en los dos casos.
FROZEN = getattr(sys, "frozen", False)
HERE = os.path.dirname(os.path.abspath(__file__))
if FROZEN:
    RES = getattr(sys, "_MEIPASS", HERE)          # recursos empaquetados (solo lectura)
    APPDIR = os.path.dirname(os.path.abspath(sys.executable))
    KESTREL = APPDIR
    STATE = os.path.join(os.environ.get("LOCALAPPDATA") or os.path.expanduser("~"), "kestrel64")
else:
    RES = HERE
    KESTREL = os.path.dirname(os.path.dirname(HERE))   # .../N64/kestrel64
    APPDIR = KESTREL
    STATE = HERE
ROOT = os.path.dirname(KESTREL)
WEB = os.path.join(RES, "web")
CACHE = os.path.join(STATE, "cache")
ART = os.path.join(CACHE, "boxart")
PROFILE = os.path.join(STATE, "profile.json")
os.makedirs(STATE, exist_ok=True)

if not FROZEN:
    sys.path.insert(0, HERE)
import options as OPT  # noqa: E402
import gamecard as CARD  # noqa: E402
import tele as TELE  # noqa: E402

ROM_EXT = (".z64", ".n64", ".v64", ".rom", ".zip", ".gz")
ROM_EXT_CRUDA = (".z64", ".n64", ".v64", ".rom")   # lo que puede haber DENTRO de un contenedor

# Cliente de telemetria, uno y reutilizado: abrir un socket por peticion contra un
# emulador que puede estar arrancando da falsos "no hay emulador".
TELE_CLIENT = [None]
TELE_PORT = [9128]

# El nucleo manda pc, gpr y los registros COP0 como enteros de 64 bits. JSON.parse los mete
# en un double: por encima de 2^53 el paso pierde los bits BAJOS, que en un PC son justo los
# que importan (0xffffffff80246dd8 llegaria con la direccion cambiada, y un punto de ruptura
# puesto sobre eso caeria en otro sitio). Aqui el entero todavia es exacto, asi que los que
# no caben salen ya como cadena hexadecimal y el navegador no los toca nunca como numero.
SAFE_INT = (1 << 53) - 1


def tele_safe(v):
    if isinstance(v, bool):
        return v
    if isinstance(v, int) and abs(v) > SAFE_INT:
        return "0x%016x" % (v & 0xFFFFFFFFFFFFFFFF)
    if isinstance(v, dict):
        return {k: tele_safe(x) for k, x in v.items()}
    if isinstance(v, list):
        return [tele_safe(x) for x in v]
    return v

# libretro-thumbnails: la coleccion de caratulas mas completa y de acceso libre que hay.
# (Nota: NNID es un identificador de cuenta de Wii U / 3DS, no tiene nada que ver con arte
# de N64. Las fuentes reales son esta, TheGamesDB y ScreenScraper.)
THUMB_BASE = ("https://raw.githubusercontent.com/libretro-thumbnails/"
              "Nintendo_-_Nintendo_64/master/Named_Boxarts/%s.png")
# El repo nombra los ficheros con la convencion No-Intro. Una ROM que no venga nombrada asi
# no acierta por nombre exacto, asi que se baja UNA vez el indice del directorio (1117
# entradas, cabe entero en una respuesta del API de arboles) y se casa normalizando.
THUMB_TREE = ("https://api.github.com/repos/libretro-thumbnails/"
              "Nintendo_-_Nintendo_64/git/trees/master:Named_Boxarts")
INDEX = os.path.join(CACHE, "boxart-index.json")
# Region del cartucho -> como la escribe No-Intro. Sirve de desempate cuando hay varias
# entradas para el mismo juego.
REGION_TAG = {"E": "USA", "J": "Japan", "P": "Europe", "D": "Germany", "F": "France",
              "I": "Italy", "S": "Spain", "U": "Australia", "X": "Europe", "Y": "Europe"}


# --------------------------------------------------------------------------- utilidades
def jdump(x):
    return json.dumps(x, ensure_ascii=False).encode("utf-8")


def load_profile():
    p = OPT.defaults()
    try:
        with open(PROFILE, "r", encoding="utf-8") as f:
            p.update(json.load(f))
    except Exception:
        pass
    return p


def save_profile(p):
    os.makedirs(STATE, exist_ok=True)
    tmp = PROFILE + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(p, f, ensure_ascii=False, indent=1)
    os.replace(tmp, PROFILE)


def builds():
    """Compilaciones presentes. El plugin grafico se elige en tiempo de COMPILACION
    (opcion de cmake KESTREL_PRDP), asi que el lanzador elige EJECUTABLE, no una dll."""
    out = []
    for d, pid, label, desc in (
        ("build", "soft", "SoftRDP",
         "Rasterizador propio en CPU. Determinista, no necesita GPU. Es el oraculo."),
        ("build-prdp", "prdp", "paraLLEl-RDP",
         "RDP a bajo nivel sobre Vulkan, en la GPU. Mas rapido y mas exacto en subpixel."),
    ):
        exe = os.path.join(KESTREL, d, "kestrel64.exe")
        if os.path.isfile(exe):
            out.append(dict(id=pid, label=label, desc=desc, exe=exe, dir=d,
                            mtime=int(os.path.getmtime(exe))))
    if not out:
        # Instalacion: los .exe al lado del lanzador. El paquete trae kestrel64.exe (el
        # recomendado, con parallel-RDP) y kestrel64-soft.exe (el mismo emulador con el
        # rasterizador por software), asi que aqui SI hay eleccion que ofrecer.
        DESC = {
            "soft": "Rasterizador propio en CPU. Determinista, no necesita GPU. Es el oraculo.",
            "prdp": "RDP a bajo nivel sobre Vulkan, en la GPU. Mas rapido y mas exacto en subpixel.",
        }
        for fname in ("kestrel64.exe", "kestrel64-soft.exe"):
            exe = os.path.join(APPDIR, fname)
            if not os.path.isfile(exe):
                continue
            # Que backend lleva dentro no se ve desde fuera del .exe, asi que el empaquetado
            # (scripts/dist.sh) deja la nota al lado. Sin nota se asume el oraculo.
            pid, label = "soft", "SoftRDP"
            try:
                tagfile = os.path.splitext(fname)[0] + ".build"
                with open(os.path.join(APPDIR, tagfile), encoding="utf-8") as f:
                    tag = f.read().strip()
            except OSError:
                tag = ""
            if tag.startswith("prdp"):
                pid, label = "prdp", "paraLLEl-RDP"
            if any(x["id"] == pid for x in out):
                continue
            out.append(dict(id=pid, label=label, dir=".", exe=exe,
                            desc=DESC[pid], mtime=int(os.path.getmtime(exe))))
    return out


def pick_exe(plugin):
    b = builds()
    if not b:
        return None
    if plugin in ("soft", "prdp"):
        for x in b:
            if x["id"] == plugin:
                return x
    # auto: paraLLEl-RDP si esta compilado, si no el de siempre.
    for want in ("prdp", "soft"):
        for x in b:
            if x["id"] == want:
                return x
    return b[0]


# --------------------------------------------------------------------------- cabecera ROM
def _rom_bytes(path):
    """Devuelve (primeros 0x40 bytes, tamano logico) mirando DENTRO del contenedor.

    El emulador abre .zip y .gz por su cuenta (src/core/archive.cpp), asi que la lista de
    ROMs tiene que ensenar el juego de dentro y no un "formato desconocido". Se descomprime
    solo lo justo: del zip la entrada con pinta de ROM, del gz los primeros bytes.
    """
    low = path.lower()
    if low.endswith(".zip"):
        import zipfile
        with zipfile.ZipFile(path) as z:
            best = None
            for i in z.infolist():
                if i.is_dir():
                    continue
                isrom = i.filename.lower().endswith(ROM_EXT_CRUDA)
                if (best is None or (isrom and not best[0])
                        or (isrom == best[0] and i.file_size > best[1].file_size)):
                    best = (isrom, i)
            if best is None:
                return None, 0
            with z.open(best[1]) as f:
                return f.read(0x40), best[1].file_size
    if low.endswith(".gz"):
        import gzip
        with gzip.open(path, "rb") as f:
            h = f.read(0x40)
        # El tamano exacto solo se sabe descomprimiendo entero; el ISIZE del final sirve de
        # estimacion (es modulo 4 GB, y ninguna ROM de N64 llega ahi).
        with open(path, "rb") as f:
            f.seek(-4, 2)
            size = int.from_bytes(f.read(4), "little")
        return h, size
    with open(path, "rb") as f:
        h = f.read(0x40)
    return h, os.path.getsize(path)


def rom_header(path):
    """Lee la cabecera de 64 bytes y normaliza el orden de bytes.

    El mismo juego circula en tres ordenaciones distintas segun el volcador:
      0x80371240  z64  big-endian, el orden nativo del cartucho
      0x37804012  v64  byteswapped, pares de bytes intercambiados
      0x40123780  n64  little-endian, palabras enteras del reves
    """
    try:
        h, size = _rom_bytes(path)
    except Exception:
        return None
    if h is None:
        return None
    if len(h) < 0x40:
        return None
    m = int.from_bytes(h[:4], "big")
    if m == 0x80371240:
        fmt, b = "z64", h
    elif m == 0x37804012:
        fmt, b = "v64", bytes(b for p in range(0, len(h), 2) for b in (h[p + 1], h[p]))
    elif m == 0x40123780:
        fmt, b = "n64", bytes(h[p ^ 3] for p in range(len(h)))
    else:
        return None
    # Una ROM casera puede traer la cabecera medio vacia: se limpia lo no imprimible
    # antes de ensenarlo, o la lista sale con NULs.
    clean = lambda t: "".join(c for c in t if 32 <= ord(c) < 127).strip()
    name = clean(b[0x20:0x34].decode("latin-1"))
    cart = clean(b[0x3B:0x3E].decode("latin-1"))
    region = clean(chr(b[0x3E]) if 0x3E < len(b) else "") or "?"
    crc = "%08X%08X" % (int.from_bytes(b[0x10:0x14], "big"),
                        int.from_bytes(b[0x14:0x18], "big"))
    regions = {"E": "USA", "P": "Europa", "J": "Japon", "D": "Alemania", "F": "Francia",
               "I": "Italia", "S": "Espana", "U": "Australia", "A": "Asia", "X": "Europa",
               "Y": "Europa"}
    return dict(fmt=fmt, name=name or os.path.splitext(os.path.basename(path))[0],
                cart=cart, region=region, region_label=regions.get(region, region),
                crc=crc, size=size, mb=round(size / 1048576.0, 1))


def art_key(fname, header):
    """Clave de caratula. libretro nombra los ficheros como No-Intro, o sea con el nombre
    del FICHERO, no el interno del cartucho. Se prueba el nombre del fichero primero."""
    base = os.path.splitext(os.path.basename(fname))[0]
    keys = [base]
    if header and header.get("name"):
        keys.append(header["name"])
    return keys


def art_norm(t):
    """Normaliza un titulo para casarlo: fuera los grupos entre parentesis o corchetes (que
    en No-Intro son region, idiomas y revision), fuera todo lo que no sea alfanumerico, y a
    minusculas. "Super Mario 64 (USA)" y "SUPER MARIO 64" acaban iguales."""
    t = re.sub(r"[\(\[][^\)\]]*[\)\]]", " ", t)
    t = t.replace("&", " and ")
    return re.sub(r"[^a-z0-9]", "", t.lower())


_index = {"names": None}


def art_index():
    """Lista de nombres de fichero del repo de caratulas. Se cachea en disco; un fallo de red
    se cachea como lista vacia para no reintentar en cada refresco de la biblioteca."""
    if _index["names"] is not None:
        return _index["names"]
    names = None
    if os.path.isfile(INDEX):
        try:
            with io.open(INDEX, encoding="utf-8") as f:
                names = json.load(f)
        except Exception:
            names = None
    if names is None:
        names = []
        try:
            req = urllib.request.Request(THUMB_TREE, headers={"User-Agent": "kestrel64"})
            with urllib.request.urlopen(req, timeout=15) as r:
                tree = json.load(r).get("tree", [])
            names = [e["path"][:-4] for e in tree
                     if e.get("type") == "blob" and e["path"].endswith(".png")]
        except Exception:
            names = []
        os.makedirs(CACHE, exist_ok=True)
        with io.open(INDEX, "w", encoding="utf-8") as f:
            json.dump(names, f)
    _index["names"] = names
    return names


def art_match(keys, region=""):
    """Nombre de fichero del repo que corresponde a estas claves, o None. Con varias entradas
    del mismo juego gana la de la region del cartucho, y si no la de USA; entre las que
    quedan, la mas corta, que es la version base (sin Beta / Rev / Demo)."""
    names = art_index()
    if not names:
        return None
    by = {}
    for n in names:
        by.setdefault(art_norm(n), []).append(n)
    tag = REGION_TAG.get(region, "")
    def pick(cand):
        for want in (tag, "USA"):
            if want:
                hit = [c for c in cand if "(%s)" % want in c]
                if hit:
                    return min(hit, key=len)
        return min(cand, key=len)

    for k in keys:
        cand = by.get(art_norm(k))
        if cand:
            return pick(cand)
    # Sin acierto exacto: prefijo, pero solo si es INEQUIVOCO. El nombre interno del cartucho
    # suele venir recortado ("GOLDENEYE" por "GoldenEye 007") y el del fichero suele traer
    # cola ("mario_kart_64_rom"), asi que se mira en los dos sentidos. La condicion de
    # unicidad es lo que lo hace seguro: "Mario Party" es prefijo de "Mario Party" y de
    # "Mario Party 2" a la vez, y ahi no se elige -- se deja sin caratula.
    for k in keys:
        n = art_norm(k)
        if len(n) < 6:
            continue
        for match in ((lambda c: c.startswith(n)), (lambda c: n.startswith(c) and len(c) >= 6)):
            hit = [c for c in by if match(c)]
            if len(hit) == 1:
                return pick(by[hit[0]])
    return None


def art_fetch(keys, region=""):
    os.makedirs(ART, exist_ok=True)
    for k in keys:
        safe = re.sub(r'[\\/:*?"<>|]', "_", k)
        local = os.path.join(ART, safe + ".png")
        if os.path.isfile(local):
            return local if os.path.getsize(local) > 0 else None
    # No esta en cache: intentar red una sola vez por clave. Sin red, se marca con un
    # fichero vacio para no volver a intentarlo en cada refresco de la lista.
    m = art_match(keys, region)
    for k in ([m] if m else []) + list(keys):
        safe = re.sub(r'[\\/:*?"<>|]', "_", k)
        local = os.path.join(ART, safe + ".png")
        url = THUMB_BASE % urllib.parse.quote(k)
        try:
            with urllib.request.urlopen(url, timeout=6) as r:
                data = r.read()
            if data[:4] == b"\x89PNG":
                with open(local, "wb") as f:
                    f.write(data)
                if k != keys[0]:
                    # Guardar tambien bajo la clave que pidio el cliente: el siguiente
                    # refresco acierta en cache sin volver a pasar por el indice.
                    alias = re.sub(r'[\\/:*?"<>|]', "_", keys[0])
                    with open(os.path.join(ART, alias + ".png"), "wb") as f:
                        f.write(data)
                return local
        except Exception:
            continue
    safe = re.sub(r'[\\/:*?"<>|]', "_", keys[0])
    open(os.path.join(ART, safe + ".png"), "wb").close()
    return None


# --------------------------------------------------------------------------- proceso emu
class Emu:
    def __init__(self):
        self.proc = None
        self.lines = []
        self.lock = threading.Lock()
        self.cmd = None
        self.started = 0
        self.card = None     # clave de ficha de la sesion en marcha, para contar el tiempo

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def start(self, exe, rom, env, argv, cwd, card=None):
        if self.alive():
            self.stop()
        e = dict(os.environ)
        e.update({k: str(v) for k, v in env.items()})
        # El exe es de MSYS2 CLANG64 y va enlazado contra sus dll: sin esto en PATH el
        # proceso muere en silencio al arrancar, sin una sola linea de salida.
        e["PATH"] = "C:\\msys64\\clang64\\bin" + os.pathsep + e.get("PATH", "")
        cmd = [exe, rom] + list(argv)
        with self.lock:
            self.lines = []
            self.cmd = cmd
            self.started = time.time()
            self.card = card
        # Windows no deja que un proceso cualquiera se ponga en primer plano: el emulador
        # abriria su ventana DETRAS del navegador y, como el teclado se lee de la ventana
        # enfocada, correria sin responder a ninguna tecla. AllowSetForegroundWindow cede
        # ese derecho; ASFW_ANY (-1) vale para el hijo que aun no existe. Si el sistema lo
        # deniega (el lanzador no es la ventana activa) no pasa nada: la ventana parpadea
        # en la barra de tareas y basta un clic.
        try:
            import ctypes
            ctypes.windll.user32.AllowSetForegroundWindow(-1)
        except Exception:
            pass
        self.proc = subprocess.Popen(
            cmd, cwd=cwd, env=e, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            bufsize=1, universal_newlines=True, errors="replace")
        try:
            import ctypes
            ctypes.windll.user32.AllowSetForegroundWindow(self.proc.pid)
        except Exception:
            pass
        threading.Thread(target=self._pump, args=(self.started, card), daemon=True).start()
        return cmd

    def _pump(self, t0, card):
        p = self.proc
        try:
            for ln in p.stdout:
                with self.lock:
                    self.lines.append(ln.rstrip("\n").replace("\0", ""))
                    if len(self.lines) > 400:
                        del self.lines[:200]
        except Exception:
            pass
        # stdout cerrado = el proceso se fue. La sesion cuenta en la ficha del juego.
        try:
            p.wait(timeout=5)
        except Exception:
            pass
        if card:
            try:
                CARD.meta_played(STATE, card, time.time() - t0)
            except Exception:
                pass

    def stop(self):
        if self.proc is None:
            return False
        try:
            self.proc.terminate()
            self.proc.wait(timeout=3)
        except Exception:
            try:
                self.proc.kill()
            except Exception:
                pass
        return True

    def status(self):
        with self.lock:
            return dict(running=self.alive(), cmd=self.cmd,
                        uptime=round(time.time() - self.started, 1) if self.started else 0,
                        code=None if self.alive() else (self.proc.returncode if self.proc else None),
                        log=self.lines[-120:])


EMU = Emu()


# --------------------------------------------------------------------------- servidor
class H(BaseHTTPRequestHandler):
    server_version = "kestrel64-launcher"

    def log_message(self, *a):
        pass

    def _send(self, code, body, ctype="application/json; charset=utf-8", extra=None):
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        for k, v in (extra or {}).items():
            self.send_header(k, v)
        self.end_headers()
        try:
            self.wfile.write(body)
        except Exception:
            pass

    def _json(self, obj, code=200):
        self._send(code, jdump(obj))

    def _body(self):
        n = int(self.headers.get("Content-Length") or 0)
        if not n:
            return {}
        try:
            return json.loads(self.rfile.read(n).decode("utf-8"))
        except Exception:
            return {}

    # ---------------------------------------------------------------- GET
    def do_GET(self):
        u = urllib.parse.urlparse(self.path)
        q = urllib.parse.parse_qs(u.query)
        p = u.path

        if p.startswith("/api/"):
            return self._api_get(p, q)

        # estatico
        rel = "index.html" if p in ("/", "") else p.lstrip("/")
        f = os.path.normpath(os.path.join(WEB, rel))
        if not f.startswith(WEB) or not os.path.isfile(f):
            return self._send(404, b"no", "text/plain")
        ctype = mimetypes.guess_type(f)[0] or "application/octet-stream"
        with open(f, "rb") as fh:
            data = fh.read()
        self._send(200, data, ctype)

    def _api_get(self, p, q):
        if p == "/api/schema":
            return self._json(dict(categories=OPT.CATEGORIES, pad_buttons=OPT.PAD_BUTTONS,
                                   pad_axes=OPT.PAD_AXES, defaults=OPT.defaults()))
        if p == "/api/config":
            return self._json(load_profile())
        if p == "/api/builds":
            return self._json(dict(builds=builds(), kestrel=KESTREL))
        if p == "/api/browse":
            return self._json(self._browse(q.get("dir", [""])[0]))
        if p == "/api/roms":
            return self._json(self._roms(q.get("dir", [""])[0]))
        if p == "/api/boxart":
            return self._boxart(q)
        if p == "/api/status":
            return self._json(EMU.status())
        if p == "/api/tele":
            return self._tele(q)
        if p == "/api/tele/fb":
            return self._tele_fb(q)
        if p == "/api/game":
            return self._game(q)
        if p == "/api/manual":
            return self._manual(q)
        return self._json(dict(error="ruta desconocida"), 404)

    # ---------------------------------------------------------------- telemetria
    # El emulador trae el servidor de telemetria dentro (puerto 9128 por defecto). El
    # lanzador lo expone tal cual: no reinterpreta nada, solo pasa la orden y devuelve lo
    # que conteste. Asi la ventana de telemetria no puede "mentir" respecto al MCP.
    def _tele(self, q):
        cmd = q.get("cmd", [""])[0]
        if not cmd:
            return self._json(dict(ok=False, error="falta cmd"), 400)
        args = {}
        raw = q.get("args", [""])[0]
        if raw:
            try:
                args = json.loads(raw)
            except Exception:
                return self._json(dict(ok=False, error="args no es JSON"), 400)
        snap = q.get("snapshot", ["0"])[0] not in ("0", "", "false")
        try:
            t = self._tele_client(q)
            data, blob = t.snapshot(cmd, **args) if snap else t.query(cmd, **args)
        except TELE.TeleError as e:
            return self._json(dict(ok=False, error=str(e)), 200)
        out = dict(ok=True, data=tele_safe(data))
        # mem.read devuelve los bytes crudos por el lado binario del marco, que es justo
        # lo que lo hace barato. Al navegador van en base64: sigue siendo un tercio de lo
        # que costaria en hexadecimal y no obliga a un segundo viaje.
        if blob:
            out["blob"] = base64.b64encode(blob).decode()
        return self._json(out)

    def _tele_fb(self, q):
        """Framebuffer que el VI esta escaneando, en PNG. Es la ruta de captura propia del
        emulador: se lee la RDRAM que la consola mostraria, sin leer de vuelta la GPU."""
        try:
            h = int(q.get("height", ["240"])[0])
            data, blob = self._tele_client(q).query("vi.capture", height=h)
        except TELE.TeleError as e:
            return self._send(503, str(e).encode(), "text/plain")
        except Exception as e:
            return self._send(500, str(e).encode(), "text/plain")
        w, hh = data["width"], data["height"]
        if not blob or len(blob) < w * hh * 4:
            return self._send(503, b"sin imagen", "text/plain")
        self._send(200, TELE.png(w, hh, blob), "image/png", {"Cache-Control": "no-store"})

    def _tele_client(self, q):
        port = int(q.get("port", [str(TELE_PORT[0])])[0])
        if TELE_CLIENT[0] is None or TELE_CLIENT[0].port != port:
            if TELE_CLIENT[0] is not None:
                TELE_CLIENT[0].close()
            TELE_CLIENT[0] = TELE.Tele(port)
        return TELE_CLIENT[0]

    def _browse(self, d):
        if not d:
            # raices: unidades de Windows
            roots = []
            for c in "CDEFGHIJKLMNOPQRSTUVWXYZ":
                if os.path.isdir(c + ":\\"):
                    roots.append(dict(name=c + ":\\", path=c + ":\\"))
            return dict(cwd="", up=None, dirs=roots)
        d = os.path.abspath(d)
        if not os.path.isdir(d):
            return dict(error="no es carpeta", cwd=d, up=None, dirs=[])
        dirs, nroms = [], 0
        try:
            for e in sorted(os.scandir(d), key=lambda x: x.name.lower()):
                if e.is_dir():
                    dirs.append(dict(name=e.name, path=e.path))
                elif e.name.lower().endswith(ROM_EXT):
                    nroms += 1
        except PermissionError:
            return dict(error="sin permiso", cwd=d, up=os.path.dirname(d), dirs=[])
        up = os.path.dirname(d.rstrip("\\/"))
        return dict(cwd=d, up=(up if up and up != d else ""), dirs=dirs, roms=nroms)

    def _roms(self, d):
        if not d or not os.path.isdir(d):
            return dict(error="carpeta no valida", roms=[])
        out = []
        for e in sorted(os.scandir(d), key=lambda x: x.name.lower()):
            if not e.is_file() or not e.name.lower().endswith(ROM_EXT):
                continue
            h = rom_header(e.path)
            out.append(dict(file=e.name, path=e.path, header=h,
                            title=(h["name"] if h else os.path.splitext(e.name)[0]),
                            id=os.path.splitext(e.name)[0]))
        return dict(dir=d, roms=out)

    def _boxart(self, q):
        key = q.get("id", [""])[0]
        name = q.get("name", [""])[0]
        if not key:
            return self._send(404, b"no", "text/plain")
        f = art_fetch([k for k in (key, name) if k], q.get("region", [""])[0])
        if not f:
            return self._send(404, b"no", "text/plain")
        with open(f, "rb") as fh:
            data = fh.read()
        self._send(200, data, "image/png", {"Cache-Control": "max-age=86400"})

    # ---------------------------------------------------------------- POST
    def do_POST(self):
        p = urllib.parse.urlparse(self.path).path
        if p == "/api/game/manual":
            return self._manual_upload()
        b = self._body()
        if p == "/api/config":
            save_profile(b)
            return self._json(dict(ok=True))
        if p == "/api/stop":
            return self._json(dict(ok=EMU.stop()))
        if p == "/api/launch":
            return self._json(self._launch(b))
        if p.startswith("/api/game/"):
            return self._game_post(p, b)
        return self._json(dict(error="ruta desconocida"), 404)

    # ---------------------------------------------------------------- ficha de juego
    @staticmethod
    def _rom_ok(rom):
        """La ficha solo habla de ROMs de verdad: fichero con extension de ROM y cabecera
        N64 legible. Todo lo que se lee o escribe despues cuelga de esa ruta (misma
        carpeta, mismo nombre base), asi que no hay forma de pedirle un fichero cualquiera."""
        if not rom or not os.path.isfile(rom) or not rom.lower().endswith(ROM_EXT):
            return None
        return rom_header(rom)

    def _game(self, q):
        rom = q.get("rom", [""])[0]
        h = self._rom_ok(rom)
        if h is None:
            return self._json(dict(error="ROM no valida"), 404)
        c = CARD.card(STATE, rom, h)
        # El fichero de trucos del perfil (KESTREL_CHEATS) le gana al de al lado de la ROM.
        c["cheats"]["override"] = load_profile().get("cheats") or ""
        c["running"] = EMU.alive() and EMU.card == c["key"]
        return self._json(c)

    def _manual(self, q):
        rom = q.get("rom", [""])[0]
        h = self._rom_ok(rom)
        if h is None:
            return self._send(404, b"no", "text/plain")
        try:
            f = CARD.manuals(rom, h)[int(q.get("i", ["0"])[0])]
        except (ValueError, IndexError):
            return self._send(404, b"no", "text/plain")
        with open(f, "rb") as fh:
            data = fh.read()
        ctype = CARD.MANUAL_EXT.get(os.path.splitext(f)[1].lower(), "application/octet-stream")
        name = urllib.parse.quote(os.path.basename(f))
        self._send(200, data, ctype, {"Content-Disposition": "inline; filename*=UTF-8''" + name})

    def _manual_upload(self):
        # cuerpo crudo = el fichero; la ROM y el nombre original van en la consulta
        q = urllib.parse.parse_qs(urllib.parse.urlparse(self.path).query)
        n = int(self.headers.get("Content-Length") or 0)
        if n > CARD.MAX_UPLOAD:
            return self._json(dict(ok=False, error="manual demasiado grande"))
        data = self.rfile.read(n) if n else b""
        rom = q.get("rom", [""])[0]
        if self._rom_ok(rom) is None:
            return self._json(dict(ok=False, error="ROM no valida"))
        try:
            dst = CARD.manual_store(rom, q.get("name", [""])[0], data)
        except (ValueError, OSError) as e:
            return self._json(dict(ok=False, error=str(e)))
        return self._json(dict(ok=True, file=os.path.basename(dst)))

    def _game_post(self, p, b):
        rom = b.get("rom", "")
        h = self._rom_ok(rom)
        if h is None:
            return self._json(dict(ok=False, error="ROM no valida"))
        try:
            if p == "/api/game/meta":
                m = CARD.meta_save(STATE, CARD.meta_key(rom, h), b.get("meta") or {})
                return self._json(dict(ok=True, meta=m))
            if p == "/api/game/cheats":
                return self._json(dict(ok=True, cheats=CARD.set_cheats(rom, b.get("states") or {})))
            if p == "/api/game/cheat/add":
                return self._json(dict(ok=True, cheats=CARD.add_cheat(
                    rom, b.get("name", ""), b.get("codes", ""), b.get("on", True))))
            if p == "/api/game/cheat/del":
                return self._json(dict(ok=True, cheats=CARD.del_cheat(rom, b.get("id", -1))))
        except (ValueError, OSError) as e:
            return self._json(dict(ok=False, error=str(e)))
        return self._json(dict(ok=False, error="ruta desconocida"), 404)

    def _launch(self, b):
        prof = b.get("profile") or load_profile()
        rom = b.get("rom") or prof.get("rom") or ""
        if not rom or not os.path.isfile(rom):
            return dict(ok=False, error="ROM no encontrada: %s" % rom)
        env, argv = OPT.to_env(prof)
        # El emulador lleva el mismo catalogo de opciones dentro de su ventana y escribe en
        # ESTE perfil, no en otro: en arbol de desarrollo STATE no es %LOCALAPPDATA%.
        env["KESTREL_PROFILE"] = PROFILE
        bl = pick_exe(prof.get("plugin", "auto"))
        if not bl:
            return dict(ok=False, error="no hay ningun kestrel64.exe compilado")
        if bl["id"] != "prdp":
            env["KESTREL_PRDP"] = "0"   # ese exe no lleva backend GPU; que quede dicho
        # El mapeo del mando va en fichero, no en variables: son 18 asignaciones por
        # conector. El mando 1 se guarda bajo la clave "pad" (la de siempre) y los otros
        # tres bajo "pad2".."pad4"; el dialogo del emulador escribe las cuatro.
        pads, accs = "", ""
        for q in range(4):
            key = "pad" if q == 0 else "pad%d" % (q + 1)
            pad = prof.get(key)
            if pad:
                pf = os.path.join(STATE, "pad%d.cfg" % (q + 1))
                with open(pf, "w", encoding="utf-8") as f:
                    for k, v in pad.items():
                        f.write("%s %s %s\n" % (k, v.get("key", "") or "-", v.get("gp", "") or "-"))
                env["KESTREL_PAD%d" % (q + 1)] = pf
            # Ajustes por conector. De fabrica solo esta enchufado el mando 1, y con
            # Controller Pak: es lo que hacia el emulador cuando solo habia un puerto.
            on = prof.get("pad%d_on" % (q + 1), q == 0)
            acc = prof.get("pad%d_acc" % (q + 1), 1)
            dev = prof.get("pad%d_dev" % (q + 1), "auto" if q == 0 else "")
            pads += "0" if on in (0, "0", False, "false", "") else "1"
            try:
                acc = int(acc)
            except (TypeError, ValueError):
                acc = 1
            accs += str(acc if 0 <= acc <= 2 else 0)
            if dev:
                env["KESTREL_PADDEV%d" % (q + 1)] = str(dev)
        env["KESTREL_PADS"] = pads
        env["KESTREL_PADACC"] = accs
        # La ventana de telemetria habla con el puerto que se le acaba de dar al emulador.
        TELE_PORT[0] = int(prof.get("port", 9128) or 9128)
        if TELE_CLIENT[0] is not None:
            TELE_CLIENT[0].close()
            TELE_CLIENT[0] = None
        cmd = EMU.start(bl["exe"], rom, env, argv, os.path.dirname(bl["exe"]),
                        CARD.meta_key(rom, rom_header(rom)))
        return dict(ok=True, cmd=cmd, env=env, build=bl["id"], tele_port=TELE_PORT[0])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--port", type=int, default=9140)
    ap.add_argument("--no-open", action="store_true")
    a = ap.parse_args()

    # Empaquetado sin consola, sys.stdout es None y cualquier print revienta. Se le da un
    # sumidero para que el codigo de siempre no tenga que preguntar donde vive.
    if sys.stdout is None:
        sys.stdout = open(os.devnull, "w")
    if sys.stderr is None:
        sys.stderr = sys.stdout

    os.makedirs(ART, exist_ok=True)
    url = "http://127.0.0.1:%d/" % a.port
    try:
        srv = ThreadingHTTPServer(("127.0.0.1", a.port), H)
    except OSError:
        # El puerto ya esta cogido: o hay otra copia del lanzador abierta -- y entonces lo
        # util es ensenar SU ventana, no fallar -- o el puerto es de otro programa.
        if not a.no_open:
            open_app(url)
        return
    print("kestrel64 launcher -> %s" % url)
    if not a.no_open:
        threading.Thread(target=lambda: (time.sleep(0.4), open_app(url)), daemon=True).start()
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass
    EMU.stop()


def open_app(url):
    """Modo aplicacion: ventana sin barra de direcciones ni pestanas. Si no hay ningun
    navegador basado en Chromium, se abre en el predeterminado y ya."""
    prof = os.path.join(CACHE, "chrome-profile")
    for exe in (
        r"C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe",
        r"C:\Program Files\Google\Chrome\Application\chrome.exe",
        r"C:\Program Files (x86)\Google\Chrome\Application\chrome.exe",
    ):
        if os.path.isfile(exe):
            try:
                subprocess.Popen([exe, "--app=" + url, "--window-size=1500,950",
                                  "--user-data-dir=" + prof])
                return
            except Exception:
                pass
    webbrowser.open(url)


if __name__ == "__main__":
    main()
