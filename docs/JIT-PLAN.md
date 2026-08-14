# kestrel64 — Plan del recompilador (dynarec) y optimización del intérprete

Objetivo: **100% de velocidad N64** (medido por `speed.cpuPct`, 100 = tiempo real).
Precisión NO negociable: `n64-systemtest` debe seguir en **0/3721** en cada paso.

## Estado / medidas (host i7-870, ~2.93–3.6 GHz, single-thread débil, 2009)

| Build | PD (MIPS) | CPU % realtime | Notas |
|-------|-----------|----------------|-------|
| Baseline intérprete | 16.5 | ~18% | ~180 ciclos host / instr emulada |
| + prólogo debug gated (`debugArmed`) | 19.2 | ~20.5% | +16% |
| + `stepTraps()` fría (noinline) | 19.6 | ~21% | mejor I-cache host |
| + jump-log gated bajo debug | ~20.0 | ~21.5% | +21% total vs baseline |
| + interrupt-poll inline + delivery frío | ~19.7 | ~21% | neutral (call ya barato) |
| + reserved64 bitmask (mata 2º switch) | ~19.7 | ~21% | neutral (clang ya fusionaba) |

Todos con `0/3721` intacto. `-O3 + -march=native + thin-LTO`.

**Techo del intérprete alcanzado en ~21%.** Los micro-opts posteriores al +21% son
lossless pero **neutrales** — el coste no está donde parecía.

## Diagnóstico DEFINITIVO (medido, no teórico)

Experimentos A/B/C de bisección del coste por-instrucción:

| Experimento | Qué quita | Ganancia |
|-------------|-----------|----------|
| A: throttle interrupt-poll 1/32 | `checkInterrupts` 31/32 | **+5%** (pero rompe modelo Timing → descartado) |
| B: skip fetch (translate+icFetch inline) | máquina de fetch | **0%** |
| C: skip D-cache sim (RDRAM directo) | index/tag/fill de dcache | **0%** |

Quitar fetch = 0. Quitar dcache = 0. Aislar ALU = 0. Pero el agregado son **~150
ciclos host / instr emulada** (un intérprete "normal" hace 30–50). Cuando quitar cada
pieza da cero pero el total es enorme, el coste **no está en ninguna operación
concreta**: está en el **thrash de I-cache del host**.

`execute()` son ~3000 líneas; `special()`, `cop1op()`, `regimm()`, `cop0op()` son
funciones gigantes aparte. Cada instrucción emulada recorre `step() → execute() →
special()/…`, un working-set de código caliente que **desborda la L1I del host (32 KB)**.
El branch indirecto del switch + las rutas frías intercaladas contaminan la caché. Por
eso quitar operaciones sueltas no mueve la aguja: el cuello es la localidad de código,
no el trabajo aritmético.

**Corolario**: la Etapa 1 original (block-cache que solo salta fetch/decode) daría poco
— B ya demostró que saltar fetch = 0%. El único cambio arquitectónico que ataca el
thrash de I-cache es **codegen**: cada bloque se compila a código host **recto y
diminuto** que contiene exactamente las ops de ese bloque, sin switch, sin funciones
gigantes, con los gpr calientes en registros host. Eso colapsa el working-set de código
por bloque a decenas de bytes → I-cache del host feliz → 5–15×.

## Estrategia: dynarec por etapas, intérprete como oráculo/fallback

Regla de oro: **cada etapa detrás de flag** (`KESTREL_JIT`), fallback al intérprete
para cualquier opcode/situación no cubierta. `0/3721` se mantiene por construcción
(flag OFF = intérprete actual, byte-idéntico). El JIT se valida contra el intérprete
diffando estado (regs/mem) bloque a bloque antes de activarlo por defecto.

### Etapa 0 — micro-opt intérprete (HECHO, +21%, techo alcanzado)
Gating de debug + función fría. Sin riesgo, en árbol. **No queda win barato.**

