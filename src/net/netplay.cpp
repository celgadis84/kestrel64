#include "netplay.hpp"
#include "tcp.hpp"
#include "relay.hpp"
#include "upnp.hpp"
#include "../core/system.hpp"
#include "../core/savestate.hpp"
#include <array>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace kestrel::netplay {
namespace {

constexpr u32 kMagic     = 0x4E34364Bu;   // "K64N"
constexpr u32 kVersion   = 2;            // 2: vista de pantalla partida en "in"
constexpr u64 kHashEvery = 60;            // campos entre hashes de comprobacion
constexpr u32 kMaxDelay  = 30;

using Pad = NetPad;
struct Frame { Pad p[4]; u8 view = kViewFull; };

// Mensajes: [tipo en el json][cuerpo binario little-endian en el blob].
struct W {
  std::vector<u8> v;
  auto b8(u8 x) -> void { v.push_back(x); }
  auto b32(u32 x) -> void { for(int i = 0; i < 4; i++) v.push_back((u8)(x >> (8 * i))); }
  auto b64(u64 x) -> void { b32((u32)x); b32((u32)(x >> 32)); }
  auto pad(const Pad& p) -> void { b32(p.b); b8((u8)p.x); b8((u8)p.y); }
};
struct R {
  const std::vector<u8>& v;
  usize o = 0;
  bool ok = true;
  auto b8() -> u8 { if(o + 1 > v.size()) { ok = false; return 0; } return v[o++]; }
  auto b32() -> u32 {
    if(o + 4 > v.size()) { ok = false; return 0; }
    u32 x = (u32)v[o] | (u32)v[o + 1] << 8 | (u32)v[o + 2] << 16 | (u32)v[o + 3] << 24;
    o += 4; return x;
  }
  auto b64() -> u64 { u64 lo = b32(); return lo | (u64)b32() << 32; }
  auto pad() -> Pad { Pad p; p.b = b32(); p.x = (s8)b8(); p.y = (s8)b8(); return p; }
};

// Una conexion con su candado de ENVIO: mandan el hilo del nucleo y los de recepcion.
struct Link {
  net::TcpConn conn;
  std::mutex sendMu;
  int slot = 0;
  std::thread rx;
  bool alive = true;               // bajo Session::mu
  auto send(const char* t, const W& w) -> bool {
    std::lock_guard<std::mutex> lk(sendMu);
    return conn.sendFrame(t, w.v.data(), (u32)w.v.size());
  }
  auto kill() -> void { std::lock_guard<std::mutex> lk(sendMu); conn.close(); }
};

struct PadCfg { bool connected = false, mouse = false; u8 accessory = 0; };

// Orden de candados: coreMutex antes que mu, nunca al reves (la telemetria pide el estado
// con coreMutex tomado). Por eso el invitado suelta mu antes de cargar el estado.
struct Session {
  System* sys = nullptr;
  bool on = false, isHost = false;
  int slot = 0;
  u32 delay = 2;
  std::string peerName, msg;
  u64 romHash = 0;

  std::mutex mu;
  std::condition_variable cv;
  bool closing = false, lost = false;
  bool synced = false;
  u32 epoch = 0;
  u64 field = 0;
  u8  mask = 1;                    // conectores con jugador en la partida (bit = conector)
  std::map<u64, Frame> ready;      // entradas completas de esta epoca, por campo
  u32 desyncs = 0, checks = 0;
  double waitAcc = 0; u32 waitN = 0; double waitMs = 0;
  PadCfg saved[4];                 // configuracion de mandos de antes de la sesion
  bool savedValid = false;

