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
- **frontend**: HTML/CSS/JS a pelo en `web/`. El 3D es propio: un motor WebGL1 de unas
  seiscientas lineas (`gl.js`) con geometria generada en codigo (`models.js`), sin una sola
  biblioteca vendorizada. Las caratulas de la biblioteca son cajas de carton con volumen, no
  laminas; queda CSS `preserve-3d` de respaldo para navegadores sin WebGL.
- **ventana**: Edge o Chrome en modo `--app`, sin barra de direcciones ni pestanas. Si no
  hay ninguno se cae al navegador por defecto.

El perfil vive en `tools/launcher/profile.json` y se guarda solo, sin boton de guardar.

## El mismo catalogo DENTRO de la ventana del emulador

La ventana de juego no es un visor: es la aplicacion. Lleva barra de menu nativa (Win32) con
**las mismas opciones que el lanzador**, porque las dos leen el mismo catalogo.

```
tools/launcher/options.py      esquema unico (fuente de la verdad)
        |
        +-- OPT.to_env()               -> lanzador (Python)
        +-- tools/gen_optdefs.py       -> src/ui/optdefs.cpp  (GENERADO, no editar a mano)
                                          src/ui/profile.cpp  lee/escribe el MISMO profile.json
                                          src/ui/menu_win32.cpp barra de menu + dos dialogos
```

**Al tocar `options.py` hay que regenerar**:

```bash
python tools/gen_optdefs.py      # reescribe src/ui/optdefs.cpp
```

### Que se aplica en caliente y que no

`src/core/runtime.hpp` es un punado de atomicos que se **siembran desde el entorno al
arrancar** (`rt::initFromEnv()` en `main()`), asi que el modo lote y los gates se comportan
bit a bit igual que antes; a partir de ahi manda quien los escriba, o sea el menu.

| En caliente (`rt::`) | Exige relanzar |
|---|---|
| escala / tamano / pantalla completa | backend RDP, hilos, dynarec |
| HUD de telemetria | overclock por dominio |
| sonido y volumen | tipo de guardado, puerto |
| limitador de velocidad | todo lo demas |
| mapa de mando (`rt::padGen`) | |

Lo que no se puede cambiar en marcha se guarda en el perfil y el emulador **se relanza a si
mismo**: reconstruye entorno y linea de ordenes con `ui::toEnv()`, borra antes toda variable
`KESTREL_*` heredada (si no, una opcion recien apagada seguiria encendida por herencia) y
hace `CreateProcess`. El relanzado ocurre en `main()` **despues** de `audio::shutdown()`,
porque el proceso nuevo abre waveOut nada mas arrancar. Las opciones marcadas `(*)` en el
dialogo son justo esas.

### Detalles que no son cosmeticos

- Los dialogos corren en **su propio hilo** con su propio bucle `IsDialogMessage`. El hilo
  principal esta presentando cuadros y no puede meterse en un bucle modal sin congelar la
  imagen.
- La subclase del `WndProc` de GLFW se engancha con `SetWindowLongPtrW`/`CallWindowProcW`:
  con las versiones `A` la ventana pasaria a ANSI y GLFW recibiria `WM_CHAR` mutilado.
- Los atajos (F11, F12, Ctrl+O, Ctrl+P) se atienden y **se dejan pasar**: el teclado del
  juego se lee con `glfwGetKey`, que se alimenta de esos mismos mensajes, y tragarse el
  `WM_KEYDOWN` sin el `WM_KEYUP` dejaria la tecla clavada.
- Poner la barra encoge el area de cliente: se agranda la ventana `SM_CYMENU` para que la
  imagen no pierda una franja (en pantalla completa no, ahi ya cubre el monitor).
- El dialogo de mando escribe `pad1.cfg` y sube `rt::padGen`; `loadPadMap()` relee el fichero
  y lo **superpone** al mapa de fabrica, en vez de sustituirlo.

## Arquitectura

