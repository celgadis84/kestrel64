#pragma once
// kestrel64 — minimal blocking TCP server with length-prefixed framing.
// One client at a time (the MCP bridge). Winsock on Windows, BSD sockets elsewhere.

#include "../core/types.hpp"
#include <string>
#include <vector>
#include <functional>

namespace kestrel::net {

struct TcpServer {
  // Bind + listen on 127.0.0.1:port. Returns false on failure.
  auto listen(u16 port) -> bool;
  auto close() -> void;

  // Block until a client connects; returns false if the server is shutting down.
  auto accept() -> bool;
  auto connected() const -> bool { return client >= 0; }
  auto dropClient() -> void;

  // Framed I/O: [u32 LE totalLen][payload]. payload = [u32 LE jsonLen][json][blob].
  // recvFrame blocks; returns false on disconnect/error.
  auto recvFrame(std::string& json, std::vector<u8>& blob) -> bool;
  auto sendFrame(const std::string& json, const u8* blob, u32 blobLen) -> bool;

  ~TcpServer() { close(); }

private:
  long long sock = -1;     // listening socket (SOCKET on win is u64; store wide)
  long long client = -1;   // accepted client socket
  bool initialized = false;

  auto recvAll(u8* buf, u32 len) -> bool;
  auto sendAll(const u8* buf, u32 len) -> bool;
};

}  // namespace kestrel::net