  // anfitrion
  net::TcpServer server;
  std::thread acceptTh;
  std::vector<std::unique_ptr<Link>> peers;
  net::TcpConn relayCtl;           // control de la sala en el rele (vacio sin rele)
  std::string relayAddr, room;
  std::thread relayTh;
  bool listening = false;          // puerto propio abierto (con rele es opcional)
  u32 gen = 0;                     // sesion en curso: los hilos de UPnP no pisan otra
  net::upnp::Gateway gw;                // router con el puerto abierto por UPnP (gw.ok)
  u16 mappedPort = 0;
  std::string publicAddr;
  std::map<u64, std::array<std::pair<bool, Pad>, 4>> partial;
  std::map<u64, u8> partialView;   // vista que puso el anfitrion al mandar su mando del campo
  u8 hostView = kViewFull;         // vista pedida (entra en el campo f+D de su siguiente mando)
  u8 pendingMask = 0;              // entran en la proxima sincronizacion
  std::map<u64, u64> myHash;
  struct PeerHash { int slot; u64 f, h; };
  std::vector<PeerHash> peerHash;

  // invitado
  std::unique_ptr<Link> up;
  u32 rxEpoch = 0;
  bool syncReady = false;
  std::vector<u8> syncBlob;
  u32 syncDelay = 2;
  u8 syncMask = 1;
  PadCfg syncCfg[4];
};

Session S;
System* g_sys = nullptr;          // el de startFromEnv, para el menu
auto setCur(const Frame& fr) -> void {
  for(int i = 0; i < 4; i++) g_pad[i] = fr.p[i];
  g_view.store(fr.view, std::memory_order_relaxed);
}
auto viewFrame() -> Frame { Frame fr; fr.view = g_view.load(std::memory_order_relaxed); return fr; }

auto log(const std::string& m) -> void {
  std::printf("[netplay] %s\n", m.c_str());
  std::fflush(stdout);
}
auto note(const std::string& m) -> void { S.msg = m; log(m); }   // mu tomado

auto romHashOf(const System& sys) -> u64 {
  u64 h = 1469598103934665603ull;
  const auto& r = sys.memory.rom;
  for(usize i = 0; i < r.size(); i++) { h ^= r[i]; h *= 1099511628211ull; }
  return h ^ r.size();
}

// Hash del estado de la CPU al cierre del campo. Mismo criterio que KESTREL_FIELDHASH:
// determinista con SPLEAD=0 y barato (la RDRAM la escriben a la vez los hilos del RCP).
auto cpuHash(const System& sys) -> u64 {
  const CPU& cpu = sys.cpu;
  u64 h = 1469598103934665603ull;
  auto mix = [&](u64 v) { h ^= v; h *= 1099511628211ull; };
  for(int r = 0; r < 32; r++) mix((u64)cpu.gpr[r]);
  mix(cpu.pc); mix(cpu.nextPc);
  mix((u64)cpu.cop0[9]); mix((u64)cpu.cop0[11]); mix((u64)cpu.cop0[12]); mix((u64)cpu.cop0[13]);
  mix(cpu.retired);
  return h;
}

// Mando local = el del conector 1 de esta maquina, con el inyectado por telemetria encima.
auto localPad(System& sys) -> Pad {
  Memory& m = sys.memory;
  Pad p;
  if(m.padRemotePolls[0].load(std::memory_order_acquire) != 0) {
    p.b = m.padRemoteButtons[0].load(std::memory_order_relaxed);
    s32 st = m.padRemoteStick[0].load(std::memory_order_relaxed);
    p.x = (s8)(st & 0xff); p.y = (s8)((st >> 8) & 0xff);
  } else {
    p.b = m.padPort[0].buttons; p.x = m.padPort[0].stickX; p.y = m.padPort[0].stickY;
  }
  return p;
}

auto savePadCfg() -> void {
  if(S.savedValid) return;
  for(int i = 0; i < 4; i++) {
    auto& pp = S.sys->memory.padPort[i];
    S.saved[i] = {pp.connected, pp.mouse, pp.accessory};
  }
  S.savedValid = true;
}
auto applyPadCfg(const PadCfg* c) -> void {      // coreMutex tomado
  for(int i = 0; i < 4; i++) {
    auto& pp = S.sys->memory.padPort[i];
    pp.connected = c[i].connected; pp.mouse = c[i].mouse; pp.accessory = c[i].accessory;
  }
}

auto encodeFrame(W& w, u32 epoch, u64 f, const Frame& fr) -> void {
  w.b32(epoch); w.b64(f);
  for(const Pad& p : fr.p) w.pad(p);
  w.b8(fr.view);
}

// --- anfitrion ---------------------------------------------------------------------------

// Campo f completo si todos los conectores de la partida han mandado su mando: se publica y
// se reparte. mu tomado.
auto tryComplete(u64 f) -> void {
  auto it = S.partial.find(f);
  if(it == S.partial.end()) return;
  for(int i = 0; i < 4; i++)
    if((S.mask >> i & 1) && !it->second[i].first) return;
  Frame fr;
  for(int i = 0; i < 4; i++) if(it->second[i].first) fr.p[i] = it->second[i].second;
  S.partial.erase(it);
  if(auto v = S.partialView.find(f); v != S.partialView.end()) { fr.view = v->second; S.partialView.erase(v); }
  S.ready[f] = fr;
  W w; encodeFrame(w, S.epoch, f, fr);
  for(auto& l : S.peers) if(l->alive) l->send("in", w);
  S.cv.notify_all();
}

auto submit(int slot, u64 f, const Pad& p) -> void {   // mu tomado
  if(f < S.field || S.ready.count(f)) return;          // tarde o repetido
  auto& e = S.partial[f];
  e[slot] = {true, p};
  tryComplete(f);
}

auto hostSubmit(u64 f, const Pad& p) -> void {   // mu tomado
  S.partialView[f] = S.hostView;
  submit(0, f, p);
}

auto compareHash(int slot, u64 f, u64 theirs, u64 mine) -> void {   // mu tomado
  S.checks++;
  if(theirs == mine) return;
  S.desyncs++;
  note("DESINCRONIA con jugador " + std::to_string(slot + 1) + " en el campo " + std::to_string(f) +
       ": resincronizando");
  S.sys->netSyncReq.store(true, std::memory_order_release);
}

auto hostRx(Link* l) -> void {
  std::string t; std::vector<u8> b;
  while(l->conn.recvFrame(t, b)) {
    R r{b};
    if(t == "in") {
      u32 ep = r.b32(); u64 f = r.b64(); Pad p = r.pad();
      std::lock_guard<std::mutex> lk(S.mu);
      if(r.ok && ep == S.epoch && S.synced) submit(l->slot, f, p);
    } else if(t == "h") {
      u32 ep = r.b32(); u64 f = r.b64(), h = r.b64();
      std::lock_guard<std::mutex> lk(S.mu);
      if(!r.ok || ep != S.epoch) continue;
      auto it = S.myHash.find(f);
      if(it != S.myHash.end()) compareHash(l->slot, f, h, it->second);
      else S.peerHash.push_back({l->slot, f, h});
    } else if(t == "bye") {
      break;
    }
  }
  std::lock_guard<std::mutex> lk(S.mu);
  l->alive = false;
  const u8 bit = (u8)(1u << l->slot);
  S.pendingMask &= (u8)~bit;
  if(S.mask & bit) {
    // Su conector sigue enchufado (cambiarlo exigiria resincronizar) pero manda reposo.
    S.mask &= (u8)~bit;
    std::vector<u64> pend;
    for(auto& kv : S.partial) pend.push_back(kv.first);
    for(u64 f : pend) tryComplete(f);
  }
  if(!S.closing) note("jugador " + std::to_string(l->slot + 1) + " se ha ido");
  S.cv.notify_all();
}

// Una conexion entrante, del puerto propio o empalmada por el rele: espera su "hello" y le
// da conector. Falso = la sesion se cierra (dejar de aceptar).
auto admit(net::TcpConn c) -> bool {
  std::string t; std::vector<u8> b;
  if(!c.recvFrame(t, b) || t != "hello") return true;
  R r{b};
  const u32 magic = r.b32(), ver = r.b32();
  const u64 rh = r.b64();
  auto reject = [&](const std::string& why) {
    W w; for(char ch : why) w.b8((u8)ch);
    c.sendFrame("reject", w.v.data(), (u32)w.v.size());
  };
  if(!r.ok || magic != kMagic || ver != kVersion) { reject("version de kestrel64 distinta"); return true; }
  if(rh != S.romHash) { reject("la ROM no es la misma que la del anfitrion"); return true; }
  std::lock_guard<std::mutex> lk(S.mu);
  if(S.closing) return false;
  u8 used = (u8)(S.mask | S.pendingMask | 1u);
  for(auto& l : S.peers) if(l->alive) used |= (u8)(1u << l->slot);
  int slot = -1;
  for(int i = 1; i < 4; i++) if(!(used >> i & 1)) { slot = i; break; }
  if(slot < 0) { reject("partida llena (4 jugadores)"); return true; }
  auto l = std::make_unique<Link>();
  l->conn = std::move(c);
  l->slot = slot;
  W w; w.b32((u32)slot); w.b32(S.delay);
  if(!l->send("welcome", w)) return true;
  S.pendingMask |= (u8)(1u << slot);
  Link* lp = l.get();
  l->rx = std::thread(hostRx, lp);
  S.peers.push_back(std::move(l));
  note("entra el jugador " + std::to_string(slot + 1) + ": sincronizando");
  S.sys->netSyncReq.store(true, std::memory_order_release);
  return true;
}

auto acceptLoop() -> void {
  for(;;) {
    net::TcpConn c = S.server.accept();
    if(!c.valid()) return;                        // escucha cerrada: fin de sesion
    if(!admit(std::move(c))) return;
  }
}

// Invitados que llegan por la sala del rele. Acaba cuando se cierra el control (stop) o se
// cae el rele.
auto relayLoop() -> void {
  for(;;) {
    net::TcpConn c = net::relay::nextGuest(S.relayCtl, S.relayAddr);
    if(!c.valid()) break;
    if(!admit(std::move(c))) return;
  }
  std::lock_guard<std::mutex> lk(S.mu);
  if(!S.closing) note("conexion con el rele perdida: la sala " + S.room + " ya no admite a nadie");
}

// Abre el puerto en el router por UPnP y averigua la IP publica. Hilo suelto: si la sesion
// cambia mientras tanto, deshace lo que haya hecho.
auto upnpTask(u32 gen, u16 port) -> void {
  net::upnp::Gateway g = net::upnp::discover();
  std::string err, ip;
  bool mapped = false;
  if(g.ok) mapped = net::upnp::addMapping(g, port, err);
  if(mapped) net::upnp::externalIp(g, ip);
  std::lock_guard<std::mutex> lk(S.mu);
  if(S.gen != gen || S.closing || !S.on) {
    if(mapped) net::upnp::deleteMapping(g, port);
    return;
  }
  if(!g.ok) { log("UPnP: " + g.err + " (abre el puerto a mano o usa un rele)"); return; }
  if(!mapped) { log("UPnP: " + err); return; }
  S.gw = g; S.mappedPort = port;
  S.publicAddr = ip.empty() ? std::string() : ip + ":" + std::to_string(port);
  note("UPnP: puerto " + std::to_string(port) + " abierto en el router" +
       (ip.empty() ? std::string() : "; los invitados entran por " + S.publicAddr));
}

// --- invitado ----------------------------------------------------------------------------

auto clientRx(Link* l) -> void {
  std::string t; std::vector<u8> b;
  while(l->conn.recvFrame(t, b)) {
    R r{b};
    if(t == "in") {
      u32 ep = r.b32(); u64 f = r.b64();
      Frame fr; for(Pad& p : fr.p) p = r.pad();
      fr.view = r.b8();
      std::lock_guard<std::mutex> lk(S.mu);
      if(r.ok && ep == S.rxEpoch) { S.ready[f] = fr; S.cv.notify_all(); }
    } else if(t == "sync") {
      u32 ep = r.b32(), d = r.b32(); u8 m = r.b8();
      PadCfg cfg[4];
      for(auto& c : cfg) { c.connected = r.b8() != 0; c.mouse = r.b8() != 0; c.accessory = r.b8(); }
      if(!r.ok) break;
      std::lock_guard<std::mutex> lk(S.mu);
      S.rxEpoch = ep; S.syncDelay = d; S.syncMask = m;
      for(int i = 0; i < 4; i++) S.syncCfg[i] = cfg[i];
      S.syncBlob.assign(b.begin() + (std::ptrdiff_t)r.o, b.end());
      S.syncReady = true;
      S.ready.clear();
      S.cv.notify_all();
    } else if(t == "bye") {
      break;
    }
  }
  std::lock_guard<std::mutex> lk(S.mu);
  if(!S.closing) { S.lost = true; note("conexion con el anfitrion perdida"); }
  S.cv.notify_all();
}

// Cargar el estado recibido. Entra con mu tomado (lk) y lo suelta para coger coreMutex.
auto clientApplySync(std::unique_lock<std::mutex>& lk) -> void {
  std::vector<u8> blob = std::move(S.syncBlob);
  const u32 ep = S.rxEpoch, d = S.syncDelay;
  const u8 m = S.syncMask;
  PadCfg cfg[4]; for(int i = 0; i < 4; i++) cfg[i] = S.syncCfg[i];
  S.syncReady = false;
  lk.unlock();
  std::string err;
  bool ok;
  {
    std::lock_guard<System::CoreMutex> ck(S.sys->coreMutex);
    S.sys->quiesceRcp();
    savePadCfg();
    applyPadCfg(cfg);
    ok = restoreState(*S.sys, blob.data(), blob.size(), err);
    S.sys->rewinder.clear();
  }
  g_tainted.store(true);
  lk.lock();
  if(!ok) { S.lost = true; note("no se pudo cargar el estado del anfitrion: " + err); return; }
  if(S.syncReady) return;                        // ya hay otra en camino: manda esa
  S.epoch = ep; S.delay = d; S.mask = m; S.field = 0;
  for(u64 f = 0; f < d; f++) S.ready.emplace(f, viewFrame());
  S.synced = true;
  g_lockstep.store(true);
  g_viewSlot.store(S.slot);
  setCur(S.ready[0]);
  W w; w.b32(S.epoch); w.b64(d); w.pad(localPad(*S.sys));
  S.up->send("in", w);
  note("sincronizado (epoca " + std::to_string(ep) + ", jugador " + std::to_string(S.slot + 1) + ")");
}

auto resetSession() -> void {   // mu tomado o sin hilos vivos
  S.closing = S.lost = S.synced = S.syncReady = false;
  S.epoch = S.rxEpoch = 0; S.field = 0; S.mask = 1; S.pendingMask = 0;
  S.ready.clear(); S.partial.clear(); S.partialView.clear(); S.myHash.clear(); S.peerHash.clear();
  S.hostView = kViewFull; g_viewSlot.store(-1);
  S.relayAddr.clear(); S.room.clear(); S.publicAddr.clear(); S.listening = false;
  S.gw = net::upnp::Gateway{}; S.mappedPort = 0; S.gen++;
  S.desyncs = 0; S.checks = 0; S.waitAcc = 0; S.waitN = 0; S.waitMs = 0; S.msg.clear();
  setCur(Frame{});
}

auto envDelay() -> u32 {
  const char* e = std::getenv("KESTREL_NETPLAY_DELAY");
  u32 d = e && *e ? (u32)std::strtoul(e, nullptr, 0) : 2;
  return d < 1 ? 1 : d > kMaxDelay ? kMaxDelay : d;
}

}  // namespace

