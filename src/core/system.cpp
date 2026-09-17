#include <cctype>
#include "system.hpp"
#include "savestate.hpp"
#ifdef _WIN32
#include <windows.h>
#endif
#include "../telemetry/server.hpp"
#include "../telemetry/hostprof.hpp"
#include "movie.hpp"
#include "../audio/audio.hpp"
#include "../vrdp/vrdp.hpp"   // vrdp::built: si ESTE .exe lleva el backend de GPU
#include "runtime.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace kestrel {

System::System() = default;

System::~System() {
  requestShutdown();
  memory.flushSaveFile();   // persist battery/flash save to disk on exit
  memory.stopRcpThreads();
  stopTelemetry();
}

auto System::stopTelemetry() -> void {
  if(tele) tele->stop();
  if(teleThread.joinable()) teleThread.join();
}

auto System::init(const std::string& romPath, std::string& error) -> bool {
  // Memoria de la consola. Por defecto los 8 MB del Expansion Pak, que es lo que quiere
  // cualquiera que juegue hoy (y lo que EXIGEN Donkey Kong 64 y Perfect Dark en un jugador),
  // pero la N64 de serie trae 4 MB y hay juegos que se comportan distinto segun lo que
  // encuentren: reservan menos buferes, bajan la resolucion o directamente se niegan. El
  // tamano no se puede cambiar en caliente (la RDRAM se dimensiona una vez y los estados
  // guardados llevan el suyo dentro), asi que se decide aqui y se queda.
  bool expansionPak = true;
  if(const char* e = std::getenv("KESTREL_RDRAM")) {
    std::string v(e);
    for(auto& c : v) c = (char)std::tolower((unsigned char)c);
    if(v == "4" || v == "4mb" || v == "0") expansionPak = false;
  }
  memory.reset(expansionPak);
  // El tamano no se anuncia solo: el invitado lo lee en RDRAM 0x318 (osMemSize) y 0x3F0,
  // que el arranque rapido rellena desde rdram.size(), y el camino rapido del dynarec lo
  // acota con jitRdramSz, que sale del mismo sitio al compilar cada bloque.
  std::printf("[system] RDRAM %u MB%s\n", (unsigned)(memory.rdram.size() >> 20),
              expansionPak ? " (Expansion Pak)" : " (consola de serie)");
  if(!rom.loadFile(romPath, error)) return false;
  memory.loadRom(rom.data);  // exposes CART_ROM region for telemetry
  memory.attachSaveFile(romPath);  // load existing .eep/.sra/.fla, if any, next to the ROM
  this->romPath = romPath;         // base de los nombres de ranura de estado (rom.stN)
  cheats.loadForRom(romPath);      // KESTREL_CHEATS, o el .cht que haya al lado de la ROM
  std::printf("[system] loaded \"%s\" (%s, %.2f MB, entry 0x%08x)\n",
              rom.header.name.c_str(),
              rom.originalOrder == Rom::Order::Z64 ? "z64" :
              rom.originalOrder == Rom::Order::N64 ? "n64" :
              rom.originalOrder == Rom::Order::V64 ? "v64" : "?",
              rom.data.size() / (1024.0 * 1024.0), rom.header.entryPoint);
  // Norma de television: la del cartucho. En la consola de verdad la region del cartucho y
  // la de la maquina coinciden, y libultra publica la de la maquina en osTvType; los juegos
  // la leen y algunos se niegan a funcionar con la equivocada (Perfect Dark PAL entra en un
  // `while(1)` dentro de mainInit si lee NTSC, y deja la pantalla en negro para siempre).
  // De la norma cuelga tambien el ritmo de campo, que es el reloj de todo el emulador.
  {
    TvType tv = tvTypeForCountry(rom.header.countryCode);
    if(const char* o = std::getenv("KESTREL_TVTYPE")) {   // por si una ROM trae mal la region
      std::string v(o);
      for(auto& c : v) c = (char)std::tolower((unsigned char)c);
      if(v == "pal" || v == "0") tv = TvType::Pal;
      else if(v == "ntsc" || v == "1") tv = TvType::Ntsc;
      else if(v == "mpal" || v == "2") tv = TvType::Mpal;
    }
    cpu.bootTvType = (u32)tv;
    clocks.viFieldHz = tvFieldHz(tv);
    memory.aiVidClock = tvVidClock(tv);
    std::printf("[system] region '%c' -> %s (%.2f campos/s)\n",
                rom.header.countryCode >= 0x20 ? rom.header.countryCode : '?',
                tvName(tv), clocks.viFieldHz);
  }
  if(const char* w = std::getenv("KESTREL_WATCH")) {
    memory.watchAddr = (u32)std::strtoul(w, nullptr, 0) & 0x1fff'ffff;
    if(const char* l = std::getenv("KESTREL_WATCHLEN")) memory.watchLen = (u32)std::strtoul(l, nullptr, 0);
    std::printf("[watch] store watchpoint at phys 0x%08x len %u\n", memory.watchAddr, memory.watchLen);
  }
  cpu.connect(&memory);
  // Overclock por dominio. El modelo de reloj (Clocks) ya llevaba los multiplicadores;
  // esto solo los cablea a la linea de ordenes del lanzador. KESTREL_OC pone los tres a
  // la vez; los tres especificos lo pisan. Un multiplicador NO cambia la semantica: sube
  // el ritmo objetivo de retiro (insnTarget), o sea cuantas instrucciones caben en un
  // campo de video. Subir el de CPU cambia cuanto trabajo hace el juego entre campos --
  // que es justo lo que quita la ralentizacion, y tambien lo que puede romper un juego
  // que ate su logica al reloj. RDRAM solo mueve el modelo de ancho de banda.
  //
  // El modo fiel a consola (KESTREL_SPEEDMODE=hw, ver rt::speedModeHw) desarma todo esto: si
  // lo que se pide es la velocidad de la maquina real, un multiplicador heredado del entorno
  // o de un perfil viejo la falsearia sin que se note. Por eso no se "avisa" del conflicto:
  // se ignora la variable, que es lo unico que deja el modo fiel siendo fiel.
  const bool hwSpeed = rt::speedModeHw();
  {
    auto mul = [hwSpeed](const char* n, double& dst) {
      if(hwSpeed) return;
      if(const char* v = std::getenv(n)) {
        double d = std::strtod(v, nullptr);
        if(d > 0.0) dst = d;           // 0 o basura = no tocar
      }
    };
    double all = 0.0;
    if(!hwSpeed)
      if(const char* g = std::getenv("KESTREL_OC")) { double d = std::strtod(g, nullptr); if(d > 0.0) all = d; }
    if(all > 0.0) { clocks.cpuOc = clocks.rspOc = clocks.rdramOc = all; }
    mul("KESTREL_OC_CPU",   clocks.cpuOc);
    mul("KESTREL_OC_RSP",   clocks.rspOc);
    mul("KESTREL_OC_RDRAM", clocks.rdramOc);
    if(clocks.cpuOc != 1.0 || clocks.rspOc != 1.0 || clocks.rdramOc != 1.0)
      std::printf("[system] overclock cpu=%.2fx rsp=%.2fx rdram=%.2fx (cpu %.2f MHz, rsp %.2f MHz)\n",
                  clocks.cpuOc, clocks.rspOc, clocks.rdramOc,
                  clocks.cpuTarget() / 1e6, clocks.rspTarget() / 1e6);
  }
  if(const char* t = std::getenv("KESTREL_VITICKS")) {
    u32 n = (u32)std::strtoul(t, nullptr, 0);
    if(n) clocks.viTicksPerField = n;
  }
  // El CPI sale de un solo sitio (CPU::cpiFromEnv, en 1/128 de ciclo) y de ahi cuelgan LAS DOS
  // cosas que dependen de el: el ritmo del reloj Count dentro de la CPU y el presupuesto de
  // instrucciones por campo de este bucle. Derivarlo aqui en vez de volver a leer la variable
  // de entorno es lo que impide que los dos relojes discrepen.
  // Fiel a consola: tambien el CPI vuelve al calibrado de fabrica. Es la otra mitad del
  // presupuesto de tiempo del invitado -- de nada sirve el reloj nativo si el numero de
  // instrucciones que caben en un campo viene de un KESTREL_CPI puesto a mano.
  if(hwSpeed) cpu.cpi256 = CPU::kCpiDefault256;
  clocks.cyclesPerInsn = (double)cpu.cpi256 / 128.0;
  // Plazos que son tiempo de verdad y viven en el reloj de ops: 1 op = cpi256/128 ciclos.
  memory.cartLatchTtl = Memory::CART_LATCH_TTL_CYCLES * 128 / cpu.cpi256;
  if(hwSpeed)
    std::printf("[system] modo FIEL A CONSOLA: CPU %.2f MHz, RSP %.2f MHz, RDRAM x1.00, "
                "CPI %.4f, %.2f campos/s clavados\n",
                clocks.cpuTarget() / 1e6, clocks.rspTarget() / 1e6,
                (double)cpu.cpi256 / 128.0, clocks.viFieldHz);
  if(clocks.cpiIsHistoric() == false)
    std::printf("[system] CPI=%.4f -> %llu instrucciones por campo (modelo historico: 2 -> %llu)\n",
                clocks.cyclesPerInsn, (unsigned long long)clocks.fieldInsns(),
                (unsigned long long)(u64)(clocks.cpuTarget() / 2.0 / clocks.viFieldHz + 0.5));
  // Un solo reloj de video para todo el emulador: el mismo numero de instrucciones
  // por campo que usa el bucle de System (stepCpu) lo usa la lectura de VI_V_CURRENT.
  memory.viFieldInsns = clocks.fieldInsns();
  // ...y la frecuencia de campo en mili-hercios, que es lo que el drenaje del AI necesita
  // para convertir instrucciones retiradas en segundos de guest sin coma flotante.
  memory.viFieldHzMilli = (u32)(clocks.viFieldHz * 1000.0 + 0.5);
  // ...y un solo modelo de CPI: el mismo ratio CPU:RSP para el interleave de Lockstep
  // (aqui) y para el regulador de Threaded (Memory::rcpPace).
  rspStepNum = (u64)(clocks.rspInsnsPerCpuInsn() * 65536.0 + 0.5);
  rspStepDen = 65536;
  memory.setPaceRatio(rspStepDen, rspStepNum);   // inverso: CPU por RSP
  // Peliculas de entrada (TAS). Se arma con el cartucho ya cargado y la RDRAM ya
  // dimensionada porque la cabecera guarda los dos CRC del cartucho, la norma de TV y el
  // tamano de memoria: reproducir una partida en una maquina distinta de la que la grabo no
  // es reproducirla.
  { u8 ports = 0;
    for(int i = 0; i < 4; i++) if(memory.padPort[i].connected) ports |= (u8)(1u << i);
  rewinder.init();
    movie::init(rom.header.crc1, rom.header.crc2, rom.header.name, (u8)cpu.bootTvType,
                (u8)(memory.rdram.size() >> 20), ports); }
  cpu.fastBoot(rom.header.entryPoint);  // HLE IPL3: boot segment in RDRAM, PC at entry
  std::printf("[cpu] %s boot, pc=0x%08x\n",
              (u32)cpu.pc == 0xa4000040u ? "LLE IPL3" : "HLE", (u32)cpu.pc);
  if(envFlag("KESTREL_THREADS", true)) {
    memory.rcpMode = Memory::RcpMode::Threaded;
    memory.startRcpThreads();
    std::printf("[system] RCP threading ON — RDP rasterizes on its own thread\n");
  }
  return true;
}

