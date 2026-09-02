#include "menu.hpp"
#include "optdefs.hpp"
#include "profile.hpp"
#include "../core/runtime.hpp"

#ifdef _WIN32
#include <windows.h>
#include <cstdio>
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
  kOpenRom = 0x8100, kRelaunch, kQuit,
  kPause, kReset, kSaveState, kLoadState, kNextSlot,
  kFull, kHud,
  kAudioOn, kPadCfg, kAllOpts, kAbout,
  kScale1  = 0x8140,              // +0..+6 (escala 1x..7x)
  kThrAuto = 0x8160, kThrOn, kThrOff,
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

// Aplica al vuelo lo que se puede aplicar al vuelo. El resto del perfil ya esta guardado y
// solo se hace efectivo relanzando.
auto applyLive(const Profile& p, bool windowToo) -> void {
  rt::hud.store(p.getBool("hud"));
  rt::audioOn.store(p.getBool("audio"));
  int vol = std::atoi(p.get("volume").c_str());
  rt::volume.store(vol < 0 ? 0 : vol > 100 ? 100 : vol);
  std::string th = p.get("throttle");
  rt::throttle.store(th == "1" ? 1 : th == "0" ? 0 : -1);
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
  addItem(file, kOpenRom, "&Abrir ROM...\tCtrl+O");
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
  CheckMenuRadioItem(g_menu, kThrAuto, kThrOff,
                     th < 0 ? kThrAuto : th ? kThrOn : kThrOff, MF_BYCOMMAND);
  DrawMenuBar(g_game);
}

// ---------------------------------------------------------------- relanzar

// Reconstruye la linea de ordenes y el entorno desde el perfil y arranca una copia nueva del
// emulador. Se llama SIEMPRE desde main(), con el audio y el video ya cerrados.
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

  std::string cmd = "\"" + exePath() + "\"";
  std::string rom = g_prof.get("rom");
  if(rom.empty()) rom = g_h.rom;
  if(!rom.empty()) cmd += " \"" + rom + "\"";
  for(const auto& a : argv) cmd += " " + a;

  STARTUPINFOA si = {};
  si.cb = sizeof si;
  PROCESS_INFORMATION pi = {};
  std::vector<char> line(cmd.begin(), cmd.end());
  line.push_back(0);
  if(CreateProcessA(nullptr, line.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr,
                    &si, &pi)) {
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
  char out[64];
  if(o.type == OType::Int) std::snprintf(out, sizeof out, "%lld", (long long)(x < 0 ? x - 0.5 : x + 0.5));
  else {
    std::snprintf(out, sizeof out, "%.4f", x);
    std::string t(out);
    while(t.size() > 1 && t.back() == '0') t.pop_back();
    if(!t.empty() && t.back() == '.') t.pop_back();
    return t;
  }
  return out;
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
        r.ctl = CreateWindowExA(WS_EX_CLIENTEDGE, "EDIT", g_prof.get(o.id).c_str(),
                                WS_CHILD | WS_VISIBLE | WS_TABSTOP | ES_AUTOHSCROLL,
                                316, y, o.type == OType::Path ? 260 : 300, 22, g_dlg.panel,
                                (HMENU)(INT_PTR)(kIdCtl0 + (int)g_dlg.rows.size()),
                                nullptr, nullptr);
      }
    }
    if(r.ctl) SendMessageA(r.ctl, WM_SETFONT, (WPARAM)f, TRUE);
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
  std::vector<HWND> keyBtn;
  std::vector<HWND> gpCombo;
  int capture = -1;   // fila esperando una tecla
};

PadDlg g_pad;

constexpr int kIdPadKey0 = 2000, kIdPadGp0 = 2100;
constexpr int kIdPadOk = 2300, kIdPadCancel = 2301, kIdPadDefaults = 2302;

// Nombres GLFW de los botones de mando, en el mismo orden que la tabla de present.cpp.
const char* const kGpNames[] = {
  "", "A", "B", "X", "Y", "LEFT_BUMPER", "RIGHT_BUMPER", "BACK", "START", "GUIDE",
  "LEFT_THUMB", "RIGHT_THUMB", "DPAD_UP", "DPAD_RIGHT", "DPAD_DOWN", "DPAD_LEFT",
  "LEFT_TRIGGER", "RIGHT_TRIGGER",
};
constexpr int kGpCount = (int)(sizeof kGpNames / sizeof kGpNames[0]);

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

auto padRefresh() -> void {
  for(int i = 0; i < padControlCount(); i++) {
    auto it = g_prof.pad.find(padControls()[i].id);
    std::string k = it == g_prof.pad.end() ? "" : it->second.first;
    SetWindowTextA(g_pad.keyBtn[i], k.empty() ? "(sin asignar)" : k.c_str());
    std::string g = it == g_prof.pad.end() ? "" : it->second.second;
    int sel = 0;
    for(int n = 0; n < kGpCount; n++) if(g == kGpNames[n]) { sel = n; break; }
    SendMessageA(g_pad.gpCombo[i], CB_SETCURSEL, sel, 0);
  }
}

