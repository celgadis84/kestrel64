#include "system.hpp"
#ifdef _WIN32
#include <windows.h>
#endif
#include "../telemetry/server.hpp"
#include "../telemetry/hostprof.hpp"
#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace kestrel {

System::System() = default;

System::~System() {
  requestShutdown();
  memory.flushSaveFile();   // persist battery/flash save to disk on exit
  memory.stopRcpThreads();
  if(tele) tele->stop();
  if(teleThread.joinable()) teleThread.join();
}

auto System::init(const std::string& romPath, std::string& error) -> bool {
  memory.reset(/*expansionPak=*/true);
  if(!rom.loadFile(romPath, error)) return false;
  memory.loadRom(rom.data);  // exposes CART_ROM region for telemetry
  memory.attachSaveFile(romPath);  // load existing .eep/.sra/.fla, if any, next to the ROM
  std::printf("[system] loaded \"%s\" (%s, %.2f MB, entry 0x%08x)\n",
              rom.header.name.c_str(),
              rom.originalOrder == Rom::Order::Z64 ? "z64" :
              rom.originalOrder == Rom::Order::N64 ? "n64" :
              rom.originalOrder == Rom::Order::V64 ? "v64" : "?",
              rom.data.size() / (1024.0 * 1024.0), rom.header.entryPoint);
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
  {
    auto mul = [](const char* n, double& dst) {
      if(const char* v = std::getenv(n)) {
        double d = std::strtod(v, nullptr);
        if(d > 0.0) dst = d;           // 0 o basura = no tocar
      }
    };
    double all = 0.0;
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
  // Un solo reloj de video para todo el emulador: el mismo numero de instrucciones
  // por campo que usa el bucle de System (stepCpu) lo usa la lectura de VI_V_CURRENT.
  memory.viFieldInsns = clocks.fieldInsns();
  // ...y un solo modelo de CPI: el mismo ratio CPU:RSP para el interleave de Lockstep
  // (aqui) y para el regulador de Threaded (Memory::rcpPace).
  rspStepNum = (u64)(clocks.rspInsnsPerCpuInsn() * 65536.0 + 0.5);
  rspStepDen = 65536;
  memory.paceCpuNum = rspStepDen; memory.paceCpuDen = rspStepNum;   // inverso: CPU por RSP
  cpu.fastBoot(rom.header.entryPoint);  // HLE IPL3: boot segment in RDRAM, PC at entry
  std::printf("[cpu] HLE boot, pc=0x%08x\n", (u32)cpu.pc);
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
      if(k) { i += k; if(paced) memory.rcpPace(cpu.retired); continue; }
    }
    cpu.step();
    i++;
    // Regulador Threaded: el equivalente al interleave 2:3 de abajo. Cada 64 ops basta —
    // es una lectura atomica relajada y el margen del regulador es de miles de ops.
    if(paced && (i & 0x3F) == 0) memory.rcpPace(cpu.retired);
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
    // Lockstep: interleave the RSP at Clocks::rspInsnsPerCpuInsn() (4/3 stock).
    // Threaded: the RSP runs to completion on its own worker (see rspWorkerLoop),
    // so the CPU thread must NOT also step it — that would double-execute the core.
    if(memory.rcpMode == Memory::RcpMode::Lockstep && memory.rsp.running) {
      rspPhase += rspStepNum;
      while(rspPhase >= rspStepDen) {
        rspPhase -= rspStepDen; memory.rsp.step(1); if(!memory.rsp.running) break;
      }
    }
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
  if(std::getenv("KESTREL_VIDEO"))   want = true;
  if(std::getenv("KESTREL_NOVIDEO")) want = false;
  if(!want) return;
  videoOn = presenter.start(&memory, &shutdown, &n64SpeedPct, &rspSpeedPct, &rdramSpeedPct,
                            rom.valid() ? rom.header.name.c_str() : nullptr);
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
  const char* pe = std::getenv("KESTREL_PRDP");
  bool prdpWanted = pe && pe[0] != '0';
  bool prdpReady  = memory.vrdpWaitReady(15000);
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
        memory.miDump();
        {
          u32 ph = (u32)cpu.pc & 0x1fff'ffff;
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
            if((pc & 3) || pc < 0x8000'0000u || pc >= 0x8080'0000u) continue;
            u32 q = rd32(a + 0x08), sr = rd32(a + 0x118), ra = rd32(a + 0x110);
            std::fprintf(stderr, "[wdog]   thread@%08x id=%u pri=%u state=%u queue=%08x pc=%08x ra=%08x sr=%08x\n",
                         0x8000'0000u + a, id, pri, state, q, pc, ra, sr);
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
  auto  hbT0 = winT0, hbLast = winT0;   // heartbeat lifetime baseline
  bool  hbOn = std::getenv("KESTREL_HEARTBEAT") != nullptr;

  // Limitador de velocidad, ver mas abajo. Armado cuando hay ventana; KESTREL_THROTTLE=0/1 manda.
  const char* thEnv = std::getenv("KESTREL_THROTTLE");
  bool  throttleOn = thEnv ? (thEnv[0] != '0') : videoOn;
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
  u32  stableRun = 0, maxFlips = 0, maxSyncs = 0;
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
  if(const char* s = std::getenv("KESTREL_STABLE")) {
    char* end = nullptr;
    stableEvery = std::strtoull(s, &end, 0);
    if(end && *end == ',') stableNeed = std::strtoull(end + 1, nullptr, 0);
    if(stableNeed == 0) stableNeed = 1;
    stableNext = stableEvery;
  }
  while(!shutdown.load()) {
    if(cpu.halted && exitOnHalt) { shutdown.store(true); break; }
    if(paused.load() || cpu.halted) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
      winT0 = clock::now(); winInsn = 0; winRsp = rspNow();  // don't fold idle time into speed
      winRdpNs = memory.rdpBusyNs.load(std::memory_order_relaxed);
      winRspNs = memory.rspBusyNs.load(std::memory_order_relaxed);
      winWaitNs = memory.cpuWaitNs.load(std::memory_order_relaxed);
      winFlips = memory.rcp.viFlips;
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
      fieldClosed = memory.viTick(cpu.retired);
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
    // Limitador de velocidad. Sin el, el emulador corre a lo que de el host: a 157% de
    // consola el audio sale acelerado y el juego responde a destiempo, o sea sirve para medir
    // pero no para jugar. El reloj es el campo de video (Clocks::viFieldHz), el mismo que ancla
    // todo el tiempo del guest, y el vencimiento es ABSOLUTO (campo n-esimo desde el ancla) para
    // que los errores de un campo no se acumulen.
    //
    // Apagado cuando no hay ventana: gates, bench y krom corren headless y deben ir a tope, o
    // medirian 30 fps siempre. KESTREL_THROTTLE=0/1 fuerza cualquiera de los dos.
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

    if((maxFlips && memory.rcp.viFlips >= maxFlips) ||
       (maxSyncs && memory.rcp.dpSyncs >= maxSyncs)) {
      // viFields = campos de video emitidos = tiempo del guest (a viFieldHz). viFlips solo
      // cuenta intercambios de buffer, y un juego que no llega a 60 fps intercambia menos
      // veces que campos hay: medir "tiempo real" con los swaps mide de menos.
      std::fprintf(stderr, "[frames] %u buffer swaps, %u VI fields, %u RDP syncs after %lluM insns, "
                   "origin=%06x, stopping\n",
                   memory.rcp.viFlips, memory.rcp.viFields, memory.rcp.dpSyncs,
                   (unsigned long long)(cpu.retired / 1'000'000),
                   memory.rcp.vi_origin);
      std::lock_guard<std::mutex> lk(coreMutex);
      cpu.maxInsn = cpu.retired + 1;
      // El tope de instrucciones se comprueba en la guarda de depuracion del interprete, y
      // esa guarda se calcula UNA vez al parsear el entorno. Ponerlo aqui sin re-armarla
      // dejaba el corte inerte: el emulador seguia corriendo y reimprimiendo este mensaje
      // campo tras campo. Solo no se notaba porque el gate pasa ademas KESTREL_MAXINSN,
      // que ya la arma.
      cpu.refreshDebugArmed();
      maxFlips = maxSyncs = 0;    // ya disparado: no repetir el aviso cada campo
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
      // RDRAM has no per-transaction cycle model yet → leave at 0 (unmodeled).
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
      hbLast = now;
    }
  }
  hostprof::stop();
  shutdown.store(true);
  if(wdog.joinable()) { shutdown.store(true); wdog.join(); }
  if(occTh.joinable()) occTh.join();
}

}  // namespace kestrel
