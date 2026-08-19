#pragma once
#include "../core/types.hpp"

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
}
