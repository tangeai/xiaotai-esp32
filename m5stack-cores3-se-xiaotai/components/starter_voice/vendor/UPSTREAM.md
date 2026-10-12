# Voicute wake-only source subset

Source: https://github.com/voicute/onnx-wakeword
Pinned commit: `61e76c5a12ac33da0def7bceba2e07228ad03e63`.

Copied only `esp32/sdk/voicute/mel_extractor.c`, `mel_extractor.h`,
`mel_filterbank.h`, `kws_postprocess.h`. The extractor now propagates FFT init
failure instead of continuing with an invalid table. The static FFT buffer is
declared 16-byte aligned and checked at init: the ESP32-S3 optimized FFT uses
64-bit loads/stores that ignore the low address bits, and the unaligned upstream
declaration produced wrong spectra on CoreS3-SE (measured 2026-10-10). No example
BSP, I2S, AFE,
MultiNet, SPIFFS loader, L1-L5 gates, or recognizer lifecycle was imported.

`../model/` contains the paired model/head from the user-supplied
`nihaoxiaotai_v9.3_20260915_nhwc_tflite.zip`, not the example Hey Robot assets.
Both were copied byte-for-byte. See [model provenance](../model/README.md)
for their hashes and the input contract. The FFT, window, filterbank, log-power,
top-dB normalization and model input math remain unchanged.

The CoreS3-SE port caches raw log-power for overlapping 10 ms frames in PSRAM.
Each 16,192-sample window ends on the same FFT grid, retaining at most 159 newer
samples for the next snapshot. Every window still recomputes global peak/top-dB
normalization over all 98 frames. Epoch changes, discontinuous or unaligned
windows and FFT failures invalidate reuse. `tools/test_wake_mel_stream.py`
compares the streaming and full extractors using the official C FFT and checks
bit-identical float output; it is not a device timing or wake-accuracy proof.

At the original import review, the inspected upstream and model package did not
include a license grant. That is a historical provenance observation, not the
current project's authorization status.

The project owner has confirmed that the currently bundled source subset and
model may be redistributed under MIT. See [the project authorization scope](../../../THIRD_PARTY.md).
This owner-provided authorization applies to the bundled assets; it does not
change the upstream repository's license or cover future assets. Existing
copyright notices and provenance are retained.