// Leído una vez al arranque: como `static` local se comprobaba la variable-guarda de
// inicialización en CADA vuelta del bucle, o sea por instrucción emulada.
static const bool g_jitOn = envFlag("KESTREL_JIT", true);

// Diagnostico opt-in: vigilante de las listas de hilos de libultra. KESTREL_QCHK=<addr>
// apunta a la variable de cabecera de la cola (p.ej. __osRunQueue) y cada KESTREL_QCHKN
// instrucciones se recorre la cadena exigiendo el invariante del kernel: todo hilo
// encolado esta RUNNABLE(2) y la cadena termina en el centinela de prioridad -1. La
// primera violacion vuelca el anillo de PCs y el de eventos, que es justo la ventana
// donde se rompio la seccion critica del guest.
static auto guestQueueBroken(CPU& cpu, u32 qaddr, char* why, size_t whyN) -> bool {
  // Lectura COHERENTE: las estructuras del kernel viven en KSEG0, asi que una linea
  // sucia de la D-cache tapa lo que hay en RDRAM. Mirar la RAM cruda daria falsos
  // positivos (se ve el valor viejo de una cabeza de cola ya reescrita en cache).
  auto rd32 = [&](u32 a) -> u32 {
    u32 p = a & 0x1fff'ffffu;
    return ((u32)cpu.peekPhysCoherent(p) << 24) | ((u32)cpu.peekPhysCoherent(p + 1) << 16)
         | ((u32)cpu.peekPhysCoherent(p + 2) << 8) | (u32)cpu.peekPhysCoherent(p + 3);
  };
  u32 h = rd32(qaddr);
  for(int n = 0; n < 64; n++) {
    if(h < 0x8000'0000u || h >= 0x8080'0000u) {
      std::snprintf(why, whyN, "puntero %08x invalido en el nodo %d", h, n); return true; }
    if(rd32(h + 0x04) == 0xffff'ffffu) return false;   // centinela __osThreadTail: sana
    // __osEnqueueThread deja thread->queue apuntando a la cabecera donde lo mete, asi
    // que un hilo enlazado aqui cuyo campo queue diga otra cosa esta en dos listas.
    u32 q = rd32(h + 0x08);
    if(q != qaddr) {
      std::snprintf(why, whyN, "hilo %08x enlazado en la cola pero su campo queue dice %08x (state=%u)",
                    h, q, rd32(h + 0x10) >> 16); return true; }
    h = rd32(h + 0x00);
  }
  std::snprintf(why, whyN, "cadena sin centinela tras 64 nodos");
  return true;
}

auto System::stepCpu(u64 n) -> u64 {
  const bool jitOn = g_jitOn;
  static const u32 qchkAddr  = std::getenv("KESTREL_QCHK")
                             ? (u32)std::strtoul(std::getenv("KESTREL_QCHK"), nullptr, 0) : 0u;
  static const u32 qchkEvery = !qchkAddr ? 0u : std::getenv("KESTREL_QCHKN")
                             ? (u32)std::strtoul(std::getenv("KESTREL_QCHKN"), nullptr, 0) : 1024u;
  static const u32 qchkAfter = std::getenv("KESTREL_QCHKAFTER")
                             ? (u32)std::strtoul(std::getenv("KESTREL_QCHKAFTER"), nullptr, 0) : 30u;
  static u32 qchkTick = 0;
  const bool paced = memory.rcpMode == Memory::RcpMode::Threaded;
  // Reloj de invitado del interleave CPU:RSP. El ratio de Clocks es RSP por
  // instruccion-equivalente de CPU, y una instruccion que se para en la cache NO vale
  // lo mismo que una que retira limpia: el RSP corre igual durante esos ~60 ciclos de
  // latencia de RDRAM. guestOps() = retiradas + paradas ya convertidas a equivalentes,
  // que es exactamente el reloj en que nacen y vencen el campo de video, el plazo del SI
  // y el latch del PI. Con KESTREL_CACHECOST apagado avanza 1 por instruccion y esto
  // queda byte a byte igual que el "rspPhase += rspStepNum" de siempre.
  u64 lastGuestOps = cpu.guestOps();
  // Lockstep: interleave the RSP at Clocks::rspInsnsPerCpuInsn() (4/3 stock).
  // Threaded: the RSP runs to completion on its own worker (see rspWorkerLoop),
  // so the CPU thread must NOT also step it -- that would double-execute the core.
  // Corre tras cada paso del interprete Y tras cada bloque del JIT: un bloque que lanza el
  // RSP (CLEAR_HALT en mitad del bloque) sigue hasta su final, y si aqui no se pusiera al
  // dia el reloj de ese tramo se perdia (lastGuestOps ya apuntaba al final del bloque): el
  // RSP arrancaba con los ciclos de la cola del bloque de menos y MI_SP llegaba una
  // instruccion tarde con JIT (DK64 Lockstep).
  auto rspInterleave = [&] {
    u64 nowGuestOps = cpu.guestOps();
    u64 dGuestOps   = nowGuestOps - lastGuestOps;
    lastGuestOps    = nowGuestOps;
    // Reloj del RCP ABSOLUTO: el RSP da un ciclo en cada flanco rcpOpsToCycles(ops) que cruza
    // la CPU, contado desde el lanzamiento (spKickOps) y no desde el principio del paso, que
    // con un bloque del JIT puede ser anterior al CLEAR_HALT. Antes un acumulador de fase que
    // solo avanzaba con el RSP en marcha arrastraba el resto de la tarea anterior, y el fin
    // caia +-1 instruccion distinto del plazo de Threaded (spCycleAt): DK64 divergia ahi.
    if(memory.rcpMode == Memory::RcpMode::Lockstep && memory.rsp.running && dGuestOps) {
      u64 from  = std::max(nowGuestOps - dGuestOps, memory.spKickOps);
      u64 steps = nowGuestOps > from
                ? memory.rcpOpsToCycles(nowGuestOps) - memory.rcpOpsToCycles(from) : 0;
      memory.lockRspExec = true;
      while(steps--) { memory.rsp.step(1); if(!memory.rsp.running) break; }
      memory.lockRspExec = false;
    }
  };
  u64 i = 0;
  while(i < n && !cpu.halted) {
    // Dynarec: intenta un bloque de ops seguras. Declina (0) cuando el RSP corre, cerca
    // de un borde de timer/interrupt, o ante una op no soportada → cae al intérprete.
    // El bloque solo se toma con el RSP parado, así que no altera el interleave 2:3.
    if(jitOn) {
      // Ventana que le queda a esta llamada: una cadena de bloques enlazados no puede
      // pasarse de aquí, o el bucle de arriba tickearía el VI tarde (campo estirado).
      cpu.jitOpsBudget = (u32)((n - i) > 0xFFFF'FFFFull ? 0xFFFF'FFFFull : (n - i));
      u32 k = cpu.jitTryBlock();
      if(k) { i += k;
              // Un bloque nunca cruza un plazo, pero el cobro del bucle ocioso (jitIdleSkip)
              // puede dejar el reloj EXACTAMENTE en el: el interprete lo remata tras esa
              // instruccion, asi que aqui tambien. Sin esto MI_SI llegaba una op tarde con JIT
              // (DK64 Lockstep divergia del interprete en el hilo ocioso).
              if(memory.siBusy && memory.cartNow() >= memory.siDoneAt) memory.siFinish();
              if(memory.rcpPend.load(std::memory_order_relaxed)) memory.rcpRetire();
              rspInterleave();
              if(paced) memory.rcpPace(cpu.guestOps()); continue; }
    }
    cpu.step();
    i++;
    // Vencimiento del plazo del SI (transaccion de la PIF/joybus en vuelo). Se mira por
    // instruccion, no por tramo, porque el instante en que llega MI_SI tiene que ser el
    // MISMO en los siete modos: aqui se remata en la instruccion exacta y el JIT tiene
    // prohibido meter el plazo dentro de un bloque (guarda en jitTryBlock).
    // El plazo se ARMA en cartNow() (retiradas + pendientes del JIT + paradas de cache) y
    // por tanto se tiene que VENCER en el mismo reloj. Compararlo contra cpu.retired a secas
    // mezclaba dos relojes: con el coste de cache encendido, siDoneAt nace desplazado por
    // TODAS las paradas acumuladas hasta ese momento, no por lo que dura la transaccion, asi
    // que el plazo se alejaba mas cuanto mas llevaba corriendo el juego. SM64 con
    // KESTREL_CACHECOST=60 se quedaba sin lecturas de mando y sin dibujar nada.
    if(memory.siBusy && memory.cartNow() >= memory.siDoneAt) memory.siFinish();
    // Y el fin de tarea del RCP en Threaded, por lo mismo: lo arma un worker con el coste ya
    // modelado y se hace visible cuando el reloj de invitado llega, no cuando el anfitrion
    // termina de calcular. Con Lockstep o con KESTREL_RCPDEADLINE=0 nunca hay nada armado y
    // esto es una lectura atomica relajada que sale en cero.
    if(memory.rcpPend.load(std::memory_order_relaxed)) memory.rcpRetire();
    // Regulador Threaded: el equivalente al interleave 2:3 de abajo. Cada 64 ops basta —
    // es una lectura atomica relajada y el margen del regulador es de miles de ops.
    if(paced && (i & 0x3F) == 0) memory.rcpPace(cpu.guestOps());
    // Solo se mira con interrupciones habilitadas y fuera de excepcion: dentro de
    // __osDisableInt el kernel esta a medio enlazar y el invariante no aplica.
    // El arranque no cuenta: bzero de las estructuras del kernel viola el invariante
    // legitimamente, asi que la vigilancia empieza pasados unos campos de video.
    if(qchkEvery && ++qchkTick >= qchkEvery && memory.rcp.viFlips >= qchkAfter
       && ((u32)cpu.cop0[12] & 0x7) == 0x1) {
      qchkTick = 0;
      char why[160];
      if(guestQueueBroken(cpu, qchkAddr, why, sizeof why)) {
        std::fprintf(stderr, "[qchk] cola %08x ROTA: %s  retired=%llu pc=%08x\n",
                     qchkAddr, why, (unsigned long long)cpu.retired, (u32)cpu.pc);
        cpu.wDump(48);
        cpu.pcRingDump(400);
        memory.evDump(60);
        std::fflush(stderr);
        cpu.halted = true;
        return i;
      }
    }
    // Debug breakpoints: fire when the *next* PC to execute matches (standard
    // "stop before executing addr" semantics). Only scanned while armed → the
    // common no-breakpoint case pays a single empty()-check per instruction.
    if(!breakpoints.empty()) {
      u32 pcv = (u32)cpu.pc;
      for(u32 b : breakpoints) {
        if(b == pcv) { lastBpHit.store(pcv); paused.store(true); return i; }
      }
    }
    rspInterleave();
  }
  return i;
}

auto System::startTelemetry(u16 port) -> bool {
  tele = std::make_unique<telemetry::Server>(*this);
  teleThread = std::thread([this, port] { tele->serve(port); });
  return true;
}

auto System::startVideo(bool batch) -> void {
  // La ventana viene por defecto en sesion interactiva: quien corre el emulador a mano
  // quiere verlo. En lote (--run: gates, bench, krom) no, porque abrir una ventana por
  // ROM cambia lo que se mide y encima requiere escritorio. KESTREL_VIDEO fuerza el si
  // (util para ver un caso de gate), KESTREL_NOVIDEO fuerza el no.
  bool want = !batch;
  // Por VALOR y no por presencia: "KESTREL_VIDEO=0" tiene que apagar la ventana, no
  // encenderla, que es lo que hacia mientras solo se miraba si la variable existia.
  if(const char* v = std::getenv("KESTREL_VIDEO"))   want = (v[0] != '0');
  if(const char* v = std::getenv("KESTREL_NOVIDEO")) want = (v[0] == '0');
  if(!want) return;
  videoOn = presenter.start(&memory, &shutdown, &n64SpeedPct, &rspSpeedPct, &rdramSpeedPct,
                            rom.valid() ? rom.header.name.c_str() : nullptr);
  presenter.bindState(&stateSaveReq, &stateLoadReq, &stateSlot);
  presenter.bindMenu(&paused, romPath);
  presenter.bindFrameAdvance(&stepFields);
  presenter.bindRewind(&rewindReq);
  if(videoOn) std::printf("[video] VI presentation armed (window opens on main thread)\n");
}

// Espera hasta `due`. El sleep normal de Windows tiene una granularidad de ~15.6 ms, que es
// inservible para un campo de video de 16.68 ms: dormiria un campo entero de mas. El
// temporizador de alta resolucion (Win10 1803+, solo kernel32) baja a ~0.5 ms. El ultimo
// medio milisegundo se gira, que es lo que cuesta despertar de todas formas.
static auto sleepUntilPrecise(std::chrono::steady_clock::time_point due) -> void {
  using namespace std::chrono;
#ifdef _WIN32
  static HANDLE hTimer = CreateWaitableTimerExW(nullptr, nullptr,
                            CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
  if(hTimer) {
    auto left = due - steady_clock::now() - microseconds(500);
    if(left > microseconds(0)) {
      LARGE_INTEGER li;                                   // negativo = tiempo relativo, 100 ns
      li.QuadPart = -(LONGLONG)(duration_cast<nanoseconds>(left).count() / 100);
      if(li.QuadPart < 0 && SetWaitableTimer(hTimer, &li, 0, nullptr, nullptr, FALSE))
        WaitForSingleObject(hTimer, INFINITE);
    }
    while(steady_clock::now() < due) std::this_thread::yield();
    return;
  }
#endif
  auto left = due - steady_clock::now() - microseconds(1500);
  if(left > microseconds(0)) std::this_thread::sleep_for(left);
  while(steady_clock::now() < due) std::this_thread::yield();
}

auto System::runLoop() -> void {
  if(!videoOn) { run(); return; }
  // Bring up Vulkan/GLFW on THIS (main) thread BEFORE the CPU worker starts —
  // a CPU-bound sibling stalls the driver's device bring-up on this box.
  // El backend GPU tiene que estar arriba ANTES de abrir la ventana: el presentador comparte
  // su contexto Vulkan (ver vrdp::sharedVk), y dos contextos vivos se pisan la tabla global de
  // punteros de volk. Pero levantarlo aqui NO vale: Granite ata su estado al hilo que lo crea
  // y ese tiene que seguir siendo el del RDP. Asi que se arranca la CPU primero (con ella los
  // hilos del RCP) y aqui solo se espera a que el hilo del RDP lo tenga listo.
  std::thread cpuThread([this] { run(); });
  // "Querer" parallel-RDP solo tiene sentido si este .exe lo lleva compilado. El paquete
  // trae DOS ejecutables (kestrel64.exe con GPU, kestrel64-soft.exe sin ella): en el de
  // SoftRDP, dar por bueno el default "GPU si" hacia esperar quince segundos a un backend
  // que no existe y despues NO abrir la ventana -- el emulador inutil por doble clic.
  // Sin backend se va derecho a abrir la ventana, que es lo unico que se puede hacer.
  const char* pe = std::getenv("KESTREL_PRDP");
  bool prdpWanted = vrdp::built && (!pe || pe[0] != '0');
  bool prdpReady  = memory.vrdpWaitReady(15000);
  if(pe && pe[0] != '0' && !vrdp::built)
    std::printf("[video] este ejecutable no lleva parallel-RDP; se rasteriza con SoftRDP\n");
  if(prdpWanted && !prdpReady)
    std::printf("[video] parallel-rdp no arranco a tiempo; sin ventana\n");
  else if(!presenter.open())
    std::printf("[video] init failed; running headless\n");
  while(!shutdown.load() && presenter.pumpFrame()) {}
  shutdown.store(true);      // window closed or stop → unwind the CPU worker
  cpuThread.join();
  presenter.close();
}

// Hash of the VI framebuffer exactly as the headless dump reads it (origin, VI_WIDTH
// stride, bpp from VI_CTRL). Region size only — the point is "did the picture move",
// not what it looks like, so a cheap FNV-1a over the bytes is enough.
static auto framebufferHash(Memory& mem) -> u64 {
  u32 origin = mem.rcp.vi_origin & 0x00ff'ffff;
  u32 type   = mem.rcp.vi_ctrl & 3;                        // 2=16bpp, 3=32bpp
  if(type != 2 && type != 3) return 0;
  u32 w = mem.rcp.vi_width ? mem.rcp.vi_width : 320;
  if(w == 0 || w > 640) w = 320;
  u32 ysc = mem.rcp.vi_yscale & 0xfff;
  u32 baseH = (mem.rcp.viHalflines() >= 550) ? 288 : 240;
  u32 h = ysc ? ((baseH * ysc) >> 10) : baseH;
  if(h == 0 || h > 576) h = baseH;
  usize bytes = (usize)w * h * (type == 2 ? 2 : 4);
  const auto& ram = mem.rdram;
  if((usize)origin + bytes > ram.size()) return 0;
  u64 hsh = 1469598103934665603ull ^ origin ^ ((u64)w << 32) ^ ((u64)type << 48);
  for(usize i = 0; i < bytes; i++) { hsh ^= ram[origin + i]; hsh *= 1099511628211ull; }
  return hsh;
}

// Atiende una peticion de guardar/cargar estado. Corre en el hilo de ejecucion, entre
// subtramos, que es el unico punto donde se puede dejar el RCP quieto de verdad:
//
//   - el RDP se drena (en modo hilos la cola la consume su propio hilo; con comandos a
//     medias, el estado guardado tendria un FIFO que apunta a una lista que ya no existe);
//   - la tarea del RSP se termina. El microcodigo se guarda con el nucleo PARADO a
//     proposito: en modo hilos una tarea a medias vive en el worker y nadie la reanudaria
//     tras cargar, asi que un estado con el RSP corriendo se colgaria justo al cargarlo en
//     el otro modo de RCP. Terminandola, el fichero vale en Lockstep y en Threaded.
//
// El coreMutex se coge aqui, no dentro de saveState/loadState: la telemetria puede estar
// leyendo registros en el mismo instante.
auto System::quiesceRcp() -> void {
  // Un RSP aparcado espera un tramo que ya no va a llegar: aqui la CPU se para en seco.
  memory.dpLogFlush.store(true);
  memory.rspParkNudge();
  memory.dpLogApply(~0ull);
  memory.rdpDrain();
  memory.rspAwaitIdle();
  memory.dpLogApply(~0ull);
  memory.rdpDrain();
  memory.dpLogFlush.store(false);
  // Lockstep: la tarea la lleva ESTE hilo intercalada con la CPU, asi que aqui puede
  // quedar a medias. Se la deja acabar. El tope es el mismo presupuesto de seguridad que
  // usa el propio nucleo: si no para, ya estaba colgada antes de pedir el estado.
  for(int i = 0; i < 64 && memory.rsp.running; i++) memory.rsp.step(1u << 20);
}

// Reposo del invitado. Antes el estado se tomaba donde cayera la peticion y quiesceRcp
// terminaba a la fuerza lo que hubiera en vuelo. Con las citas de Threaded eso ya no vale: un
// RSP a mitad de tarea espera a que la CPU llegue a un instante de invitado, y la CPU esta
// parada esperandole a el -- medido, SM64 con KESTREL_REWIND=1 avanzaba un punado de ciclos
// de RSP cada dos segundos. Y en los dos modos, terminar la tarea o aplicar el diario antes
// de su instante cambia la partida: la foto no era de la maquina que seguia corriendo.
// Asi que no se fuerza nada: la CPU sigue corriendo subtramos hasta que el RCP esta quieto
// por si mismo, y ahi quiesceRcp solo drena trabajo de anfitrion (pixeles del RDP).
// Los fines de tarea armados y sin vencer (rcpPend bits 0-1) SI caben en un reposo: van en el
// estado con su plazo.
auto System::rcpAtRest() -> bool {
  const bool rspOn = memory.rcpMode == Memory::RcpMode::Threaded
                   ? memory.rspBusy.load(std::memory_order_acquire) : memory.rsp.running;
  if(rspOn) return false;
  if(memory.rcpPend.load(std::memory_order_acquire) & (4u | 8u | 16u)) return false;
  const u64 now = memory.cartNow();
  return memory.dpDrainedAt(now) &&
         memory.dpVisibleAt(now) == memory.dpSubSeq.load(std::memory_order_acquire);
}

// Tope de subtramos esperando reposo (16 por campo: ~10 s de invitado). Un juego que no
// suelta nunca el RSP no puede dejar la peticion colgada; pasado el tope se para a la fuerza
// como antes y se avisa, porque esa foto ya no es de la partida que sigue.
static constexpr u32 kRestWaitMax = 16 * 600;

auto System::serviceStateReq() -> void {
  if(!stateReqPending()) return;
  if(!rcpAtRest() && !cpu.halted && ++stateWaitSlices < kRestWaitMax) return;
  if(stateWaitSlices >= kRestWaitMax) {
    std::fprintf(stderr, "[state] sin reposo del RCP en %u subtramos: parada forzada\n", stateWaitSlices);
  }
  stateWaitSlices = 0;
  int save = stateSaveReq.exchange(-1, std::memory_order_acq_rel);
  int load = stateLoadReq.exchange(-1, std::memory_order_acq_rel);
  u32 rew  = rewindReq.exchange(0, std::memory_order_acq_rel);
  if(save < 0 && load < 0 && !rew) return;

  std::lock_guard<std::mutex> lk(coreMutex);
  quiesceRcp();

  std::string err, msg;
  if(save >= 0) {
    std::string path = stateSlotPath(*this, save);
    if(saveState(*this, path, err)) msg = "estado guardado en ranura " + std::to_string(save);
    else                            msg = "fallo al guardar ranura " + std::to_string(save) + ": " + err;
  }
  if(load >= 0) {
    std::string path = stateSlotPath(*this, load);
    if(loadState(*this, path, err)) {
      msg = "estado cargado de ranura " + std::to_string(load);
      // La cinta de rebobinado describe la partida que acaba de dejar de existir: volver
      // atras por ella llevaria a un pasado que ya no es el de esta maquina.
      rewinder.clear();
    } else {
      msg = "fallo al cargar ranura " + std::to_string(load) + ": " + err;
    }
  }
  // Rebobinado. Los pasos pedidos se gastan de uno en uno: cada uno mete en la maquina la
  // foto anterior, asi que pedir cinco es retroceder cinco fotos, no saltar a la quinta.
  if(rew) {
    u32 done = 0;
    for(; done < rew && rewinder.stepBack(*this, err); done++) {}
    if(done == rew) msg = "rebobinado " + std::to_string(done) + " (quedan "
                        + std::to_string(rewinder.steps()) + ")";
    else            msg = "rebobinado " + std::to_string(done) + ": " + err;
  }
  std::printf("[state] %s\n", msg.c_str());
  std::fflush(stdout);
  { std::lock_guard<std::mutex> ml(stateMsgMutex); stateMsg = msg; }
  stateSeq.fetch_add(1, std::memory_order_release);
}

auto System::run() -> void {
  // Ciclos de RSP: los publica el propio core en cada step(), asi que el contador vale igual
  // en Lockstep (interleave del bucle) que en Threaded (tarea entera en el worker).
  auto rspNow = [this]{ return memory.rsp.cyclesRun.load(std::memory_order_relaxed); };
  // Muestreador de SOLAPE (KESTREL_OCC=<microsegundos>, opt-in). La ocupacion suelta de
  // cada worker no distingue "RSP y RDP corren a la vez y el techo es la suma de trabajo"
  // de "se serializan y el techo es la suma de TIEMPOS". Esto lee las dos banderas de golpe
  // y cuenta los cuatro estados: si el estado 'ambos' es despreciable, se serializan.
  std::thread occTh;
  std::atomic<u64> occ[4] = {};
  if(const char* o = std::getenv("KESTREL_OCC")) {
    unsigned us = (unsigned)std::strtoul(o, nullptr, 0); if(!us) us = 200;
    occTh = std::thread([this, us, &occ]{
      while(!shutdown.load()) {
        unsigned st = (memory.rspBusy.load(std::memory_order_relaxed) ? 1u : 0u)
                    | (memory.rdpBusy.load(std::memory_order_relaxed) ? 2u : 0u);
        occ[st].fetch_add(1, std::memory_order_relaxed);
        std::this_thread::sleep_for(std::chrono::microseconds(us));
      }
      u64 t = 0; for(int i = 0; i < 4; i++) t += occ[i].load();
      if(!t) t = 1;
      std::fprintf(stderr, "[occ] muestras=%llu  ninguno=%.1f%%  soloRSP=%.1f%%  soloRDP=%.1f%%  ambos=%.1f%%\n",
                   (unsigned long long)t, 100.0*occ[0].load()/t, 100.0*occ[1].load()/t,
                   100.0*occ[2].load()/t, 100.0*occ[3].load()/t);
      std::fflush(stderr);
    });
  }

  // Canario de INTEGRIDAD DE CODIGO (KESTREL_CODEWATCH=<ms>, opt-in). Cuando el guest se
  // descarrila sin tomar ni una excepcion rara hay dos familias de causa, y desde fuera se
  // ven igual: (a) la CPU salto donde no debia -- error del emulador en el flujo de control
  // -- o (b) alguien que NO es la CPU (RDP, DMA del RSP o del PI) ha escrito encima del
  // codigo. Esto vigila el rango fisico donde vive el codigo del juego y avisa del PRIMER
  // bloque de 32 bytes que cambia, con el contexto de la CPU de ese instante. Rango por
  // defecto: el que ocupa Perfect Dark mapeado por TLB en 0x70000000 (fisico 0x8000..0x50000);
  // KESTREL_CODEWATCH_LO / _HI lo mueven.
  std::thread cwTh;
  if(const char* cw = std::getenv("KESTREL_CODEWATCH")) {
    unsigned ms = (unsigned)std::strtoul(cw, nullptr, 0); if(!ms) ms = 50;
    const char* eL = std::getenv("KESTREL_CODEWATCH_LO");
    const char* eH = std::getenv("KESTREL_CODEWATCH_HI");
    u32 lo = eL ? (u32)std::strtoul(eL, nullptr, 0) : 0x8000u;
    u32 hi = eH ? (u32)std::strtoul(eH, nullptr, 0) : 0x50000u;
    if((usize)hi > memory.rdram.size()) hi = (u32)memory.rdram.size();
    // El arranque descomprime el codigo de ROM a RDRAM: eso son miles de cambios
    // legitimos. La sombra se refresca en silencio hasta que el guest ha retirado
    // KESTREL_CODEWATCH_AFTER ops (default 30 M, ya en juego).
    const char* eA = std::getenv("KESTREL_CODEWATCH_AFTER");
    u64 after = eA ? std::strtoull(eA, nullptr, 0) : 30000000ull;
    if(lo < hi) cwTh = std::thread([this, ms, lo, hi, after]{
      std::vector<u8> shadow(memory.rdram.begin() + lo, memory.rdram.begin() + hi);
      u64 hits = 0;
      while(!shutdown.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(ms));
        const u8* cur = memory.rdram.data() + lo;
        for(u32 off = 0; off + 32 <= hi - lo; off += 32) {
          if(std::memcmp(shadow.data() + off, cur + off, 32) == 0) continue;
          if(hits < 64 && cpu.retired >= after) {
            hits++;
            std::fprintf(stderr, "[codewatch] #%llu phys=0x%08x retired=%llu pc=%08x\n",
                         (unsigned long long)hits, lo + off,
                         (unsigned long long)cpu.retired, (u32)cpu.pc);
            std::fprintf(stderr, "[codewatch]   antes:");
            for(u32 k = 0; k < 8; k++) { const u8* q = shadow.data() + off + 4 * k;
              std::fprintf(stderr, " %08x", ((u32)q[0]<<24)|((u32)q[1]<<16)|((u32)q[2]<<8)|q[3]); }
            std::fprintf(stderr, "\n[codewatch]   ahora:");
            for(u32 k = 0; k < 8; k++) { const u8* q = cur + off + 4 * k;
              std::fprintf(stderr, " %08x", ((u32)q[0]<<24)|((u32)q[1]<<16)|((u32)q[2]<<8)|q[3]); }
            std::fprintf(stderr, "\n");
            std::fflush(stderr);
          }
          std::memcpy(shadow.data() + off, cur + off, 32);
        }
      }
    });
  }

  // Watchdog opt-in (KESTREL_WATCHDOG=<segundos>): un livelock del guest y un hilo CPU
  // bloqueado en un handshake del RCP se ven IGUAL desde fuera. Esto los separa: si
  // retired avanza, gira el guest; si no avanza, la CPU esta parada esperando al RCP.
  std::thread wdog;
  if(const char* w = std::getenv("KESTREL_WATCHDOG")) {
    unsigned secs = (unsigned)std::strtoul(w, nullptr, 0); if(!secs) secs = 5;
#ifdef _WIN32
    // Si retired no avanza, el hilo CPU esta atascado DENTRO del emulador. Suspenderlo y
    // leer su RIP dice exactamente donde (se simboliza con scripts/hostprof.py).
    HANDLE cpuTh = nullptr;
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &cpuTh, 0, FALSE, DUPLICATE_SAME_ACCESS);
    u64 imgBase = (u64)GetModuleHandleW(nullptr);
#endif
    wdog = std::thread([this, secs
#ifdef _WIN32
                        , cpuTh, imgBase
#endif
                       ]{
      u64 last = 0;
      while(!shutdown.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(secs));
        u64 now = cpu.retired;
        std::fprintf(stderr, "[wdog] retired=%llu (+%llu) pc=%08x sp_status=%08x sp_pc=%03x rspRun=%u rspBusy=%u rspKick=%u rdpBusy=%u rdpQ=%u mi_intr=%02x mi_mask=%02x\n",
                     (unsigned long long)now, (unsigned long long)(now - last), (u32)cpu.pc,
                     memory.rcp.sp_status.load(), memory.rcp.sp_pc, (unsigned)memory.rsp.running,
                     (unsigned)memory.rspBusy.load(), (unsigned)memory.rspKick,
                     (unsigned)memory.rdpBusy.load(), (unsigned)memory.rdpQueue.size(),
                     memory.rcp.mi_intr.load(), memory.rcp.mi_mask);
        // Un guest que gira con retired subiendo puede estar (a) en un bucle de espera con
        // las interrupciones abiertas — entonces falta que alguien las levante — o (b) con
        // IE=0 / EXL=1, esperando algo que nunca llega DENTRO de un handler. Status/Cause/EPC
        // separan los dos casos, y las cuatro palabras alrededor de la PC dicen qué bucle es.
        std::fprintf(stderr, "[wdog] status=%08x cause=%08x epc=%016llx count=%08x compare=%08x\n",
                     (u32)cpu.cop0[12], (u32)cpu.cop0[13], (unsigned long long)cpu.cop0[14],
                     (u32)cpu.cop0[9], (u32)cpu.cop0[11]);
        // Un TLBL/TLBS o un bucle de excepcion solo se explica con la MMU delante: que VA
        // fallo, con que ASID, y que hay realmente en las 32 entradas. Wired/Random deciden
        // ademas si un TLBWR pudo pisar una entrada que el juego da por estatica.
        std::fprintf(stderr, "[wdog] badva=%016llx entryhi=%016llx index=%08x wired=%u random=%u\n",
                     (unsigned long long)cpu.cop0[CPU::C0_BadVAddr], (unsigned long long)cpu.cop0[CPU::C0_EntryHi],
                     (u32)cpu.cop0[CPU::C0_Index], (u32)cpu.cop0[CPU::C0_Wired], (u32)cpu.cop0[CPU::C0_Random]);
        for(int i = 0; i < 32; i++) {
          const auto& e = cpu.tlb[i];
          if(!e.lo0 && !e.lo1 && !e.hi) continue;
          std::fprintf(stderr, "[wdog]   tlb[%02d] hi=%016llx lo0=%016llx lo1=%016llx mask=%08llx g=%d\n",
                       i, (unsigned long long)e.hi, (unsigned long long)e.lo0,
                       (unsigned long long)e.lo1, (unsigned long long)e.mask, (int)e.global);
        }
        // Histograma de excepciones: separa de un vistazo "el guest toma interrupciones
        // normales" de "esta en una tormenta de TLBL/AdEL". Es contador puro, sin coste.
        {
          static const char* nm[16] = {"Int","Mod","TLBL","TLBS","AdEL","AdES","IBE","DBE",
                                       "Sys","Bp","RI","CpU","Ov","Tr","?","FPE"};
          std::fprintf(stderr, "[wdog] exc:");
          for(int i = 0; i < 16; i++)
            if(cpu.excCodeHist[i]) std::fprintf(stderr, " %s(%d)=%llu", nm[i], i,
                                                (unsigned long long)cpu.excCodeHist[i]);
          std::fprintf(stderr, "\n");
        }
        memory.miDump();
        {
          // La PC puede venir de un segmento TLB (Perfect Dark ejecuta desde 0x70000000):
          // enmascarar a 0x1fffffff da una direccion fuera de la RDRAM y el volcado sale a
          // ceros, que se lee como un colchon de NOPs inexistente. Traducir como la CPU.
          u64 tpc = cpu.tlbProbePhys(cpu.pc);
          u32 ph = (tpc == ~0ull) ? ((u32)cpu.pc & 0x1fff'ffffu) : (u32)tpc;
          std::fprintf(stderr, "[wdog] code @%08x:", (u32)cpu.pc - 8);
          for(int k = -2; k <= 2; k++) {
            u32 a = ph + 4 * k;
            u32 w = 0;
            if(a + 4 <= memory.rdram.size()) std::memcpy(&w, memory.rdram.data() + a, 4);
            std::fprintf(stderr, " %08x", __builtin_bswap32(w));
          }
          std::fprintf(stderr, "\n");
        }
        // Volcado de hilos del guest (libultra). Un cuelgue "todo el RCP parado y la CPU
        // girando en el hilo idle" solo se explica desde dentro del SO invitado: que hilos
        // hay, en que estado, y en que PC quedo congelado cada uno. OSThread se reconoce por
        // su forma: prioridad razonable, state en {1,2,4,8}, id pequeno y context.pc (+0x11c)
        // apuntando a codigo de RDRAM. No hay simbolos ni direcciones cableadas de ningun juego.
        if(std::getenv("KESTREL_GUESTTHREADS")) {
          // Coherente: una linea sucia de la D-cache tapa la RDRAM (ver guestQueueBroken).
          auto rd32 = [&](u32 a) -> u32 {
            u32 p = a & 0x1fff'ffffu;
            return ((u32)cpu.peekPhysCoherent(p) << 24) | ((u32)cpu.peekPhysCoherent(p + 1) << 16)
                 | ((u32)cpu.peekPhysCoherent(p + 2) << 8) | (u32)cpu.peekPhysCoherent(p + 3); };
          std::fprintf(stderr, "[wdog] hilos del guest:\n");
          int found = 0; u32 qs[8] = {}; int qn = 0;
          for(u32 a = 0; a + 0x140 <= (u32)memory.rdram.size() && found < 24; a += 4) {
            u32 pri = rd32(a + 0x04), idst = rd32(a + 0x10), id = rd32(a + 0x14);
            u32 state = idst >> 16, pc = rd32(a + 0x11c);
            if(state != 1 && state != 2 && state != 4 && state != 8) continue;
            if(pri > 255 || id > 64) continue;
            // El PC de un OSThread no tiene por que estar en KSEG0: Perfect Dark pagina su
            // codigo por TLB y sus hilos viven en 0x70000000 (lib) y 0x7f000000 (game). Un
            // filtro a KSEG0 descartaba TODOS los hilos reales y solo dejaba falsos positivos.
            bool pcOk = (pc & 3) == 0 && ((pc >= 0x8000'0000u && pc < 0x8080'0000u) ||
                                          (pc >= 0x7000'0000u && pc <  0x8000'0000u));
            if(!pcOk) continue;
            // Contexto libultra: base +0x20; ra=+0xE0, sp=+0xD0, sr=+0xF8, pc=+0xFC (u64 -> mitad baja).
            u32 q  = rd32(a + 0x08), sr = rd32(a + 0x118);
            u32 ra = rd32(a + 0x104), sp = rd32(a + 0x0f4), flags = idst & 0xffffu;
            // flags bit1 = OS_FLAG_FAULT: libultra para el hilo que toma una excepcion no
            // manejada y lo saca de la cola de ejecucion. Su cause/badvaddr quedan en el
            // contexto (ctx+0x100/+0x104) y son lo unico que explica el cuelgue posterior.
            u32 cause = rd32(a + 0x120), badv = rd32(a + 0x124);
            std::fprintf(stderr, "[wdog]   thread@%08x id=%u pri=%u state=%u flags=%04x queue=%08x pc=%08x ra=%08x sp=%08x sr=%08x cause=%08x badva=%08x\n",
                         0x8000'0000u + a, id, pri, state, flags, q, pc, ra, sp, sr, cause, badv);
            if(qn < 8) { bool dup = false;
              for(int k = 0; k < qn; k++) if(qs[k] == q) dup = true;
              if(!dup && q >= 0x8000'0000u && q < 0x8080'0000u) qs[qn++] = q; }
            found++;
          }
          // Las colas del SO (run queue y colas de mensajes) encadenan OSThread por su
          // campo next. Si el guest se queda en el hilo idle con hilos de mas prioridad
          // encolados, la cola es lo unico que lo explica: se vuelca tal cual.
          for(int k = 0; k < qn; k++) {
            u32 h = rd32(qs[k] - 0x8000'0000u);
            std::fprintf(stderr, "[wdog]   cola@%08x -> ", qs[k]);
            for(int n = 0; n < 8 && h >= 0x8000'0000u && h < 0x8080'0000u; n++) {
              u32 b = h - 0x8000'0000u;
              std::fprintf(stderr, "%08x(id=%u pri=%u st=%u) ", h, rd32(b + 0x14), rd32(b + 0x04), rd32(b + 0x10) >> 16);
              h = rd32(b + 0x00);
            }
            std::fprintf(stderr, "fin=%08x\n", h);
          }
        }
        if(std::getenv("KESTREL_PCRING")) cpu.pcRingDump(150);
        // KESTREL_WATCHP=<fis>: quien escribio ultimo esa linea de 16 bytes (via D-cache).
        if(cpu.wPhys) cpu.wDump(48);
        {  // KESTREL_EVDUMP=<n>: cuantos eventos escupe el watchdog (80 por defecto).
          const char* n = std::getenv("KESTREL_EVDUMP");
          memory.evDump(n ? (u32)std::strtoul(n, nullptr, 0) : 80);
        }
        std::fflush(stderr);
#ifdef _WIN32
        if(now == last && cpuTh) {
          CONTEXT ctx{}; ctx.ContextFlags = CONTEXT_CONTROL;
          if(SuspendThread(cpuTh) != (DWORD)-1) {
            if(GetThreadContext(cpuTh, &ctx))
              std::fprintf(stderr, "[wdog] CPU thread stuck at rip +0x%llx (rsp=%llx)\n",
                           (unsigned long long)((u64)ctx.Rip - imgBase), (unsigned long long)ctx.Rsp);
            ResumeThread(cpuTh);
          }
        }
#endif
        last = now;
      }
    });
  }
  hostprof::start();   // opt-in host sampler; samples THIS (CPU) thread

  // M1: run the CPU in short batches when not paused, yielding coreMutex between
  // batches so telemetry can inspect/step. Paused → idle; the telemetry thread
  // drives stepCpu() directly under the same lock.
  using clock = std::chrono::steady_clock;
  auto  winT0 = clock::now();       // sliding speed window (recomputed ~4x/sec)
  u64   winInsn = 0, winRsp = rspNow();
  u64   winRdpNs = 0, winRspNs = 0, winWaitNs = 0, winFlips = 0;   // bases de la ventana
  u64   winRamCpu = 0, winRamRcp = 0;                              // bases de bytes de RDRAM
  auto  hbT0 = winT0, hbLast = winT0;   // heartbeat lifetime baseline
  bool  hbOn = std::getenv("KESTREL_HEARTBEAT") != nullptr;

  // Limitador de velocidad, ver mas abajo. Armado cuando hay ventana; KESTREL_THROTTLE=0/1 manda.
  // El valor vive en rt::throttle porque el menu de la ventana lo cambia en caliente; -1 es
  // "automatico" y sigue significando lo de siempre: limitar solo si hay ventana.
  const double throttleFieldNs = 1e9 / clocks.viFieldHz;
  auto  throttleT0 = winT0;
  u64   throttleField = 0;


  // KESTREL_STABLE=<insns per check>[,<checks>] — stop once the picture stops moving.
  //
  // A fixed instruction cap is the wrong yardstick for a screenshot oracle: ROMs that
  // decode an image on the CPU (DCT, Huffman, I4/I8, GRB, Mandelbrot) are still halfway
  // down the frame when the cap fires, and the reference compare then scores a partially
  // drawn picture — 33% for a third of the image, which reads like an emulation bug and
  // is not one. Raising the cap for everyone costs the whole suite. So: run with a high
  // cap and stop when the VI framebuffer has been byte-identical for N consecutive
  // checks, having changed at least once (otherwise a ROM that has not drawn yet counts
  // as "stable" while still black).
  //
  // KESTREL_MAXFLIPS=<n> is the companion bound for ROMs that never settle at all —
  // video playback, the rotating-primitive demos. Those animate, so "stable" never
  // arrives and only the instruction cap stops them, on whatever frame it happens to
  // land. Counting displayed-buffer swaps instead pins them to an early, reproducible
  // frame, and it does not touch the CPU decoders: those render one picture into one
  // buffer and never flip.
  // KESTREL_MAXSYNCS=<n> is the same idea for animation that never flips a buffer (the
  // rotating-primitive demos redraw one framebuffer in place): an RDP SYNC_FULL is the
  // end of a display list, so N syncs is N completed frames. CPU-side decoders never
  // touch the RDP, so it does not bound them either.
  u64  stableEvery = 0, stableNeed = 3, stableNext = 0, stableHash = 0;
  u32  stableRun = 0, maxFlips = 0, maxSyncs = 0, maxFields = 0;
  bool stableSawChange = false;
  // KESTREL_HANGDOG=<segundos>: perro guardian de cuelgues.
  //
  // Un juego colgado no para: la CPU sigue retirando instrucciones a toda velocidad
  // (bucle cerrado, o PC fuera de la RDRAM leyendo ceros) mientras el RCP se queda a
  // cero y no vuelve a intercambiar buffer. Eso es indistinguible de "va lento" si solo
  // se mira el reloj de pared, y obliga a esperar al timeout entero para saberlo. Con el
  // guardian armado, N segundos sin un solo intercambio de buffer del VI, habiendo
  // intercambiado antes al menos uno, se tratan como cuelgue: se dispara el mismo
  // volcado que el tope de instrucciones (jumplog, historial de excepciones, hilos del
  // OS) y se termina. No toca la emulacion: solo observa viFlips.
  //
  // Apagado por defecto: hay ROMs que legitimamente no intercambian nunca (los
  // decodificadores de imagen por CPU dibujan un cuadro y ya), y esas se acotan con
  // KESTREL_STABLE / KESTREL_MAXSYNCS.
  double hangSecs = 0.0;
  if(const char* h = std::getenv("KESTREL_HANGDOG")) hangSecs = std::strtod(h, nullptr);
  auto  hangLast = winT0;             // ultimo momento en que se vio avanzar viFlips
  u64   hangFlips = 0;                // valor de viFlips en ese momento
  u64   hangInsn = 0;                 // instrucciones retiradas en ese momento

  if(const char* f = std::getenv("KESTREL_MAXFLIPS")) maxFlips = (u32)std::strtoul(f, nullptr, 0);
  if(const char* f = std::getenv("KESTREL_MAXSYNCS")) maxSyncs = (u32)std::strtoul(f, nullptr, 0);
  // Tope por CAMPOS de video. Los otros dos topes cuentan trabajo del RCP (intercambios de
  // buffer, syncs del RDP) y no sirven justo cuando mas falta hacen: un juego que arranca y
  // NO llega a dibujar nunca se para solo, asi que la telemetria de cierre ([frames], [cache],
  // [cpi], [muldiv], [fpu]) no llega a imprimirse y hay que matarlo por timeout a ciegas. El
  // campo de video corre pase lo que pase, asi que este tope siempre llega.
  if(const char* f = std::getenv("KESTREL_MAXFIELDS")) maxFields = (u32)std::strtoul(f, nullptr, 0);
  if(const char* s = std::getenv("KESTREL_STABLE")) {
    char* end = nullptr;
    stableEvery = std::strtoull(s, &end, 0);
    if(end && *end == ',') stableNeed = std::strtoull(end + 1, nullptr, 0);
    if(stableNeed == 0) stableNeed = 1;
    stableNext = stableEvery;
  }
  while(!shutdown.load()) {
    serviceStateReq();
    if(cpu.halted && exitOnHalt) { shutdown.store(true); break; }
    // Avance por fotogramas: con campos pendientes el bucle corre aunque la pausa este
    // puesta. Se lee aqui y se descuenta abajo, al cerrar el campo.
    const u32 fadv = stepFields.load(std::memory_order_relaxed);
    // Una peticion de estado en pausa hace correr la CPU hasta el reposo del RCP (ver
    // rcpAtRest); en cuanto se atiende, la pausa vuelve a mandar.
    if((paused.load() && !fadv && !stateReqPending()) || cpu.halted) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      winT0 = clock::now(); winInsn = 0; winRsp = rspNow();  // don't fold idle time into speed
      winRdpNs = memory.rdpBusyNs.load(std::memory_order_relaxed);
      winRspNs = memory.rspBusyNs.load(std::memory_order_relaxed);
      winWaitNs = memory.cpuWaitNs.load(std::memory_order_relaxed);
      winFlips = memory.rcp.viFlips;
      winRamCpu = cpu.ramCpuBytes; winRamRcp = memory.ramBytesRcp();
      continue;
    }
    u64 did;
    bool fieldClosed = false;
    {
      std::lock_guard<std::mutex> lk(coreMutex);
      // Un subtramo de campo de trabajo de CPU por vuelta, y despues el VI. La duracion
      // sale del modelo de reloj (clocks.tickInsns() = campo / viTicksPerField), no de una
      // constante suelta: el mismo campo lo usa la lectura de VI_V_CURRENT en Memory, para
      // que la interrupcion y el sondeo del contador de medias-lineas midan el MISMO tiempo.
      did = stepCpu(clocks.tickInsns());
      // Con el reloj de invitado, no con las ops retiradas a secas: si un campo trae muchos
      // fallos de cache, en HW ese campo hace MENOS trabajo de CPU, no el mismo en mas tiempo.
      fieldClosed = memory.viTick(cpu.guestOps());
      // Trucos: el motor del GameShark colgaba de la interrupcion del VI, asi que el ritmo
      // es el campo de video y no el fotograma del juego. Va aqui dentro, con el nucleo
      // parado bajo coreMutex, para que las escrituras no crucen con la CPU ni con el RCP.
      if(fieldClosed && cheats.enabled()) cheats.applyField(cpu);
      // Un campo cerrado gasta un paso del avance por fotogramas. El fetch_sub no puede
      // bajar de cero porque solo se resta lo que se vio distinto de cero en esta vuelta y
      // nadie mas resta: quien pide avance solo suma.
      if(fieldClosed && fadv) stepFields.fetch_sub(1, std::memory_order_relaxed);
      // Diagnostico de divergencia entre modos, opt-in. El md5 final solo dice "difieren";
      // estos dicen DONDE: KESTREL_FIELDHASH=1 imprime un FNV del estado CPU al cierre de
      // cada campo (primer campo distinto = ventana a bisecar) y KESTREL_FIELDDUMP=<n>
      // vuelca los registros enteros en ese campo. El getenv se lee una vez.
      static const u64  fieldDump = std::getenv("KESTREL_FIELDDUMP")
                                  ? std::strtoull(std::getenv("KESTREL_FIELDDUMP"), nullptr, 0) : 0;
      static const bool fieldHash = std::getenv("KESTREL_FIELDHASH") != nullptr;
      if((fieldDump || fieldHash) && fieldClosed) {
        static u64 nField = 0; nField++;
        if(nField == fieldDump) {
          for(int r = 0; r < 32; r++) std::fprintf(stderr, "[fd] gpr%02d=%016llx\n", r, (unsigned long long)cpu.gpr[r]);
          for(int r = 0; r < 32; r++) std::fprintf(stderr, "[fd] cop0_%02d=%016llx\n", r, (unsigned long long)cpu.cop0[r]);
          std::fprintf(stderr, "[fd] pc=%016llx next=%016llx retired=%llu\n", (unsigned long long)cpu.pc,
                       (unsigned long long)cpu.nextPc, (unsigned long long)cpu.retired);
        }
        if(fieldHash) {
          u64 h = 1469598103934665603ull;
          auto mix = [&](u64 v){ h ^= v; h *= 1099511628211ull; };
          for(int r = 0; r < 32; r++) mix((u64)cpu.gpr[r]);
          mix(cpu.pc); mix(cpu.nextPc);
          mix((u64)cpu.cop0[9]); mix((u64)cpu.cop0[11]); mix((u64)cpu.cop0[12]); mix((u64)cpu.cop0[13]);
          mix(cpu.retired);
          std::fprintf(stderr, "[fh] %llu %016llx\n", (unsigned long long)nField, (unsigned long long)h);
        }
        std::fflush(stderr);
      }
    }
    // Foto de rebobinado. Va FUERA del subtramo de arriba y con el RCP parado a proposito:
    // una foto con el RDP a medias de una lista no se puede volver a meter en la maquina.
    // Apagado, esto es una comparacion contra cero.
    // La foto se toma en el primer reposo del RCP a partir del cierre (ver rcpAtRest).
    if(fieldClosed && rewinder.enabled) rewindDue = true;
    if(rewindDue && (rcpAtRest() || ++rewindWaitSlices >= kRestWaitMax)) {
      if(rewindWaitSlices >= kRestWaitMax)
        std::fprintf(stderr, "[rewind] sin reposo del RCP en %u subtramos: parada forzada\n", rewindWaitSlices);
      rewindDue = false; rewindWaitSlices = 0;
      std::lock_guard<std::mutex> lk(coreMutex);
      quiesceRcp();
      rewinder.onField(*this);
    }

    // Limitador de velocidad. Sin el, el emulador corre a lo que de el host: a 157% de
    // consola el audio sale acelerado y el juego responde a destiempo, o sea sirve para medir
    // pero no para jugar. El reloj es el campo de video (Clocks::viFieldHz), el mismo que ancla
    // todo el tiempo del guest, y el vencimiento es ABSOLUTO (campo n-esimo desde el ancla) para
    // que los errores de un campo no se acumulen.
    //
    // Apagado cuando no hay ventana: gates, bench y krom corren headless y deben ir a tope, o
    // medirian 30 fps siempre. KESTREL_THROTTLE=0/1 fuerza cualquiera de los dos.
    const int thWant = rt::throttle.load(std::memory_order_relaxed);
    const bool throttleOn = thWant < 0 ? videoOn : thWant != 0;
    if(throttleOn && fieldClosed) {
      throttleField++;
      auto due = throttleT0 + std::chrono::nanoseconds(
                   (u64)((double)throttleField * throttleFieldNs));
      auto now = clock::now();
      // Reancla si vamos MUY atrasados (pausa, carga de estado, un campo carisimo): recuperar
      // el tiempo perdido corriendo al doble se ve peor que perderlo y seguir a ritmo.
      if(now - due > std::chrono::milliseconds(250)) { throttleT0 = now; throttleField = 0; }
      else if(now < due) sleepUntilPrecise(due);
    }
    retiredInsns.fetch_add(did, std::memory_order_relaxed);
    winInsn += did;

    // Nivel 1 (por defecto): SOLO columnas que el invitado puede ver. Es el rastro que se
    // compara por md5 entre corridas, asi que no puede llevar nada medido en tiempo de
    // anfitrion. Nivel 2 anade `rsp=` (ciclos del RSP) y `gclk=` (reloj del RDP), que son el
    // avance REAL de los dos workers: muestreados en el limite de campo valen lo que valgan
    // segun donde estuviera cada hilo en ese instante, y difieren entre corridas sin que el
    // invitado vea nada distinto. Utiles para mirar a ojo, veneno para un diff.
    static const int fieldTrace = [] {
      const char* e = std::getenv("KESTREL_FIELDTRACE");
      if(!e || !*e || !std::strcmp(e, "0")) return 0;
      return std::atoi(e) >= 2 ? 2 : 1;
    }();
    if(fieldTrace && fieldClosed) {
      // Una sola escritura. Con fprintf directo la linea sale a trozos y otro hilo que
      // imprima a la vez la parte por la mitad: el rastro se compara por md5 entre corridas
      // y una linea partida se lee como una divergencia que no existe.
      char ln[192];
      char host[64] = "";
      if(fieldTrace >= 2)
        std::snprintf(host, sizeof(host), "rsp=%llu gclk=%llu ",
                      (unsigned long long)memory.rsp.cyclesRun.load(std::memory_order_relaxed),
                      (unsigned long long)memory.rcp.rdpGclk.load(std::memory_order_relaxed));
      int n = std::snprintf(ln, sizeof(ln),
                   "[ft] f=%u ret=%llu ops=%llu %s"
                   "sp=%u dp=%u flips=%u syncs=%u org=%06x mi=%02x\n",
                   memory.rcp.viFields, (unsigned long long)cpu.retired,
                   (unsigned long long)cpu.guestOps(), host,
                   memory.spRets.load(), memory.dpRets.load(),
                   memory.rcp.viFlips, memory.rcp.dpSyncs, memory.rcp.vi_origin,
                   (unsigned)(memory.rcp.mi_intr.load(std::memory_order_relaxed) & 0xff));
      if(n > 0) std::fwrite(ln, 1, (usize)n < sizeof(ln) ? (usize)n : sizeof(ln) - 1, stderr);
    }

    if((maxFlips && memory.rcp.viFlips >= maxFlips) ||
       (maxSyncs && memory.rcp.dpSyncs >= maxSyncs) ||
       (maxFields && memory.rcp.viFields >= maxFields)) {
      // viFields = campos de video emitidos = tiempo del guest (a viFieldHz). viFlips solo
      // cuenta intercambios de buffer, y un juego que no llega a 60 fps intercambia menos
      // veces que campos hay: medir "tiempo real" con los swaps mide de menos.
      // padPolls = comandos 0x01 del joybus en el conector 1: las veces que el JUEGO ha
      // leido el mando. Va aqui porque es la unica cuenta de "tiempo del mando" que el invitado
      // percibe, y sin ella no se puede comparar su cadencia contra los campos de video.
      std::fprintf(stderr, "[frames] %u buffer swaps, %u VI fields, %u RDP syncs, %u lecturas de mando "
                   "after %lluM insns, origin=%06x, stopping\n",
                   memory.rcp.viFlips, memory.rcp.viFields, memory.rcp.dpSyncs,
                   (unsigned)memory.padPolls.load(std::memory_order_relaxed),
                   (unsigned long long)(cpu.retired / 1'000'000),
                   memory.rcp.vi_origin);
      // Citas del sondeo de SP_STATUS (Memory::spReadSync) y BREAK relanzados por un CLEAR_HALT
      // aplazado (Memory::spLateClearHalt). Con el grano por defecto las citas son del orden de
      // una por borde cruzado; si suben al millon es que el grano se ha perdido.
      std::fprintf(stderr, "[sprdv] %u citas, %u renuncias, %u relanzados\n",
                   memory.spRdv.load(), memory.spRdvWaives.load(), memory.spLateHalts.load());
      std::fprintf(stderr, "[dplog] %llu apuntadas, %llu esperas, %u renuncias\n",
                   (unsigned long long)memory.dpLogPushes.load(), (unsigned long long)memory.dpLogWaits.load(), memory.dpLogWaives.load());
      // Fallos de cache primaria del tramo. Es la materia prima del CPI real: el VR4300 no
      // gasta un numero fijo de ciclos por instruccion, gasta uno mas la penalizacion de
      // RDRAM de cada fallo. Sin esta cuenta el CPI solo se puede suponer.
      std::fprintf(stderr, "[cache] %llu fallos D$ (%.3f%%), %llu fallos I$ (%.3f%%)\n",
                   (unsigned long long)cpu.dcMisses,
                   cpu.retired ? 100.0 * (double)cpu.dcMisses / (double)cpu.retired : 0.0,
                   (unsigned long long)cpu.icMisses,
                   cpu.retired ? 100.0 * (double)cpu.icMisses / (double)cpu.retired : 0.0);
      // Y el CPI que sale de ahi. cpiBase = el factor plano configurado (KESTREL_CPI);
      // cpiReal = cpiBase + ciclos de parada por instruccion. Con KESTREL_CACHECOST=0 los dos
      // coinciden y la linea dice justo eso: el modelo es plano. Con el coste encendido, la
      // diferencia es lo que la cache le cuesta AL JUEGO, medido, no supuesto.
      {
        double cpiBase = (double)cpu.cpi256 / 128.0;
        double cpiReal = cpiBase + (cpu.retired ? (double)cpu.stallTotal / (double)cpu.retired : 0.0);
        std::fprintf(stderr, "[cpi] base %.3f  real %.3f  (fallo %u ciclos, no-cacheado %u ciclos"
                     " x %llu lecturas = %.3f%%; %llu ciclos parados = %llu ops equivalentes)\n",
                     cpiBase, cpiReal, cpu.missCycles, cpu.uncachedCycles,
                     (unsigned long long)cpu.uncachedReads,
                     cpu.retired ? 100.0 * (double)cpu.uncachedReads / (double)cpu.retired : 0.0,
                     (unsigned long long)cpu.stallTotal, (unsigned long long)cpu.stallOps);
        // Reparto del bus de RDRAM. La consola tiene UN bus de 562,5 MB/s que arbitra el RCP
        // entre los siete maestros, y en muchos juegos el palo largo es el bus y no ningun
        // chip. Los MB/s van por segundo de tiempo de INVITADO (retiradas / ritmo a tiempo
        // real), asi que el numero es del juego y no del anfitrion. Lo que NO se cuenta esta
        // en Memory::kRdramPeakBps: es un suelo, no una cota.
        {
          double gs = clocks.insnTarget() > 0.0 ? cpu.retired / clocks.insnTarget() : 0.0;
          if(gs > 1e-6) {
            auto mb = [&](u64 b) { return (double)b / 1e6 / gs; };
            double tot = mb(cpu.ramCpuBytes + memory.ramBytesRcp());
            std::fprintf(stderr, "[rdram] %.1f%% del bus (%.1f MB/s de invitado en %.2f s):"
                         " cpu %.1f rdp %.1f vi %.1f rsp %.1f pi %.1f ai %.1f si %.1f%s",
                         tot * 1e6 / Memory::kRdramPeakBps * 100.0, tot, gs,
                         mb(cpu.ramCpuBytes),
                         mb(memory.ramBytesRdp.load(std::memory_order_relaxed)),
                         mb(memory.ramBytesVi .load(std::memory_order_relaxed)),
                         mb(memory.ramBytesRsp.load(std::memory_order_relaxed)),
                         mb(memory.ramBytesPi .load(std::memory_order_relaxed)),
                         mb(memory.ramBytesAi .load(std::memory_order_relaxed)),
                         mb(memory.ramBytesSi .load(std::memory_order_relaxed)), "\n");
          }
        }
        // Reparto del TIEMPO DE PARED del hilo de CPU y de los workers. Es la vista que dice
        // si el emulador va lento porque emular cuesta o porque alguien esta dormido esperando
        // a otro: sin ella "CPU 14%" no distingue las dos cosas. El aparcamiento del RSP se
        // saca aparte porque NO es ocupacion (ver rspParkNs).
        {
          double ws = std::chrono::duration<double>(std::chrono::steady_clock::now() - hbT0).count();
          auto pc = [&](u64 ns) { return ws > 0.0 ? ns / 1e9 / ws * 100.0 : 0.0; };
          memory.sampleWorkerCpu();
          std::fprintf(stderr, "[block] pared %.2f s | cpuWait %.1f%% (freno %.1f%% barSP %.1f%%"
                       " barDP %.1f%%) | rsp ocupado %.1f%% aparcado %.1f%% | rdp ocupado %.1f%%"
                       " | CPU real: cpu %.1f%% rsp %.1f%% rdp %.1f%%\n",
                       ws, pc(memory.cpuWaitNs.load(std::memory_order_relaxed)),
                       pc(memory.paceBlockNs.load(std::memory_order_relaxed)),
                       pc(memory.spBarBlockNs.load(std::memory_order_relaxed)),
                       pc(memory.dpBarBlockNs.load(std::memory_order_relaxed)),
                       pc(memory.rspBusyNs.load(std::memory_order_relaxed)),
                       pc(memory.rspParkNs.load(std::memory_order_relaxed)),
                       pc(memory.rdpBusyNs.load(std::memory_order_relaxed)),
                       pc(memory.cpuCpuNs.load(std::memory_order_relaxed)),
                       pc(memory.rspCpuNs.load(std::memory_order_relaxed)),
                       pc(memory.rdpCpuNs.load(std::memory_order_relaxed)));
        }
        if(cpu.mulDivMode)
          std::fprintf(stderr, "[muldiv] %llu mult/div enteras (%.3f%% de las retiradas),"
                       " %llu ciclos parados = %.3f de CPI\n",
                       (unsigned long long)cpu.mulDivOps,
                       cpu.retired ? 100.0 * (double)cpu.mulDivOps / (double)cpu.retired : 0.0,
                       (unsigned long long)cpu.mulDivStall,
                       cpu.retired ? (double)cpu.mulDivStall / (double)cpu.retired : 0.0);
        if(cpu.fpuMode)
          std::fprintf(stderr, "[fpu] %llu ops de coma flotante (%.3f%% de las retiradas),"
                       " %llu ciclos parados = %.3f de CPI\n",
                       (unsigned long long)cpu.fpuOps,
                       cpu.retired ? 100.0 * (double)cpu.fpuOps / (double)cpu.retired : 0.0,
                       (unsigned long long)cpu.fpuStall,
                       cpu.retired ? (double)cpu.fpuStall / (double)cpu.retired : 0.0);
      }
      std::lock_guard<std::mutex> lk(coreMutex);
      cpu.maxInsn = cpu.retired + 1;
      // El tope de instrucciones se comprueba en la guarda de depuracion del interprete, y
      // esa guarda se calcula UNA vez al parsear el entorno. Ponerlo aqui sin re-armarla
      // dejaba el corte inerte: el emulador seguia corriendo y reimprimiendo este mensaje
      // campo tras campo. Solo no se notaba porque el gate pasa ademas KESTREL_MAXINSN,
      // que ya la arma.
      cpu.refreshDebugArmed();
      maxFlips = maxSyncs = maxFields = 0;    // ya disparado: no repetir el aviso cada campo
    }

    if(hangSecs > 0.0) {
      u64 fl = memory.rcp.viFlips;
      auto tnow = clock::now();
      if(fl != hangFlips) { hangFlips = fl; hangLast = tnow; hangInsn = cpu.retired; }
      else if(hangFlips > 0 &&
              std::chrono::duration<double>(tnow - hangLast).count() >= hangSecs) {
        std::fprintf(stderr,
                     "[hangdog] %.1fs sin intercambiar buffer tras %llu campos, y la CPU ha"
                     " retirado %llu instrucciones en ese hueco: cuelgue.\n",
                     hangSecs, (unsigned long long)hangFlips,
                     (unsigned long long)(cpu.retired - hangInsn));
        std::lock_guard<std::mutex> lk(coreMutex);
        cpu.maxInsn = cpu.retired + 1;   // reutiliza el volcado del tope de instrucciones
        cpu.refreshDebugArmed();
        hangSecs = 0.0;                  // un solo aviso
      }
    }

    if(stableEvery && cpu.retired >= stableNext) {
      stableNext = cpu.retired + stableEvery;
      u64 h;
      { std::lock_guard<std::mutex> lk(coreMutex); h = framebufferHash(memory); }
      if(h != stableHash) { stableHash = h; stableRun = 0; stableSawChange = true; }
      else if(stableSawChange && ++stableRun >= stableNeed) {
        std::fprintf(stderr, "[stable] framebuffer unchanged for %llu insns after %lluM, stopping\n",
                     (unsigned long long)(stableEvery * stableNeed),
                     (unsigned long long)(cpu.retired / 1'000'000));
        // Reuse the cap path so the dump/report side stays in exactly one place.
        std::lock_guard<std::mutex> lk(coreMutex);
        cpu.maxInsn = cpu.retired + 1;
        cpu.refreshDebugArmed();   // idem: sin re-armar la guarda el tope no lo mira nadie
        stableEvery = 0;
      }
    }

    // Recompute the realtime-% meter over a short window. Emulated cycles/sec vs
    // each domain's (overclock-scaled) target clock. 100% == real N64 speed.
    auto now = clock::now();
    double ws = std::chrono::duration<double>(now - winT0).count();
    if(ws >= 0.25) {
      double cpuCps = winInsn / ws;                          // instrucciones retiradas/s
      double rspCps = (rspNow() - winRsp) / ws;
      // 100 % = tiempo real. El denominador es el ritmo de RETIRADA que equivale a tiempo
      // real (ciclos / CPI), no el reloj de ciclos: comparar instrucciones contra 93.75 MHz
      // daba la mitad del numero real bajo el modelo de Count de este interprete.
      n64SpeedPct.store(cpuCps / clocks.insnTarget() * 100.0, std::memory_order_relaxed);
      rspSpeedPct.store(rspCps / clocks.rspTarget() * 100.0, std::memory_order_relaxed);
      // Ocupacion del bus de RDRAM. El divisor NO es el tiempo de pared sino el tiempo de
      // INVITADO transcurrido en la ventana (instrucciones retiradas / ritmo de retirada a
      // tiempo real): el bus de la consola es de 562,5 MB/s pase lo que pase, asi que el
      // porcentaje tiene que salir igual corra el emulador al 20% o al 300%. Con la ventana
      // parada (juego en pausa, cero instrucciones) se deja el valor anterior en vez de
      // dividir por cero. Que maestros entran y cuales no: Memory::kRdramPeakBps.
      u64 ramCpu = cpu.ramCpuBytes, ramRcp = memory.ramBytesRcp();
      double guestS = clocks.insnTarget() > 0.0 ? winInsn / clocks.insnTarget() : 0.0;
      if(guestS > 1e-6)
        rdramSpeedPct.store((double)((ramCpu - winRamCpu) + (ramRcp - winRamRcp))
                            / guestS / Memory::kRdramPeakBps * 100.0, std::memory_order_relaxed);
      winRamCpu = ramCpu; winRamRcp = ramRcp;
      // Ocupacion en la misma ventana: nanosegundos de pared que cada worker paso DENTRO
      // de un trabajo, y cuantos intercambios de buffer hubo (= fps de verdad del juego).
      u64 rdpNs = memory.rdpBusyNs.load(std::memory_order_relaxed);
      u64 rspNs = memory.rspBusyNs.load(std::memory_order_relaxed);
      u64 witNs = memory.cpuWaitNs.load(std::memory_order_relaxed);
      u64 flips = memory.rcp.viFlips;
      rdpBusyPct.store((rdpNs - winRdpNs) / 1e9 / ws * 100.0, std::memory_order_relaxed);
      rspBusyPct.store((rspNs - winRspNs) / 1e9 / ws * 100.0, std::memory_order_relaxed);
      cpuWaitPct.store((witNs - winWaitNs) / 1e9 / ws * 100.0, std::memory_order_relaxed);
      fieldsPerSec.store((flips - winFlips) / ws, std::memory_order_relaxed);
      winRdpNs = rdpNs; winRspNs = rspNs; winWaitNs = witNs; winFlips = flips;
      winT0 = now; winInsn = 0; winRsp = rspNow();
    }

    if(hbOn && now - hbLast >= std::chrono::seconds(5)) {
      double s  = std::chrono::duration<double>(now - hbT0).count();
      u64    ri = retiredInsns.load(std::memory_order_relaxed);
      // Worker occupancy as a share of wall time: which domain is the long pole.
      // >100% per worker is impossible, and cpuWait is how long the CPU thread sat
      // blocked on one of them — together they localize the bottleneck.
      double rdpPct  = memory.rdpBusyNs.load(std::memory_order_relaxed) / 1e9 / s * 100.0;
      double rspPct  = memory.rspBusyNs.load(std::memory_order_relaxed) / 1e9 / s * 100.0;
      double waitPct = memory.cpuWaitNs.load(std::memory_order_relaxed) / 1e9 / s * 100.0;
      // Rendimiento del emulador de RSP: instrucciones de microcodigo por segundo DENTRO de
      // step(), sin contar el tiempo ocioso del worker. Separa "lo emulamos despacio" de
      // "el juego no le da trabajo al RSP".
      double rspBusyS = memory.rspBusyNs.load(std::memory_order_relaxed) / 1e9;
      double rspMips  = rspBusyS > 0.01 ? memory.rsp.cyclesRun.load(std::memory_order_relaxed)
                                          / 1e6 / rspBusyS : 0.0;
      // Regulador: episodios (tareas de RSP vistas), frenadas y % de pared durmiendo en el
      // freno. Si el freno esta puesto y aun asi la CPU va muy por delante del RSP, es que
      // se esta soltando (salvavidas) y hay que mirarlo.
      double pacePct = memory.paceBlockNs.load(std::memory_order_relaxed) / 1e9 / s * 100.0;
      // CPU de verdad gastada por cada worker frente a su ocupacion de pared. Si "cpu" es
      // muy inferior a la ocupacion, el worker no va lento: esta esperando nucleo (SMT o
      // planificador), y optimizar su codigo no va a mover el fps.
      memory.sampleWorkerCpu();
      double rspCpuPct = memory.rspCpuNs.load(std::memory_order_relaxed) / 1e9 / s * 100.0;
      double rdpCpuPct = memory.rdpCpuNs.load(std::memory_order_relaxed) / 1e9 / s * 100.0;
      std::fprintf(stderr, "[hb] %.0fM insns, %.2f Mips avg | N64 speed: CPU %.1f%%  RSP %.1f%%"
                           " | occupancy: rdp %.0f%%(cpu %.0f%%) rsp %.0f%%(cpu %.0f%%) cpuWait %.0f%%"
                           " | rsp %.1f Mips busy | pace %.0f%% ep=%llu hold=%llu\n",
                   ri / 1e6, ri / 1e6 / s,
                   n64SpeedPct.load(std::memory_order_relaxed),
                   rspSpeedPct.load(std::memory_order_relaxed),
                   rdpPct, rdpCpuPct, rspPct, rspCpuPct, waitPct, rspMips, pacePct,
                   (unsigned long long)memory.paceEpisodes.load(std::memory_order_relaxed),
                   (unsigned long long)memory.paceHolds.load(std::memory_order_relaxed));
      // Tasa de trabajos del RCP. Cada tarea de RSP y cada trabajo de RDP cuesta un mutex
      // + notify_all (llamada al kernel cuando hay esperador): si la tasa es de decenas de
      // miles por segundo, ese trafico de sincronizacion deja de ser ruido y pasa a ser el
      // coste dominante del hilo. Sin este numero no se puede distinguir de "emular cuesta".
      std::fprintf(stderr, "[hb] jobs/s: rsp=%.0f rdp=%.0f\n",
                   memory.rspJobsRun.load(std::memory_order_relaxed) / s,
                   memory.rdpJobsRun.load(std::memory_order_relaxed) / s);
      // Reparto del bus de RDRAM entre los siete maestros, en MB por segundo de INVITADO y
      // como porcentaje del pico de la consola. Es la unica vista que dice si el palo largo
      // de un juego es un chip o el bus: el RDP pinta a 562,5 MB/s como mucho, y con el VI
      // releyendo el framebuffer entero cada campo el techo real baja. Ver kRdramPeakBps
      // para lo que NO se cuenta (refresco, noveno bit, camino rapido del dynarec).
      {
        double gs = clocks.insnTarget() > 0.0 ? ri / clocks.insnTarget() : 0.0;
        if(gs > 1e-6) {
          auto mb = [&](u64 b) { return (double)b / 1e6 / gs; };
          double tot = mb(cpu.ramCpuBytes + memory.ramBytesRcp());
          std::fprintf(stderr, "[hb] rdram %.0f%% (%.1f MB/s invitado): cpu %.1f rdp %.1f vi %.1f"
                               " rsp %.1f pi %.1f ai %.1f si %.1f\n",
                       tot * 1e6 / Memory::kRdramPeakBps * 100.0, tot,
                       mb(cpu.ramCpuBytes),
                       mb(memory.ramBytesRdp.load(std::memory_order_relaxed)),
                       mb(memory.ramBytesVi .load(std::memory_order_relaxed)),
                       mb(memory.ramBytesRsp.load(std::memory_order_relaxed)),
                       mb(memory.ramBytesPi .load(std::memory_order_relaxed)),
                       mb(memory.ramBytesAi .load(std::memory_order_relaxed)),
                       mb(memory.ramBytesSi .load(std::memory_order_relaxed)));
        }
      }
      // Hambre del sumidero de audio EN VIVO. `KESTREL_AUDIOSTAT` solo habla al cerrar, y
      // con ventana el emulador no cierra solo: sin esta linea un "se oye entrecortado" no
      // se puede localizar mientras pasa. `min` es el colchon minimo DE ESTA VENTANA de 5 s,
      // que es lo que delata un corte; el acumulado de silencio dice si ya se oyo.
      {
        u64 apull = 0, asil = 0, adrop = 0; u32 alvl = 0, alow = 0, acap = 0;
        if(audio::statSnapshot(apull, asil, adrop, alvl, alow, acap)) {
          double sil = (apull + asil) ? 100.0 * (double)asil / (double)(apull + asil) : 0.0;
          std::fprintf(stderr, "[hb] audio: silencio %.2f%% descartadas=%llu anillo %u"
                               " (min %u de esta ventana) de %u\n",
                       sil, (unsigned long long)adrop, alvl, alow, acap);
        }
      }
      hbLast = now;
    }
  }
  hostprof::stop();
  movie::finish();
  shutdown.store(true);
  if(wdog.joinable()) { shutdown.store(true); wdog.join(); }
  if(occTh.joinable()) occTh.join();
  if(cwTh.joinable()) { shutdown.store(true); cwTh.join(); }
}

}  // namespace kestrel
