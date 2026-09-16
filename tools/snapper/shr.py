"""Port of libdragon's Shrinkler streaming decoder (src/compress/shrinkler_dec.c).

Enough to unpack snapper64's rom:/tests.pack entries on the host: each entry is a
standalone libdragon "DCA3" asset (magic[4], u16 algo, u16 flags, u32 cmp_size,
u32 orig_size, u32 inplace_margin) followed by the compressed stream.
"""
import struct

ADJUST_SHIFT = 4
NUM_SINGLE = 1
NUM_CONTEXTS = NUM_SINGLE + 4 * 256
GRP_OFFSET, GRP_LENGTH = 2, 3


class Shr:
    def __init__(self, data):
        self.d = data
        self.p = 0
        self.ctx = [0x8000] * NUM_CONTEXTS
        w = self.u32()
        self.value = w << 31
        self.bits = 1
        self.size = 0x8000

    def byte(self):
        if self.p < len(self.d):
            b = self.d[self.p]
            self.p += 1
            return b
        return 0                      # the decoder may read past the end

    def u32(self):
        b = self.byte
        return (b() << 24) | (b() << 16) | (b() << 8) | b()

    def bit(self, ci):
        while self.size < 0x8000:
            if self.bits == 0:
                self.value |= self.u32()
                self.bits = 32
            self.bits -= 1
            self.size = (self.size << 1) & 0xffffffff
            self.value = (self.value << 1) & 0xffffffffffffffff
        prob = self.ctx[ci]
        v = self.value >> 48
        thr = (self.size * prob) >> 16
        if v >= thr:
            self.value -= thr << 48
            self.size -= thr
            self.ctx[ci] = prob - (prob >> ADJUST_SHIFT)
            return 0
        self.size = thr
        self.ctx[ci] = prob + (0xffff >> ADJUST_SHIFT) - (prob >> ADJUST_SHIFT)
        return 1

    def number(self, base):
        i = 0
        while self.bit(base + i * 2 + 2):
            i += 1
        n = 1
        while i >= 0:
            n = (n << 1) | self.bit(base + i * 2 + 1)
            i -= 1
        return n

    def lz(self, c):
        return self.bit(NUM_SINGLE + c)

    def lznum(self, g):
        return self.number(NUM_SINGLE + (g << 8))


def shrinkler_decode(data, size):
    s = Shr(data)
    out = bytearray()
    ref = False
    prev_ref = False
    off = 0
    while len(out) < size:
        if not ref:
            parity = len(out) & 1
            c = 1
            for _ in range(8):
                c = (c << 1) | s.lz((parity << 8) | c)
            out.append(c & 0xff)
            prev_ref = False
        else:
            repeated = False
            if not prev_ref:
                repeated = s.lz(-1) != 0        # CONTEXT_REPEATED == -1
            if not repeated:
                off = s.lznum(GRP_OFFSET) - 2
                if off == 0:
                    break
            n = s.lznum(GRP_LENGTH)
            src = len(out) - off
            for i in range(n):
                out.append(out[src + i])
            prev_ref = True
        parity = len(out) & 1
        ref = s.lz(0 + (parity << 8)) != 0      # CONTEXT_KIND
    return bytes(out)


def asset_load(blob, at=0):
    """Decode one DCA3 asset starting at `at`. Returns (data, total_bytes_consumed)."""
    magic, algo, flags, cmp_size, orig, margin = struct.unpack_from('>4sHHIII', blob, at)
    assert magic == b'DCA3', magic
    assert algo == 3, ('only shrinkler supported here', algo)
    body = blob[at + 20: at + 20 + cmp_size]
    return shrinkler_decode(body, orig), 20 + cmp_size
