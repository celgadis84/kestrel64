# Introduction

Libdragon contains a MPEG-1 player. The player is based on [pl_mpeg](https://github.com/phoboslab/pl_mpeg), but the core algorithms have been replaced by a [RSP ucode](https://github.com/DragonMinded/libdragon/blob/unstable/src/video/rsp_mpeg1.S) that makes use of vectorization to speed up decoding.

The player is normally capable of playing back a 320x240 movie at 800 Kbit/s at 20 fps. You can use this as a ballpark for the expected quality.

# How to encode a video

Libdragon's MPEG player works with raw MPEG1 video streams. Normally, MPEG1 videos come with the `.mpg` extension which signals the presence of the `mpg` container, with interleaved video and audio. Instead, raw MPEG1 streams are normally saved with the `.m1v` extension. To play a movie with audio, then, both a `.m1v` and a `.wav64` file are required (see the [videoplayer example](https://github.com/DragonMinded/libdragon/blob/unstable/examples/videoplayer/videoplayer.c)).

## Encode a video using ffmpeg

ffmpeg is very popular for video encoding, even though it is a bit inflexible with MPEG-1 (probably the format is too old and less developed). To encode a video that libdragon can playback, use this command line:

```
$ ffmpeg -i <INPUTFILE> -vb 800K -vf 'scale=320:-16' -r 20 output.m1v
```

This creates a MPEG-1 file at horizontal resolution 320, vertical resolution auto-defined to match aspect ratio, at 20 FPS, with 800 Kbps of bitrate (average). You can play with the various encoding parameters to improve video quality or reduce stuttering. Notice that resolution disproportionately affect decoding speed, so see below for some suggested resolutions to try.

MPEG1 doesn't allow much flexibility in terms of declaring a custom aspect ratio, so it is advised to always encode it with square pixels (so that the video aspect ratio matches the intended display aspect ratio). The above command line instructs ffmpeg to do so, adding also the constraint of having the vertical resolution multiple of 16 (which is currently required by libdragon decoder).

Unfortunately, ffmpeg does not seem to respect the provided bitrate close enough, so there might still be scenes where bitrates go through the roof. The MPEG-1 encoder does not offer many knobs to avoid this (at least that we could find).

### Gamma correction

If the colors of the video appear to be washed out (saturated to white) after conversion, when played back on a CRT TV or an emulator, there are a few things to check:

 * Make sure that your display resolution (initialized by `display_init`) does not apply gamma correction (use `GAMMA_NONE`). This is a hardware feature to convert linear space colors to gamma-corrected (sRGB) colors while outputting the picture and is useful only if the source pixels are in linear color. That wouldn't be the case, as most assets are normally sRGB already.
 * You can try applying gamma correction to the video, by passing `-vf 'eq=gamma=0.4545'` to ffmpeg. This might cause the video to look darker on PC (MPEG raw streams do not have metadata to indicate the presence of gamma correction), but it might reproduce correctly on N64.

## Encode a video using TMPGEnc

TMPGEnc is a proprietary video encoder specialized in MPEG-1/2, which distributes a free version that's quite feature packed. The software is pretty old but it still runs correctly on Windows 10/11. It is far slower than ffmpeg at encoding, but it offers many more tuning knobs and a more gradual quality control. It is GUI only.

 * Download and install the [TMPGEnc free version](https://www.tmpgenc.net/en/download.html).
 * Download and install [ffdshow](https://sourceforge.net/projects/ffdshow-tryout/). TMPGEnc is only able to decode video codecs for which a VFW plugin is installed, so ffdshow is probably the best choice to cover most common formats (like H.264).
 * Run TMPGEnc, skip the wizard.
 * Select the input file and output file at the bottom of the screen. Make sure that the option `ES (Video Only)` is selected.

<img width="581" alt="Screenshot 2023-09-23 alle 23 32 21" src="https://github.com/DragonMinded/libdragon/assets/1014109/f35f12ba-e4f0-4144-86b1-a3041c7dad81">

 * Click on Settings. Select `MPEG-1 Video`, the insert the resolution and select the desired framerate. At the bottom, in `Motion Search Precision`, you can bump the level to for instance `Highest Quality (very slow)`, though this will affect encoding time, In `Rate Control Mode`, select `Constant Quality (CQ)`, then click on `Setting` and move the slider to adjust the quality level. Try levels in the range 80-95, they usually work well enough. In that same screen, edit `Maximum Bitrate` and insert a value like `1000` that puts an upper cap to the per-frame bitrate.

<img width="416" alt="Screenshot 2023-09-23 alle 23 33 34" src="https://github.com/DragonMinded/libdragon/assets/1014109/1e8bd49f-ff4d-498e-a247-47b83f2904ec">
<img width="340" alt="Screenshot 2023-09-23 alle 23 36 56" src="https://github.com/DragonMinded/libdragon/assets/1014109/adcfe9c3-c37a-49c4-8acc-ca4f84d39a73">

 * Confirm the settings, then press the `Start` button to begin encoding.

**NOTE**: TMPGEnc only supports AVI container as input file. Since the AVI format is very rare as a distribution format nowadays, it might be necessary to repackage the input file into AVI from MP4, MKV, OGG or other more modern format. This does not require a re-encoding, but just a change of container. With ffmpeg, you can do:

```
$ ffmpeg -i filename.mp4 -vcodec copy -acodec copy filename.avi
```

# Suggested resolutions

In general, the resolution affects the decoding speed more than expected because of the infamous N64 memory access speed. It is generally suggested to decrease the resolution to recover room for increasing the bitrate.

MPEG1 videos must have width and height multiple of 16 (as they are encoded as sequences of 16x16 macroblocks), but libdragon's MPEG video player currently has a further constraint: the width must be a multiple of 32.

| Aspect Ratio | Resolution | Comments |
| ------------ | ---------- | -------- |
| 4:3          | 320x240    | This is the "default" for 4:3 videos. Consider that on a CRT screen, about 16 pixels on each edge are overscanned, so part of your video will not visible |
| 4:3          | 288x208    | This is a "faster version" of 320x240, and if the video is displayed on a 320x240 framebuffer, it makes sure the whole image is visible on screen. The improvement in bitrate is evident, you can bump to 1 Mb or more |
| 16:9         | 320x176    | Default resolution for widescreen videos, letterboxed |
| 16:9         | 288x160    | This is a "faster version" of 320x176, to be displayed on a 320x240 framebuffer to achieve a widescreen video, fully visible on CRTs.

# Audio encoding

Currently, the best option is to encode the audio with VADPCM compression which is supported by the wav64 format. First you need to extract the audio as uncompressed WAV file from the original movie:

```
$ ffmpeg -i filename.mkv -vn -acodec pcm_s16le -ar 32000 -ac 1 filename.wav
```

This resamples the audio at 32 Khz, mono (1 channel). It is a good balance for full motion videos between file size and quality, but you can of course change these parameters as you please.

Afterwards, you must convert the wav file to wav64 using the `audioconv64` libdragon tool, which compresses audio files by default:

```
$ $N64_INST/bin/audioconv64 --verbose spring.wav
Converting: spring.wav => ./spring.wav64 (16 bits, 32000 Hz, 1 channels, vadpcm)
```
