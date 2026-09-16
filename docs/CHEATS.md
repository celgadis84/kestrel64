# Trucos (GameShark / Action Replay)

## Como se usa

Un fichero de texto `.cht` al lado de la ROM y con su mismo nombre
(`Super Mario 64 (USA).cht`) se carga solo. `KESTREL_CHEATS=<fichero>` -- o la casilla
**Cartucho -> Fichero de trucos** del lanzador y del menu de la ventana -- fuerza otro.
Sin fichero no hay ni motor ni coste: `Cheats::enabled()` es falso y el bucle no llama a
nada.

```
# Super Mario 64 (USA)
[Vidas infinitas]
8033B21D 0064

[-Monedas al maximo]        ; el guion delante del nombre lo deja cargado pero APAGADO
8033B21A 0063
```

`#` y `;` abren comentario (tambien al final de una linea de codigo), y los codigos admiten
los separadores con los que circulan publicados: espacio, tabulador, coma o dos puntos.

## Que hace el aparato de verdad, y por que importa

El GameShark se metia entre el cartucho y la consola: sustituia el arranque por el suyo y
enganchaba la **interrupcion del VI**. En cada campo de video su motor recorria la lista de
codigos y escribia en la RDRAM. De ahi salen las dos decisiones de esta implementacion:

- **El ritmo es el campo de video**, no el fotograma del juego. Un juego a 20 fps recibe
  los parches tres veces por fotograma suyo, y por eso "vidas infinitas" gana la carrera
  contra el codigo que las resta. El enganche esta en `System::run`, junto al
  `memory.viTick()` que cierra el campo y con el nucleo parado bajo `coreMutex`.
- **El nibble alto de la direccion es el segmento MIPS**, no decoracion. `0x80xxxxxx` es
  KSEG0: escritura **cacheada**, que el juego ve al momento aunque la linea tarde en bajar
  a la RDRAM (`CPU::pokePhysCoherent` -> `dcWrite`). `0xA0xxxxxx` es KSEG1: escritura
  **sin cache**, directa a la RDRAM, que deja la linea de D-cache como estaba -- igual que
  un store KSEG1 del VR4300. Por eso las familias A0/A1 se publican como parche unico de
  arranque: corren antes de que exista esa linea. `test/cheat_test.cpp` fija esa asimetria
  como invariante.

## Familias implementadas

| Codigo | Efecto |
|---|---|
| `80 XXXXXX 00YY` | escribe el byte `YY` en cada campo (cacheada) |
| `81 XXXXXX YYYY` | escribe la media palabra `YYYY` en cada campo (cacheada) |
| `A0 XXXXXX 00YY` | escribe el byte una sola vez, al arrancar (sin cache) |
| `A1 XXXXXX YYYY` | igual con media palabra |
| `D0 XXXXXX 00YY` | condicion: ejecuta la linea siguiente si el byte vale `YY` |
| `D1 XXXXXX YYYY` | condicion: ... si la media palabra vale `YYYY` |
| `D2 XXXXXX 00YY` | condicion: ... si el byte NO vale `YY` |
| `D3 XXXXXX YYYY` | condicion: ... si la media palabra NO vale `YYYY` |
| `50 00CCII 0000VV` | repetidor: la linea siguiente se aplica `CC` veces, sumando `II` a la direccion y `VV` al valor |

La condicion gobierna **una** linea (y si esa linea lleva prefijo repetidor, el par entero).
Las lecturas de las condiciones se hacen por la vista coherente de la CPU y **no tocan la
cache**: no rellenan ni desalojan nada, para que armar un truco no cambie el estado del
juego mas alla de las escrituras que el propio codigo pide.

## Lo que NO se aplica, y se dice al cargar

- `88` / `89`: escribir solo mientras se tiene pulsado el boton fisico del propio cartucho.
  Esta maquina no tiene ese boton; fingir que esta siempre pulsado convertiria un truco
  opcional en permanente.
- `CC` (ventana de RAM ampliada), `DE` (direccion de arranque), `EE` / `FF` (control de la
  lista): hablan del hardware del aparato, no del juego.

Al cargar, el emulador imprime cuantos trucos hay, cuantos encendidos, cuantas lineas sin
efecto y cuantas ilegibles.

## Verificacion

`cheat_test` (`cmake --build build --target cheat_test`) monta un `Memory` + `CPU` de verdad
y ejercita parser, familias, condiciones, repetidor, truco apagado y la asimetria
cacheada/sin cache. End-to-end: con `80700000 0064` + `A0700010 0077` sobre SM64, la RDRAM
queda con `64` en `0x700000` (reescrito en cada campo) y `77` en `0x700010` (una sola vez).
