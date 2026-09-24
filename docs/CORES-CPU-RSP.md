# kestrel64 — Los dos núcleos: intérpretes y dynarecs

Estado a 2026-09-24. Documento de referencia: qué ejecuta cada instrucción, en qué modo,
con qué garantías y con qué perillas. Complemento de `docs/ARCH-COMM.md` (hilos y
comunicación), `docs/PERF-CPU.md` (histórico de medidas del intérprete de CPU),
`docs/JIT-PLAN.md` (plan original del dynarec de CPU) y `docs/RSP-JIT.md` (detalle del
dynarec del RSP).

Regla de oro de los dos dynarecs: **el intérprete es el oráculo**. Nada de semántica
duplicada — donde el JIT no puede emitir código nativo correcto, llama al helper del
intérprete. Un bloque compilado no puede divergir porque, en los casos difíciles,
ejecuta el mismo código.

---

## 1. Panorama

| Núcleo | Intérprete | Dynarec | De fábrica | Apagar |
|--------|-----------|---------|-----------|--------|
| CPU VR4300 | `src/cpu/cpu.cpp` | `src/cpu/jit.cpp` (x86-64) | **JIT ON** | `KESTREL_JIT=0` |
| RSP | `src/rsp/rsp.cpp` | `src/rsp/rspjit.cpp` (x86-64) | **JIT ON** | `KESTREL_RSPJIT=0` |

Los dos dynarecs son solo x86-64 Windows (ABI Win64). Sin ellos el emulador funciona
igual, más lento.

---

## 2. CPU — intérprete (`src/cpu/cpu.cpp`, 3521 líneas)

### Qué modela

- **MIPS III de 64 bits.** GPR de 64 bits; las ops de palabra extienden el signo a 64.
- **Delay slots** con el par `pc`/`nextPc`, incluidas las variantes *likely*.
- **COP0 completo**: Status/Cause/EPC/Count/Compare/BadVAddr/EntryHi/EntryLo/Context/
  XContext/PageMask/Wired/Random/LLAddr/WatchLo/WatchHi/PRId/Config/TagLo/TagHi/ErrorEPC.
  Los registros no usados {7,21,22,23,31} comparten un latch, como el hardware.
- **COP1 (FPU)** con los modos de redondeo y las banderas de FCR31.
- **COP2**: el VR4300 no tiene unidad funcional de COP2, así que MFC2/MTC2 solo mueven un
  latch (`cp2latch`). Eso es lo que hace el chip.
- **TLB de 32 entradas** con máscaras de página, bit global, ASID, y las cuatro
  excepciones (Refill, Invalid, Modified, AdE) vectorizadas al vector correcto según KX/SX/UX.
- **Cachés I (16 KB, línea 32 B) y D (8 KB, línea 16 B)**, directas, modeladas con tags,
  bit de sucio y relleno. La instrucción CACHE opera sobre ellas de verdad.
- **Interrupciones**: IP2 (MI), IP7 (timer Count/Compare), con el bit BD de Cause y EPC
  exactos cuando la excepción cae en un delay slot.
- **Excepción Watch** (WatchLo/WatchHi) armada: se toma *antes* de completar el acceso.

### Modelo de tiempo

- `retired` sube +1 por instrucción; Count sube con ella (ver `docs/ARCH-COMM.md` §2).
- **Coste de fallo de caché**, opt-in: `KESTREL_CACHECOST=<ciclos>` acumula en
  `stallCycles` y lo convierte a ops equivalentes (`stallOps`, `stallOpsRem`). Esos
  campos son estado de invitado y entran en la foto de estado.
- **Coste de mult/div** (`chargeMulDiv`): las latencias del VR4300, cobradas también
  desde el código emitido por el JIT.
- **Interlocks** (`KESTREL_INTERLOCK`) para las paradas de registro.

### Perillas de diagnóstico

