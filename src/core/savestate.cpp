// kestrel64 - estado guardado. Ver savestate.hpp para el contrato.
//
// UNA sola descripcion del estado, recorrida en las dos direcciones: StateIO lleva una
// bandera `writing` y todo lo demas (visitCpu, visitRsp, ...) esta escrito una vez. Es la
// unica forma barata de que guardar y cargar no se desincronicen: con dos funciones
// separadas, cualquier campo anadido a una y olvidado en la otra corrompe el estado y solo
// se nota como un cuelgue raro tres partidas despues.

#include "archive.hpp"
#include "savestate.hpp"
#include "system.hpp"
#include "memory.hpp"
#include "movie.hpp"
#include "../cpu/cpu.hpp"
#include "../cpu/jit.hpp"
#include "../rsp/rsp.hpp"
#include "../rdp/rdp.hpp"
#include <cstdio>
#include <cstring>
#include <type_traits>
#include <vector>

namespace kestrel {

namespace {
constexpr u32 kMagic   = 0x4b535436;   // 'KST6'
constexpr u32 kVersion = 10;  // 10: ciclos de parada pendientes (coste de fallo de cache);
                              // 9: registros DPS (puerto de test al buffer de spans);
                              // 8: plano oculto de RDRAM (cobertura del RDP para el AA del VI);
                              // 7: transaccion del SI/joybus en vuelo (plazo del mando);
                              // 6: resto fraccionario del reloj Count (CPI); 5: posicion de la
                              // pelicula (TAS); 4: los cuatro puertos de mando
}  // namespace

// Flujo de bytes bidireccional. En escritura acumula en `out`; en lectura consume `in`.
// Cualquier lectura mas alla del final marca `bad` y desde ese momento todo devuelve ceros,
// asi que un fichero truncado no puede meter basura en el estado: se aborta la carga entera.
struct StateIO {
  bool writing = false;
  std::vector<u8>* out = nullptr;
  const u8* in = nullptr;
  usize len = 0, pos = 0;
  bool bad = false;

  auto bytes(void* p, usize n) -> void {
    if(bad) { if(!writing) std::memset(p, 0, n); return; }
    if(writing) { const u8* b = (const u8*)p; out->insert(out->end(), b, b + n); return; }
    if(pos + n > len) { bad = true; std::memset(p, 0, n); return; }
    std::memcpy(p, in + pos, n);
    pos += n;
  }
  template <class T> auto pod(T& v) -> void {
    static_assert(std::is_trivially_copyable<T>::value, "campo de estado no copiable");
    bytes(&v, sizeof(T));
  }
  template <class T> auto arr(T* p, usize n) -> void {
    static_assert(std::is_trivially_copyable<T>::value, "campo de estado no copiable");
    bytes(p, n * sizeof(T));
  }
  // Atomicos: el valor, no el objeto. El estado se toma con el RCP en reposo, asi que un
  // relaxed basta -- no hay otro hilo tocandolos en ese instante.
  auto atom(std::atomic<u32>& a) -> void {
    u32 v = a.load(std::memory_order_relaxed); pod(v);
    if(!writing) a.store(v, std::memory_order_relaxed);
  }
  auto atom(std::atomic<u64>& a) -> void {
    u64 v = a.load(std::memory_order_relaxed); pod(v);
    if(!writing) a.store(v, std::memory_order_relaxed);
  }
  // Marca de seccion. No hace falta para leer, hace falta para NO leer: si las dos
  // direcciones se desincronizan, la carga se para aqui en vez de repartir bytes
  // desplazados por todo el estado de la maquina.
  auto tag(const char* four) -> void {
    char t[4];
    std::memcpy(t, four, 4);
    bytes(t, 4);
    if(!writing && !bad && std::memcmp(t, four, 4) != 0) bad = true;
  }
  // Bloque con longitud delante: en carga exige el mismo tamano que el destino.
  auto blob(void* p, usize n) -> void {
    u64 sz = n; pod(sz);
    if(!writing && sz != n) { bad = true; return; }
    bytes(p, n);
  }
  auto vecBlob(std::vector<u8>& v) -> void {
    u64 sz = v.size(); pod(sz);
    if(!writing) { if(sz > ((u64)1 << 30)) { bad = true; return; } v.resize((usize)sz); }
    bytes(v.data(), (usize)sz);
  }
};

// Amigo de Rsp / SoftRdp / Memory: aqui viven las listas de campos que tocan su estado
// privado. Todo lo demas es publico y se recorre en las funciones libres de abajo.
struct StateVisitor {
  static auto rsp(StateIO& io, Rsp& r) -> void {
    io.tag("RSP ");
    io.arr(r.r, 32);
    io.pod(r.pc);
    io.arr(r.vpr, 32);
    io.pod(r.acch); io.pod(r.accm); io.pod(r.accl);
    io.pod(r.vcoh); io.pod(r.vcol); io.pod(r.vcch); io.pod(r.vccl); io.pod(r.vce);
    io.pod(r.divin); io.pod(r.divout); io.pod(r.divdp);
    io.pod(r.running);
    io.atom(r.cyclesRun);
    // Estado privado del secuenciador. `running` es false en un estado bien tomado (la
    // tarea se termina antes de guardar), pero se serializa igual: el latch de ranura de
    // retardo y el PC de reanudacion son estado de la maquina, no un detalle del bucle.
    io.pod(r.curpc);
    io.pod(r.branch); io.pod(r.branchTarget); io.pod(r.branchState);
    io.pod(r.halt); io.pod(r.broke);
    io.pod(r.inDelay); io.pod(r.pendingTarget);
    io.pod(r.budget);
  }

