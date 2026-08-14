This documents highlights how to best use OpenGL on Nintendo 64 via the libdragon preview branch. This branch features a OpenGL 1.1/1.2 implementation that implements several official extensions, plus some N64-specific extensions. 

## Learning OpenGL basics

This is not an OpenGL guide. Reading this page requires some previous knowledge of classic OpenGL programming, as it only underlines specific optimizations or tricks required for maximum performance on Nintendo 64.

This is a collection of links to study OpenGL:
 * Read the "Red Book" for OpenGL 1.1 https://dragorn421.github.io/n64-www.glprogramming.com/red/
 * Reference the "Blue Book" for a per-function reference https://dragorn421.github.io/n64-www.glprogramming.com/blue/
 * Minimal-ish demos of using OpenGL: https://github.com/Dragorn421/n64homebrew/tree/main/gltest
 * A demo with Sausage64 (a blender plugin for exporting N64 models including rigged meshes and animations) + OpenGL: https://github.com/buu342/N64-Sausage64/
 * A demo with libdragon's model64 tool (Blender -> gltf -> model64). Model64 is WIP and currently supports static and animated meshes (skinned meshes), animation overlays, and basic material support (vertex colors and textures): https://github.com/Dragorn421/n64homebrew/tree/main/brew3d


## Features

Currently, we implement most of OpenGL 1.1, plus some bits of OpenGL 1.2. In particular:

 * Two rendering pipelines: "RSP", which is the production-ready pipeline, and "CPU", which is the reference implementation that can still be useful to increase the total throughput by supplementing the RSP one.
 * Primitives: triangles (list, strips, fans), lines, points.
 * OpenGL matrix stack, fully RSP accelerated
 * Vertices: following OpenGL, we support a completely flexible vertex format. You can define as many components as you need through vertex arrays, and support both SoA and AoS layout.
 * Support for rigid skinning of meshes (via the `GL_ARB_matrix_palette` extension).
 * Textures: through a N64 specific extension (`GL_N64_surface_image`), we basically support the full set of RDP texture features on OpenGL, which includes: all texture formats, all sampling parameters, all filtering types.
 * Materials and lights: we implement the standard OpenGL material (ambient, diffuse, specular, emissive, shininess). We also support directional, point and spot lights. **NOTE:** currently, specular materials and spotlights are not RSP accelerated, see below for more information.
 * Possibility of using the native `rdpq` library to define materials using the whole set of RDP features (eg: custom color combiner). This is done via the `GL_N64_RDPQ_interop` extension.
 * Support for all the standard rendering features: blending modes, fogging, z-buffering, alpha-testing.
 * Texture coordinate generation: linear, eye-linear, spherical




## Implemented extensions

We also implement the following extensions:

 * `GL_ARB_multisample`. Use to activate the RDP/VI antialias, via `glEnable(GL_MULTISAMPLE_ARB)`. Also see the proprietary extension `GL_N64_reduced_aliasing` to check how to switch between the two different antialias mode RDP supports (reduced and standard).
 * `GL_EXT_packed_pixels`
 * `GL_ARB_vertex_buffer_object`. Implement VBOs (vertex buffer objects). This is mostly useful to help porting existing code, rather than for native N64 code (see below).
 * `GL_ARB_texture_mirrored_repeat`. You can use `glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_MIRRORED_REPEAT_ARB)` to activate mirrored wrapping.
 * `GL_ARB_texture_non_power_of_two`. You can use textures of any size, including sizes which are not powers of two. Notice that this is allowed only on clamped textures though, because of a hardware limit of the RDP.
 * `GL_ARB_vertex_array_object` 
 * `GL_ARB_matrix_palette`. This can be used to implement rigid skinning.

