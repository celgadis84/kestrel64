The stable branch guarantees API stability "forever". The idea is that if you build your ROM against libdragon stable, we will strive to make sure that your ROM will build against newer libdragon versions as well. There maybe transient failures where we messed something up and we broke something inadvertently. We will always aim at fixing such incompatibilities once identified and we expect those to be temporary.

We feel that API stability is paramount for our small community of N64 homebrewers. If libdragon kept on continuously breaking its API, it would be impossible for people to leave a stable legacy. Some code written a few years ago (or even weeks ago) could fail to compile on newer libdragon versions, which in turn might contain important bugfixes, even for full hardware compatibility.

Given [Hyrum's law](https://www.hyrumslaw.com), despite our best efforts, some code might end up relying on some grey-area behavior and eventually break. This page tries to provide a guidance on how we intend the API stability to be used, to try to minimize these cases. 

If you found an old ROM that you can't build anymore with a newer libdragon, have a look at the [upgrade troubleshooting](https://github.com/DragonMinded/libdragon/wiki/Upgrade-troubleshooting) guide.


## General overview

The API stability guarantee covers:

 * Public functions, as exposed by libdragon headers and documented in the [reference manual](https://libdragon.dev/ref/modules.html). Libdragon will not change the prototype of the functions in a way that make calling code not work anymore (eg: adding a parameter). It might sometimes make changes to them as long as it is backwards compatible (eg: a `int` parameter could be changed to `float` in the future, as this is deemed to be a backward compatibile change).
 * Public structures, as exposed by libdragon headers. Libdragon will not rename or delete a field of the public structures. We instead reserve the right instead add more fields, or reorder them, so some care must be taken while filling them. See below for some examples.
 * Asset conversion tools. Tools like `mksprite`, `audioconv64`, etc. are guaranteed to change only in a backward compatible way. This means that existing command line options will not be removed.

The API stability guarantee **DOES NOT COVER**:

 * Binary file formats, such as `.sprite` or `.wav64`. These can change at any time.
 * Compression algorithms shipped as part of the asset pipeline. 
 * Internal building tools used by `n64.mk` (eg: `n64tool`, `n64sym`, etc.). 

## Rules to follow

To benefit from API stability in the stable branch, follow these simple rules:

 * **Include `libdragon.h` only**. `libdragon.h` brings in the whole libdragon API, so no other libdragon include is necessary. Do not include other header files such as `display.h`, `rsp.h`, etc., as these might get renamed, removed, or rearranged in the future.
 * **Only use public APIs**. Libdragon public APIs are those exposed via `libdragon.h` and documented in the [reference manual](https://libdragon.dev/ref/modules.html). Sometimes, header files expose other symbols for internal purposes, that are clearly marked as such with code comments, and specifically hidden from the reference manual. Those symbols are not part of the public API and should not be used as they could go away at any time.
 * **Use a Makefile including n64.mk, like examples do**. Some people prefer to use other build systems but unfortunately N64 is an embedded system and building a ROM includes spawning many different external tools during build, with specific rules. `n64.mk` is our own "build system" that describes exactly what needs to be done, and it is updated as libdragon progresses over years. If you manage to build your ROM by copying what n64.mk does today into your own favorite build system, this will break in the future when we change n64.mk (because maybe we changed an internal tool to accept different command line arguments, added another build step, or whatnot).
 * **Commit original assets, not converted ones**. For instance, for sprites and textures, commit into your repository the original PNG files, not the converted `.sprite` files. Libdragon consider all its proprietary formats as not part of its public API, so their binary content can change at any time. If you commit a `.sprite` file, a future Libdragon might not be able to open and parse it correctly anymore. If you instead commit the PNG file, the API stability guarantees that `mksprite` will always be able to convert it.
   * Note that this applies to asset compression formats too. Libdragon can change the compression formats at any time in the future, so in case you want to use libdragon compression over your own binary assets (via `mkasset`), make sure to commit the original, uncompressed file, not the compressed one.
 * **Use designated initializers for C structures**. When initializing a structure defined by libdragon, make sure to use designated initializer to specify the value of one or multiple members of the structure. For instance:

```C
resolution_t res = { .width = 320, .height = 240, .interlace=false };
display_init(res, ...);

// or you can do that inline if you prefer
display_init((resolution_t){.width=320, .height=240, .interlace=false}, ...);
```

This allows libdragon maintainers to add more fields to the structure (as long as 0 is a good, backward compatible default for them), and freely reorder fields in the definition. Using other approaches might create problems in the future. For instance:

```C
// DO NOT DO THIS. All fields not mentioned in your code will be uninitialized causing undefined behavior.
// This will make it impossible to libdragon authors to add more fields to the structure.
resolution_t res;
res.width = 320; 
res.height = 240;
res.interlace = false;
display_init(res, ...);
```

```C
// DO NOT DO THIS. This would make it impossible to libdragon authors to reorder fields in the structure, which
// is sometimes needed.
resolution_t res = {320, 240, false};
display_init(res, ...);
```

NOTE: Recent C++ versions (like C++17 with GNU extensions, that libdragon defaults to) support designated initializers, but they force them to be in the same order of the structure definition.


## RSP include files (.inc)

Libdragon also ships include files for RSP. There is a basic `rsp.inc` files to help with RSP programming in general, and then there is the rspq/rdpq libraries with their own include files to help building ucodes based on those frameworks.

In general, it is basically impossible to guarantee full stability like we do with C. In assembly, even small changes can have cascading effect that break existing code. For instance, by adding a few opcodes to `rsp_queue.inc` (common code used by all rspq-based ucodes), we could break an existing code that used all available IMEM, overflowing it.

So for RSP, we can just do a best-effort guarantee. We will avoid any kind of gratuitous breakages (also for sanity with libdragon's own ucodes!) but at the same time, if you are a ucode programmer, you have to expect that something might break at some point.