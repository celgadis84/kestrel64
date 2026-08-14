# N64 Texture Formats — decode reference + kestrel64 conformance

RDP texel formats. Each entry: bit layout → 32-bit RGBA8888 output, plus the
conformance status of the current SoftRDP decoder in `src/rdp/rdp.cpp`
(`sampleTexel`, expansion helper `exp5` at line 40).

The N64 has **6 texel formats** (RGBA, YUV, CI, IA, I) × **4 bit-sizes**
(4/8/16/32b). Not all combinations are legal; the common ones are below.

---

## Endianness

Texel data in TMEM is **big-endian** (N64 byte order). On an x86 host every
multi-byte read must reassemble bytes explicitly, MSB first — never a raw
`*(u16*)`/`*(u32*)` cast. kestrel does this per-byte in the decoders:

```cpp
// RGBA16: two BE bytes → u16
u32 px = ((u32)tmem[off] << 8) | tmem[off+1];
// RGBA32: four BE bytes → RGBA directly
return (tmem[off]<<24)|(tmem[off+1]<<16)|(tmem[off+2]<<8)|tmem[off+3];
```

---

## 5-bit → 8-bit expansion (bit replication, NOT ×255/31)

A 5-bit channel expands to 8-bit by **replicating the high bits into the low**,
not by linear `v*255/31`. Matches angrylion `tmem.c`.

```cpp
static inline u32 exp5(u32 v) { return (v << 3) | (v >> 2); }   // rdp.cpp:40
```

`0x1f → 0xff`, `0 → 0`, midpoints land right. Linear scaling drifts ~±4 LSB and
fails checkerboard/gradient tests. **This is the single most common color bug.**

---

## Formats

### I4 — 4-bit intensity
Nibble `I`. Grey, **alpha = intensity** (I copied into A).

```
I8 = (I4 << 4) | I4   == I4 * 17     // 0..15 → 0,17,34,...,255
out = I8 in R,G,B AND A
```
kestrel `rdp.cpp:436` — `i = nib*17; return (i<<24)|(i<<16)|(i<<8)|i;` ✅ matches.

### IA4 — 3-bit intensity + 1-bit alpha
Bit layout `IIIA`.

```
I  = (nib >> 1) & 7
I8 = I * 255 / 7
A  = (nib & 1) ? 255 : 0
```
kestrel `rdp.cpp:434` — `i=(((nib>>1)&7)*255)/7, a=(nib&1)?255:0`. ✅ matches.

### CI4 — 4-bit color-index → TLUT
Nibble indexes the palette (`palette` field picks the 16-entry bank). TLUT entry
decoded as RGBA5551 or IA16 by `tlutMode`.
kestrel `rdp.cpp:433` → `tlutLookup(...)`. ✅

### I8 — 8-bit intensity
Byte `I`. Grey, **opaque** (A=255).

```
out = I in R,G,B; A = 255
```
kestrel `rdp.cpp:477` — `((u32)v<<24)|(v<<16)|(v<<8)|v`. ✅
(Note: I8 → A=255 opaque; contrast I4 → A=intensity.)

### IA8 — 4-bit intensity + 4-bit alpha
```
I8 = (v>>4)*17;  A = (v&0xf)*17
```
kestrel `rdp.cpp:475`. ✅

### CI8 — 8-bit color-index → 256-entry TLUT
kestrel `rdp.cpp:474` → `tlutLookup(v)`. ✅

### IA16 — 8-bit intensity + 8-bit alpha
```
I = hi byte;  A = lo byte
```
kestrel `rdp.cpp:457`. ✅

### RGBA16 (5551) — 16-bit
Bit layout `RRRRR GGGGG BBBBB A`.

```
R = exp5((px>>11) & 0x1f)
G = exp5((px>>6)  & 0x1f)
B = exp5((px>>1)  & 0x1f)
A = (px & 1) ? 255 : 0
```
kestrel `rdp.cpp:459-463`. ✅ Uses `exp5` bit-replication (not ×255/31).

### RGBA32 (8888) — 32-bit
Four BE bytes R,G,B,A direct — no expansion.
kestrel `rdp.cpp:465-468`. ✅

### YUV (16-bit) — YUYV 4:2:2
Y per texel, U/V shared across pair; YUV→RGB in the color combiner path.
kestrel `rdp.cpp:442`. ✅ (present; used by video/movie microcode only)

---

## TMEM word-swapping (the "diagonal bands" gotcha)

On `LoadBlock`/`LoadTile`, the RDP writes TMEM in **64-bit words** and **swaps
the two 32-bit halves on odd TMEM lines** (and 32-bit textures split across the
lo/hi TMEM banks: RG in low half `0x000-0x7ff`, BA in high half `0x800-0xfff`).

If a texture renders as **diagonal stripes / checkerboard** but the colors are
otherwise right, the bug is **TMEM interleave on load**, NOT the texel color
decode above. Look at the load path (`LOAD_BLOCK`/`LOAD_TILE`, rdp.cpp ~692-728),
not `sampleTexel`.

Reference oracles for this: angrylion `tmem.c` (local checkout
`ares-64/thirdparty/angrylion-rdp-plus/src`), n64brew wiki "Texture loading".

---

## Conformance summary

| Format | Layout | kestrel line | Status |
|--------|--------|--------------|--------|
| I4     | I, A=I | 436 | ✅ |
| IA4    | 3I/1A  | 434 | ✅ |
| CI4    | idx→TLUT | 433 | ✅ |
| I8     | I, A=255 | 477 | ✅ |
| IA8    | 4I/4A  | 475 | ✅ |
| CI8    | idx→TLUT | 474 | ✅ |
| IA16   | 8I/8A  | 457 | ✅ |
| RGBA16 | 5551, exp5 | 459 | ✅ |
| RGBA32 | 8888 direct | 465 | ✅ |
| YUV    | YUYV 4:2:2 | 442 | ✅ |

All texel color decoders present and matching HW semantics (bit-replication
expansion, BE byte order, alpha rules per format). Open item is TMEM load-time
interleave correctness for large/32-bit textures (load path, not decode).
