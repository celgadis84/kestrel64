#pragma once
// ROM library -- entry screen with a 3D cover carousel (docs/LAUNCHER.md).
//
// Shown when the emulator starts without a ROM, or from "File > Open ROM..." in the game window.
// Pure Win32 + GDI on its own canvas: it touches neither GLFW, Vulkan nor the game present path.
// On other platforms pickRomLibrary() returns an empty string.
//
// Invariants:
// - pickRomLibrary() BLOCKS its caller in a private message loop and returns the chosen ROM
//   path, or "" if closed without choosing or if a library is already open.
// - One library per process (libraryOpen()).
// - From the game window thread it must be launched on a separate thread: that thread is
//   presenting frames and cannot enter a modal loop.
//
// Threads: runs on the thread that calls it (main() at startup, or a helper thread started by
// the menu, detached); the open flag is atomic so libraryOpen() may be polled from any thread.
// kestrel64 -- biblioteca de ROMs con carrusel 3D de caratulas.
//
// Es la puerta de entrada del emulador cuando se abre sin ROM (doble clic en el .exe, o
// "Archivo > Abrir ROM..." desde la ventana del juego). La estetica es la de un menu
// (menu de flashcart de N64): lienzo de baja resolucion escalado por entero, tipografia de
// mapa de bits, pocos colores planos y transiciones cortas. Ver docs/LAUNCHER.md.
//
// Todo es Win32 + GDI puro sobre un lienzo propio: no toca GLFW, ni Vulkan, ni el camino de
// presentacion del juego. En otras plataformas la funcion existe y devuelve cadena vacia.

#include <string>

namespace kestrel::ui {

// Abre la biblioteca y devuelve la ROM elegida, o "" si se cierra sin elegir.
//
// BLOQUEA al hilo que la llama: monta su ventana y su propio bucle de mensajes. Desde el
// hilo de la ventana del juego hay que lanzarla en un hilo aparte (ese hilo esta
// presentando cuadros y no puede meterse en un bucle modal sin congelar la imagen).
// `ownerHwnd` puede ser nullptr: solo sirve para centrar la ventana sobre el juego.
auto pickRomLibrary(void* ownerHwnd) -> std::string;

// Ya hay una biblioteca abierta (una sola instancia por proceso).
auto libraryOpen() -> bool;

}  // namespace kestrel::ui
