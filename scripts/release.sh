#!/bin/sh
# UN SOLO COMANDO PARA GENERAR EL PRODUCTO ENTERO.
#
# Regla del proyecto: cada generacion de .exe produce TODO -- los tres arboles de build, el
# lanzador grafico congelado, el paquete portable y el instalador de Windows. Antes esto
# eran cuatro invocaciones que habia que recordar (y que se olvidaban), asi que dist/ y el
# instalador se quedaban con el .exe de hace semanas mientras el codigo avanzaba. Aqui esta
# la secuencia completa, escrita una vez, para no volver a deducirla.
#
# Que sale:
#   build/                     exe SoftRDP dinamico  (oraculo determinista de las puertas)
#   build-prdp/                exe parallel-RDP dinamico (puerta de GPU)
#   build-prdp-static/         exe parallel-RDP ESTATICO = el que se distribuye
#   build-static/              exe SoftRDP ESTATICO      = el que se distribuye tambien
#   dist/                      kestrel64.exe (GPU) + kestrel64-soft.exe (CPU)
#                              + kestrel64-gui.exe + LEEME.txt + VERSION.txt
#   kestrel64-<ver>-win64.zip  paquete portable
#   kestrel64-<ver>-setup.exe  instalador (si hay Inno Setup)
#
# Uso:
#   sh scripts/release.sh            # todo
#   sh scripts/release.sh --gates    # ademas pasa gate_all + gate_prdp ANTES de empaquetar
#   sh scripts/release.sh --quick    # solo los arboles estaticos + empaquetado (iteracion rapida)
#   NOISS=1 sh scripts/release.sh    # sin instalador
set -e
cd "$(dirname "$0")/.."
export PATH=/c/msys64/clang64/bin:$PATH

GATES=""; QUICK=""
for a in "$@"; do
  case "$a" in
    --gates) GATES=1 ;;
    --quick) QUICK=1 ;;
    -h|--help) sed -n '2,24p' "$0"; exit 0 ;;
    *) echo "opcion desconocida: $a"; exit 2 ;;
  esac
done

# Windows mantiene abierto el .exe que este corriendo y el enlazado falla con un error
# ("permission denied") que no parece lo que es. Siempre antes de compilar.
kill_exes() {
  taskkill //F //IM kestrel64.exe      >/dev/null 2>&1 || true
  taskkill //F //IM kestrel64-soft.exe >/dev/null 2>&1 || true
  taskkill //F //IM kestrel64-gui.exe >/dev/null 2>&1 || true
}

conf() { # conf <dir> <opciones de cmake...>
  d=$1; shift
  [ -f "$d/CMakeCache.txt" ] || cmake -S . -B "$d" -G Ninja -DCMAKE_BUILD_TYPE=Release "$@"
}

T0=$(date +%s)
kill_exes

if [ -z "$QUICK" ]; then
  echo "== build/ (SoftRDP, oraculo) =="
  conf build
  cmake --build build -j8
  echo "== build-prdp/ (parallel-RDP) =="
  conf build-prdp -DKESTREL_PRDP=ON
  cmake --build build-prdp -j8
fi

if [ -n "$GATES" ]; then
  echo "== puertas =="
  sh scripts/gate_all.sh
  sh scripts/gate_prdp.sh
  kill_exes
fi

echo "== build-prdp-static/ (lo que se distribuye) =="
conf build-prdp-static -DKESTREL_PRDP=ON -DKESTREL_STATIC=ON
cmake --build build-prdp-static -j8

# El paquete lleva los DOS rasterizadores. El de GPU es el recomendado y el que se llama
# kestrel64.exe; este sale como kestrel64-soft.exe y es la salida para una maquina sin
# Vulkan (o para comparar contra el rasterizador de referencia sin recompilar nada).
echo "== build-static/ (SoftRDP, tambien se distribuye) =="
conf build-static -DKESTREL_STATIC=ON
cmake --build build-static -j8

# dist.sh ya congela el lanzador (scripts/gui.sh), copia lo que haga falta, escribe el LEEME
# el VERSION.txt y genera el zip; pack.sh anade el instalador. No se duplica nada aqui.
echo "== paquete + instalador =="
BUILD=build-prdp-static SOFTBUILD=build-static sh scripts/pack.sh

# El manifiesto (version, commit, md5) lo escribe dist.sh, que es quien cierra el zip y la
# carpeta del instalador: escrito aqui llegaba tarde y el portable salia sin el.
VER=$(sed -n 's/.*kVersion = "\([^"]*\)".*/\1/p' src/core/system.hpp | head -1)
[ -n "$VER" ] || VER=dev

echo
echo "== listo en $(( $(date +%s) - T0 )) s =="
ls -1 dist/
ls -1 kestrel64-"$VER"-win64.zip kestrel64-"$VER"-setup.exe 2>/dev/null || true
