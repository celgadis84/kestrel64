#pragma once
// kestrel64 -- contenedores comprimidos de ROM (.zip, .gz) desempaquetados en memoria.
//
// Las ROMs circulan casi siempre dentro de un zip, y hasta ahora habia que descomprimirlas a
// mano antes de arrastrarlas al emulador. Aqui va ese desempaquetado, con su propio DEFLATE
// (RFC 1951) en vez de una dependencia nueva: son ~200 lineas, el formato lleva treinta anos
// congelado, y el .exe que se publica es autocontenido -- meter zlib en el enlace estatico por
// un descompresor que corre UNA vez al abrir la ROM no sale a cuenta.
//
// Lo que NO se soporta: .7z y .rar (LZMA / algoritmos propietarios, otro orden de magnitud de
// codigo). Se detectan por su firma y se dice con nombre y apellidos, en vez de fallar con un
// "magic no reconocido" que hace pensar que la ROM esta rota.

#include "types.hpp"
#include <string>
#include <vector>

namespace kestrel::archive {

// CRC-32 (el de zip/gzip). Publico porque los dos contenedores lo traen y se comprueba.
auto crc32(const u8* p, usize n) -> u32;

// DEFLATE crudo, sin cabecera. `out` se rellena desde donde este.
auto inflate(const u8* in, usize n, std::vector<u8>& out, std::string& error) -> bool;

// Si `data` es un contenedor conocido, lo sustituye por la ROM que lleva dentro y devuelve
// true dejando en `what` lo que se hizo (para el log). Si no lo es, devuelve true sin tocar
// nada y con `what` vacio. Devuelve false solo cuando SI era un contenedor y algo fallo.
auto unwrap(std::vector<u8>& data, std::string& what, std::string& error) -> bool;

// Quita una extension de contenedor (".zip" / ".gz") del final de una ruta. Los ficheros que
// viven al lado de la ROM (.eep/.sra/.fla, .st0-.st9, .cht) cuelgan del nombre de la ROM, y
// asi "mario.z64.gz" y "mario.z64" comparten partida guardada en vez de tener cada uno la
// suya segun con cual se arranco ese dia.
auto stripContainerExt(const std::string& path) -> std::string;

}  // namespace kestrel::archive