### Etapa 1 — Infra de code-cache + emisor x86-64 (SIGUIENTE, cimiento)
No acelera todavía; monta la maquinaria verificable en aislado:
- `CodeBuffer`: región RWX (Windows `VirtualAlloc MEM_COMMIT | PAGE_EXECUTE_READWRITE`,
  POSIX `mmap PROT_EXEC`), bump-allocator, `FlushInstructionCache`.
- `Emitter`: helpers x86-64 (REX/ModRM/SIB), mov reg↔[mem], add/sub/and/or/xor/shift
  imm y reg, cmp, jcc, call, ret. Convención Win64 (rcx,rdx,r8,r9; rax retorno).
- `Block { u32 phys; u8* code; u16 guestLen; u16 hostLen; }`, cache por dirección
  física en hash-map + tabla de páginas para invalidación.
- **Self-test**: emitir en runtime una función que sume dos slots de memoria y
  ejecutarla; comparar con el resultado C. Prueba el emisor antes de tocar el hot loop.

### Etapa 2 — Codegen por bloque (el salto de velocidad)
- Un *bloque* = instrucciones desde una PC física hasta (incl.) el delay-slot del primer
  branch/jump, sin cruzar página 4K. `translate(pc)` una vez por bloque.
- Emitir x86-64 para el subconjunto caliente: ALU imm/reg, shifts, LUI, moves, slt,
  loads/stores a RDRAM cacheable (fast-path inline: calcular phys, XOR endianness,
  acceder al array RDRAM directo), branches/jumps. **Fallback a handler C++** (llamar al
  intérprete para esa op) para el resto: COP1, CACHE, TLB, ops raras, MMIO/uncached.
  Un JIT parcial que cubra el ~90% de ejecutadas ya da casi todo el win.
- Reg-alloc simple: mapear gpr calientes del bloque a registros host callee-saved
  (rbx,rsi,rdi,r12-r15); prólogo carga de `gpr[]`, epílogo hace spill. Sin esto, un JIT
  "memory-operand" (gpr en RAM) ya mata dispatch+I-thrash pero deja el tráfico de regs;
  medir ambos.
- Delay slots: emitir el slot antes de materializar el salto (como pc/nextPc del intér.).
- Excepciones: el bloque comprueba fallos (align/TLB/overflow) y **sale al manejador C++**
  con `curPc`/EPC exactos. Count/Compare y el sample de interrupt: por-instrucción DENTRO
  del bloque (no hoistar) hasta validar Timing/Cycle.
- Ganancia esperada: 5–15× → **100%+ realtime**.

### Etapa 2a — HECHO (2026-08-09): ALU-solo-KSEG0, byte-exacto pero net-negativo
Cableado + validado (systemtest 0/3721 nativo y diff, 2 bugs HW arreglados: SRA/SRAV 64b,
guarda KSEG0 64b). Ver `STATUS.md`. **NO acelera PD** por dos causas MEDIDAS que redefinen
la Etapa 2b:

1. **PD corre desde TLB-map 0x0000000070xxxxxx, NO KSEG0.** `KESTREL_JIT_STATS` = cover
   0.0%. La entrada de bloque calcula phys como `(u32)pc & 0x1FFFFFFF` (solo KSEG0/1
   directo). Para cubrir PD hay que **traducir pc→phys por el TLB en la entrada del
   bloque**, con un *probe sin efectos* (replica el match de `tlbLookup` SIN vectorizar
   excepción; devuelve phys o sentinela "no-map" → declina). Invalidar cache de bloques en
   `tlbWrite`/cambio de ASID.
2. **avgK≈2.11.** Runs ALU puros son diminutos; el overhead por bloque no se amortiza. Hay
   que compilar **basic-blocks reales** (hasta el delay-slot del primer branch), lo que
   obliga a meter loads/stores + branches (no solo ALU).

### Etapa 2b — el salto de velocidad real (redefinida por lo medido)
- **Entrada TLB-mapeada**: probe `tlbProbe(vaddr)` sin efectos → phys+cacheable o miss.
  Solo compilar si mapea a RDRAM cacheable. Clave de cache = phys.
