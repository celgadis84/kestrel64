// CPU/RSP/RDP synchronisation: pacing, guest-time barriers, rendezvous, RSP worker.
// Split out of memory.cpp; shared internals live in memory_internal.hpp.
#include "memory.hpp"
#include "memory_internal.hpp"
#include "../telemetry/hostprof.hpp"
#include "../net/netplay.hpp"
#include "runtime.hpp"
#include <algorithm>

namespace kestrel {

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
const bool g_spinPauseOn = [] {
  const char* e = std::getenv("KESTREL_SPINPAUSE");
  return !e || !*e || (std::strcmp(e, "0") && std::strcmp(e, "off"));
}();
// Cadencia de una de cada 16 vueltas en quien llama: PAUSE no cuesta lo mismo en todos los
// anfitriones (Nehalem ~9 ciclos, Skylake y posteriores ~140), y una por vuelta estiraria el
// giro de 2048 vueltas dos ordenes de magnitud en una maquina moderna.

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
// sobrado (GPU-RDP) el freno no llega a morder y el solape se conserva entero.
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
  dpJobSync[seq & kDpRingM]    = false;
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
  const u32 pipe0 = rcp.dpc_pipebusy.load(std::memory_order_relaxed);
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
  // El tramo acaba en SYNC_FULL (ventana del riesgo de dpcHazardCheck). Solo se sabe con el
  // pase de coste.
  dpJobSync[seq & kDpRingM] = costed && softCost.cmdTail == 1;
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
  // CMD_BUSY y PIPE_BUSY se separan en los huecos (n64brew, "Reality Display Processor/
  // Interface"): CMD_BUSY cuenta mientras el FIFO tiene comandos, PIPE_BUSY desde el primer
  // comando hasta el SYNC_FULL, tambien con el FIFO vacio esperando al RSP. El modelo de coste
  // ya suma el trabajo a los dos; al cerrar con SYNC_FULL, PIPE_BUSY recibe ademas el tiempo
  // de invitado de la ventana que no fue trabajo. Sin huecos (un tramo, como Thar0) no cambia.
  // Si el invitado borro el contador dentro de la ventana, lo trabajado sale mayor que la
  // ventana y no se suma nada.
  if(costed && current != end) {
    const u32 pbNow = rcp.dpc_pipebusy.load(std::memory_order_relaxed);
    if(!dpPipeFrom) { dpPipeFrom = t0 ? t0 : 1; dpPipeBusy0 = pipe0; }
    if(softCost.sawSyncFull) {
      const u64 span   = insnsToRcpCycles(t1) - insnsToRcpCycles(dpPipeFrom);
      const u32 worked = pbNow - dpPipeBusy0;
      if(span > worked) rcp.dpc_pipebusy.fetch_add((u32)(span - worked), std::memory_order_relaxed);
      dpPipeFrom = 0;
    }
  }
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
    if(!tlIsRspThread) jitCancelChain();
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
      jitCancelChain();
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

// Adelanto de la CPU en la cita de lectura de DPC (ver dpLogWait). PUESTO de fabrica en
// velocidad libre y QUITADO con `KESTREL_SPEEDMODE=hw`; las puertas lo fijan a 0 y por eso
// siguen siendo el oraculo determinista. OJO AL MEDIR A MANO: una corrida suelta sin la
// variable lo lleva PUESTO, o sea que el statehash NO tiene por que repetir -- y eso es
// ESTO, no el adelanto de la barrera del SP. Con la CPU por delante del RSP los eventos y plazos que nacen del RSP llegan tarde
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
  rdpHwHung.store(false, std::memory_order_relaxed);
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

// DMA RDRAM -> SP del RSP en `now`. El grano solo vale mientras el RSP no pueda saber que
// hora es en la CPU, y un DMA se lo dice: baja lo que la CPU escribio en RDRAM hasta `now`.
// Si las senales escritas ANTES siguieran aplazadas, el microcodigo veria el efecto sin la
// causa. Es el orden de rspq en libdragon: SET_SIG_HIGHPRI_REQUESTED, luego el comando
// WRITE_STATUS en la cola y luego SIG_MORE; con REQUESTED aun invisible el RSP bajaba la cola,
// no entraba en highpri, paraba y REQUESTED quedaba colgado (test_rspq_highpri_multiple:
// "rspq_highpri_sync wait loop timed out", y rapid_flush igual con SPLEAD). Las que ya tocan
// pasan a su flanco real, como en el lanzamiento; las de despues siguen con su grano. `upto`
// puede pasar de `now` (la CPU va por delante con KESTREL_SPLEAD y la copia ya trajo lo suyo):
// esas se ven desde `now`, no desde su flanco, o el RSP tendria el dato sin la senal.
auto Memory::spSigUpTo(u64 upto, u64 now) -> void {
  std::lock_guard<std::mutex> lk(spSigMx);
  for(u32 k = 0; k < spSigCount; ++k) {
    SpSigWr& w = spSigRing[(spSigHead + k) % kSpSigN];
    if(w.raw > upto) break;
    w.stamp = std::min(w.raw, now); w.flags &= ~1u;
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

// CLEAR_HALT de la CPU POSTERIOR al BREAK que aun no ha visto el HALT. En Threaded el fin de
// tarea se publica en un plazo (spEndArm) que va `spDateOps()` ops por detras del BREAK, el
// adelanto fijo de la CPU (KESTREL_SPLEAD). Una escritura SET_SIG_MORE|CLEAR_HALT que cae en
// ese hueco ve el nucleo "en marcha" y es un no-op; al vencer el plazo se publicaba HALT|BROKE
// encima y el RSP se quedaba parado con la senal puesta para siempre. En hardware el RSP ya
// estaba parado en ese instante y el CLEAR_HALT lo relanza, lleve senales o no: es justo la
// segunda escritura de rspq_flush_internal en libdragon ("RSP CRASH rspq_syncpoint_wait ...
// wait loop timed out" con SIG7 y HALT|BROKE). Con SPLEAD=0 el hueco no existe. Devuelve 0 si
// no hay ninguna, o 1 | (2 si la escritura llevaba CLEAR_BROKE). SOLO hilo de CPU.
auto Memory::spClearHaltAfter(u64 tBreak) -> u32 {
  std::lock_guard<std::mutex> lk(spSigMx);
  for(u32 k = 0; k < spSigCount; ++k) {
    SpSigWr& w = spSigRing[(spSigHead + k) % kSpSigN];
    if((w.flags & 1u) && w.raw > tBreak) {
      const u32 f = w.flags; w.flags &= ~1u;
      spLateRelaunch.fetch_add(1, std::memory_order_relaxed);
      return 1u | (f & 2u);
    }
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
  // KESTREL_RDP_STRICT=1 tras el riesgo de dpcHazardCheck: el RDP real queda colgado con
  // START_VALID|END_VALID|DMA_BUSY|CMD_BUSY y sin CBUF_READY (0x740, medido en consola).
  if(rdpHwHung.load(std::memory_order_relaxed)) return (st & 0x7u) | 0x740u;
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

// Riesgo de HW verificado en consola: escribir DPC_START o DPC_END mientras el RDP ejecuta un
// SYNC_FULL lo cuelga (CURRENT clavado a mitad de comando, DPC_STATUS 0x740, sin MI_DP). El
// modelo no separa el SYNC_FULL del resto del tramo, asi que la ventana es el tramo EN CURSO
// (arrancado y sin cerrar en el instante de invitado de la escritura) que acaba en SYNC_FULL.
// Por defecto solo avisa, una vez por corrida. KESTREL_RDP_STRICT=1 congela el RDP como la
// consola: START/END ignorados, STATUS fijo en 0x740 y MI_DP no vuelve a subir.
// Devuelve true si la escritura NO debe aplicarse (RDP colgado).
auto Memory::dpcHazardCheck(u32 reg, u32 v) -> bool {
  if(rdpHwHung.load(std::memory_order_relaxed)) return true;
  if(!dpGuestOn()) return false;
  const u64 now = lockRspExec ? rspGuestNowAt(rsp.exactCycles())
                : tlDpLogApply ? tlDpLogAt : tlIsRspThread ? rspGuestNow() : cartNow();
  const u64 sub = dpSubSeq.load(std::memory_order_acquire);
  bool hit = false; u64 j = 0;
  for(u64 k = sub; k > 0 && sub - k < 8; --k) {     // el motor es uno: a lo sumo un tramo en curso
    const u64 i = (k - 1) & kDpRingM;
    if(dpJobEndG[i] <= now) break;                   // este y los anteriores ya cerraron
    if(dpJobStartG[i] <= now) { hit = dpJobSync[i]; j = i; break; }
  }
  if(!hit) return false;
  static const bool strict = [] { const char* e = std::getenv("KESTREL_RDP_STRICT"); return e && *e && *e != '0'; }();
  if(!dpHazardWarned) {
    dpHazardWarned = true;
    const bool fromRsp = tlIsRspThread || tlDpLogApply || lockRspExec;
    std::fprintf(stderr, "[rdp] HW hazard: DPC_%s written while SYNC_FULL in progress (hangs real HW): "
                 "v=%06x by %s pc=%08llx span=%06x..%06x at=%llu end=%llu%s\n",
                 reg == 0 ? "START" : "END", v & 0xffffffu, fromRsp ? "RSP" : "CPU",
                 fromRsp ? (unsigned long long)(tlDpLogApply ? 0 : rsp.pc)
                         : (unsigned long long)storePc,
                 dpJobAddr[j], dpJobEndAddr[j], (unsigned long long)now,
                 (unsigned long long)dpJobEndG[j], strict ? " -- RDP congelado (KESTREL_RDP_STRICT)" : "");
    std::fflush(stderr);
  }
  if(!strict) return false;
  rdpHwHung.store(true, std::memory_order_relaxed);
  return true;
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
  rspRdpHostDrain();
}

// El RSP acaba de LEER el RDP parado (DPC_STATUS sin CMD_BUSY/DMA_BUSY) en su horario de
// invitado. Eso es informacion que el microcodigo le pasa a la CPU (libdragon rdpq_fence:
// SYNC_FULL + bucle sobre DPC_STATUS, y luego la CPU lee el framebuffer), asi que en el
// anfitrion los pixeles tienen que estar ya en la RDRAM: esperar al drenado entero del worker
// (bit2 de rcpPend, que con GPU-RDP incluye el fence de la GPU). Solo tiempo de
// anfitrion: el reloj del RSP no avanza mientras, el invitado sale identico.
auto Memory::rspRdpIdleWait() -> void {
  if(!tlIsRspThread || !dpBarrierOn()) return;
  if(!(rcpPend.load(std::memory_order_acquire) & 4u)) return;
  rspIdleRdpWaits.fetch_add(1, std::memory_order_relaxed);
  rspRdpHostDrain();
}

auto Memory::rspRdpHostDrain() -> void {
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
  // Juego en red: el modo reproducible (SPLEAD=0), o dos maquinas divergen con las mismas
  // entradas.
  return netplay::lockstep() ? 0ull : v;
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

auto Memory::spDateOps() -> u64 {
  static const s64 kSpDate = []{ const char* e = std::getenv("KESTREL_SPDATE");
                                 return (e && *e) ? (s64)std::strtoll(e, nullptr, 10) : (s64)-1; }();
  return kSpDate < 0 ? spLeadOps() : (u64)kSpDate;
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
  spDoneAt = spCycleAt(cyclesUsed) + spDateOps();
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
    // CLEAR_HALT de la CPU entre el BREAK y este plazo: relanza en vez de parar.
    // KESTREL_SPRELAUNCH=0: A/B, vuelve a parar siempre (el fallo de libdragon reaparece).
    static const bool kRelaunch = []{ const char* e = std::getenv("KESTREL_SPRELAUNCH");
                                      return !(e && *e == '0'); }();
    const u64 date = spDateOps();
    if(kRelaunch && !tlIsRspThread && rcpMode == RcpMode::Threaded && date) {
      if(u32 f = spClearHaltAfter(spDoneAt >= date ? spDoneAt - date : 0)) {
        spRets.fetch_add(1, std::memory_order_relaxed);
        if(rcp.sp_intr_on_break.load(std::memory_order_acquire)) raiseIntr(MI_SP);
        if(!(f & 2u)) rcp.sp_status.fetch_or(2u, std::memory_order_acq_rel);   // BROKE
        spSigAtKick(); rsp.mem = this; rsp.start(); rspSubmitKick();
        pend &= ~1u;
      }
    }
  }
  if(pend & 1u) {
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
    // prologo; en Threaded con plazos esa guarda ya no se emite (ver jit_compile.cpp), asi que se
    // anula el permiso igual que siDma o dpScheduleSpan.
    jitCancelChain();
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

// Ocupacion del RSP hasta AHORA: lo cobrado por tareas acabadas mas la parte en vuelo de
// la actual (menos lo aparcado de ella). Lectura de tres valores sin candado: si la tarea
// empieza o acaba entre medias, rspTaskT0Ns cambia y se reintenta.
auto Memory::rspBusyNow() const -> u64 {
  for(;;) {
    u64 t0 = rspTaskT0Ns.load();
    u64 busy = rspBusyNs.load();
    u64 park0 = rspTaskPark0.load();
    u64 park = rspParkNs.load();
    if(rspTaskT0Ns.load() != t0) continue;
    if(!t0) return busy;
    u64 now = (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count();
    u64 dt = now > t0 ? now - t0 : 0, slept = park - park0;
    return busy + (dt > slept ? dt - slept : 0);
  }
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
    rspTaskPark0.store(park0);
    rspTaskT0Ns.store((u64)std::chrono::duration_cast<std::chrono::nanoseconds>(
                        t0.time_since_epoch()).count() | 1);
    rsp.step(~0ull);   // start() ya corrio en el hilo CPU al escribir CLEAR_HALT
    // Ocupacion = tiempo DENTRO de la tarea menos lo que se paso dormido en el aparcamiento:
    // un hilo aparcado no esta trabajando. Ver rspParkNs.
    {
      u64 dt = (u64)std::chrono::duration_cast<std::chrono::nanoseconds>(
                 std::chrono::steady_clock::now() - t0).count();
      u64 slept = rspParkNs.load(std::memory_order_relaxed) - park0;
      rspBusyNs.fetch_add(dt > slept ? dt - slept : 0);
      rspTaskT0Ns.store(0);   // DESPUES de sumar: rspBusyNow() reintenta si ve el cambio
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