// --- API ---------------------------------------------------------------------------------

auto host(System& sys, u16 port, std::string& err, const std::string& relay) -> bool {
  stop();
  if(!port) port = kDefaultPort;
  // La sala se abre antes de tomar mu: es red y puede tardar.
  net::TcpConn ctl;
  std::string code;
  if(!relay.empty() && !net::relay::openRoom(relay, ctl, code, err)) {
    err = "rele " + relay + ": " + err;
    return false;
  }
  std::lock_guard<std::mutex> lk(S.mu);
  resetSession();
  S.sys = &sys;
  S.listening = S.server.listen(port, true);
  // Sin rele el puerto propio es la unica entrada; con rele, solo un extra para la red local.
  if(!S.listening && relay.empty()) {
    err = "no se puede escuchar en el puerto " + std::to_string(port);
    return false;
  }
  S.romHash = romHashOf(sys);
  S.on = true; S.isHost = true; S.slot = 0;
  S.delay = envDelay();
  S.peerName = std::to_string(port);
  if(S.listening) S.acceptTh = std::thread(acceptLoop);
  const std::string d = " (retardo " + std::to_string(S.delay) + " campos)";
  if(!relay.empty()) {
    S.relayCtl = std::move(ctl);
    S.relayAddr = relay;
    S.room = code;
    S.relayTh = std::thread(relayLoop);
    note("partida abierta en el rele " + relay + ", sala " + code + ": los invitados entran con " +
         relay + "/" + code + d);
  } else {
    note("partida abierta en el puerto " + S.peerName + d);
    const char* u = std::getenv("KESTREL_NETPLAY_UPNP");
    if(!(u && u[0] == '0')) std::thread(upnpTask, S.gen, port).detach();
  }
  return true;
}

