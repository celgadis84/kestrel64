# -*- coding: utf-8 -*-
"""Ficha de juego del lanzador: lo que hay alrededor de una ROM.

Todo sale de ficheros al lado del cartucho, con los MISMOS nombres que usa el emulador,
para que la ficha ensene lo que el emulador de verdad va a cargar y no una idea propia:

  <base>.eep/.sra/.fla     partida del cartucho      (src/core/memory.cpp, withExt)
  <base>.mpk, .mpk2..4     Controller Pak por mando  (idem)
  <base>.st0..st9          estados guardados         (src/core/savestate.cpp, stateSlotPath)
  <base>.cht               trucos, se cargan solos   (src/core/cheats.cpp, loadForRom)

<base> es la ruta de la ROM sin la extension del contenedor (.zip/.gz) y sin la suya
(archive::stripContainerExt + quitar una extension), igual que en el emulador.

Los manuales no los usa el emulador: se buscan en la carpeta de la ROM y en su
subcarpeta `manuals/`, por nombre que EMPIECE por la base del fichero o por el nombre
interno del cartucho.

Los datos que no estan en ningun fichero del juego (anio, estudio, notas, veces jugado,
tiempo) van al estado del lanzador, un json por juego, con clave = CRC de la cabecera:
el mismo cartucho renombrado o comprimido sigue siendo la misma ficha.
"""

import json
import os
import re
import time

MANUAL_EXT = {".pdf": "application/pdf", ".html": "text/html; charset=utf-8",
              ".htm": "text/html; charset=utf-8", ".txt": "text/plain; charset=utf-8",
              ".md": "text/plain; charset=utf-8", ".png": "image/png",
              ".jpg": "image/jpeg", ".jpeg": "image/jpeg", ".webp": "image/webp"}
META_FIELDS = ("year", "developer", "publisher", "genre", "players", "notes", "favorite")
MAX_UPLOAD = 64 * 1048576
HEX = re.compile(r"[0-9A-Fa-f]{1,8}")


# ------------------------------------------------------------------ nombres
def rom_base(path):
    """Ruta de la ROM sin contenedor y sin extension: la raiz de todos sus ficheros."""
    root, ext = os.path.splitext(path)
    p = root if ext.lower() in (".zip", ".gz") else path
    root, ext = os.path.splitext(p)   # splitext ignora un punto del directorio
    return root if ext else p


def _norm(s):
    return re.sub(r"[^a-z0-9]+", "", (s or "").lower())


def _atomic(f, data):
    os.makedirs(os.path.dirname(f) or ".", exist_ok=True)
    tmp = f + ".tmp"
    with open(tmp, "wb") as fh:
        fh.write(data)
    os.replace(tmp, f)


# ------------------------------------------------------------------ partidas
def saves(path):
    base = rom_base(path)
    kinds = [(".eep", "EEPROM"), (".sra", "SRAM"), (".fla", "FlashRAM"), (".mpk", "Controller Pak 1")]
    kinds += [(".mpk%d" % i, "Controller Pak %d" % i) for i in (2, 3, 4)]
    kinds += [(".st%d" % i, "Estado %d" % i) for i in range(10)]
    out = []
    for ext, label in kinds:
        f = base + ext
        if os.path.isfile(f):
            st = os.stat(f)
            out.append(dict(kind=ext[1:], label=label, file=os.path.basename(f),
                            size=st.st_size, mtime=int(st.st_mtime)))
    return out


# ------------------------------------------------------------------ manuales
def manuals(path, header=None):
    d = os.path.dirname(path)
    stem = _norm(os.path.basename(rom_base(path)))
    iname = _norm((header or {}).get("name", ""))
    found, seen = [], set()
    for sub in ("", "manuals", "manuales"):
        dd = os.path.join(d, sub) if sub else d
        if not os.path.isdir(dd):
            continue
        try:
            ents = sorted(os.scandir(dd), key=lambda e: e.name.lower())
        except OSError:
            continue
        for e in ents:
            n, ext = os.path.splitext(e.name)
            if not e.is_file() or ext.lower() not in MANUAL_EXT:
                continue
            k = _norm(n)
            key = os.path.normcase(os.path.realpath(e.path))
            if key in seen:
                continue   # Windows no distingue manuals/Manuals: la misma carpeta dos veces
            if (stem and k.startswith(stem)) or (len(iname) >= 4 and k.startswith(iname)):
                seen.add(key)
                found.append(e.path)
    return found


