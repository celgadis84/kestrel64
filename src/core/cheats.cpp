#include "archive.hpp"
#include "cheats.hpp"
#include "memory.hpp"
#include "wrtag.hpp"
#include "../cpu/cpu.hpp"
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>

namespace kestrel {

namespace {

auto trim(std::string s) -> std::string {
  usize a = 0, b = s.size();
  while(a < b && std::isspace((unsigned char)s[a])) a++;
  while(b > a && std::isspace((unsigned char)s[b - 1])) b--;
  return s.substr(a, b - a);
}

auto hex(const std::string& t, u32& out) -> bool {
  if(t.empty()) return false;
  char* end = nullptr;
  unsigned long v = std::strtoul(t.c_str(), &end, 16);
  if(!end || *end) return false;
  out = (u32)v;
  return true;
}

// El nibble alto dice si la escritura llevaba cache: 8 = KSEG0 (por la D-cache),
// A = KSEG1 (a la RDRAM, sin tocar la cache, que es lo que hace el hardware).
constexpr u32 kRdramMask = 0x00ff'ffff;

}  // namespace

auto Cheats::loadFile(const std::string& path, std::string& error) -> bool {
  std::ifstream f(path);
  if(!f) { error = "no se puede abrir " + path; return false; }
  list.clear();
  bootDone = false;
  u32 unsupported = 0, badLines = 0;
  std::string ln;
  while(std::getline(f, ln)) {
    // Un comentario puede ir al final de una linea de codigo.
    for(usize i = 0; i < ln.size(); i++)
      if(ln[i] == '#' || ln[i] == ';') { ln = ln.substr(0, i); break; }
    ln = trim(ln);
    if(ln.empty()) continue;
    if(ln.front() == '[') {
      usize e = ln.find(']');
      std::string name = trim(ln.substr(1, e == std::string::npos ? std::string::npos : e - 1));
      Entry en;
      if(!name.empty() && name.front() == '-') { en.on = false; name = trim(name.substr(1)); }
      en.name = name.empty() ? "sin nombre" : name;
      list.push_back(std::move(en));
      continue;
    }
    // Linea de codigo: dos numeros hexadecimales. Se aceptan los separadores con los que
    // circulan publicados (espacio, tabulador, coma, dos puntos).
    for(auto& c : ln) if(c == ',' || c == ':' || c == '\t') c = ' ';
    std::istringstream is(ln);
    std::string ta, tv;
    u32 a = 0, v = 0;
    if(!(is >> ta >> tv) || !hex(ta, a) || !hex(tv, v)) { badLines++; continue; }
    if(list.empty()) { Entry en; en.name = "sin nombre"; list.push_back(std::move(en)); }
    const u32 t = a >> 24;
    if(t == 0x88 || t == 0x89 || t == 0xcc || t == 0xde || t == 0xee || t == 0xff) unsupported++;
    list.back().lines.push_back(Line{a, v});
  }
  armed = false;
  u32 on = 0;
  for(auto& e : list) if(e.on && !e.lines.empty()) { armed = true; on++; }
  std::printf("[cheats] \"%s\": %u trucos (%u encendidos, %u lineas sin efecto, %u ilegibles)\n",
              path.c_str(), (unsigned)list.size(), on, unsupported, badLines);
  if(unsupported)
    std::printf("[cheats] las familias 88/89 (boton del propio GameShark) y CC/DE/EE/FF"
                " (control interno del cartucho) no se aplican\n");
  return true;
}

auto Cheats::loadForRom(const std::string& romPath0) -> void {
  std::string path;
  if(const char* c = std::getenv("KESTREL_CHEATS")) { if(*c) path = c; }
  if(path.empty()) {
    const std::string romPath = archive::stripContainerExt(romPath0);   // .zip/.gz no cuentan
    usize dot = romPath.find_last_of('.');
    usize sep = romPath.find_last_of("/\\");
    if(dot != std::string::npos && (sep == std::string::npos || dot > sep)) path = romPath.substr(0, dot) + ".cht";
    else path = romPath + ".cht";
    std::ifstream probe(path);
    if(!probe) return;    // lo normal: no hay trucos, y no se dice nada
  }
  std::string err;
  if(!loadFile(path, err)) std::printf("[cheats] %s\n", err.c_str());
}

auto Cheats::applyField(CPU& cpu) -> void {
  if(!armed) return;
  if(!bootDone) {
    for(const auto& e : list) if(e.on) run(cpu, e, Phase::Boot);
    bootDone = true;
  }
  for(const auto& e : list) if(e.on) run(cpu, e, Phase::Frame);
}

auto Cheats::run(CPU& cpu, const Entry& e, Phase ph) -> void {
  Memory* mem = cpu.mem;
  if(!mem) return;
  const u32 ramSz = (u32)mem->rdram.size();

  // Una escritura del motor de trucos. `cached` = la publico una direccion 0x8xxxxxxx, o sea
  // que pasa por la D-cache de la CPU y el juego la ve aunque la linea tarde en bajar a la
  // RDRAM; sin cache va directa a la RDRAM y deja la linea de cache como estuviera, que es
  // exactamente lo que hace un store KSEG1 en el VR4300.
  auto write = [&](u32 phys, u32 size, u32 val, bool cached) {
    if(phys + size > ramSz) return;
    if(cached) { cpu.pokePhysCoherent(phys, size, val); return; }
    wrtag::markRange(phys, size, wrtag::kCpu, (u32)cpu.pc);
    if(mem->watchAddr) mem->watchHit(phys, size, val, false);
    for(u32 i = 0; i < size; i++) mem->rdram[phys + i] = (u8)(val >> (8 * (size - 1 - i)));
  };
  // La condicion lee lo que ve la CPU (por la D-cache): el motor comparaba contra el valor
  // vivo del juego, no contra lo que hubiera bajado ya a la RDRAM.
  auto read = [&](u32 phys, u32 size) -> u32 {
    if(phys + size > ramSz) return 0;
    return (u32)cpu.peekPhysCoherent(phys, size);
  };

  u32 rep = 0, repAddr = 0, repVal = 0;   // repetidor 50 pendiente para la linea siguiente
  bool skipNext = false;                  // condicion D0..D3 que fallo

  for(const Line& l : e.lines) {
    const u32 t = l.addr >> 24;
    const u32 phys = l.addr & kRdramMask;

    if(t == 0x50) {   // prefijo repetidor: describe a la linea siguiente, no escribe nada
      if(skipNext) { skipNext = false; continue; }   // la condicion se salta el par entero
      rep = (l.addr >> 8) & 0xff;
      repAddr = l.addr & 0xff;
      repVal = l.val & 0xffff;
      if(rep == 0) rep = 1;
      continue;
    }
    if(skipNext) { skipNext = false; rep = 0; continue; }

    switch(t) {
      // Condiciones. Se evaluan en las dos fases: son la guarda de la linea siguiente, y
      // esa linea puede ser tanto de arranque como de cada campo.
      case 0xd0: skipNext = read(phys, 1) != (l.val & 0xff);   break;
      case 0xd1: skipNext = read(phys, 2) != (l.val & 0xffff); break;
      case 0xd2: skipNext = read(phys, 1) == (l.val & 0xff);   break;
      case 0xd3: skipNext = read(phys, 2) == (l.val & 0xffff); break;

      case 0x80: case 0x81: case 0xa0: case 0xa1: {
        const bool boot   = (t == 0xa0 || t == 0xa1);   // A0/A1 = parche unico de arranque
        const bool cached = (t == 0x80 || t == 0x81);   // 8x = KSEG0, Ax = KSEG1
        const u32  size   = (t == 0x81 || t == 0xa1) ? 2u : 1u;
        if(boot != (ph == Phase::Boot)) break;          // cada familia en su fase
        const u32 mask = size == 1 ? 0xffu : 0xffffu;
        u32 a = phys, v = l.val & mask;
        for(u32 n = 0, k = rep ? rep : 1; n < k; n++) {
          write(a, size, v & mask, cached);
          a += repAddr;
          v += repVal;
        }
        break;
      }
      default: break;   // 88/89 y control interno del cartucho: sin efecto, avisado al cargar
    }
    rep = 0;
  }
}

}  // namespace kestrel
