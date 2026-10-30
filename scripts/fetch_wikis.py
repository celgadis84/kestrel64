#!/usr/bin/env python3
"""Download the N64 community wiki pages kestrel64 uses as reference into docs/wiki/.

docs/wiki/ is in .gitignore: the wikis are public, so the repo keeps links, not copies.
Sources:
  libdragon  https://github.com/DragonMinded/libdragon/wiki   (GitHub wiki, Markdown)
  n64brew    https://n64brew.dev/wiki/                         (MediaWiki, raw wikitext)

Usage: python scripts/fetch_wikis.py [--force]
Existing files are kept unless --force is given.
"""
import argparse
import os
import sys
import urllib.parse
import urllib.request

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "docs", "wiki")

LIBDRAGON = "https://raw.githubusercontent.com/wiki/DragonMinded/libdragon/{}.md"
LIBDRAGON_PAGES = [
    "API-stability", "Audio-playback", "Audioconv64", "Compression", "Cpaktool",
    "DSO-(dynamic-libraries)", "Debugging-via-gdb", "Fighting-memory-fragmentation", "Home",
    "Installing-libdragon", "Logos", "MPEG1-Player", "Magma-[WIP]", "Mkfont", "Mksprite",
    "OpenGL-on-N64", "Opus-decompression", "PAL-NTSC-support", "PAL60", "Preview-branch",
    "Projects-using-libdragon", "Random-numbers", "Rdpq", "Stable-branch--Changelog",
    "Upgrade-troubleshooting", "Videoconv64",
]

N64BREW = "https://n64brew.dev/wiki/{}?action=raw"
N64BREW_PAGES = [
    "Reality_Display_Processor", "Reality_Signal_Processor",
    "Reality_Signal_Processor/CPU_Core", "Reality_Signal_Processor/CPU_Pipeline",
    "Reality_Signal_Processor/Interface",
]


def fetch(url, dest, force):
    if os.path.exists(dest) and not force:
        return "kept"
    req = urllib.request.Request(url, headers={"User-Agent": "kestrel64-fetch-wikis"})
    with urllib.request.urlopen(req, timeout=30) as r:
        data = r.read()
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    with open(dest, "wb") as f:
        f.write(data)
    return f"{len(data)} B"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--force", action="store_true", help="re-download existing pages")
    a = ap.parse_args()
    jobs = [(LIBDRAGON.format(urllib.parse.quote(p)), os.path.join(ROOT, "libdragon", p + ".md"))
            for p in LIBDRAGON_PAGES]
    jobs += [(N64BREW.format(urllib.parse.quote(p)),
              os.path.join(ROOT, "n64brew", p.replace("/", "_") + ".wiki"))
             for p in N64BREW_PAGES]
    bad = 0
    for url, dest in jobs:
        try:
            print(f"{os.path.relpath(dest, ROOT)}: {fetch(url, dest, a.force)}")
        except Exception as e:  # noqa: BLE001 -- una pagina caida no para el resto
            bad += 1
            print(f"{os.path.relpath(dest, ROOT)}: ERROR {e}")
    print(f"{len(jobs) - bad}/{len(jobs)} pages in {os.path.normpath(ROOT)}")
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
