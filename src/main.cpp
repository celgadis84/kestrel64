// kestrel64 — entry point.
// Usage: kestrel64 <rom.z64|.n64|.v64> [--port N]

#include "core/system.hpp"
#include "cpu/jit.hpp"
#include "rsp/rsp.hpp"
#include "audio/audio.hpp"
#include "core/runtime.hpp"
#include "ui/menu.hpp"
#include "ui/library.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <csignal>

#ifdef _WIN32
#include <windows.h>
#endif

#ifdef KESTREL_PGO_GEN
// Modo de captura de perfil (--pgo-capture). El tiempo de ejecucion de la instrumentacion de
// clang escribe el .profraw al salir y toma el nombre del fichero del entorno; estas dos
// funciones son la misma decision tomada DESDE DENTRO, que es lo que permite que una persona
// capture jugando sin tener que exportar nada antes de arrancar.
extern "C" void __llvm_profile_set_filename(const char*);
extern "C" void __llvm_profile_reset_counters(void);
#endif

// Nombre corto de la ROM, sin carpeta ni extension: con el se nombra el .profraw para que una
// carpeta de capturas diga de un vistazo de que juego salio cada una.
static auto romStem(const std::string& path) -> std::string {
  std::string::size_type a = path.find_last_of("/\\");
  std::string s = (a == std::string::npos) ? path : path.substr(a + 1);
  std::string::size_type d = s.find_last_of('.');
  if(d != std::string::npos) s = s.substr(0, d);
  for(char& c : s) if(c == ' ' || c == '(' || c == ')' || c == ',') c = '-';
  return s.empty() ? std::string("rom") : s;
}

static kestrel::System* g_system = nullptr;

static void onSignal(int) {
  if(g_system) g_system->requestShutdown();
}

#ifdef _WIN32
// Host-crash forensics: on an access violation, dump the guest CPU state so we
// can map the host fault back to the interpreter path / guest PC that caused it.
static LONG WINAPI crashFilter(EXCEPTION_POINTERS* ep) {
  auto* rec = ep->ExceptionRecord;
  std::fprintf(stderr, "\n==== HOST EXCEPTION 0x%08lx at host RIP %p ====\n",
               (unsigned long)rec->ExceptionCode, rec->ExceptionAddress);
  // De donde es ese RIP. El numero pelado no dice NADA: con ASLR cambia en cada arranque.
  // Lo primero que hay que saber es si el fallo cayo dentro de una imagen -- y entonces el
  // desplazamiento desde su base es estable y se lo puede tragar `addr2line` -- o en memoria
  // sin imagen, que en este emulador significa el codigo que el dynarec genera al vuelo.
  // Esa distincion decide a que mitad del programa se mira.
  {
    const void* rip = rec->ExceptionAddress;
    HMODULE mod = nullptr;
    if(GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS
                          | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          (LPCSTR)rip, &mod) && mod) {
      char path[MAX_PATH] = {0};
      GetModuleFileNameA(mod, path, (DWORD)sizeof path);
      std::fprintf(stderr, "  modulo %s  base=%p  RVA=0x%llx\n", path, (void*)mod,
                   (unsigned long long)((const char*)rip - (const char*)mod));
    } else {
      MEMORY_BASIC_INFORMATION mbi{};
      const bool ok = VirtualQuery(rip, &mbi, sizeof mbi) != 0;
      std::fprintf(stderr, "  RIP FUERA de toda imagen%s -> codigo emitido por el dynarec\n",
                   ok && mbi.Type == MEM_PRIVATE ? " (memoria privada)" : "");
    }
  }
  if(rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
    std::fprintf(stderr, "  access violation: %s at host addr 0x%llx\n",
                 rec->ExceptionInformation[0] == 0 ? "READ" :
                 rec->ExceptionInformation[0] == 1 ? "WRITE" : "EXEC",
                 (unsigned long long)rec->ExceptionInformation[1]);
  }
  if(g_system) {
    auto& c = g_system->cpu;
    std::fprintf(stderr, "  guest pc=0x%016llx nextPc=0x%016llx halted=%d\n",
                 (unsigned long long)c.pc, (unsigned long long)c.nextPc, c.halted);
    // Jump log: last control transfers, newest first.
    std::fprintf(stderr, "  --- jumplog (newest first) ---\n");
    for(int k = 0; k < kestrel::CPU::kJumpLog; k++) {
      kestrel::u32 i = (c.jlogIdx - 1 - k) % kestrel::CPU::kJumpLog;
      if(c.jlogSrc[i] == 0 && c.jlogDst[i] == 0) continue;
      std::fprintf(stderr, "    [%02d] op=0x%02x  src=0x%08llx -> dst=0x%08llx\n",
                   k, c.jlogOp[i],
                   (unsigned long long)c.jlogSrc[i], (unsigned long long)c.jlogDst[i]);
    }
  }
  std::fflush(stderr);
  return EXCEPTION_EXECUTE_HANDLER;  // let the process die after we've logged
}
#endif