auto join(System& sys, const std::string& addr, std::string& err) -> bool {
  stop();
  std::string h = addr;
  u16 port = kDefaultPort;
  auto l = std::make_unique<Link>();
  if(auto sl = addr.find('/'); sl != std::string::npos) {
    // rele[:puerto]/CODIGO: el rele empalma con el anfitrion y desde ahi es como directo.
    const std::string rel = addr.substr(0, sl);
    std::string code = addr.substr(sl + 1);
    for(char& ch : code) ch = (char)std::toupper((unsigned char)ch);
    l->conn = net::relay::joinRoom(rel, code, err);
    if(!l->conn.valid()) { err = "rele " + rel + ": " + err; return false; }
    h = rel + "/" + code;
  } else {
    if(auto c = addr.rfind(':'); c != std::string::npos) {
      h = addr.substr(0, c);
      port = (u16)std::atoi(addr.c_str() + c + 1);
    }
    if(h.empty()) h = "127.0.0.1";
    l->conn = net::tcpConnect(h, port, err);
    if(!l->conn.valid()) return false;
    h += ":" + std::to_string(port);
  }
  W w; w.b32(kMagic); w.b32(kVersion); w.b64(romHashOf(sys));
  std::string t; std::vector<u8> b;
  if(!l->send("hello", w) || !l->conn.recvFrame(t, b)) { err = "el anfitrion no responde"; return false; }
  if(t == "reject") { err = "rechazado: " + std::string(b.begin(), b.end()); return false; }
  R r{b};
  const u32 slot = r.b32(), d = r.b32();
  if(t != "welcome" || !r.ok || slot < 1 || slot > 3) { err = "respuesta invalida del anfitrion"; return false; }
  std::lock_guard<std::mutex> lk(S.mu);
  resetSession();
  S.sys = &sys;
  S.on = true; S.isHost = false; S.slot = (int)slot; S.delay = d;
  S.peerName = h;
  l->slot = (int)slot;
  S.up = std::move(l);
  S.up->rx = std::thread(clientRx, S.up.get());
  note("conectado a " + S.peerName + " como jugador " + std::to_string(slot + 1) + "; esperando el estado");
  return true;
}

