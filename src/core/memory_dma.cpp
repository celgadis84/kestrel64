// DMA engines: PI, SP, SI and the DP command FIFO kick.
// Split out of memory.cpp; shared internals live in memory_internal.hpp.
#include "memory.hpp"
#include "memory_internal.hpp"
#include "wrtag.hpp"
#include "../vrdp/vrdp.hpp"
#include <algorithm>

namespace kestrel {

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
      // FlashRAM en modo Read: solo la direccion de ARRANQUE pasa por el cableado del chip
      // (ver flashArrayOff); la rafaga sigue lineal con el contador interno del chip.
      const bool flashArr = !toCart && isFlash() && flashMode == FlashMode::Read;
      const u32 flashBase = flashArr ? flashArrayOff(cartPhys) : 0;
      for(u32 i = 0; i < len; i++) {
        if(flashArr) {
          wrtag::mark(dram, wrtag::kPiDma, 0);
          if(watchAddr) watchHit(dram, 1, 0, true);
          const u32 o = flashBase + i;
          if(dram < rdram.size()) rdram[dram] = o < saveRam.size() ? saveRam[o] : 0;
          dram++; cart++;
          continue;
        }
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
  if(toCart && scOn) {
    // RDRAM -> SummerCart64: el buffer siempre, la SDRAM si CFG_ROM_WRITE esta puesto.
    u32 len = ((rcp.pi_rd_len & 0x00ff'ffff) + 1 + 1) & ~1u;
    ramBytesPi.fetch_add(len, std::memory_order_relaxed);
    const u32 cartStart = rcp.pi_cart_addr & 0x1fff'fffe;
    const u32 dram = rcp.pi_dram_addr & 0x00ff'fffe;
    for(u32 i = 0; i < len; i++) scDmaWrite(cartStart + i, dram + i < rdram.size() ? rdram[dram + i] : 0);
    rcp.pi_cart_addr = (rcp.pi_cart_addr + len) & 0xffff'fffe;
    rcp.pi_dram_addr = (rcp.pi_dram_addr + len) & 0x00ff'fffe;
    piArm(cartStart, len);
    return;
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
      if(scOn && cart >= kScBuf && cart + 1 < kScBuf + kScBufSz) {
        data = (u16)(scBuf[cart - kScBuf] << 8 | scBuf[cart - kScBuf + 1]);
      } else if(romOff != 0xffff'ffffu) {
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
  // El motor solo tiene los bits 23:3 de la direccion de RDRAM (ares: skip.bit(3,11)), asi que
  // los 3 bits bajos del salto se pierden. libdragon DMAExec pone skip = pitch - ancho sin
  // redondear y cuenta con eso (rsp_mpeg1 block_predict: 17 bytes por fila, pitch 128).
  u32 skip   = (len >> 20) & 0xff8;
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
  const u32 wrPc = 0x04001000u | (rsp.pc & 0xffc);   // KESTREL_WRTAG: pc del RSP en espacio SP IMEM
  for(u32 c = 0; c < count; c++) {
    if(byByte) {
      for(u32 i = 0; i < length; i++) {
        u32 mo = (memOff + i) & 0xfff;
        u32 d  = dramAddr + i;
        bool have = d < rdram.size();
        if(toRam) { if(!have) continue;
                    wrtag::mark(d, wrtag::kSpDma, wrPc); watchHit(d, 1, sp[mo], true); rdram[d] = sp[mo]; }
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
          if(toRam) { dmaCopy(&rdram[d], &sp[mo], n); wrtag::markRange(d, n, wrtag::kSpDma, wrPc); }
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
  if(diag || watchAddr || spTrace() || wrtag::tag) return false;   // wrtag marca en spDma
  const u32 length = ((len & 0xfff) + 8) & ~7u;
  const u32 count  = ((len >> 12) & 0xff) + 1;
  const u32 skip   = (len >> 20) & 0xff8;   // bits 2:0 no existen (ver spDma)
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
    pifCommand();
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
  jitCancelChain();
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

}  // namespace kestrel
