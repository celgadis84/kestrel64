# paraLLEl-RDP integration (GPU RDP backend)

Goal: replace/augment the CPU SoftRDP with Themaister's paraLLEl-RDP (cycle-accurate,
GPU compute). Fixes SM64 title/effect corruption (SoftRDP accuracy gaps) AND offloads
the RDP raster from the CPU to the idle GPU (RX570), moving toward realtime — the
prerequisite for audible audio (waveOut underruns below realtime).

Toggle: `-DKESTREL_PRDP=ON` at configure; runtime `KESTREL_PRDP=1`. Default OFF so the
deterministic core (systemtest / lockstep md5) never depends on it.

## Status (2026-08-13) — WIRED + FIRST LIGHT ✅

paraLLEl-RDP is now fully wired and renders SM64. See the "Wiring done" section below; the
older "build/link derisked" notes follow it.

### Wiring done — SM64 first light

- **Runs, no crash, single-thread Granite.** `KESTREL_PRDP=1 KESTREL_VIDEO=1` boots SM64;
  resumed over MCP it reaches the title at ~15% realtime CPU with the RX570 doing the raster.
- **SM64 title: Mario's head renders CORRECTLY** (hat/eyes/skin/mustache) where SoftRDP was a
  blown-out white/pink mess at the same point (~380M insns). Background is still rainbow noise
  — a remaining accuracy/VI-config item, but the head proves the pipeline is sound. Reference
  dumps: `tools/mcp/p_prdp.png` (PRDP) vs `tools/mcp/s_soft.png` (SoftRDP).
- **Determinism preserved.** Default build (PRDP OFF): systemtest interp **0/3721·0/2·0/6**,
  JIT **0/3721·0/2·0/6**; SM64-300M Lockstep==Threaded framebuffer md5 identical
  (`cbf8aa761b92adab89ddde949b6ff24b`). The core edits (memory.cpp rdpRunJob routing, VI-write
  forward, viTick frameBegin) are inline no-ops when `!KESTREL_PRDP`, so the SoftRDP path is
  byte-identical.

### Three bugs fixed to get here (all runtime, all from linking volk into the exe)

1. **DEP fault at `vkCreateInstance`.** present.cpp included Vulkan **prototypes** (via
   `GLFW_INCLUDE_VULKAN`) so it treated `vkCreateInstance` as a *function* and jumped to the
   symbol's address. But volk *defines* those symbols as **function-pointer variables** — so the
   call executed the pointer's storage bytes → DEP violation. FIX: under `KESTREL_PRDP`,
   present.cpp defines `VK_NO_PROTOTYPES` and includes `<volk.h>` (pointer decls that match the
   linked volk globals), then calls `volkInitialize()` + `volkLoadInstance()` + `volkLoadDevice()`
   so the pointers are populated before use. Diagnosed with lldb + a `%p` print showing
   `vkCreateInstance` == its own storage address, not a loaded PFN.
2. **"Thread does not exist in thread manager" spam + crash.** Granite is single-thread. We were
   calling it from THREE threads: runFifo/init (RDP worker), viWrite/frameBegin (CPU), scanout
   (main). FIX: the RDP worker is the SOLE Granite driver. viWrite/frameBegin only stash state
   in atomics; the RDP worker applies pending VI regs + rotates the frame context inside runFifo
   and produces the scanout RGBA into a mutex-guarded buffer right after SYNC_FULL; present just
   copies that buffer. `Util::register_thread_index(0)` on the RDP thread in `init`. Also removed
   `GRANITE_VULKAN_MT` from the cmake (ares builds without it and drives from one thread — same).
3. **`project(kestrel64 CXX)` dropped volk.c** (fixed earlier): needs `CXX C`.

### Remaining

- Background rainbow noise on the SM64 title (VI dedither/divot or unrendered-region readback?).
  Mario head is correct, so it's a VI-filter/clear-path detail, not a pipeline break.
