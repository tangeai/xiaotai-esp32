#include "p4_playback_resampler.h"

/* 63-tap Blackman-windowed sinc, cutoff 4 kHz at 16 kHz, padded to two
 * equal polyphases. Each phase sums to one: no volume/EQ gain. Coefficients
 * are fixed inputs and the target implementation is verified on the device.
 * Group delay: 31 / 16000 = 1.9375 ms, independent of packet boundaries. */
static const float s_coefficients[P4_PLAYBACK_FIR_TAPS] = {
    0.0f, 0.0f, 0.00008235793522f, 0.0f,
    -0.0003687317315f, 0.0f, 0.0009524530465f, 0.0f,
    -0.0019780798f, 0.0f, 0.003646515747f, 0.0f,
    -0.006220342853f, 0.0f, 0.01003444902f, 0.0f,
    -0.01552229541f, 0.0f, 0.02327967172f, 0.0f,
    -0.03421711438f, 0.0f, 0.04993939897f, 0.0f,
    -0.07380189683f, 0.0f, 0.1145268133f, 0.0f,
    -0.204298041f, 0.0f, 0.6339448423f, 1.0f,
    0.6339448423f, 0.0f, -0.204298041f, 0.0f,
    0.1145268133f, 0.0f, -0.07380189683f, 0.0f,
    0.04993939897f, 0.0f, -0.03421711438f, 0.0f,
    0.02327967172f, 0.0f, -0.01552229541f, 0.0f,
    0.01003444902f, 0.0f, -0.006220342853f, 0.0f,
    0.003646515747f, 0.0f, -0.0019780798f, 0.0f,
    0.0009524530465f, 0.0f, -0.0003687317315f, 0.0f,
    0.00008235793522f, 0.0f, 0.0f, 0.0f,
};

esp_err_t p4_playback_resampler_reset(p4_playback_resampler_t *s, int16_t first)
{
    if (!s) return ESP_ERR_INVALID_ARG;
    s->initialized = false;
    s->pending_tail = false;
    s->clipped = 0;
    /* ESP-DSP's initializer stores, but never modifies, the coefficients.
     * Supplying delay storage avoids its internal malloc path. */
    esp_err_t err = dsps_firmr_init_f32(&s->fir, (float *)s_coefficients,
        s->delay, P4_PLAYBACK_FIR_TAPS, 2, 1, 0);
    if (err != ESP_OK) return err;
    for (size_t i = 0; i < P4_PLAYBACK_FIR_TAPS / 2U; ++i) s->delay[i] = first;
    s->initialized = true;
    return ESP_OK;
}

static size_t convert(p4_playback_resampler_t *s, const int16_t *input,
                      size_t count, int16_t *stereo)
{
    size_t written = 0;
    for (size_t offset = 0; offset < count;) {
        size_t n = count - offset;
        if (n > P4_PLAYBACK_FIR_BATCH) n = P4_PLAYBACK_FIR_BATCH;
        for (size_t i = 0; i < n; ++i) s->input[i] = input ? input[offset + i] : 0;
        int produced = dsps_firmr_f32_ansi(&s->fir, s->input, s->output, (int)n);
        for (int i = 0; i < produced; ++i) {
            float value = s->output[i];
            /* FIR reconstruction can overshoot near full scale. Saturate only
             * the int16 boundary, count it, never wrap or silently lower gain. */
            int32_t sample = (int32_t)(value + (value >= 0 ? 0.5f : -0.5f));
            if (sample > 32767) { sample = 32767; ++s->clipped; }
            if (sample < -32768) { sample = -32768; ++s->clipped; }
            stereo[written++] = (int16_t)sample;
            stereo[written++] = (int16_t)sample;
        }
        offset += n;
    }
    return written;
}

size_t p4_playback_resampler_process(p4_playback_resampler_t *s,
    const int16_t *input, size_t count, int16_t *stereo, size_t capacity)
{
    if (!s || !s->initialized || !input || !stereo || !count || count > capacity / 4U)
        return 0;
    size_t written = convert(s, input, count, stereo);
    s->pending_tail = true;
    return written;
}

size_t p4_playback_resampler_finish(p4_playback_resampler_t *s,
    int16_t *stereo, size_t capacity)
{
    if (!s || !s->initialized || !s->pending_tail || !stereo ||
        capacity < P4_PLAYBACK_FIR_TAIL_INPUT * 4U) return 0;
    size_t written = convert(s, NULL, P4_PLAYBACK_FIR_TAIL_INPUT, stereo);
    s->pending_tail = false;
    return written;
}
