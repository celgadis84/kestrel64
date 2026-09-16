import os, struct, zlib, sys, shr

# Where `dumpdfs -e` left tests.pack / tests.pack.idx (see README.md).
DIR = os.environ.get('SNAPPER_PACK_DIR', os.path.dirname(os.path.abspath(__file__)))

idx, _ = shr.asset_load(open(os.path.join(DIR, 'tests.pack.idx'), 'rb').read())
N = struct.unpack_from('>I', idx, 0)[0]
ENT = [struct.unpack_from('>III', idx, 4 + i * 12) for i in range(N)]
PACK = open(os.path.join(DIR, 'tests.pack'), 'rb').read()


def ref(group, test, assertId=1):
    g = zlib.crc32(group.encode()) & 0xffffffff
    t = zlib.crc32(test.encode()) & 0xffffffff
    for i, (gg, tt, off) in enumerate(ENT):
        if gg == g and tt == t:
            i += assertId - 1
            base = ENT[i][2] & 0x7fffffff
            data, _ = shr.asset_load(PACK, base)
            return data
    raise KeyError(group + '/' + test)


if __name__ == '__main__':
    d = ref('RDP Fill Mode Tri (Sweep)', 'Sweep 0 | 0.00 | 0.00')
    print('bytes', len(d))
    W = H = 128
    for y in range(0, 12):
        row = ''.join('%08x ' % struct.unpack_from('>I', d, (y * W + x) * 4)[0]
                      for x in range(118, 128))
        print('y=%2d ' % y, row)
