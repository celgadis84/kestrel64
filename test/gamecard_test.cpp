// Test de la ficha de juego del exe (src/ui/gamecard.cpp). Es el mismo recorrido que
// tools/launcher/gamecard_test.py hace sobre gamecard.py: las dos caras del programa leen y
// escriben los MISMOS ficheros, asi que tienen que comportarse igual.
//
// Trabaja en una carpeta temporal propia; KESTREL_PROFILE apunta dentro para que los
// metadatos (<estado>/games/<CRC>.json) no toquen el perfil de verdad.

#include "ui/gamecard.hpp"
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace kestrel::ui::card;

static int fails = 0;

static void check(bool ok, const char* what) {
  if(!ok) { std::printf("FALLO: %s\n", what); fails++; }
}

static void put(const fs::path& p, const std::string& data) {
  std::ofstream f(p, std::ios::binary);
  f << data;
}

static auto get(const fs::path& p) -> std::string {
  std::ifstream f(p, std::ios::binary);
  return std::string((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static auto endsWith(const std::string& s, const std::string& t) -> bool {
  return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0;
}

static auto names(const std::vector<Cheat>& c) -> std::string {
  std::string o;
  for(auto& x : c) o += x.name + "|";
  return o;
}

static auto ons(const std::vector<Cheat>& c) -> std::string {
  std::string o;
  for(auto& x : c) o += x.on ? '1' : '0';
  return o;
}

static void run(const fs::path& d) {
  const std::string ds = d.string();
  std::string err;

  // Nombres = los del emulador: sin contenedor y sin extension
  const std::string rom = (d / "Space Demo 64 (USA).z64.zip").string();
  put(rom, "x");
  check(endsWith(romBase(rom), "Space Demo 64 (USA)"), "romBase quita .zip y .z64");
  check(endsWith(romBase((d / "Game.zip").string()), "Game"), "romBase .zip");
  check(endsWith(romBase((d / "a.b" / "Game").string()), "a.b/Game") ||
        endsWith(romBase((d / "a.b" / "Game").string()), "a.b\\Game"), "romBase punto en carpeta");
  check(baseName(rom) == "Space Demo 64 (USA).z64.zip", "baseName");

  // Partidas y estados
  put(d / "Space Demo 64 (USA).eep", std::string(512, '\0'));
  put(d / "Space Demo 64 (USA).st1", std::string(9, '\0'));
  auto s = saves(rom);
  check(s.size() == 2 && s[0].kind == "eep" && s[1].kind == "st1", "saves eep+st1");
  check(s.size() == 2 && s[0].size == 512, "saves tamano");

  // Trucos: mismo lector que src/core/cheats.cpp, reescritura que respeta el fichero
  const fs::path cht = d / "Space Demo 64 (USA).cht";
  put(cht, "# demo\r\n8033B21D 0064\r\n[Vidas infinitas] ; nota\r\n"
           "8033B21D 0064\r\n\r\n# otro\r\n[-Monedas]\r\n8033B21A:0063\r\nzz\r\n");
  check(chtPath(rom) == romBase(rom) + ".cht", "chtPath");
  auto c = cheats(rom);
  check(names(c) == "sin nombre|Vidas infinitas|Monedas|", "cht nombres");
  check(ons(c) == "110" && c.size() == 3 && c[2].bad == 1, "cht estado y lineas malas");
  check(c.size() == 3 && c[2].codes.size() == 1 && c[2].codes[0] == "8033B21A 0063", "cht codigo normalizado");
  check(setCheat(rom, 0, false, err), "setCheat 0");
  check(setCheat(rom, 1, false, err), "setCheat 1");
  check(setCheat(rom, 2, true, err), "setCheat 2");
  std::string raw = get(cht);
  check(raw.find("[-Vidas infinitas] ; nota\r\n") != std::string::npos, "comentario conservado");
  check(raw.find("\r\n[Monedas]\r\n") != std::string::npos, "cabecera encendida");
  check(raw.find("[-sin nombre]\r\n8033B21D") != std::string::npos, "truco implicito con cabecera");
  {
    std::string t = raw;
    for(size_t p; (p = t.find("\r\n")) != std::string::npos;) t.erase(p, 2);
    check(t.find('\n') == std::string::npos, "CRLF conservado");
  }
  check(ons(cheats(rom)) == "001", "cht releido");
  check(addCheat(rom, "Nuevo [x]", "81000000 FFFF\n88000000 0001", true, err), "addCheat");
  c = cheats(rom);
  check(!c.empty() && c.back().name == "Nuevo x" && c.back().inert == 1 && c.back().codes.size() == 2,
        "addCheat nombre/inertes/codigos");
  check(!addCheat(rom, "", "80000000 0001", true, err), "addCheat sin nombre");
  check(!addCheat(rom, "x", "", true, err), "addCheat sin codigos");
  check(!addCheat(rom, "x", "hola", true, err), "addCheat codigo ilegible");
  check(delCheat(rom, 2, err), "delCheat");
  check(names(cheats(rom)) == "sin nombre|Vidas infinitas|Nuevo x|", "delCheat nombres");
  const std::string rom2 = (d / "Otro.n64").string();
  check(addCheat(rom2, "Primero", "80000000 0001", false, err), "addCheat fichero nuevo");
  c = cheats(rom2);
  check(c.size() == 1 && !c[0].on, "fichero nuevo apagado");

  // Manuales: por nombre de fichero o nombre interno, carpeta de la ROM o manuals/
  fs::create_directories(d / "manuals");
  put(d / "manuals" / "Space Demo 64 (USA) - manual.pdf", "%PDF");
  put(d / "SPACEDEMO64.txt", "hi");
  put(d / "Other.pdf", "%PDF");
  put(d / "Space Demo 64 (USA).exe", "MZ");
  auto m = manuals(rom, "SPACE DEMO 64");
  check(m.size() == 2 && baseName(m[0]) == "SPACEDEMO64.txt" &&
        baseName(m[1]) == "Space Demo 64 (USA) - manual.pdf", "manuales");
  put(d / "x.pdf", "%PDF2");
  put(d / "y.PDF", "%PDF3");
  put(d / "x.exe", "MZ");
  put(d / "empty.pdf", "");
  std::string p = manualStore(rom, (d / "x.pdf").string(), err);
  check(endsWith(p, "Space Demo 64 (USA).pdf") && p.find("manuals") != std::string::npos, "manualStore");
  p = manualStore(rom, (d / "y.PDF").string(), err);
  check(endsWith(p, "Space Demo 64 (USA) (2).pdf"), "manualStore (2)");
  check(manualStore(rom, (d / "x.exe").string(), err).empty(), "manualStore rechaza .exe");
  check(manualStore(rom, (d / "empty.pdf").string(), err).empty(), "manualStore rechaza vacio");

  // Indice de una fuente de manuales (formato external_files.csv de MiSTer): cabecera,
  // comillas con coma dentro, extensiones no admitidas fuera, etiquetas (...) ignoradas.
  auto idx = manualIndexParse(
      "Path,URL,Size,MD5,Tags,Overwrite,Comment\r\n"
      "docs/N64/Manuals/Space Demo 64.pdf,https://ejemplo.invalid/a.pdf,4,abc,,,\r\n"
      "\"docs/N64/Manuals/Otro, El (Europe).pdf\",https://ejemplo.invalid/b.pdf,5,,,,\r\n"
      "docs/N64/Manuals/Space Demo 64 Extra.pdf,https://ejemplo.invalid/c.pdf,6,,,,\r\n"
      "docs/N64/Manuals/Spacedemo.exe,https://ejemplo.invalid/d.exe,7,,,,\r\n"
      "docs/N64/Manuals/SPACEDEMO64.pdf,https://ejemplo.invalid/e.pdf,,,,,\n");
  check(idx.size() == 4, "indice: 4 manuales validos");
  check(idx.size() == 4 && idx[1].path == "docs/N64/Manuals/Otro, El (Europe).pdf" && idx[1].size == 5,
        "indice: campo entre comillas");
  check(!idx.empty() && idx[0].md5 == "abc" && idx[0].size == 4, "indice: tamano y md5");
  auto hit = manualIndexMatch(idx, rom, "SPACE DEMO 64");
  check(hit.size() == 2 && hit[0].url.find("a.pdf") != std::string::npos &&
        hit[1].url.find("e.pdf") != std::string::npos, "indice: mejor nivel (nombre de fichero)");
  hit = manualIndexMatch(idx, (d / "Otro, El (USA) [!].z64").string(), "");
  check(hit.size() == 1 && hit[0].url.find("b.pdf") != std::string::npos, "indice: region distinta");
  check(manualIndexMatch(idx, (d / "Nada.z64").string(), "").empty(), "indice: sin coincidencia");
  check(setManualSource(" https://ejemplo.invalid/idx.csv ") &&
        manualSource() == "https://ejemplo.invalid/idx.csv", "fuente de manuales");

  // JSON plano: claves desconocidas sobreviven, \u y pares sustitutos se decodifican
  Meta j;
  check(parseFlatJson("{\"year\": \"1996\", \"n\": 5, \"fav\": true, \"t\": \"a\\u00f1o \\ud83c\\udfae\","
                      " \"x\": [1, {\"y\": 2}]}", j), "parseFlatJson");
  check(j.str("year") == "1996" && j.num("n") == 5 && j.flag("fav"), "json valores");
  check(j.str("t") == "a\xc3\xb1o \xf0\x9f\x8e\xae", "json unicode");
  Meta j2;
  check(parseFlatJson(writeFlatJson(j), j2) && j2.kv == j.kv, "json ida y vuelta");

  // Metadatos de la ficha
  Meta mm;
  mm.setStr("year", "1996");
  mm.setFlag("favorite", true);
  mm.kv.push_back({"bogus", "3"});
  check(metaSave("ABC", mm), "metaSave");
  metaPlayed("ABC", 100);
  metaPlayed("ABC", 1);   // menos de 3 s: no cuenta
  Meta r = metaLoad("ABC");
  check(r.str("year") == "1996" && r.flag("favorite") && r.num("plays") == 1, "meta partidas");
  check(r.num("seconds") == 100 && r.num("bogus") == 3, "meta segundos y clave ajena");
  check(fs::exists(d / "games" / "ABC.json"), "meta en <estado>/games");
  (void)ds;
}

int main() {
  fs::path d = fs::temp_directory_path() / "kestrel64_gamecard_test";
  std::error_code ec;
  fs::remove_all(d, ec);
  fs::create_directories(d);
  const std::string prof = (d / "profile.json").string();
#ifdef _WIN32
  _putenv_s("KESTREL_PROFILE", prof.c_str());
#else
  setenv("KESTREL_PROFILE", prof.c_str(), 1);
#endif
  run(d);
  fs::remove_all(d, ec);
  if(fails) { std::printf("gamecard_test: %d FALLOS\n", fails); return 1; }
  std::printf("gamecard_test: OK\n");
  return 0;
}
