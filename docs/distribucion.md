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

## Instalador

`installer/kestrel64.iss` (Inno Setup 6) se compila **sobre `dist/`**, no sobre el arbol de
build, para que el instalador y el zip lleven exactamente los mismos bytes y no exista una
segunda lista de DLL que se pueda desincronizar.

```sh
sh scripts/dist.sh
"C:/Program Files (x86)/Inno Setup 6/ISCC.exe" installer/kestrel64.iss
```

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
