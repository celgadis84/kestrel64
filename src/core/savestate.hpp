#pragma once
// kestrel64 — estado guardado (savestate).
//
// Una foto completa del estado de la maquina: RDRAM, DMEM/IMEM, PIF RAM, el medio de
// guardado del cartucho, los registros del RCP y el estado arquitectonico de CPU, RSP y
// RDP. Se toma con el RCP EN REPOSO — la cola del RDP drenada y la tarea del RSP
// terminada — para que el fichero no dependa de en que hilo iba cada worker: asi un
// estado guardado en Lockstep se carga en Threaded y al reves.
//
// El formato es explicito, campo a campo, no un volcado de structs: un cambio de layout
// en CPU/Rsp/SoftRdp no puede corromper en silencio un fichero viejo. La version del
// contenedor sube cuando cambia la lista de campos.

#include "types.hpp"
#include <string>

namespace kestrel {

struct System;
// Visitante del estado. Definido en savestate.cpp; es amigo de Rsp/SoftRdp/Memory para
// poder leer y reescribir el estado privado de ejecucion (latch de ranura de retardo del
// RSP, punteros de reanudacion del FIFO del RDP) sin abrirlo al resto del emulador.
struct StateVisitor;

// Guarda / carga el estado en `path`. Devuelve false con `err` puesto. NO son seguras
// desde otro hilo: el llamante tiene que tener coreMutex (el bucle de System las llama
// desde el hilo de la CPU, ver stateSaveReq/stateLoadReq).
auto saveState(System& sys, const std::string& path, std::string& err) -> bool;
auto loadState(System& sys, const std::string& path, std::string& err) -> bool;

// Ruta de una ranura: la ROM con la extension cambiada a .stN, igual que el fichero de
// guardado de la pila (.eep/.sra/.fla) vive al lado de la ROM.
auto stateSlotPath(const System& sys, int slot) -> std::string;

}  // namespace kestrel
