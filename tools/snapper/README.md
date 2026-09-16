# snapper64 reference reader

`snapper64.z64` compares each test surface **byte for byte** against a reference captured on
real hardware. Those references travel inside the ROM, so they can be read on the host without
running anything.

## Extract the two files from the ROM

They live in the libdragon DFS appended to the ROM:

```sh
dumpdfs -l snapper64.z64                        # tests.pack  +  tests.pack.idx
dumpdfs -e snapper64.z64 tests.pack     > tests.pack
dumpdfs -e snapper64.z64 tests.pack.idx > tests.pack.idx
```

`dumpdfs` is a libdragon host tool (`libdragon/tools/dumpdfs`). It is Linux-only here, so run
it under WSL.

## Read a reference surface

```sh
python dump.py            # prints the reference for one Fill-Mode-Tri sweep test
```

```python
import dump
data = dump.ref('RDP Rect-Tri - Slopes', 'Tri-Rect ISL 186', 2)   # group, test, assert id
```

`data` is the raw surface exactly as hardware left it (RGBA32, big-endian words).

## Format

Both files are libdragon `DCA3` assets — header version 3, algorithm 3 (Shrinkler):

| Field | Size |
|---|---|
| `magic` = `"DCA3"` | 4 |
| `algo` (3 = Shrinkler) | u16 |
| `flags` | u16 |
| `cmp_size` | u32 |
| `orig_size` | u32 |
| `inplace_margin` | u32 |

`tests.pack.idx` decompresses to `{u32 fileCount, {u32 groupHash, u32 testHash, u32 offset}[]}`,
sorted by `(groupHash, testHash)` so the ROM can binary-search it. Several entries share an id —
one per assert in that test, in order. Bit 31 of `offset` means "the entry's size is odd"; the
size itself is the next entry's offset minus this one's (minus 1 when that bit is set).
`tests.pack` is those entries concatenated, each one a `DCA3` asset of its own.

The hashes are plain zlib CRC32 of the group and test name.

`shr.py` is a port of libdragon's Shrinkler decoder (`src/compress/shrinkler_dec.c`).
