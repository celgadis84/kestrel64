# Comprueba tras enlazar que el .exe no importa ninguna DLL de clang64 (libc++, libunwind,
# glfw3, libwinpthread). Si las importa, solo arranca con C:/msys64/clang64/bin en el PATH y
# fuera de la shell de MSYS2 muere con rc=127 sin mensaje. Falla el build en vez de dejarlo.
execute_process(COMMAND "${OBJDUMP}" -p "${EXE}" OUTPUT_VARIABLE out RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
  message(WARNING "check_static: no se pudo leer ${EXE} con ${OBJDUMP}")
  return()
endif()
string(REGEX MATCHALL "DLL Name: [^\r\n]+" dlls "${out}")
foreach(d ${dlls})
  string(TOLOWER "${d}" dl)
  if(dl MATCHES "libc[+][+]|libunwind|glfw3|libwinpthread|libgcc|libstdc")
    file(REMOVE "${EXE}")
    message(FATAL_ERROR "check_static: ${EXE} importa ${d}: no arrancaria fuera de la shell "
                        "clang64. Enlaza estatico (quita KESTREL_DYNAMIC) o usa ese PATH.")
  endif()
endforeach()
