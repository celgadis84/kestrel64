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

  captureState(sys, scratch);
  if(!primed) { cur.swap(scratch); primed = true; return; }

  std::vector<u8> d;
  codec::makeDelta(scratch, cur, d);
  used += d.size();
  deltas.push_back(std::move(d));
  cur.swap(scratch);

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
