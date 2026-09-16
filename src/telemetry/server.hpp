#pragma once
// kestrel64 — telemetry server. Length-framed JSON(+binary) over TCP; the in-core
// half of the MCP bridge. Runs on its own thread, dispatches command requests
// against the live System state.

#include "../core/types.hpp"
#include "../net/tcp.hpp"
#include "json.hpp"
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

namespace kestrel {

struct System;

namespace telemetry {

struct Server {
  explicit Server(System& sys) : system(sys) {}

  // Bind + serve on 127.0.0.1:port. Blocks (run this on a dedicated thread) until
  // stop() is called or the system shuts down. Returns after the socket closes.
  auto serve(u16 port) -> void;
  auto stop() -> void;

private:
  System& system;
  net::TcpServer tcp;
  std::atomic<bool> stopping{false};

  // Un hilo por cliente (el puente MCP, scripts, el lanzador...), pero el DESPACHO va
  // serializado: cada orden ve el mismo estado consistente que cuando solo habia un
  // cliente. Las conexiones vivas se guardan para poder cerrarlas desde stop() y
  // despertar a los hilos que estan bloqueados en recv.
  std::mutex dispatchMx;
  std::mutex clientsMx;
  std::vector<std::shared_ptr<net::TcpConn>> clients;

  // Atiende una conexion hasta que se cierra. Corre en su propio hilo.
  auto serveClient(std::shared_ptr<net::TcpConn> conn) -> void;

  // Dispatch one request; fills `reply` JSON and optional `blob`.
  auto dispatch(const json::Value& req, json::Value& reply, std::vector<u8>& blob) -> void;

  // Command handlers.
  auto cmdStatus(const json::Value& args, json::Value& data) -> void;
  auto cmdMemRegions(const json::Value& args, json::Value& data) -> void;
  auto cmdMemRead(const json::Value& args, json::Value& data, std::vector<u8>& blob) -> bool;
  auto cmdMemWrite(const json::Value& args, const std::vector<u8>& blob, json::Value& data) -> bool;

  // CPU control/inspection (all take coreMutex).
  auto cmdCpuRegs(const json::Value& args, json::Value& data) -> void;
  auto cmdCpuStep(const json::Value& args, json::Value& data) -> void;
  auto cmdCpuDisasm(const json::Value& args, json::Value& data) -> bool;
  auto cmdRunControl(const std::string& cmd, json::Value& data) -> void;
  auto cmdFrameAdvance(const json::Value& args, json::Value& data) -> void;
  auto cmdRewind(const json::Value& args, json::Value& data) -> bool;
  auto cmdState(const std::string& cmd, const json::Value& args, json::Value& data) -> bool;
  auto cmdRcpRegs(const json::Value& args, json::Value& data) -> void;
  auto cmdRspRegs(const json::Value& args, json::Value& data) -> void;
  // Decode the live VI framebuffer to RGBA8888 (blob) + geometry (data).
  auto cmdViCapture(const json::Value& args, json::Value& data, std::vector<u8>& blob) -> bool;
  auto cmdBpAdd(const json::Value& args, json::Value& data) -> void;
  auto cmdBpDel(const json::Value& args, json::Value& data) -> void;
  auto cmdBpList(const json::Value& args, json::Value& data) -> void;
  auto cmdRunUntil(const json::Value& args, json::Value& data) -> void;
  auto cmdProfControl(const std::string& cmd, json::Value& data) -> void;
  auto cmdProfCpu(const json::Value& args, json::Value& data) -> void;
  auto cmdProfRsp(const json::Value& args, json::Value& data) -> void;
  auto cmdPad(const std::string& cmd, const json::Value& args, json::Value& data) -> bool;
};

}  // namespace telemetry
}  // namespace kestrel
