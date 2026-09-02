#!/bin/sh
# Deja listo lo que se le da a alguien que no compila: el .exe estatico, el zip portable
# y el instalador de Windows.
#
# Existe porque las baterias (gate_all.sh / gate_prdp.sh) recompilan build/ y build-prdp/,
# pero NO el arbol estatico, que es el unico que arranca fuera de MSYS2. Sin este paso el
# dist/ se queda con el .exe de hace semanas y lo que se prueba desde el lanzador no es
# el codigo actual. Un solo comando para que no vuelva a desincronizarse.
#
# Uso:  sh scripts/pack.sh            (build-prdp-static: GPU + enlazado estatico)
#       NOISS=1 sh scripts/pack.sh    (sin instalador, solo dist/ + zip)
set -e
cd "$(dirname "$0")/.."
export PATH=/c/msys64/clang64/bin:$PATH

BUILD=${BUILD:-build-prdp-static}

# El .exe no se puede reenlazar mientras una copia corre: Windows tiene el fichero abierto.
taskkill //F //IM kestrel64.exe >/dev/null 2>&1 || true
taskkill //F //IM kestrel64-gui.exe >/dev/null 2>&1 || true

# KESTREL_STATIC mete libc++/GLFW dentro del .exe: cero DLL que copiar y cero
# "no se encontro glfw3.dll" en una maquina sin MSYS2.
[ -f "$BUILD/CMakeCache.txt" ] || \
  cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DKESTREL_PRDP=ON -DKESTREL_STATIC=ON
cmake --build "$BUILD" -j8

sh scripts/dist.sh "$BUILD" dist

[ -n "$NOISS" ] && exit 0

# Inno Setup: la version instalada cambia de sitio (6 -> 7), asi que se busca en vez de
# fijar la ruta.
ISCC=""
for c in "/c/Program Files/Inno Setup 7/ISCC.exe" \
         "/c/Program Files (x86)/Inno Setup 6/ISCC.exe" \
         "/c/Program Files/Inno Setup 6/ISCC.exe"; do
  [ -x "$c" ] && { ISCC=$c; break; }
done
[ -n "$ISCC" ] || { echo "(sin Inno Setup: no se genera instalador)"; exit 0; }

# La version sale del codigo, no de una segunda copia en el .iss que se quede vieja.
VER=$(sed -n 's/.*kVersion = "\([^"]*\)".*/\1/p' src/core/system.hpp | head -1)
[ -n "$VER" ] || VER=dev
# MSYS2 traduce a ruta de Windows cualquier argumento que empiece por "/", y ISCC acabaria
# viendo un segundo nombre de script en vez de la definicion. MSYS2_ARG_CONV_EXCL lo excluye.
MSYS2_ARG_CONV_EXCL="/D" "$ISCC" "/DAppVer=$VER" "$(cygpath -w installer/kestrel64.iss)" | tail -3
