cmake_minimum_required(VERSION 3.16)
project(kestrel64 CXX C)   # C needed for volk.c (parallel-rdp's Vulkan loader)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Tune for this host by default (weak Nehalem i7-870, SSE4.2). Toggle off for portable builds.
option(KESTREL_BUILD_LOCAL "Tune for the build host (-march=native)" ON)
set(KESTREL_LTO "thin" CACHE STRING "thin | full | off")

# GPU RDP via Themaister's paraLLEl-RDP. ON by default: el rasterizado en GPU es EL camino
# del emulador (ver docs/parallel-rdp-integration.md). El arbol esta vendorizado bajo
# third_party/, asi que esto no pide red ni compilador de shaders; lo unico que hace falta en
# ejecucion es un vulkan-1.dll utilizable, y si no lo hay el backend se cae solo a SoftRDP.
#
# El arbol `build/` de los gates se configura a mano con -DKESTREL_PRDP=OFF: gate_all es el
# oraculo DETERMINISTA (systemtest / md5 en lockstep) y no puede depender de que haya GPU.
option(KESTREL_PRDP "Build the paraLLEl-RDP GPU rasterizer backend" ON)
if(KESTREL_PRDP)
  include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/parallel_rdp.cmake)
endif()

# Enlazado autocontenido para el paquete que se publica (ver docs/distribucion.md). OFF por
# defecto: los builds de desarrollo enlazan bastante mas rapido en dinamico, y el .exe que
# usan los gates no necesita moverse de esta maquina.
option(KESTREL_STATIC "Link libc++/GLFW statically (self-contained .exe)" OFF)

add_executable(kestrel64
  src/main.cpp
  src/core/system.cpp
  src/core/memory.cpp
  src/core/rom.cpp
  src/core/archive.cpp
  src/core/savestate.cpp
  src/core/rewind.cpp
  src/core/cheats.cpp
  src/cpu/cpu.cpp
  src/cpu/jit.cpp
  src/net/tcp.cpp
  src/telemetry/server.cpp
  src/telemetry/hostprof.cpp
  src/video/present.cpp
  src/video/vifilter.cpp
  src/rdp/rdp.cpp
  src/rsp/rsp.cpp
  src/rsp/rspjit.cpp
  src/audio/audio.cpp
  src/ui/optdefs.cpp
  src/ui/profile.cpp
  src/ui/menu_win32.cpp
  src/ui/library_win32.cpp
)

target_include_directories(kestrel64 PRIVATE src)

# GPU RDP backend glue: compiled + linked only when KESTREL_PRDP=ON (target defined in the
# include above). The parallel_rdp target carries its own PUBLIC include dirs.
if(KESTREL_PRDP)
  target_sources(kestrel64 PRIVATE src/vrdp/vrdp.cpp)
  target_link_libraries(kestrel64 PRIVATE parallel_rdp)
  target_compile_definitions(kestrel64 PRIVATE KESTREL_PRDP=1)
endif()

# RSP core unit test (hand-assembled microcode; no windowing deps).
add_executable(rsp_test EXCLUDE_FROM_ALL
  test/rsp_test.cpp
  src/telemetry/hostprof.cpp
  src/cpu/cpu.cpp
  src/cpu/jit.cpp
  src/video/vifilter.cpp
  src/core/memory.cpp
  src/core/archive.cpp
  src/rdp/rdp.cpp
  src/rsp/rsp.cpp
  src/rsp/rspjit.cpp
  src/audio/audio.cpp
)
target_include_directories(rsp_test PRIVATE src)
if(WIN32)
  target_link_libraries(rsp_test PRIVATE winmm)
endif()

# Cartridge save-device unit test (SRAM + FlashRAM state machine; no windowing deps).
add_executable(save_test EXCLUDE_FROM_ALL
  test/save_test.cpp
  src/telemetry/hostprof.cpp
  src/cpu/cpu.cpp
  src/cpu/jit.cpp
  src/video/vifilter.cpp
  src/core/memory.cpp
  src/core/archive.cpp
  src/rdp/rdp.cpp
  src/rsp/rsp.cpp
  src/rsp/rspjit.cpp
  src/audio/audio.cpp
)
target_include_directories(save_test PRIVATE src)
if(WIN32)
  target_link_libraries(save_test PRIVATE winmm)
endif()

