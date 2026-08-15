#!/usr/bin/env bash
# Thin wrapper around scripts/validate.py.
#
# Two things it fixes that cost time otherwise:
#  - the comparator needs a Python with numpy + Pillow (the Windows one here),
#    and it must be resolved BEFORE the MSYS CLANG64 dir goes on PATH, or the
#    clang64 python.exe wins and every compare dies with "No module named numpy";
#  - the emulator links against the MSYS2 CLANG64 runtime, so without that dir on
#    PATH it dies with "libc++.dll: cannot open shared object file".
#
# usage: bash scripts/validate.sh all --mode interp
set -u

PY=${KESTREL_PY:-}
if [ -z "$PY" ]; then
  for cand in python python3 py; do
    p=$(command -v "$cand" 2>/dev/null) || continue
    if "$p" -c "import numpy, PIL" >/dev/null 2>&1; then PY=$p; break; fi
  done
fi
if [ -z "$PY" ]; then
  echo "error: no python with numpy+Pillow found (set KESTREL_PY)" >&2
  exit 2
fi

export PATH=/c/msys64/clang64/bin:$PATH
exec "$PY" "$(dirname "$0")/validate.py" "$@"
