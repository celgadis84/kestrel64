#pragma once
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
#include <cstdlib>
#include <mutex>
#include <string>

namespace kestrel::rt {

// Limitador de velocidad: -1 = automatico (limita si hay ventana), 0 = suelto, 1 = limitado.
inline std::atomic<int> throttle{-1};
// HUD de telemetria pintado sobre la imagen.
inline std::atomic<bool> hud{true};
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
inline std::atomic<int>  padAcc[4] = {{1}, {1}, {1}, {1}};   // 0 nada, 1 Controller Pak, 2 Rumble Pak

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
}

}  // namespace kestrel::rt
