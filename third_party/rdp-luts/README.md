# rdp-luts

`luts.hpp` comes from paraLLEl-RDP by Themaister (MIT, see `LICENSE`), upstream commit
`1cecd042b2619bc505c12bfdc713808386f2b54d`. It is the only file kept from that tree since
the parallel-RDP backend was retired in favour of GPU-RDP (docs/GPU-RDP.md).

Only `RDP::blender_lut` is used: the RDP blender's divider, with the hardware's quirks
(wrap past 255, fixed output for some sums). It is a measured table, not a formula, so
both SoftRDP (`src/rdp/rdp.cpp`) and GPU-RDP (`src/gpurdp/gpurdp.cpp`) read it as is.
