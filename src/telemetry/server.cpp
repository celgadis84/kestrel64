#include "server.hpp"
#include "../core/savestate.hpp"
#include "../core/system.hpp"
#include "../core/movie.hpp"
#include "../vrdp/vrdp.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

namespace kestrel::telemetry {

auto Server::stop() -> void {
  stopping.store(true);
  tcp.close();
  // Cerrar tambien las conexiones vivas: sus hilos estan bloqueados en recv y solo
  // despiertan si el socket se les cae debajo.
  std::vector<std::shared_ptr<net::TcpConn>> snap;
  { std::lock_guard<std::mutex> lk(clientsMx); snap = clients; }
  for(auto& c : snap) c->close();
}

auto Server::serveClient(std::shared_ptr<net::TcpConn> conn) -> void {
  for(;;) {
    std::string reqJson;
    std::vector<u8> reqBlob;
    if(!conn->recvFrame(reqJson, reqBlob)) break;

    bool ok = false;
    json::Value req = json::parse(reqJson, ok);
    json::Value reply = json::Value::object();
    std::vector<u8> blob;

    if(!ok || !req.isObject()) {
      reply.set("ok", false).set("error", "malformed request JSON");
    } else {
      reply.set("id", req.get("id"));
      // Una orden cada vez contra el estado vivo, igual que cuando solo se atendia a un
      // cliente: lo unico que pasa a ir en paralelo es esperar en la red.
      std::lock_guard<std::mutex> lk(dispatchMx);
      dispatch(req, reply, blob);
    }

    if(!conn->sendFrame(json::dump(reply), blob.empty() ? nullptr : blob.data(), (u32)blob.size()))
      break;
  }
  conn->close();
  std::lock_guard<std::mutex> lk(clientsMx);
  clients.erase(std::remove(clients.begin(), clients.end(), conn), clients.end());
}

auto Server::serve(u16 port) -> void {
  if(!tcp.listen(port)) {
    std::fprintf(stderr, "[telemetry] failed to bind port %u\n", port);
    return;
  }
  std::printf("[telemetry] listening on 127.0.0.1:%u\n", port);
  std::fflush(stdout);

  // Un hilo por cliente. Hacen falta varios a la vez porque el puente MCP deja su
  // conexion abierta toda la sesion: con un solo cliente, cualquier otra herramienta
  // (scripts/pad.py, el lanzador) se quedaba esperando en el accept para siempre.
  std::vector<std::thread> workers;
  while(!stopping.load() && !system.shutdown.load()) {
    net::TcpConn c = tcp.accept();
    if(!c.valid()) break;  // listen socket closed -> shutting down

    auto conn = std::make_shared<net::TcpConn>(std::move(c));
    { std::lock_guard<std::mutex> lk(clientsMx); clients.push_back(conn); }
    workers.emplace_back([this, conn] { serveClient(conn); });
  }

  stop();
  for(auto& t : workers) if(t.joinable()) t.join();
}

auto Server::dispatch(const json::Value& req, json::Value& reply, std::vector<u8>& blob) -> void {
  std::string cmd = req.get("cmd").asString();
  json::Value args = req.get("args");
  json::Value data = json::Value::object();

  auto fail = [&](const std::string& msg) { reply.set("ok", false).set("error", msg); };
  auto done = [&]() { reply.set("ok", true).set("data", data); };

  if(cmd == "ping") {
    data.set("pong", true);
    done();
  } else if(cmd == "status") {
    cmdStatus(args, data); done();
  } else if(cmd == "mem.regions") {
    cmdMemRegions(args, data); done();
  } else if(cmd == "mem.read") {
    if(cmdMemRead(args, data, blob)) done(); else fail("mem.read: bad region or range");
  } else if(cmd == "mem.write") {
    // request blob is not threaded here in M0; write uses args-encoded bytes instead.
    if(cmdMemWrite(args, blob, data)) done(); else fail("mem.write: bad region or range");
  } else if(cmd == "cpu.regs") {
    cmdCpuRegs(args, data); done();
  } else if(cmd == "cpu.step") {
    cmdCpuStep(args, data); done();
  } else if(cmd == "cpu.disasm") {
    if(cmdCpuDisasm(args, data)) done(); else fail("cpu.disasm: bad range");
  } else if(cmd == "pause" || cmd == "resume" || cmd == "reset") {
    cmdRunControl(cmd, data); done();
  } else if(cmd == "frame.advance") {
    cmdFrameAdvance(args, data); done();
  } else if(cmd == "rewind.step") {
    if(cmdRewind(args, data)) done(); else fail(data.has("msg") ? data.get("msg").asString() : "rewind");
  } else if(cmd == "rcp.regs") {
    cmdRcpRegs(args, data); done();
  } else if(cmd == "rsp.regs") {
    cmdRspRegs(args, data); done();
  } else if(cmd == "vi.capture") {
    if(cmdViCapture(args, data, blob)) done(); else fail("vi.capture: VI blanked or bad geometry");
  } else if(cmd == "cpu.bp.add") {
    cmdBpAdd(args, data); done();
  } else if(cmd == "cpu.bp.del") {
    cmdBpDel(args, data); done();
  } else if(cmd == "cpu.bp.list") {
    cmdBpList(args, data); done();
  } else if(cmd == "cpu.run_until") {
    cmdRunUntil(args, data); done();
  } else if(cmd == "prof.start" || cmd == "prof.stop" || cmd == "prof.reset") {
    cmdProfControl(cmd, data); done();
  } else if(cmd == "prof.cpu") {
    cmdProfCpu(args, data); done();
  } else if(cmd == "prof.rsp") {
    cmdProfRsp(args, data); done();
  } else if(cmd == "pad.set" || cmd == "pad.get") {
    if(cmdPad(cmd, args, data)) done(); else fail(data.get("msg").asString());
  } else if(cmd == "state.save" || cmd == "state.load") {
    if(cmdState(cmd, args, data)) done(); else fail(data.get("msg").asString());
  } else {
    fail("unknown command: " + cmd);
  }
}

auto Server::cmdStatus(const json::Value&, json::Value& data) -> void {
  data.set("emulator", "kestrel64");
  data.set("version", System::kVersion);
  data.set("system", "Nintendo 64");
  data.set("paused", (bool)system.paused.load());
  // Realtime speed, expressed as % of a real N64 (100 = full console speed),
  // per hardware domain. Overclock multipliers are the tunable targets.
  {
    json::Value sp = json::Value::object();
    sp.set("cpuPct",   system.n64SpeedPct.load(std::memory_order_relaxed));
    sp.set("rspPct",   system.rspSpeedPct.load(std::memory_order_relaxed));
    sp.set("rdramPct", system.rdramSpeedPct.load(std::memory_order_relaxed));
    sp.set("insns",    (u64)system.retiredInsns.load(std::memory_order_relaxed));
    json::Value oc = json::Value::object();
    oc.set("cpu",   system.clocks.cpuOc);
    oc.set("rsp",   system.clocks.rspOc);
    oc.set("rdram", system.clocks.rdramOc);
    sp.set("overclock", oc);
    // CPI del modelo: ciclos de CPU por instruccion retirada. 2 = de fabrica (Count avanza un
    // tick por op). Se publica porque cambia el significado de "instrucciones por campo", que
    // es la unidad de todas las medidas de presupuesto.
    sp.set("cpi", (double)system.cpu.cpi256 / 128.0);
    // Ocupacion de los workers: en modo threaded el % de CPU sube cuando la CPU gira
    // esperando al RCP, asi que sin esto el estado enganaria. cpuWait alto = el palo
    // largo es un worker; rdp/rsp altos dicen cual.
    json::Value oc2 = json::Value::object();
    oc2.set("rdpBusyPct", system.rdpBusyPct.load(std::memory_order_relaxed));
    oc2.set("rspBusyPct", system.rspBusyPct.load(std::memory_order_relaxed));
    oc2.set("cpuWaitPct", system.cpuWaitPct.load(std::memory_order_relaxed));
    oc2.set("fps", system.fieldsPerSec.load(std::memory_order_relaxed));
    sp.set("occupancy", oc2);
    data.set("speed", sp);
  }
  // Pelicula TAS en marcha, si la hay. El contador de sondeos es el "numero de fotograma"
  // de una repeticion: sin el, quien graba no puede decir en que punto de la cinta esta ni
  // comprobar que un estado guardado la rebobino con el juego.
  if(system.rewinder.enabled) {
    json::Value rw = json::Value::object();
    rw.set("steps", (u64)system.rewinder.steps());
    rw.set("bytes", (u64)system.rewinder.bytes());
    rw.set("interval", (u64)system.rewinder.interval);
    rw.set("budget", (u64)system.rewinder.budget);
    data.set("rewind", rw);
  }
  if(movie::mode != movie::Mode::Off) {
    json::Value mv = json::Value::object();
    mv.set("mode", std::string(movie::mode == movie::Mode::Rec ? "rec" : "play"));
    mv.set("polls", (u64)movie::polls.load(std::memory_order_relaxed));
    mv.set("total", (u64)movie::total);
    mv.set("ended", movie::ended.load(std::memory_order_relaxed));
    mv.set("path", movie::path);
    data.set("movie", mv);
  }
  // Mando: estado publicado del mando 1 y cuantas veces lo ha leido el juego. La cuenta de
  // sondeos es la unica forma de ver desde fuera si una pulsacion corta se pierde porque el
  // juego no esta preguntando, en vez de suponerlo.
  {
    json::Value pd = json::Value::object();
    pd.set("buttons", (u64)system.memory.padPort[0].buttons);
    pd.set("stickX", (s64)system.memory.padPort[0].stickX);
    pd.set("stickY", (s64)system.memory.padPort[0].stickY);
    pd.set("polls", (u64)system.memory.padPolls.load(std::memory_order_relaxed));
    // Los cuatro conectores, para ver de un vistazo quien esta enchufado y con que.
    json::Value ports = json::Value::array();
    for(const auto& pp : system.memory.padPort) {
      json::Value e = json::Value::object();
      e.set("connected", pp.connected);
      e.set("accessory", (s64)pp.accessory);
      e.set("buttons", (u64)pp.buttons);
      e.set("stickX", (s64)pp.stickX);
      e.set("stickY", (s64)pp.stickY);
      e.set("rumble", pp.rumble);
      ports.push(e);
    }
    pd.set("ports", ports);
    data.set("pad", pd);
  }
  if(system.rom.valid()) {
    json::Value g = json::Value::object();
    g.set("name", system.rom.header.name);
    g.set("entryPoint", system.rom.header.entryPoint);
    g.set("crc1", system.rom.header.crc1);
    g.set("crc2", system.rom.header.crc2);
    char cc[3] = { system.rom.header.cartId[0], system.rom.header.cartId[1], 0 };
    g.set("cartId", std::string(cc));
    const char* ord = system.rom.originalOrder == Rom::Order::Z64 ? "z64" :
                      system.rom.originalOrder == Rom::Order::N64 ? "n64" :
                      system.rom.originalOrder == Rom::Order::V64 ? "v64" : "unknown";
    g.set("romOrder", ord);
    g.set("romBytes", (u64)system.rom.data.size());
    using ST = Memory::SaveType;
    ST st = system.memory.saveType;
    const char* sv = st == ST::Eeprom4k  ? "eeprom4k"  : st == ST::Eeprom16k ? "eeprom16k" :
                     st == ST::Sram256k  ? "sram256k"  : st == ST::Sram768k  ? "sram768k"  :
                     st == ST::Flash1m   ? "flash1m"   : "none";
    g.set("saveType", std::string(sv));
    g.set("saveBytes", (u64)system.memory.saveSize());
    data.set("game", g);
  } else {
    data.set("game", json::Value{});
  }
}

auto Server::cmdMemRegions(const json::Value&, json::Value& data) -> void {
  json::Value list = json::Value::array();
  for(auto& r : system.memory.regions) {
    json::Value e = json::Value::object();
    e.set("name", r.name);
    e.set("base", r.base);
    e.set("size", r.size);
    list.push(std::move(e));
  }
  data.set("regions", list);
}

auto Server::cmdMemRead(const json::Value& args, json::Value& data, std::vector<u8>& blob) -> bool {
  Region* r = system.memory.findRegion(args.get("region").asString());
  if(!r || !r->data) return false;
  u32 addr = args.get("addr").asU32();
  u32 len  = args.has("len") ? args.get("len").asU32() : 64;
  if(addr >= r->size) return false;
  len = std::min(len, r->size - addr);
  // coherent=true reads RDRAM through the CPU's write-back D-cache, so kernel
  // structs the CPU wrote but has not flushed are visible (dirty line shadows RAM).
  bool coherent = args.has("coherent") && args.get("coherent").asU32() != 0;
  if(coherent && r->name == "RDRAM") {
    blob.resize(len);
    for(u32 i = 0; i < len; i++) blob[i] = system.cpu.peekPhysCoherent(addr + i);
  } else {
    blob.assign(r->data + addr, r->data + addr + len);
  }
  data.set("region", r->name);
  data.set("coherent", coherent);
  data.set("addr", addr);
  data.set("len", len);
  data.set("bytes", len);   // blob carries the raw data
  return true;
}

auto Server::cmdMemWrite(const json::Value& args, const std::vector<u8>&, json::Value& data) -> bool {
  Region* r = system.memory.findRegion(args.get("region").asString());
  if(!r || !r->data) return false;
  u32 addr = args.get("addr").asU32();
  if(addr >= r->size) return false;
  // M0: bytes supplied as a JSON array under "data" (hex string or array).
  json::Value bytes = args.get("data");
  u32 written = 0;
  if(bytes.type == json::Value::Type::Array && bytes.arr) {
    for(auto& v : *bytes.arr) {
      if(addr + written >= r->size) break;
      r->data[addr + written] = (u8)v.asU32();
      written++;
    }
  } else if(bytes.isString()) {
    // hex string
    const std::string& h = bytes.str;
    for(usize i = 0; i + 1 < h.size(); i += 2) {
      if(addr + written >= r->size) break;
      auto nib = [](char c) -> int { if(c>='0'&&c<='9')return c-'0'; if(c>='a'&&c<='f')return c-'a'+10; if(c>='A'&&c<='F')return c-'A'+10; return 0; };
      r->data[addr + written] = (u8)(nib(h[i])<<4 | nib(h[i+1]));
      written++;
    }
  }
  data.set("region", r->name);
  data.set("addr", addr);
  data.set("written", written);
  return true;
}

static const char* kGprName[32] = {
  "zero","at","v0","v1","a0","a1","a2","a3","t0","t1","t2","t3","t4","t5","t6","t7",
  "s0","s1","s2","s3","s4","s5","s6","s7","t8","t9","k0","k1","gp","sp","fp","ra"};

auto Server::cmdCpuRegs(const json::Value&, json::Value& data) -> void {
  std::lock_guard<std::mutex> lk(system.coreMutex);
  CPU& c = system.cpu;
  data.set("pc", c.pc);
  data.set("nextPc", c.nextPc);
  data.set("hi", c.hi);
  data.set("lo", c.lo);
  data.set("halted", c.halted);
  if(c.halted) data.set("haltReason", c.haltReason);
  data.set("retired", c.retired);
  json::Value g = json::Value::object();
  for(int i = 0; i < 32; i++) g.set(kGprName[i], c.gpr[i]);
  data.set("gpr", g);
  json::Value c0 = json::Value::object();
  c0.set("Status",  c.cop0[CPU::C0_Status]);
  c0.set("Cause",   c.cop0[CPU::C0_Cause]);
  c0.set("EPC",     c.cop0[CPU::C0_EPC]);
  c0.set("Count",   c.cop0[CPU::C0_Count]);
  c0.set("Compare", c.cop0[CPU::C0_Compare]);
  c0.set("BadVAddr",c.cop0[CPU::C0_BadVAddr]);
  data.set("cop0", c0);
  json::Value f = json::Value::object();
  for(int i = 0; i < 32; i++) { char nm[8]; std::snprintf(nm,sizeof nm,"f%d",i); f.set(nm, c.fpr[i]); }
  data.set("fpr", f);
  // disasm of the instruction at pc
  u32 op = system.memory.read32((u32)c.pc);
  data.set("op", (u64)op);
  data.set("disasm", CPU::disasm(op, c.pc));
  // control-transfer ring buffer, oldest → newest
  json::Value jl = json::Value::array();
  for(int k = 0; k < CPU::kJumpLog; k++) {
    int i = (c.jlogIdx + k) % CPU::kJumpLog;
    if(c.jlogSrc[i] == 0 && c.jlogDst[i] == 0) continue;
    json::Value e = json::Value::object();
    e.set("src", c.jlogSrc[i]);
    e.set("dst", c.jlogDst[i]);
    e.set("op",  (u64)c.jlogOp[i]);
    e.set("ins", CPU::disasm(c.jlogOp[i], c.jlogSrc[i]));
    jl.push(std::move(e));
  }
  data.set("jumplog", jl);
}

auto Server::cmdCpuStep(const json::Value& args, json::Value& data) -> void {
  u64 n = args.has("count") ? args.get("count").asU64() : 1;
  if(n == 0) n = 1;
  std::lock_guard<std::mutex> lk(system.coreMutex);
  u64 taken = system.stepCpu(n);
  data.set("stepped", taken);
  data.set("pc", system.cpu.pc);
  data.set("halted", system.cpu.halted);
  if(system.cpu.halted) data.set("haltReason", system.cpu.haltReason);
  u32 op = system.memory.read32((u32)system.cpu.pc);
  data.set("disasm", CPU::disasm(op, system.cpu.pc));
}

auto Server::cmdCpuDisasm(const json::Value& args, json::Value& data) -> bool {
  u32 addr = args.get("addr").asU32();
  u32 count = args.has("count") ? args.get("count").asU32() : 16;
  if(count == 0 || count > 4096) count = 16;
  std::lock_guard<std::mutex> lk(system.coreMutex);
  json::Value list = json::Value::array();
  for(u32 i = 0; i < count; i++) {
    u32 a = addr + i * 4;
    u32 op = system.memory.read32(a);
    json::Value e = json::Value::object();
    e.set("addr", (u64)a);
    e.set("op", (u64)op);
    e.set("text", CPU::disasm(op, a));
    list.push(std::move(e));
  }
  data.set("addr", (u64)addr);
  data.set("insns", list);
  return true;
}

auto Server::cmdRunControl(const std::string& cmd, json::Value& data) -> void {
  if(cmd == "pause") {
    system.paused.store(true);
  } else if(cmd == "resume") {
    std::lock_guard<std::mutex> lk(system.coreMutex);
    if(system.cpu.halted) { system.cpu.halted = false; system.cpu.haltReason.clear(); }
    system.paused.store(false);
  } else if(cmd == "reset") {
    std::lock_guard<std::mutex> lk(system.coreMutex);
    system.paused.store(true);
    system.cpu.fastBoot(system.rom.header.entryPoint);
  }
  data.set("paused", (bool)system.paused.load());
}

// Avanza N campos de video y vuelve a parar. Es el gemelo de cpu.step para TAS: la unidad
// no es la instruccion sino el campo, que es donde el juego lee el mando, asi que un
// avance = una entrada de la pelicula y una pulsacion se coloca en el campo exacto.
//
// Bloquea hasta que el nucleo termina (o hasta el plazo) por la misma razon que cpu.run_until:
// quien lo llama quiere mirar el estado DESPUES, y sin bloquear tendria que sondear.
// No se coge coreMutex mientras se espera -- el hilo del nucleo lo necesita para correr.
auto Server::cmdFrameAdvance(const json::Value& args, json::Value& data) -> void {
  u32 n = args.has("fields") ? args.get("fields").asU32() : 1;
  if(n == 0) n = 1;
  if(n > 100000) n = 100000;
  u32 timeoutMs = args.has("timeout_ms") ? args.get("timeout_ms").asU32() : 5000;
  {
    std::lock_guard<std::mutex> lk(system.coreMutex);
    if(system.cpu.halted) { system.cpu.halted = false; system.cpu.haltReason.clear(); }
    system.paused.store(true);                 // el avance manda por encima de la pausa
    system.stepFields.store(n, std::memory_order_relaxed);
  }
  auto t0 = std::chrono::steady_clock::now();
  bool timedOut = false;
  for(;;) {
    if(system.stepFields.load(std::memory_order_relaxed) == 0) break;
    if(system.cpu.halted || system.shutdown.load()) break;
    auto el = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if(el >= (double)timeoutMs) { timedOut = true; break; }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  {
    std::lock_guard<std::mutex> lk(system.coreMutex);
    u32 left = system.stepFields.exchange(0, std::memory_order_relaxed);
    data.set("fields", (u64)(n - left));
    data.set("timedOut", timedOut);
    data.set("paused", (bool)system.paused.load());
    data.set("halted", system.cpu.halted);
    data.set("viFields", (u64)system.memory.rcp.viFields);
    data.set("viFlips", (u64)system.memory.rcp.viFlips);
    data.set("pc", system.cpu.pc);
  }
}

// rewind.step {steps}. Como state.load: el rebobinado mete un estado entero en la maquina,
// asi que no se hace aqui sino en el bucle de ejecucion, con el RCP parado. Se deja la
// peticion en el contador y se espera al parte, que el bucle publica en stateMsg.
auto Server::cmdRewind(const json::Value& args, json::Value& data) -> bool {
  u32 n = args.has("steps") ? args.get("steps").asU32() : 1;
  if(n == 0) n = 1;
  if(n > 10000) n = 10000;
  if(!system.rewinder.enabled) {
    data.set("msg", std::string("el rebobinado no esta activo (KESTREL_REWIND=1)"));
    return false;
  }
  const u64 seq0 = system.stateSeq.load(std::memory_order_acquire);
  system.rewindReq.fetch_add(n, std::memory_order_release);
  for(int i = 0; i < 1000; i++) {
    if(system.stateSeq.load(std::memory_order_acquire) != seq0) {
      std::string msg;
      { std::lock_guard<std::mutex> ml(system.stateMsgMutex); msg = system.stateMsg; }
      std::lock_guard<std::mutex> lk(system.coreMutex);
      data.set("msg", msg);
      data.set("steps", (u64)system.rewinder.steps());
      data.set("bytes", (u64)system.rewinder.bytes());
      data.set("interval", (u64)system.rewinder.interval);
      data.set("viFields", (u64)system.memory.rcp.viFields);
      data.set("pc", system.cpu.pc);
      return msg.rfind("rebobinado", 0) == 0 && msg.find(": ") == std::string::npos;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  data.set("msg", std::string("el bucle de ejecucion no atendio la peticion (10 s)"));
  return false;
}

// Dump the RCP MMIO register file (memory.rcp). This is the state the boot code /
// scheduler polls: MI mask/intr drive CPU IP2; VI current/intr drive the retrace
// interrupt; SP status/pc the RSP; DPC the RDP FIFO. interruptPending() folds MI.
// state.save / state.load {slot}. NO guarda aqui: el estado solo se puede tomar con el RCP
// quieto, y quien puede pararlo es el bucle de ejecucion. Asi que esto deja la peticion en
// el buzon y espera a que la atienda -- el bucle la mira tambien en pausa, que es como la
// va a usar el lanzador. Si nadie contesta en 5 s es que no hay bucle corriendo, y eso se
// dice en vez de colgar al cliente.
auto Server::cmdState(const std::string& cmd, const json::Value& args, json::Value& data) -> bool {
  int slot = (int)args.get("slot").asInt();
  if(slot < 0 || slot > 9) { data.set("msg", "slot fuera de rango (0..9)"); return false; }
  auto& box = (cmd == "state.save") ? system.stateSaveReq : system.stateLoadReq;
  const u64 seq0 = system.stateSeq.load(std::memory_order_acquire);
  box.store(slot, std::memory_order_release);
  for(int i = 0; i < 500; i++) {
    if(system.stateSeq.load(std::memory_order_acquire) != seq0) {
      std::string msg;
      { std::lock_guard<std::mutex> ml(system.stateMsgMutex); msg = system.stateMsg; }
      data.set("slot", (u64)slot);
      data.set("msg", msg);
      data.set("path", stateSlotPath(system, slot));
      // El bucle deja el motivo en el mensaje; "fallo" delante es el unico marcador que
      // hay, y basta: el cliente quiere saber si hay fichero, no parsear el error.
      return msg.rfind("fallo", 0) != 0;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  data.set("msg", "el bucle de ejecucion no atendio la peticion (5 s)");
  return false;
}

auto Server::cmdRcpRegs(const json::Value&, json::Value& data) -> void {
  std::lock_guard<std::mutex> lk(system.coreMutex);
  Rcp& r = system.memory.rcp;
  json::Value mi = json::Value::object();
  mi.set("mode", (u64)r.mi_mode);
  mi.set("mask", (u64)r.mi_mask);
  mi.set("intr", (u64)r.mi_intr.load());
  mi.set("intrPending", (r.mi_intr.load() & r.mi_mask) != 0);
  data.set("mi", mi);
  json::Value sp = json::Value::object();
  sp.set("mem_addr", (u64)r.sp_mem_addr);
  sp.set("dram_addr", (u64)r.sp_dram_addr);
  sp.set("rd_len", (u64)r.sp_rd_len);
  sp.set("wr_len", (u64)r.sp_wr_len);
  sp.set("status", (u64)r.sp_status.load());
  sp.set("semaphore", (u64)r.sp_semaphore);
  sp.set("pc", (u64)r.sp_pc);
  data.set("sp", sp);
  json::Value dp = json::Value::object();
  dp.set("start", (u64)r.dpc_start);
  dp.set("end", (u64)r.dpc_end);
  dp.set("current", (u64)r.dpc_current.load());
  dp.set("clock", (u64)r.dpc_clock.load());
  dp.set("bufbusy", (u64)r.dpc_bufbusy.load());
  dp.set("pipebusy", (u64)r.dpc_pipebusy.load());
  dp.set("tmem", (u64)r.dpc_tmem.load());
  dp.set("status", (u64)r.dpc_status.load());
  data.set("dp", dp);
  json::Value vi = json::Value::object();
  vi.set("ctrl", (u64)r.vi_ctrl);
  vi.set("origin", (u64)r.vi_origin);
  vi.set("width", (u64)r.vi_width);
  vi.set("intr", (u64)r.vi_intr);
  vi.set("current", (u64)r.vi_current);
  vi.set("vstart", (u64)r.vi_vstart);
  vi.set("vburst", (u64)r.vi_vburst);
  vi.set("xscale", (u64)r.vi_xscale);
  vi.set("yscale", (u64)r.vi_yscale);
  // Contadores de progreso del video. Son lo unico que distingue "va lento" de "colgado"
  // desde fuera, y lo que usa la prueba de estado guardado para saber que la maquina sigue
  // avanzando: flips = buffers mostrados, fields = campos emitidos, syncs = listas de RDP.
  vi.set("flips", (u64)r.viFlips);
  vi.set("fields", (u64)r.viFields);
  vi.set("syncs", (u64)r.dpSyncs);
  data.set("vi", vi);
  json::Value ai = json::Value::object();
  ai.set("dram", (u64)r.ai_dram);
  ai.set("len", (u64)r.ai_len);
  ai.set("ctrl", (u64)r.ai_ctrl);
  ai.set("status", (u64)r.ai_status);
  ai.set("dacrate", (u64)r.ai_dacrate);
  ai.set("bitrate", (u64)r.ai_bitrate);
  ai.set("fifoCount", (u64)r.ai_fifo_count);
  data.set("ai", ai);
  json::Value pi = json::Value::object();
  pi.set("dram_addr", (u64)r.pi_dram_addr);
  pi.set("cart_addr", (u64)r.pi_cart_addr);
  pi.set("rd_len", (u64)r.pi_rd_len);
  pi.set("wr_len", (u64)r.pi_wr_len);
  pi.set("status", (u64)r.pi_status);
  data.set("pi", pi);
  json::Value si = json::Value::object();
  si.set("dram_addr", (u64)r.si_dram_addr);
  si.set("status", (u64)r.si_status);
  data.set("si", si);
}

// Decode the live VI framebuffer to RGBA8888. Reads VI origin/width/ctrl straight
// from the RCP register file (the exact buffer the console would be scanning out),
// then expands the pixels the same way the DAC does: 5/5/5/1 for 16bpp, direct
// 8/8/8/8 for 32bpp. Height is not a hardware register — it falls out of the active
// scan window, so we accept an optional `height` arg and default to 240 (NTSC).
auto Server::cmdViCapture(const json::Value& args, json::Value& data, std::vector<u8>& blob) -> bool {
  // Con el backend de GPU vivo, la imagen BUENA no esta en la RDRAM: parallel-rdp rasteriza
  // y escanea por su cuenta, y lo que se ve en la ventana sale de vrdp::scanout() ya con el
  // filtrado del VI aplicado. Leer la RDRAM aqui daba una foto a medio hacer y sin filtro --
  // parecia SoftRDP con los bordes rotos --, asi que cuando hay GPU se captura SU imagen.
  if(vrdp::active()) {
    u32 w = 0, h = 0;
    const u8* rgba = vrdp::scanout(w, h);
    if(rgba && w && h) {
      blob.assign(rgba, rgba + (usize)w * h * 4);
      vrdp::scanoutDone();
      data.set("width", (u64)w);
      data.set("height", (u64)h);
      data.set("format", "rgba8888");
      data.set("source", "gpu");
      std::lock_guard<std::mutex> lk(system.coreMutex);
      data.set("origin", (u64)(system.memory.rcp.vi_origin & 0x00FFFFFF));
      return true;
    }
    vrdp::scanoutDone();   // simetrico: scanout() deja el cerrojo cogido aunque no haya foto
  }
  u32 origin, width, ctrl;
  {
    std::lock_guard<std::mutex> lk(system.coreMutex);
    Rcp& r = system.memory.rcp;
    origin = r.vi_origin & 0x00FFFFFF;   // physical RDRAM address
    width  = r.vi_width & 0xFFF;
    ctrl   = r.vi_ctrl;
  }
  u32 type = ctrl & 0x3;                  // 0/1 blank, 2 = 16bpp 5551, 3 = 32bpp 8888
  if(width == 0 || (type != 2 && type != 3)) return false;
  u32 height = args.has("height") ? args.get("height").asU32() : 240;
  if(height == 0 || height > 480) height = 240;
  u32 bpp = (type == 3) ? 4 : 2;
  auto& ram = system.memory.rdram;
  u64 need = (u64)origin + (u64)width * height * bpp;
  if(need > ram.size()) return false;

  // El ultimo byte (32bpp) / el bit 0 (16bpp) del pixel NO es alfa: es la COBERTURA que el
  // VI usa para el antialias de bordes, y el DAC saca siempre imagen opaca. Meterlo en el
  // canal alfa del PNG hacia invisible todo lo que dibuja la CPU (que deja cobertura 0):
  // el menu de snapper64 salia en blanco -- fondo negro con alfa 0 -- y solo se veia lo que
  // habia pintado el RDP. La captura va opaca; la cobertura, si algun dia hace falta, es un
  // plano aparte, no el alfa.
  blob.resize((usize)width * height * 4);
  usize o = 0;
  for(u32 i = 0; i < width * height; i++) {
    u32 p = origin + i * bpp;
    u8 R, G, B;
    if(type == 3) {
      R = ram[p]; G = ram[p+1]; B = ram[p+2];
    } else {
      u16 px = (u16)((ram[p] << 8) | ram[p+1]);   // big-endian halfword
      u8 r5 = (px >> 11) & 0x1f, g5 = (px >> 6) & 0x1f, b5 = (px >> 1) & 0x1f;
      R = (r5 << 3) | (r5 >> 2);
      G = (g5 << 3) | (g5 >> 2);
      B = (b5 << 3) | (b5 >> 2);
    }
    blob[o++] = R; blob[o++] = G; blob[o++] = B; blob[o++] = 0xff;
  }
  data.set("width", (u64)width);
  data.set("height", (u64)height);
  data.set("origin", (u64)origin);
  data.set("bpp", (u64)(bpp * 8));
  data.set("format", "rgba8888");
  data.set("source", "rdram");
  return true;
}

// Dump the low-level RSP core: scalar GPRs, PC (into IMEM), running flag, and the
// 32 vector registers as 8×u16 lanes each (hex). Lets us inspect a stuck/parked
// microcode without a separate disassembler.
auto Server::cmdRspRegs(const json::Value& args, json::Value& data) -> void {
  std::lock_guard<std::mutex> lk(system.coreMutex);
  Rsp& s = system.memory.rsp;
  data.set("pc", (u64)s.pc);
  data.set("running", s.running);
  data.set("sp_status", (u64)system.memory.rcp.sp_status.load());
  data.set("sp_pc", (u64)system.memory.rcp.sp_pc);
  json::Value g = json::Value::array();
  for(int i = 0; i < 32; i++) g.push(json::Value((u64)s.r[i]));
  data.set("gpr", g);
  // Vector file only when asked (verbose); default omits to keep the frame small.
  if(args.has("vpr") && (args.get("vpr").b || args.get("vpr").asU32() != 0)) {
    json::Value v = json::Value::array();
    for(int i = 0; i < 32; i++) {
      json::Value lanes = json::Value::array();
      for(int e = 0; e < 8; e++) lanes.push(json::Value((u64)s.vpr[i].uc(e)));
      v.push(std::move(lanes));
    }
    data.set("vpr", v);
  }
}

auto Server::cmdBpAdd(const json::Value& args, json::Value& data) -> void {
  u32 addr = args.get("addr").asU32();
  std::lock_guard<std::mutex> lk(system.coreMutex);
  auto& bps = system.breakpoints;
  if(std::find(bps.begin(), bps.end(), addr) == bps.end()) bps.push_back(addr);
  data.set("addr", (u64)addr);
  data.set("count", (u64)bps.size());
}

auto Server::cmdBpDel(const json::Value& args, json::Value& data) -> void {
  std::lock_guard<std::mutex> lk(system.coreMutex);
  auto& bps = system.breakpoints;
  if(args.has("addr")) {
    u32 addr = args.get("addr").asU32();
    bps.erase(std::remove(bps.begin(), bps.end(), addr), bps.end());
  } else {
    bps.clear();   // no addr → clear all
  }
  data.set("count", (u64)bps.size());
}

auto Server::cmdBpList(const json::Value&, json::Value& data) -> void {
  std::lock_guard<std::mutex> lk(system.coreMutex);
  json::Value list = json::Value::array();
  for(u32 b : system.breakpoints) list.push(json::Value((u64)b));
  data.set("breakpoints", list);
  data.set("lastHit", (u64)system.lastBpHit.load());
}

// Resume and block until PC reaches `addr` (or halt / timeout). Uses a temporary
// breakpoint so it composes with any user breakpoints. Never holds coreMutex
// while waiting — the run() thread needs it to step. Leaves the core paused at
// the stop point so the caller can inspect immediately.
auto Server::cmdRunUntil(const json::Value& args, json::Value& data) -> void {
  u32 addr = args.get("addr").asU32();
  u32 timeoutMs = args.has("timeout_ms") ? args.get("timeout_ms").asU32() : 5000;
  bool added = false;
  {
    std::lock_guard<std::mutex> lk(system.coreMutex);
    auto& bps = system.breakpoints;
    if(std::find(bps.begin(), bps.end(), addr) == bps.end()) { bps.push_back(addr); added = true; }
    system.lastBpHit.store(0);
    if(system.cpu.halted) { system.cpu.halted = false; system.cpu.haltReason.clear(); }
    system.paused.store(false);
  }
  auto t0 = std::chrono::steady_clock::now();
  bool hit = false, timedOut = false;
  for(;;) {
    if(system.cpu.halted) break;
    if(system.paused.load()) { hit = (system.lastBpHit.load() == addr); break; }
    if(system.shutdown.load()) break;
    auto el = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if(el >= (double)timeoutMs) { timedOut = true; break; }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  {
    std::lock_guard<std::mutex> lk(system.coreMutex);
    if(added) {
      auto& bps = system.breakpoints;
      bps.erase(std::remove(bps.begin(), bps.end(), addr), bps.end());
    }
    system.paused.store(true);
    data.set("hit", hit);
    data.set("timedOut", timedOut);
    data.set("pc", system.cpu.pc);
    data.set("halted", system.cpu.halted);
    if(system.cpu.halted) data.set("haltReason", system.cpu.haltReason);
    u32 op = system.memory.read32((u32)system.cpu.pc);
    data.set("op", (u64)op);
    data.set("disasm", CPU::disasm(op, system.cpu.pc));
  }
}

// Hotpath profiler control: prof.start (enable + clear both samplers), prof.stop
// (disable, keep counts), prof.reset (zero counts, keep enabled state). The CPU
// sampler buckets by physical address (16-byte resolution); the RSP sampler counts
// per IMEM instruction. Turn on, run the workload, then query prof.cpu / prof.rsp.
auto Server::cmdProfControl(const std::string& cmd, json::Value& data) -> void {
  std::lock_guard<std::mutex> lk(system.coreMutex);
  if(cmd == "prof.start") {
    system.cpu.profEnable(true);
    system.memory.rsp.profOn = true; system.memory.rsp.profClear();
  } else if(cmd == "prof.stop") {
    system.cpu.profEnable(false);
    system.memory.rsp.profOn = false;
  } else {  // prof.reset
    system.cpu.profClear();
    system.memory.rsp.profClear();
  }
  data.set("profiling", system.cpu.profOn);
}

auto Server::cmdProfCpu(const json::Value& args, json::Value& data) -> void {
  u32 topN = args.has("top") ? args.get("top").asU32() : 20;
  std::lock_guard<std::mutex> lk(system.coreMutex);
  CPU& c = system.cpu;
  std::vector<std::pair<u32, u32>> hot;   // (count, bucket)
  hot.reserve(4096);
  for(u32 i = 0; i < (u32)c.profBuckets.size(); i++)
    if(c.profBuckets[i]) hot.push_back({c.profBuckets[i], i});
  if(hot.size() > topN)
    std::partial_sort(hot.begin(), hot.begin() + topN, hot.end(),
                      [](auto& a, auto& b){ return a.first > b.first; });
  else
    std::sort(hot.begin(), hot.end(), [](auto& a, auto& b){ return a.first > b.first; });

  json::Value list = json::Value::array();
  u64 total = c.profTotal ? c.profTotal : 1;
  for(u32 k = 0; k < hot.size() && k < topN; k++) {
    u32 phys = hot[k].second << CPU::kProfShift;
    json::Value e = json::Value::object();
    e.set("phys", (u64)phys);
    e.set("kseg0", (u64)(0x8000'0000u | phys));            // convenience VA for disasm/lookup
    e.set("count", (u64)hot[k].first);
    e.set("pct", 100.0 * (double)hot[k].first / (double)total);
    u32 op = system.memory.read32(0x8000'0000u | phys);    // first instr in the 16-byte bucket
    e.set("disasm", CPU::disasm(op, 0x8000'0000u | phys));
    list.push(e);
  }
  data.set("total", (u64)c.profTotal);
  data.set("enabled", c.profOn);
  data.set("resolutionBytes", (u64)(1u << CPU::kProfShift));
  data.set("hot", list);
}

auto Server::cmdProfRsp(const json::Value& args, json::Value& data) -> void {
  u32 topN = args.has("top") ? args.get("top").asU32() : 20;
  std::lock_guard<std::mutex> lk(system.coreMutex);
  Rsp& s = system.memory.rsp;
  std::vector<std::pair<u32, u32>> hot;   // (count, imem slot)
  for(u32 i = 0; i < 1024; i++) if(s.profPc[i]) hot.push_back({s.profPc[i], i});
  std::sort(hot.begin(), hot.end(), [](auto& a, auto& b){ return a.first > b.first; });

  json::Value list = json::Value::array();
  u64 total = s.profTotal ? s.profTotal : 1;
  for(u32 k = 0; k < hot.size() && k < topN; k++) {
    json::Value e = json::Value::object();
    e.set("imem", (u64)(hot[k].second << 2));   // IMEM byte address
    e.set("count", (u64)hot[k].first);
    e.set("pct", 100.0 * (double)hot[k].first / (double)total);
    list.push(e);
  }
  data.set("total", (u64)s.profTotal);
  data.set("enabled", s.profOn);
  data.set("hot", list);
}

// --- pad.set / pad.get -------------------------------------------------------
// Inyecta el mando 1 desde la red. Existe porque el estado del mando lo publica el bucle
// de la ventana cada cuadro: escribir los botones "a pelo" por mem.write no sirve de nada,
// el siguiente cuadro los pisa. Aqui la capa remota manda mientras le queden sondeos.
//
// La duracion se mide en SONDEOS del joybus del mando 1, no en milisegundos. Es la unica
// unidad que el juego percibe: un juego que lee el mando una vez por cuadro ve `polls`
// cuadros de pulsacion sea cual sea la velocidad a la que corra el emulador, y al agotarse
// vuelve el mando del anfitrion, o sea que el juego ve el FLANCO DE BAJADA -- que es lo que
// esperan los menus, y lo que una variable de entorno fija (KESTREL_BUTTONS) no puede dar.
//
// args: buttons  = palabra de 16 bits del mando (A=0x8000 ... C-der=0x0001, START=0x1000),
//                  o una lista por nombre: "START,A" / "dup+z".
//       stick_x/stick_y = -80..80 (rango analogico que reporta el mando N64).
//       polls    = sondeos que dura; 0 = soltar ya (devuelve el mando al anfitrion),
//                  -1 = hasta nueva orden. Por defecto 6 (~6 cuadros = una pulsacion).
auto Server::cmdPad(const std::string& cmd, const json::Value& args, json::Value& data) -> bool {
  Memory& m = system.memory;
  // `pad` elige el conector, 1..4. Por defecto el 1, que es lo que quiere quien solo
  // conduce un mando y no quiere saber que hay cuatro.
  int idx = args.has("pad") ? args.get("pad").asInt() : 1;
  if(idx < 1 || idx > 4) { data.set("msg", "pad: el conector va de 1 a 4"); return false; }
  idx -= 1;
  if(cmd == "pad.get") {
    s32 left = m.padRemotePolls[idx].load();
    s32 st   = m.padRemoteStick[idx].load();
    data.set("pad", (s64)(idx + 1));
    data.set("connected", m.padPort[idx].connected);
    data.set("accessory", (s64)m.padPort[idx].accessory);
    data.set("rumble", m.padPort[idx].rumble);
    data.set("remote", left != 0);
    data.set("polls_left", (s64)left);
    data.set("buttons", (u64)(left != 0 ? m.padRemoteButtons[idx].load() : m.padPort[idx].buttons));
    data.set("stick_x", (s64)(left != 0 ? (s8)(st & 0xff) : m.padPort[idx].stickX));
    data.set("stick_y", (s64)(left != 0 ? (s8)((st >> 8) & 0xff) : m.padPort[idx].stickY));
    return true;
  }
  // Nombres tal y como los llama el SDK (CONT_*), en minusculas y sin prefijo.
  static const struct { const char* n; u32 bit; } kNames[] = {
    {"a",0x8000},{"b",0x4000},{"z",0x2000},{"start",0x1000},
    {"dup",0x0800},{"ddown",0x0400},{"dleft",0x0200},{"dright",0x0100},
    {"l",0x0020},{"r",0x0010},
    {"cup",0x0008},{"cdown",0x0004},{"cleft",0x0002},{"cright",0x0001},
  };
  u32 btn = 0;
  json::Value bv = args.get("buttons");
  if(bv.isString()) {
    std::string s = bv.asString(), tok;
    for(usize i = 0; i <= s.size(); i++) {
      char c = i < s.size() ? s[i] : ',';
      if(c == ',' || c == '+' || c == ' ' || c == '|') {
        if(!tok.empty()) {
          u32 hit = 0;
          for(auto& e : kNames) if(tok == e.n) hit = e.bit;
          if(!hit) { data.set("msg", "pad.set: boton desconocido '" + tok + "'"); return false; }
          btn |= hit; tok.clear();
        }
      } else tok += (char)std::tolower((unsigned char)c);
    }
  } else btn = bv.asU32() & 0xffff;

  int sx = args.has("stick_x") ? args.get("stick_x").asInt() : 0;
  int sy = args.has("stick_y") ? args.get("stick_y").asInt() : 0;
  if(sx >  80) sx =  80; else if(sx < -80) sx = -80;
  if(sy >  80) sy =  80; else if(sy < -80) sy = -80;
  s32 polls = args.has("polls") ? args.get("polls").asInt() : 6;

  m.padRemoteButtons[idx].store(btn, std::memory_order_relaxed);
  m.padRemoteStick[idx].store(((s32)(u8)(s8)sy << 8) | (u8)(s8)sx, std::memory_order_relaxed);
  m.padRemotePolls[idx].store(polls, std::memory_order_release);   // publica el resto antes
  data.set("pad", (s64)(idx + 1));
  data.set("buttons", (u64)btn);
  data.set("stick_x", (s64)sx);
  data.set("stick_y", (s64)sy);
  data.set("polls", (s64)polls);
  return true;
}

}  // namespace kestrel::telemetry
