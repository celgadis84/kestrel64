// kestrel64 -- ventana del depurador: Win32 + Direct3D 11 + Dear ImGui (docs/DEBUGGER.md).
//
// Hilo propio con su bucle de mensajes y su dispositivo D3D11; no comparte nada con la
// ventana de juego (GLFW/Vulkan) ni con el presentador. Los datos se piden por
// Server::call, el mismo despacho que el MCP, con las mismas ordenes: lo que se ve aqui es
// lo que ve un agente por el puente, y viceversa.
//
// Paneles: control (pausa/paso/cuadro), CPU, desensamblado con puntos de ruptura, memoria
// con edicion y punto de observacion, lista de comandos del RDP por cuadro, tareas del RSP,
// linea de tiempo CPU/RSP/RDP, visor de framebuffer/z/cobertura, TMEM decodificada por
// tile, TLB, perfilador, anillo de eventos, salida USB (SC64) y registros RCP/RSP.

#include "debugger.hpp"

#ifdef _WIN32

#include "../core/system.hpp"
#include "../telemetry/server.hpp"
#include "../telemetry/json.hpp"

#include <windows.h>
#include <d3d11.h>

#include "imgui.h"
#include "imgui_internal.h"   // DockBuilder: la distribucion de fabrica
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace kestrel::dbgwin {

namespace {

using json::Value;

System*           g_sys = nullptr;
std::mutex        g_mx;          // open/close
std::thread       g_thr;
std::atomic<bool> g_run{false};
std::atomic<bool> g_quit{false};
std::atomic<HWND> g_hwnd{nullptr};

ID3D11Device*           g_dev = nullptr;
ID3D11DeviceContext*    g_ctx = nullptr;
IDXGISwapChain*         g_sc  = nullptr;
ID3D11RenderTargetView* g_rtv = nullptr;
ID3D11SamplerState*     g_point = nullptr;   // muestreo sin filtrar para ver pixeles
UINT g_resizeW = 0, g_resizeH = 0;

// ------------------------------------------------------------------ peticiones

struct Reply {
  bool ok = false;
  Value data;
  std::string err;
  std::vector<u8> blob;
};

auto Q(const char* cmd, const Value& args = Value::object(), const std::vector<u8>& in = {}) -> Reply {
  Reply r;
  if(!g_sys || !g_sys->telemetryServer()) { r.err = "sin servidor de telemetria"; return r; }
  Value req = Value::object();
  req.set("id", 0);
  req.set("cmd", cmd);
  req.set("args", args);
  Value rep = Value::object();
  g_sys->telemetryServer()->call(req, in, rep, r.blob);
  r.ok = rep.get("ok").b;
  r.data = rep.get("data");
  r.err = rep.get("error").asString();
  return r;
}

auto A() -> Value { return Value::object(); }

auto hexParse(const char* s) -> u64 { return std::strtoull(s, nullptr, 16); }

auto num(const Value& v) -> u64 { return v.asU64(); }

auto arr(const Value& v) -> const json::Array& {
  static const json::Array empty;
  return (v.type == Value::Type::Array && v.arr) ? *v.arr : empty;
}

auto scalarText(const Value& v) -> std::string {
  char b[64];
  switch(v.type) {
    case Value::Type::Null: return "null";
    case Value::Type::Bool: return v.b ? "true" : "false";
    case Value::Type::String: return v.str;
    case Value::Type::Number:
      if(!v.isInt) { std::snprintf(b, sizeof b, "%.4g", v.num); return b; }
      if(v.sign && (s64)v.u < 0) { std::snprintf(b, sizeof b, "%lld", (long long)(s64)v.u); return b; }
      if(v.u < 10) { std::snprintf(b, sizeof b, "%llu", (unsigned long long)v.u); return b; }
      std::snprintf(b, sizeof b, "0x%llx (%llu)", (unsigned long long)v.u, (unsigned long long)v.u);
      return b;
    default: return "?";
  }
}

// Arbol generico de un JSON: para los paneles cuyo contenido es "lo que diga el servidor"
// (status, RCP, RSP) y para que una orden nueva se vea sin escribir su panel.
auto jsonTree(const char* label, const Value& v) -> void {
  if(v.type == Value::Type::Object && v.obj) {
    if(ImGui::TreeNode(label)) {
      for(const auto& kv : *v.obj) jsonTree(kv.first.c_str(), kv.second);
      ImGui::TreePop();
    }
  } else if(v.type == Value::Type::Array && v.arr) {
    if(ImGui::TreeNode(label, "%s [%zu]", label, v.arr->size())) {
      int i = 0;
      for(const auto& x : *v.arr) { char b[16]; std::snprintf(b, sizeof b, "%d", i++); jsonTree(b, x); }
      ImGui::TreePop();
    }
  } else {
    ImGui::BulletText("%s: %s", label, scalarText(v).c_str());
  }
}

auto jsonBody(const Value& v) -> void {
  if(v.type == Value::Type::Object && v.obj) for(const auto& kv : *v.obj) jsonTree(kv.first.c_str(), kv.second);
}

struct Ticker {
  double last = -1e9;
  auto due(double period) -> bool {
    const double t = ImGui::GetTime();
    if(t - last >= period) { last = t; return true; }
    return false;
  }
};

bool g_dirty = true;   // una accion ha cambiado el estado: que todos los paneles relean

// ------------------------------------------------------------------ texturas

struct Tex {
  ID3D11Texture2D* t = nullptr;
  ID3D11ShaderResourceView* v = nullptr;
  int w = 0, h = 0;
  auto release() -> void {
    if(v) v->Release();
    if(t) t->Release();
    v = nullptr; t = nullptr; w = h = 0;
  }
  auto upload(const u8* px, int W, int H) -> void {
    if(W <= 0 || H <= 0) return;
    if(W != w || H != h) {
      release();
      D3D11_TEXTURE2D_DESC d{};
      d.Width = W; d.Height = H; d.MipLevels = 1; d.ArraySize = 1;
      d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
      d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
      if(FAILED(g_dev->CreateTexture2D(&d, nullptr, &t))) return;
      if(FAILED(g_dev->CreateShaderResourceView(t, nullptr, &v))) { release(); return; }
      w = W; h = H;
    }
    g_ctx->UpdateSubresource(t, 0, nullptr, px, (UINT)W * 4, 0);
  }
};

auto setPointSampler(const ImDrawList*, const ImDrawCmd*) -> void {
  if(g_point) g_ctx->PSSetSamplers(0, 1, &g_point);
}

// Imagen con muestreo de punto, escalada; devuelve el pixel bajo el raton (o -1).
auto pixelImage(Tex& t, float zoom, int& hx, int& hy) -> void {
  hx = hy = -1;
  if(!t.v) { ImGui::TextDisabled("(sin imagen)"); return; }
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddCallback(setPointSampler, nullptr);
  ImGui::Image((ImTextureID)(uintptr_t)t.v, ImVec2(t.w * zoom, t.h * zoom));
  dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);
  if(ImGui::IsItemHovered()) {
    const ImVec2 o = ImGui::GetItemRectMin(), m = ImGui::GetMousePos();
    hx = (int)((m.x - o.x) / zoom);
    hy = (int)((m.y - o.y) / zoom);
    if(hx < 0 || hy < 0 || hx >= t.w || hy >= t.h) hx = hy = -1;
  }
}

// ------------------------------------------------------------------ paneles

bool showControl = true, showCpu = true, showDisasm = true, showMem = true, showRdp = true,
     showRsp = true, showTimeline = true, showFb = true, showTmem = false, showTlb = false,
     showProf = false, showEv = false, showUsb = false, showRcp = false;

auto panelControl() -> void {
  static Reply st, dst;
  static Ticker tk;
  if(!ImGui::Begin("Control", &showControl)) { ImGui::End(); return; }
  if(g_dirty || tk.due(0.25)) { st = Q("status"); dst = Q("dbg.status"); }
  const bool paused = st.data.get("paused").b;
  ImGui::TextColored(paused ? ImVec4(1, .8f, .3f, 1) : ImVec4(.4f, 1, .4f, 1), paused ? "PAUSADO" : "CORRIENDO");
  ImGui::SameLine();
  if(ImGui::Button(paused ? "Continuar (F5)" : "Pausa (F5)") ||
     (ImGui::IsKeyPressed(ImGuiKey_F5, false) && !ImGui::GetIO().WantTextInput)) {
    Q(paused ? "resume" : "pause"); g_dirty = true;
  }
  ImGui::SameLine();
  if(ImGui::Button("Paso (F10)") || (ImGui::IsKeyPressed(ImGuiKey_F10) && !ImGui::GetIO().WantTextInput)) {
    Value a = A(); a.set("count", 1); Q("cpu.step", a); g_dirty = true;
  }
  ImGui::SameLine();
  if(ImGui::Button("Paso x100")) { Value a = A(); a.set("count", 100); Q("cpu.step", a); g_dirty = true; }
  ImGui::SameLine();
  if(ImGui::Button("Cuadro +1")) {
    Value a = A(); a.set("fields", 1); a.set("timeout_ms", 2000); Q("frame.advance", a); g_dirty = true;
  }
  ImGui::SameLine();
  if(ImGui::Button("RESET")) { Q("reset.soft"); g_dirty = true; }

  ImGui::Separator();
  bool rec = dst.data.get("on").b, frz = dst.data.get("freeze").b;
  if(ImGui::Checkbox("Grabadora por cuadro (RDP/RSP)", &rec)) { Value a = A(); a.set("on", rec ? 1 : 0); Q("dbg.enable", a); g_dirty = true; }
  ImGui::SameLine();
  if(ImGui::Checkbox("Congelar cuadro", &frz)) { Value a = A(); a.set("on", frz ? 1 : 0); Q("dbg.freeze", a); g_dirty = true; }
  if(rec) {
    const Value last = dst.data.get("last");
    ImGui::Text("Cuadros cerrados: %llu   ultimo #%llu: %llu cmds RDP, %llu trabajos, %llu tareas RSP%s",
                (unsigned long long)num(dst.data.get("framesClosed")), (unsigned long long)num(last.get("number")),
                (unsigned long long)num(last.get("cmds")), (unsigned long long)num(last.get("jobs")),
                (unsigned long long)num(last.get("tasks")), last.get("truncated").b ? " (truncado)" : "");
  }
  ImGui::Separator();
  if(!st.ok) ImGui::TextColored(ImVec4(1, .4f, .4f, 1), "%s", st.err.c_str());
  if(ImGui::CollapsingHeader("Estado")) jsonBody(st.data);
  ImGui::End();
}

static const char* kGpr[32] = {
  "zero","at","v0","v1","a0","a1","a2","a3","t0","t1","t2","t3","t4","t5","t6","t7",
  "s0","s1","s2","s3","s4","s5","s6","s7","t8","t9","k0","k1","gp","sp","fp","ra"};

u64 g_pc = 0;   // ultimo PC leido (lo usa el desensamblado)

auto panelCpu() -> void {
  static Reply r;
  static Ticker tk;
  if(!ImGui::Begin("CPU", &showCpu)) { ImGui::End(); return; }
  if(g_dirty || tk.due(0.25)) r = Q("cpu.regs");
  if(!r.ok) { ImGui::TextColored(ImVec4(1, .4f, .4f, 1), "%s", r.err.c_str()); ImGui::End(); return; }
  g_pc = num(r.data.get("pc"));
  ImGui::Text("pc %016llx  next %016llx", (unsigned long long)g_pc, (unsigned long long)num(r.data.get("nextPc")));
  ImGui::Text("hi %016llx  lo   %016llx", (unsigned long long)num(r.data.get("hi")), (unsigned long long)num(r.data.get("lo")));
  ImGui::TextUnformatted(r.data.get("disasm").asString().c_str());
  if(r.data.get("halted").b) ImGui::TextColored(ImVec4(1, .4f, .4f, 1), "PARADA: %s", r.data.get("haltReason").asString().c_str());
  const Value g = r.data.get("gpr");
  if(ImGui::BeginTable("gpr", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
    for(int i = 0; i < 32; i++) {
      ImGui::TableNextColumn();
      const u64 v = num(g.get(kGpr[i]));
      ImGui::Text("%-4s %016llx", kGpr[i], (unsigned long long)v);
    }
    ImGui::EndTable();
  }
  const Value c0 = r.data.get("cop0");
  if(ImGui::CollapsingHeader("COP0", ImGuiTreeNodeFlags_DefaultOpen)) {
    static const char* n[] = {"Status", "Cause", "EPC", "Count", "Compare", "BadVAddr"};
    for(const char* k : n) ImGui::Text("%-8s %016llx", k, (unsigned long long)num(c0.get(k)));
  }
  if(ImGui::CollapsingHeader("FPR")) {
    const Value f = r.data.get("fpr");
    if(ImGui::BeginTable("fpr", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
      for(int i = 0; i < 32; i++) {
        char k[8]; std::snprintf(k, sizeof k, "f%d", i);
        ImGui::TableNextColumn();
        ImGui::Text("%-3s %016llx", k, (unsigned long long)num(f.get(k)));
      }
      ImGui::EndTable();
    }
  }
  ImGui::End();
}

auto panelDisasm() -> void {
  static Reply r, bps;
  static Ticker tk;
  static bool follow = true;
  static char addrBuf[24] = "80000400";
  static u64 sel = 0;
  if(!ImGui::Begin("Desensamblado", &showDisasm)) { ImGui::End(); return; }
  ImGui::Checkbox("Seguir PC", &follow);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(140);
  bool go = ImGui::InputText("Direccion", addrBuf, sizeof addrBuf, ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue);
  if(go) follow = false;
  u64 base = follow ? (g_pc >= 64 ? g_pc - 64 : g_pc) : hexParse(addrBuf);
  base &= ~3ull;
  if(g_dirty || go || tk.due(0.5)) {
    Value a = A(); a.set("addr", base); a.set("count", 64);
    r = Q("cpu.disasm", a);
    bps = Q("cpu.bp.list");
  }
  std::vector<u64> bl;
  for(const auto& b : arr(bps.data.get("breakpoints"))) bl.push_back(num(b));
  auto isBp = [&](u64 a) { return std::find(bl.begin(), bl.end(), a) != bl.end() ||
                                  std::find(bl.begin(), bl.end(), (u64)(u32)a) != bl.end(); };
  ImGui::SameLine();
  if(ImGui::Button("Ejecutar hasta seleccion") && sel) {
    Value a = A(); a.set("addr", sel); a.set("timeout_ms", 3000); Q("cpu.run_until", a); g_dirty = true;
  }
  ImGui::SameLine();
  if(ImGui::Button("Quitar todos")) { Q("cpu.bp.del"); g_dirty = true; }
  ImGui::TextDisabled("Clic en la primera columna = punto de ruptura. Ultimo disparo: %s",
                      scalarText(bps.data.get("lastHit")).c_str());
  if(!r.ok) { ImGui::TextColored(ImVec4(1, .4f, .4f, 1), "%s", r.err.c_str()); ImGui::End(); return; }
  if(ImGui::BeginTable("dis", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable)) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("BP");
    ImGui::TableSetupColumn("Direccion");
    ImGui::TableSetupColumn("Simbolo");
    ImGui::TableSetupColumn("Instruccion", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    int i = 0;
    for(const auto& e : arr(r.data.get("insns"))) {
      const u64 a = num(e.get("addr"));
      ImGui::TableNextRow();
      const bool here = (u32)a == (u32)g_pc;
      if(here) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, IM_COL32(60, 90, 30, 255));
      ImGui::TableNextColumn();
      ImGui::PushID(i++);
      const bool b = isBp(a);
      if(ImGui::SmallButton(b ? "X" : " ")) {
        Value q = A(); q.set("addr", a);
        Q(b ? "cpu.bp.del" : "cpu.bp.add", q); g_dirty = true;
      }
      ImGui::TableNextColumn();
      char t[24]; std::snprintf(t, sizeof t, "%08llx", (unsigned long long)(u32)a);
      if(ImGui::Selectable(t, sel == a, ImGuiSelectableFlags_SpanAllColumns)) sel = a;
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(e.get("sym").asString().c_str());
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(e.get("text").asString().c_str());
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  ImGui::End();
}

auto panelMem() -> void {
  static const char* regions[] = {"RDRAM", "DMEM", "IMEM", "PIF_RAM", "CART_ROM", "SAVE", "EEPROM"};
  static int reg = 0;
  static char addrBuf[24] = "0";
  static bool coherent = true;
  static Reply r, w;
  static Ticker tk;
  static char wAddr[24] = "0", wData[256] = "";
  static char watchAddr[24] = "0";
  static int watchLen = 4;
  static bool watchR = false, watchW = true;
  if(!ImGui::Begin("Memoria", &showMem)) { ImGui::End(); return; }
  ImGui::SetNextItemWidth(110);
  bool chg = ImGui::Combo("Region", &reg, regions, IM_ARRAYSIZE(regions));
  ImGui::SameLine();
  ImGui::SetNextItemWidth(120);
  chg |= ImGui::InputText("Desde", addrBuf, sizeof addrBuf, ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue);
  ImGui::SameLine();
  chg |= ImGui::Checkbox("Coherente (D$)", &coherent);
  u32 base = (u32)hexParse(addrBuf) & ~15u;
  ImGui::SameLine();
  if(ImGui::Button("<<")) { base = base >= 0x200 ? base - 0x200 : 0; std::snprintf(addrBuf, sizeof addrBuf, "%x", base); chg = true; }
  ImGui::SameLine();
  if(ImGui::Button(">>")) { base += 0x200; std::snprintf(addrBuf, sizeof addrBuf, "%x", base); chg = true; }
  if(chg || g_dirty || tk.due(0.5)) {
    Value a = A(); a.set("region", regions[reg]); a.set("addr", base); a.set("len", 512);
    if(coherent) a.set("coherent", 1);
    r = Q("mem.read", a);
  }
  if(!r.ok) ImGui::TextColored(ImVec4(1, .4f, .4f, 1), "%s", r.err.c_str());
  if(ImGui::BeginChild("hex", ImVec2(0, 34 * ImGui::GetTextLineHeightWithSpacing()), ImGuiChildFlags_Borders)) {
    const std::vector<u8>& b = r.blob;
    for(u32 row = 0; row * 16 < b.size(); row++) {
      ImGui::Text("%08x ", base + row * 16);
      for(u32 c = 0; c < 16 && row * 16 + c < b.size(); c++) {
        ImGui::SameLine(0, c == 8 ? 10.f : 4.f);
        char t[8]; std::snprintf(t, sizeof t, "%02x##%u", b[row * 16 + c], row * 16 + c);
        if(ImGui::Selectable(t, false, 0, ImGui::CalcTextSize("00"))) {
          std::snprintf(wAddr, sizeof wAddr, "%x", base + row * 16 + c);
          std::snprintf(wData, sizeof wData, "%02x", b[row * 16 + c]);
        }
      }
      ImGui::SameLine(0, 12.f);
      char asc[17] = {};
      for(u32 c = 0; c < 16 && row * 16 + c < b.size(); c++) { u8 v = b[row * 16 + c]; asc[c] = (v >= 32 && v < 127) ? (char)v : '.'; }
      ImGui::TextUnformatted(asc);
    }
    if(ImGui::IsWindowHovered() && ImGui::GetIO().MouseWheel != 0.f) {
      const float wh = ImGui::GetIO().MouseWheel;
      base = wh > 0 ? (base >= 0x40 ? base - 0x40 : 0) : base + 0x40;
      std::snprintf(addrBuf, sizeof addrBuf, "%x", base);
      tk.last = -1e9;
    }
  }
  ImGui::EndChild();
  ImGui::SetNextItemWidth(120);
  ImGui::InputText("Escribir en", wAddr, sizeof wAddr, ImGuiInputTextFlags_CharsHexadecimal);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(260);
  ImGui::InputText("bytes (hex)", wData, sizeof wData);
  ImGui::SameLine();
  if(ImGui::Button("Escribir")) {
    std::vector<u8> bytes;
    std::string h;
    for(const char* p = wData; *p; p++) if(std::isxdigit((unsigned char)*p)) h += *p;
    for(size_t i = 0; i + 1 < h.size(); i += 2) bytes.push_back((u8)std::strtoul(h.substr(i, 2).c_str(), nullptr, 16));
    if(!bytes.empty()) {
      Value a = A(); a.set("region", regions[reg]); a.set("addr", (u32)hexParse(wAddr));
      w = Q("mem.write", a, bytes); g_dirty = true;
    }
  }
  if(!w.ok && !w.err.empty()) ImGui::TextColored(ImVec4(1, .4f, .4f, 1), "%s", w.err.c_str());

  if(ImGui::CollapsingHeader("Punto de observacion (direccion fisica)", ImGuiTreeNodeFlags_DefaultOpen)) {
    static Reply ws;
    static Ticker wt;
    ImGui::SetNextItemWidth(120);
    ImGui::InputText("Fisica", watchAddr, sizeof watchAddr, ImGuiInputTextFlags_CharsHexadecimal);
    ImGui::SameLine(); ImGui::SetNextItemWidth(80); ImGui::InputInt("Bytes", &watchLen);
    ImGui::SameLine(); ImGui::Checkbox("Lectura", &watchR);
    ImGui::SameLine(); ImGui::Checkbox("Escritura", &watchW);
    ImGui::SameLine();
    if(ImGui::Button("Armar")) {
      Value a = A(); a.set("addr", (u32)hexParse(watchAddr)); a.set("len", (u32)std::max(1, watchLen));
      a.set("r", watchR ? 1 : 0); a.set("w", watchW ? 1 : 0);
      ws = Q("mem.watch", a); g_dirty = true;
    }
    ImGui::SameLine();
    if(ImGui::Button("Quitar")) { Value a = A(); a.set("clear", 1); ws = Q("mem.watch", a); }
    if(g_dirty || wt.due(0.25)) ws = Q("mem.watch");
    if(!ws.ok) ImGui::TextColored(ImVec4(1, .4f, .4f, 1), "%s", ws.err.c_str());
    else if(ws.data.get("hit").b)
      ImGui::TextColored(ImVec4(1, .8f, .3f, 1), "DISPARO: %s en %08llx desde pc %08llx (CPU en pausa)",
                         ws.data.get("hitWrite").b ? "escritura" : "lectura",
                         (unsigned long long)num(ws.data.get("hitAddr")), (unsigned long long)(u32)num(ws.data.get("hitPc")));
    else if(ws.data.get("armed").b)
      ImGui::Text("Armado [%08llx, %08llx)", (unsigned long long)num(ws.data.get("lo")), (unsigned long long)num(ws.data.get("hi")));
    else ImGui::TextDisabled("Sin punto de observacion");
  }
  ImGui::End();
}

struct RdpRow { u32 i, addr, job, op; std::string name, text, words; };

auto panelRdp() -> void {
  static std::vector<RdpRow> rows;
  static std::vector<int> vis;
  static int frame = 0, sel = -1;
  static bool prims = false, autoR = true;
  static char filter[64] = "";
  static std::string err;
  static u64 total = 0, number = 0;
  static bool trunc = false;
  static Ticker tk;
  if(!ImGui::Begin("RDP: comandos del cuadro", &showRdp)) { ImGui::End(); return; }
  bool reload = ImGui::RadioButton("Ultimo cuadro", &frame, 0);
  ImGui::SameLine(); reload |= ImGui::RadioButton("En curso", &frame, 1);
  ImGui::SameLine(); reload |= ImGui::Checkbox("Solo primitivas", &prims);
  ImGui::SameLine(); ImGui::Checkbox("Auto (1 s)", &autoR);
  ImGui::SameLine(); reload |= ImGui::Button("Leer");
  ImGui::SetNextItemWidth(200);
  bool refilter = ImGui::InputText("Filtro (nombre/texto)", filter, sizeof filter);
  if(reload || (autoR && tk.due(1.0)) || (rows.empty() && tk.due(1.0))) {
    Value a = A(); a.set("frame", frame); a.set("count", 5000); a.set("prims", prims ? 1 : 0);
    Reply r = Q("dbg.rdp.frame", a);
    err = r.ok ? "" : r.err;
    if(r.ok) {
      rows.clear();
      for(const auto& e : arr(r.data.get("cmds"))) {
        RdpRow w{(u32)num(e.get("i")), (u32)num(e.get("addr")), (u32)num(e.get("job")), (u32)num(e.get("op")),
                 e.get("name").asString(), e.get("text").asString(), {}};
        for(const auto& x : arr(e.get("words"))) { w.words += x.asString(); w.words += ' '; }
        rows.push_back(std::move(w));
      }
      total = num(r.data.get("total")); number = num(r.data.get("number")); trunc = r.data.get("truncated").b;
      refilter = true;
      if(sel >= (int)rows.size()) sel = -1;
    }
  }
  if(refilter) {
    vis.clear();
    for(int i = 0; i < (int)rows.size(); i++)
      if(!filter[0] || rows[i].name.find(filter) != std::string::npos || rows[i].text.find(filter) != std::string::npos) vis.push_back(i);
  }
  if(!err.empty()) {
    ImGui::TextColored(ImVec4(1, .4f, .4f, 1), "%s", err.c_str());
    if(ImGui::Button("Encender grabadora")) { Value a = A(); a.set("on", 1); Q("dbg.enable", a); }
    ImGui::End();
    return;
  }
  ImGui::Text("Cuadro #%llu: %llu comandos%s, mostrando %zu", (unsigned long long)number, (unsigned long long)total,
              trunc ? " (TRUNCADO)" : "", vis.size());
  const float detailH = 6 * ImGui::GetTextLineHeightWithSpacing();
  if(ImGui::BeginTable("rdp", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit,
                       ImVec2(0, -detailH))) {
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableSetupColumn("#"); ImGui::TableSetupColumn("Dir"); ImGui::TableSetupColumn("Trabajo");
    ImGui::TableSetupColumn("Comando"); ImGui::TableSetupColumn("Detalle", ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableHeadersRow();
    ImGuiListClipper clip;
    clip.Begin((int)vis.size());
    while(clip.Step())
      for(int k = clip.DisplayStart; k < clip.DisplayEnd; k++) {
        const RdpRow& w = rows[vis[k]];
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        char t[16]; std::snprintf(t, sizeof t, "%u", w.i);
        if(ImGui::Selectable(t, sel == vis[k], ImGuiSelectableFlags_SpanAllColumns)) sel = vis[k];
        ImGui::TableNextColumn(); ImGui::Text("%06x", w.addr);
        ImGui::TableNextColumn(); ImGui::Text("%u", w.job);
        ImGui::TableNextColumn(); ImGui::TextUnformatted(w.name.c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(w.text.c_str());
      }
    ImGui::EndTable();
  }
  if(ImGui::BeginChild("det", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
    if(sel >= 0 && sel < (int)rows.size()) {
      const RdpRow& w = rows[sel];
      ImGui::Text("#%u  %s  (op 0x%02x, dir %06x, trabajo %u)", w.i, w.name.c_str(), w.op, w.addr, w.job);
      ImGui::TextWrapped("%s", w.text.c_str());
      ImGui::TextDisabled("%s", w.words.c_str());
    } else ImGui::TextDisabled("Selecciona un comando");
  }
  ImGui::EndChild();
  ImGui::End();
}

auto panelRsp() -> void {
  static Reply r;
  static Ticker tk;
  static int frame = 0;
  if(!ImGui::Begin("RSP: tareas del cuadro", &showRsp)) { ImGui::End(); return; }
  ImGui::RadioButton("Ultimo", &frame, 0); ImGui::SameLine(); ImGui::RadioButton("En curso", &frame, 1);
  if(g_dirty || tk.due(1.0)) { Value a = A(); a.set("frame", frame); r = Q("dbg.rsp.tasks", a); }
  if(!r.ok) { ImGui::TextColored(ImVec4(1, .4f, .4f, 1), "%s", r.err.c_str()); ImGui::End(); return; }
  if(ImGui::BeginTable("tasks", 8, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit)) {
    ImGui::TableSetupScrollFreeze(0, 1);
    for(const char* h : {"Tipo", "ucode", "ucodeData", "data_ptr", "bytes", "ciclos RSP", "duracion", "fin"}) ImGui::TableSetupColumn(h);
    ImGui::TableHeadersRow();
    for(const auto& t : arr(r.data.get("tasks"))) {
      const u32 ty = (u32)num(t.get("type"));
      ImGui::TableNextRow();
      ImGui::TableNextColumn(); ImGui::Text("%s", ty == 1 ? "1 GFX" : ty == 2 ? "2 AUDIO" : ty == 4 ? "4 JPEG" : std::to_string(ty).c_str());
      ImGui::TableNextColumn(); ImGui::Text("%08llx", (unsigned long long)num(t.get("ucode")));
      ImGui::TableNextColumn(); ImGui::Text("%08llx", (unsigned long long)num(t.get("ucodeData")));
      ImGui::TableNextColumn(); ImGui::Text("%08llx", (unsigned long long)num(t.get("dataPtr")));
      ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)num(t.get("dataSize")));
      ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)num(t.get("cycles")));
      ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)(num(t.get("t1")) - num(t.get("t0"))));
      ImGui::TableNextColumn(); ImGui::Text("%s", t.get("done").b ? "si" : "no");
    }
    ImGui::EndTable();
  }
  ImGui::End();
}

auto panelTimeline() -> void {
  static Reply r;
  static Ticker tk;
  if(!ImGui::Begin("Linea de tiempo", &showTimeline)) { ImGui::End(); return; }
  if(g_dirty || tk.due(1.0)) r = Q("dbg.timeline");
  if(!r.ok) { ImGui::TextColored(ImVec4(1, .4f, .4f, 1), "%s", r.err.c_str()); ImGui::End(); return; }
  const double span = (double)std::max<u64>(1, num(r.data.get("span")));
  const float W = std::max(100.f, ImGui::GetContentRegionAvail().x - 60.f), H = 22.f;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  auto lane = [&](const char* name, const json::Array* items, ImU32 col, ImU32 col2, int kind) {
    ImGui::TextUnformatted(name);
    ImGui::SameLine(60);
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::InvisibleButton(name, ImVec2(W, H));
    const bool hov = ImGui::IsItemHovered();
    const float mx = ImGui::GetMousePos().x;
    dl->AddRectFilled(p, ImVec2(p.x + W, p.y + H), IM_COL32(40, 40, 48, 255));
    double busy = 0;
    if(!items) {
      dl->AddRectFilled(p, ImVec2(p.x + W, p.y + H), col);
      busy = span;
    } else {
      for(const auto& e : *items) {
        const auto& v = arr(e);
        if(v.size() < 4) continue;
        const double t0 = (double)num(v[0]), t1 = (double)num(v[1]);
        busy += t1 - t0;
        const float x0 = p.x + (float)(t0 / span * W), x1 = std::max(x0 + 1.f, p.x + (float)(t1 / span * W));
        const ImU32 c = (kind == 0 && num(v[2]) == 2) ? col2 : col;
        dl->AddRectFilled(ImVec2(x0, p.y + 2), ImVec2(x1, p.y + H - 2), c);
        if(hov && mx >= x0 && mx <= x1) {
          if(kind == 0) ImGui::SetTooltip("tarea tipo %llu\n%llu ciclos RSP\n[%llu, %llu]", (unsigned long long)num(v[2]),
                                          (unsigned long long)num(v[3]), (unsigned long long)num(v[0]), (unsigned long long)num(v[1]));
          else ImGui::SetTooltip("trabajo RDP: %llu comandos, %llu bytes\n[%llu, %llu]", (unsigned long long)num(v[2]),
                                 (unsigned long long)num(v[3]), (unsigned long long)num(v[0]), (unsigned long long)num(v[1]));
        }
      }
    }
    ImGui::SameLine();
    ImGui::Text("%3.0f%%", busy / span * 100.0);
  };
  ImGui::Text("Cuadro #%llu, %.0f unidades de reloj de invitado", (unsigned long long)num(r.data.get("number")), span);
  lane("CPU", nullptr, IM_COL32(70, 130, 200, 255), 0, -1);
  const Value rs = r.data.get("rsp"), rd = r.data.get("rdp");
  lane("RSP", rs.arr.get(), IM_COL32(220, 150, 50, 255), IM_COL32(120, 200, 90, 255), 0);
  lane("RDP", rd.arr.get(), IM_COL32(200, 70, 90, 255), 0, 1);
  ImGui::TextDisabled("RSP: naranja = GFX, verde = AUDIO. El RDP de SoftRDP corre en el instante de la CPU: sus tramos salen finos.");
  ImGui::End();
}

auto panelFb() -> void {
  static Tex tex;
  static Reply r;
  static int kind = 0, siz = 0;
  static bool autoR = true, custom = false;
  static char addrBuf[24] = "0";
  static int width = 320, height = 240;
  static float zoom = 2.f;
  static Ticker tk;
  if(!ImGui::Begin("Framebuffer", &showFb)) { ImGui::End(); return; }
  static const char* kinds[] = {"color", "depth", "coverage"};
  ImGui::SetNextItemWidth(110);
  bool reload = ImGui::Combo("Capa", &kind, kinds, 3);
  ImGui::SameLine(); ImGui::Checkbox("Auto (0,5 s)", &autoR);
  ImGui::SameLine(); reload |= ImGui::Button("Leer");
  ImGui::SameLine(); ImGui::SetNextItemWidth(100); ImGui::SliderFloat("Zoom", &zoom, 1.f, 4.f, "%.1f");
  reload |= ImGui::Checkbox("Imagen propia", &custom);
  if(custom) {
    ImGui::SameLine(); ImGui::SetNextItemWidth(100);
    reload |= ImGui::InputText("Dir", addrBuf, sizeof addrBuf, ImGuiInputTextFlags_CharsHexadecimal | ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine(); ImGui::SetNextItemWidth(80); reload |= ImGui::InputInt("Ancho", &width, 0);
    ImGui::SameLine(); ImGui::SetNextItemWidth(80); reload |= ImGui::InputInt("Alto", &height, 0);
    ImGui::SameLine(); ImGui::SetNextItemWidth(80); reload |= ImGui::Combo("bpp", &siz, "16\0" "32\0");
  }
  if(reload || (autoR && tk.due(0.5)) || g_dirty) {
    Value a = A(); a.set("kind", kinds[kind]);
    if(custom) {
      a.set("addr", (u32)hexParse(addrBuf)); a.set("width", (u32)std::clamp(width, 1, 2048));
      a.set("height", (u32)std::clamp(height, 1, 2048)); a.set("siz", siz ? 3 : 2);
    }
    r = Q("dbg.fb", a);
    if(r.ok) tex.upload(r.blob.data(), (int)num(r.data.get("width")), (int)num(r.data.get("height")));
  }
  if(!r.ok) ImGui::TextColored(ImVec4(1, .4f, .4f, 1), "%s", r.err.c_str());
  else {
    const Value sc = r.data.get("scissor");
    const auto& s = arr(sc);
    ImGui::Text("%s @ %06llx  %llux%llu  %s bpp  scissor %s", kinds[kind], (unsigned long long)num(r.data.get("addr")),
                (unsigned long long)num(r.data.get("width")), (unsigned long long)num(r.data.get("height")),
                num(r.data.get("siz")) == 3 ? "32" : "16",
                s.size() == 4 ? (std::to_string(num(s[0])) + "," + std::to_string(num(s[1])) + "-" + std::to_string(num(s[2])) + "," + std::to_string(num(s[3]))).c_str() : "?");
  }
  if(ImGui::BeginChild("img", ImVec2(0, 0), 0, ImGuiWindowFlags_HorizontalScrollbar)) {
    int hx, hy;
    pixelImage(tex, zoom, hx, hy);
    if(hx >= 0 && (size_t)(hy * tex.w + hx) * 4 + 3 < r.blob.size()) {
      const u8* p = &r.blob[((size_t)hy * tex.w + hx) * 4];
      const u64 bpp = num(r.data.get("siz")) == 3 ? 4 : 2;
      ImGui::SetTooltip("(%d, %d)  rgb %u %u %u\nRDRAM %06llx", hx, hy, p[0], p[1], p[2],
                        (unsigned long long)(num(r.data.get("addr")) + ((u64)hy * tex.w + hx) * bpp));
    }
  }
  ImGui::EndChild();
  ImGui::End();
}

// TMEM -> RGBA segun formato/tamano del tile. Las lineas impares llevan las palabras de 32
// bits cruzadas (como las deja LOAD_BLOCK/LOAD_TILE) y RGBA32 parte cada texel entre la
// mitad baja (R,G) y la alta (B,A) de la TMEM, igual que hace el hardware.
auto decodeTmem(const std::vector<u8>& tm, u32 fmt, u32 size, u32 line, u32 tbase, u32 pal, bool tlut,
                int& W, int& H, std::vector<u8>& out) -> void {
  W = H = 0;
  out.clear();
  if(tm.size() < 4096 || line == 0) return;
  const u32 bits = 4u << size;
  const u32 rowBytes = line * 8;
  W = (int)(size == 3 ? rowBytes * 8 / 16 : rowBytes * 8 / bits);
  const u32 limit = (size == 3 || fmt == 2) ? 2048 : 4096;
  const u32 start = (tbase * 8) % 4096;
  H = (int)std::min<u32>(256, (limit > start ? limit - start : 0) / rowBytes);
  if(W <= 0 || H <= 0 || W > 1024) { W = H = 0; return; }
  out.assign((size_t)W * H * 4, 255);
  auto rd8 = [&](u32 a) -> u8 { return tm[a & 0xfff]; };
  auto rd16 = [&](u32 a) -> u32 { return (u32)rd8(a) << 8 | rd8(a + 1); };
  auto c5 = [](u32 v) -> u8 { return (u8)((v << 3) | (v >> 2)); };
  for(int y = 0; y < H; y++)
    for(int x = 0; x < W; x++) {
      const u32 row = start + (u32)y * rowBytes;
      const u32 sw = (y & 1) ? 4 : 0;
      u8 R = 0, G = 0, B = 0;
      auto rgba16 = [&](u32 v) { R = c5((v >> 11) & 31); G = c5((v >> 6) & 31); B = c5((v >> 1) & 31); };
      if(size == 3) {
        const u32 a = (row + (u32)x * 2) ^ sw;
        const u32 hi = rd16(a), lo = rd16(a + 2048);
        if(fmt == 0) { R = (u8)(hi >> 8); G = (u8)hi; B = (u8)(lo >> 8); }
        else { R = G = B = (u8)(hi >> 8); }
      } else if(size == 2) {
        const u32 v = rd16((row + (u32)x * 2) ^ sw);
        if(fmt == 0) rgba16(v);
        else if(fmt == 3) R = G = B = (u8)(v >> 8);
        else R = G = B = (u8)(v >> 8);
      } else {
        u32 idx;
        if(size == 1) idx = rd8((row + (u32)x) ^ sw);
        else { const u8 b = rd8((row + (u32)x / 2) ^ sw); idx = (x & 1) ? (b & 15) : (b >> 4); }
        if(fmt == 2 && tlut) {
          const u32 e = size == 1 ? idx : ((pal & 15) << 4 | idx);
          rgba16(rd16(0x800 + e * 8));
        } else if(fmt == 3 && size == 1) R = G = B = (u8)((idx >> 4) * 17);
        else if(fmt == 3) R = G = B = (u8)(((idx >> 1) & 7) * 255 / 7);
        else if(size == 1) R = G = B = (u8)idx;
        else R = G = B = (u8)(idx * 17);
      }
      u8* o = &out[((size_t)y * W + x) * 4];
      o[0] = R; o[1] = G; o[2] = B; o[3] = 255;
    }
}

auto panelTmem() -> void {
  static Reply r;
  static Tex tex;
  static int tile = 0, fmt = 0, size = 2, line = 8, tbase = 0, pal = 0;
  static bool tlut = true, fromTile = true;
  static float zoom = 3.f;
  static Ticker tk;
  if(!ImGui::Begin("TMEM", &showTmem)) { ImGui::End(); return; }
  bool reload = ImGui::Button("Leer");
  ImGui::SameLine(); ImGui::TextDisabled("Copia de SoftRDP: con parallel-RDP puede estar vieja.");
  if(reload || tk.due(1.0) || g_dirty) r = Q("dbg.tmem");
  if(!r.ok) { ImGui::TextColored(ImVec4(1, .4f, .4f, 1), "%s", r.err.c_str()); ImGui::End(); return; }
  const auto& tiles = arr(r.data.get("tiles"));
  static const char* fmts[] = {"RGBA", "YUV", "CI", "IA", "I"};
  if(ImGui::BeginTable("tiles", 9, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_SizingFixedFit)) {
    for(const char* h : {"tile", "fmt", "bpp", "line", "tmem", "pal", "S cm/mask/shift", "T cm/mask/shift", "sl,tl - sh,th"}) ImGui::TableSetupColumn(h);
    ImGui::TableHeadersRow();
    for(int i = 0; i < (int)tiles.size(); i++) {
      const Value& t = tiles[i];
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      char b[8]; std::snprintf(b, sizeof b, "%d", i);
      if(ImGui::Selectable(b, tile == i, ImGuiSelectableFlags_SpanAllColumns)) { tile = i; fromTile = true; }
      const u32 f = (u32)num(t.get("fmt"));
      ImGui::TableNextColumn(); ImGui::TextUnformatted(f < 5 ? fmts[f] : "?");
      ImGui::TableNextColumn(); ImGui::Text("%u", 4u << (u32)num(t.get("size")));
      ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)num(t.get("line")));
      ImGui::TableNextColumn(); ImGui::Text("0x%03llx", (unsigned long long)num(t.get("tmem")) * 8);
      ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)num(t.get("palette")));
      ImGui::TableNextColumn(); ImGui::Text("%llu/%llu/%llu", (unsigned long long)num(t.get("cmS")), (unsigned long long)num(t.get("maskS")), (unsigned long long)num(t.get("shiftS")));
      ImGui::TableNextColumn(); ImGui::Text("%llu/%llu/%llu", (unsigned long long)num(t.get("cmT")), (unsigned long long)num(t.get("maskT")), (unsigned long long)num(t.get("shiftT")));
      ImGui::TableNextColumn(); ImGui::Text("%.2f,%.2f - %.2f,%.2f", num(t.get("sl")) / 4.0, num(t.get("tl")) / 4.0, num(t.get("sh")) / 4.0, num(t.get("th")) / 4.0);
    }
    ImGui::EndTable();
  }
  if(fromTile && tile < (int)tiles.size()) {
    const Value& t = tiles[tile];
    fmt = (int)num(t.get("fmt")); size = (int)num(t.get("size")); line = (int)num(t.get("line"));
    tbase = (int)num(t.get("tmem")); pal = (int)num(t.get("palette"));
  }
  ImGui::SetNextItemWidth(80); if(ImGui::Combo("fmt", &fmt, fmts, 5)) fromTile = false;
  ImGui::SameLine(); ImGui::SetNextItemWidth(70); if(ImGui::Combo("bpp", &size, "4\0" "8\0" "16\0" "32\0")) fromTile = false;
  ImGui::SameLine(); ImGui::SetNextItemWidth(80); if(ImGui::InputInt("line", &line, 0)) fromTile = false;
  ImGui::SameLine(); ImGui::SetNextItemWidth(80); if(ImGui::InputInt("tmem/8", &tbase, 0)) fromTile = false;
  ImGui::SameLine(); ImGui::SetNextItemWidth(60); if(ImGui::InputInt("pal", &pal, 0)) fromTile = false;
  ImGui::SameLine(); ImGui::Checkbox("TLUT", &tlut);
  ImGui::SameLine(); ImGui::SetNextItemWidth(90); ImGui::SliderFloat("Zoom", &zoom, 1.f, 8.f, "%.0f");
  int W, H;
  std::vector<u8> px;
  decodeTmem(r.blob, (u32)fmt & 7, (u32)size & 3, (u32)std::max(0, line), (u32)std::max(0, tbase), (u32)pal, tlut, W, H, px);
  if(W > 0) tex.upload(px.data(), W, H);
  if(ImGui::BeginChild("timg", ImVec2(0, 0), 0, ImGuiWindowFlags_HorizontalScrollbar)) {
    int hx, hy;
    if(W > 0) pixelImage(tex, zoom, hx, hy); else ImGui::TextDisabled("line = 0: nada que pintar");
    if(W > 0 && hx >= 0) ImGui::SetTooltip("texel (%d, %d)", hx, hy);
  }
  ImGui::EndChild();
  ImGui::End();
}

