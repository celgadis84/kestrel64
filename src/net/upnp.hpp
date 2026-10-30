#pragma once
// kestrel64 - apertura automatica de puerto en el router por UPnP IGD.
//
// Para jugar por internet sin rele, el anfitrion necesita que su router reenvie el puerto TCP
// de la partida a su maquina. La mayoria de routers domesticos lo hacen solos si se les pide
// por UPnP: busqueda SSDP en la red local, descripcion XML del router y una orden SOAP
// AddPortMapping al servicio WANIPConnection (o WANPPPConnection). De paso se pregunta la IP
// publica para que el anfitrion la pueda dar.
//
// Todo bloqueante (unos segundos como mucho): se llama desde un hilo aparte.

#include "../core/types.hpp"
#include <string>

namespace kestrel::net::upnp {

struct Gateway {
  bool ok = false;
  std::string controlUrl;   // http://ip:puerto/ruta del servicio WAN
  std::string service;      // urn:schemas-upnp-org:service:WANIPConnection:1 (o PPP)
  std::string localIp;      // IP de esta maquina hacia el router
  std::string err;
};

// Busca el router (SSDP, espera `waitMs`) y su servicio WAN.
auto discover(int waitMs = 2000) -> Gateway;
auto externalIp(const Gateway& g, std::string& ip) -> bool;
// Reenvia `port` TCP del router a localIp:port. Primero con plazo de 2 h; si el router solo
// admite plazo indefinido (error 725), sin plazo.
auto addMapping(const Gateway& g, u16 port, std::string& err) -> bool;
auto deleteMapping(const Gateway& g, u16 port) -> void;

}  // namespace kestrel::net::upnp