#ifdef _WIN32
// Lanzado desde el Explorador o desde una consola?
//
// Sin --run el emulador arranca EN PAUSA, que es lo correcto para depurar (deja enganchar el
// MCP antes de la primera instruccion) y lo que asumen los gates. Pero para quien hace doble
// clic en el .exe eso es indistinguible de un cuelgue: ventana negra y nada mas.
//
// La distincion la da el propio Windows. Un proceso de subsistema consola lanzado desde el
// Explorador recibe una consola RECIEN CREADA para el solo; lanzado desde una shell hereda la
// de la shell, que sigue adjunta. GetConsoleProcessList cuenta los adjuntos: 1 = nadie mas =
// venimos del Explorador. Desde MSYS2 o cmd el contador es mayor y el arranque en pausa queda
// intacto, asi que ni los gates ni el flujo de depuracion se enteran de esto.
static auto launchedFromExplorer() -> bool {
  DWORD pids[4] = {0};
  DWORD n = GetConsoleProcessList(pids, 4);
  return n == 1;
}

// Sin ROM y sin consola donde leer un mensaje de error, la unica salida util es preguntar.
// Y preguntar es la BIBLIOTECA (src/ui/library_win32.cpp): el carrusel de caratulas con la
// carpeta que ya usa el lanzador, no un dialogo de fichero pelado. Desde dentro de ella
// sigue estando el dialogo de siempre (F3 / Ctrl+O) para una ROM que este fuera de la
// carpeta. Aqui se llama en el hilo principal, que todavia no presenta nada.
static auto pickRomDialog() -> std::string {
  return kestrel::ui::pickRomLibrary(nullptr);
}

// Un fprintf(stderr) en una consola que se cierra sola al terminar el proceso no lo lee nadie.
static auto guiError(const char* msg) -> void {
  MessageBoxA(nullptr, msg, "kestrel64", MB_OK | MB_ICONERROR);
}
#endif

