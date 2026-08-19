cmake_minimum_required(VERSION 3.16)
project(kestrel64 CXX C)   # C needed for volk.c (parallel-rdp's Vulkan loader)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# Tune for this host by default (weak Nehalem i7-870, SSE4.2). Toggle off for portable builds.
option(KESTREL_BUILD_LOCAL "Tune for the build host (-march=native)" ON)

# GPU RDP via Themaister's paraLLEl-RDP. OFF by default: the deterministic core
# (systemtest / lockstep md5) must not depend on it. See cmake/parallel_rdp.cmake.
option(KESTREL_PRDP "Build the paraLLEl-RDP GPU rasterizer backend" OFF)
if(KESTREL_PRDP)
  include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/parallel_rdp.cmake)
endif()

add_executable(kestrel64
  src/main.cpp
  src/core/system.cpp
  src/core/memory.cpp
  src/core/rom.cpp
  src/cpu/cpu.cpp
  src/cpu/jit.cpp
  src/net/tcp.cpp
  src/telemetry/server.cpp
  src/telemetry/hostprof.cpp
  src/video/present.cpp
  src/rdp/rdp.cpp
  src/rsp/rsp.cpp
  src/audio/audio.cpp
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
  src/core/memory.cpp
  src/rdp/rdp.cpp
  src/rsp/rsp.cpp
  src/audio/audio.cpp
)
target_include_directories(rsp_test PRIVATE src)
if(WIN32)
  target_link_libraries(rsp_test PRIVATE winmm)
endif()

# Cartridge save-device unit test (SRAM + FlashRAM state machine; no windowing deps).
add_executable(save_test EXCLUDE_FROM_ALL
  test/save_test.cpp
  src/core/memory.cpp
  src/rdp/rdp.cpp
  src/rsp/rsp.cpp
  src/audio/audio.cpp
)
target_include_directories(save_test PRIVATE src)
if(WIN32)
  target_link_libraries(save_test PRIVATE winmm)
endif()

if(MSVC)
  target_compile_options(kestrel64 PRIVATE /O2 /W3)
else()
  # -O3 (not -O2): this appended after the Release profile's -O3, so a lower value
  # here would silently *downgrade* the interpreter hot loop. Keep them equal.
  target_compile_options(kestrel64 PRIVATE -O3 -Wall -Wextra -Wno-unused-parameter)
  target_compile_options(kestrel64 PRIVATE -flto=thin)
  target_link_options(kestrel64 PRIVATE -flto=thin)
  if(KESTREL_BUILD_LOCAL)
    target_compile_options(kestrel64 PRIVATE -march=native)
  else()
    target_compile_options(kestrel64 PRIVATE -march=x86-64-v2)  # SSE4.2 baseline for the VU later
  endif()
endif()

# Video presenter: cross-platform Vulkan + GLFW.
find_package(Vulkan REQUIRED)
target_link_libraries(kestrel64 PRIVATE Vulkan::Vulkan glfw3)

if(WIN32)
  target_link_libraries(kestrel64 PRIVATE ws2_32 gdi32 user32 winmm)
  # NOTE: no -static — GLFW/Vulkan pull system DLLs; a fully static link fights
  # the loader. The clang/libc++ runtime is found via the clang64 bin on PATH.
else()
  find_package(Threads REQUIRED)
  target_link_libraries(kestrel64 PRIVATE Threads::Threads)
endif()
