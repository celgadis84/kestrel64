This guide lists a few common hurdles when upgrading an application to the latest version of libdragon stable (`trunk`).

## Stable vs preview

This guide refers to the stable version of libdragon (branch `trunk`). We strive as much as possible (within the limits of a hobby project) not to break backward compatibility. This means that all existing libdragon applications that were developed against libdragon stable should be able to upgrade libdragon version and "just recompile and run".

The [preview version](https://github.com/DragonMinded/libdragon/tree/preview) of libdragon (branch `preview`) is, as the name implies, a preview of features being developed. The API is subject to change without notice breaking compatibility. If you develop against the preview version, you must be aware that an application might be impossible to recompile without changes in a few months or years, and you as the author might not be interested in that application anymore. If possible, develop against the stable branch to leave a stable legacy for future N64 enthusiasts.

## Common problems you may encounter after upgrading

### Invalid API usage causing assertion screens

In general, we strive to make libdragon stricter and stricter in correct usage of its APIs. One of the many hurdles in N64 development is that it is very easy to misprogram the hardware in a way that does not work, so it is important that libdragon guides you avoiding that. One of the main tool that we use to help programmers is through assertions, that lead to crashes showing an assertion screen (on both the screen, and the debug log). 

After upgrading libdragon, it might be that a working application crashes with an assertion screen. This means that the application is actually technically buggy: maybe the bug does not reproduce on emulators, or only reproduce on a few consoles, or might lead to other problems later, but in any case it was deemed to be buggy enough to block it. The assertion screen should point you to the problem and sometimes even suggest the best solution to it.

### Deprecation errors during compilations

Sometimes we evolve libdragon APIs and mark older APIs as deprecated through a GCC attribute. We do not remove the old APIs as that would break backward compatibility, and we do not change their behavior: they are expected to still work as before, but we want to advise the developer to upgrade to a newer API whenever possible.

These deprecated APIs are expected to cause deprecation *warnings* during compilations, without breaking the build. If the application is not using the standard `n64.mk` and is forcing `-Werror`, those warnings will instead become errors, blocking the build.

To fix this, you have several alternatives: you can add `-Wno-error=deprecated-declarations` to the command line, so that the deprecated notices become warnings again; you can take the chance to start using `n64.mk`, which would have handled this for you; or you can bite the bullet and fix the deprecation warning by changing your code (even though it is not technically required, it might be a good idea anyway). 


### Compilation error related to `rdp_draw_textured_rectangle` or `MIRROR_` macros

In 2019, a change to rdp.c was merged that [added the mirroring functionality](https://github.com/DragonMinded/libdragon/commit/00a6cc8e6d136cf2578a50320f6ff0814dfb6657) to existing APIs, adding one more argument to the several function calls. This broke a lot of existing libdragon applications. It was the fallout of this change that made us appreciate the important of implementing a stable API policy.

Unfortunately, there is no way around this: you need to add `MIRROR_DISABLED` as last argument to the affected function calls.

### Controllers not working

This is often caused by the fact that you are using the controller subsystem (eg: `controller_scan()`, `get_keys_down()`) but you forgot to call `controller_init()`. In older libdragon versions, `controller_init()` was basically empty, so even if was technically necessary to call it as explained in the documentation, failures to do so would not be apparent. In more recent libdragon versions, you must absolutely call `controller_init()` fo the controller subsystem to work correctly.

Later libdragon versions will show an assertion screen in this case, but if you are upgrading to some intermediate libdragon version, you might not get it.

### Linker errors related to FatFS

At some point in the history, libdragon imported a vendored copy of fatfs for its own SD card support. This broke all applications that already linked against FatFs themselves, causing duplicated symbols.

A typical error is a linker error on `disk_` functions:
```
/opt/n64//bin/mips64-elf-ld: /opt/n64/mips64-elf/lib/libdragon.a(debug.o): in function `disk_initialize':
/Users/foobar/Sources/n64/libdragon/src/debug.c:152: multiple definition of `disk_initialize'; obj/diskio.o:(.text+0x2c): first defined here
```

To fix this, it is sufficient to remove the FatFS source code from the application. FatFS is already shipped with libdragon, so you don't need to compile and link it a second time.

### Compilation error regarding `resolution_t`

If you get an error like this:

```
game.c:254:22: error: incompatible types when initializing type 'int' using type 'resolution_t'
  254 |     int resolution = RESOLUTION_320x240;
      |                      ^~~~~~~~~~~~~~~~~~
```

the fix is to change the declaration from `int` to `resolution_t`.

The problem was born when we added [arbitrary resolution support](https://github.com/DragonMinded/libdragon/pull/311). In that commit, we changed `resolution_t` from being a `enum` to a `struct`. This does not break backward compatibility in general if the user correctly declared `resolution_t` whenever necessary; if instead the user code relied on the implicit conversion of an enum to `int`, the code will not compile anymore. 

### n64tool gives an error about invalid ROM size

You might be greeted with this error:

```
/opt/n64/bin/n64tool -l 256K -t MYROM01 -h /opt/n64/mips64-elf/lib/header -o bin/MYROM.z64 bin/MYROM.bin
ERROR: Invalid size argument: 256K; must be at least 1048576 bytes
Smaller ROMs have compatibility problems with some flashcarts or emulators.
```

The error is pretty much self-explanatory: ROM files must be at least 1048576 (1MiB) (actually, they end up a bit bigger because of the header, but the command line argument must be at least `1M`). This happens because N64 official bootloader (aka IPL3) calculates a checksum of the first mebibyte of the ROM; creating a smaller ROM means that different tools (flashcarts, emulators) might differ on how they internal pad the ROM, causing problems during loading. So even if the 256K ROM previously worked for you, it might not work for other people using other tools.

Previous versions of n64tool didn't warn against this common error, while newer versions do. It is sufficient to change the Makefile to call `n64tool` specifying at least `1M` as size argument.

### UNFLoader errors out with "unknown data type"

If you use `debugf()` to print logs through USB, and [UNFLoader](https://github.com/buu342/N64-UNFLoader) as host tool to display them, you might get this error after upgrading libdragon:

![image](https://github.com/DragonMinded/libdragon/assets/1014109/c2aab460-3072-424a-8ae3-a735140d8535)

This happens because libdragon is now using a newer USB protocol. You must upgrade UNFLoader too. The minimum supported version is currently 1.5, though you should probably upgrade to version 2 now.

### Error building mikmod

If you hit an error such as:

```
    [CC] game.c
game.c:7:10: fatal error: mikmod.h: No such file or directory
    7 | #include <mikmod.h>
      |          ^~~~~~~~~~
```

this happens because libdragon does not build and install a copy of mikmod anymore. Mikmod used to be the only way to play modules in background in libdragon many years ago, but since the native RSP mixer library and XM player were merged, it is not a recommended solution anymore (Mikmod has been ported only at the CPU level, so it uses between 30 and 50% of the available CPU time just to reproduce the module), so libdragon does not ship with it anymore. To compile and install mikmod, we ship a script in `tools/build-mikmod.sh` that will do everything for you.

### Global constructors

With the introduction of `n64.mk`, libdragon started to link everything using `g++` instead of the older manual linking strategy with `ld`. Due to the differences between the two methods, the updated `__do_global_ctors` method stopped working correctly for old builds living out in the wild. At that point, reverting it back would mean breaking newer projects to fail running correctly and thus a two way compatible solution was introduced. When using the new build system, it provides `--wrap __do_global_ctors` flag to use the newer method instead. When linking directly with `ld` it should not be passed in as it was always the case previously. In the unlikely case you have a project with the new build system but before this additional fix, you may have problems running ROMs. The simple fix is just upgrading to a newer version of libdragon.

### `filesystem` directory does not exist

If you get this error after upgrading:
```
Error creating 'game.dfs': directory 'filesystem' is empty or does not exist
```

you need to be aware that `n64.mk` was changed to default to use `filesystem` as directory name for the DragonFS filesystem. If you arranged your build so that the files are in another directory, you must now explicitly declare that by setting the `N64_MKDFS_ROOT` variable to point to the path where your files are stored.