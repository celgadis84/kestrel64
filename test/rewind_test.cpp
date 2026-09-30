// Prueba del codec de diferencias del rebobinado (src/core/rewind.hpp, namespace codec).
//
// Lo que se comprueba es lo unico que importa de el: aplicar la diferencia sobre la foto
// NUEVA tiene que devolver la VIEJA byte a byte, para cualquier par de fotos. Se prueban a
// mano los casos que rompen este tipo de codigo (fotos de distinto tamano, cambios sueltos,
// cambios no alineados a palabra, tamanos que no son multiplo de cuatro, todo igual, todo
// distinto, vacias) y despues mil pares al azar con cambios dispersos, que es la forma real
// que tienen: dos campos de video seguidos difieren en trozos pequenos y repartidos.

#include "../src/core/rewind.hpp"

#include <cstdio>
#include <cstdlib>
#include <random>
#include <vector>

using namespace kestrel;

namespace {

int fails = 0;

auto check(const char* what, const std::vector<u8>& nueva, const std::vector<u8>& vieja) -> void {
  std::vector<u8> d, back;
  rewind::codec::makeDelta(nueva, vieja, d);
  if(!rewind::codec::applyDelta(nueva, d, back)) {
    std::printf("FAIL %s: applyDelta rechazo la diferencia\n", what);
    fails++;
    return;
  }
  if(back != vieja) {
    std::printf("FAIL %s: la reconstruccion no es la foto vieja (%zu vs %zu bytes)\n",
                what, back.size(), vieja.size());
    fails++;
    return;
  }
  std::printf("ok   %s  (nueva %zu, vieja %zu, delta %zu)\n", what, nueva.size(), vieja.size(), d.size());
}

auto seq(usize n, u8 base) -> std::vector<u8> {
  std::vector<u8> v(n);
  for(usize i = 0; i < n; i++) v[i] = (u8)(base + i * 7);
  return v;
}

}  // namespace