auto panelTlb() -> void {
  static Reply r;
  static Ticker tk;
  if(!ImGui::Begin("TLB", &showTlb)) { ImGui::End(); return; }
  if(g_dirty || tk.due(1.0)) r = Q("cpu.tlb");
  if(!r.ok) { ImGui::TextColored(ImVec4(1, .4f, .4f, 1), "%s", r.err.c_str()); ImGui::End(); return; }
  if(ImGui::BeginTable("tlb", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit)) {
    ImGui::TableSetupScrollFreeze(0, 1);
    for(const char* h : {"#", "VPN2", "ASID", "G", "pagina", "PFN par (V D)", "PFN impar (V D)"}) ImGui::TableSetupColumn(h);
    ImGui::TableHeadersRow();
    for(const auto& e : arr(r.data.get("entries"))) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)num(e.get("i")));
      ImGui::TableNextColumn(); ImGui::Text("%010llx", (unsigned long long)num(e.get("vpn2")));
      ImGui::TableNextColumn(); ImGui::Text("%02llx", (unsigned long long)num(e.get("asid")));
      ImGui::TableNextColumn(); ImGui::Text("%s", e.get("global").b ? "G" : "");
      ImGui::TableNextColumn(); ImGui::Text("%llu KiB", (unsigned long long)num(e.get("pageSize")) / 1024);
      ImGui::TableNextColumn(); ImGui::Text("%08llx %c %c", (unsigned long long)num(e.get("pfn0")), e.get("v0").b ? 'V' : '-', e.get("d0").b ? 'D' : '-');
      ImGui::TableNextColumn(); ImGui::Text("%08llx %c %c", (unsigned long long)num(e.get("pfn1")), e.get("v1").b ? 'V' : '-', e.get("d1").b ? 'D' : '-');
    }
    ImGui::EndTable();
  }
  ImGui::End();
}