`KESTREL_MAXINSN`, `KESTREL_PCSAMPLE`, `KESTREL_REGCHK`, `KESTREL_CACHESTAT`,
`KESTREL_BRFAIL`, `KESTREL_CPUIDLE`, `KESTREL_IDLEPACE`.

---

## 3. CPU — dynarec (`src/cpu/jit.cpp`, 3231 líneas)

Compilador de bloques de línea recta a x86-64, con enlace de bloques. **Activo de fábrica**
(`KESTREL_JIT=0` lo apaga). El intérprete sigue siendo el camino de todo lo que el
compilador no acepta, y de cualquier bloque que aborte.

### Forma del bloque

- Se compila desde una dirección **física** hasta la primera op no segura, el tope de ops,
  o el fin de RDRAM.
- La cache de bloques es un mapa indexado por física; `KESTREL_JIT_BUFMB` y
  `KESTREL_JIT_SLOTBITS` dimensionan el buffer RWX y las ranuras.
- **Residencia de registros** (`KESTREL_JIT_NOREGCACHE` la apaga): los GPR calientes se
  quedan en registros no volátiles de Win64, así que sobreviven a los CALL a helpers. Cada
  salida lleva capturada una instantánea de qué ranuras estaban sucias en ese punto, para
  que el volcado sea exacto y no haga falta vaciar el banco entero antes de cada llamada.

### Qué se emite nativo

- ALU/desplazamientos de 32 bits, LUI, movimientos.
- **ALU de 64 bits** (DADDU/DSUBU/DADDIU y los desplazamientos dobles) con guardia de modo
  kernel (KSU==0): fuera de kernel sin UX/SX el VR4300 las reserva (RI), y esa es
  condición de runtime, así que se sale por el bail sin haber escrito nada.
  `KESTREL_JIT_NOALU64`.
- **ALU con trampa de desbordamiento** (ADDI, ADD, SUB): se emite la aritmética nativa y,
  si el flag OF del anfitrión marca, se sale por el mismo stub de bail **sin escribir el
  destino**; el intérprete re-ejecuta y levanta IntegerOverflow con su semántica exacta. El
  OF de x86 tras un add/sub de 32 bits *es* el desbordamiento con signo de MIPS.
  `KESTREL_JIT_NOTRAPALU` (0 todo, 1 ninguna, 2 solo ADDI, 3 solo ADD/SUB).
- **Memoria rápida** (`KESTREL_JIT_NOFASTMEM`, máscara por tipo): LW/SW/LWC1/SWC1 y el
  resto de cargas y tiendas enteras, cuando la dirección es CKSEG0 canónico dentro de
  RDRAM. La comprobación es una suma y un unsigned-compare: `rax = a + 0x80000000`, y si
  cabe en 0x20000000 entonces `rax` ya es la física. Fuera de eso, al helper.
- **MFC0** dentro del bloque, salvo Count, Random y Cause (que no valen lo mismo a mitad de
  bloque que en el borde) y con guardia de modo kernel. `KESTREL_JIT_NOCOP0`.
- **MTC0** solo si el llamante cierra el bloque justo detrás (escribir Status/Cause puede
  dejar una interrupción lista), y nunca Count ni Compare.
- **Branches y saltos** con su delay slot absorbidos. `KESTREL_JIT_NOBRANCH`,
  `KESTREL_JIT_NOJMP`.
- **Bloques que cruzan página** (`KESTREL_JIT_NOXPAGE=1` vuelve al comportamiento previo de
  no pasar de la página de entrada), incluido el caso TLB (`KESTREL_JIT_NOTLBXPAGE`).

### Qué se delega al intérprete *dentro* del bloque

Un `call` al trampolín, sin cortar el bloque. Antes, la primera op no soportada declinaba
el bloque entero y en código real (SM64/PD) esa op es casi siempre FPU, así que los
bloques quedaban en ~2.4 ops.

- COP1 (0x11) salvo BC1x, que es un branch.
- LWC1/LDC1/SWC1/SDC1.
- LWL/LWR/SWL/SWR y LDL/LDR/SDL/SDR (memoria desalineada). Las de 64 bits importan: GCC
  (libdragon) resuelve una copia desalineada de 64 bits con LDL/LDR + SDL/SDR, mientras que
  IDO (libultra) usa las de 32.