- Two-Vulkan-device coexistence (present's device + Granite's device) works today because both
  land on the same RX570 driver; if a game needs RDRAM framebuffer readback, wire
  `begin_read_hidden_rdram`. Longer term: unify on one device / one WSI (ares' model).

## Status (2026-08-13, earlier) — build/link derisked

- **Build + link DERISKED.** `cmake/parallel_rdp.cmake` builds the full Granite Vulkan +
  RDP stack as `libparallel_rdp.a` (3.3 MB) under clang64 — ZERO errors. Shaders are
  pre-baked into `shaders/slangmosh.hpp` (embedded SPIR-V), so no shader compiler is needed
  at build time. volk loads `vulkan-1.dll` at runtime.
- **Glue compiles + links.** `src/vrdp/vrdp.{hpp,cpp}` builds and the whole `kestrel64.exe`
  links with `-DKESTREL_PRDP=ON` (3.1 MB). volk (parallel-rdp's loader) and the import lib
  `libvulkan-1.dll.a` (present.cpp's Vulkan) COEXIST with no duplicate-symbol clash.
- **Two link gotchas fixed:** (1) `project(kestrel64 CXX)` → `CXX C`; without C enabled,
  CMake silently dropped `volk.c` → undefined `volkLoad*`/EXT symbols. (2) `PRDP_DIR` is the
  OUTER tree (holds parallel-rdp/, util/, volk/, vulkan/ side by side), not the inner core dir.
- **Default build UNAFFECTED** (verified 2026-08-13): with KESTREL_PRDP OFF, vrdp.cpp isn't
  compiled and no flags change; systemtest interp **0/3721·0/2·0/6**, JIT **0/3721·0/2·0/6**.
  Lockstep==threaded md5 unaffected (no core source touched).
- Source lives in the ares checkout for now: `PRDP_DIR` defaults to
  `E:/Claude/N64/ares-64/ares/n64/vulkan/parallel-rdp`. Vendor into kestrel/third_party later.
- **Wiring: NOT yet done** (the remaining work — see Next steps). At runtime KESTREL_PRDP=1
  currently does nothing because `vrdp::init` is called nowhere yet.

## Reference interface (from ares' vulkan.cpp — the canonical driver)

`::RDP::CommandProcessor` (rdp_device.hpp):
- ctor: `CommandProcessor(device, rdram_ptr, rdram_offset, rdram_size, hidden_size, flags)`
  — reads guest RDRAM directly from a host pointer. Pass `mem->rdram.data()`, size,
  `hidden = size/2`.
- `device_is_supported()` → validate after ctor.
- `enqueue_command(u32 count_words, const u32* words)` — RDP commands as **u32 pairs,
  byte-swapped** (hi word then lo word per 64-bit command). Only enqueue opcodes `>= 8`.
- On opcode `0x29` (SYNC_FULL): `wait_for_timeline(signal_timeline())` then signal DP.
- `set_vi_register(::RDP::VIRegister(idx), data)` — feed VI regs (idx = reg byte offset/4).
- `begin_frame_context()` once per emulated frame.
- `scanout_async_buffer(VIScanoutBuffer&, ScanoutOptions&)` → async; then
  `scanout.fence->wait()` and `device.map_host_buffer(*scanout.buffer, READ)` gives RGBA8888
  + width/height. This REPLACES reading RDRAM `vi_origin` for display.
- Needs its own `::Vulkan::Context` + `::Vulkan::Device` (Granite), `init_frame_contexts(3)`.

Command-length table (indexed by `op>>24 & 63`), for splitting the FIFO into commands:
```
1,1,1,1,1,1,1,1, 4,6,12,14,12,14,20,22,  1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1,
1,1,1,1,2,2,1,1, 1,1,1,1,1,1,1,1,        1,1,1,1,1,1,1,1, 1,1,1,1,1,1,1,1
```

## Hook sites in kestrel

- **DP command FIFO**: `memory.cpp:~1094 rdpRunJob` calls `softRdp.run(*this,current,end,xbus)`
  and gates `raiseIntr(MI_DP)` on `softRdp.sawSyncFull`. This is where PRDP's
  `enqueue_command` loop goes (mirror ares render(): read command words from RDRAM — or
  DMEM when xbus — split by length table, enqueue op>=8, handle SYNC_FULL). One-owner while
  running, same as SoftRDP.
- **VI registers**: `memory.cpp` VI write switch (`case BASE_VI...`, ~0x768 vi_origin etc.).
  Forward each VI reg write to `vrdpViWrite(regIndex, value)` when PRDP active.
- **Present / scanout**: `present.cpp:481` currently reads RDRAM at `vi_origin`. When PRDP
  active, instead pull the scanout RGBA from `scanout_async_buffer` map and hand that to
  `presentFrame(v, rgba, w, h)`.
- **Frame boundary**: call `begin_frame_context()` per VI field (present.cpp field loop).

## Glue plan (src/vrdp/vrdp.{hpp,cpp})

Thin C++ wrapper hiding Granite behind kestrel types (pimpl, like ares' Vulkan struct):
```
namespace kestrel::vrdp {
  bool init(u8* rdram, u32 size);      // build Context/Device/CommandProcessor; false if no GPU
  void shutdown();
  bool active();                        // init succeeded and KESTREL_PRDP=1
  void enqueue(const u32* words, u32 count);
  void syncFull();                      // wait_for_timeline + (caller raises DP)
  void viWrite(u32 index, u32 value);
  void frameBegin();
  const u8* scanout(u32& w, u32& h);    // RGBA8888 for present; nullptr if none
  void scanoutDone();
}
```
The DP-FIFO splitter (length table + xbus DMEM vs RDRAM fetch) lives in the glue so the
core just calls `vrdp::enqueue`-per-command or hands the raw range in.

## Determinism guard

PRDP is a *display/raster* path only; it reads RDRAM and produces host pixels. It must NOT
feed anything back into the deterministic core state. Keep systemtest / lockstep md5 on the
SoftRDP path (PRDP OFF). PRDP is opt-in for interactive play. NOTE: some games read back the
RDP framebuffer from RDRAM (CPU reads rendered pixels) — SoftRDP writes RDRAM so those work;
PRDP keeps pixels in VRAM. If a title needs RDRAM readback, PRDP exposes hidden-RDRAM sync
(`begin_read_hidden_rdram`) — wire only if a game needs it.

## Next steps (ordered)

1. Write `src/vrdp/vrdp.{hpp,cpp}` glue (pimpl over CommandProcessor). Compile-link with
   the main exe under `KESTREL_PRDP` (add sources + `target_link_libraries(... parallel_rdp)`).
2. Wire the DP-FIFO splitter → `enqueue` in the `rdpRunJob` path, gated on `vrdp::active()`.
3. Forward VI reg writes → `vrdp::viWrite`; call `frameBegin()` per field.
4. Present from `vrdp::scanout()` when active; else current RDRAM path.
5. Bring up SM64: expect the title/Bowser-demo corruption to vanish and CPU load to drop.
6. Measure fps delta; re-check audio underrun margin.
7. Later: vendor source into third_party/; expose upscale (2x/4x) toggle.
```
```
