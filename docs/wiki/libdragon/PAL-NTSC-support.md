This page describes what you have to do to make a game that runs on PAL, NTSC, and MPAL, that is, all TV types that were supported by the various versions of the Nintendo 64 hardware.

# Quick cheat sheet

In general, all libdragon applications will by default **automatically support** all TV types. The whole of libdragon and its APIs have been designed to abstract this issue as much as possible. As a result, it is generally **not needed to make separate builds** for PAL vs NTSC.

What you have to do is:

* **Use the same framebuffer resolution**. It's not required to change framebuffer resolution depending on TV type.
* **Make sure to activate the VI resampling.** The last parameter of `display_init()` must be either `FILTERS_RESAMPLE`, `FILTERS_RESAMPLE_ANTIALIAS`, or `FILTERS_RESAMPLE_ANTIALIAS_DEDITHER`, so basically anything that contains the word `RESAMPLE`. This is useful to avoid scaling artifacts on PAL.
* **Do not assume that your main loop will run at a fixed refresh rate**, like 50 Hz or 60 Hz. You can use [`display_get_delta_time()`](https://github.com/DragonMinded/libdragon/blob/1b6426fc89c933961bc21af406e1df3be09d25d1/include/display.h#L404-L417) to get the so called "delta time": how much has passed since last frame. Make sure to use this value when calculating updates to your logic, physics or animations. If you don't this, your game speed will likely be 16.6% slower on PAL consoles than on NTSC consoles.

Following these simple guidelines is all you have to do to make your game NTSC, PAL, and MPAL compatible. The rest of this page provides further insights.

### How to test with emulators

Given that Libdragon ROMs support all regions, you can force emulators to use a specific TV type to test them. For instance, with Ares, you can go to the Settings menu -> Boot Options -> Region Preference to select the preferred region. Then reboot the ROM and it will use that TV type.

### NTSC vs PAL video resolution

Native video resolution is different on NTSC/MPAL vs PAL. In fact, PAL pixels are not perfect squares: they are wider than they are tall. On a same-size, 4:3 TV, NTSC will fill the display with approximately 640x480 perfectly square pixels, whereas PAL will fill it with 640x576 rectangular pixels (please note that this is a simplification of a very complex topic). So for instance, to draw a perfect square, you can fill 32x32 TV dots on NTSC, but on PAL you must instead fill 32x38 TV dots.

Your framebuffer, meanwhile, can be of any size: in fact, you can create a custom framebuffer size by simply setting the width and height fields of [`resolution_t`](https://github.com/DragonMinded/libdragon/blob/1b6426fc89c933961bc21af406e1df3be09d25d1/include/display.h#L117-L120) and passing it to `display_init()`. For now, let's assume you use 320x240, which is the most popular resolution.

By default, libdragon takes your framebuffer, whatever size it is, and stretches it to fill the 640x480 or 640x576 display. This allows to perfectly preserve the original 4:3 aspect ratio - on PAL, some lines will get doubled (and filtered) but the final picture will be exactly 4:3. This means that you can draw a 32x32 pixel square in your framebuffer, and it will be automatically scaled to cover 32x38 TV dots on PAL, so that it will still appear as a perfect square.

If instead libdragon filled only 640x480 pixels on PAL, using a 320x240 framebuffer, the image would appear squished, with black bars above and below the image, because 32x32 TV dots isn't a square on PAL, but a rectangle. Many commercial games made back in the 90s handled PAL this way (often also accompanied by the failure to account for game speed difference between 50 Hz and 60 Hz).

### 2D graphics - Scaling vs sharp pixels

The scaling performed by VI can use bilinear filtering (you have to turn it on, as explained before), but this of course will create a bit of blurry look, especially on sharp 2D graphics. **There is no practical way to avoid this**. Recall that PAL pixels are not square: your beautiful drawn actor was drawn with square pixels. So there is no way to draw it on PAL with both the correct aspect ratio and sharp pixels. If you drew it without scaling on PAL, the pixels would be sharp, but the actor would be shorter, vertically.

The only way around this would be to have *two versions of your assets*, one for the NTSC square pixels, and one for the PAL square pixels. And this would also mean redrawing the assets one by one: if you just scale them on PC, the sharp pixels would be gone anyway, and they wouldn't look much better than they do when VI does the rescaling.

### 3D graphics

For 3D graphics, the above suggestions still perfectly work. N64 graphics tend to be quite blurry anyway (small textures, dithering, anti-alias, etc.) so the additional scaling performed by VI on PAL isn't normally very visible. 

However, if you really wanted, you *can* avoid that: you can initialize a 320x288 framebuffer on PAL, and then change your 3D viewport configuration to inform that the desired aspect ratio is 4:3. You can use `gluPerspective` in OpenGL and `t3d_viewport_set_perspective()` in Tiny3D, and pass `4.0f / 3.0f` as `aspectRatio` parameter. As a result, your game will still display a 4:3 aspect ratio but will render it into the "native" PAL resolution of 320x288 which the VI can neatly line-double to the 576 scan lines of a PAL display, resulting in completely sharp pixels. Keep in mind, however, that this means that your game will render a larger framebuffer and thus can potentially have a lower framerate.

