# Lanzador grafico

`tools/launcher/` — interfaz completa para configurar y arrancar kestrel64 sin tocar una
sola variable de entorno a mano.

```bash
tools/launcher/run_launcher.cmd            # abre servidor + ventana
python tools/launcher/kestrel_launcher.py --no-open --port 9140
```

## Por que un servidor local y no una ventana nativa

Una GUI nativa habria metido una cadena de herramientas entera en el proyecto (Qt, o Rust
+ Tauri, o Electron con Node). El emulador se compila con clang y nada mas; el lanzador no
tenia por que cambiar eso. Lo que hay es:

- **backend**: Python 3.11 de la biblioteca estandar (`http.server`), sin un solo `pip
  install`. Sirve `web/`, lee cabeceras de ROM, traduce el perfil a entorno de proceso y
  arranca/mata el emulador capturando su salida.
- **frontend**: HTML/CSS/JS a pelo en `web/`. El 3D (coverflow, mando) es CSS
  `transform-style: preserve-3d`, no una biblioteca vendorizada.
- **ventana**: Edge o Chrome en modo `--app`, sin barra de direcciones ni pestanas. Si no
  hay ninguno se cae al navegador por defecto.

El perfil vive en `tools/launcher/profile.json` y se guarda solo, sin boton de guardar.

## Arquitectura

```
options.py            esquema unico de opciones (101 opciones en 9 categorias) + mando
kestrel_launcher.py   servidor HTTP + escaneo de ROMs + caratulas + proceso del emulador
web/index.html        estructura
web/style.css         tema oscuro, coverflow 3D, mando 3D
web/app.js            estado, vistas, modales, captura de teclas
run_launcher.cmd      arranque en Windows
```

`options.py` es la **unica fuente de verdad**. Cada fila dice: identificador, variable de
entorno, etiqueta, tipo, valor de fabrica, ayuda y si es avanzada. Anadir una bandera nueva
al emulador = anadir una fila; el frontend dibuja el control solo y el backend la exporta.

### API

| ruta | que hace |
|---|---|
| `GET /api/schema` | esquema de opciones + mando |
| `GET/POST /api/config` | perfil guardado |
| `GET /api/builds` | compilaciones presentes (`build/`, `build-prdp/`) |
| `GET /api/browse?dir=` | navegador de carpetas (el navegador web no puede) |
| `GET /api/roms?dir=` | lista de ROMs con la cabecera leida |
| `GET /api/boxart?id=&name=` | caratula, cache local |
| `POST /api/launch` | arranca el emulador con el perfil |
| `POST /api/stop` / `GET /api/status` | control y salida en vivo |

## Traduccion de opciones a entorno — la parte que se puede hacer mal

No todas las banderas del emulador se leen igual, y tratarlas por igual **enciende cosas
que se querian apagar**:

- La mayoria se activan por **PRESENCIA** (`std::getenv(X) != nullptr`). Exportar `X=0` las
  ENCENDERIA. Para estas, apagado = no exportar nada.
- Unas pocas leen el **VALOR** (`envFlag()` o `v[0] == '0'`): `KESTREL_JIT`,
  `KESTREL_THREADS`, `KESTREL_RSPJIT`, `KESTREL_AUDIO`, `KESTREL_FULLSCREEN`,
  `KESTREL_PRDP`, `KESTREL_THROTTLE`. Estas se escriben explicitas en los dos sentidos, y
  asi el perfil manda aunque cambie el valor por defecto del emulador. Van marcadas
  `tri=True` en el esquema.
- Las negativas (`KESTREL_NOxxx`) se muestran en positivo y se exportan cuando el control
  esta APAGADO (`invert=True`).

El valor de fabrica de cada fila **tiene que coincidir con el del emulador**, o el lanzador
apaga cosas que venian encendidas. Ejemplo real detectado al probar: `KESTREL_RSPJIT` es
default-ON en `src/rsp/rsp.cpp:154`, y el esquema lo tenia a `False`; el perfil de fabrica
exportaba `KESTREL_RSPJIT=0` y apagaba el dynarec del RSP sin que nadie lo pidiera.

## Overclock

El modelo de reloj (`System::Clocks`) ya llevaba `cpuOc` / `rspOc` / `rdramOc`; nadie los
escribia. Ahora `src/core/system.cpp` los cablea:

| variable | efecto |
|---|---|
| `KESTREL_OC=<x>` | los tres dominios a la vez |
| `KESTREL_OC_CPU=<x>` | R4300i, base 93.75 MHz |
| `KESTREL_OC_RSP=<x>` | RSP, base 62.5 MHz |
| `KESTREL_OC_RDRAM=<x>` | RDRAM, base 250 MHz DDR |

Los especificos pisan al global. Un multiplicador **no cambia la semantica**: sube el ritmo
objetivo de retiro (`insnTarget()`), o sea cuantas instrucciones caben en un campo de video.

**El ancla es el campo de video, no el reloj de CPU.** `throttleFieldNs = 1e9 /
viFieldHz` (59.94 Hz), asi que con overclock el juego hace mas trabajo entre campos pero
los campos siguen saliendo al mismo ritmo. Eso es lo que quita la ralentizacion de un juego
que no llega a su presupuesto — y tambien lo que puede romper un juego que ate su logica al
reloj. `KESTREL_THROTTLE=0` es otra cosa: quita el limitador entero y el juego va acelerado.

El lanzador ofrece 0.5 / 0.75 / 1 / 1.25 / 1.5 / 2 / 3 / 4 y un deslizador de 0.25 a 4. El
subreloj (0.5x, 0.75x) no estaba pedido pero sale gratis del mismo mando y sirve para
reproducir el comportamiento de un juego que corre por encima de su presupuesto.

## Ventana y resolucion

`src/video/present.cpp` decide el tamano de la ventana:

| variable | efecto |
|---|---|
| `KESTREL_WINSCALE=N` | N x 320x240, N de 1 a 16 |
| `KESTREL_WINSIZE=WxH` | tamano exacto, manda sobre la escala |
| `KESTREL_FULLSCREEN=1` | pantalla completa en el monitor primario |

El framebuffer del guest sigue siendo 320x240: esto solo decide a que resolucion se
**presenta**, el blit de la cadena de intercambio escala. La pantalla completa **no cambia
el modo del monitor**: toma el que ya hay. Cambiarlo de verdad deja el escritorio roto si
el emulador se cae, y no compra nada porque la imagen se escala igual en la presentacion.
(Escalado interno de verdad = trabajo de paraLLEl-RDP, que hoy no soporta upscaling en esta
integracion; ver `parallel-rdp-integration.md`.)

## Rasterizador

La eleccion de rasterizador es de **tiempo de compilacion** (`cmake -DKESTREL_PRDP=ON`), no
una dll cargable como en la arquitectura de plugins de Zilmar. El lanzador por tanto elige
**ejecutable**: `build/kestrel64.exe` para SoftRDP, `build-prdp/kestrel64.exe` para
paraLLEl-RDP, y ademas pone `KESTREL_PRDP=1`. Si una compilacion no existe, su tarjeta sale
apagada con la linea de cmake que hace falta.

## Mando configurable

Antes las teclas estaban clavadas en `present.cpp`. Ahora `KESTREL_PAD1=<fichero>` carga un
mapa. Formato, una linea por control:

```
<CONTROL> <TECLA|-> <BOTON_GAMEPAD|->
```

Controles: `A B Z START DU DD DL DR L R CU CD CL CR` y `SX+ SX- SY+ SY-` para el stick.
Nombres de GLFW sin prefijo (`X`, `SPACE`, `LEFT`, `KP_0`, `F1`, ...) y de gamepad
(`A`, `DPAD_UP`, `LEFT_BUMPER`, `LEFT_TRIGGER`, ...). `-` = sin asignar.

**Sin fichero no se carga nada y el camino de teclado/gamepad es exactamente el de antes**,
instruccion por instruccion: el mapa solo entra en juego cuando `m.loaded` es cierto. Los
gatillos no son botones sino ejes, asi que `LEFT_TRIGGER`/`RIGHT_TRIGGER` usan codigos
negativos internos que el lector resuelve contra `gp.axes`.

En la interfaz esto es un mando de N64 en 3D (CSS `preserve-3d`, se endereza al pasar el
raton): se pulsa un boton y se abre su cajon con captura en vivo de tecla y de boton de
gamepad (via `navigator.getGamepads()`).

## Biblioteca y caratulas

Cinco modos de vista: **Coverflow** (3D con reflejo, rueda del raton), **Filas** estilo
Netflix agrupadas por region, **Rejilla**, **Rueda** estilo Hyperspin (arco por coseno) y
**Tabla** con ID de cartucho, region, formato, tamano y CRC. Flechas navegan, Enter lanza,
doble clic lanza.

