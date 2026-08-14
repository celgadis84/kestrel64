#pragma once
// kestrel64 — top-level system. Owns memory, the ROM, and the telemetry server.
// At M0 there is no CPU yet; the system loads a ROM, builds the memory map, and
// serves telemetry so the MCP can already inspect state.

#include "types.hpp"
#include "memory.hpp"
#include "rom.hpp"
#include "../cpu/cpu.hpp"
#include "../video/present.hpp"
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <memory>
#include <vector>

namespace kestrel {

namespace telemetry { struct Server; }

struct System {
  Rom    rom;
  Memory memory;
  CPU    cpu;

  // The CPU runs on the run() thread; telemetry handlers touch CPU/memory from
  // the telemetry thread. coreMutex serializes the two. Lock it for any access
  // that reads or mutates emulation state across the thread boundary.
  std::mutex coreMutex;

  std::atomic<bool> paused{true};    // start paused so single-stepping is deterministic
  std::atomic<bool> shutdown{false};

  // Per-domain clock model. Nominal N64 rates; `oc` = overclock multiplier
  // (1.0 = stock console). A future UI can retune these live to model an
  // overclocked machine per region. Speed% is measured against the (possibly
  // overclocked) CPU target rate — 100% == running at real N64 speed.
  struct Clocks {
    double cpuHz   = 93'750'000.0;   // R4300i
    double rspHz   = 62'500'000.0;   // RSP
    double rdramHz = 250'000'000.0;  // RDRAM (DDR)
    double cpuOc = 1.0, rspOc = 1.0, rdramOc = 1.0;
    auto cpuTarget()   const -> double { return cpuHz   * cpuOc; }
    auto rspTarget()   const -> double { return rspHz   * rspOc; }
    auto rdramTarget() const -> double { return rdramHz * rdramOc; }
  } clocks;

  // Speed telemetry. The interpreter models 1 emulated CPU cycle per retired
  // instruction (CPI≈1 baseline); speed% = emulated-cycles/sec vs the CPU target
  // clock, sampled over a sliding window so it tracks *current* speed, not a
  // lifetime average. 100.0 == real N64 speed. retiredInsns is the lifetime
  // count for MIPS/heartbeat.
  std::atomic<double> n64SpeedPct{0.0};   // overall (CPU domain) % of realtime
  std::atomic<double> rspSpeedPct{0.0};   // RSP domain % (0 while RSP idle)
  std::atomic<double> rdramSpeedPct{0.0}; // RDRAM domain % (0 until modeled)
  std::atomic<u64>    retiredInsns{0};    // lifetime CPU instructions (=cycles)
  u64                 rspCycles = 0;      // lifetime RSP steps (=cycles), run-thread only

  static constexpr const char* kVersion = "0.0.1-M1";

  // Load a ROM, build memory, HLE-boot the CPU. Returns false with `error` set.
  auto init(const std::string& romPath, std::string& error) -> bool;

  // Start the telemetry server on its own thread (non-blocking).
  auto startTelemetry(u16 port) -> bool;

  // Arm the VI presentation window (M3.1). No-op if disabled (KESTREL_VIDEO).
  // The window loop itself runs on the main thread inside runLoop().
  auto startVideo() -> void;

  // Top-level blocking loop. With video on: CPU on a worker thread, the GLFW/
  // Vulkan present loop on THIS (main) thread — GLFW requires the main thread.
  // Without video: just runs the CPU loop here. Returns on shutdown.
  auto runLoop() -> void;

  // Block the calling thread, running the CPU when not paused, until shutdown.
  auto run() -> void;

  // Step the CPU n instructions (caller holds coreMutex). Returns steps taken.
  // While the RSP is running, it is advanced interleaved with the CPU at the
  // hardware clock ratio (RSP 62.5 MHz : CPU 93.75 MHz = 2:3) so the two cores make
  // progress together — required for CPU<->RSP SIGNAL handshakes.
  auto stepCpu(u64 n) -> u64;
  u32 rspPhase = 0;   // fractional accumulator for the 2:3 RSP:CPU step ratio

  // PC breakpoints (debug). stepCpu checks these only while the vector is
  // non-empty, so there is zero hot-path cost when nothing is set. A hit auto-
  // pauses the run loop and records the PC in lastBpHit. Guarded by coreMutex
  // like the rest of the core state; run_until pushes/pops a temporary entry.
  std::vector<u32> breakpoints;
  std::atomic<u32> lastBpHit{0};   // PC of the breakpoint that last fired (0 = none)

  auto requestShutdown() -> void { shutdown.store(true); }

  System();       // out-of-line: unique_ptr<Server> holds an incomplete type here
  ~System();

private:
  std::unique_ptr<telemetry::Server> tele;
  std::thread teleThread;
  Presenter presenter;
  bool videoOn = false;
};

}  // namespace kestrel
