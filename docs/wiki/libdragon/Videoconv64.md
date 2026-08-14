Videoconv64 is Libdragon's tool to create video/audio streams that can be used for full motion videos.

## Feature list

 * Based on [ffmpeg](https://github.com/DragonMinded/libdragon/wiki/Video-playback), so it supports basically all known video/audio formats as input for the conversion. `ffmpeg` must be installed on your PC before you can use `videoconv64`.
 * Conversion of video tracks into Libdragon supported video codecs (MPEG-1 and H.264), respecting the internal implementation limits.
 * Automatic support for anamorphic and letterboxed videos, fully respecting the video aspect ratio
 * Conversion of audio tracks into Libdragon supported audio codecs (VADPCM and Opus), using [Audioconv64](https://github.com/DragonMinded/libdragon/wiki/Audioconv64)
 * Conversion of subtitle tracks into Libdragon's optimized `.sub64` format, that supports basic styling and positioning
 * Simple, configurable resolution (`--width`)
 * Simple, configurable quality knob (`--quality 0..100`)
 * Support for making video/audio seekable (`--seek`), generating an external seek table (`.seek`) to allow for efficient runtime seeking

## Syntax reference

This is videoconv64 command line help:

```
videoconv64 -- Video conversion tool for libdragon

Usage:
   videoconv64 [flags] <input_file> [audio_or_subtitle_file ...]

Options:
   -o, --output <dir>          Specify output directory (default: same as input)
   -v, --verbose               Verbose mode (repeatable)
   -h, --help                  Show this help message

Video options:
   -c, --codec <mpeg1|h264>    Video codec to use (default: mpeg1)
   -w, --width <N>             Target width (default: 320)
                               Height is auto-calculated maintaining aspect ratio
                               and enforcing codec-specific alignment.
   -r, --fps <N>               Force a specific framerate (default: auto)
   -q, --quality <0..100>      Synthetic quality scale (default: 80)
   -Q, --quick                 Quick encoding (speed up processing as much as possible)
       --seek <SEC|FILE>       Enable seeking support:
                               - if SEC is a float, request a keyframe every SEC seconds
                               - if FILE, read a list of frame indices that must be keyframes

Audio options:
       --audio-compress <N>    Pass through to audioconv64: --wav-compress <N>
       --no-audio              Disable audio extraction

Advanced options:
       --no-progress            Disable progress output
       --deinterlace <auto|on|off>
                                Control deinterlacing (default: auto)
       --profile <auto|cartoon|film|noisy|none>
                                Content profile for optimized filtering (default: auto)
       --quant-matrix <n64|std> Quantization matrix to use (default: n64)
       --audio-parms <R,C>      Audio params: RATE,CHANNELS (default: 32000,1)
       --ffmpeg-path <path>     Path to ffmpeg executable (default: ffmpeg)
       --ffprobe-path <path>    Path to ffprobe executable (default: ffprobe)
```

You are encouraged to experiment with it by running it manually on the command line. In production, though, you can integrate it with your Makefile like any other asset conversion.

### Input files

videoconv64 can receive one or multiple input files, that are meant to refer to the same video content. 

If you specify a file in a video container (eg: mkv, mp4, ogv, etc.), videoconv64 automatically tries to convert all the supported tracks in it, and specifically:

 * One video track (the first one) will be converted into h264 or mpeg1 (as specified by the `--codec` option).
 * All audio tracks will be extracted and converted into separate `.wav64` files. This is useful if the video has for instance dubbing in multiple languages.
 * All subtitle tracks will be extracted and converted into separate `.sub64` files. Again, this is useful in case subtitles are available I multiple languages.
 * All attached static images will be extracted and converted into separate `.sprite` files, after scaling them down to low resolution. This is useful to extract cover images for the video.

Example of usage:

```
$N64_INST/bin/videoconv64 -o filesystem --codec h264 --quality 80 introseq.mkv
```

Only one container can be specified as input file, as each videoconv64 invocation must process only a single video (in all its different tracks).

In addition to the container file, you can specify video, audio, subtitle or image files that are meant to augment or override the container contents. For instance:

```
$N64_INST/bin/videoconv64 -o filesystem --codec h264 --quality 80 introseq.mkv sub_eng.srt sub_spa.srt
```

In this example, the user specified additional subtitle files which will be converted and bound to the video. All subtitle tracks in `introseq.mkv` would be also processed, but this approach is normally useful when they are missing.

It is possible also to convert a video made only of separate tracks:

```
$N64_INST/bin/videoconv64 -o filesystem --codec h264 --quality 80 finale.m2v finale.aac sub_eng.vtt sub_ita.vtt cover.webm
```

The output will be something like this:

```
finale.h264
finale.wav64
finale.eng.sub64
finale.ita.sub64
finale.sprite
```

Notice that all files are renamed to the same basename (the video track name), so that at runtime they can all be treated as a "bundle", without the user having to tie them together in code.

### `--quality`

This is the main option to affect quality and bitrate of the output. By decreasing this value, videoconv64 will create a lower quality video (usually, more "blocky") with a smaller file (lower bitrate). The file will also be lighter at runtime to process so it'll be more likely to not have slowdowns.

In general, you will need to tweak this parameter to adjust the video quality and file size until it reaches the compromise that you find more acceptable.

### `--fps`

This option specifies what is the framerate of the output video. By default, the same framerate of the input video will be used. Framerate has a *huge* effect on performance on all codecs, so make sure to reduce this if possible.

### `--width`

This option specifies the horizontal resolution at which the video will be converted (and then played back on hardware). By default, the output width is 320, but it can be adjusted to adjust quality and performance. Notice that the exact width can be rounded by videoconv64 if the codec requires some specific alignment constraint.

The height is calculated automatically to match the video aspect ratio (the expectation is that the input video aspect ratio must be preserved). For instance, a 16:9 converted with `--width 320` will end up as 320x176 (or similar, depending on the codec). Notice that the exact aspect ratio is recorded anyway and will be used during playback. So for instance, the 320x176 video will be slightly stretched vertically during playback (using custom VI resolutions) so that it will display *exactly* as 16:9, with no aspect ratio distortion.

Moreover, taking into account N64 display and resolution limits, anamorphic videos can be automatically created. For instance, if you convert a 4:3 video and specify `--width 352`, videoconv64 will create a 352x240 video but at runtime it will be played correctly back as 4:3, so anamorphically. That's encoding more than 240 lines vertically is a waste as you can't normally display more than 240 pixels vertically on analogic TVs (with default VI timings / output calibrations), but the extra horizontal pixels can still be used to increase quality instead.

In general, decreasing resolution helps ("quadratically") with performance. For instance, decreased width from 320 to 288 is a good way to increase performance quite a bit. See below for some tips on tweaking quality and performance.


### Quality, performance and file size

The most complex issue in using videconv64 is tweaking options to reach the best compromise between quality, performance and file size. In particular:

 * "Quality" is a subjective metric of how little compression artifacts can be seen in the video during playback.
   * Evaluate quality on your reference playback system. The look and feel of the same video on a CRT are much different than an emulator (in general, CRTs are much more forgiving and will hide compression artifacts). 
   * To affect quality, the main knob is, unsurprisingly, `--quality`.
   * `--width` will not directly affect compression artifacts but can help improving or reducing the overall perceived quality of the video.
 * "Performance" refers to the time it takes for N64 to decompress a frame (on average). 
   * For the full motion video case, most people will only care whether performance is enough to have no slowdowns or frameskips during playback.
   * Because of the overall runtime architecture of the video player, performance issues will cause desyncs between audio and video, because normally audio will play back at nominal rate, while video will struggle to catch up, lagging behind.
   * To workaround performance issues, at runtime you can activate frameskipping. If this happens only in a few parts of the video, it might be good enough.
   * The main toggles to impact performance are `--width` and `--fps`. Both of them have a huge effect on performance, because they directly affect the number of macroblocks per second that the video codec has to decompress. For example, a 320x240 video at 25 fps has 54% (!) more macro block per seconds than the same video encoded at 288x216 at 20 fps. That's a lot heavier to handle for N64.
   * `--quality` can also impact performance a bit, but not as much. The previous options normally are a better choice when performance is an issue.
 * "Video size" refers to the size of the video in ROM.