#pragma once
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
