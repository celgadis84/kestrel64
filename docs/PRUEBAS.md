# kestrel64 — Pruebas

Synced with: f6d6c24

Cómo reproducir la verificación de kestrel64 en tu máquina. Toda comprobación es un oráculo
(una ROM de prueba que informa de su propio resultado, una captura de hardware o una baseline
guardada), nunca un juicio visual. La versión inglesa, `docs/TESTING.md`, tiene las mismas
secciones en el mismo orden; las dos se cambian en el mismo commit
(`scripts/check_docs_sync.py` lo comprueba en CI).

El repositorio no lleva ROMs. Las suites de abajo son proyectos públicos: te las bajas o las
compilas tú y le dices a kestrel64 dónde están. Los juegos comerciales solo son comprobaciones
locales opcionales; nada de esto exige uno.

## 1. Tres niveles

| Nivel | Necesita | Qué demuestra |
|---|---|---|
| Tests unitarios | solo un build | componentes por separado (unidad vectorial del RSP, partidas, PIF, trucos, archivos comprimidos, rebobinado, vigilancias, Transfer Pak, memoria fuera de rango) |
| Suites públicas | las ROMs de la sección 4 | corrección de CPU/RCP, exactitud del RDP contra capturas de hardware, tiempos del RDP |
| ROMs comerciales locales | tus propios volcados | determinismo de extremo a extremo (md5 del framebuffer tras N campos), bancos de rendimiento |

La CI (`.github/workflows/build.yml`) solo corre el primer nivel: compila el árbol y lanza los
nueve tests unitarios. Adrede, no hay ningún trabajo de CI que descargue o corra ROMs de
prueba.

## 2. Sin ROMs: tests unitarios

Los tests unitarios están en `test/` y son objetivos de CMake fuera del build por defecto.
Cada uno imprime `ALL PASS` y sale con 0, o sale con otro valor al primer fallo.

```sh
export PATH=/c/msys64/clang64/bin:$PATH
cmake --build build -j8 --target rsp_test save_test cheat_test archive_test wildmem_test \
      rewind_test watch_test pif_test tpak_test
./build/rsp_test.exe
```

`scripts/gate_all.sh` corre los nueve antes que nada.

## 3. Configuración local

Copia `scripts/local.env.example` a `scripts/local.env` (ignorado por git) y edítalo. Las
líneas son `CLAVE="valor"`; una variable de entorno con el mismo nombre manda sobre el
fichero.

| Clave | Significado |
|---|---|
| `PY` | Python que usan los scripts `.sh`. Necesita numpy y Pillow para la puerta krom; en Windows, el Python de Windows, no el de MSYS. |
| `KESTREL_TESTROMS` | carpeta con las suites de prueba (por defecto `../test_roms`) |
| `ROM_SM64`, `ROM_PD`, `ROM_DK64` | ROMs comerciales opcionales (sección 7); vacías, las comprobaciones que las usan se saltan |
| `ARES_EXE` | opcional, solo para la comparación con ares de `scripts/fidelity.py` |

`scripts/validate.py` usa `build/kestrel64.exe` salvo que `KESTREL_EXE` diga otra cosa.

## 4. Suites públicas

