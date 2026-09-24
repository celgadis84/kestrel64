#include "archive.hpp"
#include "wrtag.hpp"
#include "../telemetry/hostprof.hpp"
#include "movie.hpp"
#include "memory.hpp"
#include "runtime.hpp"
#include <chrono>
#include "../audio/audio.hpp"
#include "../vrdp/vrdp.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <string>
#include <xmmintrin.h>   // _mm_pause: pista de espera activa
#include <string_view>
#ifdef _WIN32
#include <windows.h>

// Quien esta corriendo AHORA. Cada chip sella los eventos con SU propio reloj de invitado:
// el RSP escribe DPC_END desde su hilo y el instante correcto es el suyo, no el numero de
// instrucciones que la CPU lleve retiradas en ese momento de tiempo de pared.
static thread_local bool tlIsRspThread = false;
#endif
// Aplicando una entrada del diario DPC del RSP (ver Memory::dpLogApply): la escritura es del
// RSP y se sella con SU instante, aunque la ejecute otro hilo.
static thread_local bool tlDpLogApply = false;
static thread_local bool tlDpcKick = false;      // dpcAdvance lanzo un tramo (ver rspDpcWrite)
static thread_local kestrel::u64 tlDpLogAt = 0;
static thread_local bool tlInRetire = false, tlRetireArmed = false;   // ver rcpRetire

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
static auto selfThreadHandle() -> void* {
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
static auto pinToPhysicalCore(u32 slot) -> void {
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

// Physical base addresses of the N64 memory map.
enum : u32 {
  BASE_RDRAM  = 0x0000'0000,
  BASE_DMEM   = 0x0400'0000,
  BASE_IMEM   = 0x0400'1000,
  BASE_CART   = 0x1000'0000,  // PI domain 1 (cartridge ROM)
  BASE_PIFRAM = 0x1fc0'07c0,
  // RCP MMIO register blocks.
  BASE_RDRAMREG = 0x03f0'0000,
  BASE_SP     = 0x0404'0000,
  BASE_SP_PC  = 0x0408'0000,
  BASE_DPC    = 0x0410'0000,
  BASE_DPS    = 0x0420'0000,
  BASE_MI     = 0x0430'0000,
  BASE_VI     = 0x0440'0000,
  BASE_AI     = 0x0450'0000,
  BASE_PI     = 0x0460'0000,
  BASE_RI     = 0x0470'0000,
  BASE_SI     = 0x0480'0000,
};

// Definido mas abajo, junto al resto del Controller Pak; reset() lo necesita antes.
static auto mempakFormat(std::vector<u8>& p) -> void;

auto Memory::reset(bool expansionPak) -> void {
  rdram.assign(expansionPak ? RDRAM_SIZE_EXPANDED : 0x0040'0000, 0);
  rdramHidden.assign(rdram.size() >> 1, 0);
  wrtag::init((u32)rdram.size());
  dmem.assign(DMEM_SIZE, 0);
  imem.assign(IMEM_SIZE, 0);
  pifram.assign(PIFRAM_SIZE, 0);
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
  //   KESTREL_PADACC=1102 accesorio por puerto: 0 nada, 1 Controller Pak, 2 Rumble Pak
  // El Controller Pak es del MANDO, no de la cartuchera: existe aunque el juego no guarde.
  const char* pads = std::getenv("KESTREL_PADS");
  const char* accs = std::getenv("KESTREL_PADACC");
  const char* oldMempak = std::getenv("KESTREL_MEMPAK");   // compatibilidad: solo puerto 1
  for(int i = 0; i < 4; i++) {
    bool on = pads && pads[0] ? (pads[i] ? pads[i] != '0' : false) : (i == 0);
    u8   ac = accs && accs[i] ? (u8)(accs[i] - '0') : (u8)1;
    if(i == 0 && oldMempak && oldMempak[0] == '0') ac = 0;
    if(ac > 2) ac = 0;
    padPort[i].connected = on;
    padPort[i].accessory = ac;
    rt::padOn[i].store(on, std::memory_order_relaxed);
    rt::padAcc[i].store(ac, std::memory_order_relaxed);
    if(ac == 1 && padPort[i].mempak.empty()) mempakFormat(padPort[i].mempak);
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
  initMap();  // refresh so CART_ROM appears
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
auto Memory::piArm(u32 cartPhys, u32 len) -> void {
  static const bool instant = std::getenv("KESTREL_PIINSTANT") != nullptr;
  if(instant || !len) { rcp.pi_status = 0x8; raiseIntr(MI_PI); return; }
  piBusy   = true;
  piDoneAt = cartNow() + rcpCyclesToInsns(piXferCycles(cartPhys, len));
  rcp.pi_status = PI_DMA_BUSY;
  // Lo arranca un store que con el JIT puede ir en mitad de una cadena enlazada cuyo permiso
  // no conocia este plazo (mismo caso que siDma).
  if(jitGuardPtr) *jitGuardPtr = 0;
}

// Vencimiento del plazo del PI.
auto Memory::piFinish() -> void {
  if(!piBusy) return;
  piBusy = false;
  rcp.pi_status = 0x8;   // DMA done: interrupt pending (bit3), busy bits clear
  raiseIntr(MI_PI);
}

auto Memory::saveSize() const -> u32 {
  switch(saveType) {
    case SaveType::Eeprom4k:  return 512;      //  4 kbit
    case SaveType::Eeprom16k: return 2048;     // 16 kbit
    case SaveType::Sram256k:  return 32 * 1024;   // 256 kbit
    case SaveType::Sram768k:  return 96 * 1024;   // 768 kbit (three 32 KiB banks)
    case SaveType::Flash1m:   return 128 * 1024;  //  1 Mbit
    default:                  return 0;
  }
}

// Path of the ROM with its extension replaced. Only an extension belonging to the final
// path component is stripped, so a directory with a dot in it stays intact.
static auto withExt(const std::string& romPath0, const char* ext) -> std::string {
  // Una ROM comprimida guarda con el nombre de la ROM de dentro (ver stripContainerExt).
  const std::string romPath = archive::stripContainerExt(romPath0);
  usize dot = romPath.find_last_of('.');
  usize slash = romPath.find_last_of("/\\");
  if(dot == std::string::npos || (slash != std::string::npos && dot < slash))
    return romPath + ext;
  return romPath.substr(0, dot) + ext;
}

// Carga un respaldo de disco sobre una memoria YA dimensionada; tolera ficheros cortos o
// largos (se lee lo que quepa) y no toca nada si el fichero no existe.
static auto loadInto(const std::string& path, std::vector<u8>& back, const char* what) -> void {
  FILE* f = std::fopen(path.c_str(), "rb");
  if(!f) return;
  std::fseek(f, 0, SEEK_END);
  long n = std::ftell(f);
  std::fseek(f, 0, SEEK_SET);
  if(n > 0) {
    usize rd = (usize)n < back.size() ? (usize)n : back.size();
    std::fread(back.data(), 1, rd, f);
    std::printf("[%s] loaded %s (%zu bytes)\n", what, path.c_str(), rd);
  }
  std::fclose(f);
}

static auto storeFrom(const std::string& path, const std::vector<u8>& back) -> void {
  if(path.empty() || back.empty()) return;
  if(FILE* f = std::fopen(path.c_str(), "wb")) {
    std::fwrite(back.data(), 1, back.size(), f);
    std::fclose(f);
  }
}

// --- Controller Pak ----------------------------------------------------------
// CRC de direccion (5 bits) y de datos (8 bits) del protocolo joybus, portados de
// __osContAddressCrc / __osContDataCrc del SDK. El juego los comprueba en CADA bloque:
// si el CRC de datos no cuadra reintenta tres veces y luego pregunta el estado del pak,
// asi que devolverlos mal equivale a un pak roto.
static auto mempakAddrCrc(u16 blockAddr) -> u8 {
  u8 crc = 0;
  for(int i = 0; i < 16; i++) {
    u8 poly = (crc & 0x10) ? 0x15 : 0x00;
    crc = (u8)((crc << 1) | (u8)((blockAddr & 0x400) ? 1 : 0));
    blockAddr = (u16)(blockAddr << 1);
    crc ^= poly;
  }
  return crc & 0x1f;
}

// 33 vueltas, no 32: la ultima mete un byte de ceros por la derecha (el resto del CRC).
static auto mempakDataCrc(const u8* data) -> u8 {
  u8 crc = 0;
  for(int i = 0; i <= 32; i++) {
    for(int j = 7; j >= 0; j--) {
      u8 poly = (crc & 0x80) ? 0x85 : 0x00;
      crc = (u8)(crc << 1);
      if(i != 32) crc |= (u8)((data[i] & (1 << j)) ? 1 : 0);
      crc ^= poly;
    }
  }
  return crc;
}

// Un pak sale de fabrica FORMATEADO, y osPfsInitPak lo da por inservible si no lo esta
// (bloque de ID con suma mala -> __osCheckPackId -> PFS_ERR_ID_FATAL). Se construye aqui
// el mismo contenido que deja osPfsReFormat: bloque de ID por cuadruplicado en los bloques
// 1/3/4/6, tabla de inodos en el 8 con su copia en el 16, y directorio (bloque 24, 16
// bloques) a cero. Bloques de 32 bytes, todo big-endian.
static auto mempakFormat(std::vector<u8>& p) -> void {
  p.assign(32 * 1024, 0);
  auto be16 = [](u8* q, u16 v) { q[0] = (u8)(v >> 8); q[1] = (u8)v; };
  auto be32 = [](u8* q, u32 v) {
    q[0] = (u8)(v >> 24); q[1] = (u8)(v >> 16); q[2] = (u8)(v >> 8); q[3] = (u8)v;
  };

  u8 id[32] = {0};                       // __OSPackId
  be32(id + 0x00, 0);                    // repaired: nunca reparado
  be32(id + 0x04, 0x4b657374);           // random: sello del pak, cualquier valor sirve
  be16(id + 0x18, 1);                    // deviceid: __osGetId exige el bit 0 puesto
  id[0x1A] = 1;                          // banks: 32 KiB = un banco
  id[0x1B] = 0;                          // version
  u16 sum = 0, isum = 0;                 // __osIdCheckSum: 14 medias palabras
  for(int j = 0; j < 28; j += 2) {
    u16 d = (u16)((id[j] << 8) | id[j + 1]);
    sum = (u16)(sum + d); isum = (u16)(isum + (u16)~d);
  }
  be16(id + 0x1C, sum); be16(id + 0x1E, isum);
  for(int b : {1, 3, 4, 6}) std::memcpy(&p[(usize)b * 32], id, 32);

  u8 inode[256] = {0};                   // __OSInode: 128 entradas de media palabra
  const int startPage = 5;               // banks * 2 + 3
  for(int j = startPage; j < 128; j++) be16(inode + j * 2, 3);   // PFS_PAGE_NOT_USED
  u32 s = 0;                             // __osSumcalc sobre el resto de la tabla
  for(int j = startPage * 2; j < 256; j++) s = (s + inode[j]) & 0xffff;
  be16(inode + 0, (u16)s);
  std::memcpy(&p[8 * 32], inode, 256);   // inode_table
  std::memcpy(&p[16 * 32], inode, 256);  // minode_table (copia de respaldo)
}

// Mirror the battery/flash backing to a file beside the ROM. The extension follows the
// mupen/ares convention so saves are interchangeable: .eep (EEPROM), .sra (SRAM), .fla
// (FlashRAM), .mpk (Controller Pak). The path is the ROM path with its extension replaced.
auto Memory::attachSaveFile(const std::string& romPath) -> void {
  // Los Controller Pak van aparte: no dependen del tipo de save de la cartuchera, y cada
  // puerto lleva el suyo. El del puerto 1 conserva el nombre de siempre (.mpk) para no
  // invalidar los que ya existen; los demas anaden el numero de puerto.
  for(int i = 0; i < 4; i++) {
    PadPort& pp = padPort[i];
    // Los cuatro se preparan aunque ahora mismo no lleven pak: el accesorio se puede
    // cambiar en marcha desde el dialogo, y sin la ruta ya resuelta ese puerto escribiria
    // en un pak recien formateado y luego lo volcaria encima del fichero que ya existia.
    if(pp.mempak.empty()) mempakFormat(pp.mempak);
    pp.mempakPath = withExt(romPath, (i == 0 ? std::string(".mpk")
                                             : ".mpk" + std::to_string(i + 1)).c_str());
    loadInto(pp.mempakPath, pp.mempak, "mpk");
  }

  saveFilePath.clear();
  if(saveType == SaveType::None) return;
  saveFilePath = withExt(romPath, isEeprom() ? ".eep" : isFlash() ? ".fla" : ".sra");

  std::vector<u8>* back = isEeprom() ? &eeprom : &saveRam;
  if(back->size() < saveSize()) back->resize(saveSize(), isFlash() ? 0xFF : 0x00);
  loadInto(saveFilePath, *back, "save");
}

auto Memory::flushSaveFile() const -> void {
  for(const PadPort& pp : padPort)
    if(pp.mempakDirty && !pp.mempakPath.empty()) storeFrom(pp.mempakPath, pp.mempak);
  if(saveFilePath.empty() || saveType == SaveType::None || !saveDirty) return;
  storeFrom(saveFilePath, isEeprom() ? eeprom : saveRam);
}

// The backup save fitted to a cartridge (EEPROM 4k/16k, SRAM 256k/768k, FlashRAM) is
// not derivable from the ROM contents, so — like every N64 emulator — we resolve it
// from the game code in the header. The 4-byte code sits at 0x3B..0x3E; bytes
// 0x3C-0x3D are the two-character cart ID, 0x3E the region. This curated table lists
// the titles we are confident about; everything else keeps the EEPROM-16k default.
// KESTREL_SAVETYPE forces a type by name (eep4k/eep16k/sram256/sram768/flash/none),
// which is how any game can be brought up before it earns a table entry.
auto Memory::resolveSaveType() -> void {
  saveType = SaveType::Eeprom16k;                  // default; overridden below
  if(const char* env = std::getenv("KESTREL_SAVETYPE")) {
    std::string s = env;
    if(s == "none")          saveType = SaveType::None;
    else if(s == "eep4k")    saveType = SaveType::Eeprom4k;
    else if(s == "eep16k")   saveType = SaveType::Eeprom16k;
    else if(s == "sram256")  saveType = SaveType::Sram256k;
    else if(s == "sram768")  saveType = SaveType::Sram768k;
    else if(s == "flash")    saveType = SaveType::Flash1m;
  } else if(rom.size() > 0x3E) {
    char id0 = (char)rom[0x3C], id1 = (char)rom[0x3D];
    auto is = [&](const char* id){ return id[0] == id0 && id[1] == id1; };
    // Two-character cart IDs, grouped by save device.
    static const char* kEep4k[]  = { "SM","MK","WR","FZ","SF","KT","GE","PW",
                                      "WA","YS","DK","BC","BK","RC","PG","IC" };
    static const char* kEep16k[] = { "PD","MQ","B7","DO","IM","VL","YW","3D" };
    static const char* kSram[]   = { "ZL","AL","GC","BA","OB","MW","K4","PN",
                                      "WI","DA","P2","RE","RS","W2" };
    static const char* kFlash[]  = { "PF","ZS","DP","MM","SQ","CC","CW","AF",
                                      "PO","JF","N6","YK" };
    for(const char* id : kFlash)  if(is(id)) { saveType = SaveType::Flash1m;   goto sized; }
    for(const char* id : kSram)   if(is(id)) { saveType = SaveType::Sram256k;  goto sized; }
    for(const char* id : kEep16k) if(is(id)) { saveType = SaveType::Eeprom16k; goto sized; }
    for(const char* id : kEep4k)  if(is(id)) { saveType = SaveType::Eeprom4k;  goto sized; }
  }
sized:
  // Size the active backing, blank to the device's erased state, and release the other
  // so exactly one save device is present (reset() pre-sized EEPROM before the type was
  // known). Existing save contents of the right size are preserved.
  if(isEeprom()) {
    if(eeprom.size() != saveSize()) eeprom.assign(saveSize(), 0);
    saveRam.clear();
  } else if(saveType != SaveType::None) {
    if(saveRam.size() != saveSize()) saveRam.assign(saveSize(), isFlash() ? 0xFF : 0x00);
    eeprom.clear();
  } else {
    eeprom.clear(); saveRam.clear();
  }
}

// Map a PI domain-2 physical address to a linear backing offset. Identity for SRAM
// 256k and FlashRAM; the 768k SRAM presents three 32 KiB banks at 0x08000000,
// 0x08010000, 0x08020000, which we fold into a contiguous 96 KiB backing.
auto Memory::saveOffset(u32 phys) const -> u32 {
  u32 off = phys - 0x0800'0000;
  if(saveType == SaveType::Sram768k) {
    u32 bank = off >> 16;                 // 64 KiB apart in the address map
    return bank < 3 ? bank * (32 * 1024) + (off & 0x7fff) : 0xffff'ffff;
  }
  return off;
}

// FlashRAM reads return either the status/silicon-id doubleword (Status mode) or the
// array contents (Read mode). SRAM reads hit the backing directly. Big-endian.
auto Memory::saveRead(u32 phys, u32 nbytes) -> u32 {
  auto bePick = [&](const u8* p, u32 sz) -> u32 {
    u32 v = 0;
    for(u32 i = 0; i < nbytes; i++) v = (v << 8) | (i < sz ? p[i] : 0);
    return v;
  };
  if(isFlash()) {
    if(flashMode == FlashMode::Status) {              // status/id window (8 bytes, repeats)
      u8 st[8];
      for(int i = 0; i < 8; i++) st[i] = (u8)(flashStatus >> (56 - 8 * i));
      u32 o = (phys - 0x0800'0000) & 7;
      return bePick(st + o, 8 - o);
    }
    u32 off = phys - 0x0800'0000;                      // Read mode: array data
    return off < saveRam.size() ? bePick(saveRam.data() + off, (u32)saveRam.size() - off) : 0;
  }
  if(isSram()) {
    u32 off = saveOffset(phys);
    return off < saveRam.size() ? bePick(saveRam.data() + off, (u32)saveRam.size() - off) : 0;
  }
  return 0;
}

// A write to 0x08010000 is the FlashRAM command register; anything else writes SRAM
// (or the flash page buffer in Write mode). Big-endian byte placement.
auto Memory::saveWrite(u32 phys, u64 value, u32 nbytes) -> void {
  if(isFlash() && (phys & 0x1ffff) >= 0x10000) {       // command register at 0x08010000
    flashCommand((u32)value);
    return;
  }
  auto put = [&](u8* p, u32 sz, u32 off) {
    for(u32 i = 0; i < nbytes && off + i < sz; i++)
      p[off + i] = (u8)(value >> (8 * (nbytes - 1 - i)));
  };
  if(isFlash()) {                                       // Write mode: stage page buffer
    put(flashPageBuf, sizeof flashPageBuf, (phys - 0x0800'0000) & 0x7f);
    return;
  }
  if(isSram()) {
    u32 off = saveOffset(phys);
    if(off != 0xffff'ffffu) { put(saveRam.data(), (u32)saveRam.size(), off); saveDirty = true; }
  }
}

// FlashRAM command register (0x08010000). Drives the mode state machine; the high
// byte is the opcode and, for offset commands, the low 16 bits are a 128-byte page.
auto Memory::flashCommand(u32 cmd) -> void {
  switch(cmd >> 24) {
    case 0xE1: flashMode = FlashMode::Status; flashStatus = 0x1111'8001'00C2'001Eull; break;
    case 0xF0: flashMode = FlashMode::Read;   flashStatus = 0x1111'8004'00C2'001Eull; break;
    case 0x4B: flashErasePage = cmd & 0xffff; break;   // latch 128-byte erase page
    case 0x78: flashMode = FlashMode::Erase;  flashStatus = 0x1111'8008'00C2'001Eull; break;
    case 0xA5: flashWritePage = cmd & 0xffff; flashStatus = 0x1111'8004'00C2'001Eull;
               flashMode = FlashMode::Write; break;
    case 0xB4: flashMode = FlashMode::Write; break;
    case 0xD2:                                                              // execute / commit
      if(flashMode == FlashMode::Erase) {
        // Erase a 16 KiB sector (128 pages) starting at the latched page, to 0xFF.
        u32 base = (flashErasePage & ~0x7fu) * 128;
        for(u32 i = 0; i < 16 * 1024 && base + i < saveRam.size(); i++) saveRam[base + i] = 0xFF;
        saveDirty = true;
      } else if(flashMode == FlashMode::Write) {
        u32 base = flashWritePage * 128;
        for(u32 i = 0; i < 128 && base + i < saveRam.size(); i++) saveRam[base + i] = flashPageBuf[i];
        saveDirty = true;
      }
      break;
    default: break;
  }
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
auto Memory::piIoDecay() -> void {
  if(cartLatchValid && cartNow() >= cartLatchExpiry) {
    cartLatchValid = false;
    rcp.pi_status &= ~0x2u;
  }
}

// CPU read from cart space. If a write recently latched a value onto the PI bus
// (and it has not decayed), that value is returned and consumed. Otherwise a real
// ROM read, mangled by the 16-bit-wide PI bus: sub-word reads only reach the UPPER
// halfword of each 32-bit word (a LB/LH of the lower half aliases to the upper).
auto Memory::cartRead(u32 phys, u32 nbytes) -> u32 {
  u32 isvVal = 0;
  if(isvRead(phys, nbytes, isvVal)) return isvVal;
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
  (void)phys;
  u64 now = cartNow();
  if(cartLatchValid && now >= cartLatchExpiry) cartLatchValid = false;
  if(cartLatchValid) return;                // first-write-wins
  u32 v;
  switch(nbytes) {
    case 1:  v = ((u32)value & 0xff) << 24; break;
    case 2:  v = ((u32)value & 0xffff) << 16; break;
    case 8:  v = (u32)(value >> 32); break;
    default: v = (u32)value; break;
  }
  cartLatch = v;
  cartLatchValid = true;
  cartLatchExpiry = now + cartLatchTtl;
  rcp.pi_status |= 0x2u;                     // IOBUSY set while the bus write is pending
}

auto Memory::wordStoreQuirk(u32 phys, u64 reg, u32 width) -> bool {
  bool sp  = phys >= BASE_DMEM   && phys < BASE_SP;
  bool pif = phys >= BASE_PIFRAM && phys < BASE_PIFRAM + PIFRAM_SIZE;
  if(!sp && !pif) return false;
  if(width == 4) return false;                 // SW behaves normally on these devices
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

// --- RCP MMIO register file --------------------------------------------------
auto Memory::rcpReg32(u32 phys) -> u32 { return mmioRead32(phys); }
auto Memory::rcpRegWrite32(u32 phys, u32 value) -> void {
  // La CPU no puede tocar DPC por delante de escrituras del RSP ya fechadas en su pasado.
  // Lo mismo con SP: el diario lleva tambien las escrituras del RSP a SP_STATUS.
  if(!tlIsRspThread && !tlDpLogApply && (rcpPend.load(std::memory_order_relaxed) & 16u)
     && ((phys & 0x1ff0'0000u) == (BASE_DPC & 0x1ff0'0000u)
         || (phys & 0x1ff0'0000u) == (BASE_SP & 0x1ff0'0000u)))
    dpLogApply(cartNow());
  wrtag::markRange(phys, 4, wrtag::kCpu, (u32)storePc);
  watchHit(phys, 4, value, false);
  mmioWrite32(phys, value);
}

// El buffer de spans no es memoria plana: cada entrada ocupa cuatro ranuras de la ventana
// de 128 palabras, y solo las tres primeras tienen registro detras. La tercera guarda unicamente
// 8 bits (la cobertura del span) y la cuarta no existe. Cualquier programa que barra la ventana
// -- no solo un test -- ve ese patron, que es la forma real del RAM interno del RDP.
auto Memory::dpsSpanRead(u32 idx) const -> u32 {
  idx &= 0x7f;
  return (idx & 3) == 3 ? 0u : rcp.dps_span[idx];
}

auto Memory::dpsSpanWrite(u32 idx, u32 v) -> void {
  idx &= 0x7f;
  switch(idx & 3) {
  case 0: case 1: rcp.dps_span[idx] = v; break;
  case 2:         rcp.dps_span[idx] = v & 0xff; break;
  default:        break;   // sin registro fisico: la escritura se pierde
  }
}

auto Memory::mmioRead32(u32 a) -> u32 {
  u32 blk = a & 0x1ff0'0000;
  u32 off = a & 0x000f'ffff;
  switch(blk) {
  case BASE_MI & 0x1ff0'0000:
    switch(off & 0xff) {
    case 0x00: return rcp.mi_mode;
    case 0x04: return 0x0202'0102;   // MI_VERSION
    case 0x08: {
      static int vilog = std::getenv("KESTREL_VILOG") ? 1 : 0;
      if(vilog) { static u32 n=0; if((n++ & 0x1ff)==0)
        std::fprintf(stderr,"[vilog] READ MI_INTR=%02x mask=%02x pc=%08x\n",(u32)rcp.mi_intr,rcp.mi_mask,(u32)storePc); }
      return rcp.mi_intr;
    }
    case 0x0c: return rcp.mi_mask;
    }
    return 0;
  case BASE_SP & 0x1ff0'0000:
    if(!tlIsRspThread && !tlDpLogApply && (rcpPend.load(std::memory_order_relaxed) & 16u))
      dpLogApply(cartNow());
    if(a >= BASE_SP_PC) return (off & 0xff) == 0 ? rcp.sp_pc : 0;
    switch(off & 0xff) {
    case 0x00: return rcp.sp_mem_addr;
    case 0x04: return rcp.sp_dram_addr;
    case 0x08: return rcp.sp_rd_len;
    case 0x0c: return rcp.sp_wr_len;
    case 0x10: return rcp.sp_status.load(std::memory_order_acquire)
                    | (rcp.sp_intr_on_break.load(std::memory_order_acquire) ? 0x40u : 0u);  // bit6 = INTR_ON_BREAK
    case 0x14: return (rcp.sp_status.load(std::memory_order_relaxed) >> 2) & 1;   // SP_DMA_FULL
    case 0x18: return (rcp.sp_status.load(std::memory_order_relaxed) >> 2) & 1;   // SP_DMA_BUSY
    case 0x1c: return rcp.sp_semaphore.exchange(1, std::memory_order_acq_rel);  // leer devuelve y deja 1
    }
    return 0;
  case BASE_DPC & 0x1ff0'0000:
    if(!tlIsRspThread && !tlDpLogApply && (rcpPend.load(std::memory_order_relaxed) & 16u))
      dpLogApply(cartNow());
    {
    // La CPU con escrituras DPC del RSP aun en su futuro: la vista fechada (ver cpuDpcView).
    // El RSP sube dpcViewPend ANTES de tocar el registro vivo, asi que si sigue a 0 despues de
    // leerlo, lo leido es del pasado de la CPU.
    DpcView vw = dpcViewLive();
    if(!tlIsRspThread && (dpcViewPend.load(std::memory_order_acquire)
                          || (vw = dpcViewLive(), dpcViewPend.load(std::memory_order_acquire))))
      vw = cpuDpcView;
    switch(off & 0xff) {
    case 0x00: return vw.start;
    case 0x04: return vw.end;
    // Las dos se evaluan en el reloj de invitado del lector (ver dpcCurrentFor). En
    // Lockstep el trabajo se ejecuta dentro del propio DPC_END y dpBusyAt() ya vale false
    // antes de que nadie pueda leer: se lee idle, como en HW.
    case 0x08: return dpcCurrentFor(cartNow(), 0);
    case 0x0c:
      return (dpcStatusFor(cartNow(), 0) & ~(kDpcViewSt | kDpcRunSt)) | vw.st;
    // Performance counters, 24-bit each. The RDP accumulates them per rasterized
    // span (see SoftRdp::accountPixels); games time the RDP with these.
    case 0x10: return rcp.dpc_clock.load(std::memory_order_relaxed)    & 0xff'ffff;
    case 0x14: return rcp.dpc_bufbusy.load(std::memory_order_relaxed)  & 0xff'ffff;
    case 0x18: return rcp.dpc_pipebusy.load(std::memory_order_relaxed) & 0xff'ffff;
    case 0x1c: return rcp.dpc_tmem.load(std::memory_order_relaxed)     & 0xff'ffff;
    }
    }
    return 0;
  case BASE_DPS & 0x1ff0'0000:
    switch(off & 0xff) {
    case 0x00: return rcp.dps_tbist;
    case 0x04: return rcp.dps_test_mode;
    case 0x08: return rcp.dps_buftest_addr;
    case 0x0c: return dpsSpanRead(rcp.dps_buftest_addr);
    }
    return 0;
  case BASE_VI & 0x1ff0'0000:
    switch(off & 0xff) {
    case 0x00: return rcp.vi_ctrl;
    case 0x04: return rcp.vi_origin;
    case 0x08: return rcp.vi_width;
    case 0x0c: return rcp.vi_intr;
    case 0x10: {  // VI_V_CURRENT: continuous halfline scanout position.
      // On hardware this counter free-runs with the video clock — a program that
      // spins on `VI_CURRENT == N` (the standard vsync wait in bare-metal roms)
      // needs it to advance every few thousand CPU cycles. The interrupt cadence
      // (viTick, unchanged) is coarse-per-batch; the *polled* value must be fine, so
      // derive it from the retired-instruction clock at the SAME field length the tick
      // uses (viFieldInsns, set by System from Clocks::fieldInsns()). Con dos constantes
      // distintas el emulador corria dos relojes de video a la vez.
      u32 total = rcp.viHalflines();
      u64 cph = viFieldInsns / total;   // instrucciones retiradas por media-linea
      if(cph == 0) cph = 1;
      u64 cyc = cartNow();   // mismo reloj de invitado que viTick (incluye paradas de cache)
      return (u32)((cyc / cph) % total);
    }
    case 0x14: return rcp.vi_burst;
    case 0x18: return rcp.vi_vsync;
    case 0x1c: return rcp.vi_hsync;
    case 0x20: return rcp.vi_leap;
    case 0x24: return rcp.vi_hstart;
    case 0x28: return rcp.vi_vstart;
    case 0x2c: return rcp.vi_vburst;
    case 0x30: return rcp.vi_xscale;
    case 0x34: return rcp.vi_yscale;
    }
    return 0;
  case BASE_AI & 0x1ff0'0000:
    switch(off & 0xff) {
    case 0x04: return rcp.ai_play_remaining;   // bytes left in the playing buffer (drains)
    case 0x0c:                                 // AI_STATUS: FIFO_FULL(bit31) | DMA_BUSY(bit30)
      return (rcp.ai_fifo_count >= 2 ? 0x8000'0001u : 0u)
           | (rcp.ai_fifo_count >= 1 ? 0x4000'0000u : 0u);
    }
    return 0;
  case BASE_PI & 0x1ff0'0000:
    switch(off & 0xff) {
    case 0x00: return rcp.pi_dram_addr;
    case 0x04: return rcp.pi_cart_addr;
    case 0x08: return rcp.pi_rd_len;
    case 0x0c: return rcp.pi_wr_len;
    // Si el plazo ya vencio pero nadie lo ha rematado todavia (el invitado sondea
    // PI_STATUS en vez de dormirse en la interrupcion), rematarlo aqui: el aparato no
    // ensena DMA_BUSY un ciclo mas de lo que dura la transferencia.
    case 0x10: piIoDecay(); if(piBusy && cartNow() >= piDoneAt) piFinish(); return rcp.pi_status;
    default: if((off & 0xff) >= 0x14 && (off & 0xff) <= 0x30) return rcp.pi_bsd[((off & 0xff) - 0x14) / 4];
    }
    return 0;
  case BASE_RI & 0x1ff0'0000:
    switch(off & 0xff) {
    case 0x00: return rcp.ri_mode;
    case 0x04: return rcp.ri_config;
    case 0x0c: return rcp.ri_select;   // must be nonzero for RDRAM to be "ready"
    case 0x10: return rcp.ri_refresh;
    }
    return 0;
  case BASE_SI & 0x1ff0'0000:
    switch(off & 0xff) {
    case 0x00: return rcp.si_dram_addr;
    case 0x18: return rcp.si_status;
    }
    return 0;
  case BASE_RDRAMREG & 0x1ff0'0000:
    return 0;   // RDRAM config regs: benign
  }
  return 0;
}

auto Memory::mmioWrite32(u32 a, u32 v) -> void {
  u32 blk = a & 0x1ff0'0000;
  u32 off = a & 0x000f'ffff;
  switch(blk) {
  case BASE_MI & 0x1ff0'0000:
    switch(off & 0xff) {
    case 0x00:
      rcp.mi_mode = v & 0x7f;
      if(v & (1 << 7)) { rcp.mi_repeat_on = false;                            // clear init/repeat mode
                         if(cpuStGuard) *cpuStGuard &= (u8)~2u; }
      if(v & (1 << 8)) { rcp.mi_repeat_on = true; rcp.mi_repeat_len = (v & 0x7f) + 1;  // arm: span = length+1
                         if(cpuStGuard) *cpuStGuard |= 2u; }
      if(v & (1 << 11)) clearIntr(MI_DP);   // clear DP interrupt
      break;
    case 0x0c: {  // MI_MASK: (clear,set) pairs for the 6 interrupts
      for(u32 i = 0; i < 6; i++) {
        if(v & (1u << (2 * i)))     rcp.mi_mask &= ~(1u << i);
        if(v & (1u << (2 * i + 1))) rcp.mi_mask |=  (1u << i);
      }
      break;
    }
    }
    return;
  case BASE_SP & 0x1ff0'0000:
    if(watchAddr)
      std::fprintf(stderr, "[spMmio] a=0x%08x off=0x%02x val=0x%08x by pc=0x%08x\n",
                   a, off & 0xff, v, (u32)storePc);
    if(a >= BASE_SP_PC) { if((off & 0xff) == 0) rcp.sp_pc = v & 0xffc; return; }
    switch(off & 0xff) {
    case 0x00: rcp.sp_mem_addr = v & 0x1fff; break;
    case 0x04: rcp.sp_dram_addr = v & 0xffffff; break;
    case 0x08: rcp.sp_rd_len = v; spDma(/*toRam=*/false); break;   // RDRAM -> SP mem
    case 0x0c: rcp.sp_wr_len = v; spDma(/*toRam=*/true);  break;   // SP mem -> RDRAM
    case 0x10: {  // SP_STATUS write: paired clear/set control bits.
      // HALT as the CPU sees it *before* this write. A CLEAR_HALT only launches the
      // core when it was actually halted; arriving mid-task it is a no-op, because
      // the bit it clears is already clear. This is the launch condition — not any
      // emulator-side "is the worker thread busy" bookkeeping.
      bool wasHalted = (rcp.sp_status.load(std::memory_order_acquire) & 1u) != 0;
      // Threaded: un LANZAMIENTO (CLEAR_HALT suelto sobre un nucleo parado) tiene que caer
      // *despues* de que la tarea anterior publique su estado. El worker pone HALT|BROKE
      // desde dentro de rsp.run(); si la CPU limpiase HALT antes y esa tienda tardia cayese
      // detras, la CPU leeria un "tarea terminada" falso y sacaria el DMEM de salida antes
      // de que la tarea nueva lo escribiese. El RSP real ordena esto solo.
      //
      // La espera va atada a `wasHalted`, la MISMA condicion que el lanzamiento de abajo.
      // Con el nucleo ya corriendo, un CLEAR_HALT es un no-op en hardware: limpia un bit que
      // ya esta limpio, no lanza nada y no hay estado que ordenar. Esperar ahi era un invento
      // nuestro, y con libdragon sale carisimo: `rspq_flush_internal()` (src/rspq/rspq.c:1165)
      // escribe SET_SIG_MORE|CLEAR_HALT|CLEAR_BROKE — dos veces seguidas, a proposito — en
      // CADA vaciado de cola, con el microcodigo persistente corriendo. Como `rspq` solo hace
      // break cuando la cola se VACIA, cada uno de esos avisos clavaba a la CPU emulada hasta
      // que el RSP se comia toda la cola: 36% del tiempo de pared en junkrunner64, con el
      // worker del RSP ocupado solo un 30%. El propio libdragon dice ahi que un emulador
      // deberia "resincronizar" CPU y RSP en SP_STATUS, no bloquear hasta el BREAK.
      if(rcpMode == RcpMode::Threaded && wasHalted && (v & (1u << 0)) && !(v & (1u << 1)))
        rspAwaitIdle();
      // Each field has a (clear,set) bit pair. Writing BOTH bits of a pair at once is
      // a no-op for that field (hardware quirk); only a lone clear or lone set acts.
      auto pair = [&](u32 clrBit, u32 setBit, auto onClr, auto onSet){
        bool c = v & (1u << clrBit), s = v & (1u << setBit);
        if(c && !s) onClr(); else if(s && !c) onSet();
      };
      auto clr = [&](u32 m){ rcp.sp_status.fetch_and(~m, std::memory_order_acq_rel); };
      auto set = [&](u32 m){ rcp.sp_status.fetch_or(m, std::memory_order_acq_rel); };
      pair(0, 1, [&]{ clr(1u); }, [&]{ set(1u); });   // HALT
      if(v & (1 << 2)) clr(2u);                      // clear BROKE (no paired set bit)
      pair(3, 4, [&]{ clearIntr(MI_SP); }, [&]{ raiseIntr(MI_SP); });         // SP interrupt
      pair(7, 8, [&]{ rcp.sp_intr_on_break.store(false, std::memory_order_release); },
                 [&]{ rcp.sp_intr_on_break.store(true,  std::memory_order_release); }); // intr-on-break
      // SIGNAL bits: pairs at bits 9..24 map to SP_STATUS read bits 7..14 (SIG0..SIG7).
      // Microcode sets these at task end (SIG2 = task done) so the OS routes the SP
      // interrupt to OS_EVENT_SP (scheduler) rather than OS_EVENT_SP_BREAK.
      {
        // Threaded: la escritura de senales de la CPU se apunta con su instante (flanco n+1,
        // el mismo convenio que DPC_END) para que el RSP la vea cuando le toca y no cuando el
        // anfitrion la aplico. Una escritura del RSP es presente para el: las entradas
        // pendientes de la CPU dejan de poder deshacer esos bits. Ver Memory::spStatusForRsp.
        // Se apunta en los DOS modos: con grano > 1 Lockstep tiene que aplazarla igual que
        // Threaded o dejarian de coincidir.
        const bool track = true;
        std::unique_lock<std::mutex> lk(spSigMx, std::defer_lock);
        if(track) lk.lock();
        const u32 before = rcp.sp_status.load(std::memory_order_acquire);
        for(u32 i = 0; i < 8; i++)
          pair(9 + 2*i, 10 + 2*i, [&,i]{ clr(1u << (7+i)); }, [&,i]{ set(1u << (7+i)); });
        const u32 changed = (before ^ rcp.sp_status.load(std::memory_order_acquire)) & 0x7f80u;
        const bool fromRsp = tlIsRspThread || lockRspExec || tlDpLogApply;
        // CLEAR_HALT con el nucleo en marcha: no-op en hardware, pero con grano > 1 hay que
        // recordarlo por si el RSP hace BREAK antes de ver las senales que lo acompanan.
        const bool lateHalt = !fromRsp && !wasHalted && (v & 1u) && !(v & 2u);
        if(track && fromRsp && changed) {
          for(u32 k = 0; k < spSigCount; ++k) spSigRing[(spSigHead + k) % kSpSigN].mask &= ~changed;
        } else if(track && !fromRsp && (changed || lateHalt)) {
          if(spSigCount == kSpSigN) { spSigHead = (spSigHead + 1) % kSpSigN; --spSigCount; }
          const u64 q = spSigQuant(), raw = cartNow() + 1;
          spSigRing[(spSigHead + spSigCount) % kSpSigN] =
            {(raw + q - 1) & ~(q - 1), raw, changed, before & changed,
             (lateHalt ? 1u : 0u) | ((v & 4u) ? 2u : 0u)};
          ++spSigCount;
        }
      }
      // CPU releasing the RSP (clear HALT, not re-halting): run the microcode LLE.
      // The core executes to its BREAK, updating sp_status/sp_pc and raising the SP
      // interrupt itself. Guarded against reentrancy (microcode can poke SP_STATUS
      // via COP0). If already running, just clear the halt bit.
      if((v & (1 << 0)) && !(v & (1 << 1))) {
        clr(1u);                                     // clear HALT
        static const bool rspTrace = std::getenv("KESTREL_RSPTRACE") != nullptr;
        if(rspTrace) {
          static u32 kicks = 0;
          kicks++;
          auto d32 = [&](u32 o){ return (u32(dmem[o])<<24)|(u32(dmem[o+1])<<16)|(u32(dmem[o+2])<<8)|dmem[o+3]; };
          u32 ttype = d32(0xFC0), ucode = d32(0xFD0), udata = d32(0xFD8), dl = d32(0xFF0);
          if(kicks <= 20 || kicks % 100 == 0)
            std::fprintf(stderr, "[rsp] KICK #%u pc=0x%03x type=%u ucode=%08x udata=%08x dl=%08x by cpu=0x%08x\n",
                         kicks, rcp.sp_pc, ttype, ucode, udata, dl, (u32)storePc);
          if(kicks == 1 && ttype == 1) {
            auto dump = [&](u32 base, u32 n){
              std::fprintf(stderr, "[rsp] DL@%06x:\n", base);
              for(u32 i = 0; i < n; i += 8) {
                u32 hi = (u32(rdram[base+i])<<24)|(u32(rdram[base+i+1])<<16)|(u32(rdram[base+i+2])<<8)|rdram[base+i+3];
                u32 lo = (u32(rdram[base+i+4])<<24)|(u32(rdram[base+i+5])<<16)|(u32(rdram[base+i+6])<<8)|rdram[base+i+7];
                std::fprintf(stderr, "  %06x: %08x %08x  (op %02x)\n", base+i, hi, lo, hi>>24);
              }
            };
            dump(dl & 0x00ffffff, 48);
            dump(0x470f0, 96);
            dump(0x47170, 96);
            std::fflush(stderr);
          }
        }
        if(rcpMode == RcpMode::Threaded) {
          // Threaded: hand the task to the RSP worker (runs to BREAK, raises MI_SP).
          // The launch must never be dropped. The worker raises MI_SP from inside
          // rsp.run(), so the CPU can service that interrupt and write the next
          // CLEAR_HALT while the worker is still winding down from the previous task
          // (rspBusy still set). Gating the kick on rspBusy loses that whole task and
          // the game then waits forever on a BREAK that never comes — hardware has no
          // such window. rspSubmitKick() waits the wind-down out instead.
          // El arranque se LATCHEA aqui, en el hilo CPU, no dentro del worker: start()
          // toma sp_pc, limpia halt/broke y arma el presupuesto. El RSP real arranca en el
          // instante del CLEAR_HALT, asi que un SP_PC (o un DMA de DMEM) que la CPU escriba
          // despues NO puede afectar a la tarea ya lanzada. Dejandolo dentro del worker,
          // start() leia sp_pc cuando al planificador le venia bien — con la CPU rapida
          // (JIT) la escritura siguiente ganaba la carrera y el microcodigo arrancaba en
          // otra direccion: nunca llegaba al BREAK y el test giraba para siempre.
          static const int rspInline = std::getenv("KESTREL_RSPINLINE") ? 1 : 0;
          if(wasHalted) { spSigAtKick(); rsp.mem = this; rsp.start();
            if(rspInline) rsp.step(~0ull);   // diagnostico: RSP sincrono dentro del hilo CPU
            else          rspSubmitKick(); }
        } else {
          if(!rsp.running) { spSigAtKick(); rsp.mem = this; rsp.start(); spMarkKick(); }   // arm; System::run steps it interleaved
        }
      }
      // If both CLEAR_HALT (bit0) and SET_HALT (bit1) are written together the SET
      // wins: HALT stays set (already applied above) and the RSP does not launch.
      // A lone SET_HALT while the core is mid-task stops it (external halt).
      if((v & (1 << 1)) && !(v & (1 << 0))) { rsp.running = false; rsp.brake = false; }
      break;
    }
    case 0x1c: rcp.sp_semaphore.store(0, std::memory_order_release); break;   // escribir lo suelta
    }
    return;
  case BASE_DPC & 0x1ff0'0000: {
    // CPU con tarea de RSP en marcha: al buzon, visible en el borde de grano (ver dpcMbPost).
    if(!tlIsRspThread && !tlDpLogApply && !lockRspExec && dpcMbPost(a, v)) break;
    // Vistas del RSP aun sin aplicar (ver dpLogDueAt): antes de que esta escritura pise cpuDpcView.
    if(!tlIsRspThread && !tlDpLogApply && (rcpPend.load(std::memory_order_relaxed) & 16u))
      dpLogApply(cartNow());
    static const bool dpwr = std::getenv("KESTREL_DPSYNCLOG") != nullptr;
    if(dpwr) std::fprintf(stderr, "[dpwr] reg=%02x v=%08x st=%08x start=%06x cur=%06x end=%06x sub=%06x\n",
                          off & 0xff, v, rcp.dpc_status.load(), rcp.dpc_start, rcp.dpc_current.load(), rcp.dpc_end, rcp.dpc_submitted);
    switch(off & 0xff) {
    case 0x00:  // DPC_START write: latch start, arm START_VALID. Ignored while START_VALID
      // already set (a pending start hasn't been consumed by an END write yet).
      if(!(rcp.dpc_status & 0x400u)) { rcp.dpc_start = v & 0xfffff8; rcp.dpc_status |= 0x400u; }
      break;
    case 0x04:  // DPC_END write: kick the RDP. Rasterize the FIFO, then signal DP.
      rcp.dpc_end = v & 0xfffff8;
      // CURRENT reloads from START only when a fresh START is pending (START_VALID).
      // Otherwise the RDP continues from where CURRENT sits — the streaming case,
      // where many END bumps follow a single START.
      // OJO: CURRENT no se recarga aqui. DPC_CURRENT es el puntero de LECTURA del command
      // processor y su unico dueno es el consumidor (rdpRunJob). Si lo movemos desde el hilo
      // CPU mientras el worker sigue rasterizando el span anterior, el worker publica despues
      // su avance viejo (direcciones altas) sobre la recarga -> el microcodigo lee un CURRENT
      // adelantado, cree consumido lo que aun no lo esta y reescribe el FIFO por debajo del
      // RDP (F3DEX2 usa el buffer como FIFO circular y hace flow-control leyendo CURRENT).
      // La recarga viaja con el trabajo: rdpRunJob publica CURRENT = inicio del span al empezar.
      if(rcp.dpc_status & 0x400u) {
        // Contrapresion del FIFO. En HW el command processor tiene UN puntero de lectura:
        // instalar un buffer nuevo (START fresco + END) recarga CURRENT desde START, y lo
        // que quedara sin leer del span anterior deja de existir. El RDP consume el FIFO en
        // tiempo real, asi que cuando el juego instala el buffer del frame siguiente ya no
        // queda nada del anterior y la recarga no pierde trabajo.
        // Con el RDP en su propio hilo eso deja de ser automatico: el productor puede
        // adelantarse frames enteros y dejar cientos de spans encolados. Si entonces se
        // recarga START, el worker rasterizara DESPUES esos spans viejos y retirara sus
        // SYNC_FULL -> una interrupcion DP de mas. El kernel del juego la atiende sin tarea
        // viva (SM64: sCurrentDisplaySPTask == NULL -> deref de 0x40 -> TLBL -> el hilo de
        // interrupciones queda STOPPED con OS_FLAG_FAULT y el juego se cuelga).
        // La semantica fiel es esperar al RDP: el productor no puede instalar un buffer
        // nuevo mientras el anterior sigue en vuelo. Eso ademas mantiene DPC_CURRENT
        // honesto y acota la cola a un frame.
        // La recarga NO necesita vaciar la cola: viaja DENTRO del trabajo. El worker publica
        // DPC_CURRENT = inicio del span justo al empezar cada trabajo, asi que encolar el span
        // nuevo detras de los viejos los ejecuta en el mismo orden que el command processor y
        // retira exactamente los mismos SYNC_FULL: ni uno de mas. Y el flow-control del FIFO
        // sigue honesto, porque CURRENT solo avanza cuando el rasterizador avanza de verdad,
        // que es lo que lee F3DEX2 para no reescribir comandos sin consumir.
        // Drenar aqui serializaba RSP y RDP: medido con el perfilador de host, el hilo del RSP
        // pasaba el 44% de su tiempo dormido en este punto mientras el RDP vaciaba el frame
        // entero, y luego el RDP paraba mientras el RSP producia el siguiente.
        // ...pero ese razonamiento solo vale mientras el buffer sea el MISMO. Un START fresco
        // instala OTRO buffer de comandos, y ahi el juego deja de mirar DPC_CURRENT: da por
        // muerto lo anterior y reescribe la memoria. Con el RDP en su hilo puede quedar trabajo
        // del buffer viejo por delante, y el rasterizador acababa leyendo comandos que el
        // microcodigo ya habia reescrito, se desincronizaba a media instruccion y terminaba
        // pintando el framebuffer encima del codigo del guest (Perfect Dark: SET_COLOR_IMAGE con
        // direccion arbitraria, 97727 pixeles dentro de 0x1000-0x60000; ver docs/PD-DERAIL.md).
        // La cura NO es esperar aqui: el hardware nunca para a la CPU al escribir DPC_END, y
        // medido cuesta ~13-15 % de pared (SM64/200 intercambios: 3,57 -> 4,10 s en threaded-jit,
        // 3,56 -> 4,09 s en prdp-jit) porque serializa RSP y RDP, ademas de alejar la fidelidad
        // del oraculo lockstep (campos VI por intercambio 4,05 -> 3,31, oraculo 4,09).
        // La cura es que el consumidor lea una COPIA del tramo (rdpSnapshot), tomada en el kick,
        // que es un instante de lectura que el hardware tambien puede elegir. Aqui solo se cambia
        // de generacion para que la copia del buffer nuevo no pise la del viejo aun sin consumir.
        // KESTREL_RDPDRAIN=1 recupera el drenado, solo para bisecar.
        static const bool drainOnStart = std::getenv("KESTREL_RDPDRAIN") != nullptr;
        if(rcpMode == RcpMode::Threaded) {
          if(drainOnStart) rdpDrain();
          else if(rdpGenCount() > 2) {
            // Coger la generacion LIBRE mas baja distinta de la actual. Baja a proposito: en
            // regimen normal el worker va al dia y siempre queda libre la 0 o la 1, asi que
            // no se reservan buffers de mas; los de arriba solo se tocan cuando el
            // rasterizador se queda atras de verdad. Si no hubiera ninguna libre -- no visto
            // con ocho -- se drena, que es lento pero correcto: el hardware nunca lee
            // comandos reescritos.
            u8 nxt = 0xff;
            for(u8 g = 0; g < rdpGenCount(); g++)
              if(g != rdpGen && !rdpGenBusy[g].load(std::memory_order_acquire)) { nxt = g; break; }
            if(nxt == 0xff) { rdpGenClash.fetch_add(1, std::memory_order_relaxed); rdpDrain(); nxt = (u8)((rdpGen + 1) % rdpGenCount()); }
            rdpGen = nxt;
          }
          else {
            // Pasar a la siguiente generacion. Con dos alternas esto vuelve a la de hace un
            // START, y si el worker sigue teniendo trabajo vivo de ESA generacion la copia
            // que venga a continuacion le reescribe los comandos por debajo: la misma
            // corrupcion que la sombra evita entre generaciones contiguas, solo que una
            // vuelta mas alla. Pasa cuando el productor se adelanta dos buffers, o sea
            // cuando el anfitrion va rapido, que es exactamente la firma del bug #2 de
            // junkrunner64 (frenar el anfitrion como sea lo hace desaparecer). Se cuenta
            // siempre -- una lectura atomica por START fresco, nada -- y el numero de
            // generaciones es ajustable para poder medir el A/B.
            const u8 nxt = (u8)((rdpGen + 1) % rdpGenCount());
            if(rdpGenBusy[nxt].load(std::memory_order_acquire))
              rdpGenClash.fetch_add(1, std::memory_order_relaxed);
            rdpGen = nxt;
          }
        }
        rcp.dpc_submitted = rcp.dpc_start; rcp.dpc_status &= ~0x400u;
        ev("rdpst", rcp.dpc_start, rcp.dpc_end);
        // HW recarga CURRENT desde START en el propio kick y la CPU lo ve al momento
        // (systemtest "RDP STATUS: Flags during a run", "RDP START & END REG"), incluso
        // congelado, donde no se rasteriza nada. Publicarlo desde aqui solo es seguro si
        // el rasterizador esta parado; si sigue consumiendo un span anterior publicaria
        // un puntero de lectura ADELANTADO (y luego el worker lo pisa con su avance viejo),
        // que es justo lo que rompia el flow-control del FIFO de F3DEX2. En ese caso la
        // recarga viaja con el trabajo: rdpRunJob publica CURRENT = inicio del span.
        if(rcpMode != RcpMode::Threaded || !rdpBusy.load(std::memory_order_acquire))
          rcp.dpc_current.store(rcp.dpc_start, std::memory_order_release);
        // Congelado no se lanza ningun tramo, pero la recarga de CURRENT ya ha ocurrido: el
        // lector del horario de invitado (dpcCurrentFor) solo ve lo que hay en el anillo y
        // seguia devolviendo el cierre del ultimo tramo. DK64 congela el RDP tras MI_DP, el
        // microcodigo instala START/END con el RDP congelado y espera a ver CURRENT recargado:
        // con el valor viejo se quedaba sondeando para siempre y el juego colgado.
        if((rcp.dpc_status & (1u << 1)) && dpGuestOn()) dpScheduleReload(rcp.dpc_start);
      }
      dpcAdvance();
      // (frozen: no run, CURRENT stays where the START reload left it)
      // DP interrupt fires only when the RDP retires a SYNC_FULL (raised inside
      // rdpRunJob) — NOT on every DPC_END write. PD streams the FIFO with many
      // DPC_END bumps per frame; raising DP each time storms the CPU (~17 IRQs/frame)
      // and starves the game thread so it never advances past boot into rendering.
      break;
    case 0x0c: {  // DPC_STATUS write: clear/set flags
      if(v & (1 << 0)) rcp.dpc_status &= ~(1u << 0);  // clear xbus
      if(v & (1 << 1)) rcp.dpc_status |=  (1u << 0);
      // Descongelar reanuda el FIFO: lo que llego mientras estaba congelado sigue vivo.
      if(v & (1 << 2)) { rcp.dpc_status &= ~(1u << 1);
                        if(rcp.dpc_submitted != rcp.dpc_end) dpcAdvance(); }  // clear freeze
      if(v & (1 << 3)) rcp.dpc_status |=  (1u << 1);
      if(v & (1 << 4)) rcp.dpc_status &= ~(1u << 2);  // clear flush
      if(v & (1 << 5)) rcp.dpc_status |=  (1u << 2);
      // counter clears (bit 6 TMEM, 7 PIPE_BUSY, 8 CMD/BUF_BUSY, 9 CLOCK)
      if(v & (1 << 6)) rcp.dpc_tmem.store(0, std::memory_order_relaxed);
      if(v & (1 << 7)) rcp.dpc_pipebusy.store(0, std::memory_order_relaxed);
      if(v & (1 << 8)) rcp.dpc_bufbusy.store(0, std::memory_order_relaxed);
      if(v & (1 << 9)) rcp.dpc_clock.store(0, std::memory_order_relaxed);
      break;
    }
    }
    if(!tlIsRspThread) cpuDpcView = dpcViewLive();
    return; }
  case BASE_DPS & 0x1ff0'0000:
    switch(off & 0xff) {
    case 0x00: rcp.dps_tbist = v & 0x7ff; break;   // los 11 bits del BIST de TMEM
    case 0x04: rcp.dps_test_mode = v & 1; break;
    case 0x08: rcp.dps_buftest_addr = v & 0x7f; break;   // registro de direccion de 7 bits
    case 0x0c: dpsSpanWrite(rcp.dps_buftest_addr, v); break;
    }
    return;
  case BASE_VI & 0x1ff0'0000:
    // Mirror the VI programming to the GPU backend so its scanout matches. The N64 VI
    // register index (offset>>2, 0..13) maps 1:1 onto ::RDP::VIRegister. No-op when PRDP off.
    if(((off & 0xff) >> 2) < 14) vrdp::viWrite((off & 0xff) >> 2, v);
    switch(off & 0xff) {
    case 0x00: rcp.vi_ctrl = v; break;
    // Count buffer flips, not writes: a single-buffered ROM rewrites VI_ORIGIN with the
    // same address every field. The count is what tells an animating ROM (flips) apart
    // from one still drawing a single picture (never flips) — see KESTREL_MAXFLIPS.
    //
    // Se probo contar el intercambio en el LATCH del VI (comienzo de campo) en vez de en la
    // escritura, que es lo que hace el HW. Resultado medido: el volcado headless pasa a
    // ensenar el buffer VIEJO, que un juego de doble buffer esta reescribiendo ya — en
    // threaded eso depende del reloj de pared y los cinco modos dejaron de coincidir en
    // SM64. El punto de captura tiene que ser el buffer recien publicado, que nadie toca.
    case 0x04: { u32 nv = v & 0xffffff;
                 if(rcp.vi_origin && nv != rcp.vi_origin) {
                   rcp.viFlips++;
                   // KESTREL_FLIPLOG=1: cuantos campos de video ha costado cada cuadro. La media
                   // (campos por intercambio) esconde lo unico que importa en un juego que se
                   // autorregula: si la cadencia es estable (siempre 2) o resbala (2,3,2,4...).
                   // Un juego que cuenta cuadros para reproducir una demo se rompe con el
                   // resbalon, no con la media. Diagnostico puro: no toca estado del invitado.
                   static const int fliplog = std::getenv("KESTREL_FLIPLOG") ? 1 : 0;
                   if(fliplog) {
                     static u32 lastField = 0;
                     std::fprintf(stderr, "[flip] #%u campo=%u delta=%u\n",
                                  rcp.viFlips, rcp.viFields, rcp.viFields - lastField);
                     lastField = rcp.viFields;
                   }
                 }
                 rcp.vi_origin = nv; } break;
    case 0x08: rcp.vi_width = v & 0xfff; break;
    // Reprogramar la linea de coincidencia (o la altura del campo) mueve el plazo del VI:
    // el que estuviera armado se calculo con el valor viejo. Se invalida y se saca al JIT de
    // la cadena, igual que hace piArm, para que el siguiente prologo lo recalcule.
    case 0x0c: rcp.vi_intr = v & 0x3ff; viNextAt = viNextEvent(viLastRetired); evArm(); if(jitGuardPtr) *jitGuardPtr = 0; break;
    case 0x10: {
      static int vilog = std::getenv("KESTREL_VILOG") ? 1 : 0;
      if(vilog) { static u32 n=0; if((n++ & 0x3f)==0)
        std::fprintf(stderr,"[vilog] ACK VI_CURRENT write #%u intr=%02x pc=%08x\n",n,(u32)rcp.mi_intr,(u32)storePc); }
      clearIntr(MI_VI); break;   // VI_CURRENT write acks the VI interrupt
    }
    case 0x14: rcp.vi_burst = v; break;
    case 0x18: rcp.vi_vsync = v; viNextAt = viNextEvent(viLastRetired); evArm(); if(jitGuardPtr) *jitGuardPtr = 0; break;
    case 0x1c: rcp.vi_hsync = v; break;
    case 0x20: rcp.vi_leap = v; break;
    case 0x24: rcp.vi_hstart = v; break;
    case 0x28: rcp.vi_vstart = v; break;
    case 0x2c: rcp.vi_vburst = v; break;
    case 0x30: rcp.vi_xscale = v; break;
    case 0x34: rcp.vi_yscale = v; break;
    }
    return;
  case BASE_AI & 0x1ff0'0000:
    switch(off & 0xff) {
    case 0x00: rcp.ai_dram = v & 0xffffff; break;
    case 0x04: {  // AI_LEN write: enqueue a DMA buffer (the "go" for the audio FIFO)
      // El DAC se pone al dia ANTES de tocar la cola. aiAcc y aiLastRetired son un credito
      // fechado: armar el plazo del nuevo bufer sobre un aiLastRetired viejo lo adelantaba
      // tanto como llevara sin mirarse el AI, o sea un trozo de subtramo -- y eso hace que el
      // instante de MI_AI dependa de viTicksPerField, que es un ajuste de ANFITRION. Medido
      // en junkrunner64: la PRIMERA MI_AI caia 183 instrucciones antes con VITICKS=16 que
      // con 4, y a partir de ahi el invitado divergia.
      aiTick(cartNow());
      u32 len = v & 0x3ffff;
      rcp.ai_len = len;
      if(spTrace()) { static u32 n=0; if(n++<20) std::fprintf(stderr,"[ai] LEN write #%u len=%u fifo=%u dac=%u\n",n,len,rcp.ai_fifo_count,rcp.ai_dacrate); }
      if(len && rcp.ai_fifo_count < 2) {
        // El DAC acaba leyendo el buffer entero de RDRAM; se contabiliza al aceptarlo.
        ramBytesAi.fetch_add(len, std::memory_order_relaxed);
        rcp.ai_fifo_addr[rcp.ai_fifo_count] = rcp.ai_dram;
        rcp.ai_fifo_len[rcp.ai_fifo_count]  = len;
        if(rcp.ai_fifo_count == 0) rcp.ai_play_remaining = len;  // starts playing now
        rcp.ai_fifo_count++;
        // Un bufer nuevo en la cola pone plazo donde no lo habia (o lo adelanta): se rearma
        // y se saca al JIT de la cadena, igual que hacen piArm y la escritura de VI_INTR.
        aiArm();
        if(jitGuardPtr) *jitGuardPtr = 0;
        // Fan the accepted buffer out to the host speakers. Read-only on RDRAM, so
        // this cannot perturb determinism (systemtest / lockstep md5 unaffected).
        u32 rate = rcp.ai_dacrate ? (aiVidClock / (rcp.ai_dacrate + 1)) : 32'000u;
        dmaSettle(rcp.ai_dram, (u64)rcp.ai_dram + len);
        audio::pushRdram(rdram.data(), (u32)rdram.size(), rcp.ai_dram, len, rate);
      }
      break;
    }
    case 0x08: rcp.ai_ctrl = v & 1; break;
    case 0x0c: clearIntr(MI_AI); break;
    // La frecuencia del DAC cambia la pendiente del drenaje: el plazo armado se calculo con
    // la vieja y ya no vale.
    // El registro tiene CATORCE bits (n64brew, Audio Interface: DACRATE[13:0]); lo que se
    // escriba por encima el hardware no lo guarda. Guardarlo crudo no era solo inexacto: con
    // 0xffffffff dentro, el `dacrate + 1` de `aiNextEvent` daba la vuelta a 0 en aritmetica de
    // 32 bits y el anfitrion se moria de division entera por cero. Una ROM que escriba basura
    // en los registros -- junkrunner64 lo hace a proposito -- tumbaba el emulador entero.
    case 0x10: aiTick(cartNow()); rcp.ai_dacrate = v & 0x3fff; aiArm(); if(jitGuardPtr) *jitGuardPtr = 0; break;
    case 0x14: rcp.ai_bitrate = v & 0xf; break;                 // BITRATE[3:0], igual
    }
    return;
  case BASE_PI & 0x1ff0'0000:
    switch(off & 0xff) {
    case 0x00: rcp.pi_dram_addr = v & 0x00ff'fffe; break;   // PI masks bit0 and the top byte
    case 0x04: rcp.pi_cart_addr = v & 0xffff'fffe; break;   // PI masks bit0
    case 0x08: rcp.pi_rd_len = v & 0xffffff; piDma(/*toCart=*/true);  break;  // RDRAM -> cart
    case 0x0c: rcp.pi_wr_len = v & 0xffffff; piDma(/*toCart=*/false); break;  // cart -> RDRAM
    case 0x10:
      // Bit 0 = reinicio del controlador: aborta la transferencia en vuelo. Bit 1 = borrar la
      // interrupcion. Antes se ponia el registro entero a cero, que con el plazo del PI ya no
      // vale: el manejador del invitado escribe bit 1 para reconocer MI_PI y eso no puede
      // apagar DMA_BUSY de una transferencia que todavia esta corriendo.
      if(v & (1 << 0)) { piBusy = false; rcp.pi_status = 0; }
      if(v & (1 << 1)) { clearIntr(MI_PI); rcp.pi_status &= ~0x8u; }
      break;
    default: if((off & 0xff) >= 0x14 && (off & 0xff) <= 0x30) rcp.pi_bsd[((off & 0xff) - 0x14) / 4] = v;
    }
    return;
  case BASE_RI & 0x1ff0'0000:
    switch(off & 0xff) {
    case 0x00: rcp.ri_mode = v; break;
    case 0x04: rcp.ri_config = v; break;
    case 0x0c: rcp.ri_select = v; break;
    case 0x10: rcp.ri_refresh = v; break;
    }
    return;
  case BASE_SI & 0x1ff0'0000:
    switch(off & 0xff) {
    case 0x00: rcp.si_dram_addr = v & 0xffffff; break;
    case 0x04: siDma(/*toPif=*/false); break;   // PIF RAM -> RDRAM (read 64B)
    case 0x10: siDma(/*toPif=*/true);  break;   // RDRAM -> PIF RAM (write 64B)
    // Escribir SI_STATUS solo BORRA LA INTERRUPCION: el bit de ocupado lo lleva el
    // hardware, no el invitado. Ponerlo a cero a pelo dejaba ver el SI libre con la
    // transaccion todavia en vuelo (el manejador del SDK escribe aqui al atender MI_SI).
    case 0x18: clearIntr(MI_SI); rcp.si_status = siBusy ? SI_DMA_BUSY : 0; break;
    }
    return;
  }
}

// --- DMA engines -------------------------------------------------------------
// "Los vectores de excepcion (0x0-0x400) no son destino legitimo de ningun DMA" es un
// invariante de libultra, NO del hardware: ahi el kernel del juego pone sus manejadores y
// pisarlos es un fallo real. El N64 no protege esa zona de ninguna manera, y un SDK que no
// sea libultra puede usarla legitimamente -- libdragon mete estructuras suyas en RDRAM baja
// y dispara el aviso decenas de veces por campo (dram=0x0001a0/0x0001a8 en junkrunner64).
// Por eso el aviso pasa a ser opt-in (KESTREL_DMAWARN=1) y ademas se corta a 16 lineas: es
// diagnostico para depurar un kernel libultra, no una condicion de error del emulador.
static auto dmaVectorWarn() -> bool {
  static const bool on = std::getenv("KESTREL_DMAWARN") != nullptr;
  if(!on) return false;
  static unsigned n = 0;
  return ++n <= 16;
}

auto Memory::piDma(bool toCart) -> void {
  // El DMA del cartucho lee o escribe RDRAM directamente: diario de DMA del RSP asentado antes.
  // Y si ESCRIBE en RDRAM, es una escritura del hilo de CPU que el RSP puede leer: sin barrera
  // el RSP ve el resultado de una transferencia que en la maquina aun no habria acabado, o que
  // acaba en un instante distinto cada corrida. El cartucho es la fuente de datos NUEVOS
  // (microcodigo, listas, texturas) que luego lee el RSP, asi que este es justo el sitio por
  // donde se cuela un adelanto no acotado.
  if(!toCart && wrBarrierOn() && dmaBarrierOn()) cpuRamWrBarrier(cartNow());
  {
    const u32 d = rcp.pi_dram_addr & 0xff'ffff;
    const u32 n = (rcp.pi_wr_len > rcp.pi_rd_len ? rcp.pi_wr_len : rcp.pi_rd_len) + 1;
    dmaSettle(d, (u64)d + n);
  }
  // Arrancar una DMA con otra en vuelo no le pasa a un juego sano (el SDK espera a la
  // interrupcion), pero si ocurre se cierra la anterior en vez de dejar un plazo huerfano.
  if(piBusy) piFinish();
  // SRAM/FlashRAM save DMA (PI domain 2, 0x08000000). These devices are byte-wide and
  // linear — no ROM 16-bit-bus mux — so a plain byte copy through saveRead/saveWrite is
  // both simpler and correct. This is the path libultra's osPiStartDma uses to persist
  // and reload the save (osEepromWrite is the joybus path handled elsewhere).
  {
    u32 cartPhys = rcp.pi_cart_addr & 0x1fff'ffff;
    if(saveType != SaveType::None && isSaveDomain(cartPhys)) {
      u32 rawLen = toCart ? rcp.pi_rd_len : rcp.pi_wr_len;
      u32 len = (rawLen & 0x00ff'ffff) + 1;
      u32 dram = rcp.pi_dram_addr & 0x00ff'ffff;
      u32 cart = cartPhys;
      for(u32 i = 0; i < len; i++) {
        if(toCart) {                                    // RDRAM -> save (write/erase buffer)
          u8 b = dram < rdram.size() ? rdram[dram] : 0;
          saveWrite(cart, b, 1);
        } else {                                        // save -> RDRAM (load)
          wrtag::mark(dram, wrtag::kPiDma, 0);
          if(watchAddr) watchHit(dram, 1, 0, true);
          if(dram < rdram.size()) rdram[dram] = (u8)saveRead(cart, 1);
        }
        dram++; cart++;
      }
      ramBytesPi.fetch_add(len, std::memory_order_relaxed);
      rcp.pi_cart_addr = (cart) & 0xffff'fffe;
      rcp.pi_dram_addr = ((dram + 1) & ~1u) & 0x00ff'fffe;
      piArm(cartPhys, len);
      return;
    }
  }
  if(toCart) {
    // RDRAM -> ROM cart: ROM is read-only so nothing lands. The address registers still
    // advance by the (even-rounded) length, matching HW.
    u32 len = ((rcp.pi_rd_len & 0x00ff'ffff) + 1 + 1) & ~1u;
    // En la consola el motor LEE la RDRAM aunque el destino sea ROM y no se quede nada:
    // el bus se ocupa igual, asi que el medidor lo cuenta.
    ramBytesPi.fetch_add(len, std::memory_order_relaxed);
    const u32 cartStart = rcp.pi_cart_addr & 0x1fff'fffe;
    rcp.pi_cart_addr = (rcp.pi_cart_addr + len) & 0xffff'fffe;
    rcp.pi_dram_addr = (rcp.pi_dram_addr + len) & 0x00ff'fffe;
    piArm(cartStart, len);
    return;
  }
  // cart -> RDRAM (PI WR_LEN): genuine PI DMA transfer engine. The copy is split into
  // blocks bounded by both a 128-byte maximum and the 2 KiB (0x800) RDRAM row edge.
  // The very first block, when the RDRAM start is not 8-aligned, drops `misalign` bytes
  // off the front and (for short transfers) writes the remainder byte-by-byte instead of
  // in halfword pairs; every block then rounds the RDRAM pointer up to the next 8-byte
  // boundary. This is the observable HW quirk n64-systemtest exercises. (Ported from the
  // documented block algorithm, cf. ares n64/pi/dma.cpp.)
  u8 blk[128];
  s32 length = (s32)((rcp.pi_wr_len & 0x00ff'ffff) + 1);
  const u32 cartStart = rcp.pi_cart_addr & 0x1fff'fffe;
  const s32 xferLen = length;
  ramBytesPi.fetch_add((u64)length, std::memory_order_relaxed);
  s32 maxBlock = 128;
  bool firstBlock = true;
  u32 cart = rcp.pi_cart_addr & 0x1fff'fffe;
  u32 dram = rcp.pi_dram_addr & 0x00ff'ffff;
  if(dram < 0x400u && dmaVectorWarn())
    std::fprintf(stderr, "[dma!] PI->RDRAM sobre vectores: dram=0x%06x len=%d\n", dram, (int)length);
  while(length > 0) {
    s32 misalign = (s32)(dram & 7);
    s32 distEndOfRow = 0x800 - (s32)(dram & 0x7ff);
    s32 blockLen = std::min(maxBlock - misalign, distEndOfRow);
    s32 curLen = std::min(length, blockLen);
    for(s32 i = 0; i < curLen; i += 2) {
      u32 romOff = (cart >= BASE_CART) ? cart - BASE_CART : 0xffff'ffffu;
      u16 data = 0;
      if(romOff != 0xffff'ffffu) {
        u8 hi = (romOff     < rom.size()) ? rom[romOff]     : 0;
        u8 lo = (romOff + 1 < rom.size()) ? rom[romOff + 1] : 0;
        data = (u16)((hi << 8) | lo);
      }
      blk[i + 0] = (u8)(data >> 8);
      blk[i + 1] = (u8)(data >> 0);
      cart += 2;
      length -= 2;
    }
    wrtag::markRange(dram, (u32)std::max(0, curLen - misalign), wrtag::kPiDma, 0);
    if(watchAddr) watchHit(dram, (u32)std::max(0, curLen - misalign), 0, true);
    if(firstBlock && curLen < 127 - misalign) {
      for(s32 i = 0; i < curLen - misalign; i++) {
        if(dram < rdram.size()) rdram[dram] = blk[i];
        dram++;
      }
    } else {
      for(s32 i = 0; i < curLen - misalign; i += 2) {
        if(dram < rdram.size()) rdram[dram] = blk[i + 0]; dram++;
        if(dram < rdram.size()) rdram[dram] = blk[i + 1]; dram++;
      }
    }
    dram = (dram + 7) & ~7u;
    firstBlock = false;
    maxBlock = distEndOfRow < 8 ? 128 - misalign : 128;
  }
  rcp.pi_cart_addr = cart & 0xffff'fffe;
  rcp.pi_dram_addr = dram & 0x00ff'fffe;
  piArm(cartStart, (u32)xferLen);
}

// Copia corta sin salir a la CRT. El motor DMA del SP mueve tramos pequenos MUY a menudo
// (F3DEX2 trae matrices, vertices y trozos de display list de 8 a 128 bytes por rafaga), y
// el memcpy de ucrtbase resuelve su despacho por tamano en cada llamada: para 32 bytes eso
// cuesta mas que la copia. El perfilador de host lo veia como ~20% del hilo del RSP dentro
// de ucrtbase, con spDma como llamante. Para tramos largos se sigue delegando en memcpy,
// que ahi si gana con sus rutinas anchas.
static inline auto dmaCopy(u8* dst, const u8* src, u32 n) -> void {
  if(n >= 256) { std::memcpy(dst, src, n); return; }
  u32 i = 0;
  for(; i + 16 <= n; i += 16) {
    u64 a, b;
    std::memcpy(&a, src + i, 8); std::memcpy(&b, src + i + 8, 8);
    std::memcpy(dst + i, &a, 8); std::memcpy(dst + i + 8, &b, 8);
  }
  for(; i + 8 <= n; i += 8) { u64 a; std::memcpy(&a, src + i, 8); std::memcpy(dst + i, &a, 8); }
  for(; i < n; i++) dst[i] = src[i];
}

auto Memory::spDma(bool toRam) -> void {
  // SP_RD/WR_LEN: bits 0-11 length-1, 12-19 count-1, 20-31 skip. Model row/count.
  u32 len = toRam ? rcp.sp_wr_len : rcp.sp_rd_len;
  // Byte count: (field+1) rounded up to the next multiple of 8. The low 3 bits of
  // both the SP address and the RDRAM address are ignored (8-byte aligned engine).
  u32 length = ((len & 0xfff) + 8) & ~7u;
  u32 count  = ((len >> 12) & 0xff) + 1;
  u32 skip   = (len >> 20) & 0xfff;
  // Ocupacion del bus: el salto entre filas no se transfiere, solo se saltan direcciones.
  const int slot = tlIsRspThread ? 1 : 0;
  ramBytesRsp.add(slot, (u64)length * count);
  const u64 nb = (u64)length * count;
  (toRam ? spDmaWrCnt : spDmaRdCnt).bump(slot);
  (toRam ? spDmaWrBytes : spDmaRdBytes).add(slot, nb);
  (toRam ? spDmaWrHist : spDmaRdHist)[spDmaBucket(nb)].bump(slot);
  if(rcp.sp_mem_addr & 0x1000) { spDmaImemCnt.bump(slot); spDmaImemBytes.add(slot, nb); }
  u32 memAddr  = rcp.sp_mem_addr & 0x1fff;
  bool imem    = (memAddr & 0x1000) != 0;
  u32 memOff   = memAddr & 0xff8;
  const u32 memOff0 = memOff;   // el bucle de abajo lo avanza; el dynarec necesita el inicial
  u32 dramAddr = rcp.sp_dram_addr & 0xfffff8;
  if(rcpMode == RcpMode::Threaded)                     // solo hace algo en el hilo del RSP
    rspDmaRdpWait(dramAddr, dramAddr + (length + skip) * count);
  if(toRam && dramAddr < 0x400u && dmaVectorWarn())
    std::fprintf(stderr, "[dma!] SP->RDRAM sobre vectores: dram=0x%06x len=%u\n", dramAddr, length);

  std::vector<u8>& sp = imem ? this->imem : this->dmem;
  // Traza de transferencias del motor del SP: sin ella un microcodigo que gira sobre IMEM
  // a ceros no dice si el fallo es del RSP o de que nadie le cargo el codigo.
  if(spTrace()) {
    static u32 n = 0; n++;
    if(n <= 40) std::fprintf(stderr, "[spdma] #%u %s %s dram=0x%06x memOff=0x%03x len=%u count=%u skip=%u by pc=0x%08x\n",
                             n, imem ? "IMEM" : "DMEM", toRam ? "->RDRAM" : "<-RDRAM",
                             dramAddr, memOff, length, count, skip, (u32)storePc);
  }
  if(watchAddr && toRam)
    std::fprintf(stderr, "[spDma] %s->RDRAM dram=0x%06x memOff=0x%03x len=%u count=%u skip=%u by pc=0x%08x\n",
                 imem ? "IMEM" : "DMEM", dramAddr, memOff, length, count, skip, (u32)storePc);
  // El microcodigo mueve kilobytes por tarea con este motor, asi que el bucle byte a byte
  // salia caro dentro del hilo del RSP. Cuando no hay vigilancia de memoria puesta se copia
  // por tramos contiguos: la unica discontinuidad real es la vuelta de la memoria del SP a
  // los 4 KB (el direccionamiento del motor es de 12 bits) y el final de la RDRAM. El
  // resultado byte a byte es identico; lo que cambia es el numero de instrucciones que le
  // cuesta al anfitrion.
  // GUARDIAN DE RANGO (KESTREL_DMAGUARD=<lo>:<hi>, opt-in). El motor de DMA del SP es el
  // unico camino por el que el microcodigo escribe RDRAM, asi que si aparece contenido del
  // RSP encima del codigo o de las estructuras del kernel, ha pasado por aqui. A diferencia
  // de `watchAddr` esto NO cambia la copia a byte a byte: es una comprobacion de solape por
  // transferencia, con coste nulo cuando esta apagado, para no mover el timing que provoca
  // el fallo.
  if(toRam) {
    static const u32 gLo = []{ const char* v = std::getenv("KESTREL_DMAGUARD");
      return v ? (u32)std::strtoul(v, nullptr, 0) : 0u; }();
    static const u32 gHi = []{ const char* v = std::getenv("KESTREL_DMAGUARD");
      const char* c = v ? std::strchr(v, ':') : nullptr;
      return c ? (u32)std::strtoul(c + 1, nullptr, 0) : 0u; }();
    if(gHi > gLo) {
      u32 lo = dramAddr, hi = dramAddr + count * (length + skip);
      if(hi > gLo && lo < gHi) {
        static u64 nHit = 0;
        if(nHit < 24) {
          nHit++;
          std::fprintf(stderr, "[dmaguard] #%llu %s->RDRAM dram=0x%06x len=%u count=%u skip=%u memOff=0x%03x pc=0x%08x\n",
                       (unsigned long long)nHit, imem ? "IMEM" : "DMEM", dramAddr, length, count, skip,
                       memOff, (u32)storePc);
          std::fflush(stderr);
        }
      }
    }
  }
  // RDRAM NO POBLADA: la ventana de direcciones de la RDRAM es mucho mayor que los chips
  // instalados (4 u 8 MB). Leer donde no responde ningun chip devuelve CEROS, y escribir
  // ahi se pierde; no es un "hasta aqui" que aborte la transferencia. La IPL3 de libdragon
  // depende de esto de forma explicita para poner IMEM a cero antes de usarlo como fuente
  // del bzero por DMA (boot/ipl3.c, rsp_bzero_init): "we run a DMA from RDRAM address
  // > 8MiB where many areas return 0 on read", con SP_DRAM_ADDR = 8 MB + 0x2000. Cortar la
  // copia dejaba IMEM con la basura anterior y esa basura acababa rellenando la RDRAM
  // entera -- punteros podridos y vectores de excepcion machacados.
  const bool byByte = watchAddr != 0;
  for(u32 c = 0; c < count; c++) {
    if(byByte) {
      for(u32 i = 0; i < length; i++) {
        u32 mo = (memOff + i) & 0xfff;
        u32 d  = dramAddr + i;
        bool have = d < rdram.size();
        if(toRam) { if(!have) continue;
                    wrtag::mark(d, wrtag::kSpDma, 0); watchHit(d, 1, sp[mo], true); rdram[d] = sp[mo]; }
        else      sp[mo] = have ? rdram[d] : 0;
      }
    } else {
      for(u32 i = 0; i < length; ) {
        u32 mo = (memOff + i) & 0xfff;
        u32 d  = dramAddr + i;
        u32 n = length - i;
        if(n > 0x1000u - mo) n = 0x1000u - mo;                            // vuelta de la SP mem
        if(d < rdram.size()) {
          if(n > (u32)(rdram.size() - d)) n = (u32)(rdram.size() - d);    // final de los chips
          if(toRam) dmaCopy(&rdram[d], &sp[mo], n);
          else      dmaCopy(&sp[mo], &rdram[d], n);
        } else if(!toRam) {
          std::memset(&sp[mo], 0, n);        // sin chip que responda: se leen ceros
        }
        i += n;
      }
    }
    memOff  = (memOff + length) & 0xfff;
    dramAddr += length + skip;
  }
  // Si el DMA acaba de reescribir IMEM, el microcodigo cambio bajo cualquier bloque que el
  // dynarec del RSP tuviera compilado (esto es como se cargan los overlays de F3DEX2). Tirar
  // la tabla aqui es la unica invalidacion que necesita: el resto de escritores de IMEM los
  // caza la huella que Rsp::start comprueba al arrancar cada tarea.
  if(imem && !toRam) rsp.jitInvalidate(memOff0, count * length, sp.data());
  rcp.sp_mem_addr  = (imem ? 0x1000 : 0) | memOff;
  rcp.sp_dram_addr = dramAddr & 0xffffff;
  // After any SP DMA completes the length register counts down to a fixed 0xFF8
  // readback (LENGTH=0xFF8, COUNT/SKIP drained). Both RD_LEN and WR_LEN share it.
  rcp.sp_rd_len = rcp.sp_wr_len = 0xff8;
}

// DMA SP -> RDRAM apuntado en el diario (ver DpLogDma en memory.hpp). Solo hilo del RSP, con la
// barrera del SP armada y el reloj exacto ya publicado por el llamador (Rsp::mtc0). Devuelve false
// y deja el camino de siempre (cita + copia en el acto) cuando hay diagnostico puesto, cuando la
// transferencia es demasiado grande para el anillo o cuando cae encima de lo que el RDP puede
// estar pintando.
auto Memory::spDmaLogPush(u64 at, u32 len) -> bool {
  static const bool diag = std::getenv("KESTREL_DMAGUARD") != nullptr;
  if(diag || watchAddr || spTrace()) return false;
  const u32 length = ((len & 0xfff) + 8) & ~7u;
  const u32 count  = ((len >> 12) & 0xff) + 1;
  const u32 skip   = (len >> 20) & 0xfff;
  const u64 total  = (u64)length * count;
  if(total > kDmaPayN / 4) return false;
  const u32 memAddr = rcp.sp_mem_addr & 0x1fff;
  const bool imem   = (memAddr & 0x1000) != 0;
  u32 memOff        = memAddr & 0xff8;
  const u32 dram    = rcp.sp_dram_addr & 0xfffff8;
  if(rcpPend.load(std::memory_order_acquire) & 4u) {
    // Mismo criterio de solape que rspDmaRdpWait, pero sin esperar: si pisa una imagen del RDP
    // en vuelo, camino de siempre.
    const u32 lo = dram, hi = dram + (length + skip) * count;
    for(;;) {
      const u32 s0 = dpWrSeq.load(std::memory_order_acquire);
      if(s0 & 1u) { std::this_thread::yield(); continue; }
      bool hit = false;
      for(u32 i = 0; i < SoftRdp::kWrSlots; i++)
        hit |= lo < dpWrHi[i].load(std::memory_order_acquire) && dpWrLo[i].load(std::memory_order_acquire) < hi;
      if(dpWrSeq.load(std::memory_order_acquire) != s0) continue;
      if(hit) return false;
      break;
    }
  }
  // Anillo de bytes o diario llenos: que la CPU aplique lo pendiente (el reloj ya esta
  // publicado). Hay que hacerlo ANTES de rellenar dpLogDma: con el diario lleno la ranura de la
  // cola es la de la cabeza, aun sin aplicar.
  if(dmaPayTail + total - dmaPayHead.load(std::memory_order_acquire) > kDmaPayN
     || dpLogTail.load(std::memory_order_relaxed) - dpLogHead.load(std::memory_order_acquire) >= kDpLogN)
    dpLogWait(at, false);
  const int slot = tlIsRspThread ? 1 : 0;
  ramBytesRsp.add(slot, total);
  spDmaWrCnt.bump(slot);                                     // este camino es solo SP -> RDRAM
  spDmaWrBytes.add(slot, total);
  spDmaWrHist[spDmaBucket(total)].bump(slot);
  if(imem) { spDmaImemCnt.bump(slot); spDmaImemBytes.add(slot, total); }
  const std::vector<u8>& sp = imem ? this->imem : this->dmem;
  const u64 pay = dmaPayTail;
  u64 w = pay;
  for(u32 c = 0; c < count; c++) {
    for(u32 i = 0; i < length; ) {
      const u32 mo = (memOff + i) & 0xfff;
      u32 n = length - i;
      if(n > 0x1000u - mo) n = 0x1000u - mo;                      // vuelta de la SP mem
      const u64 wo = w & kDmaPayM;
      if(n > kDmaPayN - wo) n = (u32)(kDmaPayN - wo);              // vuelta del anillo
      std::memcpy(&dmaPay[wo], &sp[mo], n);
      w += n; i += n;
    }
    memOff = (memOff + length) & 0xfff;
  }
  dmaPayTail = w;
  // Los registros cambian en el instante del RSP, igual que cualquier otra escritura suya al SP.
  rcp.sp_mem_addr  = (imem ? 0x1000 : 0) | memOff;
  rcp.sp_dram_addr = (dram + (length + skip) * count) & 0xffffff;
  rcp.sp_rd_len = rcp.sp_wr_len = 0xff8;
  const u32 t = dpLogTail.load(std::memory_order_relaxed);
  dpLogDma[t & kDpLogM] = {dram, length, count, skip, pay};
  bumpOwned(dmaLogPushes);   // solo escribe el hilo del RSP: sin LOCK XADD
  // Las paginas se apuntan ANTES de publicar la entrada (dpLogPush suelta la cola con release):
  // asi el hilo de CPU no puede ver la entrada sin ver a que paginas afecta.
  dmaPgPend.fetch_add(1, std::memory_order_relaxed);
  dmaPgMark(dram, (u64)(length + skip) * count, +1);
  dpLogPush(at, 16u, 0);
  return true;
}

// Aplicacion en el hilo de quien consume el diario: la mitad RDRAM del DMA de arriba.
auto Memory::spDmaLogApply(const DpLogDma& d) -> void {
  u64 r = d.pay;
  u32 dram = d.dram;
  for(u32 c = 0; c < d.count; c++) {
    for(u32 i = 0; i < d.length; ) {
      const u64 ro = r & kDmaPayM;
      u32 n = d.length - i;
      if(n > kDmaPayN - ro) n = (u32)(kDmaPayN - ro);
      const u32 dd = dram + i;
      if(dd < rdram.size()) {
        u32 m = n;
        if(m > (u32)(rdram.size() - dd)) m = (u32)(rdram.size() - dd);  // final de los chips
        std::memcpy(&rdram[dd], &dmaPay[ro], m);
      }
      r += n; i += n;
    }
    dram += d.length + d.skip;
  }
  dmaPayHead.store(r, std::memory_order_release);
}

// Arranca el command processor del RDP sobre lo que haya pendiente entre el ultimo trozo
// encolado y DPC_END. Se llama desde la escritura de DPC_END y TAMBIEN al limpiar FREEZE:
// congelar el RDP en HW para el procesador de comandos pero NO tira lo pendiente, asi que
// al descongelar el RDP reanuda desde donde estaba. Sin esa reanudacion los DPC_END que
// llegan congelados se pierden para siempre -- Perfect Dark congela el RDP entre tareas
// graficas, y su segunda tarea dejaba 128 bytes de comandos (con su SYNC_FULL) sin ejecutar:
// sin interrupcion DP el planificador de libultra da el RDP por ocupado eternamente y el
// juego no vuelve a emitir una tarea grafica nunca mas.
// Diagnostico KESTREL_RDPINLINE: el RDP corre en el hilo de CPU aun en modo threaded. Decide
// tambien que hilo es el dueno de Granite, y por tanto quien puede llamar a vrdp::idle.
static auto rdpInlineDiag() -> bool {
  static const bool on = std::getenv("KESTREL_RDPINLINE") != nullptr;
  return on;
}

auto Memory::dpcAdvance() -> void {
  if(rcp.dpc_status & (1u << 1)) return;      // congelado: no se consume nada todavia
  bool xbus = rcp.dpc_status & 0x1u;   // DP_STATUS_XBUS: fetch commands from DMEM
  // Kicking the FIFO starts the graphics clock and marks the pipe busy; both
  // stay set until a SYNC_FULL drains the pipe (DP_STATUS "flags during a run").
  rcp.dpc_status |= 0x8u | 0x20u;      // START_GCLK | PIPE_BUSY
  tlDpcKick = true;
  // Se encola desde donde quedo el ULTIMO encolado, no desde CURRENT: CURRENT es
  // ahora el avance real del rasterizador y puede ir por detras si el RDP sigue
  // ocupado con el trabajo anterior.
  u32 cur = rcp.dpc_submitted;
  rcp.dpc_submitted = rcp.dpc_end;
  // Lockstep: rasterize synchronously (deterministic, systemtest path).
  // Threaded: enqueue; the RDP worker rasterizes async and raises MI_DP itself.
  // Both routes funnel through rdpRunJob, so results are identical — only the
  // DP-interrupt/pixel-visibility *timing* differs, exactly as on hardware.
  // Diagnostico: KESTREL_RDPINLINE corre el RDP en el hilo CPU aun en modo threaded.
  // Sirve para bisecar que worker introduce una carrera, no para uso normal.
  const bool rdpInline = rdpInlineDiag();
  static const bool dpSyncLog = std::getenv("KESTREL_DPSYNCLOG") != nullptr;
  if(dpSyncLog) {
    std::fprintf(stderr, "[dpkick] span=%06x..%06x xbus=%u busy=%u\n",
                 cur, rcp.dpc_end, (unsigned)xbus,
                 (unsigned)rdpBusy.load(std::memory_order_relaxed));
    std::fflush(stderr);
  }
  if(rcpMode == RcpMode::Threaded && !rdpInline) rdpSubmit(cur, rcp.dpc_end, xbus);
  else if(rcpMode == RcpMode::Lockstep && rcpDeadlineOn()) {
    // Lockstep pinta aqui mismo, pero el RDP de la consola NO termina en la instruccion que
    // escribe DPC_END: tarda lo que cuesta el tramo en GCLK. Antes MI_DP salia en ese mismo
    // instante y DPC_CURRENT/STATUS se veian drenados al momento, mientras Threaded fechaba
    // el tramo con su coste -- dos maquinas distintas, y el oraculo era la menos fiel (DK64
    // divergia en el primer MI_DP del juego). Ahora los dos modos usan el MISMO horario de
    // invitado: dpScheduleSpan fecha arranque y cierre, las lecturas de DPC salen de ese
    // horario y MI_DP lo publica rcpRetire al vencer el plazo.
    const u64 kick = lockRspExec ? rspGuestNowAt(rsp.exactCycles()) : cartNow();
    {
      std::lock_guard<std::mutex> lk(rdpMx);
      dpScheduleSpan(cur, rcp.dpc_end, xbus, rdram.data(), kick);
    }
    dpJobOps = kick;
    rdpRunJob(cur, rcp.dpc_end, xbus, rdram.data(), rdpCostOn());
  }
  else rdpRunJob(cur, rcp.dpc_end, xbus, rdram.data());
}

auto Memory::siDma(bool toPif) -> void {
  // Arrancar un DMA con otro en vuelo no le pasa a un juego sano (el SDK espera a la
  // interrupcion antes de tocar el SI otra vez), pero si ocurre se cierra el anterior en
  // vez de perderlo: dejar un plazo huerfano seria un despertar que nunca llega.
  if(siBusy) siFinish();
  u32 dram = rcp.si_dram_addr & 0xffffff;
  siDram = dram;
  u64 us = kSiXferUs;   // el traslado de los 64 B entre RDRAM y PIF RAM
  ramBytesSi.fetch_add(64, std::memory_order_relaxed);
  dmaSettle(dram, (u64)dram + 64);
  if(toPif) {
    // RDRAM -> PIF RAM: solo deja el bloque de ordenes en la PIF. El PIF NO lo ejecuta
    // aqui (ver el comentario del DMA de lectura).
    for(u32 i = 0; i < 64; i++) if(dram + i < rdram.size()) pifram[i] = rdram[dram + i];
    // Byte de CONTROL de la PIF RAM (0x3F). No es un byte de datos: es donde la CPU le pide
    // trabajo al PIF, y el PIF apaga el bit en cuanto lo acepta. El bit 0 ("ejecutar el
    // bloque de ordenes", `CONT_CMD_EXE` del SDK, que libultra escribe en `pifstatus` antes
    // de CADA escritura del bloque) se consume aqui: el PIF lo lee al recibir el bloque,
    // analiza las ordenes y lo borra. Sin borrarlo, un juego que relea el byte esperando a
    // que se apague se queda dando vueltas para siempre. Los transferimientos del joybus en
    // si no van aqui, van en la lectura (ver abajo), que es donde el hardware los corre.
    if(pifram[63] & 0x01) pifram[63] = (u8)(pifram[63] & ~0x01u);
  } else if(pifram[63] & 0x02) {
    // Bit 1 del byte de control: DESAFIO del CIC. No es una transaccion de joybus -- el PIF
    // habla con el chip CIC del cartucho por su linea serie propia -- asi que esta lectura
    // no toca a los mandos. Ver pifCicChallenge().
    pifCicChallenge();
    pifram[63] = (u8)(pifram[63] & ~0x02u);
  } else {
    // PIF RAM -> RDRAM: el PIF ejecuta el bloque de ordenes JUSTO ANTES de entregarlo.
    // Es lo que hace el hardware y de lo que depende el SDK: osContStartReadData solo
    // reescribe el bloque cuando la orden anterior no era CONT_CMD_READ_BUTTON, asi que
    // a partir del segundo fotograma el juego lanza SOLO este DMA de lectura. Si el
    // joybus no corre aqui, el juego relee la misma respuesta vieja fotograma tras
    // fotograma y los botones se congelan: solo se ven pulsaciones largas, las que
    // duran hasta el siguiente DMA de escritura.
    // El joybus corre AQUI (el PIF lo corre antes de contestar al RD64B) y dice cuanto
    // dura; la copia a RDRAM se hace al vencer el plazo, en siFinish().
    us += pifProcessJoybus();
  }
  // Plazo: el joybus es serie y lento (4 us por bit), asi que la transaccion tarda cientos
  // de microsegundos y el hardware solo levanta MI_SI cuando termina. Aqui se arma el plazo
  // en el reloj del invitado; quien lo remata es siFinish() (interprete: en la instruccion
  // exacta; JIT: la guarda le prohibe saltarselo dentro de un bloque).
  // A/B: KESTREL_SIINSTANT=1 devuelve el comportamiento anterior (la transaccion termina en
  // la misma instruccion que la arranca). Solo para bisecar: en hardware NO pasa.
  static const bool instant = std::getenv("KESTREL_SIINSTANT") != nullptr;
  siBusy   = true;
  siToPif  = toPif;
  siDoneAt = cartNow() + (instant ? 0 : usToInsns(us));
  // Lo arranca un store, que con el JIT puede ir en mitad de una cadena enlazada cuyo permiso
  // no conocia este plazo: DK64 Lockstep veia MI_SI ~2600 instrucciones tarde con JIT. El
  // resto del bloque en curso no llega (una transaccion dura cientos de us = miles de ops).
  if(jitGuardPtr) *jitGuardPtr = 0;
  rcp.si_status = SI_DMA_BUSY;
  if(instant) siFinish();
}

// Vencimiento del plazo del SI. La entrega a RDRAM se hace AQUI y no al arrancar porque es
// cuando el hardware la tiene hecha: el PIF va contestando byte a byte durante la ventana.
auto Memory::siFinish() -> void {
  if(!siBusy) return;
  siBusy = false;
  if(!siToPif) {
    u32 dram = siDram;
    // PIF -> RDRAM tambien es escritura del hilo de CPU visible para el RSP: misma barrera.
    if(wrBarrierOn() && dmaBarrierOn()) cpuRamWrBarrier(cartNow());
    if(watchAddr) std::fprintf(stderr, "[siDma] PIF->RDRAM dram=0x%06x by pc=0x%08x\n", dram, (u32)storePc);
    dmaSettle(dram, (u64)dram + 64);
    for(u32 i = 0; i < 64; i++) if(dram + i < rdram.size()) { wrtag::mark(dram + i, wrtag::kSiDma, 0); watchHit(dram + i, 1, pifram[i], true); rdram[dram + i] = pifram[i]; }
  }
  rcp.si_status = 0;
  raiseIntr(MI_SI);
}

// --- Desafio anti-pirateria del CIC-NUS-6105 ---------------------------------
// Los cartuchos firmados con 6105/7105 (Perfect Dark, Donkey Kong 64, Majora's Mask,
// Banjo-Tooie...) pueden preguntarle al chip CIC algo que solo el chip sabe contestar. El
// juego deja 15 bytes en la PIF RAM (0x30..0x3E), pone el bit 1 del byte de control y lee el
// bloque: el PIF le pasa al CIC los 30 NIBBLES en orden, el CIC devuelve otros 30, y el juego
// compara con lo que esperaba. Un cartucho copiado sin CIC autentico no puede contestar.
//
// El algoritmo es una maquina de estados sobre nibbles con una tabla de 32 entradas indexada
// por (seleccion << 4 | dato); esta publicado en n64brew (paginas PIF-NUS y CIC-NUS) desde que
// se volco la ROM del 6105. No hay nada especifico de un juego aqui: es la funcion del chip.
//
// Ningun otro CIC implementa esta orden, y ningun cartucho que no sea 6105 la pide. Si llegara
// igualmente, lo unico honesto es no inventarse una respuesta: se deja el bloque como estaba
// (que es lo que el juego leeria de una linea que nadie contesta) y se avisa una vez.
auto Memory::pifCicChallenge() -> void {
  if(std::getenv("KESTREL_SILOG"))
    std::fprintf(stderr, "[silog] desafio del CIC (6105=%d) pc=0x%08x\n", (int)cic6105, (u32)storePc);
  if(!cic6105) {
    static bool warned = false;
    if(!warned) { warned = true;
      std::fprintf(stderr, "[pif] desafio del CIC pedido por un cartucho que no es 6105: sin respuesta\n"); }
    return;
  }
  static const u8 kLut[32] = {
    0x4, 0x7, 0xa, 0x7, 0xe, 0x5, 0xe, 0x1,
    0xc, 0xf, 0x8, 0xf, 0x6, 0x3, 0x6, 0x9,
    0x4, 0x1, 0xa, 0x7, 0xe, 0x5, 0xe, 0x1,
    0xc, 0x9, 0x8, 0x5, 0x6, 0x3, 0xc, 0x9,
  };
  u8 nib[30];
  for(int i = 0; i < 15; i++) { nib[i * 2] = (u8)(pifram[0x30 + i] >> 4); nib[i * 2 + 1] = (u8)(pifram[0x30 + i] & 0xf); }
  u8 key = 0xb, sel = 0;
  for(int i = 0; i < 30; i++) {
    u8 data = (u8)((key + 5 * nib[i]) & 0xf);
    nib[i] = data;
    key = kLut[(sel << 4) | data];
    u8 mod = (u8)(data >> 3);            // bit alto del nibble
    u8 mag = (u8)(data & 7);             // los tres bajos
    if(mod) mag = (u8)(~mag & 7);
    if(mag % 3 != 1) mod = (u8)!mod;
    if(sel) {                            // dos datos fuerzan el signo cuando venimos de 1
      if(data == 0x1 || data == 0x9) mod = 1;
      if(data == 0xb || data == 0xe) mod = 0;
    }
    sel = mod;
  }
  for(int i = 0; i < 15; i++) pifram[0x30 + i] = (u8)((nib[i * 2] << 4) | nib[i * 2 + 1]);
}

// Run the 64-byte PIF RAM joybus command block and fill in device responses,
// matching what libultra's __osContGetInitData / __osEepStatus expect to read
// back. Byte scan: 0xFE ends the block, 0x00 skips to the next channel, 0xFF is
// padding. Any other value is a TX byte count that opens a command whose result
// goes into the RX area; the RX-size byte carries the channel error flags.
//
// Dispositivos emulados: canales 0-3 = los cuatro conectores de mando (cada uno con su
// accesorio: nada, Controller Pak o Rumble Pak), canal 4 = EEPROM. Un conector sin mando y
// los canales 5+ contestan "no device", que es lo que hace la consola con el hueco vacio.
auto Memory::pifProcessJoybus() -> u32 {
  static int silog = std::getenv("KESTREL_SILOG") ? 1 : 0;
  const u8 NO_DEVICE = 0x80;   // CONT_NO_RESPONSE: (rxsize & 0xC0) >> 4 = 0x8
  // Coste de LINEA del joybus, que es lo unico que hace lenta la transaccion: la consola y
  // el mando hablan por un solo hilo a 4 us por bit (bit de parada de la consola 3 us, el
  // del mando 4 us). Se factura por orden, contando los bytes que de verdad viajan: los TX
  // siempre, los RX solo si el dispositivo contesta. De ahi salen ~167 us por una lectura
  // de botones (1 byte de orden + 4 de respuesta) y ~670 us por los cuatro conectores, que
  // es la cifra que se mide en hardware.
  u32 us = 0;
  auto billTx = [&](u32 n)  { us += n * 8 * 4 + 3; };   // bytes hacia el dispositivo + parada consola
  auto billRx = [&](u32 n)  { us += n * 8 * 4 + 4; };   // bytes de vuelta + parada del mando
  int channel = 0;
  for(int i = 0; i < 63; ) {
    u8 t = pifram[i];
    if(t == 0xFE) break;                              // end of commands
    if(t == 0x00) { channel++; i++; continue; }       // skip this channel
    if(t == 0xFF) { i++; continue; }                  // padding / NOP
    u8 tx = t & 0x3f;
    u8 rx = pifram[i + 1] & 0x3f;
    int txStart = i + 2;
    int rxStart = i + 2 + tx;
    if(rxStart + rx > 64 || txStart >= 64) break;     // malformed — bail out
    u8  cmd  = pifram[txStart];
    u8* rxp  = &pifram[rxStart];
    u8& rxsz = pifram[i + 1];                          // holds the error flags on return

    billTx(tx);
    auto absent = [&] { rxsz |= NO_DEVICE;
      if(silog) std::fprintf(stderr, "[silog] ABSENT ch=%d cmd=0x%02x tx=%u rx=%u pc=0x%08x\n",
                             channel, cmd, tx, rx, (u32)storePc); };
    if(silog) std::fprintf(stderr, "[silog] cmd ch=%d cmd=0x%02x tx=%u rx=%u pc=0x%08x\n",
                           channel, cmd, tx, rx, (u32)storePc);

    if(channel < 4) {                                 // uno de los cuatro conectores
      PadPort& pp = padPort[channel];
      if(!pp.connected) { absent(); channel++; i = rxStart + rx; continue; }
      switch(cmd) {
      case 0x00:                                      // request status / info
      case 0xFF:                                      // reset (same reply)
        // Byte de estado: CONT_CARD_ON (0x01) si hay accesorio en la ranura. CONT_CARD_PULL
        // (0x02) se deja a cero a proposito -- ambos puestos significan "pak recien
        // cambiado" y el SDK devuelve PFS_ERR_NEW_PACK, que el juego ensena como error.
        if(rx >= 3) { rxp[0] = 0x05; rxp[1] = 0x00;   // tipo 0x0005 (mando estandar)
                      rxp[2] = pp.accessory ? 0x01 : 0x00; }
        break;
      case 0x01: {                                    // read buttons
        // El mando inyectado por telemetria pisa al del anfitrion mientras le queden
        // sondeos, y el gasto se contabiliza AQUI (una lectura del joybus), que es lo unico
        // que el juego percibe como el paso del tiempo del mando.
        if(channel == 0) padPolls.fetch_add(1, std::memory_order_relaxed);
        u32 btn = pp.buttons; s8 sx = pp.stickX, sy = pp.stickY;
        s32 left = padRemotePolls[channel].load(std::memory_order_acquire);
        if(left != 0) {
          btn = padRemoteButtons[channel].load(std::memory_order_relaxed);
          s32 st = padRemoteStick[channel].load(std::memory_order_relaxed);
          sx = (s8)(st & 0xff); sy = (s8)((st >> 8) & 0xff);
          if(left > 0) padRemotePolls[channel].store(left - 1, std::memory_order_release);
        }
        // Grabacion/reproduccion de entradas: esta es la frontera que el juego percibe, con
        // el mando del anfitrion y el inyectado por telemetria ya resueltos.
        movie::sample(channel, btn, sx, sy);
        for(int k = 0; k < rx; k++) rxp[k] = 0x00;
        if(rx >= 2) { rxp[0] = (btn >> 8) & 0xff; rxp[1] = btn & 0xff; }
        if(rx >= 4) { rxp[2] = (u8)sx; rxp[3] = (u8)sy; }  // analog stick
        break;
      }
      case 0x02:                                      // leer 32 bytes del accesorio
      case 0x03: {                                    // escribir 32 bytes
        // La direccion viaja como (bloque << 5) | CRC5(bloque): 11 bits de bloque de 32
        // bytes, o sea 64 KiB de espacio de direcciones para 32 KiB de Controller Pak. La
        // mitad alta la usa el Rumble Pak: 0x8000 es su firma y 0xC000 el motor.
        const bool wr = cmd == 0x03;
        u16 wire = tx >= 3 ? (u16)((pifram[txStart + 1] << 8) | pifram[txStart + 2]) : 0;
        u16 block = (u16)(wire >> 5);
        u32 off = (u32)block * 32;
        u8 data[32];
        if(wr) for(int k = 0; k < 32; k++) data[k] = (3 + k) < tx ? pifram[txStart + 3 + k] : 0;

        if(pp.accessory == 1) {                       // Controller Pak: 32 KiB de RAM
          if(wr) {
            if(off + 32 <= pp.mempak.size()) {
              std::memcpy(&pp.mempak[off], data, 32);
              pp.mempakDirty = true;
            }
          } else {
            for(int k = 0; k < 32; k++)
              data[k] = off + k < pp.mempak.size() ? pp.mempak[off + k] : 0x00;
            for(int k = 0; k < rx && k < 32; k++) rxp[k] = data[k];
          }
        } else if(pp.accessory == 2) {                // Rumble Pak: sin RAM, solo motor
          // El SDK lo reconoce escribiendo 0x80 en toda la ventana 0x8000 y releyendola:
          // si devuelve 0x80 hay Rumble. Fuera de esa ventana el pak no contesta nada
          // (ceros), y una escritura en 0xC000 enciende (dato != 0) o apaga el motor.
          if(wr) {
            if(off >= 0xC000 && off < 0xD000) pp.rumble = data[0] != 0;
          } else {
            u8 fill = (off >= 0x8000 && off < 0x9000) ? 0x80 : 0x00;
            for(int k = 0; k < 32; k++) data[k] = fill;
            for(int k = 0; k < rx && k < 32; k++) rxp[k] = data[k];
          }
        } else {                                      // ranura vacia
          if(!wr) { for(int k = 0; k < 32; k++) data[k] = 0x00;
                    for(int k = 0; k < rx && k < 32; k++) rxp[k] = 0x00; }
        }

        // El CRC de datos cierra la respuesta en los dos sentidos: en la lectura cubre lo
        // devuelto, en la escritura lo que el juego mando (asi comprueba que llego bien).
        // Con la ranura VACIA el mando devuelve el CRC INVERTIDO, que es justo como el SDK
        // detecta la ausencia antes de ir a preguntar el estado.
        u8 crc = mempakDataCrc(data);
        if(!pp.accessory) crc = (u8)~crc;
        int crcAt = wr ? 0 : 32;
        if(rx > crcAt) rxp[crcAt] = crc;
        if(silog && mempakAddrCrc(block) != (wire & 0x1f))
          std::fprintf(stderr, "[silog] pak addr CRC malo ch=%d wire=0x%04x\n", channel, wire);
        break;
      }
      default: absent(); break;
      }
    } else if(channel == 4) {                          // EEPROM (16 kbit)
      switch(cmd) {
      case 0x00:                                      // status: type byte 0x80 (4k) / 0xC0 (16k)
        if(rx >= 3) { rxp[0] = 0x00; rxp[1] = eepromTypeByte(); rxp[2] = 0x00; }
        break;
      case 0x04: {                                    // read 8-byte block; tx = [cmd, addr]
        u8 addr = tx >= 2 ? pifram[txStart + 1] : 0;
        for(int k = 0; k < rx && k < 8; k++) {
          u32 off = (u32)addr * 8 + k;
          rxp[k] = off < eeprom.size() ? eeprom[off] : 0;
        }
        break;
      }
      case 0x05: {                                    // write 8-byte block; tx = [cmd, addr, 8 data]
        u8 addr = tx >= 2 ? pifram[txStart + 1] : 0;
        for(int k = 0; k < 8 && (2 + k) < tx; k++) {
          u32 off = (u32)addr * 8 + k;
          if(off < eeprom.size()) { eeprom[off] = pifram[txStart + 2 + k]; saveDirty = true; }
        }
        if(rx >= 1) rxp[0] = 0x00;                    // status OK
        break;
      }
      default: absent(); break;
      }
    } else {
      absent();                                        // canal 5+: no existe
    }
    // Un conector vacio no contesta: la consola espera el tiempo de espera del bus y sigue.
    // Se factura como si la respuesta no hubiera existido, que es lo que ocurre en la linea.
    if(!(rxsz & NO_DEVICE)) billRx(rx);
    channel++;
    i = rxStart + rx;
  }
  return us;
}

// --- VI field tick -----------------------------------------------------------
auto Memory::viTick(u64 retiredNow) -> bool {
  // Reloj de video derivado del contador de instrucciones retiradas: un campo dura
  // viFieldInsns instrucciones (lo fija System desde Clocks::fieldInsns(), y la lectura de
  // VI_V_CURRENT usa EXACTAMENTE el mismo numero, para que interrupcion y sondeo midan el
  // mismo tiempo). El bucle llama aqui varias veces por campo, asi que hay que detectar
  // cruces, no "estar por encima": la version anterior levantaba MI_VI en casi todas las
  // llamadas (vi_current >= vi_intr se cumple casi siempre) y solo cerraba campo al dar la
  // vuelta entera al contador, o sea una vez cada 263 llamadas.
  u32 total = rcp.viHalflines();   // medias-lineas por campo (mismo sitio que VI_V_CURRENT)
  u64 field = viFieldInsns ? viFieldInsns : 1;
  // Instante del campo (en instrucciones) en que el barrido pasa por la linea programada.
  u64 off = (u64)(rcp.vi_intr & 0x3fe) * field / total;
  auto crossings = [&](u64 x) -> u64 { return x >= off ? (x - off) / field + 1 : 0ull; };
  // V_INTR == 0 es una linea de coincidencia VALIDA (media-linea 0, arranque de campo): el
  // VI compara V_CURRENT con V_INTR sin caso especial, y hay homebrew (libdragon) que deja
  // la linea en 0 y espera la interrupcion de campo ahi. El valor de reset es 256, no 0,
  // asi que no hace falta ningun guard para las ROMs que nunca programan el registro.
  bool intrFire   = crossings(retiredNow) > crossings(viLastRetired);
  bool fieldClose = (retiredNow / field) > (viLastRetired / field);
  viLastRetired = retiredNow;
  rcp.vi_current = (u32)((retiredNow % field) * total / field) & ~1u;
  if(intrFire) raiseIntr(MI_VI);
  if(fieldClose) {
    rcp.viFields++;
    // Barrido de video: el VI no tiene framebuffer propio, relee la imagen de RDRAM cada
    // campo, una linea por linea de salida (su buffer interno es de una linea). Es trafico
    // continuo y con prioridad sobre el RDP -- de ahi el factor T_VI del modelo de coste --
    // asi que en el medidor del bus pesa tanto como para verse: 320x237x2 por campo a 60 Hz
    // son ya ~9 MB/s con la pantalla quieta.
    if((rcp.vi_ctrl & 3) >= 2 && rcp.vi_origin && rcp.vi_width) {
      u32 vs = (rcp.vi_vstart >> 16) & 0x3ff, ve = rcp.vi_vstart & 0x3ff;
      u32 lines = ve > vs ? (ve - vs) / 2 : 0;          // el registro va en medias lineas
      u32 bpp   = (rcp.vi_ctrl & 3) == 3 ? 4u : 2u;
      ramBytesVi.fetch_add((u64)rcp.vi_width * lines * bpp, std::memory_order_relaxed);
    }
    // Cierre de campo: rota el contexto por-cuadro del backend GPU. Corre en el hilo de
    // emulacion, el mismo que encola comandos del RDP — begin_frame_context tiene que ir
    // serializado con la emision (el scanout es la unica llamada entre hilos). No-op si
    // esta apagado. Antes practicamente no se llamaba (solo al dar la vuelta el contador).
    vrdp::frameBegin();
    // Con el RDP en este mismo hilo (lockstep, o el diagnostico RDPINLINE) no hay worker que
    // se quede ocioso: el aviso de ocio de parallel-rdp va una vez por campo. Sin el, una lista
    // que no acaba en SYNC_FULL no llega nunca a la GPU y la RDRAM se queda sin pintar.
    if(rcpMode != RcpMode::Threaded || rdpInlineDiag()) vrdp::idle();
  }
  // El DAC drena por muestras, no por campos: fuera del cierre de campo, en cada subtramo.
  aiTick(retiredNow);
  // Rearma el plazo: a partir de aqui el bucle (y la guarda del JIT) saben en que instruccion
  // EXACTA vuelve a haber trabajo de VI, en vez de mirarlo cada campo/viTicksPerField.
  viNextAt = viNextEvent(retiredNow);
  evArm();   // aiTick (justo arriba) ya ha rearmado aiNextAt; aqui se pliegan los dos
  return fieldClose;
}

// Drain the AI playback FIFO ~one field's worth of samples per call. When the
// current buffer empties, pop it, raise MI_AI (audio DMA done) and start the
// next. This paces the audio driver: it blocks on FIFO_FULL between buffers
// instead of spinning in the frame builder and starving the gfx thread.
auto Memory::aiTick(u64 retiredNow) -> void {
  // El DAC del AI no va por campos, va por muestras: consume 4 bytes (16 bits estereo) cada
  // 1/rate segundos, con rate = vid_clock/(dacrate+1). La version anterior gastaba COMO MUCHO
  // un bufer por campo y tiraba el credito sobrante, asi que un juego que encola bufers mas
  // cortos que un campo -- SM64 los encola a ~0.75 campos -- veia su FIFO drenar al 75% del
  // ritmo real: se quedaba bloqueado en FIFO_FULL y generaba solo el 75% del audio que le
  // tocaba. Eso es el audio entrecortado, y no es del sumidero del host: el guest producia de
  // menos. Ahora el credito se acumula en el mismo reloj que todo lo demas (instrucciones
  // retiradas) y se drenan TANTOS bufers como quepan en el tiempo transcurrido.
  u64 delta = retiredNow > rcp.aiLastRetired ? retiredNow - rcp.aiLastRetired : 0;
  rcp.aiLastRetired = retiredNow;
  if(rcp.ai_fifo_count == 0) { rcp.aiAcc = 0; aiNextAt = ~0ull; return; }   // en silencio no se acumula credito
  // Un salto enorme (arranque, savestate, pausa larga) no debe vaciar la FIFO de golpe.
  if(delta > viFieldInsns * 4) delta = viFieldInsns * 4;
  u32 rate = rcp.ai_dacrate ? (aiVidClock / (rcp.ai_dacrate + 1)) : 32'000u;
  u64 den  = viFieldInsns * (u64)viFieldHzMilli;      // instrucciones por segundo x1000
  if(den == 0) { aiNextAt = ~0ull; return; }
  rcp.aiAcc += delta * ((u64)rate * 4ull * 1000ull);
  u64 bytes = rcp.aiAcc / den;
  rcp.aiAcc -= bytes * den;
  while(bytes && rcp.ai_fifo_count) {
    if(rcp.ai_play_remaining > bytes) { rcp.ai_play_remaining -= (u32)bytes; break; }
    bytes -= rcp.ai_play_remaining;
    // Bufer terminado: se saca, se avisa (MI_AI) y empieza el siguiente.
    rcp.ai_fifo_addr[0] = rcp.ai_fifo_addr[1];
    rcp.ai_fifo_len[0]  = rcp.ai_fifo_len[1];
    rcp.ai_fifo_count--;
    raiseIntr(MI_AI);
    rcp.ai_play_remaining = rcp.ai_fifo_count ? rcp.ai_fifo_len[0] : 0;
  }
  // Plazo del proximo fin de bufer: a partir de aqui el bucle sabe en que instruccion EXACTA
  // toca MI_AI, en vez de enterarse al final del subtramo (ver aiNextEvent).
  aiNextAt = aiNextEvent();
}


// --- RCP threading -----------------------------------------------------------
// Rasterize one RDP FIFO span and do the DP-done bookkeeping. Called inline on
// the CPU thread in lockstep, or on the RDP worker thread in threaded mode; in
// either case this is the sole owner of softRdp while it runs, so the rasterizer
// stays single-threaded. On SYNC_FULL it clears the busy flags and raises MI_DP —
// the atomic fetch_or in raiseIntr publishes (release) the pixel writes above it
// before the CPU can observe the interrupt (acquire on its mi_intr load).
// Arranque idempotente del backend GPU. Lo llama el hilo del RDP nada mas nacer y
// tambien rdpRunJob(), que es el unico camino en modo lockstep (ahi no hay hilo).
// std::call_once y no un bool: en modo threaded los dos sitios pueden coincidir.
auto Memory::vrdpBringUp() -> void {
  std::call_once(vrdpOnce, [this]{ vrdp::init(rdram.data(), (u32)rdram.size()); });
}

// Espera a que el hilo del RDP haya levantado parallel-rdp. El presentador comparte ese
// contexto Vulkan, pero NO puede levantarlo el mismo: Granite registra indice de hilo y ata
// su estado al hilo que lo crea, asi que traerlo arriba fuera del hilo del RDP deja al worker
// sin sus lookups por hilo y el RDP acaba sin completar trabajos (el juego se queda esperando
// MESG_DP_COMPLETE para siempre). De ahi que esto solo espere.
auto Memory::vrdpWaitReady(u32 timeoutMs) -> bool {
  if(!vrdp::built) return false;                      // este .exe no lo lleva: nada que esperar
  const char* e = std::getenv("KESTREL_PRDP");
  if(e && e[0] == '0') return false;                  // SoftRDP forzado: nada que esperar
  for(u32 i = 0; i < timeoutMs; i++) {
    if(vrdp::active()) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  return false;
}

// DPC_CURRENT es UN puntero de lectura, el del command processor. Al acabar un span no se
// puede publicar su final si detras hay spans encolados que empiezan mas abajo: el FIFO de
// F3DEX2 es circular y el microcodigo decide que hay hueco libre comparando su puntero de
// escritura contra CURRENT. Si CURRENT dice "tope del buffer" mientras siguen encolados
// tramos que arrancan en la base, el microcodigo da por consumido el ring entero, envuelve
// y reescribe comandos que el RDP todavia no ha leido: el rasterizador acaba decodificando
// basura a media instruccion, saca un SET_COLOR_IMAGE con direccion arbitraria y pinta el
// framebuffer encima del codigo del guest (visto en Perfect Dark: pixeles RGBA5551 en
// 0x0104d0, 0x009a74...). En hardware esto no pasa porque el puntero de lectura es unico y
// solo avanza cuando el RDP consume de verdad. Aqui la cola de trabajos es el artefacto, asi
// que al cerrar un span publicamos el arranque del siguiente pendiente en vez de nuestro fin.
auto Memory::rdpPublishCurrent(u32 fallback) -> void {
  u32 pub = fallback;
  u32 qlo = 0, qhi = 0;
  {
    std::lock_guard<std::mutex> lk(rdpMx);
    if(!rdpQueue.empty()) pub = rdpQueue.front().current;
    for(const auto& j : rdpQueue) {
      u32 a = j.current & 0x00ff'ffffu, b = j.end & 0x00ff'ffffu;
      if(!qhi || a < qlo) qlo = a;
      if(b > qhi) qhi = b;
    }
  }
  wrtag::setWin(0, 0, 0);
  wrtag::setWin(1, qlo, qhi);
  rcp.dpc_current.store(pub, std::memory_order_release);
}

// Cobrar el trabajo del RDP cuando quien rasteriza es la GPU.
//
// parallel-rdp dibuja en la tarjeta grafica y no tiene forma de decirle al invitado lo que
// aquello habria costado en la consola: el reloj de coste (rcp.rdpGclk) y DPC_CLOCK solo los
// alimenta SoftRdp::accountPixels. Sin esto, con la GPU activa el RDP le sale GRATIS al
// juego: DPC_CLOCK clavado a cero -- que es un registro que los juegos LEEN para medirse --
// y Memory::rdpPace no frena nunca, asi que la CPU del invitado nunca espera al RDP y el
// juego corre mas suelto de lo que corrio jamas en hardware. Medido en Donkey Kong 64: la
// demo de atraccion iba a 30 fps clavados sin perder un solo fotograma.
//
// El arreglo es pasar el MISMO tramo de FIFO por el decodificador de SoftRdp en modo
// solo-coste (ver SoftRdp::costOnly): recorre los comandos, mantiene el estado (scissor,
// modos, imagenes) y cobra los pixeles que entrarian al pipeline, pero no escribe ni un
// byte de RDRAM -- los pixeles buenos son los de la GPU. El coste es O(altura) por
// primitiva, no O(area), porque cada tramo se cobra de una vez.
//
// KESTREL_RDPCOST=0 lo apaga, para el A/B de rendimiento.
auto Memory::rdpCostOn() -> bool {
  static const bool on = []{ const char* e = std::getenv("KESTREL_RDPCOST"); return !(e && e[0] == '0'); }();
  return on;
}

static const bool g_dpSubProf = std::getenv("KESTREL_DPSUBPROF") != nullptr;

auto Memory::rdpCostPass(u32 current, u32 stop, bool xbus, const u8* cmdSrc) -> bool {
  if(!rdpCostOn() || stop == current) return false;
  softCost.costOnly = true;
  softCost.charge   = true;
  softCost.curOut   = nullptr;   // DPC_CURRENT lo publica quien pinta, no este paseo
  softCost.cmdSrc   = cmdSrc;
  const auto tC = g_dpSubProf ? std::chrono::steady_clock::now()
                              : std::chrono::steady_clock::time_point{};
  const u32 nc = softCost.run(*this, current, stop, xbus);
  if(g_dpSubProf) {
    rdpCostNs.fetch_add((u64)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - tC).count(), std::memory_order_relaxed);
    rdpCostCmds.fetch_add(nc, std::memory_order_relaxed);
  }
  return true;
}

auto Memory::rdpRunJob(u32 current, u32 end, bool xbus, const u8* cmdSrc, bool preCosted) -> void {
  // Coste de ESTE trabajo. Con preCosted ya esta cobrado y fechado por quien lanzo el tramo
  // (dpScheduleSpan): aqui no se cobra nada, solo se pinta.
  const u64 gclk0 = rcp.rdpGclk.load(std::memory_order_relaxed);
  // Publicar el fin de cuadro en el reloj del invitado en vez de en el del anfitrion. En
  // Lockstep no hace falta: lo ejecuta el propio hilo de CPU y ya cae en un punto determinista.
  const bool defer = rcpDeadlineOn();
  // GPU path (paraLLEl-RDP): opt-in via KESTREL_PRDP. Lazily brought up on first job with
  // this RDRAM block; when live it consumes the FIFO on the GPU instead of SoftRDP. The
  // guest RDRAM vector is allocated once and never resized, so its pointer is stable for
  // the CommandProcessor's lifetime. Stubs make this a no-op in non-PRDP builds.
  vrdpBringUp();
  // Reanudar un comando que quedo partido por el borde del span anterior. Solo si este span
  // continua justo donde acabo aquel: si el juego instalo un buffer nuevo (START fresco), el
  // trozo pendiente pertenece a un FIFO que ya no existe y se descarta, como en HW.
  const u32 rawCur = current;
  if(rdpHasResume && rawCur == rdpLastEnd) current = rdpResume;
  u32 costCur = rawCur;
  if(!preCosted) {
    if(rdpCostHasResume && rawCur == rdpCostLastEnd) costCur = rdpCostResume;
    rdpCostHasResume = false;
    rdpCostLastEnd = end;
  }
  rdpHasResume = false;
  rdpLastEnd = end;
  rcp.dpc_current.store(current, std::memory_order_release);   // el consumidor abre el span
  wrtag::setWin(0, current & 0x00ff'ffffu, end & 0x00ff'ffffu);
  if(vrdp::active()) {
    // `stop` puede quedar delante de `end` si el ultimo comando del span esta partido: el
    // command processor no ejecuta comandos a medias. Ese trozo NO se pierde -- el siguiente
    // kick reanuda desde ahi porque dpc_submitted retrocede al punto de parada.
    // El paseo de coste va DELANTE del dibujado, y no es un detalle de orden.
    //
    // Es software puro, determinista, y no escribe un byte: recorre los comandos con el
    // decodificador de SoftRdp y cobra en rcp.rdpGclk lo que ese tramo habria costado en la
    // consola. Ponerlo detras de vrdp::runFifo (como estaba) deja rdpGclk CLAVADO durante todo
    // el trabajo y lo sube de golpe al final, con dos consecuencias:
    //
    //   - Memory::rdpPace frena a la CPU contra "el trabajo que el RDP lleva hecho", y ese
    //     trabajo valia cero hasta el ultimo instante: el freno no mordia mientras la GPU
    //     dibujaba, que es justo cuando tiene que morder.
    //   - el plazo de fin de tarea (dpEndArm) nacia con el coste ya conocido pero con la CPU
    //     del invitado ya pasada de largo, o sea VENCIDO -- y entonces MI_DP cae donde el
    //     anfitrion decida. Dos corridas del mismo binario publicaban la interrupcion en
    //     instrucciones distintas y el juego divergia (docs/GAPS.md 3b).
    //
    // Cobrando primero, el coste del tramo esta puesto antes de que la GPU empiece: el freno
    // acota a la CPU contra el trabajo REAL y el plazo nace vivo.
    if(!preCosted && rdpCostPass(costCur, end, xbus, cmdSrc)) {
      const u32 endM = xbus ? (end & 0x0fff'ffffu) : (end & 0x00ff'ffffu);
      if(softCost.stopAt != endM) { rdpCostResume = softCost.stopAt; rdpCostHasResume = true; }
    }
    u32 stop = end;
    bool sync = vrdp::runFifo(cmdSrc, (u32)rdram.size(), dmem.data(), current, end, xbus,
                              &stop);
    if(stop != end) { rdpResume = stop; rdpHasResume = true; }
    rdpPublishCurrent(stop);
    if(sync) {
      ev("rdpint", current, end);
      if(defer) {
        // Con el tramo ya fechado, el plazo de MI_DP lo armo quien lo lanzo.
        if(!preCosted) dpEndArm(dpJobOps, rcp.rdpGclk.load(std::memory_order_relaxed) - gclk0);
      } else {
        rcp.dpc_status &= ~(0x8u | 0x20u); // pipe drained: clear START_GCLK | PIPE_BUSY
        rcp.dpSyncs++;                     // misma contabilidad que el camino SoftRDP
        dpRets.fetch_add(1, std::memory_order_relaxed);
        raiseIntr(MI_DP);
      }
    }
    return;
  }
  // El rasterizador publica su puntero de lectura en DPC_CURRENT mientras consume el
  // FIFO: es lo que el microcodigo mira para saber cuanto buffer puede reutilizar.
  rcp.dpc_current.store(current, std::memory_order_release);   // el consumidor abre el span
  wrtag::setWin(0, current & 0x00ff'ffffu, end & 0x00ff'ffffu);
  // El paseo de coste va DELANTE tambien aqui. Antes este camino cobraba mientras pintaba, y
  // eso deja el instante de cierre del trabajo sin conocer hasta el ultimo pixel: cualquiera
  // que preguntase por el RDP a mitad de trabajo recibia una respuesta que dependia de lo
  // rapido que fuese el anfitrion pintando. Con el coste por delante el horario del trabajo
  // esta publicado antes de tocar un pixel, igual que en el camino de la GPU.
  {
    const bool costed = !preCosted && rdpCostPass(costCur, end, xbus, cmdSrc);
    if(costed) {
      const u32 endM = xbus ? (end & 0x0fff'ffffu) : (end & 0x00ff'ffffu);
      if(softCost.stopAt != endM) { rdpCostResume = softCost.stopAt; rdpCostHasResume = true; }
    }
    softRdp.charge = !costed && !preCosted;   // el coste se paga UNA vez
  }
  softRdp.costOnly = false;      // este camino SI pinta
  softRdp.curOut = &rcp.dpc_current;
  softRdp.cmdSrc = cmdSrc;
  u32 nc = softRdp.run(*this, current, end, xbus);
  // stopAt vive en el espacio de direcciones que usa SoftRdp::run (enmascarado igual que
  // alli), asi que la comparacion se hace contra el mismo `end` enmascarado.
  const u32 endMasked = xbus ? (end & 0x0fff'ffffu) : (end & 0x00ff'ffffu);
  if(softRdp.stopAt != endMasked) { rdpResume = softRdp.stopAt; rdpHasResume = true; }
  rdpPublishCurrent(softRdp.stopAt == endMasked ? end : softRdp.stopAt);
  static const bool rdpTrace = std::getenv("KESTREL_RDPTRACE") != nullptr;
  if(rdpTrace) {
    static u32 dpCalls = 0;
    if(dpCalls++ < 40)
      std::fprintf(stderr, "[rdp] job cur=%06x end=%06x cmds=%u ci=%06x sz=%u\n",
                   current, end, nc, softRdp.colorImage(), softRdp.colorImageSize());
  } else { (void)nc; }
  if(softRdp.sawSyncFull) {
    ev("rdpint", current, end);
    if(defer) {
      // Con el tramo ya fechado, el plazo de MI_DP lo armo quien lo lanzo.
      if(!preCosted) dpEndArm(dpJobOps, rcp.rdpGclk.load(std::memory_order_relaxed) - gclk0);
    } else {
      rcp.dpc_status &= ~(0x8u | 0x20u); // pipe drained: clear START_GCLK | PIPE_BUSY
      rcp.dpSyncs++;                     // "a frame finished rendering" -- see KESTREL_MAXSYNCS
      dpRets.fetch_add(1, std::memory_order_relaxed);
      raiseIntr(MI_DP);
    }
  }
}

auto Memory::evDump(u32 n) -> void {
  if(!evOn) { std::fprintf(stderr, "[ev] apagado (KESTREL_EVLOG=1 para grabar)\n"); return; }
  u32 end = evIdx.load(std::memory_order_relaxed);
  if(n > kEvN) n = kEvN;
  u32 first = end > n ? end - n : 0;
  std::fprintf(stderr, "[ev] ultimos %u eventos (clk = instrucciones retiradas)\n", end - first);
  for(u32 i = first; i < end; i++) {
    const Ev& e = evRing[i & (kEvN - 1)];
    if(!e.tag) continue;
    std::fprintf(stderr, "[ev] %12llu %-8s %08x %08x\n",
                 (unsigned long long)e.clk, e.tag, e.a, e.b);
  }
  std::fflush(stderr);
}

// Generaciones de la sombra del FIFO. 2 = comportamiento historico. Ver rdpShadow.
auto Memory::rdpGenCount() -> u8 {
  static const u8 v = []() -> u8 {
    const char* e = std::getenv("KESTREL_RDPGENS");
    if(e && *e) { char* end = nullptr; long n = std::strtol(e, &end, 0);
                  if(end && !*end && n >= 2 && n <= Memory::kRdpGens) return (u8)n; }
    // OCHO de fabrica, y 2 = comportamiento historico para bisecar. Con dos, junkrunner64
    // reutiliza sucia la sombra ~900 veces por corrida y 2 de cada 8 corridas divergen; con
    // ocho y eligiendo la libre mas baja, cero y cero. Ver docs/STATUS.md.
    return 8;
  }();
  return v;
}

auto Memory::rdpSnapshot(u32 current, u32 end) -> void {
  // Copia [current, end) de RDRAM al buffer de la generacion en curso. Se reserva al vuelo
  // (una vez, del tamano de la RDRAM) para no pagar la memoria en lockstep ni en las pruebas.
  auto& sh = rdpShadow[rdpGen];
  if(sh.size() != rdram.size()) sh.assign(rdram.size(), 0);
  u32 a = current & 0x00ff'ffffu, b = end & 0x00ff'ffffu;
  if(b > (u32)rdram.size()) b = (u32)rdram.size();
  if(a >= b) return;
  const u32 n = b - a;
  rdpSnapBytes.fetch_add(n, std::memory_order_relaxed);
  rdpSnapCopies.fetch_add(1, std::memory_order_relaxed);
  // Desde el hilo del RSP la CPU puede no haber aplicado aun DMA suyos a RDRAM (diario, reg 16)
  // fechados ANTES de este DPC_END: el microcodigo baja los comandos por DMA y enseguida lanza
  // el tramo. La cabeza se lee ANTES de copiar; lo que la CPU aplique mientras se copia vuelve a
  // escribirse igual desde el diario, en orden. Solo el RSP apunta, asi que la cola esta quieta.
  const bool ov = tlIsRspThread && rcpMode == RcpMode::Threaded;
  const u32 h0 = ov ? dpLogHead.load(std::memory_order_acquire) : 0;
  const u32 t0 = ov ? dpLogTail.load(std::memory_order_relaxed) : 0;
  u8*       dst = sh.data() + a;
  const u8* src = rdram.data() + a;
  std::memcpy(dst, src, n);
  for(u32 i = h0; i != t0; ++i)
    if(dpLog[i & kDpLogM].reg & 16u) spDmaOverlay(dpLogDma[i & kDpLogM], sh.data(), a, b);
}

// Los bytes de un DMA del diario que caen en [a, b), escritos en `base` (indexado por direccion).
auto Memory::spDmaOverlay(const DpLogDma& d, u8* base, u32 a, u32 b) const -> void {
  u64 r = d.pay;
  u32 dram = d.dram;
  for(u32 c = 0; c < d.count; c++) {
    for(u32 i = 0; i < d.length; ) {
      const u64 ro = r & kDmaPayM;
      u32 n = d.length - i;
      if(n > kDmaPayN - ro) n = (u32)(kDmaPayN - ro);
      const u32 dd = dram + i;
      const u32 lo = dd > a ? dd : a, hi = dd + n < b ? dd + n : b;
      if(lo < hi) std::memcpy(base + lo, &dmaPay[ro + (lo - dd)], hi - lo);
      r += n; i += n;
    }
    dram += d.length + d.skip;
  }
}


auto Memory::rdpSubmit(u32 current, u32 end, bool xbus) -> void {
  const auto tSub = g_dpSubProf ? std::chrono::steady_clock::now()
                                : std::chrono::steady_clock::time_point{};
  struct SubProf {
    Memory* m; std::chrono::steady_clock::time_point t0;
    ~SubProf() { if(g_dpSubProf) m->rdpSubNs.fetch_add(
        (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - t0).count(), std::memory_order_relaxed); }
  } subProf_{this, tSub};
  bool wake;
  {
    // Desglose del coste de esta llamada: cuanto cuesta COGER el mutex (contencion con el
    // worker del RDP) y cuanto planificar el tramo. Ver [dpsnap].
    const auto tLk = g_dpSubProf ? std::chrono::steady_clock::now()
                                 : std::chrono::steady_clock::time_point{};
    std::lock_guard<std::mutex> lk(rdpMx);
    if(g_dpSubProf) rdpLockNs.fetch_add(
        (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - tLk).count(), std::memory_order_relaxed);
    ev("dp.sub", current, end);
    // La copia se toma con el mutex cogido y ANTES de publicar el trabajo: el worker no
    // puede ver el tramo hasta que sus bytes estan a salvo. Solo el camino RDRAM; en xbus
    // los comandos viven en DMEM, que el RSP no reescribe mientras su tarea corre.
    {
      const auto tSn = g_dpSubProf ? std::chrono::steady_clock::now()
                                   : std::chrono::steady_clock::time_point{};
      if(!xbus) rdpSnapshot(current, end);
      if(g_dpSubProf) rdpSnapNs.fetch_add(
          (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - tSn).count(), std::memory_order_relaxed);
    }
    // EL HORARIO SE HACE AQUI, y fuera del if/else de mas abajo. El tramo [current, end) que
    // acaba de escribir el invitado se fecha entero -- coste, instante de arranque, instante
    // de cierre, plazo de MI_DP -- en el mismo hilo que escribio DPC_END y antes de que nadie
    // pinte nada. Lo que el invitado ve del motor sale solo de estas fechas, asi que ya no
    // depende de por donde vaya el anfitrion. Unir o no unir tramos en la cola de abajo es,
    // desde aqui, invisible.
    {
      const u8* costSrc = (!xbus && !rdpShadow[rdpGen].empty()) ? rdpShadow[rdpGen].data()
                                                               : rdram.data();
      const auto tSch = g_dpSubProf ? std::chrono::steady_clock::now()
                                    : std::chrono::steady_clock::time_point{};
      dpScheduleSpan(current, end, xbus, costSrc,
                     tlDpLogApply ? tlDpLogAt : tlIsRspThread ? rspGuestNow() : cartNow());
      if(g_dpSubProf) rdpSchedNs.fetch_add(
          (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - tSch).count(), std::memory_order_relaxed);
    }
    const bool sync = dpLastSpanSync;
    const u64 syncAt = dpSchedEnd.load(std::memory_order_relaxed);
    if(sync) {
      if(dpSyncEnds.empty()) dpSyncBarAt.store(syncAt, std::memory_order_release);
      dpSyncEnds.push_back(syncAt);
    }
    // El FIFO del RDP es UNO: escribir DPC_END no encola "un trabajo", solo adelanta el
    // puntero final del mismo buffer de comandos. Si el ultimo tramo encolado todavia no ha
    // empezado y este continua exactamente donde acababa, con el mismo modo de bus, es el
    // mismo tramo partido en dos escrituras y se pinta de una vez: una vuelta menos del
    // worker. Esto es reparto de trabajo del anfitrion, nada mas.
    if(!rdpQueue.empty() && rdpQueue.back().end == current && rdpQueue.back().xbus == xbus
       && rdpQueue.back().gen == rdpGen) {
      rdpQueue.back().end = end;
      if(sync) { rdpQueue.back().sync = true; rdpQueue.back().syncAt = syncAt; }
      if(g_dpSubProf) rdpCoal.fetch_add(1, std::memory_order_relaxed);
    }
    else {
      rdpQueue.push_back({current, end, xbus, rdpGen,
                          tlDpLogApply ? tlDpLogAt : tlIsRspThread ? rspGuestNow() : cartNow(),
                          sync, syncAt});
      // Un lector vivo mas para esta generacion de la sombra: no se puede reutilizar hasta
      // que el worker termine de LEER sus comandos. El tramo unido (rama de arriba) no
      // cuenta: alarga un trabajo ya contado.
      if(!xbus) rdpGenBusy[rdpGen].fetch_add(1, std::memory_order_release);
      // Hay trabajo por pintar: la CPU queda acotada por dpBarrierAt(), que es el instante en
      // que el motor termina TODO lo mandado.
      if(dpBarrierOn() && rcpMode == RcpMode::Threaded)
        rcpPend.fetch_or(4u, std::memory_order_release);
      dpPending.fetch_add(1, std::memory_order_release);
    }
    rdpBusy.store(true, std::memory_order_relaxed);
    // Si el worker no esta dormido en el condvar, volvera a coger este mutex al terminar el
    // trabajo en curso y vera la cola llena: la notificacion sobra. Con esperador, notify_all
    // es una llamada al kernel, y a 38.000 DPC_END/s eso era el grueso del hilo del RSP.
    wake = rdpWaiting;
  }
  // notify_ALL, no _one: en este condvar esperan DOS clases de hilo con predicados
  // distintos (el worker, "hay trabajo"; el drenador de la CPU, "cola vacia"). notify_one
  // puede despertar al drenador, cuyo predicado sigue falso, y el worker se queda dormido
  // con trabajo encolado -- wakeup perdido: la CPU espera un BREAK que nunca llega.
  if(wake) {
    const auto tW = g_dpSubProf ? std::chrono::steady_clock::now()
                                : std::chrono::steady_clock::time_point{};
    rdpCv.notify_all();
    if(g_dpSubProf) {
      rdpWakes.fetch_add(1, std::memory_order_relaxed);
      rdpWakeNs.fetch_add((u64)std::chrono::duration_cast<std::chrono::nanoseconds>(
          std::chrono::steady_clock::now() - tW).count(), std::memory_order_relaxed);
    }
  }
  // KESTREL_SYNCRDP=1: bisecar la corrupcion de RDRAM. Deja el RSP en su hilo pero
  // obliga a la CPU a esperar a que el RDP consuma el FIFO antes de seguir, o sea el
  // RDP pasa a ser efectivamente lockstep. No es fiel al hardware; solo diagnostico.
  static const bool syncRdp = std::getenv("KESTREL_SYNCRDP") != nullptr;
  if(syncRdp) rdpDrain();
}

static inline void spinPause();

// Vueltas entre PAUSE en los giros de espera del ANFITRION (citas del RSP, worker del RDP).
// Ninguno de esos giros es tiempo de invitado: son hilos esperando a que otro llegue a un
// instante. Lo que si cuesta es que, en un nucleo con SMT, un hilo girando sin PAUSE se lleva
// la mitad de los recursos de emision del nucleo, y el hermano suele ser el hilo de CPU, que
// es el palo largo. Perfil de anfitrion (Perfect Dark PAL, 2026-09-23): spReadSync 60,9 % del
// hilo del RSP, hilo de CPU al 100 %, hilo del RDP 47 % de nucleo estando ocupado el 9 %.
// PAUSE cede esos recursos sin soltar el nucleo ni pagar una llamada al kernel, asi que es
// gratis en tiempo de invitado por construccion: no cambia ni una fecha.
// Medido (Perfect Dark PAL, 89 cuadros, media de 3): 15 -> 4818 ms, 3 -> 4521, 0 -> 4489.
// Monotono y con los contadores de invitado identicos (1082 intercambios / 2924 campos), que es
// lo esperado: el cambio solo toca como espera el anfitrion. Se queda en 0, PAUSE cada vuelta.
// KESTREL_RDVPAUSE=<n>, mascara (potencia de dos menos uno); 15 es el comportamiento viejo.
static const u32 kSpinPauseMask = [] {
  const char* e = std::getenv("KESTREL_RDVPAUSE");
  if(e && *e) { char* end = nullptr; long n = std::strtol(e, &end, 0);
                if(end && !*end && n >= 0 && n < 65536) return (u32)n; }
  return 0u;
}();


// Vueltas que gira el worker del RDP, ocioso, antes de dormirse en el condvar. Con el motor
// en la GPU cada tramo se despacha en microsegundos y el worker se duerme entre DPC_END y
// DPC_END: el siguiente productor (sobre todo el HILO DEL RSP, a decenas de miles de DPC_END
// por segundo) encuentra rdpWaiting=true y paga notify_all = llamada al kernel, mas el
// despertar del worker. Perfil del hilo del RSP 2026-09-11: 10,6 % fuera de imagen desde
// rdpSubmit. Girar un poco sobre dpPending (lectura atomica, sin mutex) coge el tramo
// siguiente sin que nadie llame al kernel. Solo coste de anfitrion: el horario del tramo ya
// esta fechado por quien lo lanzo (dpScheduleSpan), el invitado no ve cuando se pinta.
// KESTREL_RDPSPIN=<n> (0 = dormir directamente). Medido 2026-09-16, bench SM64 200
// intercambios, minimo de 5 en dos rondas intercaladas:
//   prdp-jit      0 -> 3,98/3,98 s   4096 -> 3,53/3,55   16384 -> 3,54/3,48
//                 32768 -> 3,45/3,46   131072 -> 3,49/3,47   1 M -> 3,48/3,48
//   threaded-jit  0 -> 4,93/4,93 s   32768 -> 4,64/4,62   (SoftRDP)
// Meseta desde 32768: -13 % de pared con Parallel-RDP y -6 % con SoftRDP.
// RE-BARRIDO 2026-09-18, ya con los diarios (dpLog/spLog/dmaLog) y dpBarSync puestos: la meseta
// sigue empezando en 32768 (8192 se hunde en los cuatro juegos) pero ya no es plana por arriba.
// Min de 5 rondas intercaladas, Parallel-RDP, ms de pared, DOS tandas independientes:
//   tanda 1   jr 7251 -> 7185, PD 11569 -> 11475, SM64 7483 -> 7516, DK64 11770 -> 11701
//   tanda 2   jr 7266 -> 7105, PD 11501 -> 11475, SM64 7496 -> 7466, DK64 11761 -> 11716
// 131072 gana los cuatro en la tanda 2 -- en jr (-2,2 %) y SM64 las cinco lecturas de cada lado
// son DISJUNTAS -- y gana tres de cuatro en la tanda 1. 524288 ya cobra +1,4 % en jr a cambio de
// rascar en SM64/DK64, asi que el sitio es 131072. Por que se movio: con los diarios el RSP
// archiva el tramo y sigue en vez de citarse, asi que los tramos llegan mas seguidos y mas
// pequenos, y el hueco entre dos ya no cabe en 32768 vueltas. Solo coste de anfitrion: el
// horario del tramo lo fecha quien lo lanza (dpScheduleSpan).
static auto rdpSpinLen() -> u32 {
  static const u32 v = []() -> u32 {
    const char* e = std::getenv("KESTREL_RDPSPIN");
    if(e && *e) { char* end = nullptr; long n = std::strtol(e, &end, 0);
                  if(end && !*end && n >= 0 && n <= 10'000'000) return (u32)n; }
    return 131072u;    // era 32768u
  }();
  return v;
}

auto Memory::rdpWorkerLoop() -> void {
  pinToPhysicalCore(1);
  hostprof::start("rdp");   // opt-in: KESTREL_HOSTPROF_WHO=rdp
  rdpThreadH = selfThreadHandle();
  // Levantar parallel-rdp AQUI, antes de esperar el primer trabajo. Traer arriba
  // Vulkan (device, colas, banco SPIR-V, importar los 8 MB de RDRAM) cuesta cientos
  // de milisegundos, y si se hace de forma perezosa dentro del primer job ese coste
  // sale del reloj del RDP: la CPU ya escribio DPC_END y esta mirando DPC_STATUS.
  // systemtest lo caza -- los cuatro casos "RDP STATUS" agotan su espera y leen 0xa8
  // (START_GCLK|PIPE_BUSY todavia puestos) porque el trabajo aun no ha empezado.
  // El hilo del RDP existe desde el arranque y no tiene nada que hacer hasta el
  // primer comando, asi que el arranque se solapa con el boot de la CPU. Va en este
  // hilo, no en el que llama a startRcpThreads(), para que todo el uso de Vulkan
  // siga ocurriendo en el mismo hilo que antes.
  vrdpBringUp();
  // Barrera solo en SYNC_FULL (ver dpBarrierAt): solo con Parallel-RDP vivo y pintando en su
  // worker. Se decide aqui, antes del primer trabajo; hasta entonces vale la barrera entera.
  {
    static const bool syncOnly = [] {
      const char* e = std::getenv("KESTREL_DPBARSYNC");
      return !e || !*e || (std::strcmp(e, "0") && std::strcmp(e, "off"));
    }();
    if(syncOnly && vrdp::active() && !rdpInlineDiag()) {
      std::lock_guard<std::mutex> lk(rdpMx);
      dpBarSyncOnly.store(true, std::memory_order_release);
    }
  }
  for(;;) {
    RdpJob job;
    // dpPending cuenta tramos encolados + el que se esta pintando, y el worker ya descuento el
    // suyo: aqui arriba > 0 es exactamente "cola no vacia". Sin wakeup perdido: si el tramo
    // llega despues del giro, o el productor ve rdpWaiting (lo lee con el mutex) o el
    // predicado de abajo ve la cola llena.
    bool spun = false;
    for(u32 k = 0, n = rdpSpinLen(); k < n; k++) {
      if(dpPending.load(std::memory_order_acquire)) { spun = true; break; }
      if((k & kSpinPauseMask) == kSpinPauseMask) spinPause();
    }
    // Sin trabajo tras el giro: el hilo se va a dormir. Lo acumulado en el renderer sale a la
    // GPU (ver vrdp::idle). Fuera de rdpMx: puede costar un submit de Vulkan.
    if(!spun && !rdpInlineDiag() && !dpPending.load(std::memory_order_acquire)) vrdp::idle();
    {
      std::unique_lock<std::mutex> lk(rdpMx);
      // rdpWaiting le dice al productor si hace falta despertarnos (ver rdpSubmit).
      // Se pone y se quita con el mutex cogido, que es el mismo con el que el
      // productor lo lee: no hay ventana para un wakeup perdido.
      rdpWaiting = true;
      rdpCv.wait(lk, [&]{ return rdpStop || !rdpQueue.empty(); });
      rdpWaiting = false;
      if(rdpStop && rdpQueue.empty()) return;
      // El horario de este tramo ya esta hecho desde que se lanzo (dpScheduleSpan): el worker
      // no toca ni una fecha, solo pinta.
      job = rdpQueue.front(); rdpQueue.pop_front();
    }
    auto t0 = std::chrono::steady_clock::now();
    // Los comandos salen de la copia de SU generacion; en xbus no hay copia (DMEM).
    const u8* src = (!job.xbus && !rdpShadow[job.gen].empty()) ? rdpShadow[job.gen].data()
                                                              : rdram.data();
    dpJobOps = job.ops;   // instante de invitado del lanzamiento, para el plazo de fin
    rdpRunJob(job.current, job.end, job.xbus, src, rdpCostOn());
    rdpBusyNs.fetch_add((u64)std::chrono::duration_cast<std::chrono::nanoseconds>(
                          std::chrono::steady_clock::now() - t0).count(), std::memory_order_relaxed);
    rdpJobsRun.fetch_add(1, std::memory_order_relaxed);
    ev("dp.done", job.end, (u32)rdpQueue.size());
    {
      std::lock_guard<std::mutex> lk(rdpMx);
      // El motor suelta el trabajo cuando ha terminado de LEER sus comandos, no antes: solo
      // aqui puede el productor reescribir ese buffer sin pisar al rasterizador.
      dpPending.fetch_sub(1, std::memory_order_release);
      if(!job.xbus) rdpGenBusy[job.gen].fetch_sub(1, std::memory_order_release);
      if(rdpQueue.empty()) rdpBusy.store(false, std::memory_order_relaxed);
      // dpCompSeq NO es tiempo de invitado: es cuantos trabajos lleva pintados el anfitrion.
      // Solo sirve para que los que esperan sepan que algo se ha movido de verdad.
      dpCompSeq.fetch_add(1, std::memory_order_release);
      if(job.sync) {
        while(!dpSyncEnds.empty() && dpSyncEnds.front() <= job.syncAt) dpSyncEnds.pop_front();
        dpSyncBarAt.store(dpSyncEnds.empty() ? ~0ull : dpSyncEnds.front(), std::memory_order_release);
      }
      if(rdpQueue.empty()) {
        dpSyncEnds.clear();
        dpSyncBarAt.store(~0ull, std::memory_order_release);
        rcpPend.fetch_and(~4u, std::memory_order_release);
        dpBarWaivedAt = ~0ull;
        dpWrReset();   // motor drenado: lo pintado ya esta en RDRAM (ver rspDmaRdpWait)
      }
    }
    rdpCv.notify_all();   // wake any rdpDrain() waiter
  }
}

// Espera acotada CON PARTE. Ninguna de las esperas del hilo de CPU sobre los workers del
// RCP tenia tope: si un worker deja de publicar, el emulador se queda quieto para siempre y
// sin decir por que -- la ventana sigue viva y el juego no avanza, que desde fuera parece
// lentitud y no un cuelgue. Dejar de esperar NO es una opcion: seguir sin el resultado del
// RSP o del RDP corrompe el estado del invitado, y el hardware tampoco "se cansa". Asi que
// el tope no rompe la espera, la PARTE: cada KESTREL_RCPWAIT ms (2000 por defecto, =0 vuelve
// a la espera muda de siempre) se imprime el estado de los dos dominios y se sigue esperando.
static auto rcpWaitReportMs() -> unsigned {
  static const unsigned ms = []{
    const char* e = std::getenv("KESTREL_RCPWAIT");
    return e ? (unsigned)std::strtoul(e, nullptr, 10) : 2000u;
  }();
  return ms;
}

template<typename Pred, typename Report>
static auto awaitReporting(std::unique_lock<std::mutex>& lk, std::condition_variable& cv,
                           Pred pred, Report report) -> void {
  const unsigned ms = rcpWaitReportMs();
  if(!ms) { cv.wait(lk, pred); return; }
  const auto period = std::chrono::milliseconds(ms);
  for(unsigned round = 1; !cv.wait_for(lk, period, pred); round++) report(round);
}

auto Memory::rdpDrain() -> void { RcpWaitMark rwm_{cpuRcpWait};
  if(rcpMode != RcpMode::Threaded) return;
  auto t0 = std::chrono::steady_clock::now();
  { std::unique_lock<std::mutex> lk(rdpMx);
    awaitReporting(lk, rdpCv,
      [&]{ return rdpQueue.empty() && !rdpBusy.load(std::memory_order_relaxed); },
      [&](unsigned round){
        std::fprintf(stderr, "[rcp] llevo %u x %u ms esperando al RDP: cola=%zu ocupado=%d "
                             "dpc start=%08x cur=%08x end=%08x status=%08x\n",
                     round, rcpWaitReportMs(), rdpQueue.size(),
                     (int)rdpBusy.load(std::memory_order_relaxed), rcp.dpc_start,
                     rcp.dpc_current.load(std::memory_order_relaxed), rcp.dpc_end,
                     rcp.dpc_status.load(std::memory_order_relaxed));
      }); }
  cpuWaitNs.fetch_add((u64)std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - t0).count(), std::memory_order_relaxed);
}

auto Memory::reportRspStall(unsigned round, const char* esperando) -> void {
  // El OSTask vive en DMEM 0xFC0 y su primera palabra es el tipo (1 = grafico, 2 = audio):
  // saber cual de los dos microcodigos es el que no vuelve suele bastar para el diagnostico.
  u32 taskType = 0;
  if(dmem.size() >= DMEM_SIZE) taskType = (u32)dmem[0xFC0] << 24 | (u32)dmem[0xFC1] << 16 |
                                          (u32)dmem[0xFC2] << 8  | (u32)dmem[0xFC3];
  std::fprintf(stderr, "[rcp] llevo %u x %u ms esperando a %s del RSP: ocupado=%d kick=%d "
                       "corriendo=%d sp_status=%08x sp_pc=%03x pc=%03x ciclos=%llu tarea=%u\n",
               round, rcpWaitReportMs(), esperando,
               (int)rspBusy.load(std::memory_order_relaxed), (int)rspKick, (int)rsp.running,
               rcp.sp_status.load(std::memory_order_relaxed), rcp.sp_pc, rsp.pc,
               (unsigned long long)rsp.cyclesRun.load(std::memory_order_relaxed), taskType);
}

auto Memory::rspAwaitIdle() -> void { RcpWaitMark rwm_{cpuRcpWait};
  if(rcpMode != RcpMode::Threaded) return;
  auto t0 = std::chrono::steady_clock::now();
  { std::unique_lock<std::mutex> lk(rspMx);
    rspWaiters.fetch_add(1);
    awaitReporting(lk, rspCv, [&]{ return !rspBusy.load(std::memory_order_acquire); },
                   [&](unsigned round){ reportRspStall(round, "que termine la tarea"); });
    rspWaiters.fetch_sub(1); }
  cpuWaitNs.fetch_add((u64)std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now() - t0).count(), std::memory_order_relaxed);
}

// Regulador de velocidad CPU<->RSP para el modo Threaded.
//
// En Lockstep el bucle del sistema intercala rsp.step() a 2 pasos de RSP por cada 3 de CPU
// (62.5 MHz contra 93.75 MHz), asi que la CPU emulada NO puede adelantar al RSP. En Threaded
// el worker corre la tarea entera por su cuenta y NADA acopla los dos relojes: la CPU emulada
// se va millones de instrucciones por delante y todas se gastan girando en el bucle de espera
// del juego, que ademas martillea los registros MMIO que el worker escribe (ping-pong de linea
// de cache entre nucleos). Aqui se restaura el mismo acoplamiento que en Lockstep: mientras
// haya tarea de RSP en vuelo, la CPU no puede haber avanzado mas de paceCpuNum/paceCpuDen
// instrucciones por cada ciclo de RSP consumido desde el enganche (3/4 con los relojes de
// serie: la CPU retira una instruccion cada dos ciclos de 93.75 MHz y el RSP una por ciclo
// de 62.5 MHz). La medida del lado CPU es guestOps() -- retiradas MAS las paradas de
// cache ya convertidas a instrucciones-equivalentes -- y no retired a secas: durante los
// ~60 ciclos que cuesta una falta de cache el RSP sigue corriendo, asi que contar solo
// retiradas frenaba la CPU de mas y ademas discrepaba del interleave de Lockstep, que
// mide en ese mismo reloj. Con KESTREL_CACHECOST apagado los dos valores coinciden.
// de 62.5 MHz). Era 3/2 -- el doble -- por comparar los dos relojes en vez de comparar
// instrucciones retiradas; el ratio sale ahora de Clocks, igual que el de Lockstep. Si se pasa DUERME en el condvar del RSP en vez
// de girar: mismo trabajo util del guest, un nucleo del host libre para los workers, y el
// adelanto entre dominios acotado como en el hardware.
//
// La holgura existe por la granularidad del dynarec: una cadena de bloques enlazados retira
// hasta jit::kGuardMaxOps sin volver al bucle, asi que por debajo de eso el regulador no puede
// mandar y solo generaria bloqueos inutiles.
// Con la holgura corta el freno entra tantas veces por campo que la CPU puede pasar mas tiempo
// en el condvar que emulando, y el RSP quedarse sin trabajo encolado por delante; con la holgura
// larga el adelanto entre dominios crece hasta que el hilo de CPU se come el nucleo que el worker
// necesita. El tope real lo sigue poniendo kPaceMaxWait, que corta cualquier episodio.
//
// El optimo se MUEVE con la velocidad del hilo de CPU, asi que hay que recalibrarlo cuando el
// interprete/JIT se acelera. Primera calibracion (SM64, 400 campos, i7-870), cuando el hilo de
// CPU rendia ~29 fps: 8 K -> 22.0, 128 K -> 26.1, 512 K -> 29.0, 2 M -> 26.7 -> optimo 512 K.
// Recalibrado 2026-08-26 con el hilo de CPU ya en ~47 fps: 16 K -> 38.2, 64 K -> 41.0,
// 128 K -> 43.6, 192 K -> 46.9, 256 K -> 47.8, 320 K -> 44.2, 384 K -> 41.7, 512 K -> 36.0.
// El pico se habia desplazado de 512 K a 256 K y el viejo default habia quedado justo en el
// borde del despenadero: acelerar la CPU un 3% costaba 11 fps porque el adelanto acumulado
// entre frenadas crecia con ella. Sintoma reconocible: "N64 speed: CPU" se dispara (aqui
// 202% -> 451%) mientras el RSP se queda igual, es decir la CPU gasta lo ganado girando.
// Recalibrado 2026-09-01, despues de acelerar otra vez el hilo de CPU (fetch fast-path,
// MUL.S sin MXCSR, barajados del RSP en linea). AVISO DE METRICA: el "% de tiempo real" NO
// sirve para calibrar esto, porque el numerador son CAMPOS VI emitidos y aflojar el freno
// los infla sin hacer mas trabajo (el guest quema ciclos emulados girando a la espera del
// RCP: es el mismo artefacto que documenta docs/PERF-CPU.md en "El regulador no se toca").
// La medida buena es la PARED por un numero fijo de intercambios de buffer, que son los
// fotogramas de juego que ve el jugador. SM64, 300 intercambios, min de 3 pasadas:
//   prdp-jit:   256 K -> 4.40s (68.2 int/s)   384 K -> 4.09s (73.3)   512 K -> 3.89s (77.1)
//               768 K -> 3.85s (77.9)         1 M -> 3.80s (78.9)
//   threaded-jit (SoftRDP): plano dentro del ruido, 8.06s / 8.03s / 7.93s / 7.86s para
//               256 K / 512 K / 1 M / 2 M (400 intercambios: 10.61 / 10.55 / 10.62 s).
// O sea el codo esta en 512 K y la ganancia real es de parallel-rdp (+16% de fotogramas de
// juego por segundo), no de SoftRDP. El despenadero de 512 K de la calibracion anterior ha
// desaparecido: entonces el hilo de CPU se comia el nucleo del worker, y desde que rdpSubmit
// dejo de despertar al RDP 38 K veces por segundo (docs/PERF-RCP-SYNC.md) ya no compite.
// Segundo eje, la fidelidad: campos VI por intercambio, con lockstep como oraculo (4.09,
// deterministico y con md5 identico). prdp-jit da 2.73 / 2.80 / 3.12 / 3.20 / 3.24 en la
// misma serie, o sea todos por DEBAJO del oraculo y la holgura larga es la que mas se le
// acerca. No habia canje entonces: 1 M era a la vez lo mas rapido y lo mas parecido al oraculo.
// (SoftRDP en hilos se va al otro lado, ~9-10 campos por intercambio, pero esa desviacion
// es del backend, no del regulador: apenas se mueve con la holgura.)
//
// SUPERADO 2026-09-03, y esta vez el motivo no es de rendimiento sino de correccion. Con la
// holgura de 1 M el hilo de CPU corre al ~1200% de la velocidad del N64 respecto a un RSP en
// marcha, o sea deja a la CPU emulada ~11 ms -- dos tercios de campo -- por delante de una
// tarea de RSP en vuelo. Eso el hardware no lo permite: en el N64 los dos relojes van
// rigidamente acoplados por el bus y la CPU no puede adelantar al RSP mas que unos ciclos.
// Perfect Dark lo notaba: descarrilaba en ~1 de cada 8 arranques y acababa girando en un hilo
// con IE=0 y CU1=0 tomando una excepcion de coprocesador por vuelta, con las VI apilandose sin
// atender (visto con KESTREL_WATCHDOG: retired subiendo, todo el RCP parado, mi_intr con VI
// pendiente y el contador de clear congelado). Con 4096 son 40 arranques limpios de 40.
// El unico valor que justifica el EMULADOR es la granularidad del dynarec: una cadena de
// bloques enlazados retira hasta jit::kGuardMaxOps instrucciones sin volver al bucle del
// sistema, asi que por debajo de eso el freno no puede mandar y solo generaria bloqueos
// inutiles. Por encima, la holgura la tendria que justificar el hardware, y no la justifica.
// Coste medido (SM64, 300 intercambios, min de 3, i7-870), 4096 vs 1 M:
//   prdp-jit:      5.08s vs 5.06s  (ruido; 3.32 vs 3.31 campos VI por intercambio)
//   threaded-jit:  5.18s vs 5.06s  (-2.4%)
// El despenadero de las calibraciones anteriores ha desaparecido con el enlace de bloques
// sobre codigo TLB-mapeado: la curva es plana de 4 K a 1 M. Default = jit::kGuardMaxOps.
// 4096 == jit::kGuardMaxOps. No se incluye jit.hpp aqui a proposito: el core no debe depender
// del backend de CPU.
//
// MATIZ 2026-09-18, y no deshace lo de arriba: lo que el hardware no justifica es el ADELANTO
// EN TIEMPO DE INVITADO, y desde que existen las barreras de invitado ese adelanto ya no lo
// acota el regulador. La barrera del SP se arma en CADA lanzamiento de tarea (ver rspKick) y
// spBarrierWait clava el reloj del invitado en spBarrierAt() -- el instante al que el RSP ha
// trabajado de verdad -- en cada retiro, holgura aparte; la del RDP hace lo propio con el
// motor. Con las dos puestas, la holgura del regulador ya no decide CUANTO se adelanta la CPU
// emulada, solo cada cuanto interviene el freno del ANFITRION antes de que mande la barrera.
// Ahi si es una perilla de rendimiento, y una corta cuesta: el freno entra tantas veces por
// campo que la CPU pasa mas tiempo en el condvar que emulando.
// Barrido intercalado, min de 5, Parallel-RDP, ms de pared, DOS tandas independientes
// (4096 -> 65536):
//   tanda 1   jr 7195 -> 7051, PD 11423 -> 11353, SM64 7459 -> 7425, DK64 11741 -> 11674
//   tanda 2   jr 7205 -> 7057, PD 11445 -> 11406, SM64 7485 -> 7354, DK64 11774 -> 11604
// Ocho de ocho a favor y md5 de framebuffer identico en las 30 corridas. El liston de esto NO
// es la pared, es el mismo con que se fijo el 4096: arranques limpios de Perfect Dark. 30 de 30
// con 65536 (mas los 15 de control con 4096), todos con md5 0f0adee7 y sin descarrilar.
// 1 M sigue DESCARTADO: es el orden de magnitud que descarrilaba, y aunque hoy midiera bien no
// hay nada que lo justifique.
// Cuando las barreras NO estan (KESTREL_SPBARRIER=0, KESTREL_DPBARRIER=0, KESTREL_RCPDEADLINE=0,
// que son escotillas de depuracion) el regulador vuelve a ser el unico freno de invitado y la
// holgura vuelve a 4096, que es lo unico que justifica el emulador. Ver paceSlack().
static const bool kPaceSlackSet = std::getenv("KESTREL_PACESLACK")
                               && *std::getenv("KESTREL_PACESLACK");
static const u64 kPaceSlack = kPaceSlackSet
                            ? std::strtoull(std::getenv("KESTREL_PACESLACK"), nullptr, 0) : 4096;
// Holgura cuando las barreras de invitado son la autoridad. KESTREL_PACESLACK, si esta puesta,
// manda sobre las dos: una perilla, un valor.
static const u64 kPaceSlackBar = kPaceSlackSet ? kPaceSlack : 65536;
static constexpr u64 kPaceMaxWait = 20'000'000ull;    // ns; salvavidas por episodio
// Tope de las barreras de invitado (RDP y RSP). No es el mismo caso que kPaceMaxWait: el
// regulador espera a que un contador AVANCE, cosa que pasa cada pocos microsegundos,
// mientras que una barrera espera a que un trabajo entero CIERRE. Un cuadro de DK64
// rasterizado por software pasa de 20 ms de tiempo de PARED con facilidad, asi que el
// tope viejo saltaba 32 veces en 300 campos -- y cada salto suelta la barrera, o sea que
// suelta el determinismo. Un segundo sigue siendo un salvavidas contra un worker muerto,
// pero ya no se cruza en el camino de un trabajo legitimamente largo.
static constexpr u64 kBarrierMaxWait = 1'000'000'000ull;   // ns

// Pista de pausa para los bucles de espera activa. La CPU y el RSP se vigilan leyendo
// contadores que escribe el otro (`rcpPend`, `spBarrierAt`, `cartNow`), y sin pista cada
// vuelta del bucle pide la linea en exclusiva: la coherencia le quita al otro hilo justo el
// ciclo con el que iba a soltarla, y en SMT le roba ademas las ranuras de emision. PAUSE dice
// "esto es una espera" (Nehalem ~9 ciclos) y ademas evita la penalizacion de salida del bucle
// por especulacion de orden de memoria. No cambia NADA observable: solo es una pista de
// anfitrion, el estado de invitado sale identico con ella y sin ella.
// KESTREL_SPINPAUSE=0 la quita, para medir.
static const bool g_spinPauseOn = [] {
  const char* e = std::getenv("KESTREL_SPINPAUSE");
  return !e || !*e || (std::strcmp(e, "0") && std::strcmp(e, "off"));
}();
// Cadencia de una de cada 16 vueltas en quien llama: PAUSE no cuesta lo mismo en todos los
// anfitriones (Nehalem ~9 ciclos, Skylake y posteriores ~140), y una por vuelta estiraria el
// giro de 2048 vueltas dos ordenes de magnitud en una maquina moderna.
static inline void spinPause() { if(g_spinPauseOn) _mm_pause(); }

// Vueltas de espera activa de la barrera del SP antes de dormir. KESTREL_BARSPIN lo ajusta
// (0 = dormir directamente). Tambien es solo coste de anfitrion.
// 2026-09-16 se midio PLANO de 2048 a 1 M y se dejo en 2048. RE-BARRIDO 2026-09-18, ya con los
// diarios (dpLog/spLog/dmaLog) y dpBarSync puestos: ya no es plano. Min de 5 rondas
// intercaladas, Parallel-RDP, ms de pared, 2048 -> 16384:
//   jr 7286 -> 7261, PD 11579 -> 11551, DK64 11810 -> 11802, SM64 7599 -> 7474 (-1,6 %).
// En SM64 las cinco lecturas de cada lado casi no se solapan ({7474,7490,7577,7579,7613} contra
// {7599,7599,7646,7659,7808}); en los otros tres es empate. 131072 ya cobra +1,9 % en jr, asi que
// el optimo esta en 16384 y no mas arriba. Por que se movio: con los diarios la barrera se abre
// mucho antes -- el RSP apunta y sigue en vez de citarse -- asi que la espera que antes tocaba
// dormir ahora cabe dentro del giro, y dormir cuesta un viaje al kernel que el giro se ahorra.
static auto barSpinLen() -> u32 {
  static const u32 v = []() -> u32 {
    const char* e = std::getenv("KESTREL_BARSPIN");
    if(e && *e) { char* end = nullptr; long n = std::strtol(e, &end, 0);
                  if(end && !*end && n >= 0 && n <= 1'000'000) return (u32)n; }
    return 16384u;
  }();
  return v;
}
// Grano del permiso del regulador (ver paceAllowance). Calibrable como la holgura.
static const u64 kPaceGrain = std::getenv("KESTREL_PACEGRAIN")
                            ? std::strtoull(std::getenv("KESTREL_PACEGRAIN"), nullptr, 0) : 1024;

// Freno de los DOS dominios del RCP. El permiso que vuelve es el mas corto de los dos: la
// CPU no puede adelantar ni al RSP ni al RDP mas de lo que el hardware permite.
// Holgura efectiva del regulador. Con las barreras de invitado puestas son ELLAS las que
// acotan el adelanto de la CPU emulada, asi que el regulador puede intervenir menos veces;
// sin ellas, el regulador es el unico freno y la holgura es la del dynarec (kGuardMaxOps).
auto Memory::paceSlack() -> u64 {
  return (rcpMode == RcpMode::Threaded && spBarrierOn() && dpBarrierOn() && rcpDeadlineOn())
       ? kPaceSlackBar : kPaceSlack;
}

auto Memory::rcpPace(u64 cpuOps) -> u32 {
  u32 a = rspPace(cpuOps);
  u32 b = rdpPace(cpuOps);
  return a < b ? a : b;
}

auto Memory::rspPace(u64 cpuOps) -> u32 { LazyWaitMark rwm_;
  if(!rspBusy.load(std::memory_order_acquire)) { pacePrimed = false; return 0xFFFF'FFFFu; }
  const u64 slack = paceSlack();
  u64 rspNow = rsp.cyclesRun.load(std::memory_order_relaxed);
  if(!pacePrimed) {
    pacePrimed = true; paceGiveUp = false; paceWaitedNs = 0;
    paceCpu0 = cpuOps; paceRsp0 = rspNow;
    paceEpisodes.fetch_add(1, std::memory_order_relaxed);
    return paceGrant(0, slack);
  }
  if(paceGiveUp) return 0xFFFF'FFFFu;
  for(;;) {
    u64 ahead = cpuOps - paceCpu0;
    u64 allow = paceDivDen.div((rspNow - paceRsp0) * paceCpuNum) + slack;
    if(ahead <= allow) return paceGrant(ahead, allow);
    // El freno ata la CPU al avance en CICLOS del RSP, y eso presupone que el RSP esta
    // trabajando. Cuando esta parado en una cita de lectura del FIFO (dpReadSync) el que
    // tiene que avanzar es justo el otro: el microcodigo espera a que la CPU instale el
    // siguiente buffer de comandos. Frenarla ahi es frenar al que tiene la pelota, y los dos
    // hilos se quedan mirandose hasta que salta el timeout del condvar. Quien acota a la CPU
    // en esa ventana es la barrera del SP con su adelanto (kRdvLead), no el regulador.
    if(rspRdvAt.load(std::memory_order_acquire)) return paceGrant(ahead, ahead + slack);
    // Igual con el RSP esperando a que la CPU aplique su diario DPC (ver dpLogWait).
    if(rspLogWait.load(std::memory_order_acquire)) return paceGrant(ahead, ahead + slack);
    // Y con el RSP en la cita exacta de spReadSync (sondeo de SP_STATUS, lecturas y escrituras
    // de DPC, DMA): espera a que la CPU llegue a su instante, y frenarla aqui dejaba a los dos
    // hilos parados hasta el timeout del condvar (bloqueo mutuo de latencia, no de logica).
    if(rspSyncWait.load(std::memory_order_acquire)) return paceGrant(ahead, ahead + slack);
    // Lo mismo, pero en grande: con el RSP aparcado (ver rspParkWait) el unico que puede
    // desatascar la escena es la CPU, y el freno la ata al avance de un reloj que por
    // definicion no avanza.
    if(rspPark.load(std::memory_order_acquire) &&
       !rspParkWake.load(std::memory_order_acquire))
      return paceGrant(ahead, ahead + slack);
    paceHolds.fetch_add(1, std::memory_order_relaxed);
    rwm_.arm(cpuRcpWait);
    auto t0 = std::chrono::steady_clock::now();
    {
      // El worker publica ciclos y notifica cada pocos miles de instrucciones (ver
      // Rsp::step), asi que esto despierta con el progreso real. El timeout solo cubre
      // la notificacion perdida — no se usa como muestreo.
      std::unique_lock<std::mutex> lk(rspMx);
      rspWaiters.fetch_add(1);
      rspCv.wait_for(lk, std::chrono::microseconds(500), [&]{
        return !rspBusy.load(std::memory_order_acquire)
            || rsp.cyclesRun.load(std::memory_order_relaxed) != rspNow
            || rspSyncWait.load(std::memory_order_acquire);
      });
      rspWaiters.fetch_sub(1);
    }
    u64 dt = (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now() - t0).count();
    paceBlockNs.fetch_add(dt, std::memory_order_relaxed);
    cpuWaitNs.fetch_add(dt, std::memory_order_relaxed);
    paceWaitedNs += dt;
    if(!rspBusy.load(std::memory_order_acquire)) { pacePrimed = false; return 0xFFFF'FFFFu; }
    u64 rspPrev = rspNow;
    rspNow = rsp.cyclesRun.load(std::memory_order_relaxed);
    // Salvavidas: si el RSP deja de avanzar (microcodigo esperando algo de la CPU, worker
    // que no arranca, reloj del host raro) se suelta el freno para este episodio. El
    // regulador es una optimizacion, nunca puede ser una via de bloqueo.
    //
    // El contador se reinicia CON CADA AVANCE del RSP: lo que no se tolera es un atasco,
    // no la espera acumulada. Una tarea de graficos larga hace esperar a la CPU mucho mas
    // de kPaceMaxWait en total y es exactamente lo que el regulador tiene que hacer; con
    // el contador acumulado el freno se soltaba a los 20 ms y la CPU volvia a correr al
    // 175% de la velocidad del N64 mientras el RSP iba al 61%.
    if(rspNow != rspPrev) { paceWaitedNs = 0; continue; }
    if(paceWaitedNs > kPaceMaxWait) { paceGiveUp = true; return 0xFFFF'FFFFu; }
  }
}

// Ops de CPU que el regulador permite todavia sin volver a frenar. El prologo del JIT
// consume este permiso en el camino rapido, asi que basta con volver al trampolin cuando se
// agota: la regulacion es la misma que la de rcpPace pero se paga una llamada cada `allow`
// ops en vez de una por bloque. Sin tarea de RSP en vuelo no hay limite.
//
// Toma `ahead`/`allow` YA calculados por rcpPace en vez de releer `rspBusy` y `rsp.cyclesRun`:
// son las dos lineas que el worker del RSP reescribe sin parar, y releerlas para el permiso
// justo despues de haberlas leido para el freno pagaba el fallo de cache compartida dos veces
// por vuelta al trampolin.
auto Memory::paceGrant(u64 ahead, u64 allow) -> u32 {
  u64 left   = (ahead >= allow) ? 0 : (allow - ahead);
  // Granularidad. Sin ella el permiso se encoge solo al acercarse al limite (100, 50, 20,
  // 9, 1 ...) y la cadena enlazada vuelve al trampolin cada pocas ops; cada vuelta paga dos
  // lecturas atomicas de lineas que el worker del RSP esta reescribiendo, que es un fallo de
  // cache compartida, no una lectura local. Redondear hacia arriba adelanta el freno como
  // mucho `grain` ops sobre una holgura de 256 K -- por debajo del ruido -- y convierte esa
  // cola en un solo paso. No cambia el estado del guest: el regulador solo decide CUANDO
  // duerme el hilo de CPU, y quien frena de verdad es rcpPace, que se llama igual.
  if(left < kPaceGrain) left = kPaceGrain;
  return left > 0xFFFF'FFFFull ? 0xFFFF'FFFFu : (u32)left;
}

// --- Freno del dominio RDP ---------------------------------------------------------
//
// El PROBLEMA que resuelve (medido 2026-09-09 en Donkey Kong 64, ver docs/GAPS.md): el
// worker del RDP tarda tiempo de PARED del anfitrion, y mientras tanto la CPU emulada seguia
// retirando instrucciones en el bucle de espera del juego. Como el reloj de video del
// invitado se deriva de las instrucciones retiradas, esa espera se cobraba como TIEMPO DEL
// JUEGO: con SoftRDP en hilos, DK64 pasaba de 29,2 a 18,0 fps de invitado y gastaba el doble
// de instrucciones para dibujar los mismos 3000 cuadros. Un juego que cuenta cuadros (la demo
// de apertura de DK64) se desincroniza por eso. Y la velocidad del JUEGO pasaba a depender de
// lo rapido que fuese el PC, cosa que el hardware no hace.
//
// La SEMANTICA que impone es la del bus del N64: la CPU y el RCP van acoplados. Mientras hay
// trabajo de RDP en vuelo, la CPU no puede haber avanzado mas de paceCpuNum/paceCpuDen
// instrucciones por cada GCLK que el RDP ha consumido de verdad. Es el mismo contrato que
// rspPace impone contra Rsp::cyclesRun, con dos diferencias:
//
//   - la medida de trabajo es rcp.rdpGclk, el reloj de coste del RDP (el MISMO modelo
//     calibrado contra hardware que alimenta DPC_CLOCK, ver SoftRdp::accountPixels), en una
//     copia monotona que el invitado no puede poner a cero;
//   - el RDP corre al reloj del RCP, 62,5 MHz, igual que el RSP, asi que el ratio es el mismo
//     y no hace falta uno nuevo.
//
// Frenar NO es lo mismo que esperar: el hilo de CPU duerme en tiempo de pared y NO retira
// instrucciones, o sea que el tiempo del invitado se para. El RDP sigue costando lo que dice
// su modelo de ciclos, ni mas ni menos, corra el anfitrion lo que corra. Cuando el RDP va
// sobrado (parallel-RDP sobre GPU) el freno no llega a morder y el solape se conserva entero.
//
// Salvavidas identico al del RSP: si el contador de GCLK deja de avanzar mas de kPaceMaxWait
// sin que el trabajo termine, se suelta el freno de este episodio. El regulador es una
// cuestion de fidelidad, nunca puede ser una via de bloqueo.
auto Memory::rdpPace(u64 cpuOps) -> u32 { LazyWaitMark rwm_;
  if(!rdpBusy.load(std::memory_order_acquire)) { dpacePrimed = false; return 0xFFFF'FFFFu; }
  const u64 slack = paceSlack();
  u64 gclkNow = rcp.rdpGclk.load(std::memory_order_acquire);
  if(!dpacePrimed) {
    dpacePrimed = true; dpaceGiveUp = false; dpaceWaitedNs = 0;
    dpaceCpu0 = cpuOps; dpaceGclk0 = gclkNow;
    dpaceEpisodes.fetch_add(1, std::memory_order_relaxed);
    return paceGrant(0, slack);
  }
  if(dpaceGiveUp) return 0xFFFF'FFFFu;
  for(;;) {
    u64 ahead = cpuOps - dpaceCpu0;
    u64 allow = paceDivDen.div((gclkNow - dpaceGclk0) * paceCpuNum) + slack;
    if(ahead <= allow) return paceGrant(ahead, allow);
    dpaceHolds.fetch_add(1, std::memory_order_relaxed);
    rwm_.arm(cpuRcpWait);
    auto t0 = std::chrono::steady_clock::now();
    {
      // El worker solo avisa por rdpCv al TERMINAR el trabajo, no por cada pixel, asi que
      // aqui el plazo corto no es un lujo: es como se ve el avance de rdpGclk a mitad de un
      // tramo largo. Con el predicado puesto, el aviso del final sigue despertando al vuelo.
      std::unique_lock<std::mutex> lk(rdpMx);
      rdpCv.wait_for(lk, std::chrono::microseconds(200), [&]{
        return !rdpBusy.load(std::memory_order_acquire)
            || rcp.rdpGclk.load(std::memory_order_acquire) != gclkNow;
      });
    }
    u64 dt = (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now() - t0).count();
    dpaceBlockNs.fetch_add(dt, std::memory_order_relaxed);
    cpuWaitNs.fetch_add(dt, std::memory_order_relaxed);
    dpaceWaitedNs += dt;
    if(!rdpBusy.load(std::memory_order_acquire)) { dpacePrimed = false; return 0xFFFF'FFFFu; }
    u64 prev = gclkNow;
    gclkNow = rcp.rdpGclk.load(std::memory_order_acquire);
    if(gclkNow != prev) { dpaceWaitedNs = 0; continue; }
    if(dpaceWaitedNs > kPaceMaxWait) { dpaceGiveUp = true; return 0xFFFF'FFFFu; }
  }
}

auto Memory::dpBarrierOn() -> bool {
  static const bool v = [] {
    const char* e = std::getenv("KESTREL_DPBARRIER");
    return !e || !*e || (std::strcmp(e, "0") && std::strcmp(e, "off"));
  }();
  return v;
}

// Espera activa sobre el motor del RDP: true en cuanto se cumpla el predicado de las esperas
// de la barrera (motor sin trabajo, barrera nueva o un tramo mas pintado), false si se acaban
// las vueltas. KESTREL_DPSPIN=<n>, 0 = dormir directamente.
static auto dpSpinLen() -> u32 {
  static const u32 v = []() -> u32 {
    const char* e = std::getenv("KESTREL_DPSPIN");
    if(e && *e) { char* end = nullptr; long n = std::strtol(e, &end, 0);
                  if(end && !*end && n >= 0 && n <= 10'000'000) return (u32)n; }
    // Medido 2026-09-16 (min de 5, dos rondas): prdp-jit 0 3,48 / 2048 3,46 / 262144 3,42 /
    // 1 M 3,41 s; threaded-jit 0 4,63 / 2048 4,55 / 262144 4,51 / 1 M 4,50 s. Meseta desde
    // 262144: cubre el fence de SYNC_FULL de la GPU (~1,4 ms por cuadro) sin dormir.
    return 262144u;
  }();
  return v;
}

// Vueltas de sondeo apretado antes de espaciar la lectura del reloj de la CPU. Mirar
// `cartNow()` en cada vuelta no cuesta aqui, cuesta ALLI: `cartClock` es el contador de
// instrucciones retiradas y el hilo de CPU lo escribe sin parar, asi que cada lectura nuestra
// le pasa la linea a compartida y le obliga a volver a pedirla en exclusiva en su siguiente
// op. Y el hilo de CPU es el palo largo: en junkrunner64 el RSP se pasa el 57 % de su pared
// dentro de esta cita, o sea martilleando esa linea. Las esperas cortas se resuelven en las
// primeras vueltas, asi que se mira a pelo al principio y una de cada 16 despues.
// KESTREL_RDVTIGHT=<n> (0 = espaciado desde la primera vuelta, ~0 = comportamiento viejo).
static auto rdvTightTurns() -> u32 {
  static const u32 v = [] {
    const char* e = std::getenv("KESTREL_RDVTIGHT");
    if(e && *e) { char* end = nullptr; unsigned long n = std::strtoul(e, &end, 0);
                  if(end && !*end && n <= 1'000'000ul) return (u32)n; }
    return 0u;
  }();
  return v;
}

// Una de cada cuantas vueltas la cita CEDE EL NUCLEO (std::this_thread::yield). En Windows es
// SwitchToThread, o sea una llamada al kernel, y en esta maquina (4 nucleos / 8 hilos, tres
// hilos calientes) casi siempre no hay a quien cederselo: vuelve enseguida sin haber hecho
// nada. En el perfil del hilo del RSP del 2026-09-18 (Perfect Dark, 600 campos, Parallel-RDP
// + threaded-jit) el 20,3 % de las muestras cae FUERA de la imagen con `dpLogWait` de
// llamante, y ese es justo este yield: el microcodigo sondea DPC 1,81 M de veces por corrida
// y cada sondeo es una cita con la CPU.
// Ojo con subirlo mucho: detras del yield van tambien el aviso al hilo de CPU
// (`rspCv.notify_all`) y el salvavidas de pared (`rdvWaiveDue`), asi que espaciarlo retrasa
// las dos cosas. A 65536 vueltas de unos pocos ns la comprobacion sigue cayendo muy por
// debajo de los 20 ms del salvavidas.
// Con el yield espaciado a 65536 vueltas, lo que queda dentro de la vuelta de la cita son dos
// lecturas atomicas que NO son la condicion de salida: el aviso de vaciado del diario
// (`dpLogFlush`, que solo escribe la CPU al pararse en `System::quiesceRcp`) y la parada del
// anfitrion (`rspStop`/`hostStop`, que solo se escribe al cerrar). Ninguna de las dos cambia
// mas de un punado de veces por corrida, asi que mirarlas en CADA vuelta es sondear dos lineas
// que casi nunca se mueven. Con esto pasan a la MISMA cadencia que `ready()`/`cartNow()`
// (`KESTREL_RDVPOLL`, 1 de cada 64): el cierre se retrasa como mucho 63 vueltas de unos pocos
// ns, que no lo nota nadie.
// KESTREL_RDVCHEAP=0 vuelve a mirarlas en cada vuelta.
static auto rdvCheapTurn() -> bool {
  static const bool v = [] {
    const char* e = std::getenv("KESTREL_RDVCHEAP");
    return !(e && *e && (!std::strcmp(e, "0") || !std::strcmp(e, "off")));
  }();
  return v;
}

// Barrido intercalado, min de 4, Parallel-RDP, ms de pared, DOS tandas independientes
// (256 -> 65536): jr -0,51 / -0,47, PD -0,65 / +0,04, SM64 -1,00 / -0,60, DK64 -0,37 / -0,29.
// Siete de ocho lecturas a favor y md5 de framebuffer identico en las 4 x 7 corridas. La
// primera tanda ademas sale MONOTONA en los cuatro juegos (256 -> 4096 -> 65536), que es lo
// que separa esto del ruido. Por arriba se acaba: 262144 empata con 65536 (jr y SM64 un pelo
// mejor, DK64 un pelo peor) y 1 M ya es PEOR en PD (+0,46 %), justo como se esperaba de
// retrasar el aviso al hilo de CPU. Se queda 65536, el primer punto de la meseta, que es el
// que deja el aviso y el salvavidas mas despiertos.
// KESTREL_RDVYIELD=<n>, potencia de dos; 256 es el comportamiento viejo.
static auto rdvYieldMask() -> u32 {
  static const u32 v = [] {
    const char* e = std::getenv("KESTREL_RDVYIELD");
    if(e && *e) { char* end = nullptr; unsigned long n = std::strtoul(e, &end, 0);
                  if(end && !*end && n >= 1 && n <= 1'048'576ul && (n & (n - 1)) == 0) return (u32)n - 1u; }
    return 65535u;
  }();
  return v;
}

// Una de cada cuantas vueltas se mira el reloj de la CPU, pasadas las apretadas.
static auto rdvPollMask() -> u32 {
  static const u32 v = [] {
    const char* e = std::getenv("KESTREL_RDVPOLL");
    if(e && *e) { char* end = nullptr; unsigned long n = std::strtoul(e, &end, 0);
                  if(end && !*end && n >= 1 && n <= 65536ul && (n & (n - 1)) == 0) return (u32)n - 1u; }
    return 63u;
  }();
  return v;
}

auto Memory::dpSpinUntil(u64 bar, u64 comp) -> bool {
  for(u32 k = 0, lim = dpSpinLen(); k < lim; ++k) {
    if(!(rcpPend.load(std::memory_order_acquire) & 4u) || dpBarrierAt() != bar
       || dpCompSeq.load(std::memory_order_acquire) != comp) return true;
    if((k & kSpinPauseMask) == kSpinPauseMask) spinPause();
  }
  return false;
}

// Parar el reloj del invitado en la barrera. Dormir aqui NO cuesta tiempo de invitado: el
// hilo de CPU no retira instrucciones mientras espera, exactamente igual que en rdpPace.
auto Memory::dpBarrierWait(u64 now) -> void {
  u64 bar = dpBarrierAt();
  if(now < bar || bar == dpBarWaivedAt) return;
  // El instante de cierre del trabajo es FIJO desde que se publica, asi que ya no basta con
  // esperar a que "la barrera se mueva": no se va a mover. Lo que hay que esperar es a que el
  // trabajo CIERRE (o a que entre el siguiente, que trae barrera nueva). Sin esto la espera
  // agotaba sus 20 ms en cada trabajo largo y el salvavidas soltaba la barrera -- y con ella
  // se iba tambien el determinismo, porque a partir de ahi la CPU corria libre.
  u64 comp = dpCompSeq.load(std::memory_order_acquire);
  u64 waited = 0;
  // Mientras la CPU esta parada aqui la retiene el RDP, no el RSP: las citas del RSP
  // (spReadSync/dpReadSync) no deben soltarse por su salvavidas de pared. Ver cpuDpBarWait.
  cpuDpBarWait.store(true, std::memory_order_release);
  struct Clr { std::atomic<bool>& f; ~Clr() { f.store(false, std::memory_order_release); } } clr{cpuDpBarWait};
  for(;;) {
    auto t0 = std::chrono::steady_clock::now();
    // Giro corto antes de dormir, como en spBarrierWait. Con el motor en la GPU un tramo se
    // pinta en microsegundos, y dormir en rdpCv convierte cada uno en un viaje de ida y vuelta
    // por el planificador del anfitrion. Los tres atomicos que mira el predicado los sube el
    // worker sin avisar a nadie mas que al condvar: verlos cambiar es lo mismo que despertar.
    // Solo tiempo de anfitrion: el hilo de CPU no retira instrucciones mientras gira.
    if(!dpSpinUntil(bar, comp)) {
      std::unique_lock<std::mutex> lk(rdpMx);
      rdpCv.wait_for(lk, std::chrono::microseconds(200), [&]{
        return !(rcpPend.load(std::memory_order_acquire) & 4u) || dpBarrierAt() != bar
            || dpCompSeq.load(std::memory_order_acquire) != comp; });
    }
    u64 dt = (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now() - t0).count();
    cpuWaitNs.fetch_add(dt, std::memory_order_relaxed);
    dpBarBlockNs.fetch_add(dt, std::memory_order_relaxed);
    if(!(rcpPend.load(std::memory_order_acquire) & 4u)) return;
    u64 nextComp = dpCompSeq.load(std::memory_order_acquire);
    if(nextComp != comp) { comp = nextComp; waited = 0; }
    u64 next = dpBarrierAt();
    if(next != bar) { bar = next; waited = 0; if(now < bar) return; continue; }
    waited += dt;
    // Salvavidas, igual que el del regulador: si el worker deja de publicar, la barrera se
    // suelta para ESTE valor y se sigue. La fidelidad nunca puede ser una via de bloqueo.
    if(waited > kBarrierMaxWait) { dpBarWaivedAt = bar; dpWvBar.fetch_add(1, std::memory_order_relaxed); dpBarWaives.fetch_add(1, std::memory_order_relaxed); return; }
  }
}

// DPC_CURRENT tal y como lo ve alguien cuyo reloj de invitado va por `now`.
//
// El puntero de lectura del FIFO es lo que el microcodigo mira para saber cuanto buffer puede
// reutilizar, y hasta ahora lo publicaba el worker cuando terminaba el trabajo EN TIEMPO DE
// PARED: el RSP veia el FIFO libre antes o despues segun corriese el PC, gastaba un numero
// distinto de ciclos en su bucle de sondeo y la tarea entera acababa costando otra cosa.
// Medido en SM64: `Rsp::cyclesRun` bailaba un 0,8 % entre corridas y en una corrida lenta la
// partida entera divergia (24 campos de retraso en empezar a dibujar).
//
// En tiempo de invitado la regla es simple: mientras el RDP no haya terminado el trabajo, el
// puntero sigue donde lo abrio; cuando el reloj del lector pasa del instante de fin, ya vale
// lo que el motor dejo. Las dos cosas salen del horario del trabajo, que es todo tiempo de
// invitado, asi que dos corridas dan lo mismo.
// FECHAR UN TRAMO DE FIFO, EN EL HILO QUE LO LANZA.
//
// Aqui se decide todo lo que el invitado podra ver de este tramo: cuanto cuesta, cuando
// arranca el motor con el, cuando lo cierra y en que instante cae MI_DP si trae SYNC_FULL.
// Nada de eso mira al anfitrion, y por eso dos corridas del mismo binario dan lo mismo.
//
// Va con rdpMx cogido, asi que el modelo de coste (que escribe rcp.rdpGclk y los contadores
// de DPC) sigue teniendo un solo escritor a la vez aunque lo llamen la CPU y el RSP.
//
// El motor es UNO: un tramo no puede empezar antes de que drene lo anterior, de ahi el
// maximo contra dpSchedEnd. Y como el coste se cobra ANTES de pintar, el plazo de fin de
// tarea nace siempre vivo -- que es lo que antes se rompia y hacia que MI_DP se publicase
// donde el anfitrion hubiese llegado.
// Recarga de CURRENT sin tramo (RDP congelado): se fecha como un tramo vacio [addr, addr) de
// coste cero, para que DPC_CURRENT la vea en el mismo horario de invitado que el resto. No va a
// la cola del worker (no hay nada que pintar) ni cuenta como pendiente.
auto Memory::dpScheduleReload(u32 addr) -> void {
  const u64 kick = lockRspExec ? rspGuestNowAt(rsp.exactCycles())
                 : tlDpLogApply ? tlDpLogAt : tlIsRspThread ? rspGuestNow() : cartNow();
  std::lock_guard<std::mutex> lk(rdpMx);
  const u64 seq  = dpSubSeq.load(std::memory_order_relaxed);
  const u64 kg   = (tlIsRspThread || tlDpLogApply || lockRspExec) ? kick : kick + 1;
  u64 t = dpSchedEnd.load(std::memory_order_relaxed);
  if(kg > t) t = kg;
  dpLastKick = kick;
  dpJobAddr[seq & kDpRingM]    = addr;
  dpJobEndAddr[seq & kDpRingM] = addr;
  dpJobStartG[seq & kDpRingM]  = t;
  dpJobEndG[seq & kDpRingM]    = t;
  dpJobKickG[seq & kDpRingM]   = kg;
  dpSchedEnd.store(t, std::memory_order_release);
  dpSubSeq.store(seq + 1, std::memory_order_release);
  dpcEpoch.fetch_add(1, std::memory_order_release);
  if(rspPark.load(std::memory_order_acquire)) {
    u64 exp = 0;
    if(rspParkWake.compare_exchange_strong(exp, kick, std::memory_order_acq_rel)) {
      { std::lock_guard<std::mutex> g(parkMx); }
      parkCv.notify_all();
    }
  }
}

auto Memory::dpScheduleSpan(u32 current, u32 end, bool xbus, const u8* src, u64 kick) -> void {
  const u64 gclk0 = rcp.rdpGclk.load(std::memory_order_relaxed);
  u32 costCur = current;
  if(rdpCostHasResume && current == rdpCostLastEnd) costCur = rdpCostResume;
  rdpCostHasResume = false;
  rdpCostLastEnd   = end;
  const bool costed = rdpCostPass(costCur, end, xbus, src);
  if(costed) {
    const u32 endM = xbus ? (end & 0x0fff'ffffu) : (end & 0x00ff'ffffu);
    if(softCost.stopAt != endM) { rdpCostResume = softCost.stopAt; rdpCostHasResume = true; }
  }
  const u64 cost = rcp.rdpGclk.load(std::memory_order_relaxed) - gclk0;
  u64 t0 = dpSchedEnd.load(std::memory_order_relaxed);
  if(kick > t0) t0 = kick;
  const u64 t1 = t0 + rcpCyclesToOps(cost);
  const u64 seq = dpSubSeq.load(std::memory_order_relaxed);
  if(kick < dpMaxQuery.load(std::memory_order_relaxed)) {
    dpStale.fetch_add(1, std::memory_order_relaxed);
    ((tlIsRspThread || tlDpLogApply) ? dpStaleR : dpStaleC).fetch_add(1, std::memory_order_relaxed);
  }
  if(seq && kick < dpLastKick) {
    dpOoo.fetch_add(1, std::memory_order_relaxed);
    ((tlIsRspThread || tlDpLogApply) ? dpOooR : dpOooC).fetch_add(1, std::memory_order_relaxed);
  }
  dpLastKick = kick;
  dpJobAddr[seq & kDpRingM]    = current;
  dpJobEndAddr[seq & kDpRingM] = end;
  dpJobStartG[seq & kDpRingM]  = t0;
  dpJobEndG[seq & kDpRingM]    = t1;
  // Desde que instante lo ve un lector. El RSP sella con el instante de SU ciclo, y eso ya es
  // lo que ve la CPU en su siguiente instruccion. La CPU sella con cartNow(), que son las ops
  // retiradas ANTES de la que escribe DPC_END: la escritura queda al final de esa instruccion,
  // o sea en el flanco n+1. Lockstep lo hace asi por construccion (el RSP se intercala tras la
  // instruccion entera); Threaded con la CPU adelantada lo veia un ciclo de sondeo antes y la
  // tarea de DK64 acababa 6 ciclos antes.
  dpJobKickG[seq & kDpRingM]   = (tlIsRspThread || tlDpLogApply || lockRspExec) ? kick : kick + 1;
  dpSchedEnd.store(t1, std::memory_order_release);
  dpSubSeq.store(seq + 1, std::memory_order_release);
  dpcEpoch.fetch_add(1, std::memory_order_release);
  // Sin pase de coste no se sabe si trae SYNC_FULL: se da por hecho. Un tramo vacio no trae nada.
  dpLastSpanSync = costed ? softCost.sawSyncFull : (current != end && rdpCostOn()) || !rdpCostOn();
  // Si el RSP esta aparcado esperando justo esto, su instante de despertar es el lanzamiento
  // de ESTE tramo -- un valor de invitado, o sea determinista. El primero que llega manda.
  // ORDEN: primero se publica el tramo (dpSubSeq, arriba) y DESPUES se mira el aparcamiento,
  // al reves que rspParkWait, que publica rspPark y luego mira dpSubSeq. Con los dos mirando
  // antes de publicar cabia que ninguno viera al otro: el RSP se dormia sin aviso hasta el
  // siguiente vencimiento de 20 ms de pared, y mientras la CPU corria libre hasta el tope de
  // kParkLead. Medido DK64 threaded: 3 de cada 10 corridas perdian asi una tarea de SP.
  // El aviso va con parkMx cogido: el predicado de la espera se evalua con ese mutex, asi que
  // no puede colarse entre la comprobacion y el sueno.
  if(rspPark.load(std::memory_order_acquire)) {
    u64 exp = 0;
    if(rspParkWake.compare_exchange_strong(exp, kick, std::memory_order_acq_rel)) {
      { std::lock_guard<std::mutex> g(parkMx); }
      parkCv.notify_all();
    }
  }
  // Diagnostico: el horario entero en tiempo de invitado. Dos corridas del mismo binario
  // tienen que dar el MISMO fichero linea por linea; si no, hay una fuga de tiempo de anfitrion
  // metida en el reloj. Solo instantes de invitado: nada de cartNow() ni de ciclos del RSP.
  // KESTREL_DPSCHED=1.
  static const bool schedTrace = std::getenv("KESTREL_DPSCHED") != nullptr;
  if(schedTrace) {
    static u32 n = 0;
    if(n++ < 6000)
      std::fprintf(stderr, "[ds] %llu %c kick=%llu t0=%llu t1=%llu cur=%06x end=%06x cost=%llu\n",
                   (unsigned long long)seq, tlIsRspThread ? 'R' : 'C',
                   (unsigned long long)kick, (unsigned long long)t0, (unsigned long long)t1,
                   current, end, (unsigned long long)cost);
  }
  if(costed && softCost.sawSyncFull && rcpDeadlineOn()) {
    // Lanzado por el RSP en Threaded: el plazo lo arma la CPU al llegar al instante del
    // lanzamiento, por el diario. MI_DP es estado de la CPU (rcpRetire), y armarlo desde aqui
    // lo adelantaba una op frente a Lockstep y pisaba un plazo anterior aun sin entregar.
    if(tlIsRspThread && rcpMode == RcpMode::Threaded) {
      dpLogArm[dpLogTail.load(std::memory_order_relaxed) & kDpLogM] = t1;
      dpLogPush(kick, 64u, 0);
    } else {
      dpEndArmAt(t1);
    }
    // Plazo nuevo armado desde el hilo de CPU (un store a DPC_END, que con el JIT puede ir en
    // mitad de una cadena): el permiso de la cadena no lo conocia. Igual que siDma.
    if(!tlIsRspThread && jitGuardPtr) *jitGuardPtr = 0;
  }
  // Zona que este tramo puede pintar (ver rspDmaRdpWait). Se publica ANTES de que rdpSubmit
  // levante el bit2: quien vea el bit ya ve la zona. Sin pase de coste no se sabe: todo.
  // Publicacion bajo seqlock (dpWrSeq impar = a medias): un lector nunca ve un intervalo que
  // aun no esta entero.
  if(costed || !rdpCostOn()) {
    dpWrSeq.fetch_add(1, std::memory_order_release);
    for(u32 i = 0; i < SoftRdp::kWrSlots; i++) {
      dpWrLo[i].store(costed ? softCost.wrLo[i] : (i ? ~0u : 0u), std::memory_order_release);
      dpWrHi[i].store(costed ? softCost.wrHi[i] : (i ? 0u : ~0u), std::memory_order_release);
    }
    dpWrSeq.fetch_add(1, std::memory_order_release);
  }
}

static_assert(SoftRdp::kWrSlots == sizeof(Memory::dpWrLo) / sizeof(Memory::dpWrLo[0]));
// Con rdpMx cogido y el motor drenado del todo (bit2 recien bajado).
auto Memory::dpWrReset() -> void {
  softCost.wrClear();
  dpWrSeq.fetch_add(1, std::memory_order_release);
  for(u32 i = 0; i < SoftRdp::kWrSlots; i++) {
    dpWrHi[i].store(0, std::memory_order_release); dpWrLo[i].store(~0u, std::memory_order_release);
  }
  dpWrSeq.fetch_add(1, std::memory_order_release);
}

auto Memory::dpGuestOn() -> bool {
  static const bool v = [] {
    const char* e = std::getenv("KESTREL_DPGUEST");
    return !e || !*e || (std::strcmp(e, "0") && std::strcmp(e, "off"));
  }();
  return v;
}

// La cita de lectura del FIFO nace APAGADA. Resolvia el mismo problema que el aparcamiento
// (que el RSP no lea el FIFO en un instante al que la CPU aun no ha llegado) pero por el lado
// caro: obligando al RSP a esperar a la CPU en CADA sondeo. El aparcamiento lo resuelve por el
// lado barato -- el RSP aterriza directamente en el lanzamiento del siguiente buffer, que ya es
// un instante de invitado -- y ademas sin el adelanto kRdvLead, que era lo que hacia nacer
// tarde 10-13 plazos de SP por corrida. Medido en DK64, 300 campos:
//   aparcamiento solo      9 s   spArm 173/0 tarde   rastro identico 4/4 INCLUIDO rsp=
//   aparcamiento + cita   11 s   spArm 173/11 tarde  rastro identico solo sin rsp=
// Se deja a mano (KESTREL_DPRDV=1) porque cubre un caso que el aparcamiento no toca: sondear
// DPC_CURRENT con un tramo ABIERTO y por delante de la CPU. En DK64 son ~235 sondeos de 13,9 M
// y ninguno diverge, pero otro juego podria vivir ahi.
auto Memory::dpLogOn() -> bool {
  static const bool v = []{
    const char* e = std::getenv("KESTREL_DPLOG");
    return !e || !*e || (std::strcmp(e, "0") && std::strcmp(e, "off"));
  }();
  return v;
}

// Apunta una escritura DPC del RSP (ver la nota del diario en memory.hpp). Lleno: esperar a que
// la CPU consuma. No puede bloquear: el llamador ya publico el reloj exacto, asi que la CPU
// puede llegar a todo lo apuntado.
auto Memory::dpLogPush(u64 at, u32 reg, u32 v) -> void {
  u32 t = dpLogTail.load(std::memory_order_relaxed);
  if(t - dpLogHead.load(std::memory_order_acquire) >= kDpLogN) dpLogWait(at, false);
  dpLog[t & kDpLogM] = {at, reg, v};
  dpLogTail.store(t + 1, std::memory_order_release);
  rcpPend.fetch_or(16u, std::memory_order_release);
  dpLogPushes.fetch_add(1, std::memory_order_relaxed);
}

// Escritura del microcodigo a DPC: en el acto y en su instante (ver cpuDpcView en memory.hpp).
auto Memory::rspDpcWrite(u64 now, u32 reg, u32 v) -> void {
  const bool view = rcpMode == RcpMode::Threaded;
  if(view) {
    // Diario lleno: esperar ANTES de ocupar la ranura de la cola (ver spDmaLogPush).
    // Dos ranuras: la vista y, si lanza un tramo con SYNC_FULL, el plazo de MI_DP.
    if(dpLogTail.load(std::memory_order_relaxed) - dpLogHead.load(std::memory_order_acquire) >= kDpLogN - 1)
      dpLogWait(now, false);
    dpcViewPend.fetch_add(1, std::memory_order_acq_rel);
  }
  // Sellado con el instante exacto de la instruccion, igual que cuando la CPU aplicaba el diario.
  tlDpLogApply = true; tlDpLogAt = now; tlDpcKick = false;
  rcpRegWrite32(BASE_DPC + (reg << 2), v);
  tlDpLogApply = false;
  if(view) {
    DpcView vw = dpcViewLive();
    vw.st = (vw.st & kDpcViewSt) | (tlDpcKick ? kDpcRunSt : 0u);
    dpLogView[dpLogTail.load(std::memory_order_relaxed) & kDpLogM] = vw;
    dpLogPush(now, 32u, 0);
  }
}

// Buzon CPU -> DPC (ver memory.hpp). Con tarea viva -- HALT a 0 tal como lo ve la CPU, que en
// los dos modos es estado de su hilo -- la escritura no se aplica: se apunta con el borde de grano
// siguiente a su instante, el mismo convenio que las senales de SP_STATUS (spSigQuant). Si ya
// hay algo en el buzon tambien se apunta aunque la tarea haya acabado, para no adelantar lo viejo.
auto Memory::dpcMbPost(u32 phys, u32 v) -> bool {
  if((rcp.sp_status.load(std::memory_order_acquire) & 1u) && !dpcMbN.load(std::memory_order_acquire))
    return false;
  std::lock_guard<std::mutex> lk(dpcMbMx);
  if(dpcMbCount == kDpcMbN) {
    static bool warned = false;
    if(!warned) { warned = true; std::fprintf(stderr, "[dpcmb] buzon lleno, escritura directa\n"); }
    return false;
  }
  const u64 q = spSigQuant(), raw = cartNow() + 1;
  dpcMb[(dpcMbHead + dpcMbCount) % kDpcMbN] = {(raw + q - 1) & ~(q - 1), phys, v};
  ++dpcMbCount;
  dpcMbN.fetch_add(1, std::memory_order_release);
  dpcEpoch.fetch_add(1, std::memory_order_release);
  bumpOwned(dpcMbPosts);     // solo escribe el hilo de CPU, y ademas bajo dpcMbMx
  rcpPend.fetch_or(32u, std::memory_order_release);
  return true;
}

// El RSP en un acceso a DPC en `now`, con la CPU ya en el borde de grano de `now`: todo lo que
// la CPU escribio antes de ese borde esta en el buzon. Lo visible se aplica como escritura suya
// en `now`, con vista fechada para la CPU (rspDpcWrite). Se saca bajo el mutex y se aplica
// fuera: rspDpcWrite puede esperar a la CPU, y la CPU puede estar apuntando.
auto Memory::dpcMbRsp(u64 now) -> void {
  if(!dpcMbN.load(std::memory_order_acquire)) return;
  DpcMbEnt take[kDpcMbN]; u32 n = 0;
  {
    std::lock_guard<std::mutex> lk(dpcMbMx);
    while(dpcMbCount && dpcMb[dpcMbHead].eff <= now) {
      take[n++] = dpcMb[dpcMbHead]; dpcMbHead = (dpcMbHead + 1) % kDpcMbN; --dpcMbCount;
    }
  }
  for(u32 i = 0; i < n; i++) {
    rspDpcWrite(now, (take[i].phys >> 2) & 7u, take[i].v);
    dpcMbN.fetch_sub(1, std::memory_order_release);
  }
}

// La CPU en su retiro: si la tarea ya termino (HALT visible) el RSP no va a volver a mirar, y
// lo que haya vencido se aplica en su borde de grano.
auto Memory::dpcMbCpu(u64 now) -> void {
  if(!(rcp.sp_status.load(std::memory_order_acquire) & 1u)) return;
  for(;;) {
    DpcMbEnt e;
    {
      std::lock_guard<std::mutex> lk(dpcMbMx);
      if(!dpcMbCount || dpcMb[dpcMbHead].eff > now) break;
      e = dpcMb[dpcMbHead]; dpcMbHead = (dpcMbHead + 1) % kDpcMbN; --dpcMbCount;
    }
    tlDpLogApply = true; tlDpLogAt = e.eff;
    mmioWrite32(e.phys, e.v);
    tlDpLogApply = false;
    dpcMbN.fetch_sub(1, std::memory_order_release);
  }
  if(!dpcMbN.load(std::memory_order_acquire)) {
    rcpPend.fetch_and(~32u, std::memory_order_release);
    if(dpcMbN.load(std::memory_order_acquire)) rcpPend.fetch_or(32u, std::memory_order_release);
  }
}

// Ver dmaSettle en la cabecera: el hilo de CPU va a mirar [lo, hi) de RDRAM, asi que si algun
// DMA del diario aun sin aplicar toca esas paginas hay que vaciarlo hasta el reloj de invitado
// de ESTE acceso. Aplicar el diario entero seria mas simple y esta mal: una entrada fechada
// despues de ahora tiene que seguir invisible.
auto Memory::dmaSettleSlow(u32 lo, u64 hi) -> void {
  if(tlDpLogApply) return;                       // ya estamos dentro del propio vaciado
  bumpOwned(dmaSettles);   // solo escribe el hilo de CPU
  if(hi <= lo) hi = (u64)lo + 1;
  u32 p = lo >> kDmaPgShift;
  const u32 e = (u32)((hi - 1) >> kDmaPgShift);
  // Un DMA del cartucho puede abarcar cientos de paginas; recorrerlas una a una cuesta mas
  // que vaciar el diario, y vaciar de mas es legal (dpLogApply solo aplica lo fechado hasta
  // ahora, y sale enseguida si no hay nada). Se mira pagina a pagina solo en tramos cortos.
  if(e >= p + 8) { bumpOwned(dmaSettleHits); dpLogApply(cartNow()); return; }
  for(; p <= e && p < kDmaPgN; p++)
    if(dmaPg[p].load(std::memory_order_acquire)) {
      bumpOwned(dmaSettleHits);
      dpLogApply(cartNow());
      return;
    }
}

auto Memory::dpLogApply(u64 upTo) -> void {
  {
    const u32 h = dpLogHead.load(std::memory_order_acquire);
    if(h == dpLogTail.load(std::memory_order_acquire) || dpLog[h & kDpLogM].at > upTo) return;
  }
  std::lock_guard<std::mutex> lk(dpLogMx);
  for(;;) {
    const u32 h = dpLogHead.load(std::memory_order_acquire);
    if(h == dpLogTail.load(std::memory_order_acquire)) break;
    const DpLogEnt e = dpLog[h & kDpLogM];
    if(e.at > upTo) break;
    tlDpLogApply = true; tlDpLogAt = e.at;
    if(e.reg & 32u) {
      const u32 run = cpuDpcView.st & kDpcRunSt;
      cpuDpcView = dpLogView[h & kDpLogM];
      cpuDpcView.st |= run;
      dpcViewPend.fetch_sub(1, std::memory_order_release);
    } else if(e.reg & 64u) {
      dpEndArmAt(dpLogArm[h & kDpLogM]);
      if(jitGuardPtr) *jitGuardPtr = 0;
    } else if(e.reg & 16u) {
      const DpLogDma& d = dpLogDma[h & kDpLogM];
      spDmaLogApply(d);
      dmaPgMark(d.dram, (u64)(d.length + d.skip) * d.count, -1);
      dmaPgPend.fetch_sub(1, std::memory_order_release);
    } else if(e.reg & 8u) {
      rcpRegWrite32(BASE_SP + ((e.reg & 7u) << 2), e.v);
      spLogPend.fetch_sub(1, std::memory_order_release);
      if(e.v & 0x180u) spLogCrit.fetch_sub(1, std::memory_order_release);
    } else {
      rcpRegWrite32(BASE_DPC + (e.reg << 2), e.v);
    }
    tlDpLogApply = false;
    // head se mueve DESPUES de aplicar: quien espera el diario vacio ya ve el efecto.
    dpLogHead.store(h + 1, std::memory_order_release);
  }
  if(!dpLogPending()) {
    rcpPend.fetch_and(~16u, std::memory_order_release);
    if(dpLogPending()) rcpPend.fetch_or(16u, std::memory_order_release);
  }
  if(rspLogWait.load(std::memory_order_acquire)) rspCv.notify_all();
}

// El RSP espera a que el diario quede vacio (y, con `clock`, a que la CPU llegue a `now`, que
// es la cita de siempre). Mismo salvavidas de pared que spReadSync; al soltarse aplica el
// resto desde aqui para no dejar nada colgado.
auto Memory::dpLogWait(u64 now, bool clock) -> void {
  auto ready = [&]{ return !dpLogPending() && (!clock || cartNow() >= now); };
  // La CPU se esta parando (System::quiesceRcp): lo que quede se aplica ya, desde aqui.
  if(dpLogFlush.load(std::memory_order_acquire)) { dpLogApply(~0ull); if(ready()) return; }
  if(ready()) return;
  dpLogWaits.fetch_add(1, std::memory_order_relaxed);
  // Cita de reloj (lectura de DPC). Sin adelanto, la barrera del SP deja a la CPU exactamente
  // en `now` y cada vuelta del bucle de espera del FIFO del microcodigo es otra cita de unas
  // pocas instrucciones con un cambio de hilo en medio: Perfect Dark en juego va a ~15 fps con
  // el RSP un 75 % del tiempo aqui. Con KESTREL_DPLOGLEAD=1 se abre el adelanto como en
  // dpReadSync (~39 fps) a costa del determinismo: ver dpLogLeadOn.
  const bool lead = clock && rcpMode == RcpMode::Threaded && dpLogLeadOn();
  if(lead) rspRdvAt.store(now, std::memory_order_release);
  rspLogWait.store(true, std::memory_order_release);
  if(rspWaiters.load(std::memory_order_acquire)) rspCv.notify_all();
  bool timing = false;
  const u32 tight = rdvTightTurns(), pm = rdvPollMask(), ym = rdvYieldMask();
  std::chrono::steady_clock::time_point t0{}, t1{};
  const bool cheap = rdvCheapTurn();
  for(u32 k = 0;; ++k) {
    // Espaciado como en spReadSync: `ready()` lee el reloj y el diario de la CPU.
    const bool look = (k < tight || (k & pm) == pm);
    if(look) { if(ready()) break; }
    if(look || !cheap) {
      if(dpLogFlush.load(std::memory_order_acquire)) dpLogApply(~0ull);
      if(rspStop || rsp.hostStop.load(std::memory_order_relaxed)) break;
    }
    if((k & kSpinPauseMask) == kSpinPauseMask) spinPause();
    if((k & ym) != ym) continue;
    std::this_thread::yield();
    if(ready()) break;
    if(rspWaiters.load(std::memory_order_acquire)) rspCv.notify_all();
    if(!rdvWaiveDue(timing, t0, t1)) continue;
    {
      dpLogWaives.fetch_add(1, std::memory_order_relaxed);
      dpLogApply(~0ull);
      break;
    }
  }
  rspLogWait.store(false, std::memory_order_release);
  if(lead) {
    // Fase de holgura, la misma de dpReadSync: no es de correccion (el instante leido sigue
    // siendo `now` y la condicion es `now <= cartNow()`, que pasarse cumple mejor), solo de
    // coste. Deja que la CPU se adelante un grano entero para que las vueltas siguientes del
    // bucle de espera se respondan sin volver a citarse.
    const u64 tgt = now + kRdvGrain;
    for(u32 k = 0; k < 8192 && cartNow() < tgt; ++k) {
      if(rspStop || rsp.hostStop.load(std::memory_order_relaxed)) break;
      if((k & kSpinPauseMask) == kSpinPauseMask) spinPause();
    }
    rspRdvAt.store(0, std::memory_order_release);
  }
}

// Salvavidas de las citas del RSP con la CPU (spReadSync, dpReadSync, dpLogWait). Soltar una
// cita deja al RSP ver o aplicar algo por delante de la CPU, o sea que el anfitrion vuelve a
// decidir; solo se justifica si la CPU esta BLOQUEADA esperando algo del RCP (cpuRcpWait) y
// por eso no puede llegar nunca. Si la CPU esta haciendo trabajo de anfitrion -- abrir el
// dispositivo de audio en la primera escritura del AI son ~80 ms en el hilo de CPU --, va a
// llegar sola: esperar. Los 20 ms cuentan desde que la CPU ENTRA en la espera (t1), no desde
// que empezo la cita; el tope largo (t0) solo cubre una espera sin marcar.
// Con la CPU parada en la barrera del RDP el reloj vuelve a cero (ver cpuDpBarWait).
auto Memory::rdvWaiveDue(bool& timing, std::chrono::steady_clock::time_point& t0,
                         std::chrono::steady_clock::time_point& t1) -> bool {
  const auto t = std::chrono::steady_clock::now();
  if(!timing || cpuDpBarWait.load(std::memory_order_acquire)) { timing = true; t0 = t1 = t; return false; }
  if(!cpuRcpWait.load(std::memory_order_acquire)) { t1 = t; return t - t0 > std::chrono::milliseconds(2000); }
  return t - t1 > std::chrono::milliseconds(20);
}

// Adelanto de la CPU en la cita de lectura de DPC (ver dpLogWait). Opcional y APAGADO de
// fabrica: con la CPU por delante del RSP los eventos y plazos que nacen del RSP llegan tarde
// y su reajuste depende del anfitrion, asi que threaded deja de coincidir con lockstep
// (junkrunner64 a 400M instrucciones da un statehash distinto en cada corrida). A cambio,
// Perfect Dark en juego pasa de ~15 a ~39 fps. Modo rapido, no fiel.
auto Memory::dpLogLeadOn() -> bool {
  // Sin la variable (o con "auto") manda el modo de velocidad: en "libre", el de jugar, va
  // puesto; en "Fiel a consola" (KESTREL_SPEEDMODE=hw) va quitado y threaded == lockstep.
  // Las puertas (scripts/validate.py) lo fijan a 0: son el oraculo determinista.
  static const bool v = []{
    const char* e = std::getenv("KESTREL_DPLOGLEAD");
    if(!e || !*e || !std::strcmp(e, "auto")) return !rt::speedModeHw();
    return std::strcmp(e, "0") && std::strcmp(e, "off");
  }();
  return v;
}

auto Memory::dpRdvOn() -> bool {
  static const bool v = []{
    const char* e = std::getenv("KESTREL_DPRDV");
    return e && *e && std::strcmp(e, "0") && std::strcmp(e, "off");
  }();
  return v;
}

// Ver la nota larga en memory.hpp. Aqui solo la mecanica: publicar ya lo ha hecho quien
// llama (Rsp::mfc0), asi que la barrera del SP ya no retiene a la CPU antes de `now`.
// Aparcar el RSP mientras espera al FIFO drenado.
//
// La cita de lectura (dpReadSync) obliga al RSP a ir por detras de la CPU, y la barrera del
// SP obliga a la CPU a ir por detras del RSP. Juntas dejan a los dos hilos clavados en el
// mismo instante de invitado, avanzando de cinco en cinco instrucciones con un cambio de hilo
// en medio: un millon de citas en 300 campos de DK64.
//
// La salida no es aflojar ninguna de las dos, es reconocer cuando una de ellas sobra. Con el
// motor del RDP drenado y el microcodigo dando vueltas al bucle de espera del FIFO, el RSP no
// puede producir NINGUN evento: ni cierra tarea, ni escribe DPC_END, ni toca memoria. Lo unico
// que puede sacarlo de ahi es que la CPU instale el siguiente buffer. Asi que durante esa
// ventana la barrera del SP no protege nada y se levanta entera, la CPU corre suelta, y el
// instante en que el RSP despierta es el lanzamiento de ese buffer: un instante de invitado,
// no de anfitrion. El salto sigue siendo un numero entero de vueltas del bucle.
auto Memory::rcpSchedReset() -> void {
  std::lock_guard<std::mutex> lk(rdpMx);
  dpSchedEnd.store(0, std::memory_order_relaxed);
  dpSyncEnds.clear();
  dpSyncBarAt.store(~0ull, std::memory_order_relaxed);
  dpSubSeq.store(0, std::memory_order_relaxed);
  dpcEpoch.fetch_add(1, std::memory_order_release);
  dpCompSeq.store(0, std::memory_order_relaxed);
  dpMaxQuery.store(0, std::memory_order_relaxed);
  dpLastKick = 0;
  dpBarWaivedAt = ~0ull;
  { std::lock_guard<std::mutex> lk(spSigMx); spSigHead = spSigCount = 0; }
  for(u32 i = 0; i < kDpRingN; ++i) {
    dpJobStartG[i] = dpJobEndG[i] = dpJobKickG[i] = 0;
    dpJobAddr[i] = dpJobEndAddr[i] = 0;
  }
  // Barreras: no hay tarea en vuelo (el estado se toma en reposo), asi que ninguna esta
  // armada y el ancla de la barrera del SP se vuelve a atar al reloj que acaba de entrar.
  // Dejarla en el ancla vieja daria un spBarrierAt() de otra partida. Los fines de tarea
  // armados (bits 0-1 y sus plazos) SI son de esta partida: vienen en el estado.
  rcpPend.store(rcpPend.load(std::memory_order_relaxed) & 3u, std::memory_order_relaxed);
  { std::lock_guard<std::mutex> lk(dpLogMx); dpLogHead.store(0); dpLogTail.store(0); spLogPend.store(0); spLogCrit.store(0);
    dpcViewPend.store(0); cpuDpcView = dpcViewLive();
    dmaPgPend.store(0);
    for(u32 i = 0; i < kDmaPgN; i++) dmaPg[i].store(0, std::memory_order_relaxed); }
  spMarkKick();
  rspRdvAt.store(0, std::memory_order_relaxed);
  dpRdv.store(0, std::memory_order_relaxed);
  rspPark.store(0, std::memory_order_relaxed);
  rspParkWake.store(0, std::memory_order_relaxed);
  // Firma del bucle de espera del FIFO: la lectura anterior es de la partida vieja y podria
  // casar por casualidad con la primera de la nueva.
  rsp.idlePc = 0xffffffffu;
  rsp.idleHash = rsp.idleVal = 0;
  rsp.idleHashOk = false;
  rsp.cpuSeen = 0;
  rsp.dpcFastEnd = 0;   // camino rapido del sondeo de DPC (ver rsp.hpp)
  rsp.idleAt = rsp.idleLen = 0;
  rsp.idleNoSig = rsp.idleNoDrain = rsp.idleNoRoom = 0;
}

// Carrera de publicacion, sin esperar: un tramo lanzado entre que el RSP miro el horario y
// ahora. Es lo unico que rspParkWait podia aportar con el motor drenado que sea un instante de
// invitado de verdad; lo demas era el tope del regulador. Ver rspParkWait y Rsp::idleSkip.
auto Memory::rspParkMissed(u64 seq0) -> u64 {
  if(dpSubSeq.load(std::memory_order_acquire) == seq0) return 0;
  const u64 st = dpJobStartG[seq0 & kDpRingM], kk = dpJobKickG[seq0 & kDpRingM];
  return kk < st ? kk : st;
}

auto Memory::rspParkWait(u64 now, u64 seq0, u64 until) -> u64 {
  // Tope de adelanto de la CPU. Es tambien el destino de reserva: si la CPU llega hasta el sin
  // haber lanzado nada, el RSP salta ahi -- instante de invitado exacto -- y se vuelve a aparcar.
  // Con el motor ocupado (`until`) el tope es ademas el siguiente cambio del horario del RDP:
  // pasado ese instante la lectura ya no vale, y la CPU tampoco puede dejar atras al RSP.
  const u64 cap = (until && until < now + kParkLead) ? until : now + kParkLead;
  // CARRERA DE PUBLICACION. dpScheduleSpan mira `rspPark` para saber si tiene que dejar la
  // fecha de despertar, asi que un tramo archivado entre que idleSkip vio el motor drenado y
  // que aqui se publica el aparcamiento no deja ninguna: el aviso se pierde y el RSP se queda
  // hasta el tope o hasta el tramo SIGUIENTE segun como se crucen los dos hilos -- o sea,
  // segun el anfitrion. Medido: 1 de cada 4 corridas de DK64 divergia por esto, siempre con
  // una fecha de fin de SP naciendo tarde. Se comprueba a mano con el contador de tramos:
  // si se ha movido desde `seq0`, el lanzamiento de ESE tramo es el despertar. Con el motor
  // drenado el arranque del tramo ES su lanzamiento, asi que dpJobStartG vale directamente,
  // y la carga con acquire de dpSubSeq es la que hace visible lo que escribio dpScheduleSpan.
  auto missed = [&]() -> u64 {
    if(dpSubSeq.load(std::memory_order_acquire) == seq0) return 0;
    // Con el motor drenado arranque == lanzamiento. Ocupado, el arranque espera al cierre del
    // anterior, pero DPC_STATUS (END_VALID) cambia ya en el lanzamiento: gana el menor.
    const u64 st = dpJobStartG[seq0 & kDpRingM], kk = dpJobKickG[seq0 & kDpRingM];
    return kk < st ? kk : st;
  };
  if(u64 m = missed()) return m;
  if(cartNow() >= cap) return cap;
  rspParkWake.store(0, std::memory_order_relaxed);
  // Suelta el freno del prologo del dynarec ANTES de publicar el aparcamiento: mientras el
  // RSP duerme no ejecuta microcodigo ni escribe MMIO, asi que devolver el control al
  // trampolin en cada eslabon de la cadena solo cuesta una llamada por bloque. Ver Rsp::brake.
  rsp.brake = false;
  rspParkCap.store(cap, std::memory_order_release);
  rspPark.store(now, std::memory_order_release);
  const auto tPark = std::chrono::steady_clock::now();
  if(rspWaiters.load(std::memory_order_acquire)) rspCv.notify_all();
  // Giro previo a dormir. Con el motor ocupado el tope suele estar a unos pocos miles de
  // instrucciones de la CPU, y el que lo alcanza no avisa a nadie: la CPU se queda en la
  // barrera del SP y el RSP dormiria el plazo entero del condvar (~30 ms de pared en Windows).
  {
    const u32 tight = rdvTightTurns(), pm = rdvPollMask();
    for(u32 k = 0; k < 65536u; ++k) {
      // `rspParkWake` es nuestro aviso directo (la CPU lo escribe una vez), pero `cartNow()` y
      // `missed()` son lineas que el hilo de CPU esta escribiendo sin parar: mirarlas en cada
      // vuelta se lo cobra a el. Ver la nota de rdvTightTurns.
      if(rspParkWake.load(std::memory_order_acquire)) break;
      if(k < tight || (k & pm) == pm) { if(cartNow() >= cap || missed()) break; }
      if(rspStop || rsp.hostStop.load(std::memory_order_relaxed)) break;
      if((k & kSpinPauseMask) == kSpinPauseMask) spinPause();
    }
  }
  u64 wake = 0, miss = 0;
  bool capped = false;
  u64 seen = cartNow();
  u64 stuckNs = 0;
  u32 spins = 0;
  for(;;) {
    ++spins;
    auto t0 = std::chrono::steady_clock::now();
    {
      std::unique_lock<std::mutex> lk(parkMx);
      parkCv.wait_for(lk, std::chrono::milliseconds(20), [&]{
        wake = rspParkWake.load(std::memory_order_acquire);
        if(wake) return true;
        miss = missed();
        if(miss) return true;
        if(cartNow() >= cap) { capped = true; return true; }
        return rspStop || rsp.hostStop.load(std::memory_order_relaxed);
      });
    }
    if(wake || miss || capped) break;
    if(rspStop || rsp.hostStop.load(std::memory_order_relaxed)) break;
    // Salvavidas al estilo del regulador: lo que no se tolera es que la CPU se ATASQUE, no
    // que tarde. Mientras siga retirando instrucciones se sigue esperando, porque el lanzamiento
    // que despierta al RSP es suyo y va a llegar. Sin este reinicio el corte saltaba en los dos
    // tramos lentos de DK64 y con el se iba la unica columna del rastro que aun no cuadraba.
    const u64 nowCart = cartNow();
    if(nowCart != seen) { seen = nowCart; stuckNs = 0; continue; }
    stuckNs += (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(
                 std::chrono::steady_clock::now() - t0).count();
    if(stuckNs > 200'000'000ull) break;
  }
  rsp.brake = true;                      // antes de retirar el aparcamiento: el RSP vuelve a correr
  rspPark.store(0, std::memory_order_release);
  const u64 parkNs = (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::steady_clock::now() - tPark).count();
  rspParkNs.fetch_add(parkNs, std::memory_order_relaxed);
  rspParks.fetch_add(1, std::memory_order_relaxed);
  static const bool parkLog = std::getenv("KESTREL_PARKLOG") != nullptr;
  if(parkLog)
    std::fprintf(stderr, "[pk] now=%llu until=%llu cap=%llu sched=%llu wall=%.2fms via=%s salto=%lld cart=%llu spins=%u\n",
                 (unsigned long long)now, (unsigned long long)until, (unsigned long long)cap, (unsigned long long)dpSchedEnd.load(), parkNs / 1e6,
                 wake ? "wake" : miss ? "miss" : capped ? "cap" : "lifeguard",
                 (long long)((wake ? wake : miss ? miss : capped ? cap : now) - (long long)now),
                 (unsigned long long)cartNow(), spins);
  // El lanzamiento del tramo siempre es anterior al tope (si la CPU hubiera pasado del tope
  // no habria llegado a aparcarse), asi que de haber varios candidatos gana el mas temprano.
  if(wake) return wake;
  if(miss) { rspParkMiss.fetch_add(1, std::memory_order_relaxed); return miss; }
  if(capped) { rspParkCapN.fetch_add(1, std::memory_order_relaxed); return cap; }
  // Salvavidas de anfitrion: aqui el instante ya no seria de invitado, asi que no se salta nada.
  rspParkWv.fetch_add(1, std::memory_order_relaxed);
  if(std::getenv("KESTREL_DPSYNCLOG"))
    std::fprintf(stderr, "[park] salvavidas now=%llu cart=%llu cap=%llu stop=%u/%u\n",
                 (unsigned long long)now, (unsigned long long)cartNow(),
                 (unsigned long long)cap, (unsigned)rspStop,
                 (unsigned)rsp.hostStop.load(std::memory_order_relaxed));
  return 0;
}

auto Memory::dpReadSync(u64 now) -> void {
  if(!dpRdvOn() || !dpGuestOn()) return;
  if(cartNow() >= now) return;
  dpRdv.fetch_add(1, std::memory_order_relaxed);
  rspRdvAt.store(now, std::memory_order_release);   // abre el adelanto de la CPU
  // La CPU puede estar dormida en la barrera del SP con el valor viejo: hay que sacarla.
  if(rspWaiters.load(std::memory_order_acquire)) rspCv.notify_all();
  bool timing = false;
  const u32 ym = rdvYieldMask();
  std::chrono::steady_clock::time_point t0{}, t1{};
  // Fase obligatoria: hasta `now`, con salvavidas.
  for(u32 k = 0;; ++k) {
    if(cartNow() >= now) break;
    if(rspStop || rsp.hostStop.load(std::memory_order_relaxed)) break;
    if((k & kSpinPauseMask) == kSpinPauseMask) spinPause();
    if((k & ym) != ym) continue;
    std::this_thread::yield();
    if(rspWaiters.load(std::memory_order_acquire)) rspCv.notify_all();
    // El reloj de pared no se toca hasta la primera tanda: son millones de citas por corrida
    // y dos lecturas de steady_clock en cada una costarian mas que la espera entera.
    if(!rdvWaiveDue(timing, t0, t1)) continue;
    {
      dpRdvWaives.fetch_add(1, std::memory_order_relaxed);
      break;   // salvavidas: la CPU no avanza (parada, o esperando algo del RSP)
    }
  }
  // Fase de holgura. No hace falta para la correccion -- abandonarla no rompe nada -- pero es
  // la que decide el COSTE: sin ella la cita despierta al RSP en cuanto la CPU avanza un pelo,
  // el RSP salta ese pelo y vuelve a citarse, y las dos mitades degeneran en un paso a paso de
  // cinco instrucciones con un cambio de hilo en medio. Dejando que la CPU se adelante un grano
  // entero, el salto del bucle de espera se lo come de una vez y las citas bajan un orden de
  // magnitud. El grano cabe dentro de kRdvLead, asi que la barrera del SP no lo estorba.
  const u64 tgt = now + kRdvGrain;
  for(u32 k = 0; k < 8192 && cartNow() < tgt; ++k) {
    if(rspStop || rsp.hostStop.load(std::memory_order_relaxed)) break;
    if((k & kSpinPauseMask) == kSpinPauseMask) spinPause();
  }
  rspRdvAt.store(0, std::memory_order_release);
}

// SP_STATUS tal como lo ve el RSP en su instante `now`. Las entradas con sello <= now ya
// ocurrieron y se descartan (el reloj del RSP no retrocede); las posteriores se deshacen de la
// mas nueva a la mas vieja, dejando los bits como estaban antes de cada una.
// Grano de visibilidad de SIG0..SIG7 escritos por la CPU, vistos desde el RSP. Una escritura
// en la op n (flanco n+1) se hace visible al RSP en el siguiente multiplo de Q. Con Q = 1 es
// el instante exacto, pero entonces cada sondeo de SIG0 del microcodigo (SM64: ~1300 por campo)
// obliga a la CPU a alcanzar al RSP -- que va por delante por la barrera -- y el paso a paso
// de hilos cuesta un 25% del tiempo total. Con Q = 2^14 la CPU solo tiene que llegar al ultimo
// multiplo de Q, que casi siempre ya ha pasado: la cita cae a una por borde cruzado. El retraso
// maximo es Q ops (~0,17 ms de invitado con 2^14), muy por debajo de lo que tarda un yield en
// surtir efecto (el microcodigo lo mira entre comandos). Es el mismo en Lockstep y Threaded,
// asi que el determinismo entre modos no cambia. KESTREL_SPSIGQ=<log2> lo ajusta; 0 = exacto.
auto Memory::spSigQuant() -> u64 {
  static const u64 v = [] {
    const char* e = std::getenv("KESTREL_SPSIGQ");
    u32 l = (e && *e) ? (u32)std::strtoul(e, nullptr, 10) : 14u;
    return 1ull << (l > 30 ? 30 : l);
  }();
  return v;
}

// Lanzamiento: el RSP arranca en el instante del CLEAR_HALT y todo lo que la CPU escribio antes
// en SP_STATUS ya esta en el registro. Con grano > 1 esas escrituras podian seguir aplazadas, y
// el microcodigo arrancaba viendo senales viejas (SM64 se quedaba sin tareas: osSpTaskLoad
// limpia SIG antes de lanzar). Aqui pasan a su flanco real, que es <= el del lanzamiento.
auto Memory::spSigAtKick() -> void {
  std::lock_guard<std::mutex> lk(spSigMx);
  for(u32 k = 0; k < spSigCount; ++k) {
    SpSigWr& w = spSigRing[(spSigHead + k) % kSpSigN];
    w.stamp = w.raw; w.flags &= ~1u;
  }
}

// BREAK del RSP en `now` con grano > 1. Si la CPU escribio CLEAR_HALT con el nucleo en marcha
// (flanco <= now) junto con senales que el RSP aun no ha podido ver, en hardware el microcodigo
// las habria leido antes de decidir parar. Es la carrera de rspq en libdragon: SET_SIG_MORE |
// CLEAR_HALT mientras el RSP comprueba SIG_MORE justo antes de su BREAK. Con la escritura
// aplazada el RSP pararia y el CLEAR_HALT, que fue un no-op, ya no lo despierta. Se resuelve
// como si la escritura hubiera llegado DESPUES del BREAK, que es lo que ese retraso significa:
// el CLEAR_HALT lanza de nuevo y el RSP sigue en la instruccion siguiente. Devuelve 0 si hay
// que parar, o 1 | (2 si la escritura llevaba CLEAR_BROKE).
auto Memory::spLateClearHalt(u64 now) -> u32 {
  std::lock_guard<std::mutex> lk(spSigMx);
  for(u32 k = 0; k < spSigCount; ++k) {
    SpSigWr& w = spSigRing[(spSigHead + k) % kSpSigN];
    if(w.raw > now) break;
    if((w.flags & 1u) && w.mask) { const u32 f = w.flags; w.flags &= ~1u; spLateHalts.fetch_add(1, std::memory_order_relaxed); return 1u | (f & 2u); }
  }
  return 0;
}

auto Memory::spStatusForRsp(u64 now) -> u32 {
  std::lock_guard<std::mutex> lk(spSigMx);
  while(spSigCount && spSigRing[spSigHead].stamp <= now) { spSigHead = (spSigHead + 1) % kSpSigN; --spSigCount; }
  u32 v = rcp.sp_status.load(std::memory_order_acquire);
  for(u32 k = spSigCount; k-- > 0;) {
    const SpSigWr& w = spSigRing[(spSigHead + k) % kSpSigN];
    v = (v & ~w.mask) | (w.prev & w.mask);
  }
  return v | (rcp.sp_intr_on_break.load(std::memory_order_acquire) ? 0x40u : 0u);
}

// Cita del sondeo de SP_STATUS. Es la fase obligatoria de dpReadSync y nada mas: la CPU tiene
// que haber retirado hasta `now` para que ninguna escritura suya anterior a ese instante siga
// pendiente, pero SIN el adelanto kRdvLead -- con la CPU por delante del RSP el fin de tarea
// podria caer en un instante ya rebasado y el plazo de SP naceria tarde. La barrera del SP deja
// llegar a la CPU justo hasta el reloj publicado, que el llamador ya ha publicado exacto.
// No depende de KESTREL_DPRDV: sin ella el ciclo en que el microcodigo ve SIG0 es de anfitrion.
// Hueco de invitado (ops de CPU que faltan) a partir del cual la cita del RSP DUERME en vez de
// girar. 0 = no dormir nunca, que es el comportamiento de siempre. Ver rspWakeIfDue.
static auto rdvSleepGap() -> u64 {
  static const u64 v = []() -> u64 {
    const char* e = std::getenv("KESTREL_RDVSLEEP");
    if(e && *e) { char* end = nullptr; unsigned long long n = std::strtoull(e, &end, 0);
                  if(end && !*end) return (u64)n; }
    return 0ull;
  }();
  return v;
}

auto Memory::spReadSync(u64 now, u32 site) -> void {
  const u64 at0 = cartNow();
  if(at0 >= now) return;
  spRdv.fetch_add(1, std::memory_order_relaxed);
  if(site < kRdvSites) { spRdvSiteN[site].fetch_add(1, std::memory_order_relaxed);
                         addOwned(spRdvSiteGap[site], now - at0); }   // solo el hilo del RSP
  struct SiteK {                              // vueltas del giro, para saber cual CUESTA
    std::atomic<u64>* c; u32 k = 0;
    ~SiteK() { if(c) c->fetch_add(k, std::memory_order_relaxed); }
  } sk_{site < kRdvSites ? &spRdvSiteK[site] : nullptr};
  RcpWaitMark sw_{rspSyncWait};   // antes de mirar rspWaiters: el regulador suelta a la CPU
  // PROBADO Y DESCARTADO: darle aqui el adelanto de la cita (rspRdvAt = now, o sea kRdvLead)
  // para que la CPU pueda cubrir de una vez la tanda de publicacion del RSP. No ahorra pared
  // (2923/3024/3158 ms contra 2925/2965/2935) porque las vueltas del giro apenas bajan
  // (102 M contra 111 M), y ROMPE el determinismo: el plazo de fin de tarea no conoce ese
  // adelanto, salen 9-12 plazos vencidos y statehash distinto entre corridas.
  if(rspWaiters.load(std::memory_order_acquire)) rspCv.notify_all();
  bool timing = false;
  const u32 tight = rdvTightTurns(), pm = rdvPollMask(), ym = rdvYieldMask();
  std::chrono::steady_clock::time_point t0{}, t1{};
  const bool cheap = rdvCheapTurn();
  for(u32 k = 0;; ++k) {
    sk_.k = k;
    const bool look = (k < tight || (k & pm) == pm);
    if(look) { if(cartNow() >= now) break; }
    if((look || !cheap) && (rspStop || rsp.hostStop.load(std::memory_order_relaxed))) break;
    if((k & kSpinPauseMask) == kSpinPauseMask) spinPause();
    // Siesta: con la CPU a mas de `gap` ops de distancia, girar no la acerca -- en Perfect Dark
    // el sitio del sondeo de DPC espera 11 000 ops de invitado (~117 us) de media. Se apunta el
    // instante, se duerme, y el hilo de CPU avisa al pasar por el (Memory::rspWakeIfDue). Va con
    // su PROPIA cadencia, no detras del yield: ese va 1 de cada 65536 vueltas y una espera de
    // 17 000 vueltas no llegaria a el nunca. El tope de 200 us cubre el aviso perdido; la
    // condicion de salida sigue siendo `cartNow() >= now`, asi que el invitado sale identico.
    if(const u64 gap = rdvSleepGap(); gap && (k & 0x3FFu) == 0x3FFu) {
      const u64 at = cartNow();
      if(at < now && now - at > gap) {
        rspWaitAt.store(now, std::memory_order_release);
        auto s0 = std::chrono::steady_clock::now();
        {
          std::unique_lock<std::mutex> lk(rspWaitMx);
          rspWaitCv.wait_for(lk, std::chrono::microseconds(200), [&]{
            return cartNow() >= now || rspStop || rsp.hostStop.load(std::memory_order_relaxed); });
        }
        rspWaitAt.store(0, std::memory_order_release);
        rspSleeps.fetch_add(1, std::memory_order_relaxed);
        rspSleepNs.fetch_add((u64)std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now() - s0).count(),
                             std::memory_order_relaxed);
      }
    }
    if((k & ym) != ym) continue;
    std::this_thread::yield();
    if(rspWaiters.load(std::memory_order_acquire)) rspCv.notify_all();
    if(!rdvWaiveDue(timing, t0, t1)) continue;
    {
      spRdvWaives.fetch_add(1, std::memory_order_relaxed);
      break;   // salvavidas: la CPU no avanza (parada, o esperando algo del RSP)
    }
  }
}

auto Memory::tlDpVisCur() -> u64& { static thread_local u64 v = 0; return v; }
auto Memory::tlDpCompCur() -> u64& { static thread_local u64 v = 0; return v; }

auto Memory::dpcCurrentFor(u64 now, u32 who) -> u32 {
  bumpOwned(dpcRdCur[who]);
  if(now > dpMaxQuery.load(std::memory_order_relaxed))
    dpMaxQuery.store(now, std::memory_order_relaxed);
  if(!dpGuestOn()) return rcp.dpc_current.load(std::memory_order_acquire);
  u64 c   = dpCompletedAt(now);
  u64 sub = dpVisibleAt(now);
  if(c > sub) c = sub;
  if(sub > c) {
    bumpOwned(dpcRdOpen[who]);
    // El trabajo `c` esta abierto para este reloj. DPC_CURRENT no se queda clavado en la
    // direccion de apertura: el motor va consumiendo el FIFO comando a comando y el puntero
    // avanza con el. Es lo que mira el microcodigo grafico para saber cuanto buffer puede
    // reutilizar, asi que clavarlo equivale a decirle "no he leido nada todavia" durante todo
    // el trabajo -- y se queda sondeando en bucle. Medido en DK64: 5,5 k lecturas de DPC
    // pasaban a 9,8 M y el RSP quemaba 3,5x sus ciclos.
    //
    // Sin coste por comando no se puede saber por donde va exactamente, pero el reparto lineal
    // sobre el intervalo del trabajo es la aproximacion honrada: mismo ritmo medio, extremos
    // exactos, y monotona. Todo el calculo es en tiempo de invitado.
    const u32 a0 = dpJobAddr[c & kDpRingM], a1 = dpJobEndAddr[c & kDpRingM];
    const u64 t0 = dpJobStartG[c & kDpRingM], t1 = dpJobEndG[c & kDpRingM];
    if(a1 <= a0 || now <= t0) return a0;
    if(t1 <= t0 || now >= t1) return a1;
    const u64 span = (u64)(a1 - a0);
    u32 adv = (u32)((span * (now - t0)) / (t1 - t0));
    return a0 + (adv & ~7u);   // el FIFO se lee en palabras de 64 bits
  }
  if(c > 0)   return dpJobEndAddr[(c - 1) & kDpRingM];
  return rcp.dpc_current.load(std::memory_order_acquire);
}

auto Memory::dpcStatusFor(u64 now, u32 who) -> u32 {
  u32 st = rcp.dpc_status.load(std::memory_order_acquire) | 0x80u;  // CBUF_READY
  bumpOwned(dpcRdSt[who]);
  if(now > dpMaxQuery.load(std::memory_order_relaxed))
    dpMaxQuery.store(now, std::memory_order_relaxed);
  if(!dpGuestOn()) {
    u32 pend = dpPending.load(std::memory_order_acquire);
    if(pend)      st |= 0x100u | 0x40u;
    if(pend >= 2) st |= 0x200u;
    return st;
  }
  u64 c   = dpCompletedAt(now);
  u64 vis = dpVisibleAt(now);
  u64 out = vis > c ? vis - c : 0;   // trabajos vivos para ESTE reloj
  if(out >= 1) { st |= 0x100u | 0x40u;   // DMA_BUSY | CMD_BUSY
    bumpOwned(dpcRdBusy[who]); }
  // END_VALID: ya hay un buffer esperando ademas del que el motor esta leyendo, o sea que el
  // par START/END esta lleno y no cabe otro.
  if(out >= 2) { st |= 0x200u;
    bumpOwned(dpcRdEndV[who]); }
  return st;
}

// Esperar a que el RDP alcance el instante de invitado del lector. Lo usa el hilo del RSP
// antes de mirar DPC: los dos chips van al mismo reloj de 62,5 MHz y el RSP no puede leer el
// estado del RDP "del futuro" -- ni del pasado. Sin esto el RSP adelanta al RDP en tiempo de
// pared y ve el FIFO como este en ese momento, que es justo la puerta por la que se colaba el
// anfitrion. Salvavidas de 20 ms como todas las demas esperas.
auto Memory::rdpAwaitGuest(u64 now) -> void {
  if(!dpGuestOn() || !dpBarrierOn() || rcpMode != RcpMode::Threaded) return;
  if(!(rcpPend.load(std::memory_order_acquire) & 4u)) return;
  u64 waited = 0;
  u64 comp = dpCompSeq.load(std::memory_order_acquire);
  for(;;) {
    u64 bar = dpBarrierAt();
    if(now <= bar) return;
    dpAwaits.fetch_add(1, std::memory_order_relaxed);
    auto t0 = std::chrono::steady_clock::now();
    if(!dpSpinUntil(bar, comp)) {
      std::unique_lock<std::mutex> lk(rdpMx);
      rdpCv.wait_for(lk, std::chrono::microseconds(200), [&]{
        return !(rcpPend.load(std::memory_order_acquire) & 4u) || dpBarrierAt() != bar
            || dpCompSeq.load(std::memory_order_acquire) != comp; });
    }
    u64 dt = (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now() - t0).count();
    dpBarRspNs.fetch_add(dt, std::memory_order_relaxed);
    if(!(rcpPend.load(std::memory_order_acquire) & 4u)) return;
    u64 nextComp = dpCompSeq.load(std::memory_order_acquire);
    if(nextComp != comp) { comp = nextComp; waited = 0; continue; }
    if(dpBarrierAt() != bar) { waited = 0; continue; }
    waited += dt;
    if(waited > kBarrierMaxWait) { dpWvAwait.fetch_add(1, std::memory_order_relaxed);
      dpBarWaives.fetch_add(1, std::memory_order_relaxed); return; }
  }
}

// Un DMA del RSP no puede mirar RDRAM que el RDP aun esta escribiendo. Es la UNICA via por la
// que el RSP toca la RDRAM (su bus solo ve DMEM/IMEM), asi que es aqui, y no al leer DPC,
// donde tiene que esperar a los pixeles: si el DMA solapa la zona que el motor puede estar
// pintando (dpWrLo/Hi, sacada del pase de coste al lanzar el tramo) espera a que quede drenado
// DEL TODO en el anfitrion (bit2 de rcpPend, que cubre tambien el fence). Sin mirar instantes de
// invitado a proposito: esperar de mas solo cuesta tiempo de anfitrion (el reloj del RSP no
// avanza mientras) y esperar de menos no puede pasar. No hay abrazo mortal: el worker no
// necesita a nadie para drenar. Salvavidas como las demas esperas.
auto Memory::rspDmaRdpWait(u32 lo, u32 hi) -> void {
  if(!tlIsRspThread || !dpBarrierOn()) return;
  if(!(rcpPend.load(std::memory_order_acquire) & 4u)) return;
  // Solo si el DMA cae encima de lo que el motor puede estar pintando (KESTREL_DMASPAN=0: siempre).
  static const bool span = [] { const char* e = std::getenv("KESTREL_DMASPAN");
                                return !(e && e[0] == '0'); }();
  if(span) {
    bool hit;
    for(;;) {
      const u32 s0 = dpWrSeq.load(std::memory_order_acquire);
      if(s0 & 1u) { std::this_thread::yield(); continue; }
      hit = false;
      for(u32 i = 0; i < SoftRdp::kWrSlots; i++)
        hit |= lo < dpWrHi[i].load(std::memory_order_acquire) && dpWrLo[i].load(std::memory_order_acquire) < hi;
      if(dpWrSeq.load(std::memory_order_acquire) == s0) break;
    }
    if(!hit) return;
  }
  rspDmaRdpWaits.fetch_add(1, std::memory_order_relaxed);
  const u32 bits = 4u;
  u64 waited = 0;
  while(rcpPend.load(std::memory_order_acquire) & bits) {
    auto t0 = std::chrono::steady_clock::now();
    bool done = false;
    for(u32 k = 0, lim = dpSpinLen(); k < lim; ++k) {
      if(!(rcpPend.load(std::memory_order_acquire) & bits)) { done = true; break; }
      if((k & kSpinPauseMask) == kSpinPauseMask) spinPause();
    }
    if(!done) {
      std::unique_lock<std::mutex> lk(rdpMx);
      rdpCv.wait_for(lk, std::chrono::microseconds(200), [&]{
        return !(rcpPend.load(std::memory_order_acquire) & bits); });
    }
    u64 dt = (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now() - t0).count();
    dpBarRspNs.fetch_add(dt, std::memory_order_relaxed);
    waited += dt;
    if(waited > kBarrierMaxWait) { dpBarWaives.fetch_add(1, std::memory_order_relaxed); return; }
  }
}

auto Memory::spBarrierOn() -> bool {
  static const bool v = [] {
    const char* e = std::getenv("KESTREL_SPBARRIER");
    return !e || !*e || (std::strcmp(e, "0") && std::strcmp(e, "off"));
  }();
  return v;
}

// Parar el reloj del invitado en la barrera del RSP. Dormir aqui NO cuesta tiempo de invitado:
// el hilo de CPU no retira instrucciones mientras espera, igual que en rspPace.
// Umbral de la barrera del SP en CICLOS CRUDOS del RSP. spBarrierWait giraba llamando a
// spBarrierEff() en cada vuelta, y esa cuenta no es barata: convierte ciclos de RCP a
// instrucciones de invitado con una multiplicacion de 64 bits y una division por el
// denominador del regulador, que solo se libra con el reciproco magico mientras el producto
// cabe en su limite -- pasado ese limite es un DIVQ, ~90 ciclos en Nehalem, DENTRO de un giro
// de 16384 vueltas. Y no hacia falta ninguna: en el giro lo unico que se mueve es el contador
// de ciclos que publica el RSP, y la conversion es monotona, asi que la comparacion se puede
// invertir una sola vez a la entrada y el giro queda en leer el contador y compararlo.
//
//   salir  <=>  now < spBarrierAt()
//          <=>  now < ops(spKickEdge + cyclesRun - spKickCycles)
// con ops(x) = techo(x*num/den):
//          <=>  techo(x*num/den) > now  <=>  x*num > now*den  <=>  x >= suelo(now*den/num) + 1
// y suelo(now*den/num) es exactamente rcpOpsToCycles(now), la inversa que ya existia.
// Queda un umbral fijo sobre cyclesRun. Es la MISMA condicion, ni una vuelta antes ni despues:
// no cambia el estado del invitado, solo deja de recalcular lo que no cambia.
// Adelanto permanente de la CPU sobre la barrera del SP. Ver kRdvLead y spEndArm en memory.hpp.
// Politica igual que dpLogLeadOn(): sin variable (o "auto") manda el modo de velocidad --
// puesto en "libre", el de jugar, y quitado en "Fiel a consola" (KESTREL_SPEEDMODE=hw), donde
// threaded vuelve a coincidir con lockstep. Las puertas lo fijan a 0.
// Barrido en Perfect Dark (PAL, F993->F1082, threaded+JIT+Parallel-RDP, minimo de tres):
// 1024 -> 2887 ms, 4096 -> 2728, 8192 -> 2689, 16384 -> 2676. Cada valor da su propio
// statehash, reproducible, con [frames] identico y 0 vencidos. De fabrica 8192: es donde se
// acaba la bajada, y en este juego el statehash que sale es ADEMAS el de lockstep
// (8654851006521c62) -- con 1024 y con 16384 no. SM64 da el mismo md5 con 1024 y con 8192.
// 2026-09-24. EL ADELANTO GRANDE NO ERA REPRODUCIBLE. Con 8192, SM64 (400 swaps,
// Parallel-RDP, JIT) daba statehash DISTINTO entre corridas del mismo binario: 5 valores
// distintos en 8 corridas. No es el punto de parada -- con parada por swaps el contador de
// ops es identico -- sino estado de verdad: el RSP quemaba ciclos distintos (420826365 vs
// 420770470) y hacia DMA distintos (540918 vs 540986 lecturas), y el rastro acababa en el bit
// BD de Cause, o sea la ultima interrupcion cayendo en otra instruccion. Con 0 renuncias y 0
// plazos vencidos: la fuga no es ninguna de las escotillas contadas.
// La causa es el adelanto en si. La cita de DMA obliga a la CPU a LLEGAR al reloj del RSP,
// pero no a no haberse pasado: con adelanto L la CPU ya ha escrito en RDRAM hasta L ops en el
// futuro del RSP, y lo que el RSP lee de esa zona depende de por donde vaya el anfitrion.
// Barrido en SM64, 6 corridas por valor: 0/64/256 dan un solo statehash; 512 da dos; 1024 y
// 4096 dan dos; 8192 dio uno en ese barrido y cinco en el de ocho corridas. DK64 (400 swaps)
// y Perfect Dark salen reproducibles en todos, asi que el margen depende del juego.
// Coste en Perfect Dark (1793 swaps, minimo de tres): 8192 -> 6041 ms, 1024 -> 6037,
// 256 -> 6235, 64 -> 6594, 0 -> 7423. O sea casi toda la ganancia esta en el adelanto
// PEQUENO: 256 cuesta un 3 % sobre 8192, y 0 cuesta un 23 %.
// Con 0 se pierde el 23 % y TAMPOCO se llega al estado de Lockstep (medido: PD lockstep
// 14995cfadedc7f38 vs threaded-0 0a64f3863fd236b1; SM64 lockstep b5f10ec436221af5 vs
// threaded-0 b6f551242339420c), asi que 0 no compra fidelidad, solo lentitud.
// 2026-09-24 (segunda vuelta). Media fuga tiene arreglo sin bajar el adelanto: es
// Memory::cpuRamWrBarrier. Una escritura de la CPU solo se ve desde el RSP cuando LLEGA a
// RDRAM, y eso pasa en dos sitios contados (vuelco de linea sucia de cache-D y escritura no
// cacheada), mas DMEM/IMEM, que el RSP ve en el acto. Se para la CPU en ESOS sitios con
// adelanto CERO y se la deja correr libre entre medias.
// SOLO MEDIA. Con la barrera puesta el adelanto sube a 512 y ahi se acaba: por encima SIGUE
// sin repetir. Barrido honesto en SM64, 400 swaps, 8-16 corridas por valor:
// 512 -> 16/16 un solo statehash; 1024 -> 8/2; 2048 -> 10/2; 4096 -> 7/3.
// Descartado que sea el grano del RSP (con KESTREL_RSPTANDA=64 filtra igual) y descartado
// que sea DMEM (extender la barrera ahi no limpio 1024 ni 4096). Los dos hashes de cada
// valor comparten los 28 bits bajos, o sea que se mueve UN campo, no el estado entero.
// Queda por cazar; es la misma pregunta que (g), por que Threaded no llega a Lockstep.
// A/B con el MISMO binario, intercalado, viejo (256 sin barrera) contra nuevo (512 con):
// SM64 6,97-7,09 -> 6,80-6,96 s; PD 5,85-5,94 -> 5,68-5,74; DK64 4,49-4,50 -> 4,36-4,39.
// Entre -2,5 % y -3 %. PD y DK64 dan el MISMO statehash que antes, o sea que en esos dos el
// cambio no toca la conducta; SM64 cambia de estado y repite 16/16.
// De fabrica 512 + barrera.
auto Memory::spLeadOps() -> u64 {
  static const u64 v = []() -> u64 {
    const char* e = std::getenv("KESTREL_SPLEAD");
    if(!e || !*e || !std::strcmp(e, "auto")) return rt::speedModeHw() ? 0ull : 512ull;
    return (u64)std::strtoull(e, nullptr, 0);
  }();
  return v;
}

auto Memory::spBarThreshold(u64 now) const -> u64 {
  const u64 t = rcpOpsToCycles(now) + 1 + spKickCycles;
  return t > spKickEdge ? t - spKickEdge : 0;
}

auto Memory::spBarrierWait(u64 now) -> void { LazyWaitMark rwm_;
  u64 bar = spBarrierEff();
  spBarSafe = bar;   // ver la nota de spBarSafe en memory.hpp
  if(now < bar || bar == spBarWaivedAt) return;
  // Los dos umbrales del giro: el de siempre y el de la cita, que deja al invitado adelantarse
  // kRdvLead. Cual manda lo decide rspRdvAt, que el RSP puede mover en mitad del giro; por eso
  // se precalculan los dos y dentro solo se elige. spKickEdge/spKickCycles no se mueven: los
  // escribe rspKick, que corre en ESTE hilo.
  rwm_.arm(cpuRcpWait);
  const u64 ldPlain  = spLeadOps();
  const u64 ldRdv    = kRdvLead > ldPlain ? kRdvLead : ldPlain;
  const u64 thrPlain = now >= ldPlain ? spBarThreshold(now - ldPlain) : 0;
  const u64 thrLead  = now >= ldRdv ? spBarThreshold(now - ldRdv) : 0;
  // Espera activa corta antes de dormir. Quien levanta esta barrera es el RSP publicando
  // su reloj, y cuando lo publica para pedir una cita de lectura del FIFO (dpReadSync) lo
  // que falta son decenas de instrucciones de CPU. Dormir 200 us para eso convierte cada
  // cita en un viaje de ida y vuelta de milisegundos, y hay millones de citas por corrida.
  u32 k = 0;
  const u32 lim = barSpinLen();
  // El giro tambien es ESPERA, y no se contaba: `spBarBlockNs` solo sumaba el sueno de abajo,
  // asi que el latido decia "barSP 0,6 %" mientras el hilo de CPU se dejaba ahi 120 M de
  // vueltas por corrida de PD -- que es de donde salia el grueso de `rcpRetire` en el perfil
  // de anfitrion. Dos lecturas de reloj por llamada. Cuenta de mas lo que `dpLogApply` haga
  // dentro del giro, que es trabajo y no espera, pero son unos pocos miles de llamadas.
  const auto spinT0 = std::chrono::steady_clock::now();
  // PROBADO Y DESCARTADO (2026-09-24): espaciar las lecturas AUXILIARES de este giro. De las
  // cinco lineas que mira, la unica que se mueve en el caso normal es `rsp.cyclesRun`; las otras
  // cuatro -- `rcpPend`, `rspPark`, `rspRdvAt` y `rspLogWait` -- las escribe el otro hilo un
  // punado de veces por corrida. Mirarlas 1 de cada 16 vueltas, como hace KESTREL_RDVCHEAP en el
  // lado del RSP, baja las vueltas por llamada de 126 a 120 y deja la PARED IGUAL (min de 3:
  // 6302 contra 6314 ms). Misma leccion que el retroceso por linea de cache: aqui no se espera
  // por trafico de coherencia, se espera por TIEMPO, y abaratar la vuelta solo da mas vueltas.
  for(; k < lim; ++k) {
    if(!(rcpPend.load(std::memory_order_acquire) & 8u)) break;
    // Camino raro (RSP aparcado): la barrera no sale de la conversion sino del tope del
    // aparcamiento, asi que ahi se pregunta entero.
    if(rspPark.load(std::memory_order_acquire)) { if(now < spBarrierEff()) break; }
    else if(rsp.cyclesRun.load(std::memory_order_acquire)
            >= (rspRdvAt.load(std::memory_order_acquire) ? thrLead : thrPlain)) break;
    // El RSP esperando a que apliquemos su diario no va a mover el reloj: aplicar aqui.
    if(rspLogWait.load(std::memory_order_acquire)) dpLogApply(now);
    if((k & kSpinPauseMask) == kSpinPauseMask) spinPause();
  }
  addOwned(barSpinTurns, k);   // solo escribe el hilo de CPU
  bumpOwned(barSpinCalls);
  if(k) {
    u64 sdt = (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - spinT0).count();
    cpuWaitNs.fetch_add(sdt, std::memory_order_relaxed);
    spBarBlockNs.fetch_add(sdt, std::memory_order_relaxed);
  }
  if(k < lim) return;
  // RSP aparcado con tope: llegar a la barrera ES llegar al tope, que es lo que espera el RSP
  // para despertar. Sin este aviso dormia hasta el vencimiento de su condvar.
  if(rspPark.load(std::memory_order_acquire)) {
    { std::lock_guard<std::mutex> g(parkMx); }
    parkCv.notify_all();
  }
  u64 waited = 0;
  for(;;) {
    auto t0 = std::chrono::steady_clock::now();
    {
      std::unique_lock<std::mutex> lk(rspMx);
      rspWaiters.fetch_add(1);
      rspCv.wait_for(lk, std::chrono::microseconds(200), [&]{
        return !(rcpPend.load(std::memory_order_acquire) & 8u) || spBarrierEff() != bar
            || rspLogWait.load(std::memory_order_acquire); });
      rspWaiters.fetch_sub(1);
    }
    if(rspLogWait.load(std::memory_order_acquire)) dpLogApply(now);
    u64 dt = (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now() - t0).count();
    cpuWaitNs.fetch_add(dt, std::memory_order_relaxed);
    spBarBlockNs.fetch_add(dt, std::memory_order_relaxed);
    if(!(rcpPend.load(std::memory_order_acquire) & 8u)) return;   // tarea terminada
    u64 next = spBarrierEff();
    if(next != bar) { bar = next; waited = 0; if(now < bar) return; continue; }
    waited += dt;
    // Salvavidas, igual que el del regulador: si el RSP deja de avanzar (microcodigo que
    // espera algo de la CPU) la barrera se suelta para ESTE valor. Nunca via de bloqueo.
    if(waited > kBarrierMaxWait) { spBarWaivedAt = bar; spBarWaives.fetch_add(1, std::memory_order_relaxed); return; }
  }
}

// KESTREL_DMABARRIER=0 quita la barrera de las transferencias que escriben RDRAM desde el
// hilo de CPU (PI del cartucho y del guardado, SI del PIF). Solo para A/B: de fabrica va
// ENCENDIDA porque es la misma semantica que la de los stores.
auto Memory::dmaBarrierOn() -> bool {
  static const bool v = [] {
    const char* e = std::getenv("KESTREL_DMABARRIER");
    if(!e || !*e) return true;
    return std::strcmp(e, "0") && std::strcmp(e, "off");
  }();
  return v;
}

auto Memory::wrBarrierOn() -> bool {
  static const bool v = [] {
    const char* e = std::getenv("KESTREL_WRBARRIER");
    if(!e || !*e) return true;   // de fabrica ENCENDIDA: es semantica de la maquina, no un ajuste
    return std::strcmp(e, "0") && std::strcmp(e, "off");
  }();
  return v;
}

// BARRERA DE ESCRITURA. La barrera del SP para a la CPU en CADA instruccion en cuanto se pasa
// del reloj del RSP, y eso SERIALIZA los dos hilos: medido en PD con el perfilador de anfitrion
// (2026-09-24), el hilo del RSP se pasa el 44,8 % de su tiempo dentro de spReadSync esperando a
// la CPU y el de CPU el 27 % dentro de spBarrierWait esperando al RSP. Turnandose, no
// solapando, que es justo lo contrario de para lo que estan los hilos.
//
// Pero lo UNICO que el RSP puede ver de la CPU es RDRAM: su bus solo llega a DMEM/IMEM y lo
// toca por DMA, y los registros (SP_STATUS, DPC) ya van fechados por sus diarios. Y un store
// cacheado de la CPU NO toca RDRAM: la unica via es el volcado de una linea sucia (CPU::dcFlush)
// y los accesos no cacheados. O sea que para garantizar que el RSP no lee jamas un byte de su
// futuro no hace falta parar a la CPU en cada instruccion: basta con pararla en las escrituras
// que de verdad llegan a RDRAM. Entre dos volcados la CPU puede correr libre.
//
// El plazo de fin de tarea del SP sigue protegido por la barrera normal (el adelanto acotado):
// esto no la sustituye, la complementa para poder subir ese adelanto sin que las escrituras
// se cuelen en el futuro del RSP.
//
// Sin abrazo mortal: la CPU solo espera aqui cuando va POR DELANTE del RSP, y el RSP solo
// espera en spReadSync cuando va por delante de la CPU. Las dos condiciones son excluyentes.
// Con el RSP aparcado no se espera: aparcado no puede leer RDRAM.
auto Memory::cpuRamWrBarrier(u64 now) -> void {
  if(!(rcpPend.load(std::memory_order_acquire) & 8u)) return;
  if(rspPark.load(std::memory_order_acquire)) return;
  const u64 thr = spBarThreshold(now);
  if(rsp.cyclesRun.load(std::memory_order_acquire) >= thr) return;
  bumpOwned(wrBarN);
  u32 k = 0;
  const u32 lim = barSpinLen();
  for(; k < lim; ++k) {
    if(!(rcpPend.load(std::memory_order_acquire) & 8u)) break;
    if(rspPark.load(std::memory_order_acquire)) break;
    if(rsp.cyclesRun.load(std::memory_order_acquire) >= thr) break;
    if(rspLogWait.load(std::memory_order_acquire)) dpLogApply(now);
    if((k & kSpinPauseMask) == kSpinPauseMask) spinPause();
  }
  addOwned(wrBarTurns, k);
  if(k < lim) return;
  u64 waited = 0;
  for(;;) {
    auto t0 = std::chrono::steady_clock::now();
    {
      std::unique_lock<std::mutex> lk(rspMx);
      rspWaiters.fetch_add(1);
      rspCv.wait_for(lk, std::chrono::microseconds(200), [&]{
        return !(rcpPend.load(std::memory_order_acquire) & 8u)
            || rspPark.load(std::memory_order_acquire)
            || rsp.cyclesRun.load(std::memory_order_acquire) >= thr
            || rspLogWait.load(std::memory_order_acquire); });
      rspWaiters.fetch_sub(1);
    }
    if(rspLogWait.load(std::memory_order_acquire)) dpLogApply(now);
    u64 dt = (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now() - t0).count();
    cpuWaitNs.fetch_add(dt, std::memory_order_relaxed);
    wrBarBlockNs.fetch_add(dt, std::memory_order_relaxed);
    if(!(rcpPend.load(std::memory_order_acquire) & 8u)) return;
    if(rspPark.load(std::memory_order_acquire)) return;
    if(rsp.cyclesRun.load(std::memory_order_acquire) >= thr) return;
    waited += dt;
    // Salvavidas, como las demas esperas: la fidelidad nunca puede ser via de bloqueo.
    if(waited > kBarrierMaxWait) { wrBarWaives.fetch_add(1, std::memory_order_relaxed); return; }
  }
}

auto Memory::rcpDeadlineOn() -> bool {
  static const bool v = [] {
    const char* e = std::getenv("KESTREL_RCPDEADLINE");
    return !e || !*e || (std::strcmp(e, "0") && std::strcmp(e, "off"));
  }();
  return v;
}

auto Memory::spEndArm(u64 cyclesUsed) -> void {
  // RETRASO FIJO DEL FIN DE TAREA CON ADELANTO PERMANENTE. Con kSpLeadAlways puesto la CPU
  // puede estar hasta L ops por delante del reloj del RSP, asi que el instante crudo del BREAK
  // puede nacer YA VENCIDO y habria que reencajarlo en cartNow() -- y cartNow() ahi es hora de
  // ANFITRION, o sea adios determinismo (medido: 193 plazos vencidos por corrida y statehash
  // distinto entre corridas del mismo binario). Sumando la MISMA L al instante del fin, el plazo
  // es funcion pura del instante del RSP y NUNCA puede nacer vencido: la barrera garantiza
  // cartNow() <= spBarrierAt() + L <= T + L. El precio es que MI_SP sube L ops de invitado mas
  // tarde, un retraso fijo y conocido, del mismo orden que la latencia real de la interrupcion
  // del SP en la consola. Con L=0 (defecto) no cambia nada.
  // PRUEBA (KESTREL_SPDATE=<ops>): separa el adelanto de la BARRERA del desplazamiento con
  // que se FECHA el fin de tarea. Por defecto son el mismo valor (spLeadOps), que es lo que
  // garantiza que el plazo no nazca vencido. Con SPDATE menor el MI_SP sube antes -- mas fiel --
  // a cambio de arriesgar plazos vencidos (spLate), que son los que rompen el determinismo.
  static const s64 kSpDate = []{ const char* e = std::getenv("KESTREL_SPDATE");
                                 return (e && *e) ? (s64)std::strtoll(e, nullptr, 10) : (s64)-1; }();
  spDoneAt = spCycleAt(cyclesUsed) + (kSpDate < 0 ? spLeadOps() : (u64)kSpDate);
  spArms.fetch_add(1, std::memory_order_relaxed);
  if(spDoneAt < cartNow()) {
    const u64 nowOps = cartNow();
    spLate.fetch_add(1, std::memory_order_relaxed);
    // Cual de las tres escotillas dejo pasar a la CPU. `rspRdvAt` != 0 = estabamos dentro
    // de una cita y la CPU tenia concedido el adelanto kRdvLead; `rspPark` != 0 = el RSP
    // estaba aparcado y la barrera valia el tope del aparcamiento; si no es ninguna de las
    // dos, la barrera solto por pared (spBarWaives) o no estaba puesta.
    if(rspRdvAt.load(std::memory_order_acquire))    spLateLead.fetch_add(1, std::memory_order_relaxed);
    else if(rspPark.load(std::memory_order_acquire)) spLatePark.fetch_add(1, std::memory_order_relaxed);
    else                                            spLateOther.fetch_add(1, std::memory_order_relaxed);
    const u64 over = nowOps - spDoneAt;
    u64 prev = spLateOverMax.load(std::memory_order_relaxed);
    while(over > prev && !spLateOverMax.compare_exchange_weak(prev, over, std::memory_order_relaxed)) {}
  }
  // El plazo sustituye a la barrera: a partir de aqui el fin ya tiene instante propio y la
  // CPU solo tiene que esperar a que ese instante llegue.
  rcpPend.fetch_or(1u, std::memory_order_release);
  rcpPend.fetch_and(~8u, std::memory_order_release);
  spBarWaivedAt = ~0ull;
}

auto Memory::dpEndArm(u64 kickOps, u64 gclkUsed) -> void {
  dpEndArmAt(kickOps + rcpCyclesToOps(gclkUsed));
}

auto Memory::dpEndArmAt(u64 at) -> void {
  // Dos SYNC_FULL seguidos sin que el hilo de CPU haya pasado por rcpRetire: el segundo no
  // puede hacerse visible ANTES que el primero, asi que el plazo solo puede irse hacia
  // adelante. MI_DP es un bit de nivel, con publicarlo una vez basta.
  const bool wasDue = (rcpPend.load(std::memory_order_relaxed) & 2u) && dpDoneAt <= cartNow();
  if((rcpPend.load(std::memory_order_relaxed) & 2u) && at < dpDoneAt) at = dpDoneAt;
  dpDoneAt = at;
  // Lockstep corre en linea tras la instruccion los ciclos del RSP de (inicio, fin] de esa
  // instruccion, y lo que armen vence en el retiro siguiente. Sin paradas cada instruccion
  // avanza 1 y basta con `at >= ahora`; con una parada (fallo de cache, enclavamiento) la
  // instruccion avanza varios y una entrada del diario sellada dentro de ese hueco pero por
  // debajo de `ahora` vencia en este retiro: MI_DP una op antes en Threaded (junkrunner64 con
  // KESTREL_INTERLOCK=on). Un bloque del JIT nunca cruza la hora de una entrada (rcpDueIn),
  // asi que ahi `at >= ahora` sigue siendo la condicion entera.
  if(tlInRetire && tlDpLogApply && (tlDpLogAt >= cartNow() || tlDpLogAt > retireOpStart) && !wasDue)
    tlRetireArmed = true;
  dpArms.fetch_add(1, std::memory_order_relaxed);
  if(at < cartNow()) { dpLate.fetch_add(1, std::memory_order_relaxed);
    u64 ov = cartNow() - at;
    u64 m = dpLateMax.load(std::memory_order_relaxed);
    while(ov > m && !dpLateMax.compare_exchange_weak(m, ov)) {}
  }
  rcpPend.fetch_or(2u, std::memory_order_release);
}

// Publica el fin de tarea. `bits` limita a cual (1 = SP, 2 = DP); el plazo ya se comprobo.
auto Memory::rcpFlushPending(u32 bits) -> void {
  u32 pend = rcpPend.load(std::memory_order_acquire) & bits;
  if(pend & 1u) {
    rcpPend.fetch_and(~1u, std::memory_order_relaxed);
    rcp.sp_status.fetch_or(1u | 2u, std::memory_order_acq_rel);   // HALT | BROKE
    spRets.fetch_add(1, std::memory_order_relaxed);
    if(rcp.sp_intr_on_break.load(std::memory_order_acquire)) raiseIntr(MI_SP);
  }
  if(pend & 2u) {
    rcpPend.fetch_and(~2u, std::memory_order_relaxed);
    rcp.dpc_status &= ~(0x8u | 0x20u);   // pipe drained: clear START_GCLK | PIPE_BUSY
    if(!tlIsRspThread) cpuDpcView.st &= ~kDpcRunSt;
    rcp.dpSyncs++;                       // misma contabilidad que los dos caminos del RDP
    dpRets.fetch_add(1, std::memory_order_relaxed);
    raiseIntr(MI_DP);
  }
}

auto Memory::rcpRetire(u64 opStart) -> void {
  u32 pend = rcpPend.load(std::memory_order_acquire);
  retireCalls++;
  if(!pend) { retireFast++; return; }
  u64 now = cartNow();
  // Camino corto: con SOLO la barrera del SP armada y el reloj propio todavia por detras de
  // ella no hay nada que hacer, y preguntarlo cuesta leer el reloj del hilo del RSP. Ver la
  // nota de spBarSafe. El aparcamiento se queda fuera: su tope lo mueve el otro hilo.
  if(pend == 8u && now < spBarSafe && !rspPark.load(std::memory_order_relaxed)) { retireFast++; return; }
  retireSlow++;
  retireOpStart = opStart;
  // Las barreras PRIMERO. El plazo de MI_DP se arma ahora al lanzar el tramo, o sea que puede
  // vencer antes de que el anfitrion haya pintado un pixel de el; publicar la interrupcion ahi
  // le ensenaria al invitado un fotograma que todavia no existe en RDRAM. La barrera es justo
  // lo que garantiza que, cuando el reloj del invitado llega al cierre, el trabajo esta hecho.
  // Un plazo de MI_DP que arma el propio diario aplicado en ESTE retiro no vence en el: en
  // Lockstep el RSP en linea corre despues de rcpRetire, asi que lo que arma en `t` vence en el
  // retiro de t+1. Vencerlo aqui adelantaba MI_DP una op en Threaded (junkrunner64).
  tlRetireArmed = false; tlInRetire = true;
  if(pend & 16u) dpLogApply(now);
  if(pend & 32u) dpcMbCpu(now);
  if(pend & 4u) dpBarrierWait(now);
  if(pend & 8u) spBarrierWait(now);
  pend = rcpPend.load(std::memory_order_acquire);
  // Lo que el RSP haya apuntado mientras esperabamos, si ya toca (no puede quedar detras).
  if(pend & 16u) { dpLogApply(now); pend = rcpPend.load(std::memory_order_acquire); }
  tlInRetire = false;
  u32 due = 0;
  if((pend & 1u) && now >= spDoneAt) due |= 1u;
  if((pend & 2u) && now >= dpDoneAt && !tlRetireArmed) due |= 2u;
  if(due) rcpFlushPending(due);
}

auto Memory::rspSubmitKick() -> void {
  bool wakeRsp;
  {
    std::unique_lock<std::mutex> lk(rspMx);
    // Serialize against a worker that has finished its microcode (BREAK reached,
    // MI_SP already raised) but not yet cleared rspBusy. The caller has decided a
    // launch is due, so we hold the CPU for that short wind-down rather than drop
    // the task. This never blocks on a genuinely running RSP: mid-task HALT is
    // clear, so the write is a no-op and we are not called at all.
    rspWaiters.fetch_add(1);
    awaitReporting(lk, rspCv, [&]{ return !rspBusy.load(std::memory_order_acquire); },
                   [&](unsigned round){ reportRspStall(round, "poder lanzar la siguiente"); });
    rspWaiters.fetch_sub(1);
    // Un fin de tarea anterior que todavia no habia vencido no puede quedarse colgado detras
    // del siguiente lanzamiento: se publica ya. En la practica no pasa (el invitado lanza
    // despues de atender MI_SP), es una red de seguridad.
    if(rcpPend.load(std::memory_order_relaxed) & 1u) rcpFlushPending(1u);
    // Instante de invitado del lanzamiento y base de ciclos del RSP: el plazo de fin se mide
    // contra estos dos y los toma SIEMPRE el hilo de CPU, aqui, donde el invitado escribe
    // CLEAR_HALT. Ver la nota de spEndArm en memory.hpp.
    spMarkKick();
    rspBusy.store(true, std::memory_order_release);
    if(spBarrierOn() && rcpMode == RcpMode::Threaded && rcpDeadlineOn())
      rcpPend.fetch_or(8u, std::memory_order_release);
    // La barrera de la tarea NUEVA puede nacer POR DETRAS de la vieja (la vieja llevaba el
    // adelanto sumado), asi que el atajo de spBarSafe no vale ya.
    spBarSafe = 0;
    // La barrera nueva es un plazo que el permiso de la cadena del JIT en curso no conocia (el
    // lanzamiento es un store en mitad de ella). Antes lo cubria la guarda `rsp.brake` del
    // prologo; en Threaded con plazos esa guarda ya no se emite (ver jit.cpp), asi que se
    // anula el permiso igual que siDma o dpScheduleSpan.
    if(jitGuardPtr) *jitGuardPtr = 0;
    rspKick = true;
    wakeRsp = rspIdleWaiting;
    ev("sp.kick", rcp.sp_pc, rcp.sp_status.load());
  }
  // Solo hay que despertar a quien duerme: el worker (rspIdleWaiting, leido con el mutex con el
  // que el lo pone) o un esperador contado en rspWaiters. Con el worker girando (rspSpinLen) la
  // tienda de rspBusy de arriba ya le basta, y notify_all con esperador es una llamada al kernel.
  if(wakeRsp || rspWaiters.load(std::memory_order_acquire))
    rspCv.notify_all();   // ver nota en rdpSubmit: worker y drenador comparten condvar
  // KESTREL_SYNCRSP=1: la pareja de KESTREL_SYNCRDP. El RDP sigue en su hilo, pero la
  // CPU espera a que el microcodigo termine antes de seguir. Solo diagnostico.
  static const bool syncRsp = std::getenv("KESTREL_SYNCRSP") != nullptr;
  if(syncRsp) rspAwaitIdle();
}

// La pareja de rdpSpinLen para el worker del RSP: vueltas que gira entre tareas antes de dormir.
// Aqui no hay decenas de miles de lanzamientos por segundo sino unos cientos, pero cada uno
// que encuentra al worker dormido paga la llamada al kernel en el hilo de CPU y, sobre todo,
// la latencia de despertar un hilo en Windows, con la CPU del invitado esperando en la barrera
// del SP a que el RSP empiece a avanzar. Solo coste de anfitrion: el instante de lanzamiento
// y la base de ciclos los fija el hilo de CPU en rspSubmitKick. KESTREL_RSPSPIN=<n>.
static auto rspSpinLen() -> u32 {
  static const u32 v = []() -> u32 {
    const char* e = std::getenv("KESTREL_RSPSPIN");
    if(e && *e) { char* end = nullptr; long n = std::strtol(e, &end, 0);
                  if(end && !*end && n >= 0 && n <= 10'000'000) return (u32)n; }
    return 0u;
  }();
  return v;
}

auto Memory::rspWorkerLoop() -> void {
  tlIsRspThread = true;
  pinToPhysicalCore(2);
  hostprof::start("rsp");   // opt-in: KESTREL_HOSTPROF_WHO=rsp
  hostprof::gate(&rspBusy);   // no contar el sueno entre tareas: solo el coste de emular
  rspThreadH = selfThreadHandle();
  for(;;) {
    // rspBusy lo baja este mismo hilo al final de la tarea y lo sube rspSubmitKick con el mutex
    // cogido junto con rspKick: verlo subido es "hay lanzamiento". Si llega despues del giro,
    // el lanzador ve rspIdleWaiting (mismo mutex) y notifica.
    for(u32 k = 0, n = rspSpinLen(); k < n; k++) {
      if(rspBusy.load(std::memory_order_acquire)) break;
      if((k & kSpinPauseMask) == kSpinPauseMask) spinPause();
    }
    {
      std::unique_lock<std::mutex> lk(rspMx);
      rspIdleWaiting = true;
      rspCv.wait(lk, [&]{ return rspStop || rspKick; });
      rspIdleWaiting = false;
      if(rspStop && !rspKick) return;
      rspKick = false;
    }
    // Run the armed microcode to BREAK. run() = start()+step(budget); it updates
    // sp_status/sp_pc and raises MI_SP itself. Any DPC_END the microcode writes
    // funnels through mmioWrite → rdpSubmit, so the RDP pipelines behind us.
    auto t0 = std::chrono::steady_clock::now();
    const u64 park0 = rspParkNs.load(std::memory_order_relaxed);
    rsp.step(~0ull);   // start() ya corrio en el hilo CPU al escribir CLEAR_HALT
    // Ocupacion = tiempo DENTRO de la tarea menos lo que se paso dormido en el aparcamiento:
    // un hilo aparcado no esta trabajando. Ver rspParkNs.
    {
      u64 dt = (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(
                 std::chrono::steady_clock::now() - t0).count();
      u64 slept = rspParkNs.load(std::memory_order_relaxed) - park0;
      rspBusyNs.fetch_add(dt > slept ? dt - slept : 0, std::memory_order_relaxed);
    }
    rspJobsRun.fetch_add(1, std::memory_order_relaxed);
    ev("sp.done", rcp.sp_pc, rcp.sp_status.load());
    // El flag SE BAJA CON EL MUTEX COGIDO. Sin el, la tienda+notify puede caer entre el
    // chequeo del predicado del esperador (que vio rspBusy=true, bajo el mutex) y el
    // registro de su espera: la notificacion se pierde y la CPU duerme para siempre
    // esperando un RSP que ya termino. Es el patron que exige condition_variable —
    // el worker del RDP ya lo hacia asi.
    { std::lock_guard<std::mutex> lk(rspMx); rspBusy.store(false, std::memory_order_release);
      rcpPend.fetch_and(~8u, std::memory_order_release); }
    // Los que esperan el fin de tarea (drenador, lanzador, freno) se cuentan en rspWaiters antes
    // de mirar su predicado con el mutex; rspBusy ya bajo con ese mutex, asi que o lo cuentan
    // antes y aqui se ve, o miran despues y ven rspBusy=false.
    if(rspWaiters.load(std::memory_order_acquire)) rspCv.notify_all();
  }
}

auto Memory::startRcpThreads() -> void {
  if(const char* e = std::getenv("KESTREL_EVLOG")) { evOn = true; evLvl = std::atoi(e); }
  if(rcpMode != RcpMode::Threaded) return;
  if(!rdpWorker.joinable()) { rdpStop = false; rdpWorker = std::thread([this]{ rdpWorkerLoop(); }); }
  if(!rspWorker.joinable()) { rspStop = false; rspWorker = std::thread([this]{ rspWorkerLoop(); }); }
}

auto Memory::stopRcpThreads() -> void {
  { std::lock_guard<std::mutex> lk(rdpMx); rdpStop = true; }
  rdpCv.notify_all();
  if(rdpWorker.joinable()) rdpWorker.join();
  // El apagon primero: el worker puede estar dentro de una tarea que no va a terminar
  // (ver Rsp::hostStop). rspStop solo lo mira entre tareas.
  rsp.hostStop.store(true, std::memory_order_release);
  { std::lock_guard<std::mutex> lk(rspMx); rspStop = true; }
  rspCv.notify_all();
  if(rspWorker.joinable()) rspWorker.join();
}

}  // namespace kestrel
