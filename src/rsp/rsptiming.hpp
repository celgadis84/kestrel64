#pragma once
// Modelo de ciclos del pipeline del RSP: SOLO CONTABILIDAD (KESTREL_RSP_TIMING=1).
//
// El interprete del RSP retira una instruccion por ciclo, sin paradas ni emision doble; asi
// reordenar microcodigo no cambia nada medido. Este modelo cuenta, al lado, los ciclos que
// costaria la MISMA secuencia ejecutada en un RSP real, sin tocar ni el estado ni el reloj de
// invitado (la tarea sigue durando lo que duraba: el fin de tarea, MI_SP y el statehash no se
// mueven). Fuentes:
//   - n64brew, Reality_Signal_Processor/CPU_Pipeline;
//   - RSPL evalCost (Max Bebok, Apache-2.0, src/lib/optimizer/eval/evalCost.js), del que el
//     modelo estatico tools/rsp_cycles.py de kestrel64-sdk es un port. Las reglas son las de
//     ese port, una a una, para que un tramo recto de cifras iguales en los dos:
//   * emision doble: una escalar (incluye LWC2/SWC2, MTC2/MFC2/CFC2/CTC2) + una vectorial
//     computacional en el mismo ciclo si ninguna es salto, la primera no es ranura de retardo,
//     no hay RAW ni WAW entre ellas y la segunda no es CFC2/CTC2 con la primera escribiendo;
//   * latencia de resultado: op vectorial, LWC2 y MTC2 = 4; loads escalares, MFC0, MFC2,
//     CFC2 = 3; un consumidor anterior para hasta que este listo;
//   * store emitido justo 2 ciclos tras un load (o acceso COP0/COP2-move): +1;
//   * salto TOMADO: +1 burbuja en su ranura de retardo (aqui se sabe si se tomo; el modelo
//     estatico los cuenta todos como tomados);
//   * VRCP*/VRSQ*/VMOV solo leen vt (vs es elemento); VSAR no lee vectores (acumulador).
// Diferencia con el estatico: la pareja se decide con la instruccion que DE VERDAD se ejecuta
// despues (en un salto tomado, la del destino), con un pendiente de una instruccion.
//
// DMA del SP (ESTIMACION, no hay oraculo publico de la latencia): un DMA lanzado por MTC0 a
// SP_RD_LEN/SP_WR_LEN termina en
//     fin = max(ahora, fin del DMA anterior) + kDmaSetup + filas * ceil(bytes_fila / 8)
// (8 B por ciclo de RSP = 500 MB/s, el pico de la RDRAM; kDmaSetup = 10 ciclos por DMA).
// El emulador completa el DMA en el acto, asi que el bucle de espera del microcodigo sale a la
// primera; el modelo cobra la espera que habria tenido: MFC0 de SP_DMA_BUSY para hasta que
// acaben todos, MFC0 de SP_DMA_FULL hasta que quede como mucho uno en vuelo.
#include "../core/types.hpp"
#include <algorithm>
#include <cstring>

namespace kestrel {

struct RspTiming {
  static constexpr u64 kDmaSetup = 10;

  struct Op {
    u8   src[2] = {}, nsrc = 0, dst = 0, lat = 0;   // dst 0 = ninguno (r0 nunca cuenta)
    bool vec = false, br = false, mload = false, mstore = false, ctl = false, taken = false;
    u8   dmaWait = 0;                                // 1 = DMA_BUSY, 2 = DMA_FULL
    u32  dmaLen = 0; bool dmaStart = false;          // valor de SP_RD/WR_LEN
    u16  slot = 0;
  };

  // --- estado del pipeline ---
  u64  cycle = 0;              // reloj del modelo, acumulado desde el arranque / reset
  u64  readyAt[64] = {};       // 0..31 escalares, 32..63 vectoriales
  u32  loadMask = 0;
  u32  branchStep = 0;
  bool branchTaken = false;    // el salto cuya ranura viene ahora se tomo
  bool havePend = false;
  Op   pend;
  u64  dmaDone[2] = {};        // fin de los dos DMA en vuelo como mucho (cola del SP)

  // --- contadores globales (prof.reset los pone a 0) ---
  u64 insns = 0, stalls = 0, pairs = 0, bubbles = 0, dmaStall = 0, dmas = 0;
  u64 cyc0 = 0;                // `cycle` en el ultimo reset: ciclos de la ventana = cycle - cyc0
  // --- por ranura de IMEM ---
  u32 slotCyc[1024] = {}, slotStall[1024] = {}, slotPair[1024] = {};
  // --- por tarea ---
  u64 taskStart = 0, taskIns0 = 0, taskSt0 = 0, taskPr0 = 0, taskBu0 = 0, taskDm0 = 0;
  u64 tasks = 0;
  struct Last { u64 type = 0, insns = 0, cycles = 0, stalls = 0, pairs = 0, bubbles = 0, dmaStall = 0; } last;
  u64 typeN[8] = {}, typeCyc[8] = {}, typeIns[8] = {};

  auto clearStats() -> void {
    insns = stalls = pairs = bubbles = dmaStall = dmas = 0;
    cyc0 = cycle;
    std::memset(slotCyc, 0, sizeof slotCyc); std::memset(slotStall, 0, sizeof slotStall);
    std::memset(slotPair, 0, sizeof slotPair);
    tasks = 0; last = {};
    std::memset(typeN, 0, sizeof typeN); std::memset(typeCyc, 0, sizeof typeCyc);
    std::memset(typeIns, 0, sizeof typeIns);
    taskStart = cycle; taskIns0 = 0; taskSt0 = taskPr0 = taskBu0 = taskDm0 = 0;
  }

  static auto decode(u32 w, u32 pc) -> Op {
    Op o; o.slot = (u16)((pc >> 2) & 1023);
    auto S = [&](u32 r) { if(r) o.src[o.nsrc++] = (u8)r; };
    auto D = [&](u32 r) { o.dst = (u8)r; };
    const u32 op = w >> 26, rs = (w >> 21) & 31, rt = (w >> 16) & 31, rd = (w >> 11) & 31;
    if(w == 0) return o;
    switch(op) {
    case 0x00: {
      const u32 f = w & 63;
      if(f == 0 || f == 2 || f == 3)      { S(rt); D(rd); }
      else if(f == 4 || f == 6 || f == 7) { S(rt); S(rs); D(rd); }
      else if(f == 8)                     { S(rs); o.br = true; }
      else if(f == 9)                     { S(rs); D(rd); o.br = true; }
      else if(f == 13)                    { }
      else                                { S(rs); S(rt); D(rd); }
      break; }
    case 0x01: S(rs); o.br = true; if(rt & 0x10) D(31); break;
    case 0x02: o.br = true; break;
    case 0x03: o.br = true; D(31); break;
    case 0x04: case 0x05: S(rs); S(rt); o.br = true; break;
    case 0x06: case 0x07: S(rs); o.br = true; break;
    case 0x08: case 0x09: case 0x0a: case 0x0b: case 0x0c: case 0x0d: case 0x0e: S(rs); D(rt); break;
    case 0x0f: D(rt); break;
    case 0x20: case 0x21: case 0x23: case 0x24: case 0x25: S(rs); D(rt); o.lat = 3; o.mload = true; break;
    case 0x28: case 0x29: case 0x2b: S(rs); S(rt); o.mstore = true; break;
    case 0x10:   // COP0
      o.mload = o.mstore = true;
      if(rs == 0) { D(rt); o.lat = 3; if(rd == 6) o.dmaWait = 1; else if(rd == 5) o.dmaWait = 2; }
      else if(rs == 4) { S(rt); if(rd == 2 || rd == 3) o.dmaStart = true; }
      break;
    case 0x12:   // COP2
      if(w & (1u << 25)) {
        const u32 vt = (w >> 16) & 31, vs = (w >> 11) & 31, vd = (w >> 6) & 31, f = w & 63;
        o.vec = true; o.lat = 4; o.dst = (u8)(32 + vd);
        if(f >= 0x30 && f <= 0x37) o.src[o.nsrc++] = (u8)(32 + vt);
        else if(f != 0x1d) { o.src[o.nsrc++] = (u8)(32 + vs); o.src[o.nsrc++] = (u8)(32 + vt); }
      } else {
        o.mload = o.mstore = true;
        if(rs == 0)      { o.src[o.nsrc++] = (u8)(32 + rd); D(rt); o.lat = 3; }
        else if(rs == 2) { D(rt); o.lat = 3; o.ctl = true; }
        else if(rs == 4) { S(rt); o.dst = (u8)(32 + rd); o.lat = 4; }
        else if(rs == 6) { S(rt); o.lat = 3; o.ctl = true; }
      }
      break;
    case 0x32: S(rs); o.dst = (u8)(32 + rt); o.lat = 4; o.mload = true; break;
    case 0x3a: S(rs); o.src[o.nsrc++] = (u8)(32 + rt); o.mstore = true; break;
    default: break;
    }
    return o;
  }

  auto tick(u64 c) -> void { cycle += c; loadMask = c >= 32 ? 0 : loadMask >> c; }

  static auto reads(const Op& o, u8 r) -> bool {
    for(u32 i = 0; i < o.nsrc; i++) if(o.src[i] == r) return true;
    return false;
  }

  // Grupo de emision (1 o 2 ops): paradas, emision, y un ciclo de avance.
  auto commit(Op* g, u32 n) -> void {
    for(;;) {
      const u64 before = cycle;
      for(u32 k = 0; k < n; k++) {
        for(u32 i = 0; i < g[k].nsrc; i++) {
          const u64 ra = readyAt[g[k].src[i]];
          if(ra > cycle) { const u64 s = ra - cycle; slotStall[g[k].slot] += (u32)s; slotCyc[g[k].slot] += (u32)s; stalls += s; tick(s); }
        }
        if((loadMask & 1) && g[k].mstore) { slotStall[g[k].slot]++; slotCyc[g[k].slot]++; stalls++; tick(1); }
      }
      if(before == cycle) break;
    }
    for(u32 k = 0; k < n; k++) {
      Op& o = g[k];
      if(o.dmaWait) {   // espera de DMA del microcodigo (ver cabecera)
        const u64 until = o.dmaWait == 1 ? std::max(dmaDone[0], dmaDone[1]) : std::min(dmaDone[0], dmaDone[1]);
        if(until > cycle) { const u64 s = until - cycle; dmaStall += s; slotCyc[o.slot] += (u32)s; tick(s); }
      }
      if(o.mload) loadMask |= 4;
      branchStep >>= 1;
      if(!branchStep && o.br) { branchStep = 2; branchTaken = o.taken; }
      if(branchStep == 1 && branchTaken) { bubbles++; slotCyc[o.slot]++; tick(1); }
      if(o.dst) readyAt[o.dst] = cycle + o.lat;
      if(o.dmaStart) {
        const u32 len = o.dmaLen;
        const u64 rowBytes = ((len & 0xfff) | 7) + 1, rows = ((len >> 12) & 0xff) + 1;
        const u64 cost = kDmaSetup + rows * ((rowBytes + 7) / 8);
        const u64 prev = std::max(dmaDone[0], dmaDone[1]);
        const u64 done = std::max(cycle, prev) + cost;
        // cola de dos: el que acabo antes deja su hueco
        if(dmaDone[0] <= dmaDone[1]) dmaDone[0] = done; else dmaDone[1] = done;
        dmas++;
      }
      if(n == 2) slotPair[o.slot]++;
      insns++;
    }
    if(n == 2) pairs++;
    slotCyc[g[0].slot]++;
    tick(1);
  }

  // Antes de ejecutar `w` en `pc`. `rtVal` = valor de r[rt] (para MTC0 a SP_RD/WR_LEN).
  auto feed(u32 w, u32 pc, u32 rtVal) -> void {
    Op b = decode(w, pc);
    if(b.dmaStart) b.dmaLen = rtVal;
    if(!havePend) { pend = b; havePend = true; return; }
    const Op& a = pend;
    bool dual = a.vec != b.vec && branchStep != 2 && !a.br && !b.br;
    if(dual && a.dst) dual = !reads(b, a.dst) && a.dst != b.dst && !b.ctl;
    if(dual) { Op g[2] = { a, b }; havePend = false; commit(g, 2); return; }
    Op g = a; commit(&g, 1); pend = b;
  }
  // Tras ejecutar la op que acaba de entrar en feed(): si era salto, se sabe si se tomo.
  auto post(bool taken) -> void { if(havePend) pend.taken = taken; }
  auto flush() -> void { if(havePend) { Op g = pend; havePend = false; commit(&g, 1); } }

  auto taskBegin() -> void {
    flush();
    branchStep = 0; loadMask = 0;
    taskStart = cycle; taskIns0 = insns; taskSt0 = stalls; taskPr0 = pairs; taskBu0 = bubbles; taskDm0 = dmaStall;
  }
  auto taskEnd(u32 type) -> void {
    flush();
    last.type = type; last.cycles = cycle - taskStart; last.insns = insns - taskIns0;
    last.stalls = stalls - taskSt0; last.pairs = pairs - taskPr0; last.bubbles = bubbles - taskBu0;
    last.dmaStall = dmaStall - taskDm0;
    const u32 t = type < 8 ? type : 0;
    typeN[t]++; typeCyc[t] += last.cycles; typeIns[t] += last.insns;
    tasks++;
    taskStart = cycle; taskIns0 = insns; taskSt0 = stalls; taskPr0 = pairs; taskBu0 = bubbles; taskDm0 = dmaStall;
  }
};

}  // namespace kestrel
