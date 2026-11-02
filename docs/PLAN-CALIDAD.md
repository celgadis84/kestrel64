# Plan de calidad de código y documentación (abierto 2026-10-24)

Motivo: queja externa de "poca cantidad y calidad de código/documentación" en el repo
público. El diagnóstico del 2026-10-24 apunta a que el problema no es la cantidad: el
código y la documentación son **inaccesibles para quien llega de fuera y lee inglés**.

| Aspecto | Dato 2026-10-24 | Problema |
|---|---|---|
| Código propio | ~45.700 líneas C++ en `src/` | — |
| Comentarios | 22 % de las líneas | En español sin tildes y en estilo telegráfico |
| Ficheros gigantes | `core/memory.cpp` 5.600, `cpu/jit.cpp` 4.000, `cpu/cpu.cpp` 3.600, `rdp/rdp.cpp` 3.400 | Difíciles de revisar |
| `docs/` | 35 documentos, casi todos en español | Ilegibles para el público objetivo |
| `docs/STATUS.md` | 10.400 líneas | Es un diario, no una referencia |
| `docs/GAPS.md` | Líneas de hasta 2.000 caracteres | Ilegible en el visor de GitHub |
| `docs/wip/` | Copias de `.cpp`/`.hpp` (~3.700 líneas) | Basura versionada |
| Tests | 10 tests unitarios | Las puertas de verdad necesitan ROMs que no están en el repo |

Reglas para todo el plan:
- Ningún cambio de código altera el comportamiento del invitado. Cada commit de las fases
  4 y 5 se valida con statehash, el md5 de SM64 y systemtest en el modo afectado.
- Codificación y finales de línea de cada fichero se mantienen. Los ficheros nuevos van en
  UTF-8 con LF.
- Nada de rutas personales ni de nombres de ROM comerciales en ficheros versionados.

## Fase 1 — Limpieza (unas horas)

1. Quitar `docs/wip/`. Si algo de ahí sigue vivo, se pasa a una rama.
2. Partir `STATUS.md`:
   - `STATUS.md` corto (≤300 líneas) con el estado actual por subsistema.
   - El diario se mueve a `docs/history/STATUS-2026.md` sin cambios.
3. `GAPS.md`: recortar las líneas a ≤100 columnas, sin cambiar el contenido. Se hace con un
   script (`scripts/rewrap_md.py`) que no toque bloques de código ni tablas.
4. `docs/README.md` como índice bilingüe: una línea en español y otra en inglés por documento,
   agrupados por tema (arquitectura, CPU, RSP, RDP, vídeo/audio, herramientas, histórico).

## Fase 2 — Arquitectura en dos idiomas (~1,5 días)

- `docs/ARCHITECTURE.md` (en inglés) y `docs/ARQUITECTURA.md` (en español), **misma
  estructura sección a sección**.
- Regla de sincronía:
  - Cabecera común `Synced with: <commit>`.
  - Tocar uno obliga a tocar el otro en el mismo commit.
  - Un script, `scripts/check_docs_sync.py`, compara los títulos de sección y la cabecera
    y falla en CI si se desalinean.
- Contenido, un nivel por encima de los documentos `ARCH-*` existentes, a los que enlaza
  como lectura a fondo:
  1. Visión general: objetivo, apuesta multihilo, determinismo, diagrama de hilos (mermaid,
     que GitHub dibuja).
  2. Modelo de tiempo: Lockstep frente a Threaded, citas datadas en tiempo de invitado,
     statehash como invariante.
  3. Memoria y bus: RDRAM, MMIO, DMA, cachés, TLB.
  4. CPU: intérprete, JIT (bloques, enlazado, L2), PGO.
  5. RSP: LLE, JIT del RSP, modelo de ciclos.
  6. RDP: SoftRDP (oráculo), GPU-RDP (de fábrica); espejo y
     coherencia.
  7. VI, presentación, audio.
  8. Estado: savestates, rebobinado, TAS.
  9. Telemetría/MCP y depurador.
  10. Build, empaquetado y verificación (puertas, oráculos y qué cubre cada uno).
  11. Mapa de `src/`: directorio → responsabilidad → fichero de entrada.
- Además, los documentos de diseño más consultados reciben un **resumen en inglés de 10–20
  líneas** al principio: `ARCH-THREADING`, `JIT-PLAN`, `RSP-JIT`, `GPU-RDP`, `MCP-GUIA`,
  `DEBUGGER` y `ENV-VARS`. No se traducen enteros.