# Wild-address unit test: direcciones de invitado imposibles (desalineadas, pasadas del
# final de RDRAM, kuseg sin TLB, punteros de 64 bits no canonicos) deben dar excepcion de
# invitado y el mismo resultado en interprete y dynarec, nunca un fallo de acceso del host.
add_executable(wildmem_test EXCLUDE_FROM_ALL
  test/wildmem_test.cpp
  src/telemetry/hostprof.cpp
  src/cpu/cpu.cpp
  src/cpu/jit.cpp
  src/video/vifilter.cpp
  src/core/memory.cpp
  src/core/archive.cpp
  src/rdp/rdp.cpp
  src/rsp/rsp.cpp
  src/rsp/rspjit.cpp
  src/audio/audio.cpp
)
target_include_directories(wildmem_test PRIVATE src)
if(WIN32)
  target_link_libraries(wildmem_test PRIVATE winmm)
endif()

# Excepcion Watch de COP0 (ExcCode 23): WatchLo/WatchHi vigilan una doblepalabra fisica
# y disparan ANTES de completar el acceso. El interprete es el oraculo; mientras la
# vigilancia esta armada el driver no deja correr al dynarec.
add_executable(watch_test EXCLUDE_FROM_ALL
  test/watch_test.cpp
  src/telemetry/hostprof.cpp
  src/cpu/cpu.cpp
  src/cpu/jit.cpp
  src/video/vifilter.cpp
  src/core/memory.cpp
  src/core/archive.cpp
  src/rdp/rdp.cpp
  src/rsp/rsp.cpp
  src/rsp/rspjit.cpp
  src/audio/audio.cpp
)
target_include_directories(watch_test PRIVATE src)
if(WIN32)
  target_link_libraries(watch_test PRIVATE winmm)
endif()

# PIF-NUS unit test: byte de control de la PIF RAM y desafio anti-pirateria del CIC-6105.
# Se conduce por los registros del SI, que es la puerta de verdad, no por las internas.
add_executable(pif_test EXCLUDE_FROM_ALL
  test/pif_test.cpp
  src/telemetry/hostprof.cpp
  src/cpu/cpu.cpp
  src/cpu/jit.cpp
  src/video/vifilter.cpp
  src/core/memory.cpp
  src/core/archive.cpp
  src/rdp/rdp.cpp
  src/rsp/rsp.cpp
  src/rsp/rspjit.cpp
  src/audio/audio.cpp
)
target_include_directories(pif_test PRIVATE src)
if(WIN32)
  target_link_libraries(pif_test PRIVATE winmm)
endif()

# Cheat engine unit test (GameShark code families; no windowing deps).
add_executable(cheat_test EXCLUDE_FROM_ALL
  test/cheat_test.cpp
  src/core/cheats.cpp
  src/telemetry/hostprof.cpp
  src/cpu/cpu.cpp
  src/cpu/jit.cpp
  src/video/vifilter.cpp
  src/core/memory.cpp
  src/core/archive.cpp
  src/rdp/rdp.cpp
  src/rsp/rsp.cpp
  src/rsp/rspjit.cpp
  src/audio/audio.cpp
)
target_include_directories(cheat_test PRIVATE src)
if(WIN32)
  target_link_libraries(cheat_test PRIVATE winmm)
endif()

# Descompresor de contenedores (.zip/.gz). No depende de nada del emulador: solo archive.cpp.
add_executable(archive_test EXCLUDE_FROM_ALL
  test/archive_test.cpp
  src/core/archive.cpp
)
target_include_directories(archive_test PRIVATE src)

# Codec de diferencias del rebobinado. Header-only a proposito: se prueba sin arrastrar el
# resto del emulador.
add_executable(rewind_test EXCLUDE_FROM_ALL
  test/rewind_test.cpp
)
target_include_directories(rewind_test PRIVATE src)

# Los tests compilan rsp.cpp, que usa SSSE3 en el difusor de banda: sin el mismo juego de
# instrucciones que el binario principal no enlaza.
foreach(t rsp_test save_test cheat_test archive_test wildmem_test rewind_test watch_test pif_test)
  if(NOT MSVC)
    if(KESTREL_BUILD_LOCAL)
      target_compile_options(${t} PRIVATE -march=native)
    else()
      target_compile_options(${t} PRIVATE -march=x86-64-v2)
    endif()
  endif()
endforeach()

if(MSVC)
  target_compile_options(kestrel64 PRIVATE /O2 /W3)
