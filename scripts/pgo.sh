#!/bin/sh
# Compilacion guiada por perfil (PGO): generar el perfil que usan todos los arboles.
#
# Por que: en este emulador la mitad del tiempo del anfitrion se va en codigo lleno de saltos
# (despachador del JIT, ayudantes de memoria, cita CPU<->RSP). El compilador, a ciegas, coloca
# esos saltos con reglas genericas. Con un perfil de juego real coloca la rama caliente en
# linea y aparta la fria. MEDIDO (min de 4, intercalado, i7-870): PD -7,1 %, SM64 -8,6 %,
# DK64 -4,2 %; ademas gate_all 760 -> 720 s y gate_prdp 378 -> 362 s.
#
# No toca la semantica del invitado: es colocacion de codigo del ANFITRION. La prueba es que
# el statehash de las tres tandas sale identico al del binario sin PGO, y esto lo imprime.
#
# Uso:
#   sh scripts/pgo.sh                    lote automatico LIBRE (homebrew + krom), y fusiona
#   PGO_COMMERCIAL=1 sh scripts/pgo.sh   mismo, con PD/SM64/DK64 (solo para A/B)
#   sh scripts/pgo.sh --capture "<rom>"  construye el instrumentado y lo LANZA para jugar
#   sh scripts/pgo.sh --merge            fusiona lo que haya en pgo/raw (no corre nada)
#
# Salen DOS perfiles:
#   pgo/kestrel.profdata                   lote LIBRE: el que se commitea y lleva el repo
#   pgo/local/kestrel-commercial.profdata  PGO_COMMERCIAL=1 o --capture: juegos comerciales,
#                                          NO se sube (pgo/local/ en .gitignore). Si existe,
#                                          release.sh compila con el en vez del libre.
#
# Cuando rehacerlo: tras un cambio grande de codigo caliente. Un perfil viejo NO rompe nada
# -- clang avisa con -Wno-profile-instr-out-of-date y sigue -- solo rinde menos.
#
# El binario instrumentado va un 24 % mas lento (PD en juego: 7377 ms contra 5951 ms), o sea
# 0,90x tiempo real en el juego mas apretado y de sobra en los demas. Se juega bien para
# capturar; NO es el binario que se distribuye.
set -e
cd "$(dirname "$0")/.."
export PATH=/c/msys64/clang64/bin:$PATH
ROMS=${ROMS:-/e/Claude/N64/test_roms}
OUT=pgo/kestrel.profdata
if [ -n "$PGO_COMMERCIAL" ] || [ "$1" = "--capture" ]; then
  OUT=pgo/local/kestrel-commercial.profdata; mkdir -p pgo/local
fi

merge() {
  ls pgo/raw/*.profraw >/dev/null 2>&1 || { echo "no hay nada en pgo/raw/"; exit 1; }
  llvm-profdata merge -output="$OUT" pgo/raw/*.profraw
  ls -l "$OUT"
  echo "listo. Ahora: sh scripts/release.sh   (los arboles lo cogen solos)"
}

build_gen() {
  sh "$(dirname "$0")/killown.sh"   # solo exes de este repo, no los de otra sesion
  echo "== arbol instrumentado =="
  cmake -S . -B build-pgogen -G Ninja -DCMAKE_BUILD_TYPE=Release -DKESTREL_PRDP=ON \
        -DKESTREL_PGO=gen >/dev/null
  cmake --build build-pgogen -j8 --target kestrel64
}

case "$1" in
  --merge) merge; exit 0 ;;
  --capture)
    [ -n "$2" ] || { echo 'uso: sh scripts/pgo.sh --capture "<ruta a la rom>"'; exit 2; }
    build_gen
    mkdir -p pgo/raw
    echo "== a jugar: carga un nivel de verdad y sal normal cuando lleves un rato =="
    ./build-pgogen/kestrel64.exe --play --pgo-capture "$2"
    echo
    merge
    exit 0 ;;
  "") ;;
  *) echo "opcion desconocida: $1"; exit 2 ;;
esac

build_gen
rm -rf pgo/raw; mkdir -p pgo/raw
run() { # run <nombre> <rom> <VAR=tope>   tope: KESTREL_MAXFLIPS o KESTREL_MAXINSN
  echo "-- $1"
  env LLVM_PROFILE_FILE="$PWD/pgo/raw/$1-%p.profraw" KESTREL_LOADSTATE=0 KESTREL_THROTTLE=0 \
      "$3" timeout 600 ./build-pgogen/kestrel64.exe --run "$ROMS/$2" 2>&1 \
    | tr -d '\0' | grep -oE "\[statehash\] [0-9a-f]+" || true
}
if [ -n "$PGO_COMMERCIAL" ]; then
  # Juegos comerciales (solo para comparar; el perfil que se commitea sale del lote libre).
  run PD   "Perfect Dark (Europe) (En,Fr,De,Es,It).n64" KESTREL_MAXFLIPS=1793
  run SM64 "Super Mario 64 (USA).z64"                   KESTREL_MAXFLIPS=400
  run DK64 "Donkey Kong 64 (USA).n64"                   KESTREL_MAXFLIPS=400
else
  # Lote LIBRE (homebrew y pruebas con licencia abierta): lo que se entrena y se distribuye.
  # junkrunner64 = juego 3D libdragon (CPU+RSP+RDP+audio), snapper64 = superficies RDP,
  # n64-systemtest = CPU/COP0/COP1/TLB exhaustivo, krom = RSP vectorial y RDP 3D.
  K=PeterLemon-N64
  run junk      homebrew/junkrunner64.z64 KESTREL_MAXINSN=400000000
  run snapper   snapper64.z64             KESTREL_MAXINSN=300000000
  run systest   n64-systemtest.z64        KESTREL_MAXINSN=300000000
  run niccc     $K/N64NICCC/N64NICCC.N64  KESTREL_MAXINSN=200000000
  run rsp3d     $K/RSP/XBUS/RSPTrans3DRectangle/RSPTrans3DRectangle.N64 KESTREL_MAXINSN=100000000
  run cube      $K/RDP/32BPP/Triangle/Cube/FillTriangle320x240/CubeFillTriangle32BPP320X240.N64 KESTREL_MAXINSN=100000000
fi
merge
