# kestrel64 — Reconstrucción arquitectura multihilo (RCP async)

Objetivo (mandato usuario, 2026-08-10): reconstruir el core a multihilo e implementar
todo RSP / RDP / VI / input de forma HW-fiel. Luego optimizar intérprete, luego dynarec,
y SOLO entonces volver a PD. Restricción dura: **systemtest 0/3721 (Base/Timing/Cycle)
intacto**, y cada cambio = semántica VR4300/RCP genuina, nunca hack para pasar el rom.

---

## 1. Estado actual (single-thread core)

Hilos SO hoy:
1. **main** → GLFW/Vulkan present loop (`runLoop`→`presenter.pumpFrame`).
2. **cpu worker** → `System::run` → batches `stepCpu(750000)` + `viTick`, bajo `coreMutex`.
3. **telemetry** → MCP; toma `coreMutex` para tocar estado.

Dentro de `stepCpu` (UN hilo, secuencial):
- CPU step (interp o bloque JIT).
- RSP interleaved 2:3 inline (`memory.rsp.step(1)` cada CPU insn).
- **RDP corre SÍNCRONO dentro del store CPU** a DPC_END (`softRdp.run` desde `mmioWrite32`).
- VI ticked 1×/batch.

### Puntos de sincronización (contrato HW)
| Evento | Disparo | Efecto | Visibilidad requerida |
|--------|---------|--------|----------------------|
| RSP start | store SP_STATUS clear-HALT | `rsp.start()`; interleave step | DMEM/IMEM ya DMA'd antes |
| RSP done | RSP alcanza BREAK | `sp_status`\|=BROKE, raise MI_SP | writes DMEM/RDRAM **antes** de BROKE/MI_SP |
| RDP kick | store DPC_END | mastica FIFO [current,end) | commands en RDRAM ya escritos |
| RDP done | SYNC_FULL en FIFO | clear PIPE_BUSY/GCLK, raise MI_DP | pixels en RDRAM **antes** de MI_DP |
| VI field | `viTick` por batch | vi_current, raise MI_VI | framebuffer coherente para scanout |
| MI | `mi_intr & mi_mask` | IP2 al CPU | RMW mi_intr atómico entre hilos |

Contrato clave: el juego **espera el interrupt** (MI_SP / MI_DP / MI_VI) antes de tocar el
buffer que el RCP produjo. Por tanto basta **release** en el productor (RSP/RDP) al subir el
interrupt y **acquire** en el consumidor (CPU al leer mi_intr / status). No hace falta
coherencia por-píxel: HW N64 tampoco la da.

---

## 2. Diseño destino

Dos modos, seleccionables en runtime (`System::rcpMode`):

- **lockstep** (default; systemtest y referencia determinista): comportamiento actual exacto.
  RSP interleaved 2:3 inline, RDP síncrono en DPC_END. Garantiza 0/3721 por construcción.
- **threaded** (`KESTREL_THREADS=1`; PD/gameplay/perf): RSP y RDP en hilos SO propios,
  handshake productor/consumidor. Mismos *resultados*, distinto *timing*.

El modo threaded NO es un atajo: debe producir resultados idénticos al lockstep. El lockstep
es la referencia determinista contra la que se difyea (modo-diff).

### 2.1 RDP thread (primer objetivo — más independiente)
RDP solo lee FIFO de RDRAM y escribe píxeles a RDRAM; único handshake con CPU = MI_DP en
SYNC_FULL. En HW DPC_END solo bumpea puntero; el RDP mastica async.

- Cola SPSC de jobs `{current, end, xbus}`. DPC_END push + set GCLK/PIPE_BUSY, `dpc_current=end`.
- Hilo RDP: pop job → `softRdp.run` → si SYNC_FULL: clear GCLK/PIPE_BUSY, `raiseIntr(MI_DP)`
  (con release tras los writes de píxeles).
- DPC_STATUS read: PIPE_BUSY refleja `cola no vacía || job en curso` (HW: RDP ocupado).
- Drenado determinista: en lockstep, drena la cola inline (idéntico a hoy).

Race: `mi_intr` RMW (RDP raise vs CPU clear) → `std::atomic<u32>` fetch_or/fetch_and.
`rcp.dpc_status` flags → atómico o publicados con el mismo lock ligero.

### 2.2 RSP thread (segundo)
Más delicado: microcode hace ping-pong SIGNAL con CPU mid-task (SP_STATUS SIG bits).
- `sp_status` → atómico; SIG bits set/clear visibles cross-thread (spin natural = HW).
- Start: CPU clear-HALT → arma hilo RSP (no ejecuta inline). RSP corre hasta BREAK.
- BREAK: publica DMEM/RDRAM (release) → set BROKE + raise MI_SP.
- systemtest en lockstep (default) → sin cambio. Threaded valida con krom RSP roms.

### 2.3 VI / input
- VI ya existe (`viTick`). En threaded, VI timing lo lleva su propio tick pace (o el present
  loop), desacoplado del batch CPU.
- Input: hoy solo `KESTREL_BUTTONS` OR'd en joybus. Falta: teclado GLFW → PIF/SI joybus real
  (mapa botones N64), lectura por `pifProcessJoybus`. Implementar en present thread → escribe
  `padButtons` atómico leído por SI DMA.

---

## 3. Orden de trabajo (incremental, 0/3721 tras cada paso)
1. [este doc] mapa sync + diseño.
2. RDP async detrás de `rcpMode` (lockstep default). Validar systemtest 0/3721 + krom Triangle.
3. Input real (teclado→joybus). Validar systemtest 0/3721.
4. RSP thread threaded-mode. Validar systemtest 0/3721 + krom RSP.
5. RDP: completar shade Gouraud + textura + Z (necesario para 3D real; krom = oráculo).
6. Optimizar intérprete al máximo.
7. Dynarec block-linking.
8. Volver a PD (solo cuando el usuario lo ordene).

## 4. Estrategia determinismo (por qué 0/3721 sobrevive)
- systemtest corre en **lockstep default** → ruta idéntica a la validada 0/3721.
- Threaded solo se activa con env → no afecta la suite.
- Threaded es correcto por handshake release/acquire, no por hack; difyeable contra lockstep.
