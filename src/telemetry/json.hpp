#pragma once
// Loom — minimal JSON for the telemetry protocol. Not a general-purpose library:
// enough to parse `{"id":N,"cmd":"...","args":{...}}` requests and build replies.

#include "../core/types.hpp"
#include <string>
#include <map>
#include <vector>
#include <memory>
#include <sstream>
#include <cstdlib>

namespace kestrel::json {

struct Value;
using Object = std::map<std::string, Value>;
using Array  = std::vector<Value>;

struct Value {
  enum class Type { Null, Bool, Number, String, Object, Array } type = Type::Null;
  bool        b = false;
  // Numbers keep exact 64-bit integers (register/address values must not lose
  // precision through a double). `isInt` picks the integer path; `sign` chooses
  // signed vs unsigned printing. Genuine fractions use `num`.
  bool        isInt = false;
  bool        sign  = false;
  u64         u = 0;
  double      num = 0;
  std::string str;
  std::shared_ptr<Object> obj;
  std::shared_ptr<Array>  arr;

  Value() = default;
  Value(bool v) : type(Type::Bool), b(v) {}
  Value(double v) : type(Type::Number), num(v) {}
  Value(int v) : type(Type::Number), isInt(true), sign(true),  u((u64)(s64)v) {}
  Value(u32 v) : type(Type::Number), isInt(true), sign(false), u(v) {}
  Value(u64 v) : type(Type::Number), isInt(true), sign(false), u(v) {}
  Value(s64 v) : type(Type::Number), isInt(true), sign(true),  u((u64)v) {}
  Value(const char* v) : type(Type::String), str(v) {}
  Value(const std::string& v) : type(Type::String), str(v) {}

  static auto object() -> Value { Value v; v.type = Type::Object; v.obj = std::make_shared<Object>(); return v; }
  static auto array()  -> Value { Value v; v.type = Type::Array;  v.arr = std::make_shared<Array>();  return v; }

  auto isObject() const -> bool { return type == Type::Object; }
  auto isNumber() const -> bool { return type == Type::Number; }
  auto isString() const -> bool { return type == Type::String; }

  // Object accessors (safe: return defaults if absent / wrong type).
  auto has(const std::string& k) const -> bool { return obj && obj->count(k); }
  auto get(const std::string& k) const -> Value {
    if(obj) { auto it = obj->find(k); if(it != obj->end()) return it->second; }
    return Value{};
  }
  auto asString(const std::string& def = "") const -> std::string { return type == Type::String ? str : def; }
  auto asU64(u64 def = 0) const -> u64 { return type == Type::Number ? (isInt ? u : (u64)num) : def; }
  auto asU32(u32 def = 0) const -> u32 { return type == Type::Number ? (isInt ? (u32)u : (u32)num) : def; }
  auto asInt(int def = 0) const -> int { return type == Type::Number ? (isInt ? (int)(s64)u : (int)num) : def; }

  auto& set(const std::string& k, Value v) { if(!obj){type=Type::Object;obj=std::make_shared<Object>();} (*obj)[k] = std::move(v); return *this; }
  auto& push(Value v) { if(!arr){type=Type::Array;arr=std::make_shared<Array>();} arr->push_back(std::move(v)); return *this; }
};

// --- serialization -----------------------------------------------------------
inline auto escape(const std::string& s, std::string& out) -> void {
  out += '"';
  for(char c : s) {
    switch(c) {
      case '"':  out += "\\\""; break;
      case '\\': out += "\\\\"; break;
      case '\n': out += "\\n";  break;
      case '\r': out += "\\r";  break;
      case '\t': out += "\\t";  break;
      default:
        if((unsigned char)c < 0x20) { char buf[8]; std::snprintf(buf, sizeof buf, "\\u%04x", c); out += buf; }
        else out += c;
    }
  }
  out += '"';
}

inline auto serialize(const Value& v, std::string& out) -> void {
  using T = Value::Type;
  switch(v.type) {
    case T::Null:   out += "null"; break;
    case T::Bool:   out += v.b ? "true" : "false"; break;
    case T::Number: {
      if(v.isInt) { out += v.sign ? std::to_string((s64)v.u) : std::to_string(v.u); }
      else {
        double d = v.num;
        if(d == (double)(s64)d) { out += std::to_string((s64)d); }  // integers without .0
        else { char buf[32]; std::snprintf(buf, sizeof buf, "%.10g", d); out += buf; }
      }
      break;
    }
    case T::String: escape(v.str, out); break;
    case T::Object: {
      out += '{'; bool first = true;
      if(v.obj) for(auto& [k, val] : *v.obj) { if(!first) out += ','; first = false; escape(k, out); out += ':'; serialize(val, out); }
      out += '}'; break;
    }
    case T::Array: {
      out += '['; bool first = true;
      if(v.arr) for(auto& val : *v.arr) { if(!first) out += ','; first = false; serialize(val, out); }
      out += ']'; break;
    }
  }
}

inline auto dump(const Value& v) -> std::string { std::string s; serialize(v, s); return s; }

// --- parsing -----------------------------------------------------------------
struct Parser {
  const char* p; const char* end; bool ok = true;
  explicit Parser(const std::string& s) : p(s.data()), end(s.data() + s.size()) {}

  auto ws() -> void { while(p < end && (*p==' '||*p=='\t'||*p=='\n'||*p=='\r')) p++; }

  auto parse() -> Value { ws(); Value v = value(); ws(); return v; }

  auto value() -> Value {
    ws();
    if(p >= end) { ok = false; return {}; }
    char c = *p;
    if(c == '{') return object();
    if(c == '[') return array();
    if(c == '"') return Value{string()};
    if(c == 't') { if(match("true"))  return Value{true};  ok=false; return {}; }
    if(c == 'f') { if(match("false")) return Value{false}; ok=false; return {}; }
    if(c == 'n') { if(match("null"))  return Value{};       ok=false; return {}; }
    return number();
  }

  auto match(const char* lit) -> bool {
    const char* q = p;
    while(*lit) { if(q >= end || *q != *lit) return false; q++; lit++; }
    p = q; return true;
  }

  auto string() -> std::string {
    std::string s; p++;  // opening quote
    while(p < end && *p != '"') {
      char c = *p++;
      if(c == '\\' && p < end) {
        char e = *p++;
        switch(e) {
          case 'n': s += '\n'; break; case 't': s += '\t'; break;
          case 'r': s += '\r'; break; case '"': s += '"'; break;
          case '\\': s += '\\'; break; case '/': s += '/'; break;
          case 'u': { if(p+4<=end){ char hx[5]={p[0],p[1],p[2],p[3],0}; long cp=strtol(hx,nullptr,16); p+=4; if(cp<0x80) s+=(char)cp; else s+='?'; } break; }
          default: s += e;
        }
      } else s += c;
    }
    if(p < end) p++;  // closing quote
    return s;
  }

  auto number() -> Value {
    const char* start = p;
    bool isFloat = false;
    while(p < end && (*p=='-'||*p=='+'||*p=='.'||*p=='e'||*p=='E'||(*p>='0'&&*p<='9'))) {
      if(*p=='.'||*p=='e'||*p=='E') isFloat = true;
      p++;
    }
    std::string tok(start, p);
    Value v; v.type = Value::Type::Number;
    if(isFloat) { v.num = std::strtod(tok.c_str(), nullptr); }
    else if(!tok.empty() && tok[0] == '-') { v.isInt = true; v.sign = true;  v.u = (u64)std::strtoll(tok.c_str(), nullptr, 10); }
    else                                    { v.isInt = true; v.sign = false; v.u = std::strtoull(tok.c_str(), nullptr, 10); }
    return v;
  }

  auto object() -> Value {
    Value v = Value::object(); p++;  // '{'
    ws(); if(p < end && *p == '}') { p++; return v; }
    while(p < end) {
      ws(); if(*p != '"') { ok = false; break; }
      std::string key = string(); ws();
      if(p >= end || *p != ':') { ok = false; break; }
      p++;
      (*v.obj)[key] = value(); ws();
      if(p < end && *p == ',') { p++; continue; }
      if(p < end && *p == '}') { p++; break; }
      ok = false; break;
    }
    return v;
  }

  auto array() -> Value {
    Value v = Value::array(); p++;  // '['
    ws(); if(p < end && *p == ']') { p++; return v; }
    while(p < end) {
      v.arr->push_back(value()); ws();
      if(p < end && *p == ',') { p++; continue; }
      if(p < end && *p == ']') { p++; break; }
      ok = false; break;
    }
    return v;
  }
};

inline auto parse(const std::string& s, bool& ok) -> Value {
  Parser pr(s); Value v = pr.parse(); ok = pr.ok; return v;
}

}  // namespace kestrel::json
