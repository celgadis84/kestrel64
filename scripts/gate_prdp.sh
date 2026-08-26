#!/bin/sh
# Puerta del backend GPU (parallel-rdp). NO sustituye a gate_all.sh: ese sigue
# siendo el oraculo determinista sobre SoftRDP, que no depende de que haya GPU.
# Esta corre el mismo material contra el exe de build-prdp con KESTREL_PRDP=1.
#
# `prdp` = interp lockstep (oraculo lento), `prdp-jit` = la configuracion real.
# sm64 tiene su propio md5 por modo: parallel-rdp no es pixel-identico a SoftRDP
# y no tiene por que serlo; lo que se exige es que no cambie entre corridas.
export PATH=/c/msys64/clang64/bin:$PATH
PY=/c/Users/celga/AppData/Local/Programs/Python/Python311/python
cd "$(dirname "$0")/.."
KESTREL_EXE="$(pwd)/build-prdp/kestrel64.exe"; export KESTREL_EXE
[ -f "$KESTREL_EXE" ] || { echo "gate_prdp: falta $KESTREL_EXE (cmake -B build-prdp -DKESTREL_PRDP=ON)"; exit 1; }
# Reconstruir SIEMPRE. Validar con un exe viejo da un ALL OK que no cubre el cambio que se
# acaba de hacer: paso una vez, con el binario de build-prdp de hace tres horas.
cmake --build build-prdp -j 8 >/dev/null || { echo "gate_prdp: build-prdp no compila"; exit 1; }
# systemtest solo en lockstep. En `prdp-jit` fallan cuatro casos "RDP STATUS"
# (0xa8/0xa9 en vez de 0x80/0x81) y NO son un fallo de semantica: ver
# docs/parallel-rdp-integration.md, "Known divergence: GPU submit latency".
$PY scripts/validate.py systemtest --mode prdp
for m in prdp prdp-jit; do
  $PY scripts/validate.py sm64 --mode $m
done
$PY scripts/validate.py krom --mode prdp --quiet