  static auto rdp(StateIO& io, SoftRdp& d) -> void {
    io.tag("RDP ");
    io.pod(d.sawSyncFull); io.pod(d.stopAt);
    io.pod(d.ci_addr); io.pod(d.ci_width); io.pod(d.ci_size); io.pod(d.zi_addr);
    io.pod(d.ti_addr); io.pod(d.ti_width); io.pod(d.ti_size); io.pod(d.ti_fmt);
    io.pod(d.fill_color);
    io.pod(d.blend_color); io.pod(d.fog_color); io.pod(d.prim_color); io.pod(d.env_color);
    io.pod(d.prim_lod_frac); io.pod(d.prim_z);
    io.pod(d.k0); io.pod(d.k1); io.pod(d.k2); io.pod(d.k3); io.pod(d.k4); io.pod(d.k5);
    io.pod(d.other_lo); io.pod(d.other_hi);
    io.pod(d.combine_lo); io.pod(d.combine_hi);
    io.arr(d.comb, 2);
    io.arr(d.combined, 4);
    io.pod(d.sx0); io.pod(d.sy0); io.pod(d.sx1); io.pod(d.sy1);
    io.pod(d.pxWrites); io.pod(d.pxZWrites);
    io.arr(d.tiles, 8);
    io.blob(d.tmem, sizeof(d.tmem));
    io.arr(d.tlut, 256);
  }

