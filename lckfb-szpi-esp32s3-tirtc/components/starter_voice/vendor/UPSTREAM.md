# Voicute wake-only source subset

Source: https://github.com/voicute/onnx-wakeword
Pinned commit: `61e76c5a12ac33da0def7bceba2e07228ad03e63`.

Copied only `esp32/sdk/voicute/mel_extractor.c`, `mel_extractor.h`,
`mel_filterbank.h`, `kws_postprocess.h`. The extractor now propagates FFT init
failure instead of continuing with an invalid table. No example BSP, I2S, AFE,
MultiNet, SPIFFS loader, L1-L5 gates, or recognizer lifecycle was imported.

`../model/` is the user's `docs/nihaoxiaotai_v9.3_voice_tflite` model/head,
not the example Hey Robot assets. Both were copied byte-for-byte.

The inspected upstream and model package did not include a license grant.
This records provenance, not a new license. Confirm redistribution/commercial
authorization with the provider before distributing a product.
