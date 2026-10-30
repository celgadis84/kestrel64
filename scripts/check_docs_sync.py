#!/usr/bin/env python3
"""Check that bilingual document pairs stay structurally in sync.

Each pair (English, Spanish) must have:
  - the same `Synced with: <commit>` line, and
  - the same heading skeleton: the same sequence of heading levels, and the same section
    numbers where headings are numbered ("## 3. Memory and bus" <-> "## 3. Memoria y bus").
    Heading text is not compared, since it is translated.

Usage: python scripts/check_docs_sync.py   (exit 1 on mismatch)
"""
import os
import re
import sys

ROOT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")
PAIRS = [("docs/ARCHITECTURE.md", "docs/ARQUITECTURA.md"),
         ("docs/TESTING.md", "docs/PRUEBAS.md")]

HEAD = re.compile(r"^(#{1,6})\s+(\d+(?:\.\d+)*\.?)?")
SYNC = re.compile(r"^Synced with:\s*(\S+)")


def skeleton(path):
    sync, heads, fence = None, [], False
    with open(os.path.join(ROOT, path), encoding="utf-8") as f:
        for ln in f:
            if ln.lstrip().startswith("```"):
                fence = not fence
                continue
            if fence:
                continue
            m = SYNC.match(ln)
            if m and sync is None:
                sync = m.group(1)
            m = HEAD.match(ln)
            if m:
                heads.append((len(m.group(1)), (m.group(2) or "").rstrip(".")))
    return sync, heads


def main():
    total = 0
    for en, es in PAIRS:
        bad = 0
        s1, h1 = skeleton(en)
        s2, h2 = skeleton(es)
        if s1 is None or s2 is None:
            print(f"{en} / {es}: missing 'Synced with:' line")
            bad += 1
        elif s1 != s2:
            print(f"{en} / {es}: 'Synced with:' differs ({s1} vs {s2})")
            bad += 1
        if h1 != h2:
            bad += 1
            print(f"{en} / {es}: heading skeleton differs")
            for i in range(max(len(h1), len(h2))):
                a = h1[i] if i < len(h1) else None
                b = h2[i] if i < len(h2) else None
                if a != b:
                    print(f"  first difference at heading #{i + 1}: {a} vs {b}")
                    break
        if not bad:
            print(f"{en} / {es}: OK ({len(h1)} headings, synced with {s1})")
        total += bad
    sys.exit(1 if total else 0)


if __name__ == "__main__":
    main()
