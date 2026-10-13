# SummerCart64 + tarjeta SD

Homebrew que usa **libcart** (ports PSX, reproductores, cargadores) busca un flashcart con
tarjeta SD. kestrel64 emula el **SummerCart64 v2** en la parte que libcart necesita, y por
defecto la tarjeta es **la carpeta donde esta la ROM**: el juego ve en la raiz de la SD los
ficheros que tiene al lado (p.ej. `ISS98/STREAM.BIN` junto a `iss98.z64`).

```
(sin variable)         tarjeta = carpeta de la ROM
KESTREL_SD=<carpeta>   otra carpeta del host vista como FAT32
KESTREL_SD=<fichero>   imagen de tarjeta (.img); lectura/escritura
KESTREL_SD=0           sin flashcart
```

Un juego que no abre la llave del SC64 no nota nada: los registros no responden y no se lee
nada del disco. La carpeta se recorre la primera vez que el juego inicia la tarjeta.

- **Carpeta**: se genera al vuelo un volumen FAT32 (sectores de 512 B, clusters de 4 KiB,
  nombres largos LFN, subcarpetas, sin MBR). No se copia nada: cada sector de datos se lee
  del fichero del host cuando se pide. Las escrituras del juego van a una capa en memoria y
  se pierden al cerrar. Ficheros de mas de 4 GiB se omiten (no caben en FAT32). El arbol es
  el del momento del montaje.
- **Imagen**: se abre `r+b` (o `rb` si es de solo lectura) y se lee/escribe tal cual.

Ejemplo (port PSX): `kestrel64.exe --run test_roms/psx/out/iss98.z64` encuentra
`ISS98/STREAM.BIN` y `ISS98/CDDA.BIN` sin configurar nada.

Limite: el estado del SC64 (llave, sector, buffer) no viaja en los savestates.

## Lo que se emula

Registros en `0x1FFF0000` (protocolo del firmware SC64 v2, tal como lo usa libcart):

| Dir | Lectura | Escritura |
|-----|---------|-----------|
| +0x00 | STATUS (bit 30 = error; nunca BUSY) | COMMAND |
| +0x04 | DATA0 | DATA0 |
| +0x08 | DATA1 | DATA1 |
| +0x0C | IDENTIFIER `SCv2` | - |
| +0x10 | - | KEY |

- Llave: `0`, `_UNL`, `OCK_` abre; `0xFFFFFFFF` cierra. Cerrado, solo KEY escucha; el resto
  del rango 0x1FD00000+ se queda como sin flashcart.
- Comandos: `c`/`C` (config; solo `ROM_WRITE` tiene efecto, `DD_MODE` y `SAVE_TYPE` dan 0),
  `i` (SD op: deinit, init, status, info, byteswap on/off), `I` (sector), `s`/`S` (leer/escribir
  N sectores hacia/desde el buffer `0x1FFE0000` de 8 KiB o la SDRAM `0x10000000`).
- La SDRAM es la propia ROM, que con `ROM_WRITE` crece a 64 MiB y el DMA del PI escribe en
  ella (`cart_card_rd_cart` deja ahi los sectores).
- Los comandos terminan en el acto (STATUS nunca muestra BUSY): instante valido para el HW,
  el software sondea hasta que baja.

Codigo: `src/core/sdcard.hpp` (tarjeta: imagen o FAT32 virtual), `Memory::sc*` y
`attachSdCard` en `src/core/memory.cpp` (registros, comandos, ramales en
`cartRead`/`cartWrite`/`piDma`).
