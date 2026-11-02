#pragma once
// Game card -- everything around a ROM: header, saves, manuals, cheats and per-game metadata.
//
// The logic is plain C++ (gamecard.cpp, no GUI) and the Win32 window is in gamecard_win32.cpp.
// All data comes from files next to the cartridge, using the same names as the emulator itself
// (.eep/.sra/.fla, .mpk, .st0-.st9, .cht), plus <state>/games/<CRC>.json shared with the
// launcher so both front ends show the same card.
//
// Invariants:
// - <base> is the ROM path without container (.zip/.gz) and without its extension.
// - Cheat edits rewrite only the affected lines (comments, format and line endings survive);
//   Meta keeps raw JSON values so keys unknown to the exe survive a write.
// - Nothing goes out to the network unless the user turned the cheat download on.
// - showCard() BLOCKS the calling thread in its own message loop; only one card per process
//   (a second call brings it forward and returns false).
//
// Threads: the file/parse helpers have no shared state worth locking and are called from the
// UI thread (menu, library) and main(); showCard/cardOpen belong to the UI thread that opened it.
// kestrel64 -- ficha de juego: lo que hay alrededor de una ROM.
//
// Es la misma ficha que la del lanzador (tools/launcher/gamecard.py), escrita en C++ para que
// el exe no dependa de el. Todo sale de ficheros al lado del cartucho, con los MISMOS nombres
// que usa el emulador:
//
//   <base>.eep/.sra/.fla     partida del cartucho      (src/core/memory_cart.cpp)
//   <base>.mpk, .mpk2..4     Controller Pak por mando
//   <base>.st0..st9          estados guardados         (src/core/savestate.cpp)
//   <base>.cht               trucos                    (src/core/cheats.cpp)
//
// <base> = ruta de la ROM sin contenedor (.zip/.gz) y sin su extension.
//
// Los datos que no estan en ningun fichero del juego (anio, estudio, notas, partidas jugadas)
// van a <estado>/games/<CRC>.json, el MISMO fichero que escribe el lanzador: las dos caras
// del programa ven la misma ficha. <estado> es la carpeta del perfil (profilePath()).

#include <string>
#include <utility>
#include <vector>

