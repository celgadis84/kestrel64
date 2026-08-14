## Preview branch

The preview branch is called `preview` and can be downloaded from GitHub: https://github.com/DragonMinded/libdragon/tree/preview.

This is where development happens, so you can go fully bleeding edge. Notice that there is no guarantee that the APIs will be stable: they can be broken at any time and even removed, before finally landing on trunk. If this does not worry you, feel free to experiment with it.

## Features

### OpenGL support

Libdragon preview contains a full OpenGL 1.1 implementation, with some extensions and additions (include VBOs that are formally part of OpenGL 1.2). This is also similar to OpenGL ES 2.0.

Read the [OpenGL on N64](https://github.com/DragonMinded/libdragon/wiki/OpenGL-on-N64) documentation for more details. Have a look also at the gldemo example:
https://github.com/DragonMinded/libdragon/blob/preview/examples/gldemo/gldemo.c

Note that OpenGL 1.1 is still the "old style" or "legacy" way of OpenGL programming. If you are familiar with modern OpenGL you’ll know that it is all about programmable GLSL shaders which unfortunately cannot be implemented on N64 hardware. Therefore we chose version 1.1 because it fits the N64’s capabilities best. It is based on the “fixed function” pipeline, which implements [Gouraud shading](https://en.wikipedia.org/wiki/Gouraud_shading). The feature set is very similar to what libultra’s official 3D pipeline offers.

The implementation is based on rdpq and features a [full RSP T&L pipeline](https://github.com/DragonMinded/libdragon/blob/preview/src/GL/rsp_gl_pipeline.S) which is the first open source 3D RSP ucode to be released. Moreover, a full CPU pipeline is also available, and is automatically switched to whenever the current GL state is not supported by the RSP pipeline.

[Sausage64](https://github.com/buu342/N64-Sausage64) is a third party project to handle sausage-link animations that supports libdragon. Besides this, we are currently lacking tools to import meshes, materials, etc.

You can find lots of resources for old style OpenGL programming:
* [OpenGL Programming Guide : Table of Contents](http://www.glprogramming.com/red/)
* [GitHub - Dovyski/opengl-demos: A list of small OpenGL applications to demonstrate concepts of Computer Graphics](https://github.com/Dovyski/opengl-demos/)
* [OpenGL Demos and Tests - Encelo's Projectz](https://www.autistici.org/encelo/prog_gldemos.php)
* [OpenGL 1.1 Reference: Table of Contents](https://www.talisman.org/opengl-1.1/Reference.html)
* [Legacy OpenGL Tutorial](https://www.youtube.com/watch?v=t0tRc01nAk0)

### MPEG1 video player

This video player is able to reproduce raw MPEG1 stream. The player is based on [pl_mpeg](https://github.com/phoboslab/pl_mpeg) but most of the decoding pipeline (after entropy decoding) has been replaced with a [custom RSP ucode](https://github.com/DragonMinded/libdragon/blob/preview/src/video/rsp_mpeg1.S). This includes motion compensation, IDCT, dequantization / scaling / oddification, residual calculation. The final step of YUV to RGB conversion is handled through rdpq (so it is performed by the RDP).

The player is extremely efficient. It is able to decode videos up to 2Mbit/s of bitrate at 320x240 at 20-25 FPS, or around 1.5Mbit at a somewhat higher resolution. The player has performed very well in a cross-platform video competition held by SegaXTreme in 2022, including outperforming many same-generation consoles. You can read more about the player performance in this [forum post](https://segaxtreme.net/threads/segaxtreme-cross-platform-fmv-competition.25130/page-2#post-182490) which includes also link to sample ROMs.

Compared to Resident Evil 2, this player outperforms it by a wide margin, because most of the decoding is performed on RSP which is an extremely efficient chip for pixel processing and video decoding. In RE2, most of the decoding was performed on the CPU and only the YUV conversion was done on RSP (a step that, by the way, the RDP is even faster at). That was enough for the low-resolution, low-bitrate, low-framerate videos that RE2 had to use for space constraints, but would have not been enough as a stand-alone player for FMVs.

You can have a look at the [videoplayer example](https://github.com/DragonMinded/libdragon/blob/preview/examples/videoplayer/videoplayer.c) to see how to run the player. Notice that the API is subject to change as it will probably need to be redesigned before landing to trunk.

NOTE: normally, MPEG video files come in the MPEG container format that muxes audio/video; the video player in libdragon for now only handles video-only container-less streams that are normally distributed with extension `.M1V`. ffmpeg can convert from `.mpeg` to `.m1v`.

Screenshots of a ROM playing the Big Buck Bunny video with libdragon MPEG-1 player.

<img width="280" alt="Schermata_2022-10-17_alle_18 18 55" src="https://user-images.githubusercontent.com/1014109/214957524-81002f72-1941-4257-a7e4-ef4239ee9362.png">
<img width="280" alt="Schermata_2022-10-17_alle_18 18 50" src="https://user-images.githubusercontent.com/1014109/214957545-185d1def-ca48-40bd-8032-1f4172b50f9d.png">
<img width="280" alt="Schermata_2022-10-17_alle_18 17 50" src="https://user-images.githubusercontent.com/1014109/214957564-4a280030-ed18-47dd-9744-64bb097df8fe.png">

### TrueType/Bitmap font support in rdpq

rdpq features a new library to do hardware-accelerated text drawing. This is the feature list:

 * Support direct conversion from TrueType/OpenType fonts. Conversion includes many goodies:
   * Selection of Unicode code point ranges to export
   * Support for limiting glyphs to a closed list of characters (useful for CJK fonts)
   * Automatic detection of "monochrome" fonts (aka bitmap/pixel fonts shipped in TTF container)
   * Support for adding an outline to glyphs during conversion to improve readability (the color can be adjusted at runtime)
   * Full kerning table conversion
   * Automatic optimization of font atlases for maximum performance and maximum TMEM usage, packing fonts in 1bpp, 2bpp, 4bpp or 8bpp depending on the font type.
 * Support for bitmap fonts (PNGs) in BMFont format, to import also pre-colored fonts (with gradients or patterns)
 * Very simply top-level API with a printf-like interface: `rdpq_text_printf`
 * A fully-fledged runtime layout engine, optimized for performance:
   * Horizontal alignment within a box (left, center, right)
   * Vertical alignment within a box (top, center, bottom)
   * Optional word-wrapping, at the char or word level, with optional elision with ellipsis
 * Possibility to change font or style within the same string using special escape codes
 * Possibility to layout the text separately from rendering.
 * Two builtin fonts are shipped with libdragon for debugging purposes: a fixed-font one, and a more compact variable-width one
 * Fontgallery example with 30+ ready-to-use fonts with free license

Reading pointers:

 * rdpq_text docs: https://github.com/DragonMinded/libdragon/blob/preview/include/rdpq_text.h
 * Mkfont docs: https://github.com/DragonMinded/libdragon/wiki/Mkfont

Also check the fontgallery example that showcases most of the features

<img width="752" alt="Screenshot 2025-01-02 alle 13 06 15" src="https://github.com/user-attachments/assets/3c168db4-06d4-4cbe-aa91-e40285df6515" />


### Many improvements to the audio library

 * VADPCM compression for wav64 files has been improved by adding a Huffman layer, delivering an additional 2-5% of compression ratio.
 * WAV64 files have a new API for loading them `wav64_load()` which is less error-prone than the old `wav64_open()` (now deprecated).
 * WAV64 files now support multiple simultaneous playbacks also for compressed files, up to a concurrency factor that can be specified as parameter in `wav64_load()`. Limiting the concurrency allows to save memory that would be needed to store the compression state. By default, VADPCM compressed files have a concurrency factor of 4, and Opus comprised files have a concurrency factor of 1.
 * XM64 files can now be played from any filesystem, not only from ROM.
 * Multiple XM64 files can now share samples, relying on an external sample library. To use this feature, use the new option `--xm-ext-samples` in [audioconv64](https://github.com/DragonMinded/libdragon/wiki/Audioconv64), specifying the directory where to store the samples. Then, at runtime, use the new API xm64_set_extsampledir to specify where the sample library is located.
 * XM64 files can now used compressed samples. By default, samples in XM64 files are now compressed using VADPCM. The compression options can be specified via the new command line option `--xm-compress`. The reset of the XM64 file is now also compressed via the standard [asset compression](https://github.com/DragonMinded/libdragon/wiki/Compression) (specified with `--xm-compress-data`, defaulting to level 1 compression like any other asset).


### Fast math primitives for floating point 3D calculations

Libdragon includes some math primitives like `sinf`, `cosf`, `atan2f` `fmodf`, and others, designed to be used in a typical 3D old-skool game programming scenario, where floating point calculations are often performed in a `-ffast-math` context and do not need to have 1 ULP precision, like standard library. Moreover, most floating point numbers involved in 3d calculations are then eventually converted to fixed point to go into RSP for T&L, so wasting time to obtain 1 ULP is absolutely not necessary.

The provided versions are much faster than the standard library counterparts because of these shortcuts. They do not hijack the standard ones for compatibility concerns, but can be expressly invoked using the `fm_` prefix. See the documentation in [fmath.h](https://github.com/DragonMinded/libdragon/blob/preview/include/fmath.h) for more information.

### Kernel and multithreading

Libdragon now features an experimental, optional kernel for multithreading programming. You can have a look at [`src/kernel`](https://github.com/DragonMinded/libdragon/tree/preview/src/kernel), and in particular [`kernel.h`](https://github.com/DragonMinded/libdragon/blob/preview/include/kernel.h) for an overview of the APIs.

The kernel currently features most basic multithreading primitives:

 * Preemptive threads, with hard priority
 * Support for both joinable threads and detached threads
 * Mutexes (both reentrant and normal)
 * Condition variables
 * Semaphores and queues
 * C11 atomic variables (`atomic_int`, etc.)
 * C11 thread-local storage (`thread_local` keyword)
 * Stack overflow detection via canaries
 * Newlib is now build with threading support so it correctly protects with mutexes global state (eg: the heap allocator)

You are free to experiment with threading at this point, as basic support seems stable. The main issue is that libdragon itself is not thread safe as it's never been designed with multi-threading in mind. Work is ongoing to add proper threading support to it. A main hurdle is adapting `rspq` and thus all RSP-based libraries.


### Frame limiter

Libdragon now supports a sophisticated FPS limiter which works also with non integer divides of the TV refresh rate. So it is now possible for instance to limit a game to 40 FPS (for instance) and get a smooth frame-limited result. This is done via the new API `display_set_fps_limit()`.

In addition to this, there is also a new API to estimate the delta time at which next frame should be calculated (`display_get_delta_time()`). It uses a kalman filter over the times required by previous frames to be displayed, which provides for a more stable (smoother) estimation compared to just measuring CPU time between frames in the main loop.

The FPS calculation (`display_get_fps()`) has also been updated to match the same algorithm; compared to the previous one, it reacts more quickly to sudden frame changes, and is also more stable in its output.


### New combiner expression formulas ("pixel shader")

A new tool `combexpr` is available in the preview branch that helps crafting custom combiners by using simple "pixel shader"-like expressions. For instance, if you want to create a combiner that does the following:

```
RGB = tex0 * prim * 0.8
ALPHA = prim
```

you can manually invoke `combexpr` as follows:

```
$ $N64_INST/bin/combexpr 'tex0 * prim * 0.8' 'prim'
//----- Generated by combexpr: rgb='tex0 * prim * 0.8' alpha='prim'
rdpq_mode_combiner(RDPQ_COMBINER2((PRIM,0,TEX0,0),(1,0,0,PRIM),(ENV,0,COMBINED,0),(0,0,0,COMBINED)));
rdpq_set_env_color(RGBA32(204,204,204,0));
//-----
```

and `combexpr` will show you the rdpq code to paste into your source code to obtain the requested configuration. The original formula you supplied is kept in the comment to help readability.

`combexpr` also supports JSON output (via `--json`) for integration with other tools (eg: Fast64 would be a good target).

### New Controller Pak library and cpaktool

The preview branch features a new controller pak library that creates virtual filesystem on each controller pak. This allows you to read, write, and updates notes using the standard C APIs: `fopen()`, `fwrite()`, etc. Just call `cpakfs_mount()`. 

The new library also features a comprehensive `cpakfs_fsck()` function which is able to test and recover issues on most controller pak images, including recovering partially corrupted data. This will be very useful once used by a proper pak manager.

The library also has full support for multi-bank pak files like those released by Datel, up to almost 2 MiB of available space. These special pak files were also supported by libultra, so we retain full compatibility in the filesystem format.

You can check the API of the new library in the [cpakfs.h](https://github.com/DragonMinded/libdragon/blob/preview/include/cpakfs.h). Moreover, a lower level API called [cpak.h](https://github.com/DragonMinded/libdragon/blob/preview/include/cpak.h) has been added, that provides low-level byte level access to the cpak, with no filesystem semantic. This can be useful for lower level tasks like making full dumps or implementing a custom filesystem.

Moreover, a new command like tool called [`cpaktool`](https://github.com/DragonMinded/libdragon/wiki/Cpaktool) can be used to manipulate pak files on your disk. It works like a command line archiver: you can add, extract, update, delete notes from pak files. It's quite easy and powerful to use.


### Added several iQue APIs

 * Added interrupt callback support for iQue-specific interrupts
 * Added some SKC APIs (Secure Kernel Calls)
 * Added support for NAND flash controller (read/write with ECC support)
 * Added ATB support, for mapping NAND blocks into the virtual PI bus
 * Added a complete BBFS implementation for accessing the flash filesystem, both reading and writing. Access can be performed with standard C/POSIX APIs using the virtual filesystem "bbfs:/".
 * Added support for emulated Controller Paks via the new cpak API.

