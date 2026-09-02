#pragma once
// kestrel64 -- barra de menu y dialogos DENTRO de la ventana del emulador.
//
// La ventana de juego es la aplicacion: abre ROMs, cambia la resolucion, el sonido y el
// mando, y ensena el mismo catalogo completo de opciones que el lanzador. Lo que se puede
// cambiar en caliente (ver core/runtime.hpp) se aplica al momento; lo que no, se guarda en
// el perfil y el emulador se relanza a si mismo con el entorno nuevo.
//
// Todo lo de aqui es Win32 puro: no toca GLFW ni Vulkan, solo el HWND que le pasan. En
// otras plataformas las funciones existen y no hacen nada.

#include <atomic>
#include <string>

namespace kestrel::ui {

struct Hooks {
  std::atomic<bool>* paused   = nullptr;   // pausa del bucle de ejecucion
  std::atomic<bool>* shutdown = nullptr;   // pedir el cierre ordenado
  std::atomic<int>*  stSave   = nullptr;   // buzones de estado guardado (F5/F7/F6)
  std::atomic<int>*  stLoad   = nullptr;
  std::atomic<int>*  stSlot   = nullptr;
  std::string        rom;                  // ROM en marcha (para relanzar con ella)
  bool resizeForMenu = true;               // falso en pantalla completa (la ventana ya cubre)
};

// Cuelga la barra de menu del HWND dado y engancha su procedimiento de ventana.
auto attach(void* hwnd, const Hooks& h) -> void;

// Un dialogo ha pedido relanzar el emulador. main() lo consulta DESPUES de cerrar el audio
// y el video, para que el proceso nuevo encuentre el dispositivo de sonido libre.
auto relaunchPending() -> bool;
auto doRelaunch() -> void;

}  // namespace kestrel::ui