- El README enlaza a la arquitectura. La cifra de ares se quitó (2026-11-03): la comparativa
  se descartó (ver GAPS) y el README solo da cifras de kestrel.

## Fase 3 — Verificación reproducible sin ROMs comerciales (horas)

Decisión del usuario (2026-10-24): **sin CI de pruebas ni descarga automática**. Los repos de
pruebas son públicos y no publican releases; basta con documentar y enlazar.

- `CONTRIBUTING.md` y `docs/TESTING.md` (en español y en inglés). Para cada suite:
  - el enlace a su repositorio público (n64-systemtest, las pruebas de PeterLemon/krom,
    libdragon);
  - cómo compilarla u obtenerla;
  - dónde ponerla (`scripts/local.env`);
  - qué script la lanza y qué baseline compara.
- Separar qué se puede validar sin ROMs (tests unitarios) de qué necesita las suites
  externas. Las ROMs comerciales quedan como puertas locales, nunca obligatorias.

## Fase 4 — Reestructuración de ficheros grandes (varios días, con riesgo)

Mismo comportamiento y otra organización. Un fichero por commit, validado.

- `core/memory.cpp` → bus/MMIO, DMA, RDRAM y cachés, TLB, según las responsabilidades
  reales que salgan al leerlo.
- `cpu/jit.cpp` → front-end (decodificación de bloques), emisión, enlazado y caché de
  bloques.
- `cpu/cpu.cpp` y `rdp/rdp.cpp`: igual, solo si el corte sale natural.
- Riesgo de rendimiento al partir unidades de compilación (inlining): se mide con `bench`
  y se rehace el perfil PGO al acabar la fase.

## Fase 5 — Optimización del código (nueva, ~1 semana)

Dos frentes medidos por separado.

**a) Higiene (anti-slop).**
- Código muerto: rutas experimentales abandonadas y variables `KESTREL_*` sin uso. Se
  inventarían contra `docs/ENV-VARS.md` y se retiran las muertas.
- Duplicados: ayudantes repetidos entre CPU/JIT/RSP y entre SoftRDP/GPU-RDP.
- Comprobaciones defensivas en rutas de confianza.
- Comentarios que narran el *qué* en vez del *porqué*.
- Funciones de cientos de líneas.
- Retirada de parallel-rdp: HECHA (código borrado 2026-11-02).

**b) Rendimiento, guiado por perfil.**
- Perfil de PD, SM64 y DK64 en juego. Optimizar solo lo que salga caliente.
- Validación de cada cambio:
  - `bench` (mínimo de 4, intercalado);
  - statehash idéntico;
  - perfil PGO rehecho al final.
- Sin metas inventadas: cada cambio se queda solo si gana más que el ruido medido.

**Herramienta anti-slop.** No hay una skill "antislop" canónica; hay varias de terceros
("deslop", "anti-slop") con calidad desigual. Decisión:
- Las reglas van **en el repo**, en una sección "AI-assisted code" de `CONTRIBUTING.md`:
  patrones prohibidos con un ejemplo de antes/después tomado de este código.
- Se aplican las skills integradas `/simplify` y `/code-review` sobre el diff de cada commit
  de las fases 4 y 5.
- Una skill de terceros solo se instala tras leer su contenido entero.

## Fase 6 — Comentarios en inglés en las interfaces (~1–2 días)

- Las cabeceras públicas de cada subsistema (`*.hpp` que usan otros directorios) reciben
  un comentario de cabecera en inglés: responsabilidad, invariantes e hilos que la tocan.
- Los comentarios internos en español se quedan: reescribirlos no aporta y arriesga diffs
  enormes. El código nuevo comenta en inglés.

## Orden y estado

