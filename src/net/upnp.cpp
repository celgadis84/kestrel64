#include "upnp.hpp"

#if defined(_WIN32)
  #include <winsock2.h>
  #include <ws2tcpip.h>
  static auto closesock(long long s) -> void { closesocket((SOCKET)s); }
  static constexpr long long BADSOCK = (long long)INVALID_SOCKET;
#else
  #include <sys/socket.h>
  #include <sys/select.h>
  #include <netinet/in.h>
  #include <netdb.h>
  #include <arpa/inet.h>
  #include <unistd.h>
  static auto closesock(long long s) -> void { ::close((int)s); }
  static constexpr long long BADSOCK = -1;
#endif

#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>

namespace kestrel::net::upnp {

namespace {

auto wsa() -> bool {
#if defined(_WIN32)
  static const bool ok = []{ WSADATA w; return WSAStartup(MAKEWORD(2, 2), &w) == 0; }();
  return ok;
#else
  return true;
#endif
}

auto lower(std::string s) -> std::string {
  for(char& c : s) c = (char)std::tolower((unsigned char)c);
  return s;
}

// Espera a que `s` tenga algo que leer; false al agotar `ms`.
auto readable(long long s, int ms) -> bool {
  fd_set f;
  FD_ZERO(&f);
#if defined(_WIN32)
  FD_SET((SOCKET)s, &f);
#else
  FD_SET((int)s, &f);
#endif
  timeval tv{ms / 1000, (ms % 1000) * 1000};
  return ::select((int)s + 1, &f, nullptr, nullptr, &tv) > 0;
}

struct Url {
  std::string host, path;
  u16 port = 80;
};

// Solo http:// (UPnP no usa otra cosa en la red local).
auto parseUrl(const std::string& u, Url& out) -> bool {
  if(lower(u.substr(0, 7)) != "http://") return false;
  const size_t s = u.find('/', 7);
  const std::string hp = u.substr(7, s == std::string::npos ? std::string::npos : s - 7);
  out.path = s == std::string::npos ? "/" : u.substr(s);
  const size_t c = hp.rfind(':');
  out.host = c == std::string::npos ? hp : hp.substr(0, c);
  out.port = 80;
  if(c != std::string::npos) out.port = (u16)std::atoi(hp.c_str() + c + 1);
  return !out.host.empty() && out.port != 0;
}

// Cuerpo de una respuesta HTTP/1.1 (con o sin "chunked"). Devuelve el codigo de estado.
auto http(const Url& u, const std::string& method, const std::string& extraHdr,
          const std::string& body, std::string& resp, std::string* localIp = nullptr) -> int {
  resp.clear();
  addrinfo hints{}, *res = nullptr;
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;
  const std::string ps = std::to_string(u.port);
  if(::getaddrinfo(u.host.c_str(), ps.c_str(), &hints, &res) != 0 || !res) return -1;
  long long s = (long long)::socket(AF_INET, SOCK_STREAM, 0);
  if(s == BADSOCK) { ::freeaddrinfo(res); return -1; }
  const int ok = ::connect((int)s, res->ai_addr, (int)res->ai_addrlen);
  ::freeaddrinfo(res);
  if(ok != 0) { closesock(s); return -1; }
  if(localIp) {
    sockaddr_in me{};
    socklen_t ml = sizeof me;
    if(::getsockname((int)s, (sockaddr*)&me, &ml) == 0) {
      char b[INET_ADDRSTRLEN] = {};
      ::inet_ntop(AF_INET, &me.sin_addr, b, sizeof b);
      *localIp = b;
    }
  }
  std::string req = method + " " + u.path + " HTTP/1.1\r\nHost: " + u.host + ":" + ps +
                    "\r\nConnection: close\r\n" + extraHdr;
  if(!body.empty() || method == "POST") req += "Content-Length: " + std::to_string(body.size()) + "\r\n";
  req += "\r\n" + body;
  for(size_t off = 0; off < req.size();) {
    const int n = ::send((int)s, req.data() + off, (int)(req.size() - off), 0);
    if(n <= 0) { closesock(s); return -1; }
    off += (size_t)n;
  }
  std::string raw;
  char buf[4096];
  const auto t0 = std::chrono::steady_clock::now();
  while(std::chrono::steady_clock::now() - t0 < std::chrono::seconds(5) && readable(s, 1000)) {
    const int n = ::recv((int)s, buf, sizeof buf, 0);
    if(n <= 0) break;
    raw.append(buf, (size_t)n);
    if(raw.size() > 1u << 20) break;   // una descripcion de router no pasa de unos KB
  }
  closesock(s);
  const size_t he = raw.find("\r\n\r\n");
  if(he == std::string::npos || raw.size() < 12) return -1;
  const int code = std::atoi(raw.c_str() + 9);
  const std::string hdr = lower(raw.substr(0, he));
  std::string b = raw.substr(he + 4);
  if(hdr.find("transfer-encoding: chunked") != std::string::npos) {
    std::string out;
    size_t p = 0;
    for(;;) {
      const size_t e = b.find("\r\n", p);
      if(e == std::string::npos) break;
      const size_t len = std::strtoul(b.c_str() + p, nullptr, 16);
      if(len == 0 || e + 2 + len > b.size()) break;
      out.append(b, e + 2, len);
      p = e + 2 + len + 2;
    }
    b = out;
  }
  resp = b;
  return code;
}

// Contenido de la primera <tag>...</tag> a partir de `from` (sin prefijos de espacio de nombres).
auto tagText(const std::string& xml, const std::string& tag, size_t from = 0, size_t* end = nullptr) -> std::string {
  const std::string lx = lower(xml), lt = lower(tag);
  size_t p = from;
  for(;; p += lt.size()) {
    p = lx.find(lt, p);
    if(p == std::string::npos || p == 0) return {};
    // Etiqueta de apertura: delante '<' o ':' (prefijo), detras '>' o espacio.
    const char before = lx[p - 1], after = p + lt.size() < lx.size() ? lx[p + lt.size()] : 0;
    if((before != '<' && before != ':') || (after != '>' && after != ' ')) continue;
    if(before == ':') {
      const size_t lt0 = lx.rfind('<', p);
      if(lt0 == std::string::npos || lx[lt0 + 1] == '/' || lx.find_first_of("> ", lt0) < p) continue;
    }
    const size_t gt = lx.find('>', p);
    if(gt == std::string::npos) return {};
    const size_t close = lx.find("</", gt);
    if(close == std::string::npos) return {};
    if(end) *end = close;
    std::string v = xml.substr(gt + 1, close - gt - 1);
    while(!v.empty() && std::isspace((unsigned char)v.front())) v.erase(v.begin());
    while(!v.empty() && std::isspace((unsigned char)v.back())) v.pop_back();
    return v;
  }
}

auto soap(const Gateway& g, const std::string& action, const std::string& args, std::string& resp) -> int {
  Url u;
  if(!parseUrl(g.controlUrl, u)) return -1;
  const std::string body =
      "<?xml version=\"1.0\"?>\r\n"
      "<s:Envelope xmlns:s=\"http://schemas.xmlsoap.org/soap/envelope/\" "
      "s:encodingStyle=\"http://schemas.xmlsoap.org/soap/encoding/\"><s:Body>"
      "<u:" + action + " xmlns:u=\"" + g.service + "\">" + args + "</u:" + action + ">"
      "</s:Body></s:Envelope>\r\n";
  const std::string hdr = "Content-Type: text/xml; charset=\"utf-8\"\r\nSOAPAction: \"" + g.service +
                          "#" + action + "\"\r\n";
  return http(u, "POST", hdr, body, resp);
}

}  // namespace

auto discover(int waitMs) -> Gateway {
  Gateway g;
  if(!wsa()) { g.err = "WSAStartup fallo"; return g; }
  long long s = (long long)::socket(AF_INET, SOCK_DGRAM, 0);
  if(s == BADSOCK) { g.err = "sin socket UDP"; return g; }
  sockaddr_in to{};
  to.sin_family = AF_INET;
  to.sin_port = htons(1900);
  ::inet_pton(AF_INET, "239.255.255.250", &to.sin_addr);
  // Se pregunta por los dos tipos de router: IGD v1 es lo comun; los v2 tambien responden a v1
  // casi siempre, pero no todos.
  static const char* const kSt[] = {"urn:schemas-upnp-org:device:InternetGatewayDevice:1",
                                    "urn:schemas-upnp-org:device:InternetGatewayDevice:2"};
  for(const char* st : kSt) {
    const std::string m = std::string("M-SEARCH * HTTP/1.1\r\nHOST: 239.255.255.250:1900\r\n"
                                      "MAN: \"ssdp:discover\"\r\nMX: 2\r\nST: ") + st + "\r\n\r\n";
    ::sendto((int)s, m.data(), (int)m.size(), 0, (sockaddr*)&to, sizeof to);
  }
  std::string location;
  const auto t0 = std::chrono::steady_clock::now();
  for(;;) {
    const int left = waitMs - (int)std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::steady_clock::now() - t0).count();
    if(left <= 0 || !readable(s, left)) break;
    char buf[2048];
    const int n = ::recv((int)s, buf, sizeof buf - 1, 0);
    if(n <= 0) break;
    const std::string r(buf, (size_t)n), lr = lower(r);
    const size_t p = lr.find("\r\nlocation:");
    if(p == std::string::npos) continue;
    size_t b = p + 11;
    while(b < r.size() && r[b] == ' ') b++;
    const size_t e = r.find("\r\n", b);
    location = r.substr(b, e == std::string::npos ? std::string::npos : e - b);
    break;
  }
  closesock(s);
  if(location.empty()) { g.err = "ningun router responde por UPnP"; return g; }