def manual_list(path, header=None):
    return [dict(i=i, file=os.path.basename(f), ext=os.path.splitext(f)[1].lower()[1:],
                 size=os.path.getsize(f)) for i, f in enumerate(manuals(path, header))]


def manual_store(path, filename, data):
    """Guarda un manual subido en <carpeta de la ROM>/manuals/<base><ext>."""
    ext = os.path.splitext(filename or "")[1].lower()
    if ext not in MANUAL_EXT:
        raise ValueError("tipo de manual no admitido: %s" % (ext or "sin extension"))
    if not data or len(data) > MAX_UPLOAD:
        raise ValueError("manual vacio o demasiado grande")
    dd = os.path.join(os.path.dirname(path), "manuals")
    stem = os.path.basename(rom_base(path))
    dst = os.path.join(dd, stem + ext)
    n = 2
    while os.path.exists(dst):
        dst = os.path.join(dd, "%s (%d)%s" % (stem, n, ext))
        n += 1
    _atomic(dst, data)
    return dst


# ------------------------------------------------------------------ trucos
# El lector copia al de src/core/cheats.cpp (Cheats::loadFile): comentario con # o ;,
# cabecera [nombre] con '-' delante = apagado, y codigos antes de la primera cabecera caen
# en un truco implicito "sin nombre". Se guardan las lineas crudas para reescribir SOLO la
# cabecera que cambia: los comentarios y el formato del usuario sobreviven.
def cht_path(path):
    return rom_base(path) + ".cht"


def _read_lines(f):
    with open(f, "rb") as fh:
        raw = fh.read()
    nl = "\r\n" if b"\r\n" in raw else "\n"
    return raw.decode("utf-8", "surrogateescape").splitlines(), nl


def _write_lines(f, lines, nl):
    _atomic(f, (nl.join(lines) + nl).encode("utf-8", "surrogateescape"))


def _body(ln):
    m = re.search(r"[#;]", ln)
    return (ln[:m.start()] if m else ln).strip()


def _code(body):
    """Dos numeros hexadecimales con los separadores del emulador, o None."""
    toks = [t for t in re.split(r"[\s,:]+", body) if t]
    if len(toks) < 2 or not HEX.fullmatch(toks[0]) or not HEX.fullmatch(toks[1]):
        return None
    return "%08X %04X" % (int(toks[0], 16), int(toks[1], 16) & 0xFFFF)


def parse_cht(lines):
    out = []
    for idx, ln in enumerate(lines):
        body = _body(ln)
        if not body:
            continue
        if body.startswith("["):
            e = body.find("]")
            name = body[1:e if e >= 0 else None].strip()
            on = True
            if name.startswith("-"):
                on, name = False, name[1:].strip()
            out.append(dict(name=name or "sin nombre", on=on, line=idx, first=idx, codes=[], bad=0))
            continue
        if not out:
            out.append(dict(name="sin nombre", on=True, line=-1, first=idx, codes=[], bad=0))
        c = _code(body)
        if c:
            out[-1]["codes"].append(c)
        else:
            out[-1]["bad"] += 1
    for i, c in enumerate(out):
        c["id"] = i
        # 88/89 y CC/DE/EE/FF el emulador los lee pero no los aplica (docs/CHEATS.md)
        c["inert"] = sum(1 for x in c["codes"] if x[:2] in ("88", "89", "CC", "DE", "EE", "FF"))
    return out


def cheats(path):
    f = cht_path(path)
    if not os.path.isfile(f):
        return dict(file=os.path.basename(f), exists=False, list=[])
    lines, _ = _read_lines(f)
    return dict(file=os.path.basename(f), exists=True, list=parse_cht(lines))


