# Distribuir kestrel64 en Windows

## El sintoma

Doble clic en `build\kestrel64.exe` y Windows contesta:

> La ejecución de código no puede continuar porque no se encontró glfw3.dll.

No es un fallo del emulador. El enlazado por defecto deja **libc++** y **GLFW** como DLL, y
esas dos viven en `C:\msys64\clang64\bin`. Dentro de la shell de MSYS2 ese directorio esta en
el `PATH` y el cargador las encuentra; el Explorador de Windows arranca el proceso con el
`PATH` del usuario, donde no estan.

Dependencias reales del `.exe` (`ldd build/kestrel64.exe`), separadas en dos grupos:

| Grupo | DLL | De donde sale |
|---|---|---|
| Toolchain (hay que llevarselas) | `libc++.dll`, `glfw3.dll` | MSYS2/clang64 |
| Sistema (nunca copiar) | `kernel32`, `ws2_32`, `winmm`, `user32`, `gdi32`, `ucrtbase`… | Windows |
| Driver (nunca copiar) | `vulkan-1.dll` | El driver de la GPU |

`vulkan-1.dll` merece parrafo aparte: aunque `ldd` la resuelva a `C:\WINDOWS\SYSTEM32`, la
instala el driver de video, no Windows. Distribuir la de esta maquina rompe otras
aplicaciones en la del usuario. Si falta, se arregla actualizando el driver.

## Dos formas de arreglarlo

### 1. DLL al lado del .exe — `sh scripts/dist.sh`

El directorio de la aplicacion es lo **primero** que mira el cargador de Windows, por delante
del `PATH`, asi que una copia junto al `.exe` gana siempre. El script resuelve el cierre
transitivo con `ldd`, filtra lo que cuelga de `C:\WINDOWS` y deja en `dist/` el `.exe`, sus
DLL y un `LEEME.txt`; ademas empaqueta `kestrel64-<version>-win64.zip`, que es lo que se
manda a otra maquina tal cual.

Funciona con cualquier build y no exige recompilar. Es la red de seguridad: si algun dia
entra una dependencia nueva (SDL para el audio, por ejemplo), `dist.sh` la recoge sola.

### 2. Enlazado estatico — `-DKESTREL_STATIC=ON`

`clang64` trae `libc++.a`, `libunwind.a` y `libglfw3.a`, asi que el runtime de C++ y GLFW
pueden ir *dentro* del binario. Queda un unico `kestrel64.exe` sin ninguna DLL propia, que es
lo que se quiere para el instalador. `vulkan-1.dll` sigue siendo dinamica **a proposito**: es
el *loader* de Vulkan, tiene que ser el del sistema para ver los ICD del driver instalado.

Los builds de desarrollo siguen dinamicos (enlazan mas rapido); la opcion esta pensada para
el paquete que se publica.

```sh
cmake -S . -B build-static -G Ninja -DCMAKE_BUILD_TYPE=Release -DKESTREL_STATIC=ON
cmake --build build-static -j8
ldd build-static/kestrel64.exe | grep -v /c/WINDOWS     # no imprime nada
```

Medido: 1.7 MB de `.exe` (748 KB dinamico + los ~1 MB de runtime que antes iban en las DLL),
cero dependencias propias, SM64 arranca y renderiza con el `PATH` vacio.

## El lanzador tambien es autocontenido — `sh scripts/gui.sh`

El lanzador esta escrito en Python, pero exigir Python 3 instalado a quien solo quiere jugar
es una barrera absurda. PyInstaller lo congela: `scripts/gui.sh` produce un unico
`kestrel64-gui.exe` (~8.4 MB) con el interprete, la biblioteca estandar y `web/` dentro.

Lo unico que cambia en el codigo es donde vive cada cosa, y lo decide una constante
(`FROZEN` en `kestrel_launcher.py`):

