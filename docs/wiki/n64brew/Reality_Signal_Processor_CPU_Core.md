# Reality Signal Processor/CPU Core

From N64brew Wiki

< [Reality Signal Processor](https://n64brew.dev/wiki/Reality_Signal_Processor "Reality Signal Processor")

Jump to navigation Jump to search

## Contents

  * 1 Scalar unit (SU)
    * 1.1 Missing opcodes
    * 1.2 Memory access
  * 2 Vector Unit (VU)
    * 2.1 Vector registers and glossary
    * 2.2 Accumulator
    * 2.3 Control registers
    * 2.4 Clamping
    * 2.5 Element field
    * 2.6 Broadcast modifier
    * 2.7 Instructions overview
      * 2.7.1 Loads and stores
      * 2.7.2 Single-lane instructions
      * 2.7.3 Computational instructions
      * 2.7.4 Select instructions
      * 2.7.5 VU/SU Moves
    * 2.8 Instruction details
      * 2.8.1 Scalar loads: LBV, LSV, LLV, LDV
        * 2.8.1.1 Assembly
        * 2.8.1.2 Pseudo-code
        * 2.8.1.3 Description
        * 2.8.1.4 Usage
      * 2.8.2 Scalar stores: SBV, SSV, SLV, SDV
        * 2.8.2.1 Assembly
        * 2.8.2.2 Pseudo-code
        * 2.8.2.3 Description
        * 2.8.2.4 Usage
      * 2.8.3 128-bit vector loads: LQV, LRV
        * 2.8.3.1 Assembly
        * 2.8.3.2 Pseudo-code
        * 2.8.3.3 Description
        * 2.8.3.4 Usage



## Scalar unit (SU)

The scalar is the half of the RSP core that is similar to a standard MIPS R4000 32-bit CPU. It has 32 32-bit registers (conventionally called `r0`-`r31`) and implement most standard opcodes. This page does not describe the whole scalar unit as standard MIPS documentation suffices, but it highlights the main difference. 

### Missing opcodes

The following opcodes are not implemented by RSP: 

  * **Multiplication units.** RSP does not have a multiplication unit so there is no MULT, MULTU, DIV, DIVU, MFHI, MFLO, MTHI, MTLO.
  * **64-bit instructions.** RSP only has 32-bit scalar registers in SU, so there is no 64-bit opcodes (the ones starting with D such as DADDIU, DSRL, etc.) nor 64-bit memory accesses such as LD, SD, LDL, SDL.
  * **No opcodes for misaligned memory accesses.** All memory accesses to DMEM can be correctly performed also to misaligned addresses, using the standard opcodes like LW / SW or LH / LHU / SH, so there is no LWL, LWR, SWL, SWR.
  * **No traps or exceptions.** RSP does not implement any form of interrupt or exception handling, so there is no SYSCALL nor trap instructions (TGE, TLT, etc.). BREAK is available but it has a special behavior (see below).
  * **No support for likely branches.** The "likely" variant of all branches is not supported. The missing opcodes are the ones ending with L (such as BEQL, BLEZL, etc.)



### Memory access

RSP is a harvard architecture. All opcodes are fetched from IMEM (4KB) and all data is access in DMEM (4KB). 

The PC register is 12-bit. All higher address bits in branch / call instructions are thus ignored. When PC reaches the last opcode (at 0xFFC), execution continues to the first opcode in IMEM (PC wraps to 0x000). 

All accesses to DMEM are performed using the lowest 12 bits of the address calculated by the load/store instruction (higher bits are ignored). Moreover, contrary to standard MIPS architecture, the RSP can correctly perform misaligned memory accesses (eg: it is possibly to fetch a 32-bit word at address 0x001, that will contain the 4 bytes at 0x1-0x5). Standard MIPS architecture allows to do misaligned addresses only using the LWL/LWR or SWL/SWR couples, which are not required on the RSP. 

## Vector Unit (VU)

The VU is the internal unit of the RSP CPU core that is able to perform fixed-point SIMD calculations. It is a proprietary design which does not follow any standard specification. Its opcodes and registers are exposed to the core via the COP2 interface. 

### Vector registers and glossary

VU contains 32 128-bit SIMD registers, each organized in 8 lanes of 16-bit each one. Most VU opcodes perform the same operation in parallel on each of the 8 lanes. The arrangement is thus similar to x86 SSE2 registers in EPI16 format. 

The vector registers array is called `VPR` in this document, so `VPR[4]` refers to the fifth register (usually called `v4` in assembly). When referring to specific portions of the register, we use the following convention: 

  * `VPR[vt][4..7]` refers to byte indices, that is bytes from 4 to 7, counting from the higher part of the register (in big-endian order).
  * `VPR[vt]<4..7>` refers to specific lane indices, that is lanes from 4 to 7 counting from the higher part of the register (in big-endian order).
  * Within each lane, `VPR[vt]<2>(3..0)` refers to inclusive bit ranges. Notice that bits are counted as usual in little-endian order (bit 0 is the lowest, bit 15 is the highest), and thus they are written as `(high..low)`.



Ranges are specified using the `beg..end` inclusive notation (that is, both `beg` and `end` are part of the range). 

The concatenation of disjoint ranges is written with a `,`, for instance: `[0..3,8..11]` means 8 bytes formed by concatenating 4 bytes starting at 0 with 4 bytes starting at 8. 

Vector lanes are usually interpreted as a fixed point number. As a homebrew programmer, it is useful to understand the meaning of each opcode and its correct usage while writing code, which goes beyond the exact hardware description of how bits are shuffled around. To refer to a fixed point number, we use the syntax `S1.15` where "S" means "signed" (while "U" is "unsigned"), "1" is the number of bits for the integral part, and "15" are the number of bits for the fractional part. 

### Accumulator

The RSP contains a 8-lane SIMD accumulator, that is used implicitly by multiplication opcodes. Each of the 8 lanes is 48-bits wide, that allows to accumulate intermediate results of calculations without the loss of precision that would incur when storing them into a 16-bit lane in a vector register. 

It is possible to extract the contents of the accumulator through the VSAR opcode; one call to this opcode can extract a 16-bit portion of each lane and store it into the specified vector register. The three portions are conventionally called `ACCUM_LO` (bits 15..0 of each lane), `ACCUM_MD` (bits 31..16 of each lane), and `ACCUM_HI` (bits 47..32 of each lane). 

If you exclude the VSAR instruction that cuts the accumulator piecewise for extracting it, it is better to think of it a single register where each lane is 48-bits wide. 

### Control registers

The VU contains 3 16-bit control registers: VCC, VCO, VCE. 

These registers are used as flag registers by several opcodes. As with most flags, even though they have a general meaning that is generally valid, they tend to also be used in some mind-twisting way to obtain the desired result. It doesn't really make sense to try to describe them at the general level and instead each instruction will explain if/how it uses or modifies the control registers. 

To read/write the contents of the control registers, the `ctc2` / `cfc2` instructions can be used. 

### Clamping

Multiplication opcodes perform a clamping step when extracting the accumulator into a vector register. Notice that each lane of the accumulator is always treated as a _signed_ 48-bit number. 

This is the pseudo-code for signed clamping (no surprises): 
    
    
    function clamp_signed(accum)
        if accum < -32768  => return -32768
        if accum > 32767   => return 32767
        return accum
    

The returned value is thus always within the signed 16-bit range. 

This is the pseudo-code for unsigned clamping: 
    
    
    function clamp_unsigned(accum)
        if accum < 0       => return 0
        if accum > 32767   => return 65535
        return accum
    

Notice that in unsigned clamping, the saturating threshold is 15-bit, but the saturated value is 16-bit. 

### Element field

Most VU instructions have a 3-register format with an additional modifier called "element field". For instance (using GNU assembly syntax):
    
    
    opcode v0, v1, v2,e(7)
    

`e(7)` is the "element modifier". Normally (and especially in GNU syntax, which is more orthogonal and uniform), it refers to a specific lane of the third register, which is why it is common to format it without a leading whitespace. In this example, it "selects" lane 7 of register `v2`. The exact meaning of the element modifier varies for different instruction groups, and also the way it is assembled changes wildly. Pay attention to the description of each instruction group to check what the element modifier means and how it is encoded in the opcode. 

### Broadcast modifier

One of the most common uses of the element field is the broadcast modifier. This modifier is used by computational instructions and select instructions and allows to "broadcast" (duplicate) one or more lanes to other lanes, just for the purpose of the current opcode. For instance:
    
    
    vaddc $v01, $v04,e(1)
    

`e(1)` is the broadcast modifier. Normally, the instruction would add the two registers lane by lane; with the modifier, the second lane (index 1) of `$v04` is added to all lanes of `$v01`. 

The modifier is stored in the `element` field of the opcode 

`element` | GNU syntax  | SGI syntax  | Lanes being accessed  | Description   
---|---|---|---|---  
0  |  |  | 0,1,2,3,4,5,6,7  | Normal register access (no broadcast)   
1  |  |  | 0,1,2,3,4,5,6,7  | Normal register access (no broadcast)   
2  | `e(0q)` | `[0q]` | 0,0,2,2,4,4,6,6  | Broadcast 4 of 8 lanes   
3  | `e(1q)` | `[1q]` | 1,1,3,3,5,5,7,7  | Broadcast 4 of 8 lanes   
4  | `e(0h)` | `[0h]` | 0,0,0,0,4,4,4,4  | Broadcast 2 of 8 lanes   
5  | `e(1h)` | `[1h]` | 1,1,1,1,5,5,5,5  | Broadcast 2 of 8 lanes   
6  | `e(2h)` | `[2h]` | 2,2,2,2,6,6,6,6  | Broadcast 2 of 8 lanes   
7  | `e(3h)` | `[3h]` | 3,3,3,3,7,7,7,7  | Broadcast 2 of 8 lanes   
8  | `e(0)` | `[0]` | 0,0,0,0,0,0,0,0  | Broadcast single lane   
9  | `e(1)` | `[1]` | 1,1,1,1,1,1,1,1  | Broadcast single lane   
10  | `e(2)` | `[2]` | 2,2,2,2,2,2,2,2  | Broadcast single lane   
11  | `e(3)` | `[3]` | 3,3,3,3,3,3,3,3  | Broadcast single lane   
12  | `e(4)` | `[4]` | 4,4,4,4,4,4,4,4  | Broadcast single lane   
13  | `e(5)` | `[5]` | 5,5,5,5,5,5,5,5  | Broadcast single lane   
14  | `e(6)` | `[6]` | 6,6,6,6,6,6,6,6  | Broadcast single lane   
15  | `e(7)` | `[7]` | 7,7,7,7,7,7,7,7  | Broadcast single lane   
  
### Instructions overview

#### Loads and stores

31..26  | 25..21  | 20..16  | 15..11  | 10..7  | 6..0   
---|---|---|---|---|---  
`LWC2` or `SWC2` | `base` | `vt` | `opcode` | `element` | `offset`  
  
The instructions perform a load/store from DMEM into/from a vector register. 

  * `base` is the index of a scalar register used as base for the memory access
  * `offset` is a signed offset added to the value of the base register (with some scaling, depending on the actual instruction).
  * `vt` is the vector register.
  * `element` is used to index a specific byte/word within the vector register, usually specifying the first element affected by the operation (thus allows to access sub-portions of the vector register).

List of all loads and stores opcodes  Group  | Opcode  | Instruction  | Description   
---|---|---|---  
Scalar  | 0x00  | `lbv` / `sbv` | Load / Store 1 byte into/from a VPR   
0x01  | `lsv` / `ssv` | Load / Store 2 bytes into/from a VPR   
0x02  | `llv` / `slv` | Load / Store 4 bytes into/from a VPR   
0x03  | `ldv` / `sdv` | Load / Store 8 bytes into/from a VPR   
128-bit  | 0x04  | `lqv` | Load (up to) 16 bytes into a VPR, left-aligned   
0x05  | `lrv` | Load (up to) 16 bytes into a VPR, right-aligned   
0x04  | `sqv` | Store (up to) 16 bytes from a VPR, left-aligned   
0x05  | `srv` | Store (up to) 16 bytes from a VPR, right-aligned   
8-bit packed  | 0x06  | `lpv` / `spv` | Load / store 8 8-bit signed values into a VPR   
0x07  | `luv` / `suv` | Load / store 8 8-bit unsigned values into a VPR   
0x08  | `lhv` / `shv` | Load / store 8 8-bit unsigned values into VPR, accessing every other byte in memory   
0x09  | `lfv` / `sfv` | Load / store 4 8-bit unsigned values into VPR, accessing every fourth bytes in memory   
Transpose  | 0x01  | `swv` |   
0x0B  | `ltv` | Load 8 lanes from 8 GPRs into a VPR   
0x0B  | `stv` | Store 8 lanes of a VPR into 8 GPRs   
  
#### Single-lane instructions

31..26  | 25  | 24..21  | 20..16  | 15..11  | 10..6  | 5..0   
---|---|---|---|---|---|---  
`COP2` | 1  | `vt_elem` | `vt` | `vd_elem` | `vd` | `opcode`  
  
Single-lane instructions are an instruction group that perform operations on a single lange of a single input register (`VT<se>`), and store the result into a single lane of a single output register (`VD<de>`). 

Example syntax:
    
    
    vmov $v01, e(4), $v05, e(6)
    

In this example, the value in lane `$v05<6>` is moved to lane `$v01<4>`. In the assembly syntax, the broadcast modifier syntax is used, but no actual broadcast is performed, as the instructions operate on the single specified lane. Only the single-lane broadcast modifiers (`e(0)` ... `e(7)`) are supported.+ 

In the opcode, the fields `vt_elem` and `vd_elem` are used to compute `se` and `de` that is to specify which lane, respectively of the input and output register, is affected. 

`vd_elem` is 5 bits long (range 0..31); the highest bits are always ignored, and the destination lane `de` is simply `vd_elem(2..0)`. 

`vt_elem` is 4 bits long (range 0..15). When `vt_elem(3)` is 1, `vt_elem(2..0)` is actually used as source lane `se`, as expected. When `vt_elem(3)` is 0, a hardware bug is triggered and portions of the lower bits of `vt_elem` are replaced with portion of the bits of `vd_elem` while computing `se`. Specifically, all bits in `vt_elem` from the topmost set bit and higher are replaced with the same-position bits in `vd_elem`. Notice that this behaviour is actually consistent with what happens when `vt_elem(3)` is 1, which means that there is no need to think of it as a special-case. Pseudo-code: 
    
    
    de(2..0) = vd_elem(2..0)
    msb = highest_set_bit(vt_elem)
    se(2..0) = vd_elem(2..msb) || vt_elem(msb-1..0)
    

Single-lane instructions  Opcode  | Instruction  | Description   
---|---|---  
0x33  | `vmov` | Copy one lane of a VPR into another VPR   
0x30  | `vrcp` | Compute the 32-bit reciprocal of a 16-bit fixed point   
0x34  | `vrsq` | Compute the 32-bit reciprocal square root of a 16-bit fixed point   
0x32  | `vrcph` | Extract the higher 16-bit of the result of a previous VRCP   
0x36  | `vrsqh` | Extract the higher 16-bit of the result of a previous VRSQ   
0x31  | `vrcpl` | Compute the 32-bit reciprocal of a 32-bit fixed point   
0x33  | `vrsql` | Compute the 32-bit reciprocal square root of a 32-bit fixed point   
0x37  | `vnop` | No operation (?)   
0x3F  | `vnull` | No operation (?)   
  
#### Computational instructions

31..26  | 25  | 24..21  | 20..16  | 15..11  | 10..6  | 5..0   
---|---|---|---|---|---|---  
`COP2` | 1  | `element` | `vt` | `vs` | `vd` | `opcode`  
  
Instructions have this general format: 
    
    
    VINSN vd, vs, vt,e(…)
    

where `e(…)` is the broadcast modifier (as found in other SIMD architectures), that modifies the access to `vt` duplicating some lanes and hiding others. 

This is the list of opcodes in this group. 

Opcode  | Instruction  | Description   
---|---|---  
0x00  | `vmulf` | Vector multiply S1.15 * S1.15, with rounding and signed clamping   
0x01  | `vmulu` | Vector multiply S1.15 * S1.15 with rounding and unsigned clamping   
0x04  | `vmudl` | Vector multiply U0.16 * U0.16 with signed clamping   
0x05  | `vmudm` | Vector multiply S0.16 * U0.16 with signed clamping   
0x06  | `vmudn` | Vector multiply U0.16 * S0.16 with signed clamping   
0x07  | `vmudh` | Vector multiply S0.16 * S0.16 with signed clamping   
0x08  | `vmacf` | Like VMULF, but also add the result to the accumulator   
0x09  | `vmacu` | Like VMULU, but also add the result to the accumulator   
0x0C  | `vmadl` | Like VMUDL, but also add the result to the accumulator   
0x0D  | `vmadm` | Like VMUDM, but also add the result to the accumulator   
0x0E  | `vmadn` | Like VMUDN, but also add the result to the accumulator   
0x0F  | `vmadh` | Like VMUDH, but also add the result to the accumulator   
0x10  | `vadd` | Vector add with carry   
0x13  | `vabs` | Vector absolute value   
0x14  | `vaddc` | Vector add writing overflow into carry   
0x1D  | `vsar` | Read a portion of the accumulator into a VPR   
0x28  | `vand` | Vector bitwise and (`a & b`)   
0x29  | `vnand` | Vector bitwise nand (`~(a & b)`)   
0x2A  | `vor` | Vector bitwise or (`a | b`)   
0x2B  | `vnor` | Vector bitwise nor (`~(a | b)`)   
0x2C  | `vxor` | Vector bitwise xor (`a ^ b`)   
0x2D  | `vnxor` | Vector bitwise nxor (`~(a ^ b)`)   
  
#### Select instructions

31..26  | 25  | 24..21  | 20..16  | 15..11  | 10..6  | 5..0   
---|---|---|---|---|---|---  
`COP2` | 1  | `element` | `vt` | `vs` | `vd` | `opcode`  
  
Instructions have this general format: 
    
    
    VINSN vd, vs, vt, e(…)
    

where `e(…)` is the broadcast modifier (as found in other SIMD architectures), that modifies the access to `vt` duplicating some lanes and hiding others. See the Computational instructions section for details. 

This is the list of opcodes in this group: 

Opcode  | Instruction  | Description   
---|---|---  
0x20  | `vlt` | Select the lower value between two VPR   
0x21  | `veq` | Compare two VPR to check if they are equal   
0x22  | `vne` | Compare two VPR to check if they are different   
0x23  | `vge` | Select the greater or equal value between two VPR   
0x24  | `vcl` | Clip a VPR against two bounds (lower 16-bits)   
0x25  | `vch` | Clip a VPR against two bounds (higher 16-bits)   
0x26  | `vcr` | Clip a VPR against a pow-2 bound   
0x27  | `vmrg` | Merge two VPR selecting each lane according to flags   
  
#### VU/SU Moves

31..26  | 25..21  | 20..16  | 15..11  | 10..7  | 6..0   
---|---|---|---|---|---  
`COP2` | `opcode` | `rt` | `vs` | `vs_elem` | 0   
  
These are the standard MIPS opcodes for moving data in/out the coprocessor registers. 

`opcode` | Instruction  | Description   
---|---|---  
0x0  | `mfc2` | Copy a lane of a VPR into a GPR   
0x2  | `cfc2` | Copy a VU control register into a GPR   
0x4  | `mtc2` | Copy a GPR into a lane of a VPR   
0x6  | `ctc2` | Copy a GPR into a VU control register   
  
Vector moves follow the same format as standard MIPS coprocessor moves, but use part of the lower 11 bits (which are normally unused) to specify the element field, selecting which lane of the VPR is accessed. Notice that, `vs_elem` in this case is not a broadcast modifier: it specifies a byte offset (not a lane index!), so to copy a lane, `lane*2` must be specified. 

This is an example using GNU syntax:
    
    
    mtc2 a1, $v04,e(4)
    

This example will copy the lower 16 bits of GPR `a1` into the fifth lane of `$v04`. This opcode is assembled with `vs_elem = 8`, as explained above. 

`mtc2` moves the lower 16 bits of the general purpose register `rt` to the bytes `VS[vs_elem+1..vs_elem]`. If `vs_elem` is 15, only `VS[vs_elem]` is written (with `rt[15..8]`). 

`mfc2` moves the 2 bytes `VS[vs_elem+1..vs_elem]` to GPR `rt`, sign extending the 16 bits value to 32 bits. If `vs_elem` is 15, the lower byte is taken from byte 0 of the register (that is, it wraps around). 

`ctc2` moves the lower 16 bits of GPR `rt` into the control register specified by `vs`, while `cfc2` does the reverse, moving the control register specified by `vs` into GPR `rt`, sign extending to 32 bits. Note that both `ctc2` and `cfc2` ignore the `vs_elem` field. For these instructions, the control register is specified as follows: 

`vs` | Register   
---|---  
0  | `VCO`  
1  | `VCC`  
2  | `VCE`  
  
### Instruction details

#### Scalar loads: LBV, LSV, LLV, LDV

31..26  | 25..21  | 20..16  | 15..11  | 10..7  | 6..0   
---|---|---|---|---|---  
`LWC2` | `base` | `vt` | `opcode` | `element` | `offset`  
  
##### Assembly
    
    
    lsv $v01,e(2), 0,s0    ; Load the 16-bit word at s0 into the third lane of $v01
    lbv $v04,8, 0,s1       ; Load the 8-bit word at s1 into the 9th byte of $v04 (MSB of lane 4)
    

Notice that it is possible to specify the lane syntax for the `element` field to refer to a specific lane, but if the access is made using `llv` or `ldv` (4 or 8 bytes), it will overflow into the following lanes. 

##### Pseudo-code
    
    
    addr = GPR[base] + offset * access_size
    data = DMEM[addr..addr+access_size-1]
    VPR[vt][element..element+access_size-1] = data
    

##### Description

These instructions load a scalar value (1, 2, 4, or 8 bytes) from DMEM into a VPR. Loads affect only a portion of the vector register (which is 128-bit); other bytes in the register are not modified. 

The address in DMEM where the value is fetched is computed as `GPR[base] + (offset * access_size)`, where `access_size` is the number of bytes being accessed (eg: 4 for `llv`). The address can be misaligned: despite how memory accesses usually work on MIPS, these instructions perform unaligned memory accesses. 

The part of the vector register being accessed is `VPR[vt][element..element+access_size]`, that is `element` selects the first accessed byte within the vector register. When `element+access_size` is bigger than 15, fewer bytes are processed (eg: `llv` with `element=13` only loads 3 byte from memory into `VPR[vt][13..15]`). 

##### Usage

These instructions are seldom used. Normally, it is better to structure RSP code to work across full vectors to maximize parallelism. Input data should already be provided in vectorized format by the CPU, so that it is possible to use a vector load (`lqv`, in case the input is made of 16-bit data) or a packed load (`luv`/`lpv`, in case the input is made of 8-bit data). Consider also using `mtc2` to load a 16-bit value into a lane of a VPR when the value is available in a GPR. 

A possible use-case for these instructions is to reverse the order of the lanes. For instance, in audio codecs, windowing algorithms often work combining sequences audio samples with other sequences in reverse order. RSP does not have an instruction to reverse the order of the lanes, so in that case it might be necessary to manually reverse the lanes while loading using `lsv`:
    
    
    lqv $v00, 0,s0         ; Load 8 16-bit samples from DMEM at address s0
    lsv $v01,e(7),  0,s1   ; Load 8 16-bit samples from DMEM at address s1 in reverse order
    lsv $v01,e(6),  2,s1
    lsv $v01,e(5),  4,s1
    lsv $v01,e(4),  6,s1
    lsv $v01,e(3),  8,s1
    lsv $v01,e(2), 10,s1
    lsv $v01,e(1), 12,s1
    lsv $v01,e(0), 14,s1
    

#### Scalar stores: SBV, SSV, SLV, SDV

31..26  | 25..21  | 20..16  | 15..11  | 10..7  | 6..0   
---|---|---|---|---|---  
`SWC2` | `base` | `vt` | `opcode` | `element` | `offset`  
  
##### Assembly
    
    
    ssv $v01,e(2), 0,s0    ; Store the 16-bit word in the third lane of $v01 into DMEM at address s0
    sbv $v04,8, 0,s1       ; Store the 8-bit word in the 9th byte of $v04 (MSB of lane 4) into DMEM at address s1
    

Notice that it is possible to specify the lane syntax for the `element` field to refer to a specific lane, but if the access is made using `slv` or `sdv` (4 or 8 bytes), it will overflow into the following lanes. 

##### Pseudo-code
    
    
    addr = GPR[base] + offset * access_size
    data = VPR[vt][element..element+access_size-1]
    DMEM[addr..addr+access_size-1] = data
    

##### Description

These instructions store a scalar value (1, 2, 4, or 8 bytes) from a VPR into DMEM. 

The address in DMEM where the value will be stored is computed as `GPR[base] + (offset * access_size)`, where `access_size` is the number of bytes being accessed (eg: 4 for `SLV`). The address can be misaligned: despite how memory accesses usually work on MIPS, these instructions perform unaligned memory accesses. 

The part of the vector register being accessed is `VPR[vt][element..element+access_size]`, that is `element` selects the first accessed byte within the vector register. When `element+access_size` is bigger than 15, the element access wraps within the vector and a full-size store is always performed (eg: `slv` with `element=15` stores `VPR[vt][15,0..2]` into memory, for a total of 4 bytes). 

##### Usage

These instructions are seldom used. Normally, it is better to structure RSP code to work across full vectors to maximize parallelism. Data flow between RSP and VR4300 should be structured in vectorized format, so that it is possible to use a vector store (`sqv`, in case the output is made of 16-bit data) or a packed load (`suv`/`spv`, in case the output is made of 8-bit data). Consider also using `mfc2` to store a 16-bit value from the lane of a VPR into a GPR. 

See scalar loads for an example of a use-case (reversing a vector) that can be implemented also via `ssv`. 

#### 128-bit vector loads: LQV, LRV

31..26  | 25..21  | 20..16  | 15..11  | 10..7  | 6..0   
---|---|---|---|---|---  
`LWC2` | `base` | `vt` | `opcode` | `element` | `offset`  
Insn  | `opcode` | Desc   
---|---|---  
`LQV` | 0x04  | load (up to) 16 bytes into vector, left-aligned   
`LRV` | 0x05  | load (up to) 16 bytes into vector, right-aligned   
  
##### Assembly
    
    
    // Standard 128-bit load from DMEM aligned address s0 into $v08
    lqv $v08, 0,s0
    
    // Loading a misaligned 128-bit vector from DMEM
    // (a0 is 128-bit aligned in this example)
    lqv $v00, 0x08,a0    // read bytes 0x08(a0) - 0x0F(a0) into left part of the vector (VPR[vt][0..7])
    lrv $v00, 0x18,a0    // read bytes 0x10(a0) - 0x17(a0) into right part of the vector (VPR[vt][8..15])
    
    // Advanced example using the "element" field
    lqv $v08,e(2), 0x08,a0   // read bytes 0x08(a0) - 0x0F(a0) into VPR[vt][4..11]
    lrv $v08,e(2), 0x18,a0   // read bytes 0x10(a0) - 0x13(a0) into VPR[vt][12..15]
    

Notice that the element field is optional (defaults to 0) and is usually not specified because these instructions are meant to affect the whole vector. The element field can be specified using the lane syntax (`e(N)`) or a raw number which maps to the byte offset inside the vector. 

##### Pseudo-code
    
    
    // lqv
    addr = GPR[base] + (offset * 16)
    end = addr | 15
    size = MIN(end-addr, 15-element)
    VPR[vt][element..element+size] = DMEM[addr..addr+size]
    
    
    
    // lrv
    end = GPR[base] + (offset * 16)
    addr = end & ~16
    size = MIN(end-addr, 15-element)
    VPR[vt][element..addr+size] = DMEM[addr..addr+size]
    

##### Description

Roughly, these functions behave like `lwl` and `lwr`: combined, they allow to read 128 bits of data into a vector register, irrespective of the alignment. 

When the data to be loaded is 128-bit aligned within DMEM, `lqv` is sufficient to read the whole vector (`lrv` in this case is redundant because it becomes a no-op). 

The actual bytes accessed in DMEM depend on the instruction: for `lwv`, the bytes are those starting at `GPR[base] + (offset * 16)`, up to and excluding the next 128-bit aligned byte (`a0+0x10` in the above example); for `lrv`, the bytes are those starting at the previous 128-bit aligned byte (`a0+0x10` in the above example) up to and _excluding_ `GPR[base] + (offset * 16)`. Again, this is exactly the same behavior of `lwl` and `lwr`, but for 128-bit aligned loads. 

`element` is used as a byte offset within the vector register to specify the first byte affected by the operation; that is, the part of the vector being loaded with the instruction pair is `VPR[vt][element..15]`. Thus a non-zero element means that fewer bytes are loaded. 

##### Usage

`lqv` is the most standard way to fill a full VPR vector register loading its contents from DMEM. Given that it's usually possible to define the layout of data in DMEM, it is advisable to design it so that vectors are always aligned to 128-bit (16 bytes), using the `.align 4` directory: this allows to read the vector using just `lqv`, in 1 cycle (though the load has a 3-cycle latency like all instructions that write to a VPR).
    
    
        .data
    
        .align 4
    CONST:  .half 3, 2, 7, 0, 0x4000, 0x8000, 0x7F, 0xFFF      # Several constants used for an algorithm
    
        .text
        
        lqv $v31, %lo(CONST),r0    # Load the constants
    

One example of using `lqv` and `lrv` in pair is to perform a fast memcpy from a possible misaligned address to an aligned destination buffer:
    
    
        # s0 is the pointer to source data in DMEM (possibly misaligned)
        # s4 will point to the destination buffer in DMEM (aligned)
        # t1 is the number of bytes to copy
        li s4, %lo(BUFFER)
    loop:
        lqv $v00, 0x00,s0
        lrv $v00, 0x10,s0
        sqv $v00, 0,s4
        sub t1, 16
        add s0, 16
        bgtz t1, loop
        add s4, 16
