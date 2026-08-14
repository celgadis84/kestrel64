Mkfont is the tool that is used to import a font from TrueType / OpenType format and convert it into the libdragon native `.font64` format. Mkfont is a sophisticated tool that performs many advanced optimizations automatically depending on the provided file. This page explains the various options and give some examples of the possible conversions.

In addition to standard TrueTypes, mkfont also supports also colored fonts ([OpenType SVG fonts](https://www.colorfonts.wtf)) and pure bitmap fonts in the [BMFont format](https://www.angelcode.com/products/bmfont/doc/file_format.html) ([example](https://github.com/DragonMinded/libdragon/blob/preview/examples/fontgallery/assets/CakeIcing.fnt)). OpenType colored fonts are always rasterized to bitmaps during conversion. 

AngelCode BMFont is a tool that is normally used to convert TrueTypes to bitmaps, but it has also become a standard for providing bitmap fonts in PNG format and their associated metadata (list of glyphs, positions within the picture, etc.). Notice that you shouldn't use the BMFont to convert TrueType fonts (mkfont has many more N64-specific optimizations); the support is meant just for bitmap formats.

To quickly test a font, just drop it among the assets of the [fontgallery example](https://github.com/DragonMinded/libdragon/tree/preview/examples/fontgallery) and rebuild it. You can tweak conversions options in the `Makefile` if needed, but default conversion often gives a first reasonable result.

If you want to learn how to draw text on the screen using a `.font64` file crated by Mkfont, you can have a look at the documentation at the top of [rdpq_text.h](https://github.com/DragonMinded/libdragon/blob/preview/include/rdpq_text.h) to get a feeling of the API. The documentation there contains also several examples.

## Supported font types

rdpq_font currently supports 5 different font types:

| Name | High contrast | Low contrast |
| -- |  -- | -- |
| **Monochrome**. Old school fonts, hand drawn pixel by pixel. Each pixel is either solid or transparent, there is no aliasing. Today they are shipped in TTF format. These fonts lack an outline so typically they don't look very well in a low contrast setting. | <a href="https://github.com/DragonMinded/libdragon/assets/1014109/beac397f-afa9-4d01-b25c-bafc095caf1f"><img width="700" alt="Screenshot 2024-06-24 alle 13 59 49" src="https://github.com/DragonMinded/libdragon/assets/1014109/beac397f-afa9-4d01-b25c-bafc095caf1f"></a> | <a href="https://github.com/DragonMinded/libdragon/assets/1014109/7d651152-b17a-496a-a345-fecdc53ed94c"><img width="700" alt="Screenshot 2024-06-24 alle 13 59 52" src="https://github.com/DragonMinded/libdragon/assets/1014109/7d651152-b17a-496a-a345-fecdc53ed94c"></a> |
| **Monochrome with outline**. mkfont allows to add an outline to monochrome fonts during conversion. Doing this to monochrome fonts usually helps a lot with readability in low-contrast setting, at the expense of making the font roughly twice as big in memory. Both the color of the fill and the color of the outline can be changed at runtime. | <a href="https://github.com/DragonMinded/libdragon/assets/1014109/2ab62366-60c1-4ac0-ba12-8d4ed265f9c3"><img width="752" alt="Screenshot 2024-06-24 alle 13 57 11" src="https://github.com/DragonMinded/libdragon/assets/1014109/2ab62366-60c1-4ac0-ba12-8d4ed265f9c3"></a> | <a href="https://github.com/DragonMinded/libdragon/assets/1014109/c085d8cb-50fe-40cc-bcb2-39d23732a67a"><img width="752" alt="Screenshot 2024-06-24 alle 13 57 34" src="https://github.com/DragonMinded/libdragon/assets/1014109/c085d8cb-50fe-40cc-bcb2-39d23732a67a"></a> |
| **Aliased**. This is a standard, modern font face. While technically the font is still made of a single color, the font is fully anti-aliased (as each pixel encodes a transparency factor, like an alpha channel). Aliased fonts look extremely beautiful and round on CRTs even at 320x240, but sometimes they look a bit "blurry" in emulators (especially without upscaling). | <a href="https://github.com/DragonMinded/libdragon/assets/1014109/2170799c-3504-4b39-a5ff-f7606bd8d67a"><img width="752" alt="Screenshot 2024-06-24 alle 13 58 23" src="https://github.com/DragonMinded/libdragon/assets/1014109/2170799c-3504-4b39-a5ff-f7606bd8d67a"></a> | <a href="https://github.com/DragonMinded/libdragon/assets/1014109/304f19b0-6e8b-469a-80b8-2fd66ff720de"><img width="752" alt="Screenshot 2024-06-24 alle 13 58 26" src="https://github.com/DragonMinded/libdragon/assets/1014109/304f19b0-6e8b-469a-80b8-2fd66ff720de"></a> |
| **Aliased with outline**. Just like for monochrome fonts, mkfont also allows to create outlines for aliased fonts. In this case, the size of the outline can be tuned with subpixel precision, so for instance it is possible to request a 1.5 pixel outline for a slightly bolder look. Both the outline color and the fill color can be changed at runtime. | <a href="https://github.com/DragonMinded/libdragon/assets/1014109/d4dde1b1-8fa6-499a-b9fc-32b7d4fc8b85"><img width="752" alt="Screenshot 2024-06-24 alle 15 48 45" src="https://github.com/DragonMinded/libdragon/assets/1014109/d4dde1b1-8fa6-499a-b9fc-32b7d4fc8b85"></a> | <a href="https://github.com/DragonMinded/libdragon/assets/1014109/91b0b964-ee5f-4552-84a6-4d71dcfd450a"><img width="752" alt="Screenshot 2024-06-24 alle 15 48 51" src="https://github.com/DragonMinded/libdragon/assets/1014109/91b0b964-ee5f-4552-84a6-4d71dcfd450a"></a> |
| **Bitmap fonts**. These fonts can come from either colored TTF/OTF files (containing bitmaps or SVG), or BMFont files, as they are shipped as PNGs. This gives full creativity to the designer, as the font can embed gradients, or colors of any sort. Obviously, the font color cannot be changed at runtime, as it is pre-colored. At the same time, the font can contain outlines or drop-shadows to help in low contrast settings. | <a href="https://github.com/DragonMinded/libdragon/assets/1014109/f7267f13-f150-41d2-b17c-b171f54bf29f"><img width="752" alt="Screenshot 2024-06-24 alle 15 51 13" src="https://github.com/DragonMinded/libdragon/assets/1014109/f7267f13-f150-41d2-b17c-b171f54bf29f"></a> | <a href="https://github.com/DragonMinded/libdragon/assets/1014109/88c12afb-16cb-4ecf-8eae-1382a722902e"> <img width="752" alt="Screenshot 2024-06-24 alle 15 51 19" src="https://github.com/DragonMinded/libdragon/assets/1014109/88c12afb-16cb-4ecf-8eae-1382a722902e"></a>

For TTF fonts, `mkfont` will automatically detect if a file is aliased or monochrome and choose the appropriate type. To add an outline, you must specify `--outline 1` on the command line (see below). If you want an aliased font file to render in monochrome, you can use `--monochrome`, but normally the result is not very pretty.

### Technical details on atlas generations

When mkfont processes a font, it creates texture atlases, which are bitmaps where all glyphs are rasterized and packed together in the smallest possible form factor, while still respecting N64 RDP hardware texture limits.

<img width="894" alt="Screenshot 2024-06-17 alle 17 50 32" src="https://github.com/DragonMinded/libdragon/assets/1014109/d260b5f7-9081-4176-8d27-a63d71445780">

Depending on the font type (explained before), atlases use different pixel formats. 

 * Monochrome, no outline: in this case, atlases are generated using a special 1 bpp packed format, since each texel requires just 1 bit to encoded. While RDP does not natively support 1 bpp, a trick is used by encoding 4 layers into a CI4 atlas and then using special palettes to isolate each layer. Fonts packed in this format are extremely tight.
 * Monochrome, outline: in this case, atlases are generated using a special 2 bpp packed format. In fact, for each texel we have 3 possible values to encode: either the texel is transparent, or it is part of the outline, or it is part of the glyph. This means that 2 bits are enough, and using a trick similar to the previous one, we can encode two layers into a CI4 atlas.
 * Aliased, no outline: in this case, atlases are generated using the I4 texture format (4 bpp). Each texel encodes up to 16 levels of transparency (coverage) that are used to reproduce the aliasing effect. As described above, the effect happens to work much better on CRTs than on emulators.
 * Aliased, outline: in this case, atlases are generated using the IA8 texture format (8 bpp). Each texel encodes up to 16 levels of transparency in the alpha channel, and up to 16 color gradients interpolating between the fill color and the outline color.
 * Bitmap: by default, fonts are converted into textures in RGBA16 format, as we assume this format is used for very colorful fonts. mkfont currently doesn't have any logic to automatically switch to a lighter/heavier texture format, but you can use `--format` to change it manually. Supported formats are: `RGBA32`, `RGBA16`, `CI8`, `CI4`. In particular, if you are converting an aliased colored font, you might need to use RGBA32 to reproduce antialias correctly.

Excluding the bitmap format, in all other cases, fonts are encoded without specific colors, so that it is possible to choose the colors at runtime, and even switch between different styles using the same font.

## Basic mkfont usage

In its most basic usage, just call `mkfont` specifying the truetype font:

```
$ $N64_INST/bin/mkfont Roboto-Medium.ttf
```

This will generate a file called `Roboto-Medium.font64` in the same directory, that can be directly used with rdpq. For instance:

```C
const int ROBOTO = 1;

rdpq_text_register_font(ROBOTO, rdpq_font_load("rom:/Roboto-Medium.font64"));

[...]

rdpq_text_printf(NULL, ROBOTO, 100, 100, "Hello, %s!", "world");
```

You will normally want to run this as part of your Makefile (see fontgallery's [Makefile](https://github.com/DragonMinded/libdragon/blob/preview/examples/fontgallery/Makefile) for an example), but running `mkfont` manually can be useful to test its features.

> [!TIP]
> If you use libdragon docker, remember to use `libdragon exec mkfont` to run mkfont, or any other tool.

## Command line options

These are the command line options supported by mkfont:

```
~/S/n/l/t/mkfont (gfx-rdp|✔) $ ./mkfont -h
mkfont -- Convert TTF/OTF/BMFont fonts into the font64 format for libdragon

Usage: ./mkfont [flags] <input files...>

Command-line flags:
   -o/--output <dir>         Specify output directory (default: .)
   -v/--verbose              Verbose output
   --no-kerning              Do not export kerning information
   --ellipsis <cp>,<reps>    Select glyph and repetitions to use for ellipsis (default: 2E,3)
   -c/--compress <level>     Compress output files (default: 1)
   -d/--debug                Dump also debug images

TTF/OTF specific flags:
   -s/--size <pt>            Point size of the font (default: whatever the font defaults to)
   --monochrome              Force monochrome output, with no aliasing (default: off)
   --outline <width>         Add outline to font, specifying its width in (fractional) pixels
   --char-spacing <width>    Add extra spacing between characters (default: 0)
   --display <WxH[,A:B]>     Specify target display resolution and optional aspect ratio
                             (e.g., --display 320x240 or --display 320x240,16:9)
                             Default assumes 4:3 display ratio
   --format <format>         Specify the output texture format for color fonts.
                             Valid options are: RGBA16, RGBA32, CI4, CI8 (default: autoselect)
   --var-axis <tag=value>    Override axis value of variable font
                             (e.g., --var-axis wght=800)
                             Can be specified multiple times.

   Glyph selection modes (choose one of the following):
   --charset <file>          Create a font that covers all and only the glyphs used in the
                             specified file (in UTF-8 format).
   -r/--range <start-stop>   Range of unicode codepoints to convert, as hex values (default: 20-7F)
                             Can be specified multiple times. Use "--range all" to extract all
                             glyphs in the font.

BMFont specific flags:
   --format <format>         Specify the output texture format. Valid options are:
                             RGBA16, RGBA32, CI4, CI8 (default: RGBA16)
```

### `--size`

This is probably the most common option that you will likely need to use. It specifies the font height in pixels at which the font should be exported. Notice that, contrary to most TrueType fonts, font64 fonts are composed with bitmaps so they cannot be resized at runtime (well they could, but they'd look horrible). The bigger the size, the bigger will be the font64 file and thus the number of different textures that the font will be split upon.

For instance, the ASCII subset of Roboto at size 13 can be exported into a single 86x64 font atlas:

``` 
$ $N64_INST/bin/mkfont -v --size 13 Roboto-Medium.ttf
Converting: Roboto-Medium.ttf -> ./Roboto-Medium.font64
asc: 13 dec: -4 scalable:1 fixed:0
processing codepoint range: 0020 - 007F
aliased glyphs detected (format: 4 bpp)
created atlas 0: 86 x 64 pixels (94 glyphs)
collecting kerning information
compressed: ./Roboto-Medium.font64 (4680 -> 3459, ratio 73.9%)
```

<img width="258" src="https://github.com/DragonMinded/libdragon/assets/1014109/e4b0f818-3801-4c77-be4c-655dc626f506">

But if you try to raise the size to 16, it ends up being split into two atlases:

```
$ $N64_INST/bin/mkfont -v --size 16 Roboto-Medium.ttf
Converting: Roboto-Medium.ttf -> ./Roboto-Medium.font64
processing codepoint range: 0020 - 007F
aliased glyphs detected (format: 4 bpp)
created atlas 0: 126 x 64 pixels (83 glyphs)
created atlas 1: 14 x 60 pixels (11 glyphs)
collecting kerning information
compressed: ./Roboto-Medium.font64 (6592 -> 4699, ratio 71.3%)
```

First atlas:<br/>
<img width="378" src="https://github.com/DragonMinded/libdragon/assets/1014109/e32b84f3-2c5f-4b6a-952a-e7c9709f9412">
<br/>Second atlas:<br/>
<img width="42" src="https://github.com/DragonMinded/libdragon/assets/1014109/d10c1c40-fcff-47c7-8c7c-0ea9f31897aa">

Multiple atlases are handled just as well at runtime by minimizing texture loads and switches, but they are still more resource intensive to handle.

#### `--size` and monochrome fonts

`--size` is optional: if it is not specified, the font will be output to the default size decided by the font designer. In case of an aliased font, this size is pretty arbitrary and probably has little to do with your needs. Monochrome fonts instead, when shipped in TTF format, expect to be rendered exactly at a specific size; when `--size` is not specified, mkfont will try and find the correct native size for the font, so that it is rendered correctly.

### `--outline <size>`

To add an outline to fonts, use `--outline <size>`, where `<size>` specifies the size of the outline in fractional pixels. For monochrome fonts, `1` is normally the only reasonable option and it does provide the best outcome. For aliased fonts, you can play a bit with fractional values like `1.2` or `1.5` to give a slightly bolder look to the font.

Outlines are drawn by mkfont and baked into the `font64` file, so they cannot be turned on and off at runtime. If for some reason you need this, you will have to export the same font twice, with and without outlines.

In general, outlines make the font64 file twice as big in memory (and also in ROM, before compression at least -- though compression ratios tend to be similar in the two cases).

### `--range` and Unicode

By default, `mkfont` only exports glyphs in the ASCII range. Nonetheless, rdpq_text and mkfont are fully Unicode aware: all printing functions accept UTF-8 strings, and mkfont is able to generate fonts with glyphs coming from the full Unicode range.

To export more glyphs, you must explicitly use `--range`, possibly multiple times, to specify range of glyphs that you want to be exported. Please refer to an [Unicode range table](https://jrgraphix.net/r/Unicode/) to quickly find interesting ranges of codepoints.

For instance, if you want to also export Hiragana and Katakana glyphs to be able to write Japanese text, use:

```
$ $N64_INST/bin/mkfont -v --size 13 --range 20-7F --range 3000—30FF arial-unicode-ms.ttf
Converting: arial-unicode-ms.ttf -> ./arial-unicode-ms.font64
processing codepoint range: 0020 - 007F
monochrome glyphs detected (format: 1bpp)
created atlas 0: 64 x 20 pixels (94 glyphs)
processing codepoint range: 3000 - 30FF
created atlas 1: 64 x 64 pixels (151 glyphs)
created atlas 2: 112 x 21 pixels (89 glyphs)
compressed: ./arial-unicode-ms.font64 (10464 -> 6385, ratio 61.0%)
```

In the above example, mkfont generates one atlas for the ASCII range, and two of them for the Katakana range. mkfont will always generate disjoint atlases for different ranges. This is because normally characters from different ranges are less likely to be used in the same text (eg: how many times the same text will mix Japanese and Greek glyphs?), with the obvious exceptions of ASCII characters.

After exporting, using Unicode glyphs is as easy as referencing them in your source code:

```C
rdpq_text_printf(NULL, FONT_ARIAL_UNICODE, 100, 100, "いつ にほんに いきますか。");
```

<img width="360" alt="Screenshot 2024-06-17 alle 12 31 08" src="https://github.com/DragonMinded/libdragon/assets/1014109/c0bec270-f94f-4764-aa1f-523bc5969361">

The string must be written in UTF-8 in the source code, which should be the default of any modern programming editor.

> [!TIP]
> mkfont will create separate texture groups for each different `--range` option. This provides the maximum performance if each `--range` roughly specifies one character set / language, with the implicit assumption that within a single printed text, there will mostly be only letters of a single charset (plus ASCII). So try to avoid large ranges like `--range 20-5FF` and instead split in multiple ranges, keeping always ASCII by itself.

### `--charset` for fixed list of glyphs

An alternative to specifying ranges is to let mkfont figure it out by itself all glyphs that need to be included by providing a list of all texts that will need to be displayed. While this probably requires some effort in collecting the text (a practice that is often useful also for other goals such as translations), it might allow to decreases the actual font sizes a lot especially for huge Unicode ranges such CJK.

The option `--charset` allows to specify a file, which should contain the list of all texts that must be displayed with the font. Mkfont will scan it and extract a list of all glyphs used at least once, and then proceed to create a font that only contains those glyphs. It will also automatically decide how many atlases to create (will split them by Unicode ranges, to group glyphs used within the same language scripts). If you are going to use CJK fonts, this is going to be important because otherwise the font might become too big.

The file must be encoded in UTF-8 format. When `--charset` is specified, there is no need to specify `--range`, as it will be implied by the charset itself.

### `--no-kerning`

By default, mkfont exports kerning information from the TTF font, if provided. Kerning allows font designers to specify spacing adjustments between specific couple of glyphs. The screenshots show a typical kerning adjustment, where some spacing is remove between the letters `T` and `a`, as the `a` glyph is small enough.

<img width="200" alt="Screenshot 2024-06-16 alle 16 24 55" src="https://github.com/DragonMinded/libdragon/assets/1014109/e2a3b98c-bb7c-4a9c-82be-0f3879a605d9">
<img width="200" alt="Screenshot 2024-06-16 alle 16 24 29" src="https://github.com/DragonMinded/libdragon/assets/1014109/45f9b652-ad56-4d6d-823e-85812ca40b92">

Left image is without kerning adjustments, right image is with kerning adjustment.

Kerning does increase `.font64` file size and brings some very tiny runtime overhead. For instance, the Pacifico font used in these screenshots is 10568 bytes (with default libdragon compression), and its size can be reduced to 9821 bytes by disabling kerning.

Normally, it is suggested to leave kerning on, unless there's a memory or performance concern (though in that case, it might be better to choose a font that doesn't require kerning rather than ignoring the kerning that the designer thought it was needed).

### `--ellipsis`

This option allows to configure the glyph to use as ellipsis when drawing in the `WRAP_ELLIPSES` word wrap mode. rdpq_text allows to specify several word wrap modes:

```C
typedef enum {
    WRAP_NONE = 0,         ///< Truncate the text (if any)
    WRAP_ELLIPSES = 1,     ///< Truncate the text adding ellipsis (if any)
    WRAP_CHAR = 2,         ///< Wrap at character boundaries 
    WRAP_WORD = 3,         ///< Wrap at word boundaries 
} rdpq_textwrap_t;
```
which can be specified as text parameters:

```C
typedef struct rdpq_textparms_s {
    int16_t width;           ///< Maximum horizontal width of the paragraph, in pixels (0 if unbounded)
    int16_t height;          ///< Maximum vertical height of the paragraph, in pixels (0 if unbounded)
    [...]
    rdpq_textwrap_t wrap;    ///< Wrap mode
```

When `WRAP_ELLIPSES` is specified, the text is truncated to the specified width, and the ellipsis character is inserted to show elision:

<img width="467" alt="Screenshot 2024-06-16 alle 22 45 54" src="https://github.com/DragonMinded/libdragon/assets/1014109/4ed6275c-758f-4be2-9ed3-d11da6038a21">

By default, three ASCII full stops (U+2E `.`) are inserted as ellipses, but `--ellipses` allows to configure this. It is possible in fact to specify a custom codepoint to use for ellipses and the number of repetitions. For instance, the use the more compact Unicode ellipsis character (U+2026 `…`), the syntax to use is `--ellipses 2026,1`.

### `--var-axis <tag>=<value>` for variable font axis customization

For [variable fonts](
https://en.wikipedia.org/wiki/Variable_font), i.e. fonts that support customizing various font parameters (or axes) within a continuous range, this option allows setting an axis to a custom value.
This can for example be the weight, the character width or the slant of the font. 
This argument can be used multiple times to set multiple different axes.

Examples of how to use the option with some standard font axes that are frequently supported:
| Axis         | Tag    | Unit                                  | Example  |
| ------------ | ------ | ---                                   | ---      |
| Font weight  | `wght` | Usually `100` (thin) to `900` (black) | `--var-axis wght=700` for bold text. |
| Font width   | `wdth` | Percentage of normal width            |  `--var-axis wdth=125` for 125% width |
| Slant        | `slnt` | Degrees (left-leaning positive)       |`--var-axis slnt=-10` for 10 degrees to the right (note negative value) |
| Italic       | `ital` | `0` or `1`                            | `--var-axis ital=1` for italic.  |
| Optical size | `opsz` | Points (pt)                           |`--var-axis opsz=16` for optical size of 16 pt. |

Which axes are supported and which value range is supported for an axis will depend on the particular font, check the documentation of the font to ensure which values are supported. If the verbose level is above two, mkfont will print a list of
all available axes for the chosen font and their default values and value ranges.


## Colored fonts

Modern colored fonts (normally shipped in OpenType SVG format) are normally drawn for current, high resolution screens, as the format was born in 2016. To support importing such fonts, mkfont will rasterize SVG glyphs to bitmaps automatically and create an atlas like for any other font. Normally, these fonts will be really usable only at large sizes (`--size 24` or more), so mainly for titles or huge on screen text.

<img width="256" height="154" alt="gilbert font64_0" src="https://github.com/user-attachments/assets/d8e36737-46b2-4d7d-9332-27ffdb2b70dd" />

By default, mkfont will select either RGBA16 or RGBA32 as texture formats depending on the usage of the alpha channel in rasterized glyphs. This should result in good quality without manual intervention (click to zoom):

<a href="https://github.com/user-attachments/assets/4910d9cf-0090-49dc-b3c2-8e0ecb2b8c70"><img width="250" alt="Screenshot 2025-08-31 alle 02 00 19" src="https://github.com/user-attachments/assets/4910d9cf-0090-49dc-b3c2-8e0ecb2b8c70" /></a>
<a href="https://github.com/user-attachments/assets/5188fce6-b2c4-440e-9f10-5d231ca2b3a7"> <img width="250" alt="Screenshot 2025-08-31 alle 02 29 38" src="https://github.com/user-attachments/assets/5188fce6-b2c4-440e-9f10-5d231ca2b3a7" /></a>
<a href="https://github.com/user-attachments/assets/919f72a6-44bd-41fa-bad5-d521856a063c"><img width="250" alt="Screenshot 2025-08-31 alle 02 36 11" src="https://github.com/user-attachments/assets/919f72a6-44bd-41fa-bad5-d521856a063c" /></a>

If you want to force a different format, you can use `--format` and choose between CI4, CI8, RGBA16, RGBA32. The first three will likely result in less RDRAM usage (smaller file) and possibly faster rendering, but they don't allow for a real alpha channel which is required for antialiasing. For instance, this is one of the previous fonts rendered in CI4, which uses approximately 8 times less memory (click to zoom):

<a href="https://github.com/user-attachments/assets/ef4a8434-72fe-4da4-bacb-7a63a9a1ffca"><img width="250" alt="Screenshot 2025-08-31 alle 22 45 10" src="https://github.com/user-attachments/assets/ef4a8434-72fe-4da4-bacb-7a63a9a1ffca" /></a>

Zoomed comparison between CI4 and RGAB32 showing the absence of anti-aliasing:

<img width="300" height="282" alt="Screenshot 2025-08-31 alle 22 51 14" src="https://github.com/user-attachments/assets/7d4d2055-7438-4fbc-b22a-72ee17b380da" />
<img width="300" height="282" alt="Screenshot 2025-08-31 alle 22 51 19" src="https://github.com/user-attachments/assets/7e4cd0ac-4435-4cdd-b6ac-1ca20db61471" />


## Encoding CJK fonts

Mkfont is quite flexible and implements several tricks to allow to use it also for CJK fonts. There are a few suggestions that you might want to follow to create fonts that are small enough:

* Use the `--charset` option. N64 environment is too constrained and you cannot expect to convert and use at runtime a whole CJK font with 20K+ glyphs. To limit the font to the glyphs that you actually then need to display, you must use `--charset` to provide the glyph list (or, simply, a list of all texts that will be later printed at runtime). This will trim down the font size quite a bit.
* Consider using `--monochrome`. Your font might or might not be a monochrome font already, but in any case you should consider converting it to monochrome. This will drop aliasing detail from each glyph but might make your font 50% smaller. It can help quite a bit.
* Remember to convert texts to UTF-8, possibly at build time. Especially if you are porting an older codebase, your text might be encoded in a local encoding like Shift-JIS or Big-5. Mkfont and rdpq_font *only* support UTF-8, and libdragon does not offer conversion functions from other encodings. To make sure the font works correctly, make sure to convert the text to UTF-8. If possible, do that at build time, because CJK encoding conversion at runtime requires very large lookup tables that would waste RDRAM.

To give an example, let's consider the font `JF-Dot-K12.ttf` (2.4 MiB), a 12x12 CJK font. Let's first try to convert the ASCII range and the main CJK range. We will disable compression to see the uncompressed file size that also matches the memory usage at runtime.

```
$N64_INST/bin/mkfont --range 20-7F --range 4E00-9FFF --compress 0 JF-Dot-K12.ttf
```

This creates a 669 KiB font64 file, containing 9589 glyphs. Let's now try to switch to the monochrome version:

```
$N64_INST/bin/mkfont --range 20-7F --range 4E00-9FFF --monochrome --compress 0 JF-Dot-K12.ttf
```

The font has now decreased to 242 KiB which is already a huge saving. Let's now provide a charset that shows that only 1203 glyphs are actually used at runtime:

```
$N64_INST/bin/mkfont --charset texts.txt --monochrome --compress 0 JF-Dot-K12.ttf
```

Now the font file is just 33 KiB. This is a huge saving compared to the initial, naive attempt.


## Changing aspect ratio

By default, mkfont generates a font that is valid for a standard 1:1 PAR (pixel aspect ratio). This is the default for libdragon when you select a 320x240 or 640x480 framebuffer via `display_init` using a standard `resolution_t` such as `RESOLUTION_320x240` or `RESOUTION_640x480`. If you use a different resolution (eg: a custom one specifying `resolution_t::width` and `resolution_t::height`) and/or modify the display aspect ratio of the framebuffer (via `resolution_t::aspect_ratio`), fonts might appear squished or distorted. In this case, you should use the `--display` option.

`--display` lets you specify the pixel aspect ratio with the same fields you configure in `resolution_t`. This makes for a very simple API. For instance, a videoplayer might do something like this:

```C
display_init((resolution_t){
   .width = 288, .height = 220, .aspect_ratio=16./9,
}, ...);
```

for a video that is encoded in 288x220 with a 16:9 intended aspect ratio. If you want to display some text above the video (eg: subtitles), you must make sure to pass the same parameters to `mkfont` that is:

```
$N64_INST/bin/mkfont --size 22 --display 288x220,16:9 Roboto.ttf
````

This will make sure the font will look correctly when displayed in the above context.

Notice that runtime scaling of fonts is not fully supported because it looks ugly for most fonts; if you want to use the same font in different resolutions, the suggested way is to generate multiple font64 files and select the correct one at runtime.