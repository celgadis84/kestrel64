// kestrel64 -- ventana de la ficha de juego (Win32 puro). La logica esta en gamecard.cpp.
//
// Cuatro pestanas, como la ficha del lanzador: Info (cabecera y datos editables), Manual,
// Trucos (el .cht de la ROM) y Partidas (lo que hay guardado al lado del cartucho). Las
// pestanas son botones que muestran u ocultan su grupo de controles: no hace falta un
// control de pestanas de comctl32 para cuatro paginas.
//
// Todo en la version W de la API: las notas y los nombres que escribe el lanzador son UTF-8
// y una caja de texto ANSI los romperia. Las rutas siguen en la pagina de codigos ANSI, que
// es como las maneja el resto del emulador.

#include "gamecard.hpp"

#ifdef _WIN32
#include <windows.h>
#include <commdlg.h>
#include <shellapi.h>
#include <urlmon.h>
#include <atomic>
#include <cstdio>
#include <ctime>
#include <string>
#include <vector>

namespace kestrel::ui::card {
namespace {

enum : int {
  kTab0 = 100,   // 100..103
  kPlay = 110, kClose,
  kInfoSave = 120, kFav,
  kManList = 130, kManOpen, kManAdd, kManDir, kManSrc, kManGet,
  kChtList = 140, kChtToggle, kChtDel, kChtAdd, kChtName, kChtCodes,
  kSavList = 150, kSavDir,
  kEdit0 = 160,   // 160.. campos de Info
};

const char* const kFields[] = {"year", "developer", "publisher", "genre", "players"};
const wchar_t* const kFieldLabels[] = {L"A\u00f1o", L"Desarrolla", L"Edita", L"G\u00e9nero",
                                       L"Jugadores"};
constexpr int kNF = 5;

struct Dlg {
  HWND win = nullptr;
  std::string rom;
  Header hdr;
  std::string key;
  bool canPlay = false, play = false;
  int tab = 0;
  std::vector<HWND> page[4];
  HWND tabBtn[4] = {};
  HWND edit[kNF] = {}, notes = nullptr, fav = nullptr, stats = nullptr, infoMsg = nullptr;
  HWND manList = nullptr, manSrc = nullptr, manMsg = nullptr, chtList = nullptr, chtName = nullptr, chtCodes = nullptr,
       chtMsg = nullptr, savList = nullptr;
  std::vector<std::string> man;
  HFONT font = nullptr, big = nullptr;
};
Dlg d;
std::atomic<bool> g_open{false};
std::atomic<HWND> g_win{nullptr};

auto wide(const std::string& s, UINT cp) -> std::wstring {
  if(s.empty()) return {};
  int n = MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), nullptr, 0);
  std::wstring w((size_t)n, L'\0');
  MultiByteToWideChar(cp, 0, s.data(), (int)s.size(), &w[0], n);
  return w;
}
auto narrow(const std::wstring& w, UINT cp) -> std::string {
  if(w.empty()) return {};
  int n = WideCharToMultiByte(cp, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
  std::string s((size_t)n, '\0');
  WideCharToMultiByte(cp, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
  return s;
}
auto u8w(const std::string& s) -> std::wstring { return wide(s, CP_UTF8); }
auto aw(const std::string& s) -> std::wstring { return wide(s, CP_ACP); }

auto textOf(HWND h) -> std::wstring {
  int n = GetWindowTextLengthW(h);
  std::wstring w((size_t)n + 1, L'\0');
  GetWindowTextW(h, &w[0], n + 1);
  w.resize((size_t)n);
  return w;
}

auto mk(const wchar_t* cl, const std::wstring& txt, DWORD st, int x, int y, int w, int h, int id,
        int pg) -> HWND {
  HWND c = CreateWindowExW(cl == std::wstring(L"EDIT") || cl == std::wstring(L"LISTBOX")
                               ? WS_EX_CLIENTEDGE : 0,
                           cl, txt.c_str(), WS_CHILD | st | (pg <= 0 ? WS_VISIBLE : 0), x, y, w, h,
                           d.win, (HMENU)(INT_PTR)id, nullptr, nullptr);
  SendMessageW(c, WM_SETFONT, (WPARAM)d.font, TRUE);
  if(pg >= 0) d.page[pg].push_back(c);
  return c;
}

auto fmtSize(unsigned long long b) -> std::string {
  char t[32];
  if(b >= 1048576) std::snprintf(t, sizeof t, "%.1f MB", b / 1048576.0);
  else if(b >= 1024) std::snprintf(t, sizeof t, "%.1f KB", b / 1024.0);
  else std::snprintf(t, sizeof t, "%llu B", b);
  return t;
}

auto fmtTime(long long t) -> std::string {
  if(t <= 0) return "-";
  time_t tt = (time_t)t;
  struct tm lt;
  localtime_s(&lt, &tt);
  char b[32];
  std::strftime(b, sizeof b, "%d/%m/%Y %H:%M", &lt);
  return b;
}

auto showTab(int t) -> void {
  d.tab = t;
  for(int p = 0; p < 4; p++) {
    for(HWND c : d.page[p]) ShowWindow(c, p == t ? SW_SHOW : SW_HIDE);
    // La pestana activa se ve "pulsada": un boton de radio con aspecto de boton.
    SendMessageW(d.tabBtn[p], BM_SETCHECK, p == t ? BST_CHECKED : BST_UNCHECKED, 0);
  }
}

// ---------------------------------------------------------------- relleno de paginas
auto fillStats() -> void {
  Meta m = metaLoad(d.key);
  char b[160];
  const long long s = m.num("seconds");
  std::snprintf(b, sizeof b, "Partidas: %lld    Tiempo: %lldh %02lldm    Ultima: %s",
                m.num("plays"), s / 3600, (s / 60) % 60, fmtTime(m.num("last")).c_str());
  SetWindowTextW(d.stats, aw(b).c_str());
}

auto fillInfo() -> void {
  Meta m = metaLoad(d.key);
  for(int i = 0; i < kNF; i++) SetWindowTextW(d.edit[i], u8w(m.str(kFields[i])).c_str());
  // Las notas pueden traer \n sueltos del lanzador: la caja de Windows quiere \r\n.
  std::string n = m.str("notes"), o;
  for(size_t i = 0; i < n.size(); i++) {
    if(n[i] == '\n' && (i == 0 || n[i - 1] != '\r')) o.push_back('\r');
    o.push_back(n[i]);
  }
  SetWindowTextW(d.notes, u8w(o).c_str());
  SendMessageW(d.fav, BM_SETCHECK, m.flag("favorite") ? BST_CHECKED : BST_UNCHECKED, 0);
  fillStats();
}

auto saveInfo() -> void {
  Meta m = metaLoad(d.key);   // releer: plays/seconds los ha podido tocar otra sesion
  for(int i = 0; i < kNF; i++) m.setStr(kFields[i], narrow(textOf(d.edit[i]), CP_UTF8));
  std::string n = narrow(textOf(d.notes), CP_UTF8), o;
  for(char c : n)
    if(c != '\r') o.push_back(c);   // en disco con \n, como el lanzador
  m.setStr("notes", o);
  m.setFlag("favorite", SendMessageW(d.fav, BM_GETCHECK, 0, 0) == BST_CHECKED);
  SetWindowTextW(d.infoMsg, metaSave(d.key, m) ? L"Guardado." : L"No se pudo guardar.");
}

auto fillManuals() -> void {
  d.man = manuals(d.rom, d.hdr.name);
  SendMessageW(d.manList, LB_RESETCONTENT, 0, 0);
  for(const std::string& f : d.man) {
    std::string row = baseName(f) + "   (" + fmtSize(0) + ")";
    WIN32_FILE_ATTRIBUTE_DATA a;
    if(GetFileAttributesExA(f.c_str(), GetFileExInfoStandard, &a))
      row = baseName(f) + "   (" +
            fmtSize(((unsigned long long)a.nFileSizeHigh << 32) | a.nFileSizeLow) + ")";
    SendMessageW(d.manList, LB_ADDSTRING, 0, (LPARAM)aw(row).c_str());
  }
  if(d.man.empty())
    SendMessageW(d.manList, LB_ADDSTRING, 0,
                 (LPARAM)L"(sin manual: se buscan PDF/HTML/TXT/imagen junto a la ROM o en manuals/)");
  else
    SendMessageW(d.manList, LB_SETCURSEL, 0, 0);
}

auto fillCheats(int keepSel) -> void {
  std::vector<Cheat> lst = cheats(d.rom);
  SendMessageW(d.chtList, LB_RESETCONTENT, 0, 0);
  for(const Cheat& c : lst) {
    char extra[96] = "";
    int n = std::snprintf(extra, sizeof extra, "   %d codigo%s", (int)c.codes.size(),
                          c.codes.size() == 1 ? "" : "s");
    if(c.inert) n += std::snprintf(extra + n, sizeof extra - n, ", %d sin efecto", c.inert);
    if(c.bad) std::snprintf(extra + n, sizeof extra - n, ", %d ilegible%s", c.bad,
                            c.bad == 1 ? "" : "s");
    std::wstring row = std::wstring(c.on ? L"[x]  " : L"[  ]  ") + u8w(c.name) + aw(extra);
    SendMessageW(d.chtList, LB_ADDSTRING, 0, (LPARAM)row.c_str());
  }
  if(!lst.empty())
    SendMessageW(d.chtList, LB_SETCURSEL, keepSel >= 0 && keepSel < (int)lst.size() ? keepSel : 0, 0);
}

auto fillSaves() -> void {
  SendMessageW(d.savList, LB_RESETCONTENT, 0, 0);
  std::vector<SaveFile> s = saves(d.rom);
  for(const SaveFile& f : s) {
    char b[400];
    std::snprintf(b, sizeof b, "%-18s %-40s %10s   %s", f.label.c_str(), f.file.c_str(),
                  fmtSize(f.size).c_str(), fmtTime(f.mtime).c_str());
    SendMessageW(d.savList, LB_ADDSTRING, 0, (LPARAM)aw(b).c_str());
  }
  if(s.empty())
    SendMessageW(d.savList, LB_ADDSTRING, 0, (LPARAM)L"(nada guardado todavia)");
}

auto chtMsg(const std::string& s) -> void { SetWindowTextW(d.chtMsg, aw(s).c_str()); }

// ---------------------------------------------------------------- acciones
auto openFolder(const std::string& dir) -> void {
  ShellExecuteW(d.win, L"open", aw(dir.empty() ? std::string(".") : dir).c_str(), nullptr,
                nullptr, SW_SHOWNORMAL);
}

auto romDir() -> std::string {
  size_t k = d.rom.find_last_of("/\\");
  return k == std::string::npos ? std::string(".") : d.rom.substr(0, k);
}

auto openManual() -> void {
  int i = (int)SendMessageW(d.manList, LB_GETCURSEL, 0, 0);
  if(i < 0 || i >= (int)d.man.size()) return;
  // El visor de PDF/imagenes/navegador que tenga el sistema: el exe no lleva visor propio.
  ShellExecuteW(d.win, L"open", aw(d.man[i]).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

auto addManual() -> void {
  char file[MAX_PATH] = {0};
  OPENFILENAMEA ofn = {};
  ofn.lStructSize = sizeof ofn;
  ofn.hwndOwner = d.win;
  static const char kFilter[] =
      "Manual\0*.pdf;*.html;*.htm;*.txt;*.md;*.png;*.jpg;*.jpeg;*.webp\0";
  ofn.lpstrFilter = kFilter;
  ofn.lpstrTitle = "Manual del juego";
  ofn.lpstrFile = file;
  ofn.nMaxFile = MAX_PATH;
  ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
  if(!GetOpenFileNameA(&ofn) || !file[0]) return;
  std::string err;
  if(manualStore(d.rom, file, err).empty()) {
    MessageBoxW(d.win, aw(err).c_str(), L"Manual", MB_OK | MB_ICONWARNING);
    return;
  }
  fillManuals();
}

// Copia `src` (URL o ruta local) a `dst`. Bloquea: un indice son KB y un manual pocos MB.
auto fetch(const std::string& src, const std::string& dst, std::string& err) -> bool {
  DeleteFileW(aw(dst).c_str());
  if(src.find("://") == std::string::npos) {
    if(CopyFileW(u8w(src).c_str(), aw(dst).c_str(), FALSE)) return true;
    err = "no se pudo leer " + src;
    return false;
  }
  const HRESULT hr = URLDownloadToFileW(nullptr, u8w(src).c_str(), aw(dst).c_str(), 0, nullptr);
  if(SUCCEEDED(hr)) return true;
  char b[48];
  std::snprintf(b, sizeof b, " (error 0x%08lx)", (unsigned long)hr);
  err = "no se pudo descargar " + src + b;
  return false;
}

auto fileSize(const std::string& f) -> unsigned long long {
  WIN32_FILE_ATTRIBUTE_DATA a;
  if(!GetFileAttributesExA(f.c_str(), GetFileExInfoStandard, &a)) return 0;
  return ((unsigned long long)a.nFileSizeHigh << 32) | a.nFileSizeLow;
}

auto manMsg(const std::string& s) -> void { SetWindowTextW(d.manMsg, u8w(s).c_str()); }

// Busca el juego en el indice de la fuente que haya puesto el usuario y baja sus manuales a
// manuals/. La fuente se recuerda para todas las fichas.
auto downloadManuals() -> void {
  const std::string src = narrow(textOf(d.manSrc), CP_UTF8);
  if(src.empty()) { manMsg("Escribe primero la URL (o ruta) de un \u00edndice CSV de manuales."); return; }
  if(src != manualSource()) setManualSource(src);
  if(!d.man.empty() &&
     MessageBoxW(d.win, L"Este juego ya tiene manual. \u00bfBuscar y descargar otro igualmente?",
                 L"Manual", MB_YESNO | MB_ICONQUESTION) != IDYES)
    return;
  HCURSOR old = SetCursor(LoadCursor(nullptr, IDC_WAIT));
  manMsg("Leyendo el \u00edndice...");
  UpdateWindow(d.win);
  std::string err;
  const std::string idxFile = stateDir() + "/manuals_index.csv";
  std::vector<ManualEntry> idx;
  if(fetch(src, idxFile, err)) {
    std::string csv;
    if(std::FILE* f = std::fopen(idxFile.c_str(), "rb")) {
      char buf[65536];
      for(size_t n; (n = std::fread(buf, 1, sizeof buf, f)) > 0;) csv.append(buf, n);
      std::fclose(f);
    }
    idx = manualIndexParse(csv);
  }
  DeleteFileW(aw(idxFile).c_str());
  if(idx.empty()) {
    SetCursor(old);
    manMsg(err.empty() ? std::string("La fuente no es un \u00edndice CSV de manuales (Path,URL,...).") : err);
    return;
  }
  std::vector<ManualEntry> hits = manualIndexMatch(idx, d.rom, d.hdr.name);
  if(hits.empty()) {
    SetCursor(old);
    manMsg("La fuente tiene " + std::to_string(idx.size()) + " manuales; ninguno de este juego.");
    return;
  }
  if(hits.size() > 3) hits.resize(3);
  int got = 0;
  std::string last;
  for(const ManualEntry& m : hits) {
    std::string ext = m.path.substr(m.path.find_last_of('.') == std::string::npos ? m.path.size()
                                                                                 : m.path.find_last_of('.'));
    const std::string tmp = stateDir() + "/manual_dl.tmp" + ext;
    manMsg("Descargando " + baseName(m.path) + "...");
    UpdateWindow(d.win);
    if(!fetch(m.url, tmp, err)) continue;
    if(m.size && fileSize(tmp) != m.size) {
      err = baseName(m.path) + ": tama\u00f1o distinto del que dice el \u00edndice";
    } else if(!manualStore(d.rom, tmp, err).empty()) {
      got++;
      last = baseName(m.path);
    }
    DeleteFileW(aw(tmp).c_str());
  }
  SetCursor(old);
  fillManuals();
  if(got) manMsg(got == 1 ? "Descargado: " + last : "Descargados " + std::to_string(got) + " manuales.");
  else manMsg(err);
}

auto toggleCheat() -> void {
  int i = (int)SendMessageW(d.chtList, LB_GETCURSEL, 0, 0);
  std::vector<Cheat> lst = cheats(d.rom);
  if(i < 0 || i >= (int)lst.size()) return;
  std::string err;
  if(!setCheat(d.rom, i, !lst[i].on, err)) { chtMsg(err); return; }
  fillCheats(i);
  chtMsg("Cambiado. Los trucos se leen al arrancar la ROM.");
}

auto deleteCheat() -> void {
  int i = (int)SendMessageW(d.chtList, LB_GETCURSEL, 0, 0);
  std::vector<Cheat> lst = cheats(d.rom);
  if(i < 0 || i >= (int)lst.size()) return;
  std::wstring q = L"\u00bfBorrar el truco \"" + u8w(lst[i].name) + L"\"?";
  if(MessageBoxW(d.win, q.c_str(), L"Trucos", MB_YESNO | MB_ICONQUESTION) != IDYES) return;
  std::string err;
  if(!delCheat(d.rom, i, err)) { chtMsg(err); return; }
  fillCheats(i);
  chtMsg("Borrado.");
}

auto addCheatUi() -> void {
  std::string err;
  const std::string name = narrow(textOf(d.chtName), CP_UTF8);
  const std::string codes = narrow(textOf(d.chtCodes), CP_UTF8);
  if(!addCheat(d.rom, name, codes, true, err)) { chtMsg(err); return; }
  SetWindowTextW(d.chtName, L"");
  SetWindowTextW(d.chtCodes, L"");
  fillCheats((int)cheats(d.rom).size() - 1);
  chtMsg("Anadido. Los trucos se leen al arrancar la ROM.");
}

LRESULT CALLBACK cardProc(HWND h, UINT m, WPARAM w, LPARAM l) {
  switch(m) {
    case WM_COMMAND: {
      const int id = LOWORD(w), code = HIWORD(w);
      if(id >= kTab0 && id < kTab0 + 4) { showTab(id - kTab0); return 0; }
      switch(id) {
        case kPlay: d.play = true; DestroyWindow(h); return 0;
        case kClose: case IDCANCEL: DestroyWindow(h); return 0;
        case kInfoSave: saveInfo(); return 0;
        case kManOpen: openManual(); return 0;
        case kManList: if(code == LBN_DBLCLK) openManual(); return 0;
        case kManAdd: addManual(); return 0;
        case kManDir: openFolder(romDir()); return 0;
        case kManGet: downloadManuals(); return 0;
        case kChtToggle: toggleCheat(); return 0;
        case kChtList: if(code == LBN_DBLCLK) toggleCheat(); return 0;
        case kChtDel: deleteCheat(); return 0;
        case kChtAdd: addCheatUi(); return 0;
        case kSavDir: openFolder(romDir()); return 0;
        default: break;
      }
      break;
    }
    case WM_CTLCOLORSTATIC:
      // El titulo grande va en otro color; el resto como el sistema.
      if((HWND)l == GetDlgItem(h, 1)) {
        SetTextColor((HDC)w, RGB(0x20, 0x40, 0x90));
        SetBkMode((HDC)w, TRANSPARENT);
        return (LRESULT)GetSysColorBrush(COLOR_BTNFACE);
      }
      break;
    case WM_CLOSE:
      DestroyWindow(h);
      return 0;
    case WM_DESTROY:
      PostQuitMessage(0);
      return 0;
  }
  return DefWindowProcW(h, m, w, l);
}

auto build(HWND owner) -> bool {
  static bool cls = false;
  if(!cls) {
    WNDCLASSW wc = {};
    wc.lpfnWndProc = cardProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"kestrel64_card";
    RegisterClassW(&wc);
    cls = true;
  }
  const int cw = 640, ch = 440;
  RECT want = {0, 0, cw, ch};
  const DWORD style = WS_OVERLAPPEDWINDOW & ~WS_MAXIMIZEBOX & ~WS_THICKFRAME;
  AdjustWindowRect(&want, style, FALSE);
  const int ww = want.right - want.left, wh = want.bottom - want.top;
  int x = CW_USEDEFAULT, y = CW_USEDEFAULT;
  RECT o;
  if(owner && GetWindowRect(owner, &o)) {
    x = o.left + ((o.right - o.left) - ww) / 2;
    y = o.top + ((o.bottom - o.top) - wh) / 2;
  }
  const std::string base = baseName(romBase(d.rom));
  const std::wstring title = L"kestrel64 - ficha - " + aw(base);
  d.win = CreateWindowExW(0, L"kestrel64_card", title.c_str(), style, x, y, ww, wh, owner,
                          nullptr, GetModuleHandleW(nullptr), nullptr);
  if(!d.win) return false;
  d.font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
  d.big = CreateFontW(-20, 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY,
                      0, L"Segoe UI");

  // Cabecera comun: nombre interno (o el del fichero) y la linea tecnica del cartucho.
  HWND t = mk(L"STATIC", aw(d.hdr.ok && !d.hdr.name.empty() ? d.hdr.name : base), 0, 12, 8,
              cw - 24, 26, 1, -1);
  SendMessageW(t, WM_SETFONT, (WPARAM)d.big, TRUE);
  std::string line = baseName(d.rom);
  if(d.hdr.ok)
    line += "    " + d.hdr.cart + "  region " + std::string(1, d.hdr.region) + "    CRC " + d.hdr.crc;
  else
    line += "    (cabecera sin leer)";
  mk(L"STATIC", aw(line), SS_ENDELLIPSIS, 12, 36, cw - 24, 18, 0, -1);

  static const wchar_t* kTabs[4] = {L"Info", L"Manual", L"Trucos", L"Partidas"};
  for(int i = 0; i < 4; i++)
    d.tabBtn[i] = mk(L"BUTTON", kTabs[i], WS_TABSTOP | BS_AUTORADIOBUTTON | BS_PUSHLIKE |
                                              (i == 0 ? WS_GROUP : 0),
                     12 + i * 96, 60, 92, 26, kTab0 + i, -1);

  const int y0 = 98;
  // ---- Info
  for(int i = 0; i < kNF; i++) {
    const int col = i % 2, row = i / 2;
    const int lx = 12 + col * 310, ly = y0 + row * 30;
    mk(L"STATIC", kFieldLabels[i], 0, lx, ly + 3, 70, 18, 0, 0);
    d.edit[i] = mk(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, lx + 74, ly, 226, 22, kEdit0 + i, 0);
  }
  d.fav = mk(L"BUTTON", L"Favorito", WS_TABSTOP | BS_AUTOCHECKBOX, 322, y0 + 60, 120, 22, kFav, 0);
  mk(L"STATIC", L"Notas", 0, 12, y0 + 94, 70, 18, 0, 0);
  d.notes = mk(L"EDIT", L"", WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN |
                                 WS_VSCROLL,
               86, y0 + 92, cw - 98, 150, 0, 0);
  d.stats = mk(L"STATIC", L"", 0, 12, y0 + 252, cw - 24, 18, 0, 0);
  mk(L"BUTTON", L"Guardar", WS_TABSTOP, 12, y0 + 280, 100, 26, kInfoSave, 0);
  d.infoMsg = mk(L"STATIC", L"", 0, 124, y0 + 285, 300, 18, 0, 0);

  // ---- Manual
  d.manList = mk(L"LISTBOX", L"", WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                 12, y0, cw - 24, 180, kManList, 1);
  mk(L"BUTTON", L"Abrir", WS_TABSTOP, 12, y0 + 188, 100, 26, kManOpen, 1);
  mk(L"BUTTON", L"A\u00f1adir...", WS_TABSTOP, 120, y0 + 188, 100, 26, kManAdd, 1);
  mk(L"BUTTON", L"Abrir carpeta", WS_TABSTOP, 228, y0 + 188, 110, 26, kManDir, 1);
  // Fuente de manuales: la pone el usuario (indice CSV estilo external_files.csv de MiSTer).
  mk(L"STATIC", L"Fuente (\u00edndice CSV):", 0, 12, y0 + 229, 120, 18, 0, 1);
  d.manSrc = mk(L"EDIT", u8w(manualSource()), WS_TABSTOP | ES_AUTOHSCROLL, 134, y0 + 226,
                cw - 300, 22, kManSrc, 1);
  mk(L"BUTTON", L"Buscar y descargar", WS_TABSTOP, cw - 158, y0 + 225, 146, 26, kManGet, 1);
  d.manMsg = mk(L"STATIC", L"", SS_ENDELLIPSIS, 12, y0 + 258, cw - 24, 18, 0, 1);
  mk(L"STATIC", L"Todo va a manuals/ junto a la ROM. La fuente es tuya: el emulador no trae ninguna.",
     0, 12, y0 + 284, cw - 24, 18, 0, 1);

  // ---- Trucos
  d.chtList = mk(L"LISTBOX", L"", WS_TABSTOP | WS_VSCROLL | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT,
                 12, y0, 380, 230, kChtList, 2);
  mk(L"BUTTON", L"Activar / desactivar", WS_TABSTOP, 12, y0 + 236, 150, 26, kChtToggle, 2);
  mk(L"BUTTON", L"Borrar", WS_TABSTOP, 170, y0 + 236, 80, 26, kChtDel, 2);
  mk(L"STATIC", L"Nuevo truco", 0, 404, y0, 200, 18, 0, 2);
  d.chtName = mk(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, 404, y0 + 20, cw - 416, 22, kChtName, 2);
  mk(L"STATIC", L"C\u00f3digos (AAAAAAAA VVVV)", 0, 404, y0 + 48, 200, 18, 0, 2);
  d.chtCodes = mk(L"EDIT", L"", WS_TABSTOP | ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN |
                                    WS_VSCROLL,
                  404, y0 + 68, cw - 416, 130, kChtCodes, 2);
  mk(L"BUTTON", L"A\u00f1adir", WS_TABSTOP, 404, y0 + 204, 100, 26, kChtAdd, 2);
  d.chtMsg = mk(L"STATIC", L"", SS_ENDELLIPSIS, 12, y0 + 270, cw - 24, 18, 0, 2);
  mk(L"STATIC", aw("Fichero: " + baseName(chtPath(d.rom))), SS_ENDELLIPSIS, 12, y0 + 290,
     cw - 24, 18, 0, 2);

  // ---- Partidas
  d.savList = mk(L"LISTBOX", L"", WS_TABSTOP | WS_VSCROLL | LBS_NOINTEGRALHEIGHT, 12, y0, cw - 24,
                 240, kSavList, 3);
  SendMessageW(d.savList, WM_SETFONT, (WPARAM)GetStockObject(ANSI_FIXED_FONT), TRUE);
  mk(L"BUTTON", L"Abrir carpeta", WS_TABSTOP, 12, y0 + 250, 110, 26, kSavDir, 3);

  // ---- botones de abajo
  if(d.canPlay) mk(L"BUTTON", L"Jugar", WS_TABSTOP | BS_DEFPUSHBUTTON, cw - 232, ch - 38, 110, 28,
                   kPlay, -1);
  mk(L"BUTTON", L"Cerrar", WS_TABSTOP, cw - 116, ch - 38, 104, 28, kClose, -1);

  fillInfo();
  fillManuals();
  fillCheats(0);
  fillSaves();
  showTab(0);
  return true;
}

}  // namespace

auto cardOpen() -> bool { return g_open.load(); }

auto showCard(const std::string& rom, void* owner, bool canPlay) -> bool {
  if(g_open.exchange(true)) {
    if(HWND w = g_win.load()) SetForegroundWindow(w);
    return false;
  }
  d = Dlg();
  d.rom = rom;
  d.hdr = readHeader(rom);
  d.key = metaKey(rom, d.hdr);
  d.canPlay = canPlay;
  HWND own = (HWND)owner;
  bool played = false;
  if(build(own)) {
    g_win.store(d.win);
    // Modal a mano: el dueno no recibe entrada mientras la ficha esta abierta, pero sigue
    // pintandose (sus mensajes los despacha este mismo bucle si es del mismo hilo).
    if(own) EnableWindow(own, FALSE);
    ShowWindow(d.win, SW_SHOW);
    SetForegroundWindow(d.win);
    MSG msg;
    while(GetMessageW(&msg, nullptr, 0, 0) > 0) {
      if(!d.win || !IsWindow(d.win) || !IsDialogMessageW(d.win, &msg)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
    }
    if(own) {
      EnableWindow(own, TRUE);
      SetForegroundWindow(own);
    }
    played = d.play;
  }
  if(d.big) DeleteObject(d.big);
  d = Dlg();
  g_win.store(nullptr);
  g_open.store(false);
  return played;
}

}  // namespace kestrel::ui::card

#else   // !_WIN32

namespace kestrel::ui::card {
auto showCard(const std::string&, void*, bool) -> bool { return false; }
auto cardOpen() -> bool { return false; }
}  // namespace kestrel::ui::card

#endif