auto panelProf() -> void {
  static Reply r, rr;
  static int top = 30, metric = 0;
  static Ticker tk;
  if(!ImGui::Begin("Perfilador", &showProf)) { ImGui::End(); return; }
  if(ImGui::Button("Empezar")) Q("prof.start");
  ImGui::SameLine(); if(ImGui::Button("Parar")) Q("prof.stop");
  ImGui::SameLine(); if(ImGui::Button("Poner a cero")) Q("prof.reset");
  static const char* metrics[] = {"exec", "dmiss", "imiss", "dline"};
  ImGui::SameLine(); ImGui::SetNextItemWidth(90); ImGui::Combo("Metrica", &metric, metrics, 4);
  ImGui::SameLine(); ImGui::SetNextItemWidth(80); ImGui::InputInt("Top", &top, 0);
  if(tk.due(1.0)) {
    Value a = A(); a.set("top", std::clamp(top, 1, 500)); a.set("metric", metrics[metric]);
    r = Q("prof.cpu", a);
    Value b = A(); b.set("top", 20); rr = Q("prof.rsp", b);
  }
  if(!r.ok) ImGui::TextColored(ImVec4(1, .4f, .4f, 1), "%s", r.err.c_str());
  else {
    if(r.data.has("warn")) ImGui::TextColored(ImVec4(1, .8f, .3f, 1), "%s", r.data.get("warn").asString().c_str());
    if(ImGui::BeginTable("prof", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_Resizable,
                         ImVec2(0, ImGui::GetContentRegionAvail().y * 0.6f))) {
      ImGui::TableSetupScrollFreeze(0, 1);
      for(const char* h : {"VA", "%", "cuenta", "simbolo"}) ImGui::TableSetupColumn(h);
      ImGui::TableSetupColumn("instruccion", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableHeadersRow();
      for(const auto& e : arr(r.data.get("hot"))) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("%08llx", (unsigned long long)(u32)num(e.get("va")));
        ImGui::TableNextColumn(); ImGui::TextUnformatted(scalarText(e.get("pct")).c_str());
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)num(e.get("count")));
        ImGui::TableNextColumn(); ImGui::TextUnformatted(e.get("sym").asString().c_str());
        ImGui::TableNextColumn(); ImGui::TextUnformatted(e.get("disasm").asString().c_str());
      }
      ImGui::EndTable();
    }
  }
  if(ImGui::CollapsingHeader("RSP")) { if(rr.ok) jsonBody(rr.data); else ImGui::TextDisabled("%s", rr.err.c_str()); }
  ImGui::End();
}

