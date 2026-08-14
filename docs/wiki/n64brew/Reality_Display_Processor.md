# Reality Display Processor

From N64brew Wiki

Jump to navigation Jump to search

The **Reality Display Processor** , or **RDP** , is the portion of the [RCP](https://n64brew.dev/wiki/RCP "RCP") responsible for graphics tasks such as Z-buffering, texturing, blending, anti-aliasing, etc. It contains 4KB of texture memory (TMEM,) and is sent commands called "primitives". 

## RDP interface

Main article: [Reality Display Processor/Interface](https://n64brew.dev/wiki/Reality_Display_Processor/Interface "Reality Display Processor/Interface")

The RDP interface is made of several memory-mapped registers that allows the VR4300 and RSP to control the RDP. Both VR4300 and RSP are in fact able to control RDP execution by accessing the interface: VR4300 through memory mapped registers, while RSP through its COP0. 

## RDP commands

Main article: [Reality Display Processor/Commands](https://n64brew.dev/wiki/Reality_Display_Processor/Commands "Reality Display Processor/Commands")

RDP processes a display list made of commands. Most commands are 64-bit words, though some are bigger. Command lists can be sent to RDP via DMA (either from RDRAM or DMEM).
