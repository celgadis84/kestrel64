#pragma once
// kestrel64 — VI presentation (M3.1). A cross-platform Vulkan + GLFW window that
// reads the VI framebuffer out of RDRAM each refresh and blits it to the
// swapchain. This is the "eyes" of the emulator: it shows whatever the RDP has
// left in RDRAM at VI_ORIGIN, independent of how those pixels got there (RDP
// LLE, a software rasterizer, or a CPU test pattern). The Vulkan instance/device
// set up here is the same stack parallel-rdp will render into at M3.2.
// Enabled by KESTREL_VIDEO. If Vulkan or the window can't come up, presentation
// silently disables and the emulator keeps running headless.

#include "../core/types.hpp"
#include <atomic>
#include <string>
#include <thread>
#include <vector>

namespace kestrel {

struct Memory;
struct Vk;   // opaque Vulkan state, defined in present.cpp

struct Presenter {
  // Wire up the presenter (store pointers). Returns whether video is wanted.
  // The three speed gauges (CPU / RSP / RDRAM domains, % of realtime) are read
  // each frame to paint the on-window HUD footer; pass nullptr to omit a gauge.
  auto start(Memory* mem, std::atomic<bool>* shutdown,
             const std::atomic<double>* cpuPct = nullptr,
             const std::atomic<double>* rspPct = nullptr,
             const std::atomic<double>* rdramPct = nullptr,
             const char* title = nullptr) -> bool;

  // Bring up Vulkan/GLFW on the calling (main) thread. Call BEFORE starting the
  // CPU worker — a busy sibling thread stalls driver bring-up on this box.
  auto open() -> bool;
  // Present one frame (compose from RDRAM + blit). false → window closed.
  auto pumpFrame() -> bool;
  // Tear down Vulkan/GLFW.
  auto close() -> void;

private:
  Memory* mem = nullptr;
  std::atomic<bool>* shutdown = nullptr;
  const std::atomic<double>* cpuPct = nullptr;    // HUD gauges (% of realtime)
  const std::atomic<double>* rspPct = nullptr;
  const std::atomic<double>* rdramPct = nullptr;
  std::string windowTitle = "kestrel64";   // set to the loaded ROM's internal name
  Vk* vk = nullptr;         // pimpl: all Vulkan/GLFW handles
  std::vector<u32> frame;   // scratch R8G8B8A8 buffer uploaded to the Vulkan image
};

}  // namespace kestrel
