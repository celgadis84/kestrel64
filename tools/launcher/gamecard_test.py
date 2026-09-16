# -*- coding: utf-8 -*-
"""Prueba de gamecard.py (ficha de juego):  python tools/launcher/gamecard_test.py"""
import os
import shutil
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gamecard as G  # noqa: E402


def main():
    d = tempfile.mkdtemp(prefix="k64card_")
    try:
        run(d)
    finally:
        shutil.rmtree(d, ignore_errors=True)
    print("gamecard_test: ALL PASS")


def run(d):
    # Nombres = los del emulador: sin contenedor y sin extension
    rom = os.path.join(d, "Super Mario 64 (USA).z64.zip")
    open(rom, "wb").write(b"x")
    assert G.rom_base(rom) == os.path.join(d, "Super Mario 64 (USA)")
    assert G.rom_base(os.path.join(d, "Game.zip")) == os.path.join(d, "Game")
    assert G.rom_base(os.path.join(d, "a.b", "Game")) == os.path.join(d, "a.b", "Game")

    # Partidas y estados
    open(os.path.join(d, "Super Mario 64 (USA).eep"), "wb").write(b"\0" * 512)
    open(os.path.join(d, "Super Mario 64 (USA).st1"), "wb").write(b"\0" * 9)
    s = G.saves(rom)
    assert [x["kind"] for x in s] == ["eep", "st1"], s

    # Trucos: mismo lector que src/core/cheats.cpp, y reescritura que respeta el fichero
    cht = os.path.join(d, "Super Mario 64 (USA).cht")
    open(cht, "wb").write(b"# SM64\r\n8033B21D 0064\r\n[Vidas infinitas] ; nota\r\n"
                          b"8033B21D 0064\r\n\r\n# otro\r\n[-Monedas]\r\n8033B21A:0063\r\nzz\r\n")
    c = G.cheats(rom)["list"]
    assert [x["name"] for x in c] == ["sin nombre", "Vidas infinitas", "Monedas"], c
    assert [x["on"] for x in c] == [True, True, False] and c[2]["bad"] == 1
    G.set_cheats(rom, {"0": False, "1": False, "2": True})
    raw = open(cht, "rb").read()
    assert b"[-Vidas infinitas] ; nota\r\n" in raw, raw        # comentario conservado
    assert b"\r\n[Monedas]\r\n" in raw and b"[-sin nombre]\r\n8033B21D" in raw, raw
    assert b"\n" not in raw.replace(b"\r\n", b"")               # CRLF conservado
    assert [x["on"] for x in G.cheats(rom)["list"]] == [False, False, True]
    G.add_cheat(rom, "Nuevo [x]", "81000000 FFFF\n88000000 0001")
    c = G.cheats(rom)["list"]
    assert c[-1]["name"] == "Nuevo x" and c[-1]["inert"] == 1 and len(c[-1]["codes"]) == 2
    for bad in (("", "80000000 0001"), ("x", ""), ("x", "hola")):
        try:
            G.add_cheat(rom, *bad)
            assert 0, bad
        except ValueError:
            pass
    G.del_cheat(rom, 2)
    c = G.cheats(rom)["list"]
    assert [x["name"] for x in c] == ["sin nombre", "Vidas infinitas", "Nuevo x"], c
    rom2 = os.path.join(d, "Otro.n64")
    G.add_cheat(rom2, "Primero", "80000000 0001", on=False)
    c = G.cheats(rom2)
    assert c["exists"] and c["list"][0]["on"] is False

    # Manuales: por nombre de fichero o nombre interno, carpeta de la ROM o manuals/
    os.makedirs(os.path.join(d, "manuals"))
    open(os.path.join(d, "manuals", "Super Mario 64 (USA) - manual.pdf"), "wb").write(b"%PDF")
    open(os.path.join(d, "SUPERMARIO64.txt"), "wb").write(b"hi")
    open(os.path.join(d, "Zelda.pdf"), "wb").write(b"%PDF")
    open(os.path.join(d, "Super Mario 64 (USA).exe"), "wb").write(b"MZ")
    m = G.manual_list(rom, {"name": "SUPER MARIO 64"})
    assert [x["file"] for x in m] == ["SUPERMARIO64.txt", "Super Mario 64 (USA) - manual.pdf"], m
    p = G.manual_store(rom, "x.pdf", b"%PDF2")
    assert p.endswith(os.path.join("manuals", "Super Mario 64 (USA).pdf"))
    p = G.manual_store(rom, "y.PDF", b"%PDF3")
    assert p.endswith("Super Mario 64 (USA) (2).pdf"), p
    for name, data in (("x.exe", b"MZ"), ("x.pdf", b"")):
        try:
            G.manual_store(rom, name, data)
            assert 0, name
        except ValueError:
            pass

    # Notas del lanzador
    G.meta_save(d, "ABC", {"year": 1996, "favorite": 1, "bogus": 3})
    G.meta_played(d, "ABC", 100)
    G.meta_played(d, "ABC", 1)
    m = G.meta_load(d, "ABC")
    assert m["year"] == "1996" and m["favorite"] is True and m["plays"] == 1, m
    assert m["seconds"] == 100 and "bogus" not in m, m


if __name__ == "__main__":
    main()
