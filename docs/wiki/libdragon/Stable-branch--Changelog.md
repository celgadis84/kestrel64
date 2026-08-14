# Changelog of the stable branch.

This pages lists all main features that have been merged to the stable branch (`trunk`), and is updated quarterly. Bug fixes or small feature tweaks are not listed here. If you want to see the upcoming features, have a look at the [preview branch](https://github.com/DragonMinded/libdragon/wiki/Preview-branch) which is where features are developed and stabilized.

## 2026 Q1

### EEPROM write behaviour fix

Thanks to improved understanding of the EEPROM hardware on real cartridges, we were able to improve EEPROM write behaviour in Libdragon. It turns out that a real EEPROM can be busy for up to 6 milliseconds which is long enough to fail writes if accessed too often and to cause a noticeable stutter which must be handled. This behaviour is now also properly emulated in Ares v148 or newer so older versions of libdragon will (accurately) fail to create an EEPROM save on emulator.

## 2025 Q4

### Hardware-accelerated text font rendering

Libdragon now features comprehensive import and rendering support for text fonts. Fonts can be converted directly from TrueType (TTF), OpenType (OTF), OpenType SVG color fonts, and BMFont assets into the native `.font64` format with the new `mkfont` tool. The conversion pipeline automatically detects monochrome and anti-aliased fonts, generates optimized texture atlases within N64 hardware constraints, preserves kerning information, and supports optional outlines with configurable thickness. Runtime-configurable colors are available for monochrome and anti-aliased fonts, while color fonts are rasterized and packed using selectable texture formats for maximum flexibility and memory efficiency.

For more information, please refer to the [Mkfont](https://github.com/DragonMinded/libdragon/wiki/Mkfont) wiki page.

## 2025 Q3

### New controller module (`joypad`) with first-class GameCube controller support

The [`joypad`](https://github.com/DragonMinded/libdragon/blob/trunk/include/joypad.h) module is a new abstraction to unify N64 and GameCube Joypad input styles into a more-common interface. GameCube controllers are now fully and transparently supported, including dual analog stick input, as well as other Joypad devices of all shapes and sizes, and their accessories. Performance is also improved with the usage of synchronous Joybus commands replaced with an asynchronous model so that games aren't burning CPU cycles waiting on a slow serial interface.

### Memory usage information

You can now call `sys_get_heap_stats` to inspect heap usage of your game.

## 2025 Q1

### Opus audio decompression

WAV64 now supports real-time Opus (CELT) decompression that can be used for compressed audio. This is the same state-of-the-art audio algorithm that is currently mainstream on PCs. `audioconv64` has grown support for compressing wav64 files with Opus using `--wav-compress 3` (level 3 compression) option.

Compression ratios are around 15:1 for audio files which makes them 4 or 5 times smaller than their VADPCM counterparts. At runtime, all wav64 can be played back exactly in the same way, irrespective of the compression level. The only difference is that you must explicitly initialize playback of Opus files by calling `wav64_init_compression(3)`. This basically tells libdragon to link support for decompression of level 3 WAV64 files.

For more information about Opus in libdragon (including benchmarks), please refer to the [Opus decompression](https://github.com/DragonMinded/libdragon/wiki/Opus-decompression) wiki page.

### Dynamic library support (DSOs)

Libdragon now supports the creation of dynamic libraries (sometimes called "overlays"), that allows to load and unload portions of code at runtime. This allows to reduce memory consumption for parts of code which are not necessary to be always available. For instance, each actor could be compiled into its own dynamic library, and loaded / unloaded depending on the game area where the player is.

Dynamic libraries have the `.dso` extension, and can be loaded using the standard POSIX API `dlopen()`. Functions in the dynamic library can be accessed via `dlsym()` and then called through a normal function pointer. To create a DSO, use the new `n64dso` tool. For more information, please refer to the [DSO](https://github.com/DragonMinded/libdragon/wiki/DSO-(dynamic-libraries)) wiki page.

## 2024 Q4

### Improved boot (open-source IPL3)

The IPL3 (boot code part of the ROM) is now a fully [open-source, clean-room implementation](https://github.com/DragonMinded/libdragon/tree/trunk/boot) with many, many improvements over the proprietary one that was used in commercial games. It boots about 5 times faster than before (eg: a 350 KiB ELF file is booted in ~100 ms rather than ~550 ms) and supports decompressing ELF files on the fly. Moreover, it is not necessary to calculate a ROM checksum anymore as we felt that the checksum is not useful anymore in the modern world, which simplifies building a Z64 file.

Thanks to [ELF file compression](https://github.com/DragonMinded/libdragon/wiki/Compression#compressing-game-code) and the absence of a checksum, simple ROMs are much smaller than before. A basic hello world is now ~150 KiB, for instance. This speeds up real hardware testing, especially when using flashcards with SD or with slow USB (like Everdrive 64).

You can refer to the [README file](https://github.com/DragonMinded/libdragon/blob/trunk/boot/README.md) for more details on IPL3. As a developer, you don't need to do anything special to use it or benefit from it. Just build your ROM using up-to-date libdragon, and the new IPL3 will be used automatically by the build system, and the main ELF binary will also be automatically compressed. 

## 2024 Q2

### VADPCM support for WAV64 and audioconv64 improvements

WAV64 now supports RSP-accelerated VADPCM decompression at runtime. VADPCM is a variant of ADPCM that is amenable of being accelerated with SIMD instructions such as those on the RSP processor. `audioconv64` has grown support for compressing wav64 files in VADPCM format through the new `--wav-compress` option. Since VADPCM is extremely fast at runtime, we activate it by default in `audioconv64`.

`audioconv64` now also supports `--wav-resample` to resample the input WAV file during conversion. Resampling is performed using state-of-the-art algorithms with the same quality level of professional DAWs. This allows to keep a high-quality version of the original asset in the repository, and change the resampling quality during the build, adjusting the size / quality ratio at will. 

Moreover, `audioconv64` now also accepts MP3 files as source files instead of plain WAVs, to keep the high quality version of music tracks smaller.


### New `fmath.h` library

A new library [`fmath.h`](https://github.com/DragonMinded/libdragon/blob/trunk/include/fmath.h) has been added to provide fast approximations of mathematical functions that are useful in the game programming context.

Moreover, `n64.mk` now enables `-ffast-math` by default, which is meant to provide a better programming environment for (retro) game programming, in addition to producing faster code.

### `get_ticks()` now returns a 64-bit counter

`get_ticks()` now returns a 64-bit counter that never wraps. The previous version was identical to `TICKS_READ()` and returned a 32-bit counter that overflows every ~90 seconds. This means that you can safely use it without worrying of overflow.

The `timer_ticks()` function used to be more complicated and required the timer module to be initialized, but it is now just an alias of `get_ticks()`.

### `dumpdfs` improvements

You can now run `dumpdfs` to list and exact filesystem contents directly on a `.z64` file (instead of only a `.dfs` file). `dumpdfs` will in fact search for the filesystem embedded in the ROM and parse it.

### Improve asset library algorithms

The asset library has been improved by changing the level 2 algorithm (from lzh5 to aplib/apultra) and adding a new level 3 algorithm (Shinkler). Level 2 algorithm is now a compelling choice because it is only a bit slower than level 1 (LZ4) while providing a much higher compression ratio (similar or better to gzip). On the other hand, level 3 has best in class ratio (similar to xz on PC), and is quite slow at decompressing, so it can be a good choice for cold data.

See our [wiki page on compression](https://github.com/DragonMinded/libdragon/wiki/Compression) for more information and benchmarks.

Remember that compression algorithms and bitstreams are [**not** guaranteed to be stable](https://github.com/DragonMinded/libdragon/wiki/API-stability#general-overview) so you should not commit compressed files as source assets in your repository. Always commit the original files and let the compression happens as part of the build pipeline.

### Add FPS calculator

You can now call [`display_get_fps()`](https://github.com/DragonMinded/libdragon/blob/51326fc9dc01b1e8a6eada231557d70ae5fb3391/include/display.h#L265-L270) to acquire the current framerate. This is a very common request, especially while developing.

### Added `audio_push` API

A new [`audio_push()`](https://github.com/DragonMinded/libdragon/blob/51326fc9dc01b1e8a6eada231557d70ae5fb3391/include/audio.h#L179-L205) has been added to the low-level audio library, to simplify the task of pushing samples out to the hardware. This is an alternative to [`audio_write_begin()`](https://github.com/DragonMinded/libdragon/blob/51326fc9dc01b1e8a6eada231557d70ae5fb3391/include/audio.h#L147-L167) / [`audio_write_end()`](https://github.com/DragonMinded/libdragon/blob/51326fc9dc01b1e8a6eada231557d70ae5fb3391/include/audio.h#L169-L177), which are the zero-copy APIs for maximum performance but more stringent requirements. A third, older alternative (`audio_write()`) is now deprecated.


## 2024 Q1

### Added support for single display buffer

`display.h` now supports calling `display_init()` specifying a single framebuffer. This can be useful to display static images without allocating too much memory. Obviously, you will get tearing if you keep drawing while the framebuffer is displayed.

Moreover, `display_lock()` is now deprecated. Use `display_get()` instead, that blocks until a framebuffer is available.

### New sprite library and new mksprite tool

The sprite format, together with the `mksprite` generation tool, has been vastly enhanced to support:

 * All RDP pixel formats
 * Quantization to create best CI4/CI8 palettes from a true color image, using state-of-the-art algorithms
 * Dithering (random or ordered)
 * Generation of mipmaps
 * Embedding of texture parameters (repetitions, mirroring, etc.)
 * Compression

See the page dedicate to the [`mksprite` tool](https://github.com/DragonMinded/libdragon/wiki/Mksprite) for more information. The command line syntax is also changed to adhere the common standard for CLI tools (options prefixed with dashes, etc.), though the old syntax is silently accepted for full backward compatibility.

Together with these changes, a new [sprite library](https://github.com/DragonMinded/libdragon/blob/trunk/include/sprite.h) has been added. You can now call `sprite_load()` to load a sprite, and there are several functions to access all the above features (eg: access the single mipmaps, or the embedded palette).

```NOTE:``` Compatibility with existing codebases that call `mksprite` with the old syntax and manually loads sprites from the filesystem and cast their contents to `sprite_t` has been preserved, but is limited to the old functionalities. Once you switch to the new `mksprite` syntax, you need also to switch to the sprite library, or the code will not work anymore.


## 2023 Q4

### Upgrade builtin FatFS

Builtin FatFS has been upgraded to R0.15 Patch3.


## 2023 Q3

### New logo

Libdragon now has a nice logo, designed by Spooky Илюха and Cedar Branch:<br/>
<img src="https://github.com/DragonMinded/libdragon/assets/127010686/1167a1e7-6773-4a67-97d4-d251c12ef8ba" width="200">

See the wiki page about [Logos](https://github.com/DragonMinded/libdragon/wiki/Logos) for more information.

### Reworked display filtering options

`display_init` has been reworked to clarify the various filters performed by VI and rule out invalid configurations that might create video errors on specific configurations. The old `ANTIALIAS_*` macros have been deprecated (as in general antialias is just one of the possible filters) and in their place, you can now use the new `FILTER_*` macros:

 * `FILTER_NONE`: disable any kind of filtering; the framebuffer is scaled to the TV resolution just skipping pixels or duplicating them, with no interpolation at all.
 * `FILTER_RESAMPLE`: the framebuffer is scaled to the TV size using bilinear interpolation.
 * `FILTER_DEDITHER`: Post-process dithering as performed by RDP during triangle drawing, trying to reconstruct a 32-bpp cleaner output. Scaling is performed without bilinear.
 * `FILTERS_RESAMPLE_ANTIALIAS`: Post-process outer triangle edges to smooth them with the nearest pixels to avoid jagged lines; this is a crude form of anti-aliasing. Scaling is performed with bilinear filter.
 * `FILTERS_RESAMPLE_ANTIALIAS_DEDITHER`: Post-process using both dedither and antialiasing. Scaling is performed with bilinear filter.

### Mikmod is not built anymore by default

Libdragon previously always built the mikmod module player library. This was a very old port using CPU only, that doesn't perform very well. The new RSP mixer library together with the xm64 player is a superior option for a game (even if it supports only the .XM format, you can use tools OpenMPT to convert other module formats to XM, with minimal to none loss).

If you still need mikmod, you can build it manually using the provided script `tools/build-mikmod.sh`.

### New asset library

Libdragon now features a library to help loading asset files from the filesystem, with transparent compression support. The library is used via two main functions:

 * `asset_load()`: fully load a file from the filesystem into RDRAM, allocating the required memory from the heap. If the file is compressed, it is transparently decompressed during load.
 * `asset_fopen()`: return a FILE* which can be used to read and parse a file a bit at a time, with optional transparent decompression. This can be used for file formats that require parsing or for files that can be streamed.

To compress files, you can use the new `mkasset` tool. Currently two compression levels are support: level 1 which is very fast and with lower compression ratio, and level 2 which is slower but with higher compression ratio. The actual algorithms at the moment are LZ4 and LZH5 respectively but this is an implementation detail. Do not commit compressed assets to your repo as the compressed format can change at any time; instead, compress them during the build.

For more information, refer to the new [Compression](https://github.com/DragonMinded/libdragon/wiki/Compression) page in the wiki.

## 2023 Q2

### Stack traces on crashes and interactive crash inspector

In case of any crash (unhandled MIPS exceptions, unhandled C++ exceptions) or assertion, you will automatically be shown a full stack trace (include source code file names and lines). This is very useful to quickly pinpoint the reason for the crash.

<img width="752" alt="Schermata 2022-12-26 alle 15 55 22" src="https://user-images.githubusercontent.com/1014109/214957054-630276e3-5f6f-4753-b344-7ec2859bdbf2.png">

The crash screen has also been rehauled and it is now a multi-page inspector offering also a builtin disassembler, in addition to register dumps:
 
<img width="752" alt="Schermata 2022-12-26 alle 15 47 33" src="https://user-images.githubusercontent.com/1014109/214957173-4a18dfe6-61fc-47a2-9e7d-acd2c6145f12.png">

This happens fully automatic on recompilation after updating libdragon, provided that you use a n64.mk-based Makefile.

It is also possible to manually print a stack trace from a certain calling point using the new [`debug_backtrace()`](https://libdragon.dev/ref/debug_8h.html#a48a47644097877a8b22a70951445524d) function.

### Improved compatibility of SD card support

Libdragon's debug library allows to read and write files to SD cards supported by flashcarts, through a simple file-based API (`fopen("sd:/file.txt", "w")`). The support has been greatly enhanced in compatibility thanks to the integration of [libcart](https://github.com/devwizard64/libcart/tree/main). SD support is now available on:

 * 64Drive HW1 and HW2
 * EverDrive-64 V1, V2, V2.5, V3, X7 and X5
 * ED64Plus / Super 64
 * SummerCart64

### Improved USB debugging library

Libdragon's [vendored usb library](https://github.com/buu342/N64-UNFLoader/tree/master/USB%2BDebug%20Library) has been upgraded to the latest version, which offers better performance and supports connection / disconnection of the host tool, to ease debugging. If you are using UNFLoader to see the `debugf()` logs from libdragon, make sure to upgrade it to the latest version.

### Separate prefix for the toolchain

It is now possible to install the toolchain in a different prefix from libdragon itself. Use `$N64_GCCPREFIX` to specify the prefix for the toolchain, while leaving `$N64_INST` pointing to libdragon installation directory. If `$N64_GCCPREFIX` is not defined, it will default to `$N64_INST`.


## 2023 Q1

### Support for usage of `fatfs`

Years ago, libdragon started linking with fatfs, and this prevented software (such as flashcart menus) to bring their own copies of fatfs because of duplicated symbols failure. Now libdragon exports `fatfs.h`, so basically applications can rely on fatfs directly if they need to. Remember that for SD card access, you can use the [libdragon debug library](https://libdragon.dev/ref/group__debug.html), without needing to use fatfs directly.

The change also allowed files on the SD card to be generated using the correct time whenever a RTC is present. Simply call `rtc_init()` to initialize the RTC, and then create a file via `fopen("sd:/test.txt", "wb")`: you will see it with the correct timestamp.

### Binary toolchain built via CI

Historically, libdragon has provided a script to build a toolchain on a Linux system, but the user had to build the toolchain on their own computer. The only alternative for a long time was the usage of a Docker container. Now, libdragon builds via CI an [updated toolchain](https://github.com/DragonMinded/libdragon/releases/tag/toolchain-continuous-prerelease) that is uploaded on GitHub, for Windows and Linux (deb/rpm). 

Please make sure to follow the [installation instructions](https://github.com/DragonMinded/libdragon/wiki/Installing-libdragon).

### FPU Exceptions activated by default

Libdragon now activates MIPS FPU exceptions by default. This allows the game to instantly crash whenever there is a division by zero, or uninitialized memory is accessed by the floating point unit, which in turns helps catching bugs much earlier and with fewer surprises. When the exception screen triggers, it shows the PC address where the exception triggered, that you can lookup in the map file to see the name of the function.

## 2022 Q4

### Speedup boot

We improved the libdragon boot by using PI DMA instead of CPU writes to load the binary from ROM, and also avoiding loading some part that was preloaded by IPL3. The boot was speedup by about 200ms for a standard ROM.

### C++ exceptions

You can now reliably use C++ exceptions in libdragon games, they used to be unsupported before. Uncaught exceptions terminate the program and display an error message on the debug channel (on the unstable branch, they are properly caught and displayed in the upcoming inspector).

### Support for arbitrary screen resolutions

Libdragon now supports configuring arbitrary resolution via `display_init`. Before, the choice was limited to a few common ones like (320x240, 640x480, etc.), but the VI hardware has a flexible resolution scaler so it is actually able to configure any resolution between 2 and 800 horizontal, and between 2 and 600 vertical. Assuming a proper 3D engine that adapts the viewport to the current resolution, increasing resolution to some custom value (until FPS is high enough) is a good way to increase the visual quality of a game.

This is an example configure a custom 444x240 resolution:

```C
    display_init((resolution_t){ .width=444, .height=240, .interlaced=false }, DEPTH_16_BPP, GAMMA_NONE, ANTIALIAS_RESAMPLE);
```

### Improved syntax for RSP assembly

RSP assembly syntax has been improved. It is now possible to reduce the number of arguments avoiding redundant ones, like in the following example where all lines produce the same instruction:

```asm
    vaddc $v01, $v01, $v02,e(0)
    vaddc $v01, $v01, $v02
    vaddc $v01, $v02,e(0)
    vaddc $v01, $v02
```

Moreover, a new dot-based syntax has been added to element specifiers that is easier to write and read, more similar to GLSL shaders, and also more orthogonal in its usage across the different opcodes:

```asm
    vaddc $v01, $v02.e4      # Add the contents of lane 4 of $v02 to all lanes in $v01
    vmov  $v01.e7, $v02.e1   # Move lane 1 of $v02 into lane 7 of $v01.
```

Expert users will notice that the syntax `.eN` always uniformly refers to a lane, even though it is ended differently in different opcodes. The equivalent version in SGI syntax for vmov would be:

```asm
    vaddc $v01, $v02[e4]
    vmov  $v01[e15], $v02[e9]   # !!!
```

In addition to `.eN` to specify a lane, there is also `.qN` and `.hN` for partial broadcasts in math opcodes.

## 2022 Q3

### Add APIs to handle the reset button and the pre-NMI interrupt

When the reset button is pressed, a pre-NMI interrupt is generated and a 500ms grace time is given before the console is reset (as the RCP must be in idle state before the NMI happens). It is now possible to register a handler for the pre-NMI interrupt via `register_RESET_handler()`. Moreover, for simpler use cases where it is sufficient to poll whether the reset button was pressed or not, it is possible to call `exception_reset_time()` that will return the number of ticks since the button was pressed (or 0 if it wasn't pressed).

Some initial support has also be added to help leaving the RCP in idle state. For instance, the audio library will stop sending buffers to AI just before the NMI arrives, and the mixer library will also fade out the audio automatically during the 500ms grace time.

### New `surface_t` structure

Libdragon now defines [`surface_t`](https://github.com/DragonMinded/libdragon/blob/trunk/include/surface.h) which is an arbitrary rectangular surface, with a specific pixel format ([`tex_format_t`](https://github.com/DragonMinded/libdragon/blob/8b10174bbe5c4d83d3cd1081a0cd2c692a57a0ec/include/surface.h#L100-L113)).

This is a building block upon which the new rdpq library will build upon (in the unstable branch). `display_lock()` has been updated to return a `surface_t*` now, so that it's possible to easily access the pixels of the current framebuffer if needed. Also [`graphics.h`](https://github.com/DragonMinded/libdragon/blob/trunk/include/graphics.h) (the CPU-based drawing API) has been updated to draw upon a generic `surface_t*`, so that it can be used also on off-screen memory buffers.

### Make `dma_read_async` / `dma_write_async` APIs work on the full PI range

Historically, both `dma_read` and `dma_write` constrain the provided PI addresses to the ROM address space, and this is impossible to change for backward compatibility.

The newer DMA APIs `dma_read_async` and `dma_write_async` (together with the raw counterparts: `dma_read_raw_async` and `dma_write_raw_asycn`) have been instead enhanced to work to the full address space, including for instance 64DD or SRAM.

## 2022 Q2

### Support for CART interrupts

Devices on the cartridge connectors (eg: flashcarts, 64DD) can generate an interrupt called `CART`. It is now possible to register a handler for this interrupt via [`register_CART_handler()`](https://github.com/DragonMinded/libdragon/blob/8b10174bbe5c4d83d3cd1081a0cd2c692a57a0ec/src/interrupt.c#L518) and enable it via [`set_CART_interrupt(true)`](https://github.com/DragonMinded/libdragon/blob/8b10174bbe5c4d83d3cd1081a0cd2c692a57a0ec/src/interrupt.c#L688). 

In fact, it is possible to register multiple interrupts as there will probably be more libraries that could wait for it (eg: a flashcard library and a 64DD library).

### Misc new APIs

 * [`debug_hexdump()`](https://github.com/DragonMinded/libdragon/blob/8b10174bbe5c4d83d3cd1081a0cd2c692a57a0ec/include/debug.h#L206) dumps a binary buffer to the debug channel, with a nice formatted hexdump format, including ASCII conversion.
 * [`malloc_uncached_aligned()`](https://github.com/DragonMinded/libdragon/blob/8b10174bbe5c4d83d3cd1081a0cd2c692a57a0ec/src/n64sys.c#L228-L242) allows to allocate an uncached memory buffer (just like `malloc_uncached`) but specifying an alignment for the base pointer.
 * [`rsp_write_begin()`](https://github.com/DragonMinded/libdragon/blob/8b10174bbe5c4d83d3cd1081a0cd2c692a57a0ec/include/rspq.h#L412-L444) / `rspq_write_arg()` / `rspq_write_end()` allow to call a RSPQ function in an overlay with more than 16 arguments, which is the limit for the standard `rspq_write()` function. This is currently used in the unstable channel for the RDP triangle primitive that might require many different parameters.

### Add workaround for AI hardware bug

A [workaround](https://github.com/DragonMinded/libdragon/commit/dd87a633521bce2b34997a29b59887f26cf930f6) has been added for the rare case of hitting a AI hardware bug that causes sound corruptions when the audio buffers happen to have some specific page alignment. The bug is also accurately reproduced on emulators such as Ares.

## 2022 Q2

### Rework main exception handler including performance improvements

The main libdragon exception/interrupt handlers has been reworked with several improvements:

 * The exception frame is now saved to the standard user stack. Previously, a dedicated interrupt stack was used. In addition to saving memory for interrupt stack, this is a required step as we move towards a multi-threaded kernel, as each thread must have its own exception frame when preempted.
 * When servicing an interrupt, it is possible for an exception to trigger, reentrantly. This allows to better catch bugs in interrupt handlers (before, things like dereferencing a NULL pointer would go unnoticed as exceptions were disabled during interrupt servicing).
 * The state of the FPU is lazily saved for interrupt handlers, on the first actual usage of the FPU. Since most interrupt handlers do not need the FPU at all, the FPU state is not saved to the stack at all, improving speed.
