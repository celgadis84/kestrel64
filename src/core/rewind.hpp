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

// Diferencia EN CALIENTE: la misma diferencia de `nueva` a `vieja`, pero sin tener la foto
// nueva entera. `base` es la foto vieja; el estado vivo entra a trozos por feed() en el orden
// en que se serializa, se compara contra `base` en su posicion, lo distinto se apunta (bytes
// VIEJOS) y se reescribe sobre `base`. Al acabar, `base` ES la foto nueva y `out` la
// diferencia hacia atras. Ahorra la copia entera del estado (~16 MB) y la segunda pasada de
// makeDelta: una lectura del vivo, una de la vieja y solo se escribe lo que cambio.
//
// Los tramos no salen iguales que los de makeDelta (esa va por palabras alineadas, esta por
// bytes y funde huecos iguales de menos de kGap bytes dentro de un tramo distinto), pero el
// formato es el mismo y applyDelta no pide mas que tramos que cubran la foto vieja. Exige que
// el estado nuevo mida EXACTAMENTE lo que la vieja: el llamante lo comprueba antes, porque a
// mitad de camino `base` ya esta pisada y no se puede volver atras.
struct DeltaSink {
  static constexpr usize kGap = 16;   // hueco igual minimo que cierra un tramo distinto (2 cabeceras)
  u8* base = nullptr;
  usize baseLen = 0, pos = 0;
  std::vector<u8>* out = nullptr;
  usize same = 0, pendEq = 0;
  std::vector<u8> diff;               // bytes viejos del tramo distinto abierto
  bool overflow = false;

  auto begin(std::vector<u8>& vieja, std::vector<u8>& o) -> void {
    base = vieja.data(); baseLen = vieja.size(); pos = 0; out = &o;
    same = 0; pendEq = 0; diff.clear(); overflow = false;
    o.clear(); putU64(o, baseLen);
  }
  auto pair() -> void {
    putU32(*out, (u32)same); putU32(*out, (u32)diff.size());
    out->insert(out->end(), diff.begin(), diff.end());
    diff.clear();
  }
  auto eq(usize n) -> void {
    if(diff.empty()) { same += n; return; }
    pendEq += n;
    if(pendEq >= kGap) { pair(); same = pendEq; pendEq = 0; }
  }
  // n bytes distintos en base+pos (ya se ha avanzado el eq anterior).
  auto ne(const u8* src, usize n) -> void {
    if(pendEq) { diff.insert(diff.end(), base + pos - pendEq, base + pos); pendEq = 0; }
    diff.insert(diff.end(), base + pos, base + pos + n);
    std::memcpy(base + pos, src, n);
  }
  auto feed(const void* p, usize n) -> void {
    if(overflow || pos + n > baseLen) { overflow = true; return; }
    const u8* s = (const u8*)p;
    usize i = 0;
    while(i < n) {
      const u8* b = base + pos;
      usize j = i;
      while(j + 1024 <= n && std::memcmp(s + j, b + j - i, 1024) == 0) j += 1024;
      while(j + 32 <= n && std::memcmp(s + j, b + j - i, 32) == 0) j += 32;
      while(j + 8 <= n && std::memcmp(s + j, b + j - i, 8) == 0) j += 8;
      while(j < n && s[j] == b[j - i]) j++;
      if(j > i) { eq(j - i); pos += j - i; i = j; }
      if(i >= n) break;
      b = base + pos;
      usize k = i + 1;
      while(k < n && s[k] != b[k - i]) k++;
      ne(s + i, k - i); pos += k - i; i = k;
    }
  }
  auto finish() -> bool {
    if(overflow || pos != baseLen) return false;
    if(!diff.empty()) {
      pair();
      if(pendEq) { same = pendEq; pair(); }
    } else if(same) pair();
    return true;
  }
};

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
  codec::DeltaSink sink;           // diferencia en caliente, reutilizada
  auto push(std::vector<u8>&& d) -> void;
};

}  // namespace rewind
}  // namespace kestrel
