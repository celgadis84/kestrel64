#pragma once
// Rebobinado (rewind): deshacer lo que acaba de pasar.
//
// El emulador ya sabe fotografiar la maquina entera (savestate.hpp). Rebobinar es tener esas
// fotos hechas de antemano, una cada pocos campos de video, y volver a la anterior cuando el
// jugador lo pide. Lo caro es la RDRAM: una foto son ~8 MB, o sea que guardarlas enteras a
// treinta por segundo se come un gigabyte en cuatro segundos.
//
// Por eso NO se guardan enteras. Se guarda una sola foto viva (la mas reciente) y, detras,
// una pila de DIFERENCIAS HACIA ATRAS: cada entrada dice lo que hay que reescribir sobre la
// foto de ahora para que vuelva a ser la de antes. Es la direccion util -- rebobinar recorre
// la pila del final al principio -- y ademas es la barata, porque entre dos campos
// consecutivos un juego toca una porcion pequena de la RDRAM (el framebuffer que dibuja y
// sus estructuras vivas), no los 8 MB.
//
// Formato de una diferencia (va de la foto NUEVA a la VIEJA):
//
//   u64 tamano de la foto vieja
//   registros hasta cubrirla:  u32 iguales · u32 distintos · bytes de la vieja
//
// "iguales" se copian de la foto nueva en la misma posicion; "distintos" van literales. La
// comparacion se hace por palabras de 4 bytes, que es como esta escrito el estado, y el
// tramo final que la foto nueva no cubre (si la vieja era mas larga) va entero literal.
//
// El coste por foto es un recorrido del estado + una pasada de comparacion sobre el, o sea
// dos lecturas lineales de RDRAM. Va en el hilo de la CPU, en el mismo sitio donde se toma
// un estado guardado (RCP en reposo, coreMutex cogido), porque una foto tomada con el RDP a
// medias no vale para volver a ella.
//
// Apagado (por defecto) no cuesta nada: una comparacion contra cero al cerrar cada campo.

#include "types.hpp"

#include <cstring>
#include <deque>
#include <string>
#include <vector>

namespace kestrel {

struct System;

namespace rewind {

// El codec de diferencias vive aqui, y no en el .cpp, para que se pueda probar suelto:
// test/rewind_test.cpp lo machaca con casos limite sin arrastrar medio emulador.
namespace codec {

inline auto putU32(std::vector<u8>& v, u32 x) -> void {
  v.push_back((u8)x); v.push_back((u8)(x >> 8)); v.push_back((u8)(x >> 16)); v.push_back((u8)(x >> 24));
}
inline auto putU64(std::vector<u8>& v, u64 x) -> void { putU32(v, (u32)x); putU32(v, (u32)(x >> 32)); }
inline auto getU32(const u8* p) -> u32 { return (u32)p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
inline auto getU64(const u8* p) -> u64 { return (u64)getU32(p) | ((u64)getU32(p + 4) << 32); }

// Diferencia de `nueva` a `vieja`: lo que hay que reescribir sobre la nueva para recuperar
// la vieja. La comparacion va por palabras de 4 bytes porque el estado esta escrito asi
// (registros, longitudes, RDRAM); comparar byte a byte encontraria los mismos tramos y
// costaria cuatro veces mas vueltas de bucle.
inline auto makeDelta(const std::vector<u8>& nueva, const std::vector<u8>& vieja, std::vector<u8>& out) -> void {
  out.clear();
  putU64(out, vieja.size());
  const usize common = nueva.size() < vieja.size() ? nueva.size() : vieja.size();
  const usize words = common / 4;
  usize i = 0;                       // posicion en la foto vieja, en bytes
  while(i < vieja.size()) {
    // Tramo igual.
    usize same = 0;
    while(i + same + 4 <= words * 4
          && std::memcmp(nueva.data() + i + same, vieja.data() + i + same, 4) == 0) same += 4;
    // Tramo distinto: hasta la siguiente palabra igual, o hasta el final de la vieja.
    usize j = i + same;
    usize diff = 0;
    while(j + diff < vieja.size()) {
      if(j + diff + 4 <= words * 4
         && std::memcmp(nueva.data() + j + diff, vieja.data() + j + diff, 4) == 0) break;
      // Fuera de la parte comun todo es literal; dentro, se avanza de palabra en palabra
      // salvo el ultimo cacho suelto.
      usize adv = (j + diff + 4 <= words * 4) ? 4 : (vieja.size() - (j + diff));
      diff += adv;
    }
    putU32(out, (u32)same);
    putU32(out, (u32)diff);
    if(diff) out.insert(out.end(), vieja.data() + j, vieja.data() + j + diff);
    i = j + diff;
  }
}

// Aplica la diferencia sobre `nueva` y deja la foto vieja en `dst`. false si el registro no
// cuadra: mejor negarse a rebobinar que meter en la maquina un estado a medias.
inline auto applyDelta(const std::vector<u8>& nueva, const std::vector<u8>& d, std::vector<u8>& dst) -> bool {
  if(d.size() < 8) return false;
  const u64 oldLen = getU64(d.data());
  if(oldLen > ((u64)1 << 31)) return false;
  dst.clear();
  dst.resize((usize)oldLen);
  usize p = 8, i = 0;
  while(i < dst.size()) {
    if(p + 8 > d.size()) return false;
    const u32 same = getU32(d.data() + p), diff = getU32(d.data() + p + 4);
    p += 8;
    if((u64)i + same > dst.size() || (u64)i + same > nueva.size()) return false;
    if(same) std::memcpy(dst.data() + i, nueva.data() + i, same);
    i += same;
    if((u64)i + diff > dst.size() || p + diff > d.size()) return false;
    if(diff) std::memcpy(dst.data() + i, d.data() + p, diff);
    p += diff;
    i += diff;
    if(same == 0 && diff == 0) return false;   // registro vacio: no avanza, seria bucle
  }
  return i == dst.size();
}

}  // namespace codec


struct Engine {
  bool enabled = false;
  u32  interval = 2;        // campos de video entre foto y foto
  u64  budget = 0;          // tope de memoria en bytes (0 = apagado)

  // Lee KESTREL_REWIND / KESTREL_REWIND_MB / KESTREL_REWIND_FIELDS. Una vez, al arrancar.
  auto init() -> void;

  // Al cerrar un campo de video, con el RCP en reposo y coreMutex cogido.
  auto onField(System& sys) -> void;

  // Un paso atras. false con `err` puesto si no queda cinta o el estado no se pudo meter.
  auto stepBack(System& sys, std::string& err) -> bool;

  // Tras cargar un estado o reiniciar: la cinta describe otra partida.
  auto clear() -> void;

  auto steps() const -> usize { return deltas.size(); }
  auto bytes() const -> u64 { return used; }

 private:
  std::vector<u8> cur;             // la foto mas reciente, entera
  std::vector<u8> scratch;         // foto de trabajo, reutilizada
  std::deque<std::vector<u8>> deltas;   // diferencias hacia atras, la ultima es la mas nueva
  u64 used = 0;                    // bytes que ocupan las diferencias
  u32 sinceLast = 0;               // campos desde la ultima foto
  bool primed = false;             // ya hay foto viva
};

}  // namespace rewind
}  // namespace kestrel
