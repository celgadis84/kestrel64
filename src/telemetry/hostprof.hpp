#pragma once
#include "../core/types.hpp"

// Host sampling profiler (Windows). Opt-in via KESTREL_HOSTPROF=<period ms>.
// start() must be called from the thread to be sampled; stop() dumps the histogram.
namespace kestrel::hostprof {
auto start() -> void;
auto stop()  -> void;
}
