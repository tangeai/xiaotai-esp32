#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Amplitude evidence only: a large adjacent step can be valid high-frequency
 * content. Near-full-scale samples do not on their own prove clipping. */
typedef struct {
    uint32_t samples, near_full, peak, step_max;
    int16_t last;
    bool have_last;
} p4_playback_meter_t;

static inline void p4_playback_meter_add(p4_playback_meter_t *m, const int16_t *pcm,
                                          size_t samples, size_t stride)
{
    for (size_t i = 0; i < samples; ++i) {
        int32_t sample = pcm[i * stride];
        uint32_t peak = (uint32_t)(sample < 0 ? -sample : sample);
        if (peak > m->peak) m->peak = peak;
        if (peak >= 32000U) m->near_full++;
        if (m->have_last) {
            int32_t delta = sample - m->last;
            uint32_t step = (uint32_t)(delta < 0 ? -delta : delta);
            if (step > m->step_max) m->step_max = step;
        }
        m->last = (int16_t)sample;
        m->have_last = true;
    }
    m->samples += (uint32_t)samples;
}

static inline void p4_playback_meter_clear_window(p4_playback_meter_t *m)
{
    /* Preserve adjacency across diagnostic windows, reset only on a session. */
    m->samples = m->near_full = m->peak = m->step_max = 0;
}
