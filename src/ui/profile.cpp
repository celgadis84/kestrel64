#include "profile.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>

#ifdef _WIN32
#include <windows.h>
#endif

namespace kestrel::ui {

// NOTA de estilo: en este fichero no se escribe casi nunca una barra invertida literal (se
// usa el valor 92 y separadores '/'), porque las rutas de Windows aceptan la barra normal.

auto findOption(const char* id) -> const Option* {
  for(int c = 0; c < categoryCount(); c++) {
    const Category& cat = categories()[c];
    for(int i = 0; i < cat.n; i++)
      if(!std::strcmp(cat.opts[i].id, id)) return &cat.opts[i];
  }
  return nullptr;
}

auto Profile::get(const char* id) const -> std::string {
  auto it = v.find(id);
  if(it != v.end()) return it->second;
  if(const Option* o = findOption(id)) return o->def ? o->def : "";
  return {};
}

auto Profile::getBool(const char* id) const -> bool {
  std::string s = get(id);
  return !s.empty() && s != "0" && s != "false";
}

// ------------------------------------------------------------------ ruta y defaults

auto profilePath() -> std::string {
  // El lanzador dice donde vive el perfil: instalado es %LOCALAPPDATA%/kestrel64, pero en
  // arbol de desarrollo es tools/launcher/. Sin esto, el menu de la ventana escribiria en un
  // fichero distinto del que lee el lanzador y las dos caras del programa se desincronizan.
  if(const char* p = std::getenv("KESTREL_PROFILE")) if(*p) return p;
#ifdef _WIN32
  const char* base = std::getenv("LOCALAPPDATA");
  std::string dir = base && *base ? std::string(base) + "/kestrel64" : std::string(".");
  CreateDirectoryA(dir.c_str(), nullptr);
  return dir + "/profile.json";
#else
  const char* home = std::getenv("HOME");
  return (home ? std::string(home) : std::string(".")) + "/.kestrel64-profile.json";
#endif
}

auto defaultProfile() -> Profile {
  Profile p;
  for(int c = 0; c < categoryCount(); c++) {
    const Category& cat = categories()[c];
    for(int i = 0; i < cat.n; i++) p.v[cat.opts[i].id] = cat.opts[i].def ? cat.opts[i].def : "";
  }
  for(int i = 0; i < padControlCount(); i++) {
    const PadCtl& b = padControls()[i];
    p.pad[b.id] = {b.key ? b.key : "", b.gp ? b.gp : ""};
  }
  return p;
}

// ------------------------------------------------------------------ JSON minimo
// Solo hace falta un objeto plano de valores escalares mas un objeto "pad" de un nivel: es
// lo unico que escribe el lanzador. Cualquier cosa mas rara se ignora en silencio, que es
// justo lo que quiere un fichero de preferencias (nunca impedir el arranque).

namespace {

struct Jp {
  const char* s;
  auto ws() -> void { while(*s == ' ' || *s == 9 || *s == 10 || *s == 13) s++; }
  auto lit(char c) -> bool { ws(); if(*s == c) { s++; return true; } return false; }

  // Devuelve el texto de un valor escalar. Las cadenas pierden las comillas; true/false se
  // vuelven "1"/"0" para que el resto del codigo trate todo como texto.
  auto value(std::string& out) -> bool {
    ws();
    if(*s == '"') {
      s++;
      out.clear();
      while(*s && *s != '"') {
        if(*s == (char)92 && s[1]) {
          s++;
          char c = *s++;
          out += (c == 'n') ? (char)10 : (c == 't') ? (char)9 : c;
        } else out += *s++;
      }
      if(*s == '"') s++;
      return true;
    }
    if(!std::strncmp(s, "true", 4))  { s += 4; out = "1"; return true; }
    if(!std::strncmp(s, "false", 5)) { s += 5; out = "0"; return true; }
    if(!std::strncmp(s, "null", 4))  { s += 4; out = "";  return true; }
    const char* b = s;
    while(*s && !std::strchr(",}] \n\r\t", *s)) s++;
    if(s == b) return false;
    out.assign(b, s - b);
    return true;
  }

