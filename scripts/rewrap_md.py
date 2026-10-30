#!/usr/bin/env python3
"""Rewrap long Markdown prose lines to a column limit without changing content.

Only lines longer than the limit are touched; short lines and the line structure around them
stay as they are, so the diff is limited to the lines that were too long. Never touched:
fenced code blocks, table rows (first non-blank character `|`), headings, and indented code.
List items and blockquotes keep their prefix; continuation lines get the matching indent
(or the `>` marker). A word that would start a continuation line and turn it into Markdown
syntax (`-`, `1.`, `#`, `|`, `>`...) is kept on the previous line.

Usage: python scripts/rewrap_md.py [--width 100] [--check] FILE...
Encoding and line endings of each file are preserved.
"""
import argparse
import re
import sys

PREFIX = re.compile(r"^(\s*(?:>\s?)*)(\s*(?:[-*+]|\d+[.)])\s+)?")
BAD_START = re.compile(r"^(?:[-*+]|\d+[.)]|#+|>|\||=+|```.*|~~~.*)$")


def wrap_line(line, width):
    m = PREFIX.match(line)
    quote, marker = m.group(1), m.group(2) or ""
    head = quote + marker
    body = line[len(head):]
    if quote.strip():
        cont = quote if quote.endswith(" ") else quote + " "
        cont += " " * len(marker)
    else:
        cont = " " * len(head)
    words = body.split(" ")
    # Pegar a su predecesora las palabras que no pueden abrir una linea.
    units = []
    for w in words:
        if units and (w == "" or BAD_START.match(w)):
            units[-1] += " " + w
        else:
            units.append(w)
    out, cur = [], head
    first = True
    for u in units:
        base = head if first else cont
        if cur == base:
            cur += u
        elif len(cur) + 1 + len(u) <= width:
            cur += " " + u
        else:
            out.append(cur)
            first = False
            cur = cont + u
    out.append(cur)
    return out


def process(text, width, nl):
    lines = text.split(nl)
    out, fence, changed = [], None, 0
    for ln in lines:
        s = ln.lstrip()
        if fence:
            out.append(ln)
            if s.startswith(fence):
                fence = None
            continue
        if s.startswith("```") or s.startswith("~~~"):
            fence = s[:3]
            out.append(ln)
            continue
        quote_free = re.sub(r"^(\s*>\s?)+", "", ln)
        if (len(ln) <= width or s.startswith("|") or quote_free.lstrip().startswith("|")
                or s.startswith("#") or (ln.startswith("    ") and not PREFIX.match(ln).group(2)
                                         and not out_is_list(out))):
            out.append(ln)
            continue
        new = wrap_line(ln, width)
        changed += len(new) > 1
        out.extend(new)
    return nl.join(out), changed


def out_is_list(out):
    """Una linea con 4+ espacios es continuacion de lista si lo anterior no esta en blanco."""
    return bool(out) and out[-1].strip() != ""


def words(text, nl):
    """Palabras del texto sin los `>` de cita del principio de linea (el rewrap los repite)."""
    return " ".join(re.sub(r"^(\s*>\s?)+", "", ln) for ln in text.split(nl)).split()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--width", type=int, default=100)
    ap.add_argument("--check", action="store_true", help="report only, do not write")
    ap.add_argument("files", nargs="+")
    a = ap.parse_args()
    for f in a.files:
        raw = open(f, "rb").read()
        enc = "utf-8"
        try:
            text = raw.decode(enc)
        except UnicodeDecodeError:
            enc = "cp1252"
            text = raw.decode(enc)
        nl = "\r\n" if "\r\n" in text else "\n"
        new, n = process(text, a.width, nl)
        # La comprobacion de contenido: mismas palabras en el mismo orden.
        if words(new, nl) != words(text, nl):
            sys.exit(f"{f}: el rewrap cambiaria el contenido, abortado")
        print(f"{f}: {n} lineas recortadas")
        if n and not a.check:
            open(f, "wb").write(new.encode(enc))


if __name__ == "__main__":
    main()
