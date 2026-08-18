#include "memory.hpp"
#include "../audio/audio.hpp"
#include "../vrdp/vrdp.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <string_view>

namespace kestrel {

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

auto Memory::reset(bool expansionPak) -> void {
  rdram.assign(expansionPak ? RDRAM_SIZE_EXPANDED : 0x0040'0000, 0);
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
  if(const char* b = std::getenv("KESTREL_BUTTONS")) padButtons = (u32)strtoul(b, nullptr, 16);
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
  resolveSaveType();
  initMap();  // refresh so CART_ROM appears
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

// Mirror the battery/flash backing to a file beside the ROM. The extension follows the
// mupen/ares convention so saves are interchangeable: .eep (EEPROM), .sra (SRAM), .fla
// (FlashRAM). The path is the ROM path with its extension replaced.
auto Memory::attachSaveFile(const std::string& romPath) -> void {
  saveFilePath.clear();
  if(saveType == SaveType::None) return;
  const char* ext = isEeprom() ? ".eep" : isFlash() ? ".fla" : ".sra";
  usize dot = romPath.find_last_of('.');
  usize slash = romPath.find_last_of("/\\");
  // Only strip an extension that belongs to the final path component.
  if(dot == std::string::npos || (slash != std::string::npos && dot < slash))
    saveFilePath = romPath + ext;
  else
    saveFilePath = romPath.substr(0, dot) + ext;

  std::vector<u8>* back = isEeprom() ? &eeprom : &saveRam;
  if(FILE* f = std::fopen(saveFilePath.c_str(), "rb")) {
    std::fseek(f, 0, SEEK_END);
    long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if(n > 0) {
      usize want = saveSize();
      usize rd = (usize)n < want ? (usize)n : want;   // tolerate short/long files
      if(back->size() < want) back->resize(want, isFlash() ? 0xFF : 0x00);
      std::fread(back->data(), 1, rd, f);
      std::printf("[save] loaded %s (%zu bytes)\n", saveFilePath.c_str(), rd);
    }
    std::fclose(f);
  }
}

auto Memory::flushSaveFile() const -> void {
  if(saveFilePath.empty() || saveType == SaveType::None || !saveDirty) return;
  const std::vector<u8>& back = isEeprom() ? eeprom : saveRam;
  if(back.empty()) return;
  if(FILE* f = std::fopen(saveFilePath.c_str(), "wb")) {
    std::fwrite(back.data(), 1, back.size(), f);
    std::fclose(f);
  }
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
      if(std::getenv("KESTREL_FPDBG")) {
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

// CPU read from cart space. If a write recently latched a value onto the PI bus
// (and it has not decayed), that value is returned and consumed. Otherwise a real
// ROM read, mangled by the 16-bit-wide PI bus: sub-word reads only reach the UPPER
// halfword of each 32-bit word (a LB/LH of the lower half aliases to the upper).
auto Memory::cartRead(u32 phys, u32 nbytes) -> u32 {
  u32 isvVal = 0;
  if(isvRead(phys, nbytes, isvVal)) return isvVal;
  u64 now = cartClock ? *cartClock : 0;
  if(cartLatchValid && now < cartLatchExpiry) {
    u32 v = cartLatch;
    cartLatchValid = false;
    rcp.pi_status &= ~0x2u;                 // reading the bus clears IOBUSY
    if(nbytes == 1) return (v >> 24) & 0xff;
    if(nbytes == 2) return (v >> 16) & 0xffff;
    return v;
  }
  if(cartLatchValid && now >= cartLatchExpiry) { cartLatchValid = false; rcp.pi_status &= ~0x2u; }
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
  u64 now = cartClock ? *cartClock : 0;
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
  cartLatchExpiry = now + CART_LATCH_TTL;
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
  for(u32 j = phys; j < end; j++) {
    u32 a = page | (j & 0x7ff);
    if(a < rdram.size()) rdram[a] = pat[j & (unit - 1)];
  }
}

auto Memory::write8(u32 addr, u8 value) -> void {
  if(isvWrite(addr & 0x1fff'ffff, value, 1)) return;
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
    if(a >= BASE_SP_PC) return (off & 0xff) == 0 ? rcp.sp_pc : 0;
    switch(off & 0xff) {
    case 0x00: return rcp.sp_mem_addr;
    case 0x04: return rcp.sp_dram_addr;
    case 0x08: return rcp.sp_rd_len;
    case 0x0c: return rcp.sp_wr_len;
    case 0x10: return rcp.sp_status.load(std::memory_order_acquire)
                    | (rcp.sp_intr_on_break ? 0x40u : 0u);  // bit6 = INTR_ON_BREAK
    case 0x14: return (rcp.sp_status.load(std::memory_order_relaxed) >> 2) & 1;   // SP_DMA_FULL
    case 0x18: return (rcp.sp_status.load(std::memory_order_relaxed) >> 2) & 1;   // SP_DMA_BUSY
    case 0x1c: { u32 s = rcp.sp_semaphore; rcp.sp_semaphore = 1; return s; }  // read sets
    }
    return 0;
  case BASE_DPC & 0x1ff0'0000:
    switch(off & 0xff) {
    case 0x00: return rcp.dpc_start;
    case 0x04: return rcp.dpc_end;
    case 0x08: return rcp.dpc_current;
    case 0x0c: return rcp.dpc_status | 0x80u;   // COMMAND_BUFFER_READY: the soft RDP is
                                                 // always ready to accept a new command list
    // Performance counters, 24-bit each. The RDP accumulates them per rasterized
    // span (see SoftRdp::accountPixels); games time the RDP with these.
    case 0x10: return rcp.dpc_clock.load(std::memory_order_relaxed)    & 0xff'ffff;
    case 0x14: return rcp.dpc_bufbusy.load(std::memory_order_relaxed)  & 0xff'ffff;
    case 0x18: return rcp.dpc_pipebusy.load(std::memory_order_relaxed) & 0xff'ffff;
    case 0x1c: return rcp.dpc_tmem.load(std::memory_order_relaxed)     & 0xff'ffff;
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
      // derive it from the retired-instruction clock (CPI≈1) at ~60 fields/s.
      u32 total = rcp.vi_vsync ? (rcp.vi_vsync & 0x3ff) : 525;
      if(total < 2) total = 525;
      u64 cph = 93'750'000ull / (60ull * total);   // CPU cycles per halfline
      if(cph == 0) cph = 1;
      u64 cyc = cartClock ? *cartClock : 0;
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
    case 0x10: return rcp.pi_status;
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
      if(v & (1 << 7)) rcp.mi_repeat_on = false;                              // clear init/repeat mode
      if(v & (1 << 8)) { rcp.mi_repeat_on = true; rcp.mi_repeat_len = (v & 0x7f) + 1; }  // arm: span = length+1
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
      // Threaded: a launch write (lone CLEAR_HALT) must land *after* the previous task
      // published its own status. The worker sets HALT|BROKE from inside rsp.run(); if
      // the CPU cleared HALT first and that late store landed afterwards, the CPU would
      // read a spurious "task finished" and pull the output DMEM before the new task
      // ever wrote it. The single real RSP orders these implicitly. Only launches wait —
      // a mid-task SP_STATUS poke (abort, signal bits) must never block on the worker.
      if(rcpMode == RcpMode::Threaded && (v & (1u << 0)) && !(v & (1u << 1))) rspAwaitIdle();
      // HALT as the CPU sees it *before* this write. A CLEAR_HALT only launches the
      // core when it was actually halted; arriving mid-task it is a no-op, because
      // the bit it clears is already clear. This is the launch condition — not any
      // emulator-side "is the worker thread busy" bookkeeping.
      bool wasHalted = (rcp.sp_status.load(std::memory_order_acquire) & 1u) != 0;
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
      pair(7, 8, [&]{ rcp.sp_intr_on_break = false; }, [&]{ rcp.sp_intr_on_break = true; }); // intr-on-break
      // SIGNAL bits: pairs at bits 9..24 map to SP_STATUS read bits 7..14 (SIG0..SIG7).
      // Microcode sets these at task end (SIG2 = task done) so the OS routes the SP
      // interrupt to OS_EVENT_SP (scheduler) rather than OS_EVENT_SP_BREAK.
      for(u32 i = 0; i < 8; i++)
        pair(9 + 2*i, 10 + 2*i, [&,i]{ clr(1u << (7+i)); }, [&,i]{ set(1u << (7+i)); });
      // CPU releasing the RSP (clear HALT, not re-halting): run the microcode LLE.
      // The core executes to its BREAK, updating sp_status/sp_pc and raising the SP
      // interrupt itself. Guarded against reentrancy (microcode can poke SP_STATUS
      // via COP0). If already running, just clear the halt bit.
      if((v & (1 << 0)) && !(v & (1 << 1))) {
        clr(1u);                                     // clear HALT
        if(std::getenv("KESTREL_RSPTRACE")) {
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
          if(wasHalted) { rsp.mem = this; rspSubmitKick(); }
        } else {
          if(!rsp.running) { rsp.mem = this; rsp.start(); }   // arm; System::run steps it interleaved
        }
      }
      // If both CLEAR_HALT (bit0) and SET_HALT (bit1) are written together the SET
      // wins: HALT stays set (already applied above) and the RSP does not launch.
      // A lone SET_HALT while the core is mid-task stops it (external halt).
      if((v & (1 << 1)) && !(v & (1 << 0))) rsp.running = false;
      break;
    }
    case 0x1c: rcp.sp_semaphore = 0; break;          // write clears
    }
    return;
  case BASE_DPC & 0x1ff0'0000:
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
      if(rcp.dpc_status & 0x400u) { rcp.dpc_current = rcp.dpc_start; rcp.dpc_status &= ~0x400u; }
      if(!(rcp.dpc_status & (1u << 1))) { // not frozen
        bool xbus = rcp.dpc_status & 0x1u;   // DP_STATUS_XBUS: fetch commands from DMEM
        // Kicking the FIFO starts the graphics clock and marks the pipe busy; both
        // stay set until a SYNC_FULL drains the pipe (DP_STATUS "flags during a run").
        rcp.dpc_status |= 0x8u | 0x20u;      // START_GCLK | PIPE_BUSY
        u32 cur = rcp.dpc_current;
        rcp.dpc_current = rcp.dpc_end;       // CURRENT tracks END immediately (CPU view)
        // Lockstep: rasterize synchronously (deterministic, systemtest path).
        // Threaded: enqueue; the RDP worker rasterizes async and raises MI_DP itself.
        // Both routes funnel through rdpRunJob, so results are identical — only the
        // DP-interrupt/pixel-visibility *timing* differs, exactly as on hardware.
        if(rcpMode == RcpMode::Threaded) rdpSubmit(cur, rcp.dpc_end, xbus);
        else                             rdpRunJob(cur, rcp.dpc_end, xbus);
      }
      // (frozen: no run, CURRENT stays where the START reload left it)
      // DP interrupt fires only when the RDP retires a SYNC_FULL (raised inside
      // rdpRunJob) — NOT on every DPC_END write. PD streams the FIFO with many
      // DPC_END bumps per frame; raising DP each time storms the CPU (~17 IRQs/frame)
      // and starves the game thread so it never advances past boot into rendering.
      break;
    case 0x0c: {  // DPC_STATUS write: clear/set flags
      if(v & (1 << 0)) rcp.dpc_status &= ~(1u << 0);  // clear xbus
      if(v & (1 << 1)) rcp.dpc_status |=  (1u << 0);
      if(v & (1 << 2)) rcp.dpc_status &= ~(1u << 1);  // clear freeze
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
    case 0x04: { u32 nv = v & 0xffffff;
                 if(rcp.vi_origin && nv != rcp.vi_origin) rcp.viFlips++;
                 rcp.vi_origin = nv; } break;
    case 0x08: rcp.vi_width = v & 0xfff; break;
    case 0x0c: rcp.vi_intr = v & 0x3ff; break;
    case 0x10: {
      static int vilog = std::getenv("KESTREL_VILOG") ? 1 : 0;
      if(vilog) { static u32 n=0; if((n++ & 0x3f)==0)
        std::fprintf(stderr,"[vilog] ACK VI_CURRENT write #%u intr=%02x pc=%08x\n",n,(u32)rcp.mi_intr,(u32)storePc); }
      clearIntr(MI_VI); break;   // VI_CURRENT write acks the VI interrupt
    }
    case 0x14: rcp.vi_burst = v; break;
    case 0x18: rcp.vi_vsync = v; break;
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
      u32 len = v & 0x3ffff;
      rcp.ai_len = len;
      if(spTrace()) { static u32 n=0; if(n++<20) std::fprintf(stderr,"[ai] LEN write #%u len=%u fifo=%u dac=%u\n",n,len,rcp.ai_fifo_count,rcp.ai_dacrate); }
      if(len && rcp.ai_fifo_count < 2) {
        rcp.ai_fifo_addr[rcp.ai_fifo_count] = rcp.ai_dram;
        rcp.ai_fifo_len[rcp.ai_fifo_count]  = len;
        if(rcp.ai_fifo_count == 0) rcp.ai_play_remaining = len;  // starts playing now
        rcp.ai_fifo_count++;
        // Fan the accepted buffer out to the host speakers. Read-only on RDRAM, so
        // this cannot perturb determinism (systemtest / lockstep md5 unaffected).
        u32 rate = rcp.ai_dacrate ? (48'681'812u / (rcp.ai_dacrate + 1)) : 32'000u;
        audio::pushRdram(rdram.data(), (u32)rdram.size(), rcp.ai_dram, len, rate);
      }
      break;
    }
    case 0x08: rcp.ai_ctrl = v & 1; break;
    case 0x0c: clearIntr(MI_AI); break;
    case 0x10: rcp.ai_dacrate = v; break;
    case 0x14: rcp.ai_bitrate = v; break;
    }
    return;
  case BASE_PI & 0x1ff0'0000:
    switch(off & 0xff) {
    case 0x00: rcp.pi_dram_addr = v & 0x00ff'fffe; break;   // PI masks bit0 and the top byte
    case 0x04: rcp.pi_cart_addr = v & 0xffff'fffe; break;   // PI masks bit0
    case 0x08: rcp.pi_rd_len = v & 0xffffff; piDma(/*toCart=*/true);  break;  // RDRAM -> cart
    case 0x0c: rcp.pi_wr_len = v & 0xffffff; piDma(/*toCart=*/false); break;  // cart -> RDRAM
    case 0x10:
      if(v & (1 << 1)) clearIntr(MI_PI);   // clear PI interrupt
      rcp.pi_status = 0;
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
    case 0x18: clearIntr(MI_SI); rcp.si_status = 0; break;
    }
    return;
  }
}

// --- DMA engines -------------------------------------------------------------
auto Memory::piDma(bool toCart) -> void {
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
          if(dram < rdram.size()) rdram[dram] = (u8)saveRead(cart, 1);
        }
        dram++; cart++;
      }
      rcp.pi_cart_addr = (cart) & 0xffff'fffe;
      rcp.pi_dram_addr = ((dram + 1) & ~1u) & 0x00ff'fffe;
      rcp.pi_status = 0x8;
      raiseIntr(MI_PI);
      return;
    }
  }
  if(toCart) {
    // RDRAM -> ROM cart: ROM is read-only so nothing lands. The address registers still
    // advance by the (even-rounded) length, matching HW.
    u32 len = ((rcp.pi_rd_len & 0x00ff'ffff) + 1 + 1) & ~1u;
    rcp.pi_cart_addr = (rcp.pi_cart_addr + len) & 0xffff'fffe;
    rcp.pi_dram_addr = (rcp.pi_dram_addr + len) & 0x00ff'fffe;
    rcp.pi_status = 0x8;
    raiseIntr(MI_PI);
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
  s32 maxBlock = 128;
  bool firstBlock = true;
  u32 cart = rcp.pi_cart_addr & 0x1fff'fffe;
  u32 dram = rcp.pi_dram_addr & 0x00ff'ffff;
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
  rcp.pi_status = 0x8;   // DMA done: interrupt pending (bit3), busy bits clear
  raiseIntr(MI_PI);
}

auto Memory::spDma(bool toRam) -> void {
  // SP_RD/WR_LEN: bits 0-11 length-1, 12-19 count-1, 20-31 skip. Model row/count.
  u32 len = toRam ? rcp.sp_wr_len : rcp.sp_rd_len;
  // Byte count: (field+1) rounded up to the next multiple of 8. The low 3 bits of
  // both the SP address and the RDRAM address are ignored (8-byte aligned engine).
  u32 length = ((len & 0xfff) + 8) & ~7u;
  u32 count  = ((len >> 12) & 0xff) + 1;
  u32 skip   = (len >> 20) & 0xfff;
  u32 memAddr  = rcp.sp_mem_addr & 0x1fff;
  bool imem    = (memAddr & 0x1000) != 0;
  u32 memOff   = memAddr & 0xff8;
  u32 dramAddr = rcp.sp_dram_addr & 0xfffff8;
  std::vector<u8>& sp = imem ? this->imem : this->dmem;
  if(watchAddr && toRam)
    std::fprintf(stderr, "[spDma] %s->RDRAM dram=0x%06x memOff=0x%03x len=%u count=%u skip=%u by pc=0x%08x\n",
                 imem ? "IMEM" : "DMEM", dramAddr, memOff, length, count, skip, (u32)storePc);
  for(u32 c = 0; c < count; c++) {
    for(u32 i = 0; i < length; i++) {
      u32 mo = (memOff + i) & 0xfff;
      u32 d  = dramAddr + i;
      if(d >= rdram.size()) break;
      if(toRam) { watchHit(d, 1, sp[mo], true); rdram[d] = sp[mo]; }
      else      sp[mo]   = rdram[d];
    }
    memOff  = (memOff + length) & 0xfff;
    dramAddr += length + skip;
  }
  rcp.sp_mem_addr  = (imem ? 0x1000 : 0) | memOff;
  rcp.sp_dram_addr = dramAddr & 0xffffff;
  // After any SP DMA completes the length register counts down to a fixed 0xFF8
  // readback (LENGTH=0xFF8, COUNT/SKIP drained). Both RD_LEN and WR_LEN share it.
  rcp.sp_rd_len = rcp.sp_wr_len = 0xff8;
}

auto Memory::siDma(bool toPif) -> void {
  u32 dram = rcp.si_dram_addr & 0xffffff;
  if(toPif) {
    for(u32 i = 0; i < 64; i++) if(dram + i < rdram.size()) pifram[i] = rdram[dram + i];
    pifProcessJoybus();   // execute the command block so responses are ready for read-back
  } else {
    if(watchAddr) std::fprintf(stderr, "[siDma] PIF->RDRAM dram=0x%06x by pc=0x%08x\n", dram, (u32)storePc);
    for(u32 i = 0; i < 64; i++) if(dram + i < rdram.size()) { watchHit(dram + i, 1, pifram[i], true); rdram[dram + i] = pifram[i]; }
  }
  rcp.si_status = 0;
  raiseIntr(MI_SI);
}

// Parse the 64-byte PIF RAM joybus command block and fill in device responses,
// matching what libultra's __osContGetInitData / __osEepStatus expect to read
// back. Byte scan: 0xFE ends the block, 0x00 skips to the next channel, 0xFF is
// padding. Any other value is a TX byte count that opens a command whose result
// goes into the RX area; the RX-size byte carries the channel error flags.
//
// Emulated devices: channel 0 = standard controller (no buttons held);
// channel 4 = 16 kbit EEPROM; every other channel reports "no device".
auto Memory::pifProcessJoybus() -> void {
  static int silog = std::getenv("KESTREL_SILOG") ? 1 : 0;
  const u8 NO_DEVICE = 0x80;   // CONT_NO_RESPONSE: (rxsize & 0xC0) >> 4 = 0x8
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

    auto absent = [&] { rxsz |= NO_DEVICE;
      if(silog) std::fprintf(stderr, "[silog] ABSENT ch=%d cmd=0x%02x tx=%u rx=%u pc=0x%08x\n",
                             channel, cmd, tx, rx, (u32)storePc); };
    if(silog) std::fprintf(stderr, "[silog] cmd ch=%d cmd=0x%02x tx=%u rx=%u pc=0x%08x\n",
                           channel, cmd, tx, rx, (u32)storePc);

    if(channel == 0) {                                // player 1 controller
      switch(cmd) {
      case 0x00:                                      // request status / info
      case 0xFF:                                      // reset (same reply)
        if(rx >= 3) { rxp[0] = 0x05; rxp[1] = 0x00; rxp[2] = 0x00; }  // type 0x0005, no pak
        break;
      case 0x01:                                      // read buttons
        for(int k = 0; k < rx; k++) rxp[k] = 0x00;
        if(rx >= 2) { rxp[0] = (padButtons >> 8) & 0xff; rxp[1] = padButtons & 0xff; }
        if(rx >= 4) { rxp[2] = (u8)padStickX; rxp[3] = (u8)padStickY; }  // analog stick
        break;
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
      absent();                                        // channels 1-3, 5+ : nothing plugged in
    }
    channel++;
    i = rxStart + rx;
  }
}

// --- VI field tick -----------------------------------------------------------
auto Memory::viTick() -> void {
  // Advance the scanline counter one field; raise the VI interrupt when the
  // programmed halfline is reached. A real field is ~525 halflines; we model the
  // wrap so main loops that wait on VI make progress each host tick.
  u32 prevLine = rcp.vi_current;
  rcp.vi_current = (rcp.vi_current + 2) % 525;
  if(rcp.vi_current >= (rcp.vi_intr & 0x3fe) && rcp.vi_intr != 0)
    raiseIntr(MI_VI);
  // Field boundary (counter wrapped): rotate the GPU backend's per-frame context. Runs on
  // the emulation thread, same thread that enqueues RDP commands — begin_frame_context must
  // be serialized with submission (scanout is the only cross-thread call). No-op when off.
  if(rcp.vi_current < prevLine) vrdp::frameBegin();
  aiTick();
}

// Drain the AI playback FIFO ~one field's worth of samples per call. When the
// current buffer empties, pop it, raise MI_AI (audio DMA done) and start the
// next. This paces the audio driver: it blocks on FIFO_FULL between buffers
// instead of spinning in the frame builder and starving the gfx thread.
auto Memory::aiTick() -> void {
  if(rcp.ai_fifo_count == 0) return;
  // Bytes played per field from the sample rate: rate = vid_clock/(dacrate+1),
  // 4 bytes/sample (16-bit stereo), 60 fields/s. Guard against a zero dacrate.
  u32 rate  = rcp.ai_dacrate ? (48'681'812u / (rcp.ai_dacrate + 1)) : 32'000u;
  u32 perFld = (rate * 4u) / 60u;
  if(perFld == 0) perFld = 1;
  if(rcp.ai_play_remaining > perFld) { rcp.ai_play_remaining -= perFld; return; }
  // Current buffer finished: pop it, signal AI, advance to the next.
  rcp.ai_fifo_addr[0] = rcp.ai_fifo_addr[1];
  rcp.ai_fifo_len[0]  = rcp.ai_fifo_len[1];
  rcp.ai_fifo_count--;
  raiseIntr(MI_AI);
  rcp.ai_play_remaining = rcp.ai_fifo_count ? rcp.ai_fifo_len[0] : 0;
}

// --- RCP threading -----------------------------------------------------------
// Rasterize one RDP FIFO span and do the DP-done bookkeeping. Called inline on
// the CPU thread in lockstep, or on the RDP worker thread in threaded mode; in
// either case this is the sole owner of softRdp while it runs, so the rasterizer
// stays single-threaded. On SYNC_FULL it clears the busy flags and raises MI_DP —
// the atomic fetch_or in raiseIntr publishes (release) the pixel writes above it
// before the CPU can observe the interrupt (acquire on its mi_intr load).
auto Memory::rdpRunJob(u32 current, u32 end, bool xbus) -> void {
  // GPU path (paraLLEl-RDP): opt-in via KESTREL_PRDP. Lazily brought up on first job with
  // this RDRAM block; when live it consumes the FIFO on the GPU instead of SoftRDP. The
  // guest RDRAM vector is allocated once and never resized, so its pointer is stable for
  // the CommandProcessor's lifetime. Stubs make this a no-op in non-PRDP builds.
  {
    static bool tried = false;
    if(!tried) { tried = true; vrdp::init(rdram.data(), (u32)rdram.size()); }
  }
  if(vrdp::active()) {
    bool sync = vrdp::runFifo(rdram.data(), (u32)rdram.size(), dmem.data(), current, end, xbus);
    if(sync) {
      rcp.dpc_status &= ~(0x8u | 0x20u);   // pipe drained: clear START_GCLK | PIPE_BUSY
      raiseIntr(MI_DP);
    }
    return;
  }
  u32 nc = softRdp.run(*this, current, end, xbus);
  if(std::getenv("KESTREL_RDPTRACE")) {
    static u32 dpCalls = 0;
    if(dpCalls++ < 40)
      std::fprintf(stderr, "[rdp] job cur=%06x end=%06x cmds=%u ci=%06x sz=%u\n",
                   current, end, nc, softRdp.colorImage(), softRdp.colorImageSize());
  } else { (void)nc; }
  if(softRdp.sawSyncFull) {
    rcp.dpc_status &= ~(0x8u | 0x20u);   // pipe drained: clear START_GCLK | PIPE_BUSY
    rcp.dpSyncs++;                       // "a frame finished rendering" — see KESTREL_MAXSYNCS
    raiseIntr(MI_DP);
  }
}

auto Memory::rdpSubmit(u32 current, u32 end, bool xbus) -> void {
  {
    std::lock_guard<std::mutex> lk(rdpMx);
    rdpQueue.push_back({current, end, xbus});
    rdpBusy.store(true, std::memory_order_relaxed);
  }
  rdpCv.notify_one();
}

auto Memory::rdpWorkerLoop() -> void {
  for(;;) {
    RdpJob job;
    {
      std::unique_lock<std::mutex> lk(rdpMx);
      rdpCv.wait(lk, [&]{ return rdpStop || !rdpQueue.empty(); });
      if(rdpStop && rdpQueue.empty()) return;
      job = rdpQueue.front(); rdpQueue.pop_front();
    }
    rdpRunJob(job.current, job.end, job.xbus);
    {
      std::lock_guard<std::mutex> lk(rdpMx);
      if(rdpQueue.empty()) rdpBusy.store(false, std::memory_order_relaxed);
    }
    rdpCv.notify_all();   // wake any rdpDrain() waiter
  }
}

auto Memory::rdpDrain() -> void {
  if(rcpMode != RcpMode::Threaded) return;
  std::unique_lock<std::mutex> lk(rdpMx);
  rdpCv.wait(lk, [&]{ return rdpQueue.empty() && !rdpBusy.load(std::memory_order_relaxed); });
}

auto Memory::rspAwaitIdle() -> void {
  if(rcpMode != RcpMode::Threaded) return;
  std::unique_lock<std::mutex> lk(rspMx);
  rspCv.wait(lk, [&]{ return !rspBusy.load(std::memory_order_acquire); });
}

auto Memory::rspSubmitKick() -> void {
  {
    std::unique_lock<std::mutex> lk(rspMx);
    // Serialize against a worker that has finished its microcode (BREAK reached,
    // MI_SP already raised) but not yet cleared rspBusy. The caller has decided a
    // launch is due, so we hold the CPU for that short wind-down rather than drop
    // the task. This never blocks on a genuinely running RSP: mid-task HALT is
    // clear, so the write is a no-op and we are not called at all.
    rspCv.wait(lk, [&]{ return !rspBusy.load(std::memory_order_acquire); });
    rspBusy.store(true, std::memory_order_release);
    rspKick = true;
  }
  rspCv.notify_one();
}

auto Memory::rspWorkerLoop() -> void {
  for(;;) {
    {
      std::unique_lock<std::mutex> lk(rspMx);
      rspCv.wait(lk, [&]{ return rspStop || rspKick; });
      if(rspStop && !rspKick) return;
      rspKick = false;
    }
    // Run the armed microcode to BREAK. run() = start()+step(budget); it updates
    // sp_status/sp_pc and raises MI_SP itself. Any DPC_END the microcode writes
    // funnels through mmioWrite → rdpSubmit, so the RDP pipelines behind us.
    rsp.run();
    rspBusy.store(false, std::memory_order_release);
    rspCv.notify_all();   // wake a possible drain waiter
  }
}

auto Memory::startRcpThreads() -> void {
  if(rcpMode != RcpMode::Threaded) return;
  if(!rdpWorker.joinable()) { rdpStop = false; rdpWorker = std::thread([this]{ rdpWorkerLoop(); }); }
  if(!rspWorker.joinable()) { rspStop = false; rspWorker = std::thread([this]{ rspWorkerLoop(); }); }
}

auto Memory::stopRcpThreads() -> void {
  { std::lock_guard<std::mutex> lk(rdpMx); rdpStop = true; }
  rdpCv.notify_all();
  if(rdpWorker.joinable()) rdpWorker.join();
  { std::lock_guard<std::mutex> lk(rspMx); rspStop = true; }
  rspCv.notify_all();
  if(rspWorker.joinable()) rspWorker.join();
}

}  // namespace kestrel
