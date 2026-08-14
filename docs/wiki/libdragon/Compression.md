This page describes how libdragon achieves data compression in general for different type of assets (executables, graphics, audio). It gives some general guidance and explains the rationale of the various defaults.

## Overview of compression in libdragon

Libdragon ships with three different data compression algorithms, generically identified by level numbers from 1 to 3 (with "level 0" sometimes being used to refer to "no compression"). Increasing the level means slower compression during builds, slower decompressions at runtime, but more compression ratio.

The actual algorithm behind each level is considered an implementation detail; as new algorithms are tested on N64, libdragon can change the algorithms at any point, even in the stable branch (that is, a change of compression algorithm is not considered a breaking change). It is thus important that compression of data happens during build, and compressed data **is not committed to the game repository**. Such pre-compressed, committed data might turn out to be unusable with future versions of libdragon.

 * **Level 1**: This is the fastest compression level. The ratio is not great (normally worse than gzip, just to give a ballpark) but it is so fast at decompressing that it achieves an important property: data compressed at level 1 is **faster at loading and decompressing, than loading uncompressed data**. This surprising result happens because the time saved by loading fewer bytes of data from ROM is enough to overcome the time spent decompressing the data. This property has been proven true for so many different kinds of data that it can be considered true for all data. Currently, the algorithm used is [LZ4 by Yann Collet](https://lz4.org).
 * **Level 2**: This is a good intermediate compression level. The ratio is quite good (better than `gzip -9` in most cases), but it is still decently fast at decompressing. It is probably the best algorithm to use for final builds, where you want to release a ROM and reduce its size without impacting loading time. Using it during development cycle can be a bit cumbersome because compression time (during build) is much much slower than level 1, so it can impact testing speed. Currently, the algorithm is [Aplib](https://ibsensoftware.com/products_aPLib.html).
 * **Level 3**: This is an ultra high compression level. It is tuned for absolutely maximum compression at the expense of decompression time. The compression ratio is incredibly high, and it approaches best-in-class on PC too (like xz or 7zip), thanks also to the fact that N64 files are generally quite small by today's standard. Decompression time is quite high though: turning it on for all assets will definitely noticeably increase loading times for your game, and possibly make them too long. It is mostly meant to be used for rarely loaded assets (e.g. error screens) or in situations when loading times are not important. It uses the [Shrinkler](https://github.com/askeksa/Shrinkler) algorithm

This is a table that recaps some data points and gives some advice:

| Level | Size | Ratio | Size vs gzip | Load time | Load speed | Notes |
| ---- | --- | --- | -- | -- | -- | -- |
| 0 | 1024 KiB |  100% | --     | 220 ms | 4652 KiB/s | Uncompressed data. This is normally never used, given that level 1 is faster |
| 1 | 364 KiB  | 35,5% | +9.6%  | 156 ms | 6577 KiB/s | Faster than uncompressed, used by default on everything |
| 2 | 309 KiB  | 30.2% | -6.9%  | 237 ms | 4318 KiB/s | Good for assets loaded during level changes; might not be fast enough for data streamed during game |
| 3 | 284 KiB  | 27,7% | -14.5% | 1133 ms | 904 KiB/s | Loading is very slow; good for assets loaded very rarely like error screen, or for situations where loading time is not a huge issue |

The corpus of 1 MiB is made of mixed game assets (mainly pictures in various pixel formats). For comparison `gzip --best` compresses the corpus to 332 KiB, and `xz --best` to 292 KiB. The load time is measured as the total time for the `asset_load()` function to return, so includes the whole asset loading process (filesystem lookup, memory allocation, file reading, and decompression) and all the internal overheads: it is really the real world loading time to be expected in games.

## Compressing and loading images (sprites, textures) and fonts

Normally, images are processed via the [`mksprite`](https://github.com/DragonMinded/libdragon/wiki/Mksprite) tool and converted into the `.sprite` format. Fonts instead are processed via the [`mkfont`](https://github.com/DragonMinded/libdragon/wiki/Mkfont) tool and converted into the `.font64` format.

Both of these formats accept the command line option `-c` or `--compress` which allows to specify the compression level to be used for these files. By default, level 1 is used. To change the compression level, you can add the option to your Makefile like this:

```Makefile
filesystem/%.sprite: assets/%.png
	@mkdir -p $(dir $@)
	@echo "    [SPRITE] $@"
	@$(N64_MKSPRITE) --compress 2 -o "$(dir $@)" "$<"
```

Note that libdragon does not support any form of lossy image compression at the moment (eg: jpeg).


## Compressing arbitrary data files

Libdragon compression library can be used also to compress your own arbitrary files. To do so, you must use the `mkasset` tool to compress the files, and then load them using the asset library ([asset.h](https://github.com/DragonMinded/libdragon/blob/trunk/include/asset.h)). 

### Compressing the files 

Invoking `mkasset` manually is quite easy:

```bash
$ $N64_INST/bin/mkasset --compress 1 -o filesystem myfile.dat
```

By default, the file will be overwritten with the compressed version, or you can use `-o` to specify an output directory. You can even pass multiple files on the command line if you want.

Invoking mkasset from a Makefile can be done with a custom rule:

```Makefile
filesystem/%.dat: assets/%.dat
	@mkdir -p $(dir $@)
	@echo "    [DAT] $@"
	@$(N64_BINDIR)/mkasset --compress 2 -o "$(dir $@)" "$<"
```

The above rule will compress via `mkasset` all `.dat` files in the `assets/` directory, and put the compressed files into the `filesystem` directory.

### Loading the compressed files 

Before loading the assets, you must initialize the compression algorithms. This is required so that libdragon can avoid linking in too much code into the final game: if a certain compression algorithm is not used by your assets, it will no the linked. To initalize each compression algorithm, use `asset_init_compression`:

```C
// Initialize compression level 3
asset_init_compression(3);
```

Notice that compression level 1 is initialized by default, so you don't need to do anything if you use the default compression level.

The asset library provides two main entrypoints: `asset_load()` and `asset_fopen()`.

`asset_load()` just loads the whole file into memory, uncompresses it and return a pointer to it:

```C
int sz;
void *data = asset_load("rom:/mydata.dat", &sz);
```

It also returns the (uncompressed) size of the asset in case it is useful. The returned buffer can be disposed via `free()`.

`asset_fopen()` can be used to parse the file as it is being read (and uncompressed) from disk, without reading it all into RAM:

```C
int sz;
FILE *f = asset_fopen("rom:/mydata.dat", &sz);

char id[3];
fread(id, 1, 3, f); 
if (memcmp(id, "ID", 3) != 0) {
   ....
}

[ ... proceed reading the file ... ]
```

Both `asset_load` and `asset_fopen` also works on uncompressed files, not processed via mkasset. This is made it so you can start using them from day zero has a way to load your data into the game, and then later on add compression if needed.

Notice that `mkasset` default parameters are skewed towards allowing a performant `asset_fopen()`, so it forces compression algorithms to use a very small window (4 KiB). If you load your data files using `asset_load()` exclusively, you can probably obtain slightly better compressions by adding also `--window 256` to the `mkasset` call.

## Compressing game code

Libdragon bootcode (IPL3) supports decompression of game code at boot time. Libdragon makefiles using n64.mk are already setup for that: by default, it compresses the game code with level 1 compression, which makes the ROM actually boots faster than uncompressed.

To change the compression level of game code, simply adds this variable to your Makefile:

```Makefile
# Set game code compression to level 2
N64_ROM_ELFCOMPRESS = 2
```

DSOs (overlays, that is dynamic libraries of code that is loaded at runtime) are also compressed by default with level 1. To change that, a separate Makefile variable is used:

```Makefile
# Set DSOs compression to level 2
DSO_COMPRESS_LEVEL = 2
```

This is an example testing the various compression levels on the "Brew Volley" game example in libdragon:

| Level | Size | Ratio | Compression time | Boot time |
| --  | -- | -- | -- | -- |
| 0 | 412 KiB | 100.0% | 0.00 s | 139 ms |
| 1 | 245 KiB |  59.5% | 0.39 s | 112 ms |
| 2 | 177 KiB |  43.0% | 2.93 s | 161 ms |
| 3 | 155 KiB |  37.6% | 6.09 s | 647 ms |

Compression times were measured on an Apple MacBook Pro 2021 with M1 Pro. Boot time is the time at which the N64 calls the C `main()` function, so it also includes the game code decompression.

Notice that compression times do impact the development cycle when activated in the Makefile, as the time is paid every time a new ROM is made. It is advised to keep compression at level 1 for the standard development cycle.

For comparison, `gzip --best` produces a 188 KiB file, and `xz --best` produces a 150 KiB file.
