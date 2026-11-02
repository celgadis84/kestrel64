// SoftRDP command side: TMEM loads (LOAD_BLOCK/LOAD_TILE), GPU-RDP batching and the
// command loop that walks the DP FIFO. Split out of rdp.cpp; shared internals live in
// rdp_internal.hpp.
#include "../core/wrtag.hpp"
#include "rdp.hpp"
#include "../gpurdp/gpurdp.hpp"
#include <cstdlib>
#include "../core/memory.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdio>
#include <set>
#include "rdp_internal.hpp"

namespace kestrel {

// Copia `n` bytes de RDRAM a TMEM. La direccion de TMEM es de 12 bits y ENVUELVE (el contador
// de la unidad de carga no se para en el final: parallel-rdp tmem_update enmascara igual), asi
// que una carga que se pasa de 4 KiB sigue por el principio. El lado de RDRAM si recorta: lo
// que cae fuera de la RAM instalada no existe.
template<typename V>
auto SoftRdp::copyRun(const V& m, u32 src, u32 dst, u32 n) -> void {
  if(src >= m.size()) return;
  n = std::min(n, (u32)(m.size() - src));
  dst &= 0xfff;
  while(n) {
    const u32 c = std::min(n, 0x1000u - dst);
    std::memcpy(&tmem[dst], &m[src], c);
    dst = (dst + c) & 0xfff; src += c; n -= c;
  }
}

auto SoftRdp::loadTile(Memory& mem, u32 t, bool block, u64 cmd) -> void {
  if(texpack::active()) hdNoteLoad(t, block, cmd);
  // Copy texels from the texture image (RDRAM, ti_addr/ti_width/ti_size) into TMEM
  // at the tile's base. LOAD_TILE walks a [SL,TL]..[SH,TH] rectangle (fields in
  // 10.2); LOAD_BLOCK copies a contiguous run (SL..SH linear texels).
  //
  // Filas impares: el hardware guarda cada fila impar de TMEM con las dos mitades de 32 bits
  // de cada palabra de 64 intercambiadas, y el muestreador las deshace (fetchK). LOAD_TILE
  // intercambia las filas impares de la carga. LOAD_BLOCK no sabe de filas: lleva un contador
  // T que sube DXT (1.11) por palabra escrita, T = (i*DXT) >> 11, intercambia la palabra i
  // cuando T es impar y la coloca en i + T*line (el LINE del tile de carga, casi siempre 0).
  // Con DXT bien puesto la textura queda igual que con LOAD_TILE; con DXT = 0 no intercambia
  // nada, y quien carga asi tiene que traer las filas impares ya intercambiadas en RDRAM
  // (port nativo de PD). Ref: parallel-rdp tmem_update.comp.
  //
  // 32 bpp: cada texel se parte en dos mitades de 16 bits, RG a la mitad baja de TMEM y BA a
  // la alta (+0x800), con direcciones y LINE contadas dentro de la mitad (ver fetchK). Cada
  // palabra de 64 bits de RDRAM (dos texeles) deja 4 bytes en cada mitad, y en las filas
  // impares se intercambia con su vecina (XOR 4 dentro de la mitad).
  Tile& tl = tiles[t & 7];
  const auto& m = mem.rdram;
  u32 bpt = ti_size == 3 ? 4 : ti_size == 2 ? 2 : ti_size == 1 ? 1 : 0;   // bytes/texel (0 = 4-bit)
  // Un texel de 32 bpp (4 bytes de RDRAM desde sa) a la direccion d de la mitad baja.
  // YUV a 16 bpp (parallel-rdp update_tmem_32 con yuv): cada pareja U Y0 V Y1 se sube como
  // un texel de 32 bpp, U,V a la mitad baja y Y0,Y1 a la alta.
  const bool yuv = tl.fmt == 1 && ti_size == 2;
  auto put32 = [&](u32 d, u32 sa) {
    d &= 0x7fe;
    if(sa + 3 >= m.size()) return;
    if(yuv) { tmem[d] = m[sa]; tmem[d + 1] = m[sa + 2]; tmem[d | 0x800] = m[sa + 1]; tmem[(d | 0x800) + 1] = m[sa + 3]; return; }
    tmem[d] = m[sa]; tmem[d + 1] = m[sa + 1]; tmem[d | 0x800] = m[sa + 2]; tmem[(d | 0x800) + 1] = m[sa + 3];
  };
  // LOAD_BLOCK: nb bytes de RDRAM desde src, palabra a palabra con el contador DXT.
  auto blockCopy = [&](u32 src, u32 nb, u32 dxt) {
    const u32 dst = tl.tmem * 8;
    if(bpt == 4 || yuv) {
      for(u32 i = 0, done = 0; done < nb; i++, done += 8) {
        const u32 tt = (u32)(((u64)i * dxt) >> 11);
        const u32 w = (dst + i * 4 + tt * tl.line * 8) ^ ((tt & 1) ? 4u : 0u);
        put32(w, src + done);
        if(done + 4 < nb) put32(w + 2, src + done + 4);
      }
      return;
    }
    if(!dxt && !tl.line) { copyRun(m, src, dst, nb); return; }   // T = 0 siempre: copia recta
    for(u32 i = 0, done = 0; done < nb; i++, done += 8) {
      const u32 tt = (u32)(((u64)i * dxt) >> 11);
      const u32 w = dst + (i + tt * tl.line) * 8, x = (tt & 1) ? 4u : 0u;
      const u32 n = std::min(8u, nb - done);
      for(u32 b = 0; b < n; b++) {
        const u32 d = (w + b) ^ x, sa = src + done + b;
        if(sa < m.size()) tmem[d & 0xfff] = m[sa];
      }
    }
  };
  u32 sl = (u32)((cmd >> 44) & 0xfff), tlo = (u32)((cmd >> 32) & 0xfff);
  u32 sh = (u32)((cmd >> 12) & 0xfff), th = (u32)((cmd >> 0) & 0xfff);
  // 4-bit texels (CI4/IA4/I4) pack 2/byte. LOAD_BLOCK is a linear byte copy of
  // ceil(texels/2) bytes; the SL/SH texel indices halve to byte offsets.
  if(!bpt) {
    tl.sl = sl; tl.tl = tlo; tl.sh = sh; tl.th = th;
    if(block) {
      u32 count = (sh >= sl) ? (sh - sl + 1) : 1, nb = (count + 1) / 2;
      blockCopy(ti_addr + ((tlo * ti_width + sl) >> 1), nb, th);   // SL/TL en texeles (ver abajo)
      accountLoad(mem, 1, nb);
    } else {
      u32 s0 = sl >> 2, t0 = tlo >> 2, s1 = sh >> 2, t1 = th >> 2, rowBytes = tl.line * 8;
      accountLoad(mem, t1 - t0 + 1, (s1 - s0 + 2) / 2);
      for(u32 ty = t0; ty <= t1; ty++)
        for(u32 tx = s0; tx <= s1; tx++) {         // nibble-granular copy
          u32 src = ti_addr + (ty * ti_width + tx) / 2;   // ti_width in texels
          u32 dstByte = (tl.tmem * 8 + (ty - t0) * rowBytes + (tx - s0) / 2) ^ (((ty - t0) & 1) ? 4u : 0u);
          dstByte &= 0xfff;
          if(src < m.size()) {
            u8 nib = (tx & 1) ? (m[src] & 0xf) : (m[src] >> 4);
            if((tx - s0) & 1) tmem[dstByte] = (tmem[dstByte] & 0xf0) | nib;
            else              tmem[dstByte] = (tmem[dstByte] & 0x0f) | (nib << 4);
          }
        }
    }
    return;
  }
  // On real HW LOAD_TILE/LOAD_BLOCK also latch the tile's clamp box (same SL/TL/
  // SH/TH registers SET_TILE_SIZE writes). Roms that skip SET_TILE_SIZE rely on
  // this — without it the sampler clamps every texel to (0,0).
  tl.sl = sl; tl.tl = tlo; tl.sh = sh; tl.th = th;
  if(block) {
    u32 count = (sh >= sl) ? (sh - sl + 1) : 1;        // linear texel count
    u32 nb = count * bpt;
    // LOAD_BLOCK lleva SL y TL en texeles enteros (no 10.2), y el origen en RDRAM es la fila
    // TL del texture image: tex_addr + (ti_width*TL + SL) * bpp (parallel-rdp load_tile).
    blockCopy(ti_addr + (tlo * ti_width + sl) * bpt, nb, th);
    accountLoad(mem, 1, nb);
    return;
  }
  u32 s0 = sl >> 2, t0 = tlo >> 2, s1 = sh >> 2, t1 = th >> 2;
  // SH < SL (o TH < TL) viene de un comando malformado: (s1-s0+1) da la vuelta en u32 y
  // los bucles de fila recorrian ~4G bytes colgando el hilo del RDP. No hay nada que cargar.
  if(s1 < s0 || t1 < t0) return;
  u32 rowBytes = tl.line * 8;
  accountLoad(mem, t1 - t0 + 1, u64(s1 - s0 + 1) * bpt);
  // Los texeles de una fila son contiguos en el texture image (paso `bpt`) y tambien en
  // TMEM (paso `bpt` desde el inicio de la fila), asi que la fila entera es un solo run.
  // Copiar byte a byte con dos comprobaciones de limite POR BYTE salia el 7 % del hilo.
  const u32 rowRun = (s1 - s0 + 1) * bpt;
  if(yuv) {   // parejas de pixeles; el ancho se redondea a 4 pixeles (parallel-rdp upload.width)
    const u32 pairs = (((s1 - s0 + 1) + 3) & ~3u) >> 1;
    for(u32 ty = t0; ty <= t1; ty++) {
      const u32 src = ti_addr + (ty * ti_width + s0) * 2, x = ((ty - t0) & 1) ? 4u : 0u;
      const u32 dst = tl.tmem * 8 + (ty - t0) * rowBytes;
      for(u32 k = 0; k < pairs; k++) put32((dst + k * 2) ^ x, src + k * 4);
    }
    return;
  }
  if(bpt == 4) {
    for(u32 ty = t0; ty <= t1; ty++) {
      const u32 src = ti_addr + (ty * ti_width + s0) * 4, x = ((ty - t0) & 1) ? 4u : 0u;
      const u32 dst = tl.tmem * 8 + (ty - t0) * rowBytes;
      for(u32 k = 0; k <= s1 - s0; k++) put32((dst + k * 2) ^ x, src + k * 4);
    }
    return;
  }
  for(u32 ty = t0; ty <= t1; ty++) {
    u32 src = ti_addr + (ty * ti_width + s0) * bpt;
    u32 dst = tl.tmem * 8 + (ty - t0) * rowBytes;
    if(!((ty - t0) & 1)) { copyRun(m, src, dst, rowRun); continue; }
    for(u32 b = 0; b < rowRun; b++) {                 // fila impar: mitades intercambiadas
      const u32 d = (dst + b) ^ 4u;
      if(src + b < m.size()) tmem[d & 0xfff] = m[src + b];
    }
  }
}

// KESTREL_GPUFLUSHSTAT=1: al salir, por que se vacio la cola (opcode del comando, o el
// final de run(), o una primitiva que cayo al CPU, o la cola llena).
auto SoftRdp::gpuFlushStat(int why) -> void {
  static const bool on = std::getenv("KESTREL_GPUFLUSHSTAT") != nullptr;
  if(!on) return;
  static u64 fh[68] = {};
  static const bool reg = (std::atexit([] {
    static const char* const extra[4] = {"fin de run()", "triangulo al CPU", "texrect al CPU", "cola llena"};
    for(int i = 0; i < 68; i++) {
      if(!fh[i]) continue;
      if(i < 64) std::fprintf(stderr, "[gpurdp] vaciado por op %02x: %llu\n", i, (unsigned long long)fh[i]);
      else std::fprintf(stderr, "[gpurdp] vaciado por %s: %llu\n", extra[i - 64], (unsigned long long)fh[i]);
    }
  }), true);
  (void)reg;
  fh[why]++;
}

auto SoftRdp::gpuMark(u32 lo, u32 hi) -> void {
  // Funde con un intervalo que solape o toque; si no cabe uno nuevo, agranda el ultimo
  // (sobra cobertura, nunca falta).
  for(int i = 0; i < gpuNDirty; i++)
    if(lo <= gpuDirtyHi[i] && gpuDirtyLo[i] <= hi) {
      gpuDirtyLo[i] = std::min(gpuDirtyLo[i], lo); gpuDirtyHi[i] = std::max(gpuDirtyHi[i], hi);
      return;
    }
  if(gpuNDirty < 8) { gpuDirtyLo[gpuNDirty] = lo; gpuDirtyHi[gpuNDirty] = hi; gpuNDirty++; return; }
  gpuDirtyLo[7] = std::min(gpuDirtyLo[7], lo); gpuDirtyHi[7] = std::max(gpuDirtyHi[7], hi);
}

auto SoftRdp::gpuDirtyHit(u64 lo, u64 hi) const -> bool {
  for(int i = 0; i < gpuNDirty; i++) if(lo < gpuDirtyHi[i] && gpuDirtyLo[i] < hi) return true;
  return false;
}


auto SoftRdp::gpuFlush(Memory& mem) -> void {
  gpuQueued = false;
  gpuNDirty = 0;
  static thread_local std::vector<gpurdp::TriOut> outs;
  gpurdp::flush(mem.rdram.data(), (u32)mem.rdram.size(), hiddenBits(mem), &outs);
  // Lo que dependia del resultado de cada triangulo, en el orden del FIFO: cobro (pixeles
  // escritos en color y z) y el registro COMBINED que dejo su ultimo pixel.
  for(size_t i = 0; i < gpuTris.size() && i < outs.size(); i++) {
    const GpuTri& t = gpuTris[i];
    const gpurdp::TriOut& o = outs[i];
    pxWrites += (u64)o.nWrite; pxZWrites += (u64)o.nZWrite;
    acctFinish(mem, t.acct, (u64)o.nWrite, (u64)o.nZWrite);
    if(t.comb) for(int k = 0; k < 4; k++) combined[k] = o.comb[k];
  }
  gpuTris.clear();
}

// Comandos que no leen ni escriben RDRAM: con primitivas en la GPU pendientes se pueden
// ejecutar sin bajarlas (cada primitiva encolada lleva ya su propio estado). Un FILL_RECTANGLE
// en FILL/COPY tambien, porque se encola detras. Todo lo demas (texrect, cargas de TMEM y TLUT,
// rellenos de 1/2 ciclos que mezclan con el framebuffer) necesita la RDRAM al dia.
static auto gpuNoFlush(u32 op, u32 cycle) -> bool {
  switch(op) {
  case 0x00: case 0x26: case 0x27: case 0x28: case 0x29: case 0x2a: case 0x2b: case 0x2c:
  case 0x2d: case 0x2e: case 0x2f: case 0x32: case 0x35: case 0x37: case 0x38: case 0x39:
  case 0x3a: case 0x3b: case 0x3c: case 0x3d: case 0x3e: case 0x3f:
    return true;
  case 0x36: return cycle >= 2;
  case 0x24: case 0x25: return true;   // texRect decide, como los triangulos
  // Triangulos: drawTriangle decide; si el triangulo no va a la GPU vacia la cola el mismo.
  case 0x08: case 0x09: case 0x0a: case 0x0b: case 0x0c: case 0x0d: case 0x0e: case 0x0f:
    return true;
  default: return false;
  }
}

auto SoftRdp::run(Memory& mem, u32 start, u32 end, bool xbus) -> u32 {
  const auto& m = mem.rdram;
  const auto& dm = mem.dmem;
  // Command fetch source: RDRAM by physical address, or DMEM (12-bit wrap) in xbus
  // mode. Big-endian 64-bit word either way. Pixel writes still target RDRAM.
  auto fetch = [&](u32 a) -> u64 {
    if(!xbus) return cmdSrc ? rd64p(cmdSrc, a, (u32)m.size()) : rd64(m, a);
    u64 v = 0;
    for(int i = 0; i < 8; i++) v = (v << 8) | dm[(a + i) & 0xfff];
    return v;
  };
  u32 cur, end2;
  if(xbus) { cur = start & 0xfff'ffff; end2 = end & 0xfff'ffff; }  // DMEM offsets: keep the
  else     { cur = start & 0x00ff'ffff; end2 = end & 0x00ff'ffff; }  // overflow range unmasked
  end = end2;
  u32 executed = 0;
  // Reparto CPU/GPU por tramo (KESTREL_GPURDP_MINPX, pixeles; 0 = todo a la GPU). Cada vaciado
  // de la GPU cuesta un envio + fence de ida y vuelta (~300 us en la RX 570) haga lo que haga,
  // y SM64 manda tramos de pocos triangulos pequenos: esos los pinta antes SoftRDP. Los dos
  // caminos dan los mismos bytes y la misma contabilidad, asi que es una decision del
  // anfitrion pura (md5 y estado del invitado no se mueven). Se estima el area de lo que trae
  // el tramo (caja de cada triangulo / rectangulo) y por debajo del umbral va al CPU. Con
  // escalado interno, modo sombra: el CPU pinta todo a 1x y la GPU solo la copia de alta, sin
  // esperarla (KESTREL_GPURDP_HISYNC=1 = el camino de antes: todo a la GPU, vaciado sincrono
  // por tramo, la mitad de rapido en SM64 x4).
  {
    static const u64 minPx = [] {
      const char* e = std::getenv("KESTREL_GPURDP_MINPX");
      return e && e[0] ? (u64)std::strtoull(e, nullptr, 0) : (u64)4096;
    }();
    static const bool hiSync = [] {
      const char* e = std::getenv("KESTREL_GPURDP_HISYNC");
      return e && e[0] && e[0] != '0';
    }();
    gpuRun = true;
    gpuShadow = gpurdp::active() && gpurdp::upscale() > 1 && !hiSync;
    if(minPx && gpurdp::active() && gpurdp::upscale() <= 1) {
      auto s14 = [](u64 v) -> s32 { return (s32)((u32)(v & 0x3fff) << 18) >> 18; };
      double px = 0;
      u32 a = cur, cyc = cycleType();
      while(a < end && px < (double)minPx) {
        const u64 c = fetch(a);
        const u32 o = (c >> 56) & 0x3f;
        if(o >= 0x08 && o <= 0x0f) {
          const u32 n = 4 + ((o & 4) ? 8 : 0) + ((o & 2) ? 8 : 0) + ((o & 1) ? 2 : 0);
          if(a + n * 8 > end) break;
          const double yl = s14(c >> 32) / 4.0, ym = s14(c >> 16) / 4.0, yh = s14(c) / 4.0;
          const u64 w1 = fetch(a + 8), w2 = fetch(a + 16), w3 = fetch(a + 24);
          auto x = [](u64 w) { return (s32)(w >> 32) / 65536.0; };
          auto d = [](u64 w) { return (s32)(u32)w / 65536.0; };
          const double h = std::max(0.0, yl - yh);
          const double xs[6] = {x(w2), x(w2) + d(w2) * h, x(w3), x(w3) + d(w3) * std::max(0.0, ym - yh),
                                x(w1), x(w1) + d(w1) * std::max(0.0, yl - ym)};
          double lo = xs[0], hi = xs[0];
          for(double v : xs) { lo = std::min(lo, v); hi = std::max(hi, v); }
          px += std::min(hi - lo, 2048.0) * std::min(h, 2048.0) * 0.5 + h;
          a += n * 8; continue;
        }
        if(o == 0x24 || o == 0x25 || o == 0x36) {
          const double w = (double)(((c >> 44) & 0xfff) >> 2) - (double)(((c >> 12) & 0xfff) >> 2) + 1;
          const double h = (double)(((c >> 32) & 0xfff) >> 2) - (double)((c & 0xfff) >> 2) + 1;
          // Relleno en FILL: el CPU escribe 8 bytes por vuelta, cuenta poco.
          if(w > 0 && h > 0) px += o == 0x36 && cyc == 3 ? w * h / 16 : w * h;
          a += o == 0x36 ? 8 : 16; continue;
        }
        if(o == 0x2f) cyc = (u32)(c >> 52) & 3;
        a += 8;
      }
      gpuRun = px >= (double)minPx;
    }
    if(gpurdp::active() && !gpuShadow && mem.rdpStats.on.load(std::memory_order_relaxed))
      (gpuRun ? mem.rdpStats.gpuTramos : mem.rdpStats.cpuTramos).fetch_add(1, std::memory_order_relaxed);
  }
  u64 words[24];
  int guard = 0;
  bool split = false;                 // ultimo comando del span partido: no se ejecuta
  sawSyncFull = false;
  cmdTail = 0;
  stopAt = cur;
  static bool lowCi = false; static u64 lowMark = 0;
  static const bool citrace = std::getenv("KESTREL_CITRACE") != nullptr;
  static bool ops = std::getenv("KESTREL_RDPOPS") != nullptr;
  static u32 hist[64] = {};
  while(cur < end && guard++ < 200000) {
    // Publicar el puntero de lectura ANTES de consumir el comando: el productor debe
    // ver como ocupado todo lo que aun no se ha leido.
    if(curOut) curOut->store(cur, std::memory_order_release);
    if(wrtag::tag) wrtag::moveWinLo(0, cur);
    u64 cmd = fetch(cur);
    u32 op = (cmd >> 56) & 0x3f;
    cmdTail = op == 0x29 ? 1 : 2;
    if(gpuQueued && !gpuNoFlush(op, cycleType())) {
      // Cargas de TMEM/TLUT: solo leen RDRAM. Si lo que leen no lo va a escribir nada de la
      // cola, no hace falta bajarla. Cota por arriba de lo que leen (4 B por texel, 8 de
      // margen): desde ti_addr hasta pasada la ultima fila/texel.
      bool need = true;
      if(op == 0x30 || op == 0x33 || op == 0x34) {
        const u64 sh = (cmd >> 12) & 0xfff, th = cmd & 0xfff;
        const u64 hi = op == 0x33 ? u64(ti_addr) + (sh + 1) * 4 + 8
                                  : u64(ti_addr) + ((th >> 2) + 1) * u64(ti_width) * 4 + ((sh >> 2) + 1) * 4 + 8;
        need = gpuDirtyHit(ti_addr, hi);
      }
      if(need) { gpuFlushStat(op); gpuFlush(mem); }
    }
    const bool st = charge && mem.rdpStats.on.load(std::memory_order_relaxed);
    if(st) statsCmd(mem, op, cmd);
    // KESTREL_RDPMODES: cada valor DISTINTO de los comandos de estado (modos, combinador, imagenes,
    // tile, scissor, colores) una sola vez, para comparar dos programas que deberian pintar igual.
    static const bool modesLog = std::getenv("KESTREL_RDPMODES") != nullptr;
    // KESTREL_RDPTRACE=<n>: los n primeros comandos con su direccion, para casar el stream con
    // un volcado de la consola (pantalla de panico) y ver que hubo antes del punto de cuelgue.
    static const long traceMax = std::getenv("KESTREL_RDPTRACE") ? std::atol(std::getenv("KESTREL_RDPTRACE")) : 0;
    static long traceN = 0;
    if(traceMax && !costOnly && traceN < traceMax) {
      traceN++;
      std::fprintf(stderr, "[rdptrace] %06x %02x %016llx\n", cur, op, (unsigned long long)cmd);
    }
    if(modesLog && op >= 0x2d && op != 0x31 && op != 0x33 && op != 0x34 && op != 0x30 && op != 0x36) {
      static std::set<u64> seen;
      if(seen.size() < 400 && seen.insert(cmd).second)
        std::fprintf(stderr, "[mode] %02x %016llx\n", op, (unsigned long long)cmd);
    }
    if(ops) {
      hist[op]++;
      // alert on the first time we ever see a color-image set or a triangle
      static bool sawCimg = false, sawTri = false, sawFill = false;
      if(op == 0x3f && !sawCimg) { sawCimg = true; std::fprintf(stderr, "[rdp!] first SET_COLOR_IMAGE %016llx\n",(unsigned long long)cmd); std::fflush(stderr); }
      if(op >= 0x08 && op <= 0x0f && !sawTri) { sawTri = true; std::fprintf(stderr, "[rdp!] first TRIANGLE op=%02x\n", op); std::fflush(stderr); }
      if(op == 0x36 && !sawFill) { sawFill = true; std::fprintf(stderr, "[rdp!] first FILL_RECT %016llx\n",(unsigned long long)cmd); std::fflush(stderr); }
    }

    // KESTREL_RDPVALID: reglas que el emulador perdona y la consola no (cuelgue del RDP o
    // imagen rota). Imprime los 40 primeros avisos con la direccion del comando.
    static const bool valid = std::getenv("KESTREL_RDPVALID") != nullptr;
    if(valid && !costOnly) {
      static u32 nwarn = 0;
      static bool drawPipe = false, drawLoad = false, drawTile = false;
      const bool draw = (op >= 0x08 && op <= 0x0f) || op == 0x24 || op == 0x25 || op == 0x36;
      const u32 cyc = cycleType();
      static u32 histA[24], histOp[24], histN = 0;
      static const u32 dumpAt = (u32)std::atoi(std::getenv("KESTREL_RDPVALID"));
      auto warn = [&](const char* what) {
        if(nwarn + 1 == dumpAt)
          for(u32 i = 0; i < 24; i++) {
            const u32 k = (histN + i) % 24;
            std::fprintf(stderr, "[rdpvalid]   prev %06x op %02x\n", histA[k], histOp[k]);
          }
        if(nwarn++ < 40) std::fprintf(stderr, "[rdpvalid] %06x op %02x %016llx cyc %u: %s\n", cur, op,
                                      (unsigned long long)cmd, cyc, what);
      };
      if(op >= 0x01 && op <= 0x07) warn("invalid opcode");
      if(op >= 0x10 && op <= 0x23) warn("invalid opcode");
      if(op >= 0x08 && op <= 0x0f && cyc >= 2) warn("triangle in COPY/FILL mode (hang)");
      if((op == 0x24 || op == 0x25) && cyc == 3) warn("texture rect in FILL mode (hang)");
      if((op == 0x24 || op == 0x25) && cyc == 2 && tiles[(cmd >> 24) & 7].size == 3) warn("COPY mode with 32-bit tile");
      if((op == 0x30 || op == 0x33 || op == 0x34) && ti_size == 0) warn("LOAD from a 4bpp texture image (hang)");
      if(op == 0x33 && ((cmd >> 12) & 0xfff) + 1 > 2048) warn("LOAD_BLOCK over 2048 texels");
      if(op == 0x30 && tiles[(cmd >> 24) & 7].tmem < 256) warn("LOAD_TLUT into low TMEM");
      if((op == 0x30 || op == 0x33 || op == 0x34) && drawLoad) warn("LOAD after draw without SYNC_LOAD");
      if((op == 0x35 || op == 0x32) && drawTile) warn("SET_TILE(_SIZE) after draw without SYNC_TILE");
      if((op == 0x2f || op == 0x3c || op == 0x3f || op == 0x3e || op == 0x2d) && drawPipe) warn("mode/image change after draw without SYNC_PIPE");
      static u32 nextA = 0, njump = 0;
      if(cur != nextA && njump++ < 60)
        std::fprintf(stderr, "[rdpvalid] jump %06x -> %06x (span %06x..%06x)\n", nextA, cur, start & 0xffffffu, end);
      nextA = cur + (op >= 0x08 && op <= 0x0f ? (4 + ((op & 4) ? 8 : 0) + ((op & 2) ? 8 : 0) + ((op & 1) ? 2 : 0)) * 8
                                             : op == 0x24 || op == 0x25 ? 16 : 8);
      histA[histN % 24] = cur; histOp[histN % 24] = op; histN++;
      if(draw) drawPipe = drawLoad = drawTile = true;
      if(op == 0x27 || op == 0x29) drawPipe = false;
      if(op == 0x26 || op == 0x29) drawLoad = false;
      if(op == 0x28 || op == 0x29) drawTile = false;
    }

    // triangles have a variable length; everything else is one 64-bit word.
    // Coefficient blocks (64-bit words): edge 4, +shade 8, +texture 8, +zbuffer 2
    // (op bit2=shade, bit1=texture, bit0=zbuffer). Undercounting these desyncs the
    // FIFO — the next command lands mid-block and decodes as garbage.
    if(op >= 0x08 && op <= 0x0f) {
      int n = 4 + ((op & 4) ? 8 : 0) + ((op & 2) ? 8 : 0) + ((op & 1) ? 2 : 0);
      if(n > 24) n = 24;
      // Comando partido por el borde del span: parar delante de el (ver stopAt).
      if(cur + (u32)n * 8 > end) break;
      for(int i = 0; i < n; i++) words[i] = fetch(cur + i * 8);
      drawTriangle(mem, words, n, op);
      cur += n * 8; executed++; continue;
    }

    // Carga TMEM redundante (rdp.stats): TMEM (con la paleta) identica antes y despues.
    u8 snap[sizeof tmem];
    const bool stLoad = st && (op == 0x30 || op == 0x33 || op == 0x34);
    u64 stBytes0 = 0;
    if(stLoad) {
      std::memcpy(snap, tmem, sizeof tmem);
      stBytes0 = mem.rdpStats.loadBytes.load(std::memory_order_relaxed);
    }
    switch(op) {
    case 0x00: break;                                   // no-op
    case 0x26: accountStall(mem, 25); break;            // SYNC_LOAD: 25 GCLK fijos
    case 0x27: accountStall(mem, 50); break;            // SYNC_PIPE: 50 GCLK fijos
    case 0x28: accountStall(mem, 33); break;            // SYNC_TILE: 33 GCLK fijos
    case 0x29: {
      static const bool sfLog = std::getenv("KESTREL_DPSYNCLOG") != nullptr;
      if(sfLog) { std::fprintf(stderr, "[dpsync] at=%06x span=%06x..%06x xbus=%u\n", cur, start & 0x00ffffffu, end, (unsigned)xbus); std::fflush(stderr); }
      sawSyncFull = true; break; }               // SYNC_FULL → raises DP (see caller)
    case 0x24: case 0x25: {                             // TEXTURE_RECTANGLE (+flip)
      if(cur + 16 > end) { split = true; break; }       // partido: parar delante (ver stopAt)
      words[0] = cmd; words[1] = fetch(cur + 8);        // 2 words
      texRect(mem, words, op == 0x25);
      cur += 16; executed++; continue;
    }
    case 0x2d:                                          // SET_SCISSOR
      // Coordenadas 10.2. El limite bajo trunca; el alto es exclusivo y redondea hacia
      // arriba: una fraccion deja dentro el pixel parcial (libdragon pone xl = w*4-1, o sea
      // 31.75 para un ancho de 32, y el pixel 31 se pinta). En FILL/COPY el ultimo pixel
      // es inclusivo: limite (xh>>2)+1. Mismo criterio que parallel-rdp (binning.h).
      sx0 = (int)(((cmd >> 44) & 0xfff) >> 2);
      sy0 = (int)(((cmd >> 32) & 0xfff) >> 2);
      sx1 = (int)((((cmd >> 12) & 0xfff) + 3) >> 2);
      sy1 = (int)((((cmd >> 0)  & 0xfff) + 3) >> 2);
      sx1fc = (int)(((cmd >> 12) & 0xfff) >> 2) + 1;
      break;
    // SET_KEY_GB / SET_KEY_R: centro y escala de 8 bits por canal (combinador) y ancho de
    // 12 bits (chroma key). Disposicion de parallel-rdp op_set_key_gb / op_set_key_r.
    case 0x2a:
      key_width[1] = (u16)((cmd >> 44) & 0xfff); key_width[2] = (u16)((cmd >> 32) & 0xfff);
      key_center = (key_center & 0xff0000u) | (u32)((cmd >> 24) & 0xff) << 8 | (u32)((cmd >> 8) & 0xff);
      key_scale  = (key_scale  & 0xff0000u) | (u32)((cmd >> 16) & 0xff) << 8 | (u32)(cmd & 0xff);
      break;
    case 0x2b:
      key_width[0] = (u16)((cmd >> 16) & 0xfff);
      key_center = (key_center & 0x00ffffu) | (u32)((cmd >> 8) & 0xff) << 16;
      key_scale  = (key_scale  & 0x00ffffu) | (u32)(cmd & 0xff) << 16;
      break;
    case 0x2c: {                                        // SET_CONVERT (YUV→RGB coeffs)
      auto s9 = [](u32 v) -> int { v &= 0x1ff; return (int)(v ^ 0x100) - 0x100; };  // 9-bit sign-extend
      k0 = s9((u32)(cmd >> 45)); k1 = s9((u32)(cmd >> 36)); k2 = s9((u32)(cmd >> 27));
      k3 = s9((u32)(cmd >> 18)); k4 = s9((u32)(cmd >> 9));  k5 = s9((u32)cmd);
      break;
    }
    // SET_PRIM_DEPTH (Z<<16 | dZ). The Z field is a 15-bit integer depth that the RDP
    // loads into the same s15.16 attribute the triangle z interpolator produces, and the
    // depth unit then takes bits 31:13 of that — an 18-bit value, 3 bits of it fractional.
    // Storing the bare 15-bit number instead costs those 3 bits, and they matter: near
    // z=0x7FFF the z-buffer's floating format has 1-unit steps in the 18-bit domain but
    // 64-unit steps in the 15-bit one, so successive prim depths 1 apart all quantize to
    // the same stored value and every depth compare after the first fails.
    case 0x2e: prim_z = ((u32)(cmd >> 16) & 0x7fff) << 3; prim_dz = (u32)cmd & 0xffff; break;
    case 0x2f:                                          // SET_OTHER_MODES
      other_hi = (u32)(cmd >> 32) & 0x00ff'ffff;
      other_lo = (u32)cmd;
      break;
    case 0x35: {                                        // SET_TILE
      u32 t = (cmd >> 24) & 7; Tile& tl = tiles[t];
      tl.fmt = (cmd >> 53) & 7; tl.size = (cmd >> 51) & 3;
      tl.line = (cmd >> 41) & 0x1ff; tl.tmem = (cmd >> 32) & 0x1ff;
      tl.palette = (cmd >> 20) & 0xf;
      tl.cmT = (cmd >> 18) & 3; tl.maskT = (cmd >> 14) & 0xf; tl.shiftT = (cmd >> 10) & 0xf;
      tl.cmS = (cmd >> 8)  & 3; tl.maskS = (cmd >> 4)  & 0xf; tl.shiftS = (cmd >> 0)  & 0xf;
      break;
    }
    case 0x32: {                                        // SET_TILE_SIZE
      u32 t = (cmd >> 24) & 7; Tile& tl = tiles[t];
      tl.sl = (cmd >> 44) & 0xfff; tl.tl = (cmd >> 32) & 0xfff;
      tl.sh = (cmd >> 12) & 0xfff; tl.th = (cmd >> 0) & 0xfff;
      break;
    }
    case 0x30: {                                        // LOAD_TLUT (palette load)
      // LOAD_TLUT escribe la TMEM de verdad (parallel-rdp tmem_update.comp update_tmem_lut),
      // desde la direccion del tile destino y dando la vuelta a los 4 KB. La fuente es la de
      // un LOAD_TILE de una sola fila: texel (SL..SH, TL) de la imagen. Cada palabra de 16 bits
      // de TMEM (indice po desde el tile) toma un texel segun el tamano de la imagen (vs) frente
      // al del tile (ts):
      //  - 16 b -> tile 4 b (lo normal): cada entrada repetida en los cuatro bancos, 8 B por
      //    entrada y seguidas; la mitad alta queda como la lee el muestreo CI (0x800 + idx*8).
      //  - 16 b -> tile 8 b / 16 b: solo la primera palabra de 64 bits de cada 16 / 32 B; el
      //    resto de TMEM no se toca.
      //  - imagen de 8 b / 32 b: el mismo esquema con el paso de su tamano.
      // Imagen de 4 b (el HW se cuelga) y tile de 32 b no se modelan (parallel-rdp tampoco).
      u32 t  = (cmd >> 24) & 7;
      if(texpack::active()) hdNoteTlut(mem, t, cmd);
      const u32 slo = (u32)(cmd >> 44) & 0xfff, tlo = (u32)(cmd >> 32) & 0xfff;
      const u32 shi = (u32)(cmd >> 12) & 0xfff;
      const int vs = (int)ti_size, ts = (int)tiles[t].size;
      const u32 w = (((shi >> 2) - (slo >> 2)) + 1) & 0xfff;
      if(vs == 0 || ts == 3 || !w) break;
      const int effW = vs == 1 ? (int)((w + 7) & ~7u) : vs == 2 ? (int)w : (int)((w + 1) & ~1u);
      const u32 vaddr = ti_addr + ((ti_width * (tlo >> 2) + (slo >> 2)) << (vs - 1));
      const u32 off16 = ((tiles[t].tmem * 8) & 0xfff) >> 1;
      for(u32 L = 0; L < 0x800; L++) {
        const int po = (int)((L - off16) & 0x7ff), d = vs - ts;
        int sp;
        if(d == 2) sp = (po >> 2) << (vs - 2);
        else if(d == 1) { if(po & 4) continue;   sp = (po & ~7) >> (ts + (vs == 2 ? 2 : 0)); }
        else if(d == 0) { if(po & 0xc) continue; sp = (po & ~3) >> (ts + (vs == 2 ? 2 : 0)); }
        else if(d == -1) { if(po & 0x1c) continue; sp = (po >> ts) & ~7; }
        else { sp = (po >> 2) * 4; if(sp + 2 < effW) sp += 2; }   // tile 4 b, imagen 32 b
        if(sp >= effW) continue;
        u32 ra = vaddr + ((u32)sp << (vs - 1));
        ra += 2 * (ra & 1) * (u32)(po & 3);                // fuente impar: desplazamiento raro del HW
        if(ra + 1 >= m.size()) continue;
        tmem[L * 2] = m[ra]; tmem[L * 2 + 1] = m[ra + 1];
      }
      accountLoad(mem, 1, u64(w) * 2);   // antes no cobraba nada
      break;
    }
    case 0x33: loadTile(mem, (cmd >> 24) & 7, true, cmd); break;   // LOAD_BLOCK
    case 0x34: loadTile(mem, (cmd >> 24) & 7, false, cmd); break;  // LOAD_TILE
    case 0x36: {                                        // FILL_RECTANGLE
      // 10.2. En FILL/COPY el borde derecho/inferior es inclusivo (parallel-rdp pone yl |= 3
      // y el tramo incluye el pixel de xl). En 1/2 ciclos el rect es un primitivo mas: un
      // pixel entra si su muestra (x, y enteros) cae en [xh, xl) x [yh, yl), o sea limites
      // redondeados hacia arriba.
      const int qxh = (int)((cmd >> 12) & 0xfff), qyh = (int)(cmd & 0xfff);
      const int qxl = (int)((cmd >> 44) & 0xfff), qyl = (int)((cmd >> 32) & 0xfff);
      if(cycleType() >= 2) fillRect(mem, qxh >> 2, qyh >> 2, (qxl >> 2) + 1, (qyl >> 2) + 1);
      else fillRect(mem, (qxh + 3) >> 2, (qyh + 3) >> 2, (qxl + 3) >> 2, (qyl + 3) >> 2);
      break;
    }
    case 0x37: fill_color  = (u32)cmd; break;           // SET_FILL_COLOR
    case 0x38: fog_color   = (u32)cmd; break;           // SET_FOG_COLOR
    case 0x39: blend_color = (u32)cmd; break;           // SET_BLEND_COLOR
    case 0x3a:                                          // SET_PRIM_COLOR
      // Bits 44:40 = min_level (LOD clamp, unused while we do not mipmap), 39:32 =
      // prim_lod_frac (a real combiner input), 31:0 = the RGBA colour.
      prim_color    = (u32)cmd;
      prim_lod_frac = (u8)(cmd >> 32);
      prim_min_level = (u8)((cmd >> 40) & 31);
      break;
    case 0x3b: env_color   = (u32)cmd; break;           // SET_ENV_COLOR
    case 0x3c: {                                        // SET_COMBINE
      combine_hi = (u32)(cmd >> 32) & 0x00ff'ffff; combine_lo = (u32)cmd;
      u32 hi = combine_hi, lo = combine_lo;
      // Field packing per the GBI SET_COMBINE word (see combineColor for the mux).
      comb[0] = { (int)((hi >> 20) & 0xf), (int)((lo >> 28) & 0xf), (int)((hi >> 15) & 0x1f), (int)((lo >> 15) & 0x7),
                  (int)((hi >> 12) & 0x7), (int)((lo >> 12) & 0x7), (int)((hi >> 9)  & 0x7),  (int)((lo >> 9)  & 0x7) };
      comb[1] = { (int)((hi >> 5)  & 0xf), (int)((lo >> 24) & 0xf), (int)((hi >> 0)  & 0x1f), (int)((lo >> 6)  & 0x7),
                  (int)((lo >> 21) & 0x7), (int)((lo >> 3)  & 0x7), (int)((lo >> 18) & 0x7),  (int)((lo >> 0)  & 0x7) };
      break;
    }
    case 0x3d:                                          // SET_TEXTURE_IMAGE
      ti_fmt = (cmd >> 53) & 7; ti_size = (cmd >> 51) & 3;
      ti_width = ((cmd >> 32) & 0x3ff) + 1; ti_addr = (u32)cmd & 0x00ff'ffff;
      break;
    case 0x3e: zi_addr = (u32)cmd & 0x00ff'ffff; break; // SET_Z_IMAGE
    case 0x3f:                                          // SET_COLOR_IMAGE
      ci_size = (cmd >> 51) & 3;
      ci_width = ((cmd >> 32) & 0x3ff) + 1;
      ci_addr = (u32)cmd & 0x00ff'ffff;
      // Diagnostico: ningun juego pone su framebuffer encima de los vectores de
      // excepcion de libultra (0x0-0x400) ni del area del OS. Si aparece aqui es que
      // el FIFO se ha desincronizado o el puntero llego corrupto: avisar con el
      // comando crudo y la posicion del FIFO para poder rastrear el origen.
      // El suelo por defecto son los vectores de excepcion; KESTREL_CIFLOOR=<phys> lo
      // sube para cazar un framebuffer que aterriza sobre el codigo del juego.
      static const u32 ciFloor = []{ const char* e = std::getenv("KESTREL_CIFLOOR");
                                     return e ? (u32)std::strtoul(e, nullptr, 0) : 0x400u; }();
      if(ci_addr < ciFloor || citrace) {
        lowCi = true; lowMark = pxWrites;
        std::fprintf(stderr, "[rdp!] SET_COLOR_IMAGE bajo: addr=%06x cmd=%016llx fifo=%08x\n",
                     ci_addr, (unsigned long long)cmd, cur);
        // Para distinguir "el escritor todavia no habia puesto el comando" (ceros o
        // basura alrededor) de "el puntero del FIFO apunta mal" (el vecindario si
        // tiene comandos validos), se vuelca el FIFO tal cual esta en RDRAM ahora.
        std::fprintf(stderr, "[rdp!]   dpc start=%06x end=%06x current=%06x  ucode leyo CURRENT %llu veces\n",
                     mem.rcp.dpc_start, mem.rcp.dpc_end, mem.rcp.dpc_current.load(),
                     (unsigned long long)mem.rcp.dpcCurReads.load());
        std::fprintf(stderr, "[rdp!]   span %06x..%06x  vecindario:\n", start & 0xffffff, end);
        for(int k = -3; k <= 3; k++) {
          u32 a = cur + (u32)(k * 8);
          std::fprintf(stderr, "[rdp!]     %06x %016llx%s\n", a,
                       (unsigned long long)fetch(a), k == 0 ? "  <- aqui" : "");
        }
      } else if(lowCi) {
        lowCi = false;
        std::fprintf(stderr, "[rdp!]   ...se escribieron %llu pixeles con el CI bajo\n",
                     (unsigned long long)(pxWrites - lowMark));
      }
      break;
    default: break;                                     // sync/tlut/other → no-op for now
    }
    if(stLoad) {
      auto& rs = mem.rdpStats;
      rs.loads.fetch_add(1, std::memory_order_relaxed);
      if(!std::memcmp(snap, tmem, sizeof tmem)) {
        rs.loadRedundant.fetch_add(1, std::memory_order_relaxed);
        rs.loadRedundantBytes.fetch_add(rs.loadBytes.load(std::memory_order_relaxed) - stBytes0,
                                        std::memory_order_relaxed);
      }
    }
    if(split) break;                  // el switch solo pudo salir de si mismo
    cur += 8; executed++;
  }
  stopAt = cur;
  if(gpuQueued) { gpuFlushStat(64); gpuFlush(mem); }   // fuera de run() la RDRAM siempre esta al dia
  if(gpuShadow) gpurdp::submitHi(mem.rdram.data(), (u32)mem.rdram.size(), hiddenBits(mem));
  if(ops) {
    // KESTREL_RDPOPS is the print interval in DP runs (default 512). Demos that
    // submit a single command buffer and then spin need =1, or the histogram
    // never prints and the trace looks like "the RDP did nothing".
    static const u32 every = []{ const char* s = std::getenv("KESTREL_RDPOPS");
                                 u32 v = s ? (u32)std::strtoul(s, nullptr, 0) : 0; return v ? v : 512u; }();
    static u32 calls = 0;
    if(++calls % every == 0) {
      std::fprintf(stderr, "[rdpops] ci=%06x sz=%u w=%u | ", ci_addr, ci_size, ci_width);
      for(int i = 0; i < 64; i++) if(hist[i]) std::fprintf(stderr, "%02x:%u ", i, hist[i]);
      std::fprintf(stderr, "\n"); std::fflush(stderr);
    }
  }
  return executed;
}

}  // namespace kestrel
