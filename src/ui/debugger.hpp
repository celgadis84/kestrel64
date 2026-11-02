#pragma once
// Debugger window -- separate Win32 + Direct3D 11 + Dear ImGui window (docs/DEBUGGER.md).
//
// Everything it shows is requested through Server::call, the same dispatch the MCP uses, so
// there is no second access path into the core. It opens with --debugger, KESTREL_DEBUGGER=1
// or the Debug menu, also in batch mode (--run). Outside Windows the functions do nothing.
//
// Invariants:
// - main() calls init(System&) first, before any open(), and close() on exit BEFORE the
//   telemetry server is shut down, because the window calls into it.
// - Machine data is requested through telemetry (not read directly), so it observes what a TCP
//   client would see.
//
// Threads: the window runs on its own thread (created by open(), joined by close()); it exits
// when System::shutdown is set. open() is safe from any thread.
// kestrel64 -- ventana del depurador (docs/DEBUGGER.md).
//
// Ventana propia (Win32 + Direct3D 11 + Dear ImGui) en su hilo, independiente de la ventana
// de juego: se abre con --debugger, KESTREL_DEBUGGER=1 o el menu Depurar, y tambien en lote
// (--run) sin ventana de juego. Todo lo que ensena lo pide por el MISMO despacho que el MCP
// (Server::call), asi que ventana y MCP ven exactamente lo mismo y no hay un segundo camino
// de acceso al nucleo que mantener.
//
// Fuera de Windows las funciones existen y no hacen nada.

namespace kestrel { struct System; }

namespace kestrel::dbgwin {

// Guarda la maquina a la que se asoma la ventana. main() la llama siempre, antes de nada.
auto init(System& sys) -> void;
// Abre la ventana (si ya esta abierta, la trae delante). Seguro desde cualquier hilo.
auto open() -> void;
// Cierra la ventana y espera a su hilo. main() la llama al salir.
auto close() -> void;

}  // namespace kestrel::dbgwin