We also create two N64-specific extensions:

 * `GL_N64_surface_image`: this allows to define textures using libdragon's `surface_t` and `sprite_t` objects.
 * `GL_N64_RDPQ_interop`: this allows to mix-match OpenGL and the lower-level, native rdpq API in two specific areas (material definition and texturing).
 * `GL_N64_half_fixed_point`: this allows vertex arrays to provide values in 16-bit fixed point with configurable precision.
 * `GL_N64_reduced_aliasing`: this allows to configure the quality of anti-aliasing between reduced (`RA`) and standard (`AA`). Default is standarx; you can switch to the faster reduced mode with `glHint(GL_MULTISAMPLE_HINT_N64, GL_FASTEST)`.

Available extensions can be used directly, as the functions are simply declared in the global namespace (there is no need for a `dlsym`-like approach to access them).

## Overview of an OpenGL render loop

OpenGL itself does not define a way to initialize the context, and that is instead normally left to other libraries (like SDL, GLUT, etc.). In our case, we use the rest of libdragon itself to configure the context.

This is some pseudo-code to achieve a working libdragon context, please refer to the gldemo examples for a fully working sample.

```C
    // Initialize 320x240x16, triple buffering.
    // NOTE: anti-alias will only be active if activated later in GL.
    display_init(RESOLUTION_320x240, DEPTH_16_BPP, 3, GAMMA_NONE, ANTIALIAS_RESAMPLE_FETCH_ALWAYS);
 
    // Allocate a buffer that will be used as Z-Buffer.
    surface_t zbuffer = surface_alloc(FMT_IA16, 320, 240);

    // Initialize OpenGL
    gl_init();

    // Main loop
    while (1) {
        // Acquire a framebuffer. Wait until it's available.
        surface_t *fb = display_get();

        // Attach RDP to the framebuffer and the Z-Buffer.
        // From now on, the RDP will draw to the specified buffer(s).
        rdpq_attach(fb, &zbuffer);

        // Just as an example, use rdpq to fill half of the screen with the green color.
        // This is just to show that you can now issue rdpq commands.
        rdpq_set_mode_fill(RGBA32(0, 255, 0, 255));
        rdpq_fill_rectangle(0, 0, 320, 120);

        // Enter the OpenGL context. From now on, you can start using OpenGL
        // and you must NOT use rdpq to avoid conflicts.
        gl_context_begin();
        
        // Fill the other half of the screen, using OpenGL
        glScissor(0, 120, 320, 240);
        glClearColor(0, 0, 1, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        
        // Close the OpenGL context. You can open/close the context as many times as
        // required. Now that the context is closed, you can call rdpq again.
        gl_context_end();

        // Detach RDP from the current attached buffer *and* flip it on the screen
        // as soon as it's ready. This call is non-blocking, so the RSP/RDP might continue
        // processing the issued commands in background.
        rdpq_detach_show();
    }
```

## How to efficiently draw a mesh

### Draw calls

OpenGL on N64 supports 4 different ways of drawing triangles:

 * Immediate mode: `glBegin()` + `glVertex()` + `glEnd()`
 * Using `glDrawArrays()` and providing a vertex array
 * Using `glDrawElements()` and providing both a vertex array and an array of indices
 * Using VBOs (vertex buffer objects) to store vertices and/or indices

At the moment, the suggested way to draw a mesh is to use `glDrawElements()` and record the whole mesh/sub-mesh drawing sequence in a display list. This section details why.

### Vertex cache

OpenGL automatically manages an internal vertex cache of 32 vertices, using the LRU cache eviction strategy. This means that if a polygon happens to reuse a vertex that was recently used by another polygon, it will not be necessary to transform the vertex again. Usage of the cache is automatic and transparent to the application.

Notice that the cache is used only when OpenGL explicitly knows that a vertex is being reused: for instance, when using indices (via `glDrawElements`) or triangle fans/strips (via `glDrawArrays`). No attempt is made to use the cache when vertices are submitted via immediate mode (`glBegin()` + `glEnd()`) or in triangle lists (`glDrawArrays(GL_TRIANGLES, ...)`). In that case, each submitted vertex is considered a new one even if it happens to be a duplication of a previous one.