auto stop() -> void {
  std::vector<std::unique_ptr<Link>> peers;
  std::unique_ptr<Link> up;
  std::thread acc, rel;
  net::upnp::Gateway gw;
  u16 mapped = 0;
  {
    std::lock_guard<std::mutex> lk(S.mu);
    if(!S.on) return;
    S.closing = true;
    g_lockstep.store(false);
    S.server.close();
    S.relayCtl.close();                           // despierta a relayLoop y cierra la sala
    rel = std::move(S.relayTh);
    gw = S.gw; mapped = S.mappedPort;
    for(auto& l : S.peers) { if(l->alive) l->send("bye", W{}); l->kill(); }
    if(S.up) { S.up->send("bye", W{}); S.up->kill(); }
    peers = std::move(S.peers);
    up = std::move(S.up);
    acc = std::move(S.acceptTh);
    S.cv.notify_all();
  }
  if(acc.joinable()) acc.join();
  if(rel.joinable()) rel.join();
  if(gw.ok && mapped) net::upnp::deleteMapping(gw, mapped);
  for(auto& l : peers) if(l->rx.joinable()) l->rx.join();
  if(up && up->rx.joinable()) up->rx.join();
  {
    std::lock_guard<System::CoreMutex> ck(S.sys->coreMutex);
    std::lock_guard<std::mutex> lk(S.mu);
    if(S.savedValid) { applyPadCfg(S.saved); S.savedValid = false; }
    S.on = false;
    resetSession();
  }
  log("sesion cerrada");
}

