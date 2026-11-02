// Framebuffer grabs straight from RDRAM, independent of the presenter: KESTREL_FBDUMP /
// KESTREL_FBSEQ BMP dumps and the RGB24 grab that screenshots and video capture use.
// Split out of cpu/cpu.cpp.
#include "capture.hpp"
#include "vifilter.hpp"
#include "../core/memory.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

namespace kestrel {

// Headless framebuffer dump (debug): reads the current VI framebuffer straight
// out of RDRAM and writes a 24-bit BMP. Independent of the Vulkan presenter so a
// batch/headless run can be eyeballed. Honors 16bpp (RGBA5551) and 32bpp.
auto dumpFramebufferBmp(Memory* mem, const char* path) -> void;

// KESTREL_FBSEQ=patron (printf, %u = numero de campo, p.ej. dir/f%06u.bmp): el framebuffer de
// cada campo cerrado, en tiempo de INVITADO, para montar video fotograma a fotograma; casa
// muestra a muestra con KESTREL_WAVDUMP (el presentador va al ritmo del anfitrion y no sirve).
// KESTREL_FBSEQ_EVERY=N vuelca uno de cada N campos (2 = 30 fps); KESTREL_FBSEQ_FROM=campo
// salta el arranque. KESTREL_FBSEQ_GUEST=1: en vez de por campo, los cuadros que pide el
// invitado escribiendo N en 0x13FF001C (N copias del cuadro en pantalla, volcadas al cierre del
// campo siguiente): un port que va mas lento que la consola emulada marca a SU ritmo y el video
// sale a tiempo de la maquina emulada (el port PSX con PSX_CAPTURE).
auto fbSeqField(Memory* mem, u32 field) -> void {
  static const char* pat = std::getenv("KESTREL_FBSEQ");
  if(!pat) return;
  static const bool guest = [] { const char* e = std::getenv("KESTREL_FBSEQ_GUEST"); return e && e[0] == '1'; }();
  if(guest) {
    static u32 n = 0;
    if(!mem->fbSeqReq) return;
    mem->rdpDrain();
    char path[1024];
    std::snprintf(path, sizeof path, pat, n);
    dumpFramebufferBmp(mem, path);
    for(u32 k = 1; k < mem->fbSeqReq; k++) {         // copias del mismo cuadro
      char p2[1024];
      std::snprintf(p2, sizeof p2, pat, n + k);
      if(FILE* a = std::fopen(path, "rb")) {
        if(FILE* b = std::fopen(p2, "wb")) { char buf[65536]; usize r;
          while((r = std::fread(buf, 1, sizeof buf, a)) > 0) std::fwrite(buf, 1, r, b); std::fclose(b); }
        std::fclose(a);
      }
    }
    n += mem->fbSeqReq;
    mem->fbSeqReq = 0;
    return;
  }
  static const u32 every = [] { const char* e = std::getenv("KESTREL_FBSEQ_EVERY"); u32 n = e ? (u32)std::strtoul(e, nullptr, 0) : 1; return n ? n : 1u; }();
  static const u32 from = [] { const char* e = std::getenv("KESTREL_FBSEQ_FROM"); return e ? (u32)std::strtoul(e, nullptr, 0) : 0u; }();
  if(field < from || (field - from) % every) return;
  char path[1024];
  std::snprintf(path, sizeof path, pat, (field - from) / every);
  mem->rdpDrain();
  dumpFramebufferBmp(mem, path);
}

// Framebuffer que barre el VI en RGB24 de arriba abajo; lo comparten el volcado BMP y las
// capturas (src/video/capture.hpp).
auto grabFramebuffer(Memory* mem, FbGrab& out) -> bool {
  if(!mem) return false;
  // KESTREL_FBFRAME=1: en entrelazado (SERRATE), el cuadro entero de los dos campos en vez
  // del campo que tocaba al salir.
  static const bool frame = [] { const char* e = std::getenv("KESTREL_FBFRAME"); return e && e[0] == '1'; }();
  u32 origin = frame ? mem->rcp.viFrameOrigin() : mem->rcp.vi_origin & 0x00ff'ffff;
  u32 type   = mem->rcp.vi_ctrl & 3;                 // 2=16bpp, 3=32bpp
  u32 srcW = mem->rcp.vi_width ? mem->rcp.vi_width : 320;   // framebuffer line stride (source pixels)
  // Tope = los 12 bits del registro. Capar el PASO a 320 partia el entrelazado (VI_WIDTH
  // 1280 en Donkey Kong 64): el paso no es el ancho, y recortarlo desplaza cada fila. El
  // ancho visible se recorta aparte, mas abajo; las lecturas ya van acotadas por ram.size().
  if(srcW == 0 || srcW > 4095) srcW = 320;
  // This dump is the SOURCE framebuffer exactly as the RDP wrote it: w = VI_WIDTH,
  // one output pixel per stored pixel, no X_SCALE resample.
  //
  // It used to derive the width from the H_VIDEO active window ((hend-hstart)/2),
  // which is the VI's *display* width, and then resample through X_SCALE. That is
  // the presenter's job, not the oracle's: it pinned every dump at 320 columns
  // (standard H_VIDEO $6C02EC = 640 active / 2) regardless of what the ROM
  // actually rendered, so a 640-wide framebuffer came out half as wide and a
  // 160-wide one came out doubled. 181 of the 371 PeterLemon references
  // disagreed on size with the dump for that reason alone, which makes every
  // accuracy number computed from them meaningless. The references are captures
  // of the framebuffer, so the dump has to be the framebuffer.
  // ...salvo que VI_WIDTH sea un PASO mayor que lo que el VI barre de verdad. VI_WIDTH es
  // el paso de linea del framebuffer en pixeles, no el ancho visible: lo visible sale de la
  // ventana activa de VI_H_VIDEO por X_SCALE, igual que en la consola. Coinciden en casi
  // todo (H_VIDEO estandar = 640 relojes activos: X_SCALE 0x200 -> 320, 0x400 -> 640), y
  // por eso el volcado podia usar el paso como ancho. No coinciden en ENTRELAZADO: Donkey
  // Kong 64 arranca en 640x480 con serrate y pone VI_WIDTH=1280 para que cada campo lea
  // lineas alternas del mismo framebuffer; ahi lo visible siguen siendo 640 y el resto del
  // paso es la linea del OTRO campo. Solo se recorta (visW < paso): barrer mas pixeles de
  // los que hay en la linea no pasa nunca, asi que ninguna ROM donde ambos coinciden cambia.
  u32 w = srcW;
  { const u32 hv = mem->rcp.vi_hstart;
    const u32 hs = (hv >> 16) & 0x3ff, he = hv & 0x3ff;
    const u32 xsc = mem->rcp.vi_xscale & 0xfff;
    if(he > hs && xsc) { const u32 visW = ((he - hs) * xsc) >> 10;
                         if(visW && visW < w) w = visW; } }
  // Framebuffer height is NOT fixed at 240 — the VI Y_SCALE register (2.10 fixed,
  // source lines per display line) sets it. Krom's low-res demos use YSCALE 0x200
  // (half → 120 source lines) etc. The native source height the RDP renders into is
  // baseH * y_scale, where baseH is the field's active line count (NTSC 240, PAL 288,
  // picked from the VI_V_SYNC total). Hardcoding 240 left the bottom half black and
  // mis-sized the dump vs the reference for every non-240 test.
  u32 ysc = mem->rcp.vi_yscale & 0xfff;
  u32 baseH = (mem->rcp.viHalflines() >= 550) ? 288 : 240;   // PAL (625) vs NTSC (525)
  u32 h = ysc ? ((baseH * ysc) >> 10) : baseH;
  if(h == 0 || h > 576) h = baseH;
  const auto& ram = mem->rdram;
  auto exp5 = [](u32 v){ return (v << 3) | (v >> 2); };
  // Filtros del VI (AA de cobertura, divot, de-dither) SOLO bajo peticion. Este volcado es
  // el framebuffer tal cual lo escribio el RDP, que es lo que son las referencias de
  // PeterLemon: pasarles el filtro del VI las empeora (medido: RSPGradient 100 -> 95,4,
  // FillRectangle 98,6 -> 97,5), porque el barrido filtrado es otra cosa que el contenido
  // de la RDRAM. El filtro de verdad vive en el presentador, que es lo que se ve. Aqui
  // queda como A/B con KESTREL_VIFILTER=1.
  const u32 viCtrl = mem->rcp.vi_ctrl;
  static const bool viFilt = std::getenv("KESTREL_VIFILTER") != nullptr;
  std::vector<u32> filt;
  if(viFilt && vi::active(viCtrl)) {
    filt.resize((usize)w * h);
    vi::fetchFiltered(ram.data(), ram.size(), mem->rdramHidden.data(), mem->rdramHidden.size(),
                      origin, srcW, w, h, viCtrl, filt.data());
  }
  out.w = w; out.h = h; out.type = type; out.origin = origin;
  out.rgb.assign((usize)w * h * 3, 0);
  for(u32 y = 0; y < h; y++) for(u32 x = 0; x < w; x++) {
    u32 R=0,G=0,B=0;
    u32 sx = x;
    if(!filt.empty()) { u32 c = filt[(usize)y*w+x]; R=c>>16; G=(c>>8)&0xff; B=c&0xff; }
    else if(type == 2) { u32 p = origin + (y*srcW+sx)*2; if(p+1 < ram.size()) { u32 px=((u32)ram[p]<<8)|ram[p+1]; R=exp5((px>>11)&0x1f); G=exp5((px>>6)&0x1f); B=exp5((px>>1)&0x1f); } }
    else if(type == 3) { u32 p = origin + (y*srcW+sx)*4; if(p+3 < ram.size()) { R=ram[p]; G=ram[p+1]; B=ram[p+2]; } }
    usize o = ((usize)y*w + x)*3;
    out.rgb[o]=(u8)R; out.rgb[o+1]=(u8)G; out.rgb[o+2]=(u8)B;
  }
  return true;
}

auto dumpFramebufferBmp(Memory* mem, const char* path) -> void {
  FbGrab g;
  if(!grabFramebuffer(mem, g)) return;
  const u32 w = g.w, h = g.h, type = g.type, origin = g.origin;
  std::vector<u8> rgb((usize)w * h * 3);
  for(u32 y = 0; y < h; y++) for(u32 x = 0; x < w; x++) {   // BMP is bottom-up; store BGR
    const u8* s = &g.rgb[((usize)y*w + x)*3];
    u8* d = &rgb[((usize)(h-1-y)*w + x)*3];
    d[0]=s[2]; d[1]=s[1]; d[2]=s[0];
  }
  u32 rowSize = ((w*3 + 3) & ~3u), imgSize = rowSize*h, fileSize = 54 + imgSize;
  FILE* f = std::fopen(path, "wb"); if(!f) return;
  u8 hdr[54] = {}; hdr[0]='B'; hdr[1]='M';
  auto put32=[&](int o,u32 v){ hdr[o]=v; hdr[o+1]=v>>8; hdr[o+2]=v>>16; hdr[o+3]=v>>24; };
  auto put16=[&](int o,u16 v){ hdr[o]=v; hdr[o+1]=v>>8; };
  put32(2,fileSize); put32(10,54); put32(14,40); put32(18,w); put32(22,h);
  put16(26,1); put16(28,24); put32(34,imgSize);
  std::fwrite(hdr,1,54,f);
  std::vector<u8> row(rowSize,0);
  for(u32 y=0;y<h;y++){ for(u32 x=0;x<w*3;x++) row[x]=rgb[(usize)y*w*3+x]; std::fwrite(row.data(),1,rowSize,f); }
  std::fclose(f);
  std::fprintf(stderr, "[fbdump] wrote %s (%ux%u type=%u origin=%06x)\n", path, w, h, type, origin);
  std::fflush(stderr);
}

}  // namespace kestrel