  // Se salta un valor entero, sea del tipo que sea (para las claves que no interesan).
  auto skip() -> void {
    ws();
    if(*s == '{' || *s == '[') {
      char open = *s, close = open == '{' ? '}' : ']';
      int d = 0;
      while(*s) {
        if(*s == '"') { std::string t; value(t); continue; }
        if(*s == open) { d++; s++; continue; }
        if(*s == close) { d--; s++; if(!d) return; continue; }
        s++;
      }
      return;
    }
    std::string t;
    value(t);
  }
};

}  // namespace

auto loadProfile() -> Profile {
  Profile p = defaultProfile();
  std::FILE* f = std::fopen(profilePath().c_str(), "rb");
  if(!f) return p;
  std::string txt;
  char buf[4096];
  size_t n;
  while((n = std::fread(buf, 1, sizeof buf, f)) > 0) txt.append(buf, n);
  std::fclose(f);

  Jp j{txt.c_str()};
  if(!j.lit('{')) return p;
  while(true) {
    j.ws();
    if(*j.s == '}' || !*j.s) break;
    std::string key;
    if(!j.value(key)) break;
    if(!j.lit(':')) break;
    j.ws();
    if(key == "pad" && *j.s == '{') {
      j.lit('{');
      while(true) {
        j.ws();
        if(*j.s == '}' || !*j.s) { j.lit('}'); break; }
        std::string id;
        if(!j.value(id)) break;
        if(!j.lit(':')) break;
        j.ws();
        std::string k, g;
        if(*j.s == '{') {
          j.lit('{');
          while(true) {
            j.ws();
            if(*j.s == '}' || !*j.s) { j.lit('}'); break; }
            std::string f2, v2;
            if(!j.value(f2)) break;
            if(!j.lit(':')) break;
            if(!j.value(v2)) break;
            if(f2 == "key") k = v2;
            else if(f2 == "gp") g = v2;
            j.lit(',');
          }
        } else {
          j.skip();
        }
        p.pad[id] = {k, g};
        j.lit(',');
      }
    } else if(*j.s == '{' || *j.s == '[') {
      j.skip();
    } else {
      std::string val;
      if(!j.value(val)) break;
      p.v[key] = val;
    }
    if(!j.lit(',')) break;
  }
  return p;
}

namespace {

auto jesc(const std::string& s) -> std::string {
  std::string o;
  for(char c : s) {
    if(c == '"' || c == (char)92) { o += (char)92; o += c; }
    else if(c == (char)10) { o += (char)92; o += 'n'; }
    else o += c;
  }
  return o;
}

// Un valor de texto se escribe como el tipo JSON que le toca: el lanzador (Python) espera
// bool de verdad en los booleanos y numero en los numericos, no la cadena "1".
auto jval(const Option* o, const std::string& s) -> std::string {
  if(o) {
    if(o->type == OType::Bool)
      return (!s.empty() && s != "0" && s != "false") ? "true" : "false";
    if(o->type == OType::Int || o->type == OType::Float)
      return s.empty() ? std::string(o->def ? o->def : "0") : s;
  }
  return "\"" + jesc(s) + "\"";
}

}  // namespace

auto saveProfile(const Profile& p) -> bool {
  std::string path = profilePath();
  std::string tmp = path + ".tmp";
  std::FILE* f = std::fopen(tmp.c_str(), "wb");
  if(!f) return false;
  std::fprintf(f, "{\n");
  bool first = true;
  for(const auto& kv : p.v) {
    std::fprintf(f, "%s  \"%s\": %s", first ? "" : ",\n", jesc(kv.first).c_str(),
                 jval(findOption(kv.first.c_str()), kv.second).c_str());
    first = false;
  }
  std::fprintf(f, "%s  \"pad\": {\n", first ? "" : ",\n");
  bool pf = true;
  for(const auto& kv : p.pad) {
    std::fprintf(f, "%s    \"%s\": {\"key\": \"%s\", \"gp\": \"%s\"}", pf ? "" : ",\n",
                 jesc(kv.first).c_str(), jesc(kv.second.first).c_str(),
                 jesc(kv.second.second).c_str());
    pf = false;
  }
  std::fprintf(f, "\n  }\n}\n");
  std::fclose(f);
#ifdef _WIN32
  // Reemplazo atomico: si el proceso muere a mitad, el perfil viejo sigue entero.
  return MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) != 0;
#else
  std::remove(path.c_str());
  return std::rename(tmp.c_str(), path.c_str()) == 0;
#endif
}

