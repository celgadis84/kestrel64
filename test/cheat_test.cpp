// Standalone unit test for the GameShark cheat engine. Builds a real Memory + CPU (so the
// writes go down the same D-cache path a running game would see) and drives the parser and
// the per-field engine with the code families as they are published: repeated writes,
// boot-only writes, conditionals and the repeater prefix.
#include "../src/core/cheats.hpp"
#include "../src/core/memory.hpp"
#include "../src/cpu/cpu.hpp"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>

using namespace kestrel;

static int failures = 0;
static void chk(const char* what, u64 got, u64 exp) {
  bool ok = got == exp;
  std::printf("  %-34s got=0x%08llx exp=0x%08llx  %s\n", what, (unsigned long long)got,
              (unsigned long long)exp, ok ? "OK" : "*** FAIL ***");
  if(!ok) failures++;
}

// Direccion de trabajo: RDRAM alta, fuera de todo lo que un juego real toca al arrancar.
static constexpr u32 kBase = 0x0070'0000;

static auto writeCht(const std::string& body) -> std::string {
  std::string path = "cheat_test.cht";
  std::ofstream f(path, std::ios::trunc);
  f << body;
  return path;
}

int main() {
  auto mp = std::make_unique<Memory>();
  Memory& m = *mp;
  m.reset(/*expansionPak=*/true);
  auto cp = std::make_unique<CPU>();
  CPU& c = *cp;
  c.connect(&m);

  auto clear = [&](u32 phys, u32 n) { for(u32 i = 0; i < n; i++) c.pokePhysCoherent(phys + i, 1, 0); };

  std::printf("familias 80/81 (escritura cacheada en cada campo):\n");
  {
    Cheats ch;
    std::string err;
    char buf[256];
    std::snprintf(buf, sizeof buf, "[Prueba]\n80%06X 0064\n81%06X 1234\n", kBase, kBase + 4);
    chk("carga", ch.loadFile(writeCht(buf), err), 1);
    chk("armado", ch.enabled(), 1);
    ch.applyField(c);
    chk("byte", c.peekPhysCoherent(kBase, 1), 0x64);
    chk("media palabra", c.peekPhysCoherent(kBase + 4, 2), 0x1234);
    // Se reaplica cada campo: el juego pisa el valor y el truco lo vuelve a poner.
    c.pokePhysCoherent(kBase, 1, 0);
    ch.applyField(c);
    chk("reaplicado tras pisarlo", c.peekPhysCoherent(kBase, 1), 0x64);
    clear(kBase, 8);
  }

  std::printf("familias A0/A1 (parche unico de arranque, sin cache):\n");
  {
    // Zona propia: estas escrituras van SIN cache, y una linea sucia de la seccion anterior
    // las taparia en la vista de la CPU (que es justo lo que hace el VR4300).
    const u32 base = kBase + 0x100;
    Cheats ch;
    std::string err;
    char buf[256];
    std::snprintf(buf, sizeof buf, "[Arranque]\nA0%06X 0077\nA1%06X ABCD\n", base, base + 4);
    ch.loadFile(writeCht(buf), err);
    ch.applyField(c);
    chk("byte una vez", c.peekPhysCoherent(base, 1), 0x77);
    chk("media palabra una vez", c.peekPhysCoherent(base + 4, 2), 0xabcd);
    chk("llega a la RDRAM", m.rdram[base], 0x77);
    c.pokePhysCoherent(base, 1, 0);
    ch.applyField(c);
    chk("NO se reaplica", c.peekPhysCoherent(base, 1), 0x00);
    clear(base, 8);
  }

  std::printf("KSEG1 no invalida la linea cacheada (semantica VR4300):\n");
  {
    // Un store sin cache deja la linea de D-cache como estaba: la CPU sigue viendo lo viejo
    // hasta que alguien la invalide. Un truco A0 lo aprovecha porque corre en el arranque,
    // cuando esa linea todavia no existe; mezclarlo con un valor cacheado vivo NO deberia
    // "curarse" solo, y si algun dia se curase seria que el modelo de cache ha cambiado.
    const u32 base = kBase + 0x200;
    Cheats ch;
    std::string err;
    char buf[256];
    c.pokePhysCoherent(base, 1, 0x0a);            // el juego deja la linea sucia con 0x0a
    std::snprintf(buf, sizeof buf, "[Sin cache]\nA0%06X 0077\n", base);
    ch.loadFile(writeCht(buf), err);
    ch.applyField(c);
    chk("la RDRAM si cambia", m.rdram[base], 0x77);
    chk("la CPU sigue viendo lo suyo", c.peekPhysCoherent(base, 1), 0x0a);
    clear(base, 4);
  }

  std::printf("condiciones D0/D1/D2/D3:\n");
  {
    Cheats ch;
    std::string err;
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "[Condiciones]\n"
                  "D0%06X 0005\n80%06X 0011\n"     // se cumple -> escribe
                  "D0%06X 0099\n80%06X 0022\n"     // no se cumple -> se salta
                  "D2%06X 0099\n80%06X 0033\n"     // distinto de 0x99 -> escribe
                  "D1%06X 0005\n80%06X 0044\n",    // media palabra 0x0005 != 0x0500 -> se salta
                  kBase, kBase + 1, kBase, kBase + 2, kBase, kBase + 3, kBase, kBase + 5);
    ch.loadFile(writeCht(buf), err);
    c.pokePhysCoherent(kBase, 1, 0x05);
    ch.applyField(c);
    chk("condicion cierta escribe", c.peekPhysCoherent(kBase + 1, 1), 0x11);
    chk("condicion falsa NO escribe", c.peekPhysCoherent(kBase + 2, 1), 0x00);
    chk("distinto-de cierto escribe", c.peekPhysCoherent(kBase + 3, 1), 0x33);
    chk("media palabra no casa", c.peekPhysCoherent(kBase + 5, 1), 0x00);
    clear(kBase, 8);
  }

  std::printf("repetidor 50 (tablas de items):\n");
  {
    Cheats ch;
    std::string err;
    char buf[256];
    // 4 repeticiones, +2 a la direccion y +1 al valor en cada vuelta.
    std::snprintf(buf, sizeof buf, "[Repetidor]\n50000402 00000001\n80%06X 0010\n", kBase);
    ch.loadFile(writeCht(buf), err);
    ch.applyField(c);
    chk("vuelta 0", c.peekPhysCoherent(kBase + 0, 1), 0x10);
    chk("vuelta 1", c.peekPhysCoherent(kBase + 2, 1), 0x11);
    chk("vuelta 2", c.peekPhysCoherent(kBase + 4, 1), 0x12);
    chk("vuelta 3", c.peekPhysCoherent(kBase + 6, 1), 0x13);
    chk("no desborda a la 5a", c.peekPhysCoherent(kBase + 8, 1), 0x00);
    chk("hueco intermedio intacto", c.peekPhysCoherent(kBase + 1, 1), 0x00);
    clear(kBase, 10);
  }

  std::printf("truco apagado y lineas sin efecto:\n");
  {
    Cheats ch;
    std::string err;
    char buf[256];
    std::snprintf(buf, sizeof buf, "[-Apagado]\n80%06X 0055\n", kBase);
    ch.loadFile(writeCht(buf), err);
    chk("no armado", ch.enabled(), 0);
    ch.applyField(c);
    chk("no escribe", c.peekPhysCoherent(kBase, 1), 0x00);

    Cheats ch2;
    std::snprintf(buf, sizeof buf, "[Boton GS]\n88%06X 0055\nFF000000 0000\n", kBase);
    ch2.loadFile(writeCht(buf), err);
    ch2.applyField(c);
    chk("88 (boton del cartucho) sin efecto", c.peekPhysCoherent(kBase, 1), 0x00);
    clear(kBase, 4);
  }

  std::printf("parser: comentarios, separadores y basura:\n");
  {
    Cheats ch;
    std::string err;
    char buf[512];
    std::snprintf(buf, sizeof buf,
                  "# comentario de cabecera\n"
                  "[Formatos]\n"
                  "80%06X 0021   ; valor con comentario al final\n"
                  "80%06X,0022\n"
                  "esto no es un codigo\n"
                  "80%06X:0023\n",
                  kBase, kBase + 1, kBase + 2);
    ch.loadFile(writeCht(buf), err);
    ch.applyField(c);
    chk("espacio", c.peekPhysCoherent(kBase + 0, 1), 0x21);
    chk("coma", c.peekPhysCoherent(kBase + 1, 1), 0x22);
    chk("dos puntos", c.peekPhysCoherent(kBase + 2, 1), 0x23);
    clear(kBase, 4);
  }

  std::remove("cheat_test.cht");
  std::printf("%s (%d fallos)\n", failures ? "*** HAY FALLOS ***" : "ALL PASS", failures);
  return failures ? 1 : 0;
}
