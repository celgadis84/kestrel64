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

  // Ata las teclas de estado guardado (F5 guardar / F7 cargar / F6 cambiar de ranura) a los
  // buzones del System. La ventana corre en el hilo principal y la CPU en un worker, asi que
  // la tecla no guarda nada: deja la peticion y el bucle de ejecucion la atiende con el RCP
  // parado. Sin atar, las teclas no hacen nada.
  auto bindState(std::atomic<int>* save, std::atomic<int>* load, std::atomic<int>* slot) -> void {
    stSave = save; stLoad = load; stSlot = slot;
  }

  // Ata la barra de menu de la ventana (Win32) al estado del emulador: pausa, cierre
  // ordenado y la ROM en marcha para poder relanzarse con ella. Sin atar, la ventana sale
  // sin menu y el emulador se comporta exactamente como antes.
  auto bindMenu(std::atomic<bool>* pausedFlag, const std::string& rom) -> void {
    menuPaused = pausedFlag; menuRom = rom;
  }

  // Ata el avance por fotogramas (TAS): P pausa/reanuda, F avanza un campo de video con la
  // pausa puesta, Shift+F avanza ocho. Sin atar, las teclas no hacen nada.
  auto bindFrameAdvance(std::atomic<u32>* fields) -> void { taFields = fields; }

  // Ata el rebobinado: la tecla de retroceso pide pasos mientras se mantenga apretada. Sin
  // atar, no hace nada; con el rebobinado apagado en el nucleo, tampoco.
  auto bindRewind(std::atomic<u32>* req) -> void { rwReq = req; }

private:
  Memory* mem = nullptr;
  std::atomic<bool>* shutdown = nullptr;
  const std::atomic<double>* cpuPct = nullptr;    // HUD gauges (% of realtime)
  const std::atomic<double>* rspPct = nullptr;
  const std::atomic<double>* rdramPct = nullptr;
  std::string windowTitle = "kestrel64";   // set to the loaded ROM's internal name
  Vk* vk = nullptr;         // pimpl: all Vulkan/GLFW handles
  std::vector<u32> frame;   // scratch R8G8B8A8 buffer uploaded to the Vulkan image
  std::atomic<int>* stSave = nullptr;   // buzones de estado guardado (ver bindState)
  std::atomic<int>* stLoad = nullptr;
  std::atomic<int>* stSlot = nullptr;
  bool stPrev[3] = {};      // flanco de F5/F7/F6: la tecla se sondea, no llega como evento
  std::atomic<bool>* menuPaused = nullptr;   // ver bindMenu
  std::atomic<u32>* taFields = nullptr;      // ver bindFrameAdvance
  bool taPrev[2] = {};                       // flanco de P y F
  std::atomic<u32>* rwReq = nullptr;         // ver bindRewind
  std::string menuRom;
};

}  // namespace kestrel
