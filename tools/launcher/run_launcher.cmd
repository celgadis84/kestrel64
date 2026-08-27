@echo off
rem Lanzador grafico de kestrel64. Abre el servidor local y una ventana de navegador
rem en modo aplicacion. Cerrar la ventana no mata el servidor: usar Ctrl+C aqui.
setlocal
set PY=C:\Users\%USERNAME%\AppData\Local\Programs\Python\Python311\python.exe
if not exist "%PY%" set PY=python
"%PY%" "%~dp0kestrel_launcher.py" %*
endlocal
