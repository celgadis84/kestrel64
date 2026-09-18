#!/bin/sh
# Puerta completa: systemtest + sm64 en los seis modos, luego krom. Un solo log.
export PATH=/c/msys64/clang64/bin:$PATH
PY=/c/Users/celga/AppData/Local/Programs/Python/Python311/python
cd "$(dirname "$0")/.."
cmake --build build -j 8 >/dev/null || { echo "gate_all: build no compila"; exit 1; }

# Tests unitarios primero: cuestan segundos y se pudren solos si nadie los compila (asi se
# quedaron rsp_test y save_test sin que ningun gate lo notara).
cmake --build build -j 8 --target rsp_test save_test cheat_test archive_test wildmem_test rewind_test watch_test pif_test >/dev/null   || { echo "gate_all: los tests unitarios no compilan"; exit 1; }
for t in rsp_test save_test cheat_test archive_test wildmem_test rewind_test watch_test pif_test; do
  out=$(./build/$t.exe 2>&1 | tail -1)
  case "$out" in
    *"ALL PASS"*) echo "$t: $out" ;;
    *) echo "$t: FALLO -> $out"; exit 1 ;;
  esac
done
for m in interp jit jit-nolink threaded threaded-jit rspinterp rspnolink rewind-rtt phys phys-threaded; do
  $PY scripts/validate.py systemtest --mode $m
  $PY scripts/validate.py sm64 --mode $m
done
$PY scripts/validate.py krom --quiet
