#!/usr/bin/env python3
"""Quick look at a framebuffer dump, or a dump against its reference.

    python scripts/imgstat.py out.bmp [ref.png]

Prints size, mean colour and distinct-colour count for each image, plus the
accuracy triple when a reference is given. Distinct-colour count is the useful
one when a test renders black: 1 means nothing was drawn at all, which separates
"the RDP wrote the wrong colours" from "the RDP never wrote".
"""
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from validate import compare, load_rgb   # noqa: E402


def stat(p):
    import numpy as np
    a = load_rgb(p)
    uniq = len(np.unique(a.reshape(-1, 3), axis=0))
    print(f"{p}: {a.shape[1]}x{a.shape[0]} mean={a.mean(axis=(0,1)).round(1)} colors={uniq}")


def main():
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    stat(sys.argv[1])
    if len(sys.argv) > 2:
        stat(sys.argv[2])
        ex, cl, rmse, note = compare(sys.argv[1], sys.argv[2])
        print(f"exact={ex:.2f}% close={cl:.2f}% rmse={rmse:.2f} {note}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