def set_cheats(path, states):
    """states = {id: bool}. Solo cambia el '-' de las cabeceras afectadas."""
    f = cht_path(path)
    lines, nl = _read_lines(f)
    inserts = []
    for c in parse_cht(lines):
        want = states.get(str(c["id"]), states.get(c["id"]))
        if want is None or bool(want) == c["on"]:
            continue
        head = "[%s%s]" % ("" if want else "-", c["name"])
        if c["line"] < 0:
            inserts.append((c["first"], head))
            continue
        ln = lines[c["line"]]
        s = ln.lstrip()
        e = s.find("]")
        lines[c["line"]] = ln[:len(ln) - len(s)] + head + (s[e + 1:] if e >= 0 else "")
    for at, head in sorted(inserts, reverse=True):
        lines.insert(at, head)
    _write_lines(f, lines, nl)
    return cheats(path)


def add_cheat(path, name, codes, on=True):
    name = re.sub(r"[\[\]\r\n#;]", "", name or "").strip().lstrip("-").strip()
    if not name:
        raise ValueError("el truco necesita nombre")
    good = []
    for ln in re.split(r"[\r\n]+", codes or ""):
        body = _body(ln)
        if not body:
            continue
        c = _code(body)
        if not c:
            raise ValueError("linea ilegible: %s" % body)
        good.append(c)
    if not good:
        raise ValueError("el truco no tiene codigos")
    f = cht_path(path)
    if os.path.isfile(f):
        lines, nl = _read_lines(f)
        while lines and not lines[-1].strip():
            lines.pop()
        lines.append("")
    else:
        lines, nl = ["# " + os.path.basename(rom_base(path))], "\n"
    lines.append("[%s%s]" % ("" if on else "-", name))
    lines += good
    _write_lines(f, lines, nl)
    return cheats(path)


def del_cheat(path, cid):
    f = cht_path(path)
    lines, nl = _read_lines(f)
    lst = parse_cht(lines)
    cid = int(cid)
    if not 0 <= cid < len(lst):
        raise ValueError("no existe ese truco")
    a = lst[cid]["first"]
    b = lst[cid + 1]["first"] if cid + 1 < len(lst) else len(lines)
    # los comentarios/blancos justo antes del siguiente truco son suyos, no de este
    while b > a + 1 and (not lines[b - 1].strip() or not _body(lines[b - 1])) and cid + 1 < len(lst):
        b -= 1
    del lines[a:b]
    _write_lines(f, lines, nl)
    return cheats(path)


# ------------------------------------------------------------------ metadatos del lanzador
def meta_key(path, header):
    crc = (header or {}).get("crc")
    return crc or _norm(os.path.basename(rom_base(path))) or "rom"


def _meta_file(state, key):
    return os.path.join(state, "games", re.sub(r"[^A-Za-z0-9_-]", "_", key) + ".json")


def meta_load(state, key):
    try:
        with open(_meta_file(state, key), "r", encoding="utf-8") as f:
            return json.load(f)
    except Exception:
        return {}


def _meta_write(state, key, m):
    _atomic(_meta_file(state, key), json.dumps(m, ensure_ascii=False, indent=1).encode("utf-8"))


def meta_save(state, key, upd):
    m = meta_load(state, key)
    for k in META_FIELDS:
        if k in upd:
            v = upd[k]
            m[k] = bool(v) if k == "favorite" else str(v if v is not None else "")[:4000]
    _meta_write(state, key, m)
    return m


def meta_played(state, key, seconds):
    """Una sesion terminada: suma una partida y su tiempo. Menos de 3 s no cuenta (un
    arranque fallido o cerrado al instante no es haber jugado)."""
    if seconds < 3:
        return
    m = meta_load(state, key)
    m["plays"] = int(m.get("plays", 0)) + 1
    m["seconds"] = int(m.get("seconds", 0)) + int(seconds)
    m["last"] = int(time.time())
    _meta_write(state, key, m)


def card(state, path, header):
    key = meta_key(path, header)
    return dict(path=path, base=os.path.basename(rom_base(path)), key=key, header=header,
                saves=saves(path), manuals=manual_list(path, header), cheats=cheats(path),
                meta=meta_load(state, key))
