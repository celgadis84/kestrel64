#!/bin/sh
# Congela el lanzador grafico en un unico kestrel64-gui.exe.
#
# El lanzador esta escrito en Python, pero exigir Python 3 a quien solo quiere jugar es
# una barrera absurda: PyInstaller mete el interprete, la biblioteca estandar y la web
# (index.html/app.js/style.css) dentro del propio .exe. Al arrancar, PyInstaller los
# desempaqueta en un temporal y deja la ruta en sys._MEIPASS; el lanzador ya lo sabe
# (constante FROZEN) y de ahi saca WEB, mientras que el perfil, el mapa de mando y las
# caratulas se van a %LOCALAPPDATA%\kestrel64, porque Archivos de programa no es escribible.
#
# --windowed = sin ventana de consola detras: la interfaz es la del navegador.
#
# Uso:  sh scripts/gui.sh [dir-de-salida]
set -e
cd "$(dirname "$0")/.."

PY=${PY:-/c/Users/celga/AppData/Local/Programs/Python/Python311/python}
[ -x "$PY" ] || PY=python
OUT="${1:-dist}"
WORK=build-gui

"$PY" -c "import PyInstaller" 2>/dev/null || { echo "falta PyInstaller: $PY -m pip install pyinstaller"; exit 1; }

rm -rf "$WORK"; mkdir -p "$WORK"
"$PY" -m PyInstaller \
  --noconfirm --clean --onefile --windowed \
  --name kestrel64-gui \
  --distpath "$WORK/dist" --workpath "$WORK/work" --specpath "$WORK" \
  --paths "$(cygpath -w "$(pwd)/tools/launcher")" \
  --hidden-import options --hidden-import tele \
  --add-data "$(cygpath -w "$(pwd)/tools/launcher/web");web" \
  tools/launcher/kestrel_launcher.py

EXE="$WORK/dist/kestrel64-gui.exe"
[ -f "$EXE" ] || { echo "PyInstaller no dejo $EXE"; exit 1; }
mkdir -p "$OUT"
cp "$EXE" "$OUT/"
echo "listo: $OUT/kestrel64-gui.exe ($(du -h "$EXE" | cut -f1))"