La cabecera se lee de verdad y se normaliza el orden de bytes, que es distinto segun el
volcador: `0x80371240` z64 (nativo big-endian), `0x37804012` v64 (pares intercambiados),
`0x40123780` n64 (palabras del reves).

**Caratulas**: NNID es un identificador de cuenta de Wii U / 3DS y no tiene nada que ver con
arte de N64. La fuente practica es **libretro-thumbnails**
(`libretro-thumbnails/Nintendo_-_Nintendo_64/Named_Boxarts`), que nombra los ficheros con la
convencion No-Intro, o sea por nombre de FICHERO, no por el nombre interno del cartucho. Por
eso se prueba primero el nombre del fichero y luego el interno. Alternativas si hace falta
mas cobertura: TheGamesDB y ScreenScraper (las dos con clave de API).

El nombre exacto solo acierta si la ROM viene nombrada como No-Intro, que casi nunca pasa. Asi
que ademas se baja **una vez** el indice del directorio (1117 entradas, entran enteras en una
respuesta del API de arboles de GitHub) y se casa normalizando: fuera los grupos entre
parentesis (region, idiomas, revision), fuera lo no alfanumerico, a minusculas. Con eso
`SUPER MARIO 64` (nombre interno del cartucho) encuentra `Super Mario 64 (USA)`.

Si tampoco casa exacto se prueba **prefijo en los dos sentidos** — el nombre interno viene
recortado (`GOLDENEYE` por `GoldenEye 007`) y el del fichero trae cola (`mario_kart_64_rom`) —
pero **solo si el prefijo es inequivoco**. `Mario Party` es prefijo de `Mario Party` y de
`Mario Party 2`, y ahi no se elige nada: mejor sin caratula que con la equivocada.

Con varias entradas del mismo juego gana la region del cartucho, si no la de USA, y entre las
que queden la de nombre mas corto (la version base, sin Beta / Rev / Demo).

Se cachean en `tools/launcher/cache/boxart/`. Una caratula encontrada por indice se guarda
tambien bajo la clave que pidio el cliente, para que el refresco siguiente acierte en cache. Un
fallo se marca con un fichero vacio para no volver a salir a la red en cada refresco.

## Ventana de telemetria

El emulador ya lleva el servidor de telemetria dentro (puerto 9128 por defecto, el MISMO que
usa el MCP). El lanzador lo expone en `/api/tele?cmd=<orden>&args=<json>` y
`/api/tele/fb` (framebuffer en PNG) haciendo **solo de puente**: pasa la orden y devuelve lo
que conteste, sin reinterpretar. Asi la ventana no puede contar una cosa distinta de la que
ve el MCP.

Pestanas: **Estado** (velocidad por dominio en % de N64 real, campos/s, ocupacion de los
hilos, overclock aplicado, cabecera del cartucho), **Imagen** (el framebuffer que el VI esta
escaneando, decodificado desde la RDRAM — no es una lectura de vuelta de la GPU), **CPU**
(pc, instruccion desensamblada, los 32 registros, COP0), **RCP** (el fichero de registros MMIO
entero: MI, SP, DPC, VI, AI, PI, SI), **RSP** y **Perfilador** (top de rutas calientes por
bucket de 16 bytes).

La ocupacion es la cifra que hace falta mirar en modo multihilo: el porcentaje de CPU sube
cuando la CPU **gira esperando** al RCP, asi que sin `cpuWaitPct` / `rdpBusyPct` / `rspBusyPct`
el estado engañaria sobre quien es el palo largo.

El cliente de telemetria (`tele.py`) es una copia suelta del protocolo, no una dependencia del
puente MCP: marcos con la longitud por delante, JSON y detras los bytes crudos. Las lecturas
del perfilador pausan el nucleo un instante y restauran el estado anterior, porque el bucle de
marcha libre tiene cogido el candado del nucleo en tandas de ~un campo de video.

## Que falta

- Depurador de verdad dentro de la ventana: hay puntos de ruptura, paso a paso y
  `run_until` en el servidor, pero la interfaz todavia no los ensena (solo pausa / reanuda /
  reinicia).
- Segundo, tercer y cuarto mando (hoy solo `KESTREL_PAD1`).
- Cambiar opciones en caliente sin reiniciar el emulador.
