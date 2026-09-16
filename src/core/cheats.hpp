#pragma once
// kestrel64 -- trucos tipo GameShark / Action Replay.
//
// El aparato de verdad se metia entre el cartucho y la consola y sustituia el arranque por
// el suyo: enganchaba la interrupcion del VI y, en cada campo de video, un motor propio
// recorria la lista de codigos y escribia en la RDRAM. De ahi salen las dos cosas que hay
// que respetar para que los codigos publicados funcionen tal cual:
//
//   - El ritmo es EL CAMPO DE VIDEO, no el fotograma del juego ni una constante de tiempo.
//     Un juego a 20 fps recibe los parches tres veces por fotograma suyo, que es justo lo
//     que hace que "vidas infinitas" gane la carrera contra el codigo que las resta.
//   - El nibble alto de la direccion NO es decorativo: es el segmento MIPS con el que
//     escribia el motor. 0x80xxxxxx es KSEG0 (escritura CACHEADA, la ve la CPU al momento
//     aunque tarde en bajar a la RDRAM) y 0xA0xxxxxx es KSEG1 (escritura sin cache, que es
//     por lo que las familias A0/A1 se usan como parche unico de arranque). Por eso aqui
//     unas van por la D-cache de la CPU y otras a la RDRAM a pelo.
//
// Familias implementadas (direccion de 32 bits + valor de 16, tal como se publican):
//   80 XXXXXX 00YY   escribe el byte YY cada campo            (cacheada)
//   81 XXXXXX YYYY   escribe la media palabra YYYY cada campo (cacheada)
//   A0 XXXXXX 00YY   escribe el byte una sola vez, al arrancar (sin cache)
//   A1 XXXXXX YYYY   igual con media palabra
//   D0 XXXXXX 00YY   condicion: ejecuta la linea siguiente si el byte vale YY
//   D1 XXXXXX YYYY   condicion: ... si la media palabra vale YYYY
//   D2 XXXXXX 00YY   condicion: ... si el byte NO vale YY
//   D3 XXXXXX YYYY   condicion: ... si la media palabra NO vale YYYY
//   50 00CCII 0000VV repetidor: la linea siguiente se aplica CC veces, sumando II a la
//                    direccion y VV al valor en cada vuelta (tablas de items, inventarios)
//
// Lo que NO se emula, y se dice al cargar en vez de fingir que se aplica: 88/89 (escribir
// solo mientras se tiene pulsado el boton del propio GameShark, que es un boton fisico que
// esta maquina no tiene) y las lineas internas del cartucho (CC = ventana de RAM ampliada,
// DE = direccion de arranque, EE/FF = control de la lista), que hablan del hardware del
// aparato y no del juego.

#include "types.hpp"
#include <string>
#include <vector>

namespace kestrel {

struct CPU;

struct Cheats {
  struct Line { u32 addr = 0, val = 0; };
  struct Entry {
    std::string       name;
    bool              on = true;
    std::vector<Line> lines;
  };

  std::vector<Entry> list;

  // Carga un fichero .cht. Formato de texto: `[Nombre]` abre un truco (`[-Nombre]` lo deja
  // apagado), `#` y `;` son comentarios, y el resto son lineas `AAAAAAAA VVVV` con los dos
  // numeros en hexadecimal tal como se publican.
  auto loadFile(const std::string& path, std::string& error) -> bool;
  // Fuente por defecto: KESTREL_CHEATS si esta puesta, y si no el `.cht` que haya al lado
  // de la ROM con su mismo nombre. Silencioso cuando no hay ninguno.
  auto loadForRom(const std::string& romPath) -> void;

  auto enabled() const -> bool { return armed; }
  // Se llama al cierre de cada campo de video, con el nucleo parado (coreMutex tomado).
  auto applyField(CPU& cpu) -> void;

private:
  enum class Phase { Boot, Frame };
  auto run(CPU& cpu, const Entry& e, Phase ph) -> void;
  bool armed    = false;   // hay al menos un truco encendido
  bool bootDone = false;   // las familias A0/A1 ya se aplicaron
};

}  // namespace kestrel
