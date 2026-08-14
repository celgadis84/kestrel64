# Mksprite

Mksprite is libdragon's tool for converting images (such as textures or sprites) from the PNG format into a format compatible with Nintendo 64, saved on a file with the `.sprite` extension.

A `.sprite` is an image file that can contain:

 * Pixels for an image (in one of the hardware RDP formats)
 * (optional) a palette of colors
 * (optional) precalculated mipmap levels

## Quick tutorial

Running mksprite can be as easy as:

```
mksprite filename.png
```

This will create a `.sprite` file. It will automatically select the N64 format that is more similar to the input PNG file, trying to avoid color conversions as much as possible.

For instance, trying to run mksprite on a RGB PNG, we obtain this:

```
$ mksprite --verbose ../srtitle400.png
Converting: ../srtitle400.png -> ./srtitle400.sprite [fmt=AUTO tiles=0,0 mipmap=NONE dither=NONE]
loaded ../srtitle400.png (640x400, LCT_RGB)
auto selected format: RGBA16
auto detected hslices: 40 (w=640/16)
auto detected vslices: 25 (w=400/16)
```

so `mksprite` selected the N64 RGBA16 because the input PNG is in LCT_RGB format. Instead, trying to convert a PNG file with a palette:


```
$ mksprite --verbose ../hoi.png
Converting: ../hoi.png -> ./hoi.sprite [fmt=AUTO tiles=0,0 mipmap=NONE dither=NONE]
loaded ../hoi.png (640x200, LCT_PALETTE)
palette: 43 colors (used: 41)
auto selected format: CI8
auto detected hslices: 40 (w=640/16)
auto detected vslices: 12 (w=200/16)
```

in this case, the tool selected the CI8 format, and created a sprite file that contains the exact PNG palette, without an index remapping, preserving the input data as much as possible.

It is possible to force a specific output format, via a command line flag. If necessary, mksprite will quantize the image using an industry leading quantization algorithm (exoquant), with optional dithering. For instance:

```
$ mksprite --format CI8 --dither ORDERED --verbose ../srtitle400.png
Converting: ../srtitle400.png -> ./srtitle400.sprite [fmt=CI8 tiles=0,0 mipmap=NONE dither=ORDERED]
loaded ../srtitle400.png (640x400, LCT_RGB)
quantizing image(s) to 256 colors
auto detected hslices: 40 (w=640/16)
auto detected vslices: 25 (w=400/16)
```

Another way to tell mksprite that we want a specific format is to put it as part of the filename, in an extension. For instance, if we rename `srtitle400.png` to `srtitle400.ci8.png`, mksprite will default to CI8:

```
$ mksprite --verbose srtitle400.ci8.png
Converting: srtitle400.ci8.png -> ./srtitle400.ci8.sprite [fmt=AUTO tiles=0,0 mipmap=NONE dither=NONE]
loading image: srtitle400.ci8.png
detected format from filename: CI8
loaded srtitle400.ci8.png (640x800, LCT_RGB)
auto selected format: CI8
quantizing image(s) to 256 colors
compressed: ./srtitle400.ci8.sprite (512648 -> 99883, ratio 19.5%)
```

Notice that mksprite also supports libdragon asset compression, and by default it will compress images using the "level 1" compression. Compression ratio is usually similar or better than PNG compression. 

Moreover, mksprite also has support for lossy compressions. It is mainly useful for large pictures, though it can be used also for small tiles. Use the `--lossy` option to activate it lossless compression and specify the quality level in a range 0..100:

```
$ mksprite --lossy 70 --verbose srtitle400.png
loading image: srtitle400.png
loaded srtitle400.png (640x400, LCT_RGB)
auto selected format: RGBA16
mksprite: lossy srtitle400.png -> ./srtitle400.sprite [640x400]
mksprite: lossy quality=70 -> crf=26
mksprite: PSNR: Y=39.61 U=43.65 V=44.85 avg=40.67
mksprite: written decoded debug image: ./srtitle400.debug.png
mksprite: written: ./srtitle400.sprite (22681 bytes)
```