auto panelEv() -> void {
  static Reply r;
  static int n = 200;
  static Ticker tk;
  if(!ImGui::Begin("Eventos RCP", &showEv)) { ImGui::End(); return; }
  bool on = r.data.get("on").b;
  if(ImGui::Checkbox("Grabar", &on)) { Value a = A(); a.set("on", on ? 1 : 0); a.set("n", 1); Q("ev.log", a); tk.last = -1e9; }
  ImGui::SameLine(); ImGui::SetNextItemWidth(80); ImGui::InputInt("Ultimos", &n, 0);
  if(tk.due(0.5)) { Value a = A(); a.set("n", std::clamp(n, 1, 16384)); r = Q("ev.log", a); }
  ImGui::SameLine(); ImGui::Text("total %llu", (unsigned long long)num(r.data.get("total")));
  if(ImGui::BeginTable("ev", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingFixedFit)) {
    ImGui::TableSetupScrollFreeze(0, 1);
    for(const char* h : {"reloj", "evento", "a", "b"}) ImGui::TableSetupColumn(h);
    ImGui::TableHeadersRow();
    const auto& evs = arr(r.data.get("events"));
    ImGuiListClipper clip;
    clip.Begin((int)evs.size());
    while(clip.Step())
      for(int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
        const auto& v = arr(evs[i]);
        if(v.size() < 4) continue;
        ImGui::TableNextRow();
        ImGui::TableNextColumn(); ImGui::Text("%llu", (unsigned long long)num(v[0]));
        ImGui::TableNextColumn(); ImGui::TextUnformatted(v[1].asString().c_str());
        ImGui::TableNextColumn(); ImGui::Text("%08llx", (unsigned long long)num(v[2]));
        ImGui::TableNextColumn(); ImGui::Text("%08llx", (unsigned long long)num(v[3]));
      }
    ImGui::EndTable();
  }
  ImGui::End();
}

