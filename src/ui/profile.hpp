#pragma once
// kestrel64 -- perfil de opciones persistente, COMPARTIDO con el lanzador.
//
// El lanzador guarda su configuracion en %LOCALAPPDATA%\kestrel64\profile.json (un objeto
// plano id -> valor, mas un objeto "pad" con el mapeo del mando). El menu de la ventana lee
// y escribe EXACTAMENTE ese fichero, asi que cambiar la resolucion desde dentro del juego se
// ve luego en el lanzador y al reves. Por eso hay aqui un lector/escritor de JSON minimo en
// vez de una dependencia: el formato que hay que entender son cuatro tipos de valor.

#include "optdefs.hpp"
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace kestrel::ui {

struct Profile {
  std::map<std::string, std::string> v;                       // id -> valor en texto
  // Un mapeo por conector. El 0 se guarda con la clave "pad" para que el lanzador (que
  // solo conoce un mando) siga leyendo y escribiendo el suyo; los otros tres van en
  // "pad2".."pad4" y el lanzador los conserva sin tocarlos.
  std::map<std::string, std::pair<std::string, std::string>> pad[4];  // id -> (tecla, boton)

  auto get(const char* id) const -> std::string;
  auto getBool(const char* id) const -> bool;
  auto set(const char* id, const std::string& val) -> void { v[id] = val; }
};

// Ruta del perfil compartido (crea el directorio si hace falta).
auto profilePath() -> std::string;
// Valores de fabrica sacados del catalogo.
auto defaultProfile() -> Profile;
// Carga el perfil del disco sobre los valores de fabrica. Nunca falla: si no hay fichero o
// esta roto, devuelve los valores de fabrica.
auto loadProfile() -> Profile;
auto saveProfile(const Profile& p) -> bool;

// Traduce el perfil a variables de entorno con las MISMAS reglas que el lanzador
// (tools/launcher/options.py:to_env). Devuelve tambien los argumentos extra.
auto toEnv(const Profile& p, std::vector<std::pair<std::string, std::string>>& env,
           std::vector<std::string>& argv) -> void;

// Escribe el fichero de mapeo de un conector y devuelve su ruta (vacio si no hay mapeo).
auto writePadFile(const Profile& p, int port) -> std::string;

// Ajustes por conector. No estan en el catalogo de opciones (no son opciones de linea de
// ordenes), asi que sus valores de fabrica viven aqui: solo el mando 1 viene enchufado, y
// con Controller Pak, que es lo que hacia el emulador cuando solo habia un puerto.
auto padOn(const Profile& p, int port) -> bool;
auto padAcc(const Profile& p, int port) -> int;          // 0 nada, 1 Controller Pak, 2 Rumble
auto padDevice(const Profile& p, int port) -> std::string;  // "auto", "kb" o nombre del mando
auto setPadOn(Profile& p, int port, bool on) -> void;
auto setPadAcc(Profile& p, int port, int acc) -> void;
auto setPadDevice(Profile& p, int port, const std::string& dev) -> void;

}  // namespace kestrel::ui
