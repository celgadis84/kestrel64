#pragma once
// kestrel64 - minimal blocking TCP server with length-prefixed framing.
//
// VARIOS CLIENTES A LA VEZ. Antes se atendia a uno solo y en serie, y eso mordia en la
// practica: con el puente MCP enganchado, cualquier script de la maquina (por ejemplo
// scripts/pad.py, que navega un menu inyectando el mando) se quedaba esperando en el accept
// hasta que el puente cerrara, o sea nunca. El servidor acepta ahora cada conexion en su
// propio hilo y serializa el DESPACHO con un mutex, asi que la semantica de cada orden es
// exactamente la de antes: una cada vez, contra el mismo estado.
//
// Winsock en Windows, sockets BSD en el resto.

#include "../core/types.hpp"
#include <string>
#include <vector>
#include <functional>

namespace kestrel::net {

// Una conexion aceptada. Duena de su socket, movible, no copiable.
struct TcpConn {
  TcpConn() = default;
  explicit TcpConn(long long s) : sock(s) {}
  TcpConn(TcpConn&& o) noexcept : sock(o.sock) { o.sock = -1; }
  auto operator=(TcpConn&& o) noexcept -> TcpConn& {
    if(this != &o) { close(); sock = o.sock; o.sock = -1; }
    return *this;
  }
  TcpConn(const TcpConn&) = delete;
  auto operator=(const TcpConn&) -> TcpConn& = delete;
  ~TcpConn() { close(); }

  auto valid() const -> bool { return sock >= 0; }
  // Cerrar desde OTRO hilo es legitimo y es como se despierta a un recvFrame bloqueado
  // cuando el emulador se apaga.
  auto close() -> void;

  // Framed I/O: [u32 LE totalLen][payload]. payload = [u32 LE jsonLen][json][blob].
  // recvFrame bloquea; devuelve false al desconectar o al fallar.
  auto recvFrame(std::string& json, std::vector<u8>& blob) -> bool;
  auto sendFrame(const std::string& json, const u8* blob, u32 blobLen) -> bool;

private:
  long long sock = -1;
  auto recvAll(u8* buf, u32 len) -> bool;
  auto sendAll(const u8* buf, u32 len) -> bool;
};

struct TcpServer {
  // Bind + listen on 127.0.0.1:port. Returns false on failure.
  auto listen(u16 port) -> bool;
  auto close() -> void;

  // Block until a client connects. La conexion devuelta es invalida si el socket de
  // escucha se cerro (apagado).
  auto accept() -> TcpConn;

  ~TcpServer() { close(); }

private:
  long long sock = -1;     // listening socket (SOCKET on win is u64; store wide)
  bool initialized = false;
};

}  // namespace kestrel::net