- DIV/DIVU/DMULT/DMULTU/DDIV/DDIVU.

**Excluidos a propósito**: COP0 general (cambia TLB/Status, puede vectorizar), CACHE, LL/SC,
SYSCALL/BREAK/TRAP y todo lo que salte.

### Enlace de bloques

- **Enlace estático** (`KESTREL_JIT_NOLINK`): destino conocido en compilación.
- **Cache de destinos indirectos (ITC)** para JR/JALR (`KESTREL_JIT_NOITC`,
  `KESTREL_JIT_ITCBITS`).
- **Presupuesto de cadena** (`KESTREL_JIT_CHAIN`): tope de bloques enlazados por entrada al
  driver. Sin él, un bucle auto-enlazado no devolvería el control hasta el borde del timer.
  Las interrupciones **no** dependen de esto: se re-muestrean en cada eslabón.
- **Pre-armado** (`KESTREL_JIT_NOPREARM`): el primer bloque de la cadena entra sin pasar por
  el trampolín (eran 3,1 M de llamadas Win64 por corrida de PD, 3,85% del hilo de CPU).
- Enlace con TLB: `KESTREL_JIT_NOTLBLINK`, `KESTREL_JIT_NOTLBSTATIC`. Para traducir el PC de
  entrada de un bloque mapeado por TLB sin faltar se usa `CPU::probing`, que hace
  `translate()` libre de efectos secundarios.

### Cómo NO se salta los plazos

- `rcpDueIn()` / `siDueIn` acotan el presupuesto: un bloque no puede tragarse el instante de
  una interrupción.
- Un store que arma un plazo nuevo (SI, PI, DPC_END, CLEAR_HALT del SP) anula el permiso de
  la cadena en curso poniendo `*jitGuardPtr = 0`.
- Tras cada bloque, el driver remata SI, PI, VI y los plazos del RCP en la instrucción
  exacta, igual que hace el intérprete op a op.
- Con la excepción Watch armada manda el intérprete: el código emitido no compara cada
  dirección contra WatchLo.
- **Guardia del RSP** (`KESTREL_JIT_NORSPGUARD` / `KESTREL_JIT_RSPGUARD`): en Lockstep el
  bloque solo se toma con el RSP parado, para no alterar el intercalado. En Threaded, con
  plazos, esa guardia ya no se emite.

### SMC (código automodificable) e invalidación

El prólogo valida **por línea de I-caché**, no por op: la palabra solo puede haber cambiado
si la línea se rellenó de nuevo, y eso lo dice el sello que sube `icFill`. Con sello y tag
intactos no se mira ni un byte. La comparación es contra la **línea de I-caché**, no contra
RDRAM: en hardware la CPU ejecuta código stale de I-caché si un DMA reescribe RDRAM sin
invalidar, así que validar contra RDRAM sobre-invalidaría. `KESTREL_JIT_NOSMC` mide el techo
de ese bucle.

### Diagnóstico y A/B

`KESTREL_JIT_STATS` (por dónde sale cada terminador: enlace estático acertado, ITC acertada,
salida lenta con destino estático o sin él; y cómo termina el bloque: salto, ops agotadas,
corto), `KESTREL_JIT_DIFF` / `KESTREL_JIT_DIFFGO` (corre el bloque sobre una **copia** de los
registros y compara contra el intérprete, sin contaminar el estado del invitado, así que un
barrido saca todos los bloques malos de una pasada), `KESTREL_JIT_BRDIFF`, `KESTREL_JIT_TRACE`,
`KESTREL_JIT_DUMP` / `DUMPSEL`, `KESTREL_JIT_ONLY1`, `KESTREL_JIT_PCCHK`, `KESTREL_JIT_REGCHK`
(cordura de SP y RA: en un juego sano de 32 bits siempre son la extensión de signo de un
KSEG0/KSEG1, así que en cuanto uno deja de serlo el error *acaba* de ocurrir).

