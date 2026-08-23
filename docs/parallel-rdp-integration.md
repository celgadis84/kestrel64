# paraLLEl-RDP integration (GPU RDP backend)

Goal: replace/augment the CPU SoftRDP with Themaister's paraLLEl-RDP (cycle-accurate,
GPU compute). Fixes SM64 title/effect corruption (SoftRDP accuracy gaps) AND offloads
the RDP raster from the CPU to the idle GPU (RX570), moving toward realtime — the
prerequisite for audible audio (waveOut underruns below realtime).

Toggle: `-DKESTREL_PRDP=ON` at configure; runtime `KESTREL_PRDP=1`. Default OFF so the
deterministic core (systemtest / lockstep md5) never depends on it.

## Status (2026-08-20) — VENDORED, BYTE ORDER FIXED, SM64 CORRECT ✅

Lo que hay hoy, corriendo:

- **Arbol vendorizado** en `third_party/parallel-rdp/` (commit upstream en
  `third_party/parallel-rdp/COMMIT` = `1cecd042b2619bc505c12bfdc713808386f2b54d`).
  `cmake/parallel_rdp.cmake` ya no apunta al checkout de ares — ares esta CONGELADO y
  ademas kestrel tiene que **parchear los shaders**, asi que el arbol vive aqui.
- **Orden de bytes de RDRAM resuelto** (era el arcoiris del fondo de SM64, no la ruta VI).
  parallel-rdp asume el almacenamiento de ares: palabras de 32 bits en orden nativo del
  host, o sea cada byte del guest en el indice `addr^3`. kestrel guarda RDRAM en el orden
  big-endian propio del N64. Convenio adoptado, en `shaders/small_types.h`:
  - la vista `vram8` NO lleva swizzle de indice (es la que ya coincide);
  - las vistas `vram16`/`vram32` caen sobre valores con los bytes al reves, y se pasan por
    `RDRAM_BSWAP16` / `RDRAM_BSWAP32` al leer y al escribir.
  Ficheros tocados: `small_types.h`, `memory_interfacing.h`, `extract_vram.comp`,
  `tmem_update.comp`. Los shaders de VI y el rasterizador no ven RDRAM cruda, no cambian.
- **Banco SPIR-V regenerado sin Granite.** Upstream hornea los shaders compilados en
  `shaders/slangmosh.hpp` con el `slangmosh` de Themaister, que no esta vendorizado.
  `tools/slangmosh_lite.py` lo reconstruye: lee el constructor existente para recuperar el
  orden exacto de shaders/variantes que emitio slangmosh, recompila cada permutacion con
  `glslangValidator` y parchea el par (offset, size) de cada `request_program`. El banco de
  reflexion se reutiliza tal cual porque los parches no tocan ningun binding.
  `python tools/slangmosh_lite.py --check` compila y dice que cambiaria, sin escribir.
  **Hay que ejecutarlo tras CUALQUIER edicion de un shader**; si no, el `.hpp` manda.
- **LIMITACION: upscaling NO soportado.** `update_upscaled_domain_post.comp` y
  `update_upscaled_domain_resolve.comp` no se convirtieron al convenio de bytes. Con
  upscale activado esos dos leerian RDRAM con el swizzle de ares y saldria basura. Si
  alguna vez se quiere 2x/4x, primero pasar esos dos por el mismo tratamiento.
- **Contrapresion del FIFO del RDP** — ver `docs/RDP-FIFO-BACKPRESSURE.md`. PRDP espera la
  timeline de la GPU en cada SYNC_FULL, asi que se retrasa lo bastante como para que el
  productor le saque frames de ventaja; eso destapo que la cola de trabajos del worker no
  estaba acotada y un `DPC_START` fresco dejaba spans viejos vivos -> interrupcion DP de
  mas -> el kernel del juego se cuelga. El escritor ahora hace `rdpDrain()` antes de
  instalar un START nuevo en modo threaded. El agujero existia igual en SoftRDP, solo que
  nunca se encolaba tan hondo.
- SM64 arranca y renderiza correcto con `KESTREL_PRDP=1` en interp y en JIT (300 y 400
  cambios de buffer sin cuelgue).

Pendiente conocido: en corridas largas parallel-rdp escupe
`[WARN]: Even after garbage collection, we will exceed budget` y
`[WARN]: Exhausted LinkedDeviceHost memory, falling back to host.` — sin investigar.

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

---

## Known divergence: the COMBINED pipeline register (krom GRB decoders)

**Symptom.** With `KESTREL_PRDP=1` the three ROMs `GRB12/GRB15/GRB24 Decode`
go from 100.00 to 0.00 against the hardware capture: the fill rectangle is
painted, but not one of the 27 texture rectangles shows up. Under SoftRDP the
same ROMs are pixel-exact.

**It is not** the FIFO parse, the TLUT, the address masking or the tile upload.
Ruled out one by one: all 200 DP commands reach `enqueue_command`, clearing
`EN_TLUT` in the ROM leaves the image just as black under PRDP while SoftRDP
starts drawing raw CI4 nibbles, and parallel-rdp's own log has no `LOGE`.

**Root cause.** These decoders program the combiner as

```
cycle1 RGB   = (TEXEL0 - 0) * COMBINED_ALPHA + 0
cycle1 ALPHA = (TEXEL0_A - 0) * LOD_FRACTION + 0
```

and run it in 1-cycle mode, where the RDP evaluates the *second* cycle's
equation. `COMBINED` on the RDP is a pipeline register: it is not zeroed per
pixel, it still holds the previous pixel's combiner output. In steady state
that alpha is 1.0, so `TEXEL0 * COMBINED_ALPHA` is just `TEXEL0` — which is
exactly what krom is counting on, and what the hardware capture shows.
`src/rdp/rdp.cpp` models it that way (`combined[]` persists across pixels).

parallel-rdp instead feeds a literal zero, in `shaders/shading.h`:

```glsl
CombinerInputs(derived.constant_muladd1, ..., shade, u8x4(0), texel0, texel1, ...)
                                                     ^^^^^^^ COMBINED
```

so the multiply collapses to zero and every texel comes out transparent black.

**Why we are not "fixing" it.** The zero is not an oversight, it is forced by
the architecture. parallel-rdp shades pixels in parallel on the GPU; "the
previous pixel" does not exist there, and there is no cheap way to serialise a
combiner-wide dependency without giving up the parallelism that is the whole
point of the backend. Any workaround (iterating the cycle twice to chase the
fixed point, pinning COMBINED_ALPHA to 1.0) would be a guess dressed up as
hardware, i.e. exactly the hardcoding the hard rules forbid.

Recorded as a **known, accepted divergence**: 3 ROMs out of 371, all three
synthetic video decoders that deliberately exploit a sequential quirk. The PRDP
sweep is still a net win overall (mean_exact 88.95 vs 88.71, perfect 178 vs
148). Anything that samples COMBINED/COMBINED_ALPHA in the first cycle will
read zero under PRDP — worth remembering if a real game ever looks black in
one pass.