  Url u;
  if(!parseUrl(location, u)) { g.err = "LOCATION rara: " + location; return g; }
  std::string desc;
  if(http(u, "GET", "", "", desc, &g.localIp) != 200) { g.err = "no baja la descripcion del router"; return g; }

  // Primer servicio WAN de conexion (IP o PPP) y su controlURL.
  static const char* const kSvc[] = {"urn:schemas-upnp-org:service:WANIPConnection:2",
                                     "urn:schemas-upnp-org:service:WANIPConnection:1",
                                     "urn:schemas-upnp-org:service:WANPPPConnection:1"};
  for(const char* svc : kSvc) {
    const size_t at = desc.find(std::string(">") + svc + "<");
    if(at == std::string::npos) continue;
    std::string ctl = tagText(desc, "controlURL", at);
    if(ctl.empty()) continue;
    if(lower(ctl.substr(0, 7)) != "http://") {
      // Relativa: contra URLBase si la hay, si no contra el host de LOCATION.
      std::string base = tagText(desc, "URLBase");
      if(base.empty()) base = "http://" + u.host + ":" + std::to_string(u.port);
      while(!base.empty() && base.back() == '/') base.pop_back();
      if(ctl.front() != '/') ctl = "/" + ctl;
      ctl = base + ctl;
    }
    g.controlUrl = ctl;
    g.service = svc;
    g.ok = true;
    return g;
  }
  g.err = "el router no ofrece WANIPConnection/WANPPPConnection";
  return g;
}