auto panelUsb() -> void {
  static Reply r;
  static Ticker tk;
  static bool follow = true;
  if(!ImGui::Begin("USB (SC64)", &showUsb)) { ImGui::End(); return; }
  if(ImGui::Button("Vaciar")) { Value a = A(); a.set("clear", 1); Q("usb.log", a); tk.last = -1e9; }
  ImGui::SameLine(); ImGui::Checkbox("Seguir el final", &follow);
  if(tk.due(0.5)) r = Q("usb.log");
  ImGui::SameLine();
  ImGui::Text("%llu paquetes, %llu bytes", (unsigned long long)num(r.data.get("packets")), (unsigned long long)num(r.data.get("bytes")));
  if(ImGui::BeginChild("usbtxt", ImVec2(0, 0), ImGuiChildFlags_Borders)) {
    const std::string t = r.data.get("text").asString();
    ImGui::TextUnformatted(t.data(), t.data() + t.size());
    if(follow && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4) ImGui::SetScrollHereY(1.0f);
  }
  ImGui::EndChild();
  ImGui::End();
}

auto panelRcp() -> void {
  static Reply rc, rs;
  static bool vpr = false;
  static Ticker tk;
  if(!ImGui::Begin("Registros RCP / RSP", &showRcp)) { ImGui::End(); return; }
  ImGui::Checkbox("Registros vectoriales del RSP", &vpr);
  if(g_dirty || tk.due(0.5)) { rc = Q("rcp.regs"); Value a = A(); a.set("vpr", vpr); rs = Q("rsp.regs", a); }
  if(ImGui::CollapsingHeader("RCP", ImGuiTreeNodeFlags_DefaultOpen)) jsonBody(rc.data);
  if(ImGui::CollapsingHeader("RSP", ImGuiTreeNodeFlags_DefaultOpen)) jsonBody(rs.data);
  ImGui::End();
}

