// RDP job execution and the RDP worker thread (threaded mode).
// Split out of memory.cpp; shared internals live in memory_internal.hpp.
#include "memory.hpp"
#include "memory_internal.hpp"
#include "wrtag.hpp"
#include "../telemetry/hostprof.hpp"
#include "../vrdp/vrdp.hpp"
#include "../gpurdp/gpurdp.hpp"
#include "../debug/dbg.hpp"

namespace kestrel {

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
  std::call_once(vrdpOnce, [this]{
    // KESTREL_GPURDP=1: el GPU-RDP propio en vez de parallel-RDP. Nunca los dos: comparten la
    // tabla global de volk y el presentador solo puede colgarse de un contexto.
    if(gpurdp::wanted()) gpurdp::init((u32)rdram.size());
    else vrdp::init(rdram.data(), (u32)rdram.size());
    vrdpTried.store(true, std::memory_order_release);
  });
}

// Espera a que el hilo del RDP haya levantado parallel-rdp. El presentador comparte ese
// contexto Vulkan, pero NO puede levantarlo el mismo: Granite registra indice de hilo y ata
// su estado al hilo que lo crea, asi que traerlo arriba fuera del hilo del RDP deja al worker
// sin sus lookups por hilo y el RDP acaba sin completar trabajos (el juego se queda esperando
// MESG_DP_COMPLETE para siempre). De ahi que esto solo espere.
auto Memory::vrdpFailed() const -> bool {
  return vrdpTried.load(std::memory_order_acquire) && !vrdp::active() && !gpurdp::active();
}

auto Memory::vrdpWaitReady(u32 timeoutMs) -> bool {
  if(gpurdp::built && gpurdp::wanted()) {             // GPU-RDP propio: esperar a ese
    for(u32 i = 0; i < timeoutMs; i++) {
      if(gpurdp::active()) return true;
      if(vrdpFailed()) return false;
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return false;
  }
  if(!vrdp::built) return false;                      // este .exe no lo lleva: nada que esperar
  const char* e = std::getenv("KESTREL_PRDP");
  if(e && e[0] == '0') return false;                  // SoftRDP forzado: nada que esperar
  for(u32 i = 0; i < timeoutMs; i++) {
    if(vrdp::active()) return true;
    if(vrdpFailed()) return false;                    // intentado y caido: no esperar mas
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
  // Comando cortado entre dos buffers DPC: aqui se descarta, pero el RDP real se queda
  // colgado a media instruccion esperando el resto (cmd_busy + dma_busy para siempre).
  static const bool splitLog = std::getenv("KESTREL_SPLITLOG") != nullptr;
  if(splitLog && rdpHasResume) std::fprintf(stderr, "[split] %06x..%06x -> %06x\n", rdpResume, rdpLastEnd, rawCur);
  if(rdpHasResume && rawCur != rdpLastEnd) {
    static int nsplit = 0;
    if(nsplit++ < 20)
      std::fprintf(stderr, "[rdp!] comando partido entre buffers DPC: %06x..%06x sin terminar, START nuevo %06x"
                           " (en HW cuelga el RDP)\n", rdpResume, rdpLastEnd, rawCur);
  }
  u32 costCur = rawCur;
  if(!preCosted) {
    if(rdpCostHasResume && rawCur == rdpCostLastEnd) costCur = rdpCostResume;
    rdpCostHasResume = false;
    rdpCostLastEnd = end;
  }
  rdpHasResume = false;
  rdpLastEnd = end;
  // Depurador (docs/DEBUGGER.md): graba los comandos del tramo, ya con la reanudacion
  // aplicada, desde la MISMA fuente que va a leer el rasterizador. Vale para los tres backends.
  struct DbgJobEnd { Memory* m; ~DbgJobEnd() { dbg::g.rdpJobEnd(m->cartNow()); } } dbgEnd{this};
  dbg::g.rdpJob(cmdSrc ? cmdSrc : rdram.data(), (u32)rdram.size(), dmem.data(), current, end, xbus,
                cartNow());
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
const u32 kSpinPauseMask = [] {
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
auto rdpSpinLen() -> u32 {
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
auto rcpWaitReportMs() -> unsigned {
  static const unsigned ms = []{
    const char* e = std::getenv("KESTREL_RCPWAIT");
    return e ? (unsigned)std::strtoul(e, nullptr, 10) : 2000u;
  }();
  return ms;
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

}  // namespace kestrel
