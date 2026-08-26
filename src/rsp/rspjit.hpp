#pragma once
// kestrel64 — dynarec del RSP.
//
// Por que aqui y no una micro-optimizacion mas del interprete: el perfilador de host sobre
// el hilo del RSP (KESTREL_HOSTPROF_WHO=rsp, SM64) reparte asi el tiempo:
//
//   Rsp::step 59.9% · Rsp::execCop2 31.6% · execStore 4.8% · execLoad 3.7%
//
// y el reparto de instrucciones de microcodigo (KESTREL_VUSTAT) dice que COP2 es el 41.6%
// de las instrucciones. O sea: la parte vectorial, que es la que hace el trabajo de verdad,
// rinde POR ENCIMA de su peso (41.6% de las instrucciones, 31.6% del tiempo), y lo que
// sobra esta en el bucle de despacho -- leer la palabra de IMEM, decodificar el mayor,
// mantener el pestillo de delay-slot, contar. Exactamente el problema que en la CPU
// resolvio el dynarec. Ver docs/PERF-CPU.md §12-ter.
//
// El RSP es un blanco mucho mas facil que la CPU: 4 KB de IMEM (1024 palabras), sin TLB,
// sin excepciones, sin HI/LO, PC de 12 bits. Eso permite tres simplificaciones que el
// dynarec de CPU no puede permitirse:
//
//   * el cache de bloques es una TABLA DIRECTA de 1024 entradas indexada por pc>>2 -- no
//     hay hash, ni colisiones, ni cache negativa que envejecer;
//   * la invalidacion es global y O(1): cualquier escritura en IMEM tira la tabla entera.
//     IMEM solo cambia por DMA del motor SP (carga de microcodigo o de un overlay), que es
//     un evento raro comparado con las decenas de miles de instrucciones de una tarea;
//   * un bloque nunca puede fallar a medias. No hay excepciones que vectorizar, asi que la
//     firma no necesita devolver "instrucciones retiradas antes del fallo".
//
// Etapa 1: bloques de linea recta. El bloque termina ANTES de la primera instruccion que
// pueda cambiar el flujo o parar el nucleo -- salto, BREAK, COP0 -- y esas las sigue
// ejecutando el interprete con su pestillo de delay-slot intacto. Dentro del bloque:
//
//   * ALU / desplazamientos / LUI / LB / LBU / SB se emiten NATIVOS en x86-64;
//   * COP2, LWC2, SWC2 y las cargas/tiendas escalares de 16/32 bits se emiten como una
//     llamada a los MISMOS helpers que usa el interprete (Rsp::execCop2/execLoad/execStore
//     /exec). La semantica no se duplica en ningun sitio: el JIT no puede divergir del
//     interprete porque ejecuta su codigo.
//
// Etapa 2: el bloque se lleva TAMBIEN el salto que lo cierra y su delay-slot (BEQ/BNE/
// BLEZ/BGTZ, los cuatro REGIMM, J/JAL, JR/JALR). El par salto+delay era el 2x mas caro del
// interprete -- dos vueltas del despachador mas el pestillo `inDelay` -- y ademas cortaba
// el bloque en cada bucle del microcodigo, que es justo donde se pasa el tiempo. El bloque
// escribe Rsp::pc y marca Block::setsPc; siguen fuera BREAK, COP0 y todo lo no reconocido.
//
// El oraculo es el propio interprete: mismo md5 de framebuffer con KESTREL_RSPJIT=0 y =1.
#include "../core/types.hpp"
#include "../cpu/jit.hpp"

namespace kestrel {

struct Rsp;

namespace rspjit {

// Un bloque compilado. No devuelve nada: no hay fallo parcial posible (ver cabecera), asi
// que el numero de instrucciones que consume es siempre `nOps` y lo sabe el llamante.
using BlockFn = void (*)(Rsp*);

enum class State : u8 { Unknown = 0, Compiled = 1, NoComp = 2 };

struct Block {
  BlockFn fn = nullptr;
  u16     nOps = 0;
  // El bloque termina en un salto con su delay-slot absorbido y ya ha dejado Rsp::pc
  // puesto: el llamante NO debe avanzarlo el mismo. Ver rspjit.cpp emitBranch().
  bool    setsPc = false;
};

struct Cache {
  jit::CodeBuffer buf;
  Block  blocks[1024];      // indexado por pc>>2: IMEM son 4 KB = 1024 palabras
  State  state[1024] = {};
  u64    imemFp = ~0ull;    // huella de IMEM con la que se compilo lo que hay en la tabla
  bool   ready = false;
  // estadistica (KESTREL_RSPJIT_STATS=1)
  u64 entries = 0, jitOps = 0, interpOps = 0, compiles = 0, flushes = 0;

  auto init() -> bool;
  auto clear() -> void;     // tira la tabla entera (IMEM cambio / buffer lleno)
};

// Longitud minima de bloque. Por debajo de esto el prologo+epilogo del bloque cuesta mas
// que interpretar las instrucciones, asi que no merece la pena compilar.
static constexpr u32 kMinOps = 2;
// Tope de instrucciones por bloque: una linea recta mas larga que esto no existe en
// microcodigo real, y acota el codigo emitido por entrada de la tabla.
static constexpr u32 kMaxOps = 64;

// Compila el bloque que empieza en `pc` (alineado a palabra) dentro de `c`. Siempre deja
// state[pc>>2] en Compiled o NoComp, de modo que el llamante nunca reintenta en bucle.
auto compile(Rsp& rsp, Cache& c, u32 pc) -> void;

// Huella de los 4 KB de IMEM. Se recalcula al arrancar cada tarea (Rsp::start) y si no
// coincide con la de la tabla se tira todo: cubre a CUALQUIER escritor de IMEM (DMA del
// SP, tienda de la CPU, escritura por MCP) sin poner un gancho en sus caminos calientes.
auto imemFingerprint(const u8* imem) -> u64;

}  // namespace rspjit
}  // namespace kestrel
