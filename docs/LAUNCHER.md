# Lanzador grafico

`tools/launcher/` — interfaz completa para configurar y arrancar kestrel64 sin tocar una
sola variable de entorno a mano.

```bash
tools/launcher/run_launcher.cmd            # abre servidor + ventana (desarrollo)
python tools/launcher/kestrel_launcher.py --no-open --port 9140
sh scripts/gui.sh                          # lo congela en dist/kestrel64-gui.exe
```

Lo que se instala es el `.exe` congelado, y es **el** icono del escritorio: el lanzador es
la cara del programa, el emulador se ejecuta directo solo desde la asociacion de ROM del
Explorador. Detalles del empaquetado en `docs/distribucion.md`.

Hasta donde alcanza el censo de emuladores de N64, ninguno lleva una interfaz de este tipo:
biblioteca con caratulas en 3D, overclock por dominio de reloj, mando editable y telemetria
en vivo del nucleo, todo servido en local por el propio lanzador.

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
web/gl.js             motor WebGL1 minimo (matrices, primitivas, seleccion por color)
web/models.js         geometria propia: mando, cartucho y caja
web/scene3d.js        pega esa geometria a la interfaz (mando editable, estante)
web/smoke.html        prueba visual suelta de las mallas + barrido de clics
web/smoke_e2e.html    la aplicacion entera dentro de un iframe, para captura sin manos
models_test.js        prueba de geometria sin navegador (`node tools/launcher/models_test.js`)
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

En la interfaz esto es un mando de N64 **en 3D de verdad** (WebGL, `web/scene3d.js`): se
gira arrastrando, se acerca con la rueda, se pincha un boton del modelo y se abre su cajon
con captura en vivo de tecla y de boton de gamepad (via `navigator.getGamepads()`). Mientras
el cajon esta abierto se sondea el gamepad **por el mismo mapa que se le pasa al emulador**,
asi que el boton que se enciende en el modelo es la comprobacion de que el mapa esta bien.

Debajo del modelo hay una tira de fichas con los quince controles. No es adorno: el `Z` vive
en la cara de abajo del mando y siempre hay algun boton tapado segun como se gire, de modo
que la tira garantiza que se pueda llegar a todos sin pelearse con la camara.

Si el navegador no da contexto WebGL, `SCENE3D.mountPad` devuelve `null` y se queda el mando
de CSS (`preserve-3d`) de antes, que mapea exactamente los mismos identificadores. La
funcionalidad no depende del 3D.

## Los modelos 3D

Mando, cartucho y caja estan **dibujados en codigo** (`web/models.js`), con medidas del
aparato real en centimetros: mando 17.3 cm de ancho, cartucho 8.8 x 11.4 x 2.1, caja de
carton 13.5 x 19.0 x 3.0. No hay ningun modelo descargado, y no por gusto: los modelos de
N64 que circulan por los repositorios 3D o no son descargables o llevan licencias
(`Editorial`, "todos los derechos reservados") que no permiten meterlos dentro de un
programa que se reparte. Comprobado antes de escribir una linea, via
`https://api.sketchfab.com/v3/models/<uid>` (campos `license` e `isDownloadable`).

`web/gl.js` es el motor: matrices columna, un constructor de mallas con pila de
transformaciones, primitivas (caja redondeada, cilindro, esfera, extrusion de poligono),
sombreado con luz clave + relleno frio + especular, y **seleccion por color** (una pasada
con cada tramo pintado de su indice y un `readPixels` de un pixel). Cada tramo de la malla
lleva el identificador de su parte, y esos identificadores son los de `options.py`: si
falta uno, en la interfaz no se puede pinchar.

### La carcasa del mando es una sola pieza

El mando no son tres cajas solapadas (eso se veia deforme). Es su **silueta real**: media
planta a mano (`PAD_HALF`, la pala central, el entrante y la pala derecha hasta el canto de
atras), espejada, suavizada con Catmull-Rom (`smoothPoly`, 30 puntos de control -> 128) y
extruida con los cantos redondeados. Los tres mangos son secciones superelipticas que bajan,
se estrechan y se curvan hacia el jugador, cosidas en un tubo (`handleRings` + `loft`).

