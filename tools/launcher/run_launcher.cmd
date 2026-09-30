@echo off
rem Lanzador grafico de kestrel64. Abre el servidor local y una ventana de navegador
rem en modo aplicacion. Cerrar la ventana no mata el servidor: usar Ctrl+C aqui.
setlocal
rem clang64 en PATH: los .exe de build*/ son de MSYS2 CLANG64. Desde 2026-09-30 todos se
rem enlazan estaticos (CMakeLists, candado check_static.cmake), pero un exe dinamico viejo
rem sin esto muere en silencio (rc=127, falta libc++.dll/glfw3.dll). Hijos lo heredan.
if exist C:\msys64\clang64\bin set PATH=C:\msys64\clang64\bin;%PATH%
set PY=C:\Users\%USERNAME%\AppData\Local\Programs\Python\Python311\python.exe
if not exist "%PY%" set PY=python
"%PY%" "%~dp0kestrel_launcher.py" %*
endlocal
