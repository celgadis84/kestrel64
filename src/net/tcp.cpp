#include "tcp.hpp"

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
  #pragma comment(lib, "ws2_32.lib")
  using socklen_t = int;
  static auto closesock(long long s) -> void { closesocket((SOCKET)s); }
#else
  #include <sys/socket.h>
  #include <netinet/in.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  static auto closesock(long long s) -> void { ::close((int)s); }
  static constexpr long long INVALID_SOCKET_V = -1;
#endif

#include <cstdio>

namespace kestrel::net {

#if defined(_WIN32)
static constexpr long long BADSOCK = (long long)INVALID_SOCKET;
#else
static constexpr long long BADSOCK = -1;
#endif

auto TcpServer::listen(u16 port) -> bool {
#if defined(_WIN32)
  if(!initialized) {
    WSADATA wsa;
    if(WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return false;
    initialized = true;
  }
#endif
  sock = (long long)::socket(AF_INET, SOCK_STREAM, 0);
  if(sock == BADSOCK) return false;

  int yes = 1;
  ::setsockopt((int)sock, SOL_SOCKET, SO_REUSEADDR, (const char*)&yes, sizeof yes);

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // 127.0.0.1 only
  if(::bind((int)sock, (sockaddr*)&addr, sizeof addr) != 0) { closesock(sock); sock = BADSOCK; return false; }
  // Cola de espera holgada: cada cliente aceptado se va a su propio hilo, pero entre el
  // accept y el arranque del hilo hay una ventana, y sin cola una conexion que caiga ahi
  // sale rechazada con ECONNREFUSED, que parece que el emulador se ha muerto.
  if(::listen((int)sock, 8) != 0) { closesock(sock); sock = BADSOCK; return false; }
  return true;
}

auto TcpServer::accept() -> TcpConn {
  if(sock == BADSOCK) return TcpConn{};
  long long c = (long long)::accept((int)sock, nullptr, nullptr);
  if(c == BADSOCK) return TcpConn{};
  int yes = 1;
  ::setsockopt((int)c, IPPROTO_TCP, TCP_NODELAY, (const char*)&yes, sizeof yes);
  return TcpConn{c};
}

auto TcpConn::close() -> void {
  if(sock != BADSOCK) { closesock(sock); sock = BADSOCK; }
}

auto TcpServer::close() -> void {
  if(sock != BADSOCK) { closesock(sock); sock = BADSOCK; }
#if defined(_WIN32)
  if(initialized) { WSACleanup(); initialized = false; }
#endif
}

auto TcpConn::recvAll(u8* buf, u32 len) -> bool {
  u32 got = 0;
  while(got < len) {
    int n = ::recv((int)sock, (char*)buf + got, (int)(len - got), 0);
    if(n <= 0) return false;
    got += (u32)n;
  }
  return true;
}

auto TcpConn::sendAll(const u8* buf, u32 len) -> bool {
  u32 sent = 0;
  while(sent < len) {
    int n = ::send((int)sock, (const char*)buf + sent, (int)(len - sent), 0);
    if(n <= 0) return false;
    sent += (u32)n;
  }
  return true;
}

static auto rd32le(const u8* p) -> u32 { return (u32)p[0] | (u32)p[1]<<8 | (u32)p[2]<<16 | (u32)p[3]<<24; }
static auto wr32le(u8* p, u32 v) -> void { p[0]=(u8)v; p[1]=(u8)(v>>8); p[2]=(u8)(v>>16); p[3]=(u8)(v>>24); }

auto TcpConn::recvFrame(std::string& json, std::vector<u8>& blob) -> bool {
  u8 hdr[4];
  if(!recvAll(hdr, 4)) return false;
  u32 total = rd32le(hdr);
  if(total < 4 || total > 64u*1024*1024) return false;  // sanity cap 64 MB
  std::vector<u8> payload(total);
  if(!recvAll(payload.data(), total)) return false;
  u32 jsonLen = rd32le(payload.data());
  if(4u + jsonLen > total) return false;
  json.assign((const char*)payload.data() + 4, jsonLen);
  blob.assign(payload.begin() + 4 + jsonLen, payload.end());
  return true;
}

auto TcpConn::sendFrame(const std::string& json, const u8* blob, u32 blobLen) -> bool {
  u32 jsonLen = (u32)json.size();
  u32 total = 4 + jsonLen + blobLen;
  std::vector<u8> frame(4 + total);
  wr32le(frame.data(), total);
  wr32le(frame.data() + 4, jsonLen);
  std::memcpy(frame.data() + 8, json.data(), jsonLen);
  if(blobLen) std::memcpy(frame.data() + 8 + jsonLen, blob, blobLen);
  return sendAll(frame.data(), (u32)frame.size());
}

}  // namespace kestrel::net
