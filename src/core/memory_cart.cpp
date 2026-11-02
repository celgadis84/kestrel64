// Cartridge storage: save types (EEPROM/SRAM/FlashRAM), Controller/Transfer Pak, SummerCart64 + SD.
// Split out of memory.cpp; shared internals live in memory_internal.hpp.
#include "memory.hpp"
#include "memory_internal.hpp"
#include "archive.hpp"
#include "../net/netplay.hpp"
#include "runtime.hpp"
#include <algorithm>

namespace kestrel {

auto Memory::saveSize() const -> u32 {
  switch(saveType) {
    case SaveType::Eeprom4k:  return 512;      //  4 kbit
    case SaveType::Eeprom16k: return 2048;     // 16 kbit
    case SaveType::Sram256k:  return 32 * 1024;   // 256 kbit
    case SaveType::Sram768k:  return 96 * 1024;   // 768 kbit (three 32 KiB banks)
    case SaveType::Flash1m:   return 128 * 1024;  //  1 Mbit
    case SaveType::Sram1m:    return 128 * 1024;  //  1 Mbit lineal (cabecera ED, homebrew)
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
auto mempakAddrCrc(u16 blockAddr) -> u8 {
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
auto mempakDataCrc(const u8* data) -> u8 {
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
auto mempakFormat(std::vector<u8>& p) -> void {
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
// Recoge el cartucho GB que el menu (o KESTREL_TPAK_ROMn) haya puesto en el Transfer Pak
// del puerto. Corre en el hilo de la CPU, dentro del joybus: el cambio cae entre dos
// transacciones. Cambiar de cartucho con uno ya puesto deja GBCART_PULL para la siguiente
// lectura de estado, que es como el SDK se entera (PFS_ERR_NEW_GBCART).
auto Memory::tpakSync(int port) -> void {
  PadPort& pp = padPort[port];
  u32 g = rt::tpakGen[port].load(std::memory_order_relaxed);
  if(g == pp.tpGen) return;
  pp.tpGen = g;
  std::string path;
  { std::lock_guard<std::mutex> lk(rt::tpakMx); path = rt::tpakRom[port]; }
  bool had = pp.gb.loaded();
  pp.gb.flush();
  if(path.empty()) pp.gb.clear();
  else if(!pp.gb.load(path))
    std::fprintf(stderr, "[tpak] puerto %d: no se pudo cargar la ROM de GB '%s'\n", port + 1, path.c_str());
  pp.tp.pull = had;
  pp.tp.cartOn = false;
}

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
  // Invitado de una partida en red: lo que hay en memoria es la partida del anfitrion.
  if(netplay::tainted()) return;
  for(const PadPort& pp : padPort) {
    if(pp.mempakDirty && !pp.mempakPath.empty()) storeFrom(pp.mempakPath, pp.mempak);
    pp.gb.flush();                   // RAM con bateria del cartucho GB del Transfer Pak
  }
  if(saveFilePath.empty() || saveType == SaveType::None || !saveDirty) return;
  storeFrom(saveFilePath, isEeprom() ? eeprom : saveRam);
}

// The backup save fitted to a cartridge (EEPROM 4k/16k, SRAM 256k/768k, FlashRAM) is
// not derivable from the ROM contents, so — like every N64 emulator — we resolve it
// from the game code in the header. The 4-byte code sits at 0x3B..0x3E; bytes
// 0x3C-0x3D are the two-character cart ID, 0x3E the region. This curated table lists
// the titles we are confident about; everything else keeps the EEPROM-16k default.
// KESTREL_SAVETYPE forces a type by name (eep4k/eep16k/sram256/sram768/sram1m/flash/none,
// o los nombres del lanzador eeprom4k/eeprom16k/sram256k/sram768k/flash1m; "auto" = no
// forzar), which is how any game can be brought up before it earns a table entry.
//
// Orden: KESTREL_SAVETYPE > cabecera avanzada de homebrew > tabla de IDs. La cabecera
// avanzada (n64brew "ROM Header", la de los flashcarts ED64/SummerCart): ID "ED" en
// 0x3C-0x3D y el tipo en el nibble alto de 0x3F (0 nada, 1 EEPROM 4k, 2 EEPROM 16k,
// 3 SRAM 256k, 4 SRAM 768k en tres bancos, 5 FlashRAM 1M, 6 SRAM 1M lineal). Es lo que
// escribe la toolchain de libdragon (n64tool --header / SAVE en el proyecto); sin leerla una
// ROM de SDK con SRAM arrancaba con EEPROM y no guardaba.
auto Memory::resolveSaveType() -> void {
  saveType = SaveType::Eeprom16k;                  // default; overridden below
  const char* env = std::getenv("KESTREL_SAVETYPE");
  std::string s = env ? env : "";
  if(!s.empty() && s != "auto") {
    if(s == "none")                               saveType = SaveType::None;
    else if(s == "eep4k"   || s == "eeprom4k")    saveType = SaveType::Eeprom4k;
    else if(s == "eep16k"  || s == "eeprom16k")   saveType = SaveType::Eeprom16k;
    else if(s == "sram256" || s == "sram256k")    saveType = SaveType::Sram256k;
    else if(s == "sram768" || s == "sram768k")    saveType = SaveType::Sram768k;
    else if(s == "sram1m")                        saveType = SaveType::Sram1m;
    else if(s == "flash"   || s == "flash1m")     saveType = SaveType::Flash1m;
    else std::fprintf(stderr, "[save] KESTREL_SAVETYPE='%s' desconocido, EEPROM 16k\n", env);
  } else if(rom.size() > 0x3F && rom[0x3C] == 'E' && rom[0x3D] == 'D') {
    static const SaveType kEd[] = { SaveType::None, SaveType::Eeprom4k, SaveType::Eeprom16k,
                                    SaveType::Sram256k, SaveType::Sram768k, SaveType::Flash1m,
                                    SaveType::Sram1m };
    u32 t = rom[0x3F] >> 4;
    if(t < std::size(kEd)) saveType = kEd[t];
    else std::fprintf(stderr, "[save] cabecera ED con tipo %u desconocido, EEPROM 16k\n", t);
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
  // Reloj de cartucho: KESTREL_RTC=1/0 manda; si no, bit 0 de 0x3F en la cabecera ED.
  {
    const char* e = std::getenv("KESTREL_RTC");
    rtc = Rtc{};
    rtc.on = e && e[0] ? e[0] == '1'
                       : rom.size() > 0x3F && rom[0x3C] == 'E' && rom[0x3D] == 'D' && (rom[0x3F] & 1);
  }
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
    // Tres bancos de 32 KiB elegidos por los bits 18-19 de la direccion: 0x08000000,
    // 0x08040000, 0x08080000 (Dezaemon 3D comprueba su partida leyendo 0x08040000; mapa de
    // cartucho de ares y n64js PR #84). Antes iban a 64 KiB uno de otro, bits 16-17.
    u32 bank = off >> 18;
    return bank < 3 && (off & 0x3ffff) < 0x8000 ? bank * (32 * 1024) + (off & 0x7fff) : 0xffff'ffff;
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
    if(flashMode != FlashMode::Read) {                // status/id window (8 bytes, repeats)
      u8 st[8];
      for(int i = 0; i < 8; i++) st[i] = (u8)(flashStatus >> (56 - 8 * i));
      u32 o = (phys - 0x0800'0000) & 7;
      return bePick(st + o, 8 - o);
    }
    u32 off = flashArrayOff(phys);                     // Read mode: array data
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
  if(isFlash()) {
    if(flashMode == FlashMode::Status) {                // escritura = registro de estado
      if(((phys - 0x0800'0000) & 0xffff) < 4) {         // (libultra: 0xD2 + 0 lo limpia)
        const u64 sr = value & 0xff;
        flashStatus = (flashStatus & ~(0xffull << 32)) | (sr << 32);
      }
      return;
    }
    put(flashPageBuf, sizeof flashPageBuf, (phys - 0x0800'0000) & 0x7f);   // bufer de pagina
    return;
  }
  if(isSram()) {
    u32 off = saveOffset(phys);
    if(off != 0xffff'ffffu) { put(saveRam.data(), (u32)saveRam.size(), off); saveDirty = true; }
  }
}

// Desplazamiento en la matriz para una direccion PI en modo Read. El chip que anunciamos
// (id 0x00C2001E = MX "C") es de los ANTIGUOS: libultra (`__osFlashGetAddr`, `osFlashInit`)
// los marca OLD_FLASH porque sus lineas de direccion van corridas un bit, y les pide la
// pagina N en N*64 en vez de N*128. El chip recibe esa direccion de arranque y su contador
// interno avanza linealmente durante la rafaga, asi que solo el ARRANQUE va por dos: la DMA
// lo pasa una vez (`piDma`) y sigue byte a byte. Con identidad, cualquier pagina >= 1 se leia
// de la mitad de su sitio.
auto Memory::flashArrayOff(u32 phys) const -> u32 {
  const u32 id = (u32)flashStatus;
  const bool old = id == 0x00C2'0000u || id == 0x00C2'0001u || id == 0x00C2'001Eu;
  const u32 off = (phys - 0x0800'0000) & 0xffff;
  return old ? off * 2 : off;
}

// FlashRAM command register (0x08010000). Protocolo de libultra (src/flash/*.c), que es el
// que corrio en consola: 0x4B|pagina (sector) o 0x3C (chip) eligen QUE borrar y 0x78 lo
// EJECUTA; 0xB4 abre el bufer de pagina (la DMA de 128 B lo llena) y 0xA5|pagina lo
// PROGRAMA; 0xD2 solo pasa a modo estado; 0xE1 id; 0xF0 lectura. libultra sondea el estado
// leyendo 0x08000000 justo tras 0x78/0xA5, SIN 0xD2 por medio, y luego lo limpia con 0xD2 +
// escritura de 0. Antes 0x78/0xA5 solo cambiaban de modo y el trabajo esperaba a 0xD2 (modelo
// heredado de los emuladores viejos): el sondeo leia datos de la matriz como estado y podia
// girar para siempre si el byte 3 de la partida tenia el bit de ocupado. El registro de estado
// es el byte bajo de la palabra alta de flashStatus (0x1111800S): bit0 programando, bit1
// borrando, bit2 programa OK, bit3 borrado OK. Sector = 128 paginas = 16 KiB (mupen64plus,
// `erase_page & 0xff80`); el MiSTer borra 128 B, sin oraculo de hardware que lo zanje -- con
// el patron de libultra (borrar sector, programar sus paginas) los dos dan lo mismo.
auto Memory::flashCommand(u32 cmd) -> void {
  auto setSr = [&](u32 clr, u32 set) {
    u32 sr = (u32)(flashStatus >> 32) & 0xff;
    sr = (sr & ~clr) | set;
    flashStatus = (flashStatus & ~(0xffull << 32)) | ((u64)sr << 32);
  };
  switch(cmd >> 24) {
    case 0xE1: flashMode = FlashMode::Status; setSr(0, 1); break;   // id; bit0 como mupen (PPL)
    case 0xF0: flashMode = FlashMode::Read; break;
    case 0xD2: flashMode = FlashMode::Status; break;
    case 0x3C: flashErasePage = ~0u; break;                          // chip entero
    case 0x4B: flashErasePage = cmd & 0xffff; break;
    case 0x78: {
      u32 base = 0, len = (u32)saveRam.size();
      if(flashErasePage != ~0u) { base = (flashErasePage & 0xff80) * 128; len = 128 * 128; }
      for(u32 i = 0; i < len && base + i < saveRam.size(); i++) saveRam[base + i] = 0xFF;
      saveDirty = true;
      setSr(2, 8); flashMode = FlashMode::Status;
      break;
    }
    case 0xB4: flashMode = FlashMode::Write; break;
    case 0xA5: {
      flashWritePage = cmd & 0xffff;
      u32 base = flashWritePage * 128;
      for(u32 i = 0; i < 128 && base + i < saveRam.size(); i++) saveRam[base + i] = flashPageBuf[i];
      saveDirty = true;
      setSr(1, 4); flashMode = FlashMode::Status;
      break;
    }
    default: break;
  }
}

// --- SummerCart64 ------------------------------------------------------------
// Protocolo segun el firmware de SummerCart64 v2 (registros SCR) y como lo usa libcart
// (sc_init / sc_card_*): 0x1FFF0000 STATUS (lectura) / COMMAND (escritura), +4 DATA0,
// +8 DATA1, +0xC IDENTIFIER = 'SCv2', +0x10 KEY. Los registros solo responden tras la
// secuencia de llave 0 -> '_UNL' -> 'OCK_'; 0xFFFFFFFF los vuelve a cerrar. El buffer de
// datos de 8 KiB vive en 0x1FFE0000 y la SDRAM del cartucho, que es donde esta la ROM, en
// 0x10000000 (64 MiB). Los comandos acaban en el acto: STATUS nunca da CMD_BUSY, que es un
// instante valido para el hardware (el juego sondea hasta que baja).

auto Memory::attachSdCard(const std::string& romPath) -> void {
  scOn = false; sdTried = false; scKey = 0; scSdInit = false; scRomWrite = false; scSwap = false;
  sd.close();
  const char* env = std::getenv("KESTREL_SD");
  if(env && std::string(env) == "0") return;
  if(env && *env) sdPath = env;
  else {
    const auto slash = romPath.find_last_of("/\\");
    sdPath = slash == std::string::npos ? "." : romPath.substr(0, slash + 1);
  }
  scOn = true;
  scBuf.assign(kScBufSz, 0);
  // La SDRAM del flashcart es de 64 MiB y la ROM solo ocupa su principio; lo de despues se
  // vuelve memoria escribible con CFG_ROM_WRITE. Se reserva ya para que crecer no mueva rom.
  rom.reserve(kScSdram);
}

// La tarjeta se monta la primera vez que el juego la inicia: leer la carpeta cuesta, y un
// juego que no la usa no debe pagarlo.
static auto scMount(Memory& m) -> void {
  if(m.sdTried) return;
  m.sdTried = true;
  if(m.sd.open(m.sdPath))
    std::fprintf(stderr, "[sd] SummerCart64: tarjeta %s (%s, %llu sectores)\n", m.sdPath.c_str(),
                 m.sd.isVirtual() ? "carpeta como FAT32" : "imagen", (unsigned long long)m.sd.sectors());
  else
    std::fprintf(stderr, "[sd] SummerCart64: '%s' no se pudo abrir, sin tarjeta\n", m.sdPath.c_str());
}

auto Memory::scBufAt(u32 phys) -> u8* {
  if(phys >= kScBuf && phys < kScBuf + kScBufSz) return scBuf.data() + (phys - kScBuf);
  if(phys >= BASE_CART && phys - BASE_CART < rom.size()) return rom.data() + (phys - BASE_CART);
  return nullptr;
}

auto Memory::scDmaWrite(u32 cart, u8 b) -> bool {
  if(cart >= kScBuf && cart < kScBuf + kScBufSz) { scBuf[cart - kScBuf] = b; return true; }
  if(scRomWrite && cart >= BASE_CART && cart - BASE_CART < rom.size()) { rom[cart - BASE_CART] = b; return true; }
  return false;
}

auto Memory::scRead(u32 phys, u32& out) -> bool {
  if(scKey != 2) return false;
  if(phys >= kScBuf && phys < kScBuf + kScBufSz) {
    const u32 o = (phys - kScBuf) & ~3u;
    out = (u32)scBuf[o] << 24 | (u32)scBuf[o + 1] << 16 | (u32)scBuf[o + 2] << 8 | scBuf[o + 3];
    return true;
  }
  if(phys < kScReg || phys >= kScReg + 0x20) return false;
  switch(phys & 0x1c) {
    case 0x00: out = scErr ? 0x4000'0000u : 0; break;
    case 0x04: out = scData0; break;
    case 0x08: out = scData1; break;
    case 0x0c: out = 0x5343'7632u; break;   // "SCv2"
    default:   out = 0; break;
  }
  return true;
}

auto Memory::scWrite(u32 phys, u32 value) -> bool {
  if((phys & ~3u) == kScReg + 0x10) {       // KEY: siempre escucha
    if(value == 0xffff'ffffu) scKey = 0;
    else if(value == 0) { if(scKey != 2) scKey = 0; }
    else if(value == 0x5f55'4e4cu && scKey == 0) scKey = 1;   // "_UNL"
    else if(value == 0x4f43'4b5fu && scKey == 1) scKey = 2;   // "OCK_"
    else if(scKey != 2) scKey = 0;
    return true;
  }
  if(scKey != 2) return false;
  if(phys >= kScBuf && phys < kScBuf + kScBufSz) {
    const u32 o = (phys - kScBuf) & ~3u;
    for(u32 i = 0; i < 4; i++) scBuf[o + i] = (u8)(value >> (24 - 8 * i));
    return true;
  }
  if(phys < kScReg || phys >= kScReg + 0x20) return false;
  switch(phys & 0x1c) {
    case 0x00: scCommand(value & 0xff); break;
    case 0x04: scData0 = value; break;
    case 0x08: scData1 = value; break;
    default: break;
  }
  return true;
}

auto Memory::scCommand(u32 cmd) -> void {
  scErr = false;
  switch(cmd) {
    case 'c': case 'C': {                   // CONFIG_GET / CONFIG_SET (SET devuelve el anterior)
      u32 old = 0;
      if(scData0 == 1) old = scRomWrite ? 1 : 0;   // CFG_ROM_WRITE; DD_MODE y SAVE_TYPE = 0
      if(cmd == 'C' && scData0 == 1) {
        scRomWrite = scData1 != 0;
        if(scRomWrite && rom.size() < kScSdram) rom.resize(kScSdram, 0);   // dentro de lo reservado
      }
      scData1 = old;
      break;
    }
    case 'i':                               // SD_OP
      switch(scData1) {
        case 0: scSdInit = false; break;
        case 1: scMount(*this); if(sd.present()) scSdInit = true; else scErr = true; break;
        case 2: scMount(*this); scData1 = (sd.present() ? 1u : 0u) | (scSdInit ? 2u : 0u); break;
        case 3: if(u8* p = scBufAt(scData0)) std::memset(p, 0, 32); break;   // CSD+CID: sin datos
        case 4: scSwap = true; break;
        case 5: scSwap = false; break;
        default: scErr = true; break;
      }
      break;
    case 'I': scSector = scData0; break;    // SD_SECTOR_SET
    case 's': case 'S': {                   // SD_READ / SD_WRITE: DATA0 = direccion, DATA1 = sectores
      if(!scSdInit) { scErr = true; break; }
      u8 tmp[512];
      for(u32 k = 0; k < scData1; k++) {
        const u32 a = scData0 + k * 512;
        if(!scBufAt(a) || !scBufAt(a + 511)) { scErr = true; break; }
        if(cmd == 's') {
          if(!sd.read(scSector, tmp)) { scErr = true; break; }
          const bool sw = scSwap && a < kScBuf;
          for(u32 i = 0; i < 512; i++) *scBufAt(a + i) = tmp[sw ? i ^ 1 : i];
        } else {
          for(u32 i = 0; i < 512; i++) tmp[i] = *scBufAt(a + i);
          if(!sd.write(scSector, tmp)) { scErr = true; break; }
        }
        scSector++;
      }
      break;
    }
    case 'M': {                             // USB_WRITE: DATA0 = direccion, DATA1 = tipo<<24 | longitud
      const u32 type = scData1 >> 24, len = scData1 & 0xff'ffffu;
      std::string pkt;
      pkt.reserve(len);
      for(u32 i = 0; i < len; i++) {
        const u8* p = scBufAt(scData0 + i);
        if(!p) { scErr = true; break; }
        pkt.push_back((char)*p);
      }
      if(scErr) break;
      // Trama del SDK (k64/usb.h): 'K64' + tipo + longitud BE de 32 bits + datos. Si llega
      // envuelta asi se desenvuelve; si no, el paquete es el dato tal cual.
      u32 t = type;
      std::string body = pkt;
      if(pkt.size() >= 8 && pkt[0] == 'K' && pkt[1] == '6' && pkt[2] == '4') {
        t = (u8)pkt[3];
        const u32 n = (u32)(u8)pkt[4] << 24 | (u32)(u8)pkt[5] << 16 | (u32)(u8)pkt[6] << 8 | (u8)pkt[7];
        body = pkt.substr(8, std::min<usize>(n, pkt.size() - 8));
      }
      if(usbEcho < 0) { const char* e = std::getenv("KESTREL_USB"); usbEcho = e && *e && *e != '0'; }
      std::lock_guard<std::mutex> lk(usbMx);
      usbPackets++; usbBytes += len; usbLastType = t;
      if(t == 1) {
        usbText += body;
        if(usbText.size() > kUsbKeep) usbText.erase(0, usbText.size() - kUsbKeep);
        if(usbEcho) { std::fwrite(body.data(), 1, body.size(), stdout); std::fflush(stdout); }
      }
      break;
    }
    case 'U': scData0 = 0; break;           // USB_WRITE_STATUS: bit31 = ocupado -> nunca
    case 'u': scData0 = 0; scData1 = 0; break;   // USB_READ_STATUS: nada pendiente
    default: scErr = true; break;
  }
}

}  // namespace kestrel
