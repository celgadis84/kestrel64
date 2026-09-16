# Peliculas de entradas (TAS)

## Como se usa

```
KESTREL_MOVIE_REC=partida.k64m  kestrel64.exe "Super Mario 64 (USA).z64" --play
KESTREL_MOVIE_PLAY=partida.k64m kestrel64.exe "Super Mario 64 (USA).z64" --play
```

O las dos casillas de **Peliculas** en el lanzador. Con las dos puestas manda la de
reproducir. Sin ninguna no hay ni fichero ni coste: cada lectura de botones compara un enum
contra cero.

Teclas de la ventana, para colocar una pulsacion en el fotograma exacto:

| Tecla | Que hace |
|-------|----------|
| `P` | pausa / en marcha |
| `F` | avanza UN campo de video con la pausa puesta |
| `Shift+F` | avanza ocho campos |
| `F5` / `F7` / `F6` | guardar / cargar / cambiar de ranura (los de siempre) |

Por telemetria: `frame.advance {fields, timeout_ms}` (herramienta MCP `frame_advance`)
avanza N campos y vuelve a parar, y `emu.status` trae un objeto `movie` con
`{mode, polls, total, ended, path}` mientras hay pelicula.

## Que se graba, y por que eso y no otra cosa

Lo que se graba **no es lo que aprieta el jugador** sino **lo que el juego lee**: cada
respuesta al comando `0x01` del joybus (leer botones) de cada conector, con el mando del
anfitrion y el inyectado por telemetria ya resueltos. Esa es la unica frontera que el
invitado percibe, y es la que hay que reproducir para que una repeticion sea la misma
partida.

Grabar "por fotograma" seria mentir: hay juegos que sondean el mando dos veces en un campo,
otros que se saltan campos enteros, y el orden de los cuatro conectores lo decide el bloque
de comandos que el juego escribe en la PIF RAM, no el emulador. Contando sondeos, una
pelicula vale igual al 30% que al 200% de velocidad, y vale igual en interprete que en JIT.

El punto de enganche es uno solo, `movie::sample()` en `Memory::pifProcessJoybus` justo
antes de escribir la respuesta (`src/core/memory.cpp`). Grabando la apunta; reproduciendo la
sustituye.

## Formato `.k64m`

Todo little-endian, cabecera de 64 bytes y detras la lista plana de muestras de 5 bytes.
Se lee con `xxd` y no depende del relleno del compilador.

```
0x00  8   "K64MOVIE"
0x08  4   version (1)
0x0C  4   banderas (bit0: la pelicula empieza en un arranque en frio)
0x10  4   crc1 de la cabecera del cartucho (0x10)
0x14  4   crc2 de la cabecera del cartucho (0x14)
0x18  1   norma de television (0 PAL / 1 NTSC / 2 MPAL)
0x19  1   RDRAM en MB (4 u 8)
0x1A  1   mascara de conectores enchufados al grabar
0x1B  1   reservado
0x1C  8   numero de muestras (se rellena al cerrar; 0 = pelicula truncada)
0x24  20  nombre interno del cartucho
0x38  8   reservado

muestra: u8 conector · u8 botones alto · u8 botones bajo · s8 eje X · s8 eje Y
```

Comprobaciones al reproducir:

- **CRC del cartucho distintos = no se reproduce nada.** Inyectar los botones de otra
  partida solo produce basura que parece un fallo del emulador, asi que se dice de quien es
  la pelicula y se para ahi.
- **tv / RDRAM distintos = aviso una vez.** Cambian el ritmo de campo y `osMemSize`; la
  repeticion puede seguir o desviarse, y quien la lanza tiene que saberlo.
- **Conector que no casa = aviso una vez y se sigue.** Parar en seco perderia el resto de la
  pelicula, que casi siempre sigue valiendo.
- **Fin de la cinta**: se avisa y a partir de ahi vuelve a mandar el mando del anfitrion.

Una grabacion matada a mitad (el proceso muere sin cerrar el fichero) deja la cuenta de
muestras a cero, y eso se reproduce igual: sin cuenta en la cabecera el final es el del
fichero.

## Estados guardados: la cinta se rebobina con el juego

El numero de sondeos consumidos es estado de la partida tanto como la RDRAM. Un estado
guardado lo lleva dentro (seccion `MOVI`, `src/core/savestate.cpp`) y al cargarlo la
pelicula se rebobina al mismo sondeo. Sin eso, cargar un estado rebobinaria el juego pero no
la cinta y la repeticion se desviaria justo en el uso que junta las dos cosas: rehacer un
tramo. **Grabando**, rebobinar y seguir grabando pisa la toma anterior -- o sea que es
regrabar, y la cola vieja que quede detras la ignora la cuenta de la cabecera.

Esto subio la version del formato de estado a **5**: los estados guardados con la 4 no se
cargan.

## Que esta comprobado

- SM64, 600 campos, interprete en lockstep: control (sin botones) `109c2277...`, grabacion
  con START pisado `0ce65ed4...` (distintos = la entrada de verdad cambia la partida), y la
  **reproduccion da el md5 de la grabacion, byte a byte**, con las mismas 471 muestras.
- La misma pelicula sobre Donkey Kong 64: *"esta pelicula es de otro cartucho: grabada con
  crc 635a2bff/8b022326, cargado ec58eabf/ad7c7169. No se reproduce."*
- `frame.advance` avanza exactamente lo pedido (1 -> 1 campo, 5 -> 5) y el nucleo se queda
  quieto entre avances.
- Guardar en el sondeo 54, avanzar 40 campos (sondeo 74) y cargar deja el contador otra vez
  en 54.

## Lo que todavia no hay

- No se graba la cadena de estados guardados dentro de la pelicula (una repeticion arranca
  siempre en frio; las ranuras son cosa aparte).
- No hay editor de la cinta: se edita con un script sobre el formato de arriba.
- La pelicula no lleva la version del emulador. Una repeticion vale mientras la emulacion no
  cambie, y eso lo garantizan los gates (md5 de SM64 congelado), no el fichero.
