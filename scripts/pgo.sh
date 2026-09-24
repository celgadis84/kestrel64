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
#   sh scripts/pgo.sh                    lote automatico: los tres juegos, y fusiona
#   sh scripts/pgo.sh --capture "<rom>"  construye el instrumentado y lo LANZA para jugar
#   sh scripts/pgo.sh --merge            fusiona lo que haya en pgo/raw (no corre nada)
#
# Sale: pgo/kestrel.profdata  (se commitea; los arboles lo usan SOLO si existe)
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

merge() {
  ls pgo/raw/*.profraw >/dev/null 2>&1 || { echo "no hay nada en pgo/raw/"; exit 1; }
  llvm-profdata merge -output=pgo/kestrel.profdata pgo/raw/*.profraw
  ls -l pgo/kestrel.profdata
  echo "listo. Ahora: sh scripts/release.sh   (los arboles lo cogen solos)"
}

build_gen() {
  taskkill //F //IM kestrel64.exe >/dev/null 2>&1 || true
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
run() { # run <nombre> <rom> <flips>
  echo "-- $1"
  LLVM_PROFILE_FILE="$PWD/pgo/raw/$1-%p.profraw" KESTREL_LOADSTATE=0 KESTREL_THROTTLE=0 \
  KESTREL_MAXFLIPS=$3 ./build-pgogen/kestrel64.exe --run "$ROMS/$2" 2>&1 \
    | tr -d '\0' | grep -oE "\[statehash\] [0-9a-f]+" || true
}
# Los tres perfiles del proyecto: PD es el caso apretado (en juego, desde la ranura 0),
# SM64 el de arranque con mucha compilacion, DK64 el de RSP pesado.
run PD   "Perfect Dark (Europe) (En,Fr,De,Es,It).n64" 1793
run SM64 "Super Mario 64 (USA).z64"                   400
run DK64 "Donkey Kong 64 (USA).n64"                   400
merge
