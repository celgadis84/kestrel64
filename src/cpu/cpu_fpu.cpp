// COP1: the FPU interpreter (cop1op, FPR access by FR mode) and the COP1 helpers the
// dynarec calls directly (fast paths that fall back to jitInterpOp).
// Split out of cpu.cpp; shared internals live in cpu_internal.hpp.
#include "cpu.hpp"
#include "jit.hpp"
#include "../core/memory.hpp"
#include "cpu_internal.hpp"
#include <bit>
#include <cmath>
#include <cfenv>
#include <cstring>
#include <xmmintrin.h>

#pragma STDC FENV_ACCESS ON

namespace kestrel {

// --- Entorno de coma flotante del host, sin <cfenv> --------------------------
// Cada op COP1 fija el modo de redondeo y limpia el estado IEEE antes de calcular. En
// mingw eso entra en __mingw_setfp, que sincroniza la palabra de control x87 Y MXCSR:
// el perfilador de host lo medía en ~32% del tiempo total del emulador con SM64.
// En x86-64 todo el cálculo de float/double va por SSE, así que el único registro que
// gobierna redondeo y banderas es MXCSR, y leerlo/escribirlo son dos instrucciones.
// Los bits son los mismos que expone <cfenv>, sólo que sin la capa de la CRT.
namespace mx {
constexpr u32 RC = 0x6000;                                       // bits 13-14: redondeo
constexpr u32 RN = 0x0000, RM = 0x2000, RP = 0x4000, RZ = 0x6000; // near/-inf/+inf/cero
constexpr u32 EX = 0x003f;                                       // IE DE ZE OE UE PE
constexpr u32 INVALID = 0x01, DIVZERO = 0x04, OVERFLOW_ = 0x08,
              UNDERFLOW_ = 0x10, INEXACT = 0x20;
// Redondeo pedido + banderas a cero. Escribe sólo si algo cambia: el modo casi nunca
// cambia entre ops, así que el caso normal es una lectura y una comparación.
inline auto prep(u32 rc) -> void {
  u32 v = _mm_getcsr(), w = (v & ~(RC | EX)) | rc;
  if(w != v) _mm_setcsr(w);
}
inline auto flags() -> u32 { return _mm_getcsr() & EX; }
// Solo el modo de redondeo, sin tocar las banderas. Para el camino de ADD/SUB/MUL en
// simple, que deduce el Inexact de la aritmetica y por tanto no necesita el MXCSR limpio:
// asi el `ldmxcsr` -- que serializa el pipe -- solo se paga cuando el guest cambia de modo,
// no en cada operacion por culpa de una bandera pegajosa que se quedo puesta.
inline auto prepRc(u32 rc) -> void {
  u32 v = _mm_getcsr();
  if((v & RC) != rc) _mm_setcsr((v & ~RC) | rc);
}
}  // namespace mx

// ADD/SUB/MUL de FPU con camino rapido, emitidos por el JIT como CALL directo. Son dos
// tercios de todo lo que el bloque cede al interprete en SM64. El caso comun -- operandos
// normales o cero, resultado normal o cero, sin mas bandera IEEE que Inexact y sin el Enable
// de Inexact armado -- no puede desviar el control ni necesita nada del contexto de
// instruccion, asi que se resuelve aqui y se vuelve al bloque. CUALQUIER otra cosa (CU1=0,
// NaN, infinito o subnormal en una entrada o en el resultado, Overflow/Underflow/Invalid,
// un Enable que atrape) delega en jitInterpOp, que ejecuta la op entera por el interprete
// con el contexto exacto que la excepcion necesita. Delegar es seguro en cualquier punto de
// aqui: lo unico que se ha tocado hasta entonces es el MXCSR del anfitrion, que el
// interprete vuelve a fijar en su propio mx::prep.
template<u32 FN, u32 FMT>
auto CPU::jitCop1Alu(u32 op, u32 off) -> u8 {
  if(__builtin_expect(!((u32)cop0[C0_Status] & 0x2000'0000u), 0)) return jitInterpOp(op, off);
  u32 fs = (op >> 11) & 31, ft = (op >> 16) & 31, fd = (op >> 6) & 31;
  // Mismo emparejamiento de registro que el interprete: con FR=0 el campo fuente se alinea
  // a par, y solo el campo fuente (ft y fd conservan su indice crudo).
  if(!((u32)cop0[C0_Status] & (1u << 26))) fs &= ~1u;
  u32 rm = fcr31 & 3;
  u32 rc = (rm == 1) ? mx::RZ : (rm == 2) ? mx::RP : (rm == 3) ? mx::RM : mx::RN;
  if constexpr(FMT == 0x10) {
    u32 ab = (u32)fpr[fs], bb = (u32)fpr[ft];
    // Normal o cero: exponente ni todo-ceros-con-mantisa (subnormal) ni todo-unos (inf/NaN).
    auto plain = [](u32 x) { u32 e = x & 0x7f80'0000u;
                             return (e != 0 || (x & 0x007f'ffffu) == 0) && e != 0x7f80'0000u; };
    if(__builtin_expect(!(plain(ab) && plain(bb)), 0)) return jitInterpOp(op, off);
    float a = std::bit_cast<float>(ab), b = std::bit_cast<float>(bb);
    u32 rb, ex;
    if constexpr(FN == 2) {
      // El producto de dos `float` NORMALES O CERO es EXACTO en `double`: la mantisa cabe
      // (24+24 = 48 bits contra 53) y el exponente tambien (2^-252 .. 2^256 dentro del rango
      // normal del doble). Asi que multiplicar en doble y redondear UNA sola vez a simple da
      // exactamente el mismo bit que multiplicar en simple -- no hay doble redondeo posible --
      // y el Inexact sale de comparar: si el `float` redondeado vuelto a doble no es el
      // producto exacto, es que se perdio algo.
      //
      // Lo que se gana es no tocar el MXCSR. Antes cada operacion pagaba getcsr+setcsr+getcsr,
      // y el setcsr NO se ahorraba nunca porque la bandera PE es pegajosa: en cuanto habia una
      // operacion inexacta todas las siguientes escribian el registro para limpiarla, y
      // `ldmxcsr` serializa el pipeline del anfitrion.
      //
      // Suma y resta NO entran: el resultado exacto de a+b necesita hasta ~277 bits de rango
      // (exponentes muy separados), el doble tambien redondea y entonces la comparacion diria
      // "exacto" cuando en simple no lo es. Medido: pasa en ~1 de cada 3 pares al azar.
      // La division tampoco: a/b en doble y luego a simple si sufre doble redondeo.
      mx::prepRc(rc);
      double d = (double)a * (double)b;
      float r = (float)d;                      // unico redondeo, con el modo del guest puesto
      rb = std::bit_cast<u32>(r);
      // Fuera del rango normal del simple hay que rendirse: por arriba, con redondeo hacia cero
      // o al infinito contrario, un producto que desborda NO da inf sino el maximo finito -- que
      // a ojos de `plain` es un numero cualquiera -- y lleva Overflow ademas de Inexact; por
      // abajo pasa lo mismo con Underflow y el cero. Y de aqui solo sabemos deducir el Inexact.
      // Se mira el producto EXACTO en doble (que no desborda nunca) contra [2^-126, maxfloat].
      u64 db = std::bit_cast<u64>(d) & 0x7fff'ffff'ffff'ffffull;
      if(__builtin_expect(db != 0 && (db < 0x3810'0000'0000'0000ull
                                   || db > 0x47EF'FFFF'E000'0000ull), 0))
        return jitInterpOp(op, off);
      ex = ((double)r != d) ? mx::INEXACT : 0u;
    } else if constexpr(FN != 3) {
      // Suma y resta: el mismo truco que el producto, pero condicionado. a+b en doble es
      // EXACTO cuando los dos exponentes estan cerca -- el resultado exacto ocupa
      // (ea-eb)+25 bits de mantisa, asi que con |ea-eb| <= 28 cabe en los 53 del doble --
      // y entonces redondear UNA vez a simple da el bit del guest y el Inexact sale de
      // comparar. Un cero cuenta como "cerca" de cualquier cosa: sumar cero es exacto
      // siempre (y `plain` ya garantiza que un exponente nulo es un cero, no un subnormal).
      // Cuando los exponentes se separan mucho el doble TAMBIEN redondea, la comparacion
      // diria "exacto" cuando no lo es, y hay que volver al MXCSR. Es el caso raro: el
      // codigo de juego suma magnitudes parecidas.
      u32 ea = (ab >> 23) & 0xffu, eb = (bb >> 23) & 0xffu;
      s32 de = (s32)ea - (s32)eb;
      if(__builtin_expect(ea != 0 && eb != 0 && (de > 28 || de < -28), 0)) {
        mx::prep(rc);
        float r = (FN == 0) ? a + b : a - b;
        rb = std::bit_cast<u32>(r);
        ex = mx::flags();
      } else {
        mx::prepRc(rc);
        double d = (FN == 0) ? (double)a + (double)b : (double)a - (double)b;
        float r = (float)d;                    // unico redondeo, con el modo del guest puesto
        rb = std::bit_cast<u32>(r);
        // Mismo cierre que el producto: fuera del rango normal del simple no sabemos deducir
        // Overflow/Underflow (y con redondeo hacia cero un desbordamiento da el maximo finito,
        // que `plain` no distingue de un numero cualquiera). El doble no desborda nunca aqui.
        u64 db = std::bit_cast<u64>(d) & 0x7fff'ffff'ffff'ffffull;
        if(__builtin_expect(db != 0 && (db < 0x3810'0000'0000'0000ull
                                     || db > 0x47EF'FFFF'E000'0000ull), 0))
          return jitInterpOp(op, off);
        ex = ((double)r != d) ? mx::INEXACT : 0u;
      }
    } else {
      mx::prep(rc);
      float r = a / b;
      rb = std::bit_cast<u32>(r);
      ex = mx::flags();
    }
    if(__builtin_expect(!plain(rb) || (ex & ~mx::INEXACT), 0)) return jitInterpOp(op, off);
    u32 cause = (ex & mx::INEXACT) ? (1u << 12) : 0;
    if(__builtin_expect(cause != 0 && ((fcr31 >> 7) & 1) != 0, 0)) return jitInterpOp(op, off);
    fcr31 = (fcr31 & ~0x0003'F000u) | cause | (cause >> 10);   // Cause I + Flag I pegajosa
    fpr[fd] = (u64)rb;                                          // resultado de 32 bits: limpia el alto
    return 1;
  } else {
    u64 ab = fpr[fs], bb = fpr[ft];
    auto plain = [](u64 x) { u64 e = x & 0x7ff0'0000'0000'0000ull;
                             return (e != 0 || (x & 0x000f'ffff'ffff'ffffull) == 0)
                                    && e != 0x7ff0'0000'0000'0000ull; };
    if(__builtin_expect(!(plain(ab) && plain(bb)), 0)) return jitInterpOp(op, off);
    mx::prep(rc);
    double a = std::bit_cast<double>(ab), b = std::bit_cast<double>(bb);
    double r = (FN == 0) ? a + b : (FN == 1) ? a - b : (FN == 2) ? a * b : a / b;
    u64 rb = std::bit_cast<u64>(r);
    u32 ex = mx::flags();
    if(__builtin_expect(!plain(rb) || (ex & ~mx::INEXACT), 0)) return jitInterpOp(op, off);
    u32 cause = (ex & mx::INEXACT) ? (1u << 12) : 0;
    if(__builtin_expect(cause != 0 && ((fcr31 >> 7) & 1) != 0, 0)) return jitInterpOp(op, off);
    fcr31 = (fcr31 & ~0x0003'F000u) | cause | (cause >> 10);
    fpr[fd] = rb;
    return 1;
  }
}
// CTC1 es un tercio de todo lo que el JIT cedia al interprete en SM64: el compilador de SGI
// reprograma el modo de redondeo de FCSR antes de cada conversion a entero. El caso comun es
// una escritura que NO arma ninguna trampa; solo eso se atiende aqui.
auto CPU::jitCTC1w(u32 op, u32 off) -> u8 {
  if(__builtin_expect(!((u32)cop0[C0_Status] & 0x2000'0000u), 0)) return jitInterpOp(op, off);
  u32 rd = (op >> 11) & 31;
  if(rd != 31) return 1;                     // FCR0 es de solo lectura; el resto no existe
  u32 v = (u32)gpr[(op >> 16) & 31] & 0x0183'FFFFu;
  // Una CTC1 que deja armado E (bit 17) o un Cause con su Enable puesto dispara la excepcion
  // FP en el acto, y eso arrastra el apa�o de Cause.CE: al interprete.
  if(__builtin_expect(((v >> 17) & 1) || (((v >> 12) & 0x1f) & ((v >> 7) & 0x1f)), 0))
    return jitInterpOp(op, off);
  fcr31 = v;
  return 1;
}
extern "C" KJIT_ABI u8 kestrel_jitCTC1(void* c, u32 op, u32 off) {
  return reinterpret_cast<kestrel::CPU*>(c)->jitCTC1w(op, off); }

// Conversiones COP1 emitidas por el JIT como CALL directo. Tras CTC1 son lo siguiente en la
// cuenta de cesiones al interprete en SM64: el compilador de SGI convierte a entero cada vez
// que un float se usa como indice, coordenada o contador (CVT.W.S ~16% de las cesiones,
// TRUNC.W.S ~14%), y cambia de precision al pasar por rutinas de doble (CVT.D.S ~9%,
// CVT.S.D ~5%). Aqui se resuelve SOLO la conversion normal: operando normal-o-cero,
// resultado dentro de rango y ninguna trampa armada. Todo lo demas -- CU1=0, NaN, infinito,
// subnormal, magnitud fuera del entero de destino (que en el VR4300 es Unimplemented, no un
// saturado) y el Inexact con su Enable puesto -- delega en jitInterpOp, que ejecuta la op
// entera por el interprete con el contexto que la excepcion necesita. Delegar es seguro en
// cualquier punto: lo unico tocado hasta entonces es el MXCSR del anfitrion, que el
// interprete vuelve a fijar en su propio mx::prep.
// KIND: 0=CVT.W.S 1=TRUNC.W.S 2=CVT.W.D 3=TRUNC.W.D 4=CVT.D.S 5=CVT.S.D
template<u32 KIND>
auto CPU::jitCop1Cvt(u32 op, u32 off) -> u8 {
  if(__builtin_expect(!((u32)cop0[C0_Status] & 0x2000'0000u), 0)) return jitInterpOp(op, off);
  u32 fs = (op >> 11) & 31, fd = (op >> 6) & 31;
  // Mismo emparejamiento que el interprete: con FR=0 solo el campo FUENTE se alinea a par.
  if(!((u32)cop0[C0_Status] & (1u << 26))) fs &= ~1u;
  // "Plain" = normal o cero: ni subnormal ni infinito ni NaN. El VR4300 no implementa
  // subnormales y manda NaN/inf de una conversion por el camino de excepcion.
  auto plain32 = [](u32 x) { u32 e = x & 0x7f80'0000u;
                             return (e != 0 || (x & 0x007f'ffffu) == 0) && e != 0x7f80'0000u; };
  auto plain64 = [](u64 x) { u64 e = x & 0x7ff0'0000'0000'0000ull;
                             return (e != 0 || (x & 0x000f'ffff'ffff'ffffull) == 0)
                                    && e != 0x7ff0'0000'0000'0000ull; };
  constexpr bool srcD = (KIND == 2 || KIND == 3 || KIND == 5);
  double src;
  if constexpr(srcD) {
    u64 a = fpr[fs];
    if(__builtin_expect(!plain64(a), 0)) return jitInterpOp(op, off);
    src = std::bit_cast<double>(a);
  } else {
    u32 a = (u32)fpr[fs];
    if(__builtin_expect(!plain32(a), 0)) return jitInterpOp(op, off);
    src = (double)std::bit_cast<float>(a);
  }
  u32 rm = fcr31 & 3;
  u32 rc = (rm == 1) ? mx::RZ : (rm == 2) ? mx::RP : (rm == 3) ? mx::RM : mx::RN;
  if constexpr(KIND == 4) {                            // CVT.D.S: ensanchar es siempre exacto
    fcr31 &= ~0x0003'F000u;
    fpr[fd] = std::bit_cast<u64>(src);
    return 1;
  } else if constexpr(KIND == 5) {                     // CVT.S.D: puede redondear
    mx::prep(rc);
    float r = (float)src;
    u32 rb = std::bit_cast<u32>(r);
    u32 ex = mx::flags();
    // Un resultado no-plain es overflow (inf) o underflow (subnormal/cero): camino lento.
    if(__builtin_expect(!plain32(rb) || (ex & ~mx::INEXACT), 0)) return jitInterpOp(op, off);
    u32 cause = (ex & mx::INEXACT) ? (1u << 12) : 0;
    if(__builtin_expect(cause != 0 && ((fcr31 >> 7) & 1) != 0, 0)) return jitInterpOp(op, off);
    fcr31 = (fcr31 & ~0x0003'F000u) | cause | (cause >> 10);   // Cause I + Flag I pegajosa
    fpr[fd] = (u64)rb;
    return 1;
  } else {                                             // .W: redondeo a entero de 32 bits
    constexpr bool trunc = (KIND == 1 || KIND == 3);
    mx::prep(trunc ? mx::RZ : rc);
    double r = std::rint(src);
    u32 ex = mx::flags();
    // Fuera del rango de un entero con signo de 32 bits el VR4300 levanta Unimplemented:
    // no satura. Eso es excepcion, asi que va por el interprete.
    if(__builtin_expect(!(r >= -2147483648.0 && r < 2147483648.0), 0))
      return jitInterpOp(op, off);
    u32 cause = (ex & mx::INEXACT) ? (1u << 12) : 0;
    if(__builtin_expect(cause != 0 && ((fcr31 >> 7) & 1) != 0, 0)) return jitInterpOp(op, off);
    fcr31 = (fcr31 & ~0x0003'F000u) | cause | (cause >> 10);
    fpr[fd] = (u64)(u32)(s32)r;                        // resultado de 32 bits: limpia el alto
    return 1;
  }
}
// CVT.S.W / CVT.D.W: entero de 32 bits a coma flotante. Aqui la fuente no tiene casos raros --
// cualquier patron de 32 bits es un entero con signo valido -- asi que lo unico que puede
// levantarse es Inexact, y solo en .S (un doble representa cualquier int32 de forma exacta).
// Con el Enable de Inexact armado delega en el interprete, igual que el resto de conversiones.
template<u32 KIND>   // 0 = CVT.S.W, 1 = CVT.D.W
auto CPU::jitCop1CvtW(u32 op, u32 off) -> u8 {
  if(__builtin_expect(!((u32)cop0[C0_Status] & 0x2000'0000u), 0)) return jitInterpOp(op, off);
  u32 fs = (op >> 11) & 31, fd = (op >> 6) & 31;
  if(!((u32)cop0[C0_Status] & (1u << 26))) fs &= ~1u;
  s32 v = (s32)(u32)fpr[fs];
  if constexpr(KIND == 1) {                            // .D: exacto siempre, sin banderas
    fcr31 &= ~0x0003'F000u;
    fpr[fd] = std::bit_cast<u64>((double)v);
    return 1;
  } else {                                             // .S: 24 bits de mantisa -> puede redondear
    u32 rm = fcr31 & 3;
    u32 rc = (rm == 1) ? mx::RZ : (rm == 2) ? mx::RP : (rm == 3) ? mx::RM : mx::RN;
    mx::prep(rc);
    float r = (float)v;
    u32 ex = mx::flags();
    u32 cause = (ex & mx::INEXACT) ? (1u << 12) : 0;
    if(__builtin_expect(cause != 0 && ((fcr31 >> 7) & 1) != 0, 0)) return jitInterpOp(op, off);
    fcr31 = (fcr31 & ~0x0003'F000u) | cause | (cause >> 10);
    fpr[fd] = (u64)std::bit_cast<u32>(r);
    return 1;
  }
}
// Oraculo temporal (KESTREL_FPORACLE): el camino rapido calcula, se restaura el estado y el
// interprete ejecuta la MISMA op; destino y fcr31 tienen que salir identicos. Es el patron con
// que se validaron ADD/SUB/MUL de COP1 (ver docs/PERF-RCP-SYNC.md). Sin la variable de entorno
// no cuesta nada: una lectura de bool estatico. Las conversiones son idempotentes -- el
// resultado solo depende de fs y del modo de redondeo -- asi que repetirlas no altera nada mas.
template<u32 KIND>
auto CPU::jitCop1CvtChk(u32 op, u32 off) -> u8 {
  static const bool on = std::getenv("KESTREL_FPORACLE") != nullptr;
  auto fast1 = [&]() -> u8 { if constexpr(KIND >= 6) return jitCop1CvtW<KIND - 6>(op, off);
                             else                   return jitCop1Cvt<KIND>(op, off); };
  if(__builtin_expect(!on, 1)) return fast1();
  u32 fd = (op >> 6) & 31;
  u64 fdPre = fpr[fd]; u32 fcrPre = fcr31;
  static u64 seen = 0;
  if((++seen & 0xFFFFF) == 0)
    std::fprintf(stderr, "[fporacle] comprobadas %llu conversiones sin discrepancia\n", (unsigned long long)seen);

  u8 fast = fast1();
  u64 fdFast = fpr[fd]; u32 fcrFast = fcr31;
  fpr[fd] = fdPre; fcr31 = fcrPre;                    // rebobinar y repetir por el interprete
  u8 slow = jitInterpOp(op, off);
  if(fast != slow || fpr[fd] != fdFast || fcr31 != fcrFast) {
    static u32 n = 0;
    if(n++ < 40)
      std::fprintf(stderr, "[fporacle] kind=%u op=%08x  fast fd=%016llx fcr=%08x r=%u"
                           " | interp fd=%016llx fcr=%08x r=%u\n",
                   KIND, op, (unsigned long long)fdFast, fcrFast, fast,
                   (unsigned long long)fpr[fd], fcr31, slow);
  }
  return slow;
}

#define KC1C(name, kind) \
  extern "C" KJIT_ABI u8 name(void* c, u32 op, u32 off) { \
    auto* p = reinterpret_cast<kestrel::CPU*>(c); \
    kestrel::CPU::FpuCharge g(p, op); \
    return p->jitCop1CvtChk<kind>(op, off); }
KC1C(kestrel_jitCVTWS, 0) KC1C(kestrel_jitTRUNCWS, 1)
KC1C(kestrel_jitCVTWD, 2) KC1C(kestrel_jitTRUNCWD, 3)
KC1C(kestrel_jitCVTDS, 4) KC1C(kestrel_jitCVTSD, 5)
KC1C(kestrel_jitCVTSW, 6) KC1C(kestrel_jitCVTDW, 7)
#undef KC1C

// C.cond.fmt emitida por el JIT como CALL directo. Tras las conversiones es lo siguiente en la
// cuenta de cesiones (~15%): cada comparacion en coma flotante del codigo de SGI acaba en una
// C.LT/C.LE/C.EQ seguida de BC1T/BC1F. Aqui se resuelve el caso ORDENADO: si ningun operando
// es NaN, ninguno de los dieciseis predicados puede levantar Invalid, ni por la via del
// predicado senalizador ni por la rareza del VR4300 con el MSB de la mantisa invertido. Con un
// NaN en cualquiera de los dos (o CU1=0) delega en jitInterpOp. Los subnormales SI valen: el
// interprete tampoco los filtra en la comparacion -- solo lo hacen las ops computacionales -- y
// el `<`/`==` del anfitrion da exactamente el mismo orden. `fn` se queda dinamico: un unico
// trampolin por formato cubre los dieciseis predicados.
template<u32 FMT>
auto CPU::jitCop1Cmp(u32 op, u32 off) -> u8 {
  if(__builtin_expect(!((u32)cop0[C0_Status] & 0x2000'0000u), 0)) return jitInterpOp(op, off);
  u32 fs = (op >> 11) & 31, ft = (op >> 16) & 31, fn = op & 63;
  // Mismo emparejamiento que el interprete: con FR=0 solo el campo FUENTE se alinea a par.
  if(!((u32)cop0[C0_Status] & (1u << 26))) fs &= ~1u;
  bool less, equal;
  if constexpr(FMT == 0x10) {
    u32 ab = (u32)fpr[fs], bb = (u32)fpr[ft];
    if(__builtin_expect((ab & 0x7fff'ffffu) > 0x7f80'0000u
                     || (bb & 0x7fff'ffffu) > 0x7f80'0000u, 0)) return jitInterpOp(op, off);
    float a = std::bit_cast<float>(ab), b = std::bit_cast<float>(bb);
    less = a < b; equal = a == b;
  } else {
    u64 ab = fpr[fs], bb = fpr[ft];
    if(__builtin_expect((ab & 0x7fff'ffff'ffff'ffffull) > 0x7ff0'0000'0000'0000ull
                     || (bb & 0x7fff'ffff'ffff'ffffull) > 0x7ff0'0000'0000'0000ull, 0))
      return jitInterpOp(op, off);
    double a = std::bit_cast<double>(ab), b = std::bit_cast<double>(bb);
    less = a < b; equal = a == b;
  }
  // Ordenado: Cause sale limpia y no hay Flag pegajosa que acumular. El bit `unordered` del
  // predicado (fn & 1) nunca se cumple aqui, asi que no entra en la condicion.
  bool c = (((fn & 0x4) != 0) && less) || (((fn & 0x2) != 0) && equal);
  fcr31 &= ~0x0003'F000u;
  if(c) fcr31 |= (1u << 23); else fcr31 &= ~(1u << 23);
  return 1;
}
// Mismo oraculo que las conversiones (KESTREL_FPORACLE): la comparacion solo escribe fcr31 y
// depende unicamente de fs/ft, asi que rebobinando fcr31 se puede repetir por el interprete.
template<u32 FMT>
auto CPU::jitCop1CmpChk(u32 op, u32 off) -> u8 {
  static const bool on = std::getenv("KESTREL_FPORACLE") != nullptr;
  if(__builtin_expect(!on, 1)) return jitCop1Cmp<FMT>(op, off);
  u32 fcrPre = fcr31;
  static u64 seen = 0;
  if((++seen & 0xFFFFF) == 0)
    std::fprintf(stderr, "[fporacle] comprobadas %llu comparaciones sin discrepancia\n", (unsigned long long)seen);

  u8 fast = jitCop1Cmp<FMT>(op, off);
  u32 fcrFast = fcr31;
  fcr31 = fcrPre;                                     // rebobinar y repetir por el interprete
  u8 slow = jitInterpOp(op, off);
  if(fast != slow || fcr31 != fcrFast) {
    static u32 n = 0;
    if(n++ < 40)
      std::fprintf(stderr, "[fporacle] cmp fmt=%02x op=%08x  fast fcr=%08x r=%u"
                           " | interp fcr=%08x r=%u\n",
                   FMT, op, fcrFast, fast, fcr31, slow);
  }
  return slow;
}
extern "C" KJIT_ABI u8 kestrel_jitCMPS(void* c, u32 op, u32 off) {
  auto* p = reinterpret_cast<kestrel::CPU*>(c);
  kestrel::CPU::FpuCharge g(p, op);
  return p->jitCop1CmpChk<0x10>(op, off); }
extern "C" KJIT_ABI u8 kestrel_jitCMPD(void* c, u32 op, u32 off) {
  auto* p = reinterpret_cast<kestrel::CPU*>(c);
  kestrel::CPU::FpuCharge g(p, op);
  return p->jitCop1CmpChk<0x11>(op, off); }

// Oraculo diferencial (KESTREL_FPORACLE), igual que conversiones y comparaciones: el camino
// rapido calcula, se rebobina fd y fcr31, y el interprete ejecuta la MISMA op. La aritmetica
// es idempotente -- el resultado solo depende de fs, ft y el modo de redondeo, y fd es lo
// unico que escribe, asi que rebobinarlo basta aunque fd aliasee a fs o a ft.
template<u32 FN, u32 FMT>
auto CPU::jitCop1AluChk(u32 op, u32 off) -> u8 {
  static const bool on = std::getenv("KESTREL_FPORACLE") != nullptr;
  if(__builtin_expect(!on, 1)) return jitCop1Alu<FN, FMT>(op, off);
  u32 fd = (op >> 6) & 31;
  u64 fdPre = fpr[fd]; u32 fcrPre = fcr31;
  static u64 seen = 0;
  if((++seen & 0xFFFFF) == 0)
    std::fprintf(stderr, "[fporacle] comprobadas %llu operaciones aritmeticas sin discrepancia\n",
                 (unsigned long long)seen);

  u8 fast = jitCop1Alu<FN, FMT>(op, off);
  u64 fdFast = fpr[fd]; u32 fcrFast = fcr31;
  fpr[fd] = fdPre; fcr31 = fcrPre;                    // rebobinar y repetir por el interprete
  u8 slow = jitInterpOp(op, off);
  if(fast != slow || fpr[fd] != fdFast || fcr31 != fcrFast) {
    static u32 n = 0;
    if(n++ < 40)
      std::fprintf(stderr, "[fporacle] alu fn=%u fmt=%02x op=%08x  fast fd=%016llx fcr=%08x r=%u"
                           " | interp fd=%016llx fcr=%08x r=%u\n",
                   FN, FMT, op, (unsigned long long)fdFast, fcrFast, fast,
                   (unsigned long long)fpr[fd], fcr31, slow);
  }
  return slow;
}

#define KC1A(name, fn, fmt) \
  extern "C" KJIT_ABI u8 name(void* c, u32 op, u32 off) { \
    auto* p = reinterpret_cast<kestrel::CPU*>(c); \
    kestrel::CPU::FpuCharge g(p, op); \
    return p->jitCop1AluChk<fn, fmt>(op, off); }
KC1A(kestrel_jitADDS, 0, 0x10) KC1A(kestrel_jitSUBS, 1, 0x10) KC1A(kestrel_jitMULS, 2, 0x10)
KC1A(kestrel_jitADDD, 0, 0x11) KC1A(kestrel_jitSUBD, 1, 0x11) KC1A(kestrel_jitMULD, 2, 0x11)
KC1A(kestrel_jitDIVS, 3, 0x10) KC1A(kestrel_jitDIVD, 3, 0x11)
#undef KC1A

// Movimientos COP1 (MFC1/DMFC1/CFC1/MTC1/DMTC1) emitidos por el JIT como CALL directo.
// Son una cuarta parte de todo lo que el bloque cede al interprete en SM64 y no pueden
// desviar el control: mueven 32/64 bits entre un gpr y el banco FPU y nada mas. El camino
// normal se salta entero el montaje de contexto de jitInterpOp (guardar pc/nextPc/curPc,
// fabricar la VA de la op, y las seis comprobaciones de desviacion al volver) y el switch
// de cop1op. El unico caso que SI puede desviar -- CU1=0, que vectoriza Coprocessor
// Unusable -- delega en jitInterpOp, que monta el contexto exacto que necesita la excepcion.
template<u32 RSc>
auto CPU::jitCop1Move(u32 op, u32 off) -> u8 {
  if(__builtin_expect(!((u32)cop0[C0_Status] & 0x2000'0000u), 0)) return jitInterpOp(op, off);
  u32 rt = (op >> 16) & 31, rd = (op >> 11) & 31;
  if      constexpr(RSc == 0) set(rt, sext32(fprGet32(rd)));                       // MFC1
  else if constexpr(RSc == 1) set(rt, fprGet64(rd));                               // DMFC1
  else if constexpr(RSc == 2) set(rt, sext32(rd == 31 ? fcr31 : rd == 0 ? fcr0 : 0));  // CFC1
  else if constexpr(RSc == 4) fprSet32(rd, (u32)gpr[rt]);                          // MTC1
  else                        fprSet64(rd, gpr[rt]);                               // DMTC1
  return 1;
}
#define KC1(name, rs)   extern "C" KJIT_ABI u8 name(void* c, u32 op, u32 off) {     return reinterpret_cast<kestrel::CPU*>(c)->jitCop1Move<rs>(op, off); }
KC1(kestrel_jitMFC1, 0) KC1(kestrel_jitDMFC1, 1) KC1(kestrel_jitCFC1, 2)
KC1(kestrel_jitMTC1, 4) KC1(kestrel_jitDMTC1, 5)
#undef KC1

auto CPU::cop1op(u32 op) -> void {
  // Coprocessor-1 usability: FP ops require Status.CU1 (bit 29). A thread with FP
  // disabled traps here (ExcCode 11, Coprocessor Unusable, CE=1); the OS is what
  // enables FP / saves-restores FP context per thread. Skipping this check lets a
  // CU1=0 thread scribble FP registers the OS never preserves across interrupts.
  if(!((u32)cop0[C0_Status] & 0x2000'0000u)) {
    takeException(11);
    cop0[C0_Cause] = sext32(((u32)cop0[C0_Cause] & ~0x3000'0000u) | (1u << 28));  // CE = 1
    return;
  }
  u32 rs = RS;
  switch(rs) {
  case 0x00: /*MFC1*/ set(RT, sext32(fprGet32(RD))); return;
  case 0x01: /*DMFC1*/ set(RT, fprGet64(RD)); return;
  case 0x02: /*CFC1*/ set(RT, sext32(RD == 31 ? fcr31 : RD == 0 ? fcr0 : 0)); return;
  case 0x04: /*MTC1*/ fprSet32(RD, (u32)gpr[RT]); return;
  case 0x05: /*DMTC1*/ fprSet64(RD, gpr[RT]); return;
  case 0x06: /*CTC1*/ if(RD == 31) {  // FCSR write mask
      fcr31 = (u32)gpr[RT] & 0x0183'FFFFu;
      // A CTC1 that leaves an armed Cause bit fires the FP exception immediately:
      // E (bit17, Unimplemented) always traps; V/Z/O/U/I (cause [16:12]) trap when
      // the matching Enable bit ([11:7]) is set.
      u32 c5 = (fcr31 >> 12) & 0x1f, en = (fcr31 >> 7) & 0x1f;
      if(((fcr31 >> 17) & 1) || (c5 & en)) {
        // Pipeline quirk: the FPE is recognized on CTC1 (EPC=CTC1) but Cause.CE
        // reflects the coprocessor number of the NEXT instruction (the one already
        // fetched into the pipe). MFC1(COP1)->1, MFC2(COP2)->2, NOP/other->0.
        // In the delay-slot model `pc` already points at that next instruction.
        u32 nxt = mem ? mem->read32((u32)pc) : 0;
        u32 nop = nxt >> 26, ce = (nop >= 0x10 && nop <= 0x13) ? (nop - 0x10) : 0;
        takeException(15);
        cop0[C0_Cause] = sext32(((u32)cop0[C0_Cause] & ~0x3000'0000u) | (ce << 28));
      }
    }
    return;
  case 0x08: /*BC1*/ {  // BC1F/BC1T (+ likely variants nd=bit1, tf=bit0 of rt)
    bool cond = (fcr31 >> 23) & 1;
    bool tf = RT & 1;
    bool likely = RT & 2;
    bool take = (tf == cond);
    if(likely) { if(take) { justBranched = true; nextPc = pc + (SIMM<<2); } else { pc = nextPc; nextPc = pc + 4; } }
    else branch(take, pc + (SIMM<<2));
    return;
  }
  default: break;  // fall through to the format (arithmetic/convert/compare) ops
  }

  // Latencia de la tabla 7-14. Solo si ningun trampolin COP1 del JIT la cobro ya (ver
  // CPU::FpuCharge): ese camino puede acabar aqui via jitInterpOp y se contaria dos veces.
  if(!fpuNested) chargeFpu(op);

  // fmt in rs: 0x10 S(single), 0x11 D(double), 0x14 W(int32), 0x15 L(int64).
  // Fields: ft=RT, fs=RD, fd=SA, funct.
  u32 fmt = rs, ft = RT, fs = RD, fd = SA, fn = FUNCT;
  // Half mode (Status FR=0): computational/convert/compare ops drop the LOW BIT of
  // the source field fs only. ft and fd keep their raw index, and every operand is
  // the low 32 (.S) / full 64 (.D) of that raw register — no odd->high-half pairing
  // (that pairing is a MFC1/MTC1/LWC1 quirk, handled above). See cop1/full_vs_half_mode.
  if(!((u32)cop0[C0_Status] & (1u<<26))) fs &= ~1u;
  auto getS = [&](u32 i){ return std::bit_cast<float>((u32)fpr[i]); };
  auto getD = [&](u32 i){ return std::bit_cast<double>(fpr[i]); };
  // A 32-bit arithmetic/convert result CLEARS the upper 32 bits of the destination
  // register (unlike MTC1/LWC1, which preserve them), in both FR modes.
  auto set32c = [&](u32 i, u32 v){ fpr[i] = (u64)v; };
  auto setFlag = [&](bool c){ if(c) fcr31 |= (1u<<23); else fcr31 &= ~(1u<<23); };

  // --- FCSR exception model --------------------------------------------------
  // Every computational COP1 op rewrites the Cause field [17:12] (I=12,U=13,O=14,
  // Z=15,V=16,E=17). If any raised exception has its Enable bit [11:7] set the op
  // instead traps (FPE, ExcCode 15) and the sticky Flags [6:2] are left untouched;
  // otherwise the raised bits accumulate into Flags. Host <cfenv> supplies the IEEE
  // status for arithmetic; conversions to integer detect inexact/overflow directly.
  auto rmHost = [&]() -> u32 {
    switch(fcr31 & 3) { case 1: return mx::RZ; case 2: return mx::RP; case 3: return mx::RM; default: return mx::RN; }
  };
  bool flushDenorm = (fcr31 >> 24) & 1;                 // FCSR FS bit: flush denormals to zero
  bool ueEn = (fcr31 >> 8) & 1, ieEn = (fcr31 >> 7) & 1; // underflow / inexact enables
  auto unimpl = [&](){ fcr31 = (fcr31 & ~0x0003'F000u) | (1u << 17); takeException(15); };  // Unimplemented-Operation (E)
  // Fold host IEEE flags into FCSR after an arithmetic op; returns true if it trapped.
  auto harvest = [&]() -> bool {
    u32 ex = mx::flags();
    // VR4300: Underflow is never a normal trappable cause. A tiny result that rounds
    // all the way to zero still underflows: with FS=0, or with the U/I enable set, the
    // VR4300 raises Unimplemented-Operation; otherwise it silently sets U+I and writes
    // the (signed-zero) result. Subnormal results proper are caught earlier in setS/setD.
    if(ex & mx::UNDERFLOW_) {
      if(!flushDenorm || ueEn || ieEn) { unimpl(); return true; }
      fcr31 = (fcr31 & ~0x0003'F000u) | (1u << 13) | (1u << 12);  // Cause U+I
      fcr31 |= (1u << 3) | (1u << 2);                             // Flag  U+I
      return false;
    }
    u32 cause = 0;
    if(ex & mx::INEXACT)   cause |= 1u << 12;
    if(ex & mx::OVERFLOW_)  cause |= 1u << 14;
    if(ex & mx::DIVZERO)   cause |= 1u << 15;
    if(ex & mx::INVALID)   cause |= 1u << 16;
    fcr31 = (fcr31 & ~0x0003'F000u) | cause;
    u32 c5 = (cause >> 12) & 0x1f, en = (fcr31 >> 7) & 0x1f;
    if(c5 & en) { takeException(15); return true; }
    fcr31 |= (c5 << 2);            // accumulate sticky Flags [6:2]
    return false;
  };
  auto snan32 = [](u32 x){ return (x & 0x7fff'ffffu) > 0x7f80'0000u && !((x >> 22) & 1); };
  auto snan64 = [](u64 x){ return (x & 0x7fff'ffff'ffff'ffffull) > 0x7ff0'0000'0000'0000ull && !((x >> 51) & 1); };
  // When FS=1 flushes a subnormal result, the flushed magnitude depends on the
  // rounding mode: a directed rounding *away from zero* (toward +inf for a positive
  // value, toward -inf for a negative one) rounds the subnormal up to the smallest
  // normal instead of down to a signed zero. RM: 0 nearest, 1 toward-zero, 2 +inf,
  // 3 -inf. minNorm carries the sign bit already present in `sign`.
  auto flushMin32 = [&](u32 sign) -> u32 {
    u32 rm = fcr31 & 3;
    if((rm == 2 && sign == 0) || (rm == 3 && sign != 0)) return sign | 0x0080'0000u;
    return sign;   // -> signed zero
  };
  auto flushMin64 = [&](u64 sign) -> u64 {
    u32 rm = fcr31 & 3;
    if((rm == 2 && sign == 0) || (rm == 3 && sign != 0)) return sign | 0x0010'0000'0000'0000ull;
    return sign;
  };
  // Result setters. The VR4300 substitutes its canonical qNaN (0x7fbfffff /
  // 0x7ff7ffff…) for any NaN an arithmetic op produces. A subnormal result is not
  // representable by the VR4300 FPU: with FS=1 (and no U/I enable) it is flushed
  // (to a signed zero or the smallest normal, see flushMin) with Underflow+Inexact
  // raised; otherwise it raises Unimplemented.
  auto setS = [&](u32 i, float v){
    u32 b = std::bit_cast<u32>(v);
    if((b & 0x7f80'0000u) == 0 && (b & 0x007f'ffffu) != 0) {      // subnormal
      if(!flushDenorm || ueEn || ieEn) { unimpl(); return; }
      set32c(i, flushMin32(b & 0x8000'0000u));
      fcr31 = (fcr31 & ~0x0003'F000u) | (1u << 13) | (1u << 12);  // Cause U+I
      fcr31 |= (1u << 3) | (1u << 2);                             // Flag  U+I
      return;
    }
    if((b & 0x7fff'ffffu) > 0x7f80'0000u) b = 0x7fbf'ffffu;
    if(!harvest()) set32c(i, b);   // on a trapped exception the destination is left untouched
  };
  auto setD = [&](u32 i, double v){
    u64 b = std::bit_cast<u64>(v);
    if((b & 0x7ff0'0000'0000'0000ull) == 0 && (b & 0x000f'ffff'ffff'ffffull) != 0) {  // subnormal
      if(!flushDenorm || ueEn || ieEn) { unimpl(); return; }
      fpr[i] = flushMin64(b & 0x8000'0000'0000'0000ull);
      fcr31 = (fcr31 & ~0x0003'F000u) | (1u << 13) | (1u << 12);
      fcr31 |= (1u << 3) | (1u << 2);
      return;
    }
    if((b & 0x7fff'ffff'ffff'ffffull) > 0x7ff0'0000'0000'0000ull) b = 0x7ff7'ffff'ffff'ffffull;
    if(!harvest()) fpr[i] = b;   // on a trapped exception the destination is left untouched
  };
  auto setL = [&](u32 i, u64 v){ fpr[i] = v; };   // raw .L / MOV result (no exceptions), dest index raw
  // Convert a float source to a W(32)/L(64) integer. NaN, ±inf, subnormal inputs, or
  // an out-of-range result raise the Unimplemented-Operation exception (E) on the
  // VR4300 instead of producing a value; an inexact conversion sets I.
  auto cvtInt = [&](double src, u32 hostRnd, bool isL) {
    u32 save = _mm_getcsr();
    _mm_setcsr((save & ~(mx::RC | mx::EX)) | hostRnd);
    double r = std::rint(src);
    u32 ex = _mm_getcsr() & mx::INEXACT;
    // Restaura SÓLO el redondeo, igual que hacía fesetround(save): las banderas que
    // acabe de levantar esta conversión se quedan, y la siguiente op las limpia.
    _mm_setcsr((_mm_getcsr() & ~mx::RC) | (save & mx::RC));
    double lim = isL ? 9223372036854775808.0 : 2147483648.0;   // 2^63 / 2^31
    // .L conversions only resolve results that fit in 53 significant bits; a magnitude
    // >= 2^53 needs a 54th bit the VR4300 conversion unit lacks, so it raises Unimplemented.
    bool bad = !(src == src) || std::isinf(src) || r >= lim || r < -lim
             || (isL && std::fabs(r) >= 9007199254740992.0)
             || (src != 0.0 && std::fpclassify(src) == FP_SUBNORMAL);
    if(bad) { fcr31 = (fcr31 & ~0x0003'F000u) | (1u << 17); takeException(15); return; }
    u32 cause = ex ? (1u << 12) : 0;
    fcr31 = (fcr31 & ~0x0003'F000u) | cause;
    if(cause && ((fcr31 >> 7) & 1)) { takeException(15); return; }  // inexact enabled -> trap
    if(cause) fcr31 |= (1u << 2);
    if(isL) setL(fd, (u64)(s64)r); else set32c(fd, (u32)(s32)r);
  };

  // Prepare host rounding + a clean IEEE status for the arithmetic ops below.
  mx::prep(rmHost());

  if(fn >= 0x30) {  // C.cond.fmt — compare, set fcr31 C bit. bit0=less, bit1=equal, bit2 unord.
    bool isD = (fmt == 0x11);
    u64 ab = isD ? fpr[fs] : (u32)fpr[fs];
    u64 bb = isD ? fpr[ft] : (u32)fpr[ft];
    auto isNan  = [&](u64 x){ return isD ? (x & 0x7fff'ffff'ffff'ffffull) > 0x7ff0'0000'0000'0000ull
                                         : (x & 0x7fff'ffffu) > 0x7f80'0000u; };
    // VR4300 quirk: the mantissa MSB is inverted from IEEE-2008. A NaN with the MSB
    // SET (0x7ff8.. — what the systemtest calls QUIET) is treated as *signalling* and
    // raises Invalid on any compare. A NaN with the MSB CLEAR (0x7ff0..0x7ff7 — the
    // systemtest's SIGNALLING, and the VR4300's own canonical output NaN) raises
    // nothing on the non-signalling predicates and only fires on the signalling ones.
    auto msbSet = [&](u64 x){ if(!isNan(x)) return false; return isD ? ((x >> 51) & 1) != 0 : ((x >> 22) & 1) != 0; };
    bool unordered = isNan(ab) || isNan(bb);
    bool less = false, equal = false;
    if(!unordered) {
      if(isD) { double a = getD(fs), b = getD(ft); less = a < b; equal = a == b; }
      else    { float  a = getS(fs), b = getS(ft); less = a < b; equal = a == b; }
    }
    bool sig = (fn & 0x08) != 0;                       // signaling predicate (C.SF..C.NGT)
    bool invalid = sig ? unordered : (msbSet(ab) || msbSet(bb));
    fcr31 &= ~0x0003'F000u;
    if(invalid) {
      fcr31 |= (1u << 16);                             // Cause V
      if((fcr31 >> 11) & 1) { takeException(15); return; }
      fcr31 |= (1u << 6);                              // Flag V
    }
    bool c = false;
    if(fn & 0x4) c |= less;
    if(fn & 0x2) c |= equal;
    if(fn & 0x1) c |= unordered;
    setFlag(c);
    return;
  }

  // MOV.S / MOV.D copy the raw 64-bit register and, uniquely among FPU ops, leave
  // FCSR (including the Cause field) completely untouched — so this must run before
  // the Cause-clear below.
  if(fn == 0x06 && (fmt == 0x10 || fmt == 0x11)) { setL(fd, fpr[fs]); return; }

  fcr31 &= ~0x0003'F000u;   // computational op: start with a clear Cause field

  // NaN / subnormal operand handling for add/sub/mul/div/sqrt/abs/neg. The VR4300 FPU
  // does not implement subnormals or signalling NaNs: either raises Unimplemented-Op.
  // A quiet-NaN operand is treated as signalling too — it raises Invalid-Operation and
  // yields the canonical qNaN (trapping only if Invalid is enabled). ABS/NEG are not
  // pure sign-flips: they detect NaN/subnormal operands exactly like arithmetic.
  // CVT.D.S (single->double, fn 0x21) and CVT.S.D (double->single, fn 0x20) also read a
  // float operand and fault the same way on a subnormal/NaN source. They are handled here
  // too — but their RESULT is in the *destination* format, not the source. (The integer
  // source CVTs in fmt 0x14/0x15 have no subnormal/NaN inputs and are excluded.)
  bool f2fCvt = (fmt == 0x10 && fn == 0x21) || (fmt == 0x11 && fn == 0x20);
  // Float->int conversions (ROUND/TRUNC/CEIL/FLOOR/CVT to .W/.L) screen their source
  // in its native format: a signalling OR quiet NaN, or a subnormal, all raise
  // Unimplemented (unlike arithmetic, where a qNaN raises Invalid). Detecting this on
  // the native bits matters — a single subnormal widens to a *normal* double.
  bool intCvt = (fn >= 0x08 && fn <= 0x0f) || fn == 0x24 || fn == 0x25;
  if((fn <= 0x07 && fn != 0x06) || f2fCvt || intCvt) {
    bool binary = (fn <= 0x03);                       // only add/sub/mul/div read operand b
    bool sNan = false, subn = false, qNan = false;
    if(fmt == 0x10) {
      u32 a = (u32)fpr[fs], b = (u32)fpr[ft];
      auto sub = [](u32 x){ return (x & 0x7f80'0000u) == 0 && (x & 0x007f'ffffu) != 0; };
      auto nan = [](u32 x){ return (x & 0x7fff'ffffu) > 0x7f80'0000u; };
      sNan = snan32(a) || (binary && snan32(b));
      subn = sub(a)    || (binary && sub(b));
      qNan = (nan(a) && !snan32(a)) || (binary && nan(b) && !snan32(b));
    } else if(fmt == 0x11) {
      u64 a = fpr[fs], b = fpr[ft];
      auto sub = [](u64 x){ return (x & 0x7ff0'0000'0000'0000ull) == 0 && (x & 0x000f'ffff'ffff'ffffull) != 0; };
      auto nan = [](u64 x){ return (x & 0x7fff'ffff'ffff'ffffull) > 0x7ff0'0000'0000'0000ull; };
      sNan = snan64(a) || (binary && snan64(b));
      subn = sub(a)    || (binary && sub(b));
      qNan = (nan(a) && !snan64(a)) || (binary && nan(b) && !snan64(b));
    }
    if(fpDbg && std::getenv("KESTREL_ARGDBG")) {
      std::fprintf(stderr, "[arg] fn=%u fmt=%u fs=%u ft=%u a=%08x b=%08x fcr31=%08x FS=%d en=0x%x sNan=%d subn=%d qNan=%d retired=%llu\n",
        fn, fmt, fs, ft, fprGet32(fs), fprGet32(ft), fcr31, (fcr31>>24)&1, (fcr31>>7)&0x1f, sNan, subn, qNan, (unsigned long long)retired);
      std::fflush(stderr);
    }
    if(sNan || subn) { unimpl(); return; }
    if(qNan && intCvt) { unimpl(); return; }            // qNaN -> int is Unimplemented, not Invalid
    if(qNan) {
      fcr31 |= (1u << 16);                              // Cause V (invalid)
      if((fcr31 >> 11) & 1) { takeException(15); return; }
      fcr31 |= (1u << 6);                               // Flag V
      // canonical qNaN in the *destination* format (CVT changes format; everything else keeps it)
      bool dstSingle = f2fCvt ? (fmt == 0x11) : (fmt == 0x10);
      if(dstSingle) set32c(fd, 0x7fbf'ffffu); else fprSet64(fd, 0x7ff7'ffff'ffff'ffffull);
      return;
    }
  }

  if(fmt == 0x10) {  // single
    switch(fn) {
    case 0x00: setS(fd, getS(fs) + getS(ft)); return;
    case 0x01: setS(fd, getS(fs) - getS(ft)); return;
    case 0x02: setS(fd, getS(fs) * getS(ft)); return;
    case 0x03: setS(fd, getS(fs) / getS(ft)); return;
    case 0x04: setS(fd, std::sqrt(getS(fs))); return;
    case 0x05: set32c(fd, (u32)fpr[fs] & 0x7fff'ffffu); return;   // ABS.S (clear sign, no exceptions)
    case 0x06: setL(fd, fpr[fs]); return;                 // MOV.S (copies the full 64 bits, upper included)
    case 0x07: set32c(fd, (u32)fpr[fs] ^ 0x8000'0000u); return;   // NEG.S (flip sign, no exceptions)
    case 0x08: cvtInt(getS(fs), mx::RN, true);  return; // ROUND.L.S
    case 0x09: cvtInt(getS(fs), mx::RZ, true); return; // TRUNC.L.S
    case 0x0a: cvtInt(getS(fs), mx::RP, true);     return; // CEIL.L.S
    case 0x0b: cvtInt(getS(fs), mx::RM, true);   return; // FLOOR.L.S
    case 0x0c: cvtInt(getS(fs), mx::RN, false); return; // ROUND.W.S
    case 0x0d: cvtInt(getS(fs), mx::RZ, false);return; // TRUNC.W.S
    case 0x0e: cvtInt(getS(fs), mx::RP, false);    return; // CEIL.W.S
    case 0x0f: cvtInt(getS(fs), mx::RM, false);  return; // FLOOR.W.S
    case 0x21: setD(fd, (double)getS(fs)); return;        // CVT.D.S
    case 0x24: cvtInt(getS(fs), rmHost(), false); return;      // CVT.W.S
    case 0x25: cvtInt(getS(fs), rmHost(), true);  return;      // CVT.L.S
    }
  } else if(fmt == 0x11) {  // double
    switch(fn) {
    case 0x00: setD(fd, getD(fs) + getD(ft)); return;
    case 0x01: setD(fd, getD(fs) - getD(ft)); return;
    case 0x02: setD(fd, getD(fs) * getD(ft)); return;
    case 0x03: setD(fd, getD(fs) / getD(ft)); return;
    case 0x04: setD(fd, std::sqrt(getD(fs))); return;
    case 0x05: setL(fd, fpr[fs] & 0x7fff'ffff'ffff'ffffull); return;   // ABS.D
    case 0x06: setL(fd, fpr[fs]); return;                 // MOV.D
    case 0x07: setL(fd, fpr[fs] ^ 0x8000'0000'0000'0000ull); return;   // NEG.D
    case 0x08: cvtInt(getD(fs), mx::RN, true);  return; // ROUND.L.D
    case 0x09: cvtInt(getD(fs), mx::RZ, true); return; // TRUNC.L.D
    case 0x0a: cvtInt(getD(fs), mx::RP, true);     return; // CEIL.L.D
    case 0x0b: cvtInt(getD(fs), mx::RM, true);   return; // FLOOR.L.D
    case 0x0c: cvtInt(getD(fs), mx::RN, false); return; // ROUND.W.D
    case 0x0d: cvtInt(getD(fs), mx::RZ, false);return; // TRUNC.W.D
    case 0x0e: cvtInt(getD(fs), mx::RP, false);    return; // CEIL.W.D
    case 0x0f: cvtInt(getD(fs), mx::RM, false);  return; // FLOOR.W.D
    case 0x20: setS(fd, (float)getD(fs)); return;         // CVT.S.D
    case 0x24: cvtInt(getD(fs), rmHost(), false); return;      // CVT.W.D
    case 0x25: cvtInt(getD(fs), rmHost(), true);  return;      // CVT.L.D
    }
  } else if(fmt == 0x14) {  // W (int32 source)
    s32 v = (s32)(u32)fpr[fs];
    switch(fn) {
    case 0x20: setS(fd, (float)v); return;                // CVT.S.W
    case 0x21: setD(fd, (double)v); return;               // CVT.D.W
    }
  } else if(fmt == 0x15) {  // L (int64 source)
    s64 v = (s64)fpr[fs];
    // CVT.S.L / CVT.D.L only accept a 64-bit integer that fits in a signed 56-bit field
    // (i.e. bits [63:55] all equal the sign bit). Anything larger raises Unimplemented on
    // the VR4300 — the conversion hardware simply does not handle it. (W source is int32,
    // always in range, so it is not checked.)
    if((fn == 0x20 || fn == 0x21)) {
      s64 top = v >> 55;
      if(top != 0 && top != -1) { unimpl(); return; }
    }
    switch(fn) {
    case 0x20: setS(fd, (float)v); return;                // CVT.S.L
    case 0x21: setD(fd, (double)v); return;               // CVT.D.L
    }
  }
  // Invalid/reserved COP1 encoding (e.g. CVT.S.S, bad fmt): the VR4300 raises the
  // FP Unimplemented-Operation exception (FCSR Cause bit E=17), not a host halt.
  fcr31 = (fcr31 & ~0x0003'F000u) | (1u << 17);  // clear all Cause[17:12], set only E
  takeException(15);  // FPE
}

}  // namespace kestrel