---

## 4. RSP — intérprete (`src/rsp/rsp.cpp`, 2404 líneas)

- Núcleo escalar MIPS-ish: 32 GPR, **sin** mult/div ni HI/LO, PC de 12 bits sobre 4 KB de
  IMEM, sin TLB, sin excepciones, sin interrupciones dentro de una tarea.
- **Unidad vectorial (COP2)**: 32 registros de 8 carriles de 16 bits (`R128`, `alignas(16)`),
  acumulador de 48 bits, banderas VCO/VCC/VCE, los recíprocos VRCP/VRSQ con su tabla, y todos
  los modos de elemento (broadcast) con máscaras `pshufb`.
  La semántica de decodificación sale de la referencia de opcodes de n64brew
  (`docs/wiki/n64brew`); los casos límite de acumulador, saturación y banderas están
  transcritos de la referencia SISD validada contra hardware, no reinventados.
- **Camino SSE** para COP2 (`KESTREL_NORSPSSE`, `KESTREL_NOVECFAST` lo apagan).
- DMA (SP_RD_LEN/SP_WR_LEN) con su grano y su registro (`KESTREL_DMAGRAIN`, `KESTREL_DMALOG`).
- COP0 del RSP = MMIO de SP y DP: es por ahí por donde el microcódigo gráfico patea el RDP.
- Al llegar a BREAK se para y (si el bit está puesto) sube MI_SP.

Perillas: `KESTREL_RSPTRACE`, `KESTREL_RSPSHOW`, `KESTREL_VUSTAT` (reparto de instrucciones
de microcódigo), `KESTREL_VUOP`, `KESTREL_RSPPROF`, `KESTREL_RSPTASKS`, `KESTREL_RSPBUDGET`,
`KESTREL_RSPHANG`, `KESTREL_RSPIDLE`, `KESTREL_RSPTANDA` (grano de publicación de ciclos),
`KESTREL_RSPMIX` / `KESTREL_RSPMIXFN` (mezcla intérprete/JIT para bisecar).

---

## 5. RSP — dynarec (`src/rsp/rspjit.cpp`, 1745 líneas)

**Activo de fábrica** (`KESTREL_RSPJIT=0` lo apaga). Oráculo = el propio intérprete del RSP:
mismo md5 de framebuffer con el dynarec encendido y apagado, en todos los modos de
`gate_all.sh`.

### Por qué el RSP es un blanco fácil

4 KB de IMEM, sin TLB, sin excepciones, sin HI/LO, PC de 12 bits. Eso permite tres
simplificaciones que el dynarec de CPU no puede permitirse:

- La cache de bloques es una **tabla directa de 1024 entradas** indexada por `pc>>2`. Sin
  hash, sin colisiones, sin cache negativa que envejecer.
- La **invalidación se decide por contenido** contra una sombra de los 4 KB (`syncImem`):
  una ranura solo muere si su palabra fuente cambió de verdad. Empezó siendo global —
  cualquier escritura en IMEM tiraba la tabla entera — y costaba caro: el juego alterna
  microcódigo de gráficos y de audio, y cada tarea vuelve a DMAear sus 4 KB completos.
  Medido en SM64 (600 campos): 6207 vaciados y 1,7 M compilaciones, con la compilación
  comiéndose ~32% del hilo del RSP. Comparar contra la sombra deja esas recargas en cero.
- Un bloque **nunca puede fallar a medias**: no hay excepciones que vectorizar, así que la
  firma no devuelve "instrucciones retiradas antes del fallo".

### Estado por etapas (todas hechas)

- **Etapa 1 — línea recta.** ALU, desplazamientos, LUI, LB/LBU/SB nativos. COP2, LWC2, SWC2
  y las cargas/tiendas escalares de 16/32 bits como `call` a los **mismos** helpers del
  intérprete (`Rsp::execCop2` / `execLoad` / `execStore` / `exec`).
