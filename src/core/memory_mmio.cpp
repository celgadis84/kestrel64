// RCP MMIO register file (SP/DP/MI/VI/AI/PI/RI/SI) and the VI/AI field ticks.
// Split out of memory.cpp; shared internals live in memory_internal.hpp.
#include "memory.hpp"
#include "memory_internal.hpp"
#include "wrtag.hpp"
#include "../audio/audio.hpp"
#include "../debug/dbg.hpp"

namespace kestrel {

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
    case 0x10: return dpcClockNow();
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
      // Y la MISMA formula: antes se dividia por `cph = campo / total` truncado, que en NTSC
      // deja ~1187 ops por campo sin repartir (1118437 / 525). El contador corria un poco mas
      // rapido que el campo y la deriva se acumulaba entre campos: a los ~100 campos MI_VI
      // subia al cruzar V_INTR = 2 y V_CURRENT, leido 93 ops despues, ya valia 11 (libdragon:
      // "VI WARNING: __vblank_interrupt outside of vblank period: 12").
      u32 total = rcp.viHalflines();
      u64 field = viFieldInsns ? viFieldInsns : 1;
      u64 cyc = cartNow();   // mismo reloj de invitado que viTick (incluye paradas de cache)
      return rcp.viCurrentAt(cyc, field);
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
    // El DAC drena por muestras, no por eventos: sin ponerlo al dia, AI_LEN devolvia lo que
    // quedaba en el ultimo tick (fin de bufer, campo o escritura) y osAiGetLength veia el
    // bufer parado. aiTick reparte el credito con resto exacto, asi que trocearlo aqui no
    // cambia cuando acaba cada bufer.
    case 0x04: aiTick(cartNow()); return rcp.ai_play_remaining;   // bytes left in the playing buffer
    case 0x0c:                                 // AI_STATUS: FIFO_FULL(bit31) | DMA_BUSY(bit30)
      aiTick(cartNow());
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
      jitCancelChain();
      break;
    case 0x0c: {  // MI_MASK: (clear,set) pairs for the 6 interrupts
      for(u32 i = 0; i < 6; i++) {
        if(v & (1u << (2 * i)))     rcp.mi_mask &= ~(1u << i);
        if(v & (1u << (2 * i + 1))) rcp.mi_mask |=  (1u << i);
      }
      // Quitar la mascara a una interrupcion YA pendiente la hace visible en el acto: el
      // interprete la coge en la instruccion siguiente, asi que el bloque compilado tiene que
      // salir aqui en vez de seguir hasta su final. Ver Memory::armEpoch.
      jitCancelChain();
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
      // Threaded: rsp.running es del worker; la CPU solo puede pedirle que pare (Rsp::extHalt).
      if((v & (1 << 1)) && !(v & (1 << 0))) {
        if(rcpMode == RcpMode::Threaded) { if(!(tlIsRspThread || lockRspExec || tlDpLogApply)) rsp.extHalt.store(true, std::memory_order_release); }
        else { rsp.running = false; rsp.brake = false; }
      }
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
    // START/END con un SYNC_FULL en curso: riesgo de cuelgue del HW (aviso; STRICT = colgar).
    if(((off & 0xff) == 0x00 || (off & 0xff) == 0x04) && dpcHazardCheck(off & 0xff, v)) return;
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
      if(v & (1 << 9)) rcp.dpc_clock.store((u32)insnsToRcpCycles(cartNow()), std::memory_order_relaxed);
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
                   dbg::g.flip(rcp.viFlips, cartNow());   // depurador: cierra el cuadro
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
    case 0x0c: rcp.vi_intr = v & 0x3ff; viNextAt = viNextEvent(viLastRetired); evArm(); jitCancelChain(); break;
    case 0x10: {
      static int vilog = std::getenv("KESTREL_VILOG") ? 1 : 0;
      if(vilog) { static u32 n=0; if((n++ & 0x3f)==0)
        std::fprintf(stderr,"[vilog] ACK VI_CURRENT write #%u intr=%02x pc=%08x\n",n,(u32)rcp.mi_intr,(u32)storePc); }
      clearIntr(MI_VI); break;   // VI_CURRENT write acks the VI interrupt
    }
    case 0x14: rcp.vi_burst = v; break;
    case 0x18: rcp.vi_vsync = v; viNextAt = viNextEvent(viLastRetired); evArm(); jitCancelChain(); break;
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
        jitCancelChain();
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
    case 0x10: aiTick(cartNow()); rcp.ai_dacrate = v & 0x3fff; aiArm(); jitCancelChain(); break;
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
  rcp.vi_current = rcp.viCurrentAt(retiredNow, field) & ~(rcp.vi_ctrl & 0x40 ? 0u : 1u);
  if(intrFire) raiseIntr(MI_VI);
  if(fieldClose) {
    rcp.viFields++;
    rcp.viFieldOrigin[(retiredNow / field - 1) & 1] = rcp.vi_origin & 0x00ffffff;
    // Plazo del PIF para el 0x08 (fin del arranque), contado desde el encendido (instante 0
    // del reloj de invitado) y mirado por campo: la congelacion llega como mucho un campo
    // tarde, que para un plazo de 5 s da igual. KESTREL_PIFBOOTWAIT=0 lo quita (solo para
    // bisecar: en hardware no pasa).
    if(pifBootMode && !pifFrozen) {
      static const bool off = [] { const char* e = std::getenv("KESTREL_PIFBOOTWAIT"); return e && !std::strcmp(e, "0"); }();
      if(!pifBootDeadline) pifBootDeadline = usToInsns(kPifBootUs);
      if(!off && pifBootDeadline && retiredNow >= pifBootDeadline)
        pifFreeze("el cartucho no mando el fin de arranque (0x08) en 5 s");
    }
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

}  // namespace kestrel
