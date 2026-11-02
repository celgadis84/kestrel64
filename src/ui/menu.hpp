#pragma once
// Menu -- menu bar and dialogs INSIDE the emulator window (Win32 only).
//
// The game window is the application: it opens ROMs and changes video, audio and pad options.
// Options that can change live (core/runtime.hpp) are applied at once; the rest are saved to
// the profile and the emulator relaunches itself with the new environment. Uses only the HWND
// it is given (no GLFW/Vulkan); on other platforms the functions do nothing.
//
// Invariants:
// - Hooks carries pointers to atomics owned by the run loop (pause, shutdown, state
//   save/load/slot mailboxes); the menu writes them and the core loop consumes them.
// - relaunchPending() must be queried by main() AFTER audio and video are closed, so the new
//   process finds the sound device free; doRelaunch() then starts it.
//
// Threads: attach() and the menu procedure run on the window (UI/main) thread; the hook
// mailboxes are atomics consumed by the core loop.
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
