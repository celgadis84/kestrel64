# Reality Signal Processor/Interface

From N64brew Wiki

< [Reality Signal Processor](https://n64brew.dev/wiki/Reality_Signal_Processor "Reality Signal Processor")

Jump to navigation Jump to search

The RSP interface is accessed by VR4300 via memory mapped registers at the physical address `0x040x xxxx`. However, because all memory accesses in the VR4300 are made using virtual addresses, it is normally used `0xA40x xxxx` to access the interface in the uncached segment. 

## Contents

  * 1 DMEM and IMEM
  * 2 DMA transfers
    * 2.1 Double buffering
  * 3 RSP Internal Registers
    * 3.1 SP_DMA_SPADDR
    * 3.2 SP_DMA_RAMADDR
    * 3.3 SP_DMA_RDLEN
    * 3.4 SP_DMA_WRLEN
    * 3.5 SP_STATUS
    * 3.6 SP_DMA_FULL
    * 3.7 SP_DMA_BUSY
    * 3.8 SP_SEMAPHORE
  * 4 RSP PC register
    * 4.1 SP_PC



## DMEM and IMEM

Both RSP memory banks are fully memory mapped into the VR4300 address space, as follows: 

Address range  | Memory   
---|---  
0x04000000  | 0x04000FFF  | RSP DMEM   
0x04001000  | 0x04001FFF  | RSP IMEM   
  
Accesses are usually performed using 32-bit reads and writes by VR4300. Different access sizes follow the standard behavior of RCP accesses documented in the [Memory Map](https://n64brew.dev/wiki/Memory_map#Physical_Memory_Map_accesses "Memory map"). 

Since the memory is single-port, it can only be accessed by either the VR4300 or the RSP itself at the same time (including its internal DMA engine). Notice that there is no bus arbiter: an access happening at the same time by both processors will cause problems: typically what happens is that VR4300 wins the race, so the RSP write is lost, or the RSP read returns the same data read by the VR4300 (even if the address was different). Also, if a DMA was in progress, the address of the memory access performed by VR4300 becomes the current address of the DMA transfer, corrupting it. So, in general, VR4300 should access DMEM/IMEM only when RSP is halted. 

## DMA transfers

DMA transfers can be initiated by either VR4300 or RSP. They can transfer from/to RDRAM to/from IMEM/DMEM very efficiently, much faster than copying the data word by word using VR4300 over the memory mapped addresses of the memory banks. The speed of transfer is about 3.7 bytes per VR4300 (PClock) cycle (plus some small fixed overhead). It is the fastest DMA engine in the N64. 

The DMA engine allows to transfer multiple "rows" of data in RDRAM, separated by a "skip" value. This allows for instance to transfer a rectangular portion of a larger image, by specifying the size of each row of the selection portion, the number of rows, and a "skip" value that corresponds to the bytes between the end of a row and the beginning of the following one. Notice that this applies only to RDRAM: accesses in IMEM/DMEM are always linear. 

DMA transfers only happen between 8-byte aligned addresses (in both RDRAM and IMEM/DMEM). DMA registers do not allow misaligned addresses to be written, as the lowest 3 bits are ignored and fixed to 0. The same applies to the length registers, so that the transfer size is always a multiple of 8. 

A single DMA transfer can only transfer to/from one of DMEM or IMEM. It is not possible for instance to write data to both DMEM and IMEM in a single transfer. If the transfer hits the end of either memory area, it wraps around to the beginning of it. 

### Double buffering

All DMA registers are double-buffered: this means that it is possible to program a DMA transfer while another one is in progress. As soon as the first transfer finishes, the second one will start. The RSP status register reports in separate bits whether there is a transfer ongoing, and whether there is a transfer pending. 

Reading from the DMA registers always return information on the ongoing transfer, or the last finished transfer. Pending values that are written to the registers can not be read back, until the transfer begins: at that point, they become visible as the transfer progresses. 

## RSP Internal Registers

The internal RSP registers are memory mapped into the VR4300 physical address space starting from 0x0404 0000. Normally, accesses are performed through the virtual uncached segment, so at 0xA404 0000. 

The exact same physical registers are also exposed as COP0 registers to RSP itself, and can thus be accessed using the MTC0 / MFC0 opcodes. Since access to all registers is shared by VR4300 and RSP, special care must be taken while writing software to decide who is in charge of each different resource / feature. For instance, normally DMA operations are performed by either the CPU or the RSP only; if the software architecture requires both to issue DMA transfers, some kind of mutex protocol must be established (for instance, using either the SIG bits in the SP_STATUS register, or the SP_SEMAPHORE register). 

VR4300 address  | RSP COP0 register  | Name  | Description   
---|---|---|---  
0x0404 0000  | c0  | SP_DMA_SPADDR | Address in IMEM/DMEM for a DMA transfer   
0x0404 0004  | c1  | SP_DMA_RAMADDR | Address in RDRAM for a DMA transfer   
0x0404 0008  | c2  | SP_DMA_RDLEN | Length of a DMA transfer. Writing this register triggers a DMA transfer from RDRAM to IMEM/DMEM   
0x0404 000C  | c3  | SP_DMA_WRLEN | Length of a DMA transfer. Writing this register triggers a DMA transfer from IMEM/DMEM to RDRAM.   
0x0404 0010  | c4  | SP_STATUS | RSP status register.   
0x0404 0014  | c5  | SP_DMA_FULL | Report whether there is a pending DMA transfer (mirror of `DMA_FULL` bit of SP_STATUS)   
0x0404 0018  | c6  | SP_DMA_BUSY | Report whether there is a DMA transfer in progress (mirror of `DMA_BUSY` bit of SP_STATUS)   
0x0404 001C  | c7  | SP_SEMAPHORE | Register to assist implementing a simple mutex between VR4300 and RSP.   
  
### SP_DMA_SPADDR

SP_DMA_SPADDR `0x0404 0000` (RSP COP0: `c0`)   
---  
31:24  | U-0 | U-0 | U-0 | U-0 | U-0 | U-0 | U-0 | U-0   
— | — | — | — | — | — | — | —   
23:16  | U-0 | U-0 | U-0 | U-0 | U-0 | U-0 | U-0 | U-0   
— | — | — | — | — | — | — | —   
15:8  | U-0 | U-0 | U-0 | RW-0 | RW-0 | RW-0 | RW-0 | RW-0   
— | MEM_BANK | MEM_ADDR[11:8]   
7:0  | RW-0 | RW-0 | RW-0 | RW-0 | RW-0 | U-0 | U-0 | U-0   
MEM_ADDR[7:3] | 0 | 0 | 0   
bit 31-13 | **Undefined:** Initialized to `0`  
---|---  
bit 12 | **MEM_BANK:** Bank accessed by the transfer   
0 = DMEM   
1 = IMEM   
bit 11-0 | **MEM_ADDR[11:0]:** DMEM or IMEM address used in SP DMAs. Notice that the lowest 3 bits are always 0.   
  
**Extra Details:**

    **MEM_BANK**

    This bit selects the memory bank that will be accessed by the DMA transfer. Notice that, even though the memory banks appear to be contiguous in VR4300 address space, it is not possible to perform a single DMA transfer that spans across two banks. Each transfer will only access a single bank. For instance, to load a microcode, it is normally necessary to do two separate transfers: one for IMEM and one for DMEM.
    **MEM_ADDR**

    This field contains the address in SP memory where the DMA transfer begins. The address is always aligned to 8 bytes, as the lowest 3 bits cannot be written. Notice that after writing to this register, the value is latched by SP but it is kept "pending" until the transfer is initiated via writes to `SP_DMA_WRLEN` or `SP_DMA_RDLEN`. Reads will continue returning the current (non-pending) value that refers to either an ongoing DMA transfer, or the last finished one. After a DMA transfer is finished, reading this register contains the address after the last one that was written.
    **Reads**

    Reading this register while a transfer is progress returns the current SP pointer (that is, the value is updated while the transfer is in progress). Notice that this is true even if another value was previously written to this register and it is pending: see the section on double buffering above for more information.

### SP_DMA_RAMADDR

SP_DMA_RAMADDR `0x0404 0004` (RSP COP0: `c1`)   
---  
31:24  | U-0 | U-0 | U-0 | U-0 | U-0 | U-0 | U-0 | U-0   
— | — | — | — | — | — | — | —   
23:16  | RW-0 | RW-0 | RW-0 | RW-0 | RW-0 | RW-0 | RW-0 | RW-0   
DRAM_ADDR[23:16]   
15:8  | RW-0 | RW-0 | RW-0 | RW-0 | RW-0 | RW-0 | RW-0 | RW-0   
DRAM_ADDR[15:8]   
7:0  | RW-0 | RW-0 | RW-0 | RW-0 | RW-0 | U-0 | U-0 | U-0   
DRAM_ADDR[7:3] | 0 | 0 | 0   
bit 31-24 | **Undefined:** Initialized to `0`  
---|---  
bit 23-0 | **DRAM_ADDR[23:0]:** RDRAM address used in SP DMAs. Notice that the lowest 3 bits are always 0.   
  
**Extra Details:**

    **DRAM_ADDR**

    This field contains the address in RDRAM memory where the DMA transfer begins. The address is always aligned to 8 bytes, as the lowest 3 bits cannot be written. Notice that after writing to this register, the value is latched by SP but it is kept "pending" until the transfer is initiated via writes to `SP_DMA_WRLEN` or `SP_DMA_RDLEN`. Reads will continue returning the current (non-pending) value that refers to either an ongoing DMA transfer, or the last finished one. After a DMA transfer is finished, reading this register contains the address after the last one that was written.
    **Reads**

    Reading this register while a transfer is progress returns the current RDRAM pointer (that is, the value is updated while the transfer is in progress). Notice that this is true even if another value was previously written to this register and it is pending: see the section on double buffering above for more information.

### SP_DMA_RDLEN

This register is used to initiate a DMA transfer from RDRAM to DMEM/IMEM. It must be written as third register, after programming `SP_DMA_SPADDR` and `SP_DMA_RAMADDR`. As soon as it is written, if the DMA engine was idle, a DMA transfer is started. Otherwise, the DMA transfer is enqueued (double-buffered), waiting for the previous one to be finished. 

SP_DMA_RDLEN `0x0404 0008` (RSP COP0: `c2`)   
---  
31:24  | RW-0 | RW-0 | RW-0 | RW-0 | RW-0 | RW-0 | RW-0 | RW-0   
SKIP[11:4]   
23:16  | RW-0 | U-0 | U-0 | U-0 | RW-0 | RW-0 | RW-0 | RW-0   
SKIP[3] | 0 | 0 | 0 | COUNT[7:4]   
15:8  | RW-0 | RW-0 | RW-0 | RW-0 | RW-0 | RW-0 | RW-0 | RW-0   
COUNT[3:0] | RDLEN[11:8]   
7:0  | RW-0 | RW-0 | RW-0 | RW-0 | RW-0 | U-0 | U-0 | U-0   
RDLEN[7:3] | 0 | 0 | 0   
bit 31-20 | **SKIP[11:0]:** Number of bytes to skip in RDRAM after each row. Notice that the lowest 3 bits are always 0.   
---|---  
bit 19-12 | **COUNT[7:0]:** Number of rows to transfer minus 1.   
bit 21-0 | **RDLEN[11:0]:** Number of bytes to transfer for each row minus 1. Notice that the lowest 3 bits are always 0.   
  
**Extra Details:**

    **RDLEN**

    Like other DMA transfers in N64, this field holds the number of bytes to transfer minus 1. Since the DMA engine works in 64-bit words, writing 0 (or any value up to and including 7) starts a transfer of exactly 8 bytes. After the DMA transfer is finished, this field contains the value `0xFF8`; the reason is that the field is internally decremented by 8 for each transferred word, so the final value will be `-8` (in hex, `0xFF8`
    **COUNT and SKIP**

    Setting `COUNT` to 0 initiates a linear transfer of `RDLEN` plus 1 bytes (rounded up to 8 bytes); in this case, the value of `SKIP` is effectively ignored as only one row is transferred. With any other value, `COUNT` indicates the number of rows, to transfer a portion of a rectangular image, and `SKIP` indicates the so-called row stride, that is number of bytes to add to jump from the end of a row to the beginning of next one. After a DMA transfer is finished, `COUNT` is reset to 0, and `SKIP` is unchanged.
    **Reads**

    Reading this register while a transfer is progress returns the updated `RDLEN` and `COUNT` values (that is, the value is updated while the transfer is in progress). Notice that this is true even if another value was previously written to this register and it is pending: see the section on double buffering above for more information. Notice also that `SP_DMA_WRLEN` and `SP_DMA_RDLEN` both always returns the same data on read, relative to the current transfer, irrespective on the direction of the transfer.

### SP_DMA_WRLEN

This register is used to initiate a DMA transfer from DMEM/IMEM to RDRAM. It must be written as third register, after programming `SP_DMA_SPADDR` and `SP_DMA_RAMADDR`. As soon as it is written, if the DMA engine was idle, a DMA transfer is started. Otherwise, the DMA transfer is enqueued (double-buffered), waiting for the previous one to be finished. 

SP_DMA_WRLEN `0x0404 000C` (RSP COP0: `c3`)   
---  
31:24  | RW-0 | RW-0 | RW-0 | RW-0 | RW-0 | RW-0 | RW-0 | RW-0   
SKIP[11:4]   
23:16  | RW-0 | U-0 | U-0 | U-0 | RW-0 | RW-0 | RW-0 | RW-0   
SKIP[3] | 0 | 0 | 0 | COUNT[7:4]   
15:8  | RW-0 | RW-0 | RW-0 | RW-0 | RW-0 | RW-0 | RW-0 | RW-0   
COUNT[3:0] | WRLEN[11:8]   
7:0  | RW-0 | RW-0 | RW-0 | RW-0 | RW-0 | U-0 | U-0 | U-0   
WRLEN[7:3] | 0 | 0 | 0   
bit 31-20 | **SKIP[11:0]:** Number of bytes to skip in RDRAM after each row. Notice that the lowest 3 bits are always 0.   
---|---  
bit 19-12 | **COUNT[7:0]:** Number of rows to transfer minus 1.   
bit 21-0 | **WRLEN[11:0]:** Number of bytes to transfer for each row minus 1. Notice that the lowest 3 bits are always 0.   
  
**Extra Details:** Please refer to `SP_DMA_RDLEN` for details. 

### SP_STATUS

The SP_STATUS register is the main status register for the RSP. Like many other flag registers in N64, it has two different layouts when accessed for reading and writing: this allows to perform atomic set / clear operations on each flag using a simple memory write operation, without risking race conditions that would be frequent if a read-modify-write sequence was issued by the processor. Note that writing both the set and clear bits for a particular flag in the same write results in no effect, the prior state of the flag is preserved. 

SP_STATUS `0x0404 0010` (RSP COP0: `c4`) - Read access   
---  
31:24  | U-0 | U-0 | U-0 | U-0 | U-0 | U-0 | U-0 | U-0   
— | — | — | — | — | — | — | —   
23:16  | U-0 | U-0 | U-0 | U-0 | U-0 | U-0 | U-0 | U-0   
— | — | — | — | — | — | — | —   
15:8  | U-0 | R-0 | R-0 | R-0 | R-0 | R-0 | R-0 | R-0   
— | SIG7 | SIG6 | SIG5 | SIG4 | SIG3 | SIG2 | SIG1   
7:0  | R-0 | R-0 | R-0 | R-0 | R-0 | R-0 | R-0 | R-0   
SIG0 | INTBREAK | SSTEP | IO_BUSY | DMA_FULL | DMA_BUSY | BROKE | HALTED   
bit 7-14 | **SIG <n>:** Status of the 8 custom bits that can be freely used to communicate state between VR4300 and RSP.   
---|---  
bit 6 | **INTBREAK:** Configure the RSP to trigger a RSP MI interrupt when the `BREAK` is run.   
0 = When `BREAK` is run, just halt the RSP core without triggering a RSP MI interrupt.   
1 = When `BREAK` is run, halt the RSP core and trigger a MI interrupt.   
bit 5 | **SSTEP:** Set to `1` when single-step mode is activated. In single-step mode, RSP auto-halts itself after a single opcode is run. See the details below.   
bit 4 | **IO_BUSY:** Set to `1` when the RSP is accessing either DMEM or IMEM. (*TODO*: verify this)   
bit 3 | **DMA_FULL:** Set to `1` when there is a DMA transfer pending in the DMA register in addition to a DMA already in progress.   
bit 2 | **DMA_BUSY:** Set to `1` when there is a DMA transfer currently in progress.   
bit 1 | **BROKE:** Set to `1` when the RSP executes a `BREAK` opcode. It must be manually reset.   
bit 0 | **HALTED:** Current running status of the RSP   
0 = Running   
1 = Idle / halted   
SP_STATUS `0x0404 0010` (RSP COP0: `c4`) - Write access   
---  
31:24  | U-0 | U-0 | U-0 | U-0 | U-0 | U-0 | U-0 | W-0   
— | — | — | — | — | — | — | SET_SIG7   
23:16  | W-0 | W-0 | W-0 | W-0 | W-0 | W-0 | W-0 | W-0   
CLR_SIG7 | SET_SIG6 | CLR_SIG6 | SET_SIG5 | CLR_SIG5 | SET_SIG4 | CLR_SIG4 | SET_SIG3   
15:8  | W-0 | W-0 | W-0 | W-0 | W-0 | W-0 | W-0 | W-0   
CLR_SIG3 | SET_SIG2 | CLR_SIG2 | SET_SIG1 | CLR_SIG1 | SET_SIG0 | CLR_SIG0 | SET_INTBREAK   
7:0  | W-0 | W-0 | W-0 | W-0 | W-0 | W-0 | W-0 | W-0   
CLR_INTBREAK | SET_SSTEP | CLR_SSTEP | SET_INTR | CLR_INTR | CLR_BROKE | SET_HALT | CLR_HALT   
bit 9-24 | **CLR_SIG <n>/SET_SIG<n>:** Set to `0` or `1` the 8 available bitflags that can be used as communication protocol between RSP and CPU.   
---|---  
bit 8 | **SET_INTBREAK:** Enable the `INTBREAK` flag. When this flag is enabled, running a `BREAK` opcode will generate a RSP MI interrupt, in addition to halting the RSP.   
bit 7 | **CLR_INTBREAK:** Disable the `INTBREAK` flag. When this flag is disabled, running a `BREAK` opcode will not generate any RSP MI interrupt, but it will still halt the RSP.   
bit 6 | **SET_SSTEP:** Enable single-step mode. When this mode is activated, the RSP auto-halts itself after every opcode that is run. The VR4300 can then trigger a new step by unhalting it.   
bit 5 | **CLR_SSTEP:** Disable single-step mode.   
bit 4 | **SET_INTR:** Manually trigger a RSP MI interrupt on the VR4300. It might be useful if the RSP wants to manually trigger a VR4300 interrupt at any point during its execution.   
bit 3 | **CLR_INTR:** Acknowledge a pending RSP MI interrupt. This must be done any time a RSP MI interrupt was generated, otherwise the interrupt line on the VR4300 will stay asserted.   
bit 2 | **CLR_BROKE:** Clear the `BROKE` flag, that is automatically set every time a `BREAK` opcode is run. This flag has no effect on the running/idle state of the RSP; it is just a latch that remembers whether a `BREAK` opcode was ever run.   
bit 1 | **SET_HALT:** Pause running RSP code (set the `HALTED` flag)   
bit 0 | **CLR_HALT:** Start running RSP code from the current RSP PC (clear the `HALTED` flag)   
  
**Extra Details:**

    **HALT**

    The HALT flag can be thought of as a "pause" flag. When the RSP is halted by writing the `SET_HALT` bit, the RSP core pauses the pipeline without flushing it, maintaining the current PC but also the intermediate status like pending writebacks and delay slots. If a new ucode is loaded instead, make sure to also write SP_PC to the new entry point (writing to SP_PC also fully discards the RSP core pipeline). Given the "pause" behavior, it would look like the VR4300 could pause and unpause the RSP at any time during its execution without side effects. Unfortunately, this only works "most" of the time: there is at least one hardware bug that can cause corruption when a halt is triggered within a specific sequence of opcodes. This bug has been [observed during libdragon development](https://github.com/DragonMinded/libdragon/blob/2c8b04b7f08fe5e732101fe64d23590f464d6b51/include/rsp_queue.inc#L254-L259), but the developers could not manage to isolate it or reduce it to a small snippet of code. In general, it is thus very risky to prepare a communication protocol that comprehends VR4300 pausing/unpausing the RSP at random times while it is running.
    **HALT and DMA**

    Setting the HALT bit does not pause the DMA transfers in progress. The DMAs will continue running until they finish. This will be reflected by the status register, so that it is possible that both `HALT` and `DMA_BUSY` are set. This is specifically important if VR4300 halts the RSP and wants to access IMEM/DMEM immediately: it is important to wait for `DMA_BUSY` to be cleared before accessing the memory banks, or corruption can happen (see above for more information on what happens when both VR4300 and RSP access the memory banks at the same time).
    **SSTEP**

    The single step mode allows the RSP to execute a single instruction and pause itself. In particular, whenever `SSTEP` is set while the RSP is running, RSP will pause itself before next instruction by setting the `HALT` flag. To perform single-stepping through RSP code, VR4300 should set the `SSTEP` flag and then reset `HALT` to execute exactly one instruction. Unfortunately, this hardware mode is very buggy. There are at least two specific bugs that have been isolated. The presence of these two bugs are enough to consider the feature broken beyond any expectation of being useful. 

  * Conditional branch instructions are sometimes broken; that is a branch is taken where it should not or viceversa
  * `MTC0` / `MFC0` are broken: the instructions have a 2-cycle latency but it looks like the pending writeback is lost in single step mode; `MFC0` actually writes `(PC+4)/4` into the register (where `PC` is the address of the `MFC0` instruction itself). `MTC0` simply doesn't work, and the target register is not written.


    **SIG <n>**

    Signal bits are software-controlled bits with no hardware meaning. They can be set or reset by writing to the status register. Since both VR4300 and RSP can access the status register, they can be used to perform a simple communication / handshaking protocol between the two CPUs. For instance the RSP might set `SIG0` to `1` when some data has been processed and sent back to RDRAM via DMA, so that VR4300 can access the results after it sees `SIG0` being set. Because of the design of the hardware register that allows for atomic modification of bits thanks to the separate write access structure, it is possible for VR4300 and RSP to set/reset different signal bits at the same time without risking race conditions.
    **DMA_FULL**

    This bit is set whenever a DMA transfer is pending, that is it has been programmed via the DMA registers but it has not started yet because another transfer is in progress. This is possible because of the double-buffering of the DMA registers (explained above). Notice that this bit goes to `0` a few clock cycles *before* the previous DMA transfer is finished (probably the RSP internally has some preparation work for DMA that is able to parallelize with the last memory writes of another transfer). Anyway, as soon as the bit goes to zero, it is possible to enqueue a new DMA transfer.

### SP_DMA_FULL

SP_DMA_FULL `0x0404 0014` (RSP COP0: `c5`)   
---  
31:24  | U-0 | U-0 | U-0 | U-0 | U-0 | U-0 | U-0 | U-0   
— | — | — | — | — | — | — | —   
23:16  | U-0 | U-0 | U-0 | U-0 | U-0 | U-0 | U-0 | U-0   
— | — | — | — | — | — | — | —   
15:8  | U-0 | U-0 | U-0 | U-0 | U-0 | U-0 | U-0 | U-0   
— | — | — | — | — | — | — | —   
7:0  | U-0 | U-0 | U-0 | U-0 | U-0 | U-0 | U-0 | R-0   
— | — | — | — | — | — | — | DMA_FULL   
bit 31-1 | **Undefined:** Initialized to `0`  
---|---  
bit 0 | **DMA_FULL:** Mirror of DMA_FULL bit in SP_STATUS   
  
### SP_DMA_BUSY

SP_DMA_FULL `0x0404 0018` (RSP COP0: `c6`)   
---  
31:24  | U-0 | U-0 | U-0 | U-0 | U-0 | U-0 | U-0 | U-0   
— | — | — | — | — | — | — | —   
23:16  | U-0 | U-0 | U-0 | U-0 | U-0 | U-0 | U-0 | U-0   
— | — | — | — | — | — | — | —   
15:8  | U-0 | U-0 | U-0 | U-0 | U-0 | U-0 | U-0 | U-0   
— | — | — | — | — | — | — | —   
7:0  | U-0 | U-0 | U-0 | U-0 | U-0 | U-0 | U-0 | R-0   
— | — | — | — | — | — | — | DMA_BUSY   
bit 31-1 | **Undefined:** Initialized to `0`  
---|---  
bit 0 | **DMA_BUSY:** Mirror of DMA_BUSY bit in SP_STATUS   
  
### SP_SEMAPHORE

SP_SEMAPHORE `0x0404 001C` (RSP COP0: `c7`)   
---  
31:24  | U-0 | U-0 | U-0 | U-0 | U-0 | U-0 | U-0 | U-0   
— | — | — | — | — | — | — | —   
23:16  | U-0 | U-0 | U-0 | U-0 | U-0 | U-0 | U-0 | U-0   
— | — | — | — | — | — | — | —   
15:8  | U-0 | U-0 | U-0 | U-0 | U-0 | U-0 | U-0 | U-0   
— | — | — | — | — | — | — | —   
7:0  | U-0 | U-0 | U-0 | U-0 | U-0 | U-0 | U-0 | RW-0   
— | — | — | — | — | — | — | SEMAPHORE   
bit 31-1 | **Undefined:** Initialized to `0`  
---|---  
bit 0 | **SEMAPHORE:** Semaphore bit to implement a hardware-assisted mutex with atomic access. The bit behaves normally on reads and writes, with the only difference that after each read, the bit is always automatically set to 1 by the hardware (though the previous value is returned).   
  
**Extra Details:**

    **SEMAPHORE**

    The goal of this bit is to help implementing a mutex between VR4300 and RSP. The mutex can be used to guard access to any shared hardware resource, a typical example being the DMA engine. To acquire the mutex, the CPU (either VR4300 or RSP) should spin reading the `SEMAPHORE` bit until it reads 0. At that point, the bit is automatically flipped to 1 by the hardware, so reading 0 means "the semaphore was free, and you have just acquired it". After the CPU is done using the shared resource, it can simply write 0 to `SEMAPHORE` to release it.

## RSP PC register

RSP has an internal PC (program counter) register that cannot be explicitly accessed via RSP opcodes. Instead, a memory mapped register is available to VR4300 to control the RSP PC while RSP is halted. The register is called `SP_PC`. 

Notice that VR4300 is allowed to access SP_PC only while RSP is halted. Reading from SP_PC while RSP is running returns garbage data, and writing to it causes RSP to misbehave. 

### SP_PC

SP_PC `0x0408 0000`  
---  
31:24  | U-0 | U-0 | U-0 | U-0 | U-0 | U-0 | U-0 | U-0   
— | — | — | — | — | — | — | —   
23:16  | U-0 | U-0 | U-0 | U-0 | U-0 | U-0 | U-0 | U-0   
— | — | — | — | — | — | — | —   
15:8  | U-0 | U-0 | U-0 | U-0 | RW-0 | RW-0 | RW-0 | RW-0   
— | — | — | — | PC[11:8]   
7:0  | RW-0 | RW-0 | RW-0 | RW-0 | RW-0 | RW-0 | R-0 | R-0   
PC[7:2] | 0 | 0   
bit 31-12 | **Undefined:** Initialized to `0`  
---|---  
bit 11-0 | **PC[11:0]:** Read/write the RSP PC (program counter). Notice that the lowest two bits are always 0.   
  
**Extra Details:**

    **PC**

    Reads while RSP is running returns random bits. Reads while RSP is halted return the address of the instruction that the RSP will execute when it is unhalted.   
Writes will also reset the RSP CPU core pipeline, so any pending writeback or branch are discarded.
