# Comunicacion CPU <-> RSP <-> RDP en el hardware (referencia, 2026-09-22)

Resumen de n64brew (Reality_Signal_Processor/Interface, Reality_Display_Processor/Interface,
MIPS_Interface) mas el protocolo de libultra/libdragon. Sirve para no volver a leer las
fuentes. Lo que hace kestrel y lo que sobra esta en `docs/ARCH-SYNC.md`.

## SP (RSP), registros en 0x0404_0000 y COP0 c0..c7 del RSP

| reg | que es | notas |
|-----|--------|-------|
| SP_DMA_SPADDR / RAMADDR | direcciones DMA | doble buffer: el valor pendiente no se ve hasta que arranca |
| SP_DMA_RDLEN / WRLEN | escribir = lanzar DMA | RD = RDRAM->DMEM/IMEM, WR = DMEM/IMEM->RDRAM |
| SP_STATUS | lectura/escritura con disposicion distinta | escrituras SET/CLR por bit, atomicas por diseno |
| SP_DMA_FULL | hay un DMA en cola (doble buffer) | baja unos ciclos ANTES de acabar el DMA anterior |
| SP_DMA_BUSY | DMA en curso | |
| SP_SEMAPHORE | mutex HW | leer devuelve el valor y lo deja a 1; escribir lo pone a 0 |
| SP_PC (0x0408_0000) | PC del RSP | la CPU solo lo toca con el RSP parado |

Bits de lectura de SP_STATUS: HALTED, BROKE, DMA_BUSY, DMA_FULL, IO_BUSY, SSTEP, INTBREAK,
SIG0..SIG7.
- BROKE lo pone BREAK y hay que borrarlo a mano.
- INTBREAK: BREAK para el nucleo y levanta MI_SP.
- SIG0..7: bits de software para el protocolo. CPU y RSP pueden tocar bits distintos a la vez
  sin carrera (escrituras SET/CLR por bit).

DMA: unos 3,7 bytes por ciclo de CPU mas un coste fijo pequeno. Es el DMA mas rapido de la
consola.

Contrato de DMEM/IMEM: la CPU solo debe tocarlas con el RSP parado y DMA_BUSY a 0. Si no, se
corrompen: suele ganar la CPU y se pierden escrituras del RSP.

Fallos de HW:
- HALT en ciertas secuencias de opcodes corrompe.
- SSTEP falla con saltos condicionales y con MTC0/MFC0.

La documentacion NO da ninguna latencia de visibilidad para las escrituras de la CPU en
SP_STATUS. No hay orden al ciclo garantizado.

## DPC (RDP), registros en 0x0410_0000 y COP0 c8..c15 del RSP

| reg | que es |
|-----|--------|
| DPC_START | inicio de la lista de comandos (24 bits, alineado a 8) |
| DPC_END | fin de la lista (exclusivo); escribirlo lanza o encola |
| DPC_CURRENT | solo lectura; por donde va el DMA de comandos |
| DPC_STATUS | control y estado |
| DPC_CLOCK / CMD_BUSY / PIPE_BUSY | contadores de 24 bits al reloj del RCP |

- Doble buffer:
  - escribir DPC_START pone START_PENDING;
  - escribir DPC_END lanza el tramo si no hay ninguno en marcha, y si lo hay lo encola
    (END_PENDING);
  - reescribir START con uno ya pendiente lo cambia, y eso es fuente de carreras.
- Bits de lectura de DPC_STATUS: XBUS (0 = RDRAM, 1 = DMEM), FREEZE, FLUSH, START_GCLK,
  TMEM_BUSY, PIPE_BUSY (del primer comando al SYNC_FULL), CMD_BUSY (FIFO no vacio),
  CBUF_READY, DMA_BUSY, END_PENDING, START_PENDING.
- Escritura de DPC_STATUS: SET/CLR de XBUS, FREEZE y FLUSH, y resets de los contadores.
- CPU y RSP comparten TODO el bloque DPC. El HW no arbitra: los usuarios deben garantizar la
  exclusion mutua. En la practica, durante una tarea grafica solo escribe el RSP.
- SYNC_FULL tiene que ser el ultimo comando. No se lanza nada mas hasta que baje BUSY (el RDP
  puede colgarse). Al completarse levanta MI_DP.
- Sondear DPC_CURRENT es la forma normal de ver el avance. Los contadores permiten medir sin
  parar la CPU.

## MI (0x0430_0000)

- MI_INTERRUPT tiene los bits SP(0), SI(1), AI(2), VI(3), PI(4) y DP(5).
- MI_MASK decide si el bit llega a la CPU (IP2). Enmascarar no impide que el bit se levante.
  Si se desenmascara con el bit ya puesto, la CPU lo ve al instante.
- Como se borra cada uno:
  - DP: MI_MODE bit 11 (CLR_DP);
  - SP: SP_STATUS CLR_INTR;
  - SI, AI, VI y PI: con la escritura a su propio registro de estado o de linea.
- MI_SP sale de un BREAK con INTBREAK, o de una escritura SET_INTR en SP_STATUS.

## Protocolo de software (lo que de verdad sincroniza)

libultra:
1. `osSpTaskLoad`:
   - con el RSP parado, la CPU hace writeback de la cache, copia el OSTask a DMEM y el
     arranque (rspboot) a IMEM por DMA;
   - borra SIG0..2 (yield, yielded, task done).
2. `osSpTaskStartGo`: CLEAR_HALT. A partir de aqui el RSP va solo.
3. El microcodigo baja por DMA la display list que la CPU ya dejo en RDRAM ANTES de lanzar,
   con writeback. La CPU no la toca mientras la tarea corre.
4. RSP -> RDP:
   - el microcodigo escribe DPC_START/END por COP0 (c8/c9) y sondea DPC_CURRENT/STATUS (c10/c11)
     para no pisar el buffer circular;
   - la CPU no escribe DPC durante la tarea;
   - `osDpSetNextBuffer` solo se usa sin RSP (XBUS/modo directo).
5. Yield: `osSpTaskYield` pone SIG0 desde la CPU. El microcodigo lo consulta en puntos fijos,
   guarda estado, pone SIG1 (yielded) y hace BREAK.
6. Fin: BREAK con INTBREAK -> MI_SP. La CPU lee SP_STATUS (SIG1/SIG2) en su manejador.
7. Fin de frame RDP: SYNC_FULL -> MI_DP. La CPU espera esa interrupcion antes de usar el
   framebuffer.

libdragon (rspq):
- la CPU escribe comandos en un buffer de RDRAM MIENTRAS el RSP corre;
- despues pone una senal (SIG) para despertarlo;
- el RSP sondea la senal y, cuando la ve, baja el buffer por DMA.

El orden escritura-RDRAM -> senal es lo unico que el juego necesita. Es el unico caso normal
de DMA desde RDRAM con datos recien escritos por la CPU con tarea en marcha.

Conclusion: los puntos reales de encuentro son
- CLEAR_HALT;
- BREAK / MI_SP;
- los bits SIG de SP_STATUS;
- el semaforo;
- DMA RDRAM <-> SP (con los datos entregados antes por el protocolo);
- DPC_END / SYNC_FULL / MI_DP.

Entre esos puntos CPU y RSP no se miran, y el ciclo relativo varia en HW con la contencion
de RDRAM.
