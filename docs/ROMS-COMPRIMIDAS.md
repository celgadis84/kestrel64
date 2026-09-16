# ROMs comprimidas (.zip, .gz)

Cerrado 2026-09-04. Hueco P1 #5 de `docs/GAPS.md`.

kestrel64 abre directamente una ROM metida en un **zip** o en un **gzip**, igual que se
descargó. No hay que descomprimir nada a mano ni configurar nada: se arrastra el `.zip` al
emulador, o se pasa por línea de órdenes, y arranca.

```
kestrel64.exe "Super Mario 64 (USA).zip" --run
kestrel64.exe mario.n64.gz
```

## Qué se soporta

| Contenedor | Firma | Métodos |
|---|---|---|
| zip | `PK\x03\x04` | 0 (guardado sin comprimir) y 8 (DEFLATE) |
| gzip | `\x1f\x8b` | DEFLATE, con FEXTRA / FNAME / FCOMMENT / FHCRC |
| 7z | `37 7A BC AF 27 1C` | **no** — se rechaza diciéndolo |
| rar | `Rar!` | **no** — se rechaza diciéndolo |

Dentro del zip se elige la entrada con extensión de ROM (`.z64 .n64 .v64 .rom .bin .u64
.ndd`) y, si hay varias o ninguna, la más grande — el reparto habitual de un zip de ROM es
la ROM más un `.txt` o un `.nfo`. El **CRC-32** que traen los dos formatos se comprueba
siempre: un fichero corrupto da un error claro en vez de una ROM que arranca a medias.

El desempaquetado ocurre **antes** de normalizar el orden de bytes (`Rom::loadFile`), así
que un `.v64` o un `.n64` dentro de un zip se reordena igual que si estuviera suelto.

## Por qué un DEFLATE propio y no zlib

`src/core/inflate` no existe: el descompresor está en **`src/core/archive.{hpp,cpp}`**, son
unas 200 líneas y no depende de nada. La alternativa era enlazar la zlib de MSYS2, pero el
`.exe` que se publica se enlaza con `-DKESTREL_STATIC=ON` para ser autocontenido
(`docs/distribucion.md`), y meter una dependencia externa por un descompresor que corre
**una vez** al abrir la ROM no sale a cuenta. El formato lleva treinta años congelado
(RFC 1951), así que no hay mantenimiento que temer.

El decodificador usa la forma compacta de la propia RFC — cuántos códigos hay de cada
longitud y los símbolos ordenados por (longitud, símbolo) — y decodifica recorriendo
longitudes, sin construir tablas de búsqueda. Para una ROM de 8 MB el coste es
inapreciable frente al arranque.

## Ficheros que viven al lado de la ROM

La partida guardada (`.eep`/`.sra`/`.fla`/`.mpk`), los estados (`.st0`..`.st9`) y los trucos
(`.cht`) cuelgan del nombre de la ROM **sin la extensión del contenedor**
(`archive::stripContainerExt`): `mario.z64.gz` y `mario.z64` comparten partida en vez de
tener cada uno la suya según con cuál se arrancó ese día.

## Verificación

- **`test/archive_test.cpp`** (objetivo `archive_test`, dentro de `gate_all.sh`): vectores
  generados con zlib/zipfile e incrustados. Cubre las tres clases de bloque de DEFLATE
  (crudo, árbol fijo, árbol dinámico), la copia solapada de distancia 1, el flujo truncado,
  el CRC-32, gzip con las tres banderas opcionales, la elección de entrada en un zip con
  tres ficheros, zip guardado, CRC roto en ambos formatos, método 12 no soportado, las
  firmas de 7z y rar, la ROM cruda que no debe tocarse y `stripContainerExt`.
- **Extremo a extremo**: SM64 arrancado 60 campos de VI desde siete envoltorios distintos
  (zip deflate, zip guardado, zip con tres entradas, zip con la ROM en `.v64`, gzip de un
  `.n64`, gzip de un `.z64`, y el `.n64` crudo). Los siete dan el md5 del oráculo,
  `466282775dbd0ac084946558a1c30771`.
- El lanzador (`tools/launcher/kestrel_launcher.py`) lee la cabecera **dentro** del
  contenedor, así que la lista de ROMs enseña el nombre, la región y el CRC del juego y no
  un "formato desconocido".
