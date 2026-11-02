#pragma once
// Savestate -- full machine snapshot to file or memory buffer, used by save slots, rewind and
// netplay sync.
//
// Responsibility: serialise RDRAM, DMEM/IMEM, PIF RAM, cartridge save media, RCP registers and
// CPU/RSP/RDP architectural state field by field (versioned container), and restore it.
//
// Invariants:
// - Take or load a state only with the RCP at rest (RDP queue drained, no RSP task in flight);
//   this makes a state portable between Lockstep and Threaded. Derived schedules are not
//   stored; they are reset on load.
// - The caller must hold System::coreMutex; these functions do not lock. They are not safe
//   from another thread while the core runs.
// - loadState restores the input movie position too (movie::seek) so a replay follows a rewind.
// - captureDelta returns false (leaving `cur` untouched) when the state size changed; the
//   caller then falls back to captureState.
//
// Threads: called on the run (CPU) loop thread from System::serviceStateReq and the rewind
// engine, and from netplay when it syncs a peer. Other threads only post requests
// (System::stateSaveReq / stateLoadReq / rewindReq / netSyncReq).
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
#include <vector>

namespace kestrel {

struct System;
namespace rewind::codec { struct DeltaSink; }
// Visitante del estado. Definido en savestate.cpp; es amigo de Rsp/SoftRdp/Memory para
// poder leer y reescribir el estado privado de ejecucion (latch de ranura de retardo del
// RSP, punteros de reanudacion del FIFO del RDP) sin abrirlo al resto del emulador.
struct StateVisitor;

// Guarda / carga el estado en `path`. Devuelve false con `err` puesto. NO son seguras
// desde otro hilo: el llamante tiene que tener coreMutex (el bucle de System las llama
// desde el hilo de la CPU, ver stateSaveReq/stateLoadReq).
auto saveState(System& sys, const std::string& path, std::string& err) -> bool;
auto loadState(System& sys, const std::string& path, std::string& err) -> bool;

// Las mismas dos operaciones SIN fichero: el estado entero (cabecera incluida, para que la
// comprobacion de identidad valga igual) en un vector de bytes. Es lo que usa el rebobinado,
// que toma un estado por campo y no puede pagar un viaje al disco. Mismas condiciones que
// saveState/loadState: RCP en reposo y coreMutex en manos del llamante.
auto captureState(System& sys, std::vector<u8>& out) -> void;
auto restoreState(System& sys, const u8* data, usize len, std::string& err) -> bool;

// Foto del rebobinado SIN foto: compara el estado vivo contra `cur` (la foto anterior) segun
// se recorre, deja en `delta` la diferencia hacia atras y en `cur` la foto nueva. false si el
// estado ya no mide lo que `cur` (entonces `cur` NO se ha tocado y toca captureState).
auto captureDelta(System& sys, rewind::codec::DeltaSink& sink, std::vector<u8>& cur, std::vector<u8>& delta) -> bool;

// Ruta de una ranura: la ROM con la extension cambiada a .stN, igual que el fichero de
// guardado de la pila (.eep/.sra/.fla) vive al lado de la ROM.
auto stateSlotPath(const System& sys, int slot) -> std::string;

}  // namespace kestrel