```
options.py            esquema unico de opciones (116 opciones en 11 categorias) + mando
kestrel_launcher.py   servidor HTTP + escaneo de ROMs + caratulas + proceso del emulador
web/index.html        estructura
web/style.css         tema oscuro, lienzo de la biblioteca 3D, coverflow de respaldo
web/app.js            estado, vistas, modales, captura de teclas
web/gl.js             motor WebGL1 minimo (matrices, primitivas, seleccion por color)
web/models.js         geometria propia: mando, cartucho y caja
web/scene3d.js        pega esa geometria a la interfaz (mando, estante, carrusel)
web/smoke.html        prueba visual suelta de las mallas + barrido de clics
web/smoke_car.html    las tres colocaciones del carrusel 3D, sin servidor ni ROMs
web/smoke_e2e.html    la aplicacion entera dentro de un iframe, para captura sin manos
models_test.js        prueba de geometria sin navegador (`node tools/launcher/models_test.js`)
carousel_test.js      prueba del reparto de casillas y de la malla de K cajas
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

Siete modos de vista. Los tres de caratula son **3D de verdad** (ver la seccion siguiente):
**Coverflow 3D** (fila recta, las de los lados giradas hacia dentro), **Carrusel 3D** (anillo
giratorio, al estilo de USB Loader GX) y **Rejilla 3D** (pared de cajas). Los otros cuatro son
de texto: **Filas** estilo Netflix agrupadas por region, **Tabla** con ID de cartucho, region,
formato, tamano y CRC, **Lista Wonder** (filas altas al estilo WonderMenu, ver mas abajo) y
**Estante**, que pone la caja de carton y el cartucho del juego elegido con la caratula de
textura (si no hay caratula, la portada se queda de un gris azulado, que se lee como "no hay"
y no como un fallo de carga). Flechas navegan, rueda del raton mueve, Enter lanza, doble clic
lanza.

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

## La biblioteca en 3D del lanzador web

Las caratulas no son imagenes en perspectiva: son **cajas de carton con volumen**
(`MODELS.buildBox`, 13,5 x 19,0 x 3,0 cm, con la franja roja del canto de las cajas europeas
de N64) con la portada de textura en la cara frontal. `SCENE3D.mountCarousel` las coloca de
tres maneras, y el selector de vista solo cambia la colocacion:

| vista | colocacion | camara |
|---|---|---|
| Coverflow 3D | fila recta, giro de los lados por `tanh` | `[0, 1.5, 66]` |
| Carrusel 3D | anillo de radio 34, paso 0,50 rad | `[0, 10, 60]` |
| Rejilla 3D | pared de 5 columnas, filas que suben y bajan | `[0, 0, 104]` |

Tres decisiones que son las que hacen que esto valga para una biblioteca de verdad:

- **Una sola malla, una matriz por caja.** Un tramo de malla (`part`) puede llevar su propia
  matriz (`part.mat`, anadido en `gl.js`). Las K cajas se suben a la GPU **una vez**, y
  moverse por la biblioteca o cambiar de vista no reconstruye geometria: solo cambian K
  uniformes por cuadro. Veinticinco cajas son 2900 vertices y 2500 triangulos.
- **Casillas recicladas por modulo.** Solo hay K = 25 cajas. El juego `i` vive siempre en la
  casilla `i % K`, y `SCENE3D.slotItem` da, para cada casilla y cada centro, el unico juego
  congruente que cae en la ventana visible. Al desplazarse, la unica casilla que cambia de
  juego (y de textura) es la que acaba de salir por el otro lado: la biblioteca puede tener
  mil cartuchos y el coste por cuadro no se mueve. Eso lo comprueba
  `node tools/launcher/carousel_test.js`, que para varias longitudes de lista verifica que
  ninguna casilla repite juego, que el juego elegido SIEMPRE le toca a alguna casilla (si no,
  el centro del carrusel saldria vacio), que la ventana no tiene huecos y que un centro
  fraccionario -- el carrusel a medio camino -- reparte igual que el entero mas cercano.
- **Orden de dibujo por profundidad.** `Scene.order` permite dibujar los tramos de atras
  hacia delante, que es lo que hace que la caja elegida tape a las vecinas cuando se cruzan.

La seleccion se hace con la misma pasada de color por tramo que el mando: cada portada lleva
el id `s<casilla>:COVER`, asi que un clic sabe exactamente en que caja ha caido, sin
matematicas de proyeccion. Un clic en la caja elegida lanza el juego; en otra, la elige.

Sin WebGL (`mountCarousel` devuelve `null`) se cae a las vistas de CSS de antes -- coverflow
con reflejo, rueda por coseno y rejilla de tarjetas -- que se quedan en el fichero justo para
eso.

Para verlo sin servidor ni ROMs: `web/smoke_car.html` monta las tres colocaciones con
caratulas pintadas al vuelo (`#flow`, `#ring`, `#wall` en el hash). Sin ventana:
`msedge --headless=new --use-gl=swiftshader --enable-unsafe-swiftshader
 --virtual-time-budget=4000 --screenshot=x.png .../smoke_car.html#ring` (hace falta el
presupuesto de tiempo virtual: el dibujo va por `requestAnimationFrame` y sin el la captura
sale antes del primer cuadro).

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

