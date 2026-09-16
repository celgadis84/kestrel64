#include "vifilter.hpp"

#include <vector>

namespace kestrel::vi {

namespace {

// Un pixel ya extraido de RDRAM: color de 8 bits por canal y cobertura de 3 bits.
struct Px { u32 r, g, b, a; };

inline auto exp5(u32 v) -> u32 { return (v << 3) | (v >> 2); }

inline auto median3(u32 l, u32 c, u32 r) -> u32 {
  if(l < c) { u32 t = l; l = c; c = t; }
  if(c < r) { u32 t = c; c = r; r = t; }
  if(l < c) { u32 t = l; l = c; c = t; }
  return c;
}

// Segundo mas bajo y segundo mas alto de los vecinos con cobertura llena, tal cual
// `check_neighbor` de vi_fetch.frag.
inline auto checkNeighbor(const Px& n, u32* lo, u32* hi, u32* slo, u32* shi) -> void {
  if(n.a != 7) return;
  const u32 c[3] = { n.r, n.g, n.b };
  for(int i = 0; i < 3; i++) {
    u32 mx = c[i] > lo[i] ? c[i] : lo[i];
    u32 mn = c[i] < hi[i] ? c[i] : hi[i];
    if(mx < slo[i]) slo[i] = mx;
    if(mn > shi[i]) shi[i] = mn;
    if(c[i] < lo[i]) lo[i] = c[i];
    if(c[i] > hi[i]) hi[i] = c[i];
  }
}

}  // namespace

auto active(u32 ctrl) -> bool {
  const u32 t = ctrlType(ctrl);
  if(t != 2 && t != 3) return false;
  return ctrlAA(ctrl) || ctrlDedither(ctrl);
}

auto fetchFiltered(const u8* ram, usize ramSize, const u8* hid, usize hidSize,
                   u32 origin, u32 stride, u32 w, u32 h, u32 ctrl, u32* out) -> void {
  const u32  type     = ctrlType(ctrl);
  const bool aa       = ctrlAA(ctrl);
  const bool divot    = ctrlDivot(ctrl);
  const bool dedither = ctrlDedither(ctrl);
  const usize n = (usize)w * h;

  // Etapa 1: extraer VRAM (extract_vram.comp). La cobertura del RGBA5551 esta partida entre
  // el bit 0 de la palabra (bit alto) y el plano oculto del noveno bit de RDRAM (2 bajos);
  // en RGBA8888 el byte de alfa ES la cobertura desplazada 5. Con AA apagado el VI lee 7
  // en todos, que es lo que desactiva de hecho AA y divot.
  // Los dos buffers son por hilo y persistentes: el presentador entra aqui 60 veces por
  // segundo y no hace falta pedir y devolver 1 MB en cada campo.
  static thread_local std::vector<Px> src;
  src.assign(n, Px{0, 0, 0, 7});
  for(u32 y = 0; y < h; y++) for(u32 x = 0; x < w; x++) {
    Px p{0, 0, 0, 7};
    if(type == 2) {
      const u32 a = origin + ((u32)y * stride + x) * 2;
      if((usize)a + 1 < ramSize) {
        const u32 word = ((u32)ram[a] << 8) | ram[a + 1];
        p.r = exp5((word >> 11) & 0x1f);
        p.g = exp5((word >> 6) & 0x1f);
        p.b = exp5((word >> 1) & 0x1f);
        const u32 lowbits = (hid && (usize)(a >> 1) < hidSize) ? (hid[a >> 1] & 3) : 0;
        p.a = ((word & 1) << 2) | lowbits;
      }
    } else if(type == 3) {
      const u32 a = origin + ((u32)y * stride + x) * 4;
      if((usize)a + 3 < ramSize) {
        p.r = ram[a]; p.g = ram[a + 1]; p.b = ram[a + 2];
        p.a = ram[a + 3] >> 5;
      }
    }
    if(!aa) p.a = 7;
    src[(usize)y * w + x] = p;
  }

  // Etapa 2: filtro de AA / de-dither (vi_fetch.frag). Los vecinos fuera del frame se
  // recortan al borde en vez de inventarse, que es lo que hace el muestreo de la GPU.
  static thread_local std::vector<Px> aaBuf;
  aaBuf.resize(n);
  auto at = [&](int x, int y) -> const Px& {
    if(x < 0) x = 0; else if(x >= (int)w) x = (int)w - 1;
    if(y < 0) y = 0; else if(y >= (int)h) y = (int)h - 1;
    return src[(usize)y * w + (u32)x];
  };
  for(u32 y = 0; y < h; y++) for(u32 x = 0; x < w; x++) {
    const Px& mid = src[(usize)y * w + x];
    Px o = mid;
    const int ix = (int)x, iy = (int)y;
    if(mid.a != 7) {
      u32 lo[3]  = { mid.r, mid.g, mid.b }, hi[3]  = { mid.r, mid.g, mid.b };
      u32 slo[3] = { mid.r, mid.g, mid.b }, shi[3] = { mid.r, mid.g, mid.b };
      checkNeighbor(at(ix - 1, iy - 1), lo, hi, slo, shi);
      checkNeighbor(at(ix + 1, iy - 1), lo, hi, slo, shi);
      checkNeighbor(at(ix - 2, iy),     lo, hi, slo, shi);
      checkNeighbor(at(ix + 2, iy),     lo, hi, slo, shi);
      checkNeighbor(at(ix - 1, iy + 1), lo, hi, slo, shi);
      checkNeighbor(at(ix + 1, iy + 1), lo, hi, slo, shi);
      const u32 coeff = 7 - mid.a;
      const u32 c[3] = { mid.r, mid.g, mid.b };
      u32 r[3];
      for(int i = 0; i < 3; i++) {
        const u32 off = slo[i] + shi[i] - (c[i] << 1);   // aritmetica de 32 bits sin signo, como uvec3
        r[i] = (c[i] + (((off * coeff) + 4) >> 3)) & 0xff;
      }
      o.r = r[0]; o.g = r[1]; o.b = r[2];
    } else if(dedither) {
      const int cb[3] = { (int)(mid.r >> 3), (int)(mid.g >> 3), (int)(mid.b >> 3) };
      int acc[3] = { 0, 0, 0 };
      for(int dy = -1; dy <= 1; dy++) for(int dx = -1; dx <= 1; dx++) {
        const Px& nb = at(ix + dx, iy + dy);
        const int v[3] = { (int)(nb.r >> 3), (int)(nb.g >> 3), (int)(nb.b >> 3) };
        for(int i = 0; i < 3; i++) {
          int d = v[i] - cb[i];
          acc[i] += d < -1 ? -1 : d > 1 ? 1 : d;
        }
      }
      o.r = ((mid.r & 0xf8) + (u32)acc[0]) & 0xff;
      o.g = ((mid.g & 0xf8) + (u32)acc[1]) & 0xff;
      o.b = ((mid.b & 0xf8) + (u32)acc[2]) & 0xff;
    }
    aaBuf[(usize)y * w + x] = o;
  }

  // Etapa 3: divot (vi_divot.frag). Mediana horizontal de 3 cuando alguno de los tres no
  // tiene cobertura llena.
  for(u32 y = 0; y < h; y++) for(u32 x = 0; x < w; x++) {
    const Px& m = aaBuf[(usize)y * w + x];
    u32 R = m.r, G = m.g, B = m.b;
    if(divot) {
      const Px& l = aaBuf[(usize)y * w + (x ? x - 1 : 0)];
      const Px& r = aaBuf[(usize)y * w + (x + 1 < w ? x + 1 : w - 1)];
      if((l.a & m.a & r.a) != 7) {
        R = median3(l.r, m.r, r.r);
        G = median3(l.g, m.g, r.g);
        B = median3(l.b, m.b, r.b);
      }
    }
    out[(usize)y * w + x] = (R << 16) | (G << 8) | B;
  }
}

}  // namespace kestrel::vi