int main(int argc, char** argv) {
  std::setvbuf(stderr, nullptr, _IONBF, 0);   // diagnósticos (heartbeat/jitstats) en streaming
  std::string romPath;
  kestrel::u16 port = 9128;
  bool freeRun = false;
  bool play = false;   // modo usuario final: corriendo Y con ventana
  [[maybe_unused]] bool useLibrary = false;  // abrir la biblioteca aunque haya consola (--library)
  bool pgoCapture = false;                  // --pgo-capture: escribir el perfil de PGO al salir
  std::string pgoName;                      // nombre que lo etiqueta (por defecto, el de la ROM)

  for(int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if(a == "--port" && i + 1 < argc) { port = (kestrel::u16)std::atoi(argv[++i]); }
    else if(a == "--run") { freeRun = true; }  // start unpaused (default: paused for stepping)
    // --run solo significa "sin pausa", y ademas apaga la ventana porque nacio para el lote
    // (gates, bench, krom). --play es lo que quiere una persona: corriendo Y viendose.
    else if(a == "--play") { play = true; }
    // Captura de perfil jugando de verdad. Solo hace algo en un binario instrumentado
    // (cmake -DKESTREL_PGO=gen, o sh scripts/pgo.sh --capture, que lo construye y lo lanza).
    // Sin nombre opcional a proposito: lo siguiente en la linea de ordenes es casi siempre la
    // ROM, y tragarsela dejaba al emulador sin nada que cargar. El .profraw se nombra con la
    // ROM; para etiquetar una captura concreta esta KESTREL_PGO_NAME.
    else if(a == "--pgo-capture") { pgoCapture = true; }
#ifdef _WIN32
    // Abre la BIBLIOTECA (el carrusel de caratulas) aunque haya consola. Es el modo de
    // uso normal cuando se lanza a mano, y la unica forma de probar el lanzador desde una
    // shell: al abrir con doble clic sale solo.
    else if(a == "--library" || a == "--lib") { play = true; useLibrary = true; }
#endif
    else if(a == "--rspfuzz") {                 // differential VU fuzz: scalar vs SSE, then exit
      unsigned long long iters = (i + 1 < argc && argv[i + 1][0] != '-') ? std::strtoull(argv[++i], nullptr, 10) : 20000000ull;
      kestrel::Rsp rsp;
      std::fprintf(stderr, "[rspfuzz] running %llu iterations...\n", iters);
      kestrel::u64 fails = rsp.fuzzVU(iters);
      return fails == 0 ? 0 : 1;
    }
    else if(a == "--rspjitfuzz") {              // VU en linea del dynarec vs interprete
      unsigned long long iters = (i + 1 < argc && argv[i + 1][0] != 0x2d) ? std::strtoull(argv[++i], nullptr, 10) : 200000ull;
      kestrel::Memory bus;
      bus.reset(false);
      kestrel::Rsp rsp; rsp.mem = &bus;
      std::fprintf(stderr, "[rspjitfuzz] %llu iteraciones...\n", iters);
      kestrel::u64 fails = rsp.fuzzVuJit(iters);
      return fails == 0 ? 0 : 1;
    }
    else if(a == "--rspldfuzz") {               // fuzz de cargas/tiendas vectoriales
      unsigned long long iters = (i + 1 < argc && argv[i + 1][0] != 0x2d) ? std::strtoull(argv[++i], nullptr, 10) : 500000ull;
      kestrel::Memory bus;
      bus.reset(false);
      kestrel::Rsp rsp; rsp.mem = &bus;
      std::fprintf(stderr, "[rspldfuzz] %llu iteraciones...\n", iters);
      return rsp.fuzzLdSt(iters) == 0 ? 0 : 1;
    }
    else if(a == "--rspbench") {                // rendimiento del interprete de RSP entero
      unsigned long long iters = (i + 1 < argc && argv[i + 1][0] != '-') ? std::strtoull(argv[++i], nullptr, 10) : 100000000ull;
      kestrel::Memory bus;
      bus.reset(false);
      kestrel::Rsp rsp; rsp.mem = &bus;
      rsp.benchStep(iters);
      return 0;
    }
    else if(a == "--vubench") {                 // time scalar vs SSE VU path, then exit
      unsigned long long iters = (i + 1 < argc && argv[i + 1][0] != '-') ? std::strtoull(argv[++i], nullptr, 10) : 100000000ull;
      kestrel::Rsp rsp;
      rsp.benchVU(iters);
      return 0;
    }
    else if(a == "--help" || a == "-h") {
      std::printf("kestrel64 %s\nusage: %s <rom> [--port N] [--run|--play]\n"
                  "  --run   sin pausa y sin ventana (lote: gates, bench)\n"
                  "  --play  sin pausa y con ventana (uso normal; implicito al abrir desde el Explorador)\n"
                  "  --library  abre la biblioteca de ROMs (carrusel de caratulas)\n"
                  "  --pgo-capture  graba perfil de PGO jugando (solo binario instrumentado)\n",
                  kestrel::System::kVersion, argv[0]);
      return 0;
    }
    else if(!a.empty() && a[0] != '-') { romPath = a; }
  }

  std::printf("kestrel64 %s - N64 emulator\n", kestrel::System::kVersion);

  // Dynarec foundation self-test (Etapa 1): verifies the RWX code buffer + x86-64
  // emitter produce runnable code. Gated; does not touch the interpreter path.
  if(std::getenv("KESTREL_JIT_SMOKE")) {
    bool ok = kestrel::jit::smokeTest();
    // SRA $3,$4,4  (rd=3 rt=4 sa=4, funct=3): esperado 0xfffffffff89abcde para rt=...89abcdef
    kestrel::u32 sra = (0u<<26)|(0u<<21)|(4u<<16)|(3u<<11)|(4u<<6)|3u;
    kestrel::jit::opSelfTest(sra, 0, 0x0123456789abcdefull, 3);
    // SRAV $3,$4,$5 (rd=3 rt=4 rs=5 funct=7): shift = rs&31
    kestrel::u32 srav = (0u<<26)|(5u<<21)|(4u<<16)|(3u<<11)|(0u<<6)|7u;
    kestrel::jit::opSelfTest(srav, 4, 0x0123456789abcdefull, 3);
    // Etapa 2b: prueba call-C + bail condicional con patch de rel32 (aún sin cablear).
    bool cb = kestrel::jit::callBailSelfTest();
    ok = ok && cb;
    if(!romPath.empty()) std::fprintf(stderr, "[jit] smoke test %s; continuing\n", ok ? "ok" : "FAILED");
    else return ok ? 0 : 1;
  }

#ifdef _WIN32
  // Doble clic en el .exe: ni --play ni ROM en la linea de ordenes, y una consola que se cerrara
  // sola. Se asume el modo de uso normal y se pregunta por la ROM.
  if(useLibrary || (!freeRun && launchedFromExplorer())) {
    play = true;
    if(romPath.empty()) romPath = pickRomDialog();
    if(romPath.empty()) return 0;                    // el usuario cancelo: salir en silencio
  }
#endif
  if(play) freeRun = true;

  if(romPath.empty()) {
    std::fprintf(stderr, "error: no ROM given.\nusage: %s <rom> [--port N] [--run|--play]\n", argv[0]);
    return 1;
  }

  // Siembra desde el entorno los ajustes que el menu de la ventana podra cambiar en
  // caliente. Antes de arrancar nada: a partir de aqui manda el atomico, no getenv().
  if(pgoCapture) {
#ifdef KESTREL_PGO_GEN
    if(const char* e = std::getenv("KESTREL_PGO_NAME"); e && *e) pgoName = e;
    if(pgoName.empty()) pgoName = romStem(romPath);
    // %p = pid, para que dos capturas seguidas no se pisen. La carpeta es la que lee
    // scripts/pgo.sh --merge.
    std::string out = "pgo/raw/" + pgoName + "-%p.profraw";
    __llvm_profile_set_filename(out.c_str());
    // Los contadores traen ya lo que costo arrancar (biblioteca, dialogos, carga de la ROM).
    // Eso no es el codigo caliente que se quiere colocar bien, asi que se tiran y el perfil
    // empieza limpio en el primer fotograma.
    __llvm_profile_reset_counters();
    std::printf("[pgo] capturando: al salir se escribe %s\n", out.c_str());
    std::printf("[pgo] juega un rato de VERDAD (nivel cargado, no el menu) y sal normal.\n");
    std::printf("[pgo] luego: sh scripts/pgo.sh --merge\n");
#else
    std::fprintf(stderr, "[pgo] este binario NO esta instrumentado: --pgo-capture no hace nada.\n"
                         "[pgo] usa: sh scripts/pgo.sh --capture \"<rom>\"\n");
#endif
  }

  kestrel::rt::initFromEnv();

  kestrel::System system;
  g_system = &system;
  std::signal(SIGINT, onSignal);
  std::signal(SIGTERM, onSignal);
#ifdef _WIN32
  SetUnhandledExceptionFilter(crashFilter);
#endif

  std::string error;
  if(!system.init(romPath, error)) {
    std::fprintf(stderr, "error: %s\n", error.c_str());
#ifdef _WIN32
    if(play) guiError(("No se pudo abrir la ROM:\n" + error).c_str());
#endif
    return 1;
  }

  system.paused.store(!freeRun);
  const bool batch = freeRun && !play;   // lote = corre sin ventana; jugar = corre CON ventana
  system.exitOnHalt = batch;      // lote headless: halt = fin de sesión, no punto de inspección
  system.startTelemetry(port);
  system.startVideo(batch);     // --run = lote: sin ventana salvo KESTREL_VIDEO
  // El cartel decia siempre "CPU interpreter", que dejo de ser verdad cuando el dynarec
  // paso a ir puesto por defecto: quien mirase la consola creia estar midiendo el
  // interprete. Ahora dice lo que de verdad esta armado.
  std::printf("[system] running (CPU %s, %s). Ctrl-C to quit.\n",
              kestrel::envFlag("KESTREL_JIT", true) ? "dynarec" : "interprete",
              !freeRun ? "paused � step over MCP" : (play ? "free-run + video" : "free-run"));
  std::fflush(stdout);

  system.runLoop();
  kestrel::audio::shutdown();
  std::printf("[system] shutdown.\n");
  std::fflush(stdout);
  // El relanzado va AQUI, con el audio ya cerrado: el proceso nuevo abre waveOut nada
  // mas arrancar, y encontrarselo ocupado por el viejo lo dejaria mudo. El servidor de
  // telemetria se cierra igual y por lo mismo: si no, el proceso nuevo se encuentra el
  // puerto cogido por este, que todavia no ha muerto.
  if(kestrel::ui::relaunchPending()) {
    system.stopTelemetry();
    kestrel::ui::doRelaunch();
  }
  return 0;
}