## Referencia estetica: WonderMenu

<https://github.com/lmcd/WonderMenu> — menu/lanzador para flashcarts de N64, escrito para
correr EN la consola. Es la estetica que queremos replicar en el lanzador de kestrel64: la
referencia no es un lanzador de PC (RetroArch, Big Box), es un menu de cartucho, hecho con
la paleta y las limitaciones de la maquina.

Lo que hay que mirar de el, en orden:

- **Tipografia y espaciado**: fuente de mapa de bits pensada para 320x240 y sobreescaneo,
  con margenes gordos. Nuestro lanzador vive en un navegador a resolucion de PC, asi que el
  parecido tiene que salir de la proporcion y del pixelado deliberado, no de copiar tamanos.
- **Paleta**: pocos colores, planos, alto contraste. Nada de degradados de escritorio.
- **Layout de lista**: una columna de juegos con seleccion resaltada y un panel lateral con
  los datos del cartucho, que es justo lo que ya sacamos de la cabecera (ID, region, formato,
  tamano, CRC). Encaja con la vista **Tabla** que ya existe.
- **Transiciones**: cortas y duras, sin suavizado; se lee como hardware, no como web.
- **Sonido de menu**: pitidos cortos al mover y al elegir.

Ya no es solo una nota de direccion artistica: **esta implementada** en la biblioteca que
abre el propio emulador (`src/ui/library_win32.cpp`, seccion siguiente). En el lanzador web
de `tools/launcher/` sigue pendiente, y cuando se ataque entra como un tema/piel mas (las
seis vistas siguen), no como una reescritura.

## Temas: el aspecto sale de tokens, no de reglas repetidas

El lanzador tiene dos aspectos y se elige en el desplegable de arriba a la derecha:

| tema | de que va |
|------|-----------|
| **Tema cernicalo** | el de siempre: fondo azulado, acento ambar, esquinas de 12 px |
| **Tema WonderMenu** | monocromo, letra de titular ancha, pastilla maciza de radio 17 |

Todo el aspecto vive en variables de `:root` en `web/style.css`. El tema es un atributo del
elemento raiz (`<html data-theme="wonder">`) y lo unico que hace `applyTheme()` en `app.js`
es ponerlo: el navegador reevalua los tokens solo, no hay que repintar nada a mano ni volver
a montar ninguna vista 3D. Un nombre de tema desconocido cae al de fabrica. Se guarda en el
perfil como `theme`, igual que `viewmode`.

Los tokens que un tema puede cambiar, ademas de los colores de siempre: `--font-display`
(letra de titulares, con su `--disp-track` y `--disp-weight`), `--bgimg` (los degradados del
fondo), `--glass`/`--glass2` (barras superior e inferior), `--onacc` (letra sobre el acento),
`--selbg`/`--seltxt`/`--selr` (fondo, letra y radio de lo elegido). Anadir un tercer tema es
anadir un bloque `:root[data-theme="..."]` y su nombre a `THEMES`, nada mas.

### De donde sale el tema WonderMenu

