// SPDX-License-Identifier: MIT
#include "xmini_echo_guard.h"

#include <stdbool.h>

// Worst-case speaker-to-microphone power gain at volume 40 and 24 dB microphone gain.
#define ERL_REF_DB 6
#define CLOSED_DB (-40)
#define DOUBLE_TALK_DB (-6)
#define SPEECH_MARGIN 8U
#define HANG_FRAMES 8U
#define STARTUP_FRAMES 15U // ADC low-frequency start-up transient
#define NOISE_MIN 64U
#define NOISE_MAX (1U << 30)
#define OPEN_STEP 1024
#define CLOSE_STEP 82

static uint32_t power_q8(int db)
{
    static const uint16_t tenths[10] = {256, 322, 406, 511, 643, 810, 1019, 1283, 1615, 2033};
    if (db < -30) return 0;
    if (db > 60) db = 60;
    uint32_t value = tenths[(db + 30) % 10];
    for (int decade = (db + 30) / 10; decade < 3; ++decade) value = (value + 5) / 10;
    for (int decade = (db + 30) / 10; decade > 3; --decade) value *= 10;
    return value;
}

static uint16_t amplitude_q15(int db)
{
    static const uint16_t steps[20] = {
        32768, 29205, 26029, 23198, 20675, 18427, 16423, 14637, 13045, 11627,
        10362, 9235, 8231, 7336, 6538, 5827, 5193, 4629, 4125, 3677,
    };
    uint32_t value = steps[-db % 20];
    for (int decade = -db / 20; decade > 0; --decade) value = (value + 5) / 10;
    return (uint16_t)value;
}

static uint32_t ac_power(const int16_t *pcm, size_t n)
{
    int32_t sum = 0;
    uint64_t squares = 0;
    for (size_t i = 0; i < n; ++i) {
        sum += pcm[i];
        squares += (uint32_t)(pcm[i] * pcm[i]);
    }
    if (!n) return 0;
    int32_t mean = sum / (int32_t)n;
    return (uint32_t)(squares / n) - (uint32_t)(mean * mean);
}

void xmini_echo_guard_levels(xmini_echo_guard_t *g, int volume, int gain_db)
{
    volume = volume < 0 ? 0 : volume > 100 ? 100 : volume;
    gain_db = gain_db < 0 ? 0 : gain_db > 42 ? 42 : gain_db;
    gain_db -= gain_db % 6;
    // esp_codec_dev maps volume 1..100 to -49.5..0 dB and 0 to -96 dB; ES8311 gain uses 6 dB steps.
    int erl_db = ERL_REF_DB + gain_db - 24 + (volume ? (volume + 1) / 2 - 20 : -66);
    int excess = erl_db > ERL_REF_DB ? erl_db - ERL_REF_DB : 0;
    if (g->mic_db != gain_db) {
        uint64_t noise = (uint64_t)g->noise * power_q8(gain_db - g->mic_db) >> 8;
        g->noise = noise > NOISE_MAX ? NOISE_MAX : (uint32_t)noise;
        g->mic_db = (uint8_t)gain_db;
    }
    g->erl_q8 = power_q8(erl_db);
    // Extra playback level raises both echo paths of a two-device loop.
    g->closed_q15 = amplitude_q15(CLOSED_DB - 2 * excess);
    g->dt_q15 = amplitude_q15(DOUBLE_TALK_DB - excess);
}

void xmini_echo_guard_reset(xmini_echo_guard_t *g, int volume, int gain_db)
{
    *g = (xmini_echo_guard_t){0};
    xmini_echo_guard_levels(g, volume, gain_db);
    g->noise = NOISE_MAX;
    g->startup = STARTUP_FRAMES;
    g->gain_q15 = g->closed_q15;
}

void xmini_echo_guard_downlink(xmini_echo_guard_t *g, const int16_t *pcm, size_t n)
{
    g->far[g->far_next] = ac_power(pcm, n);
    g->far_next = (uint8_t)((g->far_next + 1) % XMINI_ECHO_GUARD_FAR_FRAMES);
}

void xmini_echo_guard_uplink(xmini_echo_guard_t *g, int16_t *pcm, size_t n)
{
    uint32_t power = ac_power(pcm, n), far = 0, rise = g->noise + (g->noise >> 6) + 1;
    for (unsigned i = 0; i < XMINI_ECHO_GUARD_FAR_FRAMES; ++i)
        if (g->far[i] > far) far = g->far[i];
    g->noise = power < rise ? power : rise;
    if (g->noise < NOISE_MIN) g->noise = NOISE_MIN;
    uint64_t echo = (uint64_t)far * g->erl_q8 >> 8;
    uint64_t level = echo > g->noise ? echo : g->noise;
    bool speech = !g->startup && power > level * SPEECH_MARGIN;
    bool open = speech || g->hang;
    bool echoed = echo > 2ULL * g->noise && echo * 100 > power;
    if (g->startup) --g->startup;
    if (speech) g->hang = HANG_FRAMES;
    else if (g->hang) --g->hang;
    int32_t target = !open ? g->closed_q15 : echoed ? g->dt_q15 : XMINI_ECHO_GUARD_UNITY;
    int32_t gain = g->gain_q15;
    size_t i = 0;
    for (; i < n && gain != target; ++i) {
        if (gain < target) gain = gain + OPEN_STEP < target ? gain + OPEN_STEP : target;
        else gain = gain - CLOSE_STEP > target ? gain - CLOSE_STEP : target;
        pcm[i] = (int16_t)((pcm[i] * gain + 16384) >> 15);
    }
    for (; i < n && gain != XMINI_ECHO_GUARD_UNITY; ++i) pcm[i] = (int16_t)((pcm[i] * gain + 16384) >> 15);
    g->gain_q15 = (uint16_t)gain;
    if (!open) ++g->closed_frames;
    else if (echoed) ++g->dt_frames;
    else ++g->open_frames;
}