mksprite is also able to automatically generate mipmaps, even while quantizing:

```
$ mksprite --format CI4 --mipmap BOX --verbose diamond0.png
Converting: diamond0.png -> ./diamond0.sprite [fmt=CI4 tiles=0,0 mipmap=BOX dither=NONE]
loaded diamond0.png (32x32, LCT_RGBA)
mipmap: generated 16x16
mipmap: generated 8x8
mipmap: generated 4x4
quantizing image(s) to 16 colors
compressed: ./diamond0.sprite (784 -> 252, ratio 32.1%)
```

In this case, the input 32x32 RGBA image was automatically scaled multiple times to generate the various mipmaps, and then all the mipmaps were converted to a single 16 color palette through quantization. This is important as to achieve the better quality, the choice of colors should take all mipmap levels into account.

## Command line options

This is mksprite usage, that gives an overview of all options:

```
Usage: mksprite [flags] <input files...>

Command-line flags:
   -v/--verbose          Verbose output
   -o/--output <dir>     Specify output directory (default: .)
   -f/--format <fmt>     Specify output format (default: AUTO)
   -D/--dither <dither>  Dithering algorithm (default: NONE)
   -c/--compress <level> Compress output files (default: 1)
   -d/--debug            Dump computed images (eg: mipmaps) as PNG files in output directory

Sampling flags:
   --texparms <x,s,r,m>          Sampling parameters:
                                 x=translation, s=scale, r=repetitions, m=mirror
   --texparms <x,x,s,s,r,r,m,m>  Sampling parameters (different for S/T)

Mipmapping flags:
   -m/--mipmap <algo>                    Calculate mipmap levels using the specified algorithm (default: NONE)
   --detail [<image>[,<fmt>]][,<factor>] Activate detail texture:
                                         <image> is the file to use as detail (default: reuse input image)
                                         <fmt> is the output format (default: AUTO)
                                         <factor> is the blend factor in range 0..1 (default: 0.5)
   --detail-texparms <x,x,s,s,r,r,m,m>   Sampling parameters for the detail texture

Supported formats: AUTO, RGBA32, RGBA16, IA16, CI8, I8, IA8, CI4, I4, IA4, ZBUF, IHQ, SHQ
Supported mipmap algorithms: NONE (disable), BOX
Supported dithering algorithms: NONE (disable), RANDOM, ORDERED.
Note that dithering is only applied while quantizing an image.
```

All PNG files passed on the command line are converted separately, generating one `.sprite` file for each of them. By default, the `.sprite` file is generated in the current directory, but it is possible to change this via `-o/--output`.

## Sprite formats

This tables lists the formats supported by N64; for each one, it lists the PNG formats that are supported in input when using the `--format` option, and what kind of PNG must be supplied so that the format is autodetected by mksprite without an explicit `--format`.