Primitivas nuevas que hacen falta para eso, todas en `gl.js`:

| Funcion | Que hace |
|---------|----------|
| `smoothPoly(pts, per)` | Catmull-Rom cerrada: contorno sin esquinas |
| `chartArea(poly)` | area firmada en el plano (x,z); el signo = la orientacion |
| `earClip(poly)` | triangula un poligono **concavo** (los entrantes entre palas) |
| `polyInset(poly)` | normal interior + inglete por vertice, para encoger el contorno |
| `roundPrism(poly, h, r, seg)` | extrusion con los cantos de arriba y abajo redondeados |
| `loft(rings, opt)` | cose una pila de anillos en un tubo con normales suaves |

### La trampa de la orientacion (culling)

`gl.enable(CULL_FACE)` esta activo, asi que **un triangulo al reves no da error: desaparece**.
Aparecio tres veces seguidas y de tres maneras distintas:

- Los botones salian como **medias lunas**: las tapas del cilindro miraban hacia dentro.
  `cylinder`, `sphere` y `extrude` venian con el bobinado invertido desde el primer dia.
- La carcasa se veia **por dentro**: las paredes de `roundPrism` al reves (volumen -114).
- Un **faldon** colgando del cuerpo: `polyInset` elegia el lado interior por vertice
  midiendo contra el centroide, y eso se invierte en los vertices reflejos (los entrantes).
  Ahora el lado sale del bobinado global del poligono, y en un vertice reflejo el inglete
  se queda en 1.

Reglas que quedan: en un poligono del plano (x,z) la normal es `n_y = -area_firmada`, asi
que **las tapas se orientan triangulo a triangulo** por su propia area, y los barridos
normalizan la orientacion de ENTRADA (`chartArea(poly) > 0 -> reverse`) en vez de dar la
vuelta a la malla entera (voltear toda la malla arregla las paredes y rompe las tapas).

Trampa de la caja redondeada: el radio nunca puede pasar de la mitad del lado mas corto. La
primitiva lleva la rejilla del cubo hacia dentro y luego la empuja `r` hacia fuera, asi que
con `r > lado/2` la pieza se **hincha** hasta `2r` en ese eje. Costo una isla de botones C
que se tragaba los cuatro botones que tenia encima.

### Pruebas, las dos sin depender de que haya GPU en la maquina de turno

- `node tools/launcher/models_test.js` — sin navegador: que estan todos los identificadores,
  que los indices caben en `Uint16` (WebGL1 no tiene mas), normales unitarias, arrays
  cuadrados, tramos contiguos que suman el total, la caja envolvente en las medidas reales
  y el **volumen firmado** de cada primitiva y de cada pieza: cerrada y bien orientada da
  positivo y parecido al analitico (cilindro `2pi`, cono `pi`, esfera `4/3 pi`, cubo `8`).
  Eso es lo que caza las caras invertidas, que a ojo pasan por buenas.
  Un identificador puede ocupar varios tramos **seguidos** (el stick son tres colores); si
  reaparece mas tarde es que se ha reutilizado por error en otro control, y falla.
- `web/smoke.html` — con navegador: monta las dos escenas y **pincha cada control en el
  centro de su cara de arriba**, proyectado con la misma camara que dibuja. Si vuelve otro
  identificador, dice quien tapa a quien. El Z se comprueba aparte, girando la camara por
  debajo, porque va en la panza del mango central y de frente lo tapa el mango (que se
  cumpla eso tambien se comprueba). Un barrido a ciegas de miles de clics no vale: tumbaba
  el renderer por software y ademas no decia quien tapaba. Con Edge sin ventana:
  `msedge --headless=new --use-angle=swiftshader --enable-unsafe-swiftshader
   --screenshot=x.png http://127.0.0.1:9140/smoke.html`.
  Detalle que costo un rato: hay que llamar a `sc.resize()` antes de proyectar, porque el
  lienzo todavia no se ha dibujado y su relacion de aspecto es la de por defecto (300x150).

