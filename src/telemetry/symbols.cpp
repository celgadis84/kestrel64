// kestrel64 -- simbolos del invitado desde un map de GNU ld. Ver symbols.hpp.

#include "symbols.hpp"
#include "../cpu/cpu.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace kestrel::telemetry {

auto globalSymbols() -> Symbols& {
  static Symbols s;
  return s;
}

static auto isIdent(const std::string& t) -> bool {
  if(t.empty() || !(std::isalpha((u8)t[0]) || t[0] == '_' || t[0] == '.')) return false;
  for(char c : t) if(!(std::isalnum((u8)c) || c == '_' || c == '.' || c == '$')) return false;
  return true;
}

static auto isHex(const std::string& t, u32& v) -> bool {
  if(t.size() < 3 || t[0] != '0' || (t[1] != 'x' && t[1] != 'X')) return false;
  char* end = nullptr;
  unsigned long long x = std::strtoull(t.c_str() + 2, &end, 16);
  if(!end || *end) return false;
  v = (u32)x;   // ld de 64 bits escribe 0x0000000080001050: basta la mitad baja
  return true;
}

auto Symbols::clear() -> void {
  text.clear(); syms.clear(); byName.clear(); mapPath.clear();
}

auto Symbols::load(const std::string& p, std::string& err) -> bool {
  std::ifstream in(p);
  if(!in) { err = "cannot open " + p; return false; }
  clear();
  std::string line, pendingSec;
  bool inMap = false;                 // antes de "Linker script and memory map" van los
                                      // tramos DESCARTADOS, con direccion 0: fuera
  while(std::getline(in, line)) {
    if(!line.empty() && line.back() == '\r') line.pop_back();
    if(!inMap) { if(line.rfind("Linker script and memory map", 0) == 0) inMap = true; continue; }
    if(line.size() < 2 || line[0] != ' ') { pendingSec.clear(); continue; }  // secciones de salida
    std::istringstream ss(line);
    std::vector<std::string> tok;
    for(std::string t; ss >> t; ) tok.push_back(t);
    if(tok.empty()) continue;
    // Tramo de entrada: " .text  0xADDR  0xSIZE  objeto". Si el nombre es largo, ld lo
    // escribe solo en su linea y los numeros en la siguiente.
    if(tok[0][0] == '.' || !pendingSec.empty()) {
      std::string sec = pendingSec.empty() ? tok[0] : pendingSec;
      size_t base = pendingSec.empty() ? 1 : 0;
      pendingSec.clear();
      if(tok.size() == 1 && tok[0][0] == '.') { pendingSec = tok[0]; continue; }
      u32 a = 0, n = 0;
      if(tok.size() >= base + 3 && isHex(tok[base], a) && isHex(tok[base + 1], n)) {
        if(n && a && (sec == ".text" || sec.rfind(".text.", 0) == 0)) {
          std::string obj = tok[base + 2];
          size_t s = obj.find_last_of("/\\");
          if(s != std::string::npos) obj = obj.substr(s + 1);
          text.push_back({a, a + n, obj});
        }
      }
      continue;
    }
    // Simbolo: exactamente "0xADDR nombre". Las asignaciones del guion llevan '=' y mas tokens.
    u32 a = 0;
    if(tok.size() == 2 && isHex(tok[0], a) && isIdent(tok[1])) {
      syms.push_back({a, tok[1]});
      byName.emplace(tok[1], a);
    }
  }
  if(syms.empty()) { err = "no symbols in " + p; clear(); return false; }
  std::stable_sort(syms.begin(), syms.end(), [](auto& x, auto& y){ return x.first < y.first; });
  std::sort(text.begin(), text.end(), [](auto& x, auto& y){ return x.lo < y.lo; });
  mapPath = p;
  return true;
}

auto Symbols::any(u32 va) const -> Hit {
  Hit h;
  auto it = std::upper_bound(syms.begin(), syms.end(), va,
                             [](u32 v, auto& s){ return v < s.first; });
  if(it == syms.begin()) return h;
  --it;
  if(va - it->first >= 0x100000) return h;
  h.ok = true; h.name = it->second; h.addr = it->first; h.off = va - it->first;
  return h;
}

auto Symbols::code(u32 va) const -> Hit {
  Hit h;
  auto r = std::upper_bound(text.begin(), text.end(), va,
                            [](u32 v, auto& x){ return v < x.lo; });
  if(r == text.begin()) return h;
  --r;
  if(va >= r->hi) return h;
  h = any(va);
  if(!h.ok || h.addr < r->lo) {          // funcion static: solo sabemos el objeto
    h.ok = true; h.name = r->obj; h.addr = r->lo; h.off = va - r->lo;
  }
  return h;
}

auto Symbols::find(const std::string& name, u32& va) const -> bool {
  auto it = byName.find(name);
  if(it == byName.end()) return false;
  va = it->second;
  return true;
}

auto Symbols::format(const Hit& h) -> std::string {
  if(!h.ok) return {};
  if(!h.off) return h.name;
  char b[24];
  std::snprintf(b, sizeof b, "+0x%x", h.off);
  return h.name + b;
}

auto Symbols::codePhys(const CPU& cpu, u32 phys, u32& va) const -> Hit {
  for(const auto& e : cpu.tlb) {
    u32 m = (u32)e.mask & 0x01FF'E000;
    u32 page = ((m | 0x1FFF) + 1) >> 1;
    u32 vbase = (u32)e.hi & ~(m | 0x1FFF);
    for(int odd = 0; odd < 2; odd++) {
      u64 lo = odd ? e.lo1 : e.lo0;
      if(!(lo & 2)) continue;
      u32 pbase = (u32)((lo >> 6) & 0xFFFFF) << 12;
      if(phys < pbase || phys - pbase >= page) continue;
      u32 v = vbase + (odd ? page : 0) + (phys - pbase);
      Hit h = code(v);
      if(h.ok) { va = v; return h; }
    }
  }
  va = 0x8000'0000u | phys;
  return code(va);
}

}  // namespace kestrel::telemetry