To make better use of the vertex cache, make sure to sort triangles in a cache friendly way. There are several algorithms like [Forsyth's](https://tomforsyth1000.github.io/papers/fast_vert_cache_opt.html) that can be used in your asset pipeline. OpenGL itself does not attempt to reorder triangles submitted in a draw call.

### Display lists

OpenGL has a concept of "display list", which allows to record a sequence of commands, and later replay it. This is an example of display list creation:

```C
   // Allocate one display list.
   GLuint dl_cube = glGenLists(1);

   // Start recording ("compiling") the display list
   glNewList(dl_cube, GL_COMPILE);

   // Configure material
   glEnable(GL_COLOR_MATERIAL);
   glColorMaterial(GL_FRONT_AND_BACK, GL_AMBIENT_AND_DIFFUSE);

   // Configure vertex arrays with the various vertex components
   glEnableClientState(GL_VERTEX_ARRAY);
   glEnableClientState(GL_TEXTURE_COORD_ARRAY);
   glEnableClientState(GL_NORMAL_ARRAY);
   glEnableClientState(GL_COLOR_ARRAY);

   glVertexPointer(3, GL_FLOAT, sizeof(vertex_t), (void*)(0*sizeof(float) + (void*)cube_vertices));
   glTexCoordPointer(2, GL_FLOAT, sizeof(vertex_t), (void*)(3*sizeof(float) + (void*)cube_vertices));
   glNormalPointer(GL_FLOAT, sizeof(vertex_t), (void*)(5*sizeof(float) + (void*)cube_vertices));
   glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(vertex_t), (void*)(8*sizeof(float) + (void*)cube_vertices));

   // Draw the cube
   glDrawElements(GL_TRIANGLES, sizeof(cube_indices) / sizeof(uint16_t), GL_UNSIGNED_SHORT, cube_indices);

   // Disable vertex arrays
   glDisableClientState(GL_VERTEX_ARRAY);
   glDisableClientState(GL_TEXTURE_COORD_ARRAY);
   glDisableClientState(GL_NORMAL_ARRAY);
   glDisableClientState(GL_COLOR_ARRAY);

   // Disable material
   glDisable(GL_COLOR_MATERIAL);

   // Terminate display list recording
   glEndList()
```

and this is how you would later use it to draw that mesh:

```C
    // Setup some transformation for the cube
    glPushMatrix();
    glRotatef(rotation*0.23f, 1, 0, 0);
    glRotatef(rotation*0.98f, 0, 0, 1);
    glRotatef(rotation*1.71f, 0, 1, 0);

    // Draw the cube, replaying the display list
    glCallList(dl_cube);

    glPopMatrix();
```
 
The fundamental advantage is that, in our OpenGL implementations, display lists are compiled as an **optimized sequence of RSP commands**. This means that replaying a display list can be done with basically zero CPU usage: the CPU just tells the RSP to run a sequence of commands stored at some address in memory. 

It is important to notice that the whole OpenGL implementation on N64 has been **carefully designed and optimized for display list usage**, to maximize the benefits: this means that it is strongly suggested to use them as much as possible.

Just to give an example, this is a list of things that are optimal when using display lists:

 * Internally, vertices must be converted to fixed point, as RSP does not have a floating point unit. This conversion normally happens as part of the draw call (eg: `glDrawElements()`), so any time the mesh is drawn. Instead, when using display lists, the conversion is only done once: the compiled display list in fact contains a copy of the vertices in fixed point format. In fact, once you created the display list, you could even dispose the original vertex array if you don't need it anymore for other tasks.
 * OpenGL support a flexible vertex format: you can enable / disable vertex components for each draw call, and each vertex component can be at a different position in the vertex structure (or even in different arrays). To handle this flexibility with the maximum performance, our OpenGL implementation synthesizes a fragment of RSP code called "vertex loader" (with a process similar to JITs). When using display lists, the vertex loader is created only once and saved into the display list.
 * As explained above, OpenGL handles a vertex cache to avoid transforming shared vertices twice. To do so, any time a new triangle is processed, it has to lookup the cache to check if each of the three vertices is still available there. When using display lists, all the cache lookups are done only at compilation time: the display list in fact already records for each triangle whether each vertex has been pre-transformed or not, and where in the cache it is stored.

### VBOs

The extension `GL_ARB_vertex_buffer_object` introduces the concept of VBOs (vertex buffer objects) that allow to store vertices and indices in the GPU memory. This is different from `glDrawArray()` / `glDrawElements()` that use standard vertex arrays, which are stored in RAM. Since N64 has a UMA architecture, there is no "GPU memory" where vertices or indices should be stored, and in fact they are implemented as OpenGL-managed buffers in RAM. Basically, right now, VBOs cause an additional copy of data without providing any real advantage. 

For new code using OpenGL (rather than porting of existing code), VBOs should be avoided.

## Object allocations

Classic OpenGL manages several "objects" like textures, vertex arrays, display lists, etc. These objects are referenced by IDs (of type `GLuint`) which are normally allocated via a family of `glGen*` functions. For instance, to create a vertex array, this is how it is normally done:

```C
    GLuint sphere_array;

    // Generate one vertex array ID into sphere_array.
    glGenVertexArrays(1, &sphere_array);
    
    // Bind the vertex array (= make it "current")
    glBindVertexArray(sphere_array);

    // Procede to configure it
    [...]
```

This is the standard code, which works on Nintendo 64 as well. Unfortunately, OpenGL specifications for 1.1/1.2 allow to use manually-allocated IDs for textures and display lists (as in OpenGL 1.0 when the `glGen*` functions did not exist yet):

```C
    // Choose any ID I want
    GLuint texture_wall = 0x1234;

    // Bind the texture
    glBindTexture(GL_TEXTURE_2D, sphere_wall);

    // Load the texture image
    glTexImage2D(...)
```

so basically an application can have its own ID allocation mechanism. For instance, GlQuake has its own texture ID generation system, which is simply:

```C
int texture_id = 1;  // next ID to allocate

int generate_texture_id(void) {
    return texture_id++;
}
```

We decided to not support this style of ID self-allocation on Nintendo 64. In fact, to support it, we would have to slow down the implementation by adding hash table lookups for each function in OpenGL that references an ID.

If you try to use an ID not allocated via `glGen*` functions, you will get an error screen like this:

<img width="575" alt="Screenshot 2023-06-21 alle 10 41 11" src="https://github.com/DragonMinded/libdragon/assets/1014109/2d4a2bf7-2fcf-49fe-8530-6905c7f5e908">


## Texture limits

N64 supports both square and rectangular textures. Width and height can be of any size when the texture is clamped. When using wrapping, instead, the texture size must be a power of two in the direction(s) in which wrapping is active.

In general, each texture (including all mipmaps, if present) must fit into TMEM, which is only 4096 bytes. This table shows the limits fo textures without mipmaps:

| Format | Limit (square) | Limit (texels) | Description |
| ------ | ----- | ---- | -------------------------------------------- |
| RGBA16 | 44x44 | 2048 | 16-bit texels, only 1 bit of alpha. |
| RGBA32 | 32x32 | 1024 | 32-bit texels |
| CI8    | 43x43 | 2048 | 256 colors, with palette |
| CI4    | 64x64 | 4096 | 16 colors, with palette |

## Texture loading

In classic OpenGL, textures are managed via "texture objects": 
 
 * at load time, you allocate one ID for each texture that you plan to use; then bind it (to make it current) and load the graphics for it, typically via `glTexImage2D`. This is also a good moment to configure texture attributes (like filtering or wrapping behavior).
 * at run time, you bind the texture again and draw triangles using it.

OpenGL was designed for an architecture where the GPU had its own video memory. Thus, `glTexImage2D` is defined to **take a copy** of the texture pixels. The idea is that the OpenGL implementation will copy the texture pixels into the GPU VRAM, and the CPU is then free to release the buffer right away.

Nintendo 64 has a UMA architecture so there is not a concept of Video RAM for exclusive access to the RDP. There is indeed a TMEM (texture memory) but that is more similar to a *texture cache*: it can contain just one texture and is basically the intermediate buffer where to load a texture immediately before drawing it. Thus, textures must reside in RDRAM. Implementing the actual `glTexImage2D` semantic is indeed possible (and we did it, to simplify porting) but it is wasteful because OpenGL has to allocate a new buffer and copy the texture pixels.

To implement a more lightweight semantic, taking advantage of the UMA architecture, we introduced an extension that comprehends two new functions: `glSurfaceTexImageN64()` and `glSpriteTextureN64()`.

### `glSpriteTextureN64()`

```C
void glSpriteTextureN64(GLenum target, sprite_t *sprite, rdpq_texparms_t *texparms)
```

This is the highest level texture creation function, and the easiest to use. It uses a `sprite_t` which is the object created by loading a `.sprite` file, the native N64 image format generated by the `mksprite` tool. The easiest pipeline to import a texture from an image file is thus:

 * Prepare your texture in PNG format. Make sure it follows the limits described above in terms of texture size
 * Convert your PNG texture into `.sprite` using `mksprite`. This can be tested manually by running `mksprite` but it is normally run as part of the build system via the `Makefile`. `mksprite` supports automatic mipmap creation, and color format conversion (eg: it will quantize images to create a palletized version if asked to do so).
 * Load the sprite from ROM using `sprite_load`. This will allocate a `sprite_t` object.
 * Configure the OpenGL texture object specifying the sprite object via `glSpriteTextureN64()`.

For instance, this is how to manually convert a texture to a `.sprite`.

```
$ $N64_INST/bin/mksprite --verbose --compress --mipmap BOX --format CI4 circle0.png
Converting: circle0.png -> ./circle0.sprite [fmt=CI4 tiles=0,0 mipmap=BOX dither=NONE]
loading image: circle0.png
loaded circle0.png (32x32, LCT_RGBA)
mipmap: generated 16x16
mipmap: generated 8x8
mipmap: generated 4x4
quantizing image(s) to 16 colors
auto detected hslices: 2 (w=32/16)
auto detected vslices: 2 (w=32/16)
compressed: ./circle0.sprite (848 -> 280, ratio 33.0%)
```

In this case, we started from a RGBA PNG, and we asked to convert it to CI4 (16 colors with palette), generate mipmaps, and also compress the resulting file using libdragon's builtin compression support.

Then, at runtime, we can load the texture like this:

```C
    // Load the sprite from ROM (decompressing it transparently if it is compressed)
    sprite_t *circle = sprite_load("rom:/circle0.sprite");
    
    // Allocate texture object ID
    GLuint tcircle;
    glGenTextures(1, & tcircle);

    // Configure the texture, including all mipmaps
    glBindTexture(GL_TEXTURE_2D, tcircle);
    glSpriteTextureN64(GL_TEXTURE_2D, circle, NULL);
```

See below for the usage of the third parameter (`rdpq_texparms_t*`) to configure the texture sampler.


### `glSurfaceTexImageN64()`

```C
void glSurfaceTexImageN64(GLenum target, GLint level, surface_t *surface, rdpq_texparms_t *texparms);
```

While `glSpriteTextureN64` allows to configure the whole texture object in one go (all images for all mimaps), `glSurfaceTexImageN64`  is more similar to `glTexImage2D` and allows to configure one image at a time. Thus, it is a lower-level function which is probably more useful while porting existing code bases that do not use `.sprite` files.

These are the main differences compared to `glTexImage2D`:

 * The input buffer is passed as a `surface_t`, which is Libdragon's data structure to define memory buffers used to store images.
 * There is no memory copy being performed nor change of ownership. OpenGL expected that the data referenced by the `surface_t` will stay available during runtime. It is responsibility of the caller not to dispose the data referenced by `surface_t`, as long as the texture object is being used (note that the `surface_t` structure itself is just a temporary; as long as the actual buffer stays available, the `surface_t` structure itself can be disposed immediately after the call).
 * The function accepts also an optional `rdpq_texparms_t` structure which can be used to configure the texture sampler parameters. See below for more information. This structure is just a temporary, and can be disposed after the call.

This function must be called one time per each mipmap level, specifying the mipmap level in the parameter `level`. 

## Texture sampler

RDP has a very peculiar and advanced texture sampler, that is capable of effects not commonly found in other GPUs. For instance, it is possible to add a translation and a scale to all texture coordinates while sampling (similar to applying a texture matrix), and it can be configured to both wrap (a finite, fractional amount of times) and then clamp. For instance, you can request a texture to repeat for two times and a half horizontally and then clamp the last pixel indefinitely. 

To access all these features, both `glSpriteTextureN64` and `glSurfaceTexImageN64` accept also an optional `rdpq_texparms_t` structure. This structure is defined in rdpq (libdragon's native RDP library, upon which OpenGL is built), and exposes all the sampler functionalities:

```C
typedef struct rdpq_texparms_s {
    int tmem_addr;           ///< TMEM address where to load the texture (default: 0)
    int palette;             ///< Palette number where TLUT is stored (used only for CI4 textures)

    struct {
        float   translate;    ///< Translation of the texture (in pixels)
        int     scale_log;    ///< Power of 2 scale modifier of the texture (default: 0). Eg: -2 = make the texture 4 times smaller

        float   repeats;      ///< Number of repetitions before the texture clamps (default: 1). Use #REPEAT_INFINITE for infinite repetitions (wrapping)
        bool    mirror;       ///< Repetition mode (default: MIRROR_NONE). If true (MIRROR_REPEAT), the texture mirrors at each repetition 
    } s, t; // S/T directions of texture parameters

} rdpq_texparms_t;
```

The first two fields are for very specific cases and can be generally ignored when using OpenGL (leaving them to 0). The sampler parameters can be specified for both `s` and `t` (horizontal and vertical). This is an example of usage:

```C
    glSpriteTextureN64(GL_TEXTURE_2D, sprite, &(rdpq_textparms_t){
        .s.translate = 8, .s.repeates = 2.5, 
        .t.repeats = REPEAT_INFINITE, .t.mirror = true,
    });
```

In this case, we are configuring the `s` coordinate (horizontal) to repeat two and a half time before starting to clamp. Moreover the texture will be translated by 8 texels to the left (just as if all `s` coordinates in all vertices were added to `8`). On the `t` coordinate (vertical) instead, the texture will repeat infinite times, mirroring at each repetition.

Notice the usage of C99 designated initializers, which are more readable and guarantee that all other fields are left to 0 (which is designed to be a good default for all the fields).

**NOTE**: if you use `glTexSurfaceImageN64` to upload mipmaps one by one, and provide your own `rdpq_texparms_t` parameters, make sure to also update the scaling factor (fields `.s.scale_log` and `.t.scale_log`), increasing it by 1 for each subsequent level.

## Texture sampler via mksprite

As an alternative to provide the texture sampler parameters in code, it is possible to embed the default sampler parameters in a `.sprite` file via `mksprite`. The is the relevant excerpt of the help:

```
Sampling flags:
   --texparms <x,s,r,m>          Sampling parameters:
                                 x=translation, s=scale, r=repetitions, m=mirror
   --texparms <x,x,s,s,r,r,m,m>  Sampling parameters (different for S/T)
```

The four parameters `x,s,r,m` corresponds respectively to the fields `translate`, `scale_log`, `repeats`, `mirror` of the `rdpq_texparms_t` structure. There are two accepted syntax: the first one can be used when the sampler must be configured both horizontally and vertically in the same way; the second instead can be used when the configuration is different.

This example embeds within the sprite file the same configuration shown in the above example:

```
$ $N64_INST/bin/mksprite --texparms 8,0,0,0,2.5,inf,0,1
```

in fact, from left to right:

 * `8,0`: translation parameters (`8` for `s`, `0` for `t`)
 * `0,0`: scale parameters. Being the log2, this means a scale factor of 1 (= no scale).
 * `2.5,inf`: repetitions. The texture will repeat 2.5 times horizontally, and infinite times vertically.
 * `0,1`. mirror. The texture will mirror vertically, and repeat normally horizontally.

To use the values embed in the sprite, just pass NULL to `glSpriteTextureN64`:

```C
   glSpriteTextureN64(GL_TEXTURE_2D, sprite, NULL);
```

In fact, the semantic of passing `NULL` to `glSpriteTextureN64` is as follows:

 * If the sprite contains embedded parameters, use those.
 * Otherwise, if the user called `glTexParameteri` with values `GL_TEXTURE_WRAP_S`/`GL_TEXTURE_WRAP_T` on the texture object before calling `glSpriteTextureN64`, use those configurations.
 * Otherwise, fallback to OpenGL default which is making an infinite non-mirrored wrapping on both axis.

## CPU and RSP usage

It might be useful to understand the overall architecture of the OpenGL implementation with respect to hardware resources (specifically, CPU, RSP and RDP), to create a mental model of what happens when you run OpenGL functions. The implementation is based on libdragon's rspq and rdpq libraries, so for a better in-depth study please refer to the respective documentations (in rspq.h and rdpq.h).

Most OpenGL functions, when run on the CPU, ends up creating one or more "commands" for the RSP to run. The commands are not run synchronously: the CPU only enqueues them for execution, by writing them into a ring buffer (similar to a work queue). The RSP runs in parallel to the CPU and constantly polls this work queue; as soon as commands are available, it starts running them. The RSP is very efficient to do 3D math computations, so it will take care of all matrix stacks, plus the full transform and lighting (T&L) CPU, that transforms each vertex, calculates lighting, assemble the final polygon, clip it onto the viewport, and hands it over the RDP for rasterization. In a similar fashion, the RSP just enqueues triangles (or render mode changes) as commands for the RDP into a ring buffer, again in RAM; the RDP will constantly fetches these commands (via DMA) and executes them in parallel to RSP (and CPU).

The main takeaway is then that OpenGL functions are "non blocking", and just schedule work to be done. If, mainly for debugging reasons, you absolutely want to wait until the RSP and the RDP have finished executing whatever was scheduled, you can use `glFinish()`.

### Pipelines

The OpenGL implementation ships with two identical T&L pipeline implementations: one that runs on RSP (which is the faster one) and one that runs on the CPU. In general, OpenGL will run draw calls using the RSP pipeline as it is the most efficient, but it currently does not implement 100% of the supported features. When the user requests a feature that is not available on the RSP pipeline, OpenGL will fallback to the CPU implementation (which is instead complete), just for the draw calls that require those features. Switching between CPU and RSP pipeline does not carry any overhead, nor it creates syncing problem: it is completely transparent to the ruling application, if not for the performance impact.

In the cases in which the CPU pipeline is triggered, a message is emitted on the debug spew (once):

```
GL WARNING: The CPU pipeline is being used because a feature is enabled that is not supported on RSP: <feature>
```

The CPU pipeline is several times slower than the RSP pipeline, but it can be still a reasonable fallback for a small subset of polygons in the scene. The list of features currently unimplemented on RSP (and that will thus cause a fallback on the CPU) are:

 * Lines
 * Points
 * Flat shading
 * Spotlights
 * Specular material

There is currently no way to disable the CPU pipeline, nor there is a way to force it even if not necessary. In the future, we plan to explore using the CPU pipeline when the RSP is saturated, as a way to process more polygons than then RSP alone can process.

**NOTE**: display lists are not available when using the CPU pipeline. So for instance at the moment you cannot record a display list drawing a mesh lighted with a spotlight. 

## Unsupported functions

The following functions are not supported by OpenGL on N64:

 * `glGet*` functions. These are currently not implemented. Notice that, even when they are implemented, they will be very slow as they would require a full sync (just like calling `glFinish`) which would remove the important parallelism between CPU, RSP, and RDP. We would probably like to implement them at some point, to help porting existing code, but their usage should be strongly avoided for new code.
 * `glTexSubImage2D`

Unsupported functions are simply not declared, so you will get a compilation error if you try to call them.