- **Loads/stores a RDRAM cacheable**: emitir *llamada a helper C++* (`dcRead/dcWrite` vía
  `read32/write32`) por op de memoria. Tras cada llamada, emitir check de `memAbort` → si
  set, **salir del bloque con `curPc` exacto** en esa op (checkpoint de pc por-op) para que
  el intérprete la re-ejecute y faulte igual. Nada de commit del resultado en abort.
- **Branches terminan bloque**: emitir la condición + delay-slot, escribir `nextPc`, salir.
- **Checkpoint de pc por-op**: el bloque avanza `pc` incrementalmente (o guarda índice) para
  que cualquier fault (align/TLB/overflow) salga con EPC exacto.
- **Block linking** (Etapa 3 abajo) para no volver al despachador entre bloques.
- Todo bajo modo-diff antes de activar. Riesgo alto al oracle → incremental, gated.

### Etapa 3 — Refinos
- Block linking (encadenar bloques sin volver al despachador).
- Idle-loop detection (spin de VI-wait → saltar trabajo inútil).
- Hoist de interrupt-check a límites de bloque **solo** si Timing/Cycle siguen 0/N
  (con contador de ciclos por bloque).

## Overclock por región (feature ya cimentada)
`System::Clocks` tiene `cpuOc/rspOc/rdramOc`. El medidor `speed.*Pct` mide contra
`hz*oc`. Cuando el dynarec suba el techo, exponer estos multiplicadores permite
"overclockear" CPU/RSP/RDRAM por separado y ver el % por dominio.

## Riesgos
- El JIT debe clonar EXACTO el intérprete (mismo orden de efectos, mismas excepciones).
  Mitigación: modo diff (ejecuta ambos, compara estado) hasta confianza.
- SMC / DMA a páginas de código: invalidación robusta obligatoria (tabla de páginas).
- Timing preciso (systemtest Timing/Cycle): no hoistar Count/interrupts hasta validar.
- W^X en Windows: buffer RWX simple al principio; endurecer (RW→RX flip) más tarde.

## Etapa 2b — DONE (loads/stores en bloque, correcto, sin speedup)

- Bloques ahora compilan loads/stores alineados simples vía helper C `jitMem` (call + test al + je bail).
- Bail limpio: `probing` translate side-effect-free decide; retiro parcial por índice; intérprete levanta la excepción exacta. Gate 64b-reservado (LWU/LD/SD RI en no-kernel) espejado.
- Oráculo: **0/3721** con JIT ON (base/timing/cycle), nativo y diff. avgK 1.1→3.2.
- Perf: JIT ON **más lento** (13.5 vs 19.4 Mips). Impuestos medidos: probe TLB/instr 2.9, dispatch 2.1, sample 0.7. Ejecución de bloque NEUTRAL (mem-ops llaman helper C = re-translate).
- **JIT default-OFF.** Recorte de overhead insuficiente (techo ~16.6 < 19.4).

## Etapa 2c/3 — el speedup real (pendiente, build grande)

1. **softTLB**: cache guest-vpn→host-ptr (invalida por TLB write / ASID / modo). Mata el probe 2.9 y el re-translate de mem-ops.
2. **Fast-path RDRAM inline en x86**: cacheable + in-range → índice directo sobre puntero host de rdram, sin call C, sin `translate`. Slow-path (MMIO/cart/TLB-miss) cae a `jitMem`.
3. **Branches + delay-slot dentro del bloque**: baja la tasa de decline del 58% (hoy corta en cada branch/jr/cop1/mult-div).
4. **Cache de traducción por PC de entrada** + link de bloques + validación SMC barata.

## Etapa 2c-parcial — softTLB HECHO + RAÍZ colapso cobertura (2026-08-09)