// ------------------------------------------------------------------ perfil -> entorno

auto writePadFile(const Profile& p) -> std::string {
  if(p.pad.empty()) return {};
  std::string dir = profilePath();
  size_t slash = dir.find_last_of('/');
  dir = slash == std::string::npos ? std::string(".") : dir.substr(0, slash);
  std::string path = dir + "/pad1.cfg";
  std::FILE* f = std::fopen(path.c_str(), "w");
  if(!f) return {};
  for(const auto& kv : p.pad) {
    std::fprintf(f, "%s %s %s\n", kv.first.c_str(),
                 kv.second.first.empty() ? "-" : kv.second.first.c_str(),
                 kv.second.second.empty() ? "-" : kv.second.second.c_str());
  }
  std::fclose(f);
  return path;
}

auto toEnv(const Profile& p, std::vector<std::pair<std::string, std::string>>& env,
           std::vector<std::string>& argv) -> void {
  auto put = [&](const char* k, const std::string& val) {
    for(auto& e : env) if(e.first == k) { e.second = val; return; }
    env.emplace_back(k, val);
  };
  auto drop = [&](const char* k) {
    for(size_t i = 0; i < env.size(); i++)
      if(env[i].first == k) { env.erase(env.begin() + (long)i); return; }
  };

  for(int c = 0; c < categoryCount(); c++) {
    const Category& cat = categories()[c];
    for(int i = 0; i < cat.n; i++) {
      const Option& o = cat.opts[i];
      if(!o.env) continue;
      std::string val = p.get(o.id);
      switch(o.type) {
        case OType::Bool: {
          bool on = !val.empty() && val != "0" && val != "false";
          if(o.invert) { if(!on) put(o.env, "1"); }
          else if(o.tri) put(o.env, on ? "1" : "0");
          else if(on) put(o.env, "1");
          break;
        }
        case OType::Int: {
          long long n = std::strtoll(val.c_str(), nullptr, 10);
          long long d = std::strtoll(o.def ? o.def : "0", nullptr, 10);
          if(n != d) put(o.env, std::to_string(n));
          break;
        }
        case OType::Float: {
          double x = val.empty() ? std::strtod(o.def ? o.def : "0", nullptr)
                                 : std::strtod(val.c_str(), nullptr);
          double d = std::strtod(o.def ? o.def : "0", nullptr);
          if(std::fabs(x - d) > 1e-9) {
            char b[32];
            std::snprintf(b, sizeof b, "%.4f", x);
            std::string s(b);
            while(s.size() > 1 && s.back() == '0') s.pop_back();
            if(!s.empty() && s.back() == '.') s.pop_back();
            put(o.env, s);
          }
          break;
        }
        case OType::Choice:
          if(!val.empty() && val != "auto") put(o.env, val);
          break;
        default:
          if(!val.empty()) put(o.env, val);
          break;
      }
    }
  }

  if(p.getBool("oc_link")) {
    drop("KESTREL_OC_CPU"); drop("KESTREL_OC_RSP"); drop("KESTREL_OC_RDRAM");
  } else {
    drop("KESTREL_OC");
  }
  if(p.get("throttle") == "auto") drop("KESTREL_THROTTLE");

  std::string plug = p.get("plugin");
  if(plug == "prdp") put("KESTREL_PRDP", "1");
  else if(plug == "soft") put("KESTREL_PRDP", "0");

  // Relanzarse siempre es "modo usuario": corriendo Y con ventana, salvo que se pida pausa
  // o se apague el video a proposito.
  bool video = p.getBool("video");
  if(!p.getBool("paused")) {
    argv.push_back(video ? "--play" : "--run");
    if(video) put("KESTREL_VIDEO", "1");
    else { drop("KESTREL_VIDEO"); put("KESTREL_NOVIDEO", "1"); }
  } else if(!video) {
    put("KESTREL_NOVIDEO", "1");
  }

  std::string port = p.get("port");
  if(port.empty()) port = "9128";
  argv.push_back("--port");
  argv.push_back(port);

  std::string padFile = writePadFile(p);
  if(!padFile.empty()) put("KESTREL_PAD1", padFile);
}

}  // namespace kestrel::ui
