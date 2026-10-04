#!/bin/sh
# Puerta RAPIDA por cambio (~2 min). El barrido completo (gate_all + gate_prdp, ~20 min)
# se deja para el cierre de fase / bloque de puntos, no para cada cambio.
#
# Corre contra los exe STATIC que deja release.sh (no piden clang64 en PATH):
#   systemtest threaded-jit (SoftRDP)  -> CPU/RCP/tiempos en la config real
#   sm64 interp (SoftRDP, lockstep)    -> md5 determinista del oraculo
#   sm64 prdp-jit (parallel-rdp)       -> md5 del backend GPU con JIT
#   sm64 gpurdp-jit (GPU-RDP propio)   -> md5 de SoftRDP (tiene que ser bit a bit)
# Extras segun lo que toque el cambio (argumentos, se pueden juntar):
#   krom  -> suite krom SoftRDP entera (rasterizador; ~6 min)
#   lock  -> systemtest interp (oraculo lockstep)
#   phys  -> systemtest + sm64 en modo phys
#   thar0 -> tiempos RDP contra HW (Thar0, ~2 min): rmse tiene que quedar en 0.1332
# Uso: sh scripts/gate_quick.sh [krom] [lock] [phys] [thar0]
PY=/c/Users/celga/AppData/Local/Programs/Python/Python311/python
cd "$(dirname "$0")/.."
SOFT="$(pwd)/build-static/kestrel64.exe"
PRDP="$(pwd)/build-prdp-static/kestrel64.exe"
rc=0
KESTREL_EXE="$SOFT" $PY scripts/validate.py systemtest --mode threaded-jit || rc=1
KESTREL_EXE="$SOFT" $PY scripts/validate.py sm64 --mode interp || rc=1
KESTREL_EXE="$PRDP" $PY scripts/validate.py sm64 --mode prdp-jit || rc=1
KESTREL_EXE="$PRDP" $PY scripts/validate.py sm64 --mode gpurdp-jit || rc=1
for x in "$@"; do
  case "$x" in
    krom) KESTREL_EXE="$SOFT" $PY scripts/validate.py krom --quiet || rc=1 ;;
    lock) KESTREL_EXE="$SOFT" $PY scripts/validate.py systemtest --mode interp || rc=1 ;;
    phys) KESTREL_EXE="$SOFT" $PY scripts/validate.py systemtest --mode phys-threaded || rc=1
          KESTREL_EXE="$SOFT" $PY scripts/validate.py sm64 --mode phys || rc=1 ;;
    thar0) timeout 300 "$SOFT" ../rdp-timing-tests/rdp_fill_timing.z64 --run > out/rdptiming.txt 2>/dev/null
           r=$($PY scripts/rdptiming.py compare out/rdptiming.txt | head -1); echo "thar0: $r"
           echo "$r" | grep -q "rmse=0.1332 " || rc=1 ;;
    *) echo "gate_quick: extra desconocido '$x'"; rc=1 ;;
  esac
done
[ $rc = 0 ] && echo "gate_quick: ALL OK" || echo "gate_quick: FAILED"
exit $rc