auto status() -> Status {
  std::lock_guard<std::mutex> lk(S.mu);
  Status s;
  s.active = S.on; s.host = S.isHost; s.synced = S.synced;
  s.slot = S.slot; s.delay = S.delay; s.field = S.field; s.epoch = S.epoch;
  s.desyncs = S.desyncs; s.checks = S.checks; s.waitMs = S.waitMs; s.peer = S.peerName; s.msg = S.msg;
  int n = 0; for(int i = 0; i < 4; i++) n += S.mask >> i & 1;
  s.players = n;
  s.view = g_view.load(std::memory_order_relaxed);
  s.room = S.room; s.publicAddr = S.publicAddr;
  return s;
}

auto host(u16 port, std::string& err, const std::string& relay) -> bool {
  if(!g_sys) { err = "emulador sin arrancar"; return false; }
  return host(*g_sys, port, err, relay);
}

auto join(const std::string& addr, std::string& err) -> bool {
  if(!g_sys) { err = "emulador sin arrancar"; return false; }
  return join(*g_sys, addr, err);
}

auto setView(int mode, std::string& err) -> bool {
  std::lock_guard<std::mutex> lk(S.mu);
  if(!S.on || !S.isHost) { err = "la vista la elige el anfitrion de una partida en red"; return false; }
  if(mode < kViewFull || mode > kViewAuto) { err = "vista desconocida"; return false; }
  if(mode == kViewAuto) {
    int n = 0; for(int i = 0; i < 4; i++) n += S.mask >> i & 1;
    mode = n > 2 ? kViewQuad : kViewRows;
  }
  S.hostView = (u8)mode;
  static const char* kName[4] = {"completa", "franjas", "columnas", "cuadrantes"};
  note(std::string("pantalla ") + kName[mode] + " desde el campo " + std::to_string(S.field + S.delay + 1));
  return true;
}

