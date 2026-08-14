Libdragon features a realtime Opus (CELT) decompressor that can be used for compressed audio. 

This is the same state-of-the-art audio algorithm that is currently mainstream on PCs. It is also probably the last one that would be able to run on N64 because all the new research is moved into deep-learning based model whose inference will be impossible to run at interactive rate.

Compression ratios are around 15:1 for audio files (see below for more data), which makes them 4 or 5 times smaller than their VADPCM counterparts. This means around 3-5 minutes of mono audio per megabyte of ROM, depending on the wanted quality. 

The playback is RSP-accelerated with a custom microcode that obtains decent performance, though it can still impact on framerate (depending on your game resource usage). Normally, a mono stream takes about 18-20% of CPU usage and about 15% of RSP usage. This makes it a good fit for the following uses:

 * Background music for menus or similar scenes with low performance 
 * Background music for lighter, less resource intensive games, such as 2D ones
 * Speech. With Opus compression ratios, a fully talkie game is indeed possible.

## How to compress a WAV file

Opus compression is transparently integrated in the [`audioconv64`](https://github.com/DragonMinded/libdragon/wiki/Audioconv64) tool and the `wav64` file format. Audioconv64 is libdragon's tool to convert input WAV files into libdragon's optimized `wav64` file format. 

See [`audioconv64`](https://github.com/DragonMinded/libdragon/wiki/Audioconv64) for more information in general to the compression process.

## How to playback

At runtime, all wav64 can be played back exactly in the same way, irrespective of the compression level. See the `wav64.h` header file for an overview of the API, and the `mixertest` example as a simple audio playback example.

The only difference is that you must explicitly initialize playback of Opus files by calling `wav64_init_compression(3)`. This basically tells libdragon to link support for decompression of level 3 WAV64 files.

## How to tune the quality and compression ratio

To avoid exposing many knobs, audioconv64 will automatically select a compression ratio (aka bitrate) that linearly follows the input sample rate. The idea is that the user can play with compression ratio by reducing the input sample rate (and thus the input frequency bands). For instance, if you have a music file at 44 Khz and you want to experiment with compressing it more with audioconv64 itself:

```
$ $N64_INST/bin/audioconv64 --wav-compress 3 --wav-resample 22050 music.wav
```

In general, to obtain the highest possibile quality, provide the highest quality version of your audio file to audioconv64, and let it handle resampling by itself (because in the case of opus, that resampling is actually also part of the compression process, which produces a better result if it is fed the highest-quality file).

If you add the `--debug` option, `audioconv64` will also generate a `.opus.wav` file, which is the *decompressed* version of the compressed file it just created. This allows you to quickly evaluate the quality by simply playing the wav file.

## Benchmarks

This table shows some benchmarks:

| Song name | Duration | Channels | Rate  | Raw size | Opus size | Opus CPU time |
| --------  | -------  | -------- | ----  | -------- | --------- | ------------- |
| Octane    |   19s    |  2       | 48000 | 3.5 MiB  |  232 KiB  |    6969 us (35%) |
| Octane    |   19s    |  2       | 32000 | 2.3 MiB  |  157 KiB  |    6543 us (33%) |
| Octane    |   19s    |  2       | 24000 | 1.8 MiB  |  119 KiB  |    6210 us (31%) |
| Octane    |   19s    |  1       | 48000 | 1.7 MiB  |  119 KiB  |    4093 us (20%) |
| Octane    |   19s    |  1       | 32000 | 1.2 MiB  |   82 KiB  |    3826 us (19%) |
| Octane    |   19s    |  1       | 24000 | 0.9 MiB  |   63 KiB  |    3654 us (18%) |


The quoted CPU time is the amount of CPU time required to decompress one 20 ms audioframe (on average on the whole song). As you can see, the current impact of Opus is still quite high even with RSP acceleration, so it should be judiciously used, especially in stereo mode.

## Technical details

The opus format is built upon two different codecs: CELT and SILK. Simplifying, CELT is used for music and audio in general (so it is the default codec for wideband audio), while SILK is more specialized for speech (and thus narrowband audio). Opus is also able to actually mix CELT and SILK in this same audioframe. Libdragon's implementation of Opus only uses CELT. SILK is more resource intensive and only makes sense in specific use cases (speech at very low bitrates). A 16 Khz speech audio file compressed with CELT will still produce very good audio quality, while still providing a 16:1 compression ratio.

By default, Opus internally works by default at a sample rate of 48 Khz. Support for "custom modes" is available in the libopus codebase (though disabled by default), that allows for different sample rates. We activated such support for libdragon and we are experimenting with 32 Khz to reduce the resources, but currently this is not supported. If you inspect an opus-compressed wav64 file at runtime (via `wav->wave.frequency`) you will see that it will always look like a 48 Khz file (or an integer decimated version of it: 24 Khz, 12 Khz, etc.). Don't worry about that though: it is an internal detail of how Opus works. The input sample rate was still took into account to decide the compression factor, so if you compress a 22 Khz file it will be about half as small and sound worse than a 44 Khz file, as expected.

Opus splits the input file in "frames" made of a fixed number of samples, and compress them separately. We use Opus in VBR (variable bitrate mode), which is the one that gives the best quality at any given file size; this in turns means that each frame will use a variable number of bytes in its compressed format. Currently, audioconv64 will always generate frames of exactly 20 ms of audio (960 samples, at the internal sample rate of 48 Khz), which is the default and suggested frame size for standard audio playback.

The opus decompression library has been accelerated in several parts via custom RSP ucode, mainly in three areas: IMDCT/FFT, Comb Filter and Emphasis Filter. RSP acceleration is fundamental to make Opus manageable on Nintendo 64: a CPU-only implementation in fact takes about 16/18 ms for each 20 ms audioframe, that is it uses 80-90% of CPU time. All the RSP implementation has been made with 32-bit fixed point numbers to avoid rising too much the noise floor with precision issues; the final RMSD is about 5 (on a 16 bit sample) which is basically nothing. This means that the playback on N64 will not introduce any noise or artifact: it should sound exactly as if you decompress and play the same file on PC.