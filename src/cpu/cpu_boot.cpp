// CPU boot: CIC detection from the IPL3 image, cold (PIF-less) and warm (NMI) boot.
// Split out of cpu.cpp; shared internals live in cpu_internal.hpp.
#include "cpu.hpp"
#include "jit.hpp"          // CodeCache completo: warmBoot vacia la cache del dynarec
#include "../core/memory.hpp"
#include "../core/cic.hpp"
#include "cpu_internal.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace kestrel {

// --- CIC boot-chip detection --------------------------------------------------
// The cart carries its own CIC-signed IPL3 in ROM bytes 0x40..0xFFF. Which CIC
// variant matters at hand-off: each leaves a different seed in s6 (and games
// keyed to it check osCicId). We identify the chip by CRC32 of the IPL3 image
// (0xFC0 bytes) against the well-known community table, then pick the seed.
// Every field below is a property of the IPL3 image itself, so the CRC identifies
// all of them at once:
//   seed       the byte the CIC feeds the PIF as the IPL2/IPL3 checksum seed. IPL3
//              leaves it in s6, and games keyed to their chip check it.
//   entryDelta what this IPL3 subtracts from the boot address in the ROM header
//              before using it. 6103/7103 and 5101 take off 1 MB, 6106/7106 take off
//              2 MB — obfuscation, per n64brew's CIC-NUS table. The subtraction hits
//              the DMA destination as well as the jump: IPL3 keeps one boot address
//              in one register, and loading the megabyte anywhere else would leave
//              the game unloaded at the address it was linked for.
//   entryFixed non-zero when this IPL3 ignores the header outright (only 7102 does,
//              hardwired to 0x80000480).
//   idNtsc/idPal  the chip's part number. Most NTSC/PAL pairs SHARE one IPL3 image
//              (6102/7101, 6103/7103, 6105/7105, 6106/7106), so the CRC alone cannot
//              tell them apart — the cart's region byte does. 6101 and 7102 are the
//              exception: different images, and 7102 is PAL-only.
struct CicInfo { u32 crc; int idNtsc, idPal; u8 seed; u32 entryDelta; u32 entryFixed; };
static const CicInfo kCicTable[] = {
  { 0x6170A4A1, 6101, 6101, 0x3f,        0, 0 },           // 6101 (Star Fox 64), NTSC only
  { 0x90BB6CB5, 6102, 7101, 0x3f,        0, 0 },           // 6102 / 7101, ~88% of the library
  { 0x009E9EA3, 7102, 7102, 0x3f,        0, 0x8000'0480 }, // 7102 (Lylat Wars), PAL only
  { 0x0B050EE0, 6103, 7103, 0x78, 0x10'0000, 0 },          // 6103 / 7103 (Banjo-Kazooie, DKR)
  { 0x98BC2C86, 6105, 7105, 0x91,        0, 0 },           // 6105 / 7105 (Perfect Dark, DK64, OoT/MM)
  { 0xACC8580A, 6106, 7106, 0x85, 0x20'0000, 0 },          // 6106 / 7106 (F-Zero X, Yoshi's Story)
  { 0x0E018159, 5101, 5101, 0xac, 0x10'0000, 0 },          // 5101 (Aleck 64 arcade)
};
static constexpr u32 kCrcCic6105 = 0x98BC2C86;  // the one IPL3 with an RSP boot stage

// IPL2 checksum (48 bits) the CIC holds; the PIF compares it with the one IPL2 computes over
// IPL3 (commands 0x20/0x40). 0 = unknown chip (unsigned IPL3).
static auto cicIpl2Checksum(int id) -> u64 {
  for(auto& c : cic::kChips) if(c.idNtsc == id || c.idPal == id) return c.sum;
  return 0;
}

static auto crc32(const u8* p, usize n) -> u32 {
  u32 c = 0xffff'ffffu;
  for(usize i = 0; i < n; i++) {
    c ^= p[i];
    for(int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB8'8320u & (~(c & 1) + 1));
  }
  return ~c;
}

static auto detectCic(const std::vector<u8>& rom) -> CicInfo {
  if(rom.size() >= 0x1000) {
    u32 c = crc32(rom.data() + 0x40, 0x1000 - 0x40);
    for(auto& e : kCicTable) if(e.crc == c) return e;
    // Not an IPL3 we know (homebrew bootcode, e.g. libdragon's open IPL3): it can only boot
    // if it was signed for some chip, so find the chip whose seed makes IPL2 yield that
    // chip's checksum. crc stays 0 = "run it LLE".
    if(auto* chip = cic::chipForIpl3(rom.data() + cic::kIpl3Off))
      return { 0, chip->idNtsc, chip->idPal, chip->seed, 0, 0 };
  }
  return { 0, 6102, 7101, 0x3f, 0, 0 };  // unsigned → assume the common 6102/7101 (no verdict)
}

auto CPU::warmBoot(u32 entryPoint) -> void {
  // El reloj de invitado no vuelve atras con el NMI: todos los plazos (VI, SI, PI, AI) estan
  // medidos en el. Count tampoco lo toca el NMI.
  const u64 ret = retired, sc = stallCycles, so = stallOps, sr = stallOpsRem;
  const u64 cnt = cop0[C0_Count]; const u32 cf = countFrac;
  warm = true;
  fastBoot(entryPoint);
  warm = false;
  retired = ret; stallCycles = sc; stallOps = so; stallOpsRem = sr;
  cop0[C0_Count] = cnt; countFrac = cf;
  // El codigo en RDRAM/IMEM acaba de reescribirse por debajo (copia del IPL3).
  if(jitCache) jitCache->clear();
  jitTlbValid = false; fetchLineVBase = 1; bumpXlat();
  jitPending = jitChain = jitChainOps = jitOpsBudget = jitGuard = 0;
  memAbort = false;
}

auto CPU::fastBoot(u32 entryPoint) -> void {
  reset();
  // Identify the cart's CIC boot chip from its IPL3 image — sets the correct
  // hand-off seed (s6) and osCicId. PD NTSC is 6105 (seed 0x91), not 6102.
  CicInfo cic = mem ? detectCic(mem->rom) : CicInfo{ 0, 6102, 7101, 0x3f, 0, 0 };
  bool pal = (bootTvType == 0);                     // 0 PAL / 1 NTSC / 2 MPAL (MPAL carts use NTSC chips)
  int  cicId = pal ? cic.idPal : cic.idNtsc;
  bool is6105 = (cic.crc == kCrcCic6105);   // this IPL3 image (it has an RSP boot stage)
  // El PIF necesita saberlo: el desafio anti-pirateria solo lo contesta el chip 6105, lleve
  // el IPL3 que lleve (un homebrew firmado para 6105 tambien lo tiene).
  if(mem) mem->cic6105 = (cicId == 6105 || cicId == 7105);
  // IPL1/IPL2 are HLE: leave the PIF where real IPL2 leaves it (ROM locked, checksum it
  // computed over the cart's IPL3 handed over and checked), still in reset mode. An IPL3
  // no chip accepts would halt real hardware; with no chip to name, no verdict is given.
  if(mem) {
    u64 ipl2 = mem->rom.size() >= 0x1000 ? cic::ipl2Checksum(cic.seed, mem->rom.data() + cic::kIpl3Off) : 0;
    u64 chip = cicIpl2Checksum(cicId);
    if(cic.crc == 0 && ipl2 != chip) {
      chip = 0;
      std::fprintf(stderr, "[boot] IPL3 sin firma para ningun CIC conocido (checksum IPL2 %012llx)\n",
                   (unsigned long long)ipl2);
    }
    mem->pifBootHle(chip, ipl2);
  }
  // Arrancar por IPL3 real o emular su resultado. El camino HLE no es generico: sabe
  // reproducir el efecto de los IPL3 que CONOCE (direccion de arranque propia de cada CIC,
  // la etapa RSP del 6105, la copia del primer megabyte desde ROM+0x1000). Un cartucho con
  // un IPL3 que no esta en la tabla -- homebrew con bootcode propio, y en particular el IPL3
  // libre de libdragon, que es un trampolin firmado en ROM 0x40 que carga el IPL3 de verdad
  // desde ROM 0x1040 y luego un cargador ELF -- no tiene nada que emular en alto nivel: lo
  // unico fiel es EJECUTARLO, que es lo que hace la consola. Con HLE ese cartucho se comia
  // como codigo de juego el IPL3 que hay en ROM+0x1000 y descarrilaba.
  // KESTREL_HLE_IPL3=1 fuerza el camino viejo (bisecar); KESTREL_LLE_IPL3=1 lo fuerza al reves.
  bool lle = std::getenv("KESTREL_LLE_IPL3") != nullptr;
  if(!lle && cic.crc == 0 && !std::getenv("KESTREL_HLE_IPL3")) lle = true;   // IPL3 desconocido
  // Where this IPL3 actually boots the cart. Only 7102, 6103/7103, 6106/7106 and 5101
  // differ from the header word; for everyone else this is the header word verbatim.
  u32 bootAddr = cic.entryFixed ? cic.entryFixed : (entryPoint - cic.entryDelta);
  std::fprintf(stderr, "[boot] CIC detected: %d (seed 0x%02x)%s\n",
               cicId, cic.seed, lle ? " [LLE IPL3]" : "");
  if(bootAddr != entryPoint)
    std::fprintf(stderr, "[boot] CIC-%d boot address: header 0x%08x -> 0x%08x\n",
                 cicId, entryPoint, bootAddr);
  std::fflush(stderr);

  // --- CIC-6105 IPL3 low-RDRAM image + boot microcode ------------------------
  // Carts signed with CIC-NUS-6105 (Perfect Dark, Donkey Kong 64, Majora's Mask,
  // Banjo-Tooie...) all ship the same IPL3, and that IPL3 does two things no other
  // CIC's does before handing control to the game. Games check the result and spin
  // forever if it is missing, so both have to happen on every 6105 boot.
  //
  // Stage 1 — the IPL3 copies a slice of its OWN image out of DMEM into low RDRAM
  // (`lw`/`sw` loop at DMEM 0x524..0x538: DMEM 0x554..0x888 -> RDRAM 0x004..0x338, a
  // fixed -0x550 skew) and carries on executing from that copy. The image is the very
  // bytes the cart holds at ROM 0x554..0x888. Perfect Dark's bootloader spins unless
  // *(0xA00002E8) == 0xC86E2000 — that is just IPL3 code word ROM 0x838 landing at
  // RDRAM 0x2E8.
  //
  // Stage 2 — the IPL3 then starts the RSP (SP_STATUS = 0xAD, DMEM 0x550) on a
  // microcode it XOR-decrypts into IMEM from the IPL2 code the PIF ROM left there.
  // That microcode DMAs RDRAM 0x1E8 (0x1F0 bytes) into IMEM 0x120 and issues ONE
  // strided SP write DMA: SP_DRAM_ADDR = 0x2FB1F0, SP_WR_LEN = 0xFE817000 (length 8,
  // count 24, skip 0xFE8), scattering 24 8-byte rows 0xFF0 apart. Donkey Kong 64
  // checks row 3: *(0xA02FE1C0) == 0xAD170014, i.e. RDRAM 0x200 (the IPL3's
  // `sw s7,0x14(t0)`) at 0x2FB1F0 + 3*0xFF0. The microcode itself cannot be run
  // without the PIF ROM's IMEM residue, and its memory effect is entirely fixed, so
  // reproduce the effect. Same bytes for every 6105 cart; nothing game-specific.
  auto cic6105Ipl3 = [&]() {
    if(!mem || !is6105) return;
    if(mem->rom.size() < 0x888 || mem->rdram.size() < 0x313000 || mem->imem.size() < 0x310) return;
    for(u32 i = 0x554; i < 0x888; i++) mem->rdram[i - 0x550] = mem->rom[i];
    // The first instruction word of that microcode stays behind at IMEM 0x004 once the
    // RSP halts, and 6105 games do read it back. It differs by console region because
    // the microcode is the XOR of the IPL3 key table with the PIF ROM's IPL2 residue,
    // and the PAL PIF ROM is a different image.
    { u32 w = pal ? 0xbda8'07fcu : 0x8da8'07fcu;
      mem->imem[4]=w>>24; mem->imem[5]=w>>16; mem->imem[6]=w>>8; mem->imem[7]=w; }
    for(u32 i = 0; i < 0x1f0; i++)     mem->imem[0x120 + i]  = mem->rdram[0x1e8 + i];
    for(u32 row = 0; row < 24; row++) {
      u32 dst = 0x2fb1f0 + row * 0xff0, src = 0x120 + row * 8;
      for(u32 i = 0; i < 8; i++) mem->rdram[dst + i] = mem->imem[src + i];
    }
  };

  if(lle && mem && mem->rom.size() >= 0x1000) {
    // LLE boot: run the cart's OWN CIC-signed IPL3 instead of faking its result.
    // The PIF copies the 0xFC0-byte IPL3 image (ROM 0x40..0xFFF) into SP DMEM at
    // 0x04000040 and starts the CPU there; the IPL3 then inits RDRAM, copies the
    // boot segment ROM->RDRAM, verifies its checksum against the CIC seed, and
    // jumps to the game entry. We reproduce that hand-off exactly.
    for(u32 i = 0; i < 0xFC0 && i < mem->dmem.size() - 0x40; i++)
      mem->dmem[0x40 + i] = mem->rom[0x40 + i];
    // IPL2 -> IPL3 register hand-off (what the PIF/IPL2 leave for IPL3).
    gpr[19] = 0;                              // s3 = osRomType (cart)
    gpr[20] = (u64)bootTvType;                // s4 = osTvType (0 PAL / 1 NTSC / 2 MPAL)
    gpr[21] = warm ? 1 : 0;                   // s5 = osResetType (0 frio, 1 NMI)
    gpr[22] = (u64)cic.seed;                 // s6 = CIC seed
    gpr[23] = pal ? 6u : 0u;                  // s7 = osVersion (PAL PIF ROM reports 6)
    // t3 apunta al PROPIO IPL3 ya copiado en DMEM. No es decorativo: el IPL3 de los
    // cartuchos CIC-6105 (Perfect Dark, Zelda, Banjo) arranca con un descifrador que lee
    // su tabla con `lw t2, 0x44(t3)`, es decir DMEM+0x84, justo detras del stub. Sin ese
    // registro la primera lectura va a la direccion 0x44 y el IPL3 muere en un TLBL.
    gpr[11] = 0xffff'ffff'a400'0040ull;      // t3 = base del IPL3 en DMEM
    gpr[31] = 0xffff'ffff'a400'1550ull;      // ra (el IPL3 no vuelve, pero es el valor real)
    gpr[29] = 0xffff'ffff'a400'1ff0ull;      // sp in SP DMEM
    pc = sext32(0xa400'0040);                // execute IPL3 from DMEM (uncached)
    nextPc = pc + 4;
    // osMemSize/osTvType/etc. the real IPL3 leaves at 0x300; write the few the
    // game reads even though a full IPL3 would compute them, so post-boot matches.
    if(mem->rdram.size() >= 0x400) {
      auto putw = [&](u32 p, u32 v) {
        mem->rdram[p]=v>>24; mem->rdram[p+1]=v>>16; mem->rdram[p+2]=v>>8; mem->rdram[p+3]=v; };
      putw(0x300, bootTvType);               // osTvType (region del cartucho)
      putw(0x318, (u32)mem->rdram.size());   // osMemSize (IPL3 probes RDRAM for this)
      // The PIF's RDRAM self test leaves the size here too; the 6105 IPL3 reads it back
      // (`lw t1,0xf0(t0)`, t0 = 0xA0000300) and is what actually fills osMemSize at 0x318.
      putw(0x3f0, (u32)mem->rdram.size());
    }
    // The real IPL3 redoes stage 1 itself (identical bytes), but stage 2 needs the RSP
    // microcode that only exists once the PIF ROM's IPL2 residue is in IMEM — which no
    // HLE PIF leaves behind. Seed both here so LLE lands on the same memory state.
    cic6105Ipl3();
    goto envflags;
  }
  // HLE IPL3: copy the boot segment (up to 1 MB from ROM+0x1000) to RDRAM at the
  // entry's physical address, then jump to the entry point.
  if(mem && !mem->rom.empty()) {
    u32 phys = bootAddr & 0x1fff'ffff;
    usize count = mem->rom.size() > 0x1000 ? mem->rom.size() - 0x1000 : 0;
    if(count > 0x0010'0000) count = 0x0010'0000;
    for(usize i = 0; i < count && phys + i < mem->rdram.size(); i++) {
      mem->rdram[phys + i] = mem->rom[0x1000 + i];
    }
  }
  cic6105Ipl3();               // 6105 low-RDRAM image + boot-microcode scatter (see above)
  // PIF boot globals the OS/game spin-wait on (physical 0x300..0x3FF in RDRAM).
  // Real IPL3/PIF populates these; the HLE boot must too or osInitialize hangs.
  if(mem && mem->rdram.size() >= 0x400) {
    auto putw = [&](u32 phys, u32 v) {
      mem->rdram[phys+0]=v>>24; mem->rdram[phys+1]=v>>16;
      mem->rdram[phys+2]=v>>8;  mem->rdram[phys+3]=v;
    };
    putw(0x300, bootTvType);   // osTvType (0 PAL / 1 NTSC / 2 MPAL), segun la region del cart
    putw(0x304, 0);            // osRomType (0 = cart)
    putw(0x308, 0xb000'0000);  // osRomBase (cart domain-1, KSEG1)
    putw(0x30c, warm ? 1 : 0); // osResetType: 0 = cold boot, 1 = NMI (boton RESET)
    putw(0x310, (u32)cicId);   // osCicId (6101/6102/6103/6105/6106, PAL 71xx)
    putw(0x314, 0);            // osVersion
    putw(0x318, (u32)mem->rdram.size());  // osMemSize (RDRAM bytes)
    if(!warm) putw(0x31c, 0);  // osAppNMIBuffer[0] (el arranque en caliente no lo toca)
    // The PIF's RDRAM power-on self test leaves the measured size at 0x3F0 as well;
    // the 6105 IPL3 reads it back (`lw t1,0xf0(t0)` with t0 = 0xA0000300) and forwards
    // it to osMemSize. Under HLE nothing reads it, but the word is part of the boot
    // state a game may inspect, and the LLE IPL3 path needs it to compute 0x318.
    putw(0x3f0, (u32)mem->rdram.size());
  }
  // --- register hand-off, exactly as the real IPL3 leaves it ------------------
  // Most of these are not "initialisation": they are the leftovers of the checksum
  // work IPL3 does over the first megabyte of the cart, so they depend on BOTH the
  // CIC variant (each one uses a different seed and magic) and the console region
  // (the PIF ROM the checksum runs against differs). v0/v1/a0 and, on PAL, a1 are
  // literally pieces of the IPL2 checksum for that chip — for 6102 the checksum is
  // 0xA536C0F1D859 and the boot leaves a0 = 0xA536 with PAL a1 = 0xC0F1D859.
  // A game that reads them (a few anti-piracy checks do) sees the same values a
  // console would. Table cross-checked against Project64's post-IPL3 state.
  gpr[ 6] = 0xffff'ffff'a400'1f0cull;      // a2
  gpr[ 7] = 0xffff'ffff'a400'1f08ull;      // a3
  gpr[ 8] = 0x0000'0000'0000'00c0ull;      // t0
  gpr[10] = 0x0000'0000'0000'0040ull;      // t2
  gpr[11] = 0xffff'ffff'a400'0040ull;      // t3 = the IPL3 image still in DMEM
  gpr[20] = (u64)bootTvType;               // s4 = osTvType
  gpr[21] = warm ? 1 : 0;                  // s5 = osResetType
  gpr[22] = (u64)cic.seed;                 // s6 = CIC seed
  gpr[23] = pal ? 6u : 0u;                 // s7 = osVersion
  gpr[24] = pal ? 0u : 3u;                 // t8 (6105/6106 override it below on PAL)
  gpr[29] = 0xffff'ffff'a400'1ff0ull;      // sp
  gpr[31] = pal ? 0xffff'ffff'a400'1554ull // ra: the PAL IPL3 tail is one instruction later
                : 0xffff'ffff'a400'1550ull;
  {
    auto set = [&](u64 v0, u64 a0, u64 t4, u64 t5, u64 t7, u64 t9, u64 at) {
      gpr[2] = gpr[3] = v0; gpr[4] = a0; gpr[12] = t4; gpr[13] = t5;
      gpr[15] = t7; gpr[25] = t9; gpr[1] = at;
    };
    switch(cic.crc) {
      case 0x90BB6CB5:  // 6102 / 7101
        set(0x0000'0000'0ebd'a536ull, 0xa536, 0xffff'ffff'ed10'd0b3ull, 0x0000'0000'1402'a4ccull,
            0x0000'0000'3103'e121ull, 0xffff'ffff'9deb'b54full, 1);
        gpr[5]  = pal ? 0xffff'ffff'c0f1'd859ull : 0xffff'ffff'c959'73d5ull;   // a1
        gpr[14] = pal ? 0x0000'0000'2de1'08eaull : 0x0000'0000'2449'a366ull;   // t6
        break;
      case 0x0B050EE0:  // 6103 / 7103
        set(0x0000'0000'49a5'ee96ull, 0xee96, 0xffff'ffff'ce9d'fbf7ull, 0xffff'ffff'ce9d'fbf7ull,
            0x0000'0000'18b6'3d28ull, 0xffff'ffff'825b'21c9ull, 1);
        gpr[5]  = pal ? 0xffff'ffff'd464'6273ull : 0xffff'ffff'9531'5a28ull;
        gpr[14] = pal ? 0x0000'0000'1af9'9984ull : 0x0000'0000'5bac'a1dfull;
        break;
      case kCrcCic6105:  // 6105 / 7105
        set(0xffff'ffff'f58b'0fbfull, 0x0fbf, 0xffff'ffff'9651'f81eull, 0x0000'0000'2d42'aac5ull,
            0x0000'0000'5658'4d60ull, 0xffff'ffff'cdce'565full, 0);
        gpr[5]  = pal ? 0xffff'ffff'deca'aad1ull : 0x0000'0000'5493'fb9aull;
        gpr[14] = pal ? 0x0000'0000'0cf8'5c13ull : 0xffff'ffff'c2c2'0384ull;
        if(pal) gpr[24] = 2;
        break;
      case 0xACC8580A:  // 6106 / 7106
        set(0xffff'ffff'a959'30a4ull, 0x30a4, 0xffff'ffff'bcb5'9510ull, 0xffff'ffff'bcb5'9510ull,
            0x0000'0000'7a3c'07f4ull, 0x0000'0000'465e'3f72ull, 0);
        gpr[5]  = pal ? 0xffff'ffff'b04d'c903ull : 0xffff'ffff'e067'221full;
        gpr[14] = pal ? 0x0000'0000'1af9'9984ull : 0x0000'0000'5cd2'b70full;
        if(pal) gpr[24] = 2;
        break;
      default: break;   // 6101, 7102, 5101 and unknown images: only the seed is known
    }
  }
  // COP0 state the real IPL3 leaves at game entry (CIC-6102 hand-off, matches krom
  // COP0Register + n64brew "initial register state"): Status has CU1|FR|SR and 64-bit
  // addressing enabled in all modes (KX|SX|UX); EPC and ErrorEPC keep their power-on
  // all-ones (the boot chain never writes them). Kernel mode → CU0 is unnecessary.
  cop0[C0_Status]   = 0x2410'00e0;
  cop0[C0_EPC]      = sext32(0xffff'ffff);
  cop0[C0_ErrorEPC] = sext32(0xffff'ffff);
  pc = sext32(bootAddr);
  nextPc = pc + 4;
envflags:
  if(const char* b = std::getenv("KESTREL_BP")) bpAddr = sext32((u32)std::strtoul(b, nullptr, 0));
  if(std::getenv("KESTREL_BPTRACE")) bpTrace = true;
  if(std::getenv("KESTREL_TRAPWILD")) trapWild = true;
  if(mem && std::getenv("KESTREL_TRAPSPREG")) mem->trapSpRegStore = true;
  if(std::getenv("KESTREL_HUFT")) huftTrap = true;
  if(const char* a = std::getenv("KESTREL_AUDIOHOOK")) audioHook = (u32)std::strtoul(a, nullptr, 0);
  if(const char* m = std::getenv("KESTREL_MAXINSN")) maxInsn = std::strtoull(m, nullptr, 0);
  if(std::getenv("KESTREL_EXCTRACE")) excTrace = true;
  if(std::getenv("KESTREL_EXCTAIL")) excTail = true;
  if(std::getenv("KESTREL_FPDBG")) fpDbg = true;
  if(std::getenv("KESTREL_FPTRACE")) fpTrace = true;
  if(std::getenv("KESTREL_PCRING")) pcRingOn = true;
  if(const char* e = std::getenv("KESTREL_SEENADDR"))
    g_seenAddr = (u32)std::strtoul(e, nullptr, 0) & 0x1fff'fffcu;
  if(const char* w = std::getenv("KESTREL_WATCHP")) wPhys = (u32)std::strtoul(w, nullptr, 0) & 0x1fff'ffffu;
  if(std::getenv("KESTREL_HALT_UNIMPL")) haltUnimpl = true;
  dcDbgOn = (wPhys != 0) || g_dcWriteThrough;
  if(dcDbgOn) stGuard |= StGuardDbg; else stGuard = (u8)(stGuard & ~StGuardDbg);
  refreshDebugArmed();
}

}  // namespace kestrel
