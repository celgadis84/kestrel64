#pragma once
// Relay -- rendezvous server and client helpers for netplay without open ports.
//
// Everybody connects OUT to the relay: the host gets a room code, guests join with it, and the
// relay splices the two connections and forwards frames verbatim (it knows nothing about the
// game). The host sees each spliced connection like one accepted on its own port.
//
// Invariants:
// - openRoom() leaves `ctl` as the host control connection; nextGuest() must be called on it
//   in a loop and returns an invalid TcpConn when the control closes (end of session or relay
//   down). Each guest needs a NEW connection ("room.accept"), never the control one.
// - serve() only returns if it cannot listen (the value is the process exit code).
//
// Threads: serve() runs on the main thread (`--relay`, no ROM, no window); it spawns one
// detached thread per connection plus a pump thread per spliced pair, with a global mutex for
// the room table and a per-control mutex serialising sends. openRoom/nextGuest/joinRoom are
// called from netplay (host start-up, its relay loop, guest join) and may block on the network.
// kestrel64 - rele (punto de encuentro) para el juego en red.
//
// Sin rele, el anfitrion tiene que abrir un puerto en su router y dar su IP publica. Con el
// rele nadie abre nada: todos SALEN hacia el rele (que si esta a la vista, por ejemplo en un
// servidor alquilado), el anfitrion recibe un codigo de sala y los invitados entran con el.
// El rele no entiende el juego: empareja conexiones y reenvia tramas tal cual.
//
//   anfitrion --"room.open"--> rele --"room" {codigo}-->            (conexion de control)
//   invitado  --"room.join" {codigo}--> rele --"room.guest" {ficha}--> anfitrion (control)
//   anfitrion --"room.accept" {ficha}--> rele (conexion NUEVA, una por invitado)
//   rele --"room.ok"--> invitado; desde ahi las dos conexiones quedan empalmadas
//
// Para el anfitrion, la conexion nueva es como una aceptada en su propio puerto: le llega el
// "hello" del invitado y sigue el protocolo de siempre (netplay.cpp).
//
// Servidor: `kestrel64 --relay [puerto]` (6465 por defecto), sin ROM ni ventana.

#include "../core/types.hpp"
#include "tcp.hpp"
#include <string>

namespace kestrel::net::relay {

constexpr u16 kDefaultPort = 6465;

// Bucle del servidor; solo vuelve si no puede escuchar (devuelve el codigo de salida).
auto serve(u16 port) -> int;

// "host[:puerto]" -> host y puerto (kDefaultPort si no lo trae).
auto splitAddr(const std::string& addr, std::string& host, u16& port) -> void;

// Anfitrion: abre sala. `ctl` queda como conexion de control (llegan "room.guest").
auto openRoom(const std::string& relayAddr, TcpConn& ctl, std::string& code, std::string& err) -> bool;
// Anfitrion: espera en `ctl` al siguiente invitado y devuelve su conexion ya empalmada.
// Invalida si el control se cierra (fin de sesion o rele caido).
auto nextGuest(TcpConn& ctl, const std::string& relayAddr) -> TcpConn;
// Invitado: entra en la sala `code`; la conexion devuelta ya va empalmada con el anfitrion.
auto joinRoom(const std::string& relayAddr, const std::string& code, std::string& err) -> TcpConn;

}  // namespace kestrel::net::relay