// Distribucion de fabrica, solo si el .ini no trae una: control y CPU a la izquierda,
// desensamblado y memoria en el centro, imagen y RDP a la derecha; el resto en pestanas.
auto defaultLayout(ImGuiID dock) -> void {
  if(ImGuiDockNode* n = ImGui::DockBuilderGetNode(dock); n && n->IsSplitNode()) return;
  ImGui::DockBuilderRemoveNode(dock);
  ImGui::DockBuilderAddNode(dock, ImGuiDockNodeFlags_DockSpace);
  ImGui::DockBuilderSetNodeSize(dock, ImGui::GetMainViewport()->WorkSize);
  ImGuiID left, rest, center, right;
  ImGui::DockBuilderSplitNode(dock, ImGuiDir_Left, 0.24f, &left, &rest);
  ImGui::DockBuilderSplitNode(rest, ImGuiDir_Right, 0.45f, &right, &center);
  ImGuiID leftTop, leftBot, cTop, cBot, rTop, rBot;
  ImGui::DockBuilderSplitNode(left, ImGuiDir_Up, 0.30f, &leftTop, &leftBot);
  ImGui::DockBuilderSplitNode(center, ImGuiDir_Up, 0.50f, &cTop, &cBot);
  ImGui::DockBuilderSplitNode(right, ImGuiDir_Up, 0.55f, &rTop, &rBot);
  ImGui::DockBuilderDockWindow("Control", leftTop);
  ImGui::DockBuilderDockWindow("CPU", leftBot);
  ImGui::DockBuilderDockWindow("Registros RCP / RSP", leftBot);
  ImGui::DockBuilderDockWindow("Desensamblado", cTop);
  ImGui::DockBuilderDockWindow("Perfilador", cTop);
  ImGui::DockBuilderDockWindow("Memoria", cBot);
  ImGui::DockBuilderDockWindow("TLB", cBot);
  ImGui::DockBuilderDockWindow("Eventos RCP", cBot);
  ImGui::DockBuilderDockWindow("USB (SC64)", cBot);
  ImGui::DockBuilderDockWindow("Framebuffer", rTop);
  ImGui::DockBuilderDockWindow("TMEM", rTop);
  ImGui::DockBuilderDockWindow("RDP: comandos del cuadro", rBot);
  ImGui::DockBuilderDockWindow("RSP: tareas del cuadro", rBot);
  ImGui::DockBuilderDockWindow("Linea de tiempo", rBot);
  ImGui::DockBuilderFinish(dock);
}

