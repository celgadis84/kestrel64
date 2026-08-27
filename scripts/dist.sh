#!/bin/sh
# Empaqueta un kestrel64 que arranque con doble clic fuera de MSYS2.
#
# El error "no se encontro glfw3.dll" no es un fallo del programa: el enlazado por defecto
# deja libc++ y GLFW como DLL, y esas viven en /c/msys64/clang64/bin. Dentro de la shell de
# MSYS2 el PATH las encuentra; el Explorador de Windows no. Aqui se resuelve de la unica
# forma que Windows respeta siempre: la DLL al lado del .exe (el directorio de la aplicacion
# es lo primero que mira el cargador, por delante del PATH).
#
# Con -DKESTREL_STATIC=ON no queda ninguna DLL que copiar y este script solo mueve el .exe.
#
# Uso:  sh scripts/dist.sh [dir-de-build] [dir-de-salida]
set -e
cd "$(dirname "$0")/.."

BUILD="${1:-}"
if [ -z "$BUILD" ]; then
  for b in build-prdp-static build-static build-prdp build; do
    [ -x "$b/kestrel64.exe" ] && { BUILD=$b; break; }
  done
fi
OUT="${2:-dist}"
EXE="$BUILD/kestrel64.exe"
[ -x "$EXE" ] || { echo "no hay $EXE -- compila primero"; exit 1; }

echo "empaquetando desde $BUILD/"
rm -rf "$OUT"; mkdir -p "$OUT"
cp "$EXE" "$OUT/"

# El lanzador grafico, si esta compilado/presente, viaja con el emulador: es la cara del
# programa para quien no vive en una terminal.
[ -d tools/launcher ] && { mkdir -p "$OUT/tools"; cp -r tools/launcher "$OUT/tools/"; }

# ldd da el cierre transitivo ya resuelto. Todo lo que NO cuelgue de C:\WINDOWS es una
# dependencia del toolchain que el usuario final no tiene: se copia. Lo que cuelga de
# C:\WINDOWS (kernel32, ws2_32, winmm y sobre todo vulkan-1.dll, que instala el driver de
# la GPU) NO se copia: llevarse la vulkan-1.dll de esta maquina romperia otras.
ldd "$EXE" | while read -r name arrow path rest; do
  case "$path" in
    /c/WINDOWS/*|/C/WINDOWS/*|"") continue ;;
  esac
  [ -f "$path" ] || continue
  cp -u "$path" "$OUT/"
  echo "  + $name"
done

# Segunda pasada: una DLL copiada puede arrastrar las suyas (libc++ -> libunwind, por
# ejemplo, segun como se haya construido el paquete de MSYS2).
for i in 1 2 3; do
  before=$(ls "$OUT" | wc -l)
  for d in "$OUT"/*.dll; do
    [ -f "$d" ] || continue
    ldd "$d" | while read -r name arrow path rest; do
      case "$path" in
        /c/WINDOWS/*|/C/WINDOWS/*|"") continue ;;
      esac
      [ -f "$path" ] || continue
      [ -f "$OUT/$name" ] && continue
      cp "$path" "$OUT/"; echo "  + $name (indirecta)"
    done
  done
  [ "$(ls "$OUT" | wc -l)" = "$before" ] && break
done

cat > "$OUT/LEEME.txt" <<'TXT'
kestrel64 -- emulador de Nintendo 64

Arrancar:   doble clic en kestrel64.exe y elegir la ROM, o desde consola:
              kestrel64.exe ruta\la.rom.z64

Lanzado desde una consola el emulador arranca EN PAUSA a proposito: es el modo de
depuracion, para poder enganchar el depurador antes de la primera instruccion.
Con --run arranca corriendo.

Requisitos: Windows de 64 bits y una GPU con Vulkan. vulkan-1.dll la instala el
driver de la tarjeta grafica y NO se distribuye aqui: si Windows dice que falta,
lo que hay que actualizar es el driver de video.

Las DLL de esta carpeta son parte del programa: deben quedarse junto al .exe.
(Si no hay ninguna, este build va enlazado estatico y el .exe se basta solo.)

tools\launcher\run_launcher.cmd abre el lanzador grafico: biblioteca de ROM con
caratulas, overclock por componente, mando, telemetria y depurador. Ese lanzador
esta escrito en Python y necesita Python 3 instalado; el emulador en si no.
TXT

# Zip portable: es lo que se manda a otra maquina tal cual. Se usa el ZipFile de .NET y no
# Compress-Archive porque en esta maquina el modulo Microsoft.PowerShell.Archive no carga,
# ni `zip`, que no viene con MSYS2 por defecto.
VER=$(sed -n 's/.*kVersion = "\([^"]*\)".*/\1/p' src/core/system.hpp | head -1)
[ -n "$VER" ] || VER=dev
ZIP="$(pwd)/kestrel64-$VER-win64.zip"
rm -f "$ZIP"
WD=$(cygpath -w "$(cd "$OUT" && pwd)"); WZ=$(cygpath -w "$ZIP")
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command   "Add-Type -AssemblyName System.IO.Compression.FileSystem; [System.IO.Compression.ZipFile]::CreateFromDirectory('$WD','$WZ')"   >/dev/null 2>&1 && echo "zip: $ZIP" || echo "(zip omitido)"

echo
echo "listo: $OUT/"
ls -la "$OUT"
