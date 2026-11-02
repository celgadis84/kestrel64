// PIF: boot handshake, CIC challenge, cartridge RTC and joybus.
// Split out of memory.cpp; shared internals live in memory_internal.hpp.
#include "memory.hpp"
#include "memory_internal.hpp"
#include "movie.hpp"
#include "../net/netplay.hpp"
#include "runtime.hpp"
#include <ctime>
#ifdef _WIN32
#include <windows.h>
#endif

namespace kestrel {

// --- Modo de arranque del PIF -------------------------------------------------------
// Ver la cabecera en memory.hpp. Solo se atienden aqui los bits del MODO RESET; los del modo
// normal (joybus, desafio del CIC) siguen en siDma porque dependen del sentido del DMA.
auto Memory::pifBootHle(u64 cicChecksum, u64 ipl2Sum) -> void {
  // Lo que ya hizo el IPL2 antes de saltar al IPL3: bloquear la ROM (0x10), entregar el
  // checksum que calculo sobre el IPL3 del cartucho (0x20, `ipl2Sum`, ver core/cic.hpp) y
  // pedir al PIF que lo compare con el del CIC (0x40). Si no casa, la consola se queda ahi.
  // Sigue en modo reset: falta el 0x08 del cartucho. La palabra 0x24 (seeds + tipo de
  // arranque) no se deja visible: el PIF la vuelve a ocultar con el 0x20, y el IPL2 ya paso
  // lo que lleva por registros (s3/s5/s6/s7, CPU::fastBoot).
  cicSum = cicChecksum;
  pifRomLocked = true;
  for(int i = 0; i < 6; i++) pifSum[i] = (u8)(ipl2Sum >> (8 * (5 - i)));
  pifSumGot = true;
  if(cicSum && ipl2Sum != cicSum) pifFreeze("checksum IPL2 distinto del del CIC (orden 0x40)");
}

auto Memory::pifFreeze(const char* why) -> void {
  if(pifFrozen) return;
  pifFrozen = true; pifFreezeWhy = why;
  std::fprintf(stderr, "[pif] consola CONGELADA por el PIF: %s\n", why);
  std::fflush(stderr);
}

auto Memory::pifCommand() -> void {
  u8& c = pifram[63];
  if(!pifBootMode) return;                 // modo normal: 0x04/0x08 son banderas del joybus
  if(c & 0x10) { pifRomLocked = true; c = (u8)(c & ~0x10u); }
  if(c & 0x20) {
    // Recoger checksum: 6 bytes en 0x32..0x37, el PIF se los queda, los borra de la PIF RAM
    // y avisa con 0x80 en el byte de control.
    for(int i = 0; i < 6; i++) { pifSum[i] = pifram[0x32 + i]; pifram[0x32 + i] = 0; }
    pifSumGot = true;
    c = (u8)((c & ~0x20u) | 0x80u);
  }
  if(c & 0x40) {
    c = (u8)(c & ~0x40u);
    u64 got = 0;
    for(int i = 0; i < 6; i++) got = (got << 8) | pifSum[i];
    if(!cicSum) {
      // IPL3 desconocido (homebrew): no sabemos que chip lleva el cartucho, asi que no hay
      // checksum del CIC con que comparar. Inventarse un veredicto seria peor que no darlo.
      static bool warned = false;
      if(!warned) { warned = true;
        std::fprintf(stderr, "[pif] verificacion de checksum con CIC desconocido: se da por buena\n"); }
    } else if(!pifSumGot || got != cicSum) {
      pifFreeze("checksum IPL2 distinto del del CIC (orden 0x40)");
    }
  }
  if(c & 0x08) {
    // Fin del arranque: el PIF pasa a modo normal y el plazo de 5 s deja de correr. El bit
    // se consume (en modo normal el mismo bit es una bandera del joybus).
    pifBootMode = false;
    c = (u8)(c & ~0x08u);
  }
}

// --- Desafio anti-pirateria del CIC-NUS-6105 ---------------------------------
// Los cartuchos firmados con 6105/7105 (Perfect Dark, Donkey Kong 64, Majora's Mask,
// Banjo-Tooie...) pueden preguntarle al chip CIC algo que solo el chip sabe contestar. El
// juego deja 15 bytes en la PIF RAM (0x30..0x3E), pone el bit 1 del byte de control y lee el
// bloque: el PIF le pasa al CIC los 30 NIBBLES en orden, el CIC devuelve otros 30, y el juego
// compara con lo que esperaba. Un cartucho copiado sin CIC autentico no puede contestar.
//
// El algoritmo es una maquina de estados sobre nibbles con una tabla de 32 entradas indexada
// por (seleccion << 4 | dato); esta publicado en n64brew (paginas PIF-NUS y CIC-NUS) desde que
// se volco la ROM del 6105. No hay nada especifico de un juego aqui: es la funcion del chip.
//
// Ningun otro CIC implementa esta orden, y ningun cartucho que no sea 6105 la pide. Si llegara
// igualmente, lo unico honesto es no inventarse una respuesta: se deja el bloque como estaba
// (que es lo que el juego leeria de una linea que nadie contesta) y se avisa una vez.
auto Memory::pifCicChallenge() -> void {
  if(std::getenv("KESTREL_SILOG"))
    std::fprintf(stderr, "[silog] desafio del CIC (6105=%d) pc=0x%08x\n", (int)cic6105, (u32)storePc);
  if(!cic6105) {
    static bool warned = false;
    if(!warned) { warned = true;
      std::fprintf(stderr, "[pif] desafio del CIC pedido por un cartucho que no es 6105: sin respuesta\n"); }
    return;
  }
  static const u8 kLut[32] = {
    0x4, 0x7, 0xa, 0x7, 0xe, 0x5, 0xe, 0x1,
    0xc, 0xf, 0x8, 0xf, 0x6, 0x3, 0x6, 0x9,
    0x4, 0x1, 0xa, 0x7, 0xe, 0x5, 0xe, 0x1,
    0xc, 0x9, 0x8, 0x5, 0x6, 0x3, 0xc, 0x9,
  };
  u8 nib[30];
  for(int i = 0; i < 15; i++) { nib[i * 2] = (u8)(pifram[0x30 + i] >> 4); nib[i * 2 + 1] = (u8)(pifram[0x30 + i] & 0xf); }
  u8 key = 0xb, sel = 0;
  for(int i = 0; i < 30; i++) {
    u8 data = (u8)((key + 5 * nib[i]) & 0xf);
    nib[i] = data;
    key = kLut[(sel << 4) | data];
    u8 mod = (u8)(data >> 3);            // bit alto del nibble
    u8 mag = (u8)(data & 7);             // los tres bajos
    if(mod) mag = (u8)(~mag & 7);
    if(mag % 3 != 1) mod = (u8)!mod;
    if(sel) {                            // dos datos fuerzan el signo cuando venimos de 1
      if(data == 0x1 || data == 0x9) mod = 1;
      if(data == 0xb || data == 0xe) mod = 0;
    }
    sel = mod;
  }
  for(int i = 0; i < 15; i++) pifram[0x30 + i] = (u8)((nib[i * 2] << 4) | nib[i * 2 + 1]);
}

// --- RTC de cartucho (ver Memory::Rtc) ---------------------------------------
static auto bcd(u32 v) -> u8 { return (u8)(((v / 10) << 4) | (v % 10)); }
static auto unbcd(u8 v) -> int { return (v >> 4) * 10 + (v & 0xf); }
static auto rtcLocal(long long t) -> std::tm {
  std::time_t tt = (std::time_t)t; std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &tt);
#else
  localtime_r(&tt, &tm);
#endif
  return tm;
}
// Estado: bit 7 = parado (cristal y pila, bits 1 y 0, siempre bien).
auto Memory::rtcStatus() const -> u8 { return rtc.stopped() ? 0x80 : 0x00; }
auto Memory::rtcNow() const -> long long {
  return rtc.stopped() ? rtc.frozen : (long long)std::time(nullptr) + rtc.offset;
}
auto Memory::rtcRead(u8 blk, u8* d) -> void {
  if(blk == 0) std::memcpy(d, rtc.ctrl, 8);
  else if(blk == 1) std::memcpy(d, rtc.nv, 8);
  else if(blk == 2) {
    const std::tm tm = rtcLocal(rtcNow());
    d[0] = bcd(tm.tm_sec % 60); d[1] = bcd(tm.tm_min); d[2] = (u8)(bcd(tm.tm_hour) | 0x80);
    d[3] = bcd(tm.tm_mday); d[4] = bcd(tm.tm_wday); d[5] = bcd(tm.tm_mon + 1);
    d[6] = bcd(tm.tm_year % 100); d[7] = bcd(tm.tm_year / 100);   // siglo contado desde 1900
  }
}
auto Memory::rtcWrite(u8 blk, const u8* d) -> void {
  if(blk == 0) {
    const bool was = rtc.stopped();
    const long long now = rtcNow();
    std::memcpy(rtc.ctrl, d, 8);
    if(!was && rtc.stopped()) rtc.frozen = now;                       // se para: congela
    else if(was && !rtc.stopped()) rtc.offset = rtc.frozen - (long long)std::time(nullptr);
  } else if(blk == 1) {
    if(!(rtc.ctrl[0] & 1)) std::memcpy(rtc.nv, d, 8);
  } else if(blk == 2) {
    // La hora solo se pone con el reloj parado y sin proteccion (n64brew): asi la fija el SDK.
    if((rtc.ctrl[0] & 2) || !rtc.stopped()) return;
    std::tm tm{};
    tm.tm_sec = unbcd(d[0] & 0x7f); tm.tm_min = unbcd(d[1] & 0x7f); tm.tm_hour = unbcd(d[2] & 0x3f);
    tm.tm_mday = unbcd(d[3] & 0x3f); tm.tm_mon = unbcd(d[5] & 0x1f) - 1;
    tm.tm_year = unbcd(d[6]) + 100 * unbcd(d[7]); tm.tm_isdst = -1;
    const std::time_t t = std::mktime(&tm);
    if(t != (std::time_t)-1) rtc.frozen = (long long)t;
  }
}