auto main() -> int {
  // Iguales: la diferencia tiene que ser diminuta y la vuelta exacta.
  {
    auto a = seq(4096, 1);
    check("identicas", a, a);
  }
  // Una palabra distinta en medio.
  {
    auto a = seq(4096, 1); auto b = a;
    b[2048] ^= 0xff;
    check("una palabra distinta", a, b);
  }
  // Cambio NO alineado a palabra (un solo byte a caballo).
  {
    auto a = seq(4096, 1); auto b = a;
    b[1023] ^= 0x01;
    check("byte suelto no alineado", a, b);
  }
  // Todo distinto.
  {
    auto a = seq(1024, 1); auto b = seq(1024, 200);
    check("todo distinto", a, b);
  }
  // Tamanos distintos en los dos sentidos, y no multiplos de 4.
  {
    auto a = seq(1000, 1); auto b = seq(1731, 1);
    check("vieja mas larga", a, b);
    check("vieja mas corta", b, a);
  }
  // Longitudes que no son multiplo de cuatro con cambio en la cola suelta.
  {
    auto a = seq(4099, 1); auto b = a;
    b[4098] ^= 0x80;
    check("cola no multiplo de 4", a, b);
  }
  // Vacias.
  {
    std::vector<u8> e, a = seq(64, 3);
    check("vieja vacia", a, e);
    check("nueva vacia", e, a);
    check("las dos vacias", e, e);
  }

  // Mil pares al azar con cambios dispersos: la forma que tienen dos campos seguidos.
  {
    std::mt19937 rng(12345);
    u64 totalDelta = 0, totalOld = 0;
    for(int it = 0; it < 1000; it++) {
      usize n = 1 + (rng() % 8192);
      std::vector<u8> a(n);
      for(usize i = 0; i < n; i++) a[i] = (u8)rng();
      std::vector<u8> b = a;
      // Entre 0 y 20 parches de hasta 64 bytes.
      int patches = (int)(rng() % 21);
      for(int p = 0; p < patches; p++) {
        usize off = rng() % n;
        usize len = 1 + (rng() % 64);
        if(off + len > n) len = n - off;
        for(usize i = 0; i < len; i++) b[off + i] = (u8)rng();
      }
      // A veces, ademas, cambia de tamano.
      if(rng() % 4 == 0) b.resize(1 + (rng() % 8192), (u8)rng());
      std::vector<u8> d, back;
      rewind::codec::makeDelta(a, b, d);
      if(!rewind::codec::applyDelta(a, d, back) || back != b) {
        std::printf("FAIL azar it=%d (n=%zu, parches=%d)\n", it, n, patches);
        fails++;
        break;
      }
      totalDelta += d.size();
      totalOld += b.size();
    }
    if(totalOld)
      std::printf("ok   1000 pares al azar  (delta = %.1f%% de la foto)\n",
                  100.0 * (double)totalDelta / (double)totalOld);
  }

  // Diferencia en caliente (DeltaSink): la nueva entra a trozos de tamano al azar, como la
  // recorre el visitante del estado. Al acabar la base tiene que SER la nueva y la diferencia
  // aplicada sobre ella devolver la vieja. Mismo tamano siempre: es lo que exige el sink.
  {
    std::mt19937 rng(777);
    rewind::codec::DeltaSink sink;
    u64 totalDelta = 0, totalOld = 0;
    int bad = 0;
    for(int it = 0; it < 2000 && !bad; it++) {
      usize n = rng() % 8192;
      std::vector<u8> vieja(n);
      for(usize i = 0; i < n; i++) vieja[i] = (u8)rng();
      std::vector<u8> nueva = vieja;
      int patches = n ? (int)(rng() % 21) : 0;
      for(int p = 0; p < patches; p++) {
        usize off = rng() % n;
        usize len = 1 + (rng() % 64);
        if(off + len > n) len = n - off;
        // A veces el parche deja bytes iguales dentro (huecos cortos que el sink funde).
        for(usize i = 0; i < len; i++) if(rng() % 3) nueva[off + i] = (u8)rng();
      }
      if(it % 50 == 0) for(auto& x : nueva) x = (u8)rng();   // todo distinto
      std::vector<u8> base = vieja, d, back;
      sink.begin(base, d);
      usize i = 0;
      while(i < n) {
        usize k = 1 + (rng() % 300);
        if(i + k > n) k = n - i;
        sink.feed(nueva.data() + i, k);
        i += k;
      }
      if(!sink.finish()) { std::printf("FAIL sink it=%d finish\n", it); bad++; break; }
      if(base != nueva) { std::printf("FAIL sink it=%d la base no es la nueva\n", it); bad++; break; }
      if(!rewind::codec::applyDelta(base, d, back) || back != vieja) {
        std::printf("FAIL sink it=%d (n=%zu) la vuelta no es la vieja\n", it, n); bad++; break;
      }
      totalDelta += d.size();
      totalOld += n;
    }
    fails += bad;
    if(!bad) std::printf("ok   2000 pares en caliente  (delta = %.1f%% de la foto)\n",
                         totalOld ? 100.0 * (double)totalDelta / (double)totalOld : 0.0);
    // De mas y de menos: se niega sin salirse de la base.
    std::vector<u8> base = seq(64, 1), d, big = seq(65, 2);
    sink.begin(base, d); sink.feed(big.data(), big.size());
    if(sink.finish()) { std::printf("FAIL sink acepto un estado mas largo\n"); fails++; }
    else std::printf("ok   sink rechaza estado mas largo\n");
    sink.begin(base, d); sink.feed(big.data(), 10);
    if(sink.finish()) { std::printf("FAIL sink acepto un estado mas corto\n"); fails++; }
    else std::printf("ok   sink rechaza estado mas corto\n");
  }

  // Diferencia corrupta: tiene que NEGARSE, no reventar ni devolver basura.
  {
    auto a = seq(256, 1); auto b = seq(256, 9);
    std::vector<u8> d, back;
    rewind::codec::makeDelta(a, b, d);
    d.resize(d.size() / 2);                    // truncada
    if(rewind::codec::applyDelta(a, d, back)) { std::printf("FAIL diferencia truncada aceptada\n"); fails++; }
    else std::printf("ok   diferencia truncada rechazada\n");
    std::vector<u8> corta = { 1, 2, 3 };
    if(rewind::codec::applyDelta(a, corta, back)) { std::printf("FAIL cabecera corta aceptada\n"); fails++; }
    else std::printf("ok   cabecera corta rechazada\n");
  }

  std::printf(fails ? "FAILED (%d)\n" : "ALL PASS\n", fails);
  return fails ? 1 : 0;
}
