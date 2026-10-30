# kestrel64 — índice de documentación / documentation index

Cada documento lleva una línea en español y otra en inglés. Casi todos están escritos en español;
los marcados *(EN)* están en inglés.
Each document has one line in Spanish and one in English. Most are written in Spanish; those
marked *(EN)* are in English.

## Estado y planes / Status and plans

- [STATUS.md](STATUS.md) — estado actual por subsistema (corto, se actualiza en sitio).
  Current state per subsystem (short, updated in place).
- [GAPS.md](GAPS.md) — huecos frente a otros emuladores y pendientes propios, ordenados.
  Gaps versus other emulators and own to-dos, in priority order.
- [PLAN-CALIDAD.md](PLAN-CALIDAD.md) — plan de calidad de código y documentación.
  Code and documentation quality plan.
- [history/STATUS-2026.md](history/STATUS-2026.md) — diario cronológico: qué se probó, qué midió,
  qué se revirtió. Chronological log: what was tried, measured and reverted.
- [history/SUBSCHED-2026-09.md](history/SUBSCHED-2026-09.md) *(EN)* — planificación del RDP al
  enviar, reloj exacto del RSP y aparcamiento en la espera del FIFO. Submit-time RDP scheduling,
  exact RSP clock and parking on the FIFO wait.

## Arquitectura / Architecture

- [ARCH-THREADING.md](ARCH-THREADING.md) — arquitectura multihilo (RCP asíncrono).
  Multithreaded architecture (asynchronous RCP).
- [ARCH-COMM.md](ARCH-COMM.md) — modelo de ejecución y comunicación entre componentes.
  Execution model and communication between components.
- [ARCH-SYNC.md](ARCH-SYNC.md) — sincronización CPU/RSP/RDP: análisis y rediseño.
  CPU/RSP/RDP synchronisation: analysis and redesign.
- [HW-RCP-COMM.md](HW-RCP-COMM.md) — cómo se comunican CPU, RSP y RDP en el hardware real.
  How CPU, RSP and RDP communicate on real hardware.
- [PERF-RCP-SYNC.md](PERF-RCP-SYNC.md) — coste de sincronización del RCP.
  Cost of RCP synchronisation.
- [VI-CLOCK.md](VI-CLOCK.md) — el reloj del VI, un solo reloj para todo el tiempo del invitado.
  The VI clock: one clock for all guest time.

## CPU y RSP / CPU and RSP

- [CORES-CPU-RSP.md](CORES-CPU-RSP.md) — los dos núcleos: intérpretes y dynarecs.
  The two cores: interpreters and dynarecs.
- [JIT-PLAN.md](JIT-PLAN.md) — plan del dynarec de la CPU y optimización del intérprete.
  CPU dynarec plan and interpreter optimisation.
- [PERF-CPU.md](PERF-CPU.md) *(EN)* — rendimiento de CPU: intérprete y dynarec.
  CPU performance: interpreter and dynarec.
- [RSP-JIT.md](RSP-JIT.md) *(EN)* — dynarec del RSP. RSP dynarec.

## RDP y vídeo / RDP and video

- [GPU-RDP.md](GPU-RDP.md) — GPU-RDP propio (Vulkan compute), rasterizador por defecto.
  In-house GPU-RDP (Vulkan compute), the default rasterizer.
- [parallel-rdp-integration.md](parallel-rdp-integration.md) *(EN)* — integración de
  parallel-RDP (retirándose). parallel-RDP integration (being retired).
- [RDP-TIMING.md](RDP-TIMING.md) *(EN)* — temporización del RDP frente a datos de hardware.
  RDP timing against hardware data.
- [RDP-FIFO-BACKPRESSURE.md](RDP-FIFO-BACKPRESSURE.md) *(EN)* — contrapresión del FIFO DPC.
  DPC FIFO back-pressure.
- [RDP-COVERAGE-AA.md](RDP-COVERAGE-AA.md) — cobertura del RDP y antialias del VI.
  RDP coverage and VI antialiasing.
- [TEXTURE-FORMATS.md](TEXTURE-FORMATS.md) *(EN)* — formatos de textura: referencia y
  conformidad. Texture formats: decode reference and conformance.
- [TEXTURAS-HD.md](TEXTURAS-HD.md) — texturas HD y realce de texturas.
  HD textures and texture enhancement.
- [ESCALADO-Y-ASPECTO.md](ESCALADO-Y-ASPECTO.md) — escalado interno y relación de aspecto.
  Internal upscaling and aspect ratio.
- [FILTROS-IMAGEN.md](FILTROS-IMAGEN.md) — filtros de imagen del presentador.
  Presenter image filters.

## Funciones del emulador / Emulator features

- [SAVESTATES.md](SAVESTATES.md) — estados guardados. Savestates.
- [REWIND.md](REWIND.md) — rebobinado. Rewind.
- [TAS.md](TAS.md) — películas de entradas. Input movies (TAS).
- [CHEATS.md](CHEATS.md) — trucos GameShark / Action Replay. GameShark / Action Replay cheats.
- [FLASHCART-SD.md](FLASHCART-SD.md) — SummerCart64 y tarjeta SD. SummerCart64 and SD card.
- [ROMS-COMPRIMIDAS.md](ROMS-COMPRIMIDAS.md) — ROMs comprimidas (.zip, .gz).
  Compressed ROMs (.zip, .gz).

## Herramientas y uso / Tools and usage

- [ENV-VARS.md](ENV-VARS.md) — referencia de las variables `KESTREL_*`.
  Reference of the `KESTREL_*` variables.
- [DEBUGGER.md](DEBUGGER.md) — depurador: capas, ventana y MCP.
  Debugger: layers, window and MCP.
- [MCP-GUIA.md](MCP-GUIA.md) — guía del MCP para depurar u optimizar un juego.
  MCP guide for debugging or optimising a game.
- [LAUNCHER.md](LAUNCHER.md) — lanzador gráfico. Graphical launcher.
- [distribucion.md](distribucion.md) — distribuir kestrel64 en Windows.
  Distributing kestrel64 on Windows.
- [FIDELITY.md](FIDELITY.md) *(EN)* — puntuación de fidelidad con suites de prueba de la
  comunidad. Fidelity score from community hardware test suites.

## Juegos concretos / Specific games

- [PD-DERAIL.md](PD-DERAIL.md) — Perfect Dark: descarrilamiento tardío (investigación).
  Perfect Dark: late derail (investigation).

## Medidas y referencia / Measurements and reference

- [baselines/](baselines/) — hashes y tiempos de referencia de las puertas (`timings.md`,
  `sm64.txt`, `krom-*.tsv`, `pd-opt.md`). Gate reference hashes and timings.
- Wikis de la comunidad N64 usadas como referencia *(EN)*: [libdragon](https://github.com/DragonMinded/libdragon/wiki)
  y [n64brew](https://n64brew.dev/wiki/) (sobre todo
  [RSP](https://n64brew.dev/wiki/Reality_Signal_Processor) y
  [RDP](https://n64brew.dev/wiki/Reality_Display_Processor)). No se versionan copias;
  `python scripts/fetch_wikis.py` las baja a `docs/wiki/` (ignorado).
  N64 community wikis used as reference. No copies are versioned; `scripts/fetch_wikis.py`
  downloads them into `docs/wiki/` (git-ignored).