## Biblioteca y caratulas

Seis modos de vista: **Coverflow** (3D con reflejo, rueda del raton), **Filas** estilo
Netflix agrupadas por region, **Rejilla**, **Rueda** estilo Hyperspin (arco por coseno) y
**Tabla** con ID de cartucho, region, formato, tamano y CRC, y **Estante**, que pone la caja
de carton y el cartucho del juego elegido en 3D con la caratula de textura (si no hay
caratula, la portada se queda de un gris azulado, que se lee como "no hay" y no como un
fallo de carga). Flechas navegan, Enter lanza,
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
entero: MI, SP, DPC, VI, AI, PI, SI), **RSP**, **Perfilador** (top de rutas calientes por
bucket de 16 bytes, con arranque/parada/vaciado desde la propia pestana) y **Depurador**.

La ocupacion es la cifra que hace falta mirar en modo multihilo: el porcentaje de CPU sube
cuando la CPU **gira esperando** al RCP, asi que sin `cpuWaitPct` / `rdpBusyPct` / `rspBusyPct`
el estado engañaria sobre quien es el palo largo.

### Depurador

Encima de las ordenes que ya existian en el servidor y que el MCP usa desde siempre:
`cpu.disasm`, `cpu.step`, `cpu.run_until`, `cpu.bp.add/del/list` y `mem.read`. No hubo que
tocar el nucleo para tenerlo; solo faltaba ensenarlo.

- Desensamblado con el PC resaltado y los puntos de ruptura marcados al margen. Pulsar una
  linea pone o quita el punto. "Seguir al PC" se puede apagar para mirar otra direccion.
- Paso de 1, 10 y 1000 instrucciones; "Correr hasta" usa `cpu.run_until`, que lleva su propio
  tope de tiempo (5 s), de modo que una direccion que no se alcanza no cuelga la ventana:
  vuelve diciendo que no llego.
- Visor de memoria por region (RDRAM, DMEM, IMEM, PIF_RAM, CART_ROM, SAVE, EEPROM) con
  interruptor **Coherente**: la lectura pasa por la cache de datos de la CPU, asi que lo que
  el nucleo escribio y todavia no ha volcado a RDRAM se ve. Sin eso, las estructuras del
  kernel de libultra salen rancias.
- El depurador **no** se refresca solo. Cada repintado toma el candado del nucleo y, a marcha
  libre, ese candado se suelta una vez por campo de video: refrescar en bucle convertiria la
  ventana en un freno. Se repinta tras cada accion y con "Releer".
- El paso a paso pausa el nucleo antes de nada. Pasar instrucciones sobre un nucleo en marcha
  no significa nada.

**Enteros de 64 bits.** El nucleo manda `pc`, los registros generales y COP0 como u64.
`JSON.parse` los mete en un `double` y por encima de 2^53 se pierden los bits **bajos**, que
en una direccion son justo los que importan: `0xffffffff80246dd8` llegaria al navegador con
la direccion cambiada y un punto de ruptura puesto sobre ella caeria en otro sitio. El puente
Python todavia tiene el entero exacto, asi que lo que no cabe sale ya como cadena
`"0x...."` y el navegador nunca lo trata como numero. Los bytes crudos (`mem.read`) van en
base64: un tercio de lo que costaria en hexadecimal y sin un segundo viaje.

El cliente de telemetria (`tele.py`) es una copia suelta del protocolo, no una dependencia del
puente MCP: marcos con la longitud por delante, JSON y detras los bytes crudos. Las lecturas
del perfilador pausan el nucleo un instante y restauran el estado anterior, porque el bucle de
marcha libre tiene cogido el candado del nucleo en tandas de ~un campo de video.

## Que falta

- Vigilar escrituras (`KESTREL_WATCHP`) desde el depurador, y puntos de ruptura sobre el RSP.
- Segundo, tercer y cuarto mando (hoy solo `KESTREL_PAD1`).
- Cambiar opciones en caliente sin reiniciar el emulador.
