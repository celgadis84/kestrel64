# Reality Signal Processor/CPU Pipeline

From N64brew Wiki

< [Reality Signal Processor](https://n64brew.dev/wiki/Reality_Signal_Processor "Reality Signal Processor")

Jump to navigation Jump to search

## Contents

  * 1 RSP pipeline
  * 2 VU / SU pipelines
  * 3 Pipeline stalls: write latency
  * 4 Pipeline stalls: branches
  * 5 Pipeline stalls: stores after loads
  * 6 Dual-issue
  * 7 Dual-issue: hardware bug with single-lane instructions



### RSP pipeline

This page describes how the RSP CPU pipeline works, from a software point of view. It does not guess how the hardware internally work, but it just describes the effects of the pipelines on code execution 

### VU / SU pipelines

The CPU core contains two different pipelines that can run in parallel. 

  * **VU (vector unit)** : this is the vector pipeline, that is able to execute the special vector instructions (encoded as COP2 opcodes). The opcodes that run in the VU are identified by the opcode name that starts with letter `v`. For instance, `vadd` and `vmod` are vector instructions that runs in the VU. **NOTE:** vector loads/stores such as `lqv` or `sqv` are not considered vector instructions and do not run on the VU, as testified by their opcode name,
  * **SU (scalar unit)** : this is the scalar pipeline, that is able to execute the standard MIPS 32-bit scalar instructions. For instance, `lw`, `srl`, `suv` are all instructions that run in the SU.



### Pipeline stalls: write latency

In a normal condition, all RSP opcodes run exactly in 1 clock cycle but they might have different latency. This means that while the opcode itself appears to run in 1 clock cycle, the destination register might not be available in time for the next instruction. In general: 

  * All instructions that write to a VU register run in 1 clock cycle but their destination register has a 4 cycle latency. This includes all VU opcodes (eg: ``vaddc``) but also SU opcodes that writes to a VU register such as all load ops (eg: `lqv`) and `mtc2`.
  * Most instructions that write to a SU register run in 1 clock cycle and have a 1 cycle latency. The following are the exceptions: 
    * DMEM loads (`lw`, etc.) run in 1 cycle too but have a 3-cycle latency. Notice that stores do the same, but there is no way to cause a pipeline stall with stores, so we are not focusing on them.
    * `mfc0,` `mfc2` , `cfc2` run in 1 cycle and have a 3-cycle latency



Attempting to access a register that is not yet ready cause a _pipeline stall_ that halts the RSP until the register is available. For instance:
    
    
    vsubc $v02, $v10, $v11   # Writes to $v02
    vlt $v02, $v02, $v12     # Reads from $v02 => STALL (3 cycles)
    

Since `vsubc` has 4 cycle latency, the destination register is available only on the 4th cycle after it. Given that the next opcode `vlt` tries to read from `$v02`, a stall is issued that lasts for 3 cycles. Basically the effect is similar to:
    
    
    vsubc $v02, $v10, $v11 # Writes to $v02
    vnop
    vnop
    vnop
    vlt $v02, $v02, $v12 # Reads from $v02 => NO STALL (it's on the 4th cycle after vsubc
    

This is an example of a stall caused by the a SU instruction such as `lw`: 
    
    
    lw t0, 0(s0)    # Writes to t0
    sll t0, 2       # Reads from t0 => STALL (2 cycles)
    

and this is an example of a stall caused by a SU instruction that writes to a VU register:
    
    
    lpv $v00, 0(s0)      # Writes to $v00
    vaddc $v00, $v01     # Reads from $v00 => STALL (3 cycles)
    

### Pipeline stalls: branches

Branches run in 3 cycles (including the delay slot): 

  * One cycle for the branch instruction
  * One cycle for the delay slot
  * One cycle of delay ("pipeline bubble") for internally finalizing the branch.



Example:
    
    
    and t0, 1                  # Cycle 0
    bnez t0, label             # Cycle 1
    vmulf $v00, $v01, $v02     # Cycle 2
    
    label:
        addiu a0, 1            # Cycle 4 (cycle 3 was the bubble)
    

Notice that if either the branch instruction or the delay slot causes stalls themselves, these will just delay the branch bubble, it is not "absorbed" by the other stalls:
    
    
    lw a0, (s0)             # Cycle 0
    bnez t0, label          # Cycle 1 
    addiu a0, 8             # Cycle 2-3 (1 stall because a0 wasn't ready yet)
    
    label:
        addiu a0, 1         # Cycle 5 (cycle 4 was the bubble)
    

### Pipeline stalls: stores after loads

A stall is generated any time a memory store follows **exactly** 2 cycles after a memory load, irrespective of what instructions they are, or what registers they do affect. For instance:
    
    
    lw t0, 0(s0)
    nop
    sqv $v04, 0(s1)    # STALL: write happening two cycles after load
    

As an additional special case, the instructions `mfc0`, `mtc0`, `mfc2`, `mtc2` , `cfc2`, `ctc2` do not access memory, but they also cause the same exact stall, and they even count as **both** loads and stores, irrespective of the behavior of the actual instruction. For instance:
    
    
    lw t0, 0(s0)
    nop
    mtc0 v0, COP0_SP_STATUS     # STALL: mtc0 happening two cycles after load
    
    
    
    lw t0, 0(s0)
    nop
    mfc0 v0, COP0_SP_STATUS     # STALL: mfc0 happening two cycles after load (even if "mfc0" seems itself a load...)
    
    
    
    mtc2 t0, $v04.e2
    nop
    sw t8, 4(s1)                # STALL: "sw" happening two cycles after "mtc2"
    
    
    
    mtc2 t0, $v04.e0
    nop
    cfc2 v0, COP2_VCC           # STALL: cfc2 happening two cycles after mtc2
    

### Dual-issue

Given that VU and SU mostly run in parallel, RSP is able to run **two instructions in just 1 clock cycle**. This happens when a SU and a VU instructions are run next to each other (though not always, see below):
    
    
    vmudh $v02, $v04, $v06.e7
    add t0, t1                    # DUAL-ISSUE (SU after VU)
    

In normal cases, the order does not matter:
    
    
    add t0, t1                    
    vmudh $v02, $v04, $v06.e7     # DUAL-ISSUE (VU after SU)
    

In general, most optimized RSP code will try to interleave SU and VU instructions as much as possible to benefit from dual-issue. 

There are a few cases where it is not possible to dual-issue: 

  * After a branch: the first instruction on the target of a branch can dual-issue only if it is 8-byte aligned. When writing a hot loop, make sure the loop start (target of the end-loop branch) is 8-byte aligned so that you don't lose the dual-issue opportunity on the first instruction.
  * The delay slot of a branch never dual-issue (whether the branch is taken or not).
  * If the first instruction of a pair **writes** to a vector register that is **either read or written** by the second instruction, the pair will not dual-issue. Since we are discussing vector registers here, this applies when the SU instruction is one of the few that access vector registers (that is, vector loads, `mfc2`, `mtc2`) .


    
    
    vand $v04, $v30, $v31
    lqv $v04, 0(s0)            # NO DUAL ISSUE with previous op: writes to $v04 which was also written by vand
    
    
    
    vand $v04, $v30, $v31
    mfc2 t0, $v04.e4            # NO DUAL ISSUE with previous op: reads from $v04 which was written by vand
    

Notice that in this case, it would be sufficient for the instructions to appear in the opposite order to dual-issue (though the semantic would be different, but just for the sake of pipeline analysis):
    
    
    mfc2 t0, $v04.e4            
    vand $v04, $v30, $v31       # DUAL ISSUE with previous op: the previous instruction only reads from $v04
    

  * Similarly, CFC2/CTC2 (SU instructions) can prevent dual-issue if they access a control register that is read/written by the instruction they dual-issue with. In this case, the RSP is a bit overbroad because VU instructions that access the control register are counted as both reading and writing it, even though they only read them.


    
    
    ctc2 t0, VCO
    vadd $v00, $v01, $v02       # NO DUAL ISSUE: vadd reads from VCO, which was written by the ctc2
    
    
    
    vmrg $v00, $v01, $v02       # NOTE: vmrg only reads from VCC
    ctc2 t0, VCC                # NO DUAL-ISSUE: when pairing with ctc2, first instruction is treated as if it was also writing to VCC
    

### Dual-issue: hardware bug with single-lane instructions

There is a hardware bug in RSP that prevents dual-issuing when using single-lane instructions as second instruction of the pair in special cases. The single-lane instructions affected by this bug are: `VRCP`, `VRCPL`, `VRCPH`, `VMOV`, `VRSQ`, `VRSQL`, `VRSQH`, `VNOP`. 

The bug is related to the interpretation of the `de` field of the opcode, which is treated as a register number (rather than an element modifier) for the purpose of checking read/write conflicts. 

This is better explained with an example:
    
    
    mtc2 t0, $v04.e2
    vrcp $v01.e4, $v02.e7       # NO DUAL ISSUE: hardware bug: "e4" wrongly treated as a reference to "$v04"
    

The above code would normally dual-issue. The first instruction writes to VU register $v04, while the second instruction reads and writes from different registers ($v01 and $v02), so in theory there should be no conflict. Unfortunately, the hardware bug triggers here: the field in the opcode that encodes "e4" is misinterpreted by the RSP internal dual-issue conflict logic, and it believes that the instruction references "$v04" instead: so it does create a conflict with the previous one, that prevents dual-issue. Notice that using the modern GCC syntax (`.e0` \- `.e7`), it is possible to create conflicts only with registers `$v00` \- `$v07`. At the hardware level, though, it is possible to specify a full 5-bit index number (from 0 to 31), which is exposed via the old SGI syntax. For instance, these instructions use different opcodes encoding but produce exactly the same result:
    
    
    vrcp $v01[e4], $v02[e7]
    vrcp $v01[e12], $v02[e7]      # Same as $v01.e4
    vrcp $v01[e20], $v02[e7]      # Same as $v01.e4
    vrcp $v01[e28], $v02[e7]      # Same as $v01.e4
    

So a solution to manually workaround the dual-issue conflict is to switch to a different element encoding:
    
    
    mtc2 t0, $v04.e2
    vrcp $v01[e12], $v02[e7]       # DUAL ISSUE: will still write to $v01.e4, but the hardware bug sees it as a fake reference to $v12