auto toggleView(std::string& err) -> bool {
  u8 cur;
  { std::lock_guard<std::mutex> lk(S.mu); cur = S.hostView; }
  return setView(cur == kViewFull ? kViewAuto : kViewFull, err);
}

auto startFromEnv(System& sys) -> void {
  g_sys = &sys;
  std::string err;
  if(const char* h = std::getenv("KESTREL_NETPLAY_HOST"); h && *h) {
    const char* r = std::getenv("KESTREL_NETPLAY_RELAY");
    if(!host(sys, (u16)std::atoi(h), err, r ? r : "")) log("error: " + err);
  } else if(const char* j = std::getenv("KESTREL_NETPLAY_JOIN"); j && *j) {
    if(!join(sys, j, err)) log("error: " + err);
  }
}

auto serviceSync(System& sys) -> void {
  std::lock_guard<std::mutex> lk(S.mu);
  if(!S.on || !S.isHost || S.closing) return;
  S.mask = (u8)((S.mask | S.pendingMask | 1u));
  S.pendingMask = 0;
  u8 alive = 1;
  for(auto& l : S.peers) if(l->alive) alive |= (u8)(1u << l->slot);
  S.mask &= alive;
  S.epoch++;
  S.field = 0;
  S.ready.clear(); S.partial.clear(); S.partialView.clear(); S.myHash.clear(); S.peerHash.clear();
  // Los conectores de la partida, enchufados en todas las maquinas igual. Lo demas
  // (raton, accesorio) es lo que tenga el anfitrion.
  savePadCfg();
  PadCfg cfg[4];
  for(int i = 0; i < 4; i++) {
    const auto& pp = sys.memory.padPort[i];
    cfg[i] = {(S.mask >> i & 1) != 0, pp.mouse, pp.accessory};
  }
  applyPadCfg(cfg);
  std::vector<u8> state;
  captureState(sys, state);
  W w;
  w.b32(S.epoch); w.b32(S.delay); w.b8(S.mask);
  for(auto& c : cfg) { w.b8(c.connected); w.b8(c.mouse); w.b8(c.accessory); }
  w.v.insert(w.v.end(), state.begin(), state.end());
  for(auto& l : S.peers) if(l->alive) l->send("sync", w);
  // El anfitrion arranca del estado RESTAURADO, igual que los invitados: el vivo puede
  // llevar cosas que la foto no guarda.
  std::string err;
  if(!restoreState(sys, state.data(), state.size(), err)) note("fallo al recargar el estado propio: " + err);
  sys.rewinder.clear();
  for(u64 f = 0; f < S.delay; f++) S.ready.emplace(f, viewFrame());
  S.synced = true;
  g_lockstep.store(true);
  g_viewSlot.store(0);
  setCur(S.ready[0]);
  hostSubmit(S.delay, localPad(sys));
  int n = 0; for(int i = 0; i < 4; i++) n += S.mask >> i & 1;
  note("sincronizado: " + std::to_string(n) + " jugadores, estado de " +
       std::to_string(state.size() / 1024) + " KiB (epoca " + std::to_string(S.epoch) + ")");
}

