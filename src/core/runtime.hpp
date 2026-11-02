#pragma once
// Runtime settings -- process-wide values that the UI may change while the emulator runs.
//
// Each value is an atomic (or a mutex-guarded record) seeded once from the environment by
// initFromEnv(), so default and gate behaviour is unchanged, and from then on owned by whoever
// writes it (window menu, telemetry). Anything not listed here needs a restart.
//
// Invariants:
// - Call initFromEnv() once before starting any other thread; environment variables are not
//   written afterwards (changing the environment under other threads is unsafe).
// - Multi-field requests use a flag published last with release: winReq is set only after
//   winW/winH/winFull are written; liveReq is set after liveOpts is stored under liveMx.
//   liveOpts take effect only when System::serviceStateReq applies them with the RCP at rest.
// - Pad and Transfer Pak mappings are versioned by padGen / tpakGen[]; the consumer rereads on
//   a generation change. The cartridge image itself is loaded by the joybus on the CPU thread.
//
// Threads: written mostly by the UI/menu thread and the telemetry thread; read by the CPU
// thread (resetReq, tpak*, liveReq via System, throttle in the run loop), the presenter/main
// thread (hud, aspect, filter, win*, pad mapping) and the audio code (audioOn, volume).
// joyName/joyGen are published by the presenter, the only thread allowed to query GLFW;
// joyName is guarded by joyMx.
// kestrel64 -- ajustes que se pueden cambiar CON EL EMULADOR EN MARCHA.
//
// Hasta aqui toda la configuracion se leia una vez del entorno con getenv(), que es lo
// correcto para los gates y el modo lote: el entorno del proceso es inmutable y el
// experimento sale reproducible. Pero el menu de la ventana tiene que poder subir la escala
// o quitar el sonido sin relanzar nada, y `getenv` en el bucle de video no vale (ni es
// atomico, ni el menu puede escribir el entorno de forma visible para otro hilo).
//
// Esto es el puente: un punado de atomicos que se SIEMBRAN desde el entorno al arrancar --
// asi el comportamiento por defecto y el de los gates no cambia ni un bit -- y que a partir
// de ahi manda quien los escriba. Todo lo que NO esta aqui exige relanzar el proceso, y el
// menu lo dice en vez de fingir que se ha aplicado.

#include "types.hpp"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