namespace kestrel::ui::card {

// ---------------------------------------------------------------- nombres y cabecera
auto romBase(const std::string& path) -> std::string;
auto baseName(const std::string& path) -> std::string;   // ultimo componente de la ruta

struct Header {
  bool ok = false;
  std::string name;     // nombre interno (sin no imprimibles)
  std::string crc;      // "%08X%08X" de CRC1/CRC2: la clave de la ficha
  std::string cart;     // codigo de 3 letras
  char region = '?';
};
// Lee la cabecera de la ROM, desenvolviendo .zip/.gz si hace falta.
auto readHeader(const std::string& path) -> Header;

// ---------------------------------------------------------------- partidas
struct SaveFile {
  std::string kind, label, file;
  unsigned long long size = 0;
  long long mtime = 0;   // segundos unix
};
auto saves(const std::string& path) -> std::vector<SaveFile>;

// ---------------------------------------------------------------- manuales
// PDF/HTML/TXT/MD/imagen en la carpeta de la ROM, en manuals/ o en manuales/, cuyo nombre
// normalizado empiece por la base del fichero o por el nombre interno (4+ caracteres).
auto manuals(const std::string& path, const std::string& internalName) -> std::vector<std::string>;
auto manualExtOk(const std::string& file) -> bool;
// Copia `src` a <carpeta de la ROM>/manuals/<base><ext> (o "<base> (N)<ext>" si ya existe).
// Devuelve la ruta destino, o "" con `err` relleno.
auto manualStore(const std::string& path, const std::string& src, std::string& err) -> std::string;

// Fuente de manuales que configura el USUARIO: una URL (o ruta) a un indice CSV con el formato
// external_files.csv de las bases del Downloader de MiSTer: Path,URL,tamano,md5[,...]. El exe
// no trae ninguna fuente de fabrica; la guarda en <estado>/manuals_source.txt.
struct ManualEntry {
  std::string path, url, md5;
  unsigned long long size = 0;
};
auto manualIndexParse(const std::string& csv) -> std::vector<ManualEntry>;
// Las entradas del indice que son de esta ROM: titulo del manual sin etiquetas "(...)"/"[...]"
// igual al nombre del fichero de la ROM (sin las suyas) o a su nombre interno. Mejor primero.
auto manualIndexMatch(const std::vector<ManualEntry>& idx, const std::string& path,
                      const std::string& internalName) -> std::vector<ManualEntry>;
auto manualSource() -> std::string;
auto setManualSource(const std::string& url) -> bool;
// Biblioteca global de manuales, <estado>/manuals/: "Descargar todos" baja ahi el indice entero
// de la fuente y manuals() mira tambien ahi (casado por titulo, sin etiquetas "(...)").
auto manualDbDir() -> std::string;
// Nombre de fichero seguro para la biblioteca: sin carpeta, ASCII y sin \/:*?"<>|.
auto dbFileName(const std::string& name) -> std::string;

// ---------------------------------------------------------------- trucos
// Mismo lector que Cheats::loadFile: comentario con # o ;, cabecera [nombre] con '-' delante
// = apagado, y codigos antes de la primera cabecera forman un truco implicito "sin nombre".
// La reescritura toca SOLO las lineas afectadas: comentarios, formato y fin de linea del
// usuario sobreviven.
struct Cheat {
  std::string name;
  bool on = true;
  int line = -1;      // linea de la cabecera (-1 = truco implicito)
  int first = 0;      // primera linea del truco
  std::vector<std::string> codes;   // normalizados "AAAAAAAA VVVV"
  int bad = 0;        // lineas ilegibles
  int inert = 0;      // 88/89/CC/DE/EE/FF: el emulador los lee pero no los aplica
};
auto chtPath(const std::string& path) -> std::string;
auto parseCht(const std::vector<std::string>& lines) -> std::vector<Cheat>;
auto cheats(const std::string& path) -> std::vector<Cheat>;
auto setCheat(const std::string& path, int id, bool on, std::string& err) -> bool;
auto addCheat(const std::string& path, const std::string& name, const std::string& codes, bool on,
              std::string& err) -> bool;
auto delCheat(const std::string& path, int id, std::string& err) -> bool;

// ---------------------------------------------------------------- biblioteca de trucos
// Como las caratulas del lanzador: una coleccion nombrada a lo No-Intro que se baja a la maquina
// del usuario (el exe no redistribuye nada). De fabrica, la carpeta N64 de libretro-database
// (CC BY-SA 4.0, .cht de RetroArch); el usuario puede poner otra URL de GitHub o una carpeta
// local (la de trucos de RetroArch, por ejemplo). Se guarda en <estado>/cheats_source.txt.
// La descarga es OPCIONAL y viene apagada: la fuente se ensena, pero nada sale a la red (ni se
// copia de la carpeta) hasta que el usuario marca la casilla.
//
// Lo bajado va a <estado>/cheats/ ya en el formato del emulador y con TODO apagado, asi que no
// cambia ningun juego por si solo. La ficha lo ensena cuando la ROM no tiene <base>.cht propio,
// y al encender, anadir o borrar un truco se copia a <base>.cht: el propio siempre manda.
extern const char* const kCheatSourceDefault;
auto cheatSource() -> std::string;                      // la del usuario o la de fabrica
auto cheatSourceOn() -> bool;                           // descarga encendida (defecto: no)
auto setCheatSource(const std::string& src, bool on) -> bool;  // src "" = la de fabrica
auto cheatDbDir() -> std::string;
// Donde estan los ficheros de una fuente. URL: raw.githubusercontent.com/<o>/<r>/<rama>/<ruta>
// o github.com/<o>/<r>/tree/<rama>/<ruta>; `base` + nombre codificado = el fichero, `index` =
// API de arboles de GitHub (una sola peticion lista la carpeta). Carpeta local: local=true.
struct CheatSrc {
  std::string base, index;
  bool local = false, ok = false;
};
auto cheatSrcResolve(const std::string& src) -> CheatSrc;
auto urlEncodePath(const std::string& s) -> std::string;
// Nombres de fichero .cht (con extension) de una respuesta del API de arboles.
auto cheatTreeParse(const std::string& json) -> std::vector<std::string>;
// El nombre de la coleccion que es esta ROM, o "": titulo exacto sin etiquetas, y si no prefijo
// en los dos sentidos SOLO si es inequivoco ("Mario Party" no elige entre 1 y 2). Con varias
// entradas gana la region del cartucho, luego USA, luego la mas corta (la version base).
auto cheatNameMatch(const std::vector<std::string>& names, const std::string& path,
                    const std::string& internalName, char region) -> std::string;
// .cht de RetroArch (cheatN_desc / cheatN_code con '+' / cheatN_enable) o .cht del emulador ->
// formato del emulador, todo apagado, con `origin` de comentario. "" si no sale ningun truco.
// Un truco con algun codigo ilegible se descarta entero: aplicarlo a medias es peor.
auto chtFromRetroArch(const std::string& text, const std::string& origin) -> std::string;
// Indice de la fuente cacheado en <estado>/cheats/index.txt (7 dias; el API de GitHub sin
// cuenta admite 60 peticiones por hora).
auto cheatIndexCached(const std::string& src, std::vector<std::string>& names) -> bool;
auto cheatIndexStore(const std::string& src, const std::vector<std::string>& names) -> void;
// Convierte y guarda en `dir` un fichero bajado de la fuente.
auto cheatDbStore(const std::string& dir, const std::string& name, const std::string& raw,
                  std::string& err) -> bool;
auto cheatDbFile(const std::string& path) -> std::string;   // el de la biblioteca, o ""
auto cheatFile(const std::string& path) -> std::string;     // el propio si existe, si no ese

// ---------------------------------------------------------------- metadatos
// Objeto JSON plano. Se guardan los valores en crudo (texto JSON) para que las claves que no
// conoce el exe sobrevivan a una escritura.
struct Meta {
  std::vector<std::pair<std::string, std::string>> kv;   // clave -> valor JSON crudo
  auto str(const char* k) const -> std::string;          // valor de texto decodificado
  auto num(const char* k) const -> long long;
  auto flag(const char* k) const -> bool;
  auto setStr(const char* k, const std::string& v) -> void;
  auto setNum(const char* k, long long v) -> void;
  auto setFlag(const char* k, bool v) -> void;
};
auto stateDir() -> std::string;
auto metaKey(const std::string& path, const Header& h) -> std::string;
auto metaLoad(const std::string& key) -> Meta;
auto metaSave(const std::string& key, const Meta& m) -> bool;
// Una sesion terminada: suma una partida y su tiempo. Menos de 3 s no cuenta.
auto metaPlayed(const std::string& key, long long seconds) -> void;

// ---------------------------------------------------------------- ventana (gamecard_win32.cpp)
// Abre la ficha y BLOQUEA al hilo que llama hasta que se cierra (ventana y bucle propios; el
// dueno, si lo hay, queda deshabilitado mientras tanto). Con `canPlay` sale un boton "Jugar";
// devuelve true si se pulso. Una sola ficha por proceso: si ya hay una, la trae delante y
// devuelve false.
auto showCard(const std::string& rom, void* owner, bool canPlay) -> bool;
auto cardOpen() -> bool;

// JSON plano (expuesto para la prueba).
auto parseFlatJson(const std::string& txt, Meta& out) -> bool;
auto writeFlatJson(const Meta& m) -> std::string;

}  // namespace kestrel::ui::card