- **softTLB entrada** (cpu.hpp `jitTlb*`): cache 1-entrada vpn→phys, tag+ASID, invalida en tlbWrite. Corta el scan TLB de 32 entradas por instr. **0/3721** con JIT ON. Lossless.
- **A/B mismo build**: intérprete **16.2 Mips** vs JIT ON ~12–14 y bajando. `KESTREL_JIT_STATS`: cover 48%→~0% y blocksRun/opsJIT CONGELADOS mientras calls sube → jitTryBlock declina 100% en la fase steady.
- **RAÍZ**: game-loop de PD dominado por branches/jumps; el JIT recto corta en el 1er branch → 0% cobertura en el hot-loop real. El techo NO es el probe (ya cortado) sino la **cobertura**.

### Orden de ataque revisado (dominante primero)
1. **Branches + delay-slot en bloque** (salidas laterales por condición, delay-slot inline). *Palanca dominante para PD* — sin esto el dynarec no toca el game-loop. Mantener bail limpio: en el edge de branch tomado, salir del bloque devolviendo ops retiradas + fijar pc/nextPc al destino; el intérprete NO re-ejecuta (a diferencia del bail de mem).
2. **Fast-path RDRAM inline** en el emisor: cacheable+in-range → índice directo sobre puntero host de rdram (reXor para <8B), sin `call jitMem`. Slow-path (MMIO/cart/TLB-miss/misalign) cae al helper C actual.
3. Link de bloques + validación SMC por-bloque (no `clear()` global) + cache traducción por PC.

## Etapa 2c — Branches-en-bloque HECHO + speedup real (2026-08-09)

- **BEQ/BNE + delay-slot absorbidos en el bloque.** El bloque evalúa la condición ANTES del
  delay-slot (setcc→[rsp+32]), compila el delay-slot (ALU o mem), y escribe pc/nextPc/inDelay/
  justBranched directamente vía cmov (target vs fallthrough); devuelve flag `0x80000000|nOps`.
  El driver NO toca el control de flujo cuando ve el flag (a diferencia del bail de mem, aquí
  no se re-ejecuta). `Ctaken=4*(idx+1)+SIMM*4`, `Cfall=4*(idx+2)`, idx=rectas antes del branch.
- **avgK 3.4→5.54**; `ctrl`=7.1M salidas-branch/run. Cobertura del game-loop ya no colapsa.
- **BUG RAÍZ (branch-en-bloque regresión → RI infinito en boot):** el helper de mem-op no
  preservaba **R12** de forma fiable a través de la cadena `call jitMemThunk→jitMem→...`; tras
  el CALL, R12 (que apuntaba a `cpu`) quedaba corrupto con un delta dependiente del valor. Dos
  daños: (a) part2 del branch escribía pc/nextPc en `gpr[]` en vez de `cpu->pc` (bloque dejaba
  pc sin cambiar → deriva salvaje → RI); (b) la 2ª mem-op del bloque pasaba un `cpu` corrupto a
  jitMemThunk → store mal-direccionado corrompía la copia RDRAM→DMEM del IPL → salto a código
  basura. **Fix genérico:** direccionar `cpu` vía **RBX** (== `&gpr[0]` == `cpu`, porque `gpr`
  es el primer miembro de CPU; `static_assert(offsetof(CPU,gpr)==0)`), eliminando la dependencia
  de R12. Tanto part2 como emitMemOp usan RBX ahora. **0/3721** con JIT ON (base/timing/cycle).
- **Perf PD (pd-consolidated, host i7-870):** intérprete ~23 Mips → **JIT ON ~32 Mips (+~40%)**;
  misma ventana 35s: 601M insns (OFF) vs 965M (ON). **Primer build donde el dynarec gana.**
- Diagnóstico sano: `KESTREL_JIT_BRDIFF` valida bloques hasBranch pure-ALU vs intérprete (gpr+
  pc+nextPc); los bloques con memoria se validan por el oráculo systemtest (un diff con mem daría
  falso positivo al aliasar store→load re-ejecutado). `KESTREL_JIT_NOBRANCH` = kill-switch.

## Etapa 2c-b — Saltos absorbidos + SMC inline (2026-08-09)

