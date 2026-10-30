#include "relay.hpp"

#include <cstdlib>
#include <chrono>
#include <cstdio>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <thread>
#include <vector>

namespace kestrel::net::relay {

namespace {

using Clock = std::chrono::steady_clock;

// Invitado esperando a que el anfitrion abra su conexion de vuelta.
struct Pending {
  std::shared_ptr<TcpConn> conn;
  std::string room;
  Clock::time_point since;
};

struct Room {
  std::shared_ptr<TcpConn> ctl;   // conexion de control del anfitrion
  std::shared_ptr<std::mutex> ctlMu;   // serializa los envios por el control
};

std::mutex g_mu;
std::map<std::string, Room> g_rooms;
std::map<std::string, Pending> g_pending;   // ficha -> invitado
std::mt19937_64 g_rng{std::random_device{}()};

constexpr int kPendingSecs = 30;

// Sin letras ni cifras que se confundan al dictarlas (0/O, 1/I/L).
auto randomCode(int n) -> std::string {
  static const char kAlpha[] = "ABCDEFGHJKMNPQRSTUVWXYZ23456789";
  std::string s;
  for(int i = 0; i < n; i++) s += kAlpha[g_rng() % (sizeof kAlpha - 1)];
  return s;
}

auto send(TcpConn& c, const std::string& type, const std::string& arg = {}) -> bool {
  return c.sendFrame(type, (const u8*)arg.data(), (u32)arg.size());
}

auto argOf(const std::vector<u8>& blob) -> std::string { return std::string(blob.begin(), blob.end()); }

auto log(const char* fmt, const std::string& a) -> void {
  std::fprintf(stderr, "[relay] ");
  std::fprintf(stderr, fmt, a.c_str());
  std::fprintf(stderr, "\n");
}

// Reenvia tramas de `a` a `b` hasta que una de las dos caiga; entonces cierra las dos para
// que el otro sentido tambien acabe.
auto pump(std::shared_ptr<TcpConn> a, std::shared_ptr<TcpConn> b) -> void {
  std::string j;
  std::vector<u8> blob;
  while(a->recvFrame(j, blob))
    if(!b->sendFrame(j, blob.data(), (u32)blob.size())) break;
  a->close();
  b->close();
}

auto purgeLocked() -> void {
  const auto now = Clock::now();
  for(auto it = g_pending.begin(); it != g_pending.end();) {
    if(now - it->second.since > std::chrono::seconds(kPendingSecs)) {
      it->second.conn->close();
      it = g_pending.erase(it);
    } else {
      ++it;
    }
  }
}

// Una conexion recien aceptada: su primera trama dice que es.
auto handle(std::shared_ptr<TcpConn> c) -> void {
  std::string type;
  std::vector<u8> blob;
  if(!c->recvFrame(type, blob)) return;

  if(type == "room.open") {
    std::string code;
    auto mu = std::make_shared<std::mutex>();
    {
      std::lock_guard<std::mutex> lk(g_mu);
      do code = randomCode(6); while(g_rooms.count(code));
      g_rooms[code] = Room{c, mu};
    }
    {
      std::lock_guard<std::mutex> lk(*mu);
      send(*c, "room", code);
    }
    log("sala %s abierta", code);
    // El control no lleva nada de vuelta; leer solo sirve para enterarse del cierre.
    while(c->recvFrame(type, blob)) {}
    std::lock_guard<std::mutex> lk(g_mu);
    g_rooms.erase(code);
    for(auto it = g_pending.begin(); it != g_pending.end();) {
      if(it->second.room == code) { it->second.conn->close(); it = g_pending.erase(it); }
      else ++it;
    }
    log("sala %s cerrada", code);
    return;
  }

  if(type == "room.join") {
    const std::string code = argOf(blob);
    std::string token;
    std::shared_ptr<TcpConn> ctl;
    std::shared_ptr<std::mutex> ctlMu;
    {
      std::lock_guard<std::mutex> lk(g_mu);
      purgeLocked();
      auto it = g_rooms.find(code);
      if(it != g_rooms.end()) {
        do token = randomCode(16); while(g_pending.count(token));
        g_pending[token] = Pending{c, code, Clock::now()};
        ctl = it->second.ctl;
        ctlMu = it->second.ctlMu;
      }
    }
    if(!ctl) { send(*c, "room.err", "no existe la sala " + code); return; }
    {
      std::lock_guard<std::mutex> lk(*ctlMu);
      send(*ctl, "room.guest", token);
    }
    log("invitado pide sala %s", code);
    // El hilo acaba aqui: la conexion la recoge el "room.accept" (o la purga).
    return;
  }

  if(type == "room.accept") {
    std::shared_ptr<TcpConn> guest;
    {
      std::lock_guard<std::mutex> lk(g_mu);
      auto it = g_pending.find(argOf(blob));
      if(it != g_pending.end()) { guest = it->second.conn; g_pending.erase(it); }
    }
    if(!guest) return;
    if(!send(*guest, "room.ok")) return;
    std::thread(pump, guest, c).detach();
    pump(c, guest);
    return;
  }
}

}  // namespace

auto serve(u16 port) -> int {
  TcpServer srv;
  if(!srv.listen(port, true)) {
    std::fprintf(stderr, "[relay] no puedo escuchar en el puerto %u\n", (unsigned)port);
    return 1;
  }
  std::fprintf(stderr, "[relay] escuchando en el puerto %u\n", (unsigned)port);
  for(;;) {
    TcpConn c = srv.accept();
    if(!c.valid()) continue;
    std::thread(handle, std::make_shared<TcpConn>(std::move(c))).detach();
  }
}

auto splitAddr(const std::string& addr, std::string& host, u16& port) -> void {
  const size_t c = addr.rfind(':');
  host = c == std::string::npos ? addr : addr.substr(0, c);
  port = kDefaultPort;
  if(c != std::string::npos) {
    const int p = std::atoi(addr.c_str() + c + 1);
    if(p > 0 && p < 65536) port = (u16)p;
  }
}

auto openRoom(const std::string& relayAddr, TcpConn& ctl, std::string& code, std::string& err) -> bool {
  std::string h;
  u16 p;
  splitAddr(relayAddr, h, p);
  ctl = tcpConnect(h, p, err);
  if(!ctl.valid()) return false;
  std::string type;
  std::vector<u8> blob;
  if(!send(ctl, "room.open") || !ctl.recvFrame(type, blob) || type != "room") {
    err = "el rele no abre sala";
    ctl.close();
    return false;
  }
  code = argOf(blob);
  return true;
}

auto nextGuest(TcpConn& ctl, const std::string& relayAddr) -> TcpConn {
  std::string type;
  std::vector<u8> blob;
  while(ctl.recvFrame(type, blob)) {
    if(type != "room.guest") continue;
    std::string h, err;
    u16 p;
    splitAddr(relayAddr, h, p);
    TcpConn g = tcpConnect(h, p, err);
    if(!g.valid()) continue;
    if(!send(g, "room.accept", argOf(blob))) continue;
    return g;
  }
  return TcpConn{};
}

auto joinRoom(const std::string& relayAddr, const std::string& code, std::string& err) -> TcpConn {
  std::string h;
  u16 p;
  splitAddr(relayAddr, h, p);
  TcpConn c = tcpConnect(h, p, err);
  if(!c.valid()) return c;
  std::string type;
  std::vector<u8> blob;
  if(!send(c, "room.join", code) || !c.recvFrame(type, blob)) {
    err = "el rele corto la conexion (sala " + code + ")";
    return TcpConn{};
  }
  if(type != "room.ok") {
    err = type == "room.err" ? argOf(blob) : "respuesta rara del rele: " + type;
    return TcpConn{};
  }
  return c;
}

}  // namespace kestrel::net::relay