auto drawAll() -> void {
  if(ImGui::BeginMainMenuBar()) {
    if(ImGui::BeginMenu("Ventanas")) {
      ImGui::MenuItem("Control", nullptr, &showControl);
      ImGui::MenuItem("CPU", nullptr, &showCpu);
      ImGui::MenuItem("Desensamblado", nullptr, &showDisasm);
      ImGui::MenuItem("Memoria", nullptr, &showMem);
      ImGui::Separator();
      ImGui::MenuItem("RDP: comandos", nullptr, &showRdp);
      ImGui::MenuItem("RSP: tareas", nullptr, &showRsp);
      ImGui::MenuItem("Linea de tiempo", nullptr, &showTimeline);
      ImGui::MenuItem("Framebuffer", nullptr, &showFb);
      ImGui::MenuItem("TMEM", nullptr, &showTmem);
      ImGui::Separator();
      ImGui::MenuItem("TLB", nullptr, &showTlb);
      ImGui::MenuItem("Perfilador", nullptr, &showProf);
      ImGui::MenuItem("Eventos RCP", nullptr, &showEv);
      ImGui::MenuItem("USB (SC64)", nullptr, &showUsb);
      ImGui::MenuItem("Registros RCP / RSP", nullptr, &showRcp);
      ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
  }
  const bool dirty = g_dirty;
  if(showControl) panelControl();
  if(showCpu) panelCpu();
  if(showDisasm) panelDisasm();
  if(showMem) panelMem();
  if(showRdp) panelRdp();
  if(showRsp) panelRsp();
  if(showTimeline) panelTimeline();
  if(showFb) panelFb();
  if(showTmem) panelTmem();
  if(showTlb) panelTlb();
  if(showProf) panelProf();
  if(showEv) panelEv();
  if(showUsb) panelUsb();
  if(showRcp) panelRcp();
  // Lo que se marco sucio en ESTE cuadro lo leen todos en el siguiente; lo de antes ya esta.
  if(dirty) g_dirty = false;
}

// ------------------------------------------------------------------ D3D11 / ventana

auto createRtv() -> void {
  ID3D11Texture2D* back = nullptr;
  if(SUCCEEDED(g_sc->GetBuffer(0, IID_PPV_ARGS(&back))) && back) {
    g_dev->CreateRenderTargetView(back, nullptr, &g_rtv);
    back->Release();
  }
}

auto releaseDevice() -> void {
  if(g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
  if(g_point) { g_point->Release(); g_point = nullptr; }
  if(g_sc) { g_sc->Release(); g_sc = nullptr; }
  if(g_ctx) { g_ctx->Release(); g_ctx = nullptr; }
  if(g_dev) { g_dev->Release(); g_dev = nullptr; }
}

auto createDevice(HWND hwnd) -> bool {
  DXGI_SWAP_CHAIN_DESC sd{};
  sd.BufferCount = 2;
  sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
  sd.BufferDesc.RefreshRate.Numerator = 60;
  sd.BufferDesc.RefreshRate.Denominator = 1;
  sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  sd.OutputWindow = hwnd;
  sd.SampleDesc.Count = 1;
  sd.Windowed = TRUE;
  sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
  const D3D_FEATURE_LEVEL fls[2] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
  D3D_FEATURE_LEVEL fl;
  HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, fls, 2, D3D11_SDK_VERSION,
                                             &sd, &g_sc, &g_dev, &fl, &g_ctx);
  if(FAILED(hr))   // sin GPU utilizable: WARP (rasterizador por software de Windows)
    hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, fls, 2, D3D11_SDK_VERSION,
                                       &sd, &g_sc, &g_dev, &fl, &g_ctx);
  if(FAILED(hr)) return false;
  createRtv();
  D3D11_SAMPLER_DESC s{};
  s.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
  s.AddressU = s.AddressV = s.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  s.ComparisonFunc = D3D11_COMPARISON_ALWAYS;
  g_dev->CreateSamplerState(&s, &g_point);
  return true;
}

