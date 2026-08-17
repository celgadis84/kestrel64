#include "server.hpp"
#include "../core/system.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <thread>

namespace kestrel::telemetry {

auto Server::stop() -> void {
  stopping.store(true);
  tcp.close();
}

auto Server::serve(u16 port) -> void {
  if(!tcp.listen(port)) {
    std::fprintf(stderr, "[telemetry] failed to bind port %u\n", port);
    return;
  }
  std::printf("[telemetry] listening on 127.0.0.1:%u\n", port);
  std::fflush(stdout);

  while(!stopping.load() && !system.shutdown.load()) {
    if(!tcp.accept()) break;  // listen socket closed → shutting down

    // Serve this client until it disconnects.
    for(;;) {
      std::string reqJson;
      std::vector<u8> reqBlob;
      if(!tcp.recvFrame(reqJson, reqBlob)) break;

      bool ok = false;
      json::Value req = json::parse(reqJson, ok);
      json::Value reply = json::Value::object();
      std::vector<u8> blob;

      if(!ok || !req.isObject()) {
        reply.set("ok", false).set("error", "malformed request JSON");
      } else {
        reply.set("id", req.get("id"));
        dispatch(req, reply, blob);
      }
      // Carry request blob into write handlers by stashing it before dispatch:
      // handled inline in dispatch() via a member is avoided; write cmd re-reads.

      if(!tcp.sendFrame(json::dump(reply), blob.empty() ? nullptr : blob.data(), (u32)blob.size()))
        break;
    }
    tcp.dropClient();
  }
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
    data.set("speed", sp);
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

// Dump the RCP MMIO register file (memory.rcp). This is the state the boot code /
// scheduler polls: MI mask/intr drive CPU IP2; VI current/intr drive the retrace
// interrupt; SP status/pc the RSP; DPC the RDP FIFO. interruptPending() folds MI.
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
  dp.set("current", (u64)r.dpc_current);
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

  blob.resize((usize)width * height * 4);
  usize o = 0;
  for(u32 i = 0; i < width * height; i++) {
    u32 p = origin + i * bpp;
    u8 R, G, B, A;
    if(type == 3) {
      R = ram[p]; G = ram[p+1]; B = ram[p+2]; A = ram[p+3];
    } else {
      u16 px = (u16)((ram[p] << 8) | ram[p+1]);   // big-endian halfword
      u8 r5 = (px >> 11) & 0x1f, g5 = (px >> 6) & 0x1f, b5 = (px >> 1) & 0x1f;
      R = (r5 << 3) | (r5 >> 2);
      G = (g5 << 3) | (g5 >> 2);
      B = (b5 << 3) | (b5 >> 2);
      A = (px & 1) ? 0xff : 0x00;
    }
    blob[o++] = R; blob[o++] = G; blob[o++] = B; blob[o++] = A;
  }
  data.set("width", (u64)width);
  data.set("height", (u64)height);
  data.set("origin", (u64)origin);
  data.set("bpp", (u64)(bpp * 8));
  data.set("format", "rgba8888");
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

}  // namespace kestrel::telemetry
