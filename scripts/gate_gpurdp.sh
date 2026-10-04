#!/bin/sh
# Puerta del GPU-RDP propio (docs/GPU-RDP.md), KESTREL_GPURDP=1 sobre el exe de
# build-prdp-static (el que viaja). A diferencia de gate_prdp.sh aqui NO hay referencias
# propias: el GPU-RDP tiene que ser SoftRDP bit a bit, asi que sm64 se compara con sm64.txt
# y krom con la referencia de interp. Necesita GPU con Vulkan.
# Uso: sh scripts/gate_gpurdp.sh   (~7 min; release.sh --quick antes)
PY=/c/Users/celga/AppData/Local/Programs/Python/Python311/python
cd "$(dirname "$0")/.."
KESTREL_EXE="$(pwd)/build-prdp-static/kestrel64.exe"; export KESTREL_EXE
[ -f "$KESTREL_EXE" ] || { echo "gate_gpurdp: falta $KESTREL_EXE (sh scripts/release.sh --quick)"; exit 1; }
rc=0
$PY scripts/validate.py systemtest --mode gpurdp || rc=1
for m in gpurdp gpurdp-jit; do
  $PY scripts/validate.py sm64 --mode $m || rc=1
done
$PY scripts/validate.py krom --mode gpurdp --vs interp --quiet || rc=1
[ $rc = 0 ] && echo "gate_gpurdp: ALL OK" || echo "gate_gpurdp: FAILED"
exit $rc