// KESTREL_DEBUGGER_SHOT=ruta.bmp: vuelca el backbuffer ya pintado a un BMP de 32 bits. Sirve
// para que un agente o una prueba vean la ventana sin depender de un escritorio visible
// (PrintWindow y la copia de pantalla no ven una cadena de intercambio D3D).
auto saveBackbuffer(const char* path) -> bool {
  ID3D11Texture2D* back = nullptr;
  if(FAILED(g_sc->GetBuffer(0, IID_PPV_ARGS(&back))) || !back) return false;
  D3D11_TEXTURE2D_DESC d;
  back->GetDesc(&d);
  d.Usage = D3D11_USAGE_STAGING; d.BindFlags = 0; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ; d.MiscFlags = 0;
  ID3D11Texture2D* st = nullptr;
  bool ok = false;
  if(SUCCEEDED(g_dev->CreateTexture2D(&d, nullptr, &st))) {
    g_ctx->CopyResource(st, back);
    D3D11_MAPPED_SUBRESOURCE m;
    if(SUCCEEDED(g_ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
      if(FILE* f = std::fopen(path, "wb")) {
        const u32 W = d.Width, H = d.Height, img = W * H * 4;
        u8 hdr[54] = {'B', 'M'};
        auto put32 = [&](int o, u32 v) { for(int i = 0; i < 4; i++) hdr[o + i] = (u8)(v >> (8 * i)); };
        put32(2, 54 + img); put32(10, 54); put32(14, 40); put32(18, W); put32(22, (u32)-(s32)H);
        hdr[26] = 1; hdr[28] = 32; put32(34, img);
        std::fwrite(hdr, 1, 54, f);
        std::vector<u8> row(W * 4);
        for(u32 y = 0; y < H; y++) {
          const u8* s = (const u8*)m.pData + (size_t)y * m.RowPitch;
          for(u32 x = 0; x < W; x++) { row[x * 4] = s[x * 4 + 2]; row[x * 4 + 1] = s[x * 4 + 1]; row[x * 4 + 2] = s[x * 4]; row[x * 4 + 3] = 255; }
          std::fwrite(row.data(), 1, row.size(), f);
        }
        std::fclose(f);
        ok = true;
      }
      g_ctx->Unmap(st, 0);
    }
    st->Release();
  }
  back->Release();
  return ok;
}

auto CALLBACK wndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) -> LRESULT {
  if(ImGui_ImplWin32_WndProcHandler(h, msg, wp, lp)) return true;
  switch(msg) {
    case WM_SIZE:
      if(wp != SIZE_MINIMIZED) { g_resizeW = LOWORD(lp); g_resizeH = HIWORD(lp); }
      return 0;
    case WM_SYSCOMMAND:
      if((wp & 0xfff0) == SC_KEYMENU) return 0;   // ALT no abre el menu de sistema
      break;
    case WM_CLOSE:
      g_quit = true;   // cerrar el depurador no cierra el emulador
      return 0;
  }
  return DefWindowProcW(h, msg, wp, lp);
}

auto exeDir() -> std::string {
  char p[MAX_PATH] = {};
  GetModuleFileNameA(nullptr, p, MAX_PATH);
  std::string s = p;
  const size_t k = s.find_last_of("\\/");
  return k == std::string::npos ? std::string() : s.substr(0, k + 1);
}

auto threadMain() -> void {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof wc;
  wc.style = CS_CLASSDC;
  wc.lpfnWndProc = wndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
  wc.lpszClassName = L"kestrel64_debugger";
  RegisterClassExW(&wc);
  HWND hwnd = CreateWindowW(wc.lpszClassName, L"kestrel64 - depurador", WS_OVERLAPPEDWINDOW, 60, 60, 1500, 950,
                            nullptr, nullptr, wc.hInstance, nullptr);
  if(!hwnd || !createDevice(hwnd)) {
    std::fprintf(stderr, "[debugger] no se pudo crear la ventana o el dispositivo D3D11\n");
    releaseDevice();
    if(hwnd) DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    g_run = false;
    return;
  }
  g_hwnd = hwnd;
  ShowWindow(hwnd, SW_SHOWDEFAULT);
  UpdateWindow(hwnd);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_DockingEnable;
  static std::string ini = exeDir() + "kestrel64_debugger.ini";
  io.IniFilename = ini.c_str();
  ImGui::StyleColorsDark();
  ImGui_ImplWin32_Init(hwnd);
  ImGui_ImplDX11_Init(g_dev, g_ctx);

  // Un dispositivo nuevo: las texturas de los paneles se crean de nuevo al primer uso.
  g_dirty = true;
  bool occluded = false;
  const char* shot = std::getenv("KESTREL_DEBUGGER_SHOT");
  int frames = 0;
  while(!g_quit.load()) {
    MSG msg;
    while(PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
      if(msg.message == WM_QUIT) g_quit = true;
    }
    if(g_quit.load()) break;
    if(g_sys && g_sys->shutdown.load()) break;
    if(occluded && !shot && g_sc->Present(0, DXGI_PRESENT_TEST) == DXGI_STATUS_OCCLUDED) { Sleep(20); continue; }
    occluded = false;
    if(g_resizeW) {
      if(g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
      g_sc->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
      g_resizeW = g_resizeH = 0;
      createRtv();
    }
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    static const ImGuiID dock = ImHashStr("kestrel64_dock");
    static bool laidOut = false;
    if(!laidOut) { defaultLayout(dock); laidOut = true; }
    ImGui::DockSpaceOverViewport(dock, ImGui::GetMainViewport());
    drawAll();
    ImGui::Render();
    const float clear[4] = {0.07f, 0.07f, 0.09f, 1.f};
    g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr);
    g_ctx->ClearRenderTargetView(g_rtv, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    // ~3 s de paneles refrescandose antes de la foto (cada panel pide a su ritmo).
    if(shot && *shot && ++frames == 180)
      std::fprintf(stderr, "[debugger] captura %s: %s\n", shot, saveBackbuffer(shot) ? "ok" : "FALLO");
    occluded = g_sc->Present(1, 0) == DXGI_STATUS_OCCLUDED;   // vsync: ~60 Hz de tope
  }

  ImGui_ImplDX11_Shutdown();
  ImGui_ImplWin32_Shutdown();
  ImGui::DestroyContext();
  releaseDevice();
  g_hwnd = nullptr;
  DestroyWindow(hwnd);
  UnregisterClassW(wc.lpszClassName, wc.hInstance);
  g_run = false;
}

}  // namespace

auto init(System& sys) -> void { g_sys = &sys; }

auto open() -> void {
  std::lock_guard<std::mutex> lk(g_mx);
  if(!g_sys) return;
  if(g_run.load()) {
    if(HWND h = g_hwnd.load()) { ShowWindow(h, SW_RESTORE); SetForegroundWindow(h); }
    return;
  }
  if(g_thr.joinable()) g_thr.join();
  g_quit = false;
  g_run = true;
  g_thr = std::thread(threadMain);
}

auto close() -> void {
  std::lock_guard<std::mutex> lk(g_mx);
  g_quit = true;
  if(HWND h = g_hwnd.load()) PostMessageW(h, WM_NULL, 0, 0);
  if(g_thr.joinable()) g_thr.join();
}

}  // namespace kestrel::dbgwin

#else   // !_WIN32

namespace kestrel::dbgwin {
auto init(System&) -> void {}
auto open() -> void {}
auto close() -> void {}
}  // namespace kestrel::dbgwin

#endif
