// kestrel64 — ordenes del depurador y de carga en caliente (docs/DEBUGGER.md).
//
// Van aparte de server.cpp para no engordarlo: aqui viven las capas que lee la ventana del
// depurador (comandos del RDP por cuadro, tareas del RSP, linea de tiempo, visor de
// framebuffer/z/cobertura, TMEM, TLB, anillo de eventos, puntos de observacion) y la carga
// de codigo en caliente (code.load / rsp.load / rom.load). El MCP y la ventana llegan por
// el MISMO despacho, asi que lo que ve uno lo ve el otro.

#include "server.hpp"
#include "../core/system.hpp"
#include "../debug/dbg.hpp"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>

namespace kestrel::telemetry {

namespace {

auto hex64(u64 v) -> std::string { char b[24]; std::snprintf(b, sizeof b, "%016llx", (unsigned long long)v); return b; }

// Desensamblado del RSP: lo escalar es MIPS R4000 (se delega en CPU::disasm); aqui solo lo
// propio: COP0 con nombres SP_*/DPC_*, mfc2/cfc2/mtc2/ctc2, operaciones de la VU y las
// cargas/almacenes vectoriales LWC2/SWC2 (offset de 7 bits con signo escalado por tamano).
auto rspDisasm(u32 op, u32 pc) -> std::string {
  static const char* gpr[32] = {
    "zero","at","v0","v1","a0","a1","a2","a3","t0","t1","t2","t3","t4","t5","t6","t7",
    "s0","s1","s2","s3","s4","s5","s6","s7","t8","t9","k0","k1","gp","sp","fp","ra"};
  static const char* c0[16] = {
    "SP_MEM_ADDR","SP_DRAM_ADDR","SP_RD_LEN","SP_WR_LEN","SP_STATUS","SP_DMA_FULL",
    "SP_DMA_BUSY","SP_SEMAPHORE","DPC_START","DPC_END","DPC_CURRENT","DPC_STATUS",
    "DPC_CLOCK","DPC_BUFBUSY","DPC_PIPEBUSY","DPC_TMEM"};
  static const char* vu[64] = {
    "vmulf","vmulu","vrndp","vmulq","vmudl","vmudm","vmudn","vmudh",
    "vmacf","vmacu","vrndn","vmacq","vmadl","vmadm","vmadn","vmadh",
    "vadd","vsub",nullptr,"vabs","vaddc","vsubc",nullptr,nullptr,
    nullptr,nullptr,nullptr,nullptr,nullptr,"vsar",nullptr,nullptr,
    "vlt","veq","vne","vge","vcl","vch","vcr","vmrg",
    "vand","vnand","vor","vnor","vxor","vnxor",nullptr,nullptr,
    "vrcp","vrcpl","vrcph","vmov","vrsq","vrsql","vrsqh","vnop",
    nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr};
  static const char* ls = "bsldqrpuhfwt";
  static const u8 lsShift[12] = {0,1,2,3,4,4,3,3,4,4,4,4};
  char b[96];
  const u32 opc = op >> 26, rs = (op >> 21) & 31, rt = (op >> 16) & 31, rd = (op >> 11) & 31;
  auto el = [](u32 e) -> std::string {
    static const char* n[16] = {"","","[0q]","[1q]","[0h]","[1h]","[2h]","[3h]",
                                "[0]","[1]","[2]","[3]","[4]","[5]","[6]","[7]"};
    return n[e & 15];
  };
  if(opc == 0x10 && (rs == 0 || rs == 4)) {
    std::snprintf(b, sizeof b, "%s %s,%s", rs ? "mtc0" : "mfc0", gpr[rt], c0[rd & 15]);
    return b;
  }
  if(opc == 0x12) {
    if(op & (1u << 25)) {
      const char* m = vu[op & 63];
      if(!m) { std::snprintf(b, sizeof b, "cop2 0x%07x", op & 0x1ffffff); return b; }
      const u32 vd = (op >> 6) & 31, vs = rd, e = (op >> 21) & 15;
      const u32 f = op & 63;
      if(f == 55) return "vnop";
      if(f >= 48)   // una sola lane: vd[de], vt[e]
        std::snprintf(b, sizeof b, "%s v%u[%u],v%u[%u]", m, vd, vs & 7, rt, e & 7);
      else
        std::snprintf(b, sizeof b, "%s v%u,v%u,v%u%s", m, vd, vs, rt, el(e).c_str());
      return b;
    }
    static const char* mv[8] = {"mfc2",nullptr,"cfc2",nullptr,"mtc2",nullptr,"ctc2",nullptr};
    if(rs < 8 && mv[rs]) {
      if(rs == 2 || rs == 6) std::snprintf(b, sizeof b, "%s %s,vc%u", mv[rs], gpr[rt], rd & 3);
      else std::snprintf(b, sizeof b, "%s %s,v%u[%u]", mv[rs], gpr[rt], rd, (op >> 7) & 15);
      return b;
    }
  }
  if((opc == 0x32 || opc == 0x3a) && rd < 12) {
    s32 off = (s32)(op << 25) >> 25;
    off *= 1 << lsShift[rd];
    std::snprintf(b, sizeof b, "%c%cv v%u[%u],%s0x%x(%s)", opc == 0x32 ? 'l' : 's', ls[rd], rt,
                  (op >> 7) & 15, off < 0 ? "-" : "", (u32)(off < 0 ? -off : off), gpr[rs]);
    return b;
  }
  return CPU::disasm(op, pc);
}

auto readFile(const std::string& path, std::vector<u8>& out) -> bool {
  std::ifstream f(path, std::ios::binary);
  if(!f) return false;
  out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}

auto be16(const u8* p) -> u32 { return (u32)p[0] << 8 | p[1]; }
auto be32(const u8* p) -> u32 { return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | p[3]; }

// KSEG0/KSEG1 -> fisica. Lo mapeado por TLB no se resuelve aqui (un cargador no sabe que
// TLB tendra el juego): se rechaza.
auto segPhys(u32 v, u32& phys) -> bool {
  if(v >= 0x8000'0000u && v < 0xC000'0000u) { phys = v & 0x1fff'ffffu; return true; }
  if(v < 0x0080'0000u) { phys = v; return true; }   // ya fisica (bin crudo a RDRAM)
  return false;
}

auto frameArg(const json::Value& args) -> int { return args.has("frame") ? args.get("frame").asInt() : 0; }

}  // namespace

auto Server::dispatchDbg(const std::string& cmd, const json::Value& args, const std::vector<u8>& reqBlob,
                         json::Value& data, std::vector<u8>& blob, std::string& err) -> bool {
  Memory& m = system.memory;
  dbg::State& g = dbg::g;

  if(cmd == "dbg.enable") {
    const bool on = !args.has("on") || args.get("on").asU32() != 0;
    g.enable(on);
    data.set("on", on);
  } else if(cmd == "dbg.freeze") {
    const bool on = !args.has("on") || args.get("on").asU32() != 0;
    g.freeze.store(on);
    data.set("freeze", on);
  } else if(cmd == "dbg.status") {
    std::lock_guard<std::mutex> lk(g.mx);
    data.set("on", g.on.load());
    data.set("freeze", g.freeze.load());
    data.set("framesClosed", g.framesClosed);
    auto fr = [](const dbg::Frame& f) {
      json::Value o = json::Value::object();
      o.set("number", f.number); o.set("t0", f.t0); o.set("t1", f.t1);
      o.set("cmds", (u32)f.cmds.size()); o.set("jobs", (u32)f.jobs.size());
      o.set("tasks", (u32)f.tasks.size()); o.set("truncated", f.truncated);
      return o;
    };
    data.set("last", fr(g.last));
    data.set("cur", fr(g.cur));
  } else if(cmd == "dbg.rdp.frame") {
    // Lista de comandos del RDP del cuadro: direccion, opcode, palabras y texto decodificado.
    // start/count recorren la lista; prims=1 se queda con las primitivas (paso a paso).
    if(!g.on.load()) { err = "capa apagada: dbg.enable primero (graba desde el siguiente cuadro)"; return true; }
    dbg::Frame f = g.snapshot(frameArg(args));
    const u32 start = args.get("start").asU32(0);
    const u32 count = std::min<u32>(args.has("count") ? args.get("count").asU32() : 200, 5000);
    const bool decode = !args.has("decode") || args.get("decode").asU32() != 0;
    const bool prims = args.get("prims").asU32() != 0;
    json::Value list = json::Value::array();
    u32 seen = 0, emitted = 0, idx = 0;
    for(const auto& c : f.cmds) {
      const u32 i = idx++;
      if(prims && !dbg::rdpIsPrimitive(c.op)) continue;
      if(seen++ < start) continue;
      if(emitted >= count) break;
      json::Value e = json::Value::object();
      e.set("i", i);
      e.set("addr", c.addr);
      e.set("op", (u32)c.op);
      e.set("name", dbg::rdpCmdName(c.op));
      e.set("job", c.job);
      if(c.xbus) e.set("xbus", true);
      json::Value w = json::Value::array();
      for(u32 k = 0; k < c.nwords && c.word0 + k < f.words.size(); k++) w.push(hex64(f.words[c.word0 + k]));
      e.set("words", w);
      if(decode && c.word0 < f.words.size())
        e.set("text", dbg::rdpDecode(&f.words[c.word0], std::min<u32>(c.nwords, (u32)f.words.size() - c.word0)));
      list.push(std::move(e));
      emitted++;
    }
    data.set("number", f.number);
    data.set("total", (u32)f.cmds.size());
    data.set("truncated", f.truncated);
    data.set("cmds", list);
  } else if(cmd == "dbg.rsp.tasks") {
    if(!g.on.load()) { err = "capa apagada: dbg.enable primero"; return true; }
    dbg::Frame f = g.snapshot(frameArg(args));
    json::Value list = json::Value::array();
    for(const auto& t : f.tasks) {
      json::Value e = json::Value::object();
      e.set("type", t.type); e.set("flags", t.flags);
      e.set("ucode", t.ucode); e.set("ucodeData", t.ucodeData);
      e.set("dataPtr", t.dataPtr); e.set("dataSize", t.dataSize);
      e.set("pc", t.pc); e.set("t0", t.t0); e.set("t1", t.t1);
      e.set("cycles", t.cycles); e.set("done", t.done);
      list.push(std::move(e));
    }
    data.set("number", f.number);
    data.set("tasks", list);
  } else if(cmd == "dbg.timeline") {
    // Carriles CPU / RSP / RDP en reloj de invitado (instrucciones de CPU), relativos al
    // inicio del cuadro. La CPU es el cuadro entero; los huecos de RSP/RDP son su ocio.
    if(!g.on.load()) { err = "capa apagada: dbg.enable primero"; return true; }
    dbg::Frame f = g.snapshot(frameArg(args));
    json::Value rsp = json::Value::array(), rdp = json::Value::array();
    for(const auto& t : f.tasks) {
      json::Value e = json::Value::array();
      e.push(t.t0 - f.t0); e.push((t.done ? t.t1 : f.t1) - f.t0); e.push(t.type); e.push(t.cycles);
      rsp.push(std::move(e));
    }
    for(const auto& j : f.jobs) {
      json::Value e = json::Value::array();
      e.push(j.t0 - f.t0); e.push((j.t1 ? j.t1 : f.t1) - f.t0); e.push(j.ncmds); e.push(j.end - j.start);
      rdp.push(std::move(e));
    }
    data.set("number", f.number);
    data.set("span", f.t1 - f.t0);
    data.set("rsp", rsp);   // [t0, t1, OSTask.type, ciclos]
    data.set("rdp", rdp);   // [t0, t1, comandos, bytes del FIFO]
  } else if(cmd == "dbg.fb") {
    dbgFb(args, data, blob, err);
  } else if(cmd == "dbg.tmem") {
    // TMEM de SoftRDP (4 KiB) y los 8 descriptores de tile. Con GPU-RDP o parallel-RDP de
    // rasterizador la TMEM viva esta en la GPU; SoftRDP sigue decodificando el FIFO (GPU-RDP
    // usa su decodificador), pero con parallel-RDP puede estar vieja: ver docs/DEBUGGER.md.
    std::lock_guard<System::CoreMutex> lk(system.coreMutex);
    const SoftRdp& r = m.softRdp;
    blob.assign(r.tmem, r.tmem + sizeof r.tmem);
    json::Value tiles = json::Value::array();
    for(const auto& t : r.tiles) {
      json::Value e = json::Value::object();
      e.set("fmt", t.fmt); e.set("size", t.size); e.set("line", t.line); e.set("tmem", t.tmem);
      e.set("palette", t.palette);
      e.set("cmS", t.cmS); e.set("maskS", t.maskS); e.set("shiftS", t.shiftS);
      e.set("cmT", t.cmT); e.set("maskT", t.maskT); e.set("shiftT", t.shiftT);
      e.set("sl", t.sl); e.set("tl", t.tl); e.set("sh", t.sh); e.set("th", t.th);
      tiles.push(std::move(e));
    }
    data.set("tiles", tiles);
    data.set("bytes", (u32)sizeof r.tmem);
  } else if(cmd == "cpu.tlb") {
    std::lock_guard<System::CoreMutex> lk(system.coreMutex);
    json::Value list = json::Value::array();
    for(u32 i = 0; i < 32; i++) {
      const auto& t = system.cpu.tlb[i];
      json::Value e = json::Value::object();
      e.set("i", i); e.set("hi", t.hi); e.set("lo0", t.lo0); e.set("lo1", t.lo1);
      e.set("mask", t.mask); e.set("global", t.global);
      // Pagina = (mask|0x1fff)+1 por par; vpn2 y asid sacados de EntryHi.
      e.set("pageSize", (u64)((t.mask | 0x1fffull) + 1) >> 1);
      e.set("vpn2", t.hi & ~0x1fffull); e.set("asid", (u32)(t.hi & 0xff));
      e.set("pfn0", (u64)((t.lo0 >> 6) & 0xfffff) << 12); e.set("v0", (bool)(t.lo0 & 2)); e.set("d0", (bool)(t.lo0 & 4));
      e.set("pfn1", (u64)((t.lo1 >> 6) & 0xfffff) << 12); e.set("v1", (bool)(t.lo1 & 2)); e.set("d1", (bool)(t.lo1 & 4));
      list.push(std::move(e));
    }
    data.set("entries", list);
  } else if(cmd == "ev.log") {
    // Anillo de eventos del RCP (el mismo de KESTREL_EVLOG): on=1/0 lo arma, n = ultimas.
    if(args.has("on")) m.evOn = args.get("on").asU32() != 0;
    const u32 n = std::min<u32>(args.has("n") ? args.get("n").asU32() : 100, Memory::kEvN);
    const u32 end = m.evIdx.load(std::memory_order_relaxed);
    const u32 have = std::min<u32>(end, Memory::kEvN);
    json::Value list = json::Value::array();
    for(u32 k = std::min(n, have); k > 0; k--) {
      const Memory::Ev& e = m.evRing[(end - k) & (Memory::kEvN - 1)];
      if(!e.tag) continue;
      json::Value o = json::Value::array();
      o.push(e.clk); o.push(e.tag); o.push(e.a); o.push(e.b);
      list.push(std::move(o));
    }
    data.set("on", m.evOn);
    data.set("total", end);
    data.set("events", list);   // [reloj, etiqueta, a, b]
  } else if(cmd == "mem.watch") {
    // Punto de observacion del anfitrion sobre direcciones FISICAS: addr + len (o hi), r/w.
    // Pausa al final de la instruccion que toca el rango. clear=1 lo quita. Sin args: estado.
    std::lock_guard<System::CoreMutex> lk(system.coreMutex);
    CPU& c = system.cpu;
    if(args.get("clear").asU32()) c.hostWatchSet(0, 0, 0);
    else if(args.has("addr")) {
      const u32 lo = args.get("addr").asU32() & 0x1fff'ffffu;
      const u32 hi = args.has("hi") ? (args.get("hi").asU32() & 0x1fff'ffffu)
                                    : lo + (args.has("len") ? args.get("len").asU32() : 4);
      const u8 acc = (u8)((args.has("w") ? (args.get("w").asU32() ? 1 : 0) : 1) |
                          (args.get("r").asU32() ? 2 : 0));
      if(hi <= lo || !acc) { err = "rango vacio o sin r/w"; return true; }
      c.hostWatchSet(lo, hi, acc);
    }
    data.set("armed", c.hostWatchAcc != 0);
    data.set("lo", c.hostWatchLo); data.set("hi", c.hostWatchHi);
    data.set("r", (bool)(c.hostWatchAcc & 2)); data.set("w", (bool)(c.hostWatchAcc & 1));
    data.set("hit", c.hostWatchHit);
    if(c.hostWatchHit) {
      data.set("hitPc", c.hostWatchHitPc); data.set("hitAddr", c.hostWatchHitAddr);
      data.set("hitWrite", c.hostWatchHitW);
    }
    data.set("paused", system.paused.load());
  } else if(cmd == "code.load") {
    dbgCodeLoad(args, reqBlob, data, err);
  } else if(cmd == "rsp.load") {
    dbgRspLoad(args, reqBlob, data, err);
  } else if(cmd == "rsp.disasm") {
    // IMEM desensamblada: addr (0x000..0xFFC) + count (defecto 64, tope 1024). Marca los
    // puntos de ruptura del RSP; pc = PC actual del RSP.
    const u32 a0 = args.get("addr").asU32() & 0xffc;
    u32 count = args.has("count") ? args.get("count").asU32() : 64;
    if(count == 0 || count > 1024) count = 64;
    Memory& m = system.memory;
    // Con el RSP aparcado en un punto de ruptura se lee sin candado (como rsp.regs): en
    // Threaded la CPU puede esperarlo en su barrera con coreMutex tomado.
    std::unique_lock<System::CoreMutex> lk(system.coreMutex, std::defer_lock);
    if(!m.rsp.bpHit.load(std::memory_order_acquire)) lk.lock();
    json::Value list = json::Value::array();
    for(u32 i = 0; i < count; i++) {
      const u32 a = (a0 + i * 4) & 0xffc;
      const u32 op = ((u32)m.imem[a] << 24) | ((u32)m.imem[a + 1] << 16) | ((u32)m.imem[a + 2] << 8) | m.imem[a + 3];
      json::Value e = json::Value::object();
      e.set("addr", (u64)a);
      e.set("op", (u64)op);
      e.set("text", rspDisasm(op, a));
      if(m.rsp.bpMap[a >> 2].load(std::memory_order_relaxed)) e.set("bp", true);
      list.push(std::move(e));
    }
    data.set("pc", (u64)(m.rsp.pc & 0xffc));
    data.set("insns", list);
  } else if(cmd == "rom.load") {
    dbgRomLoad(args, data, err);
  } else if(cmd == "usb.log") {
    dbgUsb(cmd, args, reqBlob, data, blob, err);
  } else {
    return false;
  }
  return true;
}

// Framebuffer / z / cobertura de cualquier imagen de RDRAM, en RGBA8 (blob). Por defecto la
// que el RDP tiene puesta ahora (SET_COLOR_IMAGE / SET_Z_IMAGE / scissor de SoftRDP).
auto Server::dbgFb(const json::Value& args, json::Value& data, std::vector<u8>& blob, std::string& err) -> void {
  Memory& m = system.memory;
  std::lock_guard<System::CoreMutex> lk(system.coreMutex);
  const SoftRdp& r = m.softRdp;
  const std::string kind = args.has("kind") ? args.get("kind").asString() : "color";
  int x0, y0, x1, y1; r.scissorBox(x0, y0, x1, y1);
  u32 width = args.has("width") ? args.get("width").asU32() : r.colorImageWidth();
  u32 height = args.has("height") ? args.get("height").asU32() : (u32)std::max(1, y1);
  u32 siz = args.has("siz") ? args.get("siz").asU32() : r.colorImageSize();
  u32 addr = r.colorImage();
  if(kind == "depth") { addr = r.zImage(); if(!args.has("siz")) siz = 2; }
  if(args.has("addr")) addr = args.get("addr").asU32();
  addr &= 0x00ff'ffffu;
  if(siz != 2 && siz != 3) { err = "siz debe ser 2 (16 bpp) o 3 (32 bpp)"; return; }
  if(width == 0 || width > 2048 || height == 0 || height > 2048) { err = "geometria fuera de rango"; return; }
  const u32 bpp = siz == 3 ? 4 : 2;
  const u8* ram = m.rdram.data();
  const u32 ramSz = (u32)m.rdram.size();
  const u8* hid = m.rdramHidden.size() == (ramSz >> 1) ? m.rdramHidden.data() : nullptr;
  blob.assign((usize)width * height * 4, 0);
  u32 nonzero = 0;
  for(u32 y = 0; y < height; y++)
    for(u32 x = 0; x < width; x++) {
      const u32 a = addr + (y * width + x) * bpp;
      u8* o = &blob[((usize)y * width + x) * 4];
      if(a + bpp > ramSz) { o[3] = 255; continue; }
      u8 R = 0, G = 0, B = 0;
      if(kind == "color") {
        if(bpp == 2) {
          const u32 p = be16(ram + a);
          R = (u8)(((p >> 11) & 31) * 255 / 31); G = (u8)(((p >> 6) & 31) * 255 / 31); B = (u8)(((p >> 1) & 31) * 255 / 31);
        } else { R = ram[a]; G = ram[a + 1]; B = ram[a + 2]; }
      } else if(kind == "depth") {
        // Z codificado de 16 bits (exponente 3 + mantisa 11 + dz 2): se pinta tal cual, mas
        // claro = mas lejos. Para leer el valor exacto, mem.read.
        const u32 z = be16(ram + a) >> 2;
        R = G = B = (u8)(z >> 6);
      } else if(kind == "coverage") {
        // 3 bits: en 16 bpp el bit bajo del pixel + 2 de la RAM oculta; en 32 bpp alpha[7:5].
        u32 cv = 0;
        if(bpp == 2) cv = (be16(ram + a) & 1) << 2 | (hid ? (hid[a >> 1] & 3) : 0);
        else cv = ram[a + 3] >> 5;
        R = G = B = (u8)(cv * 255 / 7);
      } else { err = "kind = color | depth | coverage"; blob.clear(); return; }
      o[0] = R; o[1] = G; o[2] = B; o[3] = 255;
      nonzero += (R | G | B) != 0;
    }
  data.set("addr", addr); data.set("width", width); data.set("height", height);
  data.set("siz", siz); data.set("kind", kind); data.set("nonzero", nonzero);
  json::Value sc = json::Value::array(); sc.push(x0); sc.push(y0); sc.push(x1); sc.push(y1);
  data.set("scissor", sc);
  data.set("zImage", r.zImage());
}

// Carga de codigo en caliente: un ELF (segmentos PT_LOAD a sus direcciones) o un binario
// crudo a `addr`, desde `path` o desde el blob de la peticion. Escribe RDRAM coherente con
// las caches de la CPU, tira lo compilado por el dynarec y, con pc=1 (ELF: entrada) o
// pc=<dir>, salta ahi. La maquina sigue como estaba (pausada o corriendo).
auto Server::dbgCodeLoad(const json::Value& args, const std::vector<u8>& reqBlob, json::Value& data, std::string& err) -> void {
  std::vector<u8> buf;
  const std::string path = args.get("path").asString();
  if(!path.empty()) { if(!readFile(path, buf)) { err = "no se puede leer " + path; return; } }
  else buf = reqBlob;
  if(buf.empty()) { err = "sin datos (path o blob)"; return; }

  struct Seg { u32 phys, vaddr; usize off, filesz, memsz; };
  std::vector<Seg> segs;
  u32 entry = 0;
  const bool isElf = buf.size() >= 0x34 && buf[0] == 0x7f && buf[1] == 'E' && buf[2] == 'L' && buf[3] == 'F';
  if(isElf) {
    if(buf[4] != 1 || buf[5] != 2) { err = "solo ELF32 big-endian (MIPS o32/n32)"; return; }
    entry = be32(&buf[0x18]);
    const u32 phoff = be32(&buf[0x1c]), phent = be16(&buf[0x2a]), phnum = be16(&buf[0x2c]);
    for(u32 i = 0; i < phnum; i++) {
      const usize o = (usize)phoff + (usize)i * phent;
      if(o + 0x20 > buf.size()) { err = "cabecera de programa fuera del fichero"; return; }
      if(be32(&buf[o]) != 1) continue;   // PT_LOAD
      Seg s{};
      s.off = be32(&buf[o + 4]); s.vaddr = be32(&buf[o + 8]);
      s.filesz = be32(&buf[o + 0x10]); s.memsz = be32(&buf[o + 0x14]);
      if(s.off + s.filesz > buf.size()) { err = "segmento fuera del fichero"; return; }
      if(!segPhys(s.vaddr, s.phys)) { err = "segmento en direccion mapeada por TLB: " + std::to_string(s.vaddr); return; }
      if(s.memsz) segs.push_back(s);
    }
    if(segs.empty()) { err = "ELF sin segmentos PT_LOAD"; return; }
  } else {
    if(!args.has("addr")) { err = "binario crudo: falta addr"; return; }
    Seg s{};
    s.vaddr = args.get("addr").asU32();
    if(!segPhys(s.vaddr, s.phys)) { err = "addr debe ser KSEG0/KSEG1 o fisica de RDRAM"; return; }
    s.off = 0; s.filesz = s.memsz = buf.size();
    segs.push_back(s);
    entry = s.vaddr;
  }

  std::lock_guard<System::CoreMutex> lk(system.coreMutex);
  CPU& c = system.cpu;
  json::Value list = json::Value::array();
  u32 total = 0;
  for(const auto& s : segs) {
    u32 w = c.hostPoke(s.phys, &buf[s.off], (u32)s.filesz, true);
    if(s.memsz > s.filesz) {   // .bss
      std::vector<u8> z(s.memsz - s.filesz, 0);
      w += c.hostPoke(s.phys + (u32)s.filesz, z.data(), (u32)z.size(), true);
    }
    if(w < s.memsz) { err = "segmento se sale de la RDRAM"; return; }
    json::Value e = json::Value::object();
    e.set("vaddr", s.vaddr); e.set("phys", s.phys); e.set("filesz", (u64)s.filesz); e.set("memsz", (u64)s.memsz);
    list.push(std::move(e));
    total += w;
  }
  // Saltar: pc=1 = la entrada del ELF (o addr del crudo); pc=<dir> = esa. pc=0 / ausente = no.
  if(args.has("pc") && args.get("pc").asU64() != 0) {
    const u64 tgt = args.get("pc").asU64() == 1 ? entry : args.get("pc").asU64();
    c.pc = (u64)(s64)(s32)(u32)tgt;
    c.nextPc = c.pc + 4;
    if(c.halted) { c.halted = false; c.haltReason.clear(); }
    data.set("pc", c.pc);
  }
  data.set("elf", isElf);
  data.set("entry", entry);
  data.set("bytes", total);
  data.set("segments", list);
}

// Microcodigo / datos al RSP: IMEM o DMEM, con el RSP PARADO (SP_STATUS.HALT). Con el RSP
// corriendo se cambiaria el codigo bajo sus pies; force=1 lo permite igualmente.
auto Server::dbgRspLoad(const json::Value& args, const std::vector<u8>& reqBlob, json::Value& data, std::string& err) -> void {
  Memory& m = system.memory;
  std::vector<u8> buf;
  const std::string path = args.get("path").asString();
  if(!path.empty()) { if(!readFile(path, buf)) { err = "no se puede leer " + path; return; } }
  else buf = reqBlob;
  if(buf.empty()) { err = "sin datos (path o blob)"; return; }
  const std::string mem = args.has("mem") ? args.get("mem").asString() : "imem";
  const u32 off = args.get("offset").asU32() & 0xfff;
  std::lock_guard<System::CoreMutex> lk(system.coreMutex);
  if(!(m.rcp.sp_status.load() & 1) && !args.get("force").asU32()) { err = "el RSP no esta parado (SP_STATUS.HALT); force=1 para forzar"; return; }
  std::vector<u8>& dst = mem == "dmem" ? m.dmem : m.imem;
  if(mem != "imem" && mem != "dmem") { err = "mem = imem | dmem"; return; }
  const u32 n = (u32)std::min<usize>(buf.size(), dst.size() - off);
  std::memcpy(dst.data() + off, buf.data(), n);
  if(&dst == &m.imem && m.rsp.jc) m.rsp.jitInvalidate(off, n, m.imem.data());
  if(args.has("pc")) m.rsp.pc = args.get("pc").asU32() & 0xffc;
  data.set("mem", mem); data.set("offset", off); data.set("bytes", n);
}

// Cambio de ROM en frio: el proceso se relanza con la misma linea de ordenes y la ROM nueva
// (mismo puerto de telemetria: el puente MCP vuelve a conectar en la siguiente llamada).
// Relanzar en vez de reconstruir en caliente garantiza el mismo estado que un arranque
// limpio: caches de GPU-RDP, hilos del RCP, guardados y rebobinado salen de cero.
auto Server::dbgRomLoad(const json::Value& args, json::Value& data, std::string& err) -> void {
  const std::string path = args.get("path").asString();
  std::vector<u8> probe;
  if(path.empty()) { err = "falta path"; return; }
  { std::ifstream f(path, std::ios::binary); if(!f) { err = "no se puede abrir " + path; return; } }
  {
    std::lock_guard<System::CoreMutex> lk(system.coreMutex);
    system.romSwapPath = path;
  }
  system.shutdown.store(true, std::memory_order_release);
  data.set("path", path);
  data.set("msg", "relanzando con la ROM nueva; reconectar en ~1-2 s");
}

auto Server::dbgUsb(const std::string&, const json::Value& args, const std::vector<u8>&, json::Value& data,
                    std::vector<u8>&, std::string&) -> void {
  Memory& m = system.memory;
  std::lock_guard<std::mutex> lk(m.usbMx);
  data.set("packets", m.usbPackets);
  data.set("bytes", m.usbBytes);
  data.set("lastType", m.usbLastType);
  data.set("text", m.usbText);
  if(args.get("clear").asU32()) m.usbText.clear();
}

}  // namespace kestrel::telemetry
