#pragma once
// Host profiler -- opt-in sampling of where the EMULATOR spends host cycles (Windows only).
//
// Enabled with KESTREL_HOSTPROF=<period ms>; a watcher thread suspends one target thread at a
// fixed interval and buckets its RIP. Without the variable nothing starts; on other platforms
// the functions are empty.
//
// Invariants:
// - start(label) samples the CALLING thread, and only the one whose label matches
//   KESTREL_HOSTPROF_WHO (default "cpu"); a second start while a sampler lives is ignored.
// - gate() is optional; if installed, samples are taken only while the flag reads true.
// - stop() joins the watcher and dumps the histogram; it is a no-op if no sampler runs.
//
// Threads: start() is called by the CPU ("cpu"), RSP ("rsp") and RDP worker ("rdp") threads at
// their entry, gate() by the RSP thread, stop() by the CPU thread when its run loop ends. The
// watcher is an internal thread.
#include "../core/types.hpp"
#include <atomic>

// Host sampling profiler (Windows). Opt-in via KESTREL_HOSTPROF=<period ms>.
// start() must be called from the thread to be sampled; stop() dumps the histogram.
//
// `label` nombra al hilo llamante ("cpu", "rsp", "rdp"). Se muestrea UNO solo, el que
// pida KESTREL_HOSTPROF_WHO (por defecto "cpu"): suspender dos hilos a la vez con el
// mismo muestreador falsearia ambos perfiles, y el interesante casi siempre es el que
// resulte ser el palo largo del momento.
namespace kestrel::hostprof {
auto start(const char* label = "cpu") -> void;
auto stop()  -> void;
// Puerta opcional: si se instala, solo se muestrea mientras el flag este a true. Sin ella,
// un worker que pasa la mitad del tiempo dormido esperando tarea mete todo ese sueno en el
// histograma como "ntdll" y el perfil deja de decir nada sobre el coste de emular.
auto gate(const std::atomic<bool>* g) -> void;
}