- **Etapa 2 — saltos absorbidos.** El bloque se lleva el salto que lo cierra y su delay slot
  (BEQ/BNE/BLEZ/BGTZ, los cuatro REGIMM, J/JAL, JR/JALR) y escribe `Rsp::pc` él mismo. El par
  salto+delay era lo más caro del intérprete (dos vueltas del despachador más el pestillo
  `inDelay`) y además cortaba el bloque justo en los bucles del microcódigo. Siguen fuera
  BREAK y todo lo no reconocido.
- **Etapa 2b — COP0 en bloque.** MFC0/MTC0 por el puente `Rsp::jitCop0` con el reloj exacto
  de la instrucción. Si el MTC0 para el núcleo (SET_HALT) o lanza un DMA que reescribe IMEM,
  el bloque corta justo detrás y vuelve al bucle en C sin enlazar. Medido en SM64
  (prdp-jit, 400 intercambios): instrucciones interpretadas 8,26 M -> 0,70 M, entradas
  8,52 M -> 0,71 M; pared neutra (3,41-3,42 s), md5 iguales en todos los modos.
- **Etapa 3 — enlace de bloques** (`KESTREL_RSPJIT_LINK`). El epílogo mira la tabla él mismo
  y, si en el PC de salida hay un bloque vivo que cabe en lo que queda de tanda, salta con
  una cola (`jmp`, no `call`): la pila queda igual que a la entrada, así que el último bloque
  de la cadena vuelve directamente al bucle en C y **la pila no crece** por larga que sea la
  cadena. El enlace es **indirecto, por tabla**, nunca un `jmp rel32` cableado: en el RSP la
  IMEM se reescribe constantemente (overlays), y un enlace directo obligaría a mantener
  retroenlaces por bloque para desengancharlos al invalidar.

Tope de bloque `kMaxOps`, mínimo `kMinOps` (por debajo, el prólogo costaría más que
interpretar). Vías de la cache: `KESTREL_RSPJIT_WAYS`, `KESTREL_RSPJIT_NEWWAY`.

### Vectorial emitido en línea

Partes de COP2 salieron del `call` al helper y se emiten nativas; cada una tiene su A/B:
`KESTREL_RSPJIT_NOVECMEM` (cargas/tiendas vectoriales), `KESTREL_RSPJIT_NOVECPACK`,
`KESTREL_RSPJIT_NOVECMOVE`. El barajado del operando T usa la **misma** tabla `pshufb` que el
intérprete (`rspBcastMask` -> `kBcast`), para que no puedan existir dos broadcasts distintos.

### Qué dice el perfil

Antes del dynarec (SM64, hilo del RSP): `Rsp::step` 59,9% · `execCop2` 31,6% · `execStore`
4,8% · `execLoad` 3,7%, con COP2 al 41,6% de las instrucciones — o sea, la parte vectorial
rendía por encima de su peso y lo que sobraba estaba en el despacho.

Después (2026-09-11, build con DWARF, 926 muestras): el bucle de despacho **ya no aparece**.
Dentro de la imagen 24,6%, fuera 75,4% (de la cual código emitido 32,1%). Lo más alto dentro
de la imagen no pasa del 2,2%. Los dos números que mandan hoy en ese hilo no son de este
fichero: 26,9% **esperando** y 10,6% empujando comandos al RDP.

---

## 6. Cómo se valida cada cambio

1. `n64-systemtest` en Lockstep: 0/3721 (Base/Timing/Cycle).
2. Statehash invariante por juego, threaded contra lockstep y corrida contra corrida.
3. md5 de framebuffer con el dynarec encendido y apagado, en todos los modos de
   `scripts/gate_all.sh`.
4. Para el dynarec de CPU, además, el modo diff (`KESTREL_JIT_DIFF`) corre cada bloque sobre
   una copia y compara contra el intérprete.

Ningún fix se ata a un test concreto: cada arreglo tiene que ser semántica genuina del
VR4300/RCP.
