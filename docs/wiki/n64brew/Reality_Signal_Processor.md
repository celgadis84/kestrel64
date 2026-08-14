# Reality Signal Processor

From N64brew Wiki

Jump to navigation Jump to search

The **Reality Signal Processor** , or **RSP** , is the portion of the RCP responsible for matrix math, lighting calculations, clipping, shading, and other highly parallel graphics tasks as well as audio processing. It is a programmable MIPS processor with a custom set of SIMD instructions for vectorized fixed point operations (exposed as COP2 -- a group of reserved instructions in the standard MIPS instruction set). The RSP is also able to directly drive the RDP (the hardware rasterizer) by accessing its registers, so that it can terminate the graphic pipeline by telling the RDP to draw triangles into the framebuffer. 

RSP has two different banks of onboard dedicated memories: IMEM (4KB) for instructions, and DMEM (4KB) for data. It has no external memory buses but has a DMA engine capable to copy code/data from/into DMEM/IMEM and the main RDRAM. The DMA engine can be driven by either the main CPU or the RSP itself. 

The code running on the RSP is usually called "microcode", but it's a standard MIPS program. The RSP can be programmed in custom microcode to handle specific tasks, though most commercial games leveraged one of several stock microcodes made available by Nintendo at the time. 

## Contents

  * 1 Specs
  * 2 RSP CPU core
  * 3 RSP CPU pipeline
  * 4 RSP interface



## Specs

| Discription   
---|---  
CPU Type  | Cut down version of the MIPS4000 CPU   
Clock Speed  | 62.5mhz   
Instruction Size  | 32bit (1 Word)   
Dual Instruction  | Yes (one scalar and one vector opcode at once)   
Pipeline Stages  | 5 stage pipeline for both the Scalar and Vector Pipelines IF, RD and WB stages are shared between the two pipelines   
IMEM Data Path  | 64bit (This allows a dual instruction to happen) This can only be double word aligned for reads   
Scalar Register Size  | 32 entries of 32bit in size (Word Writable)   
Vector Register Size  | 32 entries of 128bit is size (8bit to 128bit Mask Writable File)   
DMEM Scalar Data Path  | Up to 32bit Loads and Stores   
DMEM Vector Data Path  | Up to 128bit Loads and Stores   
Scalar ALU Size  | 32bit in side only   
Vector ALU Size  | 8x 16bit vector ALU pipelines (48bit Final Accumulator)   
  
## RSP CPU core

Main article: [Reality Signal Processor/CPU Core](https://n64brew.dev/wiki/Reality_Signal_Processor/CPU_Core "Reality Signal Processor/CPU Core")

The RSP CPU core is made by a stripped-down MIPS 32-bit core (without a few more advanced opcodes) referred to as Scalar Unit (SU), composed with a coprocessor (configured as COP2) that can perform SIMD operations on a separate set of vector registers, referred to as Vector Unit (VU). 

## RSP CPU pipeline

Main article: [Reality Signal Processor/CPU Pipeline](https://n64brew.dev/wiki/Reality_Signal_Processor/CPU_Pipeline "Reality Signal Processor/CPU Pipeline")

The RSP CPU pipeline is made of two different units: SU and VU. It allows to run two instructions in a single clock cycle, when following a specific coding pattern. 

## RSP interface

Main article: [Reality Signal Processor/Interface](https://n64brew.dev/wiki/Reality_Signal_Processor/Interface "Reality Signal Processor/Interface")

The RSP interface is made of several memory-mapped registers and memory areas that allows the VR4300 to control the RSP. VR4300 is able to read and write to the internal IMEM/DMEM memory of the RSP to be able to upload the microcode to be run and fetch the results if required.
