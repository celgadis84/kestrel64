#!/bin/sh
# Puerta completa: systemtest + sm64 en los seis modos, luego krom. Un solo log.
export PATH=/c/msys64/clang64/bin:$PATH
PY=/c/Users/celga/AppData/Local/Programs/Python/Python311/python
cd "$(dirname "$0")/.."
for m in interp jit jit-nolink threaded threaded-jit rspinterp; do
  $PY scripts/validate.py systemtest --mode $m
  $PY scripts/validate.py sm64 --mode $m
done
$PY scripts/validate.py krom --quiet
