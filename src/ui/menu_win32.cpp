#include "menu.hpp"
#include "optdefs.hpp"
#include "profile.hpp"
#include "library.hpp"
#include "../core/runtime.hpp"

#ifdef _WIN32
#include <windows.h>
#include <commctrl.h>
#include <windowsx.h>   // GET_X_LPARAM: el clic sobre el dibujo del mando
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <mutex>
#include <thread>
#include <vector>

namespace kestrel::ui {
namespace {

// ---------------------------------------------------------------- estado del modulo

Hooks    g_h;
HWND     g_game = nullptr;      // la ventana del juego (creada por GLFW)
WNDPROC  g_prev = nullptr;      // procedimiento original de esa ventana
HMENU    g_menu = nullptr;
Profile  g_prof;                // perfil vivo; se guarda al aplicar
bool     g_relaunch = false;    // un dialogo ha pedido relanzar
// El perfil lo tocan el hilo de la ventana (clics de menu) y los hilos de los dialogos.
std::mutex g_mu;
HWND     g_optWin = nullptr;    // dialogo de opciones (una sola instancia)
HWND     g_padWin = nullptr;    // dialogo de mando
bool     g_menuHidden = false;  // barra retirada (pantalla completa)

// Los dialogos corren en su hilo. Tocar la barra de menu (SetMenu/DrawMenuBar) desde el
// hilo que no es dueno de la ventana es pedir problemas, asi que se le manda un aviso y lo
// hace el.
constexpr UINT kMsgSync = WM_APP + 17;

enum : UINT {
  kOpenRom = 0x8100, kOpenFile, kRelaunch, kQuit,
  kPause, kReset, kSaveState, kLoadState, kNextSlot,
  kFull, kHud,
  kAudioOn, kPadCfg, kAllOpts, kAbout,
  kScale1  = 0x8140,              // +0..+6 (escala 1x..7x)
  kThrHw = 0x8160, kThrAuto, kThrOn, kThrOff,
  kSlot0   = 0x8170,              // +0..+9
  // El volumen gasta 101 identificadores seguidos (el valor ES el desplazamiento), asi que
  // va el ULTIMO y con sitio de sobra: CheckMenuRadioItem desmarca todo el rango que se le
  // da, y solaparlo con otro grupo borraria sus marcas.
  kVol0    = 0x8200,              // +0..+100
};

// ---------------------------------------------------------------- utilidades

auto exePath() -> std::string {
  char buf[MAX_PATH] = {0};
  GetModuleFileNameA(nullptr, buf, MAX_PATH);
  return buf;
}

// Escala de ventana pedida por el perfil, en pixeles. KESTREL_WINSIZE manda sobre la escala,
// igual que en el arranque (present.cpp).
auto profileWindowSize(const Profile& p, int& w, int& h) -> void {
  int n = std::atoi(p.get("winscale").c_str());
  if(n < 1 || n > 16) n = 2;
  w = 320 * n;
  h = 240 * n;
  unsigned uw = 0, uh = 0;
  char x = 0;
  if(std::sscanf(p.get("winsize").c_str(), "%u%c%u", &uw, &x, &uh) == 3 &&
     (x == 'x' || x == 'X') && uw >= 64 && uh >= 64) {
    w = (int)uw;
    h = (int)uh;
  }
}

// El perfil trae overclock guardado? El modo fiel a consola lo anula, pero eso solo se puede
// hacer al montar los relojes, o sea relanzando; con todo a 1.00x no hay nada que anular y el
// cambio es instantaneo.
auto ocNotStock(const Profile& p) -> bool {
  static const char* kIds[4] = {"oc_all", "oc_cpu", "oc_rsp", "oc_rdram"};
  for(const char* id : kIds) {
    double v = std::atof(p.get(id).c_str());
    if(v > 0.0 && std::fabs(v - 1.0) > 1e-9) return true;
  }
  return false;
}

// Aplica al vuelo lo que se puede aplicar al vuelo. El resto del perfil ya esta guardado y
// solo se hace efectivo relanzando.
auto applyLive(const Profile& p, bool windowToo) -> void {
  rt::hud.store(p.getBool("hud"));
  rt::audioOn.store(p.getBool("audio"));
  int vol = std::atoi(p.get("volume").c_str());
  rt::volume.store(vol < 0 ? 0 : vol > 100 ? 100 : vol);
  std::string th = p.get("throttle");
  rt::throttle.store(th == "1" ? 1 : th == "0" ? 0 : -1);
  // Relacion de aspecto: "4:3", "16:9" o cualquier "W:H"/"WxH"; lo que no se parsee (o sea
  // "estirar") va a 0:0, que el presentador entiende como llenar la ventana entera.
  {
    int aw = 0, ah = 0;
    if(std::sscanf(p.get("aspect").c_str(), "%d%*[:xX]%d", &aw, &ah) != 2 || aw <= 0 || ah <= 0)
      { aw = 0; ah = 0; }
    rt::aspectW.store(aw);
    rt::aspectH.store(ah);
  }
  if(windowToo) {
    int w = 0, h = 0;
    profileWindowSize(p, w, h);
    rt::winW.store(w);
    rt::winH.store(h);
    rt::winFull.store(p.getBool("fullscreen") ? 1 : 0);
    rt::winReq.store(1, std::memory_order_release);
  }
}

// ---------------------------------------------------------------- barra de menu

auto addItem(HMENU m, UINT id, const char* text, bool checked = false, bool radio = false)
    -> void {
  MENUITEMINFOA mi = {};
  mi.cbSize = sizeof mi;
  mi.fMask = MIIM_ID | MIIM_STRING | MIIM_STATE | MIIM_FTYPE;
  mi.fType = radio ? MFT_RADIOCHECK : MFT_STRING;
  mi.fState = checked ? MFS_CHECKED : MFS_UNCHECKED;
  mi.wID = id;
  mi.dwTypeData = (LPSTR)text;
  InsertMenuItemA(m, GetMenuItemCount(m), TRUE, &mi);
}

auto addSep(HMENU m) -> void {
  MENUITEMINFOA mi = {};
  mi.cbSize = sizeof mi;
  mi.fMask = MIIM_FTYPE;
  mi.fType = MFT_SEPARATOR;
  InsertMenuItemA(m, GetMenuItemCount(m), TRUE, &mi);
}

auto addSub(HMENU parent, HMENU sub, const char* text) -> void {
  MENUITEMINFOA mi = {};
  mi.cbSize = sizeof mi;
  mi.fMask = MIIM_STRING | MIIM_SUBMENU | MIIM_FTYPE;
  mi.fType = MFT_STRING;
  mi.hSubMenu = sub;
  mi.dwTypeData = (LPSTR)text;
  InsertMenuItemA(parent, GetMenuItemCount(parent), TRUE, &mi);
}

auto buildMenu() -> void {
  g_menu = CreateMenu();

  HMENU file = CreatePopupMenu();
  addItem(file, kOpenRom, "&Biblioteca de ROMs...\tCtrl+O");
  addItem(file, kOpenFile, "Abrir un &fichero...");
  addItem(file, kRelaunch, "&Reiniciar emulador");
  addSep(file);
  addItem(file, kQuit, "&Salir");
  addSub(g_menu, file, "&Archivo");

  HMENU emu = CreatePopupMenu();
  addItem(emu, kPause, "&Pausa\tCtrl+P");
  addSep(emu);
  addItem(emu, kSaveState, "&Guardar estado\tF5");
  addItem(emu, kLoadState, "&Cargar estado\tF7");
  addItem(emu, kNextSlot, "Siguiente &ranura\tF6");
  HMENU slot = CreatePopupMenu();
  for(int i = 0; i < 10; i++) {
    char t[16];
    std::snprintf(t, sizeof t, "Ranura &%d", i);
    addItem(slot, kSlot0 + (UINT)i, t, i == 0, true);
  }
  addSub(emu, slot, "&Ranura");
  addSep(emu);
  HMENU thr = CreatePopupMenu();
  addItem(thr, kThrHw, "Fiel a consola (velocidad del N64 real)", false, true);
  addSep(thr);
  addItem(thr, kThrAuto, "Automatico (limita si hay ventana)", true, true);
  addItem(thr, kThrOn, "Siempre a 59.94 campos/s", false, true);
  addItem(thr, kThrOff, "Sin limite (a tope)", false, true);
  addSub(emu, thr, "&Velocidad");
  addSub(g_menu, emu, "&Emulacion");

  HMENU vid = CreatePopupMenu();
  HMENU sc = CreatePopupMenu();
  static const char* kScaleTxt[7] = {
    "1x - 320x240", "2x - 640x480", "3x - 960x720", "4x - 1280x960",
    "5x - 1600x1200", "6x - 1920x1440", "7x - 2240x1680",
  };
  for(int i = 0; i < 7; i++) addItem(sc, kScale1 + i, kScaleTxt[i], i == 1, true);
  addSub(vid, sc, "&Escala");
  addItem(vid, kFull, "&Pantalla completa\tF11");
  addItem(vid, kHud, "&HUD de telemetria", true);
  addSub(g_menu, vid, "&Video");

  HMENU aud = CreatePopupMenu();
  addItem(aud, kAudioOn, "&Sonido", true);
  HMENU vol = CreatePopupMenu();
  static const int kVols[5] = {0, 25, 50, 75, 100};
  static const char* kVolTxt[5] = {"Mudo", "25%", "50%", "75%", "100%"};
  for(int i = 0; i < 5; i++) addItem(vol, kVol0 + (UINT)kVols[i], kVolTxt[i], i == 4, true);
  addSub(aud, vol, "&Volumen");
  addSub(g_menu, aud, "&Audio");

  HMENU cfg = CreatePopupMenu();
  addItem(cfg, kPadCfg, "&Mando...");
  addItem(cfg, kAllOpts, "&Todas las opciones...\tF12");
  addSub(g_menu, cfg, "&Opciones");

  HMENU help = CreatePopupMenu();
  addItem(help, kAbout, "&Acerca de");
  addSub(g_menu, help, "A&yuda");
}

// En pantalla completa la barra estorba y ademas roba alto de imagen. Se retira y se
// devuelve sola al volver a ventana; F11 y F12 siguen funcionando mientras no esta.
auto syncMenuVisibility() -> void {
  if(!g_menu || !g_game) return;
  bool full = g_prof.getBool("fullscreen");
  if(full == g_menuHidden) return;
  g_menuHidden = full;
  SetMenu(g_game, full ? nullptr : g_menu);
  if(!full) {
    RECT rc;
    GetWindowRect(g_game, &rc);
    SetWindowPos(g_game, nullptr, 0, 0, rc.right - rc.left,
                 rc.bottom - rc.top + GetSystemMetrics(SM_CYMENU),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  }
  DrawMenuBar(g_game);
}

auto syncMenu() -> void {
  if(!g_menu) return;
  if(g_menuHidden) return;   // sin barra puesta no hay nada que marcar
  CheckMenuItem(g_menu, kHud, MF_BYCOMMAND | (rt::hud.load() ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(g_menu, kAudioOn,
                MF_BYCOMMAND | (rt::audioOn.load() ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(g_menu, kFull,
                MF_BYCOMMAND | (g_prof.getBool("fullscreen") ? MF_CHECKED : MF_UNCHECKED));
  CheckMenuItem(g_menu, kPause,
                MF_BYCOMMAND | ((g_h.paused && g_h.paused->load()) ? MF_CHECKED : MF_UNCHECKED));
  int n = std::atoi(g_prof.get("winscale").c_str());
  if(n < 1 || n > 7) n = 2;
  CheckMenuRadioItem(g_menu, kScale1, kScale1 + 6, kScale1 + (UINT)(n - 1), MF_BYCOMMAND);
  int v = rt::volume.load();
  UINT pick = kVol0 + (UINT)(v >= 88 ? 100 : v >= 63 ? 75 : v >= 38 ? 50 : v >= 13 ? 25 : 0);
  CheckMenuRadioItem(g_menu, kVol0, kVol0 + 100, pick, MF_BYCOMMAND);
  if(g_h.stSlot) {
    int sl = g_h.stSlot->load();
    if(sl < 0 || sl > 9) sl = 0;
    CheckMenuRadioItem(g_menu, kSlot0, kSlot0 + 9, kSlot0 + (UINT)sl, MF_BYCOMMAND);
  }
  int th = rt::throttle.load();
  const bool hw = g_prof.get("speedmode") == "hw";
  CheckMenuRadioItem(g_menu, kThrHw, kThrOff,
                     hw ? kThrHw : th < 0 ? kThrAuto : th ? kThrOn : kThrOff, MF_BYCOMMAND);
  DrawMenuBar(g_game);
}

// ---------------------------------------------------------------- relanzar

// Reconstruye la linea de ordenes y el entorno desde el perfil y arranca una copia nueva del
// emulador. Se llama SIEMPRE desde main(), con el audio y el video ya cerrados.
// Ejecutable INSTRUMENTADO para grabar perfil de PGO, y la carpeta desde la que hay que
// lanzarlo, o vacio si no esta compilado. La instrumentacion se decide al COMPILAR
// (cmake -DKESTREL_PGO=gen), asi que grabar perfil no es una opcion del mismo .exe sino OTRO
// .exe. En arbol de desarrollo el nuestro vive en <raiz>/build-*/kestrel64.exe y el
// instrumentado en <raiz>/build-pgogen/; instalado, un kestrel64-pgo.exe al lado.
static auto pgoExe(std::string& cwd) -> std::string {
  std::string me = exePath();
  std::string dir = me.substr(0, me.find_last_of("/\\"));
  std::string cands[2] = {dir.substr(0, dir.find_last_of("/\\")) + "/build-pgogen/kestrel64.exe",
                          dir + "/kestrel64-pgo.exe"};
  for(int i = 0; i < 2; i++) {
    if(GetFileAttributesA(cands[i].c_str()) == INVALID_FILE_ATTRIBUTES) continue;
    // El .profraw se escribe en "pgo/raw/", relativo a la carpeta de trabajo: para el
    // instrumentado del arbol esa carpeta es la RAIZ del proyecto, que es de donde lee
    // `sh scripts/pgo.sh --merge`.
    cwd = i == 0 ? dir.substr(0, dir.find_last_of("/\\")) : dir;
    return cands[i];
  }
  cwd.clear();
  return std::string();
}

auto relaunchNow() -> void {
  std::vector<std::pair<std::string, std::string>> env;
  std::vector<std::string> argv;
  toEnv(g_prof, env, argv);

  // El entorno del proceso nuevo se hereda del actual, asi que primero hay que BORRAR todas
  // las KESTREL_* que trae puestas: si no, una opcion que se acaba de apagar seguiria
  // encendida por herencia y el usuario veria que "no se aplico".
  if(LPCH block = GetEnvironmentStringsA()) {
    for(LPCH e = block; *e; e += std::strlen(e) + 1) {
      if(std::strncmp(e, "KESTREL_", 8)) continue;
      const char* eq = std::strchr(e, '=');
      if(!eq || eq == e) continue;
      std::string name(e, eq - e);
      SetEnvironmentVariableA(name.c_str(), nullptr);
    }
    FreeEnvironmentStringsA(block);
  }
  for(const auto& kv : env) SetEnvironmentVariableA(kv.first.c_str(), kv.second.c_str());
  // El barrido de arriba tambien se ha llevado esta, que no sale del catalogo: el proceso
  // nuevo tiene que seguir apuntando al MISMO perfil que acabamos de guardar.
  SetEnvironmentVariableA("KESTREL_PROFILE", profilePath().c_str());

  // Grabar perfil de PGO manda sobre todo lo demas: hay que relanzar OTRO binario, el
  // instrumentado, y desde la raiz del proyecto.
  std::string exe = exePath(), cwd;
  if(g_prof.getBool("pgocap")) {
    std::string pe = pgoExe(cwd);
    if(pe.empty()) {
      MessageBoxA(nullptr,
                  "Grabar perfil (PGO) pide el binario instrumentado y no esta compilado.\n\n"
                  "Compilalo con:\n"
                  "  cmake -S . -B build-pgogen -DKESTREL_PRDP=ON -DKESTREL_PGO=gen\n"
                  "  cmake --build build-pgogen -j8\n\n"
                  "o directamente: sh scripts/pgo.sh --capture \"<rom>\"",
                  "kestrel64", MB_OK | MB_ICONWARNING);
      g_prof.set("pgocap", "0");
      saveProfile(g_prof);
    } else {
      exe = pe;
    }
  }
  std::string cmd = "\"" + exe + "\"";
  std::string rom = g_prof.get("rom");
  if(rom.empty()) rom = g_h.rom;
  if(!rom.empty()) cmd += " \"" + rom + "\"";
  for(const auto& a : argv) cmd += " " + a;

  STARTUPINFOA si = {};
  si.cb = sizeof si;
  PROCESS_INFORMATION pi = {};
  std::vector<char> line(cmd.begin(), cmd.end());
  line.push_back(0);
  if(CreateProcessA(nullptr, line.data(), nullptr, nullptr, FALSE, 0, nullptr,
                    cwd.empty() ? nullptr : cwd.c_str(), &si, &pi)) {
    // Que la ventana nueva pueda ponerse delante: si no, Windows la deja detras y parece que
    // el reinicio no hizo nada (es el mismo problema del foco que ya trata present.cpp).
    AllowSetForegroundWindow(pi.dwProcessId);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
  } else {
    MessageBoxA(nullptr, ("No se pudo relanzar el emulador:\n" + cmd).c_str(), "kestrel64",
                MB_OK | MB_ICONERROR);
  }
}

// Los dialogos son ventanas de primer nivel en hilos sueltos. Si el proceso muere con
// alguno vivo, sus hilos siguen tocando estos globales mientras corren los destructores
// estaticos, que es como se consigue un cierre con cuadro de error.
auto closeDialogs() -> void {
  if(g_optWin) PostMessageA(g_optWin, WM_CLOSE, 0, 0);
  if(g_padWin) PostMessageA(g_padWin, WM_CLOSE, 0, 0);
}

auto askRelaunch(HWND owner, const char* why) -> void {
  std::string msg = std::string(why) +
      "\n\nEsas opciones solo se aplican al arrancar. Reiniciar el emulador ahora?"
      "\n(La partida en curso se pierde; guarda estado con F5 antes si quieres.)";
  if(MessageBoxA(owner, msg.c_str(), "kestrel64", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
  g_relaunch = true;
  closeDialogs();
  if(g_h.shutdown) g_h.shutdown->store(true, std::memory_order_release);
  PostMessageA(g_game, WM_CLOSE, 0, 0);
}

// ---------------------------------------------------------------- dialogo de opciones
//
// Se construye a partir del catalogo, en su PROPIO hilo con su propio bucle de mensajes: el
// hilo principal esta ocupado presentando cuadros y no puede quedarse dentro de un dialogo
// modal sin congelar la imagen, y un bucle modal ajeno tampoco puede robarle los mensajes de
// la ventana de juego. Cada dialogo es una ventana de primer nivel independiente.

struct Row {
  const Option* opt;
  HWND ctl = nullptr;
  HWND label = nullptr;
  HWND extra = nullptr;   // boton "..." de las rutas
  HWND slider = nullptr;  // barra deslizante de los numericos acotados (ver sliderTicks)
};

struct OptDlg {
  HWND win = nullptr, list = nullptr, panel = nullptr, advBox = nullptr, helpBox = nullptr;
  int  cat = 0;
  bool adv = false;
  std::vector<Row> rows;
  int  scroll = 0, extent = 0;
};

OptDlg g_dlg;

constexpr int kIdList = 700, kIdAdv = 701, kIdApply = 702, kIdSave = 703, kIdClose = 704;
constexpr int kIdPanel = 705, kIdCtl0 = 1000;

// ------------------------------------------------------------------ numericos con barra
//
// Un multiplicador de overclock (0.25x .. 8x en pasos de 0.05) se ajusta a tientas en una
// caja de texto: hay que saber de memoria el rango y teclear el numero. Con barra el rango
// se ve, el paso lo impone el control y no hace falta validar nada. La caja se queda al
// lado, editable, para teclear un valor exacto; barra y caja se espejan.
//
// Solo llevan barra los numericos ACOTADOS y de recorrido corto. Un tope como el perro
// guardian (0..100000 s) o el muestreo de solape no cabe en una barra util: esos siguen
// siendo caja de texto.
auto sliderStep(const Option& o) -> double {
  if(o.step > 0) return o.step;
  return o.type == OType::Int ? 1.0 : 0.0;
}

// Numero de posiciones de la barra, o 0 si esta opcion no debe llevarla.
auto sliderTicks(const Option& o) -> int {
  if(o.type != OType::Int && o.type != OType::Float) return 0;
  if(!(o.max > o.min)) return 0;
  double st = sliderStep(o);
  if(st <= 0) return 0;
  double n = (o.max - o.min) / st;
  if(n < 1 || n > 1000) return 0;   // recorrido inutilizable en una barra
  return (int)(n + 0.5);
}

// Texto canonico de un numero, con las mismas reglas que sanitize (los float sin ceros
// de cola, para que "1" no se guarde como "1.0000" y el perfil no cambie solo).
auto fmtNum(const Option& o, double x) -> std::string {
  char out[64];
  if(o.type == OType::Int) {
    std::snprintf(out, sizeof out, "%lld", (long long)(x < 0 ? x - 0.5 : x + 0.5));
    return out;
  }
  std::snprintf(out, sizeof out, "%.4f", x);
  std::string t(out);
  while(t.size() > 1 && t.back() == '0') t.pop_back();
  if(!t.empty() && t.back() == '.') t.pop_back();
  return t;
}

auto valToPos(const Option& o, double x) -> int {
  double st = sliderStep(o);
  if(st <= 0) return 0;
  if(x < o.min) x = o.min;
  if(x > o.max) x = o.max;
  int pos = (int)((x - o.min) / st + 0.5);
  int n = sliderTicks(o);
  if(pos < 0) pos = 0;
  if(pos > n) pos = n;
  return pos;
}

auto posToVal(const Option& o, int pos) -> double { return o.min + pos * sliderStep(o); }

// La barra manda: mueve la caja. Bandera para que el EN_CHANGE que dispara esto no
// rebote y vuelva a mover la barra bajo el dedo del usuario.
bool g_syncing = false;

auto sliderToEdit(const Row& r) -> void {
  if(!r.slider || !r.ctl) return;
  int pos = (int)SendMessageA(r.slider, TBM_GETPOS, 0, 0);
  g_syncing = true;
  SetWindowTextA(r.ctl, fmtNum(*r.opt, posToVal(*r.opt, pos)).c_str());
  g_syncing = false;
}

// La caja manda: mueve la barra. Texto a medio teclear (vacio, "0.") se ignora en vez de
// saltar la barra al minimo mientras se escribe.
auto editToSlider(const Row& r) -> void {
  if(!r.slider || !r.ctl || g_syncing) return;
  char buf[64] = {0};
  GetWindowTextA(r.ctl, buf, sizeof buf - 1);
  char* end = nullptr;
  double x = std::strtod(buf, &end);
  if(end == buf) return;
  SendMessageA(r.slider, TBM_SETPOS, TRUE, (LPARAM)valToPos(*r.opt, x));
}

auto rowValue(const Row& r) -> std::string {
  char buf[512] = {0};
  switch(r.opt->type) {
    case OType::Bool:
      return SendMessageA(r.ctl, BM_GETCHECK, 0, 0) == BST_CHECKED ? "1" : "0";
    case OType::Choice: {
      LRESULT i = SendMessageA(r.ctl, CB_GETCURSEL, 0, 0);
      if(i < 0 || i >= r.opt->nchoices) return r.opt->def ? r.opt->def : "";
      return r.opt->choices[i].value;
    }
    default:
      GetWindowTextA(r.ctl, buf, sizeof buf - 1);
      return buf;
  }
}

// Un campo numerico con basura ("30 fps", vacio) daba 0 en silencio y el emulador
// arrancaba con un valor que el usuario no habia pedido. Aqui se valida: lo que no es un
// numero se descarta (se queda el valor que habia) y lo que se sale del rango se recorta,
// y en los dos casos la casilla se corrige a la vista para que se vea lo que se ha guardado.
auto sanitize(const Row& r, const std::string& raw, const std::string& prev) -> std::string {
  const Option& o = *r.opt;
  if(o.type != OType::Int && o.type != OType::Float) return raw;
  const char* b = raw.c_str();
  while(*b == ' ') b++;
  if(!*b) return prev;
  char* end = nullptr;
  double x = std::strtod(b, &end);
  if(end == b) return prev;
  while(*end == ' ') end++;
  if(*end) return prev;                       // sobra texto detras del numero
  if(o.max > o.min) {
    if(x < o.min) x = o.min;
    if(x > o.max) x = o.max;
  }
  return fmtNum(o, x);
}

auto commitPanel(Profile& p) -> void {
  for(const Row& r : g_dlg.rows) {
    if(!r.ctl) continue;
    std::string raw = rowValue(r);
    std::string val = sanitize(r, raw, p.get(r.opt->id));
    if(val != raw && (r.opt->type == OType::Int || r.opt->type == OType::Float))
      SetWindowTextA(r.ctl, val.c_str());
    p.set(r.opt->id, val);
  }
}

// Hay algo escrito en el panel que aun no esta en el perfil?
auto panelDirty(const Profile& p) -> bool {
  for(const Row& r : g_dlg.rows) {
    if(!r.ctl) continue;
    if(sanitize(r, rowValue(r), p.get(r.opt->id)) != p.get(r.opt->id)) return true;
  }
  return false;
}

auto clearPanel() -> void {
  for(Row& r : g_dlg.rows) {
    if(r.ctl) DestroyWindow(r.ctl);
    if(r.label) DestroyWindow(r.label);
    if(r.extra) DestroyWindow(r.extra);
    if(r.slider) DestroyWindow(r.slider);
  }
  g_dlg.rows.clear();
}

auto panelFont() -> HFONT { return (HFONT)GetStockObject(DEFAULT_GUI_FONT); }

auto buildPanel() -> void {
  clearPanel();
  const Category& c = categories()[g_dlg.cat];
  HFONT f = panelFont();
  int y = 8;
  for(int i = 0; i < c.n; i++) {
    const Option& o = c.opts[i];
    if(o.adv && !g_dlg.adv) continue;
    Row r;
    r.opt = &o;
    std::string label = o.label;
    if(!o.live) label += "  (*)";
    if(o.type == OType::Bool) {
      r.ctl = CreateWindowExA(0, "BUTTON", label.c_str(),
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                              10, y, 540, 20, g_dlg.panel,
                              (HMENU)(INT_PTR)(kIdCtl0 + (int)g_dlg.rows.size()),
                              nullptr, nullptr);
      SendMessageA(r.ctl, BM_SETCHECK,
                   g_prof.getBool(o.id) ? BST_CHECKED : BST_UNCHECKED, 0);
    } else {
      r.label = CreateWindowExA(0, "STATIC", label.c_str(), WS_CHILD | WS_VISIBLE,
                                10, y + 3, 300, 18, g_dlg.panel, nullptr, nullptr, nullptr);
      if(o.type == OType::Choice) {
        r.ctl = CreateWindowExA(0, "COMBOBOX", "",
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST |
                                    WS_VSCROLL,
                                316, y, 300, 260, g_dlg.panel,
                                (HMENU)(INT_PTR)(kIdCtl0 + (int)g_dlg.rows.size()),
                                nullptr, nullptr);
        std::string cur = g_prof.get(o.id);
        int sel = 0;
        for(int k = 0; k < o.nchoices; k++) {
          SendMessageA(r.ctl, CB_ADDSTRING, 0, (LPARAM)o.choices[k].label);
          if(cur == o.choices[k].value) sel = k;
        }
        SendMessageA(r.ctl, CB_SETCURSEL, sel, 0);
      } else {
        int ticks = sliderTicks(o);
        // Con barra la caja se encoge y se va a la derecha; sin barra ocupa todo el ancho
        // como siempre.
        int editX = ticks ? 556 : 316, editW = ticks ? 64 : (o.type == OType::Path ? 260 : 300);
        if(ticks) {
          r.slider = CreateWindowExA(0, TRACKBAR_CLASSA, "",
                                     WS_CHILD | WS_VISIBLE | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,
                                     316, y, 232, 24, g_dlg.panel,
                                     (HMENU)(INT_PTR)(kIdCtl0 + (int)g_dlg.rows.size()),
                                     nullptr, nullptr);
          SendMessageA(r.slider, TBM_SETRANGE, TRUE, MAKELPARAM(0, ticks));
          SendMessageA(r.slider, TBM_SETPAGESIZE, 0, (LPARAM)(ticks / 10 + 1));
        }
        r.ctl = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", g_prof.get(o.id).c_str(),
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                editX, y, editW, 22, g_dlg.panel,
                                (HMENU)(INT_PTR)(kIdCtl0 + (int)g_dlg.rows.size() + 500),
                                nullptr, nullptr);
        // La barra arranca donde diga el perfil, no en el minimo.
        if(ticks) editToSlider(r);
      }
    }
    if(r.ctl) SendMessageA(r.ctl, WM_SETFONT, (WPARAM)f, TRUE);
    if(r.slider) SendMessageA(r.slider, WM_SETFONT, (WPARAM)f, TRUE);
    if(r.label) SendMessageA(r.label, WM_SETFONT, (WPARAM)f, TRUE);
    y += 26;
    if(o.help && *o.help) {
      // La ayuda va debajo, en gris: es la misma frase que ensena el lanzador y evita tener
      // que adivinar que hace una bandera con nombre de tres letras.
      HWND hw = CreateWindowExA(0, "STATIC", o.help, WS_CHILD | WS_VISIBLE,
                                30, y, 590, 30, g_dlg.panel, nullptr, nullptr, nullptr);
      SendMessageA(hw, WM_SETFONT, (WPARAM)f, TRUE);
      if(!r.label) r.label = hw; else r.extra = hw;
      y += 34;
    }
    g_dlg.rows.push_back(r);
    y += 4;
  }
  g_dlg.extent = y;
  g_dlg.scroll = 0;

  SCROLLINFO si = {};
  si.cbSize = sizeof si;
  si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
  RECT rc;
  GetClientRect(g_dlg.panel, &rc);
  si.nMin = 0;
  si.nMax = g_dlg.extent;
  si.nPage = (UINT)(rc.bottom - rc.top);
  si.nPos = 0;
  SetScrollInfo(g_dlg.panel, SB_VERT, &si, TRUE);
  InvalidateRect(g_dlg.panel, nullptr, TRUE);
}

auto scrollPanel(int newPos) -> void {
  RECT rc;
  GetClientRect(g_dlg.panel, &rc);
  int page = rc.bottom - rc.top;
  int maxPos = g_dlg.extent - page;
  if(maxPos < 0) maxPos = 0;
  if(newPos < 0) newPos = 0;
  if(newPos > maxPos) newPos = maxPos;
  int dy = g_dlg.scroll - newPos;
  if(!dy) return;
  g_dlg.scroll = newPos;
  ScrollWindowEx(g_dlg.panel, 0, dy, nullptr, nullptr, nullptr, nullptr,
                 SW_SCROLLCHILDREN | SW_INVALIDATE | SW_ERASE);
  SCROLLINFO si = {};
  si.cbSize = sizeof si;
  si.fMask = SIF_POS;
  si.nPos = newPos;
  SetScrollInfo(g_dlg.panel, SB_VERT, &si, TRUE);
  UpdateWindow(g_dlg.panel);
}

LRESULT CALLBACK panelProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch(m) {
    // Las barras son hijas del panel: sus avisos de movimiento llegan aqui. WM_HSCROLL no
    // choca con el WM_VSCROLL del propio panel (la barra es horizontal).
    case WM_HSCROLL: {
      if(!l) break;
      for(const Row& r : g_dlg.rows)
        if(r.slider == (HWND)l) { sliderToEdit(r); return 0; }
      break;
    }
    case WM_COMMAND: {
      if(HIWORD(w) != EN_CHANGE) break;
      for(const Row& r : g_dlg.rows)
        if(r.ctl == (HWND)l && r.slider) { editToSlider(r); return 0; }
      break;
    }
    case WM_VSCROLL: {
      SCROLLINFO si = {};
      si.cbSize = sizeof si;
      si.fMask = SIF_ALL;
      GetScrollInfo(h, SB_VERT, &si);
      int pos = si.nPos;
      switch(LOWORD(w)) {
        case SB_LINEUP:   pos -= 26; break;
        case SB_LINEDOWN: pos += 26; break;
        case SB_PAGEUP:   pos -= (int)si.nPage; break;
        case SB_PAGEDOWN: pos += (int)si.nPage; break;
        case SB_THUMBTRACK:
        case SB_THUMBPOSITION: pos = si.nTrackPos; break;
        default: return 0;
      }
      scrollPanel(pos);
      return 0;
    }
    case WM_MOUSEWHEEL:
      scrollPanel(g_dlg.scroll - GET_WHEEL_DELTA_WPARAM(w) / 2);
      return 0;
    case WM_CTLCOLORSTATIC:
      SetBkMode((HDC)w, TRANSPARENT);
      return (LRESULT)GetSysColorBrush(COLOR_WINDOW);
    case WM_ERASEBKGND: {
      RECT rc;
      GetClientRect(h, &rc);
      FillRect((HDC)w, &rc, GetSysColorBrush(COLOR_WINDOW));
      return 1;
    }
  }
  return DefWindowProcA(h, m, w, l);
}

LRESULT CALLBACK optProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch(m) {
    case WM_COMMAND: {
      std::lock_guard<std::mutex> lk(g_mu);
      int id = LOWORD(w);
      if(id == kIdList && HIWORD(w) == LBN_SELCHANGE) {
        commitPanel(g_prof);
        g_dlg.cat = (int)SendMessageA(g_dlg.list, LB_GETCURSEL, 0, 0);
        if(g_dlg.cat < 0) g_dlg.cat = 0;
        SetWindowTextA(g_dlg.helpBox, categories()[g_dlg.cat].desc);
        buildPanel();
        return 0;
      }
      if(id == kIdAdv) {
        commitPanel(g_prof);
        g_dlg.adv = SendMessageA(g_dlg.advBox, BM_GETCHECK, 0, 0) == BST_CHECKED;
        buildPanel();
        return 0;
      }
      if(id == kIdApply || id == kIdSave) {
        commitPanel(g_prof);
        saveProfile(g_prof);
        applyLive(g_prof, true);
        rt::padGen.fetch_add(1, std::memory_order_release);
        if(g_game) PostMessageA(g_game, kMsgSync, 0, 0);
        if(id == kIdSave)
          askRelaunch(h, "Guardado. Hay opciones marcadas con (*) que solo se leen al arrancar.");
        else
          MessageBoxA(h, "Aplicado lo que se puede cambiar en marcha y guardado el resto.\n"
                         "Las opciones marcadas con (*) necesitan reiniciar el emulador.",
                      "kestrel64", MB_OK | MB_ICONINFORMATION);
        return 0;
      }
      if(id == kIdClose) {
        if(panelDirty(g_prof) &&
           MessageBoxA(h, "Hay cambios sin aplicar en esta categoria. Descartarlos?",
                       "kestrel64", MB_YESNO | MB_ICONWARNING) != IDYES)
          return 0;
        DestroyWindow(h);
        return 0;
      }
      return 0;
    }
    case WM_CLOSE:
      DestroyWindow(h);
      return 0;
    case WM_DESTROY:
      clearPanel();
      g_optWin = nullptr;
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcA(h, m, w, l);
}

auto optionsDialogThread() -> void {
  static bool cls = false;
  if(!cls) {
    // Sin esto la clase de la barra deslizante no existe y CreateWindowEx devuelve nullptr.
    INITCOMMONCONTROLSEX icc = {sizeof icc, ICC_BAR_CLASSES};
    InitCommonControlsEx(&icc);
    WNDCLASSA wc = {};
    wc.lpfnWndProc = optProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "kestrel64_opts";
    RegisterClassA(&wc);
    WNDCLASSA pc = {};
    pc.lpfnWndProc = panelProc;
    pc.hInstance = wc.hInstance;
    pc.hCursor = wc.hCursor;
    pc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    pc.lpszClassName = "kestrel64_panel";
    RegisterClassA(&pc);
    cls = true;
  }

  g_dlg = OptDlg();
  HWND h = CreateWindowExA(0, "kestrel64_opts", "kestrel64 - opciones",
                           WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX,
                           CW_USEDEFAULT, CW_USEDEFAULT, 900, 660,
                           nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
  if(!h) return;
  g_optWin = h;
  g_dlg.win = h;
  HFONT f = panelFont();

  g_dlg.list = CreateWindowExA(WS_EX_CLIENTEDGE, "LISTBOX", "",
                               WS_CHILD | WS_VISIBLE | WS_TABSTOP | LBS_NOTIFY | WS_VSCROLL,
                               8, 8, 200, 520, h, (HMENU)(INT_PTR)kIdList, nullptr, nullptr);
  for(int i = 0; i < categoryCount(); i++)
    SendMessageA(g_dlg.list, LB_ADDSTRING, 0, (LPARAM)categories()[i].label);
  SendMessageA(g_dlg.list, LB_SETCURSEL, 0, 0);
  SendMessageA(g_dlg.list, WM_SETFONT, (WPARAM)f, TRUE);

  g_dlg.helpBox = CreateWindowExA(0, "STATIC", categories()[0].desc, WS_CHILD | WS_VISIBLE,
                                  216, 8, 650, 36, h, nullptr, nullptr, nullptr);
  SendMessageA(g_dlg.helpBox, WM_SETFONT, (WPARAM)f, TRUE);

  g_dlg.panel = CreateWindowExA(WS_EX_CLIENTEDGE, "kestrel64_panel", "",
                                WS_CHILD | WS_VISIBLE | WS_VSCROLL,
                                216, 48, 655, 480, h, (HMENU)(INT_PTR)kIdPanel,
                                nullptr, nullptr);

  g_dlg.advBox = CreateWindowExA(0, "BUTTON", "Mostrar opciones avanzadas (diagnostico)",
                                 WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
                                 12, 540, 300, 22, h, (HMENU)(INT_PTR)kIdAdv, nullptr, nullptr);
  SendMessageA(g_dlg.advBox, WM_SETFONT, (WPARAM)f, TRUE);

  HWND note = CreateWindowExA(0, "STATIC",
                              "(*) solo se aplica al arrancar: usa 'Guardar y reiniciar'.",
                              WS_CHILD | WS_VISIBLE, 12, 566, 400, 20, h, nullptr, nullptr,
                              nullptr);
  SendMessageA(note, WM_SETFONT, (WPARAM)f, TRUE);

  struct { int id; const char* t; int x; } btn[] = {
    {kIdApply, "Aplicar", 470}, {kIdSave, "Guardar y reiniciar", 570}, {kIdClose, "Cerrar", 760},
  };
  for(auto& b : btn) {
    HWND bh = CreateWindowExA(0, "BUTTON", b.t, WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                              b.x, 560, b.id == kIdSave ? 180 : 90, 28, h,
                              (HMENU)(INT_PTR)b.id, nullptr, nullptr);
    SendMessageA(bh, WM_SETFONT, (WPARAM)f, TRUE);
  }

  buildPanel();
  ShowWindow(h, SW_SHOW);
  SetForegroundWindow(h);
  SetFocus(g_dlg.list);   // el teclado navega desde la primera pulsacion, sin tener que picar

  MSG msg;
  while(GetMessageA(&msg, nullptr, 0, 0) > 0) {
    if(!IsDialogMessageA(h, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }
  }
}

// ---------------------------------------------------------------- dialogo de mando

struct PadDlg {
  HWND win = nullptr;
  int  port = 0;              // conector que se esta editando (0..3)
  HWND cbPort = nullptr, ckOn = nullptr, cbDev = nullptr, cbAcc = nullptr;
  HWND stHint = nullptr;      // que columna se puede tocar y por que
  std::vector<HWND> keyBtn;
  std::vector<HWND> gpCombo;
  std::vector<int>  devVal;   // fila del combo de aparato -> valor de rt::padDev
  int capture = -1;           // fila esperando una tecla
  int sel = 0;                // control resaltado en el dibujo del mando
  u32 joySeen = 0xffffffffu;  // generacion de la lista de mandos ya volcada al combo
};

PadDlg g_pad;

constexpr int kIdPadKey0 = 2000, kIdPadGp0 = 2100;
constexpr int kIdPadOk = 2300, kIdPadCancel = 2301, kIdPadDefaults = 2302;
constexpr int kIdPadPort = 2303, kIdPadConn = 2304, kIdPadDev = 2305, kIdPadAcc = 2306;

// Nombres GLFW de los botones de mando, en el mismo orden que la tabla de present.cpp.
const char* const kGpNames[] = {
  "", "A", "B", "X", "Y", "LEFT_BUMPER", "RIGHT_BUMPER", "BACK", "START", "GUIDE",
  "LEFT_THUMB", "RIGHT_THUMB", "DPAD_UP", "DPAD_RIGHT", "DPAD_DOWN", "DPAD_LEFT",
  "LEFT_TRIGGER", "RIGHT_TRIGGER",
};
constexpr int kGpCount = (int)(sizeof kGpNames / sizeof kGpNames[0]);

// ------------------------------------------------------- dibujo del mando de Nintendo 64
// El dibujo no es adorno: es la forma de decir QUE se esta asignando sin un parrafo de
// texto. Se pincha el boton en el mando y el dialogo captura la tecla de esa fila.
// Coordenadas en un lienzo fijo de 320x300 pegado en la esquina kArtX/kArtY.

constexpr int kArtX = 12, kArtY = 78;

struct PadSpot {
  const char* id;             // id del control en la tabla de padControls()
  int x, y, w, h;             // rectangulo del lienzo (elipse inscrita si round)
  bool round;
  COLORREF fill;
  COLORREF ink;               // color del rotulo
  const wchar_t* cap;         // rotulo dentro de la figura (Unicode: lleva flechas)
};

// Proporciones sacadas del mando gris de Nintendo 64 visto de frente: cuerpo ancho con
// tres mangos, cruceta arriba a la izquierda, START rojo en el centro, stick analogico
// debajo, B verde y A azul a la derecha y el rombo de cuatro C amarillos a su derecha.
const PadSpot kSpots[] = {
  // Gatillos de arriba, sobresaliendo del borde superior del cuerpo.
  {"L",     34,  16,  86, 24, false, RGB(196, 196, 196), RGB(30, 30, 30),  L"L"},
  {"R",    200,  16,  86, 24, false, RGB(196, 196, 196), RGB(30, 30, 30),  L"R"},
  // Cruceta: cuatro brazos sobre el cubo central.
  {"DU",    50,  58,  24, 32, false, RGB(78, 78, 84),    RGB(235, 235, 235), L"▲"},
  {"DD",    50, 110,  24, 32, false, RGB(78, 78, 84),    RGB(235, 235, 235), L"▼"},
  {"DL",    20,  88,  32, 24, false, RGB(78, 78, 84),    RGB(235, 235, 235), L"◀"},
  {"DR",    72,  88,  32, 24, false, RGB(78, 78, 84),    RGB(235, 235, 235), L"▶"},
  // START, en el centro del cuerpo.
  {"START",143,  55,  34, 34, true,  RGB(196, 36, 36),   RGB(255, 255, 255), L""},
  // Botones de accion.
  {"B",    188,  68,  32, 32, true,  RGB(36, 150, 66),   RGB(255, 255, 255), L"B"},
  {"A",    213,  97,  38, 38, true,  RGB(46, 84, 190),   RGB(255, 255, 255), L"A"},
  // Rombo de botones C.
  {"CU",   265,  40,  22, 22, true,  RGB(242, 186, 32),  RGB(60, 40, 0),   L"▲"},
  {"CD",   265,  92,  22, 22, true,  RGB(242, 186, 32),  RGB(60, 40, 0),   L"▼"},
  {"CL",   241,  66,  22, 22, true,  RGB(242, 186, 32),  RGB(60, 40, 0),   L"◀"},
  {"CR",   289,  66,  22, 22, true,  RGB(242, 186, 32),  RGB(60, 40, 0),   L"▶"},
  // Z, en la cara de atras del mango central.
  {"Z",    140, 232,  40, 42, false, RGB(112, 112, 118), RGB(245, 245, 245), L"Z"},
  // Las cuatro direcciones del stick, entre la cabeza y el anillo.
  {"SY+",  150, 115,  20, 20, true,  RGB(226, 226, 226), RGB(50, 50, 50),  L"▲"},
  {"SY-",  150, 165,  20, 20, true,  RGB(226, 226, 226), RGB(50, 50, 50),  L"▼"},
  {"SX-",  125, 140,  20, 20, true,  RGB(226, 226, 226), RGB(50, 50, 50),  L"◀"},
  {"SX+",  175, 140,  20, 20, true,  RGB(226, 226, 226), RGB(50, 50, 50),  L"▶"},
};
constexpr int kSpotCount = (int)(sizeof kSpots / sizeof kSpots[0]);

auto spotRow(const char* id) -> int {
  for(int i = 0; i < padControlCount(); i++) if(!std::strcmp(padControls()[i].id, id)) return i;
  return -1;
}

// Rotulo serigrafiado (los que van impresos en el plastico, no dentro del boton).
auto padSilk(HDC dc, const wchar_t* s, int x0, int y0, int x1, int y1) -> void {
  RECT r = {kArtX + x0, kArtY + y0, kArtX + x1, kArtY + y1};
  DrawTextW(dc, s, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
}

auto drawPad(HDC dc) -> void {
  auto X = [](int v) { return kArtX + v; };
  auto Y = [](int v) { return kArtY + v; };

  HPEN edge = CreatePen(PS_SOLID, 1, RGB(108, 108, 112));
  HBRUSH body = CreateSolidBrush(RGB(208, 208, 206));
  HBRUSH grip = CreateSolidBrush(RGB(190, 190, 188));
  HBRUSH ring = CreateSolidBrush(RGB(118, 118, 122));
  HBRUSH knob = CreateSolidBrush(RGB(232, 232, 230));
  HGDIOBJ oldPen = SelectObject(dc, edge);
  HGDIOBJ oldBr = SelectObject(dc, grip);

  // Los tres mangos primero: el cuerpo se pinta encima y les tapa el arranque.
  POINT gl[] = {{X(30), Y(150)}, {X(96), Y(150)}, {X(88), Y(286)}, {X(36), Y(258)}};
  POINT gc[] = {{X(128), Y(150)}, {X(192), Y(150)}, {X(184), Y(296)}, {X(136), Y(296)}};
  POINT gr[] = {{X(224), Y(150)}, {X(290), Y(150)}, {X(284), Y(258)}, {X(232), Y(286)}};
  Polygon(dc, gl, 4);
  Polygon(dc, gc, 4);
  Polygon(dc, gr, 4);

  SelectObject(dc, body);
  RoundRect(dc, X(6), Y(40), X(314), Y(172), 56, 56);

  // Cubo de la cruceta, debajo de los cuatro brazos.
  SelectObject(dc, ring);
  RoundRect(dc, X(46), Y(84), X(78), Y(116), 6, 6);

  // Anillo y cabeza del stick analogico.
  Ellipse(dc, X(126), Y(116), X(194), Y(184));
  SelectObject(dc, knob);
  Ellipse(dc, X(143), Y(133), X(177), Y(167));

  SetBkMode(dc, TRANSPARENT);
  HGDIOBJ oldFont = SelectObject(dc, panelFont());

  for(int i = 0; i < kSpotCount; i++) {
    const PadSpot& sp = kSpots[i];
    HBRUSH br = CreateSolidBrush(sp.fill);
    SelectObject(dc, br);
    int x0 = X(sp.x), y0 = Y(sp.y), x1 = X(sp.x + sp.w), y1 = Y(sp.y + sp.h);
    if(sp.round) Ellipse(dc, x0, y0, x1, y1);
    else RoundRect(dc, x0, y0, x1, y1, 8, 8);
    if(*sp.cap) {
      RECT r = {x0, y0, x1, y1};
      SetTextColor(dc, sp.ink);
      DrawTextW(dc, sp.cap, -1, &r, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    SelectObject(dc, body);
    DeleteObject(br);

    // El control elegido en la lista se marca con un aro: asi se sabe siempre a que boton
    // del mando de verdad corresponde la fila que se esta editando.
    if(spotRow(sp.id) == g_pad.sel) {
      HPEN hi = CreatePen(PS_SOLID, 3, RGB(230, 120, 20));
      HGDIOBJ pb = SelectObject(dc, GetStockObject(NULL_BRUSH));
      SelectObject(dc, hi);
      if(sp.round) Ellipse(dc, x0 - 3, y0 - 3, x1 + 3, y1 + 3);
      else RoundRect(dc, x0 - 3, y0 - 3, x1 + 3, y1 + 3, 10, 10);
      SelectObject(dc, pb);
      SelectObject(dc, edge);
      DeleteObject(hi);
    }
  }

  // Serigrafia del plastico, como en el mando de verdad.
  SetTextColor(dc, RGB(70, 70, 70));
  padSilk(dc, L"START", 130, 90, 190, 106);
  padSilk(dc, L"C", 264, 66, 288, 88);
  SetTextColor(dc, RGB(90, 90, 90));
  padSilk(dc, L"Stick analogico", 116, 186, 204, 202);

  SelectObject(dc, oldFont);
  SelectObject(dc, oldBr);
  SelectObject(dc, oldPen);
  DeleteObject(edge);
  DeleteObject(body);
  DeleteObject(grip);
  DeleteObject(ring);
  DeleteObject(knob);
}

// Que control del mando hay bajo el punto (coordenadas de cliente). -1 si ninguno.
auto padHit(int px, int py) -> int {
  for(int i = kSpotCount - 1; i >= 0; i--) {
    const PadSpot& sp = kSpots[i];
    int x0 = kArtX + sp.x, y0 = kArtY + sp.y;
    if(px < x0 || py < y0 || px >= x0 + sp.w || py >= y0 + sp.h) continue;
    if(sp.round) {
      double cx = x0 + sp.w / 2.0, cy = y0 + sp.h / 2.0;
      double dx = (px - cx) / (sp.w / 2.0), dy = (py - cy) / (sp.h / 2.0);
      if(dx * dx + dy * dy > 1.0) continue;
    }
    return spotRow(sp.id);
  }
  return -1;
}

// Codigo virtual de Windows -> nombre de tecla GLFW (el que entiende el fichero de mapeo).
auto vkToGlfwName(WPARAM vk, LPARAM lp) -> std::string {
  if(vk >= 'A' && vk <= 'Z') return std::string(1, (char)vk);
  if(vk >= '0' && vk <= '9') return std::string(1, (char)vk);
  if(vk >= VK_F1 && vk <= VK_F24) {
    char b[8];
    std::snprintf(b, sizeof b, "F%d", (int)(vk - VK_F1 + 1));
    return b;
  }
  if(vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) {
    char b[8];
    std::snprintf(b, sizeof b, "KP_%d", (int)(vk - VK_NUMPAD0));
    return b;
  }
  const bool ext = (lp & (1 << 24)) != 0;   // teclas del bloque extendido
  switch(vk) {
    case VK_SPACE:   return "SPACE";
    case VK_RETURN:  return ext ? "KP_ENTER" : "ENTER";
    case VK_ESCAPE:  return "ESCAPE";
    case VK_TAB:     return "TAB";
    case VK_BACK:    return "BACKSPACE";
    case VK_INSERT:  return "INSERT";
    case VK_DELETE:  return "DELETE";
    case VK_LEFT:    return "LEFT";
    case VK_RIGHT:   return "RIGHT";
    case VK_UP:      return "UP";
    case VK_DOWN:    return "DOWN";
    case VK_PRIOR:   return "PAGE_UP";
    case VK_NEXT:    return "PAGE_DOWN";
    case VK_HOME:    return "HOME";
    case VK_END:     return "END";
    case VK_SHIFT:   return "LEFT_SHIFT";
    case VK_CONTROL: return ext ? "RIGHT_CONTROL" : "LEFT_CONTROL";
    case VK_MENU:    return ext ? "RIGHT_ALT" : "LEFT_ALT";
    case VK_LSHIFT:  return "LEFT_SHIFT";
    case VK_RSHIFT:  return "RIGHT_SHIFT";
    case VK_OEM_COMMA:  return "COMMA";
    case VK_OEM_PERIOD: return "PERIOD";
    case VK_OEM_MINUS:  return "MINUS";
    case VK_OEM_PLUS:   return "EQUAL";
    case VK_OEM_1:      return "SEMICOLON";
    case VK_OEM_2:      return "SLASH";
    case VK_OEM_3:      return "GRAVE_ACCENT";
    case VK_OEM_4:      return "LEFT_BRACKET";
    case VK_OEM_5:      return "BACKSLASH";
    case VK_OEM_6:      return "RIGHT_BRACKET";
    case VK_OEM_7:      return "APOSTROPHE";
    default: break;
  }
  return {};
}

// ------------------------------------------------------------------ aparato del conector
// La lista de mandos enchufados la publica el hilo de video (GLFW solo se puede consultar
// desde ahi). Aqui solo se lee y se vuelca al combo.

auto fillDevCombo() -> void {
  u32 gen = rt::joyGen.load(std::memory_order_acquire);
  std::string cur = padDevice(g_prof, g_pad.port);
  // Un conector recien estrenado no trae aparato elegido. El combo tiene que ensenar algo,
  // asi que se fija el teclado en el perfil en vez de dejar la lista diciendo "Teclado"
  // mientras el nucleo no lee nada de ese puerto.
  if(cur.empty()) { cur = "kb"; setPadDevice(g_prof, g_pad.port, cur); }
  SendMessageA(g_pad.cbDev, CB_RESETCONTENT, 0, 0);
  g_pad.devVal.clear();
  auto add = [&](const char* text, int val) {
    SendMessageA(g_pad.cbDev, CB_ADDSTRING, 0, (LPARAM)text);
    g_pad.devVal.push_back(val);
  };
  add("Teclado", -1);
  // El modo automatico (teclado y el primer mando a la vez) es el de siempre, pero solo
  // tiene sentido en el jugador 1: en los demas el teclado moveria a dos a la vez.
  if(g_pad.port == 0) add("Teclado + primer mando (automatico)", -2);

  int sel = -1;
  {
    std::lock_guard<std::mutex> lk(rt::joyMx);
    for(int j = 0; j < rt::kMaxJoy; j++) {
      if(rt::joyName[j].empty()) continue;
      add(rt::joyName[j].c_str(), j);
      if(cur == rt::joyName[j]) sel = (int)g_pad.devVal.size() - 1;
    }
  }
  if(cur == "kb") sel = 0;
  else if(cur == "auto" && g_pad.port == 0) sel = 1;
  else if(sel < 0 && !cur.empty()) {
    // El mando guardado no esta enchufado ahora mismo: se deja en la lista para no perder
    // la eleccion, y volvera a valer en cuanto aparezca.
    add((cur + "  (no conectado)").c_str(), -3);
    sel = (int)g_pad.devVal.size() - 1;
  }
  SendMessageA(g_pad.cbDev, CB_SETCURSEL, sel < 0 ? 0 : sel, 0);
  g_pad.joySeen = gen;
}

auto padRefresh() -> void {
  const int port = g_pad.port;
  auto& map = g_prof.pad[port];
  bool on = padOn(g_prof, port);
  std::string dev = padDevice(g_prof, port);
  bool kb = on && (dev == "kb" || dev == "auto" || dev.empty());
  bool gp = on && dev != "kb";

  for(int i = 0; i < padControlCount(); i++) {
    auto it = map.find(padControls()[i].id);
    std::string k = it == map.end() ? "" : it->second.first;
    SetWindowTextA(g_pad.keyBtn[i], k.empty() ? "(sin asignar)" : k.c_str());
    std::string g = it == map.end() ? "" : it->second.second;
    int sel = 0;
    for(int n = 0; n < kGpCount; n++) if(g == kGpNames[n]) { sel = n; break; }
    SendMessageA(g_pad.gpCombo[i], CB_SETCURSEL, sel, 0);
    // Solo se deja tocar la columna del aparato con el que juega ese conector: un mapeo de
    // teclado en un puerto que lee un mando no hace nada y confunde.
    EnableWindow(g_pad.keyBtn[i], kb);
    EnableWindow(g_pad.gpCombo[i], gp);
  }
  // Una columna en gris sin explicacion parece un dialogo roto: se dice cual manda.
  const char* hint = !on ? "Puerto desconectado."
               : dev == "kb" ? "Teclado: edita la columna TECLA."
               : (dev == "auto" || dev.empty())
                   ? "Automatico: teclado y primer mando, las dos columnas."
                   : "Mando: edita la columna BOTON DEL MANDO.";
  SetWindowTextA(g_pad.stHint, hint);
  SendMessageA(g_pad.ckOn, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0);
  SendMessageA(g_pad.cbAcc, CB_SETCURSEL, padAcc(g_prof, port), 0);
  EnableWindow(g_pad.cbDev, on);
  EnableWindow(g_pad.cbAcc, on);
  InvalidateRect(g_pad.win, nullptr, TRUE);
}

// Vuelca el perfil al emulador que ya esta corriendo: puertos, accesorios, aparatos y los
// cuatro ficheros de mapeo. El nucleo lo ve en el siguiente fotograma, sin reiniciar.
auto padApply() -> void {
  saveProfile(g_prof);
  for(int q = 0; q < 4; q++) {
    std::string f = writePadFile(g_prof, q);
    char var[20];
    std::snprintf(var, sizeof var, "KESTREL_PAD%d", q + 1);
    // present.cpp lee la ruta del entorno: si el emulador se lanzo sin el lanzador, la
    // variable aun no existe y sin esto el mapeo recien escrito no se cargaria nunca.
    if(!f.empty()) _putenv_s(var, f.c_str());

    rt::padOn[q].store(padOn(g_prof, q), std::memory_order_relaxed);
    rt::padAcc[q].store(padAcc(g_prof, q), std::memory_order_relaxed);

    std::string dev = padDevice(g_prof, q);
    int val = -3;
    if(dev == "kb") val = -1;
    else if(dev == "auto") val = -2;
    else if(!dev.empty()) {
      std::lock_guard<std::mutex> lk(rt::joyMx);
      for(int j = 0; j < rt::kMaxJoy; j++) if(rt::joyName[j] == dev) { val = j; break; }
    }
    rt::padDev[q].store(val, std::memory_order_relaxed);
  }
  rt::padGen.fetch_add(1, std::memory_order_release);
}

LRESULT CALLBACK padProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch(m) {
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC dc = BeginPaint(h, &ps);
      drawPad(dc);
      EndPaint(h, &ps);
      return 0;
    }
    case WM_LBUTTONDOWN: {
      int row = padHit(GET_X_LPARAM(l), GET_Y_LPARAM(l));
      if(row < 0) break;
      std::lock_guard<std::mutex> lk(g_mu);
      g_pad.sel = row;
      // Pinchar el mando dibujado es elegir ese control: si el conector va por teclado se
      // queda esperando la tecla; si va por mando, se abre su lista de botones.
      if(IsWindowEnabled(g_pad.keyBtn[row])) {
        g_pad.capture = row;
        SetWindowTextA(g_pad.keyBtn[row], "pulsa una tecla (Esc = ninguna)");
        SetFocus(h);
      } else if(IsWindowEnabled(g_pad.gpCombo[row])) {
        SetFocus(g_pad.gpCombo[row]);
        SendMessageA(g_pad.gpCombo[row], CB_SHOWDROPDOWN, TRUE, 0);
      }
      InvalidateRect(h, nullptr, TRUE);
      return 0;
    }
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
      if(g_pad.capture < 0) break;
      std::lock_guard<std::mutex> lk(g_mu);
      int row = g_pad.capture;
      g_pad.capture = -1;
      std::string name = w == VK_ESCAPE ? std::string() : vkToGlfwName(w, l);
      const char* id = padControls()[row].id;
      auto& e = g_prof.pad[g_pad.port][id];
      // Escape = desasignar a proposito; una tecla que no esta en la tabla de GLFW se
      // rechaza en vez de guardarse como basura que luego no responderia.
      if(w == VK_ESCAPE) e.first.clear();
      else if(!name.empty()) e.first = name;
      else MessageBeep(MB_ICONWARNING);   // tecla sin nombre en GLFW: no se puede mapear
      padRefresh();
      return 0;
    }
    case WM_COMMAND: {
      std::lock_guard<std::mutex> lk(g_mu);
      int id = LOWORD(w);
      if(id >= kIdPadKey0 && id < kIdPadKey0 + padControlCount()) {
        g_pad.capture = id - kIdPadKey0;
        g_pad.sel = g_pad.capture;
        SetWindowTextA(g_pad.keyBtn[g_pad.capture], "pulsa una tecla (Esc = ninguna)");
        SetFocus(h);
        InvalidateRect(h, nullptr, TRUE);
        return 0;
      }
      if(id >= kIdPadGp0 && id < kIdPadGp0 + padControlCount() && HIWORD(w) == CBN_SELCHANGE) {
        int row = id - kIdPadGp0;
        LRESULT sel = SendMessageA(g_pad.gpCombo[row], CB_GETCURSEL, 0, 0);
        if(sel >= 0 && sel < kGpCount)
          g_prof.pad[g_pad.port][padControls()[row].id].second = kGpNames[sel];
        g_pad.sel = row;
        InvalidateRect(h, nullptr, TRUE);
        return 0;
      }
      if(id == kIdPadPort && HIWORD(w) == CBN_SELCHANGE) {
        LRESULT sel = SendMessageA(g_pad.cbPort, CB_GETCURSEL, 0, 0);
        if(sel >= 0 && sel < 4) {
          g_pad.port = (int)sel;
          g_pad.capture = -1;
          fillDevCombo();
          padRefresh();
        }
        return 0;
      }
      if(id == kIdPadConn) {
        bool on = SendMessageA(g_pad.ckOn, BM_GETCHECK, 0, 0) == BST_CHECKED;
        setPadOn(g_prof, g_pad.port, on);
        padRefresh();
        return 0;
      }
      if(id == kIdPadDev && HIWORD(w) == CBN_SELCHANGE) {
        LRESULT sel = SendMessageA(g_pad.cbDev, CB_GETCURSEL, 0, 0);
        if(sel >= 0 && sel < (LRESULT)g_pad.devVal.size()) {
          int val = g_pad.devVal[(size_t)sel];
          // Se guarda el NOMBRE del mando, no su numero: Windows renumera al enchufar y
          // desenchufar, y un numero acabaria apuntando al mando de otro jugador.
          std::string dev = val == -1 ? "kb" : val == -2 ? "auto" : std::string();
          if(val >= 0) {
            std::lock_guard<std::mutex> lk(rt::joyMx);
            dev = rt::joyName[val];
          } else if(val == -3) {
            dev = padDevice(g_prof, g_pad.port);   // el guardado, que sigue sin aparecer
          }
          setPadDevice(g_prof, g_pad.port, dev);
          padRefresh();
        }
        return 0;
      }
      if(id == kIdPadAcc && HIWORD(w) == CBN_SELCHANGE) {
        LRESULT sel = SendMessageA(g_pad.cbAcc, CB_GETCURSEL, 0, 0);
        if(sel >= 0 && sel <= 2) setPadAcc(g_prof, g_pad.port, (int)sel);
        return 0;
      }
      if(id == kIdPadDefaults) {
        Profile d = defaultProfile();
        g_prof.pad[g_pad.port] = d.pad[g_pad.port];
        padRefresh();
        return 0;
      }
      if(id == kIdPadOk) {
        // El emulador relee los mapeos cuando sube la generacion: los mandos quedan
        // reasignados sin salir de la partida.
        padApply();
        DestroyWindow(h);
        return 0;
      }
      if(id == kIdPadCancel) { DestroyWindow(h); return 0; }
      return 0;
    }
    case WM_TIMER:
      // Enchufar o quitar un mando con el dialogo abierto tiene que verse en la lista de
      // aparatos sin cerrarlo.
      if(rt::joyGen.load(std::memory_order_acquire) != g_pad.joySeen) {
        std::lock_guard<std::mutex> lk(g_mu);
        fillDevCombo();
      }
      return 0;
    case WM_CLOSE:
      DestroyWindow(h);
      return 0;
    case WM_DESTROY:
      KillTimer(h, 1);
      g_pad = PadDlg();
      g_padWin = nullptr;
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcA(h, m, w, l);
}

auto padDialogThread() -> void {
  static bool cls = false;
  if(!cls) {
    WNDCLASSA wc = {};
    wc.lpfnWndProc = padProc;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = "kestrel64_pad";
    RegisterClassA(&wc);
    cls = true;
  }
  g_pad = PadDlg();
  const int n = padControlCount();
  const int listX = 350, rowY0 = 92, rowH = 26;
  const int cliW = 760, cliH = rowY0 + n * rowH + 56;
  RECT want = {0, 0, cliW, cliH};
  const DWORD style = WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME;
  AdjustWindowRect(&want, style, FALSE);
  HWND h = CreateWindowExA(0, "kestrel64_pad", "kestrel64 - mandos", style,
                           CW_USEDEFAULT, CW_USEDEFAULT,
                           want.right - want.left, want.bottom - want.top,
                           nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
  if(!h) return;
  g_padWin = h;
  g_pad.win = h;
  HFONT f = panelFont();
  auto mk = [&](const char* cl, const char* txt, DWORD st, int x, int y, int w2, int h2,
                int id) {
    HWND c = CreateWindowExA(0, cl, txt, WS_CHILD | WS_VISIBLE | st, x, y, w2, h2, h,
                             (HMENU)(INT_PTR)id, nullptr, nullptr);
    SendMessageA(c, WM_SETFONT, (WPARAM)f, TRUE);
    return c;
  };

  // Cabecera: que conector, si esta enchufado, con que se juega y que lleva en la ranura.
  mk("STATIC", "Mando:", 0, 12, 14, 46, 18, 0);
  g_pad.cbPort = mk("COMBOBOX", "", WS_TABSTOP | CBS_DROPDOWNLIST, 62, 10, 90, 200, kIdPadPort);
  for(int q = 1; q <= 4; q++) {
    char t[16];
    std::snprintf(t, sizeof t, "Mando %d", q);
    SendMessageA(g_pad.cbPort, CB_ADDSTRING, 0, (LPARAM)t);
  }
  SendMessageA(g_pad.cbPort, CB_SETCURSEL, 0, 0);
  g_pad.ckOn = mk("BUTTON", "Conectado", WS_TABSTOP | BS_AUTOCHECKBOX, 166, 12, 96, 20,
                  kIdPadConn);
  mk("STATIC", "Accesorio:", 0, 276, 14, 66, 18, 0);
  g_pad.cbAcc = mk("COMBOBOX", "", WS_TABSTOP | CBS_DROPDOWNLIST, 346, 10, 190, 200, kIdPadAcc);
  for(const char* a : {"Ninguno", "Controller Pak", "Rumble Pak"})
    SendMessageA(g_pad.cbAcc, CB_ADDSTRING, 0, (LPARAM)a);
  mk("STATIC", "Aparato:", 0, 12, 46, 46, 18, 0);
  g_pad.cbDev = mk("COMBOBOX", "", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, 62, 42, 474,
                   300, kIdPadDev);

  // Cabecera de las dos columnas: sin esto no se ve que la de la derecha es editable.
  mk("STATIC", "Control", 0, listX, rowY0 - 20, 110, 18, 0);
  mk("STATIC", "TECLA", 0, listX + 116, rowY0 - 20, 140, 18, 0);
  mk("STATIC", "BOTON DEL MANDO", 0, listX + 262, rowY0 - 20, 134, 18, 0);

  for(int i = 0; i < n; i++) {
    int y = rowY0 + i * rowH;
    mk("STATIC", padControls()[i].label, 0, listX, y + 4, 110, 18, 0);
    g_pad.keyBtn.push_back(mk("BUTTON", "", WS_TABSTOP, listX + 116, y, 140, 22,
                              kIdPadKey0 + i));
    HWND cb = mk("COMBOBOX", "", WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL, listX + 262, y,
                 134, 300, kIdPadGp0 + i);
    for(int k = 0; k < kGpCount; k++)
      SendMessageA(cb, CB_ADDSTRING, 0, (LPARAM)(*kGpNames[k] ? kGpNames[k] : "(ninguno)"));
    g_pad.gpCombo.push_back(cb);
  }
  int by = rowY0 + n * rowH + 12;
  mk("BUTTON", "Valores de fabrica", WS_TABSTOP, 12, by, 150, 28, kIdPadDefaults);
  g_pad.stHint = mk("STATIC", "", 0, 172, by + 5, 290, 18, 0);
  mk("BUTTON", "Aplicar y cerrar", WS_TABSTOP, 470, by, 140, 28, kIdPadOk);
  mk("BUTTON", "Cancelar", WS_TABSTOP, 620, by, 100, 28, kIdPadCancel);

  fillDevCombo();
  padRefresh();
  SetTimer(h, 1, 500, nullptr);
  ShowWindow(h, SW_SHOW);
  SetForegroundWindow(h);

  MSG msg;
  while(GetMessageA(&msg, nullptr, 0, 0) > 0) {
    // Mientras se captura una tecla NO se deja que el gestor de dialogos se coma Tab, Enter
    // o Escape: son teclas asignables como cualquier otra.
    if(g_pad.capture >= 0 || !IsDialogMessageA(h, &msg)) {
      TranslateMessage(&msg);
      DispatchMessageA(&msg);
    }
  }
}

// ---------------------------------------------------------------- ordenes del menu

auto pickRom(HWND owner) -> std::string {
  char file[MAX_PATH] = {0};
  OPENFILENAMEA ofn = {};
  ofn.lStructSize = sizeof ofn;
  ofn.hwndOwner = owner;
  static const char kFilter[] =
      "ROM de Nintendo 64\0*.z64;*.n64;*.v64;*.zip;*.gz\0Todos los archivos\0*.*\0";
  ofn.lpstrFilter = kFilter;
  ofn.lpstrTitle = "Elige una ROM de Nintendo 64";
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  return GetOpenFileNameA(&ofn) ? std::string(file) : std::string();
}

auto onCommand(UINT id) -> void {
  std::lock_guard<std::mutex> lk(g_mu);
  switch(id) {
    case kOpenRom: {
      // La biblioteca monta su ventana y su propio bucle de mensajes, asi que va en un hilo
      // aparte: este es el hilo de la ventana del juego, que esta presentando cuadros y no
      // puede meterse en un bucle modal sin congelar la imagen. El hilo se queda con el
      // resto del trabajo y vuelve a coger el candado cuando el usuario ya ha elegido.
      if(libraryOpen()) return;
      std::thread([] {
        std::string rom = pickRomLibrary(g_game);
        if(rom.empty()) return;
        std::lock_guard<std::mutex> lk(g_mu);
        g_prof.set("rom", rom);
        saveProfile(g_prof);
        g_h.rom = rom;
        askRelaunch(g_game, ("ROM elegida:\n" + rom).c_str());
      }).detach();
      return;
    }
    // El dialogo de fichero de siempre, para una ROM que este fuera de la biblioteca.
    case kOpenFile: {
      std::string rom = pickRom(g_game);
      if(rom.empty()) return;
      g_prof.set("rom", rom);
      saveProfile(g_prof);
      g_h.rom = rom;
      askRelaunch(g_game, ("ROM elegida:\n" + rom).c_str());
      return;
    }
    case kRelaunch:
      askRelaunch(g_game, "Reiniciar el emulador con la configuracion guardada.");
      return;
    case kQuit:
      closeDialogs();
      if(g_h.shutdown) g_h.shutdown->store(true, std::memory_order_release);
      PostMessageA(g_game, WM_CLOSE, 0, 0);
      return;
    case kPause:
      if(g_h.paused) {
        bool now = !g_h.paused->load();
        g_h.paused->store(now);
      }
      syncMenu();
      return;
    case kSaveState:
      if(g_h.stSave && g_h.stSlot) g_h.stSave->store(g_h.stSlot->load());
      return;
    case kLoadState:
      if(g_h.stLoad && g_h.stSlot) g_h.stLoad->store(g_h.stSlot->load());
      return;
    case kNextSlot:
      if(g_h.stSlot) g_h.stSlot->store((g_h.stSlot->load() + 1) % 10);
      return;
    case kFull: {
      bool now = !g_prof.getBool("fullscreen");
      g_prof.set("fullscreen", now ? "1" : "0");
      saveProfile(g_prof);
      applyLive(g_prof, true);
      syncMenuVisibility();
      syncMenu();
      return;
    }
    case kHud:
      rt::hud.store(!rt::hud.load());
      g_prof.set("hud", rt::hud.load() ? "1" : "0");
      saveProfile(g_prof);
      syncMenu();
      return;
    case kAudioOn:
      rt::audioOn.store(!rt::audioOn.load());
      g_prof.set("audio", rt::audioOn.load() ? "1" : "0");
      saveProfile(g_prof);
      syncMenu();
      return;
    case kPadCfg:
      if(g_padWin) { SetForegroundWindow(g_padWin); return; }
      std::thread(padDialogThread).detach();
      return;
    case kAllOpts:
      if(g_optWin) { SetForegroundWindow(g_optWin); return; }
      std::thread(optionsDialogThread).detach();
      return;
    case kAbout:
      MessageBoxA(g_game,
                  "kestrel64 - emulador de Nintendo 64 escrito desde cero.\n\n"
                  "Teclado de fabrica: X=A, C=B, Espacio=Z, Enter=Start, Q/E=L/R,\n"
                  "flechas=cruceta, I/J/K/L=botones C, W/A/S/D=stick.\n"
                  "F5 guarda estado, F7 carga, F6 cambia de ranura, F11 pantalla completa.",
                  "Acerca de kestrel64", MB_OK | MB_ICONINFORMATION);
      return;
    default: break;
  }
  if(id >= kScale1 && id < kScale1 + 7) {
    int n = (int)(id - kScale1) + 1;
    g_prof.set("winscale", std::to_string(n));
    g_prof.set("winsize", "");            // la escala manda: se limpia el tamano exacto
    g_prof.set("fullscreen", "0");
    saveProfile(g_prof);
    applyLive(g_prof, true);
    syncMenu();
    return;
  }
  if(id >= kVol0 && id <= kVol0 + 100) {
    int v = (int)(id - kVol0);
    rt::volume.store(v);
    g_prof.set("volume", std::to_string(v));
    saveProfile(g_prof);
    syncMenu();
    return;
  }
  if(id >= kSlot0 && id <= kSlot0 + 9) {
    if(g_h.stSlot) g_h.stSlot->store((int)(id - kSlot0));
    syncMenu();
    return;
  }
  if(id == kThrHw) {
    // Fiel a consola. El limitador se puede poner en caliente, pero los relojes y el CPI se
    // montan al arrancar: si el perfil traia overclock hay que relanzar para que el modo
    // signifique de verdad "velocidad del N64 real" y no "59.94 Hz con la CPU dopada".
    const bool oc = ocNotStock(g_prof);
    g_prof.set("speedmode", "hw");
    g_prof.set("throttle", "auto");
    rt::throttle.store(-1);      // automatico = limitar solo si hay ventana, y aqui la hay
    saveProfile(g_prof);
    syncMenu();
    if(oc) askRelaunch(g_game, "Modo fiel a consola: los multiplicadores de reloj quedan "
                               "anulados (1.00x).");
    return;
  }
  if(id >= kThrAuto && id <= kThrOff) {
    const char* val = id == kThrAuto ? "auto" : id == kThrOn ? "1" : "0";
    const bool wasHw = g_prof.get("speedmode") == "hw";
    g_prof.set("speedmode", "libre");
    g_prof.set("throttle", val);
    rt::throttle.store(id == kThrAuto ? -1 : id == kThrOn ? 1 : 0);
    saveProfile(g_prof);
    syncMenu();
    // Salir del modo fiel devuelve los multiplicadores guardados, y eso solo se lee al
    // arrancar. Sin overclock guardado no hay nada que devolver: no se molesta al usuario.
    if(wasHw && ocNotStock(g_prof))
      askRelaunch(g_game, "Se sale del modo fiel a consola: vuelven los multiplicadores de "
                          "reloj guardados en el perfil.");
    return;
  }
}

LRESULT CALLBACK gameProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch(m) {
    case WM_COMMAND:
      // Solo los identificadores propios: el resto (controles hijos de GLFW, si los hubiera)
      // sigue su camino de siempre.
      if(HIWORD(w) == 0 && LOWORD(w) >= 0x8100) { onCommand(LOWORD(w)); return 0; }
      break;
    case WM_KEYDOWN:
      // Atajos, y DESPUES se deja pasar el mensaje: el teclado del juego se lee con
      // glfwGetKey, que se alimenta de estos mismos WM_KEYDOWN/WM_KEYUP. Tragarse el
      // KEYDOWN y no el KEYUP dejaria la tecla clavada en el estado de GLFW.
      if(!(l & (1 << 30))) {   // bit 30 = repeticion automatica; solo el flanco
        const bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        if(w == VK_F12) onCommand(kAllOpts);
        else if(w == VK_F11) onCommand(kFull);
        else if(ctrl && w == 'O') onCommand(kOpenRom);
        else if(ctrl && w == 'P') onCommand(kPause);
      }
      break;
    case WM_INITMENUPOPUP: {
      // g_prof lo escriben tambien los hilos de los dialogos.
      std::lock_guard<std::mutex> lk(g_mu);
      syncMenu();
      break;
    }
    case kMsgSync: {
      std::lock_guard<std::mutex> lk(g_mu);
      syncMenuVisibility();
      syncMenu();
      return 0;
    }
  }
  return CallWindowProcW(g_prev, h, m, w, l);
}

}  // namespace

auto attach(void* hwnd, const Hooks& h) -> void {
  if(!hwnd) return;
  std::lock_guard<std::mutex> lk(g_mu);
  g_game = (HWND)hwnd;
  g_h = h;
  g_prof = loadProfile();
  if(!g_prof.v.count("rom") || g_prof.get("rom").empty()) g_prof.set("rom", h.rom);

  // El perfil es lo que el lanzador uso para arrancar esto, pero el usuario ha podido
  // arrancar a mano con otras variables. Lo que YA esta puesto en marcha manda sobre lo que
  // dice el fichero, o el menu ensenaria marcas que no se corresponden con la imagen.
  g_prof.set("hud", rt::hud.load() ? "1" : "0");
  g_prof.set("audio", rt::audioOn.load() ? "1" : "0");
  g_prof.set("volume", std::to_string(rt::volume.load()));
  {
    int th = rt::throttle.load();
    g_prof.set("throttle", th < 0 ? "auto" : th ? "1" : "0");
  }
  if(const char* sc = std::getenv("KESTREL_WINSCALE")) g_prof.set("winscale", sc);
  if(const char* ws = std::getenv("KESTREL_WINSIZE")) g_prof.set("winsize", ws);
  if(const char* fs = std::getenv("KESTREL_FULLSCREEN"))
    g_prof.set("fullscreen", fs[0] == '0' ? "0" : "1");

  buildMenu();
  SetMenu(g_game, g_menu);
  // Poner la barra encoge el area de cliente: se agranda la ventana justo lo que ocupa, o la
  // imagen del juego perderia una franja por abajo. En pantalla completa no: ahi la ventana
  // ya cubre el monitor y crecerla la sacaria de sitio.
  if(h.resizeForMenu) {
    RECT rc;
    GetWindowRect(g_game, &rc);
    SetWindowPos(g_game, nullptr, 0, 0, rc.right - rc.left,
                 rc.bottom - rc.top + GetSystemMetrics(SM_CYMENU),
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  }

  // Version W a proposito: la ventana de GLFW se registro en Unicode, y engancharla con la
  // A la convertiria en ANSI (WM_CHAR llegaria mutilado a GLFW).
  g_prev = (WNDPROC)SetWindowLongPtrW(g_game, GWLP_WNDPROC, (LONG_PTR)gameProc);
  syncMenu();
}

auto relaunchPending() -> bool { return g_relaunch; }

auto doRelaunch() -> void {
  if(!g_relaunch) return;
  g_relaunch = false;
  // Los dialogos ya han recibido su WM_CLOSE; se les da un momento para morir antes de
  // arrancar el proceso nuevo y salir. Con tope: si uno se atasca no se cuelga el cierre.
  for(int i = 0; i < 100 && (g_optWin || g_padWin); i++) Sleep(10);
  relaunchNow();
}

}  // namespace kestrel::ui

#else   // !_WIN32

namespace kestrel::ui {
auto attach(void*, const Hooks&) -> void {}
auto relaunchPending() -> bool { return false; }
auto doRelaunch() -> void {}
}  // namespace kestrel::ui

#endif
