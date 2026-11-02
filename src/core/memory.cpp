#include "hbmeta.hpp"
#include "wrtag.hpp"
#include "memory.hpp"
#include "memory_internal.hpp"
#include "runtime.hpp"
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#ifdef _WIN32
#include <windows.h>
#endif

// Quien esta corriendo AHORA. Cada chip sella los eventos con SU propio reloj de invitado:
// el RSP escribe DPC_END desde su hilo y el instante correcto es el suyo, no el numero de
// instrucciones que la CPU lleve retiradas en ese momento de tiempo de pared.
constinit thread_local bool tlIsRspThread = false;
// Aplicando una entrada del diario DPC del RSP (ver Memory::dpLogApply): la escritura es del
// RSP y se sella con SU instante, aunque la ejecute otro hilo.
constinit thread_local bool tlDpLogApply = false;
constinit thread_local bool tlDpcKick = false;      // dpcAdvance lanzo un tramo (ver rspDpcWrite)
constinit thread_local kestrel::u64 tlDpLogAt = 0;
constinit thread_local bool tlInRetire = false, tlRetireArmed = false;   // ver rcpRetire

namespace kestrel {

// Tiempo de CPU (usuario+nucleo) que lleva gastado un hilo, en nanosegundos. 0 si no se
// puede saber. Es lo que hay que comparar contra el tiempo de PARED que miden rspBusyNs y
// rdpBusyNs: si un worker esta dentro de un trabajo el 80% de la pared pero solo ha gastado
// el 40% de CPU, no va lento -- es que no le estan dando nucleo.
static auto threadCpuNs(void* h) -> u64 {
#ifdef _WIN32
  if(!h) return 0;
  FILETIME cre, ex, kern, usr;
  if(!GetThreadTimes((HANDLE)h, &cre, &ex, &kern, &usr)) return 0;
  auto to64 = [](const FILETIME& f) { return ((u64)f.dwHighDateTime << 32) | f.dwLowDateTime; };
  return (to64(kern) + to64(usr)) * 100ull;   // unidades de 100 ns
#else
  (void)h; return 0;
#endif
}

// Duplica el pseudo-handle del hilo actual en uno real y utilizable desde otro hilo.
auto selfThreadHandle() -> void* {
#ifdef _WIN32
  HANDLE h = nullptr;
  DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h,
                  0, FALSE, DUPLICATE_SAME_ACCESS);
  return h;
#else
  return nullptr;
#endif
}

// Reparte los tres hilos calientes (CPU, RSP, RDP) en nucleos FISICOS distintos.
//
// Por que hace falta: en una maquina con SMT dos hilos logicos del mismo nucleo comparten las
// unidades de emision. El planificador de Windows no sabe que estos tres se pelean por el mismo
// recurso ni que uno de ellos (el de CPU) es el palo largo, y emparejarlo con el del RSP -- que
// se pasa el 61 % del tiempo girando en una cita (perfil de anfitrion, Perfect Dark PAL) -- le
// quita la mitad del nucleo al que manda. Con 4 nucleos fisicos sobran sitios para no compartir.
//
// `slot` es el indice logico del nucleo fisico que le toca a cada hilo (0, 1, 2...); se traduce
// a procesador logico multiplicando por la cantidad de hilos por nucleo. Si la maquina no tiene
// SMT el factor es 1 y sigue saliendo un nucleo por hilo. Si hay menos nucleos que hilos se da
// la vuelta con el modulo y quedan compartidos, que es lo que habria pasado igualmente.
//
// Solo coste de anfitrion: ni una fecha del invitado depende de en que nucleo corre nadie.
//
// MEDIDO Y DESCARTADO (Perfect Dark PAL, 89 cuadros, media de 3, i7-870 4c/8h): fijado 4779 ms,
// suelto 4496. Fijar es un 6 % PEOR. El planificador de Windows ya evita emparejar los hilos
// calientes, y ademas mueve el de CPU cuando otro proceso le pisa el nucleo, cosa que la
// mascara dura impide. Se queda APAGADO; sigue aqui porque en otra topologia puede cambiar el
// signo y asi no hay que volver a escribirlo.
// KESTREL_AFFINITY=1 lo enciende.
auto pinToPhysicalCore(u32 slot) -> void {
#ifdef _WIN32
  static const bool on = [] {
    const char* e = std::getenv("KESTREL_AFFINITY");
    return e && *e && std::strcmp(e, "0") && std::strcmp(e, "off");
  }();
  if(!on) return;
  SYSTEM_INFO si{}; GetSystemInfo(&si);
  const u32 logical = si.dwNumberOfProcessors ? si.dwNumberOfProcessors : 1u;
  if(logical < 2) return;
  // Hilos por nucleo: la relacion entre procesadores logicos y nucleos fisicos de verdad.
  u32 perCore = 1;
  {
    DWORD len = 0;
    GetLogicalProcessorInformation(nullptr, &len);
    if(len) {
      std::vector<SYSTEM_LOGICAL_PROCESSOR_INFORMATION> v(len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION) + 1);
      len = (DWORD)(v.size() * sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));
      if(GetLogicalProcessorInformation(v.data(), &len)) {
        u32 cores = 0;
        for(u32 i = 0; i < len / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION); i++)
          if(v[i].Relationship == RelationProcessorCore) cores++;
        if(cores) perCore = logical / cores ? logical / cores : 1u;
      }
    }
  }
  const u32 cores = logical / perCore ? logical / perCore : 1u;
  const u32 cpu   = ((slot % cores) * perCore) % logical;
  SetThreadAffinityMask(GetCurrentThread(), (DWORD_PTR)1 << cpu);
