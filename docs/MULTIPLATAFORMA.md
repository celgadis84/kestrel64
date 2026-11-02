# Multiplataforma (GAPS #8)

> **English summary.** Status of porting kestrel64 beyond Windows. Linux x86-64 (WSL Ubuntu
> 24.04, clang 18) builds and links with SoftRDP; 9/9 unit tests pass and n64-systemtest passes
> in `interp` and `jit` modes. The x86-64 JITs keep their Win64 ABI on SysV hosts via
> `KJIT_ABI` (`__attribute__((ms_abi))`). Open: threaded-jit hang, audio backend, GPU-RDP build,
> aarch64 (macOS / Android: interpreter + NEON first, ARM JIT after the JIT refactor), libretro core.

Fecha de este estado: 2026-10-31.

## Hecho

- **Linux x86-64 compila y enlaza** (`-DKESTREL_GPU=OFF -DKESTREL_BUILD_LOCAL=OFF`, clang 18,
  libc++/lld, GLFW 3.3 y Vulkan del sistema). Cambios:
  - `memory.cpp`: `tlIsRspThread` fuera del `#ifdef _WIN32`.
  - `sdcard.hpp`: espacio `sdio` (`_fseeki64`/`_ftelli64`/`_wfopen` frente a `fseeko`/`ftello`/`fopen`).
  - `server_dbg.cpp`: cast explicito (`u64` es `unsigned long` en Linux).
  - `present.cpp`: `vifilter.hpp` fuera del bloque Win32.
  - CMake: GLFW por `find_package(glfw3)` fuera de Windows; `debugger_win32.cpp` siempre en la
    lista (fuera de Windows solo trae funciones vacias).
- **ABI de los JIT**: los emisores de CPU y RSP generan codigo Win64 (argumentos en
  rcx/rdx/r8/r9, rsi/rdi/xmm6-15 conservados). En SysV se pide esa ABI al compilador con
  `KJIT_ABI` (`src/core/types.hpp`) en los tipos de bloque (`BlockFn`, `SmokeFn`, `CallBailFn`)
  y en todos los ayudantes `extern "C"` que llama el codigo emitido. En Windows la macro esta vacia.
  Sin esto `rsp_test` moria con SIGSEGV.
- **Pruebas en Linux**: 9/9 tests unitarios. systemtest `interp` y `jit`: 0/3721 · 0/2 · 0/6.
  El proceso sale solo en ~26 s; con `validate.py` dentro de WSL el tiempo informado es 300 s
  (pendiente de mirar: el proceso suelto acaba, la llamada desde el script no).

## Entorno de pruebas (WSL)

- Ubuntu 24.04 con clang-18, lld, libc++, libglfw3-dev, libvulkan-dev, glslang-tools,
  python3-numpy/pil.
- Arbol de compilacion fuera del repo (`~/k64/<nombre>`); fuentes por `/mnt/<unidad>/...`.
- `KESTREL_EXE=<exe linux> KESTREL_TESTROMS=<carpeta> python3 scripts/validate.py systemtest --mode interp`.
- Desde Windows, lanzar `wsl` por PowerShell (Git Bash destroza las rutas).

## Pendiente (orden)

1. **threaded-jit se cuelga** en Linux en "Running RSP SSV" (sin resumen). interp/jit pasan,
   asi que apunta a la cita entre hilos o al RSP JIT en su hilo. Probar `rspinterp`,
   `threaded` y `KESTREL_RSPJIT=0` para aislarlo.
2. **Por que `validate.py` tarda los 300 s** cuando el exe suelto sale a los 26 s.
3. Resto de modos de `gate_all` y sm64 md5 (debe dar `b5521b24...` igual que Windows).
4. **Audio**: hoy `waveOut`. Backend portable (ALSA/Pulse, o miniaudio si la licencia encaja);
   mientras tanto silencio.
5. **Red** (`winsock2`) y `VirtualAlloc`: compilan protegidos; comprobar que el camino POSIX
   funciona (netplay, servidor de telemetria en 9128).
6. **GPU-RDP** (`-DKESTREL_GPU=ON`) en Linux: Vulkan + glslang ya instalados.
7. **CI**: trabajo Linux en `.github/workflows/build.yml` (compilar + 9 tests).
8. **aarch64 (macOS Apple Silicon, Android)**:
   - Fase A: el JIT x86 no existe en ARM -> solo interprete (CPU y RSP) con
     `#if defined(__x86_64__)` alrededor de los emisores. La VU usa SSE4.1: traducir a NEON
     (o escalar como respaldo) con compilacion condicional. Validar con compilacion cruzada
     `--target=aarch64-linux-gnu` y `qemu-aarch64` en WSL (systemtest interp).
   - Fase B: JIT aarch64 propio (emisor nuevo). **Despues de la refactorizacion del JIT**
     (PLAN-CALIDAD): separar frontend/IR de emisor abarata mucho un segundo backend.
   - macOS: Vulkan por MoltenVK; ventana GLFW; empaquetado .app y firma.
   - Android: NDK, ventana/entrada nativas (sin GLFW), Vulkan nativo, audio AAudio/Oboe.
     Probablemente via el core libretro (punto 9) antes que una app propia.
9. **Core libretro (RetroArch)**: viable. Cabecera libretro MIT; `retro_run` por cuadro,
   video por `video_refresh` o render HW Vulkan (negociar contexto para GPU-RDP), audio por
   lotes, entrada por sondeo. Requiere aislar `present.cpp`/GLFW/UI Win32 detras de una
   interfaz de frontend. Da Android, consolas y Linux/macOS de golpe.