| N64 format  | Supported PNG formats | Autodetection |
| ------------- | ------------- | ------------------ |
| CI4  | <li>LCT_PALETTE<br><li>LCT_RGB/LCT_RGBA (optionally quantized) | LCT_PALETTE or LCT_RGBA with <= 16 actually used colors |
| CI8  | <li>LCT_PALETTE<br><li>LCT_RGB/LCT_RGBA (optionally quantized) | LCT_PALETTE or LCT_RGBA with > 16 and <= 256 actually used colors |
| I4   | <li>LCT_PALETTE (greyscaled)<br><li> LCT_RGB/LCT_RGBA (greyscaled)<br><li> LCT_GREY<br><li> LCT_GREY_ALPHA | LCT_GREY with <= 16 actually used grey levels |
| I8   | <li>LCT_PALETTE (greyscaled)<br><li> LCT_RGB/LCT_RGBA (greyscaled)<br><li> LCT_GREY<br><li> LCT_GREY_ALPHA | LCT_GREY with > 16 actually used grey levels |
| IA4  | <li>LCT_PALETTE (greyscaled)<br><li> LCT_RGB/LCT_RGBA (greyscaled)<br><li> LCT_GREY<br><li> LCT_GREY_ALPHA | LCT_GREY_ALPHA with bitdepth < 4 |
| IA8  | <li>LCT_PALETTE (greyscaled)<br><li> LCT_RGB/LCT_RGBA (greyscaled)<br><li> LCT_GREY<br><li> LCT_GREY_ALPHA | LCT_GREY_ALPHA with bitdepth >= 4 and < 8 |
| IA16 | <li>LCT_PALETTE (greyscaled)<br><li> LCT_RGB/LCT_RGBA (greyscaled)<br><li> LCT_GREY<br><li> LCT_GREY_ALPHA | LCT_GREY_ALPHA with bitdepth >= 8 |
| RGBA16 | <li>LCT_PALETTE<br><li> LCT_RGB/LCT_RGBA<br><li> LCT_GREY<br><li> LCT_GREY_ALPHA | LCT_RGB/LCT_RGBA |
| RGBA32 | <li>LCT_PALETTE<br><li> LCT_RGB/LCT_RGBA<br><li> LCT_GREY<br><li> LCT_GREY_ALPHA | -- |
| ZBUF | <li>LCT_GREY (8bpp or 16bpp) </li> | -- |
| IHQ  | <li>LCT_RGB/LCT_RGBA </li>| -- |
| SHQ  | <li>LCT_RGB/LCT_RGBA </li>| -- |

To select a specific format which is not the auto-selected default, you can:

 * Use the `--format` command line option expliclty
 * Rename the file so that it contains requested format as an additional extension (eg: `image.ia8.png`)

### `IHQ / SHQ` texture formats (preview branch)

The IHQ and SHQ formats allow to import 64x64 textures (or equivalent 128x32 etc.) that:

- Fit into texture cache 
- Use direct color with no palletes
- Have full mipmap chains (IHQ only)
- Support per-pixel 1-bit alpha

And so they are recommended (but still experimental) to be used as textures in 3D models with higher total fidelity than other formats allow.

The special `IHQ / SHQ` formats stand for **Interpolated High Quality** and **Subtractive High Quality**. These formats optimize the texture cache space usage with custom configurations and a special detail texture in hardware. The main idea is to use a I4 or IA4 texture as the highly detailed intensity texture and a lower scale RGBA16 texture as the source of the hue.
The difference between the formats is how the detail textures are interpolated, IHQ uses a direct linear mipmap interpolation, providing support for full mipmap chains, while SHQ uses subtractive blending which gives better colors overall, but has no mipmaps.

SHQ textures as they are achieved by using a detailed texture and a subtractive color texture:

![image](https://github.com/user-attachments/assets/855fe0a9-4056-4a97-ad91-162bc50b2eae) - 
![image](https://github.com/user-attachments/assets/505f51f9-9f9a-4148-bcf7-e4c23dd84999) = 
![image](https://github.com/user-attachments/assets/09ad5971-a43c-4b38-9c85-1c512a44e726)

IHQ textures as they are achieved by using a detailed texture and a interpolated color texture:

![image](https://github.com/user-attachments/assets/5426ef74-332e-4b90-9d6b-a90f7abfb7ab) *
![image](https://github.com/user-attachments/assets/b0eb07bc-2c20-437d-b62b-cd2e4c9becb5) =
![image](https://github.com/user-attachments/assets/ee3baa6f-a215-40aa-9b4f-9f6bdec4add0)

Scenes made with these formats can have both texture detail, mipmaps and transparency (distant trees in this instance). Especially if textures are made to be mirrored to achieve resolutions of 128x128 or equivalent:

![image](https://github.com/user-attachments/assets/05d6ff17-b951-4f8b-9eca-0020f8003e06)

### Lossy compression (preview only)

Libdragon support lossy-compressed sprites (think JPEG). The main usecase is similar to that of standard lossless compression: reduce ROM size. Currently, libdragon features a single lossy codec based on H264 Intrapicture format. 

Lossy sprites are decompressed immediately when loaded and converted into standard RGBA16 sprites. After that, they can be used just like normal sprites. Loading a lossy sprite thus is much slower than a standard sprite has it needs to go through H264 decompression plus YUV to RGB conversion. 

| Image (click to zoom)  | Pixel size | Compression   | ROM Size | Loading time |
| ------ | ------------- | ------------------ | -- | -- |
| <a href="https://github.com/user-attachments/assets/00e7cab3-db31-4f2a-8a2e-9c11b31e020a"> <img width="128" alt="bg1s" src="https://github.com/user-attachments/assets/00e7cab3-db31-4f2a-8a2e-9c11b31e020a" /></a> | 320x240 | Lossless (Level 1) | 111,344 | 25 ms |
| <a href="https://github.com/user-attachments/assets/00e7cab3-db31-4f2a-8a2e-9c11b31e020a"> <img width="128" alt="bg1s" src="https://github.com/user-attachments/assets/00e7cab3-db31-4f2a-8a2e-9c11b31e020a" /></a> | 320x240 | Lossless (Level 2) | 97,704 | 51 ms |
| <a href="https://github.com/user-attachments/assets/00e7cab3-db31-4f2a-8a2e-9c11b31e020a"> <img width="128" alt="bg1s" src="https://github.com/user-attachments/assets/00e7cab3-db31-4f2a-8a2e-9c11b31e020a" /></a> | 320x240 | Lossless (Level 3) | 84,266 | 314 ms |
| <a href="https://github.com/user-attachments/assets/812728ba-2406-4510-b574-20475d47d2b9"> <img width="128" alt="bg1s debug" src="https://github.com/user-attachments/assets/812728ba-2406-4510-b574-20475d47d2b9" /></a> | 320x240 | Lossy (90%) | 38,044 | 174 ms |
| <a href="https://github.com/user-attachments/assets/35e3e228-88d6-40b1-adb9-2a87ddb22d35"> <img width="128" alt="bg1s debug" src="https://github.com/user-attachments/assets/35e3e228-88d6-40b1-adb9-2a87ddb22d35" /></a> | 320x240 | Lossy (75%) | 22,191 | 152 ms |
| <a href="https://github.com/user-attachments/assets/1769f1d6-cda6-4fe0-9a39-fdc07cd2c9db"> <img width="128" alt="bg1s debug" src="https://github.com/user-attachments/assets/1769f1d6-cda6-4fe0-9a39-fdc07cd2c9db" /></a> | 320x240 | Lossy (50%) | 14,496 | 126 ms |

To use the lossy sprite support at runtime, you must call the `lossysprite_init()` function at boot. After that, you can use a regular `sprite_load()` call to load a lossy sprite.

Lossy sprites are currently only converted to RGBA16 at loading, and can not encode any alpha channel.

### `ZBUF` format

The special `ZBUF` format is used to create a sprite encoded in the same format of the RDP Z-Buffer (a custom 14-bit floating point format). This can be useful to preload the Z-Buffer with some precalculated values; for instance, it is useful to reproduce a 2D background which is intermexed with 3D models (like Final Fantasy 7, Resident Evil 2, etc.).

The input image must be a greyscale PNG, where black is used for near pixels, and white for far pixels. Notice that the Z-Buffer has much more than 8bpp of precision, so it is suggested to render the PNG with a depth of 16 bpp, to preserve information. 8-bit greyscale PNGs are accepted but the precision will be lower.

![image](https://github.com/user-attachments/assets/5db122ae-f489-4af7-9661-1094eb6bd922)
![image](https://github.com/user-attachments/assets/aff0dda1-3499-4d1f-a9a3-83bc2e2701c0)


## Sampling parameters

It is possible to optionally embed the RDP texture sampling parameters within the `.sprite` file. These parameters affect how RDP samples the texture while used, and they allow effect like wrapping, mirroring, etc. If a sprite embeds sampling parameters, they can be queried at runtime via `sprite_get_texparms()`, and they are automatically applied when running `rdpq_sprite_upload()` to load the sprite into TMEM.

This is the option to specify the parameters:

```
   --texparms <x,s,r,m>          Sampling parameters:
                                 x=translation, s=scale, r=repetitions, m=mirror
```

 * `translation`: this is a floating point signed value that specifies a translation value applied to the texture.
 * `scale`: this is an integer scale factor to apply to the texture, expressed as a power-of-two exponent. For instance, specifying `3` makes the texture 8 times bigger (virtually). Notice that scale is applied before any translation. Negative values are also allowed (to shrink the texture).
 * `repetitions`: a positive floating point value specifying how many times the texture (virtually) repeats, before clamping. For instance, assuming a 32x32 texture, specifying `2.5` as `repetitions` means that the texture repeats two and a half time (creating a virtual 80x80 texture), and outside that it is clamped. **NOTE**: only texture whose size is a power of two can repeat. Specifying `1` causes the texture to just clamp outside. Specifying a very high number or `inf` (for infinite) effectively causes the texture to repeat forever.
 * `mirror`: either `0` or `1`. When `1`, at each repetition, the texture is mirrored.

Notice that the above settings apply to both the horizontal and vertical coordinates. If you want to specify different behaviors for horizontal and vertical coordinates, you can use this variant of the command line option:

```
   --texparms <x,x,s,s,r,r,m,m>  Sampling parameters (different for S/T)
```

where each parameter must be specified twice (once per axis).

## Avoiding Outline Artifacts with Bilinear Filtering

Sometimes, sprites show an unexpected bright or dark outline that does not appear in the source asset. This typically occurs when rendering a texture with alpha cutoff or blending enabled and using bilinear interpolation.

**Source asset: no visible outline**  
![image](https://github.com/user-attachments/assets/c6080971-7d0e-47fb-9b18-fa5844c0f4c9)

**Rendered with bilinear filtering and rotation: unexpected glow appears**  
![image](https://github.com/user-attachments/assets/0cef53d9-c888-4711-a2ba-e62c6c5acad5)

The unexpected outline originates from RGB values stored in fully transparent pixels. These RGB values are normally invisible, but they still participate in texture interpolation and therefore leak into the final render.

To quickly verify what the N64 is actually sampling from your source textures, temporarily render the sprite without `rdpq_set_alpha_compare` or any blending modes. This shows the raw texel data, including hidden RGB values in transparent areas.

**Debug view of source asset, rendered in standard mode**  
![image](https://github.com/user-attachments/assets/082d1313-af7a-406a-93ae-b45f3bcdcf7c)

On the image above, hidden RGB data on transparent pixels becomes visible, in this case a bright color from the indexed palette. A common fix is to “bleed” the edge colors of the visible pixels into the neighboring, fully transparent pixels. With this, interpolation uses meaningful color values instead of black or other unwanted background colors.

**Debug view of corrected source asset, after bleeding edge pixels twice (exaggerated for visualization purposes)**  
![image](https://github.com/user-attachments/assets/19dc9d50-ca67-4fb3-adb8-fa3acc33b888)

**Final result**  
![image](https://github.com/user-attachments/assets/eed0319e-7e88-48d7-8db0-ebad2dc09088)

A quick external tool for fixing this type of artifact is “Cutout Alpha Fixer,” available on Romhacking.net:  
https://romhacking.com/tools

Many sprite-asset tools also support scripting, which can be used to automate edge bleeding during asset-export.