  // Punteros de reanudacion del consumidor del FIFO. Con la cola drenada valen "nada
  // pendiente", pero se guardan para no depender de eso.
  static auto rdpFifo(StateIO& io, Memory& m) -> void {
    io.pod(m.rdpResume); io.pod(m.rdpLastEnd); io.pod(m.rdpHasResume);
  }
};

namespace {

auto visitCpu(StateIO& io, CPU& c) -> void {
  io.tag("CPU ");
  io.arr(c.gpr, 32);
  io.pod(c.pc); io.pod(c.nextPc); io.pod(c.curPc);
  io.pod(c.hi); io.pod(c.lo);
  io.arr(c.cop0, 32);
  io.arr(c.fpr, 32);
  io.pod(c.fcr0); io.pod(c.fcr31);
  io.pod(c.cp2latch); io.pod(c.cop0Unused);
  io.pod(c.llbit);
  io.pod(c.memAbort); io.pod(c.xlatCacheable);
  io.pod(c.inDelay); io.pod(c.justBranched); io.pod(c.jitDelaySlot);
  io.pod(c.timerIntr); io.pod(c.randomReload);
  io.pod(c.countFrac);   // resto del reloj Count: sin el, un estado cargado con CPI<2 arranca
                         // con la fase del divisor equivocada y el timer se desvia un tick
  io.pod(c.stallCycles); // ciclos de parada por fallo de cache aun sin volcar a Count: mismo
                         // motivo que countFrac -- es reloj de invitado a medio consumir
  io.pod(c.stallOps); io.pod(c.stallOpsRem);   // y su traduccion a ops, que es lo que ven el
                         // campo de video (viTick) y los plazos de PI/SI (cartNow)
  io.pod(c.halted);
  io.pod(c.retired); io.pod(c.exceptions);
  io.pod(c.icSeq);
  // TLB + las dos caches primarias. Son estado de la maquina, no un acelerador: la VR4300
  // no tiene coherencia, asi que una linea sucia que aun no ha bajado a RDRAM es un dato
  // que solo existe en la cache. Sin ellas, cargar un estado perderia esas escrituras.
  io.tag("TLB ");
  io.arr(c.tlb, 32);
  io.tag("DC  ");
  io.blob(c.dcache, sizeof(c.dcache));
  io.tag("IC  ");
  io.blob(c.icache, sizeof(c.icache));
}

auto visitRcp(StateIO& io, Rcp& p) -> void {
  io.tag("RCP ");
  io.pod(p.mi_mode); io.pod(p.mi_mask); io.atom(p.mi_intr);
  io.pod(p.mi_repeat_on); io.pod(p.mi_repeat_len);
  io.pod(p.sp_mem_addr); io.pod(p.sp_dram_addr); io.pod(p.sp_rd_len); io.pod(p.sp_wr_len);
  io.atom(p.sp_status); io.pod(p.sp_semaphore); io.pod(p.sp_pc); io.pod(p.sp_intr_on_break);
  io.pod(p.dpc_start); io.pod(p.dpc_end);
  io.atom(p.dpc_current); io.atom(p.dpcCurReads);
  io.pod(p.dpc_submitted); io.atom(p.dpc_status);
  io.atom(p.dpc_clock); io.atom(p.dpc_bufbusy); io.atom(p.dpc_pipebusy); io.atom(p.dpc_tmem);
  io.pod(p.dps_tbist); io.pod(p.dps_test_mode); io.pod(p.dps_buftest_addr);
  io.arr(p.dps_span, 128);
  io.pod(p.vi_ctrl); io.pod(p.vi_origin); io.pod(p.vi_width); io.pod(p.vi_intr); io.pod(p.vi_current);
  io.pod(p.viFlips); io.pod(p.viFields); io.pod(p.dpSyncs);
  io.pod(p.vi_burst); io.pod(p.vi_vsync); io.pod(p.vi_hsync); io.pod(p.vi_leap);
  io.pod(p.vi_hstart); io.pod(p.vi_vstart); io.pod(p.vi_vburst);
  io.pod(p.vi_xscale); io.pod(p.vi_yscale);
  io.pod(p.ai_dram); io.pod(p.ai_len); io.pod(p.ai_ctrl); io.pod(p.ai_status);
  io.pod(p.ai_dacrate); io.pod(p.ai_bitrate);
  io.arr(p.ai_fifo_addr, 2); io.arr(p.ai_fifo_len, 2);
  io.pod(p.ai_fifo_count); io.pod(p.ai_play_remaining);
  io.pod(p.aiLastRetired); io.pod(p.aiAcc);
  io.pod(p.pi_dram_addr); io.pod(p.pi_cart_addr); io.pod(p.pi_rd_len); io.pod(p.pi_wr_len);
  io.pod(p.pi_status); io.arr(p.pi_bsd, 8);
  io.pod(p.ri_mode); io.pod(p.ri_config); io.pod(p.ri_select); io.pod(p.ri_refresh);
  io.pod(p.si_dram_addr); io.pod(p.si_status);
}

auto visitMemory(StateIO& io, Memory& m) -> void {
  io.tag("MEM ");
  // El medio de guardado del cartucho va DENTRO del estado. Si no, cargar una partida
  // vieja dejaria la EEPROM con el contenido de la nueva y el juego veria un fichero de
  // guardado del futuro -- que es justo lo que hace irreproducible un savestate.
  u32 st = (u32)m.saveType; io.pod(st);
  if(!io.writing) m.saveType = (Memory::SaveType)st;
  u32 fm = (u32)m.flashMode; io.pod(fm);
  if(!io.writing) m.flashMode = (Memory::FlashMode)fm;
  io.pod(m.flashStatus); io.pod(m.flashErasePage); io.pod(m.flashWritePage);
  io.blob(m.flashPageBuf, sizeof(m.flashPageBuf));
  io.pod(m.cartLatch); io.pod(m.cartLatchValid); io.pod(m.cartLatchExpiry);
  io.pod(m.viLastRetired);
  io.blob(m.isvHdr, sizeof(m.isvHdr));
  StateVisitor::rdpFifo(io, m);
}

auto visitRam(StateIO& io, Memory& m) -> void {
  io.tag("RAM ");
  io.blob(m.rdram.data(), m.rdram.size());   // el tamano de RDRAM no cambia en caliente
  // Noveno bit de los chips RDRAM: ahi guarda el RDP los 2 bits bajos de la cobertura de
  // cada pixel y de ahi los lee el filtro de antialias del VI. Es estado de invitado como
  // la propia RDRAM; sin el, el frame siguiente a cargar un estado sale con los bordes mal.
  io.vecBlob(m.rdramHidden);
  io.vecBlob(m.dmem);
  io.vecBlob(m.imem);
  // Transaccion del SI en vuelo: es estado de invitado. Sin ella, un estado guardado justo
  // despues de que el juego arranque la lectura del mando se carga sin plazo armado, nadie
  // levanta MI_SI y el hilo que espera en la cola del SI no despierta nunca.
  io.pod(m.siBusy); io.pod(m.siToPif); io.pod(m.siDram); io.pod(m.siDoneAt);
  io.vecBlob(m.pifram);
  io.vecBlob(m.eeprom);
  io.vecBlob(m.saveRam);
  // Los pak son RAM viva: rebobinar un estado tiene que rebobinarlos. Van los cuatro
  // puertos, y con ellos el estado del motor del Rumble (que el juego enciende y apaga).
  for(auto& pp : m.padPort) { io.vecBlob(pp.mempak); io.pod(pp.rumble); }
}

// Peliculas TAS: los sondeos consumidos son estado de la partida igual que la RDRAM. Sin
// esto, cargar un estado rebobina el juego pero no la cinta, y la repeticion se desvia justo
// en el uso que junta las dos cosas (rehacer un tramo). Va aunque no haya pelicula: el campo
// existe siempre para que el formato no dependa de una variable de entorno.
auto visitMovie(StateIO& io) -> void {
  io.tag("MOVI");
  u64 p = movie::polls.load(std::memory_order_relaxed);
  io.pod(p);
  if(!io.writing && !io.bad) movie::seek(p);
}

auto visitAll(StateIO& io, System& sys) -> void {
  io.tag("SYS ");
  io.atom(sys.retiredInsns);
  io.pod(sys.rspCycles);
  io.pod(sys.rspPhase);
  visitCpu(io, sys.cpu);
  visitRcp(io, sys.memory.rcp);
  visitMemory(io, sys.memory);
  StateVisitor::rsp(io, sys.memory.rsp);
  StateVisitor::rdp(io, sys.memory.softRdp);
  visitRam(io, sys.memory);
  visitMovie(io);
  io.tag("END ");
}

// Cabecera: identidad de la maquina. Cargar un estado de OTRA ROM sobre esta la deja en un
// estado incoherente (RDRAM de un juego, ROM de otro) que se manifiesta como un cuelgue sin
// causa aparente, asi que se comprueba antes de tocar nada.
struct Header {
  u32 magic, version;
  u32 crc1, crc2;
  u32 romSizeKb;
  u32 rdramSize;
  char name[24];
};

auto makeHeader(System& sys) -> Header {
  Header h = {};
  h.magic = kMagic;
  h.version = kVersion;
  h.crc1 = sys.rom.header.crc1;
  h.crc2 = sys.rom.header.crc2;
  h.romSizeKb = (u32)(sys.rom.data.size() >> 10);
  h.rdramSize = (u32)sys.memory.rdram.size();
  std::snprintf(h.name, sizeof(h.name), "%s", sys.rom.header.name.c_str());
  return h;
}

// Deja el emulador coherente despues de reescribirle la memoria por debajo.
auto afterLoad(System& sys) -> void {
  CPU& c = sys.cpu;
  // El codigo compilado describe la RDRAM/IMEM que habia, no la que hay: cualquier bloque
  // superviviente ejecutaria instrucciones del estado anterior. Se tiran los dos dynarecs.
  if(c.jitCache) c.jitCache->clear();
  if(sys.memory.rsp.jc) sys.memory.rsp.jitInvalidate(0, Memory::IMEM_SIZE, sys.memory.imem.data());
  // Memoizaciones de traduccion: el TLB y el modo de la CPU acaban de cambiar de golpe.
  c.jitTlbValid = false;
  c.fetchLineVBase = 1;
  c.bumpXlat();
  // Contabilidad diferida de la cadena de bloques enlazados: no hay cadena en vuelo.
  c.jitPending = c.jitChain = c.jitChainOps = c.jitOpsBudget = c.jitGuard = 0;
  c.memAbort = false;
  c.refreshDebugArmed();
  // Punteros del RSP a DMEM/IMEM: los vectores no se han movido, pero volver a atarlos
  // no cuesta nada y cubre un vecBlob que hubiera cambiado su tamano.
  sys.memory.rsp.bindMem();
  // El bit de modo repeticion de MI vive duplicado en la guardia de store del dynarec.
  if(sys.memory.cpuStGuard) {
    if(sys.memory.rcp.mi_repeat_on) *sys.memory.cpuStGuard |= (u8)CPU::StGuardRepeat;
    else                            *sys.memory.cpuStGuard &= (u8)~(u8)CPU::StGuardRepeat;
  }
  // Horario del RCP: el anillo de tramos del RDP y los anclajes de las barreras hablan de
  // la partida anterior. Ver Memory::rcpSchedReset.
  sys.memory.rcpSchedReset();
  // El medio de guardado que acaba de entrar tiene que llegar al disco como cualquier otro.
  sys.memory.saveDirty = true;
}

}  // namespace

auto stateSlotPath(const System& sys, int slot) -> std::string {
  std::string p = archive::stripContainerExt(sys.romPath);
  usize dot = p.find_last_of('.');
  usize sep = p.find_last_of("/\\");
  if(dot != std::string::npos && (sep == std::string::npos || dot > sep)) p.resize(dot);
  char ext[8];
  std::snprintf(ext, sizeof(ext), ".st%d", slot);
  return p + ext;
}

auto captureState(System& sys, std::vector<u8>& out) -> void {
  // clear() conserva la capacidad: el rebobinado reutiliza el mismo vector campo tras campo
  // y no vuelve a pedir los 8 MB al asignador cada vez.
  out.clear();
  out.reserve(sys.memory.rdram.size() + (1u << 20));
  Header h = makeHeader(sys);
  out.insert(out.end(), (const u8*)&h, (const u8*)&h + sizeof(h));
  StateIO io;
  io.writing = true;
  io.out = &out;
  visitAll(io, sys);
}

auto restoreState(System& sys, const u8* data, usize len, std::string& err) -> bool {
  if(len < sizeof(Header)) { err = "estado truncado"; return false; }
  Header h = {};
  std::memcpy(&h, data, sizeof(h));
  Header want = makeHeader(sys);
  if(h.magic != kMagic)     { err = "no es un estado de kestrel64"; return false; }
  if(h.version != kVersion) { err = "version de estado incompatible"; return false; }
  if(h.crc1 != want.crc1 || h.crc2 != want.crc2) { err = "el estado es de otra ROM"; return false; }
  if(h.rdramSize != want.rdramSize) { err = "tamano de RDRAM distinto"; return false; }

  StateIO io;
  io.writing = false;
  io.in = data + sizeof(Header);
  io.len = len - sizeof(Header);
  visitAll(io, sys);
  if(io.bad) { err = "estado corrupto o incompleto"; return false; }
  afterLoad(sys);
  return true;
}

auto saveState(System& sys, const std::string& path, std::string& err) -> bool {
  std::vector<u8> buf;
  captureState(sys, buf);

  FILE* f = std::fopen(path.c_str(), "wb");
  if(!f) { err = "no se pudo abrir " + path; return false; }
  bool ok = std::fwrite(buf.data(), 1, buf.size(), f) == buf.size();
  std::fclose(f);
  if(!ok) { err = "escritura incompleta en " + path; return false; }
  return true;
}

auto loadState(System& sys, const std::string& path, std::string& err) -> bool {
  FILE* f = std::fopen(path.c_str(), "rb");
  if(!f) { err = "no existe " + path; return false; }
  std::fseek(f, 0, SEEK_END);
  long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if(n < (long)sizeof(Header)) { std::fclose(f); err = "fichero truncado"; return false; }
  std::vector<u8> buf((usize)n);
  bool rd = std::fread(buf.data(), 1, buf.size(), f) == buf.size();
  std::fclose(f);
  if(!rd) { err = "lectura incompleta"; return false; }

  // La lectura se valida entera sobre la marcha (marcas de seccion + longitudes). Un fallo
  // a mitad deja la maquina a medio reescribir, y por eso el llamante la tiene pausada y
  // avisa: no hay forma barata de deshacerlo sin duplicar los 8 MB de RDRAM.
  return restoreState(sys, buf.data(), buf.size(), err);
}

}  // namespace kestrel
