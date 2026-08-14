Audioconv64 is a libdragon tool to convert audio files into formats optimized for playback on N64. It currently handles three different kind of files:

 * WAV/MP3 files (streamed music format, or sound effects). These can be converted into `.wav64` files, which are then handled by the [wav64.h](https://github.com/DragonMinded/libdragon/blob/trunk/include/wav64.h) library.
 * XM modules (sequenced music format). These can be converted into `.xm64` files, which are handled by the [xm64.h](https://github.com/DragonMinded/libdragon/blob/trunk/include/xm64.h) library.
 * YM audio files (chiptune "8-bit like" music, originally designed for the AY-3-8910 PSG chip). These can be converted into the .ym64 format, that can be handled by the [ym64.h](https://github.com/DragonMinded/libdragon/blob/trunk/include/ym64.h) library.
 
The command line help of audioconv64 highlights the main options, divided by the various supported formats:

```
audioconv64 -- Audio conversion tool for libdragon

Usage:
   audioconv64 [flags] <file-or-dir> [[flags] <file-or-dir>..]

Supported conversions:
   * WAV/MP3 => WAV64 (Waveforms)
   * XM  => XM64  (MilkyTracker, OpenMPT)
   * YM  => YM64  (Arkos Tracker II)

Global options:
   -o / --output <dir>       	Specify output directory
   -v / --verbose            	Verbose mode
   -d / --debug              	Dump uncompressed files in output directory for debugging
   -h / --help               	Show this help message
        --help-compress      	Show detailed help for compression options

WAV/MP3 options:
   --wav-mono                	Force mono output
   --wav-resample <N>        	Resample to a different sample rate
   --wav-compress <0|1|3>    	Enable compression: 0=none, 1=vadpcm (default), 3=opus
   --wav-loop <true|false>   	Activate playback loop by default
   --wav-loop-offset <N>     	Set looping offset (in samples; default: 0)
   --wav-seek-offset <N>[,<N>]	Add additional seeking offsets (in samples; can specify multiple times)

XM options:
   --xm-8bit                 	Convert all samples to 8-bit
   --xm-ext-samples <dir>    	Export samples externally as wav64 files in the specified directory
   --xm-compress <0|1>          Compression level for XM samples (default: 1=vadpcm)
   --xm-compress-data <0..3>    Compression level for XM binary data (default: 1)

YM options:
   --ym-compress <true|false>  	Compress output file
```

## Compression and quality

Most of this page elaborates on the various options available to tune audio quality/compression and thus output file size. This focus is needed because **audio files are easily the largest ones** in a standard N64 ROM. As a data point, the N64brew 2024 Game Jam ROM is a 15.7 MiB ROM, of which 11.5 MiB (73%) are audio files.

So while planning for final ROM size, make sure to study audioconv64 and its options in detail, as tuning those is likely the most important thing you will have to do to fit the size budget.

## wav64

wav64 is the main waveform format. wav64 can be optionally compressed (with either VADPCM or Opus), and can be either streamed from ROM (or other filesystems, like SD) or preloaded into RDRAM.

audioconv64 accepts either WAV files (in many different variants or internal compressions, with the hope that "any WAV will work") or MP3 files as input format. MP3 files are immediately decompressed into raw samples, and then treated exactly like WAV files. Using MP3 as input is suggested to decrease file sizes in the git repository during development, so that smaller files can be committed as assets. Notice that libdragon does not currently support native MP3 playback at the moment, so MP3 are just treated as "zipped WAV input files". 

> [!TIP]
> We advise to **store input assets as very high-quality WAV or MP3 format** (eg. 256 Kbps or more), and in general at the higher possibile quality. Then, use audioconv64 resampling, downmixing and compression options to reduce the quality and the size of the N64 ROM. This gives you much more flexibility in the future to tune these quality parameters differently by simply changing audiconv64 options. If you commit low-quality input assets files, then it will be harder to increase the quality back (you will have to go back to the DAW tool you used to produce them to change parameters there).

Converting a WAV or MP3 can be done very easily by running `audioconv64` manually:

```
$N64_INST/bin/audioconv64 music.wav
```

By default no resampling is performed and the resulting wav64 file is compressed using VADPCM, which is very light at runtime but still provides decent compression ratio.

### wav64: Resampling and quality options

It is possible to process the audio files during conversion to reduce the quality and improve compression. Two main options are provided:

 * `--wav-mono` forces downmixing to mono in case the input file is stereo. This normally halves the size of the output file. 
 * `--wav-resample <N>` resamples the input file to a target sample rate. For instance, specifying `--wav-resample 22050` resamples the input file to 22050, reducing the quality but decreasing also the size. Resampling is performed using state-of-the-art algorithms tuned at the highest possible quality, so they should match any DAW quality-wise.

> [!TIP]
> When experimenting with compression and resampling options, you can use the `-d` option to ask audioconv64 to generate an output,
> decompressed .wav file (in addition to the .wav64) so that you can immediately listen to the final quality directly on your PC.

### wav64: VADPCM audio compression

The wav64 format supports VADPCM compression. VADPCM is a special variant of ADPCM that has been designed to be fast to decompress on RSP. VADPCM is very light on the RSP, and provides a very good audio quality, with a compression factor of ~3.8:1 for 16-bit files.

The codec was originally designed by Nintendo but not documented in its implementation; thanks to the work of Vanadium in [Skelly64](https://github.com/depp/skelly64/tree/main/lib/vadpcm), there is now a clean room, open source implementation available of both the compressor and decompressor in C, that we integrated in libdragon and rewrote in RSP for optimization. Further improvements were made, in particular there is now an additional entropy coding layer for coefficients that saves another 10% in size

Compression is performed by audioconv64 during the conversion to .wav64. It is active by default as it gives a very good balance between resource usage and quality. It is anyway controlled by the new `--wav-compress` option so that it can be disabled if needed.
The runtime code is mostly unaffected: you can call `wav64_open` and `wav64_play` just like uncompressed files. You must call the new `wav64_close` to dispose the file once it is not needed anymore.

An example of manually compressing a file with VADPCM:

```
$ $N64_INST/bin/audioconv64 -v --wav-compress 1 music.wav
Converting: music.wav => ./music.wav64 (vadpcm)
  input: 16 bits, 44100 Hz, 2 channels
  compressing into VADPCM format (52284 frames)
  huffman compressed 941112 bytes into 852021 bytes (ratio: 90.5%)
  uncompressed: 3346132 bytes, compressed: 852417 bytes (ratio: 25.5%)
```

To tune and improve VADPCM there are a few compression options that can be specified:

  * `--wav-compress 1,bits=[2,3,4]` is used to specify VADPCM variants with either 2, 3 or 4 bits per coefficient. Default is 4. Reducing to 3 or 2 will improve compression ratio but the quality will decrease (decompression time on N64 is not affected).
  * `--wav-compress 1,huffman=0` disables the additional Huffman compression for VADPCM coefficients. This will increase the compressed file size, have no impact on quality, and slightly reduce the CPU time using during decompression.

For instance, this is the same file compressed with 2-bit VADPCM:

```
$ $N64_INST/bin/audioconv64 -v --wav-compress 1,bits=2 music.wav
Converting: music.wav => ./music.wav64 (vadpcm)
  input: 16 bits, 44100 Hz, 2 channels
  compressing into VADPCM format (52284 frames)
  huffman compressed 941112 bytes into 458083 bytes (ratio: 48.7%)
  uncompressed: 3346132 bytes, compressed: 458479 bytes (ratio: 13.7%)
```

### wav64: Opus audio compression

This is a huge topic by itself so we moved it to [Opus decompression](https://github.com/DragonMinded/libdragon/wiki/Opus-decompression)

### wav64: Allow for seeking

Uncompressed wav64 files can be freely seeked at any point. Compressed wav64 files can only be seeded at predefined seek points, decided when running `audioconv64`. To specify a seek point, you have a few options:

 * You can insert specific seek points in the input WAV file using a DAW tool. Seek points must be saved as "WAV cue points" in the file, so make sure to check the documentation of the tool to see if this function is supported. 
 * You can specify seek points in a text file, and pass it to `audioconv64` using `--wav-seek <filename.txt>`. The file must contain one seek point per line, and they can either be specified as timestamp (`HH:MM:SS.mmm`) or absolute sample number (`nnnnn`). The file can contain also comments prefixed by `#`.
 * You can ask `audioconv64` to generate automatically seekpoints every few seconds, using `--wav-seek <number>`, where the argument is the number of seconds. For instance `--wav-seek 3.5` will generate one seekpoint every 3.5 seconds.

The first two approaches are preferred when seekpoints refer to specific points in the audio track that will be explicitly seeked by the code (eg: scene changes). The third is the best when you just need the possibility of "seek forward" or "seek backward", like an audio player.

To seek at runtime, use [`wav64_seek()`](https://github.com/DragonMinded/libdragon/blob/96b9ca36071499883c5f05836bbb1b492b652583/include/wav64.h#L158-L171). The function will find the closest seekpoint to the specified timestamp, and seek. The exact timestamp at which the seek was performed is returned.

> [!NOTE] 
> Seeking is only available in the preview branch

## xm64 

### Samples and metadata compression

By default, xm64 files are compressed to save space. The big amount of data in a xm64 file is the samples, and they are compressed using VADPCM, the same algorithm used by wav64 (in fact, a xm64 acts as a container of multiple wav64s). This can be controller by the `--xm-compress` option, which is very similar to `--wav-compress`. 

The default option for `--xm-compress` is VADPCM with Huffman disabled (so it acts as if `--xm-compress=1,huffman=false` is specified). This is different from wav64 where huffman is enabled by default. Again, this is a tradeoff for ROM space vs performance, as decoding huffman on multiple channels does have an impact in CPU time. Notice that Opus compression is not supported for xm64 files, because running multiple Opus codecs in parallel for a multi-channel xm64 file would be impossible in real-time on a Nintendo 64, and would also require quite a bit of RAM (as the Opus decoding state is not so small).

The rest of the data of the XM file (mostly, headers and patterns) is just binary data, whose compression can be tuned with `--xm-compress-data`. This is just standard [asset compression](https://github.com/DragonMinded/libdragon/wiki/Compression) so it follows the same algorithms described there. As usual, level 1 compression is used by default.

### How to share samples across different xm64 files

If you want to ship multiple XM64 files that contain common samples, you can use the `--xm-ext-samples` option. This option tells audioconv64 not to put samples within each xm64 files, but instead to create a "sample database" into the specified directory on your filesystem. Every sample will be stored there as a separate `.wav64` file with a special name derived from its contents, so that common samples in multiple XM files will be automatically deduplicated.

Make sure to specify a directory that is then shipped into the ROM (so e.g. use a directory such as `filesystem/samples/`). At runtime, you need to instruct the xm64 library where to go looking for samples:

```C
     // Configure the directory used to search for external samples
     xm64_set_extsampledir("rom:/samples");
```