// Run the 64-byte PIF RAM joybus command block and fill in device responses,
// matching what libultra's __osContGetInitData / __osEepStatus expect to read
// back. Byte scan: 0xFE ends the block, 0x00 skips to the next channel, 0xFF is
// padding. Any other value is a TX byte count that opens a command whose result
// goes into the RX area; the RX-size byte carries the channel error flags.
//
// Dispositivos emulados: canales 0-3 = los cuatro conectores de mando (cada uno con su
// accesorio: nada, Controller Pak o Rumble Pak), canal 4 = EEPROM. Un conector sin mando y
// los canales 5+ contestan "no device", que es lo que hace la consola con el hueco vacio.
auto Memory::pifProcessJoybus() -> u32 {
  static int silog = std::getenv("KESTREL_SILOG") ? 1 : 0;
  const u8 NO_DEVICE = 0x80;   // CONT_NO_RESPONSE: (rxsize & 0xC0) >> 4 = 0x8
  // Coste de LINEA del joybus, que es lo unico que hace lenta la transaccion: la consola y
  // el mando hablan por un solo hilo a 4 us por bit (bit de parada de la consola 3 us, el
  // del mando 4 us). Se factura por orden, contando los bytes que de verdad viajan: los TX
  // siempre, los RX solo si el dispositivo contesta. De ahi salen ~167 us por una lectura
  // de botones (1 byte de orden + 4 de respuesta) y ~670 us por los cuatro conectores, que
  // es la cifra que se mide en hardware.
  u32 us = 0;
  auto billTx = [&](u32 n)  { us += n * 8 * 4 + 3; };   // bytes hacia el dispositivo + parada consola
  auto billRx = [&](u32 n)  { us += n * 8 * 4 + 4; };   // bytes de vuelta + parada del mando
  int channel = 0;
  for(int i = 0; i < 63; ) {
    u8 t = pifram[i];
    if(t == 0xFE) break;                              // end of commands
    if(t == 0x00) { channel++; i++; continue; }       // skip this channel
    if(t == 0xFF) { i++; continue; }                  // padding / NOP
    u8 tx = t & 0x3f;
    u8 rx = pifram[i + 1] & 0x3f;
    int txStart = i + 2;
    int rxStart = i + 2 + tx;
    if(rxStart + rx > 64 || txStart >= 64) break;     // malformed — bail out
    u8  cmd  = pifram[txStart];
    u8* rxp  = &pifram[rxStart];
    u8& rxsz = pifram[i + 1];                          // holds the error flags on return

    billTx(tx);
    auto absent = [&] { rxsz |= NO_DEVICE;
      if(silog) std::fprintf(stderr, "[silog] ABSENT ch=%d cmd=0x%02x tx=%u rx=%u pc=0x%08x\n",
                             channel, cmd, tx, rx, (u32)storePc); };
    if(silog) std::fprintf(stderr, "[silog] cmd ch=%d cmd=0x%02x tx=%u rx=%u pc=0x%08x\n",
                           channel, cmd, tx, rx, (u32)storePc);

    if(channel < 4) {                                 // uno de los cuatro conectores
      PadPort& pp = padPort[channel];
      if(!pp.connected) { absent(); channel++; i = rxStart + rx; continue; }
      switch(cmd) {
      case 0x00:                                      // request status / info
      case 0xFF:                                      // reset (same reply)
        // Byte de estado: CONT_CARD_ON (0x01) si hay accesorio en la ranura. CONT_CARD_PULL
        // (0x02) se deja a cero a proposito -- ambos puestos significan "pak recien
        // cambiado" y el SDK devuelve PFS_ERR_NEW_PACK, que el juego ensena como error.
        if(rx >= 3 && pp.mouse) { rxp[0] = 0x02; rxp[1] = 0x00; rxp[2] = 0x00; }   // raton 0x0200
        else if(rx >= 3) { rxp[0] = 0x05; rxp[1] = 0x00;   // tipo 0x0005 (mando estandar)
                           rxp[2] = pp.accessory ? 0x01 : 0x00; }
        break;
      case 0x01: {                                    // read buttons
        // El mando inyectado por telemetria pisa al del anfitrion mientras le queden
        // sondeos, y el gasto se contabiliza AQUI (una lectura del joybus), que es lo unico
        // que el juego percibe como el paso del tiempo del mando.
        if(channel == 0) padPolls.fetch_add(1, std::memory_order_relaxed);
        u32 btn = pp.buttons; s8 sx = pp.stickX, sy = pp.stickY;
        s32 left = padRemotePolls[channel].load(std::memory_order_acquire);
        if(left != 0) {
          btn = padRemoteButtons[channel].load(std::memory_order_relaxed);
          s32 st = padRemoteStick[channel].load(std::memory_order_relaxed);
          sx = (s8)(st & 0xff); sy = (s8)((st >> 8) & 0xff);
          if(left > 0) padRemotePolls[channel].store(left - 1, std::memory_order_release);
        }
        // Juego en red: mandan las entradas repartidas para este campo, no las locales.
        if(netplay::lockstep()) netplay::pad(channel, btn, sx, sy);
        // Grabacion/reproduccion de entradas: esta es la frontera que el juego percibe, con
        // el mando del anfitrion y el inyectado por telemetria ya resueltos.
        movie::sample(channel, btn, sx, sy);
        for(int k = 0; k < rx; k++) rxp[k] = 0x00;
        if(rx >= 2) { rxp[0] = (btn >> 8) & 0xff; rxp[1] = btn & 0xff; }
        if(rx >= 4) { rxp[2] = (u8)sx; rxp[3] = (u8)sy; }  // analog stick
        // Raton: mismos 4 bytes, botones solo A (izquierdo) y B (derecho) y el desplazamiento
        // desde la ultima lectura donde el mando lleva la posicion del stick.
        if(pp.mouse && rx >= 2) { rxp[0] &= 0xc0; rxp[1] = 0x00; }
        break;
      }
      case 0x02:                                      // leer 32 bytes del accesorio
      case 0x03: {                                    // escribir 32 bytes
        if(pp.mouse) { absent(); break; }             // el raton no tiene ranura
        // La direccion viaja como (bloque << 5) | CRC5(bloque): 11 bits de bloque de 32
        // bytes, o sea 64 KiB de espacio de direcciones para 32 KiB de Controller Pak. La
        // mitad alta la usa el Rumble Pak: 0x8000 es su firma y 0xC000 el motor.
        const bool wr = cmd == 0x03;
        u16 wire = tx >= 3 ? (u16)((pifram[txStart + 1] << 8) | pifram[txStart + 2]) : 0;
        u16 block = (u16)(wire >> 5);
        u32 off = (u32)block * 32;
        u8 data[32];
        if(wr) for(int k = 0; k < 32; k++) data[k] = (3 + k) < tx ? pifram[txStart + 3 + k] : 0;

        if(pp.accessory == 3) {                       // Transfer Pak (ver PadPort)
          tpakSync(channel);
          PadPort::TpakRegs& tp = pp.tp;
          bool gbWin = false;
          u8 fill = 0x00;
          if(off >= 0x8000 && off < 0x9000) {         // alimentacion del pak
            if(wr) { if(data[31] == 0x84) tp.power = true; else if(data[31] == 0xFE) tp.power = false; }
            else fill = tp.power ? 0x84 : 0x00;
          } else if(!tp.power) {
            // Apagado no contesta: ceros, y lo escrito se pierde.
          } else if(off >= 0xA000 && off < 0xB000) {  // banco del bus GB
            if(wr) tp.bank = data[31];
          } else if(off >= 0xB000 && off < 0xC000) {  // cartucho GB: alimentacion / estado
            if(wr) { tp.cartOn = data[31] & 1; tp.rst = 0x04; }
            else if(pp.gb.loaded()) {
              // GBCART_ON, y con el cartucho alimentado POWER + RSTB_STATUS (la linea de
              // reset del GB ya soltada). RSTB_DETECTION (en el primer byte) y GBCART_PULL
              // se entregan una sola vez.
              fill = (u8)(0x80 | (tp.cartOn ? 0x09 : 0x00) | (tp.pull ? 0x40 : 0x00));
              tp.pull = false;
            } else fill = 0x40;                       // sin cartucho: GBCART_ON a cero
          } else if(off >= 0xC000) {                  // ventana de 16 KiB sobre el bus GB
            gbWin = true;
            u16 ga = (u16)((tp.bank & 3) * 0x4000u + (off - 0xC000u));
            if(wr) { for(int k = 0; k < 32; k++) pp.gb.write((u16)(ga + k), data[k]); }
            else   { for(int k = 0; k < 32; k++) data[k] = pp.gb.read((u16)(ga + k)); }
          }
          if(!wr) {
            if(!gbWin) {
              for(int k = 0; k < 32; k++) data[k] = fill;
              if(tp.power && off >= 0xB000 && off < 0xC000 && pp.gb.loaded()) { data[0] |= tp.rst; tp.rst = 0; }
            }
            for(int k = 0; k < rx && k < 32; k++) rxp[k] = data[k];
          }
        } else if(pp.accessory == 1) {                       // Controller Pak: 32 KiB de RAM
          if(wr) {
            if(off + 32 <= pp.mempak.size()) {
              std::memcpy(&pp.mempak[off], data, 32);
              pp.mempakDirty = true;
            }
          } else {
            for(int k = 0; k < 32; k++)
              data[k] = off + k < pp.mempak.size() ? pp.mempak[off + k] : 0x00;
            for(int k = 0; k < rx && k < 32; k++) rxp[k] = data[k];
          }
        } else if(pp.accessory == 2) {                // Rumble Pak: sin RAM, solo motor
          // El SDK lo reconoce escribiendo 0x80 en toda la ventana 0x8000 y releyendola:
          // si devuelve 0x80 hay Rumble. Fuera de esa ventana el pak no contesta nada
          // (ceros), y una escritura en 0xC000 enciende (dato != 0) o apaga el motor.
          if(wr) {
            if(off >= 0xC000 && off < 0xD000) pp.rumble = data[0] != 0;
          } else {
            u8 fill = (off >= 0x8000 && off < 0x9000) ? 0x80 : 0x00;
            for(int k = 0; k < 32; k++) data[k] = fill;
            for(int k = 0; k < rx && k < 32; k++) rxp[k] = data[k];
          }
        } else {                                      // ranura vacia
          if(!wr) { for(int k = 0; k < 32; k++) data[k] = 0x00;
                    for(int k = 0; k < rx && k < 32; k++) rxp[k] = 0x00; }
        }

        // El CRC de datos cierra la respuesta en los dos sentidos: en la lectura cubre lo
        // devuelto, en la escritura lo que el juego mando (asi comprueba que llego bien).
        // Con la ranura VACIA el mando devuelve el CRC INVERTIDO, que es justo como el SDK
        // detecta la ausencia antes de ir a preguntar el estado.
        u8 crc = mempakDataCrc(data);
        if(!pp.accessory) crc = (u8)~crc;
        int crcAt = wr ? 0 : 32;
        if(rx > crcAt) rxp[crcAt] = crc;
        if(silog && mempakAddrCrc(block) != (wire & 0x1f))
          std::fprintf(stderr, "[silog] pak addr CRC malo ch=%d wire=0x%04x\n", channel, wire);
        break;
      }
      default: absent(); break;
      }
    } else if(channel == 4) {                          // EEPROM (16 kbit)
      switch(cmd) {
      case 0x00:                                      // status: type byte 0x80 (4k) / 0xC0 (16k)
        if(rx >= 3) { rxp[0] = 0x00; rxp[1] = eepromTypeByte(); rxp[2] = 0x00; }
        break;
      case 0x04: {                                    // read 8-byte block; tx = [cmd, addr]
        u8 addr = tx >= 2 ? pifram[txStart + 1] : 0;
        for(int k = 0; k < rx && k < 8; k++) {
          u32 off = (u32)addr * 8 + k;
          rxp[k] = off < eeprom.size() ? eeprom[off] : 0;
        }
        break;
      }
      case 0x05: {                                    // write 8-byte block; tx = [cmd, addr, 8 data]
        u8 addr = tx >= 2 ? pifram[txStart + 1] : 0;
        for(int k = 0; k < 8 && (2 + k) < tx; k++) {
          u32 off = (u32)addr * 8 + k;
          if(off < eeprom.size()) { eeprom[off] = pifram[txStart + 2 + k]; saveDirty = true; }
        }
        if(rx >= 1) rxp[0] = 0x00;                    // status OK
        break;
      }
      case 0x06:                                      // RTC: info -> 00 10 + estado
        if(!rtc.on) { absent(); break; }
        if(rx >= 3) { rxp[0] = 0x00; rxp[1] = 0x10; rxp[2] = rtcStatus(); }
        break;
      case 0x07: {                                    // RTC: leer bloque -> 8 bytes + estado
        if(!rtc.on) { absent(); break; }
        u8 blk = tx >= 2 ? pifram[txStart + 1] & 3 : 0, d[8] = {};
        rtcRead(blk, d);
        for(int k = 0; k < rx && k < 8; k++) rxp[k] = d[k];
        if(rx >= 9) rxp[8] = rtcStatus();
        break;
      }
      case 0x08: {                                    // RTC: escribir bloque -> estado
        if(!rtc.on) { absent(); break; }
        u8 blk = tx >= 2 ? pifram[txStart + 1] & 3 : 0, d[8] = {};
        for(int k = 0; k < 8 && (2 + k) < tx; k++) d[k] = pifram[txStart + 2 + k];
        rtcWrite(blk, d);
        if(rx >= 1) rxp[0] = rtcStatus();
        break;
      }
      default: absent(); break;
      }
    } else {
      absent();                                        // canal 5+: no existe
    }
    // Un conector vacio no contesta: la consola espera el tiempo de espera del bus y sigue.
    // Se factura como si la respuesta no hubiera existido, que es lo que ocurre en la linea.
    if(!(rxsz & NO_DEVICE)) billRx(rx);
    channel++;
    i = rxStart + rx;
  }
  return us;
}

}  // namespace kestrel
