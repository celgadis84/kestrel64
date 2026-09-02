#pragma once
// kestrel64 -- catalogo de opciones compartido por el lanzador y por el menu de la ventana.
//
// La ventana del emulador ensena las MISMAS opciones que el lanzador. Para que no se
// desincronicen, la tabla se genera (tools/gen_optdefs.py) desde tools/launcher/options.py,
// que sigue siendo la unica fuente de verdad. Aqui solo esta el modelo de datos.
//
// Cada opcion sabe traducirse a una variable de entorno con las mismas reglas que el
// lanzador (ver optenv.cpp), porque el emulador se reconfigura relanzandose a si mismo con
// el entorno nuevo cuando la opcion no es de las que se pueden cambiar en caliente.

#include "../core/types.hpp"

namespace kestrel::ui {

enum class OType { Bool, Int, Float, Text, Path, Choice, Hex };

struct Choice { const char* value; const char* label; };

struct Option {
  const char* id;
  const char* env;        // nullptr = opcion del lanzador, no viaja en el entorno
  const char* label;
  OType       type;
  const char* def;        // valor de fabrica ya en texto ("1"/"0" para los bool)
  const char* help;
  bool        adv;        // solo visible con "mostrar avanzadas"
  bool        invert;     // KESTREL_NOxxx: la variable DESACTIVA
  bool        tri;        // se exporta explicito en ambos sentidos ("1"/"0")
  double      min, max, step;
  const Choice* choices;
  int           nchoices;
  bool        live;       // se puede aplicar sin relanzar el proceso
};

struct Category {
  const char* id;
  const char* label;
  const char* desc;
  const Option* opts;
  int n;
};

struct PadCtl {
  const char* id;         // A, B, Z, START, DU..., SX+...
  const char* label;
  u32         bit;        // bit del joybus; 0 para los ejes del stick
  const char* key;        // tecla GLFW de fabrica
  const char* gp;         // boton de mando de fabrica
};

auto categories() -> const Category*;
auto categoryCount() -> int;
auto padControls() -> const PadCtl*;
auto padControlCount() -> int;

// Busca una opcion por id en todo el catalogo. nullptr si no existe.
auto findOption(const char* id) -> const Option*;

}  // namespace kestrel::ui
