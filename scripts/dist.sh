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

# Avisos de licencia. NO es un adorno: los ejecutables que se reparten van enlazados
# estaticamente, o sea que dentro del binario hay codigo de parallel-rdp, volk, libc++ y
# GLFW, y sus licencias (MIT, zlib) exigen que el aviso viaje con la copia. Sin estos dos
# ficheros el zip incumple, aunque todas sean permisivas.
cp LICENSE "$OUT/LICENSE.txt"
cp THIRD-PARTY.txt "$OUT/"

# Segundo ejecutable: el MISMO emulador compilado con el rasterizador por software. El que
# se distribuye lleva parallel-RDP y cae solo a SoftRDP si Vulkan no arranca, pero esa caida
# es en caliente: quien tenga una GPU vieja, un driver roto o una maquina virtual paga el
# intento en cada arranque y no tiene forma de pedir el oraculo determinista del proyecto.
# Con los dos al lado, el rasterizador pasa a ser una eleccion de verdad en el lanzador.
SOFT="${SOFTBUILD:-build-static}"
if [ -x "$SOFT/kestrel64.exe" ]; then
  cp "$SOFT/kestrel64.exe" "$OUT/kestrel64-soft.exe"
  echo soft > "$OUT/kestrel64-soft.build"
  echo "  + kestrel64-soft.exe (SoftRDP)"
else
  SOFT=""
fi

# Que backend grafico lleva dentro el .exe se decidio con cmake y desde fuera no se ve.
# El lanzador lo necesita para saber si tiene sentido pasarle KESTREL_PRDP, asi que se
# deja anotado aqui, que es el unico sitio que conoce el dir de build de origen.
case "$BUILD" in
  *prdp*) echo prdp > "$OUT/kestrel64.build" ;;
  *)      echo soft > "$OUT/kestrel64.build" ;;
esac

# El lanzador grafico es la cara del programa para quien no vive en una terminal, y viaja
# congelado: un solo kestrel64-gui.exe con el interprete de Python y la web dentro, para no
# exigir Python instalado. Si PyInstaller no esta, se cae al arbol de fuentes de siempre.
if sh scripts/gui.sh "$OUT" >/dev/null 2>&1; then
  echo "  + kestrel64-gui.exe"
elif [ -d tools/launcher ]; then
  echo "  (sin PyInstaller: va el lanzador en Python, que necesita Python 3 instalado)"
  mkdir -p "$OUT/tools"
  cp -r tools/launcher "$OUT/tools/"
  # Bytecode y caratulas descargadas son estado de ESTA maquina, no del programa.
  rm -rf "$OUT/tools/launcher/__pycache__" "$OUT/tools/launcher/cache"
  rm -f "$OUT/tools/launcher"/*.cfg
fi

# ldd da el cierre transitivo ya resuelto. Todo lo que NO cuelgue de C:\WINDOWS es una
# dependencia del toolchain que el usuario final no tiene: se copia. Lo que cuelga de
# C:\WINDOWS (kernel32, ws2_32, winmm y sobre todo vulkan-1.dll, que instala el driver de
# la GPU) NO se copia: llevarse la vulkan-1.dll de esta maquina romperia otras.
for e in "$EXE" ${SOFT:+"$SOFT/kestrel64.exe"}; do
  ldd "$e" | while read -r name arrow path rest; do
    case "$path" in
      /c/WINDOWS/*|/C/WINDOWS/*|"") continue ;;
    esac
    [ -f "$path" ] || continue
    [ -f "$OUT/$name" ] && continue
    cp -u "$path" "$OUT/"
    echo "  + $name"
  done
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

Arrancar:   doble clic en kestrel64-gui.exe (el lanzador). Tambien vale doble clic
            en kestrel64.exe y elegir la ROM, o desde consola:
              kestrel64.exe ruta\la.rom.z64

Lanzado desde una consola el emulador arranca EN PAUSA a proposito: es el modo de
depuracion, para poder enganchar el depurador antes de la primera instruccion.
Con --run arranca corriendo.

Requisitos: Windows de 64 bits y una GPU con Vulkan. vulkan-1.dll la instala el
driver de la tarjeta grafica y NO se distribuye aqui: si Windows dice que falta,
lo que hay que actualizar es el driver de video.

Las DLL de esta carpeta son parte del programa: deben quedarse junto al .exe.
(Si no hay ninguna, este build va enlazado estatico y el .exe se basta solo.)

kestrel64-soft.exe es el MISMO emulador con el rasterizador por software (SoftRDP):
no necesita GPU ni Vulkan y es el rasterizador de referencia del proyecto, a cambio de
ir bastante mas lento. El .exe normal usa parallel-RDP (GPU) y es el recomendado; este
es la salida para una maquina sin Vulkan o para comparar. El lanzador deja elegir.

kestrel64-gui.exe es el lanzador grafico y la forma normal de usar esto: biblioteca
de ROM con caratulas, overclock por componente, mando, telemetria y depurador. No
necesita nada instalado, lleva el interprete y la interfaz dentro. El perfil, el mapa
de mando y las caratulas descargadas se guardan en %LOCALAPPDATA%\kestrel64.

(Si en esta carpeta no hay kestrel64-gui.exe sino tools\launcher\, ese paquete se
empaqueto sin PyInstaller y el lanzador necesita Python 3 instalado.)
TXT

VER=$(sed -n 's/.*kVersion = "\([^"]*\)".*/\1/p' src/core/system.hpp | head -1)
[ -n "$VER" ] || VER=dev

# Manifiesto. Va AQUI, y no en release.sh, porque aqui es donde se cierran el zip y la
# carpeta que lee el instalador: escrito despues, el portable viajaba sin el y el .iss
# recogia el del paquete anterior. Sin esto un zip suelto en un escritorio no dice de que
# commit salio ni si el arbol estaba sucio, que es justo lo que se pregunta cuando llega un
# fallo de fuera. El md5 es el del fichero ya empaquetado, no el del arbol de build.
{
  echo "kestrel64 $VER"
  echo "fecha:   $(date '+%Y-%m-%d %H:%M:%S')"
  echo "commit:  $(git rev-parse --short HEAD 2>/dev/null || echo '?')$(git diff --quiet 2>/dev/null || echo ' (arbol sucio)')"
  echo "backend: $(cat "$OUT/kestrel64.build" 2>/dev/null || echo '?')"
  for f in kestrel64.exe kestrel64-soft.exe kestrel64-gui.exe; do
    [ -f "$OUT/$f" ] && echo "md5:     $(cd "$OUT" && md5sum "$f")"
  done
} > "$OUT/VERSION.txt"
cat "$OUT/VERSION.txt"
echo

# Zip portable: es lo que se manda a otra maquina tal cual. Se usa el ZipFile de .NET y no
# Compress-Archive porque en esta maquina el modulo Microsoft.PowerShell.Archive no carga,
# ni `zip`, que no viene con MSYS2 por defecto.
ZIP="$(pwd)/kestrel64-$VER-win64.zip"
rm -f "$ZIP"
WD=$(cygpath -w "$(cd "$OUT" && pwd)"); WZ=$(cygpath -w "$ZIP")
powershell.exe -NoProfile -ExecutionPolicy Bypass -Command   "Add-Type -AssemblyName System.IO.Compression.FileSystem; [System.IO.Compression.ZipFile]::CreateFromDirectory('$WD','$WZ')"   >/dev/null 2>&1 && echo "zip: $ZIP" || echo "(zip omitido)"

echo
echo "listo: $OUT/"
ls -la "$OUT"