namespace kestrel::rt {

// Limitador de velocidad: -1 = automatico (limita si hay ventana), 0 = suelto, 1 = limitado.
inline std::atomic<int> throttle{-1};
// HUD de telemetria pintado sobre la imagen.
inline std::atomic<bool> hud{true};
// Boton RESET de la consola (menu / Ctrl+R / MCP reset.soft): lo recoge System al cerrar campo.
inline std::atomic<bool> resetReq{false};
// Salida de sonido del anfitrion y volumen 0..100 (el dispositivo sigue abierto: apagar el
// audio a mitad de partida no puede reabrir el dispositivo desde otro hilo sin carreras).
inline std::atomic<bool> audioOn{true};
inline std::atomic<int>  volume{100};
// Generacion del mapa de mando: subirla obliga a releer los ficheros KESTREL_PAD1..4.
inline std::atomic<u32> padGen{0};

// --- los cuatro puertos de mando ---------------------------------------------------
// La consola tiene cuatro conectores y el juego pregunta por los cuatro. Lo que hay
// enchufado en cada uno se decide en caliente desde el menu de la ventana, asi que vive
// aqui y no en el entorno: enchufar un segundo mando a mitad de partida es exactamente lo
// que se hace en un sofa.
//
// padDev: -1 = teclado, >= 0 = indice de joystick de GLFW. Un puerto lee UNA fuente, no las
// dos: con cuatro jugadores, "teclado y mando a la vez" haria que el jugador 1 moviese
// tambien al 2. El puerto 1 nace en -2 = automatico, que es la mezcla de siempre (teclado
// + primer mando) y no rompe a quien ya jugaba.
inline std::atomic<bool> padOn[4]  = {{true}, {false}, {false}, {false}};
// -3 sin aparato elegido, -2 automatico (teclado + primer mando, el modo de siempre),
// -1 teclado, >=0 indice de joystick de GLFW.
inline std::atomic<int>  padDev[4] = {{-2}, {-3}, {-3}, {-3}};
inline std::atomic<int>  padAcc[4] = {{1}, {1}, {1}, {1}};   // 0 nada, 1 Controller Pak, 2 Rumble Pak, 3 Transfer Pak
// ROM de Game Boy metida en el Transfer Pak de cada puerto ("" = sin cartucho). La escriben
// el menu y Memory::reset (KESTREL_TPAK_ROM1..4); la CARGA el joybus, en el hilo de la CPU,
// cuando ve subir la generacion: el cartucho nunca cambia debajo de una transaccion.
inline std::mutex       tpakMx;
inline std::string      tpakRom[4];
inline std::atomic<u32> tpakGen[4] = {{0}, {0}, {0}, {0}};

// Opciones que el nucleo recoge EN CALIENTE (menu Opciones > Aplicar): trucos, rebobinado y
// texturas HD / realce. El menu deja aqui los valores ya traducidos con las reglas de
// `toEnv` y sube `liveReq`; System::serviceStateReq los aplica con el RCP en reposo y el
// nucleo parado, que es el unico momento en que nadie esta leyendo un truco, una foto de la
// cinta o una textura sustituta. Los valores viajan aqui y no por setenv: cambiar el entorno
// con otros hilos leyendolo no es seguro.
struct LiveOpts {
  std::string cheats;              // KESTREL_CHEATS ("" = el .cht de al lado de la ROM)
  std::string texfx, texdump, texpack;
  bool rewind = false;
  u32  rewindMb = 256, rewindFields = 2;
  auto operator==(const LiveOpts&) const -> bool = default;
};
inline auto liveFromEnv() -> LiveOpts {
  auto env = [](const char* n) { const char* e = std::getenv(n); return std::string(e ? e : ""); };
  LiveOpts o;
  o.cheats = env("KESTREL_CHEATS");
  o.texfx = env("KESTREL_TEXFX"); o.texdump = env("KESTREL_TEXDUMP"); o.texpack = env("KESTREL_TEXPACK");
  const std::string rw = env("KESTREL_REWIND");
  o.rewind = !rw.empty() && rw != "0";
  const std::string mb = env("KESTREL_REWIND_MB"), iv = env("KESTREL_REWIND_FIELDS");
  if(!mb.empty()) o.rewindMb = (u32)std::strtoul(mb.c_str(), nullptr, 0);
  if(!iv.empty()) o.rewindFields = (u32)std::strtoul(iv.c_str(), nullptr, 0);
  return o;
}
inline std::mutex        liveMx;
inline LiveOpts          liveOpts;
inline bool              liveSeeded = false;   // liveOpts ya partio del entorno de arranque
inline std::atomic<bool> liveReq{false};
// Ultimo juego de valores pedido (o el de arranque). Con liveMx cogido.
inline auto liveCurrentLocked() -> LiveOpts& {
  if(!liveSeeded) { liveOpts = liveFromEnv(); liveSeeded = true; }
  return liveOpts;
}

// Mandos que Windows tiene enchufados AHORA, publicados por el presentador (que es el unico
// hilo que puede preguntarle a GLFW) para que el dialogo de mando pueda ensenar nombres de
// verdad en vez de numeros. La generacion sube cuando cambia la lista.
inline constexpr int kMaxJoy = 16;
inline std::mutex          joyMx;
inline std::string         joyName[kMaxJoy];   // vacio = ese hueco no tiene nada
inline std::atomic<u32>    joyGen{0};

// Relacion de aspecto con la que la imagen se coloca dentro de la ventana. El VI saca
// SIEMPRE una senal 4:3 pase lo que pase la resolucion del framebuffer, asi que 4:3 es lo
// fiel y es el valor de fabrica. 16:9 NO ensancha el campo de vision -- eso solo lo puede
// hacer el juego, dibujando mas mundo -- sino que estira la imagen anamorfica que generan
// los juegos con modo panoramico propio (Perfect Dark, GoldenEye, Turok, Rush 2...), que
// aplastan un encuadre ancho dentro del mismo framebuffer contando con que la tele lo
// estire. 0/0 = llenar la ventana entera sin conservar nada.
inline std::atomic<int> aspectW{4};
inline std::atomic<int> aspectH{3};

// Filtro de presentacion (src/video/postfx.hpp): 0 = vecino mas cercano (de fabrica, el blit
// de siempre), 1 bilineal, 2 bilineal nitido, 3 FSR, 4 CRT, 5 cel-shading, 6 oleo, 7 Anime4K.
// Solo cambia lo que se ve en la ventana; volcados, md5 y capturas siguen siendo el cuadro
// original del invitado.
inline std::atomic<int> videoFilter{0};

inline auto parseVideoFilter(const char* s) -> int {
  static const char* kNames[8] = {"nearest", "bilinear", "sharp", "fsr",
                                  "crt", "cel", "oleo", "a4k"};
  if(!s || !*s) return -1;
  if(s[0] >= '0' && s[0] <= '9') { int n = std::atoi(s); return n >= 0 && n < 8 ? n : -1; }
  for(int i = 0; i < 8; i++) if(std::strcmp(s, kNames[i]) == 0) return i;
  if(std::strcmp(s, "anime4k") == 0) return 7;
  if(std::strcmp(s, "toon") == 0) return 5;
  return -1;
}

// Peticion de cambio de ventana atendida por el presentador en su proximo cuadro. `winReq`
// se pone a 1 el ultimo, cuando el resto de campos ya estan escritos.
inline std::atomic<int> winW{0};
inline std::atomic<int> winH{0};
inline std::atomic<int> winFull{-1};   // -1 = no tocar, 0 = ventana, 1 = pantalla completa
inline std::atomic<int> winReq{0};

// KESTREL_SPEEDMODE=hw -- "fiel a consola". Un solo interruptor que devuelve el emulador a
// la velocidad de la maquina real: relojes nativos (CPU 93.75 MHz, RSP 62.5 MHz, RDRAM 250
// MHz) y el CPI calibrado del R4300i. No es una opcion de rendimiento: es la que hay que
// poner cuando lo que se quiere es que el juego vea el mismo tiempo que veria en la consola
// -- las demos que cuentan campos (la liana de DK64 es la de siempre) solo salen bien con esta.
//
// Lo que el modo fiel NO hace es armar el limitador de velocidad de PARED por su cuenta: lo
// deja en automatico, que ya significa "limitar solo si hay ventana". Lo que el JUEGO ve es
// el reparto de trabajo por campo, y eso sale de los relojes y del CPI, no del ritmo al que
// corra el anfitrion. Sin ventana no hay pantalla que respetar: un gate, un bench o la suite
// krom corren a ciegas, y clavarlos a 59.94 Hz solo los haria tardar en tiempo real lo que
// dura la partida sin cambiar ni un bit del resultado.
//
// Vive en el entorno y no solo en el perfil porque System tambien tiene que verla al montar
// los relojes, antes de que exista ningun menu.
inline auto speedModeHw() -> bool {
  const char* s = std::getenv("KESTREL_SPEEDMODE");
  return s && (s[0] == 'h' || s[0] == 'H');
}

// Siembra desde el entorno. Llamar una vez, antes de arrancar los hilos.
inline auto initFromEnv() -> void {
  // El modo fiel deja el limitador en automatico (limitar solo si hay ventana), pero una
  // peticion EXPLICITA del usuario manda sobre el: quien pone KESTREL_THROTTLE lo ha escrito
  // a proposito.
  const char* thEnv = std::getenv("KESTREL_THROTTLE");
  if(speedModeHw()) throttle.store(-1);
  if(thEnv) throttle.store(thEnv[0] == '0' ? 0 : 1);
  hud.store(std::getenv("KESTREL_HUD_OFF") == nullptr);
  audioOn.store(envFlag("KESTREL_AUDIO", true));
  if(const char* v = std::getenv("KESTREL_VOLUME")) {
    int n = std::atoi(v);
    volume.store(n < 0 ? 0 : n > 100 ? 100 : n);
  }
  // KESTREL_ASPECT: "4:3" (fiel), "16:9" (juegos con modo panoramico propio), "estirar"
  // (llenar la ventana) o cualquier "W:H" / "WxH".
  if(const char* a = std::getenv("KESTREL_ASPECT")) {
    int w = 0, h = 0;
    const char* sep = a;
    while(*sep && *sep != ':' && *sep != 'x' && *sep != 'X') sep++;
    if(*sep) { w = std::atoi(a); h = std::atoi(sep + 1); }
    if(w > 0 && h > 0) { aspectW.store(w); aspectH.store(h); }
    else               { aspectW.store(0); aspectH.store(0); }   // estirar
  }
  // KESTREL_FILTER: nearest | bilinear | sharp | fsr | crt | cel | oleo | a4k (o su numero)
  if(const char* f = std::getenv("KESTREL_FILTER")) {
    int n = parseVideoFilter(f);
    if(n >= 0) videoFilter.store(n);
    else std::fprintf(stderr, "[video] KESTREL_FILTER=%s no reconocido; vecino mas cercano\n", f);
  }
}

}  // namespace kestrel::rt
