#!/usr/bin/env python3
"""Symbolise a KESTREL_HOSTPROF dump.

The profiler prints module-relative RIP buckets; this maps them onto the exe's
symbol table (nm) and aggregates per function, which is the form you can act on.

    KESTREL_HOSTPROF=1 kestrel64.exe rom.z64 --run 2> prof.log
    python scripts/hostprof.py prof.log [build/kestrel64.exe]
"""
import subprocess, sys, re, bisect, collections, os

log = sys.argv[1]
exe = sys.argv[2] if len(sys.argv) > 2 else "build/kestrel64.exe"

# nm prints absolute VAs for PE; the preferred base is the low bits of any .text
# symbol rounded down, so derive it from the image's own default base (0x140000000
# for mingw x86_64) rather than assuming.
syms = []
out = subprocess.run(["nm", "--defined-only", exe], capture_output=True, text=True).stdout
for line in out.splitlines():
    p = line.split(None, 2)
    if len(p) == 3 and p[1] in "TtWw":
        syms.append((int(p[0], 16), p[2]))
syms.sort()
base = 0x140000000
syms = [(a - base, n) for a, n in syms if a >= base]
addrs = [a for a, _ in syms]

# Only the LAST dump block in the log (the profile is cumulative).
blocks = re.split(r"\[hostprof\] \d+ samples", open(log, errors="replace").read())
hits = re.findall(r"\[hostprof\]\s+[\d.]+%\s+\+0x([0-9a-f]+)\s+\((\d+)\)", blocks[-1])
per = collections.Counter()
total = 0
for off, n in hits:
    off, n = int(off, 16), int(n)
    i = bisect.bisect_right(addrs, off) - 1
    per[syms[i][1] if i >= 0 else "?"] += n
    total += n
for name, n in per.most_common(30):
    print(f"{100.0*n/total:6.2f}%  {name}")
print(f"(top-40 buckets only, {total} samples)")