| | arbol de fuentes | congelado |
|---|---|---|
| `web/` | `tools/launcher/web` | `sys._MEIPASS/web` (temporal que desempaqueta PyInstaller) |
| emulador | `build*/kestrel64.exe` | `kestrel64.exe` **al lado** del `.exe` del lanzador |
| perfil, mando, caratulas | `tools/launcher/` | `%LOCALAPPDATA%\kestrel64` |

La tercera fila no es un capricho: `Archivos de programa` no es escribible, y el perfil se
guarda solo, sin boton de guardar. La segunda tampoco: instalado hay **un** emulador, no un
menu de builds. Con que backend grafico se compilo ese emulador no se ve desde fuera del
binario, asi que `dist.sh` deja la nota al lado en `kestrel64.build` (`soft` o `prdp`) y el
lanzador la lee para saber si tiene sentido pasarle `KESTREL_PRDP`.

Dos detalles de `--windowed` (sin consola detras) que rompen si no se tratan: `sys.stdout`
es `None` y cualquier `print` revienta -- se le da `os.devnull` al arrancar --, y una segunda
copia no puede coger el puerto 9140, asi que en vez de morir en silencio abre la ventana de
la instancia que ya esta corriendo.

`dist.sh` llama a `gui.sh` solo; si PyInstaller no esta instalado se cae al arbol de fuentes
de siempre y lo dice en el `LEEME.txt`.

## Instalador

`installer/kestrel64.iss` se compila **sobre `dist/`**, no sobre el arbol de build, para que el
instalador y el zip lleven exactamente los mismos bytes y no exista una segunda lista de DLL que
se pueda desincronizar.

```sh
sh scripts/pack.sh            # compila estatico + dist/ + zip + instalador
```

La version del instalador sale de `kVersion` (`src/core/system.hpp`) y se le pasa a ISCC con
`/DAppVer=`; el literal del `.iss` es solo el respaldo de quien lo invoque a mano. Dos detalles
de entorno:

- Inno Setup esta instalado como **7** (`C:\Program Files\Inno Setup 7\ISCC.exe`), no como 6.
  `pack.sh` lo **busca** en vez de fijar la ruta, porque ya cambio una vez.
- MSYS2 traduce a ruta de Windows cualquier argumento que empiece por `/`, asi que `/DAppVer=...`
  le llegaba a ISCC como un segundo nombre de script (`You may not specify more than one script
  filename`). Se excluye con `MSYS2_ARG_CONV_EXCL="/D"`.

## Por que hace falta `pack.sh` y no basta con `dist.sh`

Las baterias (`gate_all.sh`, `gate_prdp.sh`) recompilan `build/` y `build-prdp/` antes de correr.
El arbol **estatico** no lo recompila nadie, y es el unico `.exe` que arranca fuera de MSYS2. Sin
un paso explicito, `dist/` y el instalador se quedan clavados en la fecha del ultimo empaquetado
mientras el codigo avanza — y lo que se prueba desde el lanzador no es el codigo actual. Paso en
esta misma linea de trabajo: `dist/` del 28-08 contra fuentes del 02-09.

`pack.sh` es ese paso, y ademas mata cualquier `kestrel64.exe`/`kestrel64-gui.exe` vivo antes de
enlazar (Windows no deja reescribir un fichero abierto).

Asocia opcionalmente `.z64` / `.n64` / `.v64`, y el verbo de apertura lleva `--run` puesto:
abrir una ROM desde el Explorador tiene que **ejecutarla**, no dejar una ventana negra.

## Por que el doble clic no se quedaba en pausa

Sin `--run` el emulador arranca pausado, que es lo correcto para depurar (permite enganchar
el MCP antes de la primera instruccion) y lo que asumen los scripts de validacion. Pero para
alguien que hace doble clic es indistinguible de un cuelgue.

La distincion se hace con `GetConsoleProcessList`: si el proceso es el **unico** adjunto a su
consola, esa consola la creo Windows al lanzarlo desde el Explorador. Lanzado desde MSYS2 o
`cmd` la shell tambien esta adjunta y el contador es mayor, con lo que el arranque en pausa se
conserva intacto para el flujo de depuracion y para los gates.