auto afterField(System& sys) -> void {
  std::unique_lock<std::mutex> lk(S.mu);
  if(!S.on) return;
  if(S.lost) {
    // Invitado sin anfitrion: sigue jugando solo con la maquina tal cual esta.
    if(S.synced) { S.synced = false; g_lockstep.store(false); g_viewSlot.store(-1); }
    return;
  }
  if(!S.isHost && (S.syncReady || !S.synced)) {
    // Invitado recien llegado: nada que jugar hasta tener el estado del anfitrion.
    while(!S.syncReady && !S.lost && !S.closing && !sys.shutdown.load())
      S.cv.wait_for(lk, std::chrono::milliseconds(100));
    if(S.syncReady) clientApplySync(lk);
    return;
  }
  if(!S.synced) return;                          // anfitrion sin invitados todavia

  S.field++;
  const u64 f = S.field;
  const Pad me = localPad(sys);
  if(S.isHost) hostSubmit(f + S.delay, me);
  else { W w; w.b32(S.epoch); w.b64(f + S.delay); w.pad(me); S.up->send("in", w); }

  if(f % kHashEvery == 0) {
    const u64 h = cpuHash(sys);
    if(S.isHost) {
      S.myHash[f] = h;
      if(S.myHash.size() > 32) S.myHash.erase(S.myHash.begin());
      for(usize i = 0; i < S.peerHash.size();) {
        if(S.peerHash[i].f == f) { compareHash(S.peerHash[i].slot, f, S.peerHash[i].h, h);
                                   S.peerHash.erase(S.peerHash.begin() + (std::ptrdiff_t)i); }
        else if(S.peerHash[i].f < f) S.peerHash.erase(S.peerHash.begin() + (std::ptrdiff_t)i);
        else i++;
      }
    } else {
      W w; w.b32(S.epoch); w.b64(f); w.b64(h); S.up->send("h", w);
    }
  }

  // Esperar las entradas de este campo. Es la unica espera del lockstep: si la red va
  // dentro del retardo, aqui no se para nunca.
  const auto t0 = std::chrono::steady_clock::now();
  auto warnAt = t0 + std::chrono::seconds(5);
  while(!S.ready.count(f) && !S.syncReady && !S.lost && !S.closing && !sys.shutdown.load()) {
    S.cv.wait_for(lk, std::chrono::milliseconds(100));
    if(std::chrono::steady_clock::now() > warnAt) {
      note("esperando a la red en el campo " + std::to_string(f));
      warnAt += std::chrono::seconds(5);
    }
  }
  S.waitAcc += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  if(++S.waitN == 60) { S.waitMs = S.waitAcc / 60; S.waitAcc = 0; S.waitN = 0; }
  if(!S.isHost && S.syncReady) { clientApplySync(lk); return; }
  auto it = S.ready.find(f);
  if(it == S.ready.end()) return;
  setCur(it->second);
  S.ready.erase(S.ready.begin(), it);            // lo anterior ya no hace falta
}

}  // namespace kestrel::netplay