- **J / JAL / JR / JALR absorbidos** (además de BEQ/BNE). Todos comparten la maquinaria de
  delay-slot y la cola de salida de control (`emitCtrlExit`: target en RCX → pc/nextPc/flags/
  flag `0x80000000|(idx+2)`). Diferencias por tipo:
  - **J/JAL**: target ESTÁTICO `= (entryVA & 0xFFFFFFFF_F0000000) | (TARGET26<<2)`, computado
    en runtime desde `cpu->pc` (RBX) — no se hornea la VA como constante (el mismo phys puede
    mapearse a distintas VA vía TLB). JAL enlaza `gpr[31]=sext32(entryVA+4*(idx+2))` ANTES del
    delay slot (en HW el delay slot ya ve el nuevo `$ra`).
  - **JR/JALR**: target `= gpr[rs]` capturado ANTES del delay slot a `[rsp+32]` (8 bytes, sobre
    el shadow-space de 32B → sobrevive al CALL de un delay-slot mem). Nuevos helpers `st64_rsp`/
    `ld64_rsp`. JALR enlaza `gpr[rd?rd:31]` tras leer rs (rd puede ==rs, target ya en stack).
  - Enlace idempotente si el delay-slot faulta: bail re-ejecuta el salto → mismo `$ra`.
- **Loop de validación SMC inlineado** (era `K×icFetch` cross-TU): mismo comportamiento
  byte-a-byte (misma fill en miss, misma extracción big-endian, compara contra la LÍNEA I-cache
  no rdram directo — en HW la CPU corre código stale de I-cache si un DMA reescribe sin
  invalidar). Nuevos helpers emitter `and_r_r`/`or_r_r`.
- **Perf PD (misma ventana 25s, runs consistentes):** intérprete 533M insns / 21.2 Mips →
  JIT BEQ-solo 663M / 26.4 (1.24x) → **JIT full 742M / 29.5 (1.39x)**. Jumps = **+12%** limpio
  sobre BEQ-solo (A/B con `KESTREL_JIT_NOJMP`); SMC-inline ~+5%. **0/3721** intacto.
- avgK BAJA 5.54→~2.5: no es regresión — ahora capturamos bloques de retorno (JR `$ra`, 2 ops)
  que antes declinaban 100% a intérprete. El cuello se desplaza al **overhead del driver por
  bloque** (guards+tlb+SMC+dispatch cada ~2.5 ops) → lo ataca el block-linking.
- Diagnósticos: `KESTREL_JIT_NOJMP` (A/B solo-saltos), `KESTREL_JIT_NOSMC` (mide techo del loop
  SMC; UNSAFE default-off — rompe detección SMC).

### Pendiente (siguiente palanca, por impacto)
1. **Block linking** (bloque→bloque sin volver al driver) — AHORA el cuello: avgK ~2.5 paga el
   overhead del driver demasiado seguido. Enlazar salida de control → entrada del bloque destino
   (patch del `jmp` de salida cuando el destino ya está compilado). Requiere validación SMC
   por-bloque (no `clear()` global) para poder invalidar un enlace sin tirar toda la cache.

## Block linking — diseño de implementación (2026-08-13, derivado del driver real)

Estado leído: `jitTryBlock()` corre un bloque, y en ctrl-exit el bloque ya escribió
`pc/nextPc/inDelay/justBranched` y devolvió `0x80000000|nOps`; el driver solo avanza contadores
y **retorna al run-loop, que vuelve a llamar `jitTryBlock`**. Entre bloques el driver paga, por
orden: (a) checks status/RE/align [barato], (b) **sample de interrupt + entrega pendiente**
[CORRECTO, imprescindible], (c) probe TLB/softTLB [cacheado, barato en hit], (d) `find(phys)`
[hash], (e) **loop de validación SMC K-iter** [~15-20% del path], (f) check borde de timer,
(g) `blk.fn()`. Encadenar dos bloques debe saltar (c)(d)(e)(g-dispatch) pero **NO puede saltar
(b) ni (f)** sin romper `systemtest` Timing/Cycle.

