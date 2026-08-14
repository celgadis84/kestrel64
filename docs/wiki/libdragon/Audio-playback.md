# Audio playback

To perform audio playback on libdragon, you will need to familiarize at some level with the following libraries:

 * [audio.h](https://github.com/DragonMinded/libdragon/blob/trunk/include/audio.h): low-level audio driver, whose responsibility is pushing buffers of audio samples to the DAC.
 * [mixer.h](https://github.com/DragonMinded/libdragon/blob/trunk/include/mixer.h): RSP accelerated mixer. This library exposes up of 32 channels of waveforms that can be dynamically resamples and mixed together. While it works with RAM buffers, its output is normally sent to the audio library for playback.

We also offer three libraries that implement different file formats with different capabilities:

 * [wav64.h](https://github.com/DragonMinded/libdragon/blob/trunk/include/wav64.h): Library to reproduce WAV-like files. WAV files are streamed from ROM as needed so that they don't use much RAM, and several audio compression algorithms are supported. To create wav64 files, use the `audioconv64` tool (see below).
 * [xm64.h](https://github.com/DragonMinded/libdragon/blob/trunk/include/xm64.h): Library to playback XM module files, a format popularized by FastTracker II. Modules are sequenced music ad can be composed using modern tools like OpenMPT or MilkyTracker. They offer extremely accurate fidelity because they playback on PC and N64 almost identically. To create xm64 files, use the `audioconv64` tool (see below).
 * [ym64.h](https://github.com/DragonMinded/libdragon/blob/trunk/include/ym64.h): Library to playback YM module files, a format for chiptune music. This format is quite niche but it can be used to give a 8-bit, retro touch to a soundtrack. YM files can be tracked used the Arkos Tracker.

There are two examples in Libdragon that focus on audio:

 * [mixertest](https://github.com/DragonMinded/libdragon/blob/trunk/examples/mixertest/mixertest.c). This is a very basic, barebone example that shows to how playback sound effects or streamed music in wav64 format.
 * [audioplayer](https://github.com/DragonMinded/libdragon/blob/trunk/examples/audioplayer/audioplayer.c). This is a full audio player of XM and YM modules, also providing statistics, allows for skipping, etc.
