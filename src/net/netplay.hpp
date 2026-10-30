#pragma once
// kestrel64 - juego en red por sincronizacion de entradas (lockstep con retardo).
//
// Todas las maquinas corren la MISMA emulacion; por la red solo viajan los mandos. Un
// anfitrion y hasta tres invitados en estrella: cada invitado manda su mando al anfitrion,
// el anfitrion junta los cuatro de cada campo de video y los reparte. Cada mando se aplica
// D campos despues de leerse (retardo de entrada), que es el margen que tiene la red para
// llevarlo a todos antes de que haga falta; si no llega, la maquina espera.
//
// Para que dos maquinas den lo mismo con las mismas entradas hace falta que la emulacion
// sea reproducible: en red se fuerza SPLEAD=0 (ver Memory::spLeadOps y ARCH-COMM §8), no
// se aplican trucos y no se cargan estados ni se rebobina. El punto de partida comun es
// un estado del anfitrion: al entrar alguien (y al detectar desincronia) el anfitrion
// fotografia la maquina en reposo del RCP, la manda y la carga EL TAMBIEN, de modo que
// todos arrancan del mismo estado restaurado y no del vivo. Con el estado viajan la
// partida guardada y el reloj, asi que los invitados juegan con la partida del anfitrion
// (y su propio fichero de partida no se toca en todo el proceso).
//
// Desincronia: cada 60 campos los invitados mandan un hash del estado de la CPU al cierre
// del campo y el anfitrion lo compara con el suyo; si difiere, vuelve a sincronizar.

#include "../core/types.hpp"
#include <atomic>
#include <string>

namespace kestrel { struct System; }

