# Local Component Changes

This is a source-vendored `esp_h264` 1.4.1 component, not an unmodified registry
binary. Preserve the existing DMA, output-bound, force-IDR, SPS timing/cropping
and PPS fixes when refreshing it.

The WT99P461-S1 color-contract change adds optional `vui_color` metadata to
`esp_h264_enc_cfg_t`. The single/dual HW adapters copy it into the parameter
owner; both initial and regenerated SPS use the same color description.
Zero-initialized metadata preserves the prior unspecified-color signaling.
This describes the pixels; it does not perform a color conversion.

Use these headers and sources together and rebuild dependent objects. Do not
mix the extended configuration structure with upstream precompiled encoders.
The application supplies full-range BT.601 matching its explicit CSI/ISP
format, without claiming unknown sensor primaries or transfer characteristics.

`tools/test_video_presentation.py` in the application compiles the production
NAL generator and parses 60 size/rate/color SPS profiles under ASan/UBSan.

`CONFIG_ESP_H264_HW_TIMING_DIAGNOSTICS` defaults off. A dedicated diagnostic
build can report slow single-stream HW calls at most once every two seconds,
separating input cache, parameter lock, setup, completion wait, output cache
and rate control. It does not change encoding, timeout or error semantics.