else()
  # -O3 (not -O2): this appended after the Release profile's -O3, so a lower value
  # here would silently *downgrade* the interpreter hot loop. Keep them equal.
  target_compile_options(kestrel64 PRIVATE -O3 -Wall -Wextra -Wno-unused-parameter)
  # LTO: thin de fabrica. `-DKESTREL_LTO=full` compila con LTO monolitico (mas lento de
  # enlazar, mas margen de inlining entre unidades) y `off` lo quita, para poder medir la
  # diferencia sin tocar el fichero.
  if(NOT KESTREL_LTO STREQUAL "off")
    if(KESTREL_LTO STREQUAL "full")
      target_compile_options(kestrel64 PRIVATE -flto)
      target_link_options(kestrel64 PRIVATE -flto)
    else()
      target_compile_options(kestrel64 PRIVATE -flto=thin)
      target_link_options(kestrel64 PRIVATE -flto=thin)
    endif()
  endif()
  if(KESTREL_BUILD_LOCAL)
    target_compile_options(kestrel64 PRIVATE -march=native)
  else()
    target_compile_options(kestrel64 PRIVATE -march=x86-64-v2)  # SSE4.2 baseline for the VU later
  endif()
endif()

# --- Compilacion guiada por perfil (PGO) -------------------------------------------
# KESTREL_PGO=gen  -> binario instrumentado; cada corrida deja un .profraw.
# KESTREL_PGO=use  -> compila con el perfil fusionado que apunte KESTREL_PGO_DATA.
# No toca NADA de la semantica del invitado: es reordenacion de codigo del anfitrion, y el
# statehash de las tres tandas tiene que salir identico al del binario sin PGO. Ver
# scripts/pgo.sh.
set(KESTREL_PGO "" CACHE STRING "off | gen | use")
set(KESTREL_PGO_DATA "${CMAKE_SOURCE_DIR}/pgo/kestrel.profdata" CACHE FILEPATH "perfil fusionado para KESTREL_PGO=use")
if(NOT MSVC AND KESTREL_PGO STREQUAL "gen")
  target_compile_options(kestrel64 PRIVATE -fprofile-generate)
  target_link_options(kestrel64 PRIVATE -fprofile-generate)
  # Deja que main.cpp sepa que el tiempo de ejecucion del perfil esta enlazado: sin esto,
  # --pgo-capture tendria que llamar a simbolos que en el binario normal no existen.
  target_compile_definitions(kestrel64 PRIVATE KESTREL_PGO_GEN=1)
elseif(NOT MSVC AND KESTREL_PGO STREQUAL "use")
  if(NOT EXISTS "${KESTREL_PGO_DATA}")
    message(FATAL_ERROR "KESTREL_PGO=use pero no existe ${KESTREL_PGO_DATA}")
  endif()
  target_compile_options(kestrel64 PRIVATE "-fprofile-use=${KESTREL_PGO_DATA}"
                                           -Wno-profile-instr-out-of-date
                                           -Wno-profile-instr-unprofiled)
  target_link_options(kestrel64 PRIVATE "-fprofile-use=${KESTREL_PGO_DATA}")
endif()

# Video presenter: cross-platform Vulkan + GLFW.
find_package(Vulkan REQUIRED)
target_link_libraries(kestrel64 PRIVATE Vulkan::Vulkan glfw3)

if(WIN32)
  # comdlg32: el dialogo de "elige una ROM" cuando se abre el .exe desde el Explorador.
  # comctl32: las barras deslizantes de los numericos acotados del dialogo de opciones.
  target_link_libraries(kestrel64 PRIVATE ws2_32 gdi32 user32 winmm comdlg32 comctl32 shell32 ole32)
  if(KESTREL_STATIC AND NOT MSVC)
    # Un unico .exe sin DLL propias. Por defecto, libc++ y GLFW quedan como DLL y viven en
    # C:/msys64/clang64/bin: dentro de la shell de MSYS2 el PATH las encuentra, pero el
    # Explorador arranca el proceso sin ese PATH y Windows aborta con "no se encontro
    # glfw3.dll". clang64 trae libc++.a / libunwind.a / libglfw3.a, asi que se meten dentro.
    #
    # vulkan-1.dll sigue siendo dinamica A PROPOSITO: es el *loader* de Vulkan y tiene que ser
    # el del sistema para ver los ICD del driver instalado. Enlazarlo estatico no tendria
    # sentido aunque se pudiera.
    target_link_options(kestrel64 PRIVATE -static)
    # GLFW estatico deja de arrastrar sus dependencias por si mismo: se nombran aqui.
    target_link_libraries(kestrel64 PRIVATE shell32 ole32 uuid imm32 setupapi version cfgmgr32)
  endif()
else()
  find_package(Threads REQUIRED)
  target_link_libraries(kestrel64 PRIVATE Threads::Threads)
endif()
