#pragma once
// kestrel64 -- tabla de simbolos del INVITADO sacada de un map de GNU ld (p.ej. el pd.map
// de un build de Perfect Dark). La usan prof.cpu, cpu.disasm y sym.lookup para devolver
// `funcion+off` en vez de direcciones peladas (PD64_pending P5).
//
// Solo se fia de un simbolo para CODIGO si la direccion cae dentro de un tramo .text de
// entrada del map: asi una PC en el heap no sale como "ultimo_simbolo_de_bss+0x3f000". Una
// funcion static (sin simbolo exportado) sale como `objeto.o+off`.

#include "../core/types.hpp"
#include <string>
#include <unordered_map>
#include <vector>

namespace kestrel {

struct CPU;

namespace telemetry {

struct Symbols {
  struct Hit { bool ok = false; std::string name; u32 addr = 0; u32 off = 0; };

  // Carga (reemplaza) desde un map de GNU ld. Devuelve false si no se pudo abrir o no
  // salio ningun simbolo; `err` dice por que.
  auto load(const std::string& path, std::string& err) -> bool;
  auto clear() -> void;
  auto loaded() const -> bool { return !syms.empty(); }
  auto count() const -> size_t { return syms.size(); }
  auto path() const -> const std::string& { return mapPath; }

  // Codigo: solo dentro de un .text; nombre del simbolo o del objeto si no hay.
  auto code(u32 va) const -> Hit;
  // Cualquier cosa (datos incluidos): simbolo mas cercano por debajo, hasta 1 MB.
  auto any(u32 va) const -> Hit;
  // Nombre -> direccion.
  auto find(const std::string& name, u32& va) const -> bool;
  // "nombre+0xoff" / "nombre" o vacio.
  static auto format(const Hit& h) -> std::string;

  // PC FISICO (lo que cuenta prof.cpu) -> simbolo de codigo. Prueba primero las paginas que
  // el TLB mapea sobre ese fisico (PD matching ejecuta el juego desde 0x7F000000) y despues
  // KSEG0.
  auto codePhys(const CPU& cpu, u32 phys, u32& va) const -> Hit;

private:
  struct Range { u32 lo, hi; std::string obj; };
  std::vector<Range> text;                          // ordenado por lo
  std::vector<std::pair<u32, std::string>> syms;    // ordenado por direccion
  std::unordered_map<std::string, u32> byName;
  std::string mapPath;
};

// Tabla unica del proceso: la carga `--symbols` en main y la orden `sym.load`.
auto globalSymbols() -> Symbols&;

}  // namespace telemetry
}  // namespace kestrel
