# Voicute wake-only source subset

Source: https://github.com/voicute/onnx-wakeword
Pinned commit: `61e76c5a12ac33da0def7bceba2e07228ad03e63`.

Copied only `esp32/sdk/voicute/mel_extractor.c`, `mel_extractor.h`,
`mel_filterbank.h`, `kws_postprocess.h`. The extractor now propagates FFT init
failure instead of continuing with an invalid table. No example BSP, I2S, AFE,
MultiNet, SPIFFS loader, L1-L5 gates, or recognizer lifecycle was imported.

`../model/` contains the paired model/head from the user-supplied
`nihaoxiaotai_v9.3_20260915_nhwc_tflite.zip`, not the example Hey Robot assets.
Both were copied byte-for-byte. See [model provenance](../model/README.md)
for their hashes and the input contract. The frontend above is unchanged.

At the original import review, the inspected upstream and model package did not
include a license grant. That is a historical provenance observation, not the
current project's authorization status.

The project owner has confirmed that the currently bundled source subset and
model may be redistributed under MIT. See [the project authorization scope](../../../THIRD_PARTY.md).
This owner-provided authorization applies to the bundled assets; it does not
change the upstream repository's license or cover future assets. Existing
copyright notices and provenance are retained.