**CRUX (el riesgo al oráculo):** el intérprete muestrea interrupt por-instrucción; el JIT lo
batchea hasta el borde de timer (`d<=K` declina). Si enlazo N bloques, retraso la entrega de
interrupt por toda la cadena → Timing/Cycle fallan. Por eso el enlace NO puede ser un `jmp`
crudo bloque→bloque: el destino debe re-verificar interrupt+timer y **bail al driver si hay
entrega/borde pendiente**.

### Plan incremental (cada paso gated `KESTREL_JIT_LINK`, oráculo tras cada uno)

1. **Prólogo de entrada re-validable, emitido en codegen.** Antes del cuerpo del bloque, emitir
   un check compacto: `Count+K < Compare` (no cruzar borde de timer) **y** `!interruptPending &&
   !timerIntr` con el gate de `Status`. Si falla → saltar al epílogo devolviendo `0` (retired 0)
   con `pc` intacto → el driver ve 0 y re-despacha por la ruta lenta (que vectoriza interrupt o
   maneja el borde exacto). En éxito, ejecuta el cuerpo. **Coste:** unos pocos loads+cmp por
   bloque, pero permite que el *siguiente* bloque se entre sin volver al driver. Validar 0/3721
   (base+timing+cycle) con el prólogo SIEMPRE activo pero SIN enlace todavía (debe ser neutro).
2. **SMC por-bloque, no global.** Hoy un SMC hit hace `cc->clear()` (tira toda la cache). Para
   enlazar hay que invalidar *un* bloque sin romper enlaces ajenos: (i) marca de página→bloques,
   (ii) en `tlbWrite`/DMA-a-código invalidar solo los bloques de esa página + despachar sus
   enlaces entrantes (patch del `jmp` de vuelta al trampolín). Mover el loop SMC del driver al
   prólogo del bloque (validar sus K palabras vs I-cache) para que el bloque enlazado también lo
   pague. Medir: si el SMC-en-prólogo iguala el coste del driver, el enlace neto gana igual por
   ahorrar (c)(d)(dispatch).
3. **Enlace real.** El ctrl-exit, en vez de `jmp epílogo`, para un **target estático conocido**
   (J/JAL con TARGET26; BEQ/BNE con destino en la misma página ya traducido): buscar el bloque
   destino en cache; si existe y su clave `phys` coincide, emitir `jmp rel32` directo a su
   prólogo re-validable (paso 1). Si no existe aún → dejar el `jmp epílogo` y **registrar el sitio
   del jmp** en el `Block` origen; cuando el destino se compile, hacer back-patch. Targets
   dinámicos (JR `$ra`) NO se enlazan estáticamente (van por el driver; opcional: cache de 1
   entrada last-target→block).
4. **Presupuesto de cadena.** Contador de ops en registro host a través de la cadena enlazada;
   al exceder un budget (p.ej. hasta el borde de timer) salir al driver para re-sincronizar
   Count/interrupt. Garantiza que Timing/Cycle ven el mismo grano que hoy.

**Validación:** `KESTREL_JIT_LINK=0` = comportamiento actual byte-idéntico. Con enlace ON, además
del oráculo systemtest, un modo `KESTREL_JIT_LINKDIFF` que corra la cadena enlazada vs el driver
no-enlazado y compare pc/gpr/Count en cada frontera de bloque. Riesgo alto → paso 1 y 2 primero
(neutros, sin enlace) para aislar cualquier regresión antes de encender el `jmp` directo.
2. **Register allocation cross-op** (mantener guest-regs calientes en host-regs a través del
   bloque, sin `ld64`/`st64` por op) — el multiplicador grande (2-3x) pero el más invasivo.
3. **Fast-path RDRAM inline** (índice directo sobre puntero host, sin `call jitMem`) — el jitMem
   pasa además por el D-cache modelado; inline exige replicar tag/fill → riesgo alto, ganancia
   acotada por ese coste. Menor prioridad que 1/2.
4. Absorber BLEZ/BGTZ, REGIMM BLTZ/BGEZ, BEQL/BNEL (cola larga de cobertura).
