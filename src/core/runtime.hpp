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

namespace kestrel::rt {

// Limitador de velocidad: -1 = automatico (limita si hay ventana), 0 = suelto, 1 = limitado.
inline std::atomic<int> throttle{-1};
// HUD de telemetria pintado sobre la imagen.
inline std::atomic<bool> hud{true};
// Salida de sonido del anfitrion y volumen 0..100 (el dispositivo sigue abierto: apagar el
// audio a mitad de partida no puede reabrir el dispositivo desde otro hilo sin carreras).
inline std::atomic<bool> audioOn{true};
inline std::atomic<int>  volume{100};
// Generacion del mapa de mando: subirla obliga a releer el fichero KESTREL_PAD1.
inline std::atomic<u32> padGen{0};

// Peticion de cambio de ventana atendida por el presentador en su proximo cuadro. `winReq`
// se pone a 1 el ultimo, cuando el resto de campos ya estan escritos.
inline std::atomic<int> winW{0};
inline std::atomic<int> winH{0};
inline std::atomic<int> winFull{-1};   // -1 = no tocar, 0 = ventana, 1 = pantalla completa
inline std::atomic<int> winReq{0};

// Siembra desde el entorno. Llamar una vez, antes de arrancar los hilos.
inline auto initFromEnv() -> void {
  if(const char* t = std::getenv("KESTREL_THROTTLE")) throttle.store(t[0] == '0' ? 0 : 1);
  hud.store(std::getenv("KESTREL_HUD_OFF") == nullptr);
  audioOn.store(envFlag("KESTREL_AUDIO", true));
  if(const char* v = std::getenv("KESTREL_VOLUME")) {
    int n = std::atoi(v);
    volume.store(n < 0 ? 0 : n > 100 ? 100 : n);
  }
}

}  // namespace kestrel::rt