LRESULT CALLBACK padProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch(m) {
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
      if(g_pad.capture < 0) break;
      std::lock_guard<std::mutex> lk(g_mu);
      int row = g_pad.capture;
      g_pad.capture = -1;
      std::string name = w == VK_ESCAPE ? std::string() : vkToGlfwName(w, l);
      const char* id = padControls()[row].id;
      auto& e = g_prof.pad[id];
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
        SetWindowTextA(g_pad.keyBtn[g_pad.capture], "pulsa una tecla (Esc = ninguna)");
        SetFocus(h);
        return 0;
      }
      if(id >= kIdPadGp0 && id < kIdPadGp0 + padControlCount() && HIWORD(w) == CBN_SELCHANGE) {
        int row = id - kIdPadGp0;
        LRESULT sel = SendMessageA(g_pad.gpCombo[row], CB_GETCURSEL, 0, 0);
        if(sel >= 0 && sel < kGpCount) g_prof.pad[padControls()[row].id].second = kGpNames[sel];
        return 0;
      }
      if(id == kIdPadDefaults) {
        Profile d = defaultProfile();
        g_prof.pad = d.pad;
        padRefresh();
        return 0;
      }
      if(id == kIdPadOk) {
        saveProfile(g_prof);
        writePadFile(g_prof);
        // El emulador relee el fichero cuando sube la generacion: el mando queda reasignado
        // sin salir de la partida.
        rt::padGen.fetch_add(1, std::memory_order_release);
        DestroyWindow(h);
        return 0;
      }
      if(id == kIdPadCancel) { DestroyWindow(h); return 0; }
      return 0;
    }
    case WM_CLOSE:
      DestroyWindow(h);
      return 0;
    case WM_DESTROY:
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
  HWND h = CreateWindowExA(0, "kestrel64_pad", "kestrel64 - mando 1",
                           WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME,
                           CW_USEDEFAULT, CW_USEDEFAULT, 560, 70 + n * 28 + 70,
                           nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
  if(!h) return;
  g_padWin = h;
  g_pad.win = h;
  HFONT f = panelFont();

  HWND hdr = CreateWindowExA(0, "STATIC",
                             "Pulsa el boton de la columna TECLA y luego la tecla que quieras."
                             " Esc la deja sin asignar.",
                             WS_CHILD | WS_VISIBLE, 12, 8, 520, 20, h, nullptr, nullptr,
                             nullptr);
  SendMessageA(hdr, WM_SETFONT, (WPARAM)f, TRUE);

  for(int i = 0; i < n; i++) {
    int y = 36 + i * 28;
    HWND lb = CreateWindowExA(0, "STATIC", padControls()[i].label, WS_CHILD | WS_VISIBLE,
                              12, y + 4, 150, 18, h, nullptr, nullptr, nullptr);
    SendMessageA(lb, WM_SETFONT, (WPARAM)f, TRUE);
    HWND kb = CreateWindowExA(0, "BUTTON", "", WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                              168, y, 180, 24, h, (HMENU)(INT_PTR)(kIdPadKey0 + i),
                              nullptr, nullptr);
    SendMessageA(kb, WM_SETFONT, (WPARAM)f, TRUE);
    HWND cb = CreateWindowExA(0, "COMBOBOX", "",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST | WS_VSCROLL,
                              356, y, 170, 300, h, (HMENU)(INT_PTR)(kIdPadGp0 + i),
                              nullptr, nullptr);
    SendMessageA(cb, WM_SETFONT, (WPARAM)f, TRUE);
    for(int k = 0; k < kGpCount; k++)
      SendMessageA(cb, CB_ADDSTRING, 0, (LPARAM)(*kGpNames[k] ? kGpNames[k] : "(ninguno)"));
    g_pad.keyBtn.push_back(kb);
    g_pad.gpCombo.push_back(cb);
  }
  int by = 44 + n * 28;
  struct { int id; const char* t; int x; int w; } btn[] = {
    {kIdPadDefaults, "Valores de fabrica", 12, 150},
    {kIdPadOk, "Aplicar y cerrar", 300, 130},
    {kIdPadCancel, "Cancelar", 440, 90},
  };
  for(auto& b : btn) {
    HWND bh = CreateWindowExA(0, "BUTTON", b.t, WS_CHILD | WS_VISIBLE | WS_TABSTOP,
                              b.x, by, b.w, 28, h, (HMENU)(INT_PTR)b.id, nullptr, nullptr);
    SendMessageA(bh, WM_SETFONT, (WPARAM)f, TRUE);
  }
  padRefresh();
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
      "ROM de Nintendo 64\0*.z64;*.n64;*.v64\0Todos los archivos\0*.*\0";
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
  if(id >= kThrAuto && id <= kThrOff) {
    const char* val = id == kThrAuto ? "auto" : id == kThrOn ? "1" : "0";
    g_prof.set("throttle", val);
    rt::throttle.store(id == kThrAuto ? -1 : id == kThrOn ? 1 : 0);
    saveProfile(g_prof);
    syncMenu();
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