| Suite | De dónde sale | Dónde va | La lanza |
|---|---|---|---|
| n64-systemtest | [lemmy-64/n64-systemtest](https://github.com/lemmy-64/n64-systemtest): se compila como explica su README | `$KESTREL_TESTROMS/n64-systemtest.z64` | `validate.py systemtest` (puerta) |
| Pruebas N64 de PeterLemon («krom») | [PeterLemon/N64](https://github.com/PeterLemon/N64): se clona; las ROMs ensambladas están junto a sus capturas de hardware (`.png`) | `$KESTREL_TESTROMS/PeterLemon-N64/` | `validate.py krom` (puerta) |
| Pruebas de tiempos del RDP | [Thar0/RDP-Timing-Tests](https://github.com/Thar0/RDP-Timing-Tests): se compila `rdp_fill_timing.z64` como explica su README | `../rdp-timing-tests/`, junto al repositorio | `gate_quick.sh thar0` |
| n64-tests de Dillon | [Dillonb/n64-tests](https://github.com/Dillonb/n64-tests) | `$KESTREL_TESTROMS/dillon-n64-tests/*.z64` | `fidelity.py --suite dillon` (nota, no puerta) |
| ROM de pruebas de libdragon | [DragonMinded/libdragon](https://github.com/DragonMinded/libdragon): se compila `tests/testrom_emu.z64` con la cadena de libdragon | `$KESTREL_TESTROMS/libdragon-tests/testrom_emu.z64` | `fidelity.py --suite libdragon` (nota, no puerta) |

Qué comprueba cada una:

- **n64-systemtest** cubre todos los opcodes y casos de excepción de CPU, COP0 y COP1, más
  comportamiento y tiempos del RSP y del RCP. La puerta lee el resumen «Failed X of Y» que
  imprime la ROM y solo pasa con cero fallos; la cifra vigente está en `docs/STATUS.md`.
- **krom**: cada ROM que trae una captura de hardware `.png` con su mismo nombre (371 hoy) se
  corre hasta que su framebuffer deja de cambiar y se compara con la captura. Las ROMs sin
  captura se saltan.
- **Tiempos del RDP** mide ciclos de relleno en la consola; `scripts/rdptiming.py compare` da
  el error del modelo de coste del RDP de kestrel64 frente a ella.
- **Dillon** y **libdragon** alimentan `scripts/fidelity.py`, una nota bajo demanda que también
  puede correr las mismas ROMs en ares. No se lanza tras cada cambio.

## 5. Puertas

| Script | Contenido | Tiempo |
|---|---|---|
| `scripts/validate.py <puerta> [--mode M]` | una puerta (`systemtest`, `krom`, `sm64`, `pd`, `bench`) en un modo de ejecución | de segundos a minutos |
| `scripts/gate_quick.sh [krom] [lock] [phys] [thar0]` | subconjunto por cambio sobre los builds estáticos: systemtest threaded-jit, sm64 interp y gpurdp-jit; extras a petición | ~1,5 min |
| `scripts/gate_all.sh` | tests unitarios, rebobinado de extremo a extremo, luego systemtest y sm64 en cada modo de CPU/RSP sobre SoftRDP, luego krom | ~20 min |
| `scripts/gate_gpurdp.sh` | el mismo material sobre GPU-RDP (necesita GPU con Vulkan); los resultados tienen que ser los de SoftRDP | ~7,5 min |

Los modos de ejecución (`--mode`) se traducen en conmutadores de entorno: `interp`
(intérprete, Lockstep: el oráculo), `jit`, `threaded`, `threaded-jit`, `rspinterp`, `phys` y
otros; la lista completa, con el motivo de cada uno, está al principio de
`scripts/validate.py`. Todos dejan a 0 los atajos del anfitrión `KESTREL_SPLEAD` y
`KESTREL_DPLOGLEAD`.

Sin ROMs comerciales las puertas siguen corriendo: las puertas `sm64` y `pd` y la prueba de
rebobinado de extremo a extremo imprimen `SKIP` y cuentan como superadas, así que
`gate_all.sh` se queda en tests unitarios, systemtest en todos los modos y krom.

## 6. Baselines y lectura de resultados

Los resultados se comparan con ficheros de `docs/baselines/`, así que una ejecución limpia
imprime unas pocas líneas en lugar de cientos:

- `krom-interp.tsv`: exactitud por ROM frente a la captura. Una ROM empeora cuando baja más
  de 0,5 puntos. Los otros modos se comparan con ella mediante `--vs interp`.
- `sm64.txt`, `pd.txt`: md5 del framebuffer de las comprobaciones locales con ROMs
  comerciales. Todos los modos, y GPU-RDP, tienen que dar el mismo md5.
- `timings.md`: tiempo de pared de cada puerta, para notar una comprobación que se ha vuelto
  lenta.

`--update-baseline` reescribe una baseline. Solo cuando el cambio de resultado se entiende y
es buscado, y diciéndolo en el mensaje del commit. El detalle por ROM y los logs van a `out/`.

## 7. ROMs comerciales (solo en local)

Algunas comprobaciones usan juegos comerciales porque ninguna ROM pública recorre el mismo
camino de extremo a extremo: Super Mario 64 para el md5 del framebuffer, la ida y vuelta del
rebobinado y el banco de medida, y Perfect Dark para la puerta del GPU-RDP. Hacen falta tus
propios volcados, configurados en `scripts/local.env`. Nunca se commitean, nunca son
obligatorios y nunca salen en material público.

El rendimiento se mide con `scripts/validate.py bench`: tiempo de pared para un número fijo de
campos de vídeo del invitado, el mínimo de varias ejecuciones. Los MIPS no sirven aquí como
medida de velocidad: en modo Threaded un hilo de CPU más rápido puede retirar más
instrucciones de espera activa mientras el emulador va más lento.

## 8. Determinismo

Dos propiedades se comprueban en cada pasada de las puertas, y se rompen con estrépito si se
pierden:

- **Equivalencia de modos.** Con los atajos del anfitrión a 0, Threaded tiene que llegar al
  mismo estado del invitado que Lockstep, y el JIT al mismo que el intérprete: el resultado de
  systemtest y los md5 son idénticos en todos los modos, y `scripts/gate_all.sh` los corre
  todos.
- **Ida y vuelta del estado.** El modo `rewind-rtt` recarga una foto de rebobinado en cada
  campo; cualquier trozo de estado del invitado que la foto no lleve cambia el md5.
  `scripts/rewind_e2e.py` y `test/state_test.py` comprueban guardar, correr, cargar y
  comparar a través del servidor de telemetría.
