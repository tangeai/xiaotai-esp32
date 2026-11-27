// SPDX-License-Identifier: MIT
#include <assert.h>

#include "pacer.h"

int main(void)
{
    media_pacer_t p;
    const xiaotai_media_latency_t profiles[] = {
        XIAOTAI_MEDIA_LATENCY_LOW, XIAOTAI_MEDIA_LATENCY_RELAYED,
        XIAOTAI_MEDIA_LATENCY_BURSTY, XIAOTAI_MEDIA_LATENCY_VOIP,
    };
    for (size_t i = 0; i < sizeof(profiles) / sizeof(*profiles); ++i) {
        const xiaotai_media_latency_t *profile = &profiles[i];
        media_pacer_open(&p, *profile, 2000, MEDIA_PACER_ADAPTIVE);
        assert(p.target_ms == profile->base_ms);
        media_pacer_arrival(&p, 10, 1000, 20000, true);
        assert(!media_pacer_start(&p, 1, 1, 120999, false, false));
        assert(media_pacer_start(&p, 1, 1, 121000, false, false));
        media_pacer_underrun(&p);
        assert(!p.started && p.target_ms == profile->base_ms + 40U);
        assert(media_pacer_start(&p, 1, 1, 121001, true, false));
        media_pacer_open(&p, *profile, profile->surplus_ms + 25U, MEDIA_PACER_ADAPTIVE);
        assert(p.profile.max_ms == 25U && p.target_ms == 25U);
        assert(media_pacer_start(&p, 1, 1, 2, false, true));
    }

    media_pacer_open(&p, profiles[0], 260, MEDIA_PACER_ADAPTIVE);
    assert(media_pacer_speed(&p, 135, 90, 15) == 0);
    assert(media_pacer_speed(&p, 136, 90, 15) == 12);
    assert(media_pacer_speed(&p, 104, 90, 15) == -12);
    assert(media_pacer_speed(&p, 200, 90, 15) == 25);
    media_pacer_open(&p, profiles[3], 260, MEDIA_PACER_ADAPTIVE);
    assert(media_pacer_speed(&p, 104, 90, 15) == -30);

    media_pacer_open(&p, profiles[1], 880, MEDIA_PACER_ADAPTIVE);
    assert(p.profile.steady_jitter_ms == 16 && p.profile.steady_peak_ms == 40 && p.profile.step_down_ms == 20);
    p.target_ms = 500;
    media_pacer_arrival(&p, 1, 1, 20000, true);
    for (int64_t s = 1; s <= 3; ++s) {
        p.peak_ms = 40;
        p.jitter_q8 = 16U * 256U;
        media_pacer_tick(&p, (s * 1000000) + 1, true, false);
    }
    assert(p.target_ms == 480);
    for (int64_t s = 4; s <= 6; ++s) {
        p.peak_ms = 41;
        media_pacer_tick(&p, (s * 1000000) + 1, true, false);
    }
    assert(p.target_ms == 480);
    p.peak_ms = 1000;
    media_pacer_tick(&p, 7000001, true, false);
    assert(p.target_ms == 520);

    media_pacer_open(&p, profiles[3], 880, MEDIA_PACER_PACKET_PREFILL);
    media_pacer_arrival(&p, 1, 1000, 120000, true);
    /* A long packet still counts as one. The wait begins on dequeue, not arrival. */
    assert(!media_pacer_start(&p, 120, 1, 5000, false, false));
    assert(!media_pacer_start(&p, 120, 1, 64999, false, false));
    assert(media_pacer_start(&p, 120, 1, 65000, false, false));
    assert(!media_pacer_on_empty(&p, 70000) && p.started);
    media_pacer_open(&p, profiles[3], 880, MEDIA_PACER_PACKET_PREFILL);
    assert(media_pacer_start(&p, 20, 3, 2, false, false));
    assert(media_pacer_speed(&p, 500, 0, 10) == 0);

    media_pacer_open(&p, profiles[0], 880, MEDIA_PACER_FIXED);
    assert(media_pacer_start(&p, 1, 1, 0, false, false));
    assert(media_pacer_speed(&p, 500, 0, 20) == 0);
    assert(media_pacer_short_tail(&p, 0, 0, 0, 90, 20, 20));

    media_pacer_open(&p, profiles[3], 880, MEDIA_PACER_TIMED);
    assert(p.target_ms == 200 && p.profile.min_ms == 60 && p.profile.max_ms == 500);
    media_pacer_arrival(&p, 10, 1000, 20000, true);
    assert(!media_pacer_start(&p, 20, 1, 200999, false, false));
    assert(media_pacer_start(&p, 20, 1, 201000, false, false));
    media_pacer_written(&p, 202000);
    assert(media_pacer_empty_wait(&p, 321000) == 1 && !media_pacer_on_empty(&p, 321999));
    assert(media_pacer_on_empty(&p, 322000) && p.target_ms == 200);
    assert(!media_pacer_short_tail(&p, 60000, 1, 10000, 0, 20, 20));
    assert(media_pacer_short_tail(&p, 60001, 1, 10000, 0, 20, 20));
    media_pacer_open(&p, profiles[3], 880, MEDIA_PACER_TIMED);
    media_pacer_arrival(&p, 0, 0, 2500, true);
    assert(!media_pacer_start(&p, 80, 32, 1000, false, false));
    media_pacer_set_prefill_packets(&p, 32);
    assert(media_pacer_start(&p, 80, 32, 1000, false, false));
    media_pacer_open(&p, profiles[3], 880, MEDIA_PACER_TIMED);
    for (unsigned i = 0; i <= 500; ++i)
        media_pacer_arrival(&p, 100 + i * 20U, 1 + (int64_t)i * 20000, 20000, i == 0);
    assert(p.jitter_us == 0 && p.target_ms == 180);
    media_pacer_arrival(&p, 10120, 10500001, 20000, false);
    assert(p.target_ms == 220 && p.jitter_us == 15625);
    media_pacer_arrival(&p, 3, 10510001, 20000, false);
    assert(p.jitter_us == 15625 && !p.timed_run);

    media_pacer_open(&p, profiles[0], 880, MEDIA_PACER_TIMED);
    assert(media_pacer_start(&p, 200, 10, 1, false, false));
    for (unsigned i = 0; i < 9; ++i)
        media_pacer_arrival(&p, 50 + i * 20U, 1840001 + (int64_t)i * 20000, 20000, i == 0);
    assert(media_pacer_timed_delta(&p, 300, 2000000, 800, 320) == 0);
    assert(media_pacer_timed_delta(&p, 300, 2000001, 800, 320) == 1);
    assert(media_pacer_timed_delta(&p, 221, 2000001, 800, 320) == 1);
    assert(media_pacer_timed_delta(&p, 220, 2000001, 800, 320) == 0);
    assert(media_pacer_timed_delta(&p, 300, 2000001, 320, 320) == 0);
    assert(media_pacer_timed_delta(&p, 139, 2000001, 800, 320) == -1);
    assert(media_pacer_timed_delta(&p, 179, 2000001, 800, 320) == -1);
    assert(media_pacer_timed_delta(&p, 180, 2000001, 800, 320) == 0);
    assert(media_pacer_timed_delta(&p, 300, 2200002, 800, 320) == 0);
}
