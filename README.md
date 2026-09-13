# libfaac
This filter encodes raw PCM audio to MPEG-4 AAC-LC using [libfaac](https://github.com/knik0/faac)
(faac 2.1, LGPL 2.1). It is the native AAC route of the player: raw AAC frames with an
out-of-band AudioSpecificConfig, ready for mp4 muxing (`isobmff_1`) and for progressive
playback through Media Source Extensions.

Filter name: `encfaac`. Option: `bitrate` (kbps, all channels together, default 128).

## What was measured

Native harness, 10 s of `wav/geminiani48.wav` (48 kHz stereo S16), AAC-LC at 128 kbit/s,
decoded back with ffmpeg and compared to the source after alignment:

| encoder | alignment | SNR |
|---|---|---|
| libfaac (this filter's library) | 1024 samples/channel | 13.5 dB |
| ffmpeg `aac`, same bitrate | 1024 samples/channel | 12.2 dB |

The 1024-sample offset is exactly the `encoder_delay` the library reports, and the decoded
length exceeds the source by delay plus padding: no frame is lost. Waveform SNR is the
expected order of magnitude for a perceptual codec at this rate; the comparison with a
second implementation is the reference, not the absolute figure.

In the player itself (`<audio is="universal-audio_1" using="solver_1"
with="rfpcm_1;libfaac_1;isobmff_1" out="mp4">`, whole 96 s file, Chrome headless):

| | value |
|---|---|
| output | 1 565 229 bytes, `mp4a` + `esds`, AAC-LC 48 kHz stereo, 130 kbit/s (ffprobe) |
| wall clock | 10.4 s for 96 s of audio, download included |
| offset after ffmpeg decode | 0 samples: the encoder delay goes into `GF_PROP_PID_DELAY`, mp4mx turns it into an edit list |
| SNR vs source | 13.7 dB (ffmpeg `aac` 128 kbit/s through the same decode: 14.6 dB) |

The audio tag only routes through this filter when it asks for `c=aac`: mp4mx accepts raw
PCM as-is (`ipcm`), so without the constraint the encoder is simply bypassed. `UAudio.ts`
sends the constraint whenever `libfaac_1` is listed in `with` and the output is not wav.

The library has no thread, no mandatory SIMD (the optional SSE2 quantizer is left out of
the wasm build) and no `setjmp`.

## Requirements

[CMake](https://cmake.org/) is used as a build system. To install it, follow
[Debian build instructions](developing_in_debian.md).

[Emscripten SDK](https://emscripten.org/) is required for building
WebAssembly artifacts. To install it, follow the
[Download and Install](https://emscripten.org/docs/getting_started/downloads.html)
guide:

```bash
cd $OPT

# Get the emsdk repo.
git clone https://github.com/emscripten-core/emsdk.git

# Enter that directory.
cd emsdk

# Download and install the latest SDK tools.
./emsdk install latest

# Make the "latest" SDK "active" for the current user. (writes ~/.emscripten file)
./emsdk activate latest
```

## Building the accessor

```bash
# Setup EMSDK and other environment variables. In practice EMSDK is set to be
# $OPT/emsdk.
source $OPT/emsdk/emsdk_env.sh

# Assuming you are in the root level of the cloned repo :
emcmake cmake .
emmake make
```

Once built, you can use and distribute libfaac_1.wasm with your universal tags.

## Documentation

For more details, please visit our documentation at https://bevara.com/documentation/develop/.
