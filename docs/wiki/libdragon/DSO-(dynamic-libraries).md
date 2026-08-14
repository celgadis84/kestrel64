### Dynamic libraries (DSO)

Libdragon supports the creation of dynamic libraries (sometimes called "overlays"), that allows to load and unload portions of code at runtime. This allows to reduce memory consumption for parts of code which are not necessary to be always available. For instance, each actor could be compiled into its own dynamic library, and loaded / unloaded depending on the game area where the player is.

Dynamic libraries have the `.dso` extension, and can be loaded using the standard posix API `dlopen()`. Functions in the dynamic library can be accessed via `dlsym()` and then called through a normal function pointer. Moreover:

 * All public (non-static) symbols in a dynamic library are accessible from the main binary via `dlsym()`. In ELF terms, all public symbols are visible.
 * Dynamic library code can transparently call symbols in the main binary, such as libdragon itself, newlib, engine code, etc. No special provision is required, it is sufficient to just call the functions or access the variables. This makes it extremely easy to split existing code into a dynamic library, as no code changes are basically required.
 * Dynamic libraries can also reference symbols in other dynamic libraries, assuming those were loaded first. For instance, if `a.dso` references a function in `b.dso`, it is necessary to load `b.dso` first, and only later load `a.dso`, otherwise an assert is hit. It is the responsibility of the programmer to assure that dynamic libraries are loaded in strict dependency order.
 * DSO files can also be compressed, via asset compression just like any other data file, and they are automatically decompressed at load time.
 * The crash inspector features a new page that lists all loaded dynamic libraries, together with their current memory addresses.
 * Stack traces and symbols are correctly resolved also across dynamic library boundaries, so they also show symbol names, filenames and line numbers for source files linked into a DSO.
 * GDB debugging integration is also provided automatically; when you debug via gdb (eg: with Ares), you will be able to put breakpoints also in DSOs that are not loaded yet, and they will resolve correctly when the DSO is loaded.

Two examples are provided to show how to use dynamic libraries. Have a look at https://github.com/DragonMinded/libdragon/tree/preview/examples/overlays.

At the moment, n64.mk setups compilation of DSOs in a way that libdragon itself (all used parts of it) is always included in full into the main binary, while DSOs only contain user code.