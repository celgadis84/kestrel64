#include "dbg.hpp"
#include <cstdio>

namespace kestrel::dbg {

State g;

namespace {

// Palabra de 64 bits en orden del invitado (big-endian), acotada como rd64p de rdp.cpp.
inline auto rd64be(const u8* m, u32 a, u32 n) -> u64 {
  if(a + 7 >= n) return 0;
  u64 v = 0;
  for(int i = 0; i < 8; i++) v = (v << 8) | m[a + i];
  return v;
}

}  // namespace

auto State::enable(bool v) -> void {
  std::lock_guard<std::mutex> lk(mx);
  if(v && !on.load()) { cur.clear(); last.clear(); }
  on.store(v, std::memory_order_release);
}

auto State::rdpJob(const u8* src, u32 srcSize, const u8* dmem, u32 start, u32 end, bool xbus, u64 now) -> void {
  if(!on.load(std::memory_order_relaxed)) return;
  std::lock_guard<std::mutex> lk(mx);
  auto fetch = [&](u32 a) -> u64 {
    if(!xbus) return rd64be(src, a, srcSize);
    u64 v = 0;
    for(int i = 0; i < 8; i++) v = (v << 8) | dmem[(a + i) & 0xfff];
    return v;
  };
  // Mismo enmascarado que SoftRdp::run: 24 bits en RDRAM, offsets de DMEM sin recortar.
  u32 a  = xbus ? (start & 0x0fff'ffffu) : (start & 0x00ff'ffffu);
  u32 e  = xbus ? (end   & 0x0fff'ffffu) : (end   & 0x00ff'ffffu);
  RdpJob job;
  job.start = a; job.end = e; job.t0 = now; job.t1 = now; job.xbus = xbus;
  job.cmd0 = (u32)cur.cmds.size();
  const u32 jobIdx = (u32)cur.jobs.size();
  while(a + 8 <= e) {
    const u64 w0 = fetch(a);
    const u32 op = (u32)(w0 >> 56) & 0x3f;
    const u32 n = rdpCmdWords(op);
    // Comando partido por el borde del tramo: lo graba el tramo siguiente, que reanuda
    // justo aqui (Memory::rdpRunJob/rdpResume).
    if(a + n * 8 > e) break;
    if(cur.cmds.size() >= kMaxCmds) { cur.truncated++; a += n * 8; continue; }
    RdpCmd c;
    c.addr = a; c.op = (u8)op; c.nwords = (u8)n; c.xbus = xbus; c.job = jobIdx;
    c.word0 = (u32)cur.words.size();
    cur.words.push_back(w0);
    for(u32 i = 1; i < n; i++) cur.words.push_back(fetch(a + i * 8));
    cur.cmds.push_back(c);
    a += n * 8;
  }
  job.ncmds = (u32)cur.cmds.size() - job.cmd0;
  cur.jobs.push_back(job);
}

auto State::rdpJobEnd(u64 now) -> void {
  if(!on.load(std::memory_order_relaxed)) return;
  std::lock_guard<std::mutex> lk(mx);
  if(!cur.jobs.empty()) cur.jobs.back().t1 = now;
}

auto State::rspStart(const u8* dmem, u32 pc, u64 now) -> void {
  if(!on.load(std::memory_order_relaxed)) return;
  auto dm = [&](u32 o) -> u32 {
    return ((u32)dmem[o] << 24) | ((u32)dmem[o + 1] << 16) | ((u32)dmem[o + 2] << 8) | dmem[o + 3];
  };
  RspTask t;
  t.type = dm(0xfc0); t.flags = dm(0xfc4);
  t.ucode = dm(0xfd0); t.ucodeData = dm(0xfd8);
  t.dataPtr = dm(0xff0); t.dataSize = dm(0xff4);
  t.pc = pc & 0xfff; t.t0 = now; t.t1 = now;
  std::lock_guard<std::mutex> lk(mx);
  if(cur.tasks.size() < 4096) cur.tasks.push_back(t);
}

auto State::rspEnd(u64 cycles, u64 now) -> void {
  if(!on.load(std::memory_order_relaxed)) return;
  std::lock_guard<std::mutex> lk(mx);
  for(auto it = cur.tasks.rbegin(); it != cur.tasks.rend(); ++it)
    if(!it->done) { it->done = true; it->cycles = cycles; it->t1 = now; return; }
  // La tarea empezo en el cuadro anterior: se cierra alli si sigue abierta.
  for(auto it = last.tasks.rbegin(); it != last.tasks.rend(); ++it)
    if(!it->done) { it->done = true; it->cycles = cycles; it->t1 = now; return; }
}

auto State::flip(u32 flips, u64 now) -> void {
  if(!on.load(std::memory_order_relaxed)) return;
  std::lock_guard<std::mutex> lk(mx);
  cur.t1 = now;
  if(!freeze.load(std::memory_order_relaxed)) {
    std::swap(last, cur);
  }
  cur.clear();
  cur.number = flips; cur.t0 = now; cur.t1 = now;
  framesClosed++;
}

auto State::snapshot(int which) -> Frame {
  std::lock_guard<std::mutex> lk(mx);
  return which ? cur : last;
}

// --- decodificador ---------------------------------------------------------------------------

auto rdpCmdWords(u32 op) -> u32 {
  op &= 0x3f;
  if(op >= 0x08 && op <= 0x0f)   // triangulo: aristas 4 + sombreado 8 + textura 8 + z 2
    return 4 + ((op & 4) ? 8 : 0) + ((op & 2) ? 8 : 0) + ((op & 1) ? 2 : 0);
  if(op == 0x24 || op == 0x25) return 2;   // TEXTURE_RECTANGLE(_FLIP): 128 bits
  return 1;
}

auto rdpCmdName(u32 op) -> const char* {
  switch(op & 0x3f) {
  case 0x00: return "NOP";
  case 0x08: return "TRI_FILL";
  case 0x09: return "TRI_FILL_Z";
  case 0x0a: return "TRI_TEX";
  case 0x0b: return "TRI_TEX_Z";
  case 0x0c: return "TRI_SHADE";
  case 0x0d: return "TRI_SHADE_Z";
  case 0x0e: return "TRI_SHADE_TEX";
  case 0x0f: return "TRI_SHADE_TEX_Z";
  case 0x24: return "TEXTURE_RECTANGLE";
  case 0x25: return "TEXTURE_RECTANGLE_FLIP";
  case 0x26: return "SYNC_LOAD";
  case 0x27: return "SYNC_PIPE";
  case 0x28: return "SYNC_TILE";
  case 0x29: return "SYNC_FULL";
  case 0x2a: return "SET_KEY_GB";
  case 0x2b: return "SET_KEY_R";
  case 0x2c: return "SET_CONVERT";
  case 0x2d: return "SET_SCISSOR";
  case 0x2e: return "SET_PRIM_DEPTH";
  case 0x2f: return "SET_OTHER_MODES";
  case 0x30: return "LOAD_TLUT";
  case 0x32: return "SET_TILE_SIZE";
  case 0x33: return "LOAD_BLOCK";
  case 0x34: return "LOAD_TILE";
  case 0x35: return "SET_TILE";
  case 0x36: return "FILL_RECTANGLE";
  case 0x37: return "SET_FILL_COLOR";
  case 0x38: return "SET_FOG_COLOR";
  case 0x39: return "SET_BLEND_COLOR";
  case 0x3a: return "SET_PRIM_COLOR";
  case 0x3b: return "SET_ENV_COLOR";
  case 0x3c: return "SET_COMBINE_MODE";
  case 0x3d: return "SET_TEXTURE_IMAGE";
  case 0x3e: return "SET_Z_IMAGE";
  case 0x3f: return "SET_COLOR_IMAGE";
  default:   return "INVALID";
  }
}

auto rdpIsPrimitive(u32 op) -> bool {
  op &= 0x3f;
  return (op >= 0x08 && op <= 0x0f) || op == 0x24 || op == 0x25 || op == 0x36;
}

namespace {

const char* kFmt[8] = {"RGBA", "YUV", "CI", "IA", "I", "?5", "?6", "?7"};
const char* kSiz[4] = {"4b", "8b", "16b", "32b"};

inline auto bits(u64 w, u32 hi, u32 lo) -> u32 { return (u32)((w >> lo) & ((1ull << (hi - lo + 1)) - 1)); }

// Coordenada 10.2 sin signo como texto ("12.25").
auto fx102(u32 v) -> std::string {
  char b[24]; std::snprintf(b, sizeof b, "%u.%02u", v >> 2, (v & 3) * 25); return b;
}
// s11.2 (14 bits con signo) de las Y de un triangulo.
auto fy112(u32 v) -> std::string {
  s32 s = (s32)(v << 18) >> 18;
  char b[24]; std::snprintf(b, sizeof b, "%.2f", s / 4.0); return b;
}
// s15.16 de las X/pendientes de un triangulo.
auto f1616(u32 v) -> std::string {
  char b[24]; std::snprintf(b, sizeof b, "%.3f", (s32)v / 65536.0); return b;
}

const char* kCSubA[16] = {"COMBINED","TEX0","TEX1","PRIM","SHADE","ENV","1","NOISE","0","0","0","0","0","0","0","0"};
const char* kCSubB[16] = {"COMBINED","TEX0","TEX1","PRIM","SHADE","ENV","KEYCENTER","K4","0","0","0","0","0","0","0","0"};
const char* kCMul[32]  = {"COMBINED","TEX0","TEX1","PRIM","SHADE","ENV","KEYSCALE","COMBINED_A",
                          "TEX0_A","TEX1_A","PRIM_A","SHADE_A","ENV_A","LOD_FRAC","PRIM_LOD_FRAC","K5",
                          "0","0","0","0","0","0","0","0","0","0","0","0","0","0","0","0"};
const char* kCAdd[8]   = {"COMBINED","TEX0","TEX1","PRIM","SHADE","ENV","1","0"};
const char* kASub[8]   = {"COMBINED","TEX0","TEX1","PRIM","SHADE","ENV","1","0"};
const char* kAMul[8]   = {"LOD_FRAC","TEX0","TEX1","PRIM","SHADE","ENV","PRIM_LOD_FRAC","0"};

}  // namespace

auto combinerText(u64 w) -> std::string {
  std::string s;
  char b[256];
  // Ciclo 0 y ciclo 1, color y alfa: (A - B) * C + D.
  std::snprintf(b, sizeof b, "c0 rgb=(%s-%s)*%s+%s a=(%s-%s)*%s+%s | c1 rgb=(%s-%s)*%s+%s a=(%s-%s)*%s+%s",
    kCSubA[bits(w,55,52)], kCSubB[bits(w,31,28)], kCMul[bits(w,51,47)], kCAdd[bits(w,17,15)],
    kASub[bits(w,46,44)],  kASub[bits(w,14,12)],  kAMul[bits(w,43,41)], kASub[bits(w,11,9)],
    kCSubA[bits(w,40,37)], kCSubB[bits(w,27,24)], kCMul[bits(w,36,32)], kCAdd[bits(w,8,6)],
    kASub[bits(w,23,21)],  kASub[bits(w,5,3)],    kAMul[bits(w,20,18)], kASub[bits(w,2,0)]);
  s = b;
  return s;
}

auto otherModesText(u64 w) -> std::string {
  static const char* cyc[4] = {"1CYC", "2CYC", "COPY", "FILL"};
  static const char* zm[4]  = {"OPA", "INTER", "XLU", "DECAL"};
  static const char* cd[4]  = {"CLAMP", "WRAP", "ZAP", "SAVE"};
  static const char* bp[4]  = {"IN", "MEM", "BLEND", "FOG"};
  static const char* ba[4]  = {"IN_A", "FOG_A", "SHADE_A", "0"};
  static const char* bb[4]  = {"1-A", "MEM_A", "1", "0"};
  std::string s = cyc[bits(w,53,52)];
  auto flag = [&](u32 bit, const char* n) { if((w >> bit) & 1) { s += ' '; s += n; } };
  flag(51, "persp"); flag(50, "detail"); flag(49, "sharpen"); flag(48, "lod");
  flag(47, "tlut"); if((w >> 47) & 1) s += ((w >> 46) & 1) ? "(IA16)" : "(RGBA16)";
  flag(45, "bilerp-sample"); flag(44, "mid-texel"); flag(43, "bilerp0"); flag(42, "bilerp1");
  flag(41, "convert-one"); flag(40, "key");
  char b[200];
  std::snprintf(b, sizeof b, " | blend c0=(%s*%s+%s*%s) c1=(%s*%s+%s*%s)",
    bp[bits(w,31,30)], ba[bits(w,27,26)], bp[bits(w,23,22)], bb[bits(w,19,18)],
    bp[bits(w,29,28)], ba[bits(w,25,24)], bp[bits(w,21,20)], bb[bits(w,17,16)]);
  s += b;
  flag(14, "force-blend"); flag(13, "alpha-cvg-sel"); flag(12, "cvg*alpha");
  s += " zmode="; s += zm[bits(w,11,10)];
  s += " cvgdest="; s += cd[bits(w,9,8)];
  flag(7, "color-on-cvg"); flag(6, "image-read"); flag(5, "z-upd"); flag(4, "z-cmp");
  flag(3, "aa"); flag(2, "z-prim"); flag(1, "dither-alpha"); flag(0, "alpha-cmp");
  return s;
}

auto rdpDecode(const u64* w, u32 n) -> std::string {
  const u64 c = w[0];
  const u32 op = (u32)(c >> 56) & 0x3f;
  std::string s = rdpCmdName(op);
  char b[256];
  switch(op) {
  case 0x08: case 0x09: case 0x0a: case 0x0b: case 0x0c: case 0x0d: case 0x0e: case 0x0f: {
    std::snprintf(b, sizeof b, " %s tile=%u lvl=%u yl=%s ym=%s yh=%s",
      bits(c,55,55) ? "left" : "right", bits(c,50,48), bits(c,53,51),
      fy112(bits(c,45,32)).c_str(), fy112(bits(c,29,16)).c_str(), fy112(bits(c,13,0)).c_str());
    s += b;
    if(n >= 4) {
      std::snprintf(b, sizeof b, " xl=%s dxl=%s xh=%s dxh=%s xm=%s dxm=%s",
        f1616((u32)(w[1] >> 32)).c_str(), f1616((u32)w[1]).c_str(),
        f1616((u32)(w[2] >> 32)).c_str(), f1616((u32)w[2]).c_str(),
        f1616((u32)(w[3] >> 32)).c_str(), f1616((u32)w[3]).c_str());
      s += b;
    }
    break;
  }
  case 0x24: case 0x25:
    std::snprintf(b, sizeof b, " tile=%u (%s,%s)-(%s,%s)", bits(c,26,24),
      fx102(bits(c,23,12)).c_str(), fx102(bits(c,11,0)).c_str(),
      fx102(bits(c,55,44)).c_str(), fx102(bits(c,43,32)).c_str());
    s += b;
    if(n >= 2) {
      const u64 d = w[1];
      std::snprintf(b, sizeof b, " s=%.3f t=%.3f dsdx=%.4f dtdy=%.4f",
        (s16)bits(d,63,48) / 32.0, (s16)bits(d,47,32) / 32.0,
        (s16)bits(d,31,16) / 1024.0, (s16)bits(d,15,0) / 1024.0);
      s += b;
    }
    break;
  case 0x2d:
    std::snprintf(b, sizeof b, " (%s,%s)-(%s,%s)%s", fx102(bits(c,55,44)).c_str(), fx102(bits(c,43,32)).c_str(),
      fx102(bits(c,23,12)).c_str(), fx102(bits(c,11,0)).c_str(),
      bits(c,25,25) ? (bits(c,24,24) ? " interlace-odd" : " interlace-even") : "");
    s += b; break;
  case 0x2e:
    std::snprintf(b, sizeof b, " z=0x%04x dz=0x%04x", bits(c,31,16), bits(c,15,0)); s += b; break;
  case 0x2f:
    s += ' '; s += otherModesText(c); break;
  case 0x30: case 0x32: case 0x33: case 0x34:
    if(op == 0x33)
      std::snprintf(b, sizeof b, " tile=%u sl=%u tl=%u sh=%u dxt=%u", bits(c,26,24),
        bits(c,55,44), bits(c,43,32), bits(c,23,12), bits(c,11,0));
    else
      std::snprintf(b, sizeof b, " tile=%u (%s,%s)-(%s,%s)", bits(c,26,24),
        fx102(bits(c,55,44)).c_str(), fx102(bits(c,43,32)).c_str(),
        fx102(bits(c,23,12)).c_str(), fx102(bits(c,11,0)).c_str());
    s += b; break;
  case 0x35:
    std::snprintf(b, sizeof b, " tile=%u fmt=%s siz=%s line=%u tmem=0x%03x pal=%u s:cm=%u%u mask=%u shift=%u t:cm=%u%u mask=%u shift=%u",
      bits(c,26,24), kFmt[bits(c,55,53)], kSiz[bits(c,52,51)], bits(c,49,41), bits(c,40,32) * 8,
      bits(c,23,20), bits(c,9,9), bits(c,8,8), bits(c,7,4), bits(c,3,0),
      bits(c,19,19), bits(c,18,18), bits(c,17,14), bits(c,13,10));
    s += b; break;
  case 0x36:
    std::snprintf(b, sizeof b, " (%s,%s)-(%s,%s)", fx102(bits(c,23,12)).c_str(), fx102(bits(c,11,0)).c_str(),
      fx102(bits(c,55,44)).c_str(), fx102(bits(c,43,32)).c_str());
    s += b; break;
  case 0x37:
    std::snprintf(b, sizeof b, " 0x%08x", (u32)c); s += b; break;
  case 0x38: case 0x39: case 0x3b:
    std::snprintf(b, sizeof b, " r=%u g=%u b=%u a=%u", bits(c,31,24), bits(c,23,16), bits(c,15,8), bits(c,7,0));
    s += b; break;
  case 0x3a:
    std::snprintf(b, sizeof b, " r=%u g=%u b=%u a=%u minlvl=%u lodfrac=%u", bits(c,31,24), bits(c,23,16),
      bits(c,15,8), bits(c,7,0), bits(c,44,40), bits(c,39,32));
    s += b; break;
  case 0x3c:
    s += ' '; s += combinerText(c); break;
  case 0x3d: case 0x3f:
    std::snprintf(b, sizeof b, " fmt=%s siz=%s w=%u addr=0x%06x", kFmt[bits(c,55,53)], kSiz[bits(c,52,51)],
      bits(c,41,32) + 1, bits(c,25,0));
    s += b; break;
  case 0x3e:
    std::snprintf(b, sizeof b, " addr=0x%06x", bits(c,25,0)); s += b; break;
  case 0x2a: case 0x2b: case 0x2c:
    std::snprintf(b, sizeof b, " 0x%014llx", (unsigned long long)(c & 0x00ff'ffff'ffff'ffffull)); s += b; break;
  default: break;
  }
  return s;
}

}  // namespace kestrel::dbg
