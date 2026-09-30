// Rebobinado. Ver rewind.hpp para el formato y el porque.

#include "rewind.hpp"
#include "savestate.hpp"
#include "system.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace kestrel::rewind {

auto Engine::init() -> void {
  const char* on = std::getenv("KESTREL_REWIND");
  enabled = on && *on && std::strcmp(on, "0") != 0;
  if(!enabled) return;
  const char* mb = std::getenv("KESTREL_REWIND_MB");
  const char* iv = std::getenv("KESTREL_REWIND_FIELDS");
  u64 mbv = mb ? std::strtoull(mb, nullptr, 0) : 256;
  if(mbv < 8) mbv = 8;
  budget = mbv << 20;
  interval = iv ? (u32)std::strtoul(iv, nullptr, 0) : 2;
  if(interval == 0) interval = 1;
  std::printf("[rewind] activo: foto cada %u campos, tope %llu MB\n",
              interval, (unsigned long long)(budget >> 20));
  std::fflush(stdout);
}

auto Engine::clear() -> void {
  deltas.clear();
  cur.clear();
  used = 0;
  sinceLast = 0;
  primed = false;
}

auto Engine::onField(System& sys) -> void {
  if(!enabled) return;
  if(++sinceLast < interval) return;
  sinceLast = 0;

  static const bool rtt = [] { const char* e = std::getenv("KESTREL_REWIND_RTT");
                               return e && *e && std::strcmp(e, "0") != 0; }();
  static const bool noSink = [] { const char* e = std::getenv("KESTREL_REWIND_NOSINK");
                                  return e && *e && std::strcmp(e, "0") != 0; }();
  // Camino normal: diferencia en caliente, sin sacar la foto entera (ver codec::DeltaSink).
  // Medido en SM64: la foto entera + makeDelta eran 12,4 + 4,5 ms por foto. El RTT necesita
  // la foto entera para recargarla, y un estado que cambia de tamano (un vecBlob que crece)
  // cae al camino de siempre.
  if(primed && !rtt && !noSink) {
    std::vector<u8> d;
    sink.pos = 0;
    if(captureDelta(sys, sink, cur, d)) { push(std::move(d)); return; }
    if(sink.pos) { clear(); sinceLast = 0; }   // no deberia pasar: `cur` a medio pisar no vale
  }

  captureState(sys, scratch);
  // KESTREL_REWIND_RTT=1 (diagnostico): cada foto se vuelve a cargar al instante. Una maquina
  // determinista tiene que dar el mismo statehash que sin rebobinado: cualquier trozo de estado
  // de invitado que la foto no lleve (y que afterLoad reinicie) aparece como divergencia o
  // cuelgue en el primer campo en que importe.
  if(rtt) {
    std::string err;
    if(!restoreState(sys, scratch.data(), scratch.size(), err))
      std::fprintf(stderr, "[rewind] RTT: fallo al recargar la foto: %s\n", err.c_str());
  }
  if(!primed) { cur.swap(scratch); primed = true; return; }

  std::vector<u8> d;
  codec::makeDelta(scratch, cur, d);
  cur.swap(scratch);
  push(std::move(d));
}

auto Engine::push(std::vector<u8>&& d) -> void {
  used += d.size();
  deltas.push_back(std::move(d));
  // El tope se cobra por el extremo VIEJO: lo que se pierde al llenarse es el pasado
  // lejano, no el reciente, que es justo lo que se va a pedir.
  while(used > budget && !deltas.empty()) {
    used -= deltas.front().size();
    deltas.pop_front();
  }
}

auto Engine::stepBack(System& sys, std::string& err) -> bool {
  if(!enabled) { err = "el rebobinado no esta activo (KESTREL_REWIND=1)"; return false; }
  if(deltas.empty()) { err = "no queda cinta hacia atras"; return false; }
  if(!codec::applyDelta(cur, deltas.back(), scratch)) { err = "diferencia de rebobinado corrupta"; return false; }
  if(!restoreState(sys, scratch.data(), scratch.size(), err)) return false;
  used -= deltas.back().size();
  deltas.pop_back();
  cur.swap(scratch);
  // La foto a la que se acaba de volver es ahora la viva; el siguiente campo no debe tomar
  // otra al instante o la cinta se llenaria de fotos identicas al rebobinar sostenido.
  sinceLast = 0;
  return true;
}

}  // namespace kestrel::rewind
