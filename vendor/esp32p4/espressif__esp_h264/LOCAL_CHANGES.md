# Local Component Changes

This is a source-vendored `esp_h264` 1.4.1 component, not an unmodified registry
binary. Both ESP32-P4 products (Waveshare P4 and WT99P461-S1) build this one
copy. Preserve the existing DMA, output-bound, force-IDR, SPS timing/cropping
and PPS fixes when refreshing it.

## Optional VUI color description

`esp_h264_enc_cfg_t` gains an optional `vui_color` field. The single/dual HW
adapters copy it into the parameter owner, so the initial and the regenerated
SPS carry the same color description. It describes the pixels; it does not
convert them.

- Zero-initialized metadata keeps the upstream unspecified-color SPS bit for
  bit. The Waveshare P4 camera pipeline relies on this.
- WT99P461-S1 declares full-range BT.601, matching its explicit CSI/ISP output
  format, without claiming sensor primaries or transfer characteristics.

Use these headers and sources together and rebuild dependent objects. Do not
mix the extended configuration structure with upstream precompiled encoders.

## Timing diagnostics

`CONFIG_ESP_H264_HW_TIMING_DIAGNOSTICS` defaults off and is then compiled out.
A diagnostic build reports slow single-stream HW calls at most once every two
seconds, separating input cache, parameter lock, setup, completion wait, output
cache and rate control. It does not change encoding, timeout or error
semantics.