#else
  (void)slot;
#endif
}

auto Memory::pinCpuThread() -> void { pinToPhysicalCore(0); }

auto Memory::sampleWorkerCpu() -> void {
  // Se llama SIEMPRE desde el hilo de CPU (heartbeat e informe final), asi que el handle
  // propio se toma aqui la primera vez y ya vale para todas.
  if(!cpuThreadH) { cpuThreadH = selfThreadHandle(); }
  rspCpuNs.store(threadCpuNs(rspThreadH), std::memory_order_relaxed);
  rdpCpuNs.store(threadCpuNs(rdpThreadH), std::memory_order_relaxed);
  cpuCpuNs.store(threadCpuNs(cpuThreadH), std::memory_order_relaxed);
}

auto Memory::reset(bool expansionPak) -> void {
  rdram.assign(expansionPak ? RDRAM_SIZE_EXPANDED : 0x0040'0000, 0);
  rdramHidden.assign(rdram.size() >> 1, 0);
  wrtag::init((u32)rdram.size());
  dmem.assign(DMEM_SIZE, 0);
  imem.assign(IMEM_SIZE, 0);
  pifram.assign(PIFRAM_SIZE, 0);
  // Encendido: el PIF arranca en modo reset (ver pifCommand). El plazo de 0x08 lo arma el
  // primer cierre de campo, que es cuando el reloj de invitado ya existe.
  pifBootMode = true; pifRomLocked = false; pifFrozen = false; pifFreezeWhy = nullptr;
  pifSumGot = false; std::memset(pifSum, 0, sizeof pifSum); pifBootDeadline = 0;
  // Size the save backings to the resolved device and blank them on a cold boot.
  // EEPROM lives on the joybus; SRAM/FlashRAM on the PI bus. Both start erased (0xFF
  // is flash-erased; SRAM/EEPROM conventionally 0x00 until written).
  if(isEeprom() && eeprom.empty())  eeprom.assign(saveSize(), 0);
  if(!isEeprom() && saveType != SaveType::None && saveRam.empty())
    saveRam.assign(saveSize(), isFlash() ? 0xFF : 0x00);
  flashMode = FlashMode::Status;
  // Los cuatro conectores. El 1 viene enchufado y los demas vacios, que es como se
  // encuentra una consola encima de la mesa; el menu de la ventana los cambia en caliente
  // (rt::padOn/padAcc) y KESTREL_PADS deja hacerlo sin ventana, por ejemplo en un gate.
  //   KESTREL_PADS=1101   mandos 1, 2 y 4
  //   KESTREL_PADACC=1102 accesorio por puerto: 0 nada, 1 Controller Pak, 2 Rumble Pak,
  //                       3 Transfer Pak (KESTREL_TPAK_ROM1..4 = ROM de GB de cada uno;
  //                       KESTREL_TPAK_ROM vale por la del puerto 1)
  // El Controller Pak es del MANDO, no de la cartuchera: existe aunque el juego no guarde.
  const char* pads = std::getenv("KESTREL_PADS");
  const char* accs = std::getenv("KESTREL_PADACC");
  const char* oldMempak = std::getenv("KESTREL_MEMPAK");   // compatibilidad: solo puerto 1
  //   KESTREL_PADTYPE=0200 tipo por puerto: 0/1 mando, 2 raton (id 0x0200; la lectura 0x01
  //                        da botones A/B y el desplazamiento con signo en los bytes del stick)
  const char* types = std::getenv("KESTREL_PADTYPE");
  for(int i = 0; i < 4; i++) {
    padPort[i].mouse = types && types[0] && types[i] == '2';
    bool on = pads && pads[0] ? (pads[i] ? pads[i] != '0' : false) : (i == 0);
    u8   ac = accs && accs[i] ? (u8)(accs[i] - '0') : (u8)1;
    if(i == 0 && oldMempak && oldMempak[0] == '0') ac = 0;
    if(ac > 3) ac = 0;
    padPort[i].connected = on;
    padPort[i].accessory = ac;
    rt::padOn[i].store(on, std::memory_order_relaxed);
    rt::padAcc[i].store(ac, std::memory_order_relaxed);
    if(ac == 1 && padPort[i].mempak.empty()) mempakFormat(padPort[i].mempak);
    // Transfer Pak: apagado al encender la consola (el SDK lo enciende en osGbpakInit).
    padPort[i].tp = PadPort::TpakRegs{};
    char var[24];
    std::snprintf(var, sizeof var, "KESTREL_TPAK_ROM%d", i + 1);
    const char* gbp = std::getenv(var);
    if(!gbp && i == 0) gbp = std::getenv("KESTREL_TPAK_ROM");
    if(gbp) {
      std::lock_guard<std::mutex> lk(rt::tpakMx);
      if(rt::tpakRom[i] != gbp || !padPort[i].gb.loaded()) {
        rt::tpakRom[i] = gbp;
        rt::tpakGen[i].fetch_add(1, std::memory_order_relaxed);
      }
    }
  }
  if(const char* b = std::getenv("KESTREL_BUTTONS")) padButtons() = (u32)strtoul(b, nullptr, 16);
  initMap();
}

auto Memory::initMap() -> void {
  regions.clear();
  regions.push_back({"RDRAM",   BASE_RDRAM,  (u32)rdram.size(),  rdram.data()});
  regions.push_back({"DMEM",    BASE_DMEM,   (u32)dmem.size(),   dmem.data()});
  regions.push_back({"IMEM",    BASE_IMEM,   (u32)imem.size(),   imem.data()});
  regions.push_back({"PIF_RAM", BASE_PIFRAM, (u32)pifram.size(), pifram.data()});
  if(!rom.empty()) {
    regions.push_back({"CART_ROM", BASE_CART, (u32)rom.size(), rom.data()});
  }
  if(!eeprom.empty()) regions.push_back({"EEPROM", 0, (u32)eeprom.size(), eeprom.data()});
  if(!saveRam.empty()) regions.push_back({"SAVE", 0x0800'0000, (u32)saveRam.size(), saveRam.data()});
}

auto Memory::loadRom(std::vector<u8> image) -> void {
  rom = std::move(image);
  // Tiempos del bus del cartucho, como los pone la consola. En el aparato los escribe el
  // IPL2 copiando los bytes 0x01..0x03 de la cabecera a PI_BSD_DOM1_{RLS,PGS,PWD,LAT}; aqui
  // el arranque es HLE, asi que si no se hace a mano los registros se quedan a cero y el
  // modelo de duracion del DMA del PI (piXferCycles) mediria un cartucho imposible.
  // Reparto de bits (n64brew, ROM Header): 0x00 reservado (0x80 en los comerciales, NO es
  // configuracion), 0x01 bits 4-5 = RLS y bits 0-3 = PGS, 0x02 = PWD, 0x03 = LAT.
  // El valor de casi todos los cartuchos es 0x80371240 -> LAT 64, PWD 18, PGS 7, RLS 3.
  if(rom.size() >= 4) {
    rcp.pi_bsd[0] = rom[3];                 // 0x14 DOM1_LAT
    rcp.pi_bsd[1] = rom[2];                 // 0x18 DOM1_PWD
    rcp.pi_bsd[2] = (u32)(rom[1] & 0x0fu);  // 0x1C DOM1_PGS
    rcp.pi_bsd[3] = (u32)((rom[1] >> 4) & 0x03u);   // 0x20 DOM1_RLS
  }
  // Dominio 2 (SRAM/FlashRAM y el bus de 0x05000000) se queda como esta: la cabecera no lo
  // lleva, el IPL2 no lo toca y el juego que usa la pila del save lo programa el mismo antes
  // de su primer DMA (el SDK pone LAT 5, PWD 0x0C, PGS 0x0D, RLS 2). Inventarle un valor de
  // reinicio seria fingir un dato que no tenemos; con los registros a cero el modelo cuenta
  // de menos, que es exactamente lo que hacia antes de existir el plazo.
  resolveSaveType();
  applyHomebrewPads();
  initMap();  // refresh so CART_ROM appears
}

// Cabecera avanzada de homebrew (ID "ED"): 0x34-0x37 dicen que va en cada puerto. El juego
// sabe lo que necesita (un raton, un Transfer Pak...), asi que manda sobre la configuracion
// general del perfil en los puertos con dato (0 = sin dato: se queda lo del usuario), como en
// ares y Parallel Launcher. KESTREL_HBPADS=0 la ignora. El menu sigue cambiandolo en caliente.
auto Memory::applyHomebrewPads() -> void {
  if(rom.size() < 0x40) return;
  const hb::Header h = hb::parseHeader(rom.data());
  if(!h.ed) return;
  if(const char* e = std::getenv("KESTREL_HBPADS"); e && e[0] == '0') return;
  for(int i = 0; i < 4; i++) {
    const u8 v = h.ctrl[i];
    if(v == 0x00) continue;
    PadPort& pp = padPort[i];
    pp.connected = v != 0xFF;
    rt::padOn[i].store(pp.connected, std::memory_order_relaxed);
    pp.mouse = v == 0x80;
    if(v < 0x80) {
      const u8 ac = v == 0x01 ? 2 : v == 0x02 ? 1 : v == 0x03 ? 3 : 0;
      pp.accessory = ac;
      rt::padAcc[i].store(ac, std::memory_order_relaxed);
      if(ac == 1 && pp.mempak.empty()) mempakFormat(pp.mempak);
    }
    std::fprintf(stderr, "[pad] cabecera ED: puerto %d = %s\n", i + 1, hb::ctrlName(v));
  }
}

// Duracion de un DMA del PI en ciclos del RCP, con los tiempos que programan PI_BSD_DOM*.
// El bus del cartucho es de 16 bits y va por PAGINAS: tras mandar la direccion base se pueden
// leer 2^(PGS+2) bytes seguidos, y al cruzar la pagina hay que mandarla otra vez.
//   * LAT+1 ciclos entre la direccion (flanco de ALE_L) y el primer /RD o /WR de la pagina
//   * PWD+1 ciclos con /RD o /WR abajo, por cada 16 bits
//   * RLS+1 ciclos con /RD o /WR arriba entre dos palabras de 16 bits
// Con los valores de fabrica (LAT 64, PWD 18, RLS 3, PGS 7) sale 65 + 256*23 = 5953 ciclos por
// pagina de 512 B, o sea 95,2 us y 5,375 MB/s: la tasa de lectura de cartucho conocida.
auto Memory::piXferCycles(u32 cartPhys, u32 len) const -> u64 {
  const bool dom2 = (cartPhys >= 0x0500'0000u && cartPhys < 0x0600'0000u)
                 || (cartPhys >= 0x0800'0000u && cartPhys < 0x1000'0000u);
  const u32 b = dom2 ? 4u : 0u;
  const u64 lat = rcp.pi_bsd[b + 0] & 0xffu, pwd = rcp.pi_bsd[b + 1] & 0xffu;
  const u64 pgs = rcp.pi_bsd[b + 2] & 0x0fu, rls = rcp.pi_bsd[b + 3] & 0x03u;
  const u64 pageBytes = 1ull << (pgs + 2);
  const u64 perWord = (pwd + 1) + (rls + 1);
  u64 cycles = 0, addr = cartPhys, left = len;
  while(left) {
    const u64 inPage = pageBytes - (addr & (pageBytes - 1));
    const u64 take = inPage < left ? inPage : left;
    cycles += (lat + 1) + ((take + 1) / 2) * perWord;
    addr += take; left -= take;
  }
  return cycles;
}

// Arma el plazo del PI: el motor queda OCUPADO y MI_PI no se levanta hasta que el reloj de
// invitado llega al final. La copia a la RDRAM ya esta hecha cuando se llama (el motor del
// aparato la va escribiendo durante la ventana, y un juego que lea antes de la interrupcion
// esta leyendo basura en las dos maquinas); lo que se retrasa es el FINAL, que es lo unico
// que el invitado observa de verdad.
// A/B: KESTREL_PIINSTANT=1 devuelve el comportamiento anterior (termina en la instruccion que
// lo arranca). Solo para bisecar: en hardware NO pasa.
// Un plazo NUEVO armado desde el hilo de CPU: se anula el permiso de la cadena del JIT (lo
// mira el prologo del bloque siguiente) y se sube el sello que hace SALIR al bloque en curso
// en la instruccion exacta (lo miran los ayudantes de memoria, ver CPU::jitMemOp y armEpoch).
// Desde el hilo del RSP -- dpLogApply cuando la CPU se para o cuando salta el salvavidas de
// pared -- solo lo primero, que el sello es del hilo de CPU y tocarlo desde fuera seria a la
// vez carrera y una salida de bloque decidida por el anfitrion.
auto Memory::jitCancelChain() -> void {
  if(jitGuardPtr) *jitGuardPtr = 0;
  if(!tlIsRspThread) armEpoch++;
}

auto Memory::piArm(u32 cartPhys, u32 len) -> void {
  static const bool instant = std::getenv("KESTREL_PIINSTANT") != nullptr;
  if(instant || !len) { rcp.pi_status = 0x8; raiseIntr(MI_PI); return; }
  piBusy   = true;
  piDoneAt = cartNow() + rcpCyclesToInsns(piXferCycles(cartPhys, len));
  rcp.pi_status = PI_DMA_BUSY;
  // Lo arranca un store que con el JIT puede ir en mitad de una cadena enlazada cuyo permiso
  // no conocia este plazo (mismo caso que siDma).
  jitCancelChain();
}

// Vencimiento del plazo del PI.
auto Memory::piFinish() -> void {
  if(!piBusy) return;
  piBusy = false;
  rcp.pi_status = 0x8;   // DMA done: interrupt pending (bit3), busy bits clear
  raiseIntr(MI_PI);
}

auto Memory::findRegion(const std::string& query) -> Region* {
  auto lower = [](std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return (char)std::tolower(c); });
    return s;
  };
  std::string q = lower(query);
  Region* exact = nullptr;
  Region* sub = nullptr;
  for(auto& r : regions) {
    std::string n = lower(r.name);
    if(n == q) { exact = &r; break; }
    if(!sub && n.find(q) != std::string::npos) sub = &r;
  }
  return exact ? exact : sub;
}

auto Memory::resolve(u32 addr, u32& remaining) -> u8* {
  // KSEG0/KSEG1 → physical: mask off the top segment bits so 0x80.../0xA0... alias RAM.
  addr &= 0x1fff'ffff;
  // RSP DMEM/IMEM span 0x04000000-0x0403FFFF: 8 KB of SP mem mirrored every 0x2000
  // (bit 12 selects IMEM). Must resolve here or these accesses fall through to the
  // SP *register* decode (0x04040000+) and get misread as DMA/status writes.
  if(addr >= BASE_DMEM && addr < BASE_SP) {
    u32 o = addr & 0x1fff;
    if(o & 0x1000) { remaining = IMEM_SIZE - (o & 0xfff); return imem.data() + (o & 0xfff); }
    remaining = DMEM_SIZE - o; return dmem.data() + o;
  }
  for(auto& r : regions) {
    if(r.data && r.contains(addr)) {
      u32 off = addr - r.base;
      remaining = r.size - off;
      return r.data + off;
    }
  }
  remaining = 0;
  return nullptr;
}

auto Memory::isMmio(u32 addr) const -> bool {
  // RCP register space (RAM-backed DMEM/IMEM are resolved before this is asked).
  return addr >= BASE_RDRAMREG && addr < 0x0490'0000;
}

// --- big-endian reads --------------------------------------------------------
auto Memory::read8(u32 addr) -> u8 {
  u32 mc = addr & 0x1fff'ffff;
  if(isCart(mc)) return (u8)cartRead(mc, 1);
  if(isSaveDomain(mc) && saveType != SaveType::None) return (u8)saveRead(mc, 1);
  u32 rem; u8* p = resolve(addr, rem);
  if(p && rem >= 1) return p[0];
  u32 m = addr & 0x1fff'ffff;
  if(isMmio(m)) { u32 w = mmioRead32(m & ~3u); return (u8)(w >> (8 * (3 - (m & 3)))); }
  return 0;
}
auto Memory::read16(u32 addr) -> u16 {
  u32 mc = addr & 0x1fff'ffff;
  if(isCart(mc)) return (u16)cartRead(mc, 2);
  if(isSaveDomain(mc) && saveType != SaveType::None) return (u16)saveRead(mc, 2);
  u32 rem; u8* p = resolve(addr, rem);
  if(p && rem >= 2) return (u16)(p[0] << 8 | p[1]);
  u32 m = addr & 0x1fff'ffff;
  if(isMmio(m)) { u32 w = mmioRead32(m & ~3u); return (u16)(w >> (16 * (1 - ((m >> 1) & 1)))); }
  return 0;
}
auto Memory::read32(u32 addr) -> u32 {
  u32 mc = addr & 0x1fff'ffff;
  if(isCart(mc)) return cartRead(mc, 4);
  if(isSaveDomain(mc) && saveType != SaveType::None) return saveRead(mc, 4);
  u32 rem; u8* p = resolve(addr, rem);
  if(p && rem >= 4) return (u32)p[0] << 24 | (u32)p[1] << 16 | (u32)p[2] << 8 | (u32)p[3];
  u32 m = addr & 0x1fff'ffff;
  if(isMmio(m)) return mmioRead32(m & ~3u);
  return 0;
}
auto Memory::read64(u32 addr) -> u64 {
  return (u64)read32(addr) << 32 | read32(addr + 4);
}

// --- debug store watchpoint --------------------------------------------------
auto Memory::watchHit(u32 physAddr, u32 nbytes, u64 value, bool dma) -> void {
  if(!watchAddr) return;
  u32 lo = physAddr, hi = physAddr + nbytes;
  if(hi <= watchAddr || lo >= watchAddr + watchLen) return;
  u32 old = physAddr < rdram.size() ? rdram[physAddr] : 0;
  std::fprintf(stderr, "[watch] %s @phys 0x%08x (%u B) old=0x%02x new=0x%llx  by pc=0x%08x\n",
               dma ? "DMA" : "CPU", physAddr, nbytes, old, (unsigned long long)value,
               (u32)storePc);
  std::fflush(stderr);
}

// --- big-endian writes -------------------------------------------------------
// IS-Viewer overlay at physical 0x13FF0000. Emulators intercept this window
// regardless of cart contents; n64-systemtest and libdragon use it for text I/O.
//   0x13FF0014 : write byte-count → flush that many staged bytes to stdout
//   0x13FF0020 : start of the text staging buffer
auto Memory::isvWrite(u32 phys, u32 value, u32 nbytes) -> bool {
  if(phys < 0x13ff'0000 || phys >= 0x1400'0000) return false;
  if(isv.empty()) isv.assign(0x10000, 0);
  if(phys == 0x13ff'0014) {  // flush trigger (word write)
    u32 len = value;
    if(len > isv.size()) len = (u32)isv.size();
    if(len) { std::fwrite(isv.data(), 1, len, stdout); std::fflush(stdout);
      // Los interruptores de traza se leen UNA vez. getenv de la CRT recorre el bloque de
      // entorno entero con un candado dentro, y estas pruebas viven en el camino de cada
      // escritura MMIO y de cada trabajo de RDP: el perfilador de host las veia como ~20%
      // del hilo del RSP dentro de ucrtbase. El entorno no cambia tras arrancar: exacto.
      static const bool fpDbg = std::getenv("KESTREL_FPDBG") != nullptr;
      if(fpDbg) {
        std::string_view sv((const char*)isv.data(), len);
        if(sv.find("Got unhandled") != std::string_view::npos) {
          std::fprintf(stderr, "[ISV-UNHANDLED]\n"); std::fflush(stderr); } } }
    return true;
  }
  if(phys >= 0x13ff'0020) {
    u32 off = phys - 0x13ff'0020;
    for(u32 i = 0; i < nbytes && off + i < isv.size(); i++)  // big-endian bytes
      isv[off + i] = (u8)(value >> (8 * (nbytes - 1 - i)));
    return true;
  }
  if(phys == 0x13ff'001c) { fbSeqReq += value; return true; }   // captura marcada por el invitado (fbSeqField)
  // Header registers below the buffer. The real IS-Viewer is a devkit cartridge with
  // RAM in this window, and probes rely on that: libdragon's isviewer_init() writes a
  // magic word to 0x13FF0000 and reads it back to decide the device is present. Backing
  // the header with actual storage is what the hardware does; leaving the readback to
  // the PI bus latch only happens to work.
  u32 off = phys - 0x13ff'0000;
  for(u32 i = 0; i < nbytes && off + i < sizeof(isvHdr); i++)
    isvHdr[off + i] = (u8)(value >> (8 * (nbytes - 1 - i)));
  return true;
}

// Readback of the IS-Viewer window. Mirrors isvWrite: header registers below 0x20,
// text staging buffer above it. Returns false when the window is untouched, so an
// unprobed cartridge keeps its normal ROM/open-bus behaviour.
auto Memory::isvRead(u32 phys, u32 nbytes, u32& out) -> bool {
  if(phys < 0x13ff'0000 || phys >= 0x1400'0000 || isv.empty()) return false;
  auto gather = [&](const u8* p, u32 avail) {   // big-endian, zero past the region
    u32 v = 0;
    for(u32 i = 0; i < nbytes; i++) v = (v << 8) | (i < avail ? p[i] : 0u);
    return v;
  };
  if(phys >= 0x13ff'0020) {
    u32 off = phys - 0x13ff'0020;
    if(off >= isv.size()) return false;
    out = gather(isv.data() + off, (u32)isv.size() - off);
    return true;
  }
  u32 off = phys - 0x13ff'0000;
  out = gather(isvHdr + off, (u32)sizeof(isvHdr) - off);
  return true;
}

// --- cartridge / PI bus ------------------------------------------------------
// Aligned 32-bit big-endian ROM word; 0 past the loaded image.
auto Memory::cartRom32(u32 phys) -> u32 {
  u32 off = phys - 0x1000'0000;
  if(off + 4 > rom.size()) {
    u32 w = 0;
    for(u32 i = 0; i < 4; i++) { w <<= 8; if(off + i < rom.size()) w |= rom[off + i]; }
    return w;
  }
  return (u32)rom[off] << 24 | (u32)rom[off + 1] << 16 | (u32)rom[off + 2] << 8 | rom[off + 3];
}

// IO_BUSY (PI_STATUS bit 1) se apaga SOLO. En el N64 la escritura pendiente en el bus del
// PI dura lo que dura el ciclo del bus y despues el bit cae; nadie tiene que leer el
// cartucho para que baje. Aqui solo lo bajaba cartRead(), asi que un juego que escribe en
// el espacio del cartucho y luego espera en PI_STATUS giraba para siempre. Es justo lo que
// hace el `dma_wait()` de libdragon:
//
//     lw   v0, 16(v1)        # v1 = 0xA4600000, +0x10 = PI_STATUS
//     andi v0, v0, 0x3       # DMA_BUSY | IO_BUSY
//     bnez v0, -8
//
// snapper64 se quedaba ahi (pc=0x8005b6c8) y nunca llegaba a configurar el VI: origin=0,
// width=0, ctrl=0 tras 1151 campos. El plazo es el mismo latch que ya existia
// (cartLatchTtl), medido en el reloj de invitado, asi que una lectura inmediata de
// PI_STATUS despues de la escritura sigue viendo IO_BUSY como en HW.
//
// Son dos plazos distintos. IO_BUSY es el ciclo de escritura del bus (n64brew "Peripheral
// Interface": ALE, LAT+1, y por cada media palabra PWD+1 abajo y RLS+1 arriba con los tiempos
// del dominio), unos 110 ciclos del RCP con los de fabrica. El valor que queda en las lineas
// es carga y dura mas (~330 ciclos de CPU, ver CART_LATCH_TTL_CYCLES): una lectura del
// cartucho lo devuelve aunque el PI ya este libre. Atarlos colgaba libdragon: su sondeo de
// flashcart hace io_write y en seguida encola un DMA; pi_enqueue ve IO_BUSY y no lo arranca,
// y dma_wait ya no vuelve a leer PI_STATUS (la cola no vacia corta la condicion), asi que
// se queda esperando una interrupcion que nunca llega (test_debug_sdfs, pc en dma.c:483).
auto Memory::piIoDecay() -> void {
  const u64 now = cartNow();
  if((rcp.pi_status & 0x2u) && now >= cartIoBusyUntil) rcp.pi_status &= ~0x2u;
  if(cartLatchValid && now >= cartLatchExpiry) cartLatchValid = false;
}

// CPU read from cart space. If a write recently latched a value onto the PI bus
// (and it has not decayed), that value is returned and consumed. Otherwise a real
// ROM read, mangled by the 16-bit-wide PI bus: sub-word reads only reach the UPPER
// halfword of each 32-bit word (a LB/LH of the lower half aliases to the upper).
auto Memory::cartRead(u32 phys, u32 nbytes) -> u32 {
  u32 isvVal = 0;
  if(isvRead(phys, nbytes, isvVal)) return isvVal;
  if(scOn && scRead(phys, isvVal)) {
    piIoDecay();
    if(nbytes == 4) return isvVal;
    const u16 hw = (u16)(isvVal >> ((phys & 2) ? 0 : 16));
    return nbytes == 2 ? hw : (phys & 1) ? (hw & 0xff) : (hw >> 8);
  }
  u64 now = cartNow();
  if(cartLatchValid && now < cartLatchExpiry) {
    u32 v = cartLatch;
    cartLatchValid = false;
    rcp.pi_status &= ~0x2u;                 // reading the bus clears IOBUSY
    if(nbytes == 1) return (v >> 24) & 0xff;
    if(nbytes == 2) return (v >> 16) & 0xffff;
    return v;
  }
  piIoDecay();
  if(nbytes == 4) return cartRom32(phys & ~3u);
  u32 w  = cartRom32((phys + 2) & ~3u);     // 16-bit bus: address the upper halfword
  u16 hw = (u16)(w >> 16);
  if(nbytes == 2) return hw;
  return (phys & 1) ? (hw & 0xff) : ((hw >> 8) & 0xff);
}

// CPU write to cart space. The first write latches its value onto the PI bus (later
// writes are ignored until the latch is consumed or decays). Sub-word writes place
// their bytes in the upper bits; a 64-bit store keeps only its upper 32 bits.
auto Memory::cartWrite(u32 phys, u64 value, u32 nbytes) -> void {
  u64 now = cartNow();
  piIoDecay();
  if(rcp.pi_status & 0x2u) return;          // el PI ignora escrituras mientras esta ocupado
  u32 v;
  switch(nbytes) {
    case 1:  v = ((u32)value & 0xff) << 24; break;
    case 2:  v = ((u32)value & 0xffff) << 16; break;
    case 8:  v = (u32)(value >> 32); break;
    default: v = (u32)value; break;
  }
  // Un registro del flashcart recoge la escritura: el bus queda ocupado lo mismo, pero no
  // hay valor flotando en las lineas que devolver en la siguiente lectura.
  if(scOn && scWrite(phys, v)) {
    cartIoBusyUntil = now + rcpCyclesToInsns(piXferCycles(phys, 4));
    rcp.pi_status |= 0x2u;
    return;
  }
  if(cartLatchValid) return;                // first-write-wins
  cartLatch = v;
  cartLatchValid = true;
  cartLatchExpiry = now + cartLatchTtl;
  cartIoBusyUntil = now + rcpCyclesToInsns(piXferCycles(phys, 4));
  rcp.pi_status |= 0x2u;                     // IOBUSY set while the bus write is pending
}

auto Memory::wordStoreQuirk(u32 phys, u64 reg, u32 width) -> bool {
  bool sp  = phys >= BASE_DMEM   && phys < BASE_SP;
  bool pif = phys >= BASE_PIFRAM && phys < BASE_PIFRAM + PIFRAM_SIZE;
  // Registros del RCP: tampoco miran los byte-enables. Un SB/SH/SD les llega como escritura de
  // palabra con el dato desplazado, igual que a DMEM. Antes write8/16 hacian leer-modificar-
  // escribir: la lectura podia tener efectos (o devolver otra cosa que lo escrito) y el byte
  // vecino conservaba el valor viejo en vez de recibir el del registro de la CPU.
  bool mmio = !sp && isMmio(phys);
  if(!sp && !pif && !mmio) return false;
  if(width == 4) return false;                 // SW behaves normally on these devices
  // SB/SH/SD a DMEM/IMEM: esta rareza NO pasa por CPU::ramUncached, asi que sin esto se cuela
  // una escritura de la CPU en la memoria que el RSP lee ahora mismo, y en Threaded con
  // adelanto esa memoria es el futuro del RSP. Misma cita que el resto. Ver cpuRamWrBarrier.
  if(sp && wrBarrierOn() && !tlIsRspThread) cpuRamWrBarrier(cartNow());
  u32 out;
  if(width == 8) {
    out = (u32)(reg >> 32);                     // SD writes only the upper 32 bits
  } else {
    u32 byteInWord = phys & 3;                  // width is 1 (SB) or 2 (SH)
    u32 shift = (4 - width - byteInWord) * 8;    // land the unit, spill the rest, zero below
    out = (u32)(reg << shift);
  }
  write32(phys & ~3u, out);                     // full aligned word write (byte-enables ignored)
  return true;
}

// RDRAM init/repeat broadcast. `value` is the FULL store-source register (not the
// size-truncated byte lane): in init mode the whole datapath word sits on the bus,
// positioned by the store address, and the RCP replays that `unit`-wide pattern
// across every unit of the span, wrapping the address within the current 2 KiB page.
//
// The pattern is the datapath word (low 32 bits for SB/SH/SW, full 64 for SD) shifted
// left so the store's low byte lands at its address lane; lanes shifted past the low
// end become zero — this is the "masking" 8/16-bit writes leave behind. Because the
// whole register shifts, an SB at an odd lane also drips the neighbouring register
// bytes into the lower lanes (e.g. 0x..DEF1 -> bytes DE,F1), matching hardware.
auto Memory::miRepeatStore(u32 phys, u64 value, u32 sz) -> void {
  u32 unit = (sz == 8) ? 8u : 4u;                 // 32-bit datapath for SB/SH/SW, 64-bit for SD
  u32 base = phys & (unit - 1);                   // store's byte lane within its unit
  u32 shift = (unit - sz - base) * 8;             // left-shift; >=0 since stores are aligned to size
  u64 word  = (unit == 8) ? value : (u64)(u32)value;
  word <<= shift;                                 // low lanes zero-fill (the undriven-lane masking)
  u8 pat[8] = {};
  for(u32 p = 0; p < unit; p++) pat[p] = (u8)(word >> (8 * (unit - 1 - p)));  // big-endian lanes
  u32 end  = (phys & ~7u) + rcp.mi_repeat_len;    // span counts from the start of the 8-byte column
  u32 page = phys & ~0x7ffu;                       // 2 KiB wrap region
  // Difusion a RDRAM desde el hilo de CPU: escribe RDRAM de verdad, asi que necesita la misma
  // cita que un volcado de linea sucia (ver cpuRamWrBarrier).
  if(wrBarrierOn() && !tlIsRspThread) cpuRamWrBarrier(cartNow());
  dmaSettle(page, (u64)page + 0x800);
  for(u32 j = phys; j < end; j++) {
    u32 a = page | (j & 0x7ff);
    if(a < rdram.size()) rdram[a] = pat[j & (unit - 1)];
  }
}

auto Memory::write8(u32 addr, u8 value) -> void {
  if(isvWrite(addr & 0x1fff'ffff, value, 1)) return;
  wrtag::mark(addr & 0x1fff'ffff, wrtag::kCpu, (u32)storePc);
  watchHit(addr & 0x1fff'ffff, 1, value, false);
  if(isCart(addr & 0x1fff'ffff)) { cartWrite(addr & 0x1fff'ffff, value, 1); return; }
  if(isSaveDomain(addr & 0x1fff'ffff) && saveType != SaveType::None) { saveWrite(addr & 0x1fff'ffff, value, 1); return; }
  u32 rem; u8* p = resolve(addr, rem);
  if(p && rem >= 1) { p[0] = value; return; }
  // MMIO byte writes are unusual; fold into the register word.
  u32 m = addr & 0x1fff'ffff;
  if(isMmio(m)) { u32 w = mmioRead32(m & ~3u); u32 sh = 8 * (3 - (m & 3)); w = (w & ~(0xffu << sh)) | ((u32)value << sh); mmioWrite32(m & ~3u, w); }
}
auto Memory::write16(u32 addr, u16 value) -> void {
  if(isvWrite(addr & 0x1fff'ffff, value, 2)) return;
  wrtag::markRange(addr & 0x1fff'ffff, 2, wrtag::kCpu, (u32)storePc);
  watchHit(addr & 0x1fff'ffff, 2, value, false);
  if(isCart(addr & 0x1fff'ffff)) { cartWrite(addr & 0x1fff'ffff, value, 2); return; }
  if(isSaveDomain(addr & 0x1fff'ffff) && saveType != SaveType::None) { saveWrite(addr & 0x1fff'ffff, value, 2); return; }
  u32 rem; u8* p = resolve(addr, rem);
  if(p && rem >= 2) { p[0] = (u8)(value >> 8); p[1] = (u8)value; return; }
  u32 m = addr & 0x1fff'ffff;
  if(isMmio(m)) { u32 w = mmioRead32(m & ~3u); u32 sh = 16 * (1 - ((m >> 1) & 1)); w = (w & ~(0xffffu << sh)) | ((u32)value << sh); mmioWrite32(m & ~3u, w); }
}
auto Memory::write32(u32 addr, u32 value) -> void {
  if(isvWrite(addr & 0x1fff'ffff, value, 4)) return;
  wrtag::markRange(addr & 0x1fff'ffff, 4, wrtag::kCpu, (u32)storePc);
  watchHit(addr & 0x1fff'ffff, 4, value, false);
  if(isCart(addr & 0x1fff'ffff)) { cartWrite(addr & 0x1fff'ffff, value, 4); return; }
  if(isSaveDomain(addr & 0x1fff'ffff) && saveType != SaveType::None) { saveWrite(addr & 0x1fff'ffff, value, 4); return; }
  u32 rem; u8* p = resolve(addr, rem);
  if(p && rem >= 4) {
    p[0] = (u8)(value >> 24); p[1] = (u8)(value >> 16);
    p[2] = (u8)(value >> 8);  p[3] = (u8)value;
    // SW de la CPU sobre la palabra del byte de control de la PIF RAM (SI WR4B): el PIF lo
    // atiende igual que tras un DMA. Es como libultra manda el 0x08 de fin de arranque.
    if((addr & 0x1fff'fffc) == BASE_PIFRAM + 0x3c) pifCommand();
    return;
  }
  u32 m = addr & 0x1fff'ffff;
  if(isMmio(m)) {
    if(trapSpRegStore && m >= 0x0404'0000 && m < 0x0404'0020) {
      std::fprintf(stderr, "[spReg] write vaddr=0x%08x phys=0x%08x val=0x%08x by pc=0x%08x\n",
                   addr, m, value, (u32)storePc);
      // A CPU store landing in the SP DMA/status registers from outside the RSP
      // driver is a stack overflow into MMIO — flag it so the CPU halts here.
      if(!pendingTrap && (u32)storePc >= 0x8000'5000 && (u32)storePc < 0x8000'8000) {
        pendingTrap = true;
        char b[96]; std::snprintf(b, sizeof b, "sp-reg store phys=0x%08x val=0x%08x pc=0x%08x", m, value, (u32)storePc);
        trapMsg = b;
      }
    }
    mmioWrite32(m & ~3u, value);
  }
}
auto Memory::write64(u32 addr, u64 value) -> void {
  write32(addr, (u32)(value >> 32));
  write32(addr + 4, (u32)value);
}

}  // namespace kestrel