| Fase | Coste | Estado |
|---|---|---|
| 1 Limpieza | horas | HECHA 2026-10-30: `docs/wip/` fuera (nota a `docs/history/SUBSCHED-2026-09.md`, copias solo en git), `STATUS.md` corto + `docs/history/`, `GAPS.md` a 100 col con `scripts/rewrap_md.py` (tablas sin tocar), índice `docs/README.md` |
| 2 Arquitectura en dos idiomas | ~1,5 días | HECHA 2026-10-30: `ARCHITECTURE.md`/`ARQUITECTURA.md` (11 secciones, `Synced with:`), `scripts/check_docs_sync.py` + workflow `docs.yml`, resumen inglés en los 7 documentos clave, enlaces en README/CONTRIBUTING. La cifra de ares se quitó del README el 2026-11-03: la comparativa se descartó (el ares del fork no pinta ni lleva telemetría) |
| 3 Verificación reproducible | horas | HECHA 2026-10-30: `TESTING.md`/`PRUEBAS.md` (pareja comprobada por `check_docs_sync.py`), enlaces en README/CONTRIBUTING; la CI corre los 9 tests unitarios sin ROM; `rewind_e2e.py` se salta sin `ROM_SM64`, así que `gate_all.sh` corre sin ROMs comerciales; `gate_quick.sh` corre sobre SoftRDP |
| 4 Reestructuración | varios días | HECHA 2026-11-03. 2026-11-02 `core/memory.cpp` (5.700) partido en `memory.cpp` 680, `memory_cart` 550, `memory_mmio` 760, `memory_dma` 540, `memory_pif` 390, `rcp_rdp` 600, `rcp_sync` 2.160 + `memory_internal.hpp`; statehash junkrunner64/SM64 T0/T1 idéntico a HEAD (`scripts/statehash_ab.sh`), SM64 md5 interp/threaded-jit, systemtest interp/threaded-jit/phys-threaded, 7 tests unitarios, bench SM64 sin cambio (min 6,62 → 6,61 s, sin PGO). `cpu/jit.cpp` (4.050) partido en `jit.cpp` 290 (búfer y caché de código, enlace, autopruebas), `jit_compile` 2.650 (residencia de registros, emisores, `compileBlock`: comparten estado de compilación, no se separan) y `jit_run` 1.130 (despacho del lado de la CPU, reentrada, salto de bucle ocioso, estadísticas) + `jit_internal.hpp`; statehash idéntico, SM64 md5 jit/threaded-jit/jit-nolink, systemtest jit/threaded-jit, bench sin cambio (6,60 → 6,57–6,61 s). `cpu/cpu.cpp` (3.700) partido en `cpu.cpp` 2.270 (núcleo del intérprete), `cpu_boot` (CIC, fastBoot, warmBoot), `cpu_fpu` (COP1 y sus atajos del dynarec), `cpu_disasm` y `video/fbdump.cpp` (volcados BMP y captura del framebuffer) + `cpu_internal.hpp`; `branch` y `fprGet/Set*` pasan a inline en la cabecera (fuera de línea costaban ~1 % en interp); statehash idéntico, systemtest interp/jit, SM64 md5, bench interp 60 campos en ruido. `rdp/rdp.cpp` (3.550): sale `rdp_cmd.cpp` 650 (cargas de TMEM, lotes de GPU-RDP, bucle de comandos `run`) + `rdp_internal.hpp`; todo lo que corre por píxel (raster, texel, combiner, blender, modelo de coste) se queda junto en `rdp.cpp` 2.900 para que siga inlineando. statehash y SM64 md5 idénticos, bench threaded-jit igual. `rcp_sync.cpp` (2.160) se queda entero: una sola responsabilidad (citas CPU/RSP/RDP) con estado compartido. PGO rehecho al cerrar la fase 5 |
| 5 Optimización | ~1 semana | HECHA 2026-11-03. 2026-11-02 higiene: parallel-RDP retirado del todo (`KESTREL_PRDP`, alias CMake, migración del lanzador, `build-prdp*` → `build-gpu*`), `emitVecRight` muerto, `sext`/`loadBe32`/`peekBe32Coherent` en vez de copias, avisos; sección "AI-assisted code" en `CONTRIBUTING.md`; statehash idéntico. Rendimiento (`scripts/ab_bench.py --mode ship`): retroceso exponencial de las barreras y drenaje diferido del RSP, los dos en ruido y descartados (GAPS «Rendimiento»). 2026-11-03: interior de cobertura plena en el paseo de coste (ruido) y cota superior sin drenaje RSP -> worker (+22,6 % MÁS lento): descartados. `spReadSync` es cita real con la CPU, no espera sobrante. Lo que queda caliente (JIT del RSP, paseo de coste con z sombra 7,7 %) está por debajo del ruido del anfitrión en micro-ajustes. PGO rehecho, `release.sh` |
| 6 Interfaces en inglés | 1–2 días | HECHA 2026-11-03: las 32 cabeceras que incluye otro subsistema (`src/*/*.hpp`) abren con un bloque en inglés (responsabilidad, invariantes, hilos), encima del comentario en español, que se queda; solo inserciones ASCII, codificación y finales de línea intactos. Regla en `CONTRIBUTING.md` «Style» |

**Arranque: martes 2026-10-27**, junto con la comparativa de velocidad con ares (GAPS).
Las fases 1–3 son las que más cambian la percepción externa y no tocan código del emulador.