De leer la fuente de [lmcd/WonderMenu](https://github.com/lmcd/WonderMenu), que es un menu de
flashcart de N64. Lo que se replica es la **disposicion y la paleta**, no los recursos: el
proyecto es AGPLv3, asi que **no se copia ni una fuente ni un sprite**.

Lo que dice su fuente y aqui se imita:

- La paleta es monocroma de verdad: su `util/Color.h` solo define `CLEAR`, `BLACK`, `WHITE`,
  `RED`, `GREEN` y `BLUE`. De ahi que el tema ponga `--acc` a blanco y deje el rojo y el
  verde solo para aviso y "todo bien".
- El subtitulo es **el mismo blanco del titulo con el alfa a la mitad** (`a *= 0.5`), no un
  gris distinto. En la Lista Wonder eso es `opacity:.5` sobre `currentColor`, no otro color.
- Lo elegido es una **pastilla maciza** redondeada, no un borde: radio 17 cuando esta
  expandida y 8 cuando no, con rectangulos que **sobresalen por los dos lados** y 5 px mas de
  alto. En CSS: `margin:0 -15px` con `padding:0 15px` y `height` de 72 a 77 px.
- Las medidas de una fila: etiqueta a 112 px del borde, subtitulo 20 px por debajo,
  accesorios de la derecha separados 12 px con 15 px de margen, contador de 50 px de ancho.
- Sus titulares van en Unbounded 900. Aqui la pila es
  `"Unbounded","Archivo Black","Segoe UI Variable Display","Segoe UI",system-ui` — si la
  fuente no esta instalada cae sola, no se descarga nada (el lanzador funciona sin red).

Un desvio deliberado: **el fondo NO es negro**. En el original la pastilla de lo elegido es
negra maciza con letra blanca, y sobre un fondo negro no se veria. El tema usa gris carbon
(`--bg:#2b2b32`), que es lo que deja resaltar la pastilla sin salirse del monocromo.

### Prueba

`web/smoke_theme.html` mete el lanzador en un iframe y comprueba lo que de verdad puede
fallar: que el selector existe con sus dos opciones, que `applyTheme()` cambia el atributo,
que los tokens resuelven distinto (`--selr` 0px contra 17px, `--bg`, `--acc`,
`--font-display`), que el selector se sincroniza, que un tema inventado cae al de fabrica,
que `body` pinta fondo propio (si fuera transparente tomaria el del anfitrion) y que el
evento `change` aplica el tema. Que `touch()` lo GUARDA no se ve desde el iframe --- `CFG` es
un `let` de guion clasico y no cuelga de `window` --- asi que eso se mira desde fuera leyendo
`tools/launcher/profile.json` (salvandolo y restaurandolo alrededor de la prueba).

```
python tools/launcher/kestrel_launcher.py --port 9141 --no-open &
msedge --headless=new --no-sandbox --user-data-dir=<tmp> --virtual-time-budget=8000   --dump-dom http://127.0.0.1:9141/smoke_theme.html
```

**GOTCHA**: `--use-gl=swiftshader --disable-gpu` **revienta** el render sin cabeza en cuanto
la pagina lleva un iframe que hace `fetch` (`Abnormal renderer termination`, el volcado sale
vacio). Sin esas dos banderas va bien. Y `--dump-dom` vuelca en el evento `load`, asi que sin
`--virtual-time-budget` no se ve nada de lo que escriben los `setTimeout`.


## Biblioteca dentro del emulador: el carrusel 3D

`src/ui/library.hpp` + `src/ui/library_win32.cpp`. Es lo que sale al abrir `kestrel64.exe`
sin ROM (doble clic en el Explorador), lo que abre `Archivo > Biblioteca de ROMs...` desde la
ventana del juego, y lo que abre `--library` a mano desde una shell. No tiene NADA que ver
con el camino de presentacion del juego: es Win32 + GDI puro sobre un lienzo propio, sin
GLFW, sin Vulkan y sin tocar `src/video/`.

**Estetica.** Toda la escena se dibuja en un lienzo de **384x216** y se sube a la ventana con
`StretchBlt` en `COLORONCOLOR` (vecino mas cercano) por un factor **entero**, con bandas del
color de fondo alrededor. El pixel gordo es el fin, no un efecto colateral. La tipografia es
la fuente de trama **Terminal** de Windows (`vgaoem.fon`, 8x12 y 8x16, `OEM_CHARSET` +
`NONANTIALIASED_QUALITY`): una fuente de consola de la epoca de verdad, no una imitacion.
La paleta son seis colores planos. Las transiciones duran ~120 ms y los pitidos de mover y
elegir son `Beep()` en un hilo suelto, respetando la opcion `audio` del perfil.

**El carrusel es 3D de verdad.** Proyeccion en perspectiva (camara pinhole, focal 300) y
rasterizado propio de los dos triangulos de cada caratula con interpolacion corregida por
`1/z`. La alternativa de Windows, `PlgBlt`, solo sabe transformaciones afines: una caratula
inclinada saldria como un paralelogramo, no como una caja girada. Las laterales giran ~58
grados sobre Y con el canto INTERIOR mas cerca de la camara, se van hundiendo en Z y se
atenuan con la distancia; la central va recta, grande y con un marco de acento que se
traslada durante la transicion en vez de saltar. Debajo va un reflejo (el 30% de arriba de
la caratula, espejado, desvanecido y rayado en las lineas impares: se lee como un tubo de
rayos catodicos, no como cristal de pagina web) y una raya tenue de suelo para que el
carrusel se apoye en algo.

**Caratulas sin dependencias nuevas.** El cache del lanzador son PNG, y aqui se leen con un
decodificador propio de ~120 lineas montado sobre el DEFLATE que ya tiene el proyecto
(`kestrel::archive::inflate`, el mismo que abre las ROMs en `.zip`): 8/16 bits, gris,
gris+alfa, RGB, RGBA y paleta, sin entrelazar. Meter GDI+ o WIC en el enlace por un
descodificador que corre un punado de veces al abrir un menu no sale a cuenta, por el mismo
motivo por el que el DEFLATE es propio. La imagen se encaja en 132x184 recortando por el
lado que sobra (nunca deformando) y promediando el bloque de origen de cada pixel: bajar de
600x800 tomando muestras sueltas convierte el texto de la caja en ruido. Se cargan como
mucho dos por cuadro y se sueltan las que quedan a mas de 16 posiciones (97 KB cada una).

Se buscan, en este orden, en `<perfil>/cache/boxart/` (el MISMO cache que llena el lanzador,
asi que una descarga vale para los dos), `<carpeta de ROMs>/boxart/`, junto a la ROM y
`<exe>/boxart/`; con dos claves, el nombre del fichero y el nombre interno del cartucho, las
dos saneadas igual que las saneaba el lanzador. Un fichero **vacio** significa "ya se busco y
no hay", igual que en el lanzador.

**Sin caratula no queda un hueco gris**: se fabrica una portada plana con el color sacado del
hash FNV-1a del titulo sobre una paleta de doce colores, con el ID de cartucho arriba, el
titulo partido en el centro y una tira de "SIN CARATULA" abajo. Cada juego queda reconocible
de un vistazo aunque no haya arte.

**Datos del cartucho.** Debajo del carrusel van el titulo interno en grande, el nombre de
fichero, y la barra de datos con ID, region, formato, tamano y los dos CRC — la misma
cabecera normalizada por orden de bytes que describe la seccion de arriba. De un `.zip` o
`.gz` no se lee la cabecera (habria que descomprimir 8-64 MB por cada linea de la lista) y se
dice, en vez de mentir con ceros.

**Entrada.** Flechas o A/D mueven, RePag/AvPag saltan de diez en diez, Inicio/Fin a los
extremos (un salto largo no cruza la biblioteca volando: la posicion se acerca de golpe y
solo se anima el ultimo tramo), Enter o Espacio lanzan, Esc sale, F2 elige carpeta, F3 o
Ctrl+O abren el dialogo de fichero de siempre para una ROM que este fuera de la carpeta, F5
relee. La rueda del raton mueve, el clic elige (con acierto sobre el cuadrilatero proyectado
de verdad, no sobre un rectangulo) y el clic sobre la central lanza. El mando se sondea con
`joyGetPosEx` (winmm, ya enlazado) y no con GLFW: la biblioteca puede correr antes de que
exista la ventana del juego y en todo caso corre en OTRO hilo, y a GLFW no se le puede llamar
desde donde no se le inicializo. Cruceta o palanca en X mueven (repeticion 340 ms y luego 90),
A o Start lanzan, B sale.

**Carpeta y perfil.** Arranca en `romdir` del perfil compartido, si no en la carpeta de la
ultima ROM jugada, si no en `<exe>/roms` o `../test_roms`; y deja la seleccion puesta sobre la
ultima ROM jugada. Al elegir carpeta o ROM se guarda `romdir` **releyendo el perfil del disco
y tocando solo esa clave**: el menu de la ventana tiene su propia copia viva del perfil y
sobrescribirla entera desde aqui le borraria cualquier cambio a medias.

**Hilos.** La biblioteca monta su ventana y su propio bucle de mensajes, o sea que BLOQUEA al
hilo que la llama. Desde `main.cpp` eso es el hilo principal antes de que haya nada que
presentar, y no pasa nada; desde el menu de la ventana del juego se lanza en un hilo aparte,
porque ese hilo esta presentando cuadros y no puede meterse en un bucle modal sin congelar la
imagen. Solo puede haber una abierta por proceso.

## Ficha de juego

Elegir un juego (clic sobre el ya elegido, Intro, o el clic central del carrusel 3D) **abre
su ficha en vez de arrancarlo**. El doble clic sigue lanzando sin preguntar, y el interruptor
"Elegir un juego lo arranca directamente, sin ficha" de la propia ficha guarda
`select_action: "play"` en el perfil para quien prefiera el comportamiento de antes (por
defecto `"card"`). El muelle lleva ademas un boton **Ficha** junto a Jugar. Dentro de la
ficha, Intro juega salvo que se este escribiendo en un campo.

Codigo: `tools/launcher/gamecard.py` (todo lo que toca disco, sin HTTP), rutas en
`kestrel_launcher.py`, interfaz en `web/card.js` + modal `#m-game`. Prueba de unidades
`python tools/launcher/gamecard_test.py`; humo visual `web/smoke_card.html` (Edge sin
cabeza, `?tab=info|manual|cheats|saves`).

**Pestanas.**

- *Informacion*: cabecera del cartucho (nombre interno, ID, region, CRC, formato de volcado,
  fichero base) y una ficha editable (anio, desarrollo, distribuidor, genero, jugadores,
  notas) que se guarda sola a los 450 ms de dejar de escribir.
- *Manual*: busca PDF/imagenes/texto/HTML en la carpeta de la ROM y en `manuals/` o
  `manuales/` cuyo nombre normalizado empiece por el nombre del fichero de la ROM o por su
  nombre interno (4+ letras). Se ven dentro (iframe o imagen) o aparte. Se puede soltar un
  fichero encima o elegirlo: se copia a `<carpeta ROM>/manuals/<base><ext>` (o `<base> (N)`).
- *Trucos*: el `.cht` que el emulador carga solo (`src/core/cheats.cpp`, ver
  `docs/CHEATS.md`). Interruptor por truco, borrar, apagar todos y anadir truco GameShark.
  Si el perfil fija `cheats` (= `KESTREL_CHEATS`) se avisa de que ese fichero manda sobre el
  de la ficha; si el juego esta en marcha, de que el cambio vale al siguiente arranque.
- *Partidas*: `.eep/.sra/.fla`, `.mpk*` y estados `.st0..9`, con tamano y fecha.

**Nombres = los del emulador.** Base = ruta de la ROM sin `.zip`/`.gz` y sin una extension
(`archive::stripContainerExt` + punto). El lector de `.cht` del lanzador es el mismo automata
que `Cheats::loadFile`: comentario `#`/`;` tambien a final de linea, cabecera `[nombre]` y
`[-nombre]` apagado, dos hex separados por espacio/tab/coma/dos puntos, codigos antes de la
primera cabecera = truco implicito "sin nombre". Las familias 88/89/CC/DE/EE/FF se muestran
como "sin efecto" porque el emulador las lee pero no las aplica. **Reescribir respeta el
fichero**: al encender/apagar solo cambia el `-` de la cabecera (comentarios, lineas
ilegibles y finales CRLF/LF intactos); el bloque implicito recibe una cabecera `[sin nombre]`
la primera vez que se apaga. Verificado con el exe real: el `.cht` escrito por la ficha lo
carga el emulador con los mismos encendidos/sin efecto que muestra la ficha.

**Datos del lanzador.** `STATE/games/<CRC>.json`, con clave el CRC de la cabecera, asi que
renombrar o comprimir la ROM no pierde la ficha. Campos en lista blanca
(`year developer publisher genre players notes favorite`) mas estadisticas `plays`,
`seconds`, `last`, que apunta el propio lanzador cuando termina el proceso del emulador que
el lanzo (sesiones de menos de 3 s no cuentan: son arranques fallidos).

**API.**

| Ruta | Que hace |
|------|----------|
| `GET /api/game?rom=` | ficha entera: cabecera, partidas, manuales, trucos (+`override`), meta, `running` |
| `GET /api/manual?rom=&i=` | sirve el manual numero `i` de los que encontro la ficha, `inline` |
| `POST /api/game/manual?rom=&name=` | cuerpo crudo = fichero de manual (extension en lista blanca, <= 64 MB) |
| `POST /api/game/meta` | `{rom, meta}` |
| `POST /api/game/cheats` | `{rom, states: {id: bool}}` |
| `POST /api/game/cheat/add` / `del` | `{rom, name, codes, on}` / `{rom, id}` |

**Seguridad.** Toda ruta de ficha valida antes la ROM (`_rom_ok`: existe, extension de ROM,
cabecera N64 legible), de modo que `rom=C:\Windows\win.ini` da 404. Los manuales se sirven
**solo por indice** dentro de la lista que la ficha descubrio, nunca por ruta que mande el
cliente.

## Que falta

- Vigilar escrituras (`KESTREL_WATCHP`) desde el depurador, y puntos de ruptura sobre el RSP.
- Segundo, tercer y cuarto mando (hoy solo `KESTREL_PAD1`).
- Cambiar opciones en caliente sin reiniciar el emulador.
- La piel WonderMenu en el lanzador web, como un tema mas de los que ya hay.