auto externalIp(const Gateway& g, std::string& ip) -> bool {
  std::string r;
  if(soap(g, "GetExternalIPAddress", "", r) != 200) return false;
  ip = tagText(r, "NewExternalIPAddress");
  return !ip.empty();
}

auto addMapping(const Gateway& g, u16 port, std::string& err) -> bool {
  const std::string ps = std::to_string(port);
  for(const char* lease : {"7200", "0"}) {
    const std::string args = "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + ps +
                             "</NewExternalPort><NewProtocol>TCP</NewProtocol><NewInternalPort>" + ps +
                             "</NewInternalPort><NewInternalClient>" + g.localIp +
                             "</NewInternalClient><NewEnabled>1</NewEnabled>"
                             "<NewPortMappingDescription>kestrel64 netplay</NewPortMappingDescription>"
                             "<NewLeaseDuration>" + lease + "</NewLeaseDuration>";
    std::string r;
    const int code = soap(g, "AddPortMapping", args, r);
    if(code == 200) return true;
    const std::string ec = tagText(r, "errorCode");
    // 725 = OnlyPermanentLeasesSupported: se reintenta sin plazo.
    if(ec == "725") continue;
    err = "AddPortMapping rechazado (HTTP " + std::to_string(code) + (ec.empty() ? "" : ", UPnP " + ec) +
          ": " + tagText(r, "errorDescription") + ")";
    return false;
  }
  err = "AddPortMapping rechazado";
  return false;
}

auto deleteMapping(const Gateway& g, u16 port) -> void {
  const std::string ps = std::to_string(port);
  std::string r;
  soap(g, "DeletePortMapping",
       "<NewRemoteHost></NewRemoteHost><NewExternalPort>" + ps + "</NewExternalPort><NewProtocol>TCP</NewProtocol>", r);
}

}  // namespace kestrel::net::upnp