namespace kestrel::netplay {

// Puerto por defecto del anfitrion.
constexpr u16 kDefaultPort = 6464;

struct Status {
  bool active = false;      // hay sesion (anfitrion escuchando o invitado conectado)
  bool host = false;
  bool synced = false;      // corriendo en lockstep (invitado: ya recibio el estado)
  int  slot = 0;            // conector de este jugador (0 = jugador 1)
  int  players = 1;         // jugadores en la partida
  u32  delay = 0;           // retardo de entrada en campos
  u64  field = 0;           // campos desde la ultima sincronizacion
  u32  epoch = 0;           // sincronizaciones hechas
  u32  desyncs = 0;         // desincronias detectadas (anfitrion)
  u32  checks = 0;          // hashes comparados (anfitrion)
  double waitMs = 0;        // espera media por campo a la red, ultimos ~60 campos
  std::string peer;         // anfitrion: puerto; invitado: direccion
  std::string msg;          // ultimo aviso
  int  view = 0;            // vista de pantalla partida vigente (kView*)
  std::string room;         // anfitrion por rele: codigo de sala que dar a los invitados
  std::string publicAddr;   // anfitrion: IP publica:puerto si UPnP abrio el puerto del router
};

// Abrir partida en `port` (todas las interfaces) o unirse. Fallan con el motivo en err. Solo
// una sesion a la vez.
//  - host con `relay` ("rele[:puerto]", src/net/relay.hpp): ademas abre sala en el rele y
//    admite a los invitados que entren por el (sin abrir puertos). Sin rele intenta abrir el
//    puerto en el router por UPnP en segundo plano (KESTREL_NETPLAY_UPNP=0 no).
//  - join: "host[:puerto]" directo, o "rele[:puerto]/CODIGO" por sala del rele.
auto host(System& sys, u16 port, std::string& err, const std::string& relay = {}) -> bool;
auto join(System& sys, const std::string& addr, std::string& err) -> bool;
auto stop() -> void;
auto status() -> Status;
// KESTREL_NETPLAY_HOST=<puerto> / KESTREL_NETPLAY_JOIN=<host:puerto | rele/CODIGO>,
// KESTREL_NETPLAY_RELAY=<rele[:puerto]> (con HOST), KESTREL_NETPLAY_DELAY, KESTREL_NETPLAY_UPNP.
auto startFromEnv(System& sys) -> void;
// Lo mismo con el System que dejo registrado startFromEnv (menu de la ventana, que no lo ve).
auto host(u16 port, std::string& err, const std::string& relay = {}) -> bool;
auto join(const std::string& addr, std::string& err) -> bool;

// --- pantalla partida ---------------------------------------------------------------------
//
// En los juegos a pantalla partida cada jugador puede ver solo su trozo, ampliado a toda la
// ventana. Lo decide el ANFITRION (en los menus el juego va a pantalla completa, asi que se
// enciende al empezar la partida y se apaga al volver al menu) y viaja dentro de las
// entradas de cada campo: todas las maquinas cambian de vista en el mismo campo. Es solo
// presentacion; la emulacion no se entera.
enum : u8 { kViewFull = 0, kViewRows = 1, kViewCols = 2, kViewQuad = 3, kViewAuto = 4 };
// Anfitrion: fija la vista (kViewAuto = filas con dos jugadores, cuadrantes con mas).
// Falla fuera de sesion o en un invitado.
auto setView(int mode, std::string& err) -> bool;
// Tecla del anfitrion: completa <-> automatica.
auto toggleView(std::string& err) -> bool;

// --- enganches del nucleo ---------------------------------------------------------------

inline std::atomic<bool> g_lockstep{false};   // entradas de red mandan (hilo de la CPU)
inline std::atomic<bool> g_tainted{false};    // invitado que cargo un estado ajeno
inline auto lockstep() -> bool { return g_lockstep.load(std::memory_order_relaxed); }
// Este proceso ha cargado la partida de otro: no se escribe ningun fichero de partida.
inline auto tainted() -> bool { return g_tainted.load(std::memory_order_relaxed); }

// Entradas del campo en curso. Las escribe afterField y las lee el joybus, los dos en el
// hilo del nucleo. Inline (como lo de arriba) para que memory.cpp no arrastre la red a los
// tests que lo compilan.
struct NetPad { u32 b = 0; s8 x = 0, y = 0; };
inline NetPad g_pad[4];
// Lectura de mando del joybus (hilo de la CPU): pisa con la entrada de red del campo.
inline auto pad(int channel, u32& btn, s8& sx, s8& sy) -> void {
  if(channel < 0 || channel > 3) return;
  btn = g_pad[channel].b; sx = g_pad[channel].x; sy = g_pad[channel].y;
}

// Vista del campo en curso y conector de esta maquina (-1 = sin sesion sincronizada). Las
// escribe el hilo del nucleo y las lee el presentador.
inline std::atomic<u8>  g_view{kViewFull};
inline std::atomic<int> g_viewSlot{-1};
// Trozo del cuadro w x h que toca ver a este jugador. Falso = cuadro entero.
inline auto viewCrop(u32 w, u32 h, u32& x, u32& y, u32& cw, u32& ch) -> bool {
  const u8 v = g_view.load(std::memory_order_relaxed);
  const int s = g_viewSlot.load(std::memory_order_relaxed);
  if(v == kViewFull || v > kViewQuad || s < 0 || s > 3 || w < 2 || h < 2) return false;
  if(v == kViewRows && s > 1) return false;      // dos franjas: el 3 y el 4 no tienen trozo
  if(v == kViewCols && s > 1) return false;
  const bool col = v == kViewCols ? s == 1 : v == kViewQuad && (s & 1);
  const bool row = v == kViewRows ? s == 1 : v == kViewQuad && (s & 2);
  cw = v == kViewRows ? w : w / 2;
  ch = v == kViewCols ? h : h / 2;
  x = col ? w - cw : 0;
  y = row ? h - ch : 0;
  return true;
}
// Cierre de campo, en el bucle del nucleo y FUERA de coreMutex: manda el mando local,
// espera las entradas del campo siguiente y atiende la sincronizacion de un invitado.
auto afterField(System& sys) -> void;
// Anfitrion: sincronizacion pedida (System::netSyncReq). La llama serviceStateReq con el
// RCP quieto y coreMutex tomado.
auto serviceSync(System& sys) -> void;

}  // namespace kestrel::netplay